#include "model/access_tree.h"

#include <algorithm>
#include <utility>

namespace floserver::ui {

AccessRect clip_rect(const AccessRect& r, const AccessRect& clip) {
    AccessRect out{std::max(r.left, clip.left), std::max(r.top, clip.top), std::min(r.right, clip.right),
                   std::min(r.bottom, clip.bottom)};
    if (out.empty()) {
        return {};
    }
    return out;
}

int AccessTree::add(int parent, AccessNode node) {
    node.parent = parent >= 0 && std::cmp_less(parent, nodes_.size()) ? parent : -1;
    node.first_child = node.last_child = node.next = node.prev = -1;
    const int index = static_cast<int>(nodes_.size());
    index_.emplace(node.id, index);
    nodes_.push_back(std::move(node));
    return index;
}

void AccessTree::finish() {
    first_top_ = last_top_ = -1;
    for (AccessNode& n : nodes_) {
        n.first_child = n.last_child = n.next = n.prev = -1;
    }
    // Each node goes after its parent's last child so far: the order they were added in.
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const int index = static_cast<int>(i);
        AccessNode& n = nodes_[i];
        int& first = n.parent >= 0 ? nodes_[static_cast<std::size_t>(n.parent)].first_child : first_top_;
        int& last = n.parent >= 0 ? nodes_[static_cast<std::size_t>(n.parent)].last_child : last_top_;
        if (last >= 0) {
            nodes_[static_cast<std::size_t>(last)].next = index;
            n.prev = last;
        } else {
            first = index;
        }
        last = index;
    }
}

const AccessNode* AccessTree::node(int index) const {
    return index >= 0 && std::cmp_less(index, nodes_.size()) ? &nodes_[static_cast<std::size_t>(index)] : nullptr;
}

int AccessTree::find(std::string_view id) const {
    const auto it = index_.find(id);
    return it != index_.end() ? it->second : -1;
}

int AccessTree::at(int x, int y) const {
    int best = -1;
    int best_depth = -1;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const AccessNode& n = nodes_[i];
        if (n.offscreen || n.bounds.empty() || x < n.bounds.left || x >= n.bounds.right || y < n.bounds.top ||
            y >= n.bounds.bottom) {
            continue;
        }
        int depth = 0;
        for (int p = n.parent; p >= 0; p = nodes_[static_cast<std::size_t>(p)].parent) {
            ++depth;
        }
        if (depth >= best_depth) {
            best = static_cast<int>(i);
            best_depth = depth;
        }
    }
    return best;
}

std::vector<std::string> AccessTree::child_ids(int parent) const {
    std::vector<std::string> out;
    const AccessNode* p = node(parent);
    for (int c = p != nullptr ? p->first_child : first_top_; c >= 0; c = nodes_[static_cast<std::size_t>(c)].next) {
        out.push_back(nodes_[static_cast<std::size_t>(c)].id);
    }
    return out;
}

std::vector<AccessChange> access_changes(const AccessTree& before, const AccessTree& after) {
    std::vector<AccessChange> out;
    // What came and went first, so the elements named after are there.
    if (before.child_ids(-1) != after.child_ids(-1)) {
        out.push_back({AccessChangeKind::Children, {}});
    }
    for (std::size_t i = 0; i < after.nodes().size(); ++i) {
        const AccessNode& now = after.nodes()[i];
        const int was = before.find(now.id);
        if (was >= 0 && before.child_ids(was) != after.child_ids(static_cast<int>(i))) {
            out.push_back({AccessChangeKind::Children, now.id});
        }
    }
    for (const AccessNode& now : after.nodes()) {
        const AccessNode* then = before.node(before.find(now.id));
        if (then == nullptr) {
            continue;
        }
        if (then->name != now.name) {
            out.push_back({AccessChangeKind::Name, now.id});
        }
        if (then->toggled != now.toggled) {
            out.push_back({AccessChangeKind::Toggle, now.id});
        }
        if (then->selected != now.selected) {
            out.push_back({AccessChangeKind::Selected, now.id});
        }
        if (then->value != now.value) {
            out.push_back({AccessChangeKind::Value, now.id});
        }
        if (then->enabled != now.enabled) {
            out.push_back({AccessChangeKind::Enabled, now.id});
        }
    }
    if (before.focus != after.focus) {
        out.push_back({AccessChangeKind::Focus, after.focus});
    }
    return out;
}

std::vector<AccessNews> access_news(const AccessTree& before, const AccessTree& after) {
    std::vector<AccessNews> out;
    for (const AccessNews& now : after.news) {
        if (now.text.empty()) {
            continue;
        }
        const bool known = std::any_of(before.news.begin(), before.news.end(), [&](const AccessNews& then) {
            return then.key == now.key && then.text == now.text;
        });
        if (!known) {
            out.push_back(now);
        }
    }
    return out;
}

std::string access_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        // U+200E, U+200F, U+202A..U+202E and U+2066..U+2069 are E2 80 8E/8F,
        // E2 80 AA..AE and E2 81 A6..A9 in UTF-8.
        if (i + 2 < text.size() && static_cast<unsigned char>(text[i]) == 0xE2U) {
            const auto b1 = static_cast<unsigned char>(text[i + 1]);
            const auto b2 = static_cast<unsigned char>(text[i + 2]);
            const bool mark = (b1 == 0x80U && (b2 == 0x8EU || b2 == 0x8FU || (b2 >= 0xAAU && b2 <= 0xAEU))) ||
                              (b1 == 0x81U && b2 >= 0xA6U && b2 <= 0xA9U);
            if (mark) {
                i += 2;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

std::string access_join(Lang lang, const std::vector<std::string>& parts) {
    // The Arabic comma in Arabic and Kurdish.
    const std::string_view comma = is_rtl(lang) ? "\xD8\x8C " : ", ";
    std::string out;
    for (const std::string& part : parts) {
        const std::string said = access_text(part);
        if (said.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += comma;
        }
        out += said;
    }
    return out;
}

}  // namespace floserver::ui
