// Rows-event structure: v1/v2 post-headers, row image bitmaps, UPDATE pairs,
// column selection and the decode budget.

#include <gtest/gtest.h>

#include <algorithm>
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

// table_id, flags and (v2) an extra-data length of 2.
Writer PostHeader(bool v2 = true) {
  Writer w;
  w.Le(10, 6).Le(0, 2);
  if (v2) w.Le(2, 2);
  return w;
}

void WriteBitmap(Writer* w, size_t bits, const std::vector<size_t>& set) {
  Bytes bytes((bits + 7) / 8, 0);
  for (size_t bit : set) bytes[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
  for (uint8_t b : bytes) w->U8(b);
}

std::vector<size_t> All(size_t n) {
  std::vector<size_t> all(n);
  for (size_t i = 0; i < n; ++i) all[i] = i;
  return all;
}

Status Decode(EventType type, const Writer& w, const TableMap& table, RowsEventData* out,
              const DecodeOptions& options = DecodeOptions()) {
  const Bytes body(w.bytes.begin(), w.bytes.end());
  return DecodeRowsEvent(Header(type), body.data(), body.size(), table, options, out);
}

// --- Shapes ---

TEST(CodecRowsEvent, WriteRowsIntAndVarchar) {
  TableMap table = Table({Column(ColumnType::kLong), Column(ColumnType::kVarchar, 100)});
  table.columns[1].has_charset = true;
  table.columns[1].charset = 45;
  Writer w = PostHeader();
  w.U8(2).U8(0x03).U8(0x00).Le(42, 4).U8(5).Str("Alice");
  RowsEventData data;
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, w, table, &data), Status::kOk);
  ASSERT_EQ(data.rows.size(), 1u);
  EXPECT_TRUE(data.updates.empty());
  ASSERT_EQ(data.rows[0].size(), 2u);
  EXPECT_EQ(data.rows[0][0].kind, Value::Kind::kInt);
  EXPECT_EQ(data.rows[0][0].i, 42);
  EXPECT_EQ(data.rows[0][1].kind, Value::Kind::kText);
  EXPECT_EQ(data.rows[0][1].bytes, "Alice");
}

TEST(CodecRowsEvent, SeveralRowsInOneEvent) {
  Writer w = PostHeader();
  w.U8(1).U8(0x01).U8(0x00).Le(100, 4).U8(0x00).Le(200, 4);
  RowsEventData data;
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(1), &data), Status::kOk);
  ASSERT_EQ(data.rows.size(), 2u);
  EXPECT_EQ(data.rows[0][0].i, 100);
  EXPECT_EQ(data.rows[1][0].i, 200);
}

TEST(CodecRowsEvent, UpdateRowsArePairs) {
  for (EventType type : {EventType::kUpdateRowsEvent, EventType::kUpdateRowsEventV1}) {
    SCOPED_TRACE(static_cast<int>(type));
    Writer w = PostHeader(type == EventType::kUpdateRowsEvent);
    w.U8(1).U8(0x01).U8(0x01).U8(0x00).Le(100, 4).U8(0x00).Le(200, 4);
    RowsEventData data;
    ASSERT_EQ(Decode(type, w, IntTable(1), &data), Status::kOk);
    EXPECT_TRUE(data.rows.empty());
    ASSERT_EQ(data.updates.size(), 1u);
    EXPECT_EQ(data.updates[0].first[0].i, 100);
    EXPECT_EQ(data.updates[0].second[0].i, 200);
  }
}

TEST(CodecRowsEvent, DeleteAndV1Shapes) {
  const EventType types[] = {EventType::kDeleteRowsEvent, EventType::kDeleteRowsEventV1,
                             EventType::kWriteRowsEventV1};
  for (EventType type : types) {
    SCOPED_TRACE(static_cast<int>(type));
    const bool v2 = type == EventType::kDeleteRowsEvent;
    Writer w = PostHeader(v2);
    w.U8(1).U8(0x01).U8(0x00).Le(999, 4);
    RowsEventData data;
    ASSERT_EQ(Decode(type, w, IntTable(1), &data), Status::kOk);
    ASSERT_EQ(data.rows.size(), 1u);
    EXPECT_EQ(data.rows[0][0].i, 999);
  }
}

