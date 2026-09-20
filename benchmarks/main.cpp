#include "../tools/arguments.hpp"
#include <algorithm>
#include <atlas/atlas.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
namespace {
using Clock = std::chrono::steady_clock;
std::string key(unsigned n) {
    auto s = std::to_string(n);
    return std::string(16 - s.size(), '0') + s;
}
template <class F> void measure(const char *name, std::uint64_t operations, F body) {
    const auto start = Clock::now();
    const auto consumed = body();
    const auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << name << ',' << operations << ',' << std::fixed << std::setprecision(6) << seconds
              << ',' << std::setprecision(1) << static_cast<double>(operations) / seconds << ','
              << consumed << '\n';
}
std::string cpu() {
#ifdef _WIN32
    char *value = nullptr;
    std::size_t size = 0;
    const auto error = _dupenv_s(&value, &size, "PROCESSOR_IDENTIFIER");
    const std::unique_ptr<char, decltype(&std::free)> owner(value, &std::free);
    if (error == 0 && owner)
        return std::string(owner.get());
#else
    std::ifstream file("/proc/cpuinfo");
    std::string line;
    while (std::getline(file, line))
        if (line.starts_with("model name"))
            return line.substr(line.find(':') + 2);
#endif
    return "unavailable";
}
std::size_t lookup_bytes(atlas::Database &db, const std::string &k) {
    const auto value = db.get(k);
    if (!value)
        throw atlas::Error("benchmark missing key");
    return value->size();
}
} // namespace
int main(int argc, char **argv) try {
    std::filesystem::path directory;
    try {
        unsigned n = 10000;
        if (argc == 3 && std::string(argv[1]) == "--records")
            n = atlas::tools::unsigned_argument(argv[2]);
        else if (argc != 1)
            throw atlas::Error("Usage: atlas-bench [--records N]");
        if (n < 100)
            throw atlas::Error("at least 100 records required");
#ifdef __clang__
        std::cout << "compiler=Clang " << __clang_version__ << '\n';
#elif defined(_MSC_VER)
        std::cout << "compiler=MSVC " << _MSC_FULL_VER << '\n';
#else
        std::cout << "compiler=GCC " << __VERSION__ << '\n';
#endif
#ifdef NDEBUG
        std::cout << "build=Release\n";
#else
        std::cout << "build=Debug\n";
#endif
        std::cout << "cpu=" << cpu() << "\nrecords=" << n
                  << " key_bytes=16 value_bytes=128 page_bytes=4096 buffer_pages=64 seed=20260920 "
                     "commit_batch=100\n";
        std::cout << "workload,operations,seconds,operations_per_second,consumed\n";
        directory = std::filesystem::temp_directory_path() /
                    ("atlas-bench-" + std::to_string(Clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(directory))
            throw atlas::IoError("benchmark temporary directory already exists");
        std::vector<unsigned> order(n);
        std::iota(order.begin(), order.end(), 0U);
        std::mt19937 rng(20260920);
        std::shuffle(order.begin(), order.end(), rng);
        const std::string value(128, 'v');
        atlas::Database sequential(directory / "sequential.db"), random(directory / "random.db");
        auto inserts = [&](atlas::Database &db, bool shuffled) {
            for (unsigned start = 0; start < n;) {
                const auto end = start + std::min(n - start, 100U);
                auto tx = db.begin();
                for (unsigned j = start; j < end; ++j)
                    tx.put(key(shuffled ? order[j] : j), value);
                tx.commit();
                start = end;
            }
            return db.verify().records;
        };
        measure("sequential_insert", n, [&] { return inserts(sequential, false); });
        measure("random_insert", n, [&] { return inserts(random, true); });
        measure("point_lookup_random", n, [&] {
            std::uint64_t bytes = 0;
            for (auto k : order)
                bytes += lookup_bytes(random, key(k));
            return bytes;
        });
        for (unsigned i = 0; i < 100; ++i)
            (void)random.get(key(i));
        measure("warm_lookup_100_key_set", n, [&] {
            std::uint64_t bytes = 0;
            for (unsigned i = 0; i < n; ++i)
                bytes += lookup_bytes(random, key(i % 100));
            return bytes;
        });
        measure("range_scan_100", 1000, [&] {
            std::uint64_t rows = 0;
            for (unsigned i = 0; i < 1000; ++i)
                rows += random.scan(key((i * 97) % (n - 99)), std::nullopt, 100).size();
            return rows;
        });
        measure("single_record_commit", 100, [&] {
            for (unsigned i = 0; i < 100; ++i) {
                auto tx = random.begin();
                tx.put(key(i), value);
                tx.commit();
            }
            return 100;
        });
        measure("mixed_80_read_20_write", 1000, [&] {
            std::uint64_t bytes = 0;
            for (unsigned i = 0; i < 1000; ++i) {
                auto k = key(order[i % n]);
                if (i % 5) {
                    bytes += lookup_bytes(random, k);
                } else {
                    auto tx = random.begin();
                    tx.put(k, value);
                    tx.commit();
                }
            }
            return bytes;
        });
        sequential.close();
        random.close();
        atlas::Options small;
        small.buffer_pages = 2;
        atlas::Database reopened(directory / "random.db", small);
        measure("tiny_buffer_lookup_os_cache_warm", n, [&] {
            std::uint64_t bytes = 0;
            for (auto k : order)
                bytes += lookup_bytes(reopened, key(k));
            return bytes;
        });
        auto stats = reopened.stats();
        reopened.verify();
        reopened.close();
        std::cout << "tiny_buffer_pages_read=" << stats.pages_read
                  << " misses=" << stats.buffer.misses << " evictions=" << stats.buffer.evictions
                  << "\n";
        std::cout << "Notes: sync commits; insert batches include final integrity verification; "
                     "reopen validation precedes timing; OS cache is NOT flushed; no claim of "
                     "cold-device throughput.\n";
        std::filesystem::remove_all(directory);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\nEvidence: " << directory.string() << '\n';
        return 1;
    }
} catch (...) {
    std::fputs("Benchmark failed: unexpected exception; diagnostic unavailable\n", stderr);
    return 1;
}
