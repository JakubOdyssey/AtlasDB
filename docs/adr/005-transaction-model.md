# ADR 005: One writer token and serialized public operations

Status: accepted.

## Context

Concurrent node modification requires latch protocols, lock ordering, isolation
rules and recovery coordination. Claiming concurrency without those rules is
worse than implementing a precise coarse model.

## Decision

One ACTIVE write transaction owns a token. Each method takes the database
mutex. Other database readers may inspect committed state between writer
operations, but commit blocks every other operation. A second begin fails
with BusyError. Exclusive process ownership prevents independent caches from
mutating the same file.

## Alternatives considered

Holding one mutex for the lifetime of a transaction makes same-thread reads and
accidental nested begin deadlock. MVCC permits read snapshots but introduces
version reclamation and visibility rules. Per-page latch coupling does not by
itself provide transaction isolation.

## Consequences and trade-offs

Read-your-writes and committed-only database reads are easy to specify and test.
No transaction holds a native mutex across application code. Public methods
still serialize, there is no writer fairness guarantee, and multiple separate
reads are not a snapshot. Throughput is intentionally limited.
