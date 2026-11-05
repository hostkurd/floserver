#include "model/settings_access.h"

#include "model/main_access.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <numeric>
#include <utility>

namespace floserver::ui {

namespace {

// A text's id keeps at most this many bytes of its words (cut at a character's start).
constexpr std::size_t kIdWords = 60;

struct Work {
    AccessNode node;
    bool container = false;
    bool absorbs = false;
    AccessRect full;
    int parent = -1;  // index into the works
    int added = -1;   // index in the tree, once added
};

long long area(const AccessRect& r) {
    return r.empty() ? 0 : static_cast<long long>(r.right - r.left) * static_cast<long long>(r.bottom - r.top);
}

bool holds(const AccessRect& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

int mid_x(const AccessRect& r) { return r.left + (r.right - r.left) / 2; }
int mid_y(const AccessRect& r) { return r.top + (r.bottom - r.top) / 2; }

bool contains_words(const std::string& in, const std::string& words) {
    return !words.empty() && in.find(words) != std::string::npos;
}

// The first `most` bytes of `s`, not cutting a UTF-8 character in two.
std::string cut_words(const std::string& s, std::size_t most) {
    if (s.size() <= most) {
        return s;
    }
    std::size_t end = most;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    return s.substr(0, end);
}

bool is_control(const Work& w) { return !w.container && w.node.role != AccessRole::Text; }

bool is_masked(std::string_view said) {
    // "●" is E2 97 8F: a masked password is a run of them.
    bool masked = !said.empty() && said.size() % 3 == 0;
    for (std::size_t i = 0; masked && i < said.size(); i += 3) {
        masked = said.substr(i, 3) == "\xE2\x97\x8F";
    }
    return masked;
}

}  // namespace

std::string page_said(std::string_view text, Lang lang) {
    if (is_masked(text)) {
        return std::string(floserver::ui::text(lang, Str::AccessPasswordHidden));
    }
    return access_text(text);
}

void add_page(AccessTree& t, int parent, const std::string& base, std::vector<PageItem> items,
              const std::vector<PageText>& texts, const AccessRect& view, Lang lang,
              std::map<std::string, AccessRect, std::less<>>* whole) {
    std::vector<Work> works;
    works.reserve(items.size() + texts.size());
    for (PageItem& item : items) {
        Work w;
        w.full = item.node.bounds;
        w.node = std::move(item.node);
        w.container = item.container;
        w.absorbs = item.absorbs;
        works.push_back(std::move(w));
    }
    const std::size_t item_count = works.size();

    // Words drawn inside an absorbing item are its own; the rest are texts.
    std::map<std::string, int> seen;
    for (const PageText& drawn : texts) {
        const std::string said = page_said(drawn.text, lang);
        if (said.empty() || drawn.rect.empty()) {
            continue;
        }
        const int x = mid_x(drawn.rect);
        const int y = mid_y(drawn.rect);
        int best = -1;
        for (std::size_t i = 0; i < item_count; ++i) {
            if (works[i].absorbs && holds(works[i].full, x, y) &&
                (best < 0 || area(works[i].full) < area(works[static_cast<std::size_t>(best)].full))) {
                best = static_cast<int>(i);
            }
        }
        if (best >= 0) {
            AccessNode& n = works[static_cast<std::size_t>(best)].node;
            if (n.role == AccessRole::ComboBox) {
                if (n.value.empty()) {
                    n.value = said;
                }
            } else if (n.name.empty()) {
                n.name = said;
            } else if (!contains_words(n.name, said) && !contains_words(n.help, said)) {
                n.help = n.help.empty() ? said : access_join(lang, {n.help, said});
            }
            continue;
        }
        Work w;
        const std::string key = cut_words(said, kIdWords);
        std::string id = base;
        if (!drawn.id.empty()) {
            id += drawn.id;
        } else {
            id += "text/";
            id += key;
            id += '#';
            id += std::to_string(seen[key]++);
        }
        w.node = text_node(std::move(id), said);
        w.node.heading = drawn.style == TextStyle::Title ? 1 : drawn.style == TextStyle::Heading ? 2 : 0;
        w.full = drawn.rect;
        works.push_back(std::move(w));
    }

    // Each inside the smallest container around its middle.
    for (std::size_t i = 0; i < works.size(); ++i) {
        const int x = mid_x(works[i].full);
        const int y = mid_y(works[i].full);
        const long long own = area(works[i].full);
        for (std::size_t j = 0; j < item_count; ++j) {
            if (j == i || !works[j].container || area(works[j].full) <= own || !holds(works[j].full, x, y)) {
                continue;
            }
            const int p = works[i].parent;
            if (p < 0 || area(works[j].full) < area(works[static_cast<std::size_t>(p)].full)) {
                works[i].parent = static_cast<int>(j);
            }
        }
    }

    // Reading order: top to bottom, then from the reading start; a container before what's in it.
    std::vector<std::size_t> order(works.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const AccessRect& ra = works[a].full;
        const AccessRect& rb = works[b].full;
        if (ra.top != rb.top) {
            return ra.top < rb.top;
        }
        if (ra.left != rb.left) {
            return ra.left < rb.left;
        }
        return area(ra) > area(rb);
    });

