/**
 * @file json_internal.h
 * @brief Binary JSON type bytes and the server's text pieces, shared by the JSON sources.
 *
 * Internal: not installed and not part of the public API.
 */

#ifndef MYBINLOG_JSON_INTERNAL_H_
#define MYBINLOG_JSON_INTERNAL_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace mybinlog {
namespace internal {

/// @name Binary JSON value types (JSONB_TYPE_*, `sql-common/json_binary.cc`)
/// @{
inline constexpr uint8_t kJsonbSmallObject = 0x00;
inline constexpr uint8_t kJsonbLargeObject = 0x01;
inline constexpr uint8_t kJsonbSmallArray = 0x02;
inline constexpr uint8_t kJsonbLargeArray = 0x03;
inline constexpr uint8_t kJsonbLiteral = 0x04;
inline constexpr uint8_t kJsonbInt16 = 0x05;
inline constexpr uint8_t kJsonbUInt16 = 0x06;
inline constexpr uint8_t kJsonbInt32 = 0x07;
inline constexpr uint8_t kJsonbUInt32 = 0x08;
inline constexpr uint8_t kJsonbInt64 = 0x09;
inline constexpr uint8_t kJsonbUInt64 = 0x0A;
inline constexpr uint8_t kJsonbDouble = 0x0B;
inline constexpr uint8_t kJsonbString = 0x0C;
inline constexpr uint8_t kJsonbOpaque = 0x0F;
/// @}

/// @name Literal bytes (JSONB_*_LITERAL)
/// @{
inline constexpr uint8_t kJsonbNullLiteral = 0x00;
inline constexpr uint8_t kJsonbTrueLiteral = 0x01;
inline constexpr uint8_t kJsonbFalseLiteral = 0x02;
/// @}

/**
 * @brief Append a finite double as `my_gcvt(MY_GCVT_ARG_DOUBLE, MY_GCVT_MAX_FIELD_WIDTH)`
 *        followed by the ".0" the server's JSON printer adds to integral values.
 */
/**
 * @brief Append a double the way the server's `my_gcvt` writes it: fixed or
 *   exponent form by the same width rule, no trailing ".0".
 * @param precision Significant digits (6 for FLOAT, as `my_gcvt` with
 *   MY_GCVT_ARG_FLOAT keeps); 0 for the shortest round-trip digits (DOUBLE).
 */
void AppendGcvt(double value, int precision, std::string* out);

/** @brief AppendGcvt() with shortest digits, plus ".0" when the text has no '.' or 'e'. */
void AppendJsonDouble(double value, std::string* out);

/** @brief Append @p data double-quoted with the server's JSON escapes (`double_quote`). */
void AppendJsonQuoted(const char* data, size_t size, std::string* out);

}  // namespace internal
}  // namespace mybinlog

#endif  // MYBINLOG_JSON_INTERNAL_H_
