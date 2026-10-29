#include "mybinlog/mariadb_gtid.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <utility>

#include "bytes_internal.h"

namespace mybinlog {
namespace {

/// seq_no(8) + domain_id(4) + flags2(1).
constexpr size_t kGtidEventMinBody = 13;

/// domain_id(4) + server_id(4) + seq_no(8).
constexpr size_t kGtidListEntrySize = 16;

constexpr uint32_t kGtidListCountMask = 0x0FFFFFFFu;
constexpr unsigned kGtidListFlagsShift = 28;

std::string_view Trim(std::string_view text) {
  constexpr std::string_view kSpace = " \t\n\r";
  const size_t begin = text.find_first_not_of(kSpace);
  if (begin == std::string_view::npos) return {};
  const size_t end = text.find_last_not_of(kSpace);
  return text.substr(begin, end - begin + 1);
}

template <typename T>
bool ParseDigits(std::string_view text, T* out) {
  if (text.empty() ||
      !std::all_of(text.begin(), text.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), *out);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

}  // namespace

Status MariaDbGtid::Parse(std::string_view text, MariaDbGtid* out) {
  const std::string_view value = Trim(text);
  const size_t first = value.find('-');
  if (first == std::string_view::npos) return Status::kMalformedValue;
  const size_t second = value.find('-', first + 1);
  if (second == std::string_view::npos || value.find('-', second + 1) != std::string_view::npos) {
    return Status::kMalformedValue;
  }
  MariaDbGtid gtid;
  if (!ParseDigits(value.substr(0, first), &gtid.domain_id) ||
      !ParseDigits(value.substr(first + 1, second - first - 1), &gtid.server_id) ||
      !ParseDigits(value.substr(second + 1), &gtid.sequence_no)) {
    return Status::kMalformedValue;
  }
  *out = gtid;
  return Status::kOk;
}

bool MariaDbGtid::IsMariaDbGtidFormat(std::string_view text) {
  MariaDbGtid unused;
  return Parse(text, &unused) == Status::kOk;
}

std::string MariaDbGtid::ToString() const {
  return std::to_string(domain_id) + "-" + std::to_string(server_id) + "-" +
         std::to_string(sequence_no);
}

Status MariaDbGtidPosition::Parse(std::string_view text, MariaDbGtidPosition* out) {
  MariaDbGtidPosition parsed;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t comma = text.find(',', pos);
    const std::string_view entry = Trim(
        text.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
    pos = comma == std::string_view::npos ? text.size() : comma + 1;
    if (entry.empty()) continue;
    MariaDbGtid gtid;
    if (MariaDbGtid::Parse(entry, &gtid) != Status::kOk) return Status::kMalformedValue;
    parsed.gtids.push_back(gtid);
  }
  // Only "" is the empty position; see the header.
  if (parsed.gtids.empty() && !text.empty()) return Status::kMalformedValue;
  *out = std::move(parsed);
  return Status::kOk;
}

std::string MariaDbGtidPosition::ToString() const {
  std::string result;
  for (const auto& gtid : gtids) {
    if (!result.empty()) result += ',';
    result += gtid.ToString();
  }
  return result;
}

bool PositionCovers(const MariaDbGtidPosition& required, const MariaDbGtidPosition& available) {
  std::map<uint32_t, uint64_t> highest;
  for (const auto& gtid : available.gtids) {
    auto [it, inserted] = highest.emplace(gtid.domain_id, gtid.sequence_no);
    if (!inserted) it->second = std::max(it->second, gtid.sequence_no);
  }
  for (const auto& gtid : required.gtids) {
    const auto it = highest.find(gtid.domain_id);
    if (it == highest.end() || it->second < gtid.sequence_no) return false;
  }
  return true;
}

Status ParseMariaDbGtidEvent(const EventHeader& header, const uint8_t* body, size_t size,
                             MariaDbGtidEvent* out) {
  if (body == nullptr || size < kGtidEventMinBody) return Status::kTruncated;
  MariaDbGtidEvent event;
  event.gtid.sequence_no = ReadU64Le(body);
  event.gtid.domain_id = ReadU32Le(body + 8);
  event.gtid.server_id = header.server_id;
  event.flags = body[12];
  *out = event;
  return Status::kOk;
}

Status ParseMariaDbGtidListEvent(const EventHeader& /*header*/, const uint8_t* body, size_t size,
                                 MariaDbGtidListEvent* out) {
  if (body == nullptr || size < 4) return Status::kTruncated;
  const uint32_t count_and_flags = ReadU32Le(body);
  const uint32_t count = count_and_flags & kGtidListCountMask;
  if (count > (size - 4) / kGtidListEntrySize) return Status::kTruncated;
  MariaDbGtidListEvent event;
  event.flags = static_cast<uint8_t>(count_and_flags >> kGtidListFlagsShift);
  event.gtids.gtids.reserve(count);
  const uint8_t* entry = body + 4;
  for (uint32_t i = 0; i < count; ++i, entry += kGtidListEntrySize) {
    MariaDbGtid gtid;
    gtid.domain_id = ReadU32Le(entry);
    gtid.server_id = ReadU32Le(entry + 4);
    gtid.sequence_no = ReadU64Le(entry + 8);
    event.gtids.gtids.push_back(gtid);
  }
  *out = std::move(event);
  return Status::kOk;
}

Status ParseMariaDbAnnotateRowsEvent(const EventHeader& /*header*/, const uint8_t* body,
                                     size_t size, MariaDbAnnotateRowsEvent* out) {
  if (body == nullptr || size == 0) return Status::kTruncated;
  out->query.assign(reinterpret_cast<const char*>(body), size);
  return Status::kOk;
}

}  // namespace mybinlog
