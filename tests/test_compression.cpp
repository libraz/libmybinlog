// Compressed events from hand-built vectors: MariaDB's zlib-compressed QUERY and
// rows events (sql/log_event.cc, binlog_buf_compress / row_log_event_uncompress)
// and MySQL's TRANSACTION_PAYLOAD (libs/mysql/binlog/event/codecs/binary.cpp).

#include <gtest/gtest.h>
#include <zlib.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "mybinlog/compression.h"
#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/query_event.h"
#include "mybinlog/row_decoder.h"
#include "mybinlog/status.h"
#include "mybinlog/table_map.h"
#include "mybinlog/value.h"
#include "mybinlog/xid.h"

namespace mybinlog {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr size_t kNoLimit = std::numeric_limits<size_t>::max();

struct Writer {
  Bytes bytes;
  Writer& U8(uint64_t v) { return Le(v, 1); }
  Writer& Le(uint64_t v, int n) {
    for (int i = 0; i < n; ++i) bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return *this;
  }
  Writer& Be(uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return *this;
  }
  Writer& Str(const std::string& s) {
    bytes.insert(bytes.end(), s.begin(), s.end());
    return *this;
  }
  Writer& Raw(const Bytes& b) {
    bytes.insert(bytes.end(), b.begin(), b.end());
    return *this;
  }
};

Bytes Deflate(const Bytes& plain) {
  uLongf size = compressBound(static_cast<uLong>(plain.size()));
  Bytes out(size);
  EXPECT_EQ(compress(out.data(), &size, plain.data(), static_cast<uLong>(plain.size())), Z_OK);
  out.resize(size);
  return out;
}

// MariaDB's compressed block: header byte, big-endian original length, zlib stream.
Bytes CompressedBlock(const Bytes& plain, int length_bytes = 0, uint8_t extra_flags = 0) {
  if (length_bytes == 0) {
    length_bytes = plain.size() > 0xFFFFFF ? 4
                   : plain.size() > 0xFFFF ? 3
                   : plain.size() > 0xFF   ? 2
                                           : 1;
  }
  Writer w;
  w.U8(0x80 | extra_flags | static_cast<uint8_t>(length_bytes));
  w.Be(plain.size(), length_bytes);
  w.Raw(Deflate(plain));
  return w.bytes;
}

EventHeader Header(EventType type, size_t body_size, size_t trailer = 0) {
  EventHeader header;
  header.type_code = static_cast<uint8_t>(type);
  header.server_id = 7;
  header.timestamp = 1700000000;
  header.event_length = static_cast<uint32_t>(kEventHeaderSize + body_size + trailer);
  header.next_position = 4242;
  return header;
}

// QUERY body up to (and including) the database's NUL: what QUERY_COMPRESSED keeps clear.
Bytes QueryPrefix(const std::string& db) {
  Writer w;
  w.Le(99, 4).Le(1, 4).U8(db.size()).Le(0, 2);
  const Bytes vars = {0x00, 0x01, 0x00, 0x00, 0x00};  // Q_FLAGS2_CODE
  w.Le(vars.size(), 2).Raw(vars).Str(db).U8(0);
  return w.bytes;
}

// Rows body up to the column bitmaps: what the compressed rows events keep clear.
Bytes RowsPrefix(bool v2, bool update, uint64_t table_id = 0x123456) {
  Writer w;
  w.Le(table_id, 6).Le(1, 2);
  if (v2) w.Le(2, 2);
  w.U8(1).U8(0x01);
  if (update) w.U8(0x01);
  return w.bytes;
}

// One INT column row image: null bitmap then the value.
Bytes IntRow(int32_t v) {
  Writer w;
  w.U8(0).Le(static_cast<uint32_t>(v), 4);
  return w.bytes;
}

Bytes Concat(const Bytes& a, const Bytes& b) {
  Bytes out = a;
  out.insert(out.end(), b.begin(), b.end());
  return out;
}

TableMap IntTable() {
  TableMap table;
  table.table_id = 0x123456;
  table.schema = "db";
  table.table = "t";
  TableMapColumn column;
  column.type = ColumnType::kLong;
  table.columns.push_back(column);
  return table;
}

TEST(CodecCompressionMariaDb, QueryCompressedBecomesQuery) {
  const std::string text = "INSERT INTO t VALUES (1), (2), (3), (4), (5), (6), (7), (8)";
  const Bytes prefix = QueryPrefix("shop");
  const Bytes body = Concat(prefix, CompressedBlock(Bytes(text.begin(), text.end())));
  const EventHeader header = Header(EventType::kMariaDBQueryCompressedEvent, body.size(), 4);

  DecompressedEvent out;
  ASSERT_EQ(DecompressMariaDbEvent(header, body.data(), body.size(), kNoLimit, &out), Status::kOk);
  EXPECT_EQ(out.header.type_code, static_cast<uint8_t>(EventType::kQueryEvent));
  EXPECT_EQ(out.header.server_id, header.server_id);
  EXPECT_EQ(out.header.timestamp, header.timestamp);
  EXPECT_EQ(out.header.next_position, header.next_position);
  // The trailer the compressed event counted is still counted.
  EXPECT_EQ(out.header.event_length, kEventHeaderSize + prefix.size() + text.size() + 4);
  EXPECT_EQ(out.body, Concat(prefix, Bytes(text.begin(), text.end())));

  QueryEvent query;
  ASSERT_EQ(ParseQueryEvent(out.header, out.body.data(), out.body.size(), &query), Status::kOk);
  EXPECT_EQ(query.schema, "shop");
  EXPECT_EQ(query.query, text);
  EXPECT_EQ(query.thread_id, 99u);
}

TEST(CodecCompressionMariaDb, QueryWithEmptyDatabase) {
  const std::string text = "BEGIN";
  const Bytes prefix = QueryPrefix("");
  const Bytes body = Concat(prefix, CompressedBlock(Bytes(text.begin(), text.end())));
  DecompressedEvent out;
  ASSERT_EQ(DecompressMariaDbEvent(Header(EventType::kMariaDBQueryCompressedEvent, body.size()),
                                   body.data(), body.size(), kNoLimit, &out),
            Status::kOk);
  QueryEvent query;
  ASSERT_EQ(ParseQueryEvent(out.header, out.body.data(), out.body.size(), &query), Status::kOk);
  EXPECT_EQ(query.schema, "");
  EXPECT_EQ(query.query, "BEGIN");
}

struct RowsCase {
  EventType compressed;
  EventType plain;
  bool v2;
  bool update;
};

constexpr RowsCase kRowsCases[] = {
    {EventType::kMariaDBWriteRowsCompressedEventV1, EventType::kWriteRowsEventV1, false, false},
    {EventType::kMariaDBUpdateRowsCompressedEventV1, EventType::kUpdateRowsEventV1, false, true},
    {EventType::kMariaDBDeleteRowsCompressedEventV1, EventType::kDeleteRowsEventV1, false, false},
    {EventType::kMariaDBWriteRowsCompressedEvent, EventType::kWriteRowsEvent, true, false},
    {EventType::kMariaDBUpdateRowsCompressedEvent, EventType::kUpdateRowsEvent, true, true},
    {EventType::kMariaDBDeleteRowsCompressedEvent, EventType::kDeleteRowsEvent, true, false},
};

TEST(CodecCompressionMariaDb, RowsCompressedBecomeRowsAndDecode) {
  for (const RowsCase& c : kRowsCases) {
    SCOPED_TRACE(EventTypeName(c.compressed));
    const Bytes prefix = RowsPrefix(c.v2, c.update);
    const Bytes rows = c.update
                           ? Concat(Concat(IntRow(1), IntRow(2)), Concat(IntRow(-3), IntRow(4)))
                           : Concat(IntRow(1), IntRow(-2));
    const Bytes body = Concat(prefix, CompressedBlock(rows));

    DecompressedEvent out;
    ASSERT_EQ(DecompressMariaDbEvent(Header(c.compressed, body.size()), body.data(), body.size(),
                                     kNoLimit, &out),
              Status::kOk);
    EXPECT_EQ(out.header.type_code, static_cast<uint8_t>(c.plain));
    EXPECT_EQ(out.header.event_length, kEventHeaderSize + prefix.size() + rows.size());
    ASSERT_EQ(out.body, Concat(prefix, rows));

    RowsEvent decoded;
    ASSERT_EQ(DecodeRowsEvent(out.header, out.body.data(), out.body.size(), IntTable(),
                              DecodeOptions(), &decoded),
              Status::kOk);
    if (c.update) {
      ASSERT_EQ(decoded.updates.size(), 2u);
      EXPECT_EQ(decoded.updates[0].first[0].i, 1);
      EXPECT_EQ(decoded.updates[0].second[0].i, 2);
      EXPECT_EQ(decoded.updates[1].first[0].i, -3);
      EXPECT_EQ(decoded.updates[1].second[0].i, 4);
    } else {
      ASSERT_EQ(decoded.rows.size(), 2u);
      EXPECT_EQ(decoded.rows[0][0].i, 1);
      EXPECT_EQ(decoded.rows[1][0].i, -2);
    }
  }
}

TEST(CodecCompressionMariaDb, V2ExtraDataStaysClear) {
  Writer prefix;
  prefix.Le(0x123456, 6).Le(0, 2).Le(5, 2).U8(0xAA).U8(0xBB).U8(0xCC).U8(1).U8(0x01);
  const Bytes rows = IntRow(9);
  const Bytes body = Concat(prefix.bytes, CompressedBlock(rows));
  DecompressedEvent out;
  ASSERT_EQ(DecompressMariaDbEvent(Header(EventType::kMariaDBWriteRowsCompressedEvent, body.size()),
                                   body.data(), body.size(), kNoLimit, &out),
            Status::kOk);
  EXPECT_EQ(out.body, Concat(prefix.bytes, rows));
}

TEST(CodecCompressionMariaDb, WideColumnCountsSpanTheirBitmaps) {
  // 300 columns: a 3-byte packed count and 38-byte bitmaps, two for UPDATE.
  Writer prefix;
  prefix.Le(1, 6).Le(0, 2).U8(0xFC).Le(300, 2).Raw(Bytes(38, 0xFF)).Raw(Bytes(38, 0xFF));
  const Bytes rows(64, 0x5A);
  const Bytes body = Concat(prefix.bytes, CompressedBlock(rows));
  DecompressedEvent out;
  ASSERT_EQ(
      DecompressMariaDbEvent(Header(EventType::kMariaDBUpdateRowsCompressedEventV1, body.size()),
                             body.data(), body.size(), kNoLimit, &out),
      Status::kOk);
  EXPECT_EQ(out.body, Concat(prefix.bytes, rows));
}

TEST(CodecCompressionMariaDb, LengthOfLengthVariants) {
  const Bytes rows = Concat(IntRow(1), IntRow(2));
  const Bytes prefix = RowsPrefix(false, false);
  // The writer picks the narrowest width, but every width 1..4 names the same length.
  for (int width = 1; width <= 4; ++width) {
    SCOPED_TRACE(width);
    const Bytes body = Concat(prefix, CompressedBlock(rows, width));
    DecompressedEvent out;
    ASSERT_EQ(
        DecompressMariaDbEvent(Header(EventType::kMariaDBWriteRowsCompressedEventV1, body.size()),
                               body.data(), body.size(), kNoLimit, &out),
        Status::kOk);
    EXPECT_EQ(out.body, Concat(prefix, rows));
  }
  // Widths the writer emits for 2-, 3- and 4-byte lengths.
  for (const size_t size : {size_t{0x100}, size_t{0x10000}, size_t{0x1000000}}) {
    SCOPED_TRACE(size);
    const Bytes big(size, 0x11);
    const Bytes block = CompressedBlock(big);
    EXPECT_EQ(block[0] & 0x07, size == 0x100 ? 2 : size == 0x10000 ? 3 : 4);
    const Bytes body = Concat(prefix, block);
    DecompressedEvent out;
    ASSERT_EQ(
        DecompressMariaDbEvent(Header(EventType::kMariaDBWriteRowsCompressedEventV1, body.size()),
                               body.data(), body.size(), kNoLimit, &out),
        Status::kOk);
    EXPECT_EQ(out.body.size(), prefix.size() + size);
  }
}

TEST(CodecCompressionMariaDb, HeaderByteVariants) {
  const Bytes rows = IntRow(5);
  const Bytes prefix = RowsPrefix(false, false);
  const Bytes good = CompressedBlock(rows);
  const auto with_byte = [&](uint8_t first) {
    Bytes block = good;
    block[0] = first;
    return Concat(prefix, block);
  };
  const auto run = [](const Bytes& body) {
    DecompressedEvent out;
    return DecompressMariaDbEvent(
        Header(EventType::kMariaDBWriteRowsCompressedEventV1, body.size()), body.data(),
        body.size(), kNoLimit, &out);
  };
  EXPECT_EQ(run(with_byte(0x81)), Status::kOk);
  // Bit 3 is not read by the source either.
  EXPECT_EQ(run(with_byte(0x89)), Status::kOk);
  // Bit 7 clear: not a compressed block.
  EXPECT_EQ(run(with_byte(0x01)), Status::kMalformedValue);
  // Length-of-length outside 1..4.
  for (const uint8_t first : {uint8_t{0x80}, uint8_t{0x85}, uint8_t{0x86}, uint8_t{0x87}}) {
    EXPECT_EQ(run(with_byte(first)), Status::kMalformedValue) << int{first};
  }
  // Algorithms other than zlib (0).
  for (const uint8_t first : {uint8_t{0x91}, uint8_t{0xA1}, uint8_t{0xC1}, uint8_t{0xF1}}) {
    EXPECT_EQ(run(with_byte(first)), Status::kUnsupported) << int{first};
  }
}

TEST(CodecCompressionMariaDb, MaxSizeBoundsTheRebuiltBody) {
  const Bytes rows(1000, 0x42);
  const Bytes prefix = RowsPrefix(false, false);
  const Bytes body = Concat(prefix, CompressedBlock(rows));
  const EventHeader header = Header(EventType::kMariaDBWriteRowsCompressedEventV1, body.size());
  DecompressedEvent out;
  EXPECT_EQ(
      DecompressMariaDbEvent(header, body.data(), body.size(), prefix.size() + rows.size(), &out),
      Status::kOk);
  DecompressedEvent untouched;
  untouched.body = {1, 2, 3};
  EXPECT_EQ(DecompressMariaDbEvent(header, body.data(), body.size(),
                                   prefix.size() + rows.size() - 1, &untouched),
            Status::kLimitExceeded);
  EXPECT_EQ(untouched.body, (Bytes{1, 2, 3}));
  EXPECT_EQ(DecompressMariaDbEvent(header, body.data(), body.size(), 0, &untouched),
            Status::kLimitExceeded);
  // A declared length near 4 GiB is refused before anything is allocated.
  Writer huge;
  huge.Raw(prefix).U8(0x84).Be(0xFFFFFFF0u, 4).Raw(Deflate(rows));
  EXPECT_EQ(
      DecompressMariaDbEvent(header, huge.bytes.data(), huge.bytes.size(), 1 << 20, &untouched),
      Status::kLimitExceeded);
}

TEST(CodecCompressionMariaDb, OriginalLengthMustBeExact) {
  const Bytes rows(100, 0x33);
  const Bytes prefix = RowsPrefix(false, false);
  const Bytes stream = Deflate(rows);
  const auto run = [&](uint32_t declared) {
    Writer w;
    w.Raw(prefix).U8(0x81).Be(declared, 1).Raw(stream);
    DecompressedEvent out;
    return DecompressMariaDbEvent(
        Header(EventType::kMariaDBWriteRowsCompressedEventV1, w.bytes.size()), w.bytes.data(),
        w.bytes.size(), kNoLimit, &out);
  };
  EXPECT_EQ(run(100), Status::kOk);
  EXPECT_EQ(run(99), Status::kMalformedValue);
  EXPECT_EQ(run(101), Status::kMalformedValue);
  EXPECT_EQ(run(0), Status::kMalformedValue);
}

TEST(CodecCompressionMariaDb, CorruptStreams) {
  const Bytes rows(200, 0x77);
  const Bytes prefix = RowsPrefix(true, false);
  const Bytes good = Concat(prefix, CompressedBlock(rows));
  const auto run = [](const Bytes& body) {
    DecompressedEvent out;
    return DecompressMariaDbEvent(Header(EventType::kMariaDBWriteRowsCompressedEvent, body.size()),
                                  body.data(), body.size(), kNoLimit, &out);
  };
  ASSERT_EQ(run(good), Status::kOk);

  Bytes flipped = good;
  flipped[prefix.size() + 4] ^= 0xFF;  // inside the zlib stream
  EXPECT_EQ(run(flipped), Status::kMalformedValue);

  Bytes trailing = good;
  trailing.push_back(0x00);
  EXPECT_EQ(run(trailing), Status::kMalformedValue);

  Bytes cut = good;
  cut.pop_back();
  EXPECT_EQ(run(cut), Status::kMalformedValue);

  // Only the header byte and length: no stream at all.
  Writer empty;
  empty.Raw(prefix).U8(0x81).U8(200);
  EXPECT_EQ(run(empty.bytes), Status::kMalformedValue);
}

TEST(CodecCompressionMariaDb, TruncatedClearFields) {
  const Bytes rows_good = Concat(RowsPrefix(true, true), CompressedBlock(IntRow(1)));
  // Every cut that ends inside the clear fields or the compression header.
  const size_t clear = RowsPrefix(true, true).size() + 2;
  for (size_t cut = 0; cut < clear; ++cut) {
    DecompressedEvent out;
    EXPECT_EQ(DecompressMariaDbEvent(Header(EventType::kMariaDBUpdateRowsCompressedEvent, cut),
                                     rows_good.data(), cut, kNoLimit, &out),
              Status::kTruncated)
        << cut;
  }
  const Bytes query_good = Concat(QueryPrefix("db"), CompressedBlock(Bytes{'x'}));
  for (size_t cut = 0; cut < QueryPrefix("db").size() + 2; ++cut) {
    DecompressedEvent out;
    EXPECT_EQ(DecompressMariaDbEvent(Header(EventType::kMariaDBQueryCompressedEvent, cut),
                                     query_good.data(), cut, kNoLimit, &out),
              Status::kTruncated)
        << cut;
  }
  DecompressedEvent out;
  EXPECT_EQ(DecompressMariaDbEvent(Header(EventType::kMariaDBQueryCompressedEvent, 0), nullptr, 0,
                                   kNoLimit, &out),
            Status::kTruncated);
}

TEST(CodecCompressionMariaDb, BadClearFields) {
  // A v2 extra-data length below its own two bytes.
  Writer extra;
  extra.Le(1, 6).Le(0, 2).Le(1, 2).U8(1).U8(1);
  const Bytes body = Concat(extra.bytes, CompressedBlock(IntRow(1)));
  DecompressedEvent out;
  EXPECT_EQ(DecompressMariaDbEvent(Header(EventType::kMariaDBWriteRowsCompressedEvent, body.size()),
                                   body.data(), body.size(), kNoLimit, &out),
            Status::kInvalidMetadata);
  // A column count with the NULL marker.
  Writer count;
  count.Le(1, 6).Le(0, 2).U8(0xFB).U8(1);
  const Bytes body2 = Concat(count.bytes, CompressedBlock(IntRow(1)));
  EXPECT_EQ(
      DecompressMariaDbEvent(Header(EventType::kMariaDBWriteRowsCompressedEventV1, body2.size()),
                             body2.data(), body2.size(), kNoLimit, &out),
      Status::kInvalidMetadata);
}

TEST(CodecCompressionMariaDb, OnlyCompressedTypes) {
  const Bytes body = Concat(RowsPrefix(false, false), CompressedBlock(IntRow(1)));
  for (const EventType type :
       {EventType::kWriteRowsEventV1, EventType::kQueryEvent,
        EventType::kMariaDBPartialRowDataEvent, EventType::kTransactionPayloadEvent}) {
    DecompressedEvent out;
    EXPECT_EQ(
        DecompressMariaDbEvent(Header(type, body.size()), body.data(), body.size(), kNoLimit, &out),
        Status::kUnsupported)
        << EventTypeName(type);
  }
  for (int code = 0; code < 256; ++code) {
    EXPECT_EQ(IsMariaDbCompressedEvent(static_cast<uint8_t>(code)), code >= 165 && code <= 171)
        << code;
  }
}

// --- TRANSACTION_PAYLOAD -----------------------------------------------------

constexpr uint64_t kEndMark = 0;
constexpr uint64_t kSizeField = 1;
constexpr uint64_t kCompressionField = 2;
constexpr uint64_t kUncompressedField = 3;

// A packed integer as net_store_length writes it.
Bytes Packed(uint64_t v) {
  Writer w;
  if (v < 0xFB) {
    w.U8(v);
  } else if (v <= 0xFFFF) {
    w.U8(0xFC).Le(v, 2);
  } else if (v <= 0xFFFFFF) {
    w.U8(0xFD).Le(v, 3);
  } else {
    w.U8(0xFE).Le(v, 8);
  }
  return w.bytes;
}

Bytes Field(uint64_t type, uint64_t value) {
  const Bytes v = Packed(value);
  return Concat(Concat(Packed(type), Packed(v.size())), v);
}

// A whole inner event: header (no checksum) and an 8-byte XID body.
Bytes XidFrame(uint64_t xid) {
  Writer w;
  w.Le(1700000000, 4).U8(static_cast<uint8_t>(EventType::kXidEvent)).Le(1, 4);
  w.Le(kEventHeaderSize + 8, 4).Le(0, 4).Le(0, 2).Le(xid, 8);
  return w.bytes;
}

// A zstd frame of raw blocks (RFC 8878 3.1): single segment, 1-byte content size.
Bytes ZstdRawFrame(const Bytes& content) {
  EXPECT_LE(content.size(), 255u);
  Writer w;
  w.Le(0xFD2FB528u, 4).U8(0x20).U8(content.size());
  w.Le(1 | (content.size() << 3), 3).Raw(content);  // last block, raw
  return w.bytes;
}

// A zstd frame of one RLE block: @p count copies of @p byte, content size unstated.
Bytes ZstdRleFrame(uint8_t byte, size_t count) {
  Writer w;
  // Frame header descriptor 0: no content size, window descriptor follows.
  w.Le(0xFD2FB528u, 4).U8(0x00).U8(0x58);  // window 2^(10+11) = 2 MiB
  w.Le(1 | (1 << 1) | (count << 3), 3).U8(byte);
  return w.bytes;
}

Bytes PayloadBody(const Bytes& fields, const Bytes& payload) { return Concat(fields, payload); }

EventHeader PayloadHeader(size_t body_size) {
  return Header(EventType::kTransactionPayloadEvent, body_size);
}

TEST(CodecCompressionPayload, ParsesFieldsInAnyOrder) {
  const Bytes events = Concat(XidFrame(1), XidFrame(2));
  // The order the source writes: compression, uncompressed size (not for NONE), size, end.
  const Bytes fields = Concat(
      Concat(Field(kCompressionField, 255), Field(kSizeField, events.size())), Packed(kEndMark));
  const Bytes body = PayloadBody(fields, events);
  TransactionPayloadEvent event;
  ASSERT_EQ(
      ParseTransactionPayloadEvent(PayloadHeader(body.size()), body.data(), body.size(), &event),
      Status::kOk);
  EXPECT_EQ(event.compression, PayloadCompression::kNone);
  EXPECT_EQ(event.payload_size, events.size());
  EXPECT_FALSE(event.has_uncompressed_size);
  EXPECT_EQ(event.payload, body.data() + fields.size());

  // Reordered, with an unknown field skipped by its length.
  Writer unknown;
  unknown.Raw(Packed(9)).Raw(Packed(3)).U8(0xFB).U8(0xFF).U8(0x00);
  const Bytes fields2 = Concat(
      Concat(Concat(Field(kSizeField, events.size()), unknown.bytes),
             Concat(Field(kUncompressedField, events.size()), Field(kCompressionField, 255))),
      Packed(kEndMark));
  const Bytes body2 = PayloadBody(fields2, events);
  TransactionPayloadEvent event2;
  ASSERT_EQ(ParseTransactionPayloadEvent(PayloadHeader(body2.size()), body2.data(), body2.size(),
                                         &event2),
            Status::kOk);
  EXPECT_TRUE(event2.has_uncompressed_size);
  EXPECT_EQ(event2.uncompressed_size, events.size());
  EXPECT_EQ(event2.payload, body2.data() + fields2.size());
}

TEST(CodecCompressionPayload, NoneExpandsToTheInnerEvents) {
  const Bytes events = Concat(Concat(XidFrame(10), XidFrame(11)), XidFrame(12));
  const Bytes body =
      PayloadBody(Concat(Concat(Field(kCompressionField, 255), Field(kSizeField, events.size())),
                         Packed(kEndMark)),
                  events);
  TransactionPayloadEvent event;
  ASSERT_EQ(
      ParseTransactionPayloadEvent(PayloadHeader(body.size()), body.data(), body.size(), &event),
      Status::kOk);
  Bytes expanded;
  ASSERT_EQ(DecompressTransactionPayload(event, kNoLimit, &expanded), Status::kOk);
  ASSERT_EQ(expanded, events);

  size_t offset = 0;
  uint64_t want = 10;
  while (offset < expanded.size()) {
    PayloadEventView view;
    ASSERT_EQ(NextPayloadEvent(expanded.data(), expanded.size(), &offset, &view), Status::kOk);
    EXPECT_EQ(view.header.type_code, static_cast<uint8_t>(EventType::kXidEvent));
    EXPECT_EQ(view.size, 8u);  // no checksum trailer inside a payload
    XidEvent xid;
    ASSERT_EQ(ParseXidEvent(view.header, view.body, view.size, &xid), Status::kOk);
    EXPECT_EQ(xid.xid, want++);
  }
  EXPECT_EQ(want, 13u);
  EXPECT_EQ(offset, expanded.size());

  Bytes small;
  EXPECT_EQ(DecompressTransactionPayload(event, events.size() - 1, &small), Status::kLimitExceeded);
  EXPECT_TRUE(small.empty());
}

TEST(CodecCompressionPayload, ZstdExpandsWhenSupported) {
  const Bytes first = Concat(XidFrame(20), XidFrame(21));
  const Bytes second = XidFrame(22);
  // Two frames back to back, as a compressor that flushed mid-transaction writes them.
  const Bytes payload = Concat(ZstdRawFrame(first), ZstdRawFrame(second));
  const Bytes all = Concat(first, second);
  const Bytes body =
      PayloadBody(Concat(Concat(Field(kCompressionField, 0), Field(kUncompressedField, all.size())),
                         Concat(Field(kSizeField, payload.size()), Packed(kEndMark))),
                  payload);
  TransactionPayloadEvent event;
  ASSERT_EQ(
      ParseTransactionPayloadEvent(PayloadHeader(body.size()), body.data(), body.size(), &event),
      Status::kOk);
  EXPECT_EQ(event.compression, PayloadCompression::kZstd);
  Bytes expanded;
  if (!HasZstdSupport()) {
    EXPECT_EQ(DecompressTransactionPayload(event, kNoLimit, &expanded), Status::kUnsupported);
    return;
  }
  ASSERT_EQ(DecompressTransactionPayload(event, kNoLimit, &expanded), Status::kOk);
  EXPECT_EQ(expanded, all);
  EXPECT_EQ(DecompressTransactionPayload(event, all.size(), &expanded), Status::kOk);
  // The declared size alone exceeds the ceiling.
  EXPECT_EQ(DecompressTransactionPayload(event, all.size() - 1, &expanded), Status::kLimitExceeded);
}

TEST(CodecCompressionPayload, ZstdLimitAndCorruption) {
  if (!HasZstdSupport()) GTEST_SKIP() << "built without zstd";
  const auto event_for = [](const Bytes& payload, bool declare, uint64_t declared) {
    TransactionPayloadEvent event;
    event.compression = PayloadCompression::kZstd;
    event.payload_size = payload.size();
    event.payload = payload.data();
    event.has_uncompressed_size = declare;
    event.uncompressed_size = declared;
    return event;
  };
  Bytes expanded;
  // Without a declared size, the stream itself is cut off at the ceiling.
  const Bytes rle = ZstdRleFrame(0xAB, 100000);
  ASSERT_EQ(DecompressTransactionPayload(event_for(rle, false, 0), kNoLimit, &expanded),
            Status::kOk);
  EXPECT_EQ(expanded, Bytes(100000, 0xAB));
  EXPECT_EQ(DecompressTransactionPayload(event_for(rle, false, 0), 100000, &expanded), Status::kOk);
  Bytes untouched = {7};
  EXPECT_EQ(DecompressTransactionPayload(event_for(rle, false, 0), 99999, &untouched),
            Status::kLimitExceeded);
  EXPECT_EQ(untouched, Bytes{7});
  // A declared size that disagrees with the stream.
  EXPECT_EQ(DecompressTransactionPayload(event_for(rle, true, 99999), kNoLimit, &untouched),
            Status::kMalformedValue);
  EXPECT_EQ(DecompressTransactionPayload(event_for(rle, true, 100001), kNoLimit, &untouched),
            Status::kMalformedValue);

  const Bytes good = ZstdRawFrame(XidFrame(1));
  Bytes magic = good;
  magic[0] ^= 0xFF;
  EXPECT_EQ(DecompressTransactionPayload(event_for(magic, false, 0), kNoLimit, &untouched),
            Status::kMalformedValue);
  const Bytes cut(good.begin(), good.end() - 1);
  EXPECT_EQ(DecompressTransactionPayload(event_for(cut, false, 0), kNoLimit, &untouched),
            Status::kMalformedValue);
  EXPECT_EQ(DecompressTransactionPayload(event_for(Bytes(), false, 0), kNoLimit, &untouched),
            Status::kMalformedValue);
  EXPECT_EQ(untouched, Bytes{7});
}

TEST(CodecCompressionPayload, MalformedFields) {
  const Bytes events = XidFrame(1);
  const auto run = [&](const Bytes& fields, const Bytes& payload) {
    const Bytes body = PayloadBody(fields, payload);
    TransactionPayloadEvent event;
    return ParseTransactionPayloadEvent(PayloadHeader(body.size()), body.data(), body.size(),
                                        &event);
  };
  const Bytes compression = Field(kCompressionField, 255);
  const Bytes size = Field(kSizeField, events.size());
  const Bytes end = Packed(kEndMark);
  EXPECT_EQ(run(Concat(Concat(compression, size), end), events), Status::kOk);
  // Missing payload size or compression type.
  EXPECT_EQ(run(Concat(compression, end), events), Status::kInvalidMetadata);
  EXPECT_EQ(run(Concat(size, end), events), Status::kInvalidMetadata);
  // No end mark: the fields run to the end of the body.
  EXPECT_EQ(run(Concat(compression, size), Bytes()), Status::kTruncated);
  // Payload size past the body.
  EXPECT_EQ(run(Concat(Concat(compression, Field(kSizeField, events.size() + 1)), end), events),
            Status::kTruncated);
  // Unknown compression type.
  EXPECT_EQ(run(Concat(Concat(Field(kCompressionField, 1), size), end), events),
            Status::kUnsupported);
  // Unknown field longer than the body.
  Writer unknown;
  unknown.Raw(Packed(7)).Raw(Packed(200)).U8(0);
  EXPECT_EQ(run(Concat(Concat(compression, unknown.bytes), Bytes()), Bytes()), Status::kTruncated);
  // NULL marker where a packed integer belongs.
  EXPECT_EQ(run(Bytes{0xFB}, events), Status::kInvalidMetadata);
  TransactionPayloadEvent event;
  EXPECT_EQ(ParseTransactionPayloadEvent(PayloadHeader(0), nullptr, 0, &event), Status::kTruncated);
  // Every cut inside the fields.
  const Bytes fields = Concat(Concat(compression, size), end);
  const Bytes body = PayloadBody(fields, events);
  for (size_t cut = 0; cut < fields.size(); ++cut) {
    EXPECT_EQ(ParseTransactionPayloadEvent(PayloadHeader(cut), body.data(), cut, &event),
              Status::kTruncated)
        << cut;
  }
}

TEST(CodecCompressionPayload, NoneSizeMismatch) {
  const Bytes events = XidFrame(3);
  TransactionPayloadEvent event;
  event.compression = PayloadCompression::kNone;
  event.payload = events.data();
  event.payload_size = events.size();
  event.has_uncompressed_size = true;
  event.uncompressed_size = events.size() + 1;
  Bytes out;
  EXPECT_EQ(DecompressTransactionPayload(event, kNoLimit, &out), Status::kMalformedValue);
  event.uncompressed_size = events.size();
  EXPECT_EQ(DecompressTransactionPayload(event, kNoLimit, &out), Status::kOk);
}

TEST(CodecCompressionPayload, InnerEventFraming) {
  const Bytes events = Concat(XidFrame(1), XidFrame(2));
  size_t offset = 0;
  PayloadEventView view;
  // Short of a header.
  EXPECT_EQ(NextPayloadEvent(events.data(), 10, &offset, &view), Status::kTruncated);
  // Event length past the buffer.
  EXPECT_EQ(NextPayloadEvent(events.data(), 26, &offset, &view), Status::kTruncated);
  EXPECT_EQ(offset, 0u);
  // Event length below the header size.
  Bytes bad = events;
  bad[9] = 18;
  EXPECT_EQ(NextPayloadEvent(bad.data(), bad.size(), &offset, &view), Status::kMalformedValue);
  EXPECT_EQ(NextPayloadEvent(nullptr, 0, &offset, &view), Status::kTruncated);
  offset = events.size() + 1;
  EXPECT_EQ(NextPayloadEvent(events.data(), events.size(), &offset, &view), Status::kTruncated);
  offset = events.size();
  EXPECT_EQ(NextPayloadEvent(events.data(), events.size(), &offset, &view), Status::kTruncated);
}

}  // namespace
}  // namespace mybinlog
