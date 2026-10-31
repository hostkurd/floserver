// Settings → Apache: config.json's "apache", the modules the config loads
// (order, what each needs), custom.conf's filter, and the service's
// apache.get / apache.set. Apache itself runs in test_web_service.cpp.

#include "floserver/core/apache_config.h"
#include "floserver/core/apache_settings.h"
#include "floserver/core/paths.h"
#include "floserver/core/project_service.h"
#include "floserver/core/runtime_store.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <set>

using namespace floserver::core;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::ptrdiff_t index_of(const std::vector<std::string>& list, const std::string& name) {
    const auto it = std::find(list.begin(), list.end(), name);
    return it == list.end() ? -1 : it - list.begin();
}

bool has_problem(const FilteredApacheConf& f, const std::string& start, const std::string& words) {
    return std::any_of(f.problems.begin(), f.problems.end(), [&](const std::string& p) {
        return p.starts_with(start) && p.find(words) != std::string::npos;
    });
}

// Each line FloServer would write, without its own comments.
std::vector<std::string> directives(const std::string& text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? text.size() : end + 1;
        if (!line.empty() && line.front() != '#') {
            out.push_back(line);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("config.json's apache is read and written, keeping what it doesn't know") {
    AppConfig app;
    app.other = json::parse(R"({"php": {"8.5": {}}, "apache": {"version": "2.4.69", "later": [1],
        "modules": {"expires": false, "cache": true, "Bad Name": true, "headers": "yes"}}})");
    std::vector<std::string> problems;
    ApacheSettings s = read_apache_settings(app, &problems);
    CHECK(s.version == "2.4.69");
    CHECK(s.modules == std::map<std::string, bool>{{"cache", true}, {"expires", false}});
    CHECK(problems.size() == 2);  // the bad name, the value that isn't true or false

    s.modules = {{"remoteip", true}};
    s.version.reset();
    write_apache_settings(app, s);
    CHECK(app.other["apache"]["later"] == json::array({1}));
    CHECK(app.other["apache"]["modules"] == json{{"remoteip", true}});
    CHECK_FALSE(app.other["apache"].contains("version"));
    CHECK(app.other.contains("php"));
    CHECK(read_apache_settings(app) == s);

    // Nothing left: the key goes, unless something unknown is in it.
    write_apache_settings(app, ApacheSettings{});
    CHECK(app.other["apache"] == json{{"later", json::array({1})}});
    app.other["apache"].erase("later");
    write_apache_settings(app, ApacheSettings{});
    CHECK_FALSE(app.other.contains("apache"));

    // Unusable: the defaults, said once.
    app.other["apache"] = "on";
    problems.clear();
    CHECK(read_apache_settings(app, &problems) == ApacheSettings{});
    CHECK(problems.size() == 1);
}

