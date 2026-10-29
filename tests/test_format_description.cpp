// FORMAT_DESCRIPTION_EVENT bodies: fixed fields, post-header lengths and the
// version-gated checksum algorithm byte.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "mybinlog/checksum.h"
#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/format_description.h"
#include "mybinlog/status.h"

namespace mybinlog {
namespace {

using Bytes = std::vector<uint8_t>;

EventHeader FdeHeader() {
  EventHeader header;
  header.type_code = static_cast<uint8_t>(EventType::kFormatDescriptionEvent);
  return header;
}

// Body as framed by ChecksumTracker: trailer excluded; kUndefined omits the alg byte.
Bytes FdeBody(const std::string& version, const Bytes& post_header_lengths,
              ChecksumAlgorithm algorithm, uint8_t common_header_length = 19) {
  Bytes body = {4, 0};
  Bytes version_field(kServerVersionFieldSize, 0);
  std::memcpy(version_field.data(), version.data(), version.size());
  body.insert(body.end(), version_field.begin(), version_field.end());
  body.insert(body.end(), {0x78, 0x56, 0x34, 0x12, common_header_length});
  body.insert(body.end(), post_header_lengths.begin(), post_header_lengths.end());
  if (algorithm != ChecksumAlgorithm::kUndefined) body.push_back(static_cast<uint8_t>(algorithm));
  return body;
}

Status Parse(const Bytes& body, FormatDescriptionEvent* out) {
  return ParseFormatDescriptionEvent(FdeHeader(), body.data(), body.size(), out);
}

TEST(CodecFormatDescription, ParsesFixedFieldsAndPostHeaderLengths) {
  for (const ChecksumAlgorithm algorithm : {ChecksumAlgorithm::kOff, ChecksumAlgorithm::kCrc32}) {
    const Bytes body = FdeBody("8.4.13-log", {56, 13, 0, 8, 0}, algorithm);
    FormatDescriptionEvent fde;
    ASSERT_EQ(Parse(body, &fde), Status::kOk);
    EXPECT_EQ(fde.binlog_version, 4);
    EXPECT_EQ(fde.server_version, "8.4.13-log");
    EXPECT_EQ(fde.create_timestamp, 0x12345678u);
    EXPECT_EQ(fde.common_header_length, 19);
    EXPECT_EQ(fde.post_header_lengths, (Bytes{56, 13, 0, 8, 0}));
    EXPECT_EQ(fde.checksum_algorithm, algorithm);
  }
}

TEST(CodecFormatDescription, PreChecksumVersionHasNoAlgorithmByte) {
  for (const char* version : {"5.5.62-log", "5.2.14-MariaDB"}) {
    SCOPED_TRACE(version);
    const Bytes body = FdeBody(version, {56, 13, 0, 8}, ChecksumAlgorithm::kUndefined);
    FormatDescriptionEvent fde;
    ASSERT_EQ(Parse(body, &fde), Status::kOk);
    EXPECT_EQ(fde.checksum_algorithm, ChecksumAlgorithm::kUndefined);
    // The last byte is a post-header length, not an algorithm.
    EXPECT_EQ(fde.post_header_lengths, (Bytes{56, 13, 0, 8}));
  }
}

TEST(CodecFormatDescription, VersionFieldWithoutNulIsKeptWhole) {
  const std::string version(kServerVersionFieldSize, '9');
  FormatDescriptionEvent fde;
  ASSERT_EQ(Parse(FdeBody(version, {}, ChecksumAlgorithm::kCrc32), &fde), Status::kOk);
  EXPECT_EQ(fde.server_version, version);
  EXPECT_TRUE(fde.post_header_lengths.empty());
}

TEST(CodecFormatDescription, UnknownAlgorithmIsUnsupported) {
  Bytes body = FdeBody("8.4.13", {13}, ChecksumAlgorithm::kCrc32);
  body.back() = 2;
  FormatDescriptionEvent fde;
  fde.server_version = "unchanged";
  EXPECT_EQ(Parse(body, &fde), Status::kUnsupported);
  EXPECT_EQ(fde.server_version, "unchanged");
}

TEST(CodecFormatDescription, ShortCommonHeaderIsMalformed) {
  FormatDescriptionEvent fde;
  EXPECT_EQ(Parse(FdeBody("8.4.13", {13}, ChecksumAlgorithm::kCrc32, 18), &fde),
            Status::kMalformedValue);
}

TEST(CodecFormatDescription, Truncated) {
  const Bytes body = FdeBody("8.4.13", {}, ChecksumAlgorithm::kUndefined);
  FormatDescriptionEvent fde;
  // Fixed fields end early.
  for (size_t size = 0; size < body.size(); ++size) {
    EXPECT_EQ(ParseFormatDescriptionEvent(FdeHeader(), body.data(), size, &fde), Status::kTruncated)
        << size;
  }
  // A checksum-aware version without its algorithm byte.
  EXPECT_EQ(Parse(body, &fde), Status::kTruncated);
  EXPECT_EQ(ParseFormatDescriptionEvent(FdeHeader(), nullptr, 100, &fde), Status::kTruncated);
}

// The body-level parser and the frame-level helper agree on the algorithm.
TEST(CodecFormatDescription, AgreesWithFrameLevelAlgorithm) {
  for (const ChecksumAlgorithm algorithm : {ChecksumAlgorithm::kOff, ChecksumAlgorithm::kCrc32}) {
    const Bytes body = FdeBody("11.8.3-MariaDB", {13, 8}, algorithm);
    Bytes frame(kEventHeaderSize, 0);
    frame[4] = static_cast<uint8_t>(EventType::kFormatDescriptionEvent);
    frame.insert(frame.end(), body.begin(), body.end());
    frame.resize(frame.size() + kChecksumSize, 0);
    ChecksumAlgorithm from_frame = ChecksumAlgorithm::kUndefined;
    ASSERT_EQ(ReadFdeChecksumAlgorithm(frame.data(), frame.size(), &from_frame), Status::kOk);
    FormatDescriptionEvent fde;
    ASSERT_EQ(Parse(body, &fde), Status::kOk);
    EXPECT_EQ(fde.checksum_algorithm, from_frame);
    EXPECT_EQ(fde.checksum_algorithm, algorithm);
  }
}

}  // namespace
}  // namespace mybinlog
