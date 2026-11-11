#pragma once

// The database engines a project can use (D12, D28): MariaDB and MySQL. Each
// is a "family" (DatabaseSpec::family in project_config.h) whose runtime
// kind in the signed list has the same name ("mariadb", "mysql"). Both speak
// MySQL's protocol and are run by the same database service
// (database_service.h): one server per series, a database and user per
// project, asleep when idle. PostgreSQL (D29) is a family of its own that
// this list will name; what it shares with these two is the series, the
// port rule and the data folder's layout.
//
// A server is known by its family and series: "mysql-8.4" names its data
// folder (<data>/data/mysql-8.4), its secrets ("mysql-8.4/root") and its
// settings' place (<data>/mysql/8.4/custom.ini).

#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

inline constexpr std::string_view kMariaDbFamily = "mariadb";
inline constexpr std::string_view kMySqlFamily = "mysql";

// The families floserver.json's "database" may name, in the order the
// window lists them.
[[nodiscard]] const std::vector<std::string>& database_families();
[[nodiscard]] bool known_database_family(std::string_view family);

// The families the database service runs (MySQL's protocol): MariaDB, MySQL.
[[nodiscard]] bool mysql_protocol_family(std::string_view family);

// "MariaDB", "MySQL"; the name itself for anything else.
[[nodiscard]] std::string database_family_title(std::string_view family);

// "mysql-8.4": a server's data folder, secrets and logs are named after it.
[[nodiscard]] std::string database_server_key(std::string_view family, std::string_view series);

// The family a request names: "mariadb" when it names none (requests from
// before D28), nothing when it names one FloServer doesn't run.
[[nodiscard]] bool request_database_family(std::string_view given, std::string& family);

}  // namespace floserver::core
