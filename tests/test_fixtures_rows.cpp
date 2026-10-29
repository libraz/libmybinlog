// Every rows event of fx.t in the real server binlogs (fixtures/, workload in
// fixtures/capture.sh) decoded and checked against the SQL that wrote it.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "bytes_internal.h"
#include "mybinlog/checksum.h"
#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/row_decoder.h"
#include "mybinlog/server_flavor.h"
#include "mybinlog/table_map.h"
#include "mybinlog/value.h"

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

struct Decoded {
  TableMap table;  // the last TABLE_MAP of fx.t
  std::vector<Row> writes;
  std::vector<std::pair<Row, Row>> updates;
  std::vector<Row> deletes;
  size_t rows_events = 0;
};

void DecodeFixture(const FixtureCase& fixture, Decoded* out) {
  Bytes file;
  ASSERT_TRUE(ReadFixture(fixture.file, &file));
  ChecksumTracker tracker;
  std::map<uint64_t, TableMap> tables;
  size_t pos = kBinlogMagicOffset;
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
      tables[map.table_id] = map;
    } else if (IsRowsEvent(header.type_code)) {
      ASSERT_GE(layout.body_length(), 6u);
      const auto table = tables.find(ReadU48Le(body));
      ASSERT_NE(table, tables.end());
      ASSERT_EQ(table->second.table, "t");
      RowsEvent data;
      ASSERT_EQ(DecodeRowsEvent(header, body, layout.body_length(), table->second, DecodeOptions(),
                                &data),
                Status::kOk);
      const auto type = static_cast<EventType>(header.type_code);
      if (type == EventType::kWriteRowsEvent || type == EventType::kWriteRowsEventV1) {
        out->writes.insert(out->writes.end(), data.rows.begin(), data.rows.end());
      } else if (type == EventType::kDeleteRowsEvent || type == EventType::kDeleteRowsEventV1) {
        out->deletes.insert(out->deletes.end(), data.rows.begin(), data.rows.end());
      } else {
        out->updates.insert(out->updates.end(), data.updates.begin(), data.updates.end());
      }
      out->table = table->second;
      ++out->rows_events;
    }
    pos += header.event_length;
  }
  EXPECT_EQ(pos, file.size());
}

// Column lookup by name, so the same expectations serve both flavors.
class RowView {
 public:
  RowView(const TableMap& table, const Row& row) : table_(table), row_(row) {}
  const Value& operator[](const char* name) const {
    for (size_t i = 0; i < table_.columns.size(); ++i) {
      if (table_.columns[i].name == name) return row_[i];
    }
    ADD_FAILURE() << "no column " << name;
    static const Value kMissing;
    return kMissing;
  }

 private:
  const TableMap& table_;
  const Row& row_;
};

void ExpectInt(const Value& v, int64_t expected) {
  EXPECT_EQ(v.kind, Value::Kind::kInt);
  EXPECT_EQ(v.i, expected);
}

void ExpectUInt(const Value& v, Value::Kind kind, uint64_t expected) {
  EXPECT_EQ(v.kind, kind);
  EXPECT_EQ(v.u, expected);
}

void ExpectPayload(const Value& v, Value::Kind kind, const std::string& expected) {
  EXPECT_EQ(v.kind, kind) << ValueKindName(v.kind) << " instead of " << ValueKindName(kind);
  EXPECT_EQ(v.bytes, expected);
}

void ExpectTemporal(const Value& v, Value::Kind kind, bool negative, int year, int month, int day,
                    int hour, int minute, int second, uint32_t micros, int fsp) {
  ASSERT_EQ(v.kind, kind);
  EXPECT_EQ(v.t.negative, negative);
  EXPECT_EQ(v.t.year, year);
  EXPECT_EQ(v.t.month, month);
  EXPECT_EQ(v.t.day, day);
  EXPECT_EQ(v.t.hour, hour);
  EXPECT_EQ(v.t.minute, minute);
  EXPECT_EQ(v.t.second, second);
  EXPECT_EQ(v.t.microsecond, micros);
  EXPECT_EQ(v.t.fsp, fsp);
}

void ExpectTimestamp(const Value& v, int64_t epoch, uint32_t micros) {
  EXPECT_EQ(v.kind, Value::Kind::kTimestamp);
  EXPECT_EQ(v.epoch_seconds, epoch);
  EXPECT_EQ(v.microsecond, micros);
  EXPECT_EQ(v.fsp, 6);
}

