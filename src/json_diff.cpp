#include "mybinlog/json_diff.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "bytes_internal.h"
#include "json_internal.h"

namespace mybinlog {
namespace {

// Json_diff_vector::write_binary: uint32 length ahead of the diffs.
constexpr size_t kVectorLengthBytes = 4;
constexpr uint8_t kOperationCount = 3;

// One step of a path without wildcards (jpl_member or jpl_array_cell).
struct PathLeg {
  bool is_member = false;
  std::string member;
  uint32_t index = 0;
  bool from_end = false;  // `last` or `last-N`
};

// my_isspace(&my_charset_utf8mb4_bin, c): bytes 0x09..0x0D and the space.
bool IsPathSpace(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

// The server's my_isalpha/my_isdigit take a char, so only the codepoint's low
// byte is looked up in the utf8mb4 ctype table: letters are A-Z, a-z and
// 0x80..0xFE, digits 0-9.
bool CtypeIsLetter(uint32_t codepoint) {
  const uint8_t b = static_cast<uint8_t>(codepoint);
  return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= 0x80 && b != 0xFF);
}

bool CtypeIsDigit(uint32_t codepoint) {
  const uint8_t b = static_cast<uint8_t>(codepoint);
  return b >= '0' && b <= '9';
}

bool IsCombiningMark(uint32_t codepoint) { return codepoint >= 0x300 && codepoint <= 0x36F; }

bool IsConnectorPunctuation(uint32_t codepoint) {
  switch (codepoint) {
    case 0x5F:
    case 0x203F:
    case 0x2040:
    case 0x2054:
    case 0xFE33:
    case 0xFE34:
    case 0xFE4D:
    case 0xFE4E:
    case 0xFE4F:
    case 0xFF3F:
      return true;
    default:
      return false;
  }
}

// Decode one well-formed UTF-8 sequence (no overlongs, surrogates or values past U+10FFFF).
bool DecodeUtf8(const std::string& s, size_t* pos, uint32_t* codepoint) {
  const auto byte = [&s](size_t i) { return static_cast<uint8_t>(s[i]); };
  const uint8_t lead = byte(*pos);
  size_t extra = 0;
  uint32_t cp = 0;
  uint32_t min = 0;
  if (lead < 0x80) {
    *codepoint = lead;
    ++*pos;
    return true;
  } else if (lead >= 0xC2 && lead <= 0xDF) {
    extra = 1;
    cp = lead & 0x1F;
    min = 0x80;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    extra = 2;
    cp = lead & 0x0F;
    min = 0x800;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    extra = 3;
    cp = lead & 0x07;
    min = 0x10000;
  } else {
    return false;
  }
  if (s.size() - *pos <= extra) return false;
  for (size_t i = 1; i <= extra; ++i) {
    const uint8_t b = byte(*pos + i);
    if ((b & 0xC0) != 0x80) return false;
    cp = (cp << 6) | (b & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
  *codepoint = cp;
  *pos += extra + 1;
  return true;
}

// is_ecmascript_identifier (`sql-common/json_path.cc`), with the server's ctype lookups.
bool IsEcmaScriptIdentifier(const std::string& name) {
  if (name.empty()) return false;
  size_t pos = 0;
  while (pos < name.size()) {
    const bool first = pos == 0;
    uint32_t cp = 0;
    if (!DecodeUtf8(name, &pos, &cp)) return false;
    if ((CtypeIsLetter(cp) && !IsCombiningMark(cp)) || cp == '$' || cp == '_') continue;
    if (first) return false;
    if (IsCombiningMark(cp) || CtypeIsDigit(cp) || IsConnectorPunctuation(cp) || cp == 0x200C ||
        cp == 0x200D) {
      continue;
    }
    return false;
  }
  return true;
}

void AppendUtf8(uint32_t cp, std::string* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

bool ReadHex4(const char* p, const char* end, uint32_t* value) {
  if (end - p < 4) return false;
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    const char c = p[i];
    uint32_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<uint32_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<uint32_t>(c - 'A' + 10);
    } else {
      return false;
    }
    v = v * 16 + digit;
  }
  *value = v;
  return true;
}

// The body of a JSON string literal (between the quotes): decode escapes,
// reject raw control characters and unescaped quotes, as RapidJSON does for
// the server's parse_name_with_rapidjson.
bool UnescapeJsonString(const char* p, const char* end, std::string* out) {
  while (p < end) {
    const char c = *p++;
    if (c == '"' || static_cast<uint8_t>(c) < 0x20) return false;
    if (c != '\\') {
      out->push_back(c);
      continue;
    }
    if (p == end) return false;
    const char e = *p++;
    switch (e) {
      case '"':
      case '\\':
      case '/':
        out->push_back(e);
        break;
      case 'b':
        out->push_back('\b');
        break;
      case 'f':
        out->push_back('\f');
        break;
      case 'n':
        out->push_back('\n');
        break;
      case 'r':
        out->push_back('\r');
        break;
      case 't':
        out->push_back('\t');
        break;
      case 'u': {
        uint32_t cp = 0;
        if (!ReadHex4(p, end, &cp)) return false;
        p += 4;
        if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          uint32_t low = 0;
          if (end - p < 2 || p[0] != '\\' || p[1] != 'u' || !ReadHex4(p + 2, end, &low) ||
              low < 0xDC00 || low > 0xDFFF) {
            return false;
          }
          p += 6;
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        }
        AppendUtf8(cp, out);
        break;
      }
      default:
        return false;
    }
  }
  return true;
}

class PathParser {
 public:
  PathParser(const char* data, size_t size) : p_(data), end_(data + size) {}

