/**
 * @file temporal_internal.h
 * @brief Width and value of the temporal column types.
 *
 * Legacy types (DATE, TIME, DATETIME, TIMESTAMP, YEAR) are little-endian;
 * the temporal2 types are big-endian with a `(fsp + 1) / 2`-byte fraction
 * (`mysys/my_time.cc`). Values a server cannot write fail as kMalformedValue
 * instead of being clamped or masked.
 */

#ifndef MYBINLOG_TEMPORAL_INTERNAL_H_
#define MYBINLOG_TEMPORAL_INTERNAL_H_

#include <cstddef>
#include <cstdint>

#include "mybinlog/event_types.h"
#include "mybinlog/status.h"
#include "mybinlog/value.h"

namespace mybinlog {
namespace internal {

/**
 * @brief Bytes a value of a temporal column occupies.
 * @return kOk, or kInvalidMetadata for a temporal2 fsp above 6.
 */
Status TemporalWidth(ColumnType type, uint32_t meta, size_t* width) noexcept;

/**
 * @brief Decode one temporal value from exactly TemporalWidth() bytes.
 * @param[out] out Kind and temporal members set on kOk.
 * @return kOk or kMalformedValue.
 */
Status DecodeTemporal(ColumnType type, uint32_t meta, const uint8_t* data, Value* out) noexcept;

}  // namespace internal
}  // namespace mybinlog

#endif  // MYBINLOG_TEMPORAL_INTERNAL_H_
