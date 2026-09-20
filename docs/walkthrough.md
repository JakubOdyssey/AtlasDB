# A technical walkthrough

This route is intended for someone reviewing the implementation, with commands
that produce evidence on their own machine. Build Release first. On Unix, use
executables under `build/release/` and omit `.exe`, keeping the same arguments.

The commands and symbols below are checked against the current implementation.
See [verification](verification.md) for the environments actually exercised;
the benchmark table records an earlier Windows snapshot, not Linux VM speed.

## 1. Start with the failure, not the feature list

Ask what happens if a leaf split writes its new root but the right leaf never
reaches disk. Follow `Transaction::commit` into `Wal::commit`: all changed pages
are logged together, and a sync barrier precedes cache installation. Then check
that the private `BufferPool::flush(Frame&)` helper enforces its own durable-LSN
boundary when called by eviction or `flush_all()`.

## 2. Exercise structural change

```powershell
.\build\windows\Release\atlas-tests.exe integration
```

The tree test uses a two-frame cache, inserts 2,400 keys with varying value
lengths, checks depth, deletes every key in shuffled order, and requires actual
merges, redistribution and root collapse. It then reinserts records and checks
that the allocation high-water mark does not grow: free pages are being reused.

Review `BPlusTree::rebalance` alongside the minimum occupancy arithmetic. This
is where an apparently functioning tree often hides deletion bugs.

## 3. Observe atomicity under real process death

```powershell
.\build\windows\Release\atlas-chaos.exe --iterations 100 --seed 81
```

Inspect the parent's oracle in `tools/atlas-chaos/main.cpp`. It has separate
rules for incomplete transactions, synchronized commits and commits that were
written but never acknowledged. Look for why allowing an unacknowledged commit
is correct and why allowing a partially applied transaction never is.

The parent also kills selected recovery processes before their WAL reset. This
tests the requirement that recovery retains its own source until redo is safe.

## 4. Distinguish integrity from checksum validity

The semantic-corruption integration tests alter a separator or a free-list
link and recompute the CRC. Open still fails because structural validation is
independent from checksum validation. Follow the validator's ownership set:
it accounts for tree pages and free pages, detects sharing/cycles, verifies
separator minima and compares tree traversal with the linked leaf chain.

## 5. Inspect the persistent representation

```powershell
.\build\windows\Release\atlas-example.exe example.db
.\build\windows\Release\atlasdb.exe verify example.db
.\build\windows\Release\atlasdb.exe tree example.db > tree.dot
.\build\windows\Release\atlasdb.exe wal example.db
```

`tree.dot` contains actual page IDs and occupancy, not a canned architecture
diagram. `wal` normally reports an empty clean log after these short commands.
To inspect an unclean log, use the evidence directory from a failed campaign
or intentionally pause the worker at a documented test hook in a debugger.
Do not edit or delete WAL as a repair procedure.

## 6. Discuss compromises quantitatively

Run `atlas-bench --records 10000`. Explain why whole-page images amplify a
small update, why two sync barriers affect individual commits, why a six-record
leaf wastes space with 128-byte values, and why a two-frame cache test is not a
cold-disk test. The limitations are useful engineering decisions to revisit,
not claims that the engine outperforms mature databases.

Potential design questions: What extra state would steal require? How would
byte-balanced occupancy change merge legality? What is the reclamation problem
with MVCC? How could a live backup observe a consistent root and WAL boundary?
The current implementation provides concrete places to discuss those changes.
