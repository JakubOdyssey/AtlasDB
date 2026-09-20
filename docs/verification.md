# Final quality verification

Date: 2026-09-20. Execution was local. No Git repository was initialized and no
commit, GitHub operation, publication or deployment was performed. This is a
verification record for a portfolio engine, not an independent certification.

## Scope

The storage format, public API, B+ tree, buffer pool, transaction protocol and
recovery architecture are unchanged. The pass fixes tool/test conversions,
argument parsing, process and thread ownership, diagnostic handling and source
hygiene. `docs/walkthrough.md` is retained and its symbols, commands and stated
behaviors were checked against the implementation.

## Environments

Windows:

* Windows 11 Home 10.0.26200, x86-64, local NTFS.
* Intel Core i9-12900H, 14 physical / 20 logical processors.
* 16,823,709,696 bytes RAM reported by the OS.
* MSVC 19.44.35228.0, Visual Studio Build Tools 2022, CMake 4.4.3.
* Windows SDK 10.0.26100.0; clang-tidy / clang-format 19.1.5.

Linux:

* Alpine Linux 3.24.2, x86-64, musl 1.2.6; kernel 6.18.52-0-virt.
* GCC 15.2.0 and Clang 21.1.8; CMake 4.2.3; Ninja-compatible Samurai 1.2.
* Local QEMU 11.1.0 development build (`v11.1.0-12130-ge470268ff4`), TCG
  emulation on the Windows machine above. GCC VM: four vCPUs / 4096 MiB;
  Clang VM: two vCPUs / 3072 MiB. The Clang VM uses a 1024 MiB translation cache.
* Test databases and WALs are on guest ext4, backed by a local qcow2 file on
  NTFS. `TMPDIR` points to that ext4 volume, not the live system's tmpfs.
* These are real Linux compiler/runtime/POSIX executions under emulation.
  They are not native Linux throughput measurements or power-cut tests.

Clang 22.1.3 was also attempted, but its emulated compilation was stopped due
to very slow frontend processing. It is not counted as a completed build or
successful verification. Clang 21 results below refer to the completed run only.

## Completed test results

There are **37 individual test cases**: 12 unit, 13 integration, 2 property and
10 recovery. CTest exposes **five groups**, including a separate 40-iteration
process-death campaign. The assertions remain active in Release.

| Configuration | Cases | CTest groups | CTest wall time | Compiler warnings |
|---|---:|---:|---:|---:|
| MSVC Debug | 37/37 | 5/5 | 101.81 s | 0 |
| MSVC Release | 37/37 | 5/5 | 35.67 s | 0 |
| MSVC ASan, RelWithDebInfo | 37/37 | 5/5 | 126.94 s | 0 |
| GCC Debug | 37/37 | 5/5 | 1207.32 s | 0 |
| GCC Release | 37/37 | 5/5 | 198.17 s | 0 |
| GCC ASan + UBSan, RelWithDebInfo | 37/37 | 5/5 | 394.35 s | 0 |
| Clang Release | 37/37 | 5/5 | 79.45 s | 0 |

Times include local scheduling/emulation effects and are not performance claims.
Neither the Windows ASan suite nor the Linux ASan/UBSan suite reported a
sanitizer diagnostic. The Linux sanitizer build also completed all eight
benchmark workloads with 100 records and no sanitizer diagnostic.
Each full suite runs 12,000 seeded transaction operations, 6,000 modeled slot
operations and 124 WAL prefix truncation boundaries. The short chaos campaign
checks actual process termination, recovery and the full record model.

## Linux Release crash campaign

GCC Release, seed `20260920`, real `SIGKILL` worker and recovery deaths:

| Measurement | Result |
|---|---:|
| Iterations | 1000 |
| Forced process deaths | 1002 |
| Deaths during recovery | 119 |
| Dirty-shutdown recoveries | 883 |
| Durable commits checked | 478 |
| Unacknowledged commits recovered | 140 |
| Integrity/model failures | 0 |
| Lost durable commits | 0 |

