#include "mybinlog/mysql_gtid.h"

#include <algorithm>
#include <charconv>

#include "mybinlog/bytes.h"
#include "mybinlog/event_types.h"

namespace mybinlog {
namespace {

/// Fixed GTID_LOG_EVENT prefix: flags(1) uuid(16) gno(8).
constexpr size_t kGtidEventFixedSize = 1 + 16 + 8;

/// Serialization format version GTID_TAGGED_LOG_EVENT is written with.
constexpr uint64_t kTaggedGtidFormatVersion = 1;

/// Gtid_event defines fields 0-11 (flags .. commit_group_ticket).
constexpr uint64_t kTaggedGtidLastKnownFieldId = 11;

constexpr uint64_t kFieldFlags = 0;
constexpr uint64_t kFieldUuid = 1;
constexpr uint64_t kFieldGno = 2;
constexpr uint64_t kFieldTag = 3;

std::string_view Trim(std::string_view text) {
  constexpr std::string_view kSpace = " \t\n\r";
  const size_t begin = text.find_first_not_of(kSpace);
  if (begin == std::string_view::npos) return {};
  const size_t end = text.find_last_not_of(kSpace);
  return text.substr(begin, end - begin + 1);
}

bool IsTagChar(char ch, bool first) {
  const bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
  return letter || ch == '_' || (!first && ch >= '0' && ch <= '9');
}

// Upstream varlen: the count of trailing one bits in the first byte is the
// number of extra bytes (0-8); the value follows them little-endian.
Status ReadVarlen(const uint8_t* data, size_t len, size_t* pos, uint64_t* out) {
  if (*pos >= len) return Status::kTruncated;
  const uint8_t first = data[*pos];
  size_t bytes = 1;
  while (bytes <= 8 && (first & (1U << (bytes - 1))) != 0) ++bytes;
  if (bytes > len - *pos) return Status::kTruncated;
  uint64_t value = bytes >= 9 ? 0 : static_cast<uint64_t>(first >> bytes);
  if (bytes > 1) {
    uint64_t high = 0;
    for (size_t i = 1; i < bytes; ++i) high |= uint64_t{data[*pos + i]} << (8 * (i - 1));
    value |= high << (8 - bytes + ((bytes + 7) >> 4));
  }
  *pos += bytes;
  *out = value;
  return Status::kOk;
}

// Peek the next field id; consume it only when it is @p expected.
Status TakeFieldId(const uint8_t* data, size_t len, size_t* pos, uint64_t expected, bool* present) {
  *present = false;
  if (*pos >= len) return Status::kOk;
  size_t peek = *pos;
  uint64_t id = 0;
  const Status status = ReadVarlen(data, len, &peek, &id);
  if (status != Status::kOk) return status;
  if (id == expected) {
    *pos = peek;
    *present = true;
  }
  return Status::kOk;
}

Status ParseGno(std::string_view text, uint64_t* out) {
  if (text.empty() ||
      !std::all_of(text.begin(), text.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
    return Status::kMalformedValue;
  }
  uint64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0 ||
      value >= kGnoEnd) {
    return Status::kMalformedValue;
  }
  *out = value;
  return Status::kOk;
}

Status ParseInterval(std::string_view text, GtidSet::Interval* out) {
  const std::string_view value = Trim(text);
  const size_t dash = value.find('-');
  uint64_t start = 0;
  uint64_t last = 0;
  if (dash == std::string_view::npos) {
    if (ParseGno(value, &start) != Status::kOk) return Status::kMalformedValue;
    last = start;
  } else if (ParseGno(value.substr(0, dash), &start) != Status::kOk ||
             ParseGno(value.substr(dash + 1), &last) != Status::kOk || last < start) {
    return Status::kMalformedValue;
  }
  *out = {start, last + 1};
  return Status::kOk;
}

// Sort and coalesce overlapping or adjacent intervals.
void NormalizeIntervals(std::vector<GtidSet::Interval>* intervals) {
  std::sort(intervals->begin(), intervals->end(),
            [](const GtidSet::Interval& lhs, const GtidSet::Interval& rhs) {
              return lhs.start < rhs.start || (lhs.start == rhs.start && lhs.end < rhs.end);
            });
  size_t write = 0;
  for (const auto& interval : *intervals) {
    if (write == 0 || interval.start > (*intervals)[write - 1].end) {
      (*intervals)[write++] = interval;
    } else if (interval.end > (*intervals)[write - 1].end) {
      (*intervals)[write - 1].end = interval.end;
    }
  }
  intervals->resize(write);
}

void StoreU64Le(std::vector<uint8_t>* out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<uint8_t>(value >> (i * 8)));
}

}  // namespace

