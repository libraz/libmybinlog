// Static server-setting rules.

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "mybinlog/server_settings.h"

namespace mybinlog {
namespace {

using Vars = std::map<std::string, std::string, std::less<>>;

bool Lookup(void* ctx, std::string_view name, std::string* value) {
  const auto* vars = static_cast<const Vars*>(ctx);
  const auto it = vars->find(name);
  if (it == vars->end()) {
    return false;
  }
  *value = it->second;
  return true;
}

std::vector<Violation> Eval(const Vars& vars, ServerFlavor flavor, std::vector<RuleId> ids) {
  std::vector<Violation> out;
  Vars copy = vars;
  const size_t n = EvaluateServerSettings(Lookup, &copy, flavor, ids.data(), ids.size(), &out);
  EXPECT_EQ(n, out.size());
  return out;
}

TEST(CodecPolicy, RuleIds) {
  const RuleId all[] = {RuleId::kLogBin,
                        RuleId::kGtidMode,
                        RuleId::kBinlogFormat,
                        RuleId::kBinlogRowImage,
                        RuleId::kNoTransactionCompression,
                        RuleId::kNoPartialJson,
                        RuleId::kNoLogBinCompress,
                        RuleId::kChecksumCrc32,
                        RuleId::kRowMetadataFull,
                        RuleId::kLogReplicaUpdates};
  ASSERT_EQ(sizeof(all) / sizeof(all[0]), kRuleCount);
  for (size_t i = 0; i < kRuleCount; ++i) {
    EXPECT_EQ(static_cast<size_t>(all[i]), i);
    EXPECT_EQ(kServerSettingRules[i].id, all[i]);
    EXPECT_EQ(&GetRule(all[i]), &kServerSettingRules[i]);
    EXPECT_STRNE(RuleIdName(all[i]), "");
    EXPECT_TRUE(kServerSettingRules[i].applies_to_mysql ||
                kServerSettingRules[i].applies_to_mariadb);
    EXPECT_EQ(kServerSettingRules[i].applies_to_mysql,
              kServerSettingRules[i].mysql_variable != nullptr);
    EXPECT_EQ(kServerSettingRules[i].applies_to_mariadb,
              kServerSettingRules[i].mariadb_variable != nullptr);
  }
  EXPECT_STREQ(RuleIdName(RuleId::kLogReplicaUpdates), "kLogReplicaUpdates");
}

TEST(CodecPolicy, RuleVariablesAndFlavors) {
  EXPECT_STREQ(GetRule(RuleId::kLogBin).mysql_variable, "log_bin");
  EXPECT_STREQ(GetRule(RuleId::kGtidMode).mysql_variable, "gtid_mode");
  EXPECT_FALSE(GetRule(RuleId::kGtidMode).applies_to_mariadb);
  EXPECT_STREQ(GetRule(RuleId::kBinlogFormat).mysql_variable, "binlog_format");
  EXPECT_STREQ(GetRule(RuleId::kBinlogRowImage).mysql_variable, "binlog_row_image");
  EXPECT_STREQ(GetRule(RuleId::kNoTransactionCompression).mysql_variable,
               "binlog_transaction_compression");
  EXPECT_FALSE(GetRule(RuleId::kNoTransactionCompression).applies_to_mariadb);
  EXPECT_STREQ(GetRule(RuleId::kNoPartialJson).mysql_variable, "binlog_row_value_options");
  EXPECT_FALSE(GetRule(RuleId::kNoPartialJson).applies_to_mariadb);
  EXPECT_STREQ(GetRule(RuleId::kNoLogBinCompress).mariadb_variable, "log_bin_compress");
  EXPECT_FALSE(GetRule(RuleId::kNoLogBinCompress).applies_to_mysql);
  EXPECT_STREQ(GetRule(RuleId::kChecksumCrc32).mysql_variable, "binlog_checksum");
  EXPECT_STREQ(GetRule(RuleId::kRowMetadataFull).mariadb_variable, "binlog_row_metadata");
  EXPECT_STREQ(GetRule(RuleId::kLogReplicaUpdates).mysql_variable, "log_replica_updates");
  EXPECT_STREQ(GetRule(RuleId::kLogReplicaUpdates).mariadb_variable, "log_slave_updates");
}

TEST(CodecPolicy, EqualsRulesAcceptExpectedValue) {
  const Vars ok = {{"log_bin", "ON"},
                   {"gtid_mode", "ON"},
                   {"binlog_format", "ROW"},
                   {"binlog_row_image", "FULL"},
                   {"binlog_checksum", "CRC32"},
                   {"binlog_row_metadata", "FULL"},
                   {"log_replica_updates", "ON"}};
  const std::vector<RuleId> ids = {RuleId::kLogBin,           RuleId::kGtidMode,
                                   RuleId::kBinlogFormat,     RuleId::kBinlogRowImage,
                                   RuleId::kChecksumCrc32,    RuleId::kRowMetadataFull,
                                   RuleId::kLogReplicaUpdates};
  EXPECT_TRUE(Eval(ok, ServerFlavor::kMySQL, ids).empty());
}

TEST(CodecPolicy, EqualsRulesRejectWrongValue) {
  const struct {
    RuleId id;
    const char* variable;
    const char* bad;
  } cases[] = {
      {RuleId::kLogBin, "log_bin", "OFF"},
      {RuleId::kGtidMode, "gtid_mode", "OFF_PERMISSIVE"},
      {RuleId::kBinlogFormat, "binlog_format", "MIXED"},
      {RuleId::kBinlogRowImage, "binlog_row_image", "MINIMAL"},
      {RuleId::kChecksumCrc32, "binlog_checksum", "NONE"},
      {RuleId::kRowMetadataFull, "binlog_row_metadata", "MINIMAL"},
      {RuleId::kLogReplicaUpdates, "log_replica_updates", "OFF"},
  };
  for (const auto& c : cases) {
    const auto v = Eval({{c.variable, c.bad}}, ServerFlavor::kMySQL, {c.id});
    ASSERT_EQ(v.size(), 1u) << c.variable;
    EXPECT_EQ(v[0].id, c.id);
    EXPECT_EQ(v[0].variable, c.variable);
    EXPECT_EQ(v[0].observed, c.bad);
    EXPECT_FALSE(v[0].absent);
    EXPECT_NE(v[0].expected, nullptr);
  }
}

TEST(CodecPolicy, AbsentVariable) {
  // Equality rules fail when the variable is missing.
  for (const RuleId id :
       {RuleId::kLogBin, RuleId::kGtidMode, RuleId::kBinlogFormat, RuleId::kBinlogRowImage,
        RuleId::kChecksumCrc32, RuleId::kRowMetadataFull, RuleId::kLogReplicaUpdates}) {
    const auto v = Eval({}, ServerFlavor::kMySQL, {id});
    ASSERT_EQ(v.size(), 1u) << RuleIdName(id);
    EXPECT_TRUE(v[0].absent);
    EXPECT_TRUE(v[0].observed.empty());
  }
  // "Must not" rules pass: the feature does not exist on that server.
  EXPECT_TRUE(
      Eval({}, ServerFlavor::kMySQL, {RuleId::kNoTransactionCompression, RuleId::kNoPartialJson})
          .empty());
  EXPECT_TRUE(Eval({}, ServerFlavor::kMariaDB, {RuleId::kNoLogBinCompress}).empty());
}

TEST(CodecPolicy, ValuesCompareCaseInsensitively) {
  EXPECT_TRUE(Eval({{"log_bin", "on"}}, ServerFlavor::kMySQL, {RuleId::kLogBin}).empty());
  EXPECT_TRUE(
      Eval({{"binlog_format", "row"}}, ServerFlavor::kMySQL, {RuleId::kBinlogFormat}).empty());
  EXPECT_TRUE(
      Eval({{"binlog_row_image", "Full"}}, ServerFlavor::kMariaDB, {RuleId::kBinlogRowImage})
          .empty());
  EXPECT_TRUE(
      Eval({{"binlog_checksum", "crc32"}}, ServerFlavor::kMySQL, {RuleId::kChecksumCrc32}).empty());
  EXPECT_EQ(Eval({{"binlog_transaction_compression", "on"}}, ServerFlavor::kMySQL,
                 {RuleId::kNoTransactionCompression})
                .size(),
            1u);
  EXPECT_EQ(Eval({{"log_bin_compress", "On"}}, ServerFlavor::kMariaDB, {RuleId::kNoLogBinCompress})
                .size(),
            1u);
  EXPECT_EQ(Eval({{"binlog_row_value_options", "partial_json"}}, ServerFlavor::kMySQL,
                 {RuleId::kNoPartialJson})
                .size(),
            1u);
}

TEST(CodecPolicy, NotEqualsRulesAcceptOtherValues) {
  EXPECT_TRUE(Eval({{"binlog_transaction_compression", "OFF"}}, ServerFlavor::kMySQL,
                   {RuleId::kNoTransactionCompression})
                  .empty());
  EXPECT_TRUE(
      Eval({{"log_bin_compress", "OFF"}}, ServerFlavor::kMariaDB, {RuleId::kNoLogBinCompress})
          .empty());
  const auto v = Eval({{"binlog_transaction_compression", "ON"}}, ServerFlavor::kMySQL,
                      {RuleId::kNoTransactionCompression});
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].observed, "ON");
}

