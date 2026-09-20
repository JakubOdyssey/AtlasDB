#include <algorithm>
#include <atlas/atlas.hpp>
#include <limits>
#include <mutex>
#include <random>
namespace atlas::detail {
struct Workspace : PageAccess {
    BufferPool &pool;
    Metadata meta;
    std::map<PageId, Page> images;
    Workspace(BufferPool &p, Metadata m) : pool(p), meta(m) {}
    Page load(PageId id) override {
        if (id >= meta.next_page)
            throw CorruptionError("page reference exceeds allocation boundary");
        if (auto it = images.find(id); it != images.end())
            return it->second;
        return pool.fetch(id).page();
    }
    void store(Page p) override { images[p.id()] = p; }
    PageId allocate(PageType type) override {
        PageId id;
        if (meta.free_head) {
            id = meta.free_head;
            auto p = load(id);
            if (p.type() != PageType::free)
                throw CorruptionError("allocation from non-free page");
            meta.free_head = p.auxiliary();
        } else {
            if (meta.next_page >=
                std::uint64_t(std::numeric_limits<std::int64_t>::max()) / page_size - 1)
                throw LimitError("database capacity exhausted");
            id = meta.next_page++;
        }
        store(Page::make(id, type));
        return id;
    }
    void release(PageId id) override {
        auto p = Page::make(id, PageType::free);
        p.set_auxiliary(meta.free_head);
        p.seal();
        store(p);
        meta.free_head = id;
    }
    Metadata &metadata() override { return meta; }
};
struct DatabaseImpl {
    mutable std::mutex mutex;
    Options options;
    std::unique_ptr<DiskManager> disk;
    std::unique_ptr<Wal> wal;
    std::unique_ptr<BufferPool> pool;
    Metadata meta;
    RecoveryReport recovery;
    TreeStats tree_stats;
    std::uint64_t active_writer = 0, next_tx = 1, commits = 0, rollbacks = 0;
    bool poisoned = false, closed = false;
    void log(LogLevel level, const std::string &message) const noexcept {
        if (options.logger) {
            try {
                options.logger(level, message);
            } catch (...) {
                // Diagnostics must never turn a successful storage operation into a failure.
                return;
            }
        }
    }
    void usable() const {
        if (closed)
            throw TransactionError("database is closed");
        if (poisoned)
            throw TransactionError("database requires reopen after failed commit/checkpoint");
    }
    void hook(CrashPoint p) const {
        if (options.fault_hook)
            options.fault_hook(p);
    }
    explicit DatabaseImpl(const std::filesystem::path &path, Options opts)
        : options(std::move(opts)) {
        if (!options.buffer_pages)
            throw LimitError("buffer pool capacity must be positive");
        const bool existed = std::filesystem::exists(path);
        auto wal_path = path;
        wal_path += ".wal";
        if (!existed && std::filesystem::exists(wal_path))
            throw InvalidDatabase("orphan WAL exists; refusing to replace it");
        disk = std::make_unique<DiskManager>(path, options.create_if_missing,
                                             !existed && options.create_if_missing);
        if (!existed) {
            std::random_device random;
            for (auto &b : meta.identity)
                b = std::byte(random() & 255U);
            disk->write(meta.encode());
            disk->write(Page::make(1, PageType::leaf));
            disk->sync();
        } else if (disk->bytes() < 2 * page_size)
            throw InvalidDatabase("database is too short");
        // Identity is immutable. Read it before validating mutable metadata so redo can repair a
        // torn metadata page.
        auto raw = disk->read_unchecked(0);
        std::copy_n(raw.bytes.begin() + 96, 16, meta.identity.begin());
        wal = std::make_unique<Wal>(wal_path, meta.identity, !existed);
        auto scanned = wal->scan();
        recovery = scanned.report;
        if (recovery.dirty_shutdown) {
            log(LogLevel::info, "[recovery] dirty shutdown; scanning " +
                                    std::to_string(recovery.records_scanned) + " WAL records");
            for (const auto &[id, page] : scanned.redo) {
                (void)id;
                disk->write(page);
                ++recovery.pages_recovered;
                hook(CrashPoint::recovery_page);
            }
            disk->sync();
            hook(CrashPoint::recovery_data_sync);
        }
        meta = Metadata::decode(disk->read(0));
        if (disk->bytes() != meta.next_page * page_size)
            throw CorruptionError("file size differs from allocation boundary");
        pool = std::make_unique<BufferPool>(*disk, options.buffer_pages);
        Workspace view(*pool, meta);
        BPlusTree(view).validate();
        // Never discard recovery material until redo, database sync and full validation all
        // succeed.
        if (recovery.dirty_shutdown) {
            hook(CrashPoint::recovery_wal_reset);
            wal->reset();
            log(LogLevel::info,
                "[recovery] committed=" + std::to_string(recovery.committed_transactions) +
                    " incomplete=" + std::to_string(recovery.incomplete_transactions) +
                    " pages=" + std::to_string(recovery.pages_recovered) + " integrity=PASS");
        }
        wal->start_session();
    }
    void checkpoint_locked(bool closing) {
        usable();
        if (active_writer)
            throw BusyError("cannot checkpoint/close with an active transaction");
        try {
            pool->flush_all();
            disk->sync();
            wal->reset();
            if (closing) {
                closed = true;
                pool.reset();
                wal.reset();
                disk.reset();
            } else {
                wal->start_session();
                next_tx = 1;
            }
        } catch (...) {
            poisoned = true;
            throw;
        }
    }
    ~DatabaseImpl() {
        if (!closed && !poisoned && !active_writer) {
            try {
                checkpoint_locked(true);
            } catch (const std::exception &e) {
                log(LogLevel::info,
                    std::string("[close] ") + e.what() + "; WAL retained when possible");
            }
        }
    }
};
struct TransactionImpl {
    std::shared_ptr<DatabaseImpl> db;
    Workspace workspace;
    std::uint64_t id;
    TransactionState state = TransactionState::active;
    TransactionImpl(std::shared_ptr<DatabaseImpl> database, std::uint64_t tx)
        : db(std::move(database)), workspace(*db->pool, db->meta), id(tx) {}
    void active() const {
        db->usable();
        if (state != TransactionState::active || db->active_writer != id)
            throw TransactionError("transaction is not active");
    }
    void abort() {
        state = TransactionState::aborted;
        db->active_writer = 0;
        ++db->rollbacks;
        workspace.images.clear();
    }
};
} // namespace atlas::detail
namespace atlas {
Database::Database(const std::filesystem::path &path, Options options)
    : impl_(std::make_shared<detail::DatabaseImpl>(path, std::move(options))) {}
Database::~Database() = default;
Transaction Database::begin() {
    std::lock_guard lock(impl_->mutex);
    impl_->usable();
    if (impl_->active_writer)
        throw BusyError("another write transaction is active");
    if (impl_->next_tx == std::numeric_limits<std::uint64_t>::max())
        throw LimitError("transaction id exhausted; checkpoint required");
    auto id = impl_->next_tx++;
    auto transaction = std::make_unique<detail::TransactionImpl>(impl_, id);
    impl_->active_writer = id;
    return Transaction(std::move(transaction));
}
std::optional<std::string> Database::get(std::string_view key) {
    std::lock_guard lock(impl_->mutex);
    impl_->usable();
    detail::Workspace view(*impl_->pool, impl_->meta);
    return BPlusTree(view).get(key);
}
std::vector<Record> Database::scan(std::string_view lower, std::optional<std::string_view> upper,
                                   std::size_t limit) {
    std::lock_guard lock(impl_->mutex);
    impl_->usable();
    detail::Workspace view(*impl_->pool, impl_->meta);
    return BPlusTree(view).scan(lower, upper, limit);
}
Validation Database::verify() {
    std::lock_guard lock(impl_->mutex);
    impl_->usable();
    auto scan = impl_->wal->scan();
    if (scan.report.truncated_tail || scan.report.incomplete_transactions)
        throw RecoveryError("unexpected incomplete WAL during live verification");
    if (impl_->disk->bytes() != impl_->meta.next_page * page_size)
        throw CorruptionError("database size mismatch");
    // Read through disk as well: cached pages must not conceal persistent corruption.
    for (PageId id = 0; id < impl_->meta.next_page; ++id)
        (void)impl_->disk->read(id);
    BufferPool fresh(*impl_->disk, impl_->options.buffer_pages);
    detail::Workspace view(fresh, Metadata::decode(impl_->disk->read(0)));
    return BPlusTree(view).validate();
}
Statistics Database::stats() const {
    std::lock_guard lock(impl_->mutex);
    impl_->usable();
    Statistics s;
    s.allocated_pages = impl_->meta.next_page;
    s.records = impl_->meta.records;
    s.database_bytes = impl_->disk->bytes();
    s.wal_bytes = impl_->wal->size();
    s.pages_read = impl_->disk->reads();
    s.pages_written = impl_->disk->writes();
    s.wal_bytes_written = impl_->wal->bytes_written();
    s.commits = impl_->commits;
    s.rollbacks = impl_->rollbacks;
    s.buffer = impl_->pool->stats();
    s.tree = impl_->tree_stats;
    s.recovery = impl_->recovery;
    return s;
}
RecoveryReport Database::recovery_report() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->recovery;
}
void Database::checkpoint() {
    std::lock_guard lock(impl_->mutex);
    impl_->checkpoint_locked(false);
}
void Database::close() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->closed)
        impl_->checkpoint_locked(true);
}
Transaction::Transaction(std::unique_ptr<detail::TransactionImpl> impl) : impl_(std::move(impl)) {}
Transaction::Transaction(Transaction &&) noexcept = default;
Transaction::~Transaction() {
    if (impl_) {
        std::lock_guard lock(impl_->db->mutex);
        if (impl_->state == TransactionState::active)
            impl_->abort();
    }
}
void Transaction::put(const std::string &key, const std::string &value) {
    if (!impl_)
        throw TransactionError("moved-from transaction");
    std::lock_guard lock(impl_->db->mutex);
    impl_->active();
    try {
        BPlusTree(impl_->workspace, &impl_->db->tree_stats).put(key, value);
    } catch (...) {
        impl_->abort();
        throw;
    }
}
bool Transaction::erase(std::string_view key) {
    if (!impl_)
        throw TransactionError("moved-from transaction");
    std::lock_guard lock(impl_->db->mutex);
    impl_->active();
    try {
        return BPlusTree(impl_->workspace, &impl_->db->tree_stats).erase(key);
    } catch (...) {
        impl_->abort();
        throw;
    }
}
std::optional<std::string> Transaction::get(std::string_view key) {
    if (!impl_)
        throw TransactionError("moved-from transaction");
    std::lock_guard lock(impl_->db->mutex);
    impl_->active();
    return BPlusTree(impl_->workspace).get(key);
}
std::vector<Record> Transaction::scan(std::string_view lower, std::optional<std::string_view> upper,
                                      std::size_t limit) {
    if (!impl_)
        throw TransactionError("moved-from transaction");
    std::lock_guard lock(impl_->db->mutex);
    impl_->active();
    return BPlusTree(impl_->workspace).scan(lower, upper, limit);
}
void Transaction::commit() {
    if (!impl_)
        throw TransactionError("moved-from transaction");
    auto &tx = *impl_;
    auto &db = *tx.db;
    std::lock_guard lock(db.mutex);
    tx.active();
    if (tx.workspace.images.empty()) {
        tx.state = TransactionState::committed;
        db.active_writer = 0;
        ++db.commits;
        return;
    }
    // Allocation can throw before WAL begins. The transaction remains active in that case.
    tx.workspace.store(tx.workspace.meta.encode());
    try {
        const auto durable = db.wal->commit(tx.id, tx.workspace.images, db.options.fault_hook);
        db.pool->set_durable_lsn(durable);
        for (const auto &[id, page] : tx.workspace.images) {
            (void)id;
            db.pool->install(page);
            db.hook(CrashPoint::after_page_install);
        }
        db.pool->flush_all();
        db.disk->sync();
        db.hook(CrashPoint::after_data_sync);
        db.meta = tx.workspace.meta;
        db.active_writer = 0;
        tx.state = TransactionState::committed;
        ++db.commits;
        db.log(LogLevel::debug, "[commit] tx=" + std::to_string(tx.id) +
                                    " pages=" + std::to_string(tx.workspace.images.size()));
        tx.workspace.images.clear();
    } catch (const std::exception &e) {
        db.poisoned = true;
        db.active_writer = 0;
        tx.state = TransactionState::in_doubt;
        throw CommitOutcomeUnknown(std::string("commit outcome requires reopen: ") + e.what());
    } catch (...) {
        db.poisoned = true;
        db.active_writer = 0;
        tx.state = TransactionState::in_doubt;
        throw;
    }
}
void Transaction::rollback() {
    if (!impl_)
        throw TransactionError("moved-from transaction");
    std::lock_guard lock(impl_->db->mutex);
    impl_->active();
    impl_->abort();
}
TransactionState Transaction::state() const {
    if (!impl_)
        throw TransactionError("moved-from transaction");
    std::lock_guard lock(impl_->db->mutex);
    return impl_->state;
}
} // namespace atlas
