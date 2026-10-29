/**
 * @file server_settings.h
 * @brief Static server-variable rules a binlog reader depends on.
 *
 * The rules cover only values that can be read once from `SHOW VARIABLES`.
 * Dynamic checks (purged GTIDs, failover, purge boundaries) and the query
 * transport stay with the caller: it supplies a lookup function and chooses
 * which rule ids apply to it.
 */

#ifndef MYBINLOG_SERVER_SETTINGS_H_
#define MYBINLOG_SERVER_SETTINGS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mybinlog/export.h"
#include "mybinlog/server_flavor.h"

namespace mybinlog {

/** @brief Identifier of one rule in kServerSettingRules. */
enum class RuleId : uint8_t {
  kLogBin,                    ///< log_bin = ON (both flavors)
  kGtidMode,                  ///< gtid_mode = ON (MySQL)
  kBinlogFormat,              ///< binlog_format = ROW (both)
  kBinlogRowImage,            ///< binlog_row_image = FULL (both)
  kNoTransactionCompression,  ///< binlog_transaction_compression != ON (MySQL)
  kNoPartialJson,             ///< binlog_row_value_options lacks PARTIAL_JSON (MySQL)
  kNoLogBinCompress,          ///< log_bin_compress != ON (MariaDB)
  kChecksumCrc32,             ///< binlog_checksum = CRC32 (both)
  kRowMetadataFull,           ///< binlog_row_metadata = FULL (both)
  kLogReplicaUpdates,         ///< log_replica_updates (MySQL) / log_slave_updates (MariaDB) = ON
};

/** @brief Number of RuleId enumerators; the length of kServerSettingRules. */
inline constexpr size_t kRuleCount = 10;

/** @brief How a variable's value is judged. All comparisons ignore ASCII case. */
enum class RuleMatch : uint8_t {
  kEquals,       ///< Value must equal `value`; an absent variable is a violation.
  kNotEquals,    ///< Value must not equal `value`; an absent variable passes.
  kNotContains,  ///< Value must not contain `value` as a substring; an absent variable passes.
};

/** @brief One static rule. */
struct ServerSettingRule {
  RuleId id = RuleId::kLogBin;      ///< This rule's identifier.
  bool applies_to_mysql = false;    ///< Evaluated against MySQL sources.
  bool applies_to_mariadb = false;  ///< Evaluated against MariaDB sources.
  /** Variable queried on MySQL; null if the rule is MariaDB-only. */
  const char* mysql_variable = nullptr;
  /** Variable queried on MariaDB; null if the rule is MySQL-only. */
  const char* mariadb_variable = nullptr;
  RuleMatch match = RuleMatch::kEquals;  ///< How the value is judged.
  const char* value = "";                ///< Comparison operand (e.g. "ON", "PARTIAL_JSON").
  const char* expected = "";             ///< Human-readable requirement for messages.
};

/**
 * @brief Every rule, in RuleId order: `kServerSettingRules[i].id == static_cast<RuleId>(i)`.
 *
 * Immutable; safe to read from any thread.
 */
extern MYBINLOG_API const std::array<ServerSettingRule, kRuleCount> kServerSettingRules;

/**
 * @brief Look up one rule.
 * @param id A RuleId enumerator.
 * @return The rule in kServerSettingRules. Reentrant.
 */
[[nodiscard]] MYBINLOG_API const ServerSettingRule& GetRule(RuleId id) noexcept;

/**
 * @brief Stable identifier of a rule, for logs and messages.
 * @param id Any value, including one outside the enumerators.
 * @return Static string such as "kLogBin" ("kUnknown" outside the enumerators); never
 *   null. Reentrant.
 */
[[nodiscard]] MYBINLOG_API const char* RuleIdName(RuleId id) noexcept;

/**
 * @brief Read one server variable.
 *
 * @param ctx Caller context passed through EvaluateServerSettings().
 * @param variable Variable name, e.g. "log_bin"; valid only during the call.
 * @param value Receives the value when the variable exists.
 * @return true when the variable exists, false when absent.
 */
using VariableLookupFn = bool (*)(void* ctx, std::string_view variable, std::string* value);

/** @brief A rule the server's settings do not satisfy. */
struct Violation {
  RuleId id = RuleId::kLogBin;  ///< The rule that failed.
  std::string variable;         ///< Variable that was checked (flavor-specific name).
  std::string observed;         ///< Value read; empty when absent.
  bool absent = false;          ///< The variable does not exist on the server.
  const char* expected = "";    ///< Static requirement text from the rule.
};

/**
 * @brief Evaluate the selected rules against a server.
 *
 * Rules that do not apply to @p flavor are skipped. Violations are appended to
 * @p violations in the order of @p ids.
 *
 * @param lookup Variable reader.
 * @param ctx Passed to @p lookup.
 * @param flavor Server flavor.
 * @param ids Rules to evaluate.
 * @param id_count Number of entries in @p ids.
 * @param violations Appended to; must not be null.
 * @return Number of violations appended. Reentrant when @p lookup is.
 */
MYBINLOG_API size_t EvaluateServerSettings(VariableLookupFn lookup, void* ctx, ServerFlavor flavor,
                                           const RuleId* ids, size_t id_count,
                                           std::vector<Violation>* violations);

}  // namespace mybinlog

#endif  // MYBINLOG_SERVER_SETTINGS_H_
