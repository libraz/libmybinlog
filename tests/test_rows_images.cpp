// MINIMAL and NOBLOB row images (columns-present bitmaps with gaps, a NULL
// bitmap over the carried columns only, kAbsent) and the v2 extra row info.

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
  Writer& Str(const std::string& s) {
    bytes.insert(bytes.end(), s.begin(), s.end());
    return *this;
  }
  Writer& Bitmap(size_t bits, const std::vector<size_t>& set) {
    Bytes map((bits + 7) / 8, 0);
    for (size_t bit : set) map[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
    bytes.insert(bytes.end(), map.begin(), map.end());
    return *this;
  }
};

TableMapColumn Column(ColumnType type, uint32_t meta = 0) {
  TableMapColumn column;
  column.type = type;
  column.meta = meta;
  return column;
}

TableMap Table(std::vector<TableMapColumn> columns) {
  TableMap table;
  table.table_id = 10;
  table.columns = std::move(columns);
  return table;
}

TableMap IntTable(size_t columns) {
  return Table(std::vector<TableMapColumn>(columns, Column(ColumnType::kLong)));
}

EventHeader Header(EventType type) {
  EventHeader header;
  header.type_code = static_cast<uint8_t>(type);
  return header;
}

// table_id, flags and a v2 extra-data block holding @p extra.
Writer PostHeader(const Bytes& extra = {}) {
  Writer w;
  w.Le(10, 6).Le(0, 2).Le(extra.size() + 2, 2);
  w.bytes.insert(w.bytes.end(), extra.begin(), extra.end());
  return w;
}

Status Decode(EventType type, const Writer& w, const TableMap& table, RowsEvent* out,
              const DecodeOptions& options = DecodeOptions()) {
  return DecodeRowsEvent(Header(type), w.bytes.data(), w.bytes.size(), table, options, out);
}

void ExpectAbsent(const Value& value, ColumnType type) {
  EXPECT_EQ(value.kind, Value::Kind::kAbsent);
  EXPECT_EQ(value.type, type);
}

void ExpectInt(const Value& value, int64_t expected) {
  EXPECT_EQ(value.kind, Value::Kind::kInt);
  EXPECT_EQ(value.i, expected);
}

// --- Columns-present bitmaps with gaps ---

TEST(CodecRowsImages, WriteImageWithGapsReadsOnlyTheCarriedColumns) {
  // Columns 0, 2 and 4 of 5 are carried; column 2 is NULL (bit 1 of the NULL bitmap).
  Writer w = PostHeader();
  w.U8(5).Bitmap(5, {0, 2, 4}).Bitmap(3, {1}).Le(10, 4).Le(30, 4);
  RowsEvent data;
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(5), &data), Status::kOk);
  ASSERT_EQ(data.rows.size(), 1u);
  const Row& row = data.rows[0];
  ASSERT_EQ(row.size(), 5u);
  ExpectInt(row[0], 10);
  ExpectAbsent(row[1], ColumnType::kLong);
  EXPECT_EQ(row[2].kind, Value::Kind::kNull);
  ExpectAbsent(row[3], ColumnType::kLong);
  ExpectInt(row[4], 30);
}

