// MariaDB COMPRESSED columns (sql/field_comp.cc) and the text/bytes decision
// they share with their uncompressed counterparts.

#include <gtest/gtest.h>
#include <zlib.h>

#include <cstdint>
#include <string>
#include <vector>

#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/row_decoder.h"
#include "mybinlog/status.h"
#include "mybinlog/table_map.h"
#include "mybinlog/value.h"

namespace mybinlog {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t kTextCollation = 45;
constexpr uint32_t kBinaryCollation = 63;

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

TableMapColumn Column(ColumnType type, uint32_t meta, bool charset_known = false,
                      uint32_t collation = kBinaryCollation) {
  TableMapColumn column;
  column.type = type;
  column.meta = meta;
  column.has_charset = charset_known;
  column.charset = collation;
  return column;
}

EventHeader WriteRowsHeader() {
  EventHeader header;
  header.type_code = static_cast<uint8_t>(EventType::kWriteRowsEvent);
  return header;
}

Status DecodeOne(const TableMapColumn& column, const Bytes& value, Value* out,
                 const DecodeOptions& options = DecodeOptions()) {
  TableMap table;
  table.table_id = 1;
  table.columns = {column};
  Writer w;
  w.Le(1, 6).Le(0, 2).Le(2, 2).U8(1).U8(0x01).U8(0x00).Raw(value);
  const Bytes body(w.bytes.begin(), w.bytes.end());
  RowsEvent data;
  const Status status =
      DecodeRowsEvent(WriteRowsHeader(), body.data(), body.size(), table, options, &data);
  if (status != Status::kOk) return status;
  if (data.rows.size() != 1 || data.rows[0].size() != 1) {
    ADD_FAILURE() << "the value did not consume exactly its own bytes";
    return Status::kMalformedValue;
  }
  *out = data.rows[0][0];
  return Status::kOk;
}

// compress_zlib's output: header 0x80 | length width | raw-deflate bit, the
// original length big-endian, then the deflate stream.
Bytes CompressedPayload(const std::string& input, bool raw_stream) {
  size_t width = 1;
  while (width < 4 && input.size() >= (size_t{1} << (width * 8))) ++width;
  Writer w;
  w.U8(0x80 | width | (raw_stream ? 0x08 : 0)).Be(input.size(), static_cast<int>(width));

  z_stream stream{};
  if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, raw_stream ? -MAX_WBITS : MAX_WBITS,
                   8, Z_DEFAULT_STRATEGY) != Z_OK) {
    return {};
  }
  Bytes out(compressBound(static_cast<uLong>(input.size())));
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_out = out.data();
  stream.avail_out = static_cast<uInt>(out.size());
  const int result = deflate(&stream, Z_FINISH);
  out.resize(stream.total_out);
  if (deflateEnd(&stream) != Z_OK || result != Z_STREAM_END) return {};
  return w.Raw(out).bytes;
}

// --- Mandatory rows: the three header anomalies ---

TEST(CodecRowsCompressed, Method0WithLowBitsIsMalformed) {
  for (uint8_t header = 0x01; header <= 0x0F; ++header) {
    SCOPED_TRACE(int{header});
    Value value;
    EXPECT_EQ(DecodeOne(Column(ColumnType::kVarcharCompressed, 100),
                        Writer().U8(6).U8(header).Str("hello").bytes, &value),
              Status::kMalformedValue);
    EXPECT_EQ(DecodeOne(Column(ColumnType::kBlobCompressed, 2),
                        Writer().Le(6, 2).U8(header).Str("hello").bytes, &value),
              Status::kMalformedValue);
  }
}

