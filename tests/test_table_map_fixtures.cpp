// Every TABLE_MAP and QUERY event of the real server binlogs in fixtures/
// (see fixtures/capture.sh for the fx.t DDL), parsed through the codec.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "mybinlog/checksum.h"
#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/query_event.h"
#include "mybinlog/server_flavor.h"
#include "mybinlog/table_map.h"

namespace mybinlog {
namespace {

using Bytes = std::vector<uint8_t>;

bool ReadFixture(const char* name, Bytes* out) {
  const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !out->empty();
}

struct FixtureCase {
  const char* file;
  ServerFlavor flavor;
};

constexpr FixtureCase kFixtures[] = {
    {"mysql84_none_tagged.bin", ServerFlavor::kMySQL},
    {"mysql84_crc32_tagged.bin", ServerFlavor::kMySQL},
    {"mysql97_none_tagged.bin", ServerFlavor::kMySQL},
    {"mysql97_crc32_tagged.bin", ServerFlavor::kMySQL},
    {"mariadb118_none.bin", ServerFlavor::kMariaDB},
    {"mariadb118_crc32.bin", ServerFlavor::kMariaDB},
};

constexpr uint32_t kBinaryCollation = 63;

struct ExpectedColumn {
  const char* name;
  ColumnType type;
  ColumnType real_type;
};

// fx.t in declaration order. MariaDB stores JSON as LONGTEXT and appends cz.
std::vector<ExpectedColumn> ExpectedColumns(ServerFlavor flavor) {
  const bool mariadb = flavor == ServerFlavor::kMariaDB;
  std::vector<ExpectedColumn> columns = {
      {"id", ColumnType::kLong, ColumnType::kLong},
      {"i8", ColumnType::kTiny, ColumnType::kTiny},
      {"u32", ColumnType::kLong, ColumnType::kLong},
      {"i64", ColumnType::kLongLong, ColumnType::kLongLong},
      {"f", ColumnType::kFloat, ColumnType::kFloat},
      {"d", ColumnType::kDouble, ColumnType::kDouble},
      {"dec1", ColumnType::kNewDecimal, ColumnType::kNewDecimal},
      {"dec2", ColumnType::kNewDecimal, ColumnType::kNewDecimal},
      {"s", ColumnType::kVarchar, ColumnType::kVarchar},
      {"b", ColumnType::kBlob, ColumnType::kBlob},
      {"vb", ColumnType::kVarchar, ColumnType::kVarchar},
      {"dt", ColumnType::kDatetime2, ColumnType::kDatetime2},
      {"d0", ColumnType::kDate, ColumnType::kDate},
      {"tm", ColumnType::kTime2, ColumnType::kTime2},
      {"ts", ColumnType::kTimestamp2, ColumnType::kTimestamp2},
      {"y", ColumnType::kYear, ColumnType::kYear},
      {"e", ColumnType::kString, ColumnType::kEnum},
      {"st", ColumnType::kString, ColumnType::kSet},
      {"bt", ColumnType::kBit, ColumnType::kBit},
      {"j", mariadb ? ColumnType::kBlob : ColumnType::kJson,
       mariadb ? ColumnType::kBlob : ColumnType::kJson},
      {"n", ColumnType::kLong, ColumnType::kLong},
  };
  if (mariadb) columns.push_back({"cz", ColumnType::kBlobCompressed, ColumnType::kBlobCompressed});
  return columns;
}

size_t IndexOf(const TableMap& map, const std::string& name) {
  for (size_t i = 0; i < map.columns.size(); ++i) {
    if (map.columns[i].name == name) return i;
  }
  return map.columns.size();
}

void ExpectFxTable(const TableMap& map, ServerFlavor flavor) {
  const std::vector<ExpectedColumn> expected = ExpectedColumns(flavor);
  ASSERT_EQ(map.columns.size(), expected.size());
  EXPECT_TRUE(map.has_column_names);
  EXPECT_TRUE(map.has_signedness);
  EXPECT_TRUE(map.has_enum_labels);
  EXPECT_TRUE(map.has_set_labels);
  for (size_t i = 0; i < expected.size(); ++i) {
    SCOPED_TRACE(expected[i].name);
    const TableMapColumn& column = map.columns[i];
    EXPECT_EQ(column.name, expected[i].name);
    EXPECT_EQ(column.type, expected[i].type);
    EXPECT_EQ(RealType(column), expected[i].real_type);
    EXPECT_EQ(column.nullable, i != 0);  // only the primary key is NOT NULL
    // u32 is INT UNSIGNED; YEAR is unsigned on both servers.
    const bool is_unsigned = column.name == "u32" || column.name == "y";
    EXPECT_EQ(column.is_unsigned, is_unsigned);
  }

  const TableMapColumn& dec2 = map.columns[IndexOf(map, "dec2")];
  EXPECT_EQ(dec2.meta, (30u << 8) | 10u);
  EXPECT_EQ(map.columns[IndexOf(map, "s")].meta, 32u * 4);  // VARCHAR(32) utf8mb4
  EXPECT_EQ(map.columns[IndexOf(map, "dt")].meta, 3u);
  EXPECT_EQ(map.columns[IndexOf(map, "tm")].meta, 6u);
  EXPECT_EQ(map.columns[IndexOf(map, "bt")].meta, 0x0102u);  // BIT(10): 1 byte + 2 bits
  EXPECT_EQ(map.columns[IndexOf(map, "j")].meta, 4u);

  const TableMapColumn& s = map.columns[IndexOf(map, "s")];
  EXPECT_TRUE(s.has_charset);
  EXPECT_NE(s.charset, kBinaryCollation);
  for (const char* binary : {"b", "vb"}) {
    SCOPED_TRACE(binary);
    const TableMapColumn& column = map.columns[IndexOf(map, binary)];
    EXPECT_TRUE(column.has_charset);
    EXPECT_EQ(column.charset, kBinaryCollation);
  }
  const TableMapColumn& j = map.columns[IndexOf(map, "j")];
  if (flavor == ServerFlavor::kMariaDB) {
    EXPECT_TRUE(j.has_charset);
    EXPECT_NE(j.charset, kBinaryCollation);
    const TableMapColumn& cz = map.columns[IndexOf(map, "cz")];
    EXPECT_TRUE(cz.has_charset);
    EXPECT_EQ(cz.charset, kBinaryCollation);
  } else {
    EXPECT_FALSE(j.has_charset);
  }

  const TableMapColumn& e = map.columns[IndexOf(map, "e")];
  const TableMapColumn& st = map.columns[IndexOf(map, "st")];
  EXPECT_EQ(e.labels, (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_EQ(st.labels, (std::vector<std::string>{"x", "y", "z"}));
  EXPECT_TRUE(e.has_charset);
  EXPECT_TRUE(st.has_charset);
  EXPECT_TRUE(map.columns[IndexOf(map, "id")].labels.empty());
}

TEST(CodecTableMapFixtures, ParsesEveryTableMapAndQueryEvent) {
  for (const FixtureCase& fixture : kFixtures) {
    SCOPED_TRACE(fixture.file);
    Bytes file;
    ASSERT_TRUE(ReadFixture(fixture.file, &file));
    ASSERT_GE(file.size(), kBinlogMagicOffset);

    ChecksumTracker tracker;
    size_t pos = kBinlogMagicOffset;
    size_t table_maps = 0;
    size_t queries = 0;
    bool saw_create_table = false;
    while (pos < file.size()) {
      SCOPED_TRACE(testing::Message() << "event at " << pos);
      EventHeader header;
      ASSERT_EQ(ParseEventHeader(file.data() + pos, file.size() - pos, &header), Status::kOk);
      ASSERT_GE(header.event_length, kEventHeaderSize);
      ASSERT_LE(header.event_length, file.size() - pos);
      FrameLayout layout;
      ASSERT_EQ(tracker.OnEvent(header, file.data() + pos, header.event_length, &layout),
                Status::kOk);
      const uint8_t* body = file.data() + pos + kEventHeaderSize;

      if (header.type_code == static_cast<uint8_t>(EventType::kTableMapEvent)) {
        TableMap map;
        ASSERT_EQ(ParseTableMapEvent(header, body, layout.body_length(), fixture.flavor, &map),
                  Status::kOk);
        EXPECT_EQ(map.schema, "fx");
        EXPECT_EQ(map.table, "t");
        EXPECT_GT(map.table_id, 0u);
        ExpectFxTable(map, fixture.flavor);
        ++table_maps;
      } else if (header.type_code == static_cast<uint8_t>(EventType::kQueryEvent)) {
        QueryEvent query;
        ASSERT_EQ(ParseQueryEvent(header, body, layout.body_length(), &query), Status::kOk);
        EXPECT_TRUE(query.status_vars_complete) << query.query;
        EXPECT_TRUE(query.has_sql_mode) << query.query;
        EXPECT_TRUE(query.has_charset) << query.query;
        EXPECT_EQ(query.error_code, 0u);
        EXPECT_FALSE(query.query.empty());
        if (query.query.rfind("CREATE TABLE t", 0) == 0) {
          saw_create_table = true;
          EXPECT_EQ(query.schema, "fx");
          EXPECT_EQ(query.sql_mode, 0u);  // SET SESSION sql_mode=''
        }
        ++queries;
      }
      pos += header.event_length;
    }
    EXPECT_EQ(pos, file.size());
    EXPECT_GE(table_maps, 4u);  // one per DML transaction of the workload
    EXPECT_GE(queries, 2u);     // CREATE DATABASE, CREATE TABLE
    EXPECT_TRUE(saw_create_table);
  }
}

}  // namespace
}  // namespace mybinlog
