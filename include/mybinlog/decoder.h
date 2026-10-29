/**
 * @file decoder.h
 * @brief High-level pull decoder: feed binlog bytes, take typed events.
 *
 * ```cpp
 * mybinlog::Decoder decoder;
 * decoder.Feed(data, size);  // a whole file, or any split of a stream
 * mybinlog::Event event;
 * mybinlog::Status status;
 * while ((status = decoder.Next(&event)) == mybinlog::Status::kOk) {
 *   if (auto* rows = std::get_if<mybinlog::TableRowsEvent>(&event.body)) { ... }
 * }
 * // kNeedMoreData: feed more, or call Finish() at the end of input.
 * ```
 *
 * The decoder skips the 4-byte binlog magic when the stream opens with it (a
 * file) and reads streams without it (a replication dump with the packet
 * framing removed). It tracks the checksum algorithm per FORMAT_DESCRIPTION
 * (ChecksumTracker), detects the flavor from the FDE's server version,
 * keeps the table id -> TABLE_MAP mapping, expands compressed events and
 * decodes rows events. The detection reads the first four bytes, so a stream
 * without the magic whose first event's timestamp is 1852400382
 * (2028-09-11) is misread as a file.
 *
 * The TABLE_MAP mapping is dropped at every FORMAT_DESCRIPTION and ROTATE, and
 * a TABLE_MAP replaces any earlier one with the same id. Servers write a
 * TABLE_MAP before the rows events of every statement, so the mapping never
 * needs to outlive a file.
 *
 * Errors are sticky: after Next() returns an error status every later call
 * returns it again until Reset(), because a stream that failed to frame
 * cannot be resynchronized safely.
 *
 * Thread safety: one instance per stream, used from one thread at a time.
 * Distinct instances are independent.
 */

#ifndef MYBINLOG_DECODER_H_
#define MYBINLOG_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "mybinlog/checksum.h"
#include "mybinlog/compression.h"
#include "mybinlog/event.h"
#include "mybinlog/export.h"
#include "mybinlog/row_decoder.h"
#include "mybinlog/server_flavor.h"
#include "mybinlog/status.h"
#include "mybinlog/table_map.h"

namespace mybinlog {

/** @brief Default ceiling on one event's length (MySQL's largest max_allowed_packet). */
inline constexpr size_t kDefaultMaxEventSize = 1024U * 1024U * 1024U;

/** @brief Default ceiling on one expanded compressed event or transaction payload. */
inline constexpr size_t kDefaultMaxDecompressedSize = 1024U * 1024U * 1024U;

/** @brief Decoder configuration; every default suits a binlog file from a trusted server. */
struct DecoderOptions {
  /**
   * Checksum algorithm assumed before the first FORMAT_DESCRIPTION: the one a
   * replica announced to the source, for the artificial ROTATE a dump opens
   * with. Files start with an FDE, so it does not matter for them.
   */
  ChecksumAlgorithm default_checksum = ChecksumAlgorithm::kCrc32;
  /** Compare CRC32 trailers; false frames by the algorithm without comparing. */
  bool verify_checksum = true;
  /** Ceiling on an event's `event_length`; a longer one fails with kLimitExceeded. */
  size_t max_event_size = kDefaultMaxEventSize;
  /** Decode rows events into TableRowsEvent::rows; false leaves only the raw body. */
  bool decode_rows = true;
  /**
   * Column selection and limits for rows decoding. When `rows.budget` is
   * null, each rows event is charged against its own
   * DecodeBudget::ForEventBody(body size, rows.max_decompressed_bytes); a
   * non-null budget is passed through unchanged and accumulates across events.
   */
  DecodeOptions rows;
  /**
   * Expand MariaDB compressed events (165-171) into their plain equivalents
   * and TRANSACTION_PAYLOAD events into the events they carry. When false,
   * both arrive as UnknownEvent with disposition kCompressed.
   */
  bool expand_compressed = true;
  /** Ceiling on one expanded MariaDB event body or transaction payload. */
  size_t max_decompressed_size = kDefaultMaxDecompressedSize;
};

/** @brief Why the last Next() or Finish() failed. */
struct DecodeError {
  Status status = Status::kOk;  ///< The failing status; kOk when nothing failed.
  /** Stream offset of the event that failed (of its payload, for an inner event). */
  uint64_t offset = 0;
  std::string message;  ///< Event type, offset and reason, for logs.
};

/**
 * @brief Pull decoder over one binlog stream.
 *
 * Feed() bytes in any split, then call Next() until it returns kNeedMoreData.
 * See the file comment for what it does and the thread-safety contract.
 */
class MYBINLOG_API Decoder {
 public:
  /** @brief A decoder with default options. */
  Decoder() : Decoder(DecoderOptions()) {}