TEST(CodecRowsCompressed, TrailingCompressedInputIsMalformed) {
  for (bool raw : {false, true}) {
    SCOPED_TRACE(raw);
    Bytes payload = CompressedPayload(std::string(1000, 'a'), raw);
    ASSERT_FALSE(payload.empty());
    Value value;
    ASSERT_EQ(DecodeOne(Column(ColumnType::kBlobCompressed, 2),
                        Writer().Le(payload.size(), 2).Raw(payload).bytes, &value),
              Status::kOk);
    payload.push_back(0x00);
    EXPECT_EQ(DecodeOne(Column(ColumnType::kBlobCompressed, 2),
                        Writer().Le(payload.size(), 2).Raw(payload).bytes, &value),
              Status::kMalformedValue);
  }
}

TEST(CodecRowsCompressed, ZeroOriginalLengthIsMalformed) {
  // compress_zlib never stores an empty value; an empty value has no header.
  Bytes payload = CompressedPayload("x", true);
  ASSERT_FALSE(payload.empty());
  payload[1] = 0;
  Value value;
  EXPECT_EQ(DecodeOne(Column(ColumnType::kVarcharCompressed, 100),
                      Writer().U8(payload.size()).Raw(payload).bytes, &value),
            Status::kMalformedValue);
}

// --- Well-formed values ---

TEST(CodecRowsCompressed, StoredAndZlibPayloadsDecode) {
  Value value;
  // Method 0: stored verbatim after the header byte.
  ASSERT_EQ(DecodeOne(Column(ColumnType::kVarcharCompressed, 100, true, kTextCollation),
                      Writer().U8(6).U8(0).Str("hello").bytes, &value),
            Status::kOk);
  EXPECT_EQ(value.kind, Value::Kind::kText);
  EXPECT_EQ(value.type, ColumnType::kVarcharCompressed);
  EXPECT_EQ(value.bytes, "hello");

  // An empty value is stored with no header at all.
  ASSERT_EQ(DecodeOne(Column(ColumnType::kBlobCompressed, 1), {0}, &value), Status::kOk);
  EXPECT_EQ(value.kind, Value::Kind::kBytes);
  EXPECT_TRUE(value.bytes.empty());

  const std::string text(1000, 'a');
  const Bytes wrapped = CompressedPayload(text, false);
  ASSERT_EQ(DecodeOne(Column(ColumnType::kVarcharCompressed, 1001, true, kTextCollation),
                      Writer().Le(wrapped.size(), 2).Raw(wrapped).bytes, &value),
            Status::kOk);
  EXPECT_EQ(value.bytes, text);

  const std::string binary(4096, '\x7F');
  const Bytes raw = CompressedPayload(binary, true);
  ASSERT_EQ(DecodeOne(Column(ColumnType::kBlobCompressed, 2),
                      Writer().Le(raw.size(), 2).Raw(raw).bytes, &value),
            Status::kOk);
  EXPECT_EQ(value.kind, Value::Kind::kBytes);
  EXPECT_EQ(value.bytes, binary);
}

TEST(CodecRowsCompressed, OtherHeaderFaults) {
  Value value;
  // Method 9 is not one MariaDB defines.
  EXPECT_EQ(DecodeOne(Column(ColumnType::kVarcharCompressed, 100), {1, 0x90}, &value),
            Status::kUnsupported);
  // zlib with a zero-width or five-byte original length, or no stream after it.
  EXPECT_EQ(DecodeOne(Column(ColumnType::kVarcharCompressed, 100), {2, 0x80, 0x00}, &value),
            Status::kMalformedValue);
  EXPECT_EQ(DecodeOne(Column(ColumnType::kVarcharCompressed, 100), {7, 0x85, 0, 0, 0, 0, 1, 0x00},
                      &value),
            Status::kMalformedValue);
  EXPECT_EQ(DecodeOne(Column(ColumnType::kVarcharCompressed, 100), {2, 0x81, 0x05}, &value),
            Status::kMalformedValue);
  // A corrupt stream.
  EXPECT_EQ(
      DecodeOne(Column(ColumnType::kVarcharCompressed, 100), {4, 0x81, 0x03, 0x00, 0x01}, &value),
      Status::kMalformedValue);
  // An original length above the column's maximum.
  const Bytes payload = CompressedPayload(std::string(300, 'q'), true);
  EXPECT_EQ(DecodeOne(Column(ColumnType::kBlobCompressed, 1),
                      Writer().U8(payload.size()).Raw(payload).bytes, &value),
            Status::kMalformedValue);
}

