#include "floserver/core/apache_config.h"

#include "floserver/core/nginx_config.h"
#include "floserver/core/paths.h"
#include "floserver/core/project_registry.h"

#include <algorithm>
#include <sstream>
#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;

namespace {

// Modules every project gets: what .htaccess files commonly use (rewrites,
// headers, caching, old-style "Order allow,deny", password-protected
// folders) and what FloServer needs (FastCGI to the pools). Built-in ones
// are skipped by the IfModule around each line.
struct Module {
    const char* id;    // as in <IfModule !id>
    const char* file;  // in the modules folder
};
constexpr Module kModules[] = {  // NOLINT(modernize-avoid-c-arrays)
    {"authz_core_module", "mod_authz_core.so"},
    {"authz_host_module", "mod_authz_host.so"},
    {"authz_user_module", "mod_authz_user.so"},
    {"authn_core_module", "mod_authn_core.so"},
    {"authn_file_module", "mod_authn_file.so"},
    {"auth_basic_module", "mod_auth_basic.so"},
    {"access_compat_module", "mod_access_compat.so"},
    {"log_config_module", "mod_log_config.so"},
    {"mime_module", "mod_mime.so"},
    {"dir_module", "mod_dir.so"},
    {"alias_module", "mod_alias.so"},
    {"rewrite_module", "mod_rewrite.so"},
    {"env_module", "mod_env.so"},
    {"setenvif_module", "mod_setenvif.so"},
    {"headers_module", "mod_headers.so"},
    {"expires_module", "mod_expires.so"},
    {"filter_module", "mod_filter.so"},
    {"deflate_module", "mod_deflate.so"},
    {"version_module", "mod_version.so"},
    {"proxy_module", "mod_proxy.so"},
    {"proxy_fcgi_module", "mod_proxy_fcgi.so"},
};

// The last digit of the client's port (the service's relay, a new one for
// each connection), split into `n` groups: how requests are spread over a
// pool's workers. (mod_proxy_balancer would be the usual way, but Apache on
// Windows didn't find its balancers: "No protocol handler was valid".)
std::vector<std::string> digit_groups(std::size_t n) {
    n = std::max<std::size_t>(1, std::min<std::size_t>(n, 10));
    std::vector<std::string> out;
    std::size_t first = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t last = (i + 1) * 10 / n - 1;
        out.push_back("[" + std::to_string(first) + "-" + std::to_string(last) + "]");
        first = last + 1;
    }
    return out;
}

class Writer {
public:
    Writer(const ApacheSpec& spec, std::string& error) : spec_(spec), error_(error) {}

