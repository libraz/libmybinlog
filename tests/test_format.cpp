// FormatValue against the server's own text: hand-transcribed cases (FLOAT and
// DOUBLE from `SELECT CAST(x AS FLOAT)` / `SELECT x` on MySQL 9.6), and the
// DECIMAL, temporal and DOUBLE columns of every FULL-metadata matrix
// configuration against expected.jsonl.gz, whose DECIMAL and temporal
// normalization is `CAST(col AS CHAR)`.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "matrix_replay.h"
#include "mybinlog/decoder.h"
#include "mybinlog/event.h"
#include "mybinlog/format.h"
#include "mybinlog/status.h"
#include "mybinlog/table_map.h"
#include "mybinlog/value.h"

namespace mybinlog {
namespace {

namespace fs = std::filesystem;
using matrix::Bytes;
using Kind = Value::Kind;

std::string Format(const Value& value) {
  std::string out;
  const Status status = FormatValue(value, &out);
  if (status != Status::kOk) return std::string("<") + StatusName(status) + ">";
  return out;
}

Value Int(int64_t i) {
  Value v;
  v.kind = Kind::kInt;
  v.i = i;
  return v;
}

Value UInt(uint64_t u, Kind kind = Kind::kUInt) {
  Value v;
  v.kind = kind;
  v.u = u;
  return v;
}

Value Real(double d, ColumnType type) {
  Value v;
  v.kind = Kind::kDouble;
  v.type = type;
  v.d = d;
  return v;
}

Value Temporal(Kind kind, uint16_t y, uint8_t mo, uint8_t d, uint16_t h, uint8_t mi, uint8_t s,
               uint32_t us, uint8_t fsp, bool negative = false) {
  Value v;
  v.kind = kind;
  v.t.negative = negative;
  v.t.year = y;
  v.t.month = mo;
  v.t.day = d;
  v.t.hour = h;
  v.t.minute = mi;
  v.t.second = s;
  v.t.microsecond = us;
  v.t.fsp = fsp;
  return v;
}

Value Timestamp(int64_t seconds, uint32_t us, uint8_t fsp) {
  Value v;
  v.kind = Kind::kTimestamp;
  v.epoch_seconds = seconds;
  v.microsecond = us;
  v.fsp = fsp;
  return v;
}

Value WithBytes(Kind kind, const std::string& bytes) {
  Value v;
  v.kind = kind;
  v.bytes = bytes;
  return v;
}

TEST(CodecFormat, Integers) {
  EXPECT_EQ(Format(Int(0)), "0");
  EXPECT_EQ(Format(Int(std::numeric_limits<int64_t>::min())), "-9223372036854775808");
  EXPECT_EQ(Format(UInt(std::numeric_limits<uint64_t>::max())), "18446744073709551615");
  EXPECT_EQ(Format(UInt(1023, Kind::kBit)), "1023");
  EXPECT_EQ(Format(UInt(2, Kind::kEnum)), "2");
  EXPECT_EQ(Format(UInt(5, Kind::kSet)), "5");
}

TEST(CodecFormat, DoubleMatchesServerText) {
  const std::pair<double, const char*> cases[] = {
      {1.7976931348623157e308, "1.7976931348623157e308"},
      {5e-324, "5e-324"},
      {-0.0, "-0"},
      {0.0, "0"},
      {1e15, "1e15"},
      {1e16, "1e16"},
      {123456789012345.0, "123456789012345"},
      {123456789012345678.0, "1.2345678901234568e17"},
      {0.0001, "0.0001"},
      {0.00001, "0.00001"},
      {100.0, "100"},
      {1.5, "1.5"},
      {-2.25, "-2.25"},
      {1e-7, "0.0000001"},
      {1e-15, "0.000000000000001"},
      {1e-16, "1e-16"},
      {0.000000000000001234, "0.000000000000001234"},
      {12345678901234567890.0, "1.2345678901234567e19"},
  };
  for (const auto& c : cases) {
    EXPECT_EQ(Format(Real(c.first, ColumnType::kDouble)), c.second) << c.second;
  }
}

TEST(CodecFormat, FloatKeepsSixSignificantDigitsLikeTheServer) {
  const std::pair<float, const char*> cases[] = {
      {3.4028234663852886e38F, "3.40282e38"},
      {1.401298464324817e-45F, "1.4013e-45"},
      {1.17549435e-38F, "1.17549e-38"},
      {0.1F, "0.1"},
      {16777216.0F, "16777200"},
      {123456789.0F, "123457000"},
      {1e10F, "10000000000"},
      {1e14F, "100000000000000"},
      {1e15F, "1e15"},
      {1e-5F, "0.00001"},
      {1e-10F, "0.0000000001"},
      {1e-15F, "0.000000000000001"},
      {1e-16F, "1e-16"},
      {-1.5F, "-1.5"},
      {-0.0F, "-0"},
      {9999995.0F, "10000000"},
      {0.000123456789F, "0.000123457"},
      {-1.23456789e38F, "-1.23457e38"},
  };
  for (const auto& c : cases) {
    EXPECT_EQ(Format(Real(static_cast<double>(c.first), ColumnType::kFloat)), c.second) << c.second;
  }
}

TEST(CodecFormat, NonFiniteDoubles) {
  EXPECT_EQ(Format(Real(std::numeric_limits<double>::quiet_NaN(), ColumnType::kDouble)), "nan");
  EXPECT_EQ(Format(Real(std::numeric_limits<double>::infinity(), ColumnType::kDouble)), "inf");
  EXPECT_EQ(Format(Real(-std::numeric_limits<double>::infinity(), ColumnType::kFloat)), "-inf");
}

TEST(CodecFormat, DateDatetimeAndTime) {
  EXPECT_EQ(Format(Temporal(Kind::kDate, 2024, 1, 31, 0, 0, 0, 0, 0)), "2024-01-31");
  EXPECT_EQ(Format(Temporal(Kind::kDate, 0, 0, 0, 0, 0, 0, 0, 0)), "0000-00-00");
  EXPECT_EQ(Format(Temporal(Kind::kDate, 2024, 0, 15, 0, 0, 0, 0, 0)), "2024-00-15");
  EXPECT_EQ(Format(Temporal(Kind::kDateTime, 2024, 2, 29, 23, 59, 59, 123000, 3)),
            "2024-02-29 23:59:59.123");
  EXPECT_EQ(Format(Temporal(Kind::kDateTime, 2024, 5, 0, 12, 34, 56, 0, 0)), "2024-05-00 12:34:56");
  EXPECT_EQ(Format(Temporal(Kind::kDateTime, 9999, 12, 31, 23, 59, 59, 999999, 6)),
            "9999-12-31 23:59:59.999999");
  EXPECT_EQ(Format(Temporal(Kind::kDateTime, 1000, 1, 1, 0, 0, 0, 100000, 1)),
            "1000-01-01 00:00:00.1");
  EXPECT_EQ(Format(Temporal(Kind::kTime, 0, 0, 0, 838, 59, 59, 0, 0)), "838:59:59");
  EXPECT_EQ(Format(Temporal(Kind::kTime, 0, 0, 0, 838, 59, 59, 0, 0, true)), "-838:59:59");
  EXPECT_EQ(Format(Temporal(Kind::kTime, 0, 0, 0, 0, 0, 0, 500000, 6, true)), "-00:00:00.500000");
  EXPECT_EQ(Format(Temporal(Kind::kTime, 0, 0, 0, 12, 34, 56, 789012, 6, true)),
            "-12:34:56.789012");
  EXPECT_EQ(Format(Temporal(Kind::kTime, 0, 0, 0, 1, 2, 3, 40000, 2)), "01:02:03.04");
}

TEST(CodecFormat, TimestampIsUtc) {
  EXPECT_EQ(Format(Timestamp(1, 0, 6)), "1970-01-01 00:00:01.000000");
  EXPECT_EQ(Format(Timestamp(2147483647, 999999, 6)), "2038-01-19 03:14:07.999999");
  EXPECT_EQ(Format(Timestamp(2147483647, 0, 0)), "2038-01-19 03:14:07");
  EXPECT_EQ(Format(Timestamp(4294967295, 0, 0)), "2106-02-07 06:28:15");
  EXPECT_EQ(Format(Timestamp(1704067200, 1, 6)), "2024-01-01 00:00:00.000001");
  EXPECT_EQ(Format(Timestamp(951782400, 0, 0)), "2000-02-29 00:00:00");
  EXPECT_EQ(Format(Timestamp(0, 0, 0)), "0000-00-00 00:00:00");
  EXPECT_EQ(Format(Timestamp(0, 0, 2)), "0000-00-00 00:00:00.00");
}

TEST(CodecFormat, YearDecimalAndStrings) {
  EXPECT_EQ(Format(UInt(0, Kind::kYear)), "0000");
  EXPECT_EQ(Format(UInt(1901, Kind::kYear)), "1901");
  EXPECT_EQ(Format(WithBytes(Kind::kDecimal, "-12345678901234567890.0123456789")),
            "-12345678901234567890.0123456789");
  EXPECT_EQ(Format(WithBytes(Kind::kText, "\xC3\xBC\xE6\x97\xA5")), "\xC3\xBC\xE6\x97\xA5");
  EXPECT_EQ(Format(WithBytes(Kind::kBytes, std::string("\x00\xFF", 2))),
            std::string("\x00\xFF", 2));
}

TEST(CodecFormat, NullAndAbsentHaveNoText) {
  std::string out = "untouched";
  Value null_value;
  EXPECT_EQ(FormatValue(null_value, &out), Status::kInvalidMetadata);
  Value absent;
  absent.kind = Kind::kAbsent;
  EXPECT_EQ(FormatValue(absent, &out), Status::kInvalidMetadata);
  EXPECT_EQ(out, "untouched");
}

TEST(CodecFormat, JsonUsesTheServerText) {
  EXPECT_EQ(Format(WithBytes(Kind::kJson, std::string("\x04\x01", 2))), "true");
  EXPECT_EQ(Format(WithBytes(Kind::kJson, std::string("\x05\xFE\xFF", 3))), "-2");
  EXPECT_EQ(Format(WithBytes(Kind::kJson, std::string("\x0C\x02hi", 4))), "\"hi\"");
  EXPECT_EQ(Format(WithBytes(Kind::kJson, std::string("\x0C\x05h", 3)))[0], '<');
}

// Json_diff_vector::write_binary: 4-byte LE length, then per diff the operation,
// packed path length and path, and (except REMOVE) packed value length and value.
std::string DiffVector(const std::vector<std::string>& diffs) {
  std::string body;
  for (const std::string& d : diffs) body += d;
  std::string out;
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(body.size() >> (8 * i)));
  return out + body;
}

std::string Diff(char op, const std::string& path, const std::string& value) {
  std::string out(1, op);
  out.push_back(static_cast<char>(path.size()));
  out += path;
  if (op != 2) {
    out.push_back(static_cast<char>(value.size()));
    out += value;
  }
  return out;
}

TEST(CodecFormat, JsonDiffRendersAnArrayOfOperations) {
  const std::string bytes =
      DiffVector({Diff(0, "$.a", std::string("\x05\x05\x00", 3)),
                  Diff(1, "$.\"b c\"", std::string("\x0C\x01x", 3)), Diff(2, "$[0]", "")});
  EXPECT_EQ(Format(WithBytes(Kind::kJsonDiff, bytes)),
            "[{\"op\": \"REPLACE\", \"path\": \"$.a\", \"value\": 5}, "
            "{\"op\": \"INSERT\", \"path\": \"$.\\\"b c\\\"\", \"value\": \"x\"}, "
            "{\"op\": \"REMOVE\", \"path\": \"$[0]\"}]");
  EXPECT_EQ(Format(WithBytes(Kind::kJsonDiff, DiffVector({}))), "[]");
  EXPECT_NE(Format(WithBytes(Kind::kJsonDiff, std::string("\x09\x00\x00\x00", 4)))[0], '[');
}

TEST(CodecFormat, EnumAndSetLabels) {
  TableMapColumn column;
  column.labels = {"x", "y", "z"};
  std::string out = "untouched";
  ASSERT_EQ(FormatEnumLabel(UInt(2, Kind::kEnum), column, &out), Status::kOk);
  EXPECT_EQ(out, "y");
  ASSERT_EQ(FormatEnumLabel(UInt(0, Kind::kEnum), column, &out), Status::kOk);
  EXPECT_EQ(out, "");
  out = "untouched";
  EXPECT_EQ(FormatEnumLabel(UInt(4, Kind::kEnum), column, &out), Status::kInvalidMetadata);
  EXPECT_EQ(FormatEnumLabel(UInt(1, Kind::kSet), column, &out), Status::kInvalidMetadata);
  EXPECT_EQ(out, "untouched");

  ASSERT_EQ(FormatSetLabels(UInt(5, Kind::kSet), column, &out), Status::kOk);
  EXPECT_EQ(out, "x,z");
  ASSERT_EQ(FormatSetLabels(UInt(0, Kind::kSet), column, &out), Status::kOk);
  EXPECT_EQ(out, "");
  out = "untouched";
  EXPECT_EQ(FormatSetLabels(UInt(8, Kind::kSet), column, &out), Status::kInvalidMetadata);
  EXPECT_EQ(FormatSetLabels(UInt(1, Kind::kEnum), column, &out), Status::kInvalidMetadata);
  EXPECT_EQ(out, "untouched");
  TableMapColumn unlabelled;
  EXPECT_EQ(FormatEnumLabel(UInt(1, Kind::kEnum), unlabelled, &out), Status::kInvalidMetadata);
}

// ---- Matrix ground truth ----------------------------------------------------

std::vector<fs::path> FullMetadataConfigs() {
  std::vector<fs::path> dirs;
  for (const matrix::MatrixConfig& config : matrix::DiscoverConfigs()) {
    if (config.config.find("_metafull_") != std::string::npos) dirs.push_back(config.dir());
  }
  return dirs;
}

// Columns compared, per table: DECIMAL and temporal text, and DOUBLE as JSON numbers.
bool IsCompared(const std::string& table, const std::string& column) {
  if (table == "t_num") return column == "d" || (column.size() > 1 && column[0] == 'd');
  if (table == "t_time")
    return column == "d" || column.rfind("dt", 0) == 0 || column.rfind("tm", 0) == 0;
  return false;
}

using Expected = std::map<std::string, std::set<std::string>>;  // "table.column" -> values

// Every value of the compared columns in expected.jsonl, as its JSON token
// (strings without their quotes; the files hold no escapes in these columns).
void CollectExpected(const std::string& text, Expected* expected) {
  size_t line_start = 0;
  while (line_start < text.size()) {
    size_t line_end = text.find('\n', line_start);
    if (line_end == std::string::npos) line_end = text.size();
    const std::string line = text.substr(line_start, line_end - line_start);
    line_start = line_end + 1;
    for (const char* table : {"t_num", "t_time"}) {
      if (line.find(std::string("\"table\":\"") + table + "\"") == std::string::npos) continue;
      size_t pos = line.find("\"rows\":");
      if (pos == std::string::npos) continue;
      pos += 7;
      while ((pos = line.find('"', pos)) != std::string::npos) {
        const size_t key_end = line.find('"', pos + 1);
        if (key_end == std::string::npos) break;
        const std::string key = line.substr(pos + 1, key_end - pos - 1);
        if (line.compare(key_end + 1, 2, ": ") != 0) {
          pos = key_end + 1;
          continue;
        }
        const size_t value_start = key_end + 3;
        std::string value;
        if (line[value_start] == '"') {
          const size_t value_end = line.find('"', value_start + 1);
          value = line.substr(value_start + 1, value_end - value_start - 1);
          pos = value_end + 1;
        } else {
          const size_t value_end = line.find_first_of(",}", value_start);
          value = line.substr(value_start, value_end - value_start);
          pos = value_end;
        }
        if (IsCompared(table, key) && value != "null") {
          (*expected)[std::string(table) + "." + key].insert(value);
        }
      }
    }
  }
}

TEST(CodecFormat, MatchesServerCastTextOnTheMatrix) {
  const std::vector<fs::path> configs = FullMetadataConfigs();
  ASSERT_FALSE(configs.empty());
  size_t compared = 0;
  std::set<std::string> kinds_seen;
  for (const fs::path& dir : configs) {
    SCOPED_TRACE(dir.string());
    const bool mysql = dir.parent_path().filename().string().rfind("mysql", 0) == 0;
    Bytes expected_bytes;
    ASSERT_TRUE(matrix::ReadGzip(dir / "expected.jsonl.gz", &expected_bytes));
    Expected expected;
    CollectExpected(std::string(expected_bytes.begin(), expected_bytes.end()), &expected);
    ASSERT_FALSE(expected.empty());

    for (const fs::path& binlog : matrix::BinlogFiles(dir)) {
      Bytes file;
      ASSERT_TRUE(matrix::ReadGzip(binlog, &file));
      Decoder decoder;
      decoder.Feed(file.data(), file.size());
      Event event;
      Status status;
      while ((status = decoder.Next(&event)) == Status::kOk) {
        const auto* rows = std::get_if<TableRowsEvent>(&event.body);
        if (rows == nullptr || !rows->decoded) continue;
        const TableMap& table = *rows->table;
        std::vector<const Row*> images;
        for (const Row& row : rows->rows.rows) images.push_back(&row);
        for (const auto& update : rows->rows.updates) {
          images.push_back(&update.first);
          images.push_back(&update.second);
        }
        for (const Row* row : images) {
          for (size_t i = 0; i < row->size(); ++i) {
            const Value& value = (*row)[i];
            const std::string& name = table.columns[i].name;
            if (!IsCompared(table.table, name)) continue;
            if (value.kind == Kind::kNull || value.kind == Kind::kAbsent) continue;
            std::string text = Format(value);
            if (mysql && value.kind == Kind::kDouble &&
                text.find_first_of(".e") == std::string::npos) {
              text += ".0";  // MySQL JSON_OBJECT's spelling of a whole double
            }
            const auto column = expected.find(table.table + "." + name);
            ASSERT_NE(column, expected.end()) << name;
            EXPECT_EQ(column->second.count(text), 1u)
                << table.table << "." << name << " formatted as " << text;
            kinds_seen.insert(ValueKindName(value.kind));
            ++compared;
          }
        }
      }
      // A build without zstd stops at the first compressed payload.
      if (status == Status::kUnsupported) continue;
      ASSERT_EQ(status, Status::kNeedMoreData) << decoder.last_error().message;
    }
  }
  RecordProperty("compared", static_cast<int>(compared));
  EXPECT_GT(compared, 1000u);
  for (const char* kind : {"kDecimal", "kDate", "kDateTime", "kTime", "kDouble"}) {
    EXPECT_EQ(kinds_seen.count(kind), 1u) << kind;
  }
}

}  // namespace
}  // namespace mybinlog