TEST(CodecRowsCompressed, ExpansionIsClampedBeforeInflating) {
  // The default ceiling: 64 MiB + 1 is refused from the header alone.
  Value value;
  EXPECT_EQ(
      DecodeOne(Column(ColumnType::kBlobCompressed, 4),
                Writer().Le(6, 4).U8(0x84).Be(64U * 1024U * 1024U + 1U, 4).U8(0).bytes, &value),
      Status::kLimitExceeded);

  const std::string expected(4096, 'z');
  const Bytes payload = CompressedPayload(expected, true);
  const Bytes wire = Writer().Le(payload.size(), 2).Raw(payload).bytes;
  DecodeOptions options;
  options.max_decompressed_bytes = expected.size() - 1;
  EXPECT_EQ(DecodeOne(Column(ColumnType::kBlobCompressed, 2), wire, &value, options),
            Status::kLimitExceeded);
  options.max_decompressed_bytes = expected.size();
  ASSERT_EQ(DecodeOne(Column(ColumnType::kBlobCompressed, 2), wire, &value, options), Status::kOk);
  EXPECT_EQ(value.bytes, expected);

  // The event budget clamps both compressed and stored payloads.
  DecodeBudget budget;
  budget.limit = expected.size() - 1;
  options = DecodeOptions();
  options.budget = &budget;
  EXPECT_EQ(DecodeOne(Column(ColumnType::kBlobCompressed, 2), wire, &value, options),
            Status::kLimitExceeded);
  budget = DecodeBudget();
  budget.limit = 4;
  EXPECT_EQ(DecodeOne(Column(ColumnType::kVarcharCompressed, 100),
                      Writer().U8(6).U8(0).Str("hello").bytes, &value, options),
            Status::kLimitExceeded);
  budget = DecodeBudget();
  budget.limit = 5;
  ASSERT_EQ(DecodeOne(Column(ColumnType::kVarcharCompressed, 100),
                      Writer().U8(6).U8(0).Str("hello").bytes, &value, options),
            Status::kOk);
  EXPECT_EQ(budget.used, 5u);
}

TEST(CodecRowsCompressed, BombAcrossColumnsStaysWithinTheBudget) {
  constexpr size_t kColumns = 8;
  constexpr size_t kExpanded = 4U * 1024U * 1024U;
  constexpr size_t kBudget = 6U * 1024U * 1024U;
  const Bytes payload = CompressedPayload(std::string(kExpanded, 'a'), true);
  ASSERT_FALSE(payload.empty());

  TableMap table;
  table.table_id = 3;
  table.columns.assign(kColumns, Column(ColumnType::kBlobCompressed, 4));
  Writer w;
  w.Le(3, 6).Le(0, 2).Le(2, 2).U8(kColumns).U8(0xFF).U8(0x00);
  for (size_t i = 0; i < kColumns; ++i) w.Le(payload.size(), 4).Raw(payload);

  DecodeBudget budget;
  budget.limit = kBudget;
  DecodeOptions options;
  options.budget = &budget;
  RowsEvent data;
  EXPECT_EQ(
      DecodeRowsEvent(WriteRowsHeader(), w.bytes.data(), w.bytes.size(), table, options, &data),
      Status::kLimitExceeded);
  EXPECT_LE(budget.used, budget.limit);
  EXPECT_LT(budget.used, kColumns * kExpanded);
  EXPECT_LT(w.bytes.size(), 64u * 1024u);
}

// --- Text or bytes: a function of the collation alone, compressed or not ---