    // A container with no name: its first heading's words, else its first text's.
    for (std::size_t c = 0; c < item_count; ++c) {
        if (!works[c].container || !works[c].node.name.empty()) {
            continue;
        }
        std::string first_text;
        for (const std::size_t i : order) {
            if (works[i].parent != static_cast<int>(c) || works[i].node.role != AccessRole::Text) {
                continue;
            }
            if (works[i].node.heading > 0) {
                works[c].node.name = works[i].node.name;
                break;
            }
            if (first_text.empty()) {
                first_text = works[i].node.name;
            }
        }
        if (works[c].node.name.empty()) {
            works[c].node.name = first_text;
        }
    }

    // The same name twice: each says which card it's in.
    std::map<std::string, int> names;
    for (const Work& w : works) {
        if (is_control(w) && !w.node.name.empty()) {
            ++names[w.node.name];
        }
    }
    for (Work& w : works) {
        if (!is_control(w) || !w.node.help.empty() || names[w.node.name] < 2) {
            continue;
        }
        for (int p = w.parent; p >= 0; p = works[static_cast<std::size_t>(p)].parent) {
            if (!works[static_cast<std::size_t>(p)].node.name.empty()) {
                w.node.help = works[static_cast<std::size_t>(p)].node.name;
                break;
            }
        }
    }

    // Where each is: cut to the page's view; scrolled away, offscreen.
    for (Work& w : works) {
        const AccessRect cut = clip_rect(w.full, view);
        w.node.offscreen = cut.empty();
        w.node.bounds = w.node.offscreen ? w.full : cut;
        w.node.scroll_item = true;
        if (whole != nullptr) {
            (*whole)[w.node.id] = w.full;
        }
    }

