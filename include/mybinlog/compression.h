/**
 * @file compression.h
 * @brief Expansion of compressed events: MariaDB's zlib-compressed QUERY and
 *        rows events (types 165-171) and MySQL's TRANSACTION_PAYLOAD (type 40).
 *
 * A MariaDB compressed event keeps its leading fields in the clear and
 * compresses only the tail (the statement text, or the row images).
 * DecompressMariaDbEvent() rebuilds the body of the equivalent plain event, so
 * ParseQueryEvent() and DecodeRowsEvent() read it unchanged.
 *
 * A TRANSACTION_PAYLOAD carries whole events back to back, optionally
 * compressed with zstd. The inner events have a common header but never a
 * checksum trailer, whatever the file's algorithm: the source writes them
 * without one (MySQL `payload_event_buffer_istream.h`, "Compressed events
 * never have a checksum, regardless of configuration"). NextPayloadEvent()
 * walks them. zstd support is a build option (`MYBINLOG_WITH_ZSTD`); without
 * it a zstd payload is kUnsupported, and HasZstdSupport() says which.
 */

#ifndef MYBINLOG_COMPRESSION_H_
#define MYBINLOG_COMPRESSION_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mybinlog/event_header.h"
#include "mybinlog/export.h"
#include "mybinlog/status.h"

namespace mybinlog {

/**
 * @brief Whether this build decompresses zstd transaction payloads.
 * @return true when built with `MYBINLOG_WITH_ZSTD` and zstd was found. Reentrant.
 */
[[nodiscard]] MYBINLOG_API bool HasZstdSupport() noexcept;

/**
 * @brief Whether a type code is a MariaDB compressed event.
 * @param type_code Header byte 4.
 * @return true for QUERY_COMPRESSED (165) and the compressed rows events
 *   (166-171). Reentrant.
 */
[[nodiscard]] MYBINLOG_API bool IsMariaDbCompressedEvent(uint8_t type_code) noexcept;

/** @brief A MariaDB compressed event rebuilt as its plain equivalent. */
struct DecompressedEvent {
  /**
   * The compressed event's header with `type_code` replaced by the plain type
   * (QUERY, WRITE/UPDATE/DELETE_ROWS v1 or v2) and `event_length` changed by
   * the growth of the body; any checksum trailer it counted is still counted.
   */
  EventHeader header;
  std::vector<uint8_t> body;  ///< Body of the plain event, no trailer.
};

/**
 * @brief Expand a MariaDB compressed QUERY or rows event.
 *
 * QUERY_COMPRESSED compresses the statement text after the default database;
 * the rows events compress everything after the column bitmaps. The
 * compressed part opens with one byte (bit 7 set, algorithm in bits 4-6 where
 * 0 is zlib, length-of-length 1..4 in bits 0-2) and the original length,
 * big-endian, followed by a zlib stream.
 *
 * @param header Header of the event; its type code selects the layout.
 * @param body Bytes after the common header; only read during the call.
 * @param size Body length, checksum trailer excluded.
 * @param max_size Ceiling on the rebuilt body's size; pass SIZE_MAX for none.
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return kOk; kTruncated when the clear fields or the compression header run
 *   past the body (or @p body is null); kInvalidMetadata for a v2 extra-data
 *   length below 2 or a column count of 0xFB/0xFF; kMalformedValue for a
 *   compression byte without bit 7 or with a length-of-length outside 1..4,
 *   an original length of 0, a corrupt stream, a stream with trailing input,
 *   or one that does not expand to exactly the original length; kUnsupported
 *   for a type code that is not a MariaDB compressed event or an algorithm
 *   other than zlib; kLimitExceeded when the rebuilt body would exceed
 *   @p max_size or 4 GiB. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status DecompressMariaDbEvent(const EventHeader& header,
                                                         const uint8_t* body, size_t size,
                                                         size_t max_size, DecompressedEvent* out);

/** @brief Compression of a TRANSACTION_PAYLOAD; values are the wire codes. */
enum class PayloadCompression : uint8_t {
  kZstd = 0,    ///< One or more zstd frames.
  kNone = 255,  ///< The events as written.
};

/** @brief Parsed TRANSACTION_PAYLOAD_EVENT. */
struct TransactionPayloadEvent {
  PayloadCompression compression = PayloadCompression::kNone;  ///< How #payload is encoded.
  uint64_t payload_size = 0;                                   ///< Bytes of #payload.
  bool has_uncompressed_size = false;  ///< The source wrote #uncompressed_size.
  uint64_t uncompressed_size = 0;      ///< Expanded size; meaningful when #has_uncompressed_size.
  /** First payload byte, inside the parsed body: valid while that buffer is. */
  const uint8_t* payload = nullptr;
};

/**
 * @brief Parse a TRANSACTION_PAYLOAD_EVENT body.
 *
 * The body opens with fields of (type, length, value) packed integers ending
 * at type 0; fields of unknown type are skipped by their length. The payload
 * follows the end mark.
 *
 * @param header Header of the event; not consulted by the current layout.
 * @param body Bytes after the common header.
 * @param size Body length, checksum trailer excluded.
 * @param[out] out Filled on kOk, pointing into @p body; untouched otherwise.
 * @return kOk; kTruncated when the fields end without the end mark, a field
 *   runs past the body, the payload size exceeds the bytes after the end mark,
 *   or @p body is null; kInvalidMetadata when the compression type or payload
 *   size is missing, or for a packed int 0xFB/0xFF; kUnsupported for an
 *   unknown compression type. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParseTransactionPayloadEvent(const EventHeader& header,
                                                               const uint8_t* body, size_t size,
                                                               TransactionPayloadEvent* out);

/**
 * @brief Expand a payload into its inner events, concatenated.
 *
 * Walk the result with NextPayloadEvent(). When the source wrote an
 * uncompressed size, the expansion must match it.
 *
 * @param event A parsed payload; its #payload must still be valid.
 * @param max_size Ceiling on the expanded size; pass SIZE_MAX for none.
 * @param[out] events Replaced on kOk; untouched otherwise.
 * @return kOk; kMalformedValue for a corrupt or incomplete zstd stream, or an
 *   expansion that differs from the declared uncompressed size; kUnsupported
 *   for zstd when HasZstdSupport() is false; kLimitExceeded when the expansion
 *   (or the declared uncompressed size) exceeds @p max_size. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status DecompressTransactionPayload(const TransactionPayloadEvent& event,
                                                               size_t max_size,
                                                               std::vector<uint8_t>* events);

/** @brief One event inside an expanded transaction payload. */
struct PayloadEventView {
  EventHeader header;             ///< Its common header.
  const uint8_t* body = nullptr;  ///< Bytes after the header, inside the walked buffer.
  size_t size = 0;                ///< Body length; inner events have no trailer.
};

/**
 * @brief Read the event at @p *offset of an expanded payload and advance past it.
 *
 * Loop while `*offset < size`.
 *
 * @param data Expanded payload, as DecompressTransactionPayload() returns it.
 * @param size Bytes of @p data.
 * @param[in,out] offset Position of the event; on kOk, the position after it.
 * @param[out] out Filled on kOk, pointing into @p data; untouched otherwise.
 * @return kOk; kTruncated when fewer than a header's bytes remain, the event
 *   runs past @p size, or @p data is null; kMalformedValue for an event length
 *   below the header size. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status NextPayloadEvent(const uint8_t* data, size_t size, size_t* offset,
                                                   PayloadEventView* out);

}  // namespace mybinlog

#endif  // MYBINLOG_COMPRESSION_H_
