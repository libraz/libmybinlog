/**
 * @file matrix_replay.h
 * @brief Shared access to the version x configuration fixtures under tests/fixtures/matrix.
 *
 * Fixture discovery, gzip inflation, a small JSON reader for meta.json and
 * expected.jsonl, and gtest parameter naming. See
 * tests/fixtures/matrix/README.md for the layout these functions read.
 */

#ifndef MYBINLOG_TESTS_MATRIX_REPLAY_H_
#define MYBINLOG_TESTS_MATRIX_REPLAY_H_

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "mybinlog/server_flavor.h"

namespace mybinlog {
namespace matrix {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

/** @return tests/fixtures/matrix in the source tree. */
fs::path MatrixRoot();

/** @brief One fixture directory, `<flavor>-<version>/<config>`. */
struct MatrixConfig {
  std::string version;  ///< Version directory, e.g. "mysql-8.4".
  std::string config;   ///< Configuration directory, e.g. "crc32_full_metafull_nozstd_nopjson".

  /** @return The directory holding the binlogs, meta.json and expected.jsonl.gz. */
  fs::path dir() const { return MatrixRoot() / version / config; }
  /** @return kMariaDB for a "mariadb-" version directory, kMySQL otherwise. */
  ServerFlavor flavor() const;
};

/** @brief gtest printer: "version/config". */
void PrintTo(const MatrixConfig& config, std::ostream* os);

/** @return Every configuration directory, sorted by version then config. */
std::vector<MatrixConfig> DiscoverConfigs();

/** @return The `binlog.*.gz` files of @p dir in file-name order. */
std::vector<fs::path> BinlogFiles(const fs::path& dir);

/**
 * @brief Inflate a gzip file.
 * @param path File to read.
 * @param[out] out Replaced with the inflated bytes.
 * @return true when the whole file inflated to at least one byte.
 */
bool ReadGzip(const fs::path& path, Bytes* out);

/** @return `version__config` with every non-alphanumeric character turned into '_'. */
std::string SanitizedName(const MatrixConfig& config);

/** @brief Parameter-name generator for INSTANTIATE_TEST_SUITE_P over MatrixConfig. */
std::string ConfigTestName(const testing::TestParamInfo<MatrixConfig>& info);

/** @brief One parsed JSON value. */
struct JsonNode {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  /** kBool: "true"/"false"; kNumber: the literal as written; kString: the decoded UTF-8. */
  std::string text;
  std::vector<JsonNode> items;                            ///< kArray.
  std::vector<std::pair<std::string, JsonNode>> members;  ///< kObject, in document order.

  /** @return The first member named @p key, or null. */
  const JsonNode* Get(const std::string& key) const;
  /** @return Whether this is the null literal. */
  bool is_null() const { return type == Type::kNull; }
};

/**
 * @brief Parse one JSON document; surrounding whitespace is allowed.
 * @return false on any syntax error or trailing garbage.
 */
bool ParseJson(const std::string& text, JsonNode* out);

/**
 * @brief Parse every non-empty line of a gzip-compressed JSON Lines file.
 * @return false when the file does not inflate or a line does not parse.
 */
bool ReadJsonLines(const fs::path& path, std::vector<JsonNode>* out);

/** @brief One column of meta.json `tables[].columns[]`. */
struct MetaColumn {
  std::string name;
  std::string norm;         ///< Normalization tag (README "Per-column normalization").
  std::string data_type;    ///< information_schema DATA_TYPE.
  std::string column_type;  ///< information_schema COLUMN_TYPE.
  std::string charset;      ///< Character set name; empty when null.
  bool compressed = false;  ///< MariaDB COMPRESSED column.
  int pk_seq = 0;           ///< 1-based position in the primary key; 0 outside it.
};

/** @brief One table of meta.json. */
struct MetaTable {
  std::string name;
  std::vector<MetaColumn> columns;  ///< In ordinal order.
  std::vector<size_t> primary_key;  ///< Column indices ordered by pk_seq.
};

/** @brief One entry of meta.json `transactions[]`. */
struct MetaTransaction {
  int seq = 0;
  std::string name;
  std::string kind;        ///< "ddl" or "txn".
  std::string gtid_after;  ///< gtid_executed (MySQL) or gtid_binlog_pos (MariaDB) afterwards.
  std::vector<std::string> tables;
};

/** @brief The parts of meta.json a replay needs. */
struct Meta {
  std::string flavor;    ///< "mysql" or "mariadb".
  std::string database;  ///< Schema holding the workload tables.
  std::vector<MetaTable> tables;
  std::vector<MetaTransaction> transactions;

  /** @return The table named @p name, or null. */
  const MetaTable* FindTable(const std::string& name) const;
};

/** @return false when meta.json in @p dir is missing or lacks a required field. */
bool ReadMeta(const fs::path& dir, Meta* out);

/** @brief One line of expected.jsonl: a table's whole contents after a transaction. */
struct ExpectedSnapshot {
  std::string txn;
  int seq = 0;
  std::string table;
  std::vector<JsonNode> rows;  ///< Objects keyed by column name, ordered by primary key.
};

/** @return false when expected.jsonl.gz does not inflate or a line lacks a field. */
bool ReadExpected(const fs::path& path, std::vector<ExpectedSnapshot>* out);

}  // namespace matrix
}  // namespace mybinlog

#endif  // MYBINLOG_TESTS_MATRIX_REPLAY_H_
