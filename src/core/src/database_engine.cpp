#include "floserver/core/database_engine.h"

#include <algorithm>

namespace floserver::core {

const std::vector<std::string>& database_families() {
    static const std::vector<std::string> families = {std::string(kMariaDbFamily), std::string(kMySqlFamily)};
    return families;
}

bool known_database_family(std::string_view family) {
    const auto& all = database_families();
    return std::find(all.begin(), all.end(), family) != all.end();
}

bool mysql_protocol_family(std::string_view family) { return family == kMariaDbFamily || family == kMySqlFamily; }

std::string database_family_title(std::string_view family) {
    if (family == kMariaDbFamily) {
        return "MariaDB";
    }
    if (family == kMySqlFamily) {
        return "MySQL";
    }
    return std::string(family);
}

std::string database_server_key(std::string_view family, std::string_view series) {
    return std::string(family).append("-").append(series);
}

bool request_database_family(std::string_view given, std::string& family) {
    if (given.empty()) {
        family = std::string(kMariaDbFamily);
        return true;
    }
    if (!mysql_protocol_family(given)) {
        return false;
    }
    family = std::string(given);
    return true;
}

}  // namespace floserver::core