    std::optional<std::string> write() {
        const auto root = path(spec_.server_root);
        // Named after the port it's started on: an Apache still shutting down
        // (another port) never shares them with the next one.
        const auto pid = path(spec_.work_dir / "apache" / ("httpd-" + std::string(kApachePortPlaceholder) + ".pid"));
        const auto log = path(spec_.work_dir / "logs" / "apache-error.log");
        const auto mime = path(spec_.server_root / "conf" / "mime.types");
        const auto empty = path(spec_.work_dir / "apache" / "empty");
        const auto runtime = path(spec_.work_dir / "apache" / ("run-" + std::string(kApachePortPlaceholder)));
        if (!root || !pid || !log || !mime || !empty || !runtime) {
            return std::nullopt;
        }
        out_ << "# Written by FloServer. It is replaced whenever projects change, so edits here are lost.\n"
             << "ServerRoot " << *root << "\n"
             << "Listen 127.0.0.1:" << kApachePortPlaceholder << "\n"
             << "ServerName localhost\n"
             << "PidFile " << *pid << "\n"
             // Locks and the balancer's shared memory: ours, not a system folder.
             << "DefaultRuntimeDir " << *runtime << "\n"
             << "ErrorLog " << *log << "\n"
             << "LogLevel warn\n";
        if (spec_.posix) {
            if (!load("mpm_event_module", "mod_mpm_event.so") || !load("unixd_module", "mod_unixd.so")) {
                return std::nullopt;
            }
        }
        for (const Module& m : kModules) {
            if (!load(m.id, m.file)) {
                return std::nullopt;
            }
        }
        out_ << "TypesConfig " << *mime << "\n"
             << "DirectoryIndex index.php index.html index.htm\n"
             << "AccessFileName .htaccess\n"
             << "UseCanonicalName Off\n"
             // Files change while you work; a shared folder may serve a
             // stale copy through these.
             << "EnableSendfile Off\n"
             << "EnableMMAP Off\n"
             << "Timeout 300\n"
             // Longer than nginx keeps an idle connection to us (10 s), so
             // nginx never sends a request on one Apache just closed.
             << "KeepAliveTimeout 15\n"
             << "ProxyTimeout 300\n"
             << "LimitRequestBody 0\n"
             << "ServerTokens Prod\n"
             << "ServerSignature Off\n"
             // Only 1 or 2 processes and a few threads: a few MB, and plenty for one person.
             << "ThreadsPerChild 25\n"
             << "<IfModule mpm_event_module>\n"
             << "    StartServers 1\n"
             << "    ServerLimit 2\n"
             << "    MaxRequestWorkers 50\n"
             << "    MinSpareThreads 5\n"
             << "    MaxSpareThreads 30\n"
             << "</IfModule>\n"
             << "<Directory />\n"
             << "    AllowOverride None\n"
             << "    Require all denied\n"
             << "</Directory>\n"
             // nginx says whether it received the request over https.
             << "SetEnvIf X-Forwarded-Proto \"^https$\" HTTPS=on\n"
             // Hidden files and folders (.env, .git, .htaccess) are never
             // served: by the address, and by the file's real name too (on
             // Windows /ENV~1 is .env).
             << "RedirectMatch 404 \"/\\.(?!well-known/)\"\n"
             << "<FilesMatch \"^\\.\">\n"
             << "    Require all denied\n"
             << "</FilesMatch>\n"
             << "<DirectoryMatch \"/\\.(?!well-known(/|$))\">\n"
             << "    Require all denied\n"
             << "</DirectoryMatch>\n";

        // Addresses that aren't a project.
        out_ << "<VirtualHost 127.0.0.1:" << kApachePortPlaceholder << ">\n"
             << "    ServerName floserver-default.invalid\n"
             << "    DocumentRoot " << *empty << "\n"
             << "    <Location \"/\">\n"
             << "        Require all denied\n"
             << "    </Location>\n"
             << "</VirtualHost>\n";
        for (const ApacheSite& site : spec_.sites) {
            if (!write_site(site)) {
                return std::nullopt;
            }
        }
        return out_.str();
    }

private:
    std::optional<std::string> path(const fs::path& p) {
        auto q = nginx_quoted_path(p);  // the same rules: quoted, '/', no '"', '$', '\' or control characters
        if (!q) {
            error_ = "Apache can't use the path " + path_to_utf8(p) + " (it has a \" or $ or a control character)";
        }
        return q;
    }

    bool load(const char* id, const char* file) {
        const auto p = path(spec_.modules_dir / file);
        if (!p) {
            return false;
        }
        out_ << "<IfModule !" << id << ">\n"
             << "    LoadModule " << id << " " << *p << "\n"
             << "</IfModule>\n";
        return true;
    }

