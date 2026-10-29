// HEARTBEAT_LOG_EVENT_V2 bodies built the way MySQL's `Heartbeat::encode`
// (libs/mysql/binlog/event/codecs/binary.cpp) writes them: a TLV section of
// packed integers (net_store_length) ending at type 0.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/heartbeat.h"
#include "mybinlog/status.h"

namespace mybinlog {
namespace {

using Bytes = std::vector<uint8_t>;

// net_store_length.
void PutPacked(Bytes* out, uint64_t n) {
  int width = 0;
  if (n < 251) {
    out->push_back(static_cast<uint8_t>(n));
    return;
  }
  if (n < 0x10000) {
    out->push_back(0xFC);
    width = 2;
  } else if (n < 0x1000000) {
    out->push_back(0xFD);
    width = 3;
  } else {
    out->push_back(0xFE);
    width = 8;
  }
  for (int i = 0; i < width; ++i) out->push_back(static_cast<uint8_t>(n >> (8 * i)));
}

// net_length_size.
uint64_t PackedSize(uint64_t n) {
  if (n < 251) return 1;
  if (n < 0x10000) return 3;
  if (n < 0x1000000) return 4;
  return 9;
}

// Heartbeat::encode: filename field (1), position field (2), end mark (0).
Bytes EncodeV2(const std::string& name, uint64_t position) {
  Bytes out;
  PutPacked(&out, 1);
  PutPacked(&out, name.size());
  out.insert(out.end(), name.begin(), name.end());
  PutPacked(&out, 2);
  PutPacked(&out, PackedSize(position));
  PutPacked(&out, position);
  PutPacked(&out, 0);
  return out;
}

EventHeader V2Header() {
  EventHeader header;
  header.type_code = static_cast<uint8_t>(EventType::kHeartbeatLogEventV2);
  header.next_position = 0;  // v2 carries the position in the body
  return header;
}

Status ParseV2(const Bytes& body, HeartbeatEvent* out) {
  return ParseHeartbeatEvent(V2Header(), body.data(), body.size(), out);
}

TEST(CodecHeartbeat, V2DecodesEncoderOutputAtEveryPackedWidth) {
  const uint64_t positions[] = {4,
                                250,
                                251,
                                65535,
                                65536,
                                0xFFFFFF,
                                0x1000000,
                                5ULL * 1024 * 1024 * 1024,
                                0xFFFFFFFFFFFFFFFFULL};
  for (const uint64_t position : positions) {
    SCOPED_TRACE(position);
    HeartbeatEvent event;
    ASSERT_EQ(ParseV2(EncodeV2("binlog.000042", position), &event), Status::kOk);
    EXPECT_EQ(event.log_name, "binlog.000042");
    EXPECT_EQ(event.position, position);
  }
}

TEST(CodecHeartbeat, V2BytesMatchTheEncoderLayout) {
  // Position 1000 needs the 0xFC form: field 2, length 3, then FC E8 03.
  const Bytes expected = {0x01, 0x03, 'b', '.', '1', 0x02, 0x03, 0xFC, 0xE8, 0x03, 0x00};
  EXPECT_EQ(EncodeV2("b.1", 1000), expected);
  HeartbeatEvent event;
  ASSERT_EQ(ParseV2(expected, &event), Status::kOk);
  EXPECT_EQ(event.log_name, "b.1");
  EXPECT_EQ(event.position, 1000u);
}

TEST(CodecHeartbeat, V2LongFileNameUsesAWideLength) {
  const std::string name(300, 'n');
  HeartbeatEvent event;
  ASSERT_EQ(ParseV2(EncodeV2(name, 7), &event), Status::kOk);
  EXPECT_EQ(event.log_name, name);
  EXPECT_EQ(event.position, 7u);
}

TEST(CodecHeartbeat, V2SkipsUnknownFieldsByLength) {
  Bytes body = {0x09, 0x03, 0xAA, 0xBB, 0xCC};  // unknown type 9, 3 bytes
  const Bytes known = EncodeV2("binlog.000001", 123);
  body.insert(body.end(), known.begin(), known.end());
  HeartbeatEvent event;
  ASSERT_EQ(ParseV2(body, &event), Status::kOk);
  EXPECT_EQ(event.log_name, "binlog.000001");
  EXPECT_EQ(event.position, 123u);
}

TEST(CodecHeartbeat, V2FieldsMayBeMissing) {
  HeartbeatEvent event;
  ASSERT_EQ(ParseV2({0x00}, &event), Status::kOk);
  EXPECT_EQ(event.log_name, "");
  EXPECT_EQ(event.position, 0u);
}

TEST(CodecHeartbeat, V2IgnoresTheHeaderPosition) {
  EventHeader header = V2Header();
  header.next_position = 99;
  const Bytes body = EncodeV2("b", 5);
  HeartbeatEvent event;
  ASSERT_EQ(ParseHeartbeatEvent(header, body.data(), body.size(), &event), Status::kOk);
  EXPECT_EQ(event.position, 5u);
}

TEST(CodecHeartbeat, V2RejectsTruncatedAndInvalidBodies) {
  const Bytes full = EncodeV2("binlog.000001", 70000);
  HeartbeatEvent event;
  event.log_name = "untouched";
  // Every proper prefix ends before the end mark.
  for (size_t n = 0; n < full.size(); ++n) {
    SCOPED_TRACE(n);
    const Bytes prefix(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n));
    EXPECT_EQ(ParseV2(prefix, &event), Status::kTruncated);
  }
  EXPECT_EQ(ParseV2({0x01, 0x05, 'a', 'b', 0x00}, &event), Status::kTruncated);  // name overruns
  EXPECT_EQ(ParseV2({0xFB}, &event), Status::kInvalidMetadata);
  EXPECT_EQ(ParseV2({0x02, 0x01, 0xFF, 0x00}, &event), Status::kInvalidMetadata);
  EXPECT_EQ(event.log_name, "untouched");
  EXPECT_EQ(ParseHeartbeatEvent(V2Header(), nullptr, 3, &event), Status::kTruncated);
}

TEST(CodecHeartbeat, V1StillTakesTheHeaderPosition) {
  EventHeader header;
  header.type_code = static_cast<uint8_t>(EventType::kHeartbeatLogEvent);
  header.next_position = 4096;
  const std::string name = "binlog.000003";
  HeartbeatEvent event;
  ASSERT_EQ(ParseHeartbeatEvent(header, reinterpret_cast<const uint8_t*>(name.data()), name.size(),
                                &event),
            Status::kOk);
  EXPECT_EQ(event.log_name, name);
  EXPECT_EQ(event.position, 4096u);
}

}  // namespace
}  // namespace mybinlog
