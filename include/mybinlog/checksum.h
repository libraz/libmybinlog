/**
 * @file checksum.h
 * @brief Per-file binlog checksum algorithm and trailer framing.
 *
 * Each binlog file declares its algorithm in its FORMAT_DESCRIPTION_EVENT
 * (FDE): one byte just before the 4-byte trailer, behind a server-version gate
 * (MySQL < 5.6.1 and MariaDB < 5.3.0 write no byte and no trailer). The FDE of
 * a checksum-aware server always carries its own trailer, even when it
 * declares OFF. The algorithm is never taken from a server variable; it
 * changes only at an FDE. The artificial ROTATE a dump sends uses the
 * algorithm the replica announced, so under OFF framing it counts as
 * checksummed only when its CRC verifies.
 */

#ifndef MYBINLOG_CHECKSUM_H_
#define MYBINLOG_CHECKSUM_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "mybinlog/event_header.h"
#include "mybinlog/export.h"
#include "mybinlog/status.h"

namespace mybinlog {

/** @brief Checksum algorithm; values are the FDE's wire byte. */
enum class ChecksumAlgorithm : uint8_t {
  kOff = 0,          ///< No trailer (except on the FDE itself).
  kCrc32 = 1,        ///< Every event ends with a CRC32 of the bytes before it.
  kUndefined = 255,  ///< Pre-checksum server: no event, FDE included, has a trailer.
};

/**
 * @brief Whether an FDE server version predates binlog checksums.
 *
 * MySQL below 5.6.1, MariaDB (version containing "MariaDB" or "-maria-") below
 * 5.3.0. Unparseable components count as 0.
 *
 * @param server_version Version text; only read during the call.
 * @return true when the server writes neither an algorithm byte nor trailers.
 *   Reentrant.
 */
[[nodiscard]] MYBINLOG_API bool ServerVersionPredatesChecksums(
    std::string_view server_version) noexcept;

/**
 * @brief Read the algorithm a complete FDE frame declares.
 *
 * Works on the whole frame because the trailer's presence is what is being
 * decided; ParseFormatDescriptionEvent() returns the same value from a body
 * framed by ChecksumTracker.
 *
 * @param frame Whole event, common header included.
 * @param size Bytes of @p frame (its `event_length`).
 * @param[out] out Filled on kOk; untouched otherwise.
 * @return kOk; kTruncated if @p frame is null or cannot hold the fixed fields
 *   (or, for a checksum-aware version, the algorithm byte and trailer after
 *   them); kUnsupported for an unknown algorithm byte. Reentrant.
 */
[[nodiscard]] MYBINLOG_API Status ReadFdeChecksumAlgorithm(const uint8_t* frame, size_t size,
                                                           ChecksumAlgorithm* out);

/** @brief Where the payload ends within one event frame. */
struct FrameLayout {
  size_t payload_length = 0;  ///< Header + body; the trailer, if any, follows.
  bool has_trailer = false;   ///< A 4-byte trailer follows the payload.

  /**
   * @brief Body length: the payload minus the common header.
   * @return The `size` to pass to the event-body parsers, trailer excluded.
   */
  [[nodiscard]] size_t body_length() const noexcept { return payload_length - kEventHeaderSize; }
};

/**
 * @brief Tracks the algorithm in effect and frames each event under it.
 *
 * Feed every event of one stream, in order, to OnEvent(). Before the first
 * FDE the default algorithm applies (SetDefaultAlgorithm()); each FDE then
 * replaces it for the rest of its file. SetVerify(false) keeps the framing but
 * skips CRC comparison, for producers that verified the trailer already; it
 * does not affect artificial-ROTATE detection, which is framing.
 *
 * Thread safety: one instance per stream, used from one thread at a time.
 * Distinct instances are independent.
 */
class MYBINLOG_API ChecksumTracker {
 public:
  /**
   * @brief Set the algorithm assumed before the first FDE of a stream (and after Reset()).
   * @param algorithm Algorithm to assume; also becomes the one in effect now.
   */
  void SetDefaultAlgorithm(ChecksumAlgorithm algorithm) noexcept {
    default_algorithm_ = algorithm;
    algorithm_ = algorithm;
  }

  /** @return The algorithm assumed before the first FDE (default kCrc32). */
  [[nodiscard]] ChecksumAlgorithm default_algorithm() const noexcept { return default_algorithm_; }

  /**
   * @brief Choose whether CRC32 trailers are compared.
   * @param verify true (the default) to compare, false to frame only.
   */
  void SetVerify(bool verify) noexcept { verify_ = verify; }

  /** @return Whether CRC32 trailers are compared. */
  [[nodiscard]] bool verify() const noexcept { return verify_; }

  /** @return The algorithm in effect for the next non-FDE event. */
  [[nodiscard]] ChecksumAlgorithm algorithm() const noexcept { return algorithm_; }

  /** @brief Start a new stream: return to the default algorithm. */
  void Reset() noexcept { algorithm_ = default_algorithm_; }

  /**
   * @brief Frame one complete event and, under CRC32, verify its trailer.
   *
   * An FDE re-derives the algorithm before it is framed.
   *
   * @param header Parsed header of @p frame.
   * @param frame Whole event, common header included.
   * @param size Bytes of @p frame; must equal `header.event_length`.
   * @param[out] layout Filled on kOk; untouched otherwise.
   * @return kOk; kTruncated when @p frame is null, @p size is shorter than the
   *   header says, or an FDE is too short; kMalformedValue when @p size is
   *   longer; kChecksumMismatch for a CRC32 mismatch or a checksummed frame
   *   under 23 bytes; kUnsupported for an FDE with an unknown algorithm byte.
   */
  [[nodiscard]] Status OnEvent(const EventHeader& header, const uint8_t* frame, size_t size,
                               FrameLayout* layout);

 private:
  ChecksumAlgorithm default_algorithm_ = ChecksumAlgorithm::kCrc32;
  ChecksumAlgorithm algorithm_ = ChecksumAlgorithm::kCrc32;
  bool verify_ = true;
};

}  // namespace mybinlog

#endif  // MYBINLOG_CHECKSUM_H_
