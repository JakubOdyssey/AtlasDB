# Benchmark methodology

`atlas-bench --records 10000` uses a fixed seed (20260920), 16-byte keys and
128-byte values. The default buffer holds 64 pages (256 KiB of page payload).
Fresh temporary databases are used for each executable invocation and removed
only after success. Failure leaves evidence behind.

The harness prints compiler, build mode, available CPU identifier, key/value
sizes, page size, buffer size, seed and commit batch size. Every measured lookup
contributes returned bytes to a printed counter, so the work cannot be removed
as unused. It verifies populated and final trees.

| Workload | Measurement unit | Scope |
|---|---|---|
| Sequential insert | records/s | Ascending keys; transactions of 100; final verify included |
| Random insert | records/s | Seeded permutation; transactions of 100; final verify included |
| Random point lookup | gets/s | Existing keys in permutation order |
| Warm lookup | gets/s | Repeated 100-key working set, explicitly pre-read |
| Range scan | scans/s | 1,000 scans, each returning exactly 100 records |
| Single-record commit | transactions/s | 100 synchronous one-record updates |
| Mixed | operations/s | 80% reads, 20% separately committed updates; 1,000 operations |
| Tiny-buffer lookup | gets/s | Reopened database with two frames; OS cache remains warm |

The tiny-buffer test is **not** a cold-storage benchmark. Startup validates all
pages before timing; the harness does not purge the OS page cache. It explores
buffer pressure and repeated I/O calls while exposing the cache miss/eviction
counts. It must not be presented as disk random-read bandwidth.

Run Release on an otherwise idle machine, repeat at least three times, and
report the individual runs or their range, not only the fastest. The checked-in
results preserve every measured workload and the original environment lines.
The three published runs precede the final warning/harness cleanup; they remain
reference measurements of that earlier snapshot. Final verification exercises
the current harness separately without replacing those historical timings.
Numbers are observations of this machine, not comparative claims against mature
databases. No SQLite comparison is made.

The conservative six-record leaf capacity, whole-page WAL, checksum validation
on decoded nodes, repeated separator descent, two commit barriers and full
startup verification are intentional costs. They explain results more honestly
than a single headline throughput value.
