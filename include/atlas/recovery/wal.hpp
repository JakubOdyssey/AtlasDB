#pragma once
#include <atlas/storage/file.hpp>
#include <functional>
#include <map>
namespace atlas {
enum class CrashPoint {
    after_begin,
    after_page_log,
    after_commit_record,
    before_wal_sync,
    after_wal_sync,
    after_page_install,
    after_data_sync,
    recovery_page,
    recovery_data_sync,
    recovery_wal_reset
};
using FaultHook = std::function<void(CrashPoint)>;
enum class LogType : std::uint16_t { session = 1, begin = 2, page = 3, commit = 4, abort = 5 };
struct RecoveryReport {
    bool dirty_shutdown = false, truncated_tail = false;
    std::uint64_t records_scanned = 0, committed_transactions = 0, incomplete_transactions = 0,
                  pages_recovered = 0, discarded_tail_bytes = 0;
};
struct WalScan {
    RecoveryReport report;
    std::map<PageId, Page> redo;
    Lsn last_lsn = 0;
};
class Wal {
  public:
    static constexpr std::size_t header_size = 64, record_header_size = 40;
    Wal(const std::filesystem::path &path, const std::array<std::byte, 16> &identity, bool create);
    WalScan scan() const;
    void reset();
    void start_session();
    Lsn commit(std::uint64_t tx, std::map<PageId, Page> &images, const FaultHook &hook);
    std::uint64_t bytes_written() const { return bytes_written_; }
    std::uint64_t size() const { return file_.size(); }

  private:
    Lsn append(LogType type, std::uint64_t tx, PageId page,
               std::span<const std::byte> payload = {});
    File file_;
    std::array<std::byte, 16> identity_;
    std::uint64_t end_ = header_size, bytes_written_ = 0;
    Lsn next_lsn_ = 1;
};
} // namespace atlas
