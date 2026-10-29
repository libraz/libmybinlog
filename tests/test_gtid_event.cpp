// GTID_LOG_EVENT / GTID_TAGGED_LOG_EVENT bodies, synthetic and from real captures.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "mybinlog/checksum.h"
#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/mariadb_gtid.h"
#include "mybinlog/mysql_gtid.h"

namespace mybinlog {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr char kSid[] = "3e11fa47-71ca-11e1-9e33-c80aa9429562";

EventHeader HeaderOf(EventType type) {
  EventHeader header;
  header.type_code = static_cast<uint8_t>(type);
  header.server_id = 1;
  return header;
}

// Upstream varlen writer (libs/mysql/serialization/variable_length_integers.h).
void PutVarlen(Bytes* out, uint64_t value) {
  int width = 0;
  for (uint64_t v = value; v != 0; v >>= 1) ++width;
  const size_t bytes = static_cast<size_t>(((width * 575) >> 12) + 1);
  out->push_back(static_cast<uint8_t>(((1U << (bytes - 1)) - 1) | (value << bytes)));
  if (bytes == 1) return;
  const uint64_t rest = value >> (8 - bytes + ((bytes + 7) >> 4));
  for (size_t i = 0; i + 1 < bytes; ++i) out->push_back(static_cast<uint8_t>(rest >> (8 * i)));
}

uint64_t ZigZag(int64_t value) {
  return (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63);
}

struct TaggedFields {
  uint64_t format_version = 1;
  uint64_t last_non_ignorable = 0;
  bool with_flags = true;
  bool with_uuid = true;
  bool with_gno = true;
  bool with_tag = true;
  uint64_t flags = 1;
  int64_t gno = 7;
  std::string tag = "mytag";
  uint64_t uuid_byte_override = 0;  ///< Non-zero replaces the first UUID byte value.
  Bytes extra_fields;               ///< Appended after the tag (already id-prefixed).
  int64_t size_adjust = 0;          ///< Added to the encoded message size.
};

Bytes BuildTagged(const TaggedFields& f) {
  Uuid uuid{};
  EXPECT_EQ(ParseUuid(kSid, &uuid), Status::kOk);
  Bytes fields;
  if (f.with_flags) {
    PutVarlen(&fields, 0);
    PutVarlen(&fields, f.flags);
  }
  if (f.with_uuid) {
    PutVarlen(&fields, 1);
    for (size_t i = 0; i < uuid.size(); ++i) {
      PutVarlen(&fields, i == 0 && f.uuid_byte_override != 0 ? f.uuid_byte_override : uuid[i]);
    }
  }
  if (f.with_gno) {
    PutVarlen(&fields, 2);
    PutVarlen(&fields, ZigZag(f.gno));
  }
  if (f.with_tag) {
    PutVarlen(&fields, 3);
    PutVarlen(&fields, f.tag.size());
    fields.insert(fields.end(), f.tag.begin(), f.tag.end());
  }
  fields.insert(fields.end(), f.extra_fields.begin(), f.extra_fields.end());

  Bytes head;
  PutVarlen(&head, f.format_version);
  Bytes tail;
  PutVarlen(&tail, f.last_non_ignorable);
  // The size counts itself, so iterate until its own width is stable.
  uint64_t size = head.size() + tail.size() + fields.size() + 1;
  for (;;) {
    Bytes encoded;
    PutVarlen(&encoded, size);
    const uint64_t next = head.size() + encoded.size() + tail.size() + fields.size();
    if (next == size) break;
    size = next;
  }
  Bytes body = head;
  PutVarlen(&body, static_cast<uint64_t>(static_cast<int64_t>(size) + f.size_adjust));
  body.insert(body.end(), tail.begin(), tail.end());
  body.insert(body.end(), fields.begin(), fields.end());
  return body;
}

Bytes BuildUntagged(uint64_t gno, uint8_t flags = 1) {
  Uuid uuid{};
  EXPECT_EQ(ParseUuid(kSid, &uuid), Status::kOk);
  Bytes body;
  body.push_back(flags);
  body.insert(body.end(), uuid.begin(), uuid.end());
  for (int i = 0; i < 8; ++i) body.push_back(static_cast<uint8_t>(gno >> (8 * i)));
  // Logical clock and the rest of the post-header follow on real servers.
  body.insert(body.end(), {0x02, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0});
  return body;
}

Status ParseTagged(const Bytes& body, GtidEvent* out) {
  return ParseGtidEvent(HeaderOf(EventType::kGtidTaggedLogEvent), body.data(), body.size(), out);
}

// --- Varlen writer sanity: matches the bytes a real server wrote ---

TEST(CodecGtidEvent, VarlenWriterMatchesServerBytes) {
  Bytes out;
  PutVarlen(&out, 1);
  PutVarlen(&out, 57);
  PutVarlen(&out, 0xbd);
  EXPECT_EQ(out, (Bytes{0x02, 0x72, 0xf5, 0x02}));
}

// --- Untagged (type 33) ---

TEST(CodecGtidEvent, UntaggedEvent) {
  const Bytes body = BuildUntagged(42, 1);
  GtidEvent event;
  ASSERT_EQ(ParseGtidEvent(HeaderOf(EventType::kGtidLogEvent), body.data(), body.size(), &event),
            Status::kOk);
  EXPECT_EQ(event.flags, 1);
  EXPECT_TRUE(event.tag.empty());
  EXPECT_EQ(event.gno, 42u);
  EXPECT_EQ(event.ToString(), std::string(kSid) + ":42");
}

TEST(CodecGtidEvent, UntaggedGnoBounds) {
  const EventHeader header = HeaderOf(EventType::kGtidLogEvent);
  GtidEvent event;
  for (uint64_t gno : {uint64_t{0}, kGnoEnd, kGnoEnd + 1, ~uint64_t{0}}) {
    const Bytes body = BuildUntagged(gno);
    EXPECT_EQ(ParseGtidEvent(header, body.data(), body.size(), &event), Status::kMalformedValue)
        << gno;
  }
  const Bytes body = BuildUntagged(kGnoEnd - 1);
  ASSERT_EQ(ParseGtidEvent(header, body.data(), body.size(), &event), Status::kOk);
  EXPECT_EQ(event.gno, kGnoEnd - 1);
}

TEST(CodecGtidEvent, UntaggedTruncated) {
  const Bytes body = BuildUntagged(5);
  GtidEvent event;
  EXPECT_EQ(ParseGtidEvent(HeaderOf(EventType::kGtidLogEvent), body.data(), 24, &event),
            Status::kTruncated);
  EXPECT_EQ(ParseGtidEvent(HeaderOf(EventType::kGtidLogEvent), body.data(), 25, &event),
            Status::kOk);
}

TEST(CodecGtidEvent, OtherEventTypesAreUnsupported) {
  const Bytes body = BuildUntagged(5);
  GtidEvent event;
  for (EventType type :
       {EventType::kAnonymousGtidLogEvent, EventType::kMariaDBGtidEvent, EventType::kQueryEvent}) {
    EXPECT_EQ(ParseGtidEvent(HeaderOf(type), body.data(), body.size(), &event),
              Status::kUnsupported);
  }
}

// --- Tagged (type 42) ---

TEST(CodecGtidEvent, TaggedEvent) {
  TaggedFields fields;
  fields.tag = "MyTag";
  GtidEvent event;
  ASSERT_EQ(ParseTagged(BuildTagged(fields), &event), Status::kOk);
  EXPECT_EQ(event.flags, 1);
  EXPECT_EQ(event.tag, "mytag");
  EXPECT_EQ(event.gno, 7u);
  EXPECT_EQ(event.ToString(), std::string(kSid) + ":mytag:7");
}

TEST(CodecGtidEvent, TaggedEventSkipsTheKnownTrailingFields) {
  TaggedFields fields;
  // last_committed (4), sequence_number (5), immediate_commit_timestamp (6).
  for (uint64_t id : {4, 5, 6}) {
    PutVarlen(&fields.extra_fields, id);
    PutVarlen(&fields.extra_fields, ZigZag(1234567));
  }
  fields.last_non_ignorable = 7;
  GtidEvent event;
  ASSERT_EQ(ParseTagged(BuildTagged(fields), &event), Status::kOk);
  EXPECT_EQ(event.gno, 7u);
}

TEST(CodecGtidEvent, TaggedEventAcceptsUnknownIgnorableFields) {
  // Field 20 is unknown, but last_non_ignorable_field_id says it may be skipped.
  TaggedFields fields;
  PutVarlen(&fields.extra_fields, 20);
  PutVarlen(&fields.extra_fields, 99);
  fields.last_non_ignorable = 12;
  GtidEvent event;
  ASSERT_EQ(ParseTagged(BuildTagged(fields), &event), Status::kOk);
  EXPECT_EQ(event.ToString(), std::string(kSid) + ":mytag:7");
}

TEST(CodecGtidEvent, TaggedEventRefusesUnknownNonIgnorableFields) {
  TaggedFields fields;
  PutVarlen(&fields.extra_fields, 12);
  PutVarlen(&fields.extra_fields, 99);
  fields.last_non_ignorable = 13;
  GtidEvent event;
  EXPECT_EQ(ParseTagged(BuildTagged(fields), &event), Status::kUnsupported);
}

TEST(CodecGtidEvent, TaggedEventRefusesUnknownFormatVersion) {
  TaggedFields fields;
  fields.format_version = 2;
  GtidEvent event;
  EXPECT_EQ(ParseTagged(BuildTagged(fields), &event), Status::kUnsupported);
}

// The layout without the leading format version (size first) must not parse.
TEST(CodecGtidEvent, TaggedEventDoesNotReadTheSizeFirst) {
  const Bytes body = BuildTagged(TaggedFields{});
  Bytes without_version(body.begin() + 1, body.end());
  GtidEvent event;
  EXPECT_NE(ParseTagged(without_version, &event), Status::kOk);
}

TEST(CodecGtidEvent, TaggedEventGnoBounds) {
  GtidEvent event;
  for (int64_t gno : {int64_t{0}, int64_t{-1}, INT64_MAX, INT64_MIN}) {
    TaggedFields fields;
    fields.gno = gno;
    EXPECT_EQ(ParseTagged(BuildTagged(fields), &event), Status::kMalformedValue) << gno;
  }
  TaggedFields fields;
  fields.gno = INT64_MAX - 1;
  ASSERT_EQ(ParseTagged(BuildTagged(fields), &event), Status::kOk);
  EXPECT_EQ(event.gno, static_cast<uint64_t>(INT64_MAX - 1));
}

TEST(CodecGtidEvent, TaggedEventTagRules) {
  GtidEvent event;
  for (const std::string& tag :
       {std::string(33, 'a'), std::string("9tag"), std::string("a-b"), std::string("t\xc3\xa9")}) {
    TaggedFields fields;
    fields.tag = tag;
    EXPECT_EQ(ParseTagged(BuildTagged(fields), &event), Status::kMalformedValue) << tag;
  }
  TaggedFields longest;
  longest.tag = std::string(32, '_');
  EXPECT_EQ(ParseTagged(BuildTagged(longest), &event), Status::kOk);

  // An empty or absent tag field decodes as an untagged GTID.
  TaggedFields empty;
  empty.tag.clear();
  ASSERT_EQ(ParseTagged(BuildTagged(empty), &event), Status::kOk);
  EXPECT_EQ(event.ToString(), std::string(kSid) + ":7");
  TaggedFields absent;
  absent.with_tag = false;
  ASSERT_EQ(ParseTagged(BuildTagged(absent), &event), Status::kOk);
  EXPECT_TRUE(event.tag.empty());
}

TEST(CodecGtidEvent, TaggedEventFieldPresence) {
  GtidEvent event;
  TaggedFields no_flags;
  no_flags.with_flags = false;
  ASSERT_EQ(ParseTagged(BuildTagged(no_flags), &event), Status::kOk);
  EXPECT_EQ(event.flags, 0);

  TaggedFields no_uuid;
  no_uuid.with_uuid = false;
  EXPECT_EQ(ParseTagged(BuildTagged(no_uuid), &event), Status::kMalformedValue);

  TaggedFields no_gno;
  no_gno.with_gno = false;
  EXPECT_EQ(ParseTagged(BuildTagged(no_gno), &event), Status::kMalformedValue);

  TaggedFields wide_uuid_byte;
  wide_uuid_byte.uuid_byte_override = 256;
  EXPECT_EQ(ParseTagged(BuildTagged(wide_uuid_byte), &event), Status::kMalformedValue);

  TaggedFields wide_flags;
  wide_flags.flags = 256;
  EXPECT_EQ(ParseTagged(BuildTagged(wide_flags), &event), Status::kMalformedValue);
}

TEST(CodecGtidEvent, TaggedEventSizeMustMatchTheBody) {
  GtidEvent event;
  TaggedFields larger;
  larger.size_adjust = 1;
  EXPECT_EQ(ParseTagged(BuildTagged(larger), &event), Status::kTruncated);
  TaggedFields smaller;
  smaller.size_adjust = -1;
  EXPECT_EQ(ParseTagged(BuildTagged(smaller), &event), Status::kMalformedValue);
}

TEST(CodecGtidEvent, TaggedEventTruncatedAtEveryLength) {
  // Every proper prefix fails without reading past it (the size no longer matches).
  const Bytes body = BuildTagged(TaggedFields{});
  GtidEvent event;
  for (size_t len = 0; len < body.size(); ++len) {
    Bytes prefix(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(len));
    EXPECT_NE(ParseTagged(prefix, &event), Status::kOk) << len;
  }
  EXPECT_EQ(ParseTagged(Bytes{}, &event), Status::kTruncated);
  EXPECT_EQ(ParseTagged(Bytes{0x02}, &event), Status::kTruncated);
}

// --- Real captures (fixtures/README.md) ---

bool ReadFixture(const char* name, Bytes* out) {
  const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !out->empty();
}

struct Frame {
  EventHeader header;
  const uint8_t* body;
  size_t body_length;
};

// Walk a whole binlog file through ChecksumTracker; bodies exclude trailers.
void WalkFixture(const char* name, std::vector<Frame>* frames, Bytes* storage) {
  ASSERT_TRUE(ReadFixture(name, storage)) << name;
  ASSERT_GE(storage->size(), kBinlogMagicOffset);
  ChecksumTracker tracker;
  size_t pos = kBinlogMagicOffset;
  while (pos < storage->size()) {
    EventHeader header;
    ASSERT_EQ(ParseEventHeader(storage->data() + pos, storage->size() - pos, &header), Status::kOk);
    ASSERT_GE(header.event_length, kEventHeaderSize);
    ASSERT_LE(header.event_length, storage->size() - pos);
    FrameLayout layout;
    ASSERT_EQ(tracker.OnEvent(header, storage->data() + pos, header.event_length, &layout),
              Status::kOk);
    frames->push_back({header, storage->data() + pos + kEventHeaderSize, layout.body_length()});
    pos += header.event_length;
  }
}

TEST(CodecGtid, TaggedEventFromRealCapture) {
  for (const char* name : {"mysql84_none_tagged.bin", "mysql84_crc32_tagged.bin",
                           "mysql97_none_tagged.bin", "mysql97_crc32_tagged.bin"}) {
    SCOPED_TRACE(name);
    Bytes storage;
    std::vector<Frame> frames;
    WalkFixture(name, &frames, &storage);

    std::vector<GtidEvent> untagged;
    std::vector<GtidEvent> tagged;
    GtidSet previous;
    bool saw_previous = false;
    for (const Frame& frame : frames) {
      if (frame.header.type_code == static_cast<uint8_t>(EventType::kPreviousGtidsEvent)) {
        PreviousGtidsEvent event;
        ASSERT_EQ(ParsePreviousGtidsEvent(frame.header, frame.body, frame.body_length, &event),
                  Status::kOk);
        previous = std::move(event.gtids);
        saw_previous = true;
        continue;
      }
      if (frame.header.type_code != static_cast<uint8_t>(EventType::kGtidLogEvent) &&
          frame.header.type_code != static_cast<uint8_t>(EventType::kGtidTaggedLogEvent)) {
        continue;
      }
      GtidEvent event;
      ASSERT_EQ(ParseGtidEvent(frame.header, frame.body, frame.body_length, &event), Status::kOk)
          << EventTypeName(frame.header.type_code);
      if (frame.header.type_code == static_cast<uint8_t>(EventType::kGtidTaggedLogEvent)) {
        tagged.push_back(event);
      } else {
        EXPECT_TRUE(event.tag.empty());
        untagged.push_back(event);
      }
    }

    ASSERT_EQ(tagged.size(), 2u);
    ASSERT_FALSE(untagged.empty());
    const std::string uuid = FormatUuid(untagged.front().uuid);
    for (size_t i = 0; i < tagged.size(); ++i) {
      EXPECT_EQ(tagged[i].uuid, untagged.front().uuid);
      EXPECT_EQ(tagged[i].tag, "mytag");
      EXPECT_EQ(tagged[i].ToString(), uuid + ":mytag:" + std::to_string(tagged[i].gno));
    }
    EXPECT_EQ(tagged[1].gno, tagged[0].gno + 1);
    for (size_t i = 1; i < untagged.size(); ++i) {
      EXPECT_EQ(untagged[i].uuid, untagged.front().uuid);
      EXPECT_EQ(untagged[i].gno, untagged[i - 1].gno + 1);
    }

    // The file's PREVIOUS_GTIDS ends where its first transaction starts.
    ASSERT_TRUE(saw_previous);
    EXPECT_EQ(previous.ToString(), uuid + ":1-" + std::to_string(untagged.front().gno - 1));

    // Every GTID in the file, tagged ones included, accumulates into one set.
    GtidSet executed = previous;
    for (const auto* list : {&untagged, &tagged}) {
      for (const GtidEvent& event : *list) {
        ASSERT_EQ(executed.Add(event.uuid, event.tag, {event.gno, event.gno + 1}), Status::kOk);
      }
    }
    EXPECT_EQ(executed.ToString(), uuid + ":1-" + std::to_string(untagged.back().gno) + "," + uuid +
                                       ":mytag:" + std::to_string(tagged[0].gno) + "-" +
                                       std::to_string(tagged[1].gno));
    std::vector<uint8_t> wire;
    executed.EncodeBinary(&wire);
    GtidSet round_trip;
    ASSERT_EQ(GtidSet::DecodeBinary(wire.data(), wire.size(), &round_trip), Status::kOk);
    EXPECT_EQ(round_trip.ToString(), executed.ToString());
  }
}

TEST(CodecGtid, MariaDbEventsFromRealCapture) {
  for (const char* name : {"mariadb118_none.bin", "mariadb118_crc32.bin"}) {
    SCOPED_TRACE(name);
    Bytes storage;
    std::vector<Frame> frames;
    WalkFixture(name, &frames, &storage);

    size_t lists = 0;
    size_t annotations = 0;
    std::vector<MariaDbGtidEvent> gtids;
    for (const Frame& frame : frames) {
      switch (static_cast<EventType>(frame.header.type_code)) {
        case EventType::kMariaDBGtidListEvent: {
          MariaDbGtidListEvent list;
          ASSERT_EQ(ParseMariaDbGtidListEvent(frame.header, frame.body, frame.body_length, &list),
                    Status::kOk);
          EXPECT_TRUE(list.gtids.empty());  // First binlog of a fresh server.
          ++lists;
          break;
        }
        case EventType::kMariaDBGtidEvent: {
          MariaDbGtidEvent event;
          ASSERT_EQ(ParseMariaDbGtidEvent(frame.header, frame.body, frame.body_length, &event),
                    Status::kOk);
          gtids.push_back(event);
          break;
        }
        case EventType::kMariaDBAnnotateRowsEvent: {
          MariaDbAnnotateRowsEvent annotate;
          ASSERT_EQ(
              ParseMariaDbAnnotateRowsEvent(frame.header, frame.body, frame.body_length, &annotate),
              Status::kOk);
          const std::string& sql = annotate.query;
          EXPECT_TRUE(sql.rfind("INSERT", 0) == 0 || sql.rfind("UPDATE", 0) == 0 ||
                      sql.rfind("DELETE", 0) == 0)
              << sql;
          ++annotations;
          break;
        }
        default:
          break;
      }
    }
    EXPECT_EQ(lists, 1u);
    EXPECT_GT(annotations, 0u);
    ASSERT_FALSE(gtids.empty());
    for (size_t i = 0; i < gtids.size(); ++i) {
      EXPECT_EQ(gtids[i].gtid.domain_id, 0u);
      EXPECT_EQ(gtids[i].gtid.server_id, 1u);
      EXPECT_EQ(gtids[i].gtid.sequence_no, i + 1);
      EXPECT_EQ(gtids[i].gtid.ToString(), "0-1-" + std::to_string(i + 1));
    }
    // The first group is DDL, written standalone.
    EXPECT_TRUE(gtids.front().standalone());
  }
}

}  // namespace
}  // namespace mybinlog
