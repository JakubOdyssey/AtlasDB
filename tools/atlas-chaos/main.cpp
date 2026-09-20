#include "../arguments.hpp"
#include <atlas/atlas.hpp>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace {
using Model = std::map<std::string, std::string>;
std::string key(unsigned n) {
    auto s = std::to_string(n);
    return std::string(6 - s.size(), '0') + s;
}
template <class Put, class Erase> void operations(unsigned seed, Put put, Erase erase) {
    std::mt19937 rng(seed);
    for (unsigned i = 0; i < 32; ++i) {
        auto k = key(static_cast<unsigned>(rng() % 512));
        if (rng() % 4 == 0)
            erase(k);
        else {
            // Sequence draws explicitly: function argument evaluation order is unspecified.
            const auto length = rng() % 513;
            const auto character = static_cast<char>('a' + rng() % 26);
            put(k, std::string(length, character));
        }
    }
}
atlas::CrashPoint point(unsigned mode) {
    switch (mode) {
    case 0:
        return atlas::CrashPoint::after_begin;
    case 1:
        return atlas::CrashPoint::after_page_log;
    case 2:
        return atlas::CrashPoint::before_wal_sync;
    case 3:
        return atlas::CrashPoint::after_wal_sync;
    case 4:
        return atlas::CrashPoint::after_page_install;
    case 5:
        return atlas::CrashPoint::after_data_sync;
    case 8:
        return atlas::CrashPoint::recovery_page;
    case 9:
        return atlas::CrashPoint::recovery_data_sync;
    default:
        return atlas::CrashPoint::recovery_wal_reset;
    }
}
void rendezvous(const std::filesystem::path &marker) {
    {
        std::ofstream f(marker);
        f << "ready\n";
        f.flush();
        if (!f)
            throw atlas::IoError("cannot write crash rendezvous");
    }
    for (;;)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
struct WorkerArguments {
    unsigned seed;
    unsigned mode;
};
int worker(const std::filesystem::path &path, WorkerArguments arguments,
           const std::filesystem::path &marker) {
    const auto [seed, mode] = arguments;
    atlas::Options opts;
    opts.buffer_pages = 2;
    unsigned installs = 0;
    if (mode != 6 && mode != 7)
        opts.fault_hook = [&](auto p) {
            if (p == point(mode)) {
                if (mode == 4 && ++installs < 3)
                    return;
                rendezvous(marker);
            }
        };
    atlas::Database db(path, opts);
    if (mode >= 8)
        throw atlas::Error("recovery did not reach requested failpoint");
    auto tx = db.begin();
    operations(
        seed, [&](const auto &k, const auto &v) { tx.put(k, v); },
        [&](const auto &k) { tx.erase(k); });
    if (mode == 7)
        rendezvous(marker);
    tx.commit();
    db.close();
    return 0;
}
void process(const std::filesystem::path &executable, const std::filesystem::path &path,
             unsigned seed, unsigned mode, const std::filesystem::path &marker, bool kill) {
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    if (ec)
        throw atlas::IoError("cannot clear crash rendezvous: " + ec.message());
#ifdef _WIN32
    auto quote = [](const std::filesystem::path &p) { return L"\"" + p.wstring() + L"\""; };
    auto command = quote(executable) + L" --worker " + quote(path) + L" " + std::to_wstring(seed) +
                   L" " + std::to_wstring(mode) + L" " + quote(marker);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child))
        throw atlas::IoError("CreateProcess failed: " + std::to_string(GetLastError()));
    CloseHandle(child.hThread);
    struct ChildGuard {
        HANDLE handle;
        bool finished = false;
        ~ChildGuard() {
            if (!finished) {
                TerminateProcess(handle, 74);
                WaitForSingleObject(handle, INFINITE);
            }
            CloseHandle(handle);
        }
    } guard{child.hProcess};
    bool killed = false, finished = false;
    for (unsigned tick = 0; tick < 6000; ++tick) {
        if (kill && std::filesystem::exists(marker)) {
            if (!TerminateProcess(child.hProcess, 73))
                throw atlas::IoError("TerminateProcess failed");
            killed = true;
            break;
        }
        const auto result = WaitForSingleObject(child.hProcess, 5);
        if (result == WAIT_FAILED)
            throw atlas::IoError("worker wait failed");
        if (result == WAIT_OBJECT_0) {
            finished = true;
            break;
        }
    }
    if (!finished && !killed)
        throw atlas::Error("worker timed out before rendezvous");
    if (WaitForSingleObject(child.hProcess, 5000) != WAIT_OBJECT_0)
        throw atlas::IoError("worker did not exit after termination");
    guard.finished = true;
    DWORD code = 0;
    if (!GetExitCodeProcess(child.hProcess, &code))
        throw atlas::IoError("cannot read worker exit code");
    if (kill ? (!killed || code != 73) : code != 0)
        throw atlas::Error("unexpected worker exit " + std::to_string(code));
