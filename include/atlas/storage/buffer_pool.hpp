#pragma once
#include <atlas/storage/disk_manager.hpp>
#include <mutex>
#include <unordered_map>
namespace atlas {
struct BufferStats {
    std::uint64_t hits = 0, misses = 0, evictions = 0;
};
class BufferPool {
    struct Frame {
        Page page;
        std::mutex latch;
        std::size_t pins = 0;
        bool valid = false, dirty = false, referenced = false;
    };

  public:
    class Guard {
      public:
        Guard(Guard &&other) noexcept;
        Guard &operator=(Guard &&) = delete;
        Guard(const Guard &) = delete;
        ~Guard();
        const Page &page() const { return frame_->page; }
        void replace(Page page);

      private:
        friend class BufferPool;
        Guard(BufferPool &pool, Frame &frame);
        BufferPool *pool_;
        Frame *frame_;
        std::unique_lock<std::mutex> latch_;
    };
    BufferPool(DiskManager &disk, std::size_t capacity);
    Guard fetch(PageId id);
    void install(const Page &page);
    void set_durable_lsn(Lsn lsn);
    void flush_all();
    BufferStats stats() const;

  private:
    Frame &victim(); // caller owns mutex_
    void flush(Frame &frame);
    DiskManager &disk_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Frame>> frames_;
    std::unordered_map<PageId, Frame *> table_;
    std::size_t hand_ = 0;
    Lsn durable_lsn_ = 0;
    BufferStats stats_;
};
} // namespace atlas