  // parse_path: `$`, then legs separated by optional whitespace.
  bool Parse(std::vector<PathLeg>* legs) {
    SkipSpace();
    if (p_ == end_ || *p_++ != '$') return false;
    SkipSpace();
    while (p_ != end_) {
      PathLeg leg;
      if (*p_ == '[') {
        if (!ParseArrayLeg(&leg)) return false;
      } else if (*p_ == '.') {
        if (!ParseMemberLeg(&leg)) return false;
      } else {
        // `**` (ellipsis) and anything else.
        return false;
      }
      legs->push_back(std::move(leg));
      SkipSpace();
    }
    return true;
  }

 private:
  void SkipSpace() {
    while (p_ != end_ && IsPathSpace(*p_)) ++p_;
  }

  // parse_array_index: N, last, or last-N (whitespace allowed around the minus).
  bool ParseIndex(PathLeg* leg) {
    if (end_ - p_ >= 4 && std::memcmp(p_, "last", 4) == 0) {
      p_ += 4;
      leg->from_end = true;
      SkipSpace();
      if (p_ == end_ || *p_ != '-') {
        leg->index = 0;
        return true;
      }
      ++p_;
      SkipSpace();
    }
    if (p_ == end_ || !IsAsciiDigit(*p_)) return false;
    uint64_t value = 0;
    while (p_ != end_ && IsAsciiDigit(*p_)) {
      value = value * 10 + static_cast<uint64_t>(*p_++ - '0');
      if (value > UINT32_MAX) return false;
    }
    leg->index = static_cast<uint32_t>(value);
    return true;
  }

  // parse_array_leg, cells only: wildcards and `M to N` ranges are refused.
  bool ParseArrayLeg(PathLeg* leg) {
    ++p_;
    SkipSpace();
    if (p_ == end_ || !ParseIndex(leg)) return false;
    SkipSpace();
    return p_ != end_ && *p_++ == ']';
  }

  // parse_member_leg: a quoted name, or an unquoted ECMAScript identifier
  // ending at whitespace, `[`, `.`, `*` or the end; either may hold escapes.
  bool ParseMemberLeg(PathLeg* leg) {
    ++p_;
    SkipSpace();
    if (p_ == end_ || *p_ == '*') return false;
    leg->is_member = true;
    if (*p_ == '"') {
      const char* const start = ++p_;
      while (p_ != end_ && *p_ != '"') {
        if (*p_ == '\\' && end_ - p_ >= 2) ++p_;
        ++p_;
      }
      if (p_ == end_) return false;
      const char* const stop = p_++;
      return UnescapeJsonString(start, stop, &leg->member);
    }
    const char* const start = p_;
    while (p_ != end_ && !IsPathSpace(*p_) && *p_ != '[' && *p_ != '.' && *p_ != '*') ++p_;
    return UnescapeJsonString(start, p_, &leg->member) && IsEcmaScriptIdentifier(leg->member);
  }

