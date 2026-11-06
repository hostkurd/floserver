#include "model/svg_path.h"

#include <cctype>
#include <charconv>
#include <cstddef>

namespace floserver::setup {

namespace {

class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}

    void skip_separators() {
        while (pos_ < data_.size() && (is_space(data_[pos_]) || data_[pos_] == ',')) {
            ++pos_;
        }
    }
    [[nodiscard]] bool done() {
        skip_separators();
        return pos_ >= data_.size();
    }
    // The next command letter, if one is next.
    [[nodiscard]] std::optional<char> command() {
        skip_separators();
        if (pos_ < data_.size() && std::isalpha(static_cast<unsigned char>(data_[pos_])) != 0 &&
            data_[pos_] != 'e' && data_[pos_] != 'E') {
            return data_[pos_++];
        }
        return std::nullopt;
    }
    // A number is next (so the last command repeats).
    [[nodiscard]] bool number_next() {
        skip_separators();
        if (pos_ >= data_.size()) {
            return false;
        }
        const char c = data_[pos_];
        return c == '-' || c == '+' || c == '.' || std::isdigit(static_cast<unsigned char>(c)) != 0;
    }
    // One number, as SVG writes them: "-.5.5" is -0.5 then 0.5.
    [[nodiscard]] std::optional<double> number() {
        skip_separators();
        const std::size_t start = pos_;
        if (pos_ < data_.size() && (data_[pos_] == '-' || data_[pos_] == '+')) {
            ++pos_;
        }
        bool digits = false;
        bool dot = false;
        while (pos_ < data_.size()) {
            const char c = data_[pos_];
            if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
                digits = true;
            } else if (c == '.' && !dot) {
                dot = true;
            } else {
                break;
            }
            ++pos_;
        }
        if (!digits) {
            return std::nullopt;
        }
        if (pos_ < data_.size() && (data_[pos_] == 'e' || data_[pos_] == 'E')) {
            std::size_t exp = pos_ + 1;
            if (exp < data_.size() && (data_[exp] == '-' || data_[exp] == '+')) {
                ++exp;
            }
            if (exp < data_.size() && std::isdigit(static_cast<unsigned char>(data_[exp])) != 0) {
                pos_ = exp;
                while (pos_ < data_.size() && std::isdigit(static_cast<unsigned char>(data_[pos_])) != 0) {
                    ++pos_;
                }
            }
        }
        std::string_view text = data_.substr(start, pos_ - start);
        if (!text.empty() && text.front() == '+') {
            text.remove_prefix(1);
        }
        double value = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || end != text.data() + text.size()) {
            return std::nullopt;
        }
        return value;
    }

private:
    static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }

    std::string_view data_;
    std::size_t pos_ = 0;
};

PathPoint lerp(PathPoint a, PathPoint b, double t) { return {a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t)}; }

}  // namespace

std::optional<std::vector<PathFigure>> parse_svg_path(std::string_view data) {
    Reader in(data);
    std::vector<PathFigure> figures;
    PathPoint current;
    PathPoint start;
    PathPoint last_control;  // for S: the previous curve's second control
    bool last_was_curve = false;
    char command = 0;

    auto curve_to = [&](PathPoint c1, PathPoint c2, PathPoint end) {
        figures.back().points.insert(figures.back().points.end(), {c1, c2, end});
        current = end;
    };
    auto line_to = [&](PathPoint end) {
        curve_to(lerp(current, end, 1.0 / 3), lerp(current, end, 2.0 / 3), end);
        last_was_curve = false;
    };

    while (!in.done()) {
        if (const auto next = in.command()) {
            command = *next;
        } else if (command == 0 || !in.number_next() || command == 'Z' || command == 'z') {
            return std::nullopt;  // numbers with no command before them, or after Z
        }
        const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
        const double ox = relative ? current.x : 0;
        const double oy = relative ? current.y : 0;
        auto point = [&]() -> std::optional<PathPoint> {
            const auto x = in.number();
            const auto y = x ? in.number() : std::nullopt;
            if (!x || !y) {
                return std::nullopt;
            }
            return PathPoint{*x + ox, *y + oy};
        };
        switch (command) {
            case 'M':
            case 'm': {
                const auto p = point();
                if (!p) {
                    return std::nullopt;
                }
                figures.push_back(PathFigure{{*p}, false});
                current = *p;
                start = *p;
                last_was_curve = false;
                // More pairs after a move are lines.
                command = relative ? 'l' : 'L';
                break;
            }
            case 'L':
            case 'l':
            case 'H':
            case 'h':
            case 'V':
            case 'v':
            case 'C':
            case 'c':
            case 'S':
            case 's': {
                if (figures.empty()) {
                    return std::nullopt;  // drawing before any M
                }
                if (figures.back().closed) {
                    // Drawing on after Z starts a new figure where the last one began.
                    figures.push_back(PathFigure{{current}, false});
                }
                const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
                if (upper == 'L') {
                    const auto p = point();
                    if (!p) {
                        return std::nullopt;
                    }
                    line_to(*p);
                } else if (upper == 'H' || upper == 'V') {
                    const auto v = in.number();
                    if (!v) {
                        return std::nullopt;
                    }
                    line_to(upper == 'H' ? PathPoint{*v + ox, current.y} : PathPoint{current.x, *v + oy});
                } else if (upper == 'C') {
                    const auto c1 = point();
                    const auto c2 = c1 ? point() : std::nullopt;
                    const auto end = c2 ? point() : std::nullopt;
                    if (!c1 || !c2 || !end) {
                        return std::nullopt;
                    }
                    curve_to(*c1, *c2, *end);
                    last_control = *c2;
                    last_was_curve = true;
                } else {  // S: the first control mirrors the last curve's second one
                    const auto c2 = point();
                    const auto end = c2 ? point() : std::nullopt;
                    if (!c2 || !end) {
                        return std::nullopt;
                    }
                    const PathPoint c1 = last_was_curve
                                             ? PathPoint{(2 * current.x) - last_control.x, (2 * current.y) - last_control.y}
                                             : current;
                    curve_to(c1, *c2, *end);
                    last_control = *c2;
                    last_was_curve = true;
                }
                break;
            }
            case 'Z':
            case 'z':
                if (figures.empty()) {
                    return std::nullopt;
                }
                figures.back().closed = true;
                current = start;
                last_was_curve = false;
                break;
            default:
                return std::nullopt;  // a command this reader doesn't know
        }
    }
    if (figures.empty()) {
        return std::nullopt;
    }
    return figures;
}

}  // namespace floserver::setup