    // Added in order, each container before what's in it (its own containers first, outermost first).
    std::vector<std::size_t> chain;
    for (const std::size_t i : order) {
        chain.clear();
        for (int at = static_cast<int>(i); at >= 0 && works[static_cast<std::size_t>(at)].added < 0;
             at = works[static_cast<std::size_t>(at)].parent) {
            chain.push_back(static_cast<std::size_t>(at));
        }
        std::reverse(chain.begin(), chain.end());
        for (const std::size_t at : chain) {
            Work& w = works[at];
            const int up = w.parent >= 0 ? works[static_cast<std::size_t>(w.parent)].added : parent;
            w.added = t.add(up, w.node);
        }
    }
}

AccessNode section_node(int index, std::string_view label, bool shown, int count) {
    AccessNode n = tab_node("nav/" + std::to_string(index), label, shown, index + 1, count);
    n.keys = "Ctrl+Tab";
    return n;
}

AccessNode sections_node(Lang lang) {
    AccessNode n;
    n.id = "nav";
    n.role = AccessRole::TabList;
    n.name = std::string(text(lang, Str::AccessSections));
    n.selection_required = true;
    return n;
}

AccessNode radio_node(std::string id, std::string_view name, bool selected, bool enabled) {
    AccessNode n;
    n.id = std::move(id);
    n.role = AccessRole::Radio;
    n.name = access_text(name);
    n.selected = selected;
    n.enabled = enabled;
    n.focusable = true;
    return n;
}

AccessNode check_node(std::string id, std::string_view name, bool on, bool enabled) {
    AccessNode n;
    n.id = std::move(id);
    n.role = AccessRole::CheckBox;
    n.name = access_text(name);
    n.toggled = on;
    n.enabled = enabled;
    n.focusable = true;
    return n;
}

AccessNode combo_node(std::string id, std::string_view name, std::string_view value, bool enabled) {
    AccessNode n;
    n.id = std::move(id);
    n.role = AccessRole::ComboBox;
    n.name = access_text(name);
    n.value = access_text(value);
    n.enabled = enabled;
    n.focusable = true;
    n.expandable = true;
    return n;
}

AccessNode page_link_node(std::string id, std::string_view name, bool enabled) {
    AccessNode n = link_node(std::move(id), std::string(name));
    n.enabled = enabled;
    n.focusable = true;
    return n;
}

std::string reset_name(std::string_view field, Lang lang) {
    return access_text(fill(lang, Str::AccessResetField, {field}));
}

std::string runtime_row_label(std::string_view group, const RuntimeRow& row) {
    // "8.5.1" alone under PHP: "PHP 8.5.1" ("nginx 1.30.6" says what it is).
    if (!row.label.empty() && row.label.front() >= '0' && row.label.front() <= '9' && !group.empty()) {
        std::string label(group);
        label += ' ';
        label += row.label;
        return label;
    }
    return row.label;
}

std::string runtime_row_name(std::string_view group, const RuntimeRow& row, std::string_view line, Lang lang) {
    std::vector<std::string> parts{runtime_row_label(group, row), std::string(text(lang, row.installed ? Str::SettingsInstalledChip
                                                                               : Str::AccessNotInstalled))};
    if (row.always_used) {
        parts.emplace_back(text(lang, Str::SettingsAlwaysUsed));
    } else if (row.installed && row.is_default) {
        parts.emplace_back(text(lang, Str::SettingsDefaultChip));
    }
    parts.emplace_back(line);
    return access_join(lang, parts);
}

AccessNode progress_node(std::string id, std::string_view said, double fraction) {
    AccessNode n;
    n.id = std::move(id);
    n.role = AccessRole::Progress;
    n.name = access_text(said);
    n.progress = std::clamp(fraction, 0.0, 1.0);
    return n;
}

int keep_focus_index(const std::vector<std::string>& before, int focus, const std::vector<std::string>& after,
                     LostFocus& lost) {
    const int count = static_cast<int>(after.size());
    const auto index_of = [&](const std::string& id) {
        const auto it = std::find(after.begin(), after.end(), id);
        return it != after.end() ? static_cast<int>(it - after.begin()) : -1;
    };
    if (focus >= 0 && std::cmp_less(focus, before.size())) {
        const std::string& was = before[static_cast<std::size_t>(focus)];
        if (const int at = index_of(was); at >= 0) {
            focus = at;
        } else {
            // Gone: the stop before it that's still there, else the one after.
            int to = -1;
            for (int i = focus - 1; i >= 0 && to < 0; --i) {
                to = index_of(before[static_cast<std::size_t>(i)]);
            }
            for (std::size_t i = static_cast<std::size_t>(focus) + 1; i < before.size() && to < 0; ++i) {
                to = index_of(before[i]);
            }
            focus = to;
            if (lost.id.empty()) {
                lost.id = was;
            }
            lost.neighbour = to >= 0 ? after[static_cast<std::size_t>(to)] : std::string();
        }
    }
    focus = std::min(focus, count - 1);
    // The one that went is back, and the keys are still where they went meanwhile.
    if (!lost.id.empty()) {
        const std::string here = focus >= 0 ? after[static_cast<std::size_t>(focus)] : std::string();
        if (here != lost.neighbour) {
            lost = {};
        } else if (const int back = index_of(lost.id); back >= 0) {
            focus = back;
            lost = {};
        }
    }
    return focus;
}

double scroll_percent(int scroll, int content, int view) {
    const int most = content - view;
    if (most <= 0) {
        return -1;
    }
    return static_cast<double>(std::clamp(scroll, 0, most)) * 100.0 / static_cast<double>(most);
}

double scroll_view_size(int content, int view) {
    if (content <= view || content <= 0) {
        return 100;
    }
    return static_cast<double>(view) * 100.0 / static_cast<double>(content);
}

}  // namespace floserver::ui