TEST(CodecRowsImages, NullBitmapWidthFollowsTheCarriedColumnCount) {
  // 12 columns but 8 carried: a one-byte NULL bitmap, where FULL would take two.
  const std::vector<size_t> carried = {0, 2, 4, 6, 8, 9, 10, 11};
  Writer w = PostHeader();
  w.U8(12).Bitmap(12, carried).Bitmap(8, {7});
  for (size_t i = 0; i + 1 < carried.size(); ++i) w.Le(carried[i] * 10, 4);
  RowsEvent data;
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(12), &data), Status::kOk);
  ASSERT_EQ(data.rows.size(), 1u);
  const Row& row = data.rows[0];
  for (size_t i = 0; i + 1 < carried.size(); ++i) ExpectInt(row[carried[i]], carried[i] * 10);
  EXPECT_EQ(row[11].kind, Value::Kind::kNull);
  for (size_t gap : {1u, 3u, 5u, 7u}) ExpectAbsent(row[gap], ColumnType::kLong);

  // Nine carried columns spill the NULL bitmap into a second byte.
  Writer nine = PostHeader();
  nine.U8(12).Bitmap(12, {0, 1, 2, 3, 4, 5, 6, 7, 11}).Bitmap(9, {8});
  for (int i = 0; i < 8; ++i) nine.Le(i, 4);
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, nine, IntTable(12), &data), Status::kOk);
  ASSERT_EQ(data.rows.size(), 1u);
  ExpectInt(data.rows[0][7], 7);
  EXPECT_EQ(data.rows[0][11].kind, Value::Kind::kNull);
  ExpectAbsent(data.rows[0][8], ColumnType::kLong);
}

TEST(CodecRowsImages, MinimalUpdateCarriesTheKeyBeforeAndTheChangeAfter) {
  for (EventType type : {EventType::kUpdateRowsEvent, EventType::kUpdateRowsEventV1}) {
    SCOPED_TRACE(static_cast<int>(type));
    Writer w;
    if (type == EventType::kUpdateRowsEvent) {
      w = PostHeader();
    } else {
      w.Le(10, 6).Le(0, 2);
    }
    // Before: column 0 (the key). After: column 2. Two rows.
    w.U8(3).Bitmap(3, {0}).Bitmap(3, {2});
    w.U8(0x00).Le(1, 4).U8(0x00).Le(100, 4);
    w.U8(0x00).Le(2, 4).U8(0x01);
    RowsEvent data;
    ASSERT_EQ(Decode(type, w, IntTable(3), &data), Status::kOk);
    ASSERT_EQ(data.updates.size(), 2u);
    ExpectInt(data.updates[0].first[0], 1);
    ExpectAbsent(data.updates[0].first[1], ColumnType::kLong);
    ExpectAbsent(data.updates[0].first[2], ColumnType::kLong);
    ExpectAbsent(data.updates[0].second[0], ColumnType::kLong);
    ExpectInt(data.updates[0].second[2], 100);
    ExpectInt(data.updates[1].first[0], 2);
    EXPECT_EQ(data.updates[1].second[2].kind, Value::Kind::kNull);
  }
}

TEST(CodecRowsImages, MinimalDeleteCarriesOnlyTheKey) {
  Writer w = PostHeader();
  w.U8(4).Bitmap(4, {1}).U8(0x00).Le(7, 4).U8(0x00).Le(8, 4);
  RowsEvent data;
  ASSERT_EQ(Decode(EventType::kDeleteRowsEvent, w, IntTable(4), &data), Status::kOk);
  ASSERT_EQ(data.rows.size(), 2u);
  ExpectInt(data.rows[1][1], 8);
  for (size_t i : {0u, 2u, 3u}) ExpectAbsent(data.rows[1][i], ColumnType::kLong);
}

TEST(CodecRowsImages, NoblobLeavesTheBlobOut) {
  TableMap table = Table({Column(ColumnType::kLong), Column(ColumnType::kBlob, 2),
                          Column(ColumnType::kVarchar, 20), Column(ColumnType::kJson, 4)});
  table.columns[2].has_charset = true;
  table.columns[2].charset = 45;
  Writer w = PostHeader();
  w.U8(4).Bitmap(4, {0, 2}).U8(0x00).Le(5, 4).U8(3).Str("abc");
  RowsEvent data;
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, w, table, &data), Status::kOk);
  ASSERT_EQ(data.rows.size(), 1u);
  ExpectInt(data.rows[0][0], 5);
  ExpectAbsent(data.rows[0][1], ColumnType::kBlob);
  EXPECT_EQ(data.rows[0][2].kind, Value::Kind::kText);
  EXPECT_EQ(data.rows[0][2].bytes, "abc");
  ExpectAbsent(data.rows[0][3], ColumnType::kJson);
}

