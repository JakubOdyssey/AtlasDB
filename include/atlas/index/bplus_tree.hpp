#pragma once
#include <atlas/storage/slotted_page.hpp>
#include <optional>
#include <string>
#include <utility>
namespace atlas {
using Record = std::pair<std::string, std::string>;
struct TreeStats {
    std::uint64_t leaf_splits = 0, internal_splits = 0, merges = 0, redistributions = 0,
                  root_collapses = 0;
};
struct Validation {
    std::uint64_t pages = 0, leaves = 0, internal_nodes = 0, free_pages = 0, records = 0;
    std::size_t height = 0;
};
class PageAccess {
  public:
    virtual ~PageAccess() = default;
    virtual Page load(PageId id) = 0;
    virtual void store(Page page) = 0;
    virtual PageId allocate(PageType type) = 0;
    virtual void release(PageId id) = 0;
    virtual Metadata &metadata() = 0;
};
class BPlusTree {
  public:
    explicit BPlusTree(PageAccess &pages, TreeStats *stats = nullptr)
        : pages_(pages), stats_(stats) {}
    std::optional<std::string> get(std::string_view key);
    bool put(const std::string &key, const std::string &value);
    bool erase(std::string_view key);
    std::vector<Record> scan(std::string_view lower = {},
                             std::optional<std::string_view> upper = std::nullopt,
                             std::size_t limit = static_cast<std::size_t>(-1));
    Validation validate();

  private:
    struct Node {
        PageId id = 0, next = 0;
        bool leaf = true;
        std::vector<std::string> keys, values;
        std::vector<PageId> children;
    };
    struct Split {
        PageId right;
    };
    Node load(PageId id);
    void save(const Node &node);
    std::string minimum(PageId id);
    void separators(Node &node);
    std::optional<Split> insert(PageId id, const std::string &key, const std::string &value,
                                bool &inserted);
    bool remove(PageId id, std::string_view key);
    void rebalance(Node &parent, std::size_t index);
    static std::size_t child_index(const Node &node, std::string_view key);
    static std::size_t occupancy(const Node &node);
    static std::size_t minimum_occupancy(const Node &node);
    PageAccess &pages_;
    TreeStats *stats_;
};
} // namespace atlas
