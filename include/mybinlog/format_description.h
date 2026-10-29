/**
 * @file format_description.h
 * @brief FORMAT_DESCRIPTION_EVENT (FDE) body: the description that opens every binlog file.
 */

#ifndef MYBINLOG_FORMAT_DESCRIPTION_H_
#define MYBINLOG_FORMAT_DESCRIPTION_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mybinlog/checksum.h"
#include "mybinlog/event_header.h"
#include "mybinlog/export.h"
#include "mybinlog/status.h"

namespace mybinlog {

/** @brief Size of the server_version field of an FDE. */
inline constexpr size_t kServerVersionFieldSize = 50;

/** @brief Parsed FORMAT_DESCRIPTION_EVENT. */
struct FormatDescriptionEvent {
  uint16_t binlog_version = 0;  ///< Binlog format version (4 for every server this library reads).
  std::string server_version;   ///< Writer's version text, up to the first NUL.
  uint32_t create_timestamp = 0;  ///< Seconds since the epoch; 0 except on a server's first file.
  uint8_t common_header_length = 0;  ///< Length of the common header (kEventHeaderSize).
  /** Post-header length per event type: entry `i` is for type code `i + 1`. */
  std::vector<uint8_t> post_header_lengths;
  /** Algorithm the file is checksummed with; kUndefined for a pre-checksum server. */
  ChecksumAlgorithm checksum_algorithm = ChecksumAlgorithm::kUndefined;
};

/**
 * @brief Parse a FORMAT_DESCRIPTION_EVENT body.
 *
 * binlog_version(2) server_version(50) create_timestamp(4)
 * common_header_length(1), then the post-header lengths and, for a
 * checksum-aware server version, the algorithm byte as the last body byte.
 *
 * @param header Header of the event; not consulted by the current layout.
 * @param body Bytes after the common header.
 * @param size Body length with the trailer excluded, as
 *   FrameLayout::body_length() reports after ChecksumTracker::OnEvent().
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return kOk; kTruncated when @p body is null or ends before the fixed fields
 *   (or before the algorithm byte of a checksum-aware version);
 *   kMalformedValue when common_header_length is below kEventHeaderSize;
 *   kUnsupported for an unknown algorithm byte. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ParseFormatDescriptionEvent(const EventHeader& header,
                                                              const uint8_t* body, size_t size,
                                                              FormatDescriptionEvent* out);

}  // namespace mybinlog

#endif  // MYBINLOG_FORMAT_DESCRIPTION_H_
