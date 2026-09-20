#pragma once
#include <atlas/index/bplus_tree.hpp>
#include <atlas/recovery/wal.hpp>
#include <atlas/storage/buffer_pool.hpp>
#include <memory>
namespace atlas {
enum class LogLevel { info, debug };
using Logger = std::function<void(LogLevel, std::string_view)>;
struct Options {
    std::size_t buffer_pages = 64;
    bool create_if_missing = true;
    Logger logger;
    // Test instrumentation: invoked while engine mutex is held; must not re-enter.
    FaultHook fault_hook;
};
enum class TransactionState { active, committed, aborted, in_doubt };
struct Statistics {
    std::uint64_t allocated_pages = 0, records = 0, database_bytes = 0, wal_bytes = 0,
                  pages_read = 0, pages_written = 0, wal_bytes_written = 0, commits = 0,
                  rollbacks = 0;
    BufferStats buffer;
    TreeStats tree;
    RecoveryReport recovery;
};
namespace detail {
struct DatabaseImpl;
struct TransactionImpl;
} // namespace detail
class Transaction {
  public:
    Transaction(Transaction &&) noexcept;
    Transaction &operator=(Transaction &&) = delete;
    Transaction(const Transaction &) = delete;
    ~Transaction();
    void put(const std::string &key, const std::string &value);
    bool erase(std::string_view key);
    std::optional<std::string> get(std::string_view key);
    std::vector<Record> scan(std::string_view lower = {},
                             std::optional<std::string_view> upper = std::nullopt,
                             std::size_t limit = static_cast<std::size_t>(-1));
    void commit();
    void rollback();
    TransactionState state() const;

  private:
    friend class Database;
    explicit Transaction(std::unique_ptr<detail::TransactionImpl> impl);
    std::unique_ptr<detail::TransactionImpl> impl_;
};
class Database {
  public:
    explicit Database(const std::filesystem::path &path, Options options = {});
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    Transaction begin();
    std::optional<std::string> get(std::string_view key);
    std::vector<Record> scan(std::string_view lower = {},
                             std::optional<std::string_view> upper = std::nullopt,
                             std::size_t limit = static_cast<std::size_t>(-1));
    Validation verify();
    Statistics stats() const;
    RecoveryReport recovery_report() const;
    void checkpoint();
    void close();

  private:
    std::shared_ptr<detail::DatabaseImpl> impl_;
};
} // namespace atlas
