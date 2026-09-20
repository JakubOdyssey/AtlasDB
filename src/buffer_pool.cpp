#include <atlas/storage/buffer_pool.hpp>
#include <utility>
namespace atlas {
BufferPool::BufferPool(DiskManager &disk, std::size_t capacity) : disk_(disk) {
    if (!capacity)
        throw LimitError("buffer pool capacity must be positive");
    for (std::size_t i = 0; i < capacity; ++i)
        frames_.push_back(std::make_unique<Frame>());
}
BufferPool::Guard::Guard(BufferPool &pool, Frame &frame)
    : pool_(&pool), frame_(&frame), latch_(frame.latch) {}
BufferPool::Guard::Guard(Guard &&other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)), frame_(other.frame_),
      latch_(std::move(other.latch_)) {}
// Mutex/ownership failure while releasing a live guard is unrecoverable; destruction terminates.
// NOLINTNEXTLINE(bugprone-exception-escape)
BufferPool::Guard::~Guard() {
    if (!pool_)
        return;
    latch_.unlock();
    std::lock_guard lock(pool_->mutex_);
    --frame_->pins;
}
void BufferPool::Guard::replace(Page p) {
    p.validate(frame_->page.id());
    frame_->page = p;
    frame_->dirty = true;
}
void BufferPool::flush(Frame &f) {
    if (!f.dirty)
        return;
    if (f.page.lsn() > durable_lsn_)
        throw BufferPoolError("WAL rule: page LSN exceeds durable WAL LSN");
    disk_.write(f.page);
    f.dirty = false;
}
BufferPool::Frame &BufferPool::victim() {
    for (std::size_t i = 0; i < 2 * frames_.size(); ++i) {
        auto &f = *frames_[hand_];
        hand_ = (hand_ + 1) % frames_.size();
        if (f.pins)
            continue;
        if (f.referenced) {
            f.referenced = false;
            continue;
        }
        if (f.valid) {
            flush(f);
            table_.erase(f.page.id());
            ++stats_.evictions;
            f.valid = false;
        }
        return f;
    }
    throw BufferPoolError("all buffer frames are pinned");
}
BufferPool::Guard BufferPool::fetch(PageId id) {
    Frame *frame;
    {
        std::lock_guard lock(mutex_);
        auto it = table_.find(id);
        if (it != table_.end()) {
            frame = it->second;
            ++stats_.hits;
        } else {
            ++stats_.misses;
            frame = &victim();
            frame->page = disk_.read(id);
            table_.emplace(id, frame);
            frame->valid = true;
            frame->dirty = false;
        }
        ++frame->pins;
        frame->referenced = true;
    }
    try {
        return Guard(*this, *frame);
    } catch (...) {
        std::lock_guard lock(mutex_);
        --frame->pins;
        throw;
    }
}
void BufferPool::install(const Page &p) {
    p.validate(p.id());
    std::lock_guard lock(mutex_);
    Frame *f;
    if (auto it = table_.find(p.id()); it != table_.end())
        f = it->second;
    else {
        f = &victim();
        table_.emplace(p.id(), f);
    }
    if (f->pins)
        throw BufferPoolError("cannot install into pinned frame");
    f->page = p;
    f->valid = true;
    f->dirty = true;
    f->referenced = true;
}
void BufferPool::set_durable_lsn(Lsn lsn) {
    std::lock_guard lock(mutex_);
    durable_lsn_ = lsn;
}
void BufferPool::flush_all() {
    std::lock_guard lock(mutex_);
    for (const auto &f : frames_)
        if (f->pins)
            throw BufferPoolError("cannot flush pinned frame");
    for (auto &f : frames_)
        flush(*f);
}
BufferStats BufferPool::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}
} // namespace atlas
