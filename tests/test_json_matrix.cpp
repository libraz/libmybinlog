// JSON column text against the server's own CAST(j AS CHAR): the FULL-image,
// uncompressed MySQL configurations under fixtures/matrix are replayed by
// primary key for t_json and t_big, and after every transaction that touched
// a table its JSON column must match the next snapshot in expected.jsonl.gz.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "bytes_internal.h"
#include "matrix_replay.h"
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
using matrix::Bytes;
// id -> JSON text, or nullopt for SQL NULL.
using TableState = std::map<int64_t, std::optional<std::string>>;

const std::set<std::string> kJsonTables = {"t_json", "t_big"};

// Snapshots of each JSON table, in transaction order.
bool ReadSnapshots(const fs::path& path, std::map<std::string, std::vector<TableState>>* out) {
  std::vector<matrix::ExpectedSnapshot> lines;
  if (!matrix::ReadExpected(path, &lines)) return false;
  for (const matrix::ExpectedSnapshot& line : lines) {
    if (kJsonTables.count(line.table) == 0) continue;
    TableState state;
    for (const matrix::JsonNode& row : line.rows) {
      const matrix::JsonNode* id = row.Get("id");
      const matrix::JsonNode* j = row.Get("j");
      if (id == nullptr || j == nullptr) return false;
      state[std::stoll(id->text)] =
          j->is_null() ? std::nullopt : std::optional<std::string>(j->text);
    }
    (*out)[line.table].push_back(std::move(state));
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
  std::vector<fs::path> out;
  for (const matrix::MatrixConfig& config : matrix::DiscoverConfigs()) {
    const std::string& name = config.config;
    if (config.version.rfind("mysql-", 0) == 0 && name.find("_full_") != std::string::npos &&
        name.find("_nozstd_") != std::string::npos && name.find("_nopjson") != std::string::npos) {
      out.push_back(config.dir());
    }
  }
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

    const std::vector<fs::path> binlogs = matrix::BinlogFiles(config);
    ASSERT_FALSE(binlogs.empty());

    Replay replay;
    for (const fs::path& binlog : binlogs) {
      Bytes file;
      ASSERT_TRUE(matrix::ReadGzip(binlog, &file)) << binlog;
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
