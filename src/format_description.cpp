#include "mybinlog/format_description.h"

#include <string_view>
#include <utility>

#include "bytes_internal.h"

namespace mybinlog {

namespace {

// binlog_version(2), server_version(50), create_timestamp(4), common_header_length(1).
constexpr size_t kServerVersionOffset = 2;
constexpr size_t kFixedSize = kServerVersionOffset + kServerVersionFieldSize + 4 + 1;
constexpr size_t kAlgorithmSize = 1;

std::string_view ServerVersion(const uint8_t* body) {
  const std::string_view field(reinterpret_cast<const char*>(body + kServerVersionOffset),
                               kServerVersionFieldSize);
  return field.substr(0, field.find('\0'));
}

Status ToAlgorithm(uint8_t byte, ChecksumAlgorithm* out) {
  switch (byte) {
    case static_cast<uint8_t>(ChecksumAlgorithm::kOff):
      *out = ChecksumAlgorithm::kOff;
      return Status::kOk;
    case static_cast<uint8_t>(ChecksumAlgorithm::kCrc32):
      *out = ChecksumAlgorithm::kCrc32;
      return Status::kOk;
    default:
      return Status::kUnsupported;
  }
}

}  // namespace

Status ReadFdeChecksumAlgorithm(const uint8_t* frame, size_t size, ChecksumAlgorithm* out) {
  if (frame == nullptr || size < kEventHeaderSize + kFixedSize) return Status::kTruncated;
  if (ServerVersionPredatesChecksums(ServerVersion(frame + kEventHeaderSize))) {
    *out = ChecksumAlgorithm::kUndefined;
    return Status::kOk;
  }
  if (size - kEventHeaderSize - kFixedSize < kAlgorithmSize + kChecksumSize) {
    return Status::kTruncated;
  }
  return ToAlgorithm(frame[size - kChecksumSize - kAlgorithmSize], out);
}

Status ParseFormatDescriptionEvent(const EventHeader& /*header*/, const uint8_t* body, size_t size,
                                   FormatDescriptionEvent* out) {
  if (body == nullptr || size < kFixedSize) return Status::kTruncated;
  FormatDescriptionEvent event;
  event.binlog_version = ReadU16Le(body);
  event.server_version = std::string(ServerVersion(body));
  event.create_timestamp = ReadU32Le(body + kServerVersionOffset + kServerVersionFieldSize);
  event.common_header_length = body[kFixedSize - 1];
  if (event.common_header_length < kEventHeaderSize) return Status::kMalformedValue;

  size_t post_header_end = size;
  if (!ServerVersionPredatesChecksums(event.server_version)) {
    if (size - kFixedSize < kAlgorithmSize) return Status::kTruncated;
    post_header_end = size - kAlgorithmSize;
    const Status status = ToAlgorithm(body[post_header_end], &event.checksum_algorithm);
    if (status != Status::kOk) return status;
  }
  event.post_header_lengths.assign(body + kFixedSize, body + post_header_end);
  *out = std::move(event);
  return Status::kOk;
}

}  // namespace mybinlog
