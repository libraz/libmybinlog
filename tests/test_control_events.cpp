// XID, HEARTBEAT and PREVIOUS_GTIDS bodies.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/heartbeat.h"
#include "mybinlog/mysql_gtid.h"
#include "mybinlog/status.h"
#include "mybinlog/xid.h"

namespace mybinlog {
namespace {

using Bytes = std::vector<uint8_t>;

EventHeader HeaderOf(EventType type) {
  EventHeader header;
  header.type_code = static_cast<uint8_t>(type);
  return header;
}

// --- XID_EVENT ---

TEST(CodecControlEvents, Xid) {
  const Bytes body = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
  XidEvent xid;
  ASSERT_EQ(ParseXidEvent(HeaderOf(EventType::kXidEvent), body.data(), body.size(), &xid),
            Status::kOk);
  EXPECT_EQ(xid.xid, 0x0102030405060708ULL);
}

TEST(CodecControlEvents, XidIgnoresTrailingBytes) {
  const Bytes body = {42, 0, 0, 0, 0, 0, 0, 0, 0xFF};
  XidEvent xid;
  ASSERT_EQ(ParseXidEvent(HeaderOf(EventType::kXidEvent), body.data(), body.size(), &xid),
            Status::kOk);
  EXPECT_EQ(xid.xid, 42u);
}

TEST(CodecControlEvents, XidTruncated) {
  const Bytes body(7, 0);
  XidEvent xid;
  xid.xid = 99;
  EXPECT_EQ(ParseXidEvent(HeaderOf(EventType::kXidEvent), body.data(), body.size(), &xid),
            Status::kTruncated);
  EXPECT_EQ(ParseXidEvent(HeaderOf(EventType::kXidEvent), nullptr, 8, &xid), Status::kTruncated);
  EXPECT_EQ(xid.xid, 99u);
}

// --- HEARTBEAT_LOG_EVENT ---

TEST(CodecControlEvents, Heartbeat) {
  const std::string name = "binlog.000042";
  EventHeader header = HeaderOf(EventType::kHeartbeatLogEvent);
  header.next_position = 1234;
  HeartbeatEvent heartbeat;
  ASSERT_EQ(ParseHeartbeatEvent(header, reinterpret_cast<const uint8_t*>(name.data()), name.size(),
                                &heartbeat),
            Status::kOk);
  EXPECT_EQ(heartbeat.log_name, name);
  EXPECT_EQ(heartbeat.position, 1234u);
}

TEST(CodecControlEvents, HeartbeatWithEmptyName) {
  HeartbeatEvent heartbeat;
  heartbeat.log_name = "stale";
  ASSERT_EQ(ParseHeartbeatEvent(HeaderOf(EventType::kHeartbeatLogEvent), nullptr, 0, &heartbeat),
            Status::kOk);
  EXPECT_TRUE(heartbeat.log_name.empty());
  EXPECT_EQ(ParseHeartbeatEvent(HeaderOf(EventType::kHeartbeatLogEvent), nullptr, 3, &heartbeat),
            Status::kTruncated);
}

TEST(CodecControlEvents, HeartbeatV2AndOtherTypesAreUnsupported) {
  const Bytes body = {'b'};
  HeartbeatEvent heartbeat;
  for (const EventType type : {EventType::kHeartbeatLogEventV2, EventType::kRotateEvent}) {
    EXPECT_EQ(ParseHeartbeatEvent(HeaderOf(type), body.data(), body.size(), &heartbeat),
              Status::kUnsupported);
  }
}

// --- PREVIOUS_GTIDS_LOG_EVENT ---

TEST(CodecControlEvents, PreviousGtidsRoundTripsTheBinaryEncoding) {
  GtidSet set;
  ASSERT_EQ(GtidSet::Parse("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5:7,"
                           "3e11fa47-71ca-11e1-9e33-c80aa9429562:blue:1-3",
                           &set),
            Status::kOk);
  Bytes wire;
  set.EncodeBinary(&wire);
  PreviousGtidsEvent event;
  ASSERT_EQ(ParsePreviousGtidsEvent(HeaderOf(EventType::kPreviousGtidsEvent), wire.data(),
                                    wire.size(), &event),
            Status::kOk);
  EXPECT_EQ(event.gtids.ToString(), set.ToString());
}

TEST(CodecControlEvents, PreviousGtidsEmptySet) {
  const Bytes wire(8, 0);
  PreviousGtidsEvent event;
  ASSERT_EQ(ParsePreviousGtidsEvent(HeaderOf(EventType::kPreviousGtidsEvent), wire.data(),
                                    wire.size(), &event),
            Status::kOk);
  EXPECT_TRUE(event.gtids.empty());
}

TEST(CodecControlEvents, PreviousGtidsErrorsFollowDecodeBinary) {
  PreviousGtidsEvent event;
  const Bytes short_wire(7, 0);
  EXPECT_EQ(ParsePreviousGtidsEvent(HeaderOf(EventType::kPreviousGtidsEvent), short_wire.data(),
                                    short_wire.size(), &event),
            Status::kTruncated);
  EXPECT_EQ(ParsePreviousGtidsEvent(HeaderOf(EventType::kPreviousGtidsEvent), nullptr, 8, &event),
            Status::kTruncated);
  Bytes trailing(9, 0);
  EXPECT_EQ(ParsePreviousGtidsEvent(HeaderOf(EventType::kPreviousGtidsEvent), trailing.data(),
                                    trailing.size(), &event),
            Status::kMalformedValue);
}

}  // namespace
}  // namespace mybinlog
