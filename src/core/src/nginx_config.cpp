#include "floserver/core/nginx_config.h"

#include "floserver/core/project_registry.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace floserver::core {

namespace fs = std::filesystem;

namespace {

bool is_unsafe(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u < 0x20 || u == 0x7F || c == '"' || c == '$' || c == '\\';
}

// The upstream for a pool, named by its first port (unique among pools).
std::string upstream_name(const std::vector<Port>& ports) { return "floserver_php_" + std::to_string(ports.front()); }

// The upstream for a project's own server.
std::string app_upstream_name(Port port) { return "floserver_app_" + std::to_string(port); }

std::string port_suffix(Port port, Port usual) {
    return port == usual ? std::string() : ":" + std::to_string(port);
}

class Writer {
public:
    Writer(const NginxSpec& spec, std::string& error) : spec_(spec), error_(error) {}

    std::optional<std::string> write() {
        const auto pid = path(spec_.work_dir / "nginx.pid");
        const auto error_log = path(spec_.work_dir / "logs" / "nginx-error.log");
        const auto mime = path(spec_.nginx_dir / "conf" / "mime.types");
        const auto temp = [this](const char* name) { return path(spec_.work_dir / "temp" / name); };
        const auto body = temp("client_body");
        const auto proxy = temp("proxy");
        const auto fastcgi = temp("fastcgi");
        const auto uwsgi = temp("uwsgi");
        const auto scgi = temp("scgi");
        const auto fastcgi_params = path(spec_.nginx_dir / "conf" / "fastcgi_params");
        if (!pid || !error_log || !mime || !body || !proxy || !fastcgi || !uwsgi || !scgi || !fastcgi_params) {
            return std::nullopt;
        }
        fastcgi_params_ = *fastcgi_params;

        out_ << "# Written by FloServer. It is replaced whenever projects change, so edits here are lost.\n"
             << (spec_.user.empty() ? std::string() : "user " + spec_.user + ";\n")
             << "worker_processes 1;\n"
             << "pid " << *pid << ";\n"
             << "error_log " << *error_log << " warn;\n\n"
             << "events {\n    worker_connections 1024;\n}\n\n"
             << "http {\n"
             << "    include " << *mime << ";\n"
             << "    default_type application/octet-stream;\n"
             << "    server_tokens off;\n"
             // Files change while you work; sendfile can serve a stale copy
             // from a shared folder.
             << "    sendfile off;\n"
             << "    keepalive_timeout 65;\n"
             << "    client_max_body_size 512m;\n"
             << "    server_names_hash_bucket_size 128;\n"
             << "    access_log off;\n"
             << "    client_body_temp_path " << *body << ";\n"
             << "    proxy_temp_path " << *proxy << ";\n"
             << "    fastcgi_temp_path " << *fastcgi << ";\n"
             << "    uwsgi_temp_path " << *uwsgi << ";\n"
             << "    scgi_temp_path " << *scgi << ";\n"
             << "    log_format floserver_idle '$msec';\n";

        const bool any_tls = std::any_of(spec_.sites.begin(), spec_.sites.end(),
                                         [](const NginxSite& s) { return s.tls.has_value(); });
        // One upstream per pool: a php-cgi worker on each port, requests
        // spread over them. A worker that sleeps still has its port held by
        // the service, so a connection never fails; max_fails=0 keeps nginx
        // from setting one aside.
        std::set<Port> written;
        for (const NginxSite& site : spec_.sites) {
            if (site.php_ports.empty() || !written.insert(site.php_ports.front()).second) {
                continue;
            }
            out_ << "\n    upstream " << upstream_name(site.php_ports) << " {\n";
            for (const Port p : site.php_ports) {
                out_ << "        server 127.0.0.1:" << p << " max_fails=0;\n";
            }
            out_ << "    }\n";
        }
        // Projects with their own server: kept-alive connections (closed
        // soon after use, so an idle app can sleep), and the Connection
        // header passed on only for WebSocket upgrades.
        bool any_app = false;
        for (const NginxSite& site : spec_.sites) {
            if (site.app_port) {
                any_app = true;
                out_ << "\n    upstream " << app_upstream_name(*site.app_port) << " {\n"
                     << "        server 127.0.0.1:" << *site.app_port << " max_fails=0;\n"
                     << "        keepalive 8;\n"
                     << "        keepalive_timeout 10s;\n"
                     << "    }\n";
            }
        }
        if (any_app) {
            out_ << "\n    map $http_upgrade $floserver_connection {\n"
                 << "        default upgrade;\n"
                 << "        \"\" \"\";\n"
                 << "    }\n";
        }
        out_ << "\n    # Addresses that aren't a project.\n"
             << "    server {\n";
        listen(spec_.ports.http, " default_server");
        out_ << "        default_type text/plain;\n";
        if (!spec_.config_id.empty()) {
            out_ << "        location = /.floserver/config-id {\n"
                 << "            return 200 \"" << spec_.config_id << "\";\n"
                 << "        }\n";
        }
        out_ << "        location / {\n"
             << "            return 404 \"There's no FloServer project at this address.\\n\";\n"
             << "        }\n"
             << "    }\n";
        if (any_tls) {
            out_ << "    server {\n";
            listen(spec_.ports.https, " ssl default_server");
            out_ << "        ssl_reject_handshake on;\n"
                 << "    }\n";
        }

        for (const NginxSite& site : spec_.sites) {
            if (!write_site(site)) {
                return std::nullopt;
            }
        }
        out_ << "}\n";
        return out_.str();
    }

private:
    std::optional<std::string> path(const fs::path& p) {
        auto q = nginx_quoted_path(p);
        if (!q) {
            const std::u8string u = p.u8string();
            error_ = "nginx can't use the path " + std::string(u.begin(), u.end()) +
                     " (it has a \" or $ or a control character)";
        }
        return q;
    }

