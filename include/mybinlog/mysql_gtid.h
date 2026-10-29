/**
 * @file mysql_gtid.h
 * @brief MySQL GTID sets (text and binary) and GTID event bodies.
 *
 * A GTID names one transaction as UUID, optional tag, and GNO, with
 * 1 <= GNO < INT64_MAX (upstream GNO_END). Sets map each TSID (UUID + tag) to
 * sorted, merged intervals with exclusive ends, the form the binary encoding
 * uses. Text "uuid:5" and "uuid:tag:5" parse as the single GTID 5; widening a
 * bare N to "1-N" is a resume convention left to each product. Tags follow
 * upstream: lower-cased, `[a-z_][a-z0-9_]{0,31}`.
 */

#ifndef MYBINLOG_MYSQL_GTID_H_
#define MYBINLOG_MYSQL_GTID_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "mybinlog/event_header.h"
#include "mybinlog/status.h"

namespace mybinlog {

/** @brief Raw 16-byte server UUID. */
using Uuid = std::array<uint8_t, 16>;

/** @brief Longest GTID tag. */
inline constexpr size_t kGtidTagMaxLength = 32;

/** @brief Exclusive upper bound of a GNO (upstream GNO_END = INT64_MAX). */
inline constexpr uint64_t kGnoEnd = 0x7FFFFFFFFFFFFFFFULL;

/**
 * @brief Validate a tag and write its lower-cased form.
 * @return kOk, or kMalformedValue when @p text is not a tag (empty included).
 */
Status NormalizeGtidTag(std::string_view text, std::string* out);

/**
 * @brief Parse an 8-4-4-4-12 hex UUID, either case, no surrounding whitespace.
 * @return kOk, or kMalformedValue.
 */
Status ParseUuid(std::string_view text, Uuid* out);

/** @brief Lower-case 8-4-4-4-12 text of @p uuid. */
std::string FormatUuid(const Uuid& uuid);

/** @brief Canonical MySQL GTID set, for resume positions and checkpoints. */
class GtidSet {
 public:
  /** @brief Transaction source: UUID plus tag (empty when untagged). */
  struct Tsid {
    Uuid uuid{};
    std::string tag;

    bool operator<(const Tsid& other) const {
      return uuid < other.uuid || (uuid == other.uuid && tag < other.tag);
    }
  };

  /** @brief GNO range [start, end). */
  struct Interval {
    uint64_t start = 0;
    uint64_t end = 0;
  };

  /** @brief Most intervals one TSID may hold; bounds memory for untrusted input. */
  static constexpr size_t kMaxIntervalsPerTsid = 65536;

  /**
   * @brief Parse set text as MySQL prints it ("uuid:1-3:7:tag:1-4,uuid2:5").
   *
   * Only "" is the empty set. Empty entries between commas are skipped, but
   * text holding no entry at all (",", whitespace) is rejected, as is any empty
   * colon piece, a tag without intervals, or a GNO outside [1, INT64_MAX).
   *
   * @return kOk; kMalformedValue for malformed text; kLimitExceeded when a TSID
   *   exceeds kMaxIntervalsPerTsid. @p out is untouched on failure.
   */
  static Status Parse(std::string_view text, GtidSet* out);

  /**
   * @brief Decode the binary encoding (PREVIOUS_GTIDS body, format 0 or 1).
   * @return kOk; kTruncated when the declared counts do not fit; kUnsupported
   *   for an unknown format; kMalformedValue for an invalid interval, tag or
   *   trailing bytes; kLimitExceeded past kMaxIntervalsPerTsid.
   */
  static Status DecodeBinary(const uint8_t* data, size_t len, GtidSet* out);

  /**
   * @brief Encode for COM_BINLOG_DUMP_GTID: format 0 when no TSID has a tag,
   *   format 1 (one-byte varlen tag length per TSID) otherwise.
   */
  void EncodeBinary(std::vector<uint8_t>* out) const;

  /** @brief Union @p other into this set; unchanged on failure (kLimitExceeded). */
  Status Merge(const GtidSet& other);

  /**
   * @brief Add one interval to a TSID.
   * @return kOk; kMalformedValue for an invalid tag or interval; kLimitExceeded.
   */
  Status Add(const Uuid& uuid, std::string_view tag, Interval interval);

  /** @brief Whether every interval of this set lies in @p other under the same TSID. */
  bool IsSubsetOf(const GtidSet& other) const;

  /** @brief Whether the set holds no GTID. */
  bool empty() const { return sets_.empty(); }

  /** @brief Remove every GTID. */
  void Clear() { sets_.clear(); }

  /**
   * @brief Canonical text: one entry per TSID, ranges always as "a-b".
   *
   * A single GTID prints as "N-N" so the text never reads as a bare N.
   */
  std::string ToString() const;

 private:
  std::map<Tsid, std::vector<Interval>> sets_;
};

/** @brief GTID carried by a GTID_LOG_EVENT (33) or GTID_TAGGED_LOG_EVENT (42). */
struct GtidEvent {
  uint8_t flags = 0;  ///< gtid_flags (bit 0: may have been committed in parallel).
  Uuid uuid{};
  std::string tag;  ///< Lower-cased; empty when untagged.
  uint64_t gno = 0;

  /** @brief "uuid:gno" or "uuid:tag:gno". */
  std::string ToString() const;
};

/**
 * @brief Parse the body of a GTID event.
 *
 * Type 33 is fixed: flags(1) uuid(16) gno(8 LE). Type 42 is a serialization
 * message: varlen format version (1), varlen message size, varlen
 * last_non_ignorable_field_id, then fields each prefixed by its varlen id.
 * Fields 0-3 (flags, uuid, gno, tag) are read; the remaining known fields are
 * skipped to the message end. A message naming a non-ignorable field beyond
 * the known ones is refused.
 *
 * @param header Header of the event; selects the layout.
 * @param body Bytes after the common header.
 * @param len Body length, trailer excluded.
 * @return kOk; kTruncated when the body ends early; kMalformedValue for a GNO
 *   outside [1, INT64_MAX), a bad tag, a missing UUID/GNO field or a size
 *   mismatch; kUnsupported for another event type, an unknown format version
 *   or an unknown non-ignorable field.
 */
Status ParseGtidEvent(const EventHeader& header, const uint8_t* body, size_t len, GtidEvent* out);

}  // namespace mybinlog

#endif  // MYBINLOG_MYSQL_GTID_H_
