// Every version-matrix configuration replayed through mybinlog::Decoder: the
// rows events are applied by primary key to an in-memory copy of every table,
// and after each data transaction every value of every table is compared with
// the server's own snapshot in expected.jsonl.gz, normalized per column as
// tests/fixtures/matrix/README.md describes. Before images are checked against
// the replayed rows as well, and each transaction's GTID against meta.json.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "matrix_replay.h"
#include "mybinlog/compression.h"
#include "mybinlog/decoder.h"
#include "mybinlog/event.h"
#include "mybinlog/format.h"
#include "mybinlog/json.h"
#include "mybinlog/json_diff.h"
#include "mybinlog/mariadb_gtid.h"
#include "mybinlog/mysql_gtid.h"
#include "mybinlog/status.h"
#include "mybinlog/table_map.h"
#include "mybinlog/value.h"

namespace mybinlog {
namespace {

using matrix::ExpectedSnapshot;
using matrix::JsonNode;
using matrix::MatrixConfig;
using matrix::Meta;
using matrix::MetaColumn;
using matrix::MetaTable;
using matrix::MetaTransaction;
using Kind = Value::Kind;

/** Mismatches printed per configuration; the rest are only counted. */
constexpr size_t kMaxReportedMismatches = 25;

/** One replayed column value; #dom holds a JSON column once a diff was applied to it. */
struct Cell {
  Value value;  ///< kNull for SQL NULL.
  bool has_dom = false;
  JsonValue dom;
};
using StoredRow = std::vector<Cell>;
using TableState = std::map<std::string, StoredRow>;  // primary-key text -> row

// ---- Normalization ----------------------------------------------------------------

// Unicode code points of cp1252 0x80..0x9F, which MySQL's latin1 uses.
constexpr uint16_t kCp1252High[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

void AppendUtf8(uint32_t cp, std::string* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

bool ToUtf8(const std::string& charset, const std::string& in, std::string* out,
            std::string* error) {
  if (charset == "utf8mb4" || charset == "utf8mb3" || charset == "utf8" || charset == "ascii") {
    *out = in;
    return true;
  }
  if (charset == "latin1") {
    out->clear();
    for (const char c : in) {
      const auto b = static_cast<uint8_t>(c);
      AppendUtf8(b >= 0x80 && b < 0xA0 ? kCp1252High[b - 0x80] : b, out);
    }
    return true;
  }
  *error = "no conversion for charset '" + charset + "'";
  return false;
}

std::string Hex(const std::string& bytes) {
  static const char kDigits[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const char c : bytes) {
    out.push_back(kDigits[static_cast<uint8_t>(c) >> 4]);
    out.push_back(kDigits[static_cast<uint8_t>(c) & 0x0F]);
  }
  return out;
}

// The n of "binary(n)".
size_t DeclaredLength(const std::string& column_type) {
  const size_t open = column_type.find('(');
  return open == std::string::npos ? 0 : std::strtoul(column_type.c_str() + open + 1, nullptr, 10);
}

std::string Ipv4Text(const uint8_t* p) {
  return std::to_string(p[0]) + "." + std::to_string(p[1]) + "." + std::to_string(p[2]) + "." +
         std::to_string(p[3]);
}

// MariaDB Inet6::to_string: the longest run of two or more zero groups becomes
// "::", and an IPv4-compatible or IPv4-mapped address ends in dotted quad.
std::string Inet6Text(const uint8_t* p) {
  uint16_t groups[8];
  for (int i = 0; i < 8; ++i) groups[i] = static_cast<uint16_t>(p[2 * i] << 8 | p[2 * i + 1]);
  int gap_pos = -1;
  int gap_len = 0;
  for (int i = 0; i < 8;) {
    if (groups[i] != 0) {
      ++i;
      continue;
    }
    int j = i;
    while (j < 8 && groups[j] == 0) ++j;
    if (j - i > gap_len) gap_pos = i, gap_len = j - i;
    i = j;
  }
  if (gap_len < 2) gap_pos = -1;
  if (gap_pos == 0 && (gap_len == 6 || (gap_len == 5 && groups[5] == 0xFFFF))) {
    return std::string(gap_len == 6 ? "::" : "::ffff:") + Ipv4Text(p + 12);
  }
  std::string out;
  char buf[8];
  for (int i = 0; i < 8; ++i) {
    if (i == gap_pos) {
      out += "::";
      i += gap_len - 1;
      continue;
    }
    if (!out.empty() && out.back() != ':') out += ":";
    std::snprintf(buf, sizeof(buf), "%x", groups[i]);
    out += buf;
  }
  return out;
}

// MariaDB stores an RFC 4122 UUID (version 1-5, variant 10x) with its five
// groups in reverse order; any other UUID is stored as is.
std::string UuidText(const uint8_t* rec) {
  uint8_t mem[16];
  const int version = rec[8] >> 4;
  if (version >= 1 && version <= 5 && (rec[6] & 0xC0) == 0x80) {
    std::memcpy(mem, rec + 12, 4);
    std::memcpy(mem + 4, rec + 10, 2);
    std::memcpy(mem + 6, rec + 8, 2);
    std::memcpy(mem + 8, rec + 6, 2);
    std::memcpy(mem + 10, rec, 6);
  } else {
    std::memcpy(mem, rec, 16);
  }
  std::string out;
  char buf[4];
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out += "-";
    std::snprintf(buf, sizeof(buf), "%02x", mem[i]);
    out += buf;
  }
  return out;
}

bool IsNull(const Cell& cell) { return !cell.has_dom && cell.value.kind == Kind::kNull; }

std::string KindError(const Cell& cell, const MetaColumn& column) {
  return std::string("kind ") + ValueKindName(cell.value.kind) + " for norm " + column.norm;
}

/**
 * Text of a non-NULL cell in the column's normalized form. FLOAT/DOUBLE print
 * through FormatValue() and are compared numerically by the caller.
 */
bool NormText(const Cell& cell, const MetaColumn& column, std::string* out, std::string* error) {
  const Value& v = cell.value;
  const std::string& norm = column.norm;
  if (norm == "json_text") {
    Status status = Status::kOk;
    if (cell.has_dom) {
      status = JsonToText(cell.dom, out);
    } else if (v.kind == Kind::kJson) {
      status =
          JsonBinaryToText(reinterpret_cast<const uint8_t*>(v.bytes.data()), v.bytes.size(), out);
    } else {
      *error = KindError(cell, column);
      return false;
    }
    if (status != Status::kOk) *error = std::string("JSON text: ") + StatusName(status);
    return status == Status::kOk;
  }
  if (norm == "int") {
    if (v.kind == Kind::kInt) return *out = std::to_string(v.i), true;
    if (v.kind == Kind::kUInt) return *out = std::to_string(v.u), true;
  } else if (norm == "bit_uint") {
    if (v.kind == Kind::kBit) return *out = std::to_string(v.u), true;
  } else if (norm == "ordinal") {
    if (v.kind == Kind::kEnum || v.kind == Kind::kSet) return *out = std::to_string(v.u), true;
  } else if (norm == "year_int") {
    if (v.kind == Kind::kYear) return *out = std::to_string(v.u), true;
  } else if (norm == "decimal_text") {
    if (v.kind == Kind::kDecimal) {
      *out = v.bytes;
      // The server stores negative zero as zero.
      if (!out->empty() && (*out)[0] == '-' &&
          out->find_first_not_of("0.", 1) == std::string::npos) {
        out->erase(0, 1);
      }
      return true;
    }
  } else if (norm == "temporal_text" || norm == "float") {
    const bool ok = norm == "float" ? v.kind == Kind::kDouble
                                    : v.kind == Kind::kDate || v.kind == Kind::kDateTime ||
                                          v.kind == Kind::kTime;
    if (ok) {
      const Status status = FormatValue(v, out);
      if (status != Status::kOk) *error = std::string("FormatValue: ") + StatusName(status);
      return status == Status::kOk;
    }
  } else if (norm == "unix_ts_text") {
    if (v.kind == Kind::kTimestamp) {
      *out = std::to_string(v.epoch_seconds);
      if (v.fsp > 0) {
        char frac[8];
        std::snprintf(frac, sizeof(frac), "%06u", v.microsecond);
        *out += "." + std::string(frac, v.fsp > 6 ? 6 : v.fsp);
      }
      return true;
    }
  } else if (norm == "text") {
    if (v.kind == Kind::kText || v.kind == Kind::kBytes) {
      if (!ToUtf8(column.charset, v.bytes, out, error)) return false;
      if (column.data_type == "char") {
        const size_t end = out->find_last_not_of(' ');
        out->resize(end == std::string::npos ? 0 : end + 1);
      }
      return true;
    }
  } else if (norm == "hex") {
    if (v.kind == Kind::kBytes) {
      std::string bytes = v.bytes;
      if (column.data_type == "binary") {
        const size_t length = DeclaredLength(column.column_type);
        if (bytes.size() < length) bytes.resize(length, '\0');
      }
      *out = Hex(bytes);
      return true;
    }
  } else if (norm == "canonical_text") {
    if (v.kind == Kind::kBytes || v.kind == Kind::kText) {
      // Fixed-width binary values reach the binlog without their trailing zero bytes.
      const size_t want = column.data_type == "inet4" ? 4 : 16;
      if (v.bytes.size() > want) {
        *error = column.data_type + " of " + std::to_string(v.bytes.size()) + " bytes";
        return false;
      }
      std::string bytes = v.bytes;
      bytes.resize(want, '\0');
      const auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
      if (column.data_type == "inet4") return *out = Ipv4Text(p), true;
      if (column.data_type == "inet6") return *out = Inet6Text(p), true;
      if (column.data_type == "uuid") return *out = UuidText(p), true;
      *error = "no canonical form for " + column.data_type;
      return false;
    }
  } else {
    *error = "unknown norm " + norm;
    return false;
  }
  *error = KindError(cell, column);
  return false;
}

// FLOAT/DOUBLE against the expected JSON number. MySQL prints the widened
// double, so the README compares as float(value); MariaDB prints FLOAT with 6
// significant digits (as FormatValue() does) and writes negative zero as 0.
bool FloatMatches(const Value& v, const std::string& literal, ServerFlavor flavor,
                  const std::string& formatted) {
  const double want = std::strtod(literal.c_str(), nullptr);
  const bool is_float = v.type == ColumnType::kFloat;
  if (flavor == ServerFlavor::kMariaDB) {
    return is_float ? std::strtod(formatted.c_str(), nullptr) == want : v.d == want;
  }
  if (is_float) {
    const float a = static_cast<float>(want);
    const float b = static_cast<float>(v.d);
    return std::memcmp(&a, &b, sizeof(a)) == 0;
  }
  return std::memcmp(&want, &v.d, sizeof(want)) == 0;
}

std::string Abbrev(const std::string& s) {
  if (s.size() <= 72) return s;
  return s.substr(0, 56) + "...(" + std::to_string(s.size()) + " bytes)";
}

std::string Quoted(const JsonNode& node) {
  if (node.is_null()) return "NULL";
  return node.type == JsonNode::Type::kString ? "\"" + Abbrev(node.text) + "\"" : node.text;
}

// ---- Replay -----------------------------------------------------------------------

class Replayer {
 public:
  Replayer(const MatrixConfig& config, const Meta& meta,
           const std::vector<ExpectedSnapshot>& expected)
      : config_(config), meta_(meta) {
    for (size_t i = 0; i < meta.transactions.size(); ++i) {
      if (meta.transactions[i].kind == "txn") data_txns_.push_back(i);
    }
    for (const ExpectedSnapshot& snapshot : expected) by_seq_[snapshot.seq].push_back(&snapshot);
    flavor_ = config.flavor();
  }

  /** Feed one binlog file through a fresh Decoder; fatal failures stop the replay. */
  void ReplayFile(const matrix::Bytes& file) {
    Decoder decoder;
    decoder.Feed(file.data(), file.size());
    Event event;
    Status status;
    while ((status = decoder.Next(&event)) == Status::kOk) {
      OnEvent(event);
      if (testing::Test::HasFatalFailure() || aborted_) return;
    }
    if (status == Status::kUnsupported && !HasZstdSupport()) {
      zstd_unsupported_ = true;
      return;
    }
    ASSERT_EQ(status, Status::kNeedMoreData) << decoder.last_error().message;
    ASSERT_EQ(decoder.Finish(), Status::kOk) << decoder.last_error().message;
  }

  /** End-of-stream checks. */
  void Finish() {
    EXPECT_FALSE(open_ && data_) << "binlog ends inside a data transaction";
    EXPECT_EQ(next_txn_, data_txns_.size()) << "meta.json lists " << data_txns_.size()
                                            << " data transactions, binlog holds " << next_txn_;
    for (const auto& entry : by_seq_) {
      EXPECT_EQ(consumed_seqs_.count(entry.first), 1u)
          << "expected.jsonl seq " << entry.first << " matched no binlog transaction";
    }
    EXPECT_GT(compared_, 0u);
    EXPECT_EQ(mismatches_, 0u) << mismatches_ << " mismatches in " << matrix::SanitizedName(config_)
                               << ", the first " << kMaxReportedMismatches << " reported above";
  }

  bool zstd_unsupported() const { return zstd_unsupported_; }
  size_t compared() const { return compared_; }
  size_t transactions() const { return next_txn_; }
  size_t rows_events() const { return rows_events_; }
  size_t partial_events() const { return partial_events_; }
  size_t diffs() const { return diffs_; }

 private:
  void OnEvent(const Event& event) {
    if (const auto* gtid = std::get_if<GtidEvent>(&event.body)) {
      Begin();
      mysql_gtid_ = *gtid;
      has_gtid_ = true;
    } else if (const auto* gtid = std::get_if<MariaDbGtidEvent>(&event.body)) {
      Begin();
      mariadb_gtid_ = gtid->gtid;
      has_gtid_ = true;
    } else if (const auto* rows = std::get_if<TableRowsEvent>(&event.body)) {
      ApplyRows(event, *rows);
    } else if (std::get_if<XidEvent>(&event.body) != nullptr) {
      Commit();
    } else if (const auto* query = std::get_if<QueryEvent>(&event.body)) {
      if (query->query == "COMMIT") Commit();
    }
  }

  void Begin() {
    ASSERT_FALSE(open_ && data_) << "GTID inside an open data transaction";
    open_ = true;
    data_ = false;
    has_gtid_ = false;
    touched_.clear();
  }

  void Mismatch(const std::string& where, const std::string& what) {
    if (++mismatches_ <= kMaxReportedMismatches) {
      ADD_FAILURE() << config_.version << "/" << config_.config << " " << where << ": " << what;
    }
  }

  std::string Where(const std::string& table, const std::string& pk,
                    const std::string& column) const {
    // While a group is open its rows belong to the next data transaction.
    const MetaTransaction* txn = txn_;
    if (open_ && next_txn_ < data_txns_.size()) txn = &meta_.transactions[data_txns_[next_txn_]];
    std::string s = txn == nullptr ? std::string("(no txn)")
                                   : "txn " + txn->name + " seq " + std::to_string(txn->seq);
    s += " " + table;
    if (!pk.empty()) s += " pk " + pk;
    if (!column.empty()) s += " " + column;
    return s;
  }

  // Primary-key text of a stored row; false when a key column is NULL or unreadable.
  bool PkText(const MetaTable& table, const StoredRow& row, std::string* out, std::string* error) {
    out->clear();
    for (const size_t index : table.primary_key) {
      if (IsNull(row[index])) {
        *error = "primary-key column " + table.columns[index].name + " is NULL or absent";
        return false;
      }
      std::string part;
      if (!NormText(row[index], table.columns[index], &part, error)) return false;
      if (!out->empty()) out->push_back(',');
      *out += part;
    }
    return true;
  }

  static bool PkText(const MetaTable& table, const JsonNode& row, std::string* out) {
    out->clear();
    for (const size_t index : table.primary_key) {
      const JsonNode* value = row.Get(table.columns[index].name);
      if (value == nullptr || value->is_null()) return false;
      if (!out->empty()) out->push_back(',');
      *out += value->text;
    }
    return true;
  }

  // A decoded value as a cell; integers are reinterpreted as unsigned when the
  // TABLE_MAP carried no SIGNEDNESS, as a consumer must.
  static Cell ToCell(const Value& value, const MetaColumn& column, bool has_signedness) {
    Cell cell;
    cell.value = value;
    if (!has_signedness && value.kind == Kind::kInt &&
        column.column_type.find("unsigned") != std::string::npos) {
      static const std::map<std::string, int> kBits = {
          {"tinyint", 8}, {"smallint", 16}, {"mediumint", 24}, {"int", 32}, {"bigint", 64}};
      const auto bits = kBits.find(column.data_type);
      const int width = bits == kBits.end() ? 64 : bits->second;
      cell.value.kind = Kind::kUInt;
      cell.value.u = static_cast<uint64_t>(value.i);
      if (width < 64) cell.value.u &= (uint64_t{1} << width) - 1;
    }
    return cell;
  }

  // Two cells hold the same value (before-image check).
  static bool SameValue(const Cell& a, const Cell& b, const MetaColumn& column) {
    if (IsNull(a) || IsNull(b)) return IsNull(a) && IsNull(b);
    if (column.norm == "float") {
      return a.value.kind == Kind::kDouble && b.value.kind == Kind::kDouble &&
             std::memcmp(&a.value.d, &b.value.d, sizeof(double)) == 0;
    }
    std::string ta, tb, error;
    return NormText(a, column, &ta, &error) && NormText(b, column, &tb, &error) && ta == tb;
  }

  static std::string CellText(const Cell& cell, const MetaColumn& column) {
    if (IsNull(cell)) return "NULL";
    std::string text, error;
    if (!NormText(cell, column, &text, &error)) return "<" + error + ">";
    return "\"" + Abbrev(text) + "\"";
  }

  // The before image's present columns must equal the replayed row.
  void CheckBefore(const MetaTable& table, const std::string& pk, const Row& before,
                   const StoredRow& stored, bool has_signedness) {
    for (size_t i = 0; i < before.size(); ++i) {
      if (before[i].kind == Kind::kAbsent) continue;
      const Cell cell = ToCell(before[i], table.columns[i], has_signedness);
      if (!SameValue(cell, stored[i], table.columns[i])) {
        Mismatch(Where(table.name, pk, table.columns[i].name),
                 "before image " + CellText(cell, table.columns[i]) + ", replayed " +
                     CellText(stored[i], table.columns[i]));
      }
    }
  }

  // Locate a row by the before image's primary key.
  TableState::iterator Locate(const MetaTable& table, TableState* state, const Row& before,
                              bool has_signedness, std::string* pk) {
    StoredRow key_row(table.columns.size());
    for (const size_t index : table.primary_key) {
      if (before[index].kind != Kind::kAbsent) {
        key_row[index] = ToCell(before[index], table.columns[index], has_signedness);
      }
    }
    std::string error;
    if (!PkText(table, key_row, pk, &error)) {
      Mismatch(Where(table.name, "", ""), "before image: " + error);
      return state->end();
    }
    const auto it = state->find(*pk);
    if (it == state->end()) Mismatch(Where(table.name, *pk, ""), "row to change is not replayed");
    return it;
  }

  // Apply a column of an UPDATE after image to the replayed cell.
  void ApplyAfter(const MetaTable& table, const std::string& pk, size_t i, const Value& after,
                  bool has_signedness, Cell* cell) {
    if (after.kind == Kind::kAbsent) return;  // unchanged, keeps the replayed value
    if (after.kind != Kind::kJsonDiff) {
      *cell = ToCell(after, table.columns[i], has_signedness);
      return;
    }
    ++diffs_;
    const std::string where = Where(table.name, pk, table.columns[i].name);
    if (!cell->has_dom) {
      if (cell->value.kind != Kind::kJson) {
        Mismatch(where, std::string("JSON diff on a ") + ValueKindName(cell->value.kind) +
                            " replayed value");
        return;
      }
      JsonValue dom;
      const Status status =
          ParseJsonBinary(reinterpret_cast<const uint8_t*>(cell->value.bytes.data()),
                          cell->value.bytes.size(), &dom);
      if (status != Status::kOk) {
        Mismatch(where, std::string("replayed JSON: ") + StatusName(status));
        return;
      }
      cell->dom = std::move(dom);
      cell->has_dom = true;
    }
    std::vector<JsonDiff> diffs;
    Status status = ParseJsonDiffs(reinterpret_cast<const uint8_t*>(after.bytes.data()),
                                   after.bytes.size(), &diffs);
    if (status == Status::kOk) status = ApplyJsonDiffs(cell->dom, diffs, &cell->dom);
    if (status != Status::kOk) Mismatch(where, std::string("JSON diff: ") + StatusName(status));
  }

  void ApplyRows(const Event& event, const TableRowsEvent& rows) {
    ASSERT_TRUE(open_) << "rows event outside a transaction at " << event.offset;
    ASSERT_TRUE(rows.decoded && rows.table != nullptr)
        << "undecoded rows event at " << event.offset;
    const TableMap& map = *rows.table;
    ASSERT_EQ(map.schema, meta_.database) << map.table;
    const MetaTable* table = meta_.FindTable(map.table);
    ASSERT_NE(table, nullptr) << "table " << map.table << " is not in meta.json";
    ASSERT_EQ(map.columns.size(), table->columns.size()) << map.table;
    if (map.has_column_names) {
      for (size_t i = 0; i < map.columns.size(); ++i) {
        ASSERT_EQ(map.columns[i].name, table->columns[i].name) << map.table << " column " << i;
      }
    }
    data_ = true;
    touched_.insert(map.table);
    ++rows_events_;
    TableState* state = &state_[map.table];
    const bool sign = map.has_signedness;
    const auto type = static_cast<EventType>(event.header.type_code);
    std::string error;

    if (type == EventType::kWriteRowsEvent || type == EventType::kWriteRowsEventV1) {
      for (const Row& image : rows.rows.rows) {
        StoredRow row(table->columns.size());
        for (size_t i = 0; i < image.size(); ++i) {
          if (image[i].kind == Kind::kAbsent) continue;  // a column the write did not set
          if (image[i].kind == Kind::kJsonDiff) {
            Mismatch(Where(map.table, "", table->columns[i].name), "JSON diff in a write image");
            continue;
          }
          row[i] = ToCell(image[i], table->columns[i], sign);
        }
        std::string pk;
        if (!PkText(*table, row, &pk, &error)) {
          Mismatch(Where(map.table, "", ""), "write image: " + error);
          continue;
        }
        if (!state->emplace(pk, std::move(row)).second) {
          Mismatch(Where(map.table, pk, ""), "write of a primary key already replayed");
        }
      }
    } else if (type == EventType::kDeleteRowsEvent || type == EventType::kDeleteRowsEventV1) {
      for (const Row& image : rows.rows.rows) {
        std::string pk;
        const auto it = Locate(*table, state, image, sign, &pk);
        if (it == state->end()) continue;
        CheckBefore(*table, pk, image, it->second, sign);
        state->erase(it);
      }
    } else if (type == EventType::kUpdateRowsEvent || type == EventType::kUpdateRowsEventV1 ||
               type == EventType::kPartialUpdateRowsEvent) {
      if (type == EventType::kPartialUpdateRowsEvent) ++partial_events_;
      for (const auto& [before, after] : rows.rows.updates) {
        std::string pk;
        const auto it = Locate(*table, state, before, sign, &pk);
        if (it == state->end()) continue;
        CheckBefore(*table, pk, before, it->second, sign);
        StoredRow row = std::move(it->second);
        state->erase(it);
        for (size_t i = 0; i < after.size(); ++i) {
          ApplyAfter(*table, pk, i, after[i], sign, &row[i]);
        }
        std::string new_pk;
        if (!PkText(*table, row, &new_pk, &error)) {
          Mismatch(Where(map.table, pk, ""), "after image: " + error);
          continue;
        }
        if (!state->emplace(new_pk, std::move(row)).second) {
          Mismatch(Where(map.table, new_pk, ""), "update onto a primary key already replayed");
        }
      }
    } else {
      FAIL() << "unexpected rows event " << EventTypeName(event.header.type_code);
    }
  }

  // Whether the decoded GTID is the one meta.json records for transaction @p index.
  bool GtidMatches(size_t index, std::string* decoded) {
    const MetaTransaction& txn = meta_.transactions[index];
    if (!has_gtid_) {
      *decoded = "(none)";
      return false;
    }
    if (flavor_ == ServerFlavor::kMariaDB) {
      *decoded = mariadb_gtid_.ToString();
      MariaDbGtidPosition after;
      if (MariaDbGtidPosition::Parse(txn.gtid_after, &after) != Status::kOk) return false;
      for (const MariaDbGtid& gtid : after.gtids) {
        if (gtid.domain_id == mariadb_gtid_.domain_id) return gtid == mariadb_gtid_;
      }
      return false;
    }
    *decoded = mysql_gtid_.ToString();
    GtidSet single, after;
    if (single.Add(mysql_gtid_.uuid, mysql_gtid_.tag, {mysql_gtid_.gno, mysql_gtid_.gno + 1}) !=
            Status::kOk ||
        GtidSet::Parse(txn.gtid_after, &after) != Status::kOk || !single.IsSubsetOf(after)) {
      return false;
    }
    if (index == 0) return true;
    GtidSet previous;
    return GtidSet::Parse(meta_.transactions[index - 1].gtid_after, &previous) == Status::kOk &&
           !single.IsSubsetOf(previous);
  }

  void Commit() {
    const bool data = open_ && data_;
    open_ = false;
    data_ = false;
    if (!data) return;  // DDL or an empty group
    if (next_txn_ >= data_txns_.size()) {
      aborted_ = true;
      FAIL() << "binlog holds more data transactions than the " << data_txns_.size()
             << " in meta.json";
    }
    const size_t index = data_txns_[next_txn_++];
    txn_ = &meta_.transactions[index];
    std::string decoded;
    if (!GtidMatches(index, &decoded)) {
      aborted_ = true;
      FAIL() << "transaction " << next_txn_ << " (" << txn_->name << ", seq " << txn_->seq
             << ") has GTID " << decoded << ", meta.json gtid_after " << txn_->gtid_after;
    }
    consumed_seqs_.insert(txn_->seq);
    const auto snapshots = by_seq_.find(txn_->seq);
    if (snapshots != by_seq_.end()) {
      for (const ExpectedSnapshot* snapshot : snapshots->second) {
        if (snapshot->txn != txn_->name) {
          aborted_ = true;
          FAIL() << "expected.jsonl seq " << txn_->seq << " names " << snapshot->txn
                 << ", meta.json " << txn_->name;
        }
        if (touched_.count(snapshot->table) == 0) {
          aborted_ = true;
          FAIL() << txn_->name << " changed " << snapshot->table
                 << " but the binlog transaction has no rows event for it";
        }
        current_[snapshot->table] = &snapshot->rows;
      }
    }
    for (const MetaTable& table : meta_.tables) CompareTable(table);
  }

  void CompareTable(const MetaTable& table) {
    static const std::vector<JsonNode> kEmpty;
    const auto current = current_.find(table.name);
    const std::vector<JsonNode>& want = current == current_.end() ? kEmpty : *current->second;
    const TableState& got = state_[table.name];
    std::set<std::string> seen;
    for (const JsonNode& row : want) {
      std::string pk;
      if (!PkText(table, row, &pk)) {
        Mismatch(Where(table.name, "", ""), "expected row without a primary key");
        continue;
      }
      seen.insert(pk);
      const auto it = got.find(pk);
      if (it == got.end()) {
        Mismatch(Where(table.name, pk, ""), "row missing from the replay");
        continue;
      }
      for (size_t i = 0; i < table.columns.size(); ++i) {
        const MetaColumn& column = table.columns[i];
        const JsonNode* value = row.Get(column.name);
        ++compared_;
        if (value == nullptr) {
          Mismatch(Where(table.name, pk, column.name), "column missing from expected.jsonl");
          continue;
        }
        CompareCell(table, pk, column, *value, it->second[i]);
      }
    }
    for (const auto& entry : got) {
      if (seen.count(entry.first) == 0) {
        Mismatch(Where(table.name, entry.first, ""), "row not in the server's table");
      }
    }
  }

  void CompareCell(const MetaTable& table, const std::string& pk, const MetaColumn& column,
                   const JsonNode& want, const Cell& cell) {
    const std::string where = Where(table.name, pk, column.name);
    if (want.is_null() || IsNull(cell)) {
      if (!(want.is_null() && IsNull(cell))) {
        Mismatch(where, "expected " + Quoted(want) + ", actual " + CellText(cell, column));
      }
      return;
    }
    std::string text, error;
    if (!NormText(cell, column, &text, &error)) {
      Mismatch(where, "expected " + Quoted(want) + ", actual <" + error + ">");
      return;
    }
    const bool equal = column.norm == "float" ? FloatMatches(cell.value, want.text, flavor_, text)
                                              : text == want.text;
    if (equal) return;
    std::string what = "expected " + Quoted(want) + ", actual \"" + Abbrev(text) + "\"";
    if (text.size() > 72 || want.text.size() > 72) {
      size_t at = 0;
      while (at < text.size() && at < want.text.size() && text[at] == want.text[at]) ++at;
      what += " (first difference at byte " + std::to_string(at) + ")";
    }
    Mismatch(where, what);
  }

  const MatrixConfig& config_;
  const Meta& meta_;
  ServerFlavor flavor_ = ServerFlavor::kMySQL;
  std::vector<size_t> data_txns_;  // indices of meta.transactions with kind "txn"
  std::map<int, std::vector<const ExpectedSnapshot*>> by_seq_;
  std::map<std::string, const std::vector<JsonNode>*> current_;  // last snapshot per table
  std::map<std::string, TableState> state_;
  std::set<int> consumed_seqs_;

  bool open_ = false;  // a GTID opened a group not yet committed
  bool data_ = false;  // the open group holds rows events
  bool has_gtid_ = false;
  GtidEvent mysql_gtid_;
  MariaDbGtid mariadb_gtid_;
  std::set<std::string> touched_;
  const MetaTransaction* txn_ = nullptr;  // the transaction last committed
  size_t next_txn_ = 0;

  bool aborted_ = false;
  bool zstd_unsupported_ = false;
  size_t rows_events_ = 0;
  size_t partial_events_ = 0;
  size_t diffs_ = 0;
  size_t compared_ = 0;
  size_t mismatches_ = 0;
};

class Replay : public testing::TestWithParam<MatrixConfig> {};

TEST_P(Replay, All) {
  const MatrixConfig& config = GetParam();
  Meta meta;
  ASSERT_TRUE(matrix::ReadMeta(config.dir(), &meta));
  std::vector<ExpectedSnapshot> expected;
  ASSERT_TRUE(matrix::ReadExpected(config.dir() / "expected.jsonl.gz", &expected));
  ASSERT_FALSE(expected.empty());
  const std::vector<matrix::fs::path> files = matrix::BinlogFiles(config.dir());
  ASSERT_FALSE(files.empty());

  Replayer replayer(config, meta, expected);
  for (const matrix::fs::path& path : files) {
    SCOPED_TRACE(path.filename().string());
    matrix::Bytes file;
    ASSERT_TRUE(matrix::ReadGzip(path, &file));
    replayer.ReplayFile(file);
    if (HasFatalFailure()) return;
    if (replayer.zstd_unsupported()) GTEST_SKIP() << "zstd payloads need MYBINLOG_WITH_ZSTD";
  }
  replayer.Finish();
  RecordProperty("transactions", static_cast<int>(replayer.transactions()));
  RecordProperty("rows_events", static_cast<int>(replayer.rows_events()));
  RecordProperty("partial_events", static_cast<int>(replayer.partial_events()));
  RecordProperty("json_diffs", static_cast<int>(replayer.diffs()));
  RecordProperty("values_compared", static_cast<int>(replayer.compared()));
  if (config.config.size() > 6 &&
      config.config.compare(config.config.size() - 6, 6, "_pjson") == 0) {
    EXPECT_GT(replayer.diffs(), 0u) << "PARTIAL_JSON configuration applied no JSON diff";
  }
}

INSTANTIATE_TEST_SUITE_P(CodecMatrix, Replay, testing::ValuesIn(matrix::DiscoverConfigs()),
                         matrix::ConfigTestName);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(Replay);

}  // namespace
}  // namespace mybinlog
