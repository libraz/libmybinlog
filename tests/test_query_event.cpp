// QUERY_EVENT bodies: the fixed post-header, the status-variable walk for both
// servers' codes, and the schema / statement that follow.

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "mybinlog/event_header.h"
#include "mybinlog/event_types.h"
#include "mybinlog/query_event.h"
#include "mybinlog/status.h"

namespace mybinlog {
namespace {

using Bytes = std::vector<uint8_t>;

Bytes Hex(const std::string& hex) {
  Bytes out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

// thread_id 4, exec_time 4, schema length 1, error_code 2, status-vars length 2,
// status vars, schema, NUL, statement.
Bytes QueryBody(const Bytes& status_vars, const std::string& schema, const std::string& query,
                uint32_t thread_id = 7, uint32_t exec_time = 3, uint16_t error_code = 0) {
  Bytes b;
  for (int shift = 0; shift < 32; shift += 8) b.push_back(static_cast<uint8_t>(thread_id >> shift));
  for (int shift = 0; shift < 32; shift += 8) b.push_back(static_cast<uint8_t>(exec_time >> shift));
  b.push_back(static_cast<uint8_t>(schema.size()));
  b.push_back(static_cast<uint8_t>(error_code));
  b.push_back(static_cast<uint8_t>(error_code >> 8));
  b.push_back(static_cast<uint8_t>(status_vars.size()));
  b.push_back(static_cast<uint8_t>(status_vars.size() >> 8));
  b.insert(b.end(), status_vars.begin(), status_vars.end());
  b.insert(b.end(), schema.begin(), schema.end());
  b.push_back(0);
  b.insert(b.end(), query.begin(), query.end());
  return b;
}

EventHeader QueryHeader() {
  EventHeader header;
  header.type_code = static_cast<uint8_t>(EventType::kQueryEvent);
  return header;
}

Status Parse(const Bytes& body, QueryEvent* out) {
  return ParseQueryEvent(QueryHeader(), body.data(), body.size(), out);
}

// Q_SQL_MODE_CODE with ANSI_QUOTES | NO_BACKSLASH_ESCAPES.
const Bytes kSqlModeVar = {0x01, 0x04, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr uint64_t kSqlMode = (1ULL << 2) | (1ULL << 20);

Bytes Concat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes& part : parts) out.insert(out.end(), part.begin(), part.end());
  return out;
}

TEST(CodecQueryEvent, FixedFieldsSchemaAndStatement) {
  QueryEvent event;
  ASSERT_EQ(Parse(QueryBody({}, "shop", "ALTER TABLE t ADD c INT", 0x01020304, 9, 1062), &event),
            Status::kOk);
  EXPECT_EQ(event.thread_id, 0x01020304u);
  EXPECT_EQ(event.exec_time, 9u);
  EXPECT_EQ(event.error_code, 1062u);
  EXPECT_EQ(event.schema, "shop");
  EXPECT_EQ(event.query, "ALTER TABLE t ADD c INT");
  EXPECT_FALSE(event.has_sql_mode);
  EXPECT_FALSE(event.has_charset);
  EXPECT_TRUE(event.status_vars_complete);
}

TEST(CodecQueryEvent, EmptySchemaAndEmptyStatement) {
  QueryEvent event;
  ASSERT_EQ(Parse(QueryBody({}, "", ""), &event), Status::kOk);
  EXPECT_TRUE(event.schema.empty());
  EXPECT_TRUE(event.query.empty());
}

TEST(CodecQueryEvent, SqlModeAndCharset) {
  const Bytes charset = {0x04, 0x21, 0x00, 0x2D, 0x00, 0xFF, 0x00};
  QueryEvent event;
  ASSERT_EQ(Parse(QueryBody(Concat({kSqlModeVar, charset}), "db", "BEGIN"), &event), Status::kOk);
  EXPECT_TRUE(event.has_sql_mode);
  EXPECT_EQ(event.sql_mode, kSqlMode);
  EXPECT_TRUE(event.has_charset);
  EXPECT_EQ(event.charset.client, 33u);
  EXPECT_EQ(event.charset.connection, 45u);
  EXPECT_EQ(event.charset.server, 255u);
  EXPECT_TRUE(event.status_vars_complete);
}

// CREATE DATABASE from mysql84_crc32_tagged.bin: FLAGS2, SQL_MODE, CATALOG_NZ,
// CHARSET, UPDATED_DB_NAMES, DDL_LOGGED_WITH_XID, DEFAULT_COLLATION_FOR_UTF8MB4,
// DEFAULT_TABLE_ENCRYPTION.
TEST(CodecQueryEvent, MySqlStatusVarsFromRealCapture) {
  const Bytes vars = Hex(
      "000000000001000000000000000006037374640408000800ff000c0166780011110000000000000012ff001400");
  QueryEvent event;
  ASSERT_EQ(Parse(QueryBody(vars, "", "CREATE DATABASE fx"), &event), Status::kOk);
  EXPECT_TRUE(event.status_vars_complete);
  EXPECT_TRUE(event.has_sql_mode);
  EXPECT_EQ(event.sql_mode, 0u);
  EXPECT_TRUE(event.has_charset);
  EXPECT_EQ(event.charset.client, 8u);
  EXPECT_EQ(event.charset.connection, 8u);
  EXPECT_EQ(event.charset.server, 255u);
  EXPECT_EQ(event.query, "CREATE DATABASE fx");
}

// CREATE TABLE from mariadb118_none.bin: CHARACTER_SET_COLLATIONS (131) and
// XID (129) around CHARSET, not in code order.
TEST(CodecQueryEvent, MariaDbStatusVarsFromRealCapture) {
  const Bytes vars =
      Hex("000000000101000000000000000006037374648305210000082300000a2d0000093600000b3c00000c040009"
          "000900"
          "09810b00000000000000");
  QueryEvent event;
  ASSERT_EQ(Parse(QueryBody(vars, "fx", "CREATE TABLE t (id INT)"), &event), Status::kOk);
  EXPECT_TRUE(event.status_vars_complete);
  EXPECT_TRUE(event.has_sql_mode);
  EXPECT_EQ(event.sql_mode, 0u);
  EXPECT_TRUE(event.has_charset);
  EXPECT_EQ(event.charset.client, 2304u);
  EXPECT_EQ(event.charset.connection, 2304u);
  EXPECT_EQ(event.charset.server, 2304u);
  EXPECT_EQ(event.schema, "fx");
}

// Every code either server writes, each at its own width, with SQL_MODE last:
// reaching it proves every length before it was right.
TEST(CodecQueryEvent, WalksEveryKnownCode) {
  const Bytes vars = Concat({
      {0x00, 1, 2, 3, 4},                          // FLAGS2
      {0x02, 0x03, 's', 't', 'd', 0x00},           // CATALOG (with NUL)
      {0x03, 1, 0, 1, 0},                          // AUTO_INCREMENT
      {0x05, 0x06, 'S', 'Y', 'S', 'T', 'E', 'M'},  // TIME_ZONE
      {0x06, 0x03, 's', 't', 'd'},                 // CATALOG_NZ
      {0x07, 0x00, 0x00},                          // LC_TIME_NAMES
      {0x08, 0x21, 0x00},                          // CHARSET_DATABASE
      {0x09, 1, 0, 0, 0, 0, 0, 0, 0},              // TABLE_MAP_FOR_UPDATE
      {0x0A, 0, 0, 0, 0},                          // MASTER_DATA_WRITTEN
      {0x0B, 0x04, 'r', 'o', 'o', 't', 0x09, 'l', 'o', 'c', 'a', 'l', 'h', 'o', 's',
       't'},                                       // INVOKER
      {0x0C, 0x02, 'a', 0x00, 'b', 'c', 0x00},     // UPDATED_DB_NAMES
      {0x0D, 1, 2, 3},                             // MICROSECONDS
      {0x10, 0x01},                                // EXPLICIT_DEFAULTS_FOR_TIMESTAMP
      {0x11, 1, 2, 3, 4, 5, 6, 7, 8},              // DDL_LOGGED_WITH_XID
      {0x12, 0xFF, 0x00},                          // DEFAULT_COLLATION_FOR_UTF8MB4
      {0x13, 0x01},                                // SQL_REQUIRE_PRIMARY_KEY
      {0x14, 0x00},                                // DEFAULT_TABLE_ENCRYPTION
      {0x15, 0x01},                                // ENABLE_CASCADE_TRIGGERS
      {0x80, 1, 2, 3},                             // MariaDB HRNOW
      {0x81, 1, 2, 3, 4, 5, 6, 7, 8},              // MariaDB XID
      {0x82, 0x04, 1, 2, 3, 4, 5, 6, 7, 8},        // MariaDB GTID_FLAGS3, alter sequence
      {0x82, 0x01},                                // MariaDB GTID_FLAGS3, flags only
      {0x83, 0x02, 1, 2, 3, 4, 5, 6, 7, 8},        // MariaDB CHARACTER_SET_COLLATIONS
      {0x04, 0x21, 0x00, 0x21, 0x00, 0x08, 0x00},  // CHARSET
      kSqlModeVar,
  });
  QueryEvent event;
  ASSERT_EQ(Parse(QueryBody(vars, "db", "SELECT 1"), &event), Status::kOk);
  EXPECT_TRUE(event.status_vars_complete);
  EXPECT_TRUE(event.has_sql_mode);
  EXPECT_EQ(event.sql_mode, kSqlMode);
  EXPECT_EQ(event.charset.server, 8u);
  EXPECT_EQ(event.schema, "db");
  EXPECT_EQ(event.query, "SELECT 1");
}

TEST(CodecQueryEvent, UpdatedDbNamesOverTheCapCarriesTheCountOnly) {
  QueryEvent event;
  ASSERT_EQ(Parse(QueryBody(Concat({{0x0C, 0xFE}, kSqlModeVar}), "", "DROP DATABASE x"), &event),
            Status::kOk);
  EXPECT_TRUE(event.has_sql_mode);
  EXPECT_TRUE(event.status_vars_complete);
}

TEST(CodecQueryEvent, UnknownCodeStopsTheWalk) {
  // Q_COMMIT_TS (14) has no defined length; nothing after it can be read.
  QueryEvent event;
  ASSERT_EQ(Parse(QueryBody(Concat({{0x0E, 0xAA, 0xBB}, kSqlModeVar}), "db", "BEGIN"), &event),
            Status::kOk);
  EXPECT_FALSE(event.status_vars_complete);
  EXPECT_FALSE(event.has_sql_mode);
  EXPECT_EQ(event.schema, "db");
  EXPECT_EQ(event.query, "BEGIN");
}

TEST(CodecQueryEvent, VariablesBeforeAnUnknownCodeAreKept) {
  QueryEvent event;
  ASSERT_EQ(Parse(QueryBody(Concat({kSqlModeVar, {0x7F, 0x00}}), "db", "BEGIN"), &event),
            Status::kOk);
  EXPECT_FALSE(event.status_vars_complete);
  EXPECT_TRUE(event.has_sql_mode);
  EXPECT_EQ(event.sql_mode, kSqlMode);
}

TEST(CodecQueryEvent, MariaDbDummyPadEndsTheWalk) {
  QueryEvent event;
  ASSERT_EQ(
      Parse(QueryBody(Concat({kSqlModeVar, {0xFF, 0xFF, 0xFF, 0xFF}}), "db", "BEGIN"), &event),
      Status::kOk);
  EXPECT_TRUE(event.status_vars_complete);
  EXPECT_TRUE(event.has_sql_mode);
  EXPECT_EQ(event.query, "BEGIN");
}

TEST(CodecQueryEvent, KnownVariableRunningPastTheBlockIsInvalid) {
  const Bytes cases[] = {
      {0x01, 0x00, 0x00, 0x00, 0x00},  // SQL_MODE: 4 of 8 bytes
      {0x04, 0x21, 0x00},              // CHARSET: 2 of 6 bytes
      {0x00},                          // FLAGS2 with no value
      {0x05, 0x05, 'U', 'T', 'C'},     // TIME_ZONE longer than the block
      {0x02, 0x03, 's', 't', 'd'},     // CATALOG missing its NUL
      {0x0B, 0x01, 'u', 0x05, 'h'},    // INVOKER host cut short
      {0x0C, 0x00},                    // UPDATED_DB_NAMES with no names
      {0x0C, 0x02, 'a', 0x00, 'b'},    // UPDATED_DB_NAMES name without NUL
      {0x82, 0x08, 1, 2, 3},           // GTID_FLAGS3 sequence cut short
      {0x83, 0x02, 1, 2, 3, 4},        // CHARACTER_SET_COLLATIONS: 1 of 2 entries
  };
  for (const Bytes& vars : cases) {
    SCOPED_TRACE(testing::Message() << "code " << static_cast<int>(vars[0]));
    QueryEvent event;
    EXPECT_EQ(Parse(QueryBody(vars, "db", "BEGIN"), &event), Status::kInvalidMetadata);
  }
}

TEST(CodecQueryEvent, TruncatedBodyIsTruncated) {
  const Bytes body = QueryBody(kSqlModeVar, "db", "");
  // Every cut ends inside the post-header, the status block, the schema or its NUL.
  for (size_t len = 0; len < body.size(); ++len) {
    SCOPED_TRACE(len);
    QueryEvent event;
    EXPECT_EQ(ParseQueryEvent(QueryHeader(), body.data(), len, &event), Status::kTruncated);
  }
}

TEST(CodecQueryEvent, StatusVarsLengthPastTheBodyIsTruncated) {
  Bytes body = QueryBody({}, "db", "BEGIN");
  body[11] = 0xFF;
  body[12] = 0xFF;
  QueryEvent event;
  EXPECT_EQ(Parse(body, &event), Status::kTruncated);
}

TEST(CodecQueryEvent, NullBodyIsTruncated) {
  QueryEvent event;
  EXPECT_EQ(ParseQueryEvent(QueryHeader(), nullptr, 64, &event), Status::kTruncated);
}

TEST(CodecQueryEvent, StopAtMalformedStillReadsTheStatement) {
  // Q_SQL_MODE_CODE, then Q_CHARSET_CODE cut short by the block's end.
  Bytes vars = kSqlModeVar;
  vars.insert(vars.end(), {0x04, 0x21, 0x00});
  const Bytes body = QueryBody(vars, "db", "ALTER TABLE t ADD c INT");
  QueryEvent event;
  ASSERT_EQ(Parse(body, &event), Status::kInvalidMetadata);
  ASSERT_EQ(ParseQueryEventWithPolicy(QueryHeader(), body.data(), body.size(), &event,
                                      StatusVarPolicy::kStopAtMalformed),
            Status::kOk);
  EXPECT_EQ(event.schema, "db");
  EXPECT_EQ(event.query, "ALTER TABLE t ADD c INT");
  EXPECT_TRUE(event.has_sql_mode);
  EXPECT_FALSE(event.has_charset);
  EXPECT_FALSE(event.status_vars_complete);
}

TEST(CodecQueryEvent, OutputUntouchedOnFailure) {
  QueryEvent event;
  event.query = "kept";
  ASSERT_EQ(Parse(QueryBody({0x01, 0x00}, "db", "BEGIN"), &event), Status::kInvalidMetadata);
  EXPECT_EQ(event.query, "kept");
}

}  // namespace
}  // namespace mybinlog
