// mybinlog::Decoder: the same event sequence whatever the Feed() split (whole,
// one byte at a time, random) on every fixture and matrix configuration;
// error offsets and messages for truncated and corrupted streams; payload
// expansion; TABLE_MAP lifetime; streams without the binlog magic.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <variant>
#include <vector>

#include "matrix_replay.h"
#include "mybinlog/compression.h"
#include "mybinlog/crc32.h"
#include "mybinlog/decoder.h"
#include "mybinlog/disposition.h"
#include "mybinlog/event.h"
#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/format.h"
#include "mybinlog/status.h"
#include "mybinlog/value.h"

namespace mybinlog {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

fs::path FixtureDir() { return fs::path(__FILE__).parent_path() / "fixtures"; }

const char* const kFixtures[] = {"mysql84_none_tagged", "mysql84_crc32_tagged",
                                 "mysql97_none_tagged", "mysql97_crc32_tagged",
                                 "mariadb118_none",     "mariadb118_crc32"};

Bytes ReadFixture(const std::string& name) {
  std::ifstream in(FixtureDir() / (name + ".bin"), std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

uint32_t ReadU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
         static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

void PutU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

struct Frame {
  size_t offset = 0;
  size_t length = 0;
  uint8_t type = 0;
};

// The events of a binlog file, by their event_length.
std::vector<Frame> Frames(const Bytes& file) {
  std::vector<Frame> frames;
  size_t pos = kBinlogMagicOffset;
  while (pos + kEventHeaderSize <= file.size()) {
    Frame frame;
    frame.offset = pos;
    frame.length = ReadU32(file.data() + pos + 9);
    frame.type = file[pos + 4];
    frames.push_back(frame);
    pos += frame.length;
  }
  return frames;
}

// ---- Event fingerprints -------------------------------------------------------

std::string ValueText(const Value& value) {
  std::string text;
  if (FormatValue(value, &text) != Status::kOk) return ValueKindName(value.kind);
  return std::string(ValueKindName(value.kind)) + ":" + text;
}

void AppendRow(const Row& row, std::string* out) {
  *out += "[";
  for (const Value& value : row) *out += ValueText(value) + "|";
  *out += "]";
}

// Everything observable about an event, offsets shifted by @p bias.
std::string Fingerprint(const Event& event, int64_t bias = 0) {
  std::string s =
      std::to_string(static_cast<int64_t>(event.offset) + bias) + " " +
      EventTypeName(event.header.type_code) + " len=" + std::to_string(event.header.event_length) +
      " " + DispositionName(event.disposition.disposition) +
      (event.from_payload ? " payload" : "") + " #" + std::to_string(event.body.index()) + " ";
  const EventBody& body = event.body;
  if (const auto* fde = std::get_if<FormatDescriptionEvent>(&body)) {
    s += fde->server_version + " " + std::to_string(static_cast<int>(fde->checksum_algorithm));
  } else if (const auto* rotate = std::get_if<RotateEvent>(&body)) {
    s += rotate->next_file + ":" + std::to_string(rotate->position);
  } else if (const auto* map = std::get_if<TableMap>(&body)) {
    s += std::to_string(map->table_id) + " " + map->schema + "." + map->table + " " +
         std::to_string(map->columns.size());
  } else if (const auto* rows = std::get_if<TableRowsEvent>(&body)) {
    s += std::to_string(rows->table_id) + " f=" + std::to_string(rows->flags) +
         " size=" + std::to_string(rows->size);
    if (rows->table != nullptr) s += " " + rows->table->schema + "." + rows->table->table;
    if (rows->decoded) {
      for (const Row& row : rows->rows.rows) AppendRow(row, &s);
      for (const auto& update : rows->rows.updates) {
        AppendRow(update.first, &s);
        AppendRow(update.second, &s);
      }
    }
  } else if (const auto* query = std::get_if<QueryEvent>(&body)) {
    s += query->schema + ":" + query->query;
  } else if (const auto* xid = std::get_if<XidEvent>(&body)) {
    s += std::to_string(xid->xid);
  } else if (const auto* gtid = std::get_if<GtidEvent>(&body)) {
    s += gtid->ToString();
  } else if (const auto* previous = std::get_if<PreviousGtidsEvent>(&body)) {
    s += previous->gtids.ToString();
  } else if (const auto* maria = std::get_if<MariaDbGtidEvent>(&body)) {
    s += maria->gtid.ToString() + " " + std::to_string(maria->flags);
  } else if (const auto* list = std::get_if<MariaDbGtidListEvent>(&body)) {
    s += list->gtids.ToString();
  } else if (const auto* annotate = std::get_if<MariaDbAnnotateRowsEvent>(&body)) {
    s += annotate->query;
  } else if (const auto* heartbeat = std::get_if<HeartbeatEvent>(&body)) {
    s += heartbeat->log_name + ":" + std::to_string(heartbeat->position);
  } else if (const auto* unknown = std::get_if<UnknownEvent>(&body)) {
    s += std::to_string(unknown->size) +
         " crc=" + std::to_string(ComputeCrc32(unknown->body, unknown->size));
  }
  return s;
}

struct DecodeRun {
  std::vector<std::string> events;
  Status status = Status::kOk;  // the status that ended the run
  DecodeError error;
};

// Feeds @p data in the chunk sizes @p next_chunk returns, draining Next() after each.
template <typename ChunkFn>
DecodeRun Decode(const Bytes& data, ChunkFn next_chunk,
                 const DecoderOptions& options = DecoderOptions(), int64_t bias = 0) {
  DecodeRun run;
  Decoder decoder(options);
  size_t pos = 0;
  while (true) {
    if (pos < data.size()) {
      const size_t n = std::min(next_chunk(), data.size() - pos);
      decoder.Feed(data.data() + pos, n);
      pos += n;
    }
    Event event;
    Status status;
    while ((status = decoder.Next(&event)) == Status::kOk) {
      run.events.push_back(Fingerprint(event, bias));
    }
    if (status != Status::kNeedMoreData) {
      run.status = status;
      break;
    }
    if (pos == data.size()) {
      run.status = decoder.Finish();
      break;
    }
  }
  run.error = decoder.last_error();
  return run;
}

DecodeRun DecodeWhole(const Bytes& data, const DecoderOptions& options = DecoderOptions()) {
  return Decode(data, [&] { return data.size(); }, options);
}

void ExpectSplitsAgree(const Bytes& data) {
  const DecodeRun whole = DecodeWhole(data);
  const DecodeRun single = Decode(data, [] { return size_t{1}; });
  EXPECT_EQ(single.status, whole.status);
  EXPECT_EQ(single.error.offset, whole.error.offset);
  EXPECT_EQ(single.error.message, whole.error.message);
  ASSERT_EQ(single.events, whole.events);
  for (const uint32_t seed : {1U, 2U, 3U}) {
    SCOPED_TRACE(seed);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> size(1, 4096);
    const DecodeRun random = Decode(data, [&] { return size(rng); });
    EXPECT_EQ(random.status, whole.status);
    EXPECT_EQ(random.error.message, whole.error.message);
    ASSERT_EQ(random.events, whole.events);
  }
}

TEST(CodecDecoder, FixtureSplitsAgreeAndNothingIsDropped) {
  for (const char* name : kFixtures) {
    SCOPED_TRACE(name);
    const Bytes file = ReadFixture(name);
    ASSERT_FALSE(file.empty());
    const DecodeRun whole = DecodeWhole(file);
    EXPECT_EQ(whole.status, Status::kOk) << whole.error.message;
    const std::vector<Frame> frames = Frames(file);
    ASSERT_EQ(whole.events.size(), frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
      EXPECT_EQ(whole.events[i].rfind(std::to_string(frames[i].offset) + " ", 0), 0u)
          << whole.events[i];
    }
    ExpectSplitsAgree(file);
  }
}

TEST(CodecDecoder, FixtureEventsAreTyped) {
  for (const char* name : kFixtures) {
    SCOPED_TRACE(name);
    const bool mariadb = std::string(name).rfind("mariadb", 0) == 0;
    const Bytes file = ReadFixture(name);
    Decoder decoder;
    decoder.Feed(file.data(), file.size());
    Event event;
    size_t rows_events = 0;
    size_t unknown = 0;
    size_t gtids = 0;
    size_t annotates = 0;
    const TableMap* last_map = nullptr;
    TableMap last_map_copy;
    while (decoder.Next(&event) == Status::kOk) {
      if (const auto* map = std::get_if<TableMap>(&event.body)) {
        last_map_copy = *map;
        last_map = &last_map_copy;
      }
      if (const auto* rows = std::get_if<TableRowsEvent>(&event.body)) {
        ++rows_events;
        ASSERT_NE(rows->table, nullptr);
        ASSERT_NE(last_map, nullptr);
        // The pointer names the TABLE_MAP that preceded the event.
        EXPECT_EQ(rows->table->table_id, last_map->table_id);
        EXPECT_EQ(rows->table->table, last_map->table);
        EXPECT_EQ(rows->table->columns.size(), last_map->columns.size());
        EXPECT_TRUE(rows->decoded);
        EXPECT_EQ(event.disposition.disposition, Disposition::kRows);
      }
      if (std::holds_alternative<UnknownEvent>(event.body)) ++unknown;
      if (std::holds_alternative<GtidEvent>(event.body) ||
          std::holds_alternative<MariaDbGtidEvent>(event.body)) {
        ++gtids;
      }
      if (std::holds_alternative<MariaDbAnnotateRowsEvent>(event.body)) ++annotates;
    }
    EXPECT_EQ(decoder.flavor(), mariadb ? ServerFlavor::kMariaDB : ServerFlavor::kMySQL);
    EXPECT_EQ(rows_events, mariadb ? 4u : 6u);
    EXPECT_GT(gtids, 0u);
    if (mariadb) {
      EXPECT_EQ(annotates, rows_events);
    }
    // MariaDB's BINLOG_CHECKPOINT (two per file) is the only type here without a parsed form.
    EXPECT_EQ(unknown, mariadb ? 2u : 0u);
  }
}

TEST(CodecDecoder, StreamWithoutMagicDecodesTheSameEvents) {
  const Bytes file = ReadFixture("mysql84_crc32_tagged");
  const Bytes stream(file.begin() + kBinlogMagicOffset, file.end());
  const DecodeRun from_file = DecodeWhole(file);
  const DecodeRun from_stream =
      Decode(stream, [&] { return stream.size(); }, DecoderOptions(), kBinlogMagicOffset);
  EXPECT_EQ(from_stream.status, Status::kOk);
  EXPECT_EQ(from_stream.events, from_file.events);
}

TEST(CodecDecoder, NeedMoreDataIsNotAnError) {
  const Bytes file = ReadFixture("mysql84_crc32_tagged");
  Decoder decoder;
  Event event;
  EXPECT_EQ(decoder.Next(&event), Status::kNeedMoreData);
  decoder.Feed(file.data(), 10);
  EXPECT_EQ(decoder.Next(&event), Status::kNeedMoreData);
  EXPECT_EQ(decoder.last_error().status, Status::kOk);
  EXPECT_EQ(decoder.buffered_bytes(), 6u);  // the magic was consumed
  EXPECT_EQ(Decoder().Finish(), Status::kOk);
}

TEST(CodecDecoder, TruncatedStreamReportsTheUnfinishedEvent) {
  const Bytes file = ReadFixture("mysql84_crc32_tagged");
  const Frame last = Frames(file).back();
  ASSERT_EQ(last.type, static_cast<uint8_t>(EventType::kRotateEvent));

  const Bytes cut_body(file.begin(), file.end() - 10);
  DecodeRun run = DecodeWhole(cut_body);
  EXPECT_EQ(run.status, Status::kTruncated);
  EXPECT_EQ(run.error.status, Status::kTruncated);
  EXPECT_EQ(run.error.offset, last.offset);
  EXPECT_NE(run.error.message.find("ROTATE_EVENT at offset " + std::to_string(last.offset)),
            std::string::npos)
      << run.error.message;
  EXPECT_NE(run.error.message.find(std::to_string(last.length - 10) + " of " +
                                   std::to_string(last.length) + " bytes"),
            std::string::npos)
      << run.error.message;

  const Bytes cut_header(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(last.offset + 5));
  run = DecodeWhole(cut_header);
  EXPECT_EQ(run.status, Status::kTruncated);
  EXPECT_EQ(run.error.offset, last.offset);
  EXPECT_NE(run.error.message.find("inside an event header"), std::string::npos);

  run = DecodeWhole(Bytes(file.begin(), file.begin() + 2));
  EXPECT_EQ(run.status, Status::kTruncated);
  EXPECT_EQ(run.error.offset, 0u);
  EXPECT_NE(run.error.message.find("magic"), std::string::npos);
}

TEST(CodecDecoder, ChecksumMismatchNamesTheEventAndSticks) {
  const Bytes file = ReadFixture("mysql84_crc32_tagged");
  Frame query;
  for (const Frame& frame : Frames(file)) {
    if (frame.type == static_cast<uint8_t>(EventType::kQueryEvent)) {
      query = frame;
      break;
    }
  }
  ASSERT_GT(query.length, 0u);
  Bytes corrupt = file;
  corrupt[query.offset + query.length - kChecksumSize - 1] ^= 0x01;  // last statement byte

  Decoder decoder;
  decoder.Feed(corrupt.data(), corrupt.size());
  Event event;
  Status status;
  size_t before = 0;
  while ((status = decoder.Next(&event)) == Status::kOk) ++before;
  EXPECT_EQ(status, Status::kChecksumMismatch);
  EXPECT_GT(before, 0u);
  EXPECT_EQ(decoder.last_error().offset, query.offset);
  EXPECT_NE(
      decoder.last_error().message.find("QUERY_EVENT at offset " + std::to_string(query.offset)),
      std::string::npos);
  EXPECT_NE(decoder.last_error().message.find("kChecksumMismatch"), std::string::npos);
  EXPECT_EQ(decoder.Next(&event), Status::kChecksumMismatch);
  EXPECT_EQ(decoder.Finish(), Status::kChecksumMismatch);

  decoder.Reset();
  EXPECT_EQ(decoder.last_error().status, Status::kOk);
  decoder.Feed(file.data(), file.size());
  while ((status = decoder.Next(&event)) == Status::kOk) {
  }
  EXPECT_EQ(status, Status::kNeedMoreData);

  // Without verification the altered statement comes through.
  DecoderOptions options;
  options.verify_checksum = false;
  const DecodeRun unverified = DecodeWhole(corrupt, options);
  EXPECT_EQ(unverified.status, Status::kOk);
  EXPECT_EQ(unverified.events.size(), Frames(file).size());
  EXPECT_NE(unverified.events, DecodeWhole(file).events);
}

TEST(CodecDecoder, BadEventLengthsFail) {
  const Bytes file = ReadFixture("mysql84_none_tagged");
  const std::vector<Frame> frames = Frames(file);
  Bytes corrupt = file;
  PutU32(corrupt.data() + frames[1].offset + 9, 10);
  DecodeRun run = DecodeWhole(corrupt);
  EXPECT_EQ(run.status, Status::kMalformedValue);
  EXPECT_EQ(run.error.offset, frames[1].offset);
  EXPECT_EQ(run.events.size(), 1u);

  Frame largest;
  for (const Frame& frame : frames) {
    if (frame.length > largest.length) largest = frame;
  }
  DecoderOptions options;
  options.max_event_size = largest.length - 1;
  run = DecodeWhole(file, options);
  EXPECT_EQ(run.status, Status::kLimitExceeded);
  EXPECT_EQ(run.error.offset, largest.offset);
  EXPECT_NE(run.error.message.find(std::string(EventTypeName(largest.type)) + " at offset"),
            std::string::npos);
}

TEST(CodecDecoder, RotateDropsTheTableMaps) {
  const Bytes file = ReadFixture("mysql84_crc32_tagged");
  const std::vector<Frame> frames = Frames(file);
  auto find = [&](EventType type) {
    for (const Frame& frame : frames) {
      if (frame.type == static_cast<uint8_t>(type)) return frame;
    }
    return Frame();
  };
  const Frame fde = find(EventType::kFormatDescriptionEvent);
  const Frame map = find(EventType::kTableMapEvent);
  const Frame rows = find(EventType::kWriteRowsEvent);
  const Frame rotate = frames.back();
  auto splice = [&](std::initializer_list<Frame> parts) {
    Bytes out(file.begin(), file.begin() + kBinlogMagicOffset);
    for (const Frame& f : parts) {
      out.insert(out.end(), file.begin() + static_cast<std::ptrdiff_t>(f.offset),
                 file.begin() + static_cast<std::ptrdiff_t>(f.offset + f.length));
    }
    return out;
  };

  DecodeRun run = DecodeWhole(splice({fde, map, rows}));
  EXPECT_EQ(run.status, Status::kOk) << run.error.message;

  const Bytes stream = splice({fde, map, rotate, rows});
  run = DecodeWhole(stream);
  EXPECT_EQ(run.status, Status::kInvalidMetadata);
  EXPECT_EQ(run.events.size(), 3u);
  EXPECT_EQ(run.error.offset, kBinlogMagicOffset + fde.length + map.length + rotate.length);
  EXPECT_NE(run.error.message.find("WRITE_ROWS_EVENT"), std::string::npos);
  EXPECT_NE(run.error.message.find("no TABLE_MAP for table id"), std::string::npos);

  run = DecodeWhole(splice({fde, map, fde, rows}));
  EXPECT_EQ(run.status, Status::kInvalidMetadata);
}

TEST(CodecDecoder, DecodeRowsOffKeepsTheRawBody) {
  const Bytes file = ReadFixture("mariadb118_crc32");
  DecoderOptions options;
  options.decode_rows = false;
  Decoder decoder(options);
  decoder.Feed(file.data(), file.size());
  Event event;
  size_t rows_events = 0;
  while (decoder.Next(&event) == Status::kOk) {
    const auto* rows = std::get_if<TableRowsEvent>(&event.body);
    if (rows == nullptr) continue;
    ++rows_events;
    EXPECT_FALSE(rows->decoded);
    EXPECT_NE(rows->table, nullptr);
    EXPECT_TRUE(rows->rows.rows.empty() && rows->rows.updates.empty());
    ASSERT_NE(rows->body, nullptr);
    ASSERT_EQ(rows->size, event.header.event_length - kEventHeaderSize - kChecksumSize);
    EXPECT_EQ(std::memcmp(rows->body, file.data() + event.offset + kEventHeaderSize, rows->size),
              0);
    break;
  }
  EXPECT_EQ(rows_events, 1u);
}

// ---- Hand-built network streams ------------------------------------------------

Bytes Header(EventType type, size_t body_size, bool trailer, uint32_t next_position = 0) {
  Bytes out(kEventHeaderSize, 0);
  out[4] = static_cast<uint8_t>(type);
  PutU32(out.data() + 5, 1);  // server_id
  PutU32(out.data() + 9,
         static_cast<uint32_t>(kEventHeaderSize + body_size + (trailer ? kChecksumSize : 0)));
  PutU32(out.data() + 13, next_position);
  return out;
}

Bytes BuildEvent(EventType type, const Bytes& body, bool trailer, uint32_t next_position = 0) {
  Bytes out = Header(type, body.size(), trailer, next_position);
  out.insert(out.end(), body.begin(), body.end());
  if (trailer) {
    uint8_t crc[4];
    PutU32(crc, ComputeCrc32(out.data(), out.size()));
    out.insert(out.end(), crc, crc + 4);
  }
  return out;
}

Bytes QueryBody(const std::string& query) {
  Bytes body(13, 0);  // thread, exec time, db_len 0, error 0, status_vars_len 0
  body.push_back(0);  // empty schema's NUL
  body.insert(body.end(), query.begin(), query.end());
  return body;
}

Bytes XidBody(uint64_t xid) {
  Bytes body(8);
  for (int i = 0; i < 8; ++i) body[i] = static_cast<uint8_t>(xid >> (8 * i));
  return body;
}

// TRANSACTION_PAYLOAD with compression NONE around @p inner (events without trailers).
Bytes PayloadBody(const Bytes& inner) {
  Bytes body = {0x02, 0x03, 0xFC, 0xFF, 0x00};  // compression type 255 (NONE)
  body.push_back(0x01);                         // payload size
  body.push_back(0x01);
  body.push_back(static_cast<uint8_t>(inner.size()));
  body.push_back(0x00);  // end mark
  body.insert(body.end(), inner.begin(), inner.end());
  return body;
}

DecoderOptions NoChecksum() {
  DecoderOptions options;
  options.default_checksum = ChecksumAlgorithm::kOff;
  return options;
}

TEST(CodecDecoder, PayloadEventsComeOutInOrder) {
  Bytes inner = BuildEvent(EventType::kQueryEvent, QueryBody("BEGIN"), false);
  const Bytes xid = BuildEvent(EventType::kXidEvent, XidBody(42), false);
  inner.insert(inner.end(), xid.begin(), xid.end());
  ASSERT_LT(inner.size(), 251u);
  Bytes stream = BuildEvent(EventType::kTransactionPayloadEvent, PayloadBody(inner), false);
  const size_t payload_length = stream.size();
  const Bytes after = BuildEvent(EventType::kXidEvent, XidBody(43), false);
  stream.insert(stream.end(), after.begin(), after.end());

  Decoder decoder(NoChecksum());
  decoder.Feed(stream.data(), stream.size());
  Event event;
  ASSERT_EQ(decoder.Next(&event), Status::kOk);
  ASSERT_TRUE(std::holds_alternative<QueryEvent>(event.body));
  EXPECT_EQ(std::get<QueryEvent>(event.body).query, "BEGIN");
  EXPECT_TRUE(event.from_payload);
  EXPECT_EQ(event.offset, 0u);
  ASSERT_EQ(decoder.Next(&event), Status::kOk);
  ASSERT_TRUE(std::holds_alternative<XidEvent>(event.body));
  EXPECT_EQ(std::get<XidEvent>(event.body).xid, 42u);
  EXPECT_TRUE(event.from_payload);
  ASSERT_EQ(decoder.Next(&event), Status::kOk);
  EXPECT_EQ(std::get<XidEvent>(event.body).xid, 43u);
  EXPECT_FALSE(event.from_payload);
  EXPECT_EQ(event.offset, payload_length);
  EXPECT_EQ(decoder.Next(&event), Status::kNeedMoreData);

  const DecodeRun single = Decode(stream, [] { return size_t{1}; }, NoChecksum());
  EXPECT_EQ(single.events, DecodeWhole(stream, NoChecksum()).events);
  EXPECT_EQ(single.events.size(), 3u);

  DecoderOptions keep = NoChecksum();
  keep.expand_compressed = false;
  Decoder raw(keep);
  raw.Feed(stream.data(), stream.size());
  ASSERT_EQ(raw.Next(&event), Status::kOk);
  ASSERT_TRUE(std::holds_alternative<UnknownEvent>(event.body));
  EXPECT_EQ(std::get<UnknownEvent>(event.body).size, payload_length - kEventHeaderSize);
  EXPECT_EQ(event.disposition.disposition, Disposition::kCompressed);
  EXPECT_FALSE(event.from_payload);
}

TEST(CodecDecoder, BrokenPayloadReportsThePayloadOffset) {
  const Bytes prefix = BuildEvent(EventType::kXidEvent, XidBody(1), false);
  Bytes inner = BuildEvent(EventType::kXidEvent, XidBody(2), false);
  inner.resize(inner.size() - 3);  // the inner event overruns the payload
  Bytes stream = prefix;
  const Bytes payload = BuildEvent(EventType::kTransactionPayloadEvent, PayloadBody(inner), false);
  stream.insert(stream.end(), payload.begin(), payload.end());
  const DecodeRun run = DecodeWhole(stream, NoChecksum());
  EXPECT_EQ(run.events.size(), 1u);
  EXPECT_EQ(run.status, Status::kTruncated);
  EXPECT_EQ(run.error.offset, prefix.size());
  EXPECT_NE(run.error.message.find("TRANSACTION_PAYLOAD"), std::string::npos);
}

TEST(CodecDecoder, HeartbeatsOnAChecksummedStream) {
  const std::string name = "binlog.000007";
  Bytes v2 = {0x01, static_cast<uint8_t>(name.size())};
  v2.insert(v2.end(), name.begin(), name.end());
  const Bytes position = {0x02, 0x09, 0xFE, 0x00, 0x00, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0x00};
  v2.insert(v2.end(), position.begin(), position.end());
  Bytes stream = BuildEvent(EventType::kHeartbeatLogEventV2, v2, true);
  const Bytes v1 =
      BuildEvent(EventType::kHeartbeatLogEvent, Bytes(name.begin(), name.end()), true, 1234);
  stream.insert(stream.end(), v1.begin(), v1.end());

  Decoder decoder;  // CRC32 assumed before any FDE
  decoder.Feed(stream.data(), stream.size());
  Event event;
  ASSERT_EQ(decoder.Next(&event), Status::kOk) << decoder.last_error().message;
  const auto* heartbeat = std::get_if<HeartbeatEvent>(&event.body);
  ASSERT_NE(heartbeat, nullptr);
  EXPECT_EQ(heartbeat->log_name, name);
  EXPECT_EQ(heartbeat->position, 0x140000000ULL);
  ASSERT_EQ(decoder.Next(&event), Status::kOk);
  heartbeat = std::get_if<HeartbeatEvent>(&event.body);
  ASSERT_NE(heartbeat, nullptr);
  EXPECT_EQ(heartbeat->log_name, name);
  EXPECT_EQ(heartbeat->position, 1234u);
}

TEST(CodecDecoder, UnknownEventsKeepTheirBytes) {
  const Bytes intvar = {0x02, 0x05, 0, 0, 0, 0, 0, 0, 0};  // INSERT_ID 5
  const Bytes stream = BuildEvent(EventType::kIntvarEvent, intvar, false);
  Decoder decoder(NoChecksum());
  decoder.Feed(stream.data(), stream.size());
  Event event;
  ASSERT_EQ(decoder.Next(&event), Status::kOk);
  const auto* unknown = std::get_if<UnknownEvent>(&event.body);
  ASSERT_NE(unknown, nullptr);
  ASSERT_EQ(unknown->size, intvar.size());
  EXPECT_EQ(std::memcmp(unknown->body, intvar.data(), intvar.size()), 0);
}

TEST(CodecDecoder, DummyTableIdNeedsNoTableMap) {
  Bytes body(6, 0xFF);
  body.insert(body.end(), {0x01, 0x00, 0x02, 0x00, 0x01, 0x00});  // STMT_END, extra len 2, 1 col
  const Bytes stream = BuildEvent(EventType::kWriteRowsEvent, body, false);
  Decoder decoder(NoChecksum());
  decoder.Feed(stream.data(), stream.size());
  Event event;
  ASSERT_EQ(decoder.Next(&event), Status::kOk) << decoder.last_error().message;
  const auto* rows = std::get_if<TableRowsEvent>(&event.body);
  ASSERT_NE(rows, nullptr);
  EXPECT_EQ(rows->table_id, kDummyTableId);
  EXPECT_EQ(rows->flags, kRowsStatementEndFlag);
  EXPECT_EQ(rows->table, nullptr);
  EXPECT_FALSE(rows->decoded);

  Bytes other = body;
  other[0] = 0x07;
  const Bytes missing = BuildEvent(EventType::kWriteRowsEvent, other, false);
  const DecodeRun run = DecodeWhole(missing, NoChecksum());
  EXPECT_EQ(run.status, Status::kInvalidMetadata);
  EXPECT_NE(run.error.message.find("table id 281474976710407"), std::string::npos)
      << run.error.message;
}

// ---- Version matrix ---------------------------------------------------------------

using matrix::MatrixConfig;

std::vector<fs::path> BinlogFiles(const MatrixConfig& config) {
  return matrix::BinlogFiles(config.dir());
}

class CodecDecoderMatrix : public testing::TestWithParam<MatrixConfig> {};

TEST_P(CodecDecoderMatrix, SplitsAgree) {
  const std::vector<fs::path> files = BinlogFiles(GetParam());
  ASSERT_FALSE(files.empty());
  for (const fs::path& path : files) {
    SCOPED_TRACE(path.string());
    Bytes file;
    ASSERT_TRUE(matrix::ReadGzip(path, &file));
    const DecodeRun whole = DecodeWhole(file);
    if (whole.status == Status::kUnsupported && !HasZstdSupport()) {
      EXPECT_NE(whole.error.message.find("TRANSACTION_PAYLOAD"), std::string::npos);
    } else {
      EXPECT_EQ(whole.status, Status::kOk) << whole.error.message;
    }
    EXPECT_GT(whole.events.size(), 0u);
    ExpectSplitsAgree(file);
  }
}

// Compressed configurations: the expanded events, in order, are exactly what the
// low-level walk finds inside each compressed event.
TEST_P(CodecDecoderMatrix, CompressedEventsExpandInOrder) {
  const MatrixConfig& config = GetParam();
  const bool mysql_zstd = config.config.find("_zstd") != std::string::npos;
  const bool mariadb_zlib = config.config.find("_zlib") != std::string::npos;
  if (!mysql_zstd && !mariadb_zlib) GTEST_SKIP() << "uncompressed configuration";
  if (mysql_zstd && !HasZstdSupport()) GTEST_SKIP() << "zstd payloads need MYBINLOG_WITH_ZSTD";
  for (const fs::path& path : BinlogFiles(config)) {
    SCOPED_TRACE(path.string());
    Bytes file;
    ASSERT_TRUE(matrix::ReadGzip(path, &file));
    std::vector<std::string> expected;  // "offset type" of every event after expansion
    size_t compressed = 0;
    for (const Frame& frame : Frames(file)) {
      EventHeader header;
      ASSERT_EQ(ParseEventHeader(file.data() + frame.offset, frame.length, &header), Status::kOk);
      if (frame.type == static_cast<uint8_t>(EventType::kTransactionPayloadEvent)) {
        ++compressed;
        TransactionPayloadEvent payload;
        const size_t trailer = config.config.rfind("crc32", 0) == 0 ? kChecksumSize : 0;
        ASSERT_EQ(
            ParseTransactionPayloadEvent(header, file.data() + frame.offset + kEventHeaderSize,
                                         frame.length - kEventHeaderSize - trailer, &payload),
            Status::kOk);
        Bytes events;
        ASSERT_EQ(DecompressTransactionPayload(payload, SIZE_MAX, &events), Status::kOk);
        size_t pos = 0;
        while (pos < events.size()) {
          PayloadEventView inner;
          ASSERT_EQ(NextPayloadEvent(events.data(), events.size(), &pos, &inner), Status::kOk);
          expected.push_back(std::to_string(frame.offset) + " " +
                             EventTypeName(inner.header.type_code) + " payload");
        }
        continue;
      }
      uint8_t type = frame.type;
      if (IsMariaDbCompressedEvent(type)) {
        ++compressed;
        type = type == static_cast<uint8_t>(EventType::kMariaDBQueryCompressedEvent)
                   ? static_cast<uint8_t>(EventType::kQueryEvent)
                   : static_cast<uint8_t>(type - 166 + 23);  // 166-171 -> 23-25, 30-32
        if (type > 25) type = static_cast<uint8_t>(type + 4);
      }
      expected.push_back(std::to_string(frame.offset) + " " + EventTypeName(type));
    }
    EXPECT_GT(compressed, 0u);

    Decoder decoder;
    decoder.Feed(file.data(), file.size());
    std::vector<std::string> actual;
    Event event;
    Status status;
    while ((status = decoder.Next(&event)) == Status::kOk) {
      actual.push_back(std::to_string(event.offset) + " " + EventTypeName(event.header.type_code) +
                       (event.from_payload ? " payload" : ""));
      if (const auto* rows = std::get_if<TableRowsEvent>(&event.body)) {
        EXPECT_TRUE(rows->decoded);
      }
    }
    ASSERT_EQ(status, Status::kNeedMoreData) << decoder.last_error().message;
    EXPECT_EQ(actual, expected);
  }
}

INSTANTIATE_TEST_SUITE_P(CodecDecoder, CodecDecoderMatrix,
                         testing::ValuesIn(matrix::DiscoverConfigs()), matrix::ConfigTestName);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(CodecDecoderMatrix);

}  // namespace
}  // namespace mybinlog