Status NormalizeGtidTag(std::string_view text, std::string* out) {
  if (text.empty() || text.size() > kGtidTagMaxLength) return Status::kMalformedValue;
  std::string normalized;
  normalized.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (!IsTagChar(ch, i == 0)) return Status::kMalformedValue;
    normalized += (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
  }
  *out = std::move(normalized);
  return Status::kOk;
}

Status ParseUuid(std::string_view text, Uuid* out) {
  if (text.size() != 36) return Status::kMalformedValue;
  const auto nibble = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  Uuid uuid{};
  size_t index = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const bool dash_slot = i == 8 || i == 13 || i == 18 || i == 23;
    if ((text[i] == '-') != dash_slot) return Status::kMalformedValue;
    if (dash_slot) continue;
    const int hi = nibble(text[i]);
    const int lo = i + 1 < text.size() ? nibble(text[i + 1]) : -1;
    if (hi < 0 || lo < 0) return Status::kMalformedValue;
    uuid[index++] = static_cast<uint8_t>((hi << 4) | lo);
    ++i;
  }
  *out = uuid;
  return Status::kOk;
}

std::string FormatUuid(const Uuid& uuid) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(36);
  for (size_t i = 0; i < uuid.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) text += '-';
    text += kHex[uuid[i] >> 4];
    text += kHex[uuid[i] & 0x0F];
  }
  return text;
}

Status GtidSet::Parse(std::string_view text, GtidSet* out) {
  GtidSet parsed;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t comma = text.find(',', pos);
    const std::string_view part = Trim(
        text.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
    pos = comma == std::string_view::npos ? text.size() : comma + 1;
    if (part.empty()) continue;

    const size_t colon = part.find(':');
    if (colon == std::string_view::npos) return Status::kMalformedValue;
    Uuid uuid{};
    if (ParseUuid(part.substr(0, colon), &uuid) != Status::kOk) return Status::kMalformedValue;

    // Untagged intervals, then any number of tags each followed by its own
    // intervals ("uuid:5:mytag:1:other:3"). Every colon piece is a tag or an
    // interval; an empty or dangling piece is malformed.
    std::vector<Interval>* target = nullptr;
    size_t run_intervals = 0;
    size_t piece_pos = colon + 1;
    for (;;) {
      const size_t next = part.find(':', piece_pos);
      const std::string_view piece = Trim(part.substr(
          piece_pos, next == std::string_view::npos ? std::string_view::npos : next - piece_pos));
      std::string tag;
      if (NormalizeGtidTag(piece, &tag) == Status::kOk) {
        if (target != nullptr && run_intervals == 0) return Status::kMalformedValue;
        target = &parsed.sets_[{uuid, tag}];
        run_intervals = 0;
      } else {
        if (target == nullptr) target = &parsed.sets_[{uuid, ""}];
        Interval interval;
        if (ParseInterval(piece, &interval) != Status::kOk) return Status::kMalformedValue;
        if (target->size() == kMaxIntervalsPerTsid) return Status::kLimitExceeded;
        target->push_back(interval);
        ++run_intervals;
      }
      if (next == std::string_view::npos) break;
      piece_pos = next + 1;
    }
    if (run_intervals == 0) return Status::kMalformedValue;
  }

  // Only "" is the empty set: separators or whitespace alone would otherwise
  // ask the server for every binlog it still retains.
  if (parsed.sets_.empty() && !text.empty()) return Status::kMalformedValue;

  for (auto& entry : parsed.sets_) NormalizeIntervals(&entry.second);
  *out = std::move(parsed);
  return Status::kOk;
}

