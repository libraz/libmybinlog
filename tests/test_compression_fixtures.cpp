// Compressed events in the version-matrix fixtures (fixtures/matrix/<flavor>-<ver>/<config>/,
// captured by fixtures/matrix/capture_matrix.sh): every compressed event of every
// configuration expands, every inner event is framed and parses, and rows events
// decode where the row image is FULL.

#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <ostream>
#include <string>
#include <system_error>
#include <vector>

#include "bytes_internal.h"
#include "mybinlog/checksum.h"
#include "mybinlog/compression.h"
#include "mybinlog/crc32.h"
#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/query_event.h"
#include "mybinlog/row_decoder.h"
#include "mybinlog/server_flavor.h"
#include "mybinlog/table_map.h"
#include "mybinlog/xid.h"

namespace mybinlog {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

constexpr size_t kNoLimit = std::numeric_limits<size_t>::max();

fs::path MatrixRoot() { return fs::path(__FILE__).parent_path() / "fixtures" / "matrix"; }

// The configuration directory name encodes the settings (see capture_matrix.sh):
// <checksum>_<row image>_meta<metadata>_<compression>[_<pjson>].
struct MatrixConfig {
  std::string version;  // e.g. "mysql-9.7"
  std::string config;   // e.g. "none_full_metaminimal_zstd_nopjson"
  ServerFlavor flavor = ServerFlavor::kMySQL;
  bool full_image = false;
  bool compressed = false;
  bool crc32 = false;
};

void PrintTo(const MatrixConfig& config, std::ostream* os) {
  *os << config.version << "/" << config.config;
}

std::vector<std::string> Split(const std::string& s) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    const size_t end = s.find('_', start);
    parts.push_back(s.substr(start, end - start));
    if (end == std::string::npos) return parts;
    start = end + 1;
  }
}

std::vector<MatrixConfig> DiscoverConfigs() {
  std::vector<MatrixConfig> configs;
  std::error_code ec;
  for (fs::directory_iterator v(MatrixRoot(), ec), end; !ec && v != end; v.increment(ec)) {
    if (!v->is_directory(ec)) continue;
    for (fs::directory_iterator c(v->path(), ec); !ec && c != end; c.increment(ec)) {
      if (!c->is_directory(ec)) continue;
      MatrixConfig config;
      config.version = v->path().filename().string();
      config.config = c->path().filename().string();
      config.flavor =
          config.version.rfind("mariadb", 0) == 0 ? ServerFlavor::kMariaDB : ServerFlavor::kMySQL;
      const std::vector<std::string> parts = Split(config.config);
      if (parts.size() < 4) continue;
      config.crc32 = parts[0] == "crc32";
      config.full_image = parts[1] == "full";
      config.compressed = parts[3] == "zlib" || parts[3] == "zstd";
      configs.push_back(config);
    }
  }
  std::sort(configs.begin(), configs.end(), [](const MatrixConfig& a, const MatrixConfig& b) {
    return a.version + "/" + a.config < b.version + "/" + b.config;
  });
  return configs;
}

