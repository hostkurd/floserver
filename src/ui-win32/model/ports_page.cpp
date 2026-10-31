#include "model/ports_page.h"

#include <algorithm>
#include <cctype>

namespace floserver::ui {

using nlohmann::json;

namespace {

std::optional<int> int_field(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j.at(key).is_number_integer()) {
        return j.at(key).get<int>();
    }
    return std::nullopt;
}

std::string string_field(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j.at(key).is_string()) {
        return j.at(key).get<std::string>();
    }
    return {};
}

bool bool_field(const json& j, const char* key, bool fallback) {
    if (j.is_object() && j.contains(key) && j.at(key).is_boolean()) {
        return j.at(key).get<bool>();
    }
    return fallback;
}

void read_ca(PortsSettings& out, const json& ca) {
    out.ca = CaState::None;
    out.ca_der.clear();
    out.ca_name.clear();
    out.ca_viewer.clear();
    out.ca_error.clear();
    if (!ca.is_object()) {
        return;
    }
    if (ca.contains("error")) {
        out.ca = CaState::Damaged;
        out.ca_error = string_field(ca, "error");
        return;
    }
    out.ca_der = string_field(ca, "der");
    out.ca_name = string_field(ca, "common_name");
    out.ca_viewer = string_field(ca, "viewer");
    // "unsupported" (not Windows) counts as not trusted: nothing here adds it.
    out.ca = string_field(ca, "trusted") == "yes" ? CaState::Trusted : CaState::NotTrusted;
}

std::string port_text(int port) { return std::to_string(port); }

}  // namespace

std::optional<PortsSettings> ports_settings_from_reply(const json& reply) {
    if (!reply.is_object() || !reply.contains("ports") || !reply.at("ports").is_string()) {
        return std::nullopt;
    }
    PortsSettings out;
    out.ports = kPortChoices[port_choice_index(reply.at("ports").get<std::string>())];
    out.http_port = int_field(reply, "http_port");
    out.https_port = int_field(reply, "https_port");
    out.https = bool_field(reply, "https", true);
    out.shared_https = bool_field(reply, "shared_https", false);
    if (reply.contains("in_use") && reply.at("in_use").is_object()) {
        out.in_use_http = int_field(reply.at("in_use"), "http");
        out.in_use_https = int_field(reply.at("in_use"), "https");
    }
    if (reply.contains("taken") && reply.at("taken").is_array()) {
        for (const json& t : reply.at("taken")) {
            const auto port = int_field(t, "port");
            if (!port) {
                continue;
            }
            PortTaken taken;
            taken.port = *port;
            if (const auto pid = t.contains("pid") && t.at("pid").is_number_integer() ? t.at("pid").get<std::int64_t>() : 0;
                pid > 0 && pid <= 0xFFFFFFFF) {
                taken.pid = static_cast<std::uint32_t>(pid);
            }
            taken.program = string_field(t, "program");
            out.taken.push_back(std::move(taken));
        }
    }
    read_ca(out, reply.contains("ca") ? reply.at("ca") : json());
    return out;
}

bool apply_ca_reply(PortsSettings& settings, const json& reply) {
    if (!reply.is_object() || !reply.contains("ca") || !reply.at("ca").is_object()) {
        return false;
    }
    const std::string viewer = settings.ca_viewer;  // certs.ensure_ca doesn't name it
    read_ca(settings, reply.at("ca"));
    if (settings.ca_viewer.empty()) {
        settings.ca_viewer = viewer;
    }
    return settings.ca != CaState::None && !settings.ca_der.empty();
}

std::size_t port_choice_index(std::string_view ports) {
    const auto it = std::find(kPortChoices.begin(), kPortChoices.end(), ports);
    return it == kPortChoices.end() ? 0 : static_cast<std::size_t>(it - kPortChoices.begin());
}

std::string port_choice_label(std::size_t index, Lang lang) {
    switch (index) {
        case 1:
            return std::string(text(lang, Str::PortsStandard));
        case 2:
            return std::string(text(lang, Str::PortsAlternate));
        case 3:
            return std::string(text(lang, Str::PortsCustom));
        default:
            return std::string(text(lang, Str::PortsAuto));
    }
}

std::string port_choice_note(std::size_t index, Lang lang) {
    switch (index) {
        case 1:
            return fill(lang, Str::PortsStandardNote, {"https://shop.localhost"});
        case 2:
            return fill(lang, Str::PortsAlternateNote, {"https://shop.localhost:8443"});
        case 3:
            return std::string(text(lang, Str::PortsCustomNote));
        default:
            return std::string(text(lang, Str::PortsAutoNote));
    }
}

std::string ports_in_use_text(const PortsSettings& settings, Lang lang) {
    if (!settings.in_use_http || !settings.in_use_https) {
        return std::string(text(lang, Str::PortsNotRunning));
    }
    return fill(lang, Str::PortsInUse, {port_text(*settings.in_use_http), port_text(*settings.in_use_https)});
}

std::vector<std::string> ports_taken_lines(const PortsSettings& settings, Lang lang) {
    std::vector<std::string> out;
    for (const PortTaken& t : settings.taken) {
        if (!t.program.empty()) {
            out.push_back(fill(lang, Str::PortsTakenBy, {port_text(t.port), t.program}));
        } else if (t.pid != 0) {
            out.push_back(fill(lang, Str::PortsTakenByPid, {port_text(t.port), std::to_string(t.pid)}));
        } else {
            out.push_back(fill(lang, Str::PortsTaken, {port_text(t.port)}));
        }
    }
    if (!out.empty() && settings.ports != "auto") {
        out.emplace_back(text(lang, Str::PortsMovesLater));
    }
    return out;
}

std::string ca_status_text(const PortsSettings& settings, Lang lang) {
    switch (settings.ca) {
        case CaState::Trusted:
            return std::string(text(lang, Str::PortsCaTrusted));
        case CaState::NotTrusted:
            return std::string(text(lang, Str::PortsCaNotTrusted));
        case CaState::Damaged:
            return fill_pattern(text(lang, Str::PortsCaDamaged), false,
                                {is_rtl(lang) ? isolate_ltr(settings.ca_error) : settings.ca_error});
        case CaState::None:
            break;
    }
    return std::string(text(lang, Str::PortsCaNone));
}

std::optional<CustomPorts> parse_custom_ports(std::string_view http, std::string_view https, Str& problem) {
    const auto number = [](std::string_view s) -> std::optional<int> {
        while (!s.empty() && s.front() == ' ') {
            s.remove_prefix(1);
        }
        while (!s.empty() && s.back() == ' ') {
            s.remove_suffix(1);
        }
        if (s.empty() || s.size() > 5) {
            return std::nullopt;
        }
        int n = 0;
        for (const char c : s) {
            if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
                return std::nullopt;
            }
            n = n * 10 + (c - '0');
        }
        if (n < 1 || n > 65535) {
            return std::nullopt;
        }
        return n;
    };
    const auto h = number(http);
    const auto s = number(https);
    if (!h || !s) {
        problem = Str::PortsBadNumber;
        return std::nullopt;
    }
    if (*h == *s) {
        problem = Str::PortsSameNumber;
        return std::nullopt;
    }
    return CustomPorts{*h, *s};
}

json port_choice_params(std::size_t index, const std::optional<CustomPorts>& custom) {
    json out = {{"ports", std::string(kPortChoices[std::min(index, kPortChoices.size() - 1)])}};
    if (custom) {
        out["http_port"] = custom->http;
        out["https_port"] = custom->https;
    }
    return out;
}

}  // namespace floserver::ui