TEST_CASE("the modules the config loads: FloServer's own, the defaults, the ones turned on, each after what it needs") {
    const std::set<std::string> install = {"authz_core", "authz_host", "authz_user", "authn_core", "authn_file",
                                           "auth_basic", "access_compat", "log_config", "mime", "dir", "alias",
                                           "rewrite", "env", "setenvif", "headers", "expires", "filter", "deflate",
                                           "version", "proxy", "proxy_fcgi", "vhost_alias", "cache", "cache_disk",
                                           "remoteip", "unique_id", "ssl", "http2", "socache_shmcb", "cache_socache"};
    const auto present = [&install](const std::string& n) { return install.contains(n); };

    // Nothing changed: what FloServer has always loaded, in that order.
    const ApacheModulePlan base = plan_apache_modules({}, present, false);
    CHECK(base.load == std::vector<std::string>{"authz_core", "authz_host", "authz_user", "authn_core", "authn_file",
                                                "auth_basic", "access_compat", "log_config", "mime", "dir", "alias",
                                                "rewrite", "env", "setenvif", "headers", "expires", "filter",
                                                "deflate", "version", "proxy", "proxy_fcgi", "vhost_alias"});
    CHECK(base.missing.empty());

    // A default turned off, optional ones turned on; one with needs gets them first.
    const ApacheModulePlan p = plan_apache_modules(
        {{"expires", false}, {"cache_disk", true}, {"remoteip", true}, {"cache_socache", true}}, present, false);
    CHECK(index_of(p.load, "expires") == -1);
    CHECK(index_of(p.load, "remoteip") > index_of(p.load, "vhost_alias"));
    CHECK(index_of(p.load, "cache") >= 0);
    CHECK(index_of(p.load, "cache") < index_of(p.load, "cache_disk"));
    CHECK(index_of(p.load, "socache_shmcb") < index_of(p.load, "cache_socache"));
    CHECK(std::count(p.load.begin(), p.load.end(), "cache") == 1);
    CHECK(p.needed_by.at("cache") == std::vector<std::string>{"cache_disk", "cache_socache"});
    CHECK(p.needed_by.at("socache_shmcb") == std::vector<std::string>{"cache_socache"});

    // filter is turned off, but deflate (on by default) needs it: loaded for deflate.
    const ApacheModulePlan f = plan_apache_modules({{"filter", false}}, present, false);
    CHECK(index_of(f.load, "filter") >= 0);
    CHECK(index_of(f.load, "filter") < index_of(f.load, "deflate"));
    CHECK(f.needed_by.at("filter") == std::vector<std::string>{"deflate"});

    // FloServer's own can't be turned off; what doesn't apply behind nginx never loads.
    const ApacheModulePlan own = plan_apache_modules({{"proxy_fcgi", false}, {"ssl", true}, {"http2", true}}, present, false);
    CHECK(index_of(own.load, "proxy_fcgi") >= 0);
    CHECK(index_of(own.load, "ssl") == -1);
    CHECK(index_of(own.load, "http2") == -1);

    // Not in the install: left out and said; so is one whose need is missing.
    const ApacheModulePlan gone = plan_apache_modules({{"brotli", true}, {"proxy_html", true}}, present, false);
    CHECK(index_of(gone.load, "brotli") == -1);
    CHECK(gone.missing == std::vector<std::string>{"brotli", "proxy_html"});

    CHECK(apache_module_info("proxy_fcgi", false).kind == ApacheModuleKind::Needed);
    CHECK(apache_module_info("expires", false).kind == ApacheModuleKind::Default);
    CHECK(apache_module_info("remoteip", false).kind == ApacheModuleKind::Optional);
    CHECK(apache_module_info("ssl", false).kind == ApacheModuleKind::Off);
    CHECK(apache_module_info("ssl", false).why == "https");
    CHECK(apache_module_info("mpm_event", true).kind == ApacheModuleKind::Needed);
    CHECK(apache_module_info("mpm_prefork", true).kind == ApacheModuleKind::Off);
    CHECK(apache_module_info("deflate", false).needs == std::vector<std::string>{"filter"});
}

TEST_CASE("the generated config loads the planned modules in order, then FloServer's lines, then custom.conf") {
    ApacheSpec spec;
    spec.server_root = fs::path("/srv/apache");
    spec.modules_dir = fs::path("/srv/apache/modules");
    spec.work_dir = fs::path("/data/web");
    spec.modules = {"authz_core", "mime", "cache", "cache_disk"};
    spec.extra = "User #65534\n";
    spec.custom_conf = "Header set X-Mine yes\n";
    spec.custom_conf_path = "/data/apache/custom.conf";
    std::string error;
    const auto text = make_apache_config(spec, error);
    REQUIRE(text);
    const auto cache = text->find("LoadModule cache_module \"/srv/apache/modules/mod_cache.so\"");
    const auto disk = text->find("LoadModule cache_disk_module \"/srv/apache/modules/mod_cache_disk.so\"");
    CHECK(cache != std::string::npos);
    CHECK(disk > cache);
    CHECK(text->find("mod_rewrite.so") == std::string::npos);  // only what the plan says
    CHECK(text->find("User #65534") < text->find("Header set X-Mine yes"));
    CHECK(text->ends_with("# Your own lines, from /data/apache/custom.conf\nHeader set X-Mine yes\n"));

    // Without a plan: the modules FloServer has always loaded.
    spec.modules.clear();
    const auto usual = make_apache_config(spec, error);
    REQUIRE(usual);
    CHECK(usual->find("mod_rewrite.so") != std::string::npos);
}

