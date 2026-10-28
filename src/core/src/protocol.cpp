#include "floserver/core/protocol.h"

namespace floserver::core::protocol {

namespace {

using nlohmann::json;

bool is_method_name(const std::string& s) {
    return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    });
}

}  // namespace

std::string make_request(std::int64_t id, std::string_view method, const json& params) {
    return json{{"v", kVersion}, {"id", id}, {"method", method}, {"params", params}}.dump();
}

std::string make_result(std::int64_t id, const json& result) {
    return json{{"v", kVersion}, {"id", id}, {"result", result}}.dump();
}

std::string make_error(std::int64_t id, std::string_view code, std::string_view message) {
    return json{{"v", kVersion}, {"id", id}, {"error", {{"code", code}, {"message", message}}}}.dump();
}

std::optional<Request> parse_request(std::string_view text, std::string& error_code) {
    const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object()) {
        error_code = "bad_request";
        return std::nullopt;
    }
    if (!doc.contains("v") || !doc["v"].is_number_integer() || doc["v"].get<std::int64_t>() != kVersion) {
        error_code = "unsupported_version";
        return std::nullopt;
    }
    if (!doc.contains("id") || !doc["id"].is_number_integer() || !doc.contains("method") ||
        !doc["method"].is_string() || !is_method_name(doc["method"].get<std::string>())) {
        error_code = "bad_request";
        return std::nullopt;
    }
    Request r;
    r.id = doc["id"].get<std::int64_t>();
    r.method = doc["method"].get<std::string>();
    if (doc.contains("params")) {
        if (!doc["params"].is_object()) {
            error_code = "bad_request";
            return std::nullopt;
        }
        r.params = doc["params"];
    }
    return r;
}

std::optional<Reply> parse_reply(std::string_view text) {
    const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object() || !doc.contains("v") || doc["v"] != kVersion || !doc.contains("id") ||
        !doc["id"].is_number_integer()) {
        return std::nullopt;
    }
    Reply r;
    r.id = doc["id"].get<std::int64_t>();
    if (doc.contains("result")) {
        r.ok = true;
        r.result = doc["result"];
        return r;
    }
    if (doc.contains("error") && doc["error"].is_object()) {
        const json& e = doc["error"];
        r.error_code = e.contains("code") && e["code"].is_string() ? e["code"].get<std::string>() : "unknown";
        r.error_message = e.contains("message") && e["message"].is_string() ? e["message"].get<std::string>() : "";
        return r;
    }
    return std::nullopt;
}

}  // namespace floserver::core::protocol
