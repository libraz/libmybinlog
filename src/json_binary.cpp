#include <cstring>
#include <utility>

#include "bytes_internal.h"
#include "json_internal.h"
#include "mybinlog/json.h"

namespace mybinlog {
namespace internal {
namespace {

// A length prefix takes at most five 7-bit groups (UINT32_MAX).
constexpr size_t kMaxVariableLengthBytes = 5;

// Overrun of the top-level value means the input ended early; inside a
// container it means an entry points past the container's declared size.
Status Overrun(bool top_level) { return top_level ? Status::kTruncated : Status::kMalformedValue; }

// read_variable_length: little-endian 7-bit groups, high bit continues.
Status ReadVariableLength(const uint8_t* data, size_t size, bool top_level, uint32_t* length,
                          size_t* used) {
  uint64_t value = 0;
  for (size_t i = 0; i < kMaxVariableLengthBytes; ++i) {
    if (i == size) return Overrun(top_level);
    value |= static_cast<uint64_t>(data[i] & 0x7F) << (7 * i);
    if ((data[i] & 0x80) == 0) {
      if (value > UINT32_MAX) return Status::kMalformedValue;
      *length = static_cast<uint32_t>(value);
      *used = i + 1;
      return Status::kOk;
    }
  }
  return Status::kMalformedValue;
}

bool IsInlined(uint8_t type, bool large) {
  switch (type) {
    case kJsonbLiteral:
    case kJsonbInt16:
    case kJsonbUInt16:
      return true;
    case kJsonbInt32:
    case kJsonbUInt32:
      return large;
    default:
      return false;
  }
}

Status ParseValue(uint8_t type, const uint8_t* data, size_t size, bool top_level, size_t depth,
                  JsonValue* out);

Status ParseScalar(uint8_t type, const uint8_t* data, size_t size, bool top_level, JsonValue* out) {
  switch (type) {
    case kJsonbLiteral:
      if (size < 1) return Overrun(top_level);
      switch (data[0]) {
        case kJsonbNullLiteral:
          out->kind = JsonValue::Kind::kNull;
          return Status::kOk;
        case kJsonbTrueLiteral:
        case kJsonbFalseLiteral:
          out->kind = JsonValue::Kind::kBoolean;
          out->b = data[0] == kJsonbTrueLiteral;
          return Status::kOk;
        default:
          return Status::kMalformedValue;
      }
    case kJsonbInt16:
      if (size < 2) return Overrun(top_level);
      out->kind = JsonValue::Kind::kInt;
      out->i = static_cast<int16_t>(ReadU16Le(data));
      return Status::kOk;
    case kJsonbUInt16:
      if (size < 2) return Overrun(top_level);
      out->kind = JsonValue::Kind::kUInt;
      out->u = ReadU16Le(data);
      return Status::kOk;
    case kJsonbInt32:
      if (size < 4) return Overrun(top_level);
      out->kind = JsonValue::Kind::kInt;
      out->i = static_cast<int32_t>(ReadU32Le(data));
      return Status::kOk;
    case kJsonbUInt32:
      if (size < 4) return Overrun(top_level);
      out->kind = JsonValue::Kind::kUInt;
      out->u = ReadU32Le(data);
      return Status::kOk;
    case kJsonbInt64:
      if (size < 8) return Overrun(top_level);
      out->kind = JsonValue::Kind::kInt;
      out->i = static_cast<int64_t>(ReadU64Le(data));
      return Status::kOk;
    case kJsonbUInt64:
      if (size < 8) return Overrun(top_level);
      out->kind = JsonValue::Kind::kUInt;
      out->u = ReadU64Le(data);
      return Status::kOk;
    case kJsonbDouble: {
      if (size < 8) return Overrun(top_level);
      const uint64_t bits = ReadU64Le(data);
      out->kind = JsonValue::Kind::kDouble;
      std::memcpy(&out->d, &bits, sizeof(out->d));
      return Status::kOk;
    }
    case kJsonbString: {
      uint32_t length = 0;
      size_t used = 0;
      const Status status = ReadVariableLength(data, size, top_level, &length, &used);
      if (status != Status::kOk) return status;
      if (length > size - used) return Overrun(top_level);
      out->kind = JsonValue::Kind::kString;
      out->bytes.assign(reinterpret_cast<const char*>(data + used), length);
      return Status::kOk;
    }
    case kJsonbOpaque: {
      if (size < 1) return Overrun(top_level);
      uint32_t length = 0;
      size_t used = 0;
      const Status status = ReadVariableLength(data + 1, size - 1, top_level, &length, &used);
      if (status != Status::kOk) return status;
      if (length > size - 1 - used) return Overrun(top_level);
      out->kind = JsonValue::Kind::kOpaque;
      out->opaque_type = static_cast<ColumnType>(data[0]);
      out->bytes.assign(reinterpret_cast<const char*>(data + 1 + used), length);
      return Status::kOk;
    }
    default:
      return Status::kMalformedValue;
  }
}

// parse_array_or_object plus Value::key / Value::element for every entry.
Status ParseContainer(bool object, bool large, const uint8_t* data, size_t size, bool top_level,
                      size_t depth, JsonValue* out) {
  if (depth > kJsonMaxDepth) return Status::kLimitExceeded;
  const size_t offset_size = large ? 4 : 2;
  const size_t key_entry_size = offset_size + 2;
  const size_t value_entry_size = offset_size + 1;
  auto read_offset = [large](const uint8_t* p) -> size_t {
    return large ? ReadU32Le(p) : ReadU16Le(p);
  };

  if (size < 2 * offset_size) return Overrun(top_level);
  const size_t count = read_offset(data);
  const size_t bytes = read_offset(data + offset_size);
  if (bytes > size) return Overrun(top_level);

  const uint64_t header_size = 2 * offset_size + uint64_t{count} * value_entry_size +
                               (object ? uint64_t{count} * key_entry_size : 0);
  if (header_size > bytes) return Status::kMalformedValue;
  const size_t key_entries = 2 * offset_size;
  const size_t value_entries = key_entries + (object ? count * key_entry_size : 0);
  const size_t first_key = static_cast<size_t>(header_size);

  JsonValue result;
  result.kind = object ? JsonValue::Kind::kObject : JsonValue::Kind::kArray;
  if (object) {
    result.members.resize(count);
  } else {
    result.elements.resize(count);
  }

  for (size_t pos = 0; pos < count; ++pos) {
    JsonValue* slot = object ? &result.members[pos].value : &result.elements[pos];
    if (object) {
      const uint8_t* entry = data + key_entries + pos * key_entry_size;
      const size_t key_offset = read_offset(entry);
      const size_t key_length = ReadU16Le(entry + offset_size);
      if (key_offset < first_key || key_offset > bytes || key_length > bytes - key_offset) {
        return Status::kMalformedValue;
      }
      result.members[pos].key.assign(reinterpret_cast<const char*>(data + key_offset), key_length);
    }

    const size_t entry_offset = value_entries + pos * value_entry_size;
    const uint8_t type = data[entry_offset];
    Status status;
    if (IsInlined(type, large)) {
      status = ParseScalar(type, data + entry_offset + 1, value_entry_size - 1, false, slot);
    } else {
      const size_t value_offset = read_offset(data + entry_offset + 1);
      if (value_offset > bytes || value_offset < entry_offset + value_entry_size) {
        return Status::kMalformedValue;
      }
      status = ParseValue(type, data + value_offset, bytes - value_offset, false, depth, slot);
    }
    if (status != Status::kOk) return status;
  }
  *out = std::move(result);
  return Status::kOk;
}

// parse_value: @p depth counts the containers enclosing this value.
Status ParseValue(uint8_t type, const uint8_t* data, size_t size, bool top_level, size_t depth,
                  JsonValue* out) {
  switch (type) {
    case kJsonbSmallObject:
      return ParseContainer(true, false, data, size, top_level, depth + 1, out);
    case kJsonbLargeObject:
      return ParseContainer(true, true, data, size, top_level, depth + 1, out);
    case kJsonbSmallArray:
      return ParseContainer(false, false, data, size, top_level, depth + 1, out);
    case kJsonbLargeArray:
      return ParseContainer(false, true, data, size, top_level, depth + 1, out);
    default:
      return ParseScalar(type, data, size, top_level, out);
  }
}

}  // namespace
}  // namespace internal

Status ParseJsonBinary(const uint8_t* data, size_t size, JsonValue* out) {
  JsonValue result;
  if (size != 0) {
    const Status status = internal::ParseValue(data[0], data + 1, size - 1, true, 0, &result);
    if (status != Status::kOk) return status;
  }
  *out = std::move(result);
  return Status::kOk;
}

Status JsonBinaryToText(const uint8_t* data, size_t size, std::string* out) {
  JsonValue value;
  const Status status = ParseJsonBinary(data, size, &value);
  if (status != Status::kOk) return status;
  return JsonToText(value, out);
}

const JsonValue* FindJsonMember(const JsonValue& object, std::string_view key) noexcept {
  if (object.kind != JsonValue::Kind::kObject) return nullptr;
  for (const JsonMember& member : object.members) {
    if (member.key == key) return &member.value;
  }
  return nullptr;
}

const char* JsonKindName(JsonValue::Kind kind) noexcept {
  switch (kind) {
    case JsonValue::Kind::kNull:
      return "kNull";
    case JsonValue::Kind::kBoolean:
      return "kBoolean";
    case JsonValue::Kind::kInt:
      return "kInt";
    case JsonValue::Kind::kUInt:
      return "kUInt";
    case JsonValue::Kind::kDouble:
      return "kDouble";
    case JsonValue::Kind::kString:
      return "kString";
    case JsonValue::Kind::kOpaque:
      return "kOpaque";
    case JsonValue::Kind::kArray:
      return "kArray";
    case JsonValue::Kind::kObject:
      return "kObject";
  }
  return "kUnknown";
}

}  // namespace mybinlog