TEST_CASE("custom.conf: Apache stays on 127.0.0.1 with FloServer's files, whatever the file says") {
    const test_util::TempDir tmp;
    const fs::path logs = tmp.path() / "web" / "logs";
    const std::string mine = path_to_utf8(logs);
    const std::string text = "# a comment that isn't copied\n"                       // 1
                             "Header set X-Frame-Options SAMEORIGIN\n"               // 2
                             "Listen 0.0.0.0:80\n"                                    // 3
                             "  listen 8080\n"                                       // 4
                             "\"Listen\" 127.0.0.1:81\n"                             // 5
                             "Lis\\\nten 0.0.0.0:82\n"                               // 6-7 (Apache joins them)
                             "Include C:/other.conf\n"                               // 8
                             "IncludeOptional conf.d/*.conf\n"                       // 9
                             "LoadModule ssl_module modules/mod_ssl.so\n"            // 10
                             "<VirtualHost *:80>\n"                                  // 11
                             "  ServerName evil.localhost\n"                         // 12
                             "  <VirtualHost *:81>\n"                                // 13
                             "  </VirtualHost>\n"                                    // 14
                             "</VirtualHost>\n"                                      // 15
                             "ServerRoot C:/\n"                                      // 16
                             "PidFile x.pid\n"                                       // 17
                             "Define X Listen\n"                                     // 18
                             "${X} 0.0.0.0:83\n"                                     // 19
                             "ErrorLog C:/elsewhere.log\n"                           // 20
                             "CustomLog \"|cmd.exe /c x\" common\n"                  // 21
                             "CustomLog C:/elsewhere/access.log common\n"            // 22
                             "CustomLog \"" + mine + "/access.log\" common\n"        // 23
                             "Header set X-Long " + std::string(1001, 'a') + "\n"    // 24
                             "Header set X-Ctl a\x01" "b\n"                          // 25
                             "<Directory \"C:/www\">\n"                              // 26
                             "    Options -Indexes\n"                                // 27
                             "</Directory>\n"                                        // 28
                             "ServerAdmin a@b.c \\\r\n"                              // 29-30, CRLF
                             "  x\r\n"
                             "# comment \\\n"                                        // 31-32: Apache reads both as the comment
                             "Listen 0.0.0.0:84\n"
                             "RewriteMap m prg:C:/run.exe\n"                         // 33
                             "Mutex file:C:/x default\n"                             // 34
                             "<IfModule !headers_module>\n"                          // 35
                             "</IfModule>\n"                                         // 36
                             "Timeout 60\\";                                        // 37: a '\' at the very end
    const FilteredApacheConf f = filter_apache_custom_conf(text, logs);
    INFO(f.text);
    const std::vector<std::string> kept = directives(f.text);
    CHECK(kept == std::vector<std::string>{"Header set X-Frame-Options SAMEORIGIN",
                                           "CustomLog \"" + mine + "/access.log\" common", "<Directory \"C:/www\">",
                                           "Options -Indexes", "</Directory>", "ServerAdmin a@b.c   x",
                                           "<IfModule !headers_module>", "</IfModule>", "Timeout 60"});
    // Nothing FloServer writes could make Apache read on: no comment, no '\' at an end, no long line.
    for (const std::string& line : directives(f.text)) {
        CHECK_FALSE(line.ends_with("\\"));
        CHECK(line.size() <= 1000);
    }
    CHECK(f.text.find("comment") == std::string::npos);
    CHECK(has_problem(f, "line 3:", "127.0.0.1"));
    CHECK(has_problem(f, "line 4:", "127.0.0.1"));
    CHECK(has_problem(f, "line 5:", "plain name"));
    CHECK(has_problem(f, "lines 6-7:", "127.0.0.1"));
    CHECK(has_problem(f, "line 8:", "every line must be here"));
    CHECK(has_problem(f, "line 9:", "every line must be here"));
    CHECK(has_problem(f, "line 10:", "tick boxes"));
    CHECK(has_problem(f, "line 11:", "VirtualHost"));
    CHECK(has_problem(f, "lines 11-15:", "left out up to its end"));
    CHECK(f.text.find("evil") == std::string::npos);
    CHECK(has_problem(f, "line 16:", "FloServer's"));
    CHECK(has_problem(f, "line 17:", "FloServer's"));
    CHECK(has_problem(f, "line 18:", "variables"));
    CHECK(has_problem(f, "line 19:", "plain name"));
    CHECK(has_problem(f, "line 20:", "ErrorLog"));
    CHECK(has_problem(f, "line 21:", "program"));
    CHECK(has_problem(f, "line 22:", "logs folder"));
    CHECK(has_problem(f, "line 24:", "1000"));
    CHECK(has_problem(f, "line 25:", "control character"));
    CHECK(has_problem(f, "line 33:", "program"));
    CHECK(has_problem(f, "line 34:", "FloServer's"));
    CHECK(f.text.find("0.0.0.0") == std::string::npos);

    // A long line Apache would join with the next one is left out with it.
    const FilteredApacheConf joined =
        filter_apache_custom_conf("Header set X " + std::string(990, 'a') + "\\\nListen 0.0.0.0:85\nTimeout 9\n", logs);
    CHECK(directives(joined.text) == std::vector<std::string>{"Timeout 9"});
    CHECK(has_problem(joined, "lines 1-2:", "1000"));

    // An unclosed refused section: everything after it is left out.
    const FilteredApacheConf open = filter_apache_custom_conf("<VirtualHost *:80>\nTimeout 9\n", logs);
    CHECK(directives(open.text).empty());
    CHECK(has_problem(open, "lines 1-2:", "no end"));

    // Notepad's byte order mark isn't part of the first line.
    CHECK(directives(filter_apache_custom_conf("\xEF\xBB\xBFTimeout 9\r\n", logs).text) ==
          std::vector<std::string>{"Timeout 9"});

    CHECK_FALSE(apache_directive_problem("<Directory", "\"C:/x\">", logs));
    CHECK_FALSE(apache_directive_problem("Header", "set X y", logs));
    CHECK(apache_directive_problem("</VirtualHost>", "", logs));
    CHECK(apache_directive_problem("ScriptLog", "logs/cgi.log", logs));  // relative: under ServerRoot
    CHECK(apache_directive_problem("CustomLog", "\"" + mine + "/../../x.log\" common", logs));
}