TEST(CodecRowsEvent, V2ExtraDataIsSkipped) {
  Writer w;
  w.Le(10, 6).Le(0, 2).Le(6, 2).U8(0x00).U8(2).U8(0xAA).U8(0xBB);  // a 4-byte extra-info TLV
  w.U8(1).U8(0x01).U8(0x00).Le(7, 4);
  RowsEventData data;
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(1), &data), Status::kOk);
  ASSERT_EQ(data.rows.size(), 1u);
  EXPECT_EQ(data.rows[0][0].i, 7);

  Writer short_extra;
  short_extra.Le(10, 6).Le(0, 2).Le(1, 2).U8(1).U8(0x01).U8(0x00).Le(7, 4);
  EXPECT_EQ(Decode(EventType::kWriteRowsEvent, short_extra, IntTable(1), &data),
            Status::kInvalidMetadata);
}

TEST(CodecRowsEvent, NonRowsEventTypesAreUnsupported) {
  Writer w = PostHeader();
  w.U8(1).U8(0x01).U8(0x00).Le(7, 4);
  RowsEventData data;
  const EventType types[] = {EventType::kPartialUpdateRowsEvent,
                             EventType::kMariaDBWriteRowsCompressedEvent,
                             EventType::kMariaDBUpdateRowsCompressedEventV1,
                             EventType::kTableMapEvent, EventType::kQueryEvent};
  for (EventType type : types) {
    SCOPED_TRACE(static_cast<int>(type));
    EXPECT_EQ(Decode(type, w, IntTable(1), &data), Status::kUnsupported);
  }
}

// --- NULL bitmaps ---

int64_t Cell(size_t image, size_t column) { return static_cast<int64_t>(image * 100 + column + 1); }

void WriteImage(Writer* w, size_t columns, size_t image, const std::vector<size_t>& nulls) {
  WriteBitmap(w, columns, nulls);
  for (size_t i = 0; i < columns; ++i) {
    if (std::find(nulls.begin(), nulls.end(), i) == nulls.end()) {
      w->Le(static_cast<uint64_t>(Cell(image, i)), 4);
    }
  }
}

void ExpectImage(const Row& row, size_t columns, size_t image, const std::vector<size_t>& nulls) {
  ASSERT_EQ(row.size(), columns);
  for (size_t i = 0; i < columns; ++i) {
    SCOPED_TRACE(i);
    const bool is_null = std::find(nulls.begin(), nulls.end(), i) != nulls.end();
    EXPECT_EQ(row[i].kind, is_null ? Value::Kind::kNull : Value::Kind::kInt);
    EXPECT_EQ(row[i].type, ColumnType::kLong);
    if (!is_null) EXPECT_EQ(row[i].i, Cell(image, i));
  }
}

TEST(CodecRowsEvent, NullBitmapsAcrossByteBoundariesAndRows) {
  struct Case {
    const char* label;
    size_t columns;
    std::vector<std::vector<size_t>> rows;
  };
  const Case cases[] = {
      {"8 columns, last NULL", 8, {{7}}},
      {"9 columns, NULL only in the second byte", 9, {{8}}},
      {"9 columns, NULL on both sides of the boundary", 9, {{7, 8}}},
      {"16 columns, first and last of each byte", 16, {{0, 7, 8, 15}}},
      {"16 columns, every column NULL", 16, {All(16)}},
      {"two rows, alternating bitmaps", 9, {{0, 2, 4, 6, 8}, {1, 3, 5, 7}}},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.label);
    Writer w = PostHeader();
    w.U8(c.columns);
    WriteBitmap(&w, c.columns, All(c.columns));
    for (size_t image = 0; image < c.rows.size(); ++image) {
      WriteImage(&w, c.columns, image, c.rows[image]);
    }
    RowsEventData data;
    ASSERT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(c.columns), &data), Status::kOk);
    ASSERT_EQ(data.rows.size(), c.rows.size());
    for (size_t image = 0; image < c.rows.size(); ++image) {
      SCOPED_TRACE(image);
      ExpectImage(data.rows[image], c.columns, image, c.rows[image]);
    }
  }
}

TEST(CodecRowsEvent, BeforeAndAfterImagesKeepTheirOwnNullBitmaps) {
  constexpr size_t kColumns = 9;
  Writer w = PostHeader();
  w.U8(kColumns);
  WriteBitmap(&w, kColumns, All(kColumns));
  WriteBitmap(&w, kColumns, All(kColumns));
  WriteImage(&w, kColumns, 0, {1});
  WriteImage(&w, kColumns, 1, {8});
  RowsEventData data;
  ASSERT_EQ(Decode(EventType::kUpdateRowsEvent, w, IntTable(kColumns), &data), Status::kOk);
  ASSERT_EQ(data.updates.size(), 1u);
  ExpectImage(data.updates[0].first, kColumns, 0, {1});
  ExpectImage(data.updates[0].second, kColumns, 1, {8});
}

