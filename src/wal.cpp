#include <algorithm>
#include <atlas/recovery/wal.hpp>
#include <limits>
#include <set>
namespace atlas {
Wal::Wal(const std::filesystem::path &path, const std::array<std::byte, 16> &identity, bool create)
    : file_(path, create, create), identity_(identity) {
    std::array<std::byte, header_size> header{};
    if (file_.size() == 0 && create) {
        write32(header, 0, 0x574c5441U);
        write16(header, 4, 1);
        write16(header, 6, page_size);
        std::copy(identity.begin(), identity.end(), header.begin() + 8);
        write32(header, 24, checksum(header));
        file_.write(0, header);
        file_.sync();
    } else {
        if (file_.size() < header_size)
            throw RecoveryError("truncated WAL header");
        file_.read(0, header);
        const auto crc = read32(header, 24);
        write32(header, 24, 0);
        if (checksum(header) != crc || read32(header, 0) != 0x574c5441U || read16(header, 4) != 1 ||
            read16(header, 6) != page_size)
            throw RecoveryError("invalid WAL header/checksum/version");
        if (!std::equal(identity.begin(), identity.end(), header.begin() + 8))
            throw RecoveryError("WAL belongs to a different database");
        if (std::any_of(header.begin() + 28, header.end(), [](auto b) { return b != std::byte{}; }))
            throw RecoveryError("invalid WAL reserved bytes");
    }
    end_ = file_.size();
}
WalScan Wal::scan() const {
    WalScan result;
    const auto end = file_.size();
    result.report.dirty_shutdown = end > header_size;
    std::uint64_t offset = header_size, active = 0, last_tx = 0;
    std::map<PageId, Page> pending;
    bool session = false;
    while (offset < end) {
        if (end - offset < record_header_size) {
            result.report.truncated_tail = true;
            break;
        }
        std::array<std::byte, record_header_size> header{};
        file_.read(offset, header);
        auto size = read32(header, 8);
        auto type = static_cast<LogType>(read16(header, 4));
        if (read32(header, 0) != 0x524c5441U || read16(header, 6) != 0 ||
            size < record_header_size || size > record_header_size + page_size)
            throw RecoveryError("invalid WAL record header at offset " + std::to_string(offset));
        const auto expected = type == LogType::page     ? record_header_size + page_size
                              : type == LogType::commit ? record_header_size + 8
                                                        : record_header_size;
        if (size != expected || type < LogType::session || type > LogType::abort)
            throw RecoveryError("invalid WAL type/length");
        if (size > end - offset) {
            result.report.truncated_tail = true;
            break;
        }
        Bytes bytes(size);
        file_.read(offset, bytes);
        const auto crc = read32(bytes, 12);
        write32(bytes, 12, 0);
        if (checksum(bytes) != crc)
            throw RecoveryError("WAL checksum mismatch at offset " + std::to_string(offset) +
                                "; refusing ambiguous corruption");
        const auto lsn = read64(bytes, 16), tx = read64(bytes, 24), page = read64(bytes, 32);
        if (lsn != result.last_lsn + 1)
            throw RecoveryError("non-contiguous WAL LSN");
        result.last_lsn = lsn;
        ++result.report.records_scanned;
        switch (type) {
        case LogType::session:
            if (session || offset != header_size || tx || page)
                throw RecoveryError("misplaced WAL session marker");
            session = true;
            break;
        case LogType::begin:
            if (!session || active || !tx || tx <= last_tx || page)
                throw RecoveryError("invalid WAL transaction begin");
            active = tx;
            last_tx = tx;
            break;
        case LogType::page: {
            if (!active || active != tx || pending.contains(page))
                throw RecoveryError("invalid/duplicate WAL page image");
            Page p;
            std::copy(bytes.begin() + record_header_size, bytes.end(), p.bytes.begin());
            p.validate(page);
            if (p.lsn() != lsn)
                throw RecoveryError("WAL image LSN mismatch");
            pending.emplace(page, p);
            break;
        }
        case LogType::commit:
            if (!active || active != tx || page || pending.empty() || !pending.contains(0) ||
                read64(bytes, 40) != pending.size())
                throw RecoveryError("invalid WAL commit/image count");
            {
                const auto meta = Metadata::decode(pending.at(0));
                if (meta.identity != identity_)
                    throw RecoveryError("WAL metadata image changed immutable database identity");
                for (const auto &[id, p] : pending) {
                    if (id >= meta.next_page)
                        throw RecoveryError("WAL page exceeds committed allocation boundary");
                    result.redo[id] = p;
                }
            }
            pending.clear();
            active = 0;
            ++result.report.committed_transactions;
            break;
        case LogType::abort:
            if (!active || active != tx || page)
                throw RecoveryError("invalid WAL abort");
            pending.clear();
            active = 0;
            ++result.report.incomplete_transactions;
            break;
        }
        offset += size;
    }
    if (active)
        ++result.report.incomplete_transactions;
    if (result.report.truncated_tail)
        result.report.discarded_tail_bytes = end - offset;
    return result;
}
Lsn Wal::append(LogType type, std::uint64_t tx, PageId page, std::span<const std::byte> payload) {
    if (next_lsn_ == std::numeric_limits<Lsn>::max())
        throw LimitError("LSN exhausted");
    Bytes record(record_header_size + payload.size());
    write32(record, 0, 0x524c5441U);
    write16(record, 4, static_cast<std::uint16_t>(type));
    write32(record, 8, static_cast<std::uint32_t>(record.size()));
    write64(record, 16, next_lsn_);
    write64(record, 24, tx);
    write64(record, 32, page);
    std::copy(payload.begin(), payload.end(), record.begin() + record_header_size);
    write32(record, 12, checksum(record));
    file_.write(end_, record);
    end_ += record.size();
    bytes_written_ += record.size();
    return next_lsn_++;
}
void Wal::reset() {
    file_.resize(header_size);
    file_.sync();
    end_ = header_size;
    next_lsn_ = 1;
}
void Wal::start_session() {
    append(LogType::session, 0, 0);
    file_.sync();
}
Lsn Wal::commit(std::uint64_t tx, std::map<PageId, Page> &images, const FaultHook &hook) {
    auto fire = [&](CrashPoint p) {
        if (hook)
            hook(p);
    };
    append(LogType::begin, tx, 0);
    fire(CrashPoint::after_begin);
    for (auto &[id, page] : images) {
        page.set_lsn(next_lsn_);
        page.seal();
        append(LogType::page, tx, id, page.bytes);
        fire(CrashPoint::after_page_log);
    }
    std::array<std::byte, 8> count{};
    write64(count, 0, images.size());
    auto lsn = append(LogType::commit, tx, 0, count);
    fire(CrashPoint::after_commit_record);
    fire(CrashPoint::before_wal_sync);
    file_.sync();
    fire(CrashPoint::after_wal_sync);
    return lsn;
}
} // namespace atlas
