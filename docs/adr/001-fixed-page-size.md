# ADR 001: Fixed 4 KiB pages and explicit encoding

Status: accepted.

## Context

The cache, tree and recovery need a common bounded I/O unit. ABI-dependent
struct dumps would turn compiler padding and alignment into format decisions.

## Decision

Use 4096-byte pages, explicit little-endian fields, a versioned 64-byte header,
and a CRC over the entire page. Store offsets rather than native pointers.

## Alternatives considered

Variable-size extents complicate allocation and torn-write recovery. Larger
pages improve fanout for large values but increase redo-image costs. Native
struct writes are shorter code but lack a stable portable representation.

## Consequences and trade-offs

All layers use one simple unit and tests can damage precise offsets. Full-page
WAL writes amplify small updates. 4 KiB is a format choice, not a promise that
hardware writes pages atomically. Large values require a future overflow format.