// --- FULL row image only ---

TEST(CodecRowsEvent, PartialRowImagesAreUnsupported) {
  RowsEventData data;
  {
    SCOPED_TRACE("write image missing column 2");
    Writer w = PostHeader();
    w.U8(3).U8(0x03).U8(0x00).Le(42, 4).Le(1, 4);
    EXPECT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(3), &data), Status::kUnsupported);
  }
  {
    SCOPED_TRACE("after image carries only the first column");
    Writer w = PostHeader();
    w.U8(2).U8(0x03).U8(0x01).U8(0x00).Le(10, 4).Le(20, 4).U8(0x00).Le(30, 4);
    EXPECT_EQ(Decode(EventType::kUpdateRowsEvent, w, IntTable(2), &data), Status::kUnsupported);
  }
  {
    SCOPED_TRACE("no columns present, trailing data");
    Writer w = PostHeader();
    w.U8(1).U8(0x00).U8(0xAA);
    EXPECT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(1), &data), Status::kUnsupported);
  }
  {
    SCOPED_TRACE("empty after-image bitmap, trailing data");
    Writer w = PostHeader();
    w.U8(1).U8(0x01).U8(0x00).U8(0x00).Le(42, 4).U8(0xAA);
    EXPECT_EQ(Decode(EventType::kUpdateRowsEvent, w, IntTable(1), &data), Status::kUnsupported);
  }
}

// --- Column count ---

TEST(CodecRowsEvent, ColumnCountMustMatchTheTableAndTheServerLimit) {
  RowsEventData data;
  {
    SCOPED_TRACE("more columns than the TABLE_MAP");
    Writer w = PostHeader();
    w.U8(2).U8(0x03).U8(0x00).Le(42, 4).Le(100, 4);
    EXPECT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(1), &data), Status::kInvalidMetadata);
  }
  {
    SCOPED_TRACE("zero columns");
    Writer w = PostHeader();
    w.U8(0).U8(0x00);
    EXPECT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(1), &data), Status::kInvalidMetadata);
  }
  {
    SCOPED_TRACE("4097 columns, agreeing with the TABLE_MAP");
    constexpr size_t kTooMany = kMaxTableColumns + 1;
    Writer w = PostHeader();
    w.U8(0xFC).Le(kTooMany, 2);
    for (size_t i = 0; i < (kTooMany + 7) / 8; ++i) w.U8(0xFF);
    TableMap table = Table(std::vector<TableMapColumn>(kTooMany, Column(ColumnType::kTiny)));
    EXPECT_EQ(Decode(EventType::kWriteRowsEvent, w, table, &data), Status::kInvalidMetadata);
  }
  {
    SCOPED_TRACE("NULL and undefined packed-int prefixes");
    for (uint8_t prefix : {0xFB, 0xFF}) {
      Writer w = PostHeader();
      w.U8(prefix).Le(0, 8);
      EXPECT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(1), &data),
                Status::kInvalidMetadata);
    }
  }
  // UINT64_MAX in the nine-byte form would make the bitmap size wrap. Run
  // under ASan: the body ends right after the count.
  for (EventType type : {EventType::kWriteRowsEventV1, EventType::kDeleteRowsEventV1,
                         EventType::kUpdateRowsEventV1}) {
    SCOPED_TRACE(static_cast<int>(type));
    Writer w = PostHeader(false);
    w.U8(0xFE).Le(UINT64_MAX, 8);
    ASSERT_EQ(w.bytes.size(), 17u);
    EXPECT_EQ(Decode(type, w, IntTable(2), &data), Status::kInvalidMetadata);
  }
}

// --- Truncation ---

TEST(CodecRowsEvent, TruncatedBodies) {
  RowsEventData data;
  Writer tiny;
  tiny.Le(0, 4);
  EXPECT_EQ(Decode(EventType::kWriteRowsEvent, tiny, IntTable(1), &data), Status::kTruncated);

  Writer no_count = PostHeader();
  EXPECT_EQ(Decode(EventType::kWriteRowsEvent, no_count, IntTable(1), &data), Status::kTruncated);

  Writer mid_value = PostHeader();
  mid_value.U8(1).U8(0x01).U8(0x00).Le(1, 4).U8(0x00).Le(2, 2);  // second row cut short
  EXPECT_EQ(Decode(EventType::kWriteRowsEvent, mid_value, IntTable(1), &data), Status::kTruncated);

  Writer after_missing = PostHeader();
  after_missing.U8(1).U8(0x01).U8(0x01).U8(0x00).Le(1, 4);  // UPDATE without its after image
  EXPECT_EQ(Decode(EventType::kUpdateRowsEvent, after_missing, IntTable(1), &data),
            Status::kTruncated);

  EXPECT_EQ(DecodeRowsEvent(Header(EventType::kWriteRowsEvent), nullptr, 10, IntTable(1),
                            DecodeOptions(), &data),
            Status::kTruncated);
}

