// TABLE_MAP_EVENT bodies built byte by byte: fixed part, per-column metadata
// widths, and the optional-metadata TLVs with their per-kind index spaces.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/server_flavor.h"
#include "mybinlog/status.h"
#include "mybinlog/table_map.h"

namespace mybinlog {
namespace {

EventHeader TableMapHeader() {
  EventHeader header;
  header.type_code = static_cast<uint8_t>(EventType::kTableMapEvent);
  return header;
}

constexpr uint8_t T(ColumnType type) { return static_cast<uint8_t>(type); }

// Optional-metadata kinds.
constexpr uint8_t kSignedness = 1;
constexpr uint8_t kDefaultCharset = 2;
constexpr uint8_t kColumnCharset = 3;
constexpr uint8_t kColumnName = 4;
constexpr uint8_t kSetStrValue = 5;
constexpr uint8_t kEnumStrValue = 6;
constexpr uint8_t kEnumAndSetDefaultCharset = 10;
constexpr uint8_t kEnumAndSetColumnCharset = 11;

class TableMapBuilder {
 public:
  void WriteTableId(uint64_t table_id) {
    for (int shift = 0; shift < 48; shift += 8) WriteByte(static_cast<uint8_t>(table_id >> shift));
  }
  void WriteFlags(uint16_t flags) {
    WriteByte(static_cast<uint8_t>(flags));
    WriteByte(static_cast<uint8_t>(flags >> 8));
  }
  void WriteDatabaseName(const std::string& name) { WriteName(name); }
  void WriteTableName(const std::string& name) { WriteName(name); }
  void WriteColumnCount(uint64_t count) { WritePacked(count); }
  void WriteColumnTypes(const std::vector<uint8_t>& types) { WriteRawBytes(types); }
  void WriteMetadataBlock(const std::vector<uint8_t>& bytes) {
    WritePacked(bytes.size());
    WriteRawBytes(bytes);
  }
  void WriteNullBitmap(const std::vector<uint8_t>& bitmap) { WriteRawBytes(bitmap); }
  void WriteRawBytes(const std::vector<uint8_t>& bytes) {
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
  }
  // One optional-metadata TLV.
  void WriteField(uint8_t kind, const std::vector<uint8_t>& value) {
    WriteByte(kind);
    WritePacked(value.size());
    WriteRawBytes(value);
  }
  void WritePacked(uint64_t value) {
    if (value < 251) {
      WriteByte(static_cast<uint8_t>(value));
    } else if (value <= 0xFFFF) {
      WriteByte(0xFC);
      for (int shift = 0; shift < 16; shift += 8) WriteByte(static_cast<uint8_t>(value >> shift));
    } else if (value <= 0xFFFFFF) {
      WriteByte(0xFD);
      for (int shift = 0; shift < 24; shift += 8) WriteByte(static_cast<uint8_t>(value >> shift));
    } else {
      WriteByte(0xFE);
      for (int shift = 0; shift < 64; shift += 8) WriteByte(static_cast<uint8_t>(value >> shift));
    }
  }

  const std::vector<uint8_t>& Data() const { return buf_; }
  size_t Size() const { return buf_.size(); }

 private:
  void WriteByte(uint8_t b) { buf_.push_back(b); }
  void WriteName(const std::string& name) {
    WriteByte(static_cast<uint8_t>(name.size()));
    buf_.insert(buf_.end(), name.begin(), name.end());
    WriteByte(0);
  }