Status GtidSet::DecodeBinary(const uint8_t* data, size_t len, GtidSet* out) {
  if (len < 8) return Status::kTruncated;
  const uint64_t count_and_format = ReadU64Le(data);
  const uint8_t format = static_cast<uint8_t>(count_and_format >> 56);
  if (format > 1) return Status::kUnsupported;
  const bool tagged = format == 1;
  if (tagged && static_cast<uint8_t>(count_and_format) != format) return Status::kMalformedValue;
  const uint64_t tsid_count = tagged ? ((count_and_format & 0x00FFFFFFFFFFFF00ULL) >> 8)
                                     : (count_and_format & 0x00FFFFFFFFFFFFFFULL);
  size_t pos = 8;
  // Smallest TSID: uuid, interval count, one interval.
  constexpr size_t kMinTsidSize = 16 + 8 + 16;
  if (tsid_count > (len - pos) / kMinTsidSize) return Status::kTruncated;

  GtidSet decoded;
  for (uint64_t i = 0; i < tsid_count; ++i) {
    if (len - pos < 16) return Status::kTruncated;
    Uuid uuid{};
    std::copy_n(data + pos, uuid.size(), uuid.begin());
    pos += uuid.size();
    std::string tag;
    if (tagged) {
      uint64_t tag_length = 0;
      const Status status = ReadVarlen(data, len, &pos, &tag_length);
      if (status != Status::kOk) return status;
      if (tag_length > kGtidTagMaxLength) return Status::kMalformedValue;
      if (tag_length > len - pos) return Status::kTruncated;
      const std::string_view raw(reinterpret_cast<const char*>(data + pos),
                                 static_cast<size_t>(tag_length));
      pos += static_cast<size_t>(tag_length);
      if (!raw.empty() && NormalizeGtidTag(raw, &tag) != Status::kOk) {
        return Status::kMalformedValue;
      }
    }
    if (len - pos < 8) return Status::kTruncated;
    const uint64_t count = ReadU64Le(data + pos);
    pos += 8;
    if (count == 0) return Status::kMalformedValue;
    if (count > kMaxIntervalsPerTsid) return Status::kLimitExceeded;
    if (count > (len - pos) / 16) return Status::kTruncated;
    auto& intervals = decoded.sets_[{uuid, tag}];
    if (intervals.size() > kMaxIntervalsPerTsid - static_cast<size_t>(count)) {
      return Status::kLimitExceeded;
    }
    for (uint64_t j = 0; j < count; ++j) {
      const uint64_t start = ReadU64Le(data + pos);
      const uint64_t end = ReadU64Le(data + pos + 8);
      pos += 16;
      // Upstream reads both bounds as signed 64-bit.
      if (start == 0 || end <= start || end > kGnoEnd) return Status::kMalformedValue;
      intervals.push_back({start, end});
    }
  }
  if (pos != len) return Status::kMalformedValue;
  for (auto& entry : decoded.sets_) NormalizeIntervals(&entry.second);
  *out = std::move(decoded);
  return Status::kOk;
}

void GtidSet::EncodeBinary(std::vector<uint8_t>* out) const {
  out->clear();
  const bool tagged = std::any_of(sets_.begin(), sets_.end(),
                                  [](const auto& entry) { return !entry.first.tag.empty(); });
  size_t size = 8;
  for (const auto& [tsid, intervals] : sets_) {
    size += 16 + (tagged ? 1 + tsid.tag.size() : 0) + 8 + intervals.size() * 16;
  }
  out->reserve(size);
  const uint64_t count = sets_.size();
  StoreU64Le(out, tagged ? ((count << 8) | (uint64_t{1} << 56) | 1) : count);
  for (const auto& [tsid, intervals] : sets_) {
    out->insert(out->end(), tsid.uuid.begin(), tsid.uuid.end());
    if (tagged) {
      // A tag is at most 32 bytes, so its varlen length is one byte.
      out->push_back(static_cast<uint8_t>(tsid.tag.size() << 1));
      out->insert(out->end(), tsid.tag.begin(), tsid.tag.end());
    }
    StoreU64Le(out, intervals.size());
    for (const auto& interval : intervals) {
      StoreU64Le(out, interval.start);
      StoreU64Le(out, interval.end);
    }
  }
}

Status GtidSet::Merge(const GtidSet& other) {
  auto merged = sets_;
  for (const auto& [tsid, intervals] : other.sets_) {
    auto& target = merged[tsid];
    if (target.size() > kMaxIntervalsPerTsid - intervals.size()) return Status::kLimitExceeded;
    target.insert(target.end(), intervals.begin(), intervals.end());
    NormalizeIntervals(&target);
  }
  sets_ = std::move(merged);
  return Status::kOk;
}

Status GtidSet::Add(const Uuid& uuid, std::string_view tag, Interval interval) {
  if (interval.start == 0 || interval.end <= interval.start || interval.end > kGnoEnd) {
    return Status::kMalformedValue;
  }
  std::string normalized;
  if (!tag.empty() && NormalizeGtidTag(tag, &normalized) != Status::kOk) {
    return Status::kMalformedValue;
  }
  const auto found = sets_.find({uuid, normalized});
  if (found != sets_.end() && found->second.size() == kMaxIntervalsPerTsid) {
    return Status::kLimitExceeded;
  }
  auto& intervals = sets_[{uuid, normalized}];
  intervals.push_back(interval);
  NormalizeIntervals(&intervals);
  return Status::kOk;
}

bool GtidSet::IsSubsetOf(const GtidSet& other) const {
  for (const auto& [tsid, intervals] : sets_) {
    const auto other_it = other.sets_.find(tsid);
    if (other_it == other.sets_.end()) return false;
    const auto& covering = other_it->second;
    size_t index = 0;
    for (const auto& interval : intervals) {
      while (index < covering.size() && covering[index].end <= interval.start) ++index;
      if (index == covering.size() || covering[index].start > interval.start ||
          covering[index].end < interval.end) {
        return false;
      }
    }
  }
  return true;
}

