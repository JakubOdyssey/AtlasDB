# Storage format, version 1

All integers are unsigned little-endian. Pages are exactly 4,096 bytes. Page ID
`p` maps to byte offset `p * 4096`; page zero is metadata. Zero is also the null
reference where a tree/free-list link is expected. Metadata cannot be a child.
No C++ object representation is serialized.

## Common page header

| Offset | Width | Meaning |
|---:|---:|---|
| 0 | 4 | Magic `ATLS` |
| 4 | 2 | Format version: 1 |
| 6 | 2 | Type: metadata=1, leaf=2, internal=3, free=4 |
| 8 | 8 | Page ID |
| 16 | 8 | LSN of this image in its WAL session |
| 24 | 4 | CRC-32 of all 4096 bytes, with this field zeroed |
| 28 | 2 | Number of slots, including deleted slots |
| 30 | 2 | End of slot directory: `64 + 4 * slot_count` |
| 32 | 2 | Start of packed/fragmented payload area |
| 34 | 2 | Flags: must be zero |
| 36 | 8 | Auxiliary link; interpretation depends on page type |
| 44 | 20 | Reserved; must be zero |

CRC-32 uses the reflected IEEE polynomial `0xEDB88320`, initial state
`0xFFFFFFFF`, and final complement. The standard `123456789` vector produces
`0xCBF43926`. This detects accidental corruption; it is not authentication.

```text
0              64                lower            upper               4096
+--------------+------------------+----------------+--------------------+
| page header  | 4-byte slots ---> | free/fragmented | <--- record bytes |
+--------------+------------------+----------------+--------------------+
```

Each slot is `{offset:u16, length:u16}`. Length zero is a tombstone. Live records
must lie at or above `upper`, end within the page, and never overlap. Deleting a
slot leaves a hole; insertion reuses a tombstone and compacts if total space is
sufficient but contiguous space is not. Compaction preserves slot indexes.
The B+ tree rewrites its small node image in sorted slot order and therefore
does not retain tombstones between node rewrites.

## Page-specific payloads

Metadata uses fixed fields rather than slots; slot count is zero, lower=64,
upper=4096. Fields are root page ID at 64, allocation high-water mark at 72,
free-list head at 80, record count at 88 (all u64), and immutable 16-byte database
identity at 96. The file contains exactly `next_page` pages. Allocation takes
the free-list head or increments the high-water mark; deletion does not shrink
the file. A free page's auxiliary field points to the next free page.

A leaf's auxiliary field is its next leaf. A leaf record contains
`key_length:u16, value_length:u16, key bytes, value bytes`.
Empty keys and empty values are valid. Maximum key length is 128 bytes; maximum
value length is 512 bytes. Keys are unique and compared using `std::string`
lexicographic ordering; no collation, Unicode normalization or text encoding is
imposed. Binary keys/values are supported by the API.

An internal page's auxiliary field is its leftmost child. Each slot contains
`key_length:u16, right_child:u64, key bytes`. Separator `i` equals the smallest
key in child `i+1`; equality descends to the right child. Parent pointers are
not persisted. Traversal validates parent/child ownership and key ranges.

## WAL header

The sidecar is the database path with `.wal` appended. Both files are required.

| Offset | Width | Meaning |
|---:|---:|---|
| 0 | 4 | Magic `ATLW` |
| 4 | 2 | Version 1 |
| 6 | 2 | Page size 4096 |
| 8 | 16 | Database identity |
| 24 | 4 | CRC-32 of 64-byte header, checksum field zeroed |
| 28 | 36 | Reserved zero bytes |

The identity prevents replaying another database's log by accident. It is
generated using `std::random_device`; it is not a security credential.

## WAL records

| Offset | Width | Meaning |
|---:|---:|---|
| 0 | 4 | Magic `ATLR` |
| 4 | 2 | Type: SESSION=1, BEGIN=2, PAGE=3, COMMIT=4, ABORT=5 |
| 6 | 2 | Reserved zero |
| 8 | 4 | Total length, including 40-byte header |
| 12 | 4 | CRC-32 of entire record, with this field zeroed |
| 16 | 8 | Contiguous session-local LSN, starting at 1 |
| 24 | 8 | Transaction ID; zero for SESSION |
| 32 | 8 | Page ID; zero for non-PAGE records |
| 40 | variable | Payload |

PAGE contains exactly one complete 4096-byte page. Its page LSN equals its
record LSN. COMMIT contains an 8-byte image count; all other types have no
payload. A modifying transaction must contain metadata page zero exactly once.
Each transaction logs each changed page once, in ascending page ID order.

Grammar: `SESSION (BEGIN PAGE+ COMMIT | BEGIN PAGE* ABORT)*` plus an incomplete
suffix after a crash. Transaction IDs increase within a session. Rollback
normally generates no records because no uncommitted page reaches disk; ABORT
is understood by the scanner but the current writer does not need to emit it.

LSNs reset after a completed checkpoint. Persisted page LSNs are diagnostic
session-local numbers, not a global timestamp and not used to skip redo across
sessions. Recovery installs the latest committed full image unconditionally.
The current session's durable LSN gates dirty-page flushing.

Version 1 has no migration mechanism. Unknown versions, malformed bounds,
unexpected sizes and checksum failures are errors, not guessed formats.
