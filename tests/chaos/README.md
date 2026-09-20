# Process-death tests

The executable is implemented in `tools/atlas-chaos/main.cpp` and registered as
CTest's `chaos` suite. It is separate from in-process recovery unit tests.

The parent selects a deterministic random operation stream and kill boundary.
The worker announces that boundary through a file rendezvous and waits. The
parent uses `TerminateProcess` on Windows or `SIGKILL` on POSIX; destructors and
normal close paths cannot run. The parent reopens the database and compares
every record to an independent `std::map` model, then checks tree invariants.

Before a complete commit record exists, only the old model is acceptable. After
WAL synchronization, only the complete new model is acceptable. Between the
commit write and its synchronization, either complete model is acceptable; a
partial transaction never is. This deliberately avoids the incorrect assumption
that a missing commit acknowledgment proves a transaction did not commit.

Some recovered transactions are killed again during recovery, cycling through
page redo, the data sync boundary, and the boundary before WAL reset. A failed
campaign retains its directory and prints its seed and iteration for inspection.

This tests process death and selected file damage, not arbitrary hardware power
loss or all possible instruction-level schedules.
