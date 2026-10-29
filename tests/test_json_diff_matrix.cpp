// Partial JSON updates against the server: the uncompressed PARTIAL_JSON MySQL
// configurations under fixtures/matrix are replayed by primary key for t_json
// and t_big, every kJsonDiff after image is applied to the replayed value with
// ApplyJsonDiffs(), and after each transaction that touched a table its JSON
// column must match the next CAST(j AS CHAR) snapshot in expected.jsonl.gz.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "bytes_internal.h"
#include "matrix_replay.h"
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
using matrix::Bytes;
// id -> JSON column, nullopt for SQL NULL.
using TableState = std::map<int64_t, std::optional<JsonValue>>;
using ExpectedState = std::map<int64_t, std::optional<std::string>>;

const std::set<std::string> kJsonTables = {"t_json", "t_big"};

bool ReadSnapshots(const fs::path& path, std::map<std::string, std::vector<ExpectedState>>* out) {
  std::vector<matrix::ExpectedSnapshot> lines;
  if (!matrix::ReadExpected(path, &lines)) return false;
  for (const matrix::ExpectedSnapshot& line : lines) {
    if (kJsonTables.count(line.table) == 0) continue;
    ExpectedState state;
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
  std::vector<fs::path> out;
  for (const matrix::MatrixConfig& config : matrix::DiscoverConfigs()) {
    const std::string& name = config.config;
    const bool pjson = name.size() > 6 && name.compare(name.size() - 6, 6, "_pjson") == 0;
    if (config.version.rfind("mysql-", 0) == 0 && pjson &&
        name.find("_nozstd_") != std::string::npos) {
      out.push_back(config.dir());
    }
  }
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

    const std::vector<fs::path> binlogs = matrix::BinlogFiles(config);
    ASSERT_FALSE(binlogs.empty());

    Replay replay;
    for (const fs::path& binlog : binlogs) {
      Bytes file;
      ASSERT_TRUE(matrix::ReadGzip(binlog, &file)) << binlog;
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
