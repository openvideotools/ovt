# Performance baselines

`<hardware-id>.json` holds the median of every benchmark on one reference machine (`m1-air-8gb`,
`linux-x64-ref`, `pixel-tablet`, `chromebook-hdr`; chapter 14 §14.9.2). `tools/perf_compare.py` fails a
nightly run when a benchmark is more than 20 % worse than its baseline entry (§14.9.4). Baselines of different
machines are never compared.

No baseline is committed yet: the reference runners are registered by the owner (`docs/dev/perf-runners.md`),
and a baseline must come from a real run on that machine. Until a file exists, `perf_compare.py` checks the
absolute budgets only and prints a notice.

## Format

```json
{
  "schema": 1,
  "ovt_meta": { "hardware_id": "m1-air-8gb", "os": "...", "compiler": "...", "commit": "..." },
  "benchmarks": {
    "bench_core_bitreader_u": { "real_time_ms": 1.58, "cpu_time_ms": 1.58, "bytes_per_second": 6.6e8 }
  }
}
```

The values are the median aggregate of the 5 repetitions: times in milliseconds, rates and user counters as the
benchmark reports them.

## Updating a baseline

A maintainer downloads the `perf-<hardware-id>` artefact of a green nightly run and, in a dedicated pull
request titled `perf: update baseline <hardware-id>` that explains the change:

```sh
python3 -I tools/perf_compare.py --accept results/<hardware-id>
git add tests/perf/baselines/<hardware-id>.json
```

Never accept a baseline from a laptop on battery, a busy machine or a debug build.
