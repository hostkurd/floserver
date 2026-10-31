#pragma once

// Settings > Ports & HTTPS (D20): web.settings.get's answer, the words the
// section shows, and what a change sends. Plain C++ so it's tested on every
// platform; the window only draws it.

#include "model/ui_strings.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

// The web port choices, in the order they're listed (config.json's names).
inline constexpr std::array<std::string_view, 4> kPortChoices = {"auto", "standard", "alternate", "custom"};

struct PortTaken {
    int port = 0;
    std::uint32_t pid = 0;  // 0: not known
    std::string program;    // "httpd.exe"; empty: not known
};

enum class CaState : std::uint8_t {
    None,        // not made yet
    Trusted,
    NotTrusted,
    Damaged,     // can't be read: `ca_error` says why
};

struct PortsSettings {
    std::string ports = "auto";  // one of kPortChoices
    std::optional<int> http_port;   // "custom"'s, kept while another choice is picked
    std::optional<int> https_port;
    bool https = true;
    bool shared_https = false;
    std::optional<int> in_use_http;  // nothing: nginx isn't running
    std::optional<int> in_use_https;
    std::vector<PortTaken> taken;
    CaState ca = CaState::None;
    std::string ca_der;     // base64, for adding it to Windows or removing it
    std::string ca_name;    // its common name, as Windows' prompt shows it
    std::string ca_viewer;  // its .crt file, for Windows' certificate viewer
    std::string ca_error;
};

// Nothing when the answer isn't web.settings.get's.
[[nodiscard]] std::optional<PortsSettings> ports_settings_from_reply(const nlohmann::json& reply);

// certs.ensure_ca's answer folded in: the CA there now. False when it has none.
[[nodiscard]] bool apply_ca_reply(PortsSettings& settings, const nlohmann::json& reply);

// The index into kPortChoices of `ports` ("auto" when it's none of them).
[[nodiscard]] std::size_t port_choice_index(std::string_view ports);

// The radio button's words and the line under it.
[[nodiscard]] std::string port_choice_label(std::size_t index, Lang lang);
[[nodiscard]] std::string port_choice_note(std::size_t index, Lang lang);

// "In use now: http 8080, https 8443", or that nothing is served now.
[[nodiscard]] std::string ports_in_use_text(const PortsSettings& settings, Lang lang);

// One line per port asked for that another program holds, with the program
// when it's known; then, for a choice other than Automatic, that FloServer
// moves there once they're free. Empty when none is taken.
[[nodiscard]] std::vector<std::string> ports_taken_lines(const PortsSettings& settings, Lang lang);

// What Windows thinks of the CA, in a sentence.
[[nodiscard]] std::string ca_status_text(const PortsSettings& settings, Lang lang);

// The custom ports as typed: both numbers, or why not (`problem`).
struct CustomPorts {
    int http = 0;
    int https = 0;
};
[[nodiscard]] std::optional<CustomPorts> parse_custom_ports(std::string_view http, std::string_view https,
                                                            Str& problem);

// web.settings.set's parameters for picking choice `index` (with the
// custom ports for "custom").
[[nodiscard]] nlohmann::json port_choice_params(std::size_t index, const std::optional<CustomPorts>& custom);

}  // namespace floserver::ui