#else
    const auto s = std::to_string(seed), m = std::to_string(mode);
    const auto pid = fork();
    if (pid < 0)
        throw atlas::IoError("fork failed");
    if (pid == 0) {
        execl(executable.c_str(), executable.c_str(), "--worker", path.c_str(), s.c_str(),
              m.c_str(), marker.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    struct ChildGuard {
        pid_t pid;
        bool reaped = false;
        ~ChildGuard() {
            if (!reaped) {
                ::kill(pid, SIGKILL);
                while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
                }
            }
        }
        bool wait(int &status, int flags) {
            pid_t result;
            do {
                result = waitpid(pid, &status, flags);
            } while (result < 0 && errno == EINTR);
            if (result < 0) {
                // ECHILD means ownership is gone; never signal a possibly reused PID.
                if (errno == ECHILD)
                    reaped = true;
                throw atlas::IoError("waitpid failed");
            }
            reaped = result == pid;
            return reaped;
        }
    } guard{pid};
    bool killed = false, finished = false;
    int status = 0;
    for (unsigned tick = 0; tick < 6000; ++tick) {
        if (kill && std::filesystem::exists(marker)) {
            if (::kill(pid, SIGKILL) != 0)
                throw atlas::IoError("SIGKILL failed");
            killed = true;
            break;
        }
        if (guard.wait(status, WNOHANG)) {
            finished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!finished && !killed)
        throw atlas::Error("worker timed out before rendezvous");
    if (!finished)
        guard.wait(status, 0);
    if (kill ? (!killed || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
             : (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
        throw atlas::Error("worker failed or timed out");
#endif
}
Model contents(atlas::Database &db) {
    auto rows = db.scan();
    return Model(rows.begin(), rows.end());
}
} // namespace
int main(int argc, char **argv) try {
    std::filesystem::path directory;
    try {
        if (argc == 6 && std::string(argv[1]) == "--worker")
            return worker(argv[2],
                          WorkerArguments{.seed = atlas::tools::unsigned_argument(argv[3]),
                                          .mode = atlas::tools::unsigned_argument(argv[4])},
                          argv[5]);
        unsigned iterations = 1000, seed = 20260920;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 >= argc)
                throw atlas::Error("Usage: atlas-chaos [--iterations N] [--seed N]");
            if (std::string(argv[i]) == "--iterations")
                iterations = atlas::tools::unsigned_argument(argv[i + 1]);
            else if (std::string(argv[i]) == "--seed")
                seed = atlas::tools::unsigned_argument(argv[i + 1]);
            else
                throw atlas::Error("unknown chaos option");
        }
        if (!iterations)
            throw atlas::Error("iterations must be positive");
        directory = std::filesystem::temp_directory_path() /
                    ("atlas-chaos-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(directory))
            throw atlas::IoError("chaos temporary directory already exists");
        auto path = directory / "chaos.db", marker = directory / "ready";
        {
            atlas::Database db(path);
            db.close();
        }
        std::mt19937 rng(seed);
        Model model;
        std::uint64_t deaths = 0, recoveries = 0, committed = 0, ambiguous_commits = 0,
                      recovery_deaths = 0;
        std::cout << "AtlasDB Chaos Campaign\nSeed: " << seed
                  << "\nDirectory: " << directory.string() << '\n'
                  << std::flush;
        for (unsigned i = 0; i < iterations; ++i) {
            // mt19937 produces exactly 32 bits; modulo results fit the target range.
            const auto operation_seed = static_cast<unsigned>(rng());
            const unsigned mode = i < 8 ? i : static_cast<unsigned>(rng() % 8);
            Model candidate = model;
            operations(
                operation_seed, [&](const auto &k, const auto &v) { candidate[k] = v; },
                [&](const auto &k) { candidate.erase(k); });
            process(std::filesystem::absolute(argv[0]), path, operation_seed, mode, marker,
                    mode != 6);
            if (mode != 6)
                ++deaths;
            if (mode >= 3 && mode <= 5 && i % 3 == 0) {
                process(std::filesystem::absolute(argv[0]), path, 0,
                        8 + static_cast<unsigned>(recovery_deaths % 3), marker, true);
                ++deaths;
                ++recovery_deaths;
            }
            atlas::Database db(path);
            auto actual = contents(db);
            db.verify();
            const bool must_commit = mode >= 3 && mode <= 6, may_commit = mode == 2;
            if (must_commit) {
                if (actual != candidate)
                    throw atlas::Error("lost durable commit at iteration " + std::to_string(i));
                model = candidate;
                ++committed;
            } else if (may_commit && actual == candidate) {
                model = candidate;
                ++ambiguous_commits;
            } else if (actual != model)
                throw atlas::Error("atomicity/model mismatch at iteration " + std::to_string(i) +
                                   " operation seed=" + std::to_string(operation_seed));
            if (db.recovery_report().dirty_shutdown)
                ++recoveries;
            db.close();
            if ((i + 1) % 100 == 0)
                std::cout << "Verified " << i + 1 << " iterations\n" << std::flush;
        }
        std::cout << "Iterations: " << iterations << "\nForced process deaths: " << deaths
                  << "\nDeaths during recovery: " << recovery_deaths
                  << "\nDirty-shutdown recoveries: " << recoveries
                  << "\nDurable commits checked: " << committed
                  << "\nUnacknowledged commits recovered: " << ambiguous_commits
                  << "\nIntegrity/model failures: 0\nLost durable commits: 0\nPASS\n";
        // These zero counts are reached only after every comparison and verifier has succeeded.
        std::filesystem::remove_all(directory);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\nEvidence retained at: " << directory.string()
                  << '\n';
        return 1;
    }
} catch (...) {
    // Even formatting the normal diagnostic can fail (for example, allocation failure).
    std::fputs("FAIL: unexpected exception; diagnostic unavailable\n", stderr);
    return 1;
}