TEST(CodecRowsEvent, FailureLeavesTheOutputUntouched) {
  RowsEventData data;
  data.rows.resize(3);
  Writer w = PostHeader();
  w.U8(1).U8(0x01).U8(0x00).Le(1, 4).U8(0x00).Le(2, 2);
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(1), &data), Status::kTruncated);
  EXPECT_EQ(data.rows.size(), 3u);
}

TEST(CodecRowsEvent, AFailingColumnFailsTheEvent) {
  // An undecodable column type has no width, so the INT after it cannot be found.
  TableMap table = Table({Column(ColumnType::kLong), Column(static_cast<ColumnType>(0x06)),
                          Column(ColumnType::kLong)});
  Writer w = PostHeader();
  w.U8(3).U8(0x07).U8(0x00).Le(42, 4).Le(0x0A0B0C0D, 4).Le(99, 4);
  RowsEventData data;
  EXPECT_EQ(Decode(EventType::kWriteRowsEvent, w, table, &data), Status::kUnsupported);

  // A NULL in an undecodable column has no bytes to measure and decodes.
  Writer nulled = PostHeader();
  nulled.U8(3).U8(0x07).U8(0x02).Le(42, 4).Le(99, 4);
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, nulled, table, &data), Status::kOk);
  EXPECT_EQ(data.rows[0][1].kind, Value::Kind::kNull);
  EXPECT_EQ(data.rows[0][2].i, 99);
}

// --- Column selection ---

TEST(CodecRowsEvent, UnselectedColumnsAreMeasuredNotDecoded) {
  TableMap table = Table({Column(ColumnType::kLong), Column(ColumnType::kBlob, 2),
                          Column(ColumnType::kDate), Column(ColumnType::kLong)});
  Writer w = PostHeader();
  w.U8(4).U8(0x0F).U8(0x00).Le(1, 4).Le(3, 2).Str("abc");
  w.Le((2024 << 9) | (13 << 5) | 1, 3);  // month 13: not checked when skipped
  w.Le(4, 4);

  DecodeOptions options;
  options.columns = {true, false, false, true};
  RowsEventData data;
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, w, table, &data, options), Status::kOk);
  ASSERT_EQ(data.rows.size(), 1u);
  const Row& row = data.rows[0];
  EXPECT_EQ(row[0].i, 1);
  EXPECT_EQ(row[1].kind, Value::Kind::kNull);
  EXPECT_EQ(row[1].type, ColumnType::kBlob);
  EXPECT_TRUE(row[1].bytes.empty());
  EXPECT_EQ(row[2].kind, Value::Kind::kNull);
  EXPECT_EQ(row[3].i, 4);

  // An ordinal past the end of the selection is not selected.
  options.columns = {true};
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, w, table, &data, options), Status::kOk);
  EXPECT_EQ(data.rows[0][3].kind, Value::Kind::kNull);

  // Selecting the DATE decodes and checks it.
  options.columns = {true, true, true, true};
  EXPECT_EQ(Decode(EventType::kWriteRowsEvent, w, table, &data, options), Status::kMalformedValue);
}

TEST(CodecRowsEvent, UnselectedColumnsStillValidateTheirMetadata) {
  // DECIMAL(5,6): the width comes from the metadata, so it is refused either way.
  TableMap table =
      Table({Column(ColumnType::kNewDecimal, (5u << 8) | 6u), Column(ColumnType::kLong)});
  Writer w = PostHeader();
  w.U8(2).U8(0x03).U8(0x00).Le(0, 8);
  DecodeOptions options;
  options.columns = {false, true};
  RowsEventData data;
  EXPECT_EQ(Decode(EventType::kWriteRowsEvent, w, table, &data, options), Status::kInvalidMetadata);

  // A MariaDB DECIMAL(40,31) keeps the row aligned when skipped.
  table.columns[0].meta = (40u << 8) | 31u;
  Writer aligned = PostHeader();
  aligned.U8(2).U8(0x03).U8(0x00);
  // 9 integer digits (4 bytes) and 31 fraction digits (3 groups + 4 digits: 12 + 2 bytes).
  for (int i = 0; i < 4 + 12 + 2; ++i) aligned.U8(i == 0 ? 0x80 : 0x00);
  aligned.Le(5, 4);
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, aligned, table, &data, options), Status::kOk);
  EXPECT_EQ(data.rows[0][1].i, 5);
}

