#include "floserver/core/mariadb.h"

#include "floserver/core/mariadb_settings.h"
#include "floserver/core/paths.h"
#include "floserver/core/version.h"

#include <algorithm>
#include <array>
#include <set>
#include <sstream>
#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxConfigLine = 1100;  // a 64-byte name, '=', a 1000-byte value

#ifdef _WIN32
constexpr const char* kExe = ".exe";
#else
constexpr const char* kExe = "";
#endif

fs::path first_existing(const std::vector<fs::path>& folders, const std::vector<std::string>& names) {
    for (const fs::path& f : folders) {
        for (const std::string& n : names) {
            std::error_code ec;
            const fs::path p = f / (n + kExe);
            if (fs::is_regular_file(p, ec)) {
                return p;
            }
        }
    }
    return folders.front() / (names.front() + kExe);
}

std::string option_path(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return "\"" + std::string(u.begin(), u.end()) + "\"";
}

std::string trimmed(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string_view::npos) {
        return {};
    }
    const auto e = s.find_last_not_of(" \t\r");
    return std::string(s.substr(b, e - b + 1));
}

// The key of a KEY=value line, or empty for a comment or other line.
std::string env_key(std::string_view line) {
    const std::string t = trimmed(line);
    if (t.empty() || t.front() == '#') {
        return {};
    }
    const auto eq = t.find('=');
    std::string key = eq == std::string::npos ? std::string() : trimmed(std::string_view(t).substr(0, eq));
    if (key.starts_with("export ") || key.starts_with("export\t")) {
        key = trimmed(std::string_view(key).substr(7));  // shell style: export KEY=value
    }
    return key;
}

}  // namespace

std::string mariadb_series(std::string_view version) {
    const auto v = Version::parse(version);
    if (!v || v->parts().size() < 2) {
        return {};
    }
    return std::to_string(v->parts()[0]) + "." + std::to_string(v->parts()[1]);
}

MariaDbFiles mariadb_files(const fs::path& install_dir, const std::string& version, const fs::path& data) {
    MariaDbFiles f;
    f.series = mariadb_series(version);
    f.base_dir = install_dir;
    const fs::path bin = install_dir / "bin";
    f.server = first_existing({bin, install_dir / "sbin"}, {"mariadbd", "mysqld"});
    f.install_db = first_existing({bin, install_dir / "scripts"}, {"mariadb-install-db", "mysql_install_db"});
    f.admin = first_existing({bin}, {"mariadb-admin", "mysqladmin"});
    f.client = first_existing({bin}, {"mariadb", "mysql"});
    f.folder = data / "data" / path_from_utf8("mariadb-" + f.series);
    f.data_dir = f.folder / "data";
#ifdef _WIN32
    f.config = f.folder / "my.ini";
#else
    f.config = f.folder / "my.cnf";
    f.socket = f.data_dir / "mysqld.sock";
#endif
    f.error_log = f.data_dir / "mariadbd.err";
    f.pid_file = f.data_dir / "mariadbd.pid";
    f.init_file = f.data_dir / "init.sql";
    f.installed_marker = f.folder / "installed";
    return f;
}

std::string mariadb_config(const MariaDbFiles& files, Port port, bool as_root, bool with_init_file,
                           const MariaDbExtras* extras) {
#ifdef _WIN32
    const char* nl = "\r\n";
#else
    const char* nl = "\n";
#endif
    std::ostringstream c;
    c << "# Written by FloServer for its MariaDB " << files.series << ". It is rewritten at every start." << nl
      << "[mysqld]" << nl;
    // A development server: small memory, no name lookups (slow on some
    // networks), utf8mb4 everywhere. Settings > Database may change these.
    for (const auto& [key, value] : mariadb_ini_defaults()) {
        c << key << "=" << value << nl;
    }
    c << "skip-name-resolve" << nl;
    if (extras != nullptr && !extras->ini.empty()) {
        c << nl << "# Settings > Database (config.json)" << nl;
        for (const auto& [key, value] : extras->ini) {
            c << key << "=" << value << nl;
        }
    }
    if (extras != nullptr && !extras->custom_ini.empty()) {
        c << nl << "# Your own lines (custom.ini)" << nl;
        const std::string& text = extras->custom_ini;
        std::size_t pos = 0;
        while (pos < text.size()) {
            const auto end = text.find('\n', pos);
            const std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? text.size() : end + 1;
            // MariaDB reads 4 KB at a time: a longer line's end would be a line of its own.
            if (line.size() <= kMaxConfigLine) {
                c << line << nl;
            }
        }
    }
    // FloServer's own lines come last, in a group of their own, so nothing
    // above (a [group] in custom.ini included) changes them.
    c << nl << "# Set by FloServer: where MariaDB keeps its files, and 127.0.0.1 only on FloServer's port." << nl
      << "[mysqld]" << nl
#ifndef _WIN32
      // On Windows mariadbd finds its own folder; a full path there would
      // be read in the ANSI code page (a folder name in Arabic breaks).
      << "basedir=" << option_path(files.base_dir) << nl
#endif
      // A full path (a relative one is taken from basedir); the files
      // below are relative to it. MariaDB 10.6+ on Windows runs with UTF-8
      // as its code page, so a non-ASCII folder is read correctly.
      << "datadir=" << option_path(files.data_dir) << nl
      << "port=" << port << nl
      << "bind-address=127.0.0.1" << nl
      << "pid-file=" << path_to_utf8(files.pid_file.filename()) << nl
      << "log-error=" << path_to_utf8(files.error_log.filename()) << nl;
    if (with_init_file) {
        c << "init-file=" << path_to_utf8(files.init_file.filename()) << nl;
    }
    if (!files.socket.empty()) {
        // Relative: mariadbd resolves it in its data folder. A full path
        // longer than 107 bytes (a long home folder) stops it from starting.
        // FloServer itself always connects over TCP.
        c << "socket=" << path_to_utf8(files.socket.filename()) << nl;
    }
    if (as_root) {
        c << "user=root" << nl;
    }
    // Never without passwords, and reached only through FloServer's port.
    c << "skip-grant-tables=0" << nl << "skip-networking=0" << nl;
#ifdef _WIN32
    // loose-: a build without one of these (MariaDB on Windows has no
    // shared-memory option) warns and starts, instead of refusing to start.
    c << "loose-named-pipe=0" << nl << "loose-shared-memory=0" << nl;
#endif
    c << nl << "[client]" << nl << "port=" << port << nl;
    return c.str();
}

