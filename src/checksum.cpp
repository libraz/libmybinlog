#include "mybinlog/checksum.h"

#include "bytes_internal.h"
#include "crc32_internal.h"

namespace mybinlog {

namespace {

constexpr size_t kMinChecksummedFrame = kEventHeaderSize + kChecksumSize;

bool TrailerMatches(const uint8_t* frame, size_t size) {
  const size_t data_length = size - kChecksumSize;
  return internal::Crc32Update(0, frame, data_length) == ReadU32Le(frame + data_length);
}

}  // namespace

bool ServerVersionPredatesChecksums(std::string_view server_version) noexcept {
  uint32_t parts[3] = {0, 0, 0};
  size_t pos = 0;
  for (auto& part : parts) {
    while (pos < server_version.size() && server_version[pos] >= '0' &&
           server_version[pos] <= '9') {
      // Saturate: an absurd component must not wrap below the floor.
      if (part < 100000) part = part * 10 + static_cast<uint32_t>(server_version[pos] - '0');
      ++pos;
    }
    if (pos >= server_version.size() || server_version[pos] != '.') break;
    ++pos;
  }
  const bool mariadb = server_version.find("MariaDB") != std::string_view::npos ||
                       server_version.find("-maria-") != std::string_view::npos;
  const uint32_t floor[3] = {5, mariadb ? 3U : 6U, mariadb ? 0U : 1U};
  for (size_t i = 0; i < 3; ++i) {
    if (parts[i] != floor[i]) return parts[i] < floor[i];
  }
  return false;
}

Status ChecksumTracker::OnEvent(const EventHeader& header, const uint8_t* frame, size_t size,
                                FrameLayout* layout) {
  if (frame == nullptr || size < kEventHeaderSize || size < header.event_length) {
    return Status::kTruncated;
  }
  if (size > header.event_length) return Status::kMalformedValue;

  if (header.type_code == static_cast<uint8_t>(EventType::kFormatDescriptionEvent)) {
    ChecksumAlgorithm declared = ChecksumAlgorithm::kUndefined;
    const Status status = ReadFdeChecksumAlgorithm(frame, size, &declared);
    if (status != Status::kOk) return status;
    algorithm_ = declared;
    if (declared == ChecksumAlgorithm::kUndefined) {
      *layout = FrameLayout{size, false};
      return Status::kOk;
    }
    // The FDE's own trailer is always present; it is only meaningful under CRC32.
    if (declared == ChecksumAlgorithm::kCrc32 && verify_ && !TrailerMatches(frame, size)) {
      return Status::kChecksumMismatch;
    }
    *layout = FrameLayout{size - kChecksumSize, true};
    return Status::kOk;
  }

  if (algorithm_ == ChecksumAlgorithm::kCrc32) {
    if (size < kMinChecksummedFrame) return Status::kChecksumMismatch;
    if (verify_ && !TrailerMatches(frame, size)) return Status::kChecksumMismatch;
    *layout = FrameLayout{size - kChecksumSize, true};
    return Status::kOk;
  }

  // The dump-start ROTATE follows the replica's announced algorithm, not the file's.
  if (header.type_code == static_cast<uint8_t>(EventType::kRotateEvent) &&
      (header.flags & kLogEventArtificialFlag) != 0 && size >= kMinChecksummedFrame &&
      TrailerMatches(frame, size)) {
    *layout = FrameLayout{size - kChecksumSize, true};
    return Status::kOk;
  }
  *layout = FrameLayout{size, false};
  return Status::kOk;
}

}  // namespace mybinlog
