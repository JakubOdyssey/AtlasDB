# Correctness evidence and review guide

The invariant is stronger than “get returns what put wrote”: every committed
state must be a well-formed disk graph, and every acknowledged modifying
transaction must be present after a process crash under the documented sync
assumptions.

## Tests by responsibility

| Suite | Evidence |
|---|---|
| Unit | Known CRC vector, explicit encoding, corrupt headers, overlapping slots, fragmentation, failed-update preservation, exact disk I/O, exclusive ownership, pinned eviction, WAL flush barrier, concurrent guards, strict tool argument ranges |
| Integration | Read-your-writes, rollback/destructor abort, states, errors, binary/empty bounds, depth-three trees, internal splits, redistribution, merges, root collapse, free-page reuse, reopen, checkpoint, concurrent readers/writers, semantic corruption with valid CRC |
| Property | Three deterministic 4,000-operation transaction streams compared to `std::map`, with validation after each batch; 6,000 independently modeled slot operations |
| Recovery | Redo, incomplete groups, truncated/corrupt WAL, torn data and metadata, interrupted recovery, systematic record-boundary prefix truncation |
| Chaos | Real worker-process deaths and recovery-process deaths with an independent parent model |

The dependency-free test runner implements exception-aware assertions and
returns a failing exit code. Assertions are not `assert()` and remain active in
Release. CTest groups test cases by responsibility; a CTest group count is not
the same as the number of individual cases. Seeds are printed before property
tests, and failed chaos directories are retained.

## Why the crash oracle allows two states at one boundary

Between writing COMMIT and synchronizing it, an abrupt process death may leave
the entire record in the OS cache. Reopen can legitimately discover the new
transaction even though the client saw no acknowledgment. The oracle permits
either complete state only at that boundary. Once WAL synchronization has
returned, it requires the new state. Before a complete commit record, it
requires the old state. In all cases, mixed old/new contents fail.

The chaos worker is terminated by its parent, not by a thrown exception. File
rendezvous makes the selected fault boundary reproducible. Seeded boundary
selection varies the workload and crash location. It is systematic boundary
coverage, not exhaustive instruction-level concurrency exploration.

## Corruption layers

CRC failures catch byte damage. Separate structural tests recompute CRCs after
damaging a separator or making a free-list cycle. These prove that a valid
checksum does not cause the engine to trust malformed topology. `verify()`
loads fresh persistent pages, accounts for every page and checks the WAL grammar.

## Sanitizers and static analysis

CMake supports AddressSanitizer, UndefinedBehaviorSanitizer and a separate
ThreadSanitizer build. MSVC supports the ASan configuration used on this machine;
UBSan and TSan options reject MSVC instead of silently producing an unsanitized
build. Unix Clang/GCC presets expose those tools where supported. Availability
is not evidence of a successful run: see `verification.md` for actual results.

Warnings are enabled for all project targets (`/W4 /permissive-` or
`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`). Optional clang-tidy rules focus
on bugs, portability and performance. No dependency downloads are required.

## A useful deep review order

1. Read `Transaction::commit` and the two independent WAL-before-data checks.
2. Read `Wal::scan`, especially its treatment of EOF versus checksum failure.
3. Follow startup through redo, data sync, full validation and WAL reset.
4. Audit the occupancy arithmetic in B+ tree split/merge/rebalance.
5. Check guard destruction against eviction and metadata locking.
6. Inspect the parent process's crash oracle and its ambiguous-commit case.
7. Run model tests with another seed and change a failure boundary deliberately.

## Remaining evidence gaps

The code does not prove freedom from all bugs. It has no formal verification,
independent external review, fuzzing service or device power-cut campaign.
The platform coverage actually exercised is listed in [verification](verification.md). Runtime
resource exhaustion and disk-full behavior produce typed errors, but exhaustive
syscall-by-syscall failure injection is future work. There is no recovery from
arbitrary corrupted history, and no complete concurrency schedule exploration.