  std::vector<uint8_t> buf_;
};

// Table id 42, "db"."t".
TableMapBuilder Header() {
  TableMapBuilder b;
  b.WriteTableId(42);
  b.WriteFlags(0);
  b.WriteDatabaseName("db");
  b.WriteTableName("t");
  return b;
}

// Header, types, metadata block and an all-nullable bitmap.
TableMapBuilder Columns(const std::vector<uint8_t>& types, const std::vector<uint8_t>& metadata) {
  TableMapBuilder b = Header();
  b.WriteColumnCount(types.size());
  b.WriteColumnTypes(types);
  b.WriteMetadataBlock(metadata);
  b.WriteNullBitmap(std::vector<uint8_t>((types.size() + 7) / 8, 0xFF));
  return b;
}

Status Parse(const TableMapBuilder& b, TableMap* out, ServerFlavor flavor = ServerFlavor::kMySQL) {
  return ParseTableMapEvent(TableMapHeader(), b.Data().data(), b.Size(), flavor, out);
}

// --- Fixed part and per-column metadata ---

TEST(CodecTableMap, ParseBasicIntColumns) {
  TableMapBuilder b;
  b.WriteTableId(42);
  b.WriteFlags(0);
  b.WriteDatabaseName("testdb");
  b.WriteTableName("users");
  b.WriteColumnCount(3);
  b.WriteColumnTypes({T(ColumnType::kLong), T(ColumnType::kLongLong), T(ColumnType::kTiny)});
  b.WriteMetadataBlock({});
  b.WriteNullBitmap({0x07});

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_EQ(map.table_id, 42u);
  EXPECT_EQ(map.schema, "testdb");
  EXPECT_EQ(map.table, "users");
  ASSERT_EQ(map.columns.size(), 3u);
  EXPECT_EQ(map.columns[0].type, ColumnType::kLong);
  EXPECT_EQ(map.columns[0].meta, 0u);
  EXPECT_TRUE(map.columns[0].nullable);
  EXPECT_EQ(map.columns[1].type, ColumnType::kLongLong);
  EXPECT_TRUE(map.columns[1].nullable);
  EXPECT_EQ(map.columns[2].type, ColumnType::kTiny);
  EXPECT_TRUE(map.columns[2].nullable);
}

TEST(CodecTableMap, ParseMixedColumnTypes) {
  TableMapBuilder b;
  b.WriteTableId(100);
  b.WriteFlags(0);
  b.WriteDatabaseName("mydb");
  b.WriteTableName("orders");
  // INT, VARCHAR(255), BLOB, DATETIME2(3), NEWDECIMAL(10,2).
  b.WriteColumnCount(5);
  b.WriteColumnTypes({T(ColumnType::kLong), T(ColumnType::kVarchar), T(ColumnType::kBlob),
                      T(ColumnType::kDatetime2), T(ColumnType::kNewDecimal)});
  b.WriteMetadataBlock({
      0xFF, 0x00,  // VARCHAR max_length=255
      0x02,        // BLOB pack_length=2
      0x03,        // DATETIME2 fsp=3
      0x0A, 0x02,  // NEWDECIMAL precision=10, scale=2
  });
  b.WriteNullBitmap({0x1E});  // column 0 NOT NULL

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_EQ(map.table_id, 100u);
  EXPECT_EQ(map.schema, "mydb");
  EXPECT_EQ(map.table, "orders");
  ASSERT_EQ(map.columns.size(), 5u);
  EXPECT_EQ(map.columns[0].type, ColumnType::kLong);
  EXPECT_EQ(map.columns[0].meta, 0u);
  EXPECT_FALSE(map.columns[0].nullable);
  EXPECT_EQ(map.columns[1].type, ColumnType::kVarchar);
  EXPECT_EQ(map.columns[1].meta, 255u);
  EXPECT_TRUE(map.columns[1].nullable);
  EXPECT_EQ(map.columns[2].type, ColumnType::kBlob);
  EXPECT_EQ(map.columns[2].meta, 2u);
  EXPECT_TRUE(map.columns[2].nullable);
  EXPECT_EQ(map.columns[3].type, ColumnType::kDatetime2);
  EXPECT_EQ(map.columns[3].meta, 3u);
  EXPECT_TRUE(map.columns[3].nullable);
  EXPECT_EQ(map.columns[4].type, ColumnType::kNewDecimal);
  EXPECT_EQ(map.columns[4].meta, (10u << 8) | 2u);
  EXPECT_TRUE(map.columns[4].nullable);
}

TEST(CodecTableMap, ParseMySqlTypedArrayMetadata) {
  TableMapBuilder b = Header();
  b.WriteColumnCount(3);
  b.WriteColumnTypes(
      {T(ColumnType::kTypedArray), T(ColumnType::kTypedArray), T(ColumnType::kLong)});
  // VARCHAR array: element type + 3-byte LE length. NEWDECIMAL array: element
  // type + precision + scale. INT consumes no metadata.
  b.WriteMetadataBlock(
      {T(ColumnType::kVarchar), 0x34, 0x12, 0x00, T(ColumnType::kNewDecimal), 10, 2});
  b.WriteNullBitmap({0x07});

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  ASSERT_EQ(map.columns.size(), 3u);
  EXPECT_EQ(map.columns[0].type, ColumnType::kTypedArray);
  EXPECT_EQ(map.columns[0].element_type, ColumnType::kVarchar);
  EXPECT_EQ(map.columns[0].meta, 0x1234u);
  EXPECT_EQ(map.columns[1].type, ColumnType::kTypedArray);
  EXPECT_EQ(map.columns[1].element_type, ColumnType::kNewDecimal);
  EXPECT_EQ(map.columns[1].meta, (10u << 8) | 2u);
  EXPECT_EQ(map.columns[2].type, ColumnType::kLong);
}

TEST(CodecTableMap, RejectsTruncatedOrUnknownTypedArrayMetadata) {
  for (const auto& metadata : std::vector<std::vector<uint8_t>>{
           {T(ColumnType::kVarchar), 0x10},  // 3-byte length cut short
           {T(ColumnType::kLong)},           // not an element type the server writes
           {T(ColumnType::kYear)},           // written with no element byte upstream
       }) {
    TableMap map;
    EXPECT_EQ(Parse(Columns({T(ColumnType::kTypedArray)}, metadata), &map),
              Status::kInvalidMetadata);
  }
}

TEST(CodecTableMap, ParseMariaDbCompressedMetadata) {
  TableMapBuilder b = Columns({T(ColumnType::kBlobCompressed), T(ColumnType::kVarcharCompressed)},
                              {0x03, 0x11, 0x01});

  TableMap map;
  ASSERT_EQ(Parse(b, &map, ServerFlavor::kMariaDB), Status::kOk);
  ASSERT_EQ(map.columns.size(), 2u);
  EXPECT_EQ(map.columns[0].type, ColumnType::kBlobCompressed);
  EXPECT_EQ(map.columns[0].meta, 3u);
  EXPECT_EQ(map.columns[1].type, ColumnType::kVarcharCompressed);
  EXPECT_EQ(map.columns[1].meta, 273u);
}

TEST(CodecTableMap, ParseMariaDbCompressedColumnCharsetMetadata) {
  TableMapBuilder b = Columns({T(ColumnType::kVarcharCompressed), T(ColumnType::kBlobCompressed)},
                              {0x90, 0x01, 0x02});
  // One collation per compressed character column, in TABLE_MAP order.
  b.WriteField(kColumnCharset, {0x2D, 0x3F});

  TableMap map;
  ASSERT_EQ(Parse(b, &map, ServerFlavor::kMariaDB), Status::kOk);
  ASSERT_EQ(map.columns.size(), 2u);
  EXPECT_TRUE(map.columns[0].has_charset);
  EXPECT_EQ(map.columns[0].charset, 45u);
  EXPECT_TRUE(map.columns[1].has_charset);
  EXPECT_EQ(map.columns[1].charset, 63u);
}

// --- SIGNEDNESS / COLUMN_NAME ---

TEST(CodecTableMap, ParseSignednessOptionalMetadata) {
  TableMapBuilder b =
      Columns({T(ColumnType::kLong), T(ColumnType::kLongLong), T(ColumnType::kTiny)}, {});
  // MSB-first over the numeric columns: 0x40 marks numeric index 1 (BIGINT).
  b.WriteField(kSignedness, {0x40});

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  ASSERT_EQ(map.columns.size(), 3u);
  EXPECT_TRUE(map.has_signedness);
  EXPECT_FALSE(map.columns[0].is_unsigned);
  EXPECT_TRUE(map.columns[1].is_unsigned);
  EXPECT_FALSE(map.columns[2].is_unsigned);
}

TEST(CodecTableMap, ParseSignednessAccountsForYearAndLegacyDecimal) {
  // YEAR and legacy DECIMAL each take a SIGNEDNESS bit, so the BIGINT bit is
  // the third, not the second.
  TableMapBuilder b = Columns({T(ColumnType::kLong), T(ColumnType::kYear), T(ColumnType::kLongLong),
                               T(ColumnType::kDecimal)},
                              {});
  b.WriteField(kSignedness, {0x30});  // BIGINT and DECIMAL unsigned

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  ASSERT_EQ(map.columns.size(), 4u);
  EXPECT_TRUE(map.has_signedness);
  EXPECT_FALSE(map.columns[0].is_unsigned);
  EXPECT_FALSE(map.columns[1].is_unsigned);
  EXPECT_TRUE(map.columns[2].is_unsigned);
  EXPECT_TRUE(map.columns[3].is_unsigned);
}

TEST(CodecTableMap, RejectsTruncatedSignednessBitmap) {
  TableMapBuilder b = Columns({T(ColumnType::kLong)}, {});
  b.WriteField(kSignedness, {});

  TableMap map;
  EXPECT_EQ(Parse(b, &map), Status::kInvalidMetadata);
}

TEST(CodecTableMap, ParseColumnNameOptionalMetadata) {
  TableMapBuilder b = Columns({T(ColumnType::kLong), T(ColumnType::kLong)}, {});
  b.WriteField(kColumnName, {0x02, 'i', 'd', 0x06, 'a', 'm', 'o', 'u', 'n', 't'});

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  ASSERT_EQ(map.columns.size(), 2u);
  EXPECT_TRUE(map.has_column_names);
  EXPECT_EQ(map.columns[0].name, "id");
  EXPECT_EQ(map.columns[1].name, "amount");
}

// --- Charsets ---

TEST(CodecTableMap, ParseDefaultCharsetOptionalMetadata) {
  TableMapBuilder b = Columns({T(ColumnType::kVarchar), T(ColumnType::kBlob)}, {0xFF, 0x00, 0x01});
  // Default collation 45, then character-column index 1 (BLOB) set to 63.
  b.WriteField(kDefaultCharset, {0x2D, 0x01, 0x3F});

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  ASSERT_EQ(map.columns.size(), 2u);
  EXPECT_TRUE(map.columns[0].has_charset);
  EXPECT_EQ(map.columns[0].charset, 45u);
  EXPECT_TRUE(map.columns[1].has_charset);
  EXPECT_EQ(map.columns[1].charset, 63u);
}

TEST(CodecTableMap, ParseColumnCharsetOptionalMetadata) {
  TableMapBuilder b = Columns({T(ColumnType::kVarchar), T(ColumnType::kBlob)}, {0xFF, 0x00, 0x01});
  b.WriteField(kColumnCharset, {0x3F, 0x2D});

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_EQ(map.columns[0].charset, 63u);
  EXPECT_EQ(map.columns[1].charset, 45u);
}

TEST(CodecTableMap, DefaultCharsetSkipsEnumAndSetColumns) {
  // (status ENUM, flags SET, name VARCHAR(200), data VARBINARY(100)). ENUM and
  // SET travel as STRING with the real type in metadata byte 0.
  TableMapBuilder b = Columns({T(ColumnType::kString), T(ColumnType::kString),
                               T(ColumnType::kVarchar), T(ColumnType::kVarchar)},
                              {
                                  0xF7, 0x01,  // ENUM, pack_length 1
                                  0xF8, 0x01,  // SET, pack_length 1
                                  0xC8, 0x00,  // VARCHAR(200)
                                  0x64, 0x00,  // VARBINARY(100)
                              });
  // DEFAULT_CHARSET indexes only the two character columns, so index 1 is
  // VARBINARY. ENUM_AND_SET_DEFAULT_CHARSET covers ENUM and SET.
  b.WriteField(kDefaultCharset, {0x2D, 0x01, 0x3F});
  b.WriteField(kEnumAndSetDefaultCharset, {0x2D});

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  ASSERT_EQ(map.columns.size(), 4u);
  EXPECT_EQ(RealType(map.columns[0]), ColumnType::kEnum);
  EXPECT_EQ(RealType(map.columns[1]), ColumnType::kSet);
  EXPECT_TRUE(map.columns[0].has_charset);
  EXPECT_EQ(map.columns[0].charset, 45u);
  EXPECT_TRUE(map.columns[1].has_charset);
  EXPECT_EQ(map.columns[1].charset, 45u);
  EXPECT_TRUE(map.columns[2].has_charset);
  EXPECT_EQ(map.columns[2].charset, 45u);
  EXPECT_TRUE(map.columns[3].has_charset);
  EXPECT_EQ(map.columns[3].charset, 63u);
}

TEST(CodecTableMap, DefaultCharsetCountsVectorColumns) {
  // (id INT, embedding VECTOR(3), name VARCHAR(50), payload BLOB). VECTOR takes
  // a character slot even though its collation is always binary.
  TableMapBuilder b = Columns(
      {T(ColumnType::kLong), T(ColumnType::kVector), T(ColumnType::kVarchar), T(ColumnType::kBlob)},
      {
          0x04,        // VECTOR pack_length 4
          0x32, 0x00,  // VARCHAR(50)
          0x02,        // BLOB pack_length 2
      });
  b.WriteField(kSignedness, {0x00});
  // Over [VECTOR, VARCHAR, BLOB]: default binary, index 1 (VARCHAR) utf8mb4.
  // Were VECTOR skipped, VARCHAR and BLOB would swap collations.
  b.WriteField(kDefaultCharset, {0x3F, 0x01, 0x2D});
  b.WriteField(13, {0x03});  // VECTOR_DIMENSIONALITY

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  ASSERT_EQ(map.columns.size(), 4u);
  EXPECT_FALSE(map.columns[0].has_charset);
  EXPECT_TRUE(map.columns[1].has_charset);
  EXPECT_EQ(map.columns[1].charset, 63u);
  EXPECT_TRUE(map.columns[2].has_charset);
  EXPECT_EQ(map.columns[2].charset, 45u);
  EXPECT_TRUE(map.columns[3].has_charset);
  EXPECT_EQ(map.columns[3].charset, 63u);
}

TEST(CodecTableMap, SkipsEveryUnhandledOptionalMetadataField) {
  TableMapBuilder b = Columns({T(ColumnType::kVarchar)}, {0xFF, 0x00});
  // Kinds read only as opaque bytes (GEOMETRY_TYPE, both primary-key forms,
  // COLUMN_VISIBILITY, VECTOR_DIMENSIONALITY) and kinds no server defines.
  for (uint8_t kind : {7, 8, 9, 12, 13, 14, 127, 255}) b.WriteField(kind, {kind});
  // A COLUMN_NAME after them proves the cursor stayed aligned.
  b.WriteField(kColumnName, {0x02, 'i', 'd'});

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  ASSERT_EQ(map.columns.size(), 1u);
  EXPECT_EQ(map.columns[0].name, "id");
}

TEST(CodecTableMap, OptionalMetadataAbsentIsSafe) {
  TableMapBuilder b = Columns({T(ColumnType::kLong)}, {});

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  ASSERT_EQ(map.columns.size(), 1u);
  EXPECT_FALSE(map.has_signedness);
  EXPECT_FALSE(map.has_column_names);
  EXPECT_FALSE(map.columns[0].is_unsigned);
  EXPECT_TRUE(map.columns[0].name.empty());
}

TEST(CodecTableMap, AbsentCharsetMetadataResolvesCharacterColumnsToBytes) {
  // (bin VARBINARY(100), txt VARCHAR(100), blb BLOB, txtblb TEXT): each pair
  // shares one type byte, so only a collation could tell them apart.
  TableMapBuilder b = Columns({T(ColumnType::kVarchar), T(ColumnType::kVarchar),
                               T(ColumnType::kBlob), T(ColumnType::kBlob)},
                              {0x64, 0x00, 0x64, 0x00, 0x02, 0x02});

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  ASSERT_EQ(map.columns.size(), 4u);
  for (size_t i = 0; i < map.columns.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_FALSE(map.columns[i].has_charset);
  }
}

// (TINYINT, SMALLINT, MEDIUMINT, INT, BIGINT), all fixed-size.
const std::vector<uint8_t> kNumericTypes = {T(ColumnType::kTiny), T(ColumnType::kShort),
                                            T(ColumnType::kInt24), T(ColumnType::kLong),
                                            T(ColumnType::kLongLong)};

TEST(CodecTableMap, AbsentSignednessMetadataResolvesNumericColumnsToSigned) {
  TableMapBuilder b = Columns(kNumericTypes, {});

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  ASSERT_EQ(map.columns.size(), kNumericTypes.size());
  EXPECT_FALSE(map.has_signedness);
  for (size_t i = 0; i < map.columns.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_FALSE(map.columns[i].is_unsigned);
  }
}

TEST(CodecTableMap, PresentSignednessMetadataResolvesNumericColumnsToUnsigned) {
  TableMapBuilder b = Columns(kNumericTypes, {});
  b.WriteField(kSignedness, {0xF8});  // all five unsigned

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  ASSERT_EQ(map.columns.size(), kNumericTypes.size());
  EXPECT_TRUE(map.has_signedness);
  for (size_t i = 0; i < map.columns.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_TRUE(map.columns[i].is_unsigned);
  }
}

// --- Lengths that do not fit ---

TEST(CodecTableMap, RejectsMetadataLengthUint64Max) {
  TableMapBuilder b = Header();
  b.WriteColumnCount(1);
  b.WriteColumnTypes({T(ColumnType::kVarchar)});
  b.WriteRawBytes({0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});

  TableMap map;
  EXPECT_EQ(Parse(b, &map), Status::kTruncated);
}

TEST(CodecTableMap, RejectsMetadataLengthSizeMax) {
  TableMapBuilder b = Header();
  b.WriteColumnCount(1);
  b.WriteColumnTypes({T(ColumnType::kVarchar)});
  b.WriteRawBytes({0xFE});
  const uint64_t size_max = std::numeric_limits<size_t>::max();
  for (int shift = 0; shift < 64; shift += 8) {
    b.WriteRawBytes({static_cast<uint8_t>(size_max >> shift)});
  }

  TableMap map;
  EXPECT_EQ(Parse(b, &map), Status::kTruncated);
}

TEST(CodecTableMap, RejectsOptionalMetadataLengthUint64Max) {
  TableMapBuilder b = Columns({T(ColumnType::kLong)}, {});
  b.WriteRawBytes({kColumnName, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});

  TableMap map;
  EXPECT_EQ(Parse(b, &map), Status::kTruncated);
}

TEST(CodecTableMap, RejectsOverflowingColumnNameLength) {
  TableMapBuilder b = Columns({T(ColumnType::kLong)}, {});
  b.WriteField(kColumnName, {0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});

  TableMap map;
  EXPECT_EQ(Parse(b, &map), Status::kInvalidMetadata);
}

TEST(CodecTableMap, RejectsAMetadataBlockThatDoesNotMatchTheColumnTypes) {
  struct Case {
    const char* description;
    std::vector<uint8_t> types;
    std::vector<uint8_t> metadata;
  };
  const Case cases[] = {
      {"one byte too long", {T(ColumnType::kLong)}, {0x01}},
      {"one byte too short", {T(ColumnType::kVarchar)}, {0x64}},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.description);
    TableMap map;
    EXPECT_EQ(Parse(Columns(c.types, c.metadata), &map), Status::kInvalidMetadata);
  }
}

TEST(CodecTableMap, ParseTruncatedBuffer) {
  const uint8_t buf[5] = {0};
  TableMap map;
  EXPECT_EQ(ParseTableMapEvent(TableMapHeader(), buf, sizeof(buf), ServerFlavor::kMySQL, &map),
            Status::kTruncated);
}

TEST(CodecTableMap, ParseNullData) {
  TableMap map;
  EXPECT_EQ(ParseTableMapEvent(TableMapHeader(), nullptr, 100, ServerFlavor::kMySQL, &map),
            Status::kTruncated);
}

TEST(CodecTableMap, OutputUntouchedOnFailure) {
  TableMap map;
  map.table_id = 7;
  map.columns.resize(2);
  TableMapBuilder b = Columns({T(ColumnType::kVarchar)}, {0x64});  // block too short
  ASSERT_EQ(Parse(b, &map), Status::kInvalidMetadata);
  EXPECT_EQ(map.table_id, 7u);
  EXPECT_EQ(map.columns.size(), 2u);
}

// --- Single-column layouts ---

TEST(CodecTableMap, VarcharColumn) {
  TableMapBuilder b = Columns({0x0F}, {0xC8, 0x00});  // max_length 200
  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_EQ(map.columns[0].type, ColumnType::kVarchar);
  EXPECT_EQ(map.columns[0].meta, 200u);
}

TEST(CodecTableMap, BlobColumn) {
  TableMapBuilder b = Columns({0xFC}, {0x02});  // pack_length 2
  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_EQ(map.columns[0].type, ColumnType::kBlob);
  EXPECT_EQ(map.columns[0].meta, 2u);
}

TEST(CodecTableMap, Datetime2Column) {
  TableMapBuilder b = Columns({0x12}, {0x06});  // fsp 6
  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_EQ(map.columns[0].type, ColumnType::kDatetime2);
  EXPECT_EQ(map.columns[0].meta, 6u);
}

TEST(CodecTableMap, NewDecimalColumn) {
  TableMapBuilder b = Columns({0xF6}, {10, 2});  // precision, scale
  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_EQ(map.columns[0].type, ColumnType::kNewDecimal);
  EXPECT_EQ(map.columns[0].meta, (10u << 8) | 2u);
}

TEST(CodecTableMap, MultipleColumnTypes) {
  TableMapBuilder b = Columns({0x03, 0x0F, 0x12}, {0xF4, 0x01, 0x03});  // VARCHAR(500), fsp 3
  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  ASSERT_EQ(map.columns.size(), 3u);
  EXPECT_EQ(map.columns[0].type, ColumnType::kLong);
  EXPECT_EQ(map.columns[1].type, ColumnType::kVarchar);
  EXPECT_EQ(map.columns[1].meta, 500u);
  EXPECT_EQ(map.columns[2].type, ColumnType::kDatetime2);
  EXPECT_EQ(map.columns[2].meta, 3u);
}

// --- Rules ---

TEST(CodecTableMapRules, ColumnCountZeroIsInvalid) {
  TableMapBuilder b = Header();
  b.WriteColumnCount(0);
  b.WriteMetadataBlock({});
  TableMap map;
  EXPECT_EQ(Parse(b, &map), Status::kInvalidMetadata);
}

TEST(CodecTableMapRules, ColumnCountAtTheLimitParses) {
  TableMapBuilder b = Columns(std::vector<uint8_t>(kMaxTableColumns, T(ColumnType::kLong)), {});
  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_EQ(map.columns.size(), kMaxTableColumns);
}

TEST(CodecTableMapRules, ColumnCountAboveTheLimitIsInvalid) {
  TableMapBuilder b = Columns(std::vector<uint8_t>(kMaxTableColumns + 1, T(ColumnType::kLong)), {});
  TableMap map;
  EXPECT_EQ(Parse(b, &map), Status::kInvalidMetadata);
}

TEST(CodecTableMapRules, ColumnCountNullMarkerOrUndefinedPrefixIsInvalid) {
  for (uint8_t prefix : {0xFB, 0xFF}) {
    SCOPED_TRACE(static_cast<int>(prefix));
    TableMapBuilder b = Header();
    b.WriteRawBytes({prefix, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    TableMap map;
    EXPECT_EQ(Parse(b, &map), Status::kInvalidMetadata);
  }
}

TEST(CodecTableMapRules, LengthPrefixWidthMustBeOneToFour) {
  const ColumnType kWidthTypes[] = {
      ColumnType::kTinyBlob, ColumnType::kMediumBlob,     ColumnType::kLongBlob,
      ColumnType::kBlob,     ColumnType::kBlobCompressed, ColumnType::kGeometry,
      ColumnType::kJson,     ColumnType::kVector,
  };
  for (ColumnType type : kWidthTypes) {
    for (uint8_t width = 0; width <= 5; ++width) {
      SCOPED_TRACE(testing::Message()
                   << "type " << static_cast<int>(type) << " width " << static_cast<int>(width));
      TableMap map;
      const Status status = Parse(Columns({T(type)}, {width}), &map);
      if (width >= 1 && width <= 4) {
        ASSERT_EQ(status, Status::kOk);
        EXPECT_EQ(map.columns[0].meta, width);
      } else {
        EXPECT_EQ(status, Status::kInvalidMetadata);
      }
    }
  }
}

TEST(CodecTableMapRules, UnknownColumnTypeIsUnsupported) {
  // MYSQL_TYPE_NULL, a gap after TYPED_ARRAY, INVALID, BOOL, and a MariaDB-range gap.
  for (uint8_t code : {0x06, 0x15, 0xF3, 0xF4, 0x8E}) {
    SCOPED_TRACE(static_cast<int>(code));
    TableMap map;
    EXPECT_EQ(Parse(Columns({T(ColumnType::kLong), code, T(ColumnType::kLong)}, {}), &map),
              Status::kUnsupported);
  }
}

TEST(CodecTableMapRules, VarStringMetadataIsRealTypeThenLength) {
  // Field_string::do_save_field_metadata writes VAR_STRING like CHAR.
  TableMapBuilder b = Columns({T(ColumnType::kVarString)}, {0xFD, 0x20});
  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_EQ(map.columns[0].meta, 0xFD20u);
  EXPECT_EQ(RealType(map.columns[0]), ColumnType::kVarString);
}

TEST(CodecTableMapRules, BitMetadataIsLittleEndian) {
  TableMapBuilder b = Columns({T(ColumnType::kBit)}, {0x02, 0x01});  // BIT(10): 2 bits, 1 byte
  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_EQ(map.columns[0].meta, 0x0102u);
}

TEST(CodecTableMapRules, CharLongerThan255IsNotTakenForEnumOrSet) {
  // CHAR(255) in utf8mb4: length 1020 folds bits 8-9 into byte 0 (0xFE ^ 0x30).
  TableMapBuilder b = Columns({T(ColumnType::kString)}, {0xCE, 0xFC});
  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_EQ(RealType(map.columns[0]), ColumnType::kString);
}

TEST(CodecTableMapRules, TableIdUsesAll48Bits) {
  TableMapBuilder b;
  b.WriteTableId(0xFFFFFFFFFFFFull);
  b.WriteFlags(0);
  b.WriteDatabaseName("db");
  b.WriteTableName("t");
  b.WriteColumnCount(1);
  b.WriteColumnTypes({T(ColumnType::kLong)});
  b.WriteMetadataBlock({});
  b.WriteNullBitmap({0x00});
  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_EQ(map.table_id, 0xFFFFFFFFFFFFull);
  EXPECT_FALSE(map.columns[0].nullable);
}

TEST(CodecTableMapRules, TruncationAnywhereInTheFixedPartIsTruncated) {
  TableMapBuilder b = Columns({T(ColumnType::kVarchar), T(ColumnType::kLong)}, {0x10, 0x00});
  for (size_t len = 0; len < b.Size(); ++len) {
    SCOPED_TRACE(len);
    TableMap map;
    EXPECT_EQ(
        ParseTableMapEvent(TableMapHeader(), b.Data().data(), len, ServerFlavor::kMySQL, &map),
        Status::kTruncated);
  }
}

TEST(CodecTableMapRules, UnknownOptionalMetadataKindIsSkippedByLength) {
  TableMapBuilder b = Columns({T(ColumnType::kLong)}, {});
  b.WriteField(200, std::vector<uint8_t>(300, 0xAB));  // two-byte packed length
  b.WriteField(kSignedness, {0x80});

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_TRUE(map.columns[0].is_unsigned);
}

TEST(CodecTableMapRules, OptionalMetadataNullMarkerLengthIsInvalid) {
  TableMapBuilder b = Columns({T(ColumnType::kLong)}, {});
  b.WriteRawBytes({kColumnName, 0xFB});
  TableMap map;
  EXPECT_EQ(Parse(b, &map), Status::kInvalidMetadata);
}

TEST(CodecTableMapRules, OptionalMetadataKindWithoutLengthIsTruncated) {
  TableMapBuilder b = Columns({T(ColumnType::kLong)}, {});
  b.WriteRawBytes({kColumnName});
  TableMap map;
  EXPECT_EQ(Parse(b, &map), Status::kTruncated);
}

TEST(CodecTableMapRules, SignednessLongerThanTheNumericColumnsIsInvalid) {
  TableMapBuilder b = Columns({T(ColumnType::kLong), T(ColumnType::kVarchar)}, {0x10, 0x00});
  b.WriteField(kSignedness, {0x80, 0x00});
  TableMap map;
  EXPECT_EQ(Parse(b, &map), Status::kInvalidMetadata);
}

TEST(CodecTableMapRules, TypedArrayTakesNoSignednessBit) {
  TableMapBuilder b =
      Columns({T(ColumnType::kTypedArray), T(ColumnType::kLong)}, {T(ColumnType::kLongLong)});
  b.WriteField(kSignedness, {0x80});  // the one bit is the INT's
  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_FALSE(map.columns[0].is_unsigned);
  EXPECT_TRUE(map.columns[1].is_unsigned);
}

TEST(CodecTableMapRules, ColumnNamesMustCoverEveryColumnExactly) {
  for (const auto& value : std::vector<std::vector<uint8_t>>{
           {0x01, 'a'},                        // one name short
           {0x01, 'a', 0x01, 'b', 0x01, 'c'},  // one name over
           {0x01, 'a', 0x01},                  // second name cut short
       }) {
    TableMapBuilder b = Columns({T(ColumnType::kLong), T(ColumnType::kLong)}, {});
    b.WriteField(kColumnName, value);
    TableMap map;
    EXPECT_EQ(Parse(b, &map), Status::kInvalidMetadata);
  }
}

TEST(CodecTableMapRules, DefaultCharsetIndexOutsideTheCharacterColumnsIsInvalid) {
  TableMapBuilder b = Columns({T(ColumnType::kVarchar), T(ColumnType::kLong)}, {0x10, 0x00});
  b.WriteField(kDefaultCharset, {0x2D, 0x01, 0x3F});  // only index 0 exists
  TableMap map;
  EXPECT_EQ(Parse(b, &map), Status::kInvalidMetadata);
}

TEST(CodecTableMapRules, ColumnCharsetMustCoverEveryCharacterColumnExactly) {
  for (const auto& value : std::vector<std::vector<uint8_t>>{{0x2D}, {0x2D, 0x3F, 0x08}}) {
    TableMapBuilder b =
        Columns({T(ColumnType::kVarchar), T(ColumnType::kBlob)}, {0x10, 0x00, 0x02});
    b.WriteField(kColumnCharset, value);
    TableMap map;
    EXPECT_EQ(Parse(b, &map), Status::kInvalidMetadata);
  }
}

TEST(CodecTableMapRules, TwoByteCollationIdsParse) {
  TableMapBuilder b = Columns({T(ColumnType::kVarchar)}, {0x10, 0x00});
  b.WriteField(kColumnCharset, {0xFC, 0x00, 0x09});  // 2304, MariaDB's uca1400 default
  TableMap map;
  ASSERT_EQ(Parse(b, &map, ServerFlavor::kMariaDB), Status::kOk);
  EXPECT_EQ(map.columns[0].charset, 2304u);
}

TEST(CodecTableMapRules, GeometryTakesACharsetSlotOnMariaDbOnly) {
  // (g GEOMETRY, s VARCHAR(16)). MariaDB writes a collation for every field
  // that has a charset, GEOMETRY included; MySQL's is_character_type excludes it.
  const std::vector<uint8_t> types = {T(ColumnType::kGeometry), T(ColumnType::kVarchar)};
  const std::vector<uint8_t> metadata = {0x04, 0x10, 0x00};
  {
    TableMapBuilder b = Columns(types, metadata);
    b.WriteField(kColumnCharset, {0x2D});
    TableMap map;
    ASSERT_EQ(Parse(b, &map, ServerFlavor::kMySQL), Status::kOk);
    EXPECT_FALSE(map.columns[0].has_charset);
    EXPECT_EQ(map.columns[1].charset, 45u);
    EXPECT_EQ(Parse(b, &map, ServerFlavor::kMariaDB), Status::kInvalidMetadata);
  }
  {
    TableMapBuilder b = Columns(types, metadata);
    b.WriteField(kColumnCharset, {0x3F, 0x2D});
    TableMap map;
    ASSERT_EQ(Parse(b, &map, ServerFlavor::kMariaDB), Status::kOk);
    EXPECT_EQ(map.columns[0].charset, 63u);
    EXPECT_EQ(map.columns[1].charset, 45u);
    EXPECT_EQ(Parse(b, &map, ServerFlavor::kMySQL), Status::kInvalidMetadata);
  }
}

TEST(CodecTableMapRules, JsonTakesNoCharsetSlot) {
  TableMapBuilder b = Columns({T(ColumnType::kJson), T(ColumnType::kVarchar)}, {0x04, 0x10, 0x00});
  b.WriteField(kColumnCharset, {0x2D});
  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_FALSE(map.columns[0].has_charset);
  EXPECT_EQ(map.columns[1].charset, 45u);
}

TEST(CodecTableMapRules, VarcharTypedArrayTakesACharsetSlot) {
  // Field_typed_array's real type is its element type.
  TableMapBuilder b =
      Columns({T(ColumnType::kTypedArray), T(ColumnType::kTypedArray), T(ColumnType::kVarchar)},
              {T(ColumnType::kVarchar), 0x40, 0x00, 0x00, T(ColumnType::kLongLong), 0x10, 0x00});
  b.WriteField(kColumnCharset, {0x3F, 0x2D});
  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_EQ(map.columns[0].charset, 63u);
  EXPECT_FALSE(map.columns[1].has_charset);
  EXPECT_EQ(map.columns[2].charset, 45u);
}

// (e ENUM('a','b'), s VARCHAR(16), st SET('x','y','z')).
TableMapBuilder EnumSetTable() {
  return Columns({T(ColumnType::kString), T(ColumnType::kVarchar), T(ColumnType::kString)},
                 {0xF7, 0x01, 0x10, 0x00, 0xF8, 0x01});
}

TEST(CodecTableMapRules, EnumAndSetLabels) {
  TableMapBuilder b = EnumSetTable();
  b.WriteField(kSetStrValue, {0x03, 0x01, 'x', 0x01, 'y', 0x01, 'z'});
  b.WriteField(kEnumStrValue, {0x02, 0x01, 'a', 0x02, 'b', 'b'});

  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_TRUE(map.has_enum_labels);
  EXPECT_TRUE(map.has_set_labels);
  EXPECT_EQ(RealType(map.columns[0]), ColumnType::kEnum);
  EXPECT_EQ(RealType(map.columns[2]), ColumnType::kSet);
  EXPECT_EQ(map.columns[0].labels, (std::vector<std::string>{"a", "bb"}));
  EXPECT_TRUE(map.columns[1].labels.empty());
  EXPECT_EQ(map.columns[2].labels, (std::vector<std::string>{"x", "y", "z"}));
}

TEST(CodecTableMapRules, EnumLabelsAbsentLeavesThemUnknown) {
  TableMap map;
  ASSERT_EQ(Parse(EnumSetTable(), &map), Status::kOk);
  EXPECT_FALSE(map.has_enum_labels);
  EXPECT_FALSE(map.has_set_labels);
  EXPECT_TRUE(map.columns[0].labels.empty());
}

TEST(CodecTableMapRules, LabelGroupsMustMatchTheColumnsOfTheirKind) {
  for (const auto& value : std::vector<std::vector<uint8_t>>{
           {},                                  // no group for the ENUM column
           {0x01, 0x01, 'a', 0x01, 0x01, 'b'},  // a second group with no column
           {0x02, 0x01, 'a'},                   // second label missing
           {0x01, 0x05, 'a'},                   // label runs past the field
           {0xFF, 0x01, 'a'},                   // count is the undefined prefix
       }) {
    TableMapBuilder b = EnumSetTable();
    b.WriteField(kEnumStrValue, value);
    TableMap map;
    EXPECT_EQ(Parse(b, &map), Status::kInvalidMetadata);
  }
}

TEST(CodecTableMapRules, EnumAndSetCharsetUsesItsOwnIndexSpace) {
  TableMapBuilder b = EnumSetTable();
  b.WriteField(kDefaultCharset, {0x2D});                        // the VARCHAR
  b.WriteField(kEnumAndSetDefaultCharset, {0x08, 0x01, 0x21});  // index 1 is the SET
  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_EQ(map.columns[0].charset, 8u);
  EXPECT_EQ(map.columns[1].charset, 45u);
  EXPECT_EQ(map.columns[2].charset, 33u);
}

TEST(CodecTableMapRules, EnumAndSetColumnCharset) {
  TableMapBuilder b = EnumSetTable();
  b.WriteField(kEnumAndSetColumnCharset, {0x08, 0x21});
  TableMap map;
  ASSERT_EQ(Parse(b, &map), Status::kOk);
  EXPECT_EQ(map.columns[0].charset, 8u);
  EXPECT_FALSE(map.columns[1].has_charset);
  EXPECT_EQ(map.columns[2].charset, 33u);
}

TEST(CodecTableMapRules, HasSignednessBit) {
  for (ColumnType type :
       {ColumnType::kTiny, ColumnType::kShort, ColumnType::kInt24, ColumnType::kLong,
        ColumnType::kLongLong, ColumnType::kYear, ColumnType::kFloat, ColumnType::kDouble,
        ColumnType::kDecimal, ColumnType::kNewDecimal}) {
    EXPECT_TRUE(HasSignednessBit(type)) << static_cast<int>(type);
  }
  for (ColumnType type : {ColumnType::kBit, ColumnType::kTypedArray, ColumnType::kVarchar,
                          ColumnType::kTimestamp2, ColumnType::kEnum, ColumnType::kString}) {
    EXPECT_FALSE(HasSignednessBit(type)) << static_cast<int>(type);
  }
}

}  // namespace
}  // namespace mybinlog
