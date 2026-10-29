#include "mybinlog/server_settings.h"

namespace mybinlog {
namespace {

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (Lower(a[i]) != Lower(b[i])) {
      return false;
    }
  }
  return true;
}

bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) {
  if (needle.size() > haystack.size()) {
    return false;
  }
  for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    if (EqualsIgnoreCase(haystack.substr(i, needle.size()), needle)) {
      return true;
    }
  }
  return false;
}

}  // namespace

const std::array<ServerSettingRule, kRuleCount> kServerSettingRules = {{
    {RuleId::kLogBin, true, true, "log_bin", "log_bin", RuleMatch::kEquals, "ON", "ON"},
    {RuleId::kGtidMode, true, false, "gtid_mode", nullptr, RuleMatch::kEquals, "ON", "ON"},
    {RuleId::kBinlogFormat, true, true, "binlog_format", "binlog_format", RuleMatch::kEquals, "ROW",
     "ROW"},
    {RuleId::kBinlogRowImage, true, true, "binlog_row_image", "binlog_row_image",
     RuleMatch::kEquals, "FULL", "FULL"},
    {RuleId::kNoTransactionCompression, true, false, "binlog_transaction_compression", nullptr,
     RuleMatch::kNotEquals, "ON", "not ON"},
    {RuleId::kNoPartialJson, true, false, "binlog_row_value_options", nullptr,
     RuleMatch::kNotContains, "PARTIAL_JSON", "without PARTIAL_JSON"},
    {RuleId::kNoLogBinCompress, false, true, nullptr, "log_bin_compress", RuleMatch::kNotEquals,
     "ON", "not ON"},
    {RuleId::kChecksumCrc32, true, true, "binlog_checksum", "binlog_checksum", RuleMatch::kEquals,
     "CRC32", "CRC32"},
    {RuleId::kRowMetadataFull, true, true, "binlog_row_metadata", "binlog_row_metadata",
     RuleMatch::kEquals, "FULL", "FULL"},
    {RuleId::kLogReplicaUpdates, true, true, "log_replica_updates", "log_slave_updates",
     RuleMatch::kEquals, "ON", "ON"},
}};

const ServerSettingRule& GetRule(RuleId id) noexcept {
  return kServerSettingRules[static_cast<size_t>(id)];
}

const char* RuleIdName(RuleId id) noexcept {
  switch (id) {
    case RuleId::kLogBin:
      return "kLogBin";
    case RuleId::kGtidMode:
      return "kGtidMode";
    case RuleId::kBinlogFormat:
      return "kBinlogFormat";
    case RuleId::kBinlogRowImage:
      return "kBinlogRowImage";
    case RuleId::kNoTransactionCompression:
      return "kNoTransactionCompression";
    case RuleId::kNoPartialJson:
      return "kNoPartialJson";
    case RuleId::kNoLogBinCompress:
      return "kNoLogBinCompress";
    case RuleId::kChecksumCrc32:
      return "kChecksumCrc32";
    case RuleId::kRowMetadataFull:
      return "kRowMetadataFull";
    case RuleId::kLogReplicaUpdates:
      return "kLogReplicaUpdates";
  }
  return "kUnknown";
}

size_t EvaluateServerSettings(VariableLookupFn lookup, void* ctx, ServerFlavor flavor,
                              const RuleId* ids, size_t id_count,
                              std::vector<Violation>* violations) {
  size_t added = 0;
  const bool maria = flavor == ServerFlavor::kMariaDB;
  for (size_t i = 0; i < id_count; ++i) {
    const ServerSettingRule& rule = GetRule(ids[i]);
    if (!(maria ? rule.applies_to_mariadb : rule.applies_to_mysql)) {
      continue;
    }
    const char* variable = maria ? rule.mariadb_variable : rule.mysql_variable;
    std::string observed;
    const bool found = lookup(ctx, variable, &observed);
    bool ok = true;
    if (!found) {
      ok = rule.match != RuleMatch::kEquals;
    } else {
      switch (rule.match) {
        case RuleMatch::kEquals:
          ok = EqualsIgnoreCase(observed, rule.value);
          break;
        case RuleMatch::kNotEquals:
          ok = !EqualsIgnoreCase(observed, rule.value);
          break;
        case RuleMatch::kNotContains:
          ok = !ContainsIgnoreCase(observed, rule.value);
          break;
      }
    }
    if (!ok) {
      violations->push_back(
          Violation{rule.id, variable, found ? observed : std::string(), !found, rule.expected});
      ++added;
    }
  }
  return added;
}

}  // namespace mybinlog