void ExpectNull(const Value& v) { EXPECT_EQ(v.kind, Value::Kind::kNull); }

// MySQL stores JSON as binary JSON (first byte the value type), MariaDB as text.
void ExpectJson(const Value& v, ServerFlavor flavor, const std::string& text, uint8_t binary_type) {
  if (flavor == ServerFlavor::kMariaDB) {
    ExpectPayload(v, Value::Kind::kText, text);
    return;
  }
  ASSERT_EQ(v.kind, Value::Kind::kJson);
  ASSERT_FALSE(v.bytes.empty());
  EXPECT_EQ(static_cast<uint8_t>(v.bytes[0]), binary_type);
}

constexpr uint8_t kJsonSmallObject = 0x00;
constexpr uint8_t kJsonSmallArray = 0x02;
constexpr uint8_t kJsonLiteral = 0x04;

// Row 1 as inserted (before the UPDATE).
void ExpectRow1(const RowView& r, ServerFlavor flavor) {
  ExpectInt(r["id"], 1);
  ExpectInt(r["i8"], -128);
  ExpectUInt(r["u32"], Value::Kind::kUInt, 4294967295u);
  ExpectInt(r["i64"], std::numeric_limits<int64_t>::min());
  EXPECT_EQ(r["f"].d, 1.5);
  EXPECT_EQ(r["d"].d, -2.25);
  // The literal -0.00 is evaluated to zero before it is stored; both servers write +0.
  ExpectPayload(r["dec1"], Value::Kind::kDecimal, "0.00");
  ExpectPayload(r["dec2"], Value::Kind::kDecimal, "-12345678901234567890.0123456789");
  ExpectPayload(r["s"], Value::Kind::kText, "abc");
  ExpectPayload(r["b"], Value::Kind::kBytes, std::string("\x00\xFF", 2));
  ExpectPayload(r["vb"], Value::Kind::kBytes, std::string("\x01\x02", 2));
  ExpectTemporal(r["dt"], Value::Kind::kDateTime, false, 2024, 2, 29, 23, 59, 59, 123000, 3);
  ExpectTemporal(r["d0"], Value::Kind::kDate, false, 2024, 1, 31, 0, 0, 0, 0, 0);
  ExpectTemporal(r["tm"], Value::Kind::kTime, true, 0, 0, 0, 12, 34, 56, 789012, 6);
  ExpectTimestamp(r["ts"], 1704067200, 1);  // 2024-01-01 00:00:00.000001 UTC
  ExpectUInt(r["y"], Value::Kind::kYear, 2024);
  ExpectUInt(r["e"], Value::Kind::kEnum, 2);    // 'b'
  ExpectUInt(r["st"], Value::Kind::kSet, 5);    // 'x,z'
  ExpectUInt(r["bt"], Value::Kind::kBit, 682);  // b'1010101010'
  ExpectJson(r["j"], flavor, "{\"k\":[1,2]}", kJsonSmallObject);
  ExpectNull(r["n"]);
}

void ExpectRow2(const RowView& r, ServerFlavor flavor) {
  ExpectInt(r["id"], 2);
  ExpectInt(r["i8"], 127);
  ExpectUInt(r["u32"], Value::Kind::kUInt, 0);
  ExpectInt(r["i64"], std::numeric_limits<int64_t>::max());
  EXPECT_EQ(r["f"].d, 0.0);
  EXPECT_EQ(r["d"].d, 0.0);
  ExpectPayload(r["dec1"], Value::Kind::kDecimal, "99999999.99");
  ExpectPayload(r["dec2"], Value::Kind::kDecimal, "0.0000000000");
  ExpectPayload(r["s"], Value::Kind::kText, "");
  ExpectPayload(r["b"], Value::Kind::kBytes, "");
  ExpectPayload(r["vb"], Value::Kind::kBytes, "");
  ExpectTemporal(r["dt"], Value::Kind::kDateTime, false, 0, 0, 0, 0, 0, 0, 0, 3);
  ExpectTemporal(r["d0"], Value::Kind::kDate, false, 0, 0, 0, 0, 0, 0, 0, 0);
  ExpectTemporal(r["tm"], Value::Kind::kTime, false, 0, 0, 0, 838, 59, 59, 0, 6);
  ExpectNull(r["ts"]);
  ExpectUInt(r["y"], Value::Kind::kYear, 0);
  ExpectUInt(r["e"], Value::Kind::kEnum, 1);
  ExpectUInt(r["st"], Value::Kind::kSet, 0);
  ExpectUInt(r["bt"], Value::Kind::kBit, 0);
  if (flavor == ServerFlavor::kMariaDB) {
    ExpectPayload(r["j"], Value::Kind::kText, "null");
  } else {
    // The JSON null literal: type 0x04, then JSONB_NULL_LITERAL.
    ExpectPayload(r["j"], Value::Kind::kJson, std::string{static_cast<char>(kJsonLiteral), 0});
  }
  ExpectInt(r["n"], 7);
}

