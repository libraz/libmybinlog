/**
 * @file decimal_internal.h
 * @brief Width and decimal text of a NEWDECIMAL value (`strings/decimal.cc` bin2decimal).
 *
 * Big-endian groups of nine digits in four bytes, a leading and trailing
 * partial group sized by `dig2bytes`, the sign in the inverted top bit and
 * every byte complemented for a negative value.
 */

#ifndef MYBINLOG_DECIMAL_INTERNAL_H_
#define MYBINLOG_DECIMAL_INTERNAL_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "mybinlog/status.h"

namespace mybinlog {
namespace internal {

/**
 * @brief Encoded size of a DECIMAL column from its `precision << 8 | scale` metadata.
 * @return kOk, or kInvalidMetadata unless precision is 1..65 and scale is at
 *   most min(precision, 38) (MariaDB's ceiling; MySQL stops at 30).
 */
Status DecimalWidth(uint32_t meta, size_t* width) noexcept;

/**
 * @brief Decimal text of DecimalWidth() bytes, e.g. "-0.00" or "12.50".
 *
 * Leading integer zeros are dropped (at least "0" remains); the fraction keeps
 * every declared digit; a negative zero keeps its sign.
 *
 * @return kOk, or kMalformedValue for a digit group above its digit count.
 */
Status DecodeDecimal(uint32_t meta, const uint8_t* data, std::string* out);

}  // namespace internal
}  // namespace mybinlog

#endif  // MYBINLOG_DECIMAL_INTERNAL_H_