The campaign printed `PASS` and exited successfully. Before WAL synchronization,
an unacknowledged complete commit may recover; after synchronization the complete
new model is mandatory. No partial model is accepted. This tests process death
with the kernel/device still running, not physical power removal.

## Compiler diagnostics

All Linux targets use `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`.
Final builds add `-Werror`; MSVC uses `/W4 /permissive- /WX`. No compiler warning
was disabled to obtain a clean build.

The untouched source snapshot was compiled separately in Release with GCC and
Clang to measure the baseline. Both full baseline builds completed.

| Compiler / diagnostic | Before | After |
|---|---:|---:|
| GCC 15.2, `-Wconversion`: `uint64_t` operation count to `double` | 8 | 0 in Debug, Release and ASan/UBSan builds |
| Clang 21.1.8, `-Wimplicit-int-float-conversion`: same conversion | 8 | 0 |
| Other warnings from the requested GCC/Clang flag set | 0 | 0 |
| MSVC `/W4` warnings | 0 | 0 |

Each compiler reported the same source expression in `benchmarks/main.cpp:21`
in eight `measure` template instantiations: sequential insert, random insert,
point lookup, warm lookup, range scan, single commit, mixed workload and tiny
buffer lookup. Clang's diagnostic is enabled by `-Wconversion`. These are eight
emitted diagnostics per compiler, not eight distinct source defects.

The fixes make bounded RNG conversions explicit, cast the benchmark operation
count intentionally to `double`, and use a signed iterator offset bounded by
the number of slots in one page. Random draws are sequenced explicitly instead
of relying on function argument evaluation order. On the tested Alpine target,
`uint_fast32_t` is `uint32_t`; chaos/property RNG narrowing did not emit a baseline
diagnostic there. The explicit conversions also address platforms where that
type is wider. No unobserved warning is counted as an actual baseline finding.

An additional, mid-pass clang-tidy review of the five changed C++ translation units
reported 13 project findings: two exception-escape diagnostics, three missing
vector reservations, seven unnecessary value parameters and one easily swapped
parameter pair. All were addressed; the final review displayed zero project
diagnostics. Suppressed SDK/STL diagnostics are not project/compiler warnings.

## Robustness and hygiene

* Chaos children are owned through RAII. Failure paths terminate/reap processes
  or close handles; `waitpid` retries `EINTR`, and exit/signal statuses are checked.
* Concurrent tests use scoped `jthread` ownership and stop requests. Exceptions
  in worker threads become failing test assertions rather than escaping threads.
* Tool arguments reject negatives, overflow and trailing characters. A new unit
  case checks numeric boundaries and invalid inputs.
* Benchmark lookups check the optional before dereferencing; batch arithmetic
  and unsigned `iota` avoid wraparound/signed-overflow traps. The Windows CPU
  environment buffer is released even if string construction fails.
* Temporary directory creation refuses collisions before using the directory.
  Failure diagnostics have a final nonallocating fallback.
* Source scanning checks binary/generated files, common secret/token signatures,
  personal absolute paths and broken local documentation links. No findings were
  reported. SHA-256 checks confirmed that all 32 C++/CMake build inputs in both
  Linux guests match the local source directory.
* `.gitignore` covers CMake, Ninja, Make, Visual Studio, compiler/linker output,
  profiling data, temporary databases and local evidence. `.gitattributes`
  keeps text line endings consistent. Neither file requires Git initialization.
* Eighteen old working logs/manifests were archived outside the source tree.
  Only the documented small reference artifacts remain in `docs/results/`.

## Reproduction

Run these from the source directory into new, empty build directories:

```sh
cmake -S . -B out/gcc-debug -G Ninja -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-Werror
cmake --build out/gcc-debug --parallel 2
ctest --test-dir out/gcc-debug --verbose --parallel 2

cmake -S . -B out/gcc-release -G Ninja -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-Werror
cmake --build out/gcc-release --parallel 2
ctest --test-dir out/gcc-release --verbose --parallel 2
out/gcc-release/atlas-chaos --iterations 1000 --seed 20260920

cmake -S . -B out/gcc-sanitizers -G Ninja -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS=-Werror \
  '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O1 -g -DNDEBUG' \
  -DATLAS_ASAN=ON -DATLAS_UBSAN=ON
cmake --build out/gcc-sanitizers --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir out/gcc-sanitizers --verbose --parallel 2

cmake -S . -B out/clang-release -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-Werror
cmake --build out/clang-release --parallel 2
ctest --test-dir out/clang-release --verbose --parallel 2
```