void ExpectRow3(const RowView& r, ServerFlavor flavor) {
  ExpectInt(r["id"], 3);
  ExpectInt(r["i8"], 0);
  ExpectUInt(r["u32"], Value::Kind::kUInt, 1);
  ExpectInt(r["i64"], 1);
  EXPECT_EQ(r["f"].d, -0.5);
  EXPECT_EQ(r["d"].d, 1e300);
  ExpectPayload(r["dec1"], Value::Kind::kDecimal, "0.01");
  ExpectPayload(r["dec2"], Value::Kind::kDecimal, "1.0000000000");
  ExpectPayload(r["s"], Value::Kind::kText, "\xC3\xBC\xE6\x97\xA5\xE6\x9C\xAC");  // ü日本
  ExpectPayload(r["b"], Value::Kind::kBytes, std::string(300, 'z'));
  ExpectPayload(r["vb"], Value::Kind::kBytes, "\xFF");
  // Partial-zero dates under sql_mode='': month (and day) 0 are values.
  ExpectTemporal(r["dt"], Value::Kind::kDateTime, false, 2024, 0, 15, 1, 2, 3, 4000, 3);
  ExpectTemporal(r["d0"], Value::Kind::kDate, false, 2024, 0, 0, 0, 0, 0, 0, 0);
  ExpectTemporal(r["tm"], Value::Kind::kTime, true, 0, 0, 0, 838, 59, 59, 0, 6);
  ExpectTimestamp(r["ts"], 1, 0);
  ExpectUInt(r["y"], Value::Kind::kYear, 1901);
  ExpectUInt(r["e"], Value::Kind::kEnum, 3);
  ExpectUInt(r["st"], Value::Kind::kSet, 7);
  ExpectUInt(r["bt"], Value::Kind::kBit, 1023);
  ExpectJson(r["j"], flavor, "[]", kJsonSmallArray);
  ExpectInt(r["n"], -1);
}

// A row the workload inserts with only id and one other column set.
void ExpectSparseRow(const RowView& r, const TableMap& table, int64_t id, const char* set_column) {
  ExpectInt(r["id"], id);
  for (const TableMapColumn& column : table.columns) {
    if (column.name == "id" || column.name == set_column) continue;
    SCOPED_TRACE(column.name);
    ExpectNull(r[column.name.c_str()]);
  }
}