  /**
   * @brief A decoder with the given options.
   * @param options Copied; `options.rows.budget`, when set, must outlive the decoder.
   */
  explicit Decoder(const DecoderOptions& options);

  /**
   * @brief Append stream bytes.
   *
   * The bytes are copied, so @p data may be reused at once. Pointers held in
   * an Event from this decoder become invalid.
   *
   * @param data Next bytes of the stream; may be null when @p size is 0.
   * @param size Bytes at @p data.
   */
  void Feed(const uint8_t* data, size_t size);

  /**
   * @brief Take the next event.
   *
   * The events of a TRANSACTION_PAYLOAD come one per call, in order, with
   * Event::from_payload set; the payload event itself is not returned.
   * Pointers held in the previous Event become invalid.
   *
   * @param[out] out Replaced on kOk; untouched otherwise.
   * @return kOk with an event; kNeedMoreData when no complete event is
   *   buffered; otherwise the error, also reported by last_error():
   *   kMalformedValue for an event_length below the header size; kLimitExceeded
   *   above DecoderOptions::max_event_size or for an expansion or rows budget
   *   overrun; kChecksumMismatch for a CRC32 mismatch; kInvalidMetadata for a
   *   rows event whose table id has no TABLE_MAP; and whatever the event's
   *   parser returns (see ParseFormatDescriptionEvent(), DecodeRowsEvent(),
   *   ...).
   */
  [[nodiscard]] Status Next(Event* out);

  /**
   * @brief Declare the end of input.
   *
   * Call once Next() has returned kNeedMoreData and no more bytes will come.
   *
   * @return kOk when the stream ended on an event boundary; kTruncated, also
   *   reported by last_error(), when buffered bytes hold only part of an event
   *   (or of the binlog magic); a sticky error from Next() is returned again.
   */
  [[nodiscard]] Status Finish();

  /** @brief Drop all buffered bytes and state, as if newly constructed with the same options. */
  void Reset();

  /** @return The last failure of Next() or Finish(); status kOk when none since construction or
   * Reset(). */
  [[nodiscard]] const DecodeError& last_error() const noexcept { return error_; }

  /** @return Flavor announced by the latest FORMAT_DESCRIPTION; kMySQL before one. */
  [[nodiscard]] ServerFlavor flavor() const noexcept { return flavor_; }

  /** @return Bytes fed but not yet consumed by Next(). */
  [[nodiscard]] size_t buffered_bytes() const noexcept { return buffer_.size() - pos_; }

 private:
  [[nodiscard]] Status Fail(Status status, uint64_t offset, const EventHeader* header,
                            const char* reason);
  [[nodiscard]] Status DecodeEvent(const EventHeader& header, const uint8_t* body, size_t size,
                                   uint64_t offset, bool from_payload, Event* out);
  [[nodiscard]] Status DecodePlain(const EventHeader& header, const uint8_t* body, size_t size,
                                   uint64_t offset, bool from_payload, Event* out);
  [[nodiscard]] Status DecodeRows(const EventHeader& header, const uint8_t* body, size_t size,
                                  uint64_t offset, Event* out);

  DecoderOptions options_;
  ChecksumTracker tracker_;
  ServerFlavor flavor_ = ServerFlavor::kMySQL;
  std::unordered_map<uint64_t, TableMap> tables_;

  std::vector<uint8_t> buffer_;
  size_t pos_ = 0;            // first unconsumed byte of buffer_
  uint64_t buffer_base_ = 0;  // stream offset of buffer_[0]
  bool magic_checked_ = false;

  DecompressedEvent decompressed_;  // the MariaDB event last expanded
  std::vector<uint8_t> payload_;    // inner events of the open TRANSACTION_PAYLOAD
  size_t payload_pos_ = 0;          // next inner event in payload_
  uint64_t payload_offset_ = 0;     // stream offset of the payload event

  DecodeError error_;
};

}  // namespace mybinlog

#endif  // MYBINLOG_DECODER_H_