std::vector<std::string> mariadb_command_line_guards(const MariaDbFiles& files, Port port) {
    // Options on the command line win over every option file.
    std::vector<std::string> args = {"--port=" + std::to_string(port), "--bind-address=127.0.0.1",
                                     "--skip-grant-tables=0", "--skip-networking=0"};
#ifdef _WIN32
    // --loose-: as above; "unknown variable 'shared-memory=0'" stopped
    // MariaDB 11.8 starting on Windows (found on a real PC, 1 Nov).
    args.emplace_back("--loose-named-pipe=0");
    args.emplace_back("--loose-shared-memory=0");
#endif
    if (!files.socket.empty()) {
        args.push_back("--socket=" + path_to_utf8(files.socket.filename()));
    }
    return args;
}

bool mariadb_initialized(const MariaDbFiles& files) {
    std::error_code ec;
    return fs::is_directory(files.data_dir / "mysql", ec) && fs::exists(files.installed_marker, ec);
}

std::string sql_identifier(std::string_view name) {
    std::string out = "`";
    for (const char c : name) {
        out += c;
        if (c == '`') {
            out += '`';
        }
    }
    return out + "`";
}

std::string sql_string(std::string_view text) {
    std::string out = "'";
    for (const char c : text) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\'') {
            out += "''";
        } else {
            out += c;
        }
    }
    return out + "'";
}

std::string root_password_sql(const std::string& password) {
    const std::string value = sql_string(password);
    std::string sql;
    for (const char* host : {"localhost", "127.0.0.1", "::1"}) {
        sql += "ALTER USER IF EXISTS 'root'@'" + std::string(host) + "' IDENTIFIED BY " + value + ";\n";
    }
    // Only those three root accounts (install-db also makes one for this
    // computer's name), and never anonymous logins (an empty user matches
    // before a named one).
    sql += "DELETE FROM mysql.global_priv WHERE (User = 'root' AND Host NOT IN ('localhost', '127.0.0.1', '::1')) OR User = '';\n";
    sql += "DELETE FROM mysql.db WHERE User = '';\n";
    return sql + "FLUSH PRIVILEGES;\n";
}

std::string grant_pattern(std::string_view database) {
    std::string escaped;
    for (const char c : database) {
        if (c == '_' || c == '%') {
            escaped += '\\';
        }
        escaped += c;
    }
    return sql_identifier(escaped);
}

std::string project_database_sql(const std::string& database, const std::string& user, const std::string& password) {
    const std::string db = sql_identifier(database);
    const std::string pw = sql_string(password);
    std::string sql = "CREATE DATABASE IF NOT EXISTS " + db + " CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;\n";
    for (const char* host : {"localhost", "127.0.0.1"}) {
        const std::string account = sql_string(user) + "@'" + host + "'";
        sql.append("CREATE USER IF NOT EXISTS ").append(account).append(" IDENTIFIED BY ").append(pw).append(";\n");
        sql.append("ALTER USER ").append(account).append(" IDENTIFIED BY ").append(pw).append(";\n");
        // In a GRANT, '_' and '%' in the name are wildcards: escaped, so
        // my_shop's user gets my_shop only, not myashop or another project's
        // database. The unescaped grant earlier versions made is removed.
        sql.append("DELETE FROM mysql.db WHERE User = ").append(sql_string(user)).append(" AND Host = '").append(host)
            .append("' AND Db = ").append(sql_string(database)).append(";\n");
        sql.append("FLUSH PRIVILEGES;\n");
        sql.append("GRANT ALL PRIVILEGES ON ").append(grant_pattern(database)).append(".* TO ").append(account).append(";\n");
    }
    return sql + "FLUSH PRIVILEGES;\n";
}

