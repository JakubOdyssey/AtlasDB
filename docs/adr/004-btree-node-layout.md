# ADR 004: Slotted records with conservative occupancy

Status: accepted.

## Context

Variable-length records must fit pages during every split, update and merge.
Counting keys alone is unsafe if records can grow without a bound.

## Decision

Bound keys at 128 bytes and values at 512 bytes. Derive maximum node counts from
the worst-case encoded record plus slot size. Use sorted slots and linked
leaves. Internal separators equal right-subtree minima; no parent backpointer
is stored.

## Alternatives considered

Byte-balanced occupancy uses space better but needs carefully defined minimum
fill rules, large-record exceptions and more complicated redistribution.
Fixed-size records eliminate compaction but unnecessarily pad every short value.
Overflow pages enable large values while adding another allocation graph.

## Consequences and trade-offs

Occupancy arithmetic has simple bounds independent of actual record lengths.
Updates cannot require a value-only split beyond the declared capacity. Small
records waste page capacity and increase tree height. This is a deliberate
version-one limitation, measurable in page counts and benchmarks.
