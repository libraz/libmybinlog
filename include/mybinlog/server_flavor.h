/**
 * @file server_flavor.h
 * @brief MySQL / MariaDB flavor and the two ways products detect it.
 *
 * DetectFlavorFromVersion() reads a handshake or VERSION() string.
 * ClassifyFlavorProbe() reads the outcome of `SELECT @@GLOBAL.gtid_binlog_pos`,
 * a variable only MariaDB has, and fails closed on anything unexpected.
 */

#ifndef MYBINLOG_SERVER_FLAVOR_H_
#define MYBINLOG_SERVER_FLAVOR_H_

#include <cstdint>
#include <optional>
#include <string_view>

#include "mybinlog/export.h"

namespace mybinlog {

/** @brief Database server flavor. */
enum class ServerFlavor : uint8_t {
  kMySQL,    ///< MySQL (including Percona Server).
  kMariaDB,  ///< MariaDB.
};

/**
 * @brief Display name of a flavor.
 * @param flavor Any value, including one outside the enumerators.
 * @return "MySQL", "MariaDB", or "Unknown"; never null. Reentrant.
 */
[[nodiscard]] MYBINLOG_API const char* ServerFlavorName(ServerFlavor flavor) noexcept;

/**
 * @brief Flavor from a server version string.
 *
 * MariaDB versions contain "MariaDB" (or "mariadb"); anything else, including
 * an empty string, is MySQL.
 *
 * @param version Handshake or `VERSION()` text; only read during the call.
 * @return The detected flavor. Reentrant.
 */
[[nodiscard]] MYBINLOG_API ServerFlavor DetectFlavorFromVersion(std::string_view version) noexcept;

/** @brief MySQL ER_UNKNOWN_SYSTEM_VARIABLE, the probe's answer on MySQL. */
inline constexpr unsigned int kErUnknownSystemVariable = 1193;

/**
 * @brief Classify the `@@GLOBAL.gtid_binlog_pos` probe.
 *
 * @param query_succeeded The query executed.
 * @param result_read_succeeded Its result was read (only meaningful on success).
 * @param error_number Server error number when the query failed.
 * @return kMariaDB when the query and its result succeeded; kMySQL when the
 *   query failed with kErUnknownSystemVariable; std::nullopt otherwise.
 *   Reentrant.
 */
[[nodiscard]] MYBINLOG_API std::optional<ServerFlavor> ClassifyFlavorProbe(
    bool query_succeeded, bool result_read_succeeded, unsigned int error_number) noexcept;

}  // namespace mybinlog

#endif  // MYBINLOG_SERVER_FLAVOR_H_
