# ADR 006: Distinguish incomplete suffixes from corrupt history

Status: accepted.

## Context

A torn trailing write is expected after a crash. A complete but checksum-failing
record could instead represent corruption of acknowledged history. Treating
both as an ignorable tail can silently erase durable transactions.

## Decision

Accept EOF within a trailing record as an incomplete suffix. Reject complete
records with checksum/grammar errors and malformed complete headers. Validate
the whole WAL before issuing any redo writes. Discard recovery material only
after data synchronization and complete structural verification.

## Alternatives considered

Stopping at the first bad checksum is permissive but ambiguous. Trying to find
the next magic sequence risks interpreting arbitrary payload bytes as records.
Duplicated logs or stronger storage-level redundancy could repair more damage,
but require a separate design and fault model.

## Consequences and trade-offs

Some genuine torn tails require manual investigation rather than automatic
recovery. That is preferable to silently claiming success after possible data
loss. The CLI's `wal` command inspects the log without starting recovery. Tests
cover both valid truncated prefixes and rejected checksum failures.