  const char* p_;
  const char* const end_;
};

bool ParsePath(const std::string& text, std::vector<PathLeg>* legs) {
  return PathParser(text.data(), text.size()).Parse(legs);
}

// Json_path::to_string.
std::string PrintPath(const std::vector<PathLeg>& legs) {
  std::string out = "$";
  for (const PathLeg& leg : legs) {
    if (leg.is_member) {
      out.push_back('.');
      if (IsEcmaScriptIdentifier(leg.member)) {
        out.append(leg.member);
      } else {
        internal::AppendJsonQuoted(leg.member.data(), leg.member.size(), &out);
      }
      continue;
    }
    out.push_back('[');
    if (leg.from_end) {
      out.append("last");
      if (leg.index > 0) out.append("-").append(std::to_string(leg.index));
    } else {
      out.append(std::to_string(leg.index));
    }
    out.push_back(']');
  }
  return out;
}

// Json_array_index: the clamped position and whether the index names an element.
size_t ArrayPosition(const PathLeg& leg, size_t size) {
  if (leg.from_end) return leg.index < size ? size - leg.index - 1 : 0;
  return std::min<size_t>(leg.index, size);
}

bool WithinBounds(const PathLeg& leg, size_t size) { return leg.index < size; }

JsonMember* FindMember(JsonValue* object, const std::string& key) {
  for (JsonMember& member : object->members) {
    if (member.key == key) return &member;
  }
  return nullptr;
}

// json_key_less: shorter keys first, equal lengths by unsigned bytes.
bool KeyLess(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return a.size() < b.size();
  return std::memcmp(a.data(), b.data(), a.size()) < 0;
}

// seek_exact_path over legs [0, count).
JsonValue* Seek(JsonValue* node, const std::vector<PathLeg>& legs, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const PathLeg& leg = legs[i];
    if (node->kind == JsonValue::Kind::kArray && !leg.is_member) {
      if (!WithinBounds(leg, node->elements.size())) return nullptr;
      node = &node->elements[ArrayPosition(leg, node->elements.size())];
    } else if (node->kind == JsonValue::Kind::kObject && leg.is_member) {
      JsonMember* member = FindMember(node, leg.member);
      if (member == nullptr) return nullptr;
      node = &member->value;
    } else {
      return nullptr;
    }
  }
  return node;
}

// apply_json_diff on one document.
Status ApplyOne(const JsonDiff& diff, JsonValue* doc) {
  std::vector<PathLeg> legs;
  if (!ParsePath(diff.path, &legs) || legs.empty()) return Status::kMalformedValue;
  const PathLeg& last = legs.back();
  switch (diff.op) {
    case JsonDiffOperation::kReplace: {
      JsonValue* target = Seek(doc, legs, legs.size());
      if (target == nullptr) return Status::kMalformedValue;
      *target = diff.value;
      return Status::kOk;
    }
    case JsonDiffOperation::kInsert: {
      JsonValue* parent = Seek(doc, legs, legs.size() - 1);
      if (parent == nullptr) return Status::kMalformedValue;
      if (parent->kind == JsonValue::Kind::kObject && last.is_member) {
        if (FindMember(parent, last.member) != nullptr) return Status::kMalformedValue;
        auto& members = parent->members;
        const auto pos = std::find_if(members.begin(), members.end(), [&last](const JsonMember& m) {
          return KeyLess(last.member, m.key);
        });
        members.insert(pos, JsonMember{last.member, diff.value});
        return Status::kOk;
      }
      if (parent->kind == JsonValue::Kind::kArray && !last.is_member) {
        auto& elements = parent->elements;
        const size_t pos = ArrayPosition(last, elements.size());
        elements.insert(elements.begin() + static_cast<std::ptrdiff_t>(pos), diff.value);
        return Status::kOk;
      }
      return Status::kMalformedValue;
    }
    case JsonDiffOperation::kRemove: {
      JsonValue* parent = Seek(doc, legs, legs.size() - 1);
      if (parent == nullptr) return Status::kMalformedValue;
      if (parent->kind == JsonValue::Kind::kObject && last.is_member) {
        auto& members = parent->members;
        const auto it = std::find_if(members.begin(), members.end(),
                                     [&last](const JsonMember& m) { return m.key == last.member; });
        if (it == members.end()) return Status::kMalformedValue;
        members.erase(it);
        return Status::kOk;
      }
      if (parent->kind == JsonValue::Kind::kArray && !last.is_member) {
        auto& elements = parent->elements;
        if (!WithinBounds(last, elements.size())) return Status::kMalformedValue;
        const size_t pos = ArrayPosition(last, elements.size());
        elements.erase(elements.begin() + static_cast<std::ptrdiff_t>(pos));
        return Status::kOk;
      }
      return Status::kMalformedValue;
    }
  }
  return Status::kMalformedValue;
}

// A packed length inside the declared vector; any failure is a corrupt diff.
Status ReadLength(ByteReader* reader, uint64_t* length) {
  if (reader->ReadPacked(length) != Status::kOk) return Status::kMalformedValue;
  if (*length > reader->remaining()) return Status::kMalformedValue;
  return Status::kOk;
}

// read_json_diff.
Status ReadDiff(ByteReader* reader, JsonDiff* out) {
  uint8_t op = 0;
  if (reader->ReadU8(&op) != Status::kOk) return Status::kMalformedValue;
  if (op >= kOperationCount) return Status::kMalformedValue;
  out->op = static_cast<JsonDiffOperation>(op);

  uint64_t path_length = 0;
  Status status = ReadLength(reader, &path_length);
  if (status != Status::kOk) return status;
  const uint8_t* path = nullptr;
  if (reader->ReadBytes(static_cast<size_t>(path_length), &path) != Status::kOk) {
    return Status::kMalformedValue;
  }
  std::vector<PathLeg> legs;
  if (!PathParser(reinterpret_cast<const char*>(path), static_cast<size_t>(path_length))
           .Parse(&legs)) {
    return Status::kMalformedValue;
  }
  out->path = PrintPath(legs);
  if (out->op == JsonDiffOperation::kRemove) return Status::kOk;

  uint64_t value_length = 0;
  status = ReadLength(reader, &value_length);
  if (status != Status::kOk) return status;
  const uint8_t* value = nullptr;
  if (reader->ReadBytes(static_cast<size_t>(value_length), &value) != Status::kOk) {
    return Status::kMalformedValue;
  }
  status = ParseJsonBinary(value, static_cast<size_t>(value_length), &out->value);
  // The declared length bounds the value, so running out is corruption, not truncation.
  if (status == Status::kTruncated) return Status::kMalformedValue;
  return status;
}

}  // namespace

