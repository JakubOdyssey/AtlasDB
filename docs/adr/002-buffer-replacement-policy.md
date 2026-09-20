# ADR 002: CLOCK with pinned, latched frames

Status: accepted.

## Context

The engine needs bounded committed-page caching and safe page lifetimes without
making transaction semantics depend on cache capacity.

## Decision

Use CLOCK reference bits and a rotating hand. A page-to-frame hash table serves
lookups. Move-only guards pin a frame and latch its bytes. Never evict pinned
frames. A dirty victim passes the WAL boundary check before being written.

## Alternatives considered

Exact LRU requires recency-list mutation on every hit. Random replacement is
simpler but ignores reuse. Caching uncommitted pages would require undo or
strictly reserving enough cache frames for every transaction.

## Consequences and trade-offs

Metadata has a single mutex; page latches serialize users of the same frame.
The database mutex currently limits overall parallelism before the pool does.
CLOCK is approximate recency, not optimal replacement. An all-pinned cache
raises an explicit error. Private write images consume additional memory.
