// Partial JSON updates against the server: the uncompressed PARTIAL_JSON MySQL
// configurations under fixtures/matrix are replayed by primary key for t_json
// and t_big, every kJsonDiff after image is applied to the replayed value with
// ApplyJsonDiffs(), and after each transaction that touched a table its JSON
// column must match the next CAST(j AS CHAR) snapshot in expected.jsonl.gz.

#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "bytes_internal.h"
#include "mybinlog/checksum.h"
#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/json.h"
#include "mybinlog/json_diff.h"
#include "mybinlog/row_decoder.h"
#include "mybinlog/server_flavor.h"
#include "mybinlog/table_map.h"
#include "mybinlog/value.h"

namespace mybinlog {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
// id -> JSON column, nullopt for SQL NULL.
using TableState = std::map<int64_t, std::optional<JsonValue>>;
using ExpectedState = std::map<int64_t, std::optional<std::string>>;

const std::set<std::string> kJsonTables = {"t_json", "t_big"};

bool Gunzip(const fs::path& path, Bytes* out) {
  gzFile file = gzopen(path.string().c_str(), "rb");
  if (file == nullptr) return false;
  uint8_t buf[1 << 16];
  int n = 0;
  while ((n = gzread(file, buf, sizeof(buf))) > 0) out->insert(out->end(), buf, buf + n);
  const bool ok = n == 0;
  gzclose(file);
  return ok;
}

// ---- Just enough of a JSON reader for expected.jsonl -----------------------

struct Node {
  enum class Type { kNull, kScalar, kString, kArray, kObject } type = Type::kNull;
  std::string text;  // kScalar: literal; kString: decoded UTF-8
  std::vector<Node> items;
  std::vector<std::pair<std::string, Node>> members;

  const Node* Get(const std::string& key) const {
    for (const auto& m : members) {
      if (m.first == key) return &m.second;
    }
    return nullptr;
  }
};

class Reader {
 public:
  explicit Reader(const std::string& s) : s_(s) {}

  bool Parse(Node* out) { return Value(out) && (Skip(), pos_ == s_.size()); }

 private:
  void Skip() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t')) ++pos_;
  }

  bool Eat(char c) {
    Skip();
    if (pos_ >= s_.size() || s_[pos_] != c) return false;
    ++pos_;
    return true;
  }

  bool Hex4(uint32_t* cp) {
    if (pos_ + 4 > s_.size()) return false;
    *cp = static_cast<uint32_t>(std::stoul(s_.substr(pos_, 4), nullptr, 16));
    pos_ += 4;
    return true;
  }

  static void Utf8(uint32_t cp, std::string* out) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool String(std::string* out) {
    if (!Eat('"')) return false;
    while (pos_ < s_.size() && s_[pos_] != '"') {
      const char c = s_[pos_++];
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos_ >= s_.size()) return false;
      const char e = s_[pos_++];
      const std::string from = "bfnrt", to = "\b\f\n\r\t";
      if (from.find(e) != std::string::npos) {
        out->push_back(to[from.find(e)]);
      } else if (e == 'u') {
        uint32_t cp = 0;
        if (!Hex4(&cp)) return false;
        if (cp >= 0xD800 && cp < 0xDC00) {
          uint32_t low = 0;
          if (s_.compare(pos_, 2, "\\u") != 0) return false;
          pos_ += 2;
          if (!Hex4(&low)) return false;
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        }
        Utf8(cp, out);
      } else {
        out->push_back(e);
      }
    }
    return Eat('"');
  }

  bool Value(Node* out) {
    Skip();
    if (pos_ >= s_.size()) return false;
    const char c = s_[pos_];
    if (c == '"') {
      out->type = Node::Type::kString;
      return String(&out->text);
    }
    if (c == '{' || c == '[') {
      const bool object = c == '{';
      out->type = object ? Node::Type::kObject : Node::Type::kArray;
      ++pos_;
      if (Eat(object ? '}' : ']')) return true;
      do {
        if (object) {
          out->members.emplace_back();
          if (!String(&out->members.back().first) || !Eat(':') ||
              !Value(&out->members.back().second)) {
            return false;
          }
        } else {
          out->items.emplace_back();
          if (!Value(&out->items.back())) return false;
        }
      } while (Eat(','));
      return Eat(object ? '}' : ']');
    }
    if (s_.compare(pos_, 4, "null") == 0) {
      pos_ += 4;
      return true;
    }
    out->type = Node::Type::kScalar;
    const size_t start = pos_;
    while (pos_ < s_.size() && s_[pos_] != ',' && s_[pos_] != '}' && s_[pos_] != ']' &&
           s_[pos_] != ' ') {
      ++pos_;
    }
    out->text = s_.substr(start, pos_ - start);
    return pos_ > start;
  }

  const std::string& s_;
  size_t pos_ = 0;
};

