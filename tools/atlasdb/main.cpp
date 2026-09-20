#include <algorithm>
#include <atlas/atlas.hpp>
#include <iomanip>
#include <iostream>
namespace {
void wal_report(const std::filesystem::path &path) {
    atlas::DiskManager disk(path, false);
    const auto raw = disk.read_unchecked(0);
    std::array<std::byte, 16> identity{};
    std::copy_n(raw.bytes.begin() + 96, 16, identity.begin());
    auto wal_path = path;
    wal_path += ".wal";
    atlas::Wal wal(wal_path, identity, false);
    const auto scan = wal.scan();
    std::cout << "Dirty session: " << std::boolalpha << scan.report.dirty_shutdown
              << "\nRecords: " << scan.report.records_scanned << "\nLast LSN: " << scan.last_lsn
              << "\nCommitted transactions: " << scan.report.committed_transactions
              << "\nIncomplete transactions: " << scan.report.incomplete_transactions
              << "\nDistinct redo pages: " << scan.redo.size()
              << "\nTruncated tail: " << scan.report.truncated_tail
              << "\nTail bytes: " << scan.report.discarded_tail_bytes << '\n';
}
void graph(const std::filesystem::path &path) {
    atlas::DiskManager disk(path, false);
    const auto meta = atlas::Metadata::decode(disk.read(0));
    std::cout << "digraph AtlasDB {\n  node [shape=record fontname=Consolas];\n";
    for (atlas::PageId id = 1; id < meta.next_page; ++id) {
        auto page = disk.read(id);
        if (page.type() == atlas::PageType::free)
            continue;
        atlas::SlottedPage slots(page);
        slots.validate();
        std::cout << "  p" << id << " [label=\"" << (id == meta.root ? "ROOT | " : "")
                  << (page.type() == atlas::PageType::leaf ? "leaf" : "internal") << " " << id
                  << " | records " << slots.slots() << " | free " << slots.free_bytes()
                  << " B | LSN " << page.lsn() << "\"];\n";
        if (page.type() == atlas::PageType::internal) {
            std::cout << "  p" << id << " -> p" << page.auxiliary() << ";\n";
            for (std::size_t slot = 0; slot < slots.slots(); ++slot)
                std::cout << "  p" << id << " -> p" << atlas::read64(slots.record(slot), 2)
                          << ";\n";
        } else if (page.auxiliary())
            std::cout << "  p" << id << " -> p" << page.auxiliary()
                      << " [style=dashed color=blue constraint=false];\n";
    }
    std::cout << "}\n";
}
} // namespace
int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "Usage: atlasdb create|put|get|delete|scan|verify|stats|checkpoint|tree|wal "
                     "DATABASE "
                     "[KEY [VALUE]]\n";
        return 2;
    }
    try {
        const std::string command = argv[1];
        if (command == "wal" && argc == 3) {
            wal_report(argv[2]);
            return 0;
        }
        atlas::Options options;
        options.create_if_missing = command == "create";
        options.logger = [](atlas::LogLevel level, std::string_view text) {
            if (level == atlas::LogLevel::info)
                std::cerr << text << '\n';
        };
        atlas::Database db(argv[2], options);
        if (command == "create" && argc == 3)
            std::cout << "Database initialized: " << argv[2] << '\n';
        else if (command == "put" && argc == 5) {
            auto tx = db.begin();
            tx.put(argv[3], argv[4]);
            tx.commit();
        } else if (command == "get" && argc == 4) {
            auto v = db.get(argv[3]);
            if (!v)
                return 3;
            std::cout << *v << '\n';
        } else if (command == "delete" && argc == 4) {
            auto tx = db.begin();
            const bool found = tx.erase(argv[3]);
            tx.commit();
            if (!found)
                return 3;
        } else if (command == "scan" && (argc == 3 || argc == 4)) {
            const std::string prefix = argc == 4 ? argv[3] : "";
            for (const auto &[k, v] : db.scan(prefix)) {
                if (!k.starts_with(prefix))
                    break;
                std::cout << std::quoted(k) << '\t' << std::quoted(v) << '\n';
            }
        } else if (command == "verify" && argc == 3) {
            auto v = db.verify();
            std::cout << "Page checksums ........ PASS\nB+ tree structure ..... PASS\nLeaf "
                         "ordering ......... PASS\nFree-page accounting .. PASS\nWAL consistency "
                         "....... PASS\n\nDatabase integrity: PASS\nPages: "
                      << v.pages << " Records: " << v.records << " Height: " << v.height
                      << " Free pages: " << v.free_pages << '\n';
        } else if (command == "stats" && argc == 3) {
            const auto s = db.stats();
            const auto v = db.verify();
            std::cout << "Records: " << s.records << "\nAllocated pages: " << s.allocated_pages
                      << "\nTree height: " << v.height << "\nFree pages: " << v.free_pages
                      << "\nDatabase bytes: " << s.database_bytes << "\nWAL bytes: " << s.wal_bytes
                      << "\nSession pages read: " << s.pages_read
                      << "\nSession pages written: " << s.pages_written
                      << "\nBuffer hits: " << s.buffer.hits
                      << "\nBuffer misses: " << s.buffer.misses
                      << "\nEvictions: " << s.buffer.evictions
                      << "\nRecovered transactions: " << s.recovery.committed_transactions << '\n';
        } else if (command == "tree" && argc == 3) {
            db.verify();
            db.close();
            graph(argv[2]);
            return 0;
        } else if (command == "checkpoint" && argc == 3)
            db.checkpoint();
        else {
            std::cerr << "Unknown command or invalid argument count\n";
            return 2;
        }
        db.close();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "AtlasDB: " << e.what() << '\n';
        return 1;
    }
}