std::string shared_database_sql(const std::string& password) {
    const std::string user(kSharedDatabaseUser);
    const std::string pw = sql_string(password);
    // The pattern's '_' is escaped so it means an underscore, not any character.
    const std::string pattern = "`" + std::string(kSharedDatabaseUser) + "\\_%`";
    std::string sql = "CREATE DATABASE IF NOT EXISTS " + sql_identifier(user) +
                      " CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;\n";
    for (const char* host : {"localhost", "127.0.0.1"}) {
        const std::string account = sql_string(user) + "@'" + host + "'";
        sql.append("CREATE USER IF NOT EXISTS ").append(account).append(" IDENTIFIED BY ").append(pw).append(";\n");
        sql.append("ALTER USER ").append(account).append(" IDENTIFIED BY ").append(pw).append(";\n");
        sql.append("GRANT ALL PRIVILEGES ON ").append(grant_pattern(user)).append(".* TO ").append(account).append(";\n");
        sql.append("GRANT ALL PRIVILEGES ON ").append(pattern).append(".* TO ").append(account).append(";\n");
    }
    return sql + "FLUSH PRIVILEGES;\n";
}

std::string project_database_name(std::string_view project) {
    std::string out(project);
    std::replace(out.begin(), out.end(), '-', '_');
    return out;
}

bool reserved_database_name(std::string_view name) {
    static constexpr std::array<std::string_view, 8> kReserved = {
        "root", "mysql", "sys", "performance_schema", "information_schema", "test", "mariadb_sys", "public"};
    return std::find(kReserved.begin(), kReserved.end(), name) != kReserved.end() || name == kSharedDatabaseUser ||
           name.starts_with(kSharedDatabasePrefix);
}

EnvStyle env_style(const fs::path& project_dir) {
    for (const char* name : {".env", ".env.example"}) {
        const auto text = read_small_file(project_dir / name, std::size_t{256} * 1024);
        if (!text) {
            continue;
        }
        std::istringstream lines(*text);
        std::string line;
        bool laravel = false;
        bool flocms = false;
        while (std::getline(lines, line)) {
            const std::string key = env_key(line);
            laravel = laravel || key == "DB_DATABASE" || key == "DB_CONNECTION";
            flocms = flocms || key == "DB_NAME" || key == "DB_TYPE";
        }
        if (laravel != flocms) {
            return laravel ? EnvStyle::Laravel : EnvStyle::FloCms;
        }
    }
    return EnvStyle::FloCms;
}

std::vector<std::pair<std::string, std::string>> env_values(EnvStyle style, const DatabaseLogin& login) {
    const bool laravel = style == EnvStyle::Laravel;
    return {{laravel ? "DB_CONNECTION" : "DB_TYPE", "mysql"},
            {"DB_HOST", "127.0.0.1"},
            {"DB_PORT", std::to_string(login.port)},
            {laravel ? "DB_DATABASE" : "DB_NAME", login.database},
            {"DB_USERNAME", login.user},
            {"DB_PASSWORD", login.password}};
}

std::string database_url(const DatabaseLogin& login, bool django) {
    const auto encode = [](const std::string& s) {
        static constexpr char kHex[] = "0123456789ABCDEF";  // NOLINT(modernize-avoid-c-arrays)
        std::string out;
        for (const char c : s) {
            const auto u = static_cast<unsigned char>(c);
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                c == '_' || c == '~') {
                out += c;
            } else {
                out += '%';
                out += kHex[u >> 4U];
                out += kHex[u & 0xFU];
            }
        }
        return out;
    };
    return std::string(django ? "mysql://" : "mysql+pymysql://") + encode(login.user) + ":" + encode(login.password) + "@127.0.0.1:" +
           std::to_string(login.port) + "/" + encode(login.database);
}

std::string update_env_text(const std::string& text, const std::vector<std::pair<std::string, std::string>>& values) {
    const std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::map<std::string, std::string> wanted(values.begin(), values.end());
    std::set<std::string> done;
    std::string out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto end = text.find('\n', pos);
        const bool last = end == std::string::npos;
        std::string line = text.substr(pos, last ? std::string::npos : end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        pos = last ? text.size() : end + 1;
        const std::string key = env_key(line);
        if (const auto it = wanted.find(key); !key.empty() && it != wanted.end()) {
            if (done.insert(key).second) {
                out.append(key).append("=").append(it->second).append(nl);
            }
            continue;  // a later line with the same key: dropped
        }
        out += line;
        if (!last) {
            out += nl;
        }
    }
    if (!out.empty() && !out.ends_with(nl)) {
        out += nl;
    }
    for (const auto& [key, value] : values) {
        if (!done.contains(key)) {
            out.append(key).append("=").append(value).append(nl);
        }
    }
    return out;
}

}  // namespace floserver::core