// --- Images that carry nothing ---

TEST(CodecRowsImages, AnAfterImageMayCarryNoColumn) {
  Writer w = PostHeader();
  w.U8(2).Bitmap(2, {0, 1}).Bitmap(2, {}).U8(0x00).Le(1, 4).Le(2, 4);
  RowsEvent data;
  ASSERT_EQ(Decode(EventType::kUpdateRowsEvent, w, IntTable(2), &data), Status::kOk);
  ASSERT_EQ(data.updates.size(), 1u);
  ExpectInt(data.updates[0].first[1], 2);
  ExpectAbsent(data.updates[0].second[0], ColumnType::kLong);
  ExpectAbsent(data.updates[0].second[1], ColumnType::kLong);
}

TEST(CodecRowsImages, BitmapsThatSelectNothingCannotHoldRows) {
  RowsEvent data;
  {
    SCOPED_TRACE("write, no rows");
    Writer w = PostHeader();
    w.U8(2).Bitmap(2, {});
    ASSERT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(2), &data), Status::kOk);
    EXPECT_TRUE(data.rows.empty());
  }
  {
    SCOPED_TRACE("write, trailing bytes");
    Writer w = PostHeader();
    w.U8(2).Bitmap(2, {}).U8(0xAA);
    EXPECT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(2), &data), Status::kInvalidMetadata);
  }
  {
    SCOPED_TRACE("update, both images empty, trailing bytes");
    Writer w = PostHeader();
    w.U8(1).Bitmap(1, {}).Bitmap(1, {}).U8(0xAA);
    EXPECT_EQ(Decode(EventType::kUpdateRowsEvent, w, IntTable(1), &data), Status::kInvalidMetadata);
  }
}

TEST(CodecRowsImages, MissingValuesOfCarriedColumnsAreTruncated) {
  Writer w = PostHeader();
  w.U8(3).Bitmap(3, {0, 2}).U8(0x00).Le(1, 4).Le(2, 2);
  RowsEvent data;
  EXPECT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(3), &data), Status::kTruncated);
}

// --- Column selection ---

TEST(CodecRowsImages, AnUnselectedColumnIsNullWhetherCarriedOrNot) {
  Writer w = PostHeader();
  w.U8(3).Bitmap(3, {0, 2}).U8(0x00).Le(1, 4).Le(3, 4);
  DecodeOptions options;
  options.columns = {false, false, true};
  RowsEvent data;
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(3), &data, options), Status::kOk);
  EXPECT_EQ(data.rows[0][0].kind, Value::Kind::kNull);
  EXPECT_EQ(data.rows[0][1].kind, Value::Kind::kNull);
  ExpectInt(data.rows[0][2], 3);

  options.columns = {false, true, false};
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(3), &data, options), Status::kOk);
  ExpectAbsent(data.rows[0][1], ColumnType::kLong);
}

TEST(CodecRowsImages, MySqlJsonIsBinaryJsonAndMariaDbJsonIsText) {
  TableMapColumn mariadb_json = Column(ColumnType::kBlob, 4);
  mariadb_json.has_charset = true;
  mariadb_json.charset = 45;
  TableMap table = Table({Column(ColumnType::kJson, 4), mariadb_json});
  Writer w = PostHeader();
  w.U8(2).Bitmap(2, {0, 1}).U8(0x00).Le(2, 4).Le(0x0004, 2).Le(2, 4).Str("{}");
  RowsEvent data;
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, w, table, &data), Status::kOk);
  EXPECT_EQ(data.rows[0][0].kind, Value::Kind::kJson);
  EXPECT_EQ(data.rows[0][0].bytes, std::string("\x04\x00", 2));
  EXPECT_EQ(data.rows[0][1].kind, Value::Kind::kText);
  EXPECT_EQ(data.rows[0][1].bytes, "{}");
}