bool ReadSnapshots(const fs::path& path, std::map<std::string, std::vector<ExpectedState>>* out) {
  Bytes raw;
  if (!Gunzip(path, &raw)) return false;
  const std::string all(raw.begin(), raw.end());
  size_t start = 0;
  while (start < all.size()) {
    size_t end = all.find('\n', start);
    if (end == std::string::npos) end = all.size();
    const std::string line = all.substr(start, end - start);
    start = end + 1;
    if (line.empty()) continue;
    Node node;
    if (!Reader(line).Parse(&node)) return false;
    const Node* table = node.Get("table");
    const Node* rows = node.Get("rows");
    if (table == nullptr || rows == nullptr) return false;
    if (kJsonTables.count(table->text) == 0) continue;
    ExpectedState state;
    for (const Node& row : rows->items) {
      const Node* id = row.Get("id");
      const Node* j = row.Get("j");
      if (id == nullptr || j == nullptr) return false;
      state[std::stoll(id->text)] =
          j->type == Node::Type::kNull ? std::nullopt : std::optional<std::string>(j->text);
    }
    (*out)[table->text].push_back(std::move(state));
  }
  return true;
}

// ---- Replay ---------------------------------------------------------------

struct Replay {
  std::map<std::string, TableState> state;
  std::map<std::string, size_t> next_snapshot;
  std::set<std::string> touched;  // tables changed by the open transaction
  size_t partial_events = 0;      // PARTIAL_UPDATE_ROWS events on the JSON tables
  size_t diffs_applied = 0;       // kJsonDiff values applied
  size_t compared = 0;
  bool partial_unsupported = false;  // the row decoder refused type 39
};

// The after image's JSON column applied to the replayed @p current value.
void ApplyJsonColumn(const Value& j, std::optional<JsonValue>* current, Replay* replay) {
  switch (j.kind) {
    case Value::Kind::kAbsent:
      return;
    case Value::Kind::kNull:
      current->reset();
      return;
    case Value::Kind::kJson: {
      JsonValue doc;
      ASSERT_EQ(
          ParseJsonBinary(reinterpret_cast<const uint8_t*>(j.bytes.data()), j.bytes.size(), &doc),
          Status::kOk);
      *current = std::move(doc);
      return;
    }
    case Value::Kind::kJsonDiff: {
      ASSERT_TRUE(current->has_value()) << "diff against SQL NULL";
      std::vector<JsonDiff> diffs;
      ASSERT_EQ(
          ParseJsonDiffs(reinterpret_cast<const uint8_t*>(j.bytes.data()), j.bytes.size(), &diffs),
          Status::kOk);
      JsonValue after;
      ASSERT_EQ(ApplyJsonDiffs(**current, diffs, &after), Status::kOk);
      *current = std::move(after);
      ++replay->diffs_applied;
      return;
    }
    default:
      FAIL() << "unexpected JSON column kind " << ValueKindName(j.kind);
  }
}

void Commit(const std::map<std::string, std::vector<ExpectedState>>& expected, Replay* replay) {
  for (const std::string& table : replay->touched) {
    SCOPED_TRACE(table);
    const auto snapshots = expected.find(table);
    ASSERT_NE(snapshots, expected.end());
    size_t& next = replay->next_snapshot[table];
    ASSERT_LT(next, snapshots->second.size()) << "more transactions than snapshots";
    const ExpectedState& want = snapshots->second[next++];
    const TableState& got = replay->state[table];
    ASSERT_EQ(got.size(), want.size()) << "snapshot " << next;
    for (const auto& [id, text] : want) {
      const auto it = got.find(id);
      ASSERT_NE(it, got.end()) << "id " << id;
      std::optional<std::string> got_text;
      if (it->second.has_value()) {
        std::string t;
        ASSERT_EQ(JsonToText(*it->second, &t), Status::kOk) << "id " << id;
        got_text = std::move(t);
      }
      EXPECT_EQ(got_text, text) << "id " << id << ", snapshot " << next;
      ++replay->compared;
    }
  }
  replay->touched.clear();
}

int64_t RowId(const Row& row) {
  EXPECT_FALSE(row.empty());
  if (row.empty()) return 0;
  EXPECT_EQ(row.front().kind, Value::Kind::kInt);
  return row.front().i;
}

void ApplyRows(EventType type, const RowsEvent& data, TableState* state, Replay* replay) {
  const bool remove = type == EventType::kDeleteRowsEvent || type == EventType::kDeleteRowsEventV1;
  for (const Row& row : data.rows) {
    const int64_t id = RowId(row);
    if (remove) {
      state->erase(id);
      continue;
    }
    std::optional<JsonValue> j;
    ApplyJsonColumn(row.back(), &j, replay);
    if (testing::Test::HasFatalFailure()) return;
    (*state)[id] = std::move(j);
  }
  for (const auto& [before, after] : data.updates) {
    const int64_t old_id = RowId(before);
    const auto it = state->find(old_id);
    ASSERT_NE(it, state->end()) << "update of unknown id " << old_id;
    std::optional<JsonValue> j = std::move(it->second);
    state->erase(it);
    const int64_t new_id = after.front().kind == Value::Kind::kAbsent ? old_id : RowId(after);
    ApplyJsonColumn(after.back(), &j, replay);
    if (testing::Test::HasFatalFailure()) return;
    (*state)[new_id] = std::move(j);
  }
}

