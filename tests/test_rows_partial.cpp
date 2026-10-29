// PARTIAL_UPDATE_ROWS_EVENT (type 39): the after image's value options and
// partial-JSON bitmap, kJsonDiff for flagged columns, and UPDATE_ROWS
// equivalence when the options are 0.

#include <gtest/gtest.h>

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

struct Writer {
  Bytes bytes;
  Writer& U8(uint64_t v) { return Le(v, 1); }
  Writer& Le(uint64_t v, int n) {
    for (int i = 0; i < n; ++i) bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return *this;
  }
  Writer& Raw(const std::string& s) {
    bytes.insert(bytes.end(), s.begin(), s.end());
    return *this;
  }
  Writer& Bitmap(size_t bits, const std::vector<size_t>& set) {
    Bytes map((bits + 7) / 8, 0);
    for (size_t bit : set) map[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
    bytes.insert(bytes.end(), map.begin(), map.end());
    return *this;
  }
  // A JSON value as a column carries it: 4-byte length, then the binary JSON.
  Writer& Json(const std::string& binary) { return Le(binary.size(), 4).Raw(binary); }
};

// A small binary JSON document: the literal `true`.
const std::string kJsonTrue("\x04\x01", 2);
const std::string kJsonFalse("\x04\x02", 2);
// One REPLACE diff of `$` with `null` (operation 0, path length 1 "$",
// value length 2, value type literal null); L10 owns its interpretation.
const std::string kDiff("\x00\x01$\x02\x04\x00", 6);

TableMapColumn Column(ColumnType type, uint32_t meta = 0) {
  TableMapColumn column;
  column.type = type;
  column.meta = meta;
  return column;
}

// id INT, j1 JSON, n INT, j2 JSON.
TableMap JsonTable() {
  TableMap table;
  table.table_id = 10;
  table.columns = {Column(ColumnType::kLong), Column(ColumnType::kJson, 4),
                   Column(ColumnType::kLong), Column(ColumnType::kJson, 4)};
  return table;
}

EventHeader Header(EventType type) {
  EventHeader header;
  header.type_code = static_cast<uint8_t>(type);
  return header;
}

Writer PostHeader(size_t columns, const std::vector<size_t>& before,
                  const std::vector<size_t>& after) {
  Writer w;
  w.Le(10, 6).Le(0, 2).Le(2, 2).U8(columns).Bitmap(columns, before).Bitmap(columns, after);
  return w;
}

Status Decode(EventType type, const Writer& w, const TableMap& table, RowsEvent* out,
              const DecodeOptions& options = DecodeOptions()) {
  return DecodeRowsEvent(Header(type), w.bytes.data(), w.bytes.size(), table, options, out);
}

// Before image of all four columns: id, j1, n, j2.
void FullBefore(Writer* w, int64_t id) {
  w->U8(0x00).Le(id, 4).Json(kJsonTrue).Le(1, 4).Json(kJsonFalse);
}

std::string Framed(const std::string& diff) {
  Writer w;
  w.Le(diff.size(), 4).Raw(diff);
  return std::string(w.bytes.begin(), w.bytes.end());
}

void ExpectValuesEqual(const Value& a, const Value& b) {
  EXPECT_EQ(a.kind, b.kind);
  EXPECT_EQ(a.type, b.type);
  EXPECT_EQ(a.i, b.i);
  EXPECT_EQ(a.bytes, b.bytes);
}

TEST(CodecRowsPartial, OptionsZeroDecodesLikeUpdateRows) {
  Writer update = PostHeader(4, {0, 1, 2, 3}, {0, 1, 2, 3});
  Writer partial = PostHeader(4, {0, 1, 2, 3}, {0, 1, 2, 3});
  FullBefore(&update, 1);
  FullBefore(&partial, 1);
  update.U8(0x00).Le(1, 4).Json(kJsonFalse).Le(2, 4).Json(kJsonTrue);
  partial.U8(0x00).U8(0x00).Le(1, 4).Json(kJsonFalse).Le(2, 4).Json(kJsonTrue);

  RowsEvent expected;
  RowsEvent data;
  ASSERT_EQ(Decode(EventType::kUpdateRowsEvent, update, JsonTable(), &expected), Status::kOk);
  ASSERT_EQ(Decode(EventType::kPartialUpdateRowsEvent, partial, JsonTable(), &data), Status::kOk);
  ASSERT_EQ(data.updates.size(), 1u);
  ASSERT_EQ(expected.updates.size(), 1u);
  for (size_t i = 0; i < 4; ++i) {
    SCOPED_TRACE(i);
    ExpectValuesEqual(data.updates[0].first[i], expected.updates[0].first[i]);
    ExpectValuesEqual(data.updates[0].second[i], expected.updates[0].second[i]);
  }
  EXPECT_EQ(data.updates[0].second[1].kind, Value::Kind::kJson);
  EXPECT_EQ(data.updates[0].second[1].bytes, kJsonFalse);
}

TEST(CodecRowsPartial, FlaggedJsonColumnIsADiffWithItsLengthPrefix) {
  Writer w = PostHeader(4, {0, 1, 2, 3}, {0, 1, 2, 3});
  FullBefore(&w, 1);
  // Options PARTIAL_JSON, partial bits: j1 (bit 0) set, j2 (bit 1) clear.
  w.U8(0x01).Bitmap(2, {0}).U8(0x00).Le(1, 4).Raw(Framed(kDiff)).Le(2, 4).Json(kJsonTrue);
  RowsEvent data;
  ASSERT_EQ(Decode(EventType::kPartialUpdateRowsEvent, w, JsonTable(), &data), Status::kOk);
  ASSERT_EQ(data.updates.size(), 1u);
  const Row& before = data.updates[0].first;
  const Row& after = data.updates[0].second;
  EXPECT_EQ(before[1].kind, Value::Kind::kJson);
  EXPECT_EQ(before[1].bytes, kJsonTrue);
  EXPECT_EQ(after[1].kind, Value::Kind::kJsonDiff);
  EXPECT_EQ(after[1].type, ColumnType::kJson);
  EXPECT_EQ(after[1].bytes, Framed(kDiff));
  EXPECT_EQ(after[2].kind, Value::Kind::kInt);
  EXPECT_EQ(after[2].i, 2);
  EXPECT_EQ(after[3].kind, Value::Kind::kJson);
  EXPECT_EQ(after[3].bytes, kJsonTrue);
}

TEST(CodecRowsPartial, PartialBitsCountEveryJsonColumnCarriedOrNot) {
  // MINIMAL: before carries id, after carries id and j2 only. j1 still owns bit 0.
  Writer w = PostHeader(4, {0}, {0, 3});
  w.U8(0x00).Le(1, 4);
  w.U8(0x01).Bitmap(2, {1}).U8(0x00).Le(1, 4).Raw(Framed(kDiff));
  RowsEvent data;
  ASSERT_EQ(Decode(EventType::kPartialUpdateRowsEvent, w, JsonTable(), &data), Status::kOk);
  const Row& after = data.updates.at(0).second;
  EXPECT_EQ(after[1].kind, Value::Kind::kAbsent);
  EXPECT_EQ(after[2].kind, Value::Kind::kAbsent);
  EXPECT_EQ(after[3].kind, Value::Kind::kJsonDiff);
  EXPECT_EQ(after[3].bytes, Framed(kDiff));
  EXPECT_EQ(data.updates[0].first[3].kind, Value::Kind::kAbsent);
}

TEST(CodecRowsPartial, PartialBitmapSpansBytesPastEightJsonColumns) {
  TableMap table;
  table.columns.push_back(Column(ColumnType::kLong));
  for (int i = 0; i < 9; ++i) table.columns.push_back(Column(ColumnType::kJson, 4));
  Writer w = PostHeader(10, {0}, {0, 9});
  w.U8(0x00).Le(1, 4);
  // The ninth JSON column (ordinal 9) is bit 8: the second bitmap byte.
  w.U8(0x01).Bitmap(9, {8}).U8(0x00).Le(1, 4).Raw(Framed(kDiff));
  RowsEvent data;
  ASSERT_EQ(Decode(EventType::kPartialUpdateRowsEvent, w, table, &data), Status::kOk);
  EXPECT_EQ(data.updates.at(0).second[9].kind, Value::Kind::kJsonDiff);
}

TEST(CodecRowsPartial, EachRowHasItsOwnOptions) {
  Writer w = PostHeader(4, {0}, {0, 1});
  w.U8(0x00).Le(1, 4).U8(0x01).Bitmap(2, {0}).U8(0x00).Le(1, 4).Raw(Framed(kDiff));
  w.U8(0x00).Le(2, 4).U8(0x00).U8(0x00).Le(2, 4).Json(kJsonTrue);
  w.U8(0x00).Le(3, 4).U8(0x01).Bitmap(2, {}).U8(0x00).Le(3, 4).Json(kJsonFalse);
  RowsEvent data;
  ASSERT_EQ(Decode(EventType::kPartialUpdateRowsEvent, w, JsonTable(), &data), Status::kOk);
  ASSERT_EQ(data.updates.size(), 3u);
  EXPECT_EQ(data.updates[0].second[1].kind, Value::Kind::kJsonDiff);
  EXPECT_EQ(data.updates[1].second[1].kind, Value::Kind::kJson);
  EXPECT_EQ(data.updates[1].second[1].bytes, kJsonTrue);
  EXPECT_EQ(data.updates[2].second[1].kind, Value::Kind::kJson);
  EXPECT_EQ(data.updates[2].second[1].bytes, kJsonFalse);
}

TEST(CodecRowsPartial, FlaggedNullStaysNull) {
  Writer w = PostHeader(4, {0}, {0, 1});
  w.U8(0x00).Le(1, 4).U8(0x01).Bitmap(2, {0}).U8(0x02).Le(1, 4);
  RowsEvent data;
  ASSERT_EQ(Decode(EventType::kPartialUpdateRowsEvent, w, JsonTable(), &data), Status::kOk);
  EXPECT_EQ(data.updates.at(0).second[1].kind, Value::Kind::kNull);
}

TEST(CodecRowsPartial, UnselectedDiffIsMeasuredAndSkipped) {
  Writer w = PostHeader(4, {0}, {0, 1, 2});
  w.U8(0x00).Le(1, 4).U8(0x01).Bitmap(2, {0}).U8(0x00).Le(1, 4).Raw(Framed(kDiff)).Le(77, 4);
  DecodeOptions options;
  options.columns = {true, false, true, true};
  RowsEvent data;
  ASSERT_EQ(Decode(EventType::kPartialUpdateRowsEvent, w, JsonTable(), &data, options),
            Status::kOk);
  const Row& after = data.updates.at(0).second;
  EXPECT_EQ(after[1].kind, Value::Kind::kNull);
  EXPECT_EQ(after[2].i, 77);
  EXPECT_EQ(after[3].kind, Value::Kind::kAbsent);
}

TEST(CodecRowsPartial, DiffCountsAgainstTheValueCeilingWithItsPrefix) {
  Writer w = PostHeader(4, {0}, {0, 1});
  w.U8(0x00).Le(1, 4).U8(0x01).Bitmap(2, {0}).U8(0x00).Le(1, 4).Raw(Framed(kDiff));
  DecodeOptions options;
  options.max_value_bytes = kDiff.size() + 3;
  RowsEvent data;
  EXPECT_EQ(Decode(EventType::kPartialUpdateRowsEvent, w, JsonTable(), &data, options),
            Status::kLimitExceeded);
  options.max_value_bytes = kDiff.size() + 4;
  EXPECT_EQ(Decode(EventType::kPartialUpdateRowsEvent, w, JsonTable(), &data, options),
            Status::kOk);
}

TEST(CodecRowsPartial, MalformedAfterImagePrefix) {
  RowsEvent data;
  {
    SCOPED_TRACE("options above PARTIAL_JSON");
    Writer w = PostHeader(4, {0}, {0});
    w.U8(0x00).Le(1, 4).U8(0x02).U8(0x00).Le(1, 4);
    EXPECT_EQ(Decode(EventType::kPartialUpdateRowsEvent, w, JsonTable(), &data),
              Status::kInvalidMetadata);
  }
  {
    SCOPED_TRACE("options as the NULL marker");
    Writer w = PostHeader(4, {0}, {0});
    w.U8(0x00).Le(1, 4).U8(0xFB).U8(0x00).Le(1, 4);
    EXPECT_EQ(Decode(EventType::kPartialUpdateRowsEvent, w, JsonTable(), &data),
              Status::kInvalidMetadata);
  }
  {
    SCOPED_TRACE("partial bits missing");
    Writer w = PostHeader(4, {0}, {0});
    w.U8(0x00).Le(1, 4).U8(0x01);
    EXPECT_EQ(Decode(EventType::kPartialUpdateRowsEvent, w, JsonTable(), &data),
              Status::kTruncated);
  }
  {
    SCOPED_TRACE("diff longer than the body");
    Writer w = PostHeader(4, {0}, {0, 1});
    w.U8(0x00).Le(1, 4).U8(0x01).Bitmap(2, {0}).U8(0x00).Le(1, 4).Le(100, 4).Raw(kDiff);
    EXPECT_EQ(Decode(EventType::kPartialUpdateRowsEvent, w, JsonTable(), &data),
              Status::kTruncated);
  }
}

TEST(CodecRowsPartial, BeforeImageHasNoOptions) {
  // A 0x01 at the start of the before image is its NULL bitmap, not options.
  Writer w = PostHeader(4, {0, 2}, {0});
  w.U8(0x01).Le(5, 4).U8(0x00).U8(0x00).Le(1, 4);
  RowsEvent data;
  ASSERT_EQ(Decode(EventType::kPartialUpdateRowsEvent, w, JsonTable(), &data), Status::kOk);
  EXPECT_EQ(data.updates.at(0).first[0].kind, Value::Kind::kNull);
  EXPECT_EQ(data.updates[0].first[2].i, 5);
}

TEST(CodecRowsPartial, ExtraRowInfoCarriesBothPartitions) {
  Writer w;
  w.Le(10, 6).Le(0, 2).Le(7, 2).U8(1).Le(4, 2).Le(6, 2);
  w.U8(4).Bitmap(4, {0}).Bitmap(4, {0}).U8(0x00).Le(1, 4).U8(0x00).U8(0x00).Le(1, 4);
  RowsEvent data;
  ASSERT_EQ(Decode(EventType::kPartialUpdateRowsEvent, w, JsonTable(), &data), Status::kOk);
  EXPECT_TRUE(data.has_partition_id);
  EXPECT_EQ(data.partition_id, 4);
  EXPECT_EQ(data.source_partition_id, 6);
}

}  // namespace
}  // namespace mybinlog
