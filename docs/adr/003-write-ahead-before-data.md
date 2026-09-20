# ADR 003: Full-page redo, no steal, force at commit

Status: accepted.

## Context

Tree mutations span several pages. A crash can leave some writes missing or
partial. Recovery must reconstruct committed structure without guessing.

## Decision

Stage all changes privately. Log BEGIN, the complete final page image set and
COMMIT, then synchronize WAL before installing any images. Synchronize database
pages before acknowledging commit. Retain the log until checkpoint/close.

## Alternatives considered

ARIES-style physiological logging, undo and compensation records permit steal
and finer-grained concurrency but substantially expand the proof obligations.
Copy-on-write trees require atomic root publication and safe page reclamation.
Logical operation replay is compact but depends on mutation code and cannot
directly repair a torn page.

## Consequences and trade-offs

Redo is straightforward and idempotent. Rollback never edits disk. A second
buffer-level LSN check enforces write-ahead ordering independently. Small writes
log whole pages, commits perform two sync barriers, and large write sets consume
memory. This design favors auditability over throughput.