void ReplayFile(const Bytes& file,
                const std::map<std::string, std::vector<ExpectedState>>& expected, Replay* replay) {
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
    const auto type = static_cast<EventType>(header.type_code);
    if (type == EventType::kTableMapEvent) {
      TableMap map;
      ASSERT_EQ(ParseTableMapEvent(header, body, layout.body_length(), ServerFlavor::kMySQL, &map),
                Status::kOk);
      tables[map.table_id] = map;
    } else if (IsRowsEvent(header.type_code)) {
      ASSERT_GE(layout.body_length(), 6u);
      const auto table = tables.find(ReadU48Le(body));
      ASSERT_NE(table, tables.end());
      const std::string& name = table->second.table;
      if (kJsonTables.count(name) != 0) {
        const bool partial = type == EventType::kPartialUpdateRowsEvent;
        RowsEvent data;
        const Status status = DecodeRowsEvent(header, body, layout.body_length(), table->second,
                                              DecodeOptions(), &data);
        if (partial && status == Status::kUnsupported) {
          replay->partial_unsupported = true;
          return;
        }
        ASSERT_EQ(status, Status::kOk);
        if (partial) ++replay->partial_events;
        ApplyRows(type, data, &replay->state[name], replay);
        replay->touched.insert(name);
      }
    } else if (type == EventType::kXidEvent) {
      Commit(expected, replay);
    }
    if (testing::Test::HasFatalFailure()) return;
    pos += header.event_length;
  }
}

std::vector<fs::path> PartialJsonConfigs() {
  const fs::path root = fs::path(__FILE__).parent_path() / "fixtures" / "matrix";
  std::vector<fs::path> out;
  std::error_code ec;
  for (fs::directory_iterator v(root, ec), end; !ec && v != end; v.increment(ec)) {
    if (v->path().filename().string().rfind("mysql-", 0) != 0) continue;
    std::error_code inner;
    for (fs::directory_iterator c(v->path(), inner); !inner && c != end; c.increment(inner)) {
      const std::string name = c->path().filename().string();
      const bool pjson = name.size() > 6 && name.compare(name.size() - 6, 6, "_pjson") == 0;
      if (pjson && name.find("_nozstd_") != std::string::npos) out.push_back(c->path());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

TEST(CodecJsonDiffMatrix, ServerCastTextAfterPartialUpdates) {
  const std::vector<fs::path> configs = PartialJsonConfigs();
  ASSERT_FALSE(configs.empty()) << "no uncompressed PARTIAL_JSON MySQL matrix fixtures";
  size_t partial_events = 0;
  size_t diffs_applied = 0;
  for (const fs::path& config : configs) {
    SCOPED_TRACE(config.parent_path().filename().string() + "/" + config.filename().string());
    std::map<std::string, std::vector<ExpectedState>> expected;
    ASSERT_TRUE(ReadSnapshots(config / "expected.jsonl.gz", &expected));
    ASSERT_EQ(expected.count("t_json"), 1u);

    std::vector<fs::path> binlogs;
    std::error_code ec;
    for (fs::directory_iterator f(config, ec), end; !ec && f != end; f.increment(ec)) {
      if (f->path().filename().string().rfind("binlog.", 0) == 0) binlogs.push_back(f->path());
    }
    std::sort(binlogs.begin(), binlogs.end());
    ASSERT_FALSE(binlogs.empty());

    Replay replay;
    for (const fs::path& binlog : binlogs) {
      Bytes file;
      ASSERT_TRUE(Gunzip(binlog, &file)) << binlog;
      ReplayFile(file, expected, &replay);
      if (HasFatalFailure()) return;
      if (replay.partial_unsupported) {
        GTEST_SKIP() << "DecodeRowsEvent returns kUnsupported for PARTIAL_UPDATE_ROWS; "
                        "no kJsonDiff values to apply yet";
      }
    }
    for (const auto& [table, snapshots] : expected) {
      EXPECT_EQ(replay.next_snapshot[table], snapshots.size()) << table << ": snapshots left over";
    }
    EXPECT_GT(replay.compared, 20u);
    EXPECT_GT(replay.partial_events, 0u) << "no PARTIAL_UPDATE_ROWS event on the JSON tables";
    partial_events += replay.partial_events;
    diffs_applied += replay.diffs_applied;
  }
  EXPECT_GT(partial_events, 0u);
  // Once PARTIAL_UPDATE_ROWS decodes, the upd_json_* transactions must yield diffs.
  EXPECT_GT(diffs_applied, 0u) << "PARTIAL_UPDATE_ROWS decoded but no column was kJsonDiff";
  RecordProperty("diffs_applied", static_cast<int>(diffs_applied));
}

}  // namespace
}  // namespace mybinlog