enum class Framing { kOneByte, kTwoByte, kCompressedOneByte, kCompressedTwoByte };

struct Encoding {
  const char* label;
  ColumnType type;
  uint32_t meta;
  Framing framing;
};

struct CharsetState {
  const char* label;
  bool known;
  uint32_t collation;
  Value::Kind kind;
};

// An embedded NUL and a byte no UTF-8 decoder keeps, so a wrong cell shows in the value.
const std::string kPayload(
    "ab\0\xFF"
    "cd",
    6);

Bytes Frame(Framing framing) {
  Writer w;
  switch (framing) {
    case Framing::kOneByte:
      return w.U8(kPayload.size()).Str(kPayload).bytes;
    case Framing::kTwoByte:
      return w.Le(kPayload.size(), 2).Str(kPayload).bytes;
    case Framing::kCompressedOneByte:
    case Framing::kCompressedTwoByte: {
      const Bytes compressed = CompressedPayload(kPayload, true);
      w.Le(compressed.size(), framing == Framing::kCompressedOneByte ? 1 : 2);
      return w.Raw(compressed).bytes;
    }
  }
  return {};
}

const Encoding kEncodings[] = {
    {"VARCHAR", ColumnType::kVarchar, 100, Framing::kOneByte},
    {"VAR_STRING", ColumnType::kVarString, 0xFE64, Framing::kOneByte},
    {"CHAR", ColumnType::kString, 0xFE0A, Framing::kOneByte},
    {"BLOB", ColumnType::kBlob, 2, Framing::kTwoByte},
    {"VARCHAR COMPRESSED", ColumnType::kVarcharCompressed, 100, Framing::kCompressedOneByte},
    {"BLOB COMPRESSED", ColumnType::kBlobCompressed, 2, Framing::kCompressedTwoByte},
};

const CharsetState kStates[] = {
    {"absent", false, 0, Value::Kind::kBytes},
    {"text collation", true, kTextCollation, Value::Kind::kText},
    {"binary collation", true, kBinaryCollation, Value::Kind::kBytes},
};

TEST(CodecRowsCompressed, CharsetMatrixClassifiesEveryEncodingByCollation) {
  size_t cells = 0;
  for (const Encoding& encoding : kEncodings) {
    for (const CharsetState& state : kStates) {
      SCOPED_TRACE(std::string(encoding.label) + " / " + state.label);
      Value value;
      ASSERT_EQ(DecodeOne(Column(encoding.type, encoding.meta, state.known, state.collation),
                          Frame(encoding.framing), &value),
                Status::kOk);
      EXPECT_EQ(value.kind, state.kind);
      EXPECT_EQ(value.bytes, kPayload);
      ++cells;
    }
  }
  EXPECT_EQ(cells, 18u);
}

TEST(CodecRowsCompressed, CharsetMatrixInOneRow) {
  const Encoding row_encodings[] = {kEncodings[0], kEncodings[3], kEncodings[4], kEncodings[5]};
  for (const CharsetState& state : kStates) {
    SCOPED_TRACE(state.label);
    TableMap table;
    table.table_id = 77;
    Writer w;
    w.Le(77, 6).Le(0, 2).Le(2, 2).U8(4).U8(0x0F).U8(0x00);
    for (const Encoding& encoding : row_encodings) {
      table.columns.push_back(Column(encoding.type, encoding.meta, state.known, state.collation));
      w.Raw(Frame(encoding.framing));
    }
    RowsEvent data;
    ASSERT_EQ(DecodeRowsEvent(WriteRowsHeader(), w.bytes.data(), w.bytes.size(), table,
                              DecodeOptions(), &data),
              Status::kOk);
    ASSERT_EQ(data.rows.size(), 1u);
    for (const Value& value : data.rows[0]) {
      EXPECT_EQ(value.kind, state.kind);
      EXPECT_EQ(value.bytes, kPayload);
    }
  }
}

}  // namespace
}  // namespace mybinlog