The emulated runs additionally set `ATLAS_TEST_TIMEOUT=7200` and
`ATLAS_CHAOS_TIMEOUT=7200`; defaults remain 240 and 300 seconds. These settings
change time budgets, not test assertions or iteration counts. Use an ext4-backed
`TMPDIR` to reproduce the guest filesystem choice. UBSan is compiled with
`-fno-sanitize-recover=undefined`, so a diagnostic fails the process/test.

Windows uses the README presets. This pass additionally configured
`CMAKE_CXX_FLAGS=/WX` and built/tested Debug, Release and ASan RelWithDebInfo in
separate local build directories outside the source tree.

## Benchmarks and review documents

The three published 10,000-record benchmark runs remain historical Windows
reference data from before this pass. Their medians and ranges were recalculated
and match the README. The README records OS, CPU, memory, filesystem, compiler,
CMake, optimization flags, workload sizes and seeds. The physical drive model
was not recorded; thermal/power state and unrelated OS activity were uncontrolled.
The measurements do not establish superiority over SQLite or another database.

The current Windows and Linux harnesses also completed all eight workloads with 1,000
records, with the expected consumed-row/byte counters. An independently
configured installed-package consumer linked `atlas::atlas`, committed/read a
value and verified its database on both Windows and Linux. These are functional checks, not replacements
for the historical benchmark table.

The walkthrough still references the actual `Transaction::commit`, `Wal::commit`,
private `BufferPool::flush(Frame&)`, `BPlusTree::rebalance`, process-death oracle,
semantic-corruption tests and CLI commands. Its 2,400-key structural test,
root-collapse/free-page-reuse behavior and DOT inspection route match the code.

## Changed files

Build/source policy: `.gitattributes`, `.gitignore`, `CMakeLists.txt`.

Tools: `tools/arguments.hpp` (new), `tools/atlas-chaos/main.cpp`,
`benchmarks/main.cpp`.

Tests: `tests/test.hpp`, `tests/unit/storage.cpp`,
`tests/integration/database.cpp`, `tests/property/model.cpp`.

Documentation: `README.md`, `docs/benchmarks.md`, `docs/correctness.md`,
`docs/verification.md`, `docs/walkthrough.md`, `docs/results/README.md` (new).

Archived from `docs/results/`: `asan-build.txt`, `asan-configure.txt`,
`asan-tests.txt`, `chaos.txt`, `configure.txt`, `debug-build.txt`,
`debug-tests.txt`, `example.txt`, `integrity.txt`, `package-consumer.txt`,
`release-build.txt`, `release-tests.txt`, `source-sha256.txt`,
`static-analysis-recheck.txt`, `static-analysis.txt`, `summary.json`,
`tree.dot`, `wal-inspection.txt`. The nontrivial `tree-example.dot` and its
verification output remain, as do all three original benchmark runs.

## Evidence boundaries

A clean build and passing tests do not prove the absence of all bugs. TSan,
exhaustive syscall/resource-exhaustion injection, fuzzing coverage and physical
power removal were not tested. The Linux runtime is Alpine/musl under local
emulation; other distributions, C libraries and architectures are not implied.
ASan/UBSan findings are reported only for executed paths. Deliberately killed
workers do not perform normal shutdown or exit-time leak checks.

The engine still has one active writer, serialized public calls, no MVCC and
0–128-byte keys / 0–512-byte values. Private write/recovery sets are unbounded in
memory, startup validates the full graph, and WAL growth requires checkpoint or
close. Durability assumes honest OS/device sync behavior. No arbitrary media
corruption recovery, online backup or filesystem-atomic initial two-file creation
is claimed. See the README and transaction/recovery contracts for details.
