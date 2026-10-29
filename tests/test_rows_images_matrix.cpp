// Every rows event of every version-matrix configuration
// (fixtures/matrix/<flavor>-<ver>/<config>/) decodes, compressed ones after
// expansion. MINIMAL and NOBLOB configurations must yield kAbsent, FULL ones
// never; PARTIAL_JSON configurations must yield kJsonDiff, the rest never.
// Values are not compared with expected.jsonl here.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "bytes_internal.h"
#include "matrix_replay.h"
#include "mybinlog/checksum.h"
#include "mybinlog/compression.h"
#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/row_decoder.h"
#include "mybinlog/server_flavor.h"
#include "mybinlog/table_map.h"
#include "mybinlog/value.h"

namespace mybinlog {
namespace {

namespace fs = std::filesystem;
using matrix::Bytes;
using matrix::MatrixConfig;

constexpr size_t kNoLimit = std::numeric_limits<size_t>::max();

// Directory name: <checksum>_<row image>_meta<metadata>_<compression>[_<pjson>].
struct ImageConfig {
  std::string image;  // "full", "minimal" or "noblob"
  bool partial_json = false;
};

ImageConfig ParseConfigName(const std::string& config) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    const size_t end = config.find('_', start);
    parts.push_back(config.substr(start, end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  ImageConfig out;
  if (parts.size() >= 4) {
    out.image = parts[1];
    out.partial_json = parts.size() > 4 && parts[4] == "pjson";
  }
  return out;
}

// Configurations whose directory name carries at least the four fixed fields.
std::vector<MatrixConfig> DiscoverConfigs() {
  std::vector<MatrixConfig> out;
  for (const MatrixConfig& config : matrix::DiscoverConfigs()) {
    if (!ParseConfigName(config.config).image.empty()) out.push_back(config);
  }
  return out;
}

struct Counts {
  size_t rows_events = 0;
  size_t partial_events = 0;    // PARTIAL_UPDATE_ROWS among them
  size_t zstd_unsupported = 0;  // payloads this build cannot expand
  std::map<Value::Kind, size_t> kinds;
};

class Walker {
 public:
  Walker(const MatrixConfig& config, Counts* counts) : flavor_(config.flavor()), counts_(counts) {}

  void Plain(const EventHeader& header, const uint8_t* body, size_t size) {
    const auto type = static_cast<EventType>(header.type_code);
    if (type == EventType::kTableMapEvent) {
      TableMap map;
      ASSERT_EQ(ParseTableMapEvent(header, body, size, flavor_, &map), Status::kOk);
      tables_[map.table_id] = map;
      return;
    }
    const bool partial = type == EventType::kPartialUpdateRowsEvent;
    if (!IsRowsEvent(header.type_code)) return;
    SCOPED_TRACE(EventTypeName(type));
    ASSERT_GE(size, 6u);
    const auto table = tables_.find(ReadU48Le(body));
    ASSERT_NE(table, tables_.end());
    RowsEvent rows;
    ASSERT_EQ(DecodeRowsEvent(header, body, size, table->second, DecodeOptions(), &rows),
              Status::kOk)
        << table->second.table;
    EXPECT_FALSE(rows.rows.empty() && rows.updates.empty());
    ++counts_->rows_events;
    if (partial) ++counts_->partial_events;
    for (const Row& row : rows.rows) Count(row);
    for (const auto& update : rows.updates) {
      Count(update.first);
      Count(update.second);
    }
  }

  void Event(const EventHeader& header, const uint8_t* body, size_t size) {
    if (IsMariaDbCompressedEvent(header.type_code)) {
      DecompressedEvent plain;
      ASSERT_EQ(DecompressMariaDbEvent(header, body, size, kNoLimit, &plain), Status::kOk);
      Plain(plain.header, plain.body.data(), plain.body.size());
      return;
    }
    if (header.type_code != static_cast<uint8_t>(EventType::kTransactionPayloadEvent)) {
      Plain(header, body, size);
      return;
    }
    TransactionPayloadEvent payload;
    ASSERT_EQ(ParseTransactionPayloadEvent(header, body, size, &payload), Status::kOk);
    Bytes events;
    const Status status = DecompressTransactionPayload(payload, kNoLimit, &events);
    if (status == Status::kUnsupported && !HasZstdSupport()) {
      ++counts_->zstd_unsupported;
      return;
    }
    ASSERT_EQ(status, Status::kOk);
    size_t offset = 0;
    while (offset < events.size()) {
      PayloadEventView inner;
      ASSERT_EQ(NextPayloadEvent(events.data(), events.size(), &offset, &inner), Status::kOk);
      Plain(inner.header, inner.body, inner.size);
      if (testing::Test::HasFatalFailure()) return;
    }
  }

 private:
  void Count(const Row& row) {
    for (const Value& value : row) ++counts_->kinds[value.kind];
  }

  ServerFlavor flavor_;
  Counts* counts_;
  std::map<uint64_t, TableMap> tables_;
};

class CodecRowsMatrix : public testing::TestWithParam<MatrixConfig> {};

TEST_P(CodecRowsMatrix, DecodesEveryRowsEvent) {
  const MatrixConfig& config = GetParam();
  const ImageConfig image = ParseConfigName(config.config);
  const std::vector<fs::path> files = matrix::BinlogFiles(config.dir());
  ASSERT_FALSE(files.empty());
  Counts counts;
  for (const fs::path& path : files) {
    SCOPED_TRACE(path.string());
    Bytes file;
    ASSERT_TRUE(matrix::ReadGzip(path, &file));
    ASSERT_GE(file.size(), kBinlogMagicOffset);
    ChecksumTracker tracker;
    Walker walker(config, &counts);
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
      walker.Event(header, file.data() + pos + kEventHeaderSize, layout.body_length());
      if (HasFatalFailure()) return;
      pos += header.event_length;
    }
  }
  RecordProperty("rows_events", static_cast<int>(counts.rows_events));
  RecordProperty("partial_events", static_cast<int>(counts.partial_events));
  RecordProperty("absent", static_cast<int>(counts.kinds[Value::Kind::kAbsent]));
  RecordProperty("json", static_cast<int>(counts.kinds[Value::Kind::kJson]));
  RecordProperty("json_diff", static_cast<int>(counts.kinds[Value::Kind::kJsonDiff]));
  // A build without zstd sees nothing of a compressed MySQL config.
  if (counts.zstd_unsupported > 0 && counts.rows_events == 0) {
    GTEST_SKIP() << "zstd payloads need MYBINLOG_WITH_ZSTD";
  }
  EXPECT_GT(counts.rows_events, 0u);

  const size_t absent = counts.kinds[Value::Kind::kAbsent];
  if (image.image == "full") {
    EXPECT_EQ(absent, 0u);
  } else {
    EXPECT_GT(absent, 0u);
  }
  const size_t diffs = counts.kinds[Value::Kind::kJsonDiff];
  if (image.partial_json) {
    EXPECT_GT(counts.partial_events, 0u);
    EXPECT_GT(diffs, 0u);
  } else {
    EXPECT_EQ(counts.partial_events, 0u);
    EXPECT_EQ(diffs, 0u);
  }
  if (config.flavor() == ServerFlavor::kMariaDB) {
    EXPECT_EQ(counts.kinds[Value::Kind::kJson], 0u);  // MariaDB JSON is LONGTEXT
  } else {
    EXPECT_GT(counts.kinds[Value::Kind::kJson], 0u);
  }
}

INSTANTIATE_TEST_SUITE_P(CodecRowsImages, CodecRowsMatrix, testing::ValuesIn(DiscoverConfigs()),
                         matrix::ConfigTestName);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(CodecRowsMatrix);

}  // namespace
}  // namespace mybinlog