TEST_CASE("custom.conf is made with a header on demand; only the header is no text") {
    const test_util::TempDir tmp;
    CHECK(read_apache_custom_conf(tmp.path()).empty());
    std::string error;
    REQUIRE(ensure_apache_custom_conf(tmp.path(), error));
    CHECK(test_util::read_file(apache_custom_conf_file(tmp.path())) == apache_custom_conf_header());
    CHECK(read_apache_custom_conf(tmp.path()).empty());
    test_util::write_file(apache_custom_conf_file(tmp.path()), "Timeout 9\n");
    REQUIRE(ensure_apache_custom_conf(tmp.path(), error));  // an existing file is left alone
    CHECK(read_apache_custom_conf(tmp.path()) == "Timeout 9\n");
}

TEST_CASE("apache.get and apache.set: the install's modules, what can be turned on, versions") {
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    for (const char* version : {"2.4.66", "2.4.69"}) {
        const fs::path dir = runtimes_dir(data) / "apache" / version;
        test_util::write_file(dir / ".floserver-runtime.json",
                              json{{"schema", 1}, {"name", "apache"}, {"version", version},
                                   {"platform", std::string(current_platform())}, {"variant", ""}, {"license", "x"},
                                   {"sha256", std::string(64, 'a')}}
                                  .dump());
        for (const char* m : {"mod_proxy_fcgi.so", "mod_expires.so", "mod_cache.so", "mod_cache_disk.so",
                              "mod_deflate.so", "mod_filter.so", "mod_ssl.so", "mod_headers.so", "readme.txt"}) {
            test_util::write_file(dir / "Apache24" / "modules" / m, "x");
        }
    }
    // Only the older one has remoteip.
    test_util::write_file(runtimes_dir(data) / "apache" / "2.4.66" / "Apache24" / "modules" / "mod_remoteip.so", "x");
    std::string error;
    REQUIRE(save_app_config(app_config_file(data), AppConfig{}, error));
    ProjectService projects(data);
    int reloads = 0;
    ApacheSettingsService::Hooks hooks;
    hooks.reload = [&reloads] { ++reloads; };
    hooks.status = [] { return json{{"running", true}}; };
    hooks.sites = [] { return json{{"wp", {{"app", "apache"}}}, {"blog", {{"pool", "x"}}}}; };
    ApacheSettingsService service(data, projects, hooks);
    ServiceRefusal refusal;

    auto got = service.get(refusal);
    REQUIRE(got);
    INFO(got->dump(2));
    CHECK((*got)["version"] == "2.4.69");
    CHECK((*got)["versions"] == json::array({"2.4.69", "2.4.66"}));
    CHECK((*got)["used_by"] == json::array({"wp"}));
    CHECK((*got)["running"] == true);
    CHECK((*got)["custom_conf_exists"] == false);
    const auto module = [](const json& reply, const std::string& name) {
        for (const json& m : reply["modules"]) {
            if (m["name"] == name) {
                return m;
            }
        }
        return json();
    };
    CHECK((*got)["modules"].size() == 8);  // the mod_*.so files
    CHECK(module(*got, "proxy_fcgi")["kind"] == "needed");
    CHECK(module(*got, "expires")["on"] == true);
    CHECK(module(*got, "cache")["on"] == false);
    CHECK(module(*got, "ssl")["kind"] == "off");
    CHECK(module(*got, "ssl")["why"] == "https");

    // Turned on and off; a change back to the default isn't kept.
    got = service.set(json{{"modules", {{"cache_disk", true}, {"expires", false}, {"headers", true}}}}, refusal);
    REQUIRE(got);
    CHECK(reloads == 1);
    CHECK((*got)["restarting"] == true);
    CHECK(module(*got, "cache_disk")["on"] == true);
    CHECK(module(*got, "cache")["needed_by"] == json::array({"cache_disk"}));
    CHECK(module(*got, "expires")["on"] == false);
    auto config = projects.config(refusal);
    REQUIRE(config);
    CHECK(config->other["apache"]["modules"] == json{{"cache_disk", true}, {"expires", false}});

    // Refused: not in the install, FloServer's own off, one that doesn't apply.
    CHECK_FALSE(service.set(json{{"modules", {{"remoteip", true}}}}, refusal));
    CHECK(refusal.code == "not_installed");
    CHECK_FALSE(service.set(json{{"modules", {{"proxy_fcgi", false}}}}, refusal));
    CHECK_FALSE(service.set(json{{"modules", {{"ssl", true}}}}, refusal));
    CHECK_FALSE(service.set(json{{"modules", {{"../x", true}}}}, refusal));
    CHECK_FALSE(service.set(json{{"version", "2.4.1"}}, refusal));

    // The older install has remoteip: picking it and turning it on in one go.
    got = service.set(json{{"version", "2.4.66"}, {"modules", {{"remoteip", true}}}}, refusal);
    REQUIRE(got);
    CHECK((*got)["version"] == "2.4.66");
    CHECK((*got)["chosen"] == "2.4.66");
    CHECK(module(*got, "remoteip")["on"] == true);
    // Nothing changed: no restart.
    got = service.set(json{{"modules", {{"remoteip", true}}}}, refusal);
    REQUIRE(got);
    CHECK((*got)["restarting"] == false);
    // null: back to the default.
    got = service.set(json{{"version", nullptr}, {"modules", {{"expires", nullptr}}}}, refusal);
    REQUIRE(got);
    CHECK((*got)["version"] == "2.4.69");
    CHECK(module(*got, "expires")["on"] == true);

    // custom.conf's refused lines are listed.
    const auto path = service.custom_conf(refusal);
    REQUIRE(path);
    test_util::write_file(*path, "Listen 80\nTimeout 9\n");
    got = service.get(refusal);
    REQUIRE(got);
    CHECK((*got)["custom_conf_exists"] == true);
    CHECK((*got)["custom_problems"].size() == 1);
}