TEST(CodecPolicy, PartialJsonIsASubstringMatch) {
  EXPECT_TRUE(
      Eval({{"binlog_row_value_options", ""}}, ServerFlavor::kMySQL, {RuleId::kNoPartialJson})
          .empty());
  EXPECT_EQ(Eval({{"binlog_row_value_options", "PARTIAL_JSON"}}, ServerFlavor::kMySQL,
                 {RuleId::kNoPartialJson})
                .size(),
            1u);
  EXPECT_EQ(Eval({{"binlog_row_value_options", "OTHER,PARTIAL_JSON"}}, ServerFlavor::kMySQL,
                 {RuleId::kNoPartialJson})
                .size(),
            1u);
}

TEST(CodecPolicy, WrongFlavorRulesAreSkipped) {
  // MySQL-only rules on MariaDB: skipped even when the variable would violate.
  const Vars bad = {{"gtid_mode", "OFF"},
                    {"binlog_transaction_compression", "ON"},
                    {"binlog_row_value_options", "PARTIAL_JSON"}};
  EXPECT_TRUE(Eval(bad, ServerFlavor::kMariaDB,
                   {RuleId::kGtidMode, RuleId::kNoTransactionCompression, RuleId::kNoPartialJson})
                  .empty());
  // MariaDB-only rule on MySQL.
  EXPECT_TRUE(Eval({{"log_bin_compress", "ON"}}, ServerFlavor::kMySQL, {RuleId::kNoLogBinCompress})
                  .empty());
}

