#pragma once
#include <atlas/storage/file.hpp>
#include <atomic>
namespace atlas {
class DiskManager {
  public:
    DiskManager(const std::filesystem::path &path, bool create, bool exclusive_create = false)
        : file_(path, create, exclusive_create) {}
    Page read(PageId id) const;
    Page read_unchecked(PageId id) const;
    void write(const Page &page);
    void sync() { file_.sync(); }
    std::uint64_t bytes() const { return file_.size(); }
    void resize_pages(PageId count);
    std::uint64_t reads() const { return reads_.load(); }
    std::uint64_t writes() const { return writes_.load(); }

  private:
    File file_;
    mutable std::atomic<std::uint64_t> reads_{0};
    std::atomic<std::uint64_t> writes_{0};
};
} // namespace atlas
