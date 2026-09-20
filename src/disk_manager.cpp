#include <atlas/storage/disk_manager.hpp>
#include <limits>
namespace atlas {
namespace {
std::uint64_t offset(PageId id) {
    if (id > std::uint64_t(std::numeric_limits<std::int64_t>::max()) / page_size - 1)
        throw LimitError("page id overflow");
    return id * page_size;
}
} // namespace
Page DiskManager::read_unchecked(PageId id) const {
    if (offset(id) + page_size > file_.size())
        throw PageNotFound("page " + std::to_string(id) + " is outside database file");
    Page p;
    file_.read(offset(id), p.bytes);
    ++reads_;
    return p;
}
Page DiskManager::read(PageId id) const {
    auto p = read_unchecked(id);
    p.validate(id);
    return p;
}
void DiskManager::write(const Page &p) {
    p.validate(p.id());
    file_.write(offset(p.id()), p.bytes);
    ++writes_;
}
void DiskManager::resize_pages(PageId count) {
    file_.resize(offset(count));
}
} // namespace atlas
