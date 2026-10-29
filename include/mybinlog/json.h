/**
 * @file json.h
 * @brief MySQL binary JSON: decoding to a DOM and printing it as the server does.
 *
 * A MySQL JSON column (Value::Kind::kJson) holds the server's binary format
 * (`sql-common/json_binary.h`). ParseJsonBinary() turns it into a JsonValue
 * tree, and JsonToText() prints that tree byte for byte as the server's
 * `CAST(j AS CHAR)`: ", " and ": " separators, object members in stored order
 * (keys sorted by length, then bytes), doubles in their shortest round-trip
 * form, DATE/TIME/DATETIME/TIMESTAMP as quoted strings, DECIMAL unquoted, and
 * any other opaque value as `"base64:typeNN:..."`. MariaDB stores JSON as
 * text and needs none of this.
 *
 * Every function is reentrant and keeps no state; a JsonValue is a plain value
 * owning all of its bytes.
 */

#ifndef MYBINLOG_JSON_H_
#define MYBINLOG_JSON_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mybinlog/event_types.h"
#include "mybinlog/export.h"
#include "mybinlog/status.h"

namespace mybinlog {

/** @brief Deepest array/object nesting accepted (upstream JSON_DOCUMENT_MAX_DEPTH). */
inline constexpr size_t kJsonMaxDepth = 100;

struct JsonMember;

/**
 * @brief One node of a decoded JSON document.
 *
 * Only the members named for #kind are meaningful; the rest keep their
 * defaults. Copying copies the whole subtree.
 */
struct JsonValue {
  /** @brief Shape of the node. */
  enum class Kind : uint8_t {
    kNull,     ///< The null literal.
    kBoolean,  ///< true or false in #b.
    kInt,      ///< Signed integer in #i (binary int16, int32 or int64).
    kUInt,     ///< Unsigned integer in #u (binary uint16, uint32 or uint64).
    kDouble,   ///< Double in #d.
    kString,   ///< utf8mb4 string in #bytes.
    kOpaque,   ///< Any other MySQL value: field type in #opaque_type, payload in #bytes.
    kArray,    ///< Elements in #elements.
    kObject,   ///< Members in #members, in stored order.
  };

  Kind kind = Kind::kNull;  ///< Which members below are meaningful.
  bool b = false;           ///< kBoolean.
  int64_t i = 0;            ///< kInt.
  uint64_t u = 0;           ///< kUInt.
  double d = 0.0;           ///< kDouble.
  /**
   * kOpaque: the payload's `enum_field_types` code. It may lie outside the
   * ColumnType enumerators. kDate, kTime, kDatetime and kTimestamp carry an
   * 8-byte packed temporal; kNewDecimal carries precision, scale and the
   * binary decimal.
   */
  ColumnType opaque_type = ColumnType::kNewDecimal;
  std::string bytes;                ///< kString, kOpaque.
  std::vector<JsonValue> elements;  ///< kArray.
  std::vector<JsonMember> members;  ///< kObject.
};

/** @brief One object member. */
struct JsonMember {
  std::string key;  ///< utf8mb4 key bytes.
  JsonValue value;  ///< Member value.
};

/**
 * @brief Decode one MySQL binary JSON document.
 *
 * Small (16-bit offsets) and large (32-bit offsets) containers and inlined
 * scalars are accepted; every offset, key range and length is bounds-checked.
 * Bytes after a complete top-level value are ignored, as the server does.
 *
 * @param data Binary JSON; read only during the call. May be null when @p size is 0.
 * @param size Byte count. Zero is the JSON null literal (the server stores an
 *   empty value for NULL forced into a NOT NULL column).
 * @param[out] out Whole document on kOk; untouched otherwise.
 * @return kOk; kTruncated if the input ends inside the top-level value;
 *   kMalformedValue for an unknown type or literal, an offset, key or length
 *   outside its container, or an ill-formed length prefix; kLimitExceeded when
 *   containers nest deeper than kJsonMaxDepth. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParseJsonBinary(const uint8_t* data, size_t size, JsonValue* out);

/**
 * @brief Print a document as the server's `CAST(j AS CHAR)`.
 *
 * Strings escape `"`, `\` and every byte below 0x20 (`\b \t \n \f \r`, else
 * `\u00XX`); other bytes, including non-ASCII, pass through. A double prints
 * its shortest round-trip digits (`my_gcvt`): fixed notation from 1e-15 up to
 * 1e15 (and beyond when every digit is still significant), exponent notation
 * such as `1.5e300` or `1e-300` otherwise, and ".0" appended when neither a
 * point nor an exponent appears.
 *
 * @param value Document to print.
 * @param[out] out Replaced with the text on kOk; untouched otherwise.
 * @return kOk; kMalformedValue for a packed temporal that is not 8 bytes or
 *   holds an impossible date or time, a DECIMAL whose payload does not match
 *   its precision and scale, or a non-finite double; kLimitExceeded when
 *   containers nest deeper than kJsonMaxDepth. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status JsonToText(const JsonValue& value, std::string* out);

/**
 * @brief ParseJsonBinary() followed by JsonToText().
 * @param data Binary JSON; read only during the call. May be null when @p size is 0.
 * @param size Byte count; zero prints "null".
 * @param[out] out Replaced with the text on kOk; untouched otherwise.
 * @return Any status of the two steps. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status JsonBinaryToText(const uint8_t* data, size_t size,
                                                   std::string* out);

/**
 * @brief First member of an object with the given key.
 * @param object Node to search; any kind other than kObject finds nothing.
 * @param key Key bytes; read only during the call.
 * @return Pointer into @p object, valid while it is neither modified nor
 *   destroyed; null when absent. Reentrant.
 */
[[nodiscard]] MYBINLOG_API const JsonValue* FindJsonMember(const JsonValue& object,
                                                           std::string_view key) noexcept;

/**
 * @brief Stable identifier of a JSON node kind, for logs and messages.
 * @param kind Any value, including one outside the enumerators.
 * @return Static string such as "kObject" ("kUnknown" outside the
 *   enumerators); never null. Reentrant.
 */
[[nodiscard]] MYBINLOG_API const char* JsonKindName(JsonValue::Kind kind) noexcept;

}  // namespace mybinlog

#endif  // MYBINLOG_JSON_H_