std::string GtidSet::ToString() const {
  std::string result;
  for (const auto& [tsid, intervals] : sets_) {
    if (intervals.empty()) continue;
    if (!result.empty()) result += ',';
    result += FormatUuid(tsid.uuid);
    if (!tsid.tag.empty()) {
      result += ':';
      result += tsid.tag;
    }
    for (const auto& interval : intervals) {
      result += ':';
      result += std::to_string(interval.start);
      result += '-';
      result += std::to_string(interval.end - 1);
    }
  }
  return result;
}

std::string GtidEvent::ToString() const {
  std::string text = FormatUuid(uuid);
  text += ':';
  if (!tag.empty()) {
    text += tag;
    text += ':';
  }
  text += std::to_string(gno);
  return text;
}

Status ParseGtidEvent(const EventHeader& header, const uint8_t* body, size_t len, GtidEvent* out) {
  GtidEvent event;
  if (header.type_code == static_cast<uint8_t>(EventType::kGtidLogEvent)) {
    if (len < kGtidEventFixedSize) return Status::kTruncated;
    event.flags = body[0];
    std::copy_n(body + 1, event.uuid.size(), event.uuid.begin());
    event.gno = ReadU64Le(body + 17);
    if (event.gno == 0 || event.gno >= kGnoEnd) return Status::kMalformedValue;
    *out = std::move(event);
    return Status::kOk;
  }
  if (header.type_code != static_cast<uint8_t>(EventType::kGtidTaggedLogEvent)) {
    return Status::kUnsupported;
  }

  size_t pos = 0;
  uint64_t value = 0;
  Status status = ReadVarlen(body, len, &pos, &value);
  if (status != Status::kOk) return status;
  if (value != kTaggedGtidFormatVersion) return Status::kUnsupported;
  uint64_t message_size = 0;
  status = ReadVarlen(body, len, &pos, &message_size);
  if (status != Status::kOk) return status;
  if (message_size > len) return Status::kTruncated;
  if (message_size < len) return Status::kMalformedValue;
  uint64_t last_non_ignorable = 0;
  status = ReadVarlen(body, len, &pos, &last_non_ignorable);
  if (status != Status::kOk) return status;
  // The id names one past the last field a decoder may not skip.
  if (last_non_ignorable > kTaggedGtidLastKnownFieldId + 1) return Status::kUnsupported;

  // A field absent from the message keeps its default, as upstream decodes.
  bool present = false;
  status = TakeFieldId(body, len, &pos, kFieldFlags, &present);
  if (status != Status::kOk) return status;
  if (present) {
    status = ReadVarlen(body, len, &pos, &value);
    if (status != Status::kOk) return status;
    if (value > UINT8_MAX) return Status::kMalformedValue;
    event.flags = static_cast<uint8_t>(value);
  }

  status = TakeFieldId(body, len, &pos, kFieldUuid, &present);
  if (status != Status::kOk) return status;
  if (!present) return Status::kMalformedValue;
  for (uint8_t& byte : event.uuid) {
    status = ReadVarlen(body, len, &pos, &value);
    if (status != Status::kOk) return status;
    if (value > UINT8_MAX) return Status::kMalformedValue;
    byte = static_cast<uint8_t>(value);
  }

  status = TakeFieldId(body, len, &pos, kFieldGno, &present);
  if (status != Status::kOk) return status;
  if (!present) return Status::kMalformedValue;
  status = ReadVarlen(body, len, &pos, &value);
  if (status != Status::kOk) return status;
  // Signed varlen: low bit is the sign, the rest the (complemented) magnitude.
  const auto gno = static_cast<int64_t>((value >> 1) ^ (0 - (value & 1)));
  if (gno <= 0 || static_cast<uint64_t>(gno) >= kGnoEnd) return Status::kMalformedValue;
  event.gno = static_cast<uint64_t>(gno);

  status = TakeFieldId(body, len, &pos, kFieldTag, &present);
  if (status != Status::kOk) return status;
  if (present) {
    uint64_t tag_length = 0;
    status = ReadVarlen(body, len, &pos, &tag_length);
    if (status != Status::kOk) return status;
    if (tag_length > kGtidTagMaxLength) return Status::kMalformedValue;
    if (tag_length > len - pos) return Status::kTruncated;
    const std::string_view raw(reinterpret_cast<const char*>(body + pos),
                               static_cast<size_t>(tag_length));
    if (!raw.empty() && NormalizeGtidTag(raw, &event.tag) != Status::kOk) {
      return Status::kMalformedValue;
    }
  }
  // Fields 4-11 (commit order, timestamps, lengths, versions) end at the
  // message size checked above; nothing past the tag is needed here.
  *out = std::move(event);
  return Status::kOk;
}

}  // namespace mybinlog