TEST(CodecFixturesRows, DecodesEveryRowsEventOfTheWorkload) {
  for (const FixtureCase& fixture : kFixtures) {
    SCOPED_TRACE(fixture.file);
    const bool mariadb = fixture.flavor == ServerFlavor::kMariaDB;
    Decoded decoded;
    DecodeFixture(fixture, &decoded);
    if (HasFatalFailure()) return;
    const TableMap& table = decoded.table;

    // MySQL: INSERT, UPDATE, DELETE and three single-row INSERTs; MariaDB:
    // INSERT, UPDATE, DELETE and one two-row INSERT.
    EXPECT_EQ(decoded.rows_events, mariadb ? 4u : 6u);
    ASSERT_EQ(decoded.writes.size(), mariadb ? 5u : 6u);
    ASSERT_EQ(decoded.updates.size(), 1u);
    ASSERT_EQ(decoded.deletes.size(), 1u);

    {
      SCOPED_TRACE("row 1");
      ExpectRow1(RowView(table, decoded.writes[0]), fixture.flavor);
    }
    {
      SCOPED_TRACE("row 2");
      ExpectRow2(RowView(table, decoded.writes[1]), fixture.flavor);
    }
    {
      SCOPED_TRACE("row 3");
      ExpectRow3(RowView(table, decoded.writes[2]), fixture.flavor);
    }
    {
      SCOPED_TRACE("UPDATE id=1");
      ExpectRow1(RowView(table, decoded.updates[0].first), fixture.flavor);
      const RowView after(table, decoded.updates[0].second);
      ExpectInt(after["id"], 1);
      ExpectPayload(after["s"], Value::Kind::kText, "changed");
      ExpectNull(after["n"]);
      ExpectTemporal(after["tm"], Value::Kind::kTime, true, 0, 0, 0, 0, 0, 0, 500000, 6);
      ExpectPayload(after["dec2"], Value::Kind::kDecimal, "-12345678901234567890.0123456789");
    }
    {
      SCOPED_TRACE("DELETE id=2");
      ExpectRow2(RowView(table, decoded.deletes[0]), fixture.flavor);
    }

    if (mariadb) {
      ExpectNull(RowView(table, decoded.writes[0])["cz"]);
      SCOPED_TRACE("COMPRESSED inserts");
      std::string expanded;
      for (int i = 0; i < 40; ++i) expanded += "compress me ";
      const RowView row20(table, decoded.writes[3]);
      ExpectSparseRow(row20, table, 20, "cz");
      ExpectPayload(row20["cz"], Value::Kind::kBytes, expanded);
      const RowView row21(table, decoded.writes[4]);
      ExpectSparseRow(row21, table, 21, "cz");
      ExpectPayload(row21["cz"], Value::Kind::kBytes, "");
    } else {
      SCOPED_TRACE("tagged and untagged inserts");
      const char* const texts[] = {"tagged-1", "tagged-2", "untagged"};
      for (int i = 0; i < 3; ++i) {
        const RowView row(table, decoded.writes[3 + static_cast<size_t>(i)]);
        ExpectSparseRow(row, table, 10 + i, "s");
        ExpectPayload(row["s"], Value::Kind::kText, texts[i]);
      }
    }
  }
}

TEST(CodecFixturesRows, ColumnSelectionSkipsWithoutMisaligning) {
  for (const FixtureCase& fixture : kFixtures) {
    SCOPED_TRACE(fixture.file);
    Bytes file;
    ASSERT_TRUE(ReadFixture(fixture.file, &file));
    ChecksumTracker tracker;
    std::map<uint64_t, TableMap> tables;
    size_t checked = 0;
    for (size_t pos = kBinlogMagicOffset; pos < file.size();) {
      EventHeader header;
      ASSERT_EQ(ParseEventHeader(file.data() + pos, file.size() - pos, &header), Status::kOk);
      FrameLayout layout;
      ASSERT_EQ(tracker.OnEvent(header, file.data() + pos, header.event_length, &layout),
                Status::kOk);
      const uint8_t* body = file.data() + pos + kEventHeaderSize;
      if (header.type_code == static_cast<uint8_t>(EventType::kTableMapEvent)) {
        TableMap map;
        ASSERT_EQ(ParseTableMapEvent(header, body, layout.body_length(), fixture.flavor, &map),
                  Status::kOk);
        tables[map.table_id] = map;
      } else if (IsRowsEvent(header.type_code)) {
        const auto found = tables.find(ReadU48Le(body));
        ASSERT_NE(found, tables.end());
        const TableMap& table = found->second;
        // Keep only id and the last column (n or cz): everything between is measured.
        DecodeOptions options;
        options.columns.assign(table.columns.size(), false);
        options.columns.front() = true;
        options.columns.back() = true;
        RowsEvent selected;
        RowsEvent full;
        ASSERT_EQ(DecodeRowsEvent(header, body, layout.body_length(), table, options, &selected),
                  Status::kOk);
        ASSERT_EQ(
            DecodeRowsEvent(header, body, layout.body_length(), table, DecodeOptions(), &full),
            Status::kOk);
        ASSERT_EQ(selected.rows.size(), full.rows.size());
        ASSERT_EQ(selected.updates.size(), full.updates.size());
        for (size_t i = 0; i < full.rows.size(); ++i) {
          EXPECT_EQ(selected.rows[i].front().i, full.rows[i].front().i);
          EXPECT_EQ(selected.rows[i].back().kind, full.rows[i].back().kind);
          EXPECT_EQ(selected.rows[i].back().bytes, full.rows[i].back().bytes);
          EXPECT_EQ(selected.rows[i][1].kind, Value::Kind::kNull);
        }
        ++checked;
      }
      pos += header.event_length;
    }
    EXPECT_GE(checked, 4u);
  }
}

}  // namespace
}  // namespace mybinlog
