#include "model/folder_removal.h"

#include <algorithm>

namespace floserver::ui {

namespace {

constexpr std::string_view kLongUnc = R"(\\?\UNC\)";
constexpr std::string_view kLong = R"(\\?\)";
constexpr std::string_view kUnc = R"(\\)";
constexpr std::string_view kDevice = R"(\\.\)";

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// The parts of `s` between separators, "." left out, ".." taking one back (never past the root).
std::vector<std::string> parts_of(std::string_view s) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t end = std::min(s.find('\\', start), s.size());
        const std::string_view part = s.substr(start, end - start);
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else if (!part.empty() && part != ".") {
            parts.emplace_back(part);
        }
        start = end + 1;
    }
    return parts;
}

bool is_root(std::string_view comparable) {
    // "c:\" or "\\server\share": nothing after the root.
    if (comparable.size() == 3 && comparable[1] == ':') {
        return true;
    }
    if (starts_with(comparable, kUnc)) {
        return std::count(comparable.begin(), comparable.end(), '\\') == 3;
    }
    return false;
}

}  // namespace

std::string comparable_path(std::string_view path) {
    std::string s;
    if (starts_with(path, kLongUnc) || starts_with(path, "//?/UNC/")) {
        s = std::string(kUnc) + std::string(path.substr(8));
    } else if (starts_with(path, kLong) || starts_with(path, "//?/")) {
        s = std::string(path.substr(4));
    } else {
        s = std::string(path);
    }
    std::replace(s.begin(), s.end(), '/', '\\');
    std::transform(s.begin(), s.end(), s.begin(), lower);
    std::string root;
    std::string_view rest;
    if (s.size() >= 3 && is_letter(s[0]) && s[1] == ':' && s[2] == '\\') {
        root = s.substr(0, 2);
        rest = std::string_view(s).substr(3);
    } else if (starts_with(s, kUnc) && s.size() > 2 && s[2] != '\\' && !starts_with(s, kDevice) &&
               !starts_with(s, kLong)) {
        // \\server\share: both make the root (\\.\ and a \\?\ left over are devices, not folders).
        const std::vector<std::string> unc = parts_of(std::string_view(s).substr(2));
        if (unc.size() < 2) {
            return {};
        }
        std::string out = std::string(kUnc) + unc[0] + "\\" + unc[1];
        for (std::size_t i = 2; i < unc.size(); ++i) {
            out += "\\" + unc[i];
        }
        return out;
    } else {
        return {};
    }
    const std::vector<std::string> parts = parts_of(rest);
    std::string out = root + "\\";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        out += (i == 0 ? "" : "\\") + parts[i];
    }
    return out;
}

bool same_or_inside(std::string_view inner, std::string_view outer) {
    if (inner.empty() || outer.empty() || !starts_with(inner, outer)) {
        return false;
    }
    return inner.size() == outer.size() || outer.back() == '\\' || inner[outer.size()] == '\\';
}

FolderRefusal folder_removal_refusal(std::string_view folder, std::string_view real, const FolderGuard& guard) {
    std::vector<std::string> checked;
    checked.push_back(comparable_path(folder));
    if (checked.front().empty()) {
        return FolderRefusal::NotFullPath;
    }
    if (!real.empty()) {
        std::string r = comparable_path(real);
        if (r.empty()) {
            return FolderRefusal::NotFullPath;
        }
        checked.push_back(std::move(r));
    }
    for (const std::string& c : checked) {
        if (is_root(c)) {
            return FolderRefusal::DriveRoot;
        }
        for (const std::string& k : guard.kept) {
            if (same_or_inside(comparable_path(k), c)) {
                return FolderRefusal::KeptFolder;
            }
        }
        for (const std::string& k : guard.kept_with_contents) {
            const std::string kc = comparable_path(k);
            if (same_or_inside(c, kc) || same_or_inside(kc, c)) {
                return FolderRefusal::DataFolder;
            }
        }
        for (const std::string& o : guard.other_projects) {
            const std::string oc = comparable_path(o);
            if (same_or_inside(c, oc) || same_or_inside(oc, c)) {
                return FolderRefusal::OtherProject;
            }
        }
    }
    return FolderRefusal::None;
}

}  // namespace floserver::ui