std::vector<fs::path> BinlogFiles(const fs::path& dir) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.rfind("binlog.", 0) == 0 && it->path().extension() == ".gz") {
      files.push_back(it->path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

bool ReadGzip(const fs::path& path, Bytes* out) {
  gzFile file = gzopen(path.string().c_str(), "rb");
  if (file == nullptr) return false;
  out->clear();
  uint8_t chunk[1 << 16];
  int n = 0;
  while ((n = gzread(file, chunk, sizeof(chunk))) > 0) out->insert(out->end(), chunk, chunk + n);
  const bool ok = n == 0;
  gzclose(file);
  return ok && !out->empty();
}

struct Counts {
  size_t compressed_events = 0;  // MariaDB 165-171 and TRANSACTION_PAYLOAD
  size_t inner_events = 0;       // events expanded out of a compressed event
  size_t rows_decoded = 0;       // rows events among them decoded with DecodeRowsEvent
  size_t zstd_unsupported = 0;   // payloads skipped because the build has no zstd
};

class Walker {
 public:
  Walker(const MatrixConfig& config, Counts* counts) : config_(config), counts_(counts) {}

  // One event that is (or came out of) a compressed event: parse what has a parser.
  void CheckPlain(const EventHeader& header, const uint8_t* body, size_t size) {
    ++counts_->inner_events;
    const auto type = static_cast<EventType>(header.type_code);
    SCOPED_TRACE(EventTypeName(type));
    if (type == EventType::kTableMapEvent) {
      TableMap map;
      ASSERT_EQ(ParseTableMapEvent(header, body, size, config_.flavor, &map), Status::kOk);
      tables_[map.table_id] = map;
    } else if (type == EventType::kQueryEvent) {
      QueryEvent query;
      ASSERT_EQ(ParseQueryEvent(header, body, size, &query), Status::kOk);
      EXPECT_FALSE(query.query.empty());
    } else if (type == EventType::kXidEvent) {
      XidEvent xid;
      ASSERT_EQ(ParseXidEvent(header, body, size, &xid), Status::kOk);
    } else if (IsRowsEvent(header.type_code)) {
      ASSERT_GE(size, 6u);
      const auto table = tables_.find(ReadU48Le(body));
      ASSERT_NE(table, tables_.end());
      // MINIMAL and NOBLOB images are framed but not decoded here.
      if (!config_.full_image) return;
      RowsEvent rows;
      ASSERT_EQ(DecodeRowsEvent(header, body, size, table->second, DecodeOptions(), &rows),
                Status::kOk)
          << table->second.table;
      EXPECT_FALSE(rows.rows.empty() && rows.updates.empty());
      ++counts_->rows_decoded;
    }
  }

  void Event(const EventHeader& header, const uint8_t* body, size_t size) {
    if (header.type_code == static_cast<uint8_t>(EventType::kTableMapEvent)) {
      TableMap map;
      ASSERT_EQ(ParseTableMapEvent(header, body, size, config_.flavor, &map), Status::kOk);
      tables_[map.table_id] = map;
      return;
    }
    if (IsMariaDbCompressedEvent(header.type_code)) {
      ++counts_->compressed_events;
      ASSERT_EQ(config_.flavor, ServerFlavor::kMariaDB);
      DecompressedEvent plain;
      ASSERT_EQ(DecompressMariaDbEvent(header, body, size, kNoLimit, &plain), Status::kOk);
      EXPECT_EQ(plain.header.event_length - kEventHeaderSize - plain.body.size(),
                header.event_length - kEventHeaderSize - size);
      CheckPlain(plain.header, plain.body.data(), plain.body.size());
      return;
    }
    if (header.type_code == static_cast<uint8_t>(EventType::kTransactionPayloadEvent)) {
      ++counts_->compressed_events;
      ASSERT_EQ(config_.flavor, ServerFlavor::kMySQL);
      TransactionPayloadEvent payload;
      ASSERT_EQ(ParseTransactionPayloadEvent(header, body, size, &payload), Status::kOk);
      EXPECT_EQ(payload.payload + payload.payload_size, body + size);
      Bytes events;
      const Status status = DecompressTransactionPayload(payload, kNoLimit, &events);
      if (payload.compression == PayloadCompression::kZstd && !HasZstdSupport()) {
        EXPECT_EQ(status, Status::kUnsupported);
        ++counts_->zstd_unsupported;
        return;
      }
      ASSERT_EQ(status, Status::kOk);
      ASSERT_TRUE(payload.has_uncompressed_size);
      size_t offset = 0;
      while (offset < events.size()) {
        PayloadEventView inner;
        ASSERT_EQ(NextPayloadEvent(events.data(), events.size(), &offset, &inner), Status::kOk);
        // Inner events carry no checksum, even in a CRC32 file.
        if (config_.crc32 && inner.size >= kChecksumSize) {
          const uint8_t* frame = inner.body - kEventHeaderSize;
          const size_t covered = kEventHeaderSize + inner.size - kChecksumSize;
          EXPECT_NE(UpdateCrc32(0, frame, covered), ReadU32Le(frame + covered));
        }
        CheckPlain(inner.header, inner.body, inner.size);
        if (HasFatalFailure()) return;
      }
      EXPECT_EQ(offset, events.size());
    }
  }

 private:
  static bool HasFatalFailure() { return testing::Test::HasFatalFailure(); }

  const MatrixConfig& config_;
  Counts* counts_;
  std::map<uint64_t, TableMap> tables_;
};

class CodecCompressionFixtures : public testing::TestWithParam<MatrixConfig> {};

TEST_P(CodecCompressionFixtures, ExpandsEveryCompressedEvent) {
  const MatrixConfig& config = GetParam();
  const std::vector<fs::path> files = BinlogFiles(MatrixRoot() / config.version / config.config);
  ASSERT_FALSE(files.empty());
  Counts counts;
  for (const fs::path& path : files) {
    SCOPED_TRACE(path.string());
    Bytes file;
    ASSERT_TRUE(ReadGzip(path, &file));
    ASSERT_GE(file.size(), kBinlogMagicOffset);
    ASSERT_TRUE(std::equal(kBinlogMagic.begin(), kBinlogMagic.end(), file.begin()));
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
    EXPECT_EQ(pos, file.size());
  }
  if (config.compressed) {
    EXPECT_GT(counts.compressed_events, 0u);
    if (counts.zstd_unsupported == 0) EXPECT_GT(counts.inner_events, 0u);
    if (config.full_image && counts.zstd_unsupported == 0) EXPECT_GT(counts.rows_decoded, 0u);
  } else {
    EXPECT_EQ(counts.compressed_events, 0u);
  }
  RecordProperty("compressed_events", static_cast<int>(counts.compressed_events));
  RecordProperty("inner_events", static_cast<int>(counts.inner_events));
  RecordProperty("rows_decoded", static_cast<int>(counts.rows_decoded));
}

std::string ConfigName(const testing::TestParamInfo<MatrixConfig>& info) {
  std::string name = info.param.version + "__" + info.param.config;
  for (char& c : name) {
    if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
  }
  return name;
}

INSTANTIATE_TEST_SUITE_P(CodecCompressionMatrix, CodecCompressionFixtures,
                         testing::ValuesIn(DiscoverConfigs()), ConfigName);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(CodecCompressionFixtures);

}  // namespace
}  // namespace mybinlog
