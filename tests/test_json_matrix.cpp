// JSON column text against the server's own CAST(j AS CHAR): the FULL-image,
// uncompressed MySQL configurations under fixtures/matrix are replayed by
// primary key for t_json and t_big, and after every transaction that touched
// a table its JSON column must match the next snapshot in expected.jsonl.gz.

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
#include <vector>

#include "bytes_internal.h"
#include "mybinlog/checksum.h"
#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/json.h"
#include "mybinlog/row_decoder.h"
#include "mybinlog/server_flavor.h"
#include "mybinlog/table_map.h"
#include "mybinlog/value.h"

namespace mybinlog {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
// id -> JSON text, or nullopt for SQL NULL.
using TableState = std::map<int64_t, std::optional<std::string>>;

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

// ---- Minimal JSON reader for expected.jsonl -------------------------------

struct Node {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject } type = Type::kNull;
  std::string text;  // kNumber: literal; kString: decoded UTF-8
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

  bool Parse(Node* out) {
    if (!ParseValue(out)) return false;
    Skip();
    return pos_ == s_.size();
  }

 private:
  void Skip() {
    while (pos_ < s_.size() &&
           (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t')) {
      ++pos_;
    }
  }

  bool Literal(const char* word) {
    const std::string w(word);
    if (s_.compare(pos_, w.size(), w) != 0) return false;
    pos_ += w.size();
    return true;
  }

  static void AppendUtf8(uint32_t cp, std::string* out) {
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

  bool Hex4(uint32_t* cp) {
    if (pos_ + 4 > s_.size()) return false;
    *cp = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s_[pos_++];
      const int digit = c >= '0' && c <= '9'   ? c - '0'
                        : c >= 'a' && c <= 'f' ? c - 'a' + 10
                        : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                               : -1;
      if (digit < 0) return false;
      *cp = *cp * 16 + static_cast<uint32_t>(digit);
    }
    return true;
  }

  bool String(std::string* out) {
    if (s_[pos_] != '"') return false;
    ++pos_;
    while (pos_ < s_.size() && s_[pos_] != '"') {
      const char c = s_[pos_++];
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos_ >= s_.size()) return false;
      const char e = s_[pos_++];
      switch (e) {
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'u': {
          uint32_t cp = 0;
          if (!Hex4(&cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00) {
            uint32_t low = 0;
            if (!Literal("\\u") || !Hex4(&low)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          }
          AppendUtf8(cp, out);
          break;
        }
        default:
          out->push_back(e);
          break;
      }
    }
    if (pos_ >= s_.size()) return false;
    ++pos_;
    return true;
  }

  bool ParseValue(Node* out) {
    Skip();
    if (pos_ >= s_.size()) return false;
    const char c = s_[pos_];
    if (c == '{') {
      out->type = Node::Type::kObject;
      ++pos_;
      Skip();
      if (s_[pos_] == '}') return ++pos_, true;
      while (true) {
        Skip();
        std::pair<std::string, Node> member;
        if (!String(&member.first)) return false;
        Skip();
        if (s_[pos_++] != ':') return false;
        if (!ParseValue(&member.second)) return false;
        out->members.push_back(std::move(member));
        Skip();
        if (s_[pos_] == ',') {
          ++pos_;
          continue;
        }
        return s_[pos_++] == '}';
      }
    }
    if (c == '[') {
      out->type = Node::Type::kArray;
      ++pos_;
      Skip();
      if (s_[pos_] == ']') return ++pos_, true;
      while (true) {
        out->items.emplace_back();
        if (!ParseValue(&out->items.back())) return false;
        Skip();
        if (s_[pos_] == ',') {
          ++pos_;
          continue;
        }
        return s_[pos_++] == ']';
      }
    }
    if (c == '"') {
      out->type = Node::Type::kString;
      return String(&out->text);
    }
    if (Literal("null")) return out->type = Node::Type::kNull, true;
    if (Literal("true")) return out->type = Node::Type::kBool, out->text = "true", true;
    if (Literal("false")) return out->type = Node::Type::kBool, out->text = "false", true;
    out->type = Node::Type::kNumber;
    const size_t start = pos_;
    while (pos_ < s_.size() && std::string("+-0123456789.eE").find(s_[pos_]) != std::string::npos) {
      ++pos_;
    }
    out->text = s_.substr(start, pos_ - start);
    return pos_ > start;
  }

  const std::string& s_;
  size_t pos_ = 0;
};

// Snapshots of each JSON table, in transaction order.
bool ReadSnapshots(const fs::path& path, std::map<std::string, std::vector<TableState>>* out) {
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
    TableState state;
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

// Column 0 is the primary key `id`; the JSON column is the last one.
void ApplyRow(const Row& row, bool remove, TableState* state) {
  ASSERT_GE(row.size(), 2u);
  ASSERT_EQ(row.front().kind, Value::Kind::kInt);
  const int64_t id = row.front().i;
  if (remove) {
    state->erase(id);
    return;
  }
  const Value& j = row.back();
  if (j.kind == Value::Kind::kNull) {
    (*state)[id] = std::nullopt;
    return;
  }
  ASSERT_TRUE(j.kind == Value::Kind::kJson || j.kind == Value::Kind::kBytes)
      << ValueKindName(j.kind);
  std::string text;
  ASSERT_EQ(
      JsonBinaryToText(reinterpret_cast<const uint8_t*>(j.bytes.data()), j.bytes.size(), &text),
      Status::kOk)
      << "id " << id;
  (*state)[id] = text;
}

struct Replay {
  std::map<std::string, TableState> state;
  std::map<std::string, size_t> next_snapshot;
  std::set<std::string> touched;  // tables changed by the open transaction
  size_t compared = 0;
};

void Commit(const std::map<std::string, std::vector<TableState>>& expected, Replay* replay) {
  for (const std::string& table : replay->touched) {
    SCOPED_TRACE(table);
    const auto snapshots = expected.find(table);
    ASSERT_NE(snapshots, expected.end());
    size_t& next = replay->next_snapshot[table];
    ASSERT_LT(next, snapshots->second.size()) << "more transactions than snapshots";
    const TableState& want = snapshots->second[next++];
    const TableState& got = replay->state[table];
    ASSERT_EQ(got.size(), want.size()) << "snapshot " << next;
    for (const auto& [id, text] : want) {
      const auto it = got.find(id);
      ASSERT_NE(it, got.end()) << "id " << id;
      EXPECT_EQ(it->second, text) << "id " << id << ", snapshot " << next;
      ++replay->compared;
    }
  }
  replay->touched.clear();
}

void ReplayFile(const Bytes& file, const std::map<std::string, std::vector<TableState>>& expected,
                Replay* replay) {
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
        RowsEvent data;
        ASSERT_EQ(DecodeRowsEvent(header, body, layout.body_length(), table->second,
                                  DecodeOptions(), &data),
                  Status::kOk);
        TableState* state = &replay->state[name];
        const bool remove =
            type == EventType::kDeleteRowsEvent || type == EventType::kDeleteRowsEventV1;
        for (const Row& row : data.rows) ApplyRow(row, remove, state);
        for (const auto& update : data.updates) {
          ApplyRow(update.first, true, state);
          ApplyRow(update.second, false, state);
        }
        replay->touched.insert(name);
      }
    } else if (type == EventType::kXidEvent) {
      Commit(expected, replay);
    }
    if (testing::Test::HasFatalFailure()) return;
    pos += header.event_length;
  }
}

