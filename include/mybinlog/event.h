/**
 * @file event.h
 * @brief One typed binlog event as Decoder::Next() returns it.
 *
 * Event::body is a std::variant over the parsed bodies; read it with
 * `std::get_if<T>(&event.body)` (no exceptions involved). Any event type
 * without a parsed form below, statement-based and informational events
 * included, arrives as UnknownEvent with its raw body, so no event is skipped
 * silently.
 *
 * Lifetime: pointers inside an Event (UnknownEvent::body,
 * TableRowsEvent::table, TableRowsEvent::body) point into the Decoder that
 * produced it and stay valid until that decoder's next Next(), Feed() or
 * Reset(). Everything else in an Event is owned by it.
 */

#ifndef MYBINLOG_EVENT_H_
#define MYBINLOG_EVENT_H_

#include <cstddef>
#include <cstdint>
#include <variant>

#include "mybinlog/disposition.h"
#include "mybinlog/event_header.h"
#include "mybinlog/format_description.h"
#include "mybinlog/heartbeat.h"
#include "mybinlog/mariadb_gtid.h"
#include "mybinlog/mysql_gtid.h"
#include "mybinlog/query_event.h"
#include "mybinlog/rotate.h"
#include "mybinlog/row_decoder.h"
#include "mybinlog/table_map.h"
#include "mybinlog/xid.h"

namespace mybinlog {

/** @brief Body of an event type without a parsed form here. */
struct UnknownEvent {
  /** Body bytes after the common header, trailer excluded; owned by the Decoder. */
  const uint8_t* body = nullptr;
  size_t size = 0;  ///< Bytes at #body.
};

/**
 * @brief A WRITE/UPDATE/DELETE_ROWS or PARTIAL_UPDATE_ROWS event and its table.
 *
 * MariaDB's compressed rows events arrive here expanded, with Event::header
 * describing the plain event.
 */
struct TableRowsEvent {
  uint64_t table_id = 0;  ///< Table id from the post-header.
  uint16_t flags = 0;     ///< Rows-event flags; see kRowsStatementEndFlag.
  /**
   * The TABLE_MAP the table id refers to, held by the Decoder; null only for
   * kDummyTableId without a TABLE_MAP of that id.
   */
  const TableMap* table = nullptr;
  /** Whether #rows holds the decoded images (DecoderOptions::decode_rows and a #table). */
  bool decoded = false;
  RowsEvent rows;  ///< The row images when #decoded; empty otherwise.
  /** Body bytes after the common header, trailer excluded; owned by the Decoder. */
  const uint8_t* body = nullptr;
  size_t size = 0;  ///< Bytes at #body.
};

/**
 * @brief Parsed body of an event; the alternative follows the header's type code.
 *
 * | Type code | Alternative |
 * |---|---|
 * | FORMAT_DESCRIPTION (15) | FormatDescriptionEvent |
 * | ROTATE (4) | RotateEvent |
 * | TABLE_MAP (19) | TableMap |
 * | rows events (23-25, 30-32, 39; MariaDB 166-171 expanded) | TableRowsEvent |
 * | QUERY (2; MariaDB 165 expanded) | QueryEvent |
 * | XID (16) | XidEvent |
 * | GTID (33), GTID_TAGGED (42) | GtidEvent |
 * | PREVIOUS_GTIDS (35) | PreviousGtidsEvent |
 * | MariaDB GTID (162) | MariaDbGtidEvent |
 * | MariaDB GTID_LIST (163) | MariaDbGtidListEvent |
 * | MariaDB ANNOTATE_ROWS (160) | MariaDbAnnotateRowsEvent |
 * | HEARTBEAT (27), HEARTBEAT_V2 (41) | HeartbeatEvent |
 * | anything else | UnknownEvent |
 */
using EventBody =
    std::variant<UnknownEvent, FormatDescriptionEvent, RotateEvent, TableMap, TableRowsEvent,
                 QueryEvent, XidEvent, GtidEvent, PreviousGtidsEvent, MariaDbGtidEvent,
                 MariaDbGtidListEvent, MariaDbAnnotateRowsEvent, HeartbeatEvent>;

/** @brief One decoded event. */
struct Event {
  /** Common header; for an expanded MariaDB compressed event, the plain event's header. */
  EventHeader header;
  EventBody body;  ///< Parsed body; see EventBody for which alternative a type takes.
  /** ClassifyEvent() of the header under the flavor the stream's FDE announced. */
  EventDisposition disposition;
  /** The event came out of a TRANSACTION_PAYLOAD; #offset is then the payload's. */
  bool from_payload = false;
  /** Stream offset of the event's first byte (the binlog position, for a file). */
  uint64_t offset = 0;
};

}  // namespace mybinlog

#endif  // MYBINLOG_EVENT_H_