TEST(CodecPolicy, LogReplicaUpdatesUsesFlavorVariable) {
  const Vars mysql_vars = {{"log_replica_updates", "OFF"}, {"log_slave_updates", "ON"}};
  const auto m = Eval(mysql_vars, ServerFlavor::kMySQL, {RuleId::kLogReplicaUpdates});
  ASSERT_EQ(m.size(), 1u);
  EXPECT_EQ(m[0].variable, "log_replica_updates");
  const Vars maria_vars = {{"log_replica_updates", "ON"}, {"log_slave_updates", "OFF"}};
  const auto b = Eval(maria_vars, ServerFlavor::kMariaDB, {RuleId::kLogReplicaUpdates});
  ASSERT_EQ(b.size(), 1u);
  EXPECT_EQ(b[0].variable, "log_slave_updates");
  EXPECT_TRUE(
      Eval({{"log_slave_updates", "ON"}}, ServerFlavor::kMariaDB, {RuleId::kLogReplicaUpdates})
          .empty());
  // The other flavor's name is not consulted.
  EXPECT_EQ(Eval({{"log_slave_updates", "ON"}}, ServerFlavor::kMySQL, {RuleId::kLogReplicaUpdates})
                .size(),
            1u);
}

TEST(CodecPolicy, ViolationsAppendInIdOrder) {
  std::vector<Violation> out;
  out.push_back(Violation{RuleId::kLogBin, "pre", "", true, ""});
  Vars vars = {{"binlog_format", "STATEMENT"}, {"log_bin", "OFF"}};
  const RuleId ids[] = {RuleId::kBinlogFormat, RuleId::kLogBin};
  EXPECT_EQ(EvaluateServerSettings(Lookup, &vars, ServerFlavor::kMySQL, ids, 2, &out), 2u);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[1].id, RuleId::kBinlogFormat);
  EXPECT_EQ(out[2].id, RuleId::kLogBin);
  EXPECT_EQ(EvaluateServerSettings(Lookup, &vars, ServerFlavor::kMySQL, nullptr, 0, &out), 0u);
}

}  // namespace
}  // namespace mybinlog