// --- v2 extra row info ---

struct ExtraCase {
  const char* name;
  EventType type;
  Bytes extra;
  Status status;
  bool has_partition;
  uint16_t partition;
  uint16_t source;
};

TEST(CodecRowsImages, ExtraRowInfo) {
  const ExtraCase cases[] = {
      {"none", EventType::kWriteRowsEvent, {}, Status::kOk, false, 0, 0},
      {"write partition",
       EventType::kWriteRowsEvent,
       {1, 0x34, 0x12},
       Status::kOk,
       true,
       0x1234,
       0},
      {"delete partition", EventType::kDeleteRowsEvent, {1, 7, 0}, Status::kOk, true, 7, 0},
      {"update partition and source",
       EventType::kUpdateRowsEvent,
       {1, 3, 0, 9, 0},
       Status::kOk,
       true,
       3,
       9},
      {"ndb entry skipped",
       EventType::kWriteRowsEvent,
       {0, 4, 0, 0xAA, 0xBB},
       Status::kOk,
       false,
       0,
       0},
      {"ndb then partition",
       EventType::kWriteRowsEvent,
       {0, 3, 0, 0xAA, 1, 5, 0},
       Status::kOk,
       true,
       5,
       0},
      {"two ndb entries", EventType::kWriteRowsEvent, {0, 2, 0, 0, 2, 0}, Status::kOk, false, 0, 0},
      {"unknown type ends the walk",
       EventType::kWriteRowsEvent,
       {7, 0xFF, 0xFF, 0xFF},
       Status::kOk,
       false,
       0,
       0},
      {"partition before an unknown type",
       EventType::kWriteRowsEvent,
       {1, 2, 0, 9, 1},
       Status::kOk,
       true,
       2,
       0},
      {"ndb length overruns",
       EventType::kWriteRowsEvent,
       {0, 5, 0, 0},
       Status::kInvalidMetadata,
       false,
       0,
       0},
      {"ndb length below its header",
       EventType::kWriteRowsEvent,
       {0, 1},
       Status::kInvalidMetadata,
       false,
       0,
       0},
      {"ndb without a length",
       EventType::kWriteRowsEvent,
       {0},
       Status::kInvalidMetadata,
       false,
       0,
       0},
      {"write partition short",
       EventType::kWriteRowsEvent,
       {1, 0},
       Status::kInvalidMetadata,
       false,
       0,
       0},
      {"update partition without source",
       EventType::kUpdateRowsEvent,
       {1, 3, 0},
       Status::kInvalidMetadata,
       false,
       0,
       0},
  };
  for (const ExtraCase& c : cases) {
    SCOPED_TRACE(c.name);
    const bool update = c.type == EventType::kUpdateRowsEvent;
    Writer w = PostHeader(c.extra);
    w.U8(1).Bitmap(1, {0});
    if (update) w.Bitmap(1, {0}).U8(0x00).Le(41, 4);
    w.U8(0x00).Le(42, 4);
    RowsEvent data;
    ASSERT_EQ(Decode(c.type, w, IntTable(1), &data), c.status);
    if (c.status != Status::kOk) continue;
    EXPECT_EQ(data.has_partition_id, c.has_partition);
    EXPECT_EQ(data.partition_id, c.partition);
    EXPECT_EQ(data.source_partition_id, c.source);
    const Row& row = update ? data.updates.at(0).second : data.rows.at(0);
    ExpectInt(row[0], 42);
  }
}

TEST(CodecRowsImages, ExtraRowInfoPastTheBodyIsTruncated) {
  Writer w;
  w.Le(10, 6).Le(0, 2).Le(10, 2).U8(1).U8(0);
  RowsEvent data;
  EXPECT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(1), &data), Status::kTruncated);
}

}  // namespace
}  // namespace mybinlog
