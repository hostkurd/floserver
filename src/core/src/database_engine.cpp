#include "floserver/core/database_engine.h"

#include "floserver/core/mariadb.h"
#include "floserver/core/version.h"

#include <algorithm>

namespace floserver::core {

const std::vector<std::string>& database_families() {
    static const std::vector<std::string> families = {std::string(kMariaDbFamily), std::string(kMySqlFamily),
                                                      std::string(kPostgreSqlFamily)};
    return families;
}

bool known_database_family(std::string_view family) {
    const auto& all = database_families();
    return std::find(all.begin(), all.end(), family) != all.end();
}

bool mysql_protocol_family(std::string_view family) { return family == kMariaDbFamily || family == kMySqlFamily; }

bool database_service_family(std::string_view family) {
    return mysql_protocol_family(family) || family == kPostgreSqlFamily;
}

std::string database_series(std::string_view family, std::string_view version) {
    if (family != kPostgreSqlFamily) {
        return mariadb_series(version);
    }
    const auto v = Version::parse(version);
    if (!v || version.empty() || version.front() < '1' || version.front() > '9') {
        return {};
    }
    return std::string(version.substr(0, version.find('.')));
}

std::string_view database_series_example(std::string_view family) {
    if (family == kMySqlFamily) {
        return "8.4";
    }
    return family == kPostgreSqlFamily ? "18" : "11.4";
}

std::string database_family_title(std::string_view family) {
    if (family == kMariaDbFamily) {
        return "MariaDB";
    }
    if (family == kMySqlFamily) {
        return "MySQL";
    }
    if (family == kPostgreSqlFamily) {
        return "PostgreSQL";
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
    if (!database_service_family(given)) {
        return false;
    }
    family = std::string(given);
    return true;
}

}  // namespace floserver::core
