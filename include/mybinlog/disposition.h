/**
 * @file disposition.h
 * @brief How a reader may treat each binlog event type.
 *
 * A reader decodes an event, skips it because it is provably data-neutral, or
 * stops. ClassifyEvent() records that decision in one switch with no default,
 * so a new EventType enumerator is a compile error (-Werror=switch) until it is
 * classified. Codes outside EventType are data-neutral only when the source
 * marked them ignorable; otherwise they fail closed.
 */

#ifndef MYBINLOG_DISPOSITION_H_
#define MYBINLOG_DISPOSITION_H_

#include <cstdint>

#include "mybinlog/event_types.h"
#include "mybinlog/server_flavor.h"

namespace mybinlog {

/** @brief What a reader does with an event. */
enum class Disposition : uint8_t {
  kFraming,   ///< Stream framing (FORMAT_DESCRIPTION, ROTATE, STOP); consumed by the reader.
  kTableMap,  ///< TABLE_MAP; decoded into a table description.
  kRows,      ///< Uncompressed rows event; decoded and applied.
  kGtid,      ///< Position event (MySQL GTID / tagged GTID, MariaDB GTID / GTID_LIST).
  kTransactionBoundary,  ///< XID; commits the open transaction.
  kQuery,                ///< QUERY; BEGIN, DDL or statement text.
  kDataNeutral,          ///< Informational; skipping it cannot lose a row or a position.
  kFailClosed,           ///< Cannot be applied and cannot be proven harmless to skip: stop.
};

/** @brief A disposition and the static reason it was chosen. */
struct EventDisposition {
  Disposition disposition;
  const char* reason;  ///< Static string, never null.
};

/** @brief Stable identifier of @p disposition (e.g. "kFailClosed"). */
const char* DispositionName(Disposition disposition) noexcept;

/**
 * @brief Classify a known event type.
 *
 * MariaDB-numbered types on a MySQL source, and MySQL GTID types (33, 42) on a
 * MariaDB source, fail closed: that server does not write them.
 */
EventDisposition ClassifyEvent(EventType type, ServerFlavor flavor) noexcept;

/**
 * @brief Classify a raw type code from an event header.
 *
 * @param type_code Header byte 4.
 * @param flags Header flags (bytes 17-18); only consulted for codes outside EventType.
 * @param flavor Source server flavor.
 */
EventDisposition ClassifyEvent(uint8_t type_code, uint16_t flags, ServerFlavor flavor) noexcept;

}  // namespace mybinlog

#endif  // MYBINLOG_DISPOSITION_H_