    bool write_site(const ApacheSite& site) {
        if (const auto problem = project_name_problem(site.name)) {
            error_ = "the project name " + site.name + " can't be used: " + *problem;
            return false;
        }
        const auto root = path(site.doc_root);
        if (!root) {
            return false;
        }
        // The root also goes into a regex between \Q and \E with | around it.
        if (root->find('|') != std::string::npos) {
            error_ = "Apache can't use the path " + path_to_utf8(site.doc_root) + " (it has a |)";
            return false;
        }
        const std::string bare = root->substr(1, root->size() - 2);  // without the quotes
        out_ << "\n# Project " << site.name << "\n"
             << "<VirtualHost 127.0.0.1:" << kApachePortPlaceholder << ">\n"
             // With the scheme: Apache's own redirects (a folder's missing
             // '/', [R] rewrites) then say https, on 8443 too.
             << "    ServerName " << (site.https ? "https://" : "") << site.name << ".localhost\n"
             << "    DocumentRoot " << *root << "\n"
             << "    <Directory " << *root << ">\n"
             << "        Options FollowSymLinks\n"
             << "        AllowOverride All\n"
             << "        Require all granted\n"
             // Authorization reaches PHP, as with nginx (REST API tokens).
             << "        CGIPassAuth On\n"
             << "    </Directory>\n";
        if (!site.php_ports.empty()) {
            // Caseless: on a disk that ignores case, x.PHP is x.php and must
            // run, never be sent as source. Only files that exist go to PHP.
            out_ << "    <FilesMatch \"(?i)\\.php$\">\n";
            const auto groups = digit_groups(site.php_ports.size());
            for (std::size_t i = 0; i < groups.size(); ++i) {
                const bool last = i + 1 == groups.size();
                out_ << "        <" << (i == 0 ? "If" : "ElseIf") << " \"-f %{REQUEST_FILENAME}"
                     << (last ? std::string() : " && %{REMOTE_PORT} =~ /" + groups[i] + "$/") << "\">\n"
                     // The '/': Apache adds the script's path, which on Windows
                     // starts with a drive letter ("D:/..."), not '/'.
                     << "            SetHandler \"proxy:fcgi://127.0.0.1:" << site.php_ports[i] << "/\"\n"
                     << "        </" << (i == 0 ? "If" : "ElseIf") << ">\n";
            }
            out_ << "    </FilesMatch>\n"
                 // php-cgi takes the script's real path (GENERIC), and the
                 // CGI names as a web server gives them, which mod_proxy_fcgi
                 // gets wrong after an .htaccess rewrite: PATH_TRANSLATED is
                 // "redirect:...", PATH_INFO the script's own path,
                 // SCRIPT_NAME empty or with the path info in it.
                 << "    ProxyFCGIBackendType GENERIC\n"
                 << R"x(    ProxyFCGISetEnvIf "true" !PATH_TRANSLATED)x" << "\n"
                 // php-cgi moves PATH_INFO aside when REDIRECT_URL is set
                 // (rewrites to index.php/$1); nginx doesn't send it either.
                 << R"x(    ProxyFCGISetEnvIf "true" !REDIRECT_URL)x" << "\n"
                 << R"x(    ProxyFCGISetEnvIf "reqenv('PATH_INFO') == reqenv('SCRIPT_FILENAME')" !PATH_INFO)x" << "\n"
                 << R"x(    ProxyFCGISetEnvIf "reqenv('SCRIPT_NAME') =~ m#^/.*?\\.php(/.*)$#i" PATH_INFO "$1")x" << "\n"
                 << R"x(    ProxyFCGISetEnvIf "reqenv('SCRIPT_FILENAME') =~ m|^\\Q)x" << bare
                 << R"x(\\E(/.*)$|i" SCRIPT_NAME "$1")x" << "\n";
        }
        out_ << "</VirtualHost>\n";
        return true;
    }

    const ApacheSpec& spec_;
    std::string& error_;
    std::ostringstream out_;
};

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

}  // namespace

std::optional<std::string> make_apache_config(const ApacheSpec& spec, std::string& error) {
    Writer writer(spec, error);
    return writer.write();
}

fs::path apache_server_root(const fs::path& apache_dir) {
    std::error_code ec;
    return fs::is_directory(apache_dir / "Apache24", ec) ? apache_dir / "Apache24" : apache_dir;
}

std::optional<fs::path> apache_program(const fs::path& apache_dir) {
    const fs::path root = apache_server_root(apache_dir);
#ifdef _WIN32
    const char* names[] = {"bin/httpd.exe"};  // NOLINT(modernize-avoid-c-arrays)
#else
    const char* names[] = {"bin/httpd", "bin/apache2", "apache2"};  // NOLINT(modernize-avoid-c-arrays)
#endif
    for (const char* n : names) {
        fs::path p = root / path_from_utf8(n);
        if (is_file(p)) {
            return p;
        }
    }
    return std::nullopt;
}

}  // namespace floserver::core
