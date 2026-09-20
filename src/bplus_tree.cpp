#include <algorithm>
#include <atlas/index/bplus_tree.hpp>
#include <functional>
#include <set>
namespace atlas {
namespace {
std::string string_from(std::span<const std::byte> b) {
    return {reinterpret_cast<const char *>(b.data()), b.size()};
}
void check_key(std::string_view key) {
    if (key.size() > max_key_size)
        throw LimitError("key exceeds 128 bytes");
}
} // namespace
BPlusTree::Node BPlusTree::load(PageId id) {
    auto p = pages_.load(id);
    SlottedPage slots(p);
    slots.validate();
    if (p.type() != PageType::leaf && p.type() != PageType::internal)
        throw CorruptionError("tree references a non-tree page");
    Node n;
    n.id = id;
    n.leaf = p.type() == PageType::leaf;
    if (n.leaf)
        n.next = p.auxiliary();
    else
        n.children.push_back(p.auxiliary());
    for (std::size_t i = 0; i < slots.slots(); ++i) {
        auto r = slots.record(i);
        if (r.size() < (n.leaf ? 4U : 10U))
            throw CorruptionError("short B+ tree record");
        auto key_size = read16(r, 0);
        if (key_size > max_key_size)
            throw CorruptionError("oversized stored key");
        if (n.leaf) {
            auto value_size = read16(r, 2);
            if (value_size > max_value_size || r.size() != 4U + key_size + value_size)
                throw CorruptionError("invalid leaf record length");
            n.keys.push_back(string_from(r.subspan(4, key_size)));
            n.values.push_back(string_from(r.subspan(4 + key_size, value_size)));
        } else {
            if (r.size() != 10U + key_size)
                throw CorruptionError("invalid internal record length");
            n.children.push_back(read64(r, 2));
            n.keys.push_back(string_from(r.subspan(10, key_size)));
        }
    }
    if (n.keys.size() > (n.leaf ? leaf_capacity : internal_capacity))
        throw CorruptionError("overfull tree node");
    for (std::size_t i = 1; i < n.keys.size(); ++i)
        if (!(n.keys[i - 1] < n.keys[i]))
            throw CorruptionError("unsorted/duplicate node keys");
    return n;
}
void BPlusTree::save(const Node &n) {
    auto p = Page::make(n.id, n.leaf ? PageType::leaf : PageType::internal);
    p.set_auxiliary(n.leaf ? n.next : n.children.at(0));
    SlottedPage slots(p);
    for (std::size_t i = 0; i < n.keys.size(); ++i) {
        Bytes record((n.leaf ? 4 : 10) + n.keys[i].size() + (n.leaf ? n.values[i].size() : 0));
        write16(record, 0, static_cast<std::uint16_t>(n.keys[i].size()));
        if (n.leaf)
            write16(record, 2, static_cast<std::uint16_t>(n.values[i].size()));
        else
            write64(record, 2, n.children[i + 1]);
        auto out = std::copy(as_bytes(n.keys[i]).begin(), as_bytes(n.keys[i]).end(),
                             record.begin() + (n.leaf ? 4 : 10));
        if (n.leaf)
            std::copy(as_bytes(n.values[i]).begin(), as_bytes(n.values[i]).end(), out);
        slots.insert(record);
    }
    p.seal();
    pages_.store(p);
}
std::size_t BPlusTree::child_index(const Node &n, std::string_view key) {
    return static_cast<std::size_t>(std::upper_bound(n.keys.begin(), n.keys.end(), key) -
                                    n.keys.begin());
}
std::size_t BPlusTree::occupancy(const Node &n) {
    return n.leaf ? n.keys.size() : n.children.size();
}
std::size_t BPlusTree::minimum_occupancy(const Node &n) {
    return n.leaf ? (leaf_capacity + 1) / 2 : (internal_capacity + 2) / 2;
}
std::string BPlusTree::minimum(PageId id) {
    for (std::size_t depth = 0; depth < 64; ++depth) {
        auto n = load(id);
        if (n.leaf) {
            if (n.keys.empty())
                throw CorruptionError("empty non-root subtree");
            return n.keys.front();
        }
        id = n.children.front();
    }
    throw CorruptionError("tree depth/cycle limit exceeded");
}
void BPlusTree::separators(Node &n) {
    if (n.leaf)
        return;
    n.keys.clear();
    for (std::size_t i = 1; i < n.children.size(); ++i)
        n.keys.push_back(minimum(n.children[i]));
}
std::optional<std::string> BPlusTree::get(std::string_view key) {
    check_key(key);
    auto id = pages_.metadata().root;
    for (std::size_t depth = 0; depth < 64; ++depth) {
        auto n = load(id);
        if (n.leaf) {
            auto it = std::lower_bound(n.keys.begin(), n.keys.end(), key);
            if (it == n.keys.end() || *it != key)
                return std::nullopt;
            return n.values[static_cast<std::size_t>(it - n.keys.begin())];
        }
        id = n.children[child_index(n, key)];
    }
    throw CorruptionError("tree depth/cycle limit exceeded");
}
std::optional<BPlusTree::Split> BPlusTree::insert(PageId id, const std::string &key,
                                                  const std::string &value, bool &inserted) {
    auto n = load(id);
    if (n.leaf) {
        auto it = std::lower_bound(n.keys.begin(), n.keys.end(), key);
        auto index = static_cast<std::size_t>(it - n.keys.begin());
        if (it != n.keys.end() && *it == key)
            n.values[index] = value;
        else {
            n.keys.insert(it, key);
            n.values.insert(n.values.begin() + static_cast<std::ptrdiff_t>(index), value);
            inserted = true;
        }
        if (n.keys.size() <= leaf_capacity) {
            save(n);
            return std::nullopt;
        }
        Node right;
        right.id = pages_.allocate(PageType::leaf);
        right.next = n.next;
        const auto middle = static_cast<std::ptrdiff_t>(n.keys.size() / 2);
        right.keys.assign(n.keys.begin() + middle, n.keys.end());
        right.values.assign(n.values.begin() + middle, n.values.end());
        n.keys.resize(static_cast<std::size_t>(middle));
        n.values.resize(static_cast<std::size_t>(middle));
        n.next = right.id;
        save(n);
        save(right);
        if (stats_)
            ++stats_->leaf_splits;
        return Split{right.id};
    }
    const auto index = child_index(n, key);
    if (auto split = insert(n.children[index], key, value, inserted))
        n.children.insert(n.children.begin() + static_cast<std::ptrdiff_t>(index + 1),
                          split->right);
    if (n.children.size() <= internal_capacity + 1) {
        separators(n);
        save(n);
        return std::nullopt;
    }
    Node right;
    right.id = pages_.allocate(PageType::internal);
    right.leaf = false;
    const auto middle = static_cast<std::ptrdiff_t>(n.children.size() / 2);
    right.children.assign(n.children.begin() + middle, n.children.end());
    n.children.resize(static_cast<std::size_t>(middle));
    separators(n);
    separators(right);
    save(n);
    save(right);
    if (stats_)
        ++stats_->internal_splits;
    return Split{right.id};
}
bool BPlusTree::put(const std::string &key, const std::string &value) {
    check_key(key);
    if (value.size() > max_value_size)
        throw LimitError("value exceeds 512 bytes");
    bool inserted = false;
    const auto old_root = pages_.metadata().root;
    if (auto split = insert(old_root, key, value, inserted)) {
        Node root;
        root.id = pages_.allocate(PageType::internal);
        root.leaf = false;
        root.children = {old_root, split->right};
        separators(root);
        save(root);
        pages_.metadata().root = root.id;
    }
    if (inserted)
        ++pages_.metadata().records;
    return inserted;
}
void BPlusTree::rebalance(Node &parent, std::size_t index) {
    auto child = load(parent.children[index]);
    if (occupancy(child) >= minimum_occupancy(child))
        return;
    const std::size_t left_index = index ? index - 1 : 0, right_index = left_index + 1;
    auto left = load(parent.children[left_index]), right = load(parent.children[right_index]);
    if (left.leaf != right.leaf)
        throw CorruptionError("sibling page types differ");
    const auto minimum_count = minimum_occupancy(child);
    if (occupancy(left) + occupancy(right) >= 2 * minimum_count) {
        if (left.leaf) {
            left.keys.insert(left.keys.end(), right.keys.begin(), right.keys.end());
            left.values.insert(left.values.end(), right.values.begin(), right.values.end());
            const auto middle = static_cast<std::ptrdiff_t>(left.keys.size() / 2);
            right.keys.assign(left.keys.begin() + middle, left.keys.end());
            right.values.assign(left.values.begin() + middle, left.values.end());
            left.keys.resize(static_cast<std::size_t>(middle));
            left.values.resize(static_cast<std::size_t>(middle));
        } else {
            left.children.insert(left.children.end(), right.children.begin(), right.children.end());
            const auto middle = static_cast<std::ptrdiff_t>(left.children.size() / 2);
            right.children.assign(left.children.begin() + middle, left.children.end());
            left.children.resize(static_cast<std::size_t>(middle));
            separators(left);
            separators(right);
        }
        save(left);
        save(right);
        if (stats_)
            ++stats_->redistributions;
    } else {
        if (left.leaf) {
            left.keys.insert(left.keys.end(), right.keys.begin(), right.keys.end());
            left.values.insert(left.values.end(), right.values.begin(), right.values.end());
            left.next = right.next;
        } else {
            left.children.insert(left.children.end(), right.children.begin(), right.children.end());
            separators(left);
        }
        save(left);
        pages_.release(right.id);
        parent.children.erase(parent.children.begin() + static_cast<std::ptrdiff_t>(right_index));
        if (stats_)
            ++stats_->merges;
    }
}
bool BPlusTree::remove(PageId id, std::string_view key) {
    auto n = load(id);
    if (n.leaf) {
        auto it = std::lower_bound(n.keys.begin(), n.keys.end(), key);
        if (it == n.keys.end() || *it != key)
            return false;
        auto index = it - n.keys.begin();
        n.keys.erase(it);
        n.values.erase(n.values.begin() + index);
        save(n);
        return true;
    }
    auto index = child_index(n, key);
    if (!remove(n.children[index], key))
        return false;
    rebalance(n, index);
    separators(n);
    save(n);
    return true;
}
bool BPlusTree::erase(std::string_view key) {
    check_key(key);
    if (!remove(pages_.metadata().root, key))
        return false;
    --pages_.metadata().records;
    auto root = load(pages_.metadata().root);
    if (!root.leaf && root.children.size() == 1) {
        pages_.metadata().root = root.children[0];
        pages_.release(root.id);
        if (stats_)
            ++stats_->root_collapses;
    }
    return true;
}
std::vector<Record> BPlusTree::scan(std::string_view lower, std::optional<std::string_view> upper,
                                    std::size_t limit) {
    std::vector<Record> result;
    if (!limit || (upper && lower >= *upper))
        return result;
    auto n = load(pages_.metadata().root);
    for (std::size_t depth = 0; !n.leaf; ++depth) {
        if (depth >= 64)
            throw CorruptionError("tree cycle");
        n = load(n.children[child_index(n, lower)]);
    }
    std::set<PageId> visited;
    while (true) {
        if (!visited.insert(n.id).second)
            throw CorruptionError("leaf chain cycle");
        for (std::size_t i = 0; i < n.keys.size(); ++i) {
            if (n.keys[i] < lower)
                continue;
            if (upper && n.keys[i] >= *upper)
                return result;
            if (!result.empty() && result.back().first >= n.keys[i])
                throw CorruptionError("leaf chain out of order");
            result.emplace_back(n.keys[i], n.values[i]);
            if (result.size() == limit)
                return result;
        }
        if (!n.next)
            return result;
        n = load(n.next);
        if (!n.leaf)
            throw CorruptionError("leaf chain references internal page");
    }
}
Validation BPlusTree::validate() {
    Validation report;
    const auto &meta = pages_.metadata();
    std::set<PageId> visited{0};
    std::vector<std::pair<PageId, PageId>> leaves;
    struct Bounds {
        std::string first, last;
        bool empty = false;
    };
    std::function<Bounds(PageId, std::size_t)> visit = [&](PageId id, std::size_t depth) -> Bounds {
        if (!id || id >= meta.next_page || depth > 64 || !visited.insert(id).second)
            throw CorruptionError("invalid/duplicate/cyclic tree page reference");
        auto n = load(id);
        if (id != meta.root && occupancy(n) < minimum_occupancy(n))
            throw CorruptionError("underfull non-root node " + std::to_string(id));
        if (n.leaf) {
            ++report.leaves;
            report.records += n.keys.size();
            if (report.height && report.height != depth)
                throw CorruptionError("leaves have unequal depths");
            report.height = depth;
            leaves.emplace_back(id, n.next);
            if (n.keys.empty()) {
                if (id != meta.root)
                    throw CorruptionError("empty non-root leaf");
                return {{}, {}, true};
            }
            return {n.keys.front(), n.keys.back(), false};
        }
        ++report.internal_nodes;
        if (n.children.size() < 2)
            throw CorruptionError("uncollapsed internal root");
        auto total = visit(n.children[0], depth + 1);
        for (std::size_t i = 1; i < n.children.size(); ++i) {
            auto b = visit(n.children[i], depth + 1);
            if (total.empty || b.empty || !(total.last < b.first) || n.keys[i - 1] != b.first)
                throw CorruptionError("invalid separator/subtree bounds");
            total.last = b.last;
        }
        return total;
    };
    visit(meta.root, 1);
    for (std::size_t i = 0; i < leaves.size(); ++i)
        if (leaves[i].second != (i + 1 < leaves.size() ? leaves[i + 1].first : 0))
            throw CorruptionError("leaf chain does not match tree traversal");
    auto free = meta.free_head;
    while (free) {
        if (free >= meta.next_page || !visited.insert(free).second)
            throw CorruptionError("invalid/duplicate/cyclic free page reference");
        auto p = pages_.load(free);
        p.validate(free);
        if (p.type() != PageType::free || read16(p.bytes, 28) != 0)
            throw CorruptionError("free list references non-free page");
        free = p.auxiliary();
        ++report.free_pages;
    }
    if (visited.size() != meta.next_page)
        throw CorruptionError("unreachable allocated pages");
    if (report.records != meta.records)
        throw CorruptionError("record count differs from metadata");
    report.pages = meta.next_page;
    return report;
}
} // namespace atlas
