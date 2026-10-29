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
 *
 * Thread safety: the free functions and static members are reentrant; a
 * GtidSet may be read concurrently but not mutated concurrently.
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
#include "mybinlog/export.h"
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
 * @param text Candidate tag; only read during the call.
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return kOk, or kMalformedValue when @p text is not a tag (empty included).
 */
[[nodiscard]] MYBINLOG_API Status NormalizeGtidTag(std::string_view text, std::string* out);

/**
 * @brief Parse an 8-4-4-4-12 hex UUID, either case, no surrounding whitespace.
 * @param text UUID text; only read during the call.
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return kOk, or kMalformedValue.
 */
[[nodiscard]] MYBINLOG_API Status ParseUuid(std::string_view text, Uuid* out);

/**
 * @brief Format a UUID.
 * @param uuid Raw UUID.
 * @return Lower-case 8-4-4-4-12 text.
 */
[[nodiscard]] MYBINLOG_API std::string FormatUuid(const Uuid& uuid);

/** @brief Canonical MySQL GTID set, for resume positions and checkpoints. */
class MYBINLOG_API GtidSet {
 public:
  /** @brief Transaction source: UUID plus tag (empty when untagged). */
  struct Tsid {
    Uuid uuid{};      ///< Source server UUID.
    std::string tag;  ///< Lower-cased tag; empty when untagged.

    /** @return Ordering by UUID, then tag. */
    [[nodiscard]] bool operator<(const Tsid& other) const {
      return uuid < other.uuid || (uuid == other.uuid && tag < other.tag);
    }
  };

  /** @brief GNO range [start, end). */
  struct Interval {
    uint64_t start = 0;  ///< First GNO in the range.
    uint64_t end = 0;    ///< One past the last GNO.
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
   * @param text Set text; only read during the call.
   * @param[out] out Filled on kOk; untouched otherwise.
   * @return kOk; kMalformedValue for malformed text; kLimitExceeded when a TSID
   *   exceeds kMaxIntervalsPerTsid.
   */
  [[nodiscard]] static Status Parse(std::string_view text, GtidSet* out);

  /**
   * @brief Decode the binary encoding (PREVIOUS_GTIDS body, format 0 or 1).
   * @param data First byte of the encoding.
   * @param size Bytes of the encoding; all must be consumed.
   * @param[out] out Filled on kOk; untouched otherwise.
   * @return kOk; kTruncated when the declared counts do not fit (or @p data is
   *   null); kUnsupported for an unknown format; kMalformedValue for an invalid
   *   interval, tag or trailing bytes; kLimitExceeded past kMaxIntervalsPerTsid.
   */
  [[nodiscard]] static Status DecodeBinary(const uint8_t* data, size_t size, GtidSet* out);

  /**
   * @brief Encode for COM_BINLOG_DUMP_GTID: format 0 when no TSID has a tag,
   *   format 1 (one-byte varlen tag length per TSID) otherwise.
   * @param[out] out Replaced with the encoding.
   */
  void EncodeBinary(std::vector<uint8_t>* out) const;

  /**
   * @brief Union another set into this one.
   * @param other Set to add.
   * @return kOk, or kLimitExceeded, leaving this set unchanged.
   */
  [[nodiscard]] Status Merge(const GtidSet& other);

  /**
   * @brief Add one interval to a TSID.
   * @param uuid Source UUID.
   * @param tag Tag, normalized before use; empty for untagged. Only read during the call.
   * @param interval GNO range [start, end).
   * @return kOk; kMalformedValue for an invalid tag or interval; kLimitExceeded.
   */
  [[nodiscard]] Status Add(const Uuid& uuid, std::string_view tag, Interval interval);

  /**
   * @brief Whether every interval of this set lies in @p other under the same TSID.
   * @param other Candidate superset.
   */
  [[nodiscard]] bool IsSubsetOf(const GtidSet& other) const;

  /** @return Whether the set holds no GTID. */
  [[nodiscard]] bool empty() const { return sets_.empty(); }

  /** @brief Remove every GTID. */
  void Clear() { sets_.clear(); }

  /**
   * @brief Canonical text: one entry per TSID, ranges always as "a-b".
   *
   * A single GTID prints as "N-N" so the text never reads as a bare N.
   *
   * @return The set text; "" for the empty set.
   */
  [[nodiscard]] std::string ToString() const;

 private:
  std::map<Tsid, std::vector<Interval>> sets_;
};

/** @brief Parsed GTID_LOG_EVENT (33) or GTID_TAGGED_LOG_EVENT (42). */
struct MYBINLOG_API GtidEvent {
  uint8_t flags = 0;  ///< gtid_flags (bit 0: may have been committed in parallel).
  Uuid uuid{};        ///< Source server UUID.
  std::string tag;    ///< Lower-cased; empty when untagged.
  uint64_t gno = 0;   ///< Transaction number, 1 <= gno < kGnoEnd.

  /** @return "uuid:gno" or "uuid:tag:gno". */
  [[nodiscard]] std::string ToString() const;
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
 * @param size Body length, checksum trailer excluded.
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return kOk; kTruncated when the body ends early (or @p body is null);
 *   kMalformedValue for a GNO outside [1, INT64_MAX), a bad tag, a missing
 *   UUID/GNO field or a size mismatch; kUnsupported for another event type,
 *   an unknown format version or an unknown non-ignorable field. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParseGtidEvent(const EventHeader& header, const uint8_t* body,
                                                 size_t size, GtidEvent* out);

/** @brief Parsed PREVIOUS_GTIDS_LOG_EVENT (35). */
struct PreviousGtidsEvent {
  GtidSet gtids;  ///< GTIDs written to the binlog before this file.
};

/**
 * @brief Parse a PREVIOUS_GTIDS_LOG_EVENT body: a GtidSet in binary encoding.
 * @param header Header of the event; not consulted by the current layout.
 * @param body Bytes after the common header.
 * @param size Body length, checksum trailer excluded.
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return As GtidSet::DecodeBinary(). Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParsePreviousGtidsEvent(const EventHeader& header,
                                                          const uint8_t* body, size_t size,
                                                          PreviousGtidsEvent* out);

}  // namespace mybinlog

#endif  // MYBINLOG_MYSQL_GTID_H_