    void listen(Port port, const char* extra) {
        out_ << "        listen 127.0.0.1:" << port << extra << ";\n";
        if (spec_.ipv6) {
            out_ << "        listen [::1]:" << port << extra << ";\n";
        }
    }

    bool write_site(const NginxSite& site) {
        if (const auto problem = project_name_problem(site.name)) {
            error_ = "the project name " + site.name + " can't be used: " + *problem;
            return false;
        }
        const auto root = path(site.doc_root);
        if (!root) {
            return false;
        }
        std::optional<std::string> cert;
        std::optional<std::string> key;
        if (site.tls) {
            cert = path(site.tls->certificate);
            key = path(site.tls->key);
            if (!cert || !key) {
                return false;
            }
        }
        std::optional<std::string> idle_log;
        if (!site.php_ports.empty() && !site.php_access_log.empty()) {
            idle_log = path(site.php_access_log);
            if (!idle_log) {
                return false;
            }
        }
        const std::string host = site.name + ".localhost";

        out_ << "\n    # Project " << site.name << "\n";
        if (site.tls) {
            out_ << "    server {\n";
            listen(spec_.ports.http, "");
            out_ << "        server_name " << host << ";\n"
                 << "        return 301 " << site_address(site.name, spec_.ports, true) << "$request_uri;\n"
                 << "    }\n";
        }
        out_ << "    server {\n";
        if (site.tls) {
            listen(spec_.ports.https, spec_.http2_directive ? " ssl" : " ssl http2");
            out_ << (spec_.http2_directive ? "        http2 on;\n" : "")
                 << "        ssl_certificate " << *cert << ";\n"
                 << "        ssl_certificate_key " << *key << ";\n";
        } else {
            listen(spec_.ports.http, "");
        }
        out_ << "        server_name " << host << ";\n";
        if (site.app_port) {
            // The app decides what it serves, dot paths included (Vite
            // serves /node_modules/.vite/).
            out_ << "        location / {\n"
                 << "            proxy_pass http://" << app_upstream_name(*site.app_port) << ";\n"
                 << "            proxy_http_version 1.1;\n"
                 // With the port: the app builds absolute links from it,
                 // and the web ports may be 8080/8443 (Q4).
                 << "            proxy_set_header Host $http_host;\n"
                 << "            proxy_set_header Upgrade $http_upgrade;\n"
                 << "            proxy_set_header Connection $floserver_connection;\n"
                 << "            proxy_set_header X-Forwarded-For $remote_addr;\n"
                 << "            proxy_set_header X-Forwarded-Proto $scheme;\n"
                 << "            proxy_set_header X-Forwarded-Host $http_host;\n"
                 << "            proxy_set_header X-Forwarded-Port $server_port;\n"
                 // Server-sent events and streamed pages arrive as they're sent.
                 << "            proxy_buffering off;\n"
                 // The first request may wait for npm install (up to 10
                 // minutes) and the start.
                 << "            proxy_read_timeout 900s;\n"
                 << "            proxy_send_timeout 900s;\n";
            // Apache projects: every request counts for their PHP pool's idle time.
            if (!site.php_access_log.empty()) {
                const auto idle = path(site.php_access_log);
                if (!idle) {
                    return false;
                }
                out_ << "            access_log " << *idle << " floserver_idle;\n";
            }
            out_ << "        }\n"
                 << "    }\n";
            return true;
        }
        out_ << "        root " << *root << ";\n";
        // Hidden files and folders (.env, .git) are never served. Locations
        // are caseless (~*): on a disk that ignores case, /x.PHP is x.php and
        // must run, never be sent as source.
        out_ << R"(        location ~* /\.(?!well-known/) {
            return 404;
        }
)";
        if (site.php_ports.empty()) {
            out_ << "        index index.html index.htm;\n"
                 << "        location / {\n"
                 << "            try_files $uri $uri/ =404;\n"
                 << "        }\n";
        } else {
            // Front controller: what isn't a file goes to /index.php, as
            // FloCMS, Laravel and WordPress expect.
            out_ << "        index index.php index.html index.htm;\n"
                 << "        location / {\n"
                 << "            try_files $uri $uri/ /index.php$is_args$args;\n"
                 << "        }\n"
                 << R"(        location ~* [^/]\.php(/|$) {
            fastcgi_split_path_info ^(.+?\.php)(/.*)$;
            # try_files empties $fastcgi_path_info, so keep a copy first.
            set $floserver_path_info $fastcgi_path_info;
            try_files $fastcgi_script_name =404;
)"
                 << "            include " << fastcgi_params_ << ";\n"
                 << "            fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name;\n"
                 << "            fastcgi_param PATH_INFO $floserver_path_info;\n";
            if (site.tls) {
                out_ << "            fastcgi_param HTTPS on;\n";
            }
            out_ << "            fastcgi_pass " << upstream_name(site.php_ports) << ";\n"
                 << "            fastcgi_read_timeout 300s;\n";
            if (idle_log) {
                out_ << "            access_log " << *idle_log << " floserver_idle;\n";
            }
            out_ << "        }\n";
        }
        out_ << "    }\n";
        return true;
    }

    const NginxSpec& spec_;
    std::string& error_;
    std::ostringstream out_;
    std::string fastcgi_params_;  // quoted, set before any site is written
};

}  // namespace

std::optional<std::string> nginx_quoted_path(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    std::string text(u.begin(), u.end());
#ifdef _WIN32
    // generic_u8string() already uses '/', but a path built from text may
    // still hold a '\' on the way in.
    std::replace(text.begin(), text.end(), '\\', '/');
#endif
    if (text.empty() || std::any_of(text.begin(), text.end(), is_unsafe)) {
        return std::nullopt;
    }
    return "\"" + text + "\"";
}

std::string site_address(const std::string& name, const WebPorts& ports, bool https) {
    constexpr Port kHttp = 80;
    constexpr Port kHttps = 443;
    return https ? "https://" + name + ".localhost" + port_suffix(ports.https, kHttps)
                 : "http://" + name + ".localhost" + port_suffix(ports.http, kHttp);
}

std::optional<std::string> make_nginx_config(const NginxSpec& spec, std::string& error) {
    Writer writer(spec, error);
    return writer.write();
}

}  // namespace floserver::core
