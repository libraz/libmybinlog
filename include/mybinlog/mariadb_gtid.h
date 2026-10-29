/**
 * @file mariadb_gtid.h
 * @brief MariaDB GTIDs, GTID positions, and the MariaDB-only events carrying them.
 *
 * A MariaDB GTID is "domain-server-seq". A position is a list of GTIDs, at most
 * one meaningful per domain (the highest seq); it is not an interval set, so it
 * is a separate type from GtidSet. Sequence numbers are unsigned 64-bit and 0
 * is a valid value.
 *
 * Thread safety: the free functions and static members are reentrant; a
 * MariaDbGtidPosition may be read concurrently but not mutated concurrently.
 */

#ifndef MYBINLOG_MARIADB_GTID_H_
#define MYBINLOG_MARIADB_GTID_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mybinlog/event_header.h"
#include "mybinlog/export.h"
#include "mybinlog/status.h"

namespace mybinlog {

/** @brief One MariaDB GTID. */
struct MYBINLOG_API MariaDbGtid {
  uint32_t domain_id = 0;    ///< Replication domain.
  uint32_t server_id = 0;    ///< Server that originated the transaction.
  uint64_t sequence_no = 0;  ///< Sequence number within the domain.

  /**
   * @brief Parse "domain-server-seq"; surrounding whitespace is allowed.
   * @param text GTID text; only read during the call.
   * @param[out] out Filled on kOk; untouched otherwise.
   * @return kOk, or kMalformedValue (wrong segment count, non-digits, overflow).
   */
  [[nodiscard]] static Status Parse(std::string_view text, MariaDbGtid* out);

  /**
   * @brief Whether text parses as a single MariaDB GTID.
   * @param text Candidate text; only read during the call.
   * @return true when Parse() would return kOk.
   */
  [[nodiscard]] static bool IsMariaDbGtidFormat(std::string_view text);

  /** @return "domain-server-seq". */
  [[nodiscard]] std::string ToString() const;

  /** @return Whether all three fields are equal. */
  [[nodiscard]] bool operator==(const MariaDbGtid& other) const {
    return domain_id == other.domain_id && server_id == other.server_id &&
           sequence_no == other.sequence_no;
  }
  /** @return Whether any field differs. */
  [[nodiscard]] bool operator!=(const MariaDbGtid& other) const { return !(*this == other); }
};

/** @brief A MariaDB GTID position ("0-1-42,1-2-100"), kept in text order. */
struct MYBINLOG_API MariaDbGtidPosition {
  std::vector<MariaDbGtid> gtids;  ///< Entries in text (or wire) order.

  /**
   * @brief Parse a comma-separated list.
   *
   * Only "" is the empty position. Empty entries between commas are skipped,
   * but text holding no GTID at all (",", whitespace) is rejected.
   *
   * @param text Position text; only read during the call.
   * @param[out] out Filled on kOk; untouched otherwise.
   * @return kOk, or kMalformedValue.
   */
  [[nodiscard]] static Status Parse(std::string_view text, MariaDbGtidPosition* out);

  /** @return Comma-separated text, in list order. */
  [[nodiscard]] std::string ToString() const;

  /** @return Whether the position holds no GTID. */
  [[nodiscard]] bool empty() const { return gtids.empty(); }
};

/**
 * @brief Whether one position has reached another in every domain.
 *
 * Compares the highest seq per domain. Server ids are ignored: they change
 * legitimately on failover.
 *
 * @param required Position that must be covered.
 * @param available Position checked against it.
 * @return true when every domain of @p required is at or below @p available. Reentrant.
 */
[[nodiscard]] MYBINLOG_API bool PositionCovers(const MariaDbGtidPosition& required,
                                               const MariaDbGtidPosition& available);

/** @brief Parsed MARIADB_GTID_EVENT (162). */
struct MariaDbGtidEvent {
  MariaDbGtid gtid;   ///< server_id comes from the common header.
  uint8_t flags = 0;  ///< flags2 (FL_STANDALONE, FL_GROUP_COMMIT_ID, ...).

  /** @return FL_STANDALONE: the group ends without COMMIT/XID. */
  [[nodiscard]] bool standalone() const { return (flags & 0x01) != 0; }
};

/**
 * @brief Parse a MARIADB_GTID_EVENT body: seq(8 LE) domain(4 LE) flags2(1) ...
 * @param header Header of the event; supplies the GTID's server_id.
 * @param body Bytes after the common header.
 * @param size Body length, checksum trailer excluded.
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return kOk, or kTruncated when @p body is null or below 13 bytes. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParseMariaDbGtidEvent(const EventHeader& header,
                                                        const uint8_t* body, size_t size,
                                                        MariaDbGtidEvent* out);

/** @brief Parsed MARIADB_GTID_LIST_EVENT (163). */
struct MariaDbGtidListEvent {
  MariaDbGtidPosition gtids;  ///< The binlog state at the start of the file.
  uint8_t flags = 0;          ///< Top 4 bits of count_and_flags, shifted down.
};

/**
 * @brief Parse a MARIADB_GTID_LIST_EVENT body.
 *
 * count_and_flags(4 LE: low 28 bits count, high 4 flags), then count x
 * {domain(4) server(4) seq(8)}. Bytes after the entries are ignored, as the
 * server does.
 *
 * @param header Header of the event; not consulted by the current layout.
 * @param body Bytes after the common header.
 * @param size Body length, checksum trailer excluded.
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return kOk, or kTruncated when @p body is null or the entries do not fit.
 *   Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParseMariaDbGtidListEvent(const EventHeader& header,
                                                            const uint8_t* body, size_t size,
                                                            MariaDbGtidListEvent* out);

/** @brief Parsed MARIADB_ANNOTATE_ROWS_EVENT (160). */
struct MariaDbAnnotateRowsEvent {
  std::string query;  ///< Statement that produced the following rows events, bytes verbatim.
};

/**
 * @brief Parse a MARIADB_ANNOTATE_ROWS_EVENT body: the statement text.
 * @param header Header of the event; not consulted by the current layout.
 * @param body Bytes after the common header.
 * @param size Body length, checksum trailer excluded.
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return kOk, or kTruncated when @p body is null or empty. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParseMariaDbAnnotateRowsEvent(const EventHeader& header,
                                                                const uint8_t* body, size_t size,
                                                                MariaDbAnnotateRowsEvent* out);

}  // namespace mybinlog

#endif  // MYBINLOG_MARIADB_GTID_H_