std::vector<fs::path> FullImageConfigs() {
  const fs::path root = fs::path(__FILE__).parent_path() / "fixtures" / "matrix";
  std::vector<fs::path> out;
  std::error_code ec;
  for (fs::directory_iterator v(root, ec), end; !ec && v != end; v.increment(ec)) {
    if (v->path().filename().string().rfind("mysql-", 0) != 0) continue;
    std::error_code inner;
    for (fs::directory_iterator c(v->path(), inner); !inner && c != end; c.increment(inner)) {
      const std::string name = c->path().filename().string();
      if (name.find("_full_") != std::string::npos && name.find("_nozstd_") != std::string::npos &&
          name.find("_nopjson") != std::string::npos) {
        out.push_back(c->path());
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

TEST(CodecJsonMatrix, ServerCastText) {
  const std::vector<fs::path> configs = FullImageConfigs();
  if (configs.empty()) GTEST_SKIP() << "no FULL-image MySQL matrix fixtures";
  for (const fs::path& config : configs) {
    SCOPED_TRACE(config.parent_path().filename().string() + "/" + config.filename().string());
    std::map<std::string, std::vector<TableState>> expected;
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
    }
    for (const auto& [table, snapshots] : expected) {
      EXPECT_EQ(replay.next_snapshot[table], snapshots.size()) << table << ": snapshots left over";
    }
    EXPECT_GT(replay.compared, 20u);
  }
}

}  // namespace
}  // namespace mybinlog
