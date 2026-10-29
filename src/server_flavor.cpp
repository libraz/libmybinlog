#include "mybinlog/server_flavor.h"

namespace mybinlog {

const char* ServerFlavorName(ServerFlavor flavor) noexcept {
  switch (flavor) {
    case ServerFlavor::kMySQL:
      return "MySQL";
    case ServerFlavor::kMariaDB:
      return "MariaDB";
  }
  return "Unknown";
}

ServerFlavor DetectFlavorFromVersion(std::string_view version) noexcept {
  if (version.find("MariaDB") != std::string_view::npos ||
      version.find("mariadb") != std::string_view::npos) {
    return ServerFlavor::kMariaDB;
  }
  return ServerFlavor::kMySQL;
}

std::optional<ServerFlavor> ClassifyFlavorProbe(bool query_succeeded, bool result_read_succeeded,
                                                unsigned int error_number) noexcept {
  if (query_succeeded) {
    if (result_read_succeeded) return ServerFlavor::kMariaDB;
    return std::nullopt;
  }
  if (error_number == kErUnknownSystemVariable) return ServerFlavor::kMySQL;
  return std::nullopt;
}

}  // namespace mybinlog