Status ParseJsonDiffs(const uint8_t* data, size_t size, std::vector<JsonDiff>* out) {
  if (size < kVectorLengthBytes) return Status::kTruncated;
  const uint32_t length = ReadU32Le(data);
  if (size - kVectorLengthBytes < length) return Status::kTruncated;
  if (size - kVectorLengthBytes > length) return Status::kMalformedValue;
  ByteReader reader(data + kVectorLengthBytes, length);
  std::vector<JsonDiff> diffs;
  while (reader.remaining() > 0) {
    JsonDiff diff;
    const Status status = ReadDiff(&reader, &diff);
    if (status != Status::kOk) return status;
    diffs.push_back(std::move(diff));
  }
  *out = std::move(diffs);
  return Status::kOk;
}

Status ApplyJsonDiffs(const JsonValue& before, const std::vector<JsonDiff>& diffs,
                      JsonValue* after) {
  JsonValue doc = before;
  for (const JsonDiff& diff : diffs) {
    const Status status = ApplyOne(diff, &doc);
    if (status != Status::kOk) return status;
  }
  *after = std::move(doc);
  return Status::kOk;
}

Status ApplyJsonDiffsToBinary(const uint8_t* before_data, size_t before_size,
                              const uint8_t* diff_data, size_t diff_size, std::string* after_text) {
  JsonValue doc;
  Status status = ParseJsonBinary(before_data, before_size, &doc);
  if (status != Status::kOk) return status;
  std::vector<JsonDiff> diffs;
  status = ParseJsonDiffs(diff_data, diff_size, &diffs);
  if (status != Status::kOk) return status;
  status = ApplyJsonDiffs(doc, diffs, &doc);
  if (status != Status::kOk) return status;
  return JsonToText(doc, after_text);
}

const char* JsonDiffOperationName(JsonDiffOperation op) noexcept {
  switch (op) {
    case JsonDiffOperation::kReplace:
      return "kReplace";
    case JsonDiffOperation::kInsert:
      return "kInsert";
    case JsonDiffOperation::kRemove:
      return "kRemove";
  }
  return "kUnknown";
}

}  // namespace mybinlog