// --- Decode budget ---

TEST(CodecRowsEvent, BudgetForEventBodyFloorsAtTheExpansionCeiling) {
  EXPECT_EQ(DecodeBudget::ForEventBody(0).limit, kMaxDecompressedColumnBytes);
  EXPECT_EQ(DecodeBudget::ForEventBody(1024).limit, kMaxDecompressedColumnBytes);
  EXPECT_EQ(DecodeBudget::ForEventBody(kMaxDecompressedColumnBytes).limit,
            kMaxDecompressedColumnBytes);
  EXPECT_EQ(DecodeBudget::ForEventBody(kMaxDecompressedColumnBytes + 1).limit,
            kMaxDecompressedColumnBytes + 1);
  EXPECT_EQ(DecodeBudget::ForEventBody(10, 100).limit, 100u);
  EXPECT_EQ(DecodeBudget::ForEventBody(4096).used, 0u);
}

TEST(CodecRowsEvent, BudgetChargeRejectsWithoutConsuming) {
  DecodeBudget budget;
  budget.limit = 10;
  EXPECT_TRUE(budget.Charge(6));
  EXPECT_FALSE(budget.Charge(5));
  EXPECT_EQ(budget.used, 6u);
  EXPECT_EQ(budget.Remaining(), 4u);
  EXPECT_TRUE(budget.Charge(4));
  EXPECT_EQ(budget.Remaining(), 0u);
}

constexpr size_t kValueOverhead = 48;
constexpr size_t kRowOverhead = 96;

TEST(CodecRowsEvent, BudgetChargesEveryRowAndColumn) {
  TableMap table = Table({Column(ColumnType::kLong), Column(ColumnType::kVarchar, 100)});
  Writer w = PostHeader();
  w.U8(2).U8(0x03);
  w.U8(0x00).Le(1, 4).U8(5).Str("Alice");
  w.U8(0x00).Le(2, 4).U8(3).Str("Bob");
  DecodeBudget budget;
  DecodeOptions options;
  options.budget = &budget;
  options.per_value_overhead = kValueOverhead;
  options.per_row_overhead = kRowOverhead;
  RowsEventData data;
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, w, table, &data, options), Status::kOk);
  ASSERT_EQ(data.rows.size(), 2u);
  EXPECT_EQ(budget.used, 5u + 3u + 2u * 2u * kValueOverhead + 2u * kRowOverhead);
}

TEST(CodecRowsEvent, BudgetChargesBothImagesOfAnUpdate) {
  TableMap table = Table({Column(ColumnType::kVarchar, 100)});
  Writer w = PostHeader();
  w.U8(1).U8(0x01).U8(0x01).U8(0x00).U8(4).Str("aaaa").U8(0x00).U8(2).Str("bb");
  DecodeBudget budget;
  DecodeOptions options;
  options.budget = &budget;
  options.per_value_overhead = kValueOverhead;
  options.per_row_overhead = kRowOverhead;
  RowsEventData data;
  ASSERT_EQ(Decode(EventType::kUpdateRowsEvent, w, table, &data, options), Status::kOk);
  EXPECT_EQ(budget.used, 4u + 2u + 2u * kValueOverhead + kRowOverhead);
}

TEST(CodecRowsEvent, ExhaustedBudgetFailsTheEvent) {
  // NULL columns cost one wire bit but a whole slot each.
  constexpr size_t kColumns = 16;
  Writer w = PostHeader();
  w.U8(kColumns);
  WriteBitmap(&w, kColumns, All(kColumns));
  for (int row = 0; row < 4; ++row) WriteBitmap(&w, kColumns, All(kColumns));
  DecodeBudget budget;
  budget.limit = 3 * kColumns * kValueOverhead;
  DecodeOptions options;
  options.budget = &budget;
  options.per_value_overhead = kValueOverhead;
  RowsEventData data;
  EXPECT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(kColumns), &data, options),
            Status::kLimitExceeded);
  EXPECT_LE(budget.used, budget.limit);

  budget = DecodeBudget();
  budget.limit = 4 * kColumns * kValueOverhead;
  ASSERT_EQ(Decode(EventType::kWriteRowsEvent, w, IntTable(kColumns), &data, options), Status::kOk);
  EXPECT_EQ(data.rows.size(), 4u);
  EXPECT_EQ(budget.used, budget.limit);
}

}  // namespace
}  // namespace mybinlog
