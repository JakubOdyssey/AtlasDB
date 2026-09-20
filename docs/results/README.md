# Reference artifacts

These small, text-only artifacts are intentionally part of the documentation:

* `benchmark-1.txt` through `benchmark-3.txt`: original Windows Release runs,
  10,000 records each, measured on 2026-09-20 before the final quality pass.
  See [methodology](../benchmarks.md) and the README for environment and limits.
* `machine.json`: the OS, CPU, memory and filesystem recorded for those runs.
* `tree-example.dot` and `tree-example-verify.txt`: the inspected 120-record,
  43-page, height-three example tree from the original verification.

Current compiler, sanitizer and test results are documented in
[verification](../verification.md). Historical benchmark timings are not
substituted for measurements of the current revision. Machine-local build logs,
temporary databases, crash evidence and toolchains are kept outside the source
directory; no executable or generated build tree is needed to review this data.
