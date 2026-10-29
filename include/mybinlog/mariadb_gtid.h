/**
 * @file mariadb_gtid.h
 * @brief MariaDB GTIDs, GTID positions, and the MariaDB-only events carrying them.
 *
 * A MariaDB GTID is "domain-server-seq". A position is a list of GTIDs, at most
 * one meaningful per domain (the highest seq); it is not an interval set, so it
 * is a separate type from GtidSet. Sequence numbers are unsigned 64-bit and 0
 * is a valid value.
 */

#ifndef MYBINLOG_MARIADB_GTID_H_
#define MYBINLOG_MARIADB_GTID_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mybinlog/event_header.h"
#include "mybinlog/status.h"

namespace mybinlog {

/** @brief One MariaDB GTID. */
struct MariaDbGtid {
  uint32_t domain_id = 0;
  uint32_t server_id = 0;
  uint64_t sequence_no = 0;

  /**
   * @brief Parse "domain-server-seq"; surrounding whitespace is allowed.
   * @return kOk, or kMalformedValue (wrong segment count, non-digits, overflow).
   */
  static Status Parse(std::string_view text, MariaDbGtid* out);

  /** @brief Whether @p text parses as a single MariaDB GTID. */
  static bool IsMariaDbGtidFormat(std::string_view text);

  /** @brief "domain-server-seq". */
  std::string ToString() const;

  bool operator==(const MariaDbGtid& other) const {
    return domain_id == other.domain_id && server_id == other.server_id &&
           sequence_no == other.sequence_no;
  }
  bool operator!=(const MariaDbGtid& other) const { return !(*this == other); }
};

/** @brief A MariaDB GTID position ("0-1-42,1-2-100"), kept in text order. */
struct MariaDbGtidPosition {
  std::vector<MariaDbGtid> gtids;

  /**
   * @brief Parse a comma-separated list.
   *
   * Only "" is the empty position. Empty entries between commas are skipped,
   * but text holding no GTID at all (",", whitespace) is rejected.
   *
   * @return kOk, or kMalformedValue; @p out is untouched on failure.
   */
  static Status Parse(std::string_view text, MariaDbGtidPosition* out);

  /** @brief Comma-separated text, in list order. */
  std::string ToString() const;

  bool empty() const { return gtids.empty(); }
};

/**
 * @brief Whether @p available has reached @p required in every domain.
 *
 * Compares the highest seq per domain. Server ids are ignored: they change
 * legitimately on failover.
 */
bool PositionCovers(const MariaDbGtidPosition& required, const MariaDbGtidPosition& available);

/** @brief MARIADB_GTID_EVENT (162) contents. */
struct MariaDbGtidEvent {
  MariaDbGtid gtid;   ///< server_id comes from the common header.
  uint8_t flags = 0;  ///< flags2 (FL_STANDALONE, FL_GROUP_COMMIT_ID, ...).

  /** @brief FL_STANDALONE: the group ends without COMMIT/XID. */
  bool standalone() const { return (flags & 0x01) != 0; }
};

/**
 * @brief Parse a MARIADB_GTID_EVENT body: seq(8 LE) domain(4 LE) flags2(1) ...
 * @return kOk, or kTruncated below 13 bytes.
 */
Status ParseMariaDbGtidEvent(const EventHeader& header, const uint8_t* body, size_t len,
                             MariaDbGtidEvent* out);

/**
 * @brief Parse a MARIADB_GTID_LIST_EVENT body: count_and_flags(4 LE, low 28
 *   bits count) then count x {domain(4) server(4) seq(8)}. Bytes after the
 *   entries are ignored, as the server does.
 * @return kOk, or kTruncated.
 */
Status ParseGtidListEvent(const uint8_t* body, size_t len, MariaDbGtidPosition* out);

/**
 * @brief The SQL text of a MARIADB_ANNOTATE_ROWS_EVENT body, bytes verbatim.
 * @return kOk, or kTruncated for an empty body.
 */
Status ExtractAnnotateRows(const uint8_t* body, size_t len, std::string* out);

}  // namespace mybinlog

#endif  // MYBINLOG_MARIADB_GTID_H_
