# Core performance benchmarks

`make benchmark` builds and runs the native benchmark harness with `-O3 -DNDEBUG`
and floating-point contraction disabled. The command prints newline-delimited JSON:
one metadata header followed by one record per case. Save clean JSONL with
`make build/core-benchmark && ./build/core-benchmark 101 5 > build/core-benchmark.jsonl`.
The executable also accepts samples and warmups as positional arguments. Defaults
are 31 samples and 2 warmups; both values are clamped to prevent unbounded runs.
No timing threshold is enforced by CI.

## Recorded native sample

The following results were recorded on 2026-10-11 on an Apple M1 Pro MacBook
Pro (10 hardware threads, macOS 26.4.1), using Apple LLVM 17.0.0 and the flags
listed above. Each value is the median of three sequential runs; each run used
31 timed samples and 2 warmups. ECS latency is reported per full update pass
over all entities. The [Core-only JSON sample](../benchmarks/results/native-sample.json)
contains the complete environment, checksums, scheduler and exchange results.

| Entities | Core ECS median ns/pass | p95 ns/pass | Checksum |
| ---: | ---: | ---: | ---: |
| 1,000 | 198.75 | 201.25 | 12,211,798,566 |
| 10,000 | 3,493.75 | 3,847.50 | 123,294,019,222 |
| 100,000 | 34,754.58 | 41,011.25 | 1,233,697,109,572 |

The requested exchange p50/p95 latency medians were 250/4,250 ns for a 64-float
payload and 458/3,958 ns for a 1,024-float payload. No payload capacity growth
occurred during the measured handoffs.

For eight scheduler tasks with one recurrence step each, median times were
792/792/3,709 ns on 1/2/4 workers. With 10,000 steps per task, they were
77,042/43,625/23,041 ns. Every worker count produced the expected checksum.
These are machine-specific observations; the exchange's long p95 tail and
scheduler variability make the raw JSONL more useful than a single ratio.

The ECS cases create 1,000, 10,000, and 100,000 entities with three-float
`Position` and `Velocity` components. Initial values are position
`{i%97, i%53, i%31}` and velocity `{0.5, -0.25, 1.0}`. A sample applies 100
ticks of `position += velocity * (1/120)` to the cached Core world or to
equivalent direct `std::vector` arrays. Setup is outside the timed region;
`setup_ns` covers world and array initialization. Reported median and p95 are
per full update pass over all queried entities, and throughput is entity updates per
second. The final checksum sums each coordinate after conversion to double and truncates the total times
100,000 to an unsigned integer. The harness aborts if Core and direct-array
checksums differ.

The `ecs_query` cases exercise repeated component-domain counts and partition
ranges across 1, 64, or 256 populated component masks. Each mask has a fixed
number of `Position` rows; the first and last range cases count the domain and
visit one of 16 equal ranges, while `count_16_ranges` counts once and visits all
16 ranges. The count case batches 16 domain queries and reports time per query.
Range cases batch 64 repetitions and report per-query or per-sweep time;
`work_iterations` records the batch size.
The `ecs_structure` cases repeatedly add and remove a tag on 128 entities.
`toggle_existing_archetypes` moves among two masks; `toggle_tag_128_masks`
moves among 128 populated masks and their 128 pre-created counterparts. World
construction, population, and counterpart creation are outside the timed region
and reported as `setup_ns`.
Iteration counts and the complete partition sweep are checked before results
are emitted. These cases are intended to isolate query-table scaling and
steady-state structural migration; they are not application-level ECS scores.

The requested-exchange cases use one requesting/acquiring consumer thread and
one producer thread. The consumer submits a request through
`RequestedExchange`, waits with bounded yielding, then acquires the payload.
Both slots are warmed while held concurrently before measurement;
`capacity_growths` counts payload-capacity changes during steady state
(expected zero). The 64-float and 1,024-float cases report full
request-to-acquire latency. `vector_copy_baseline` is a single-threaded copy
with preallocated storage and changing source data; it is a copy-cost reference,
not an equivalent handoff. Each timed sample batches 128 copies and reports
the per-copy median and p95; payload checksums are consumed outside the timed
copy interval. The header records `copies_per_sample`.

The job cases compare a serial execution of the same eight independent task
kernels with a prepared task graph using 1, 2, or 4 total workers. The caller
helps execute tasks, and remaining workers persist across samples. Each task
performs either one or 10,000 integer recurrence steps from a distinct runtime
seed and contributes to a shared atomic checksum. Warmups are excluded, every result is checked against
the expected checksum, and waits have a five-second timeout. `workers` is the
total participating worker count (serial baseline reports one).

Every result record includes `group`, `case`, `units`, `payload_bytes`,
`median_ns`, `p95_ns`, `units_per_second`, `checksum`, `workers`, `setup_ns`,
`capacity_growths`, and `work_iterations`. For ECS, `units` means entities;
exchange, floats; jobs, tasks. `payload_bytes` is zero for ECS and jobs, and is
payload size in bytes for exchange. `work_iterations` is zero outside jobs.
The header records compiler, C++ version, machine,
OS, hardware thread count, compiler flags, warmup count, sample count, and ECS
ticks per sample. Use longer runs on an otherwise idle machine for comparisons;
short smoke runs are useful only to verify execution and output shape.

These measurements describe the specific workloads above and do not establish
general engine performance.

## Range traversal and archetype lookup comparison

A comparison on 2026-10-18 used the same Apple M1 Pro build flags as above,
with 21 samples and 2 warmups per process across three alternating runs. The
baseline is `d65ad198`; “range” adds the `for_each_range` precount removal, and
“range + lookup” adds a mask-to-archetype hash lookup for structural table
resolution.

| Case | Baseline, ns | Range, ns | Range + lookup, ns |
| --- | ---: | ---: | ---: |
| Count + 16 ranges, 64 masks | 1,347.00 | 744.14 | 731.13 |
| Count + 16 ranges, 256 masks | 6,324.22 | 3,826.83 | 3,818.36 |
| Toggle tag across 128 masks | 21,333 | 21,500 | 5,834 |
| Toggle tag across 2 masks | 2,459 | 2,458 | 2,750 |

Values are median times normalized to one listed query case or sweep, not the
full batched sample time.

The lookup maps component masks to stable archetype pointers during table
creation and structural migration. The append-only `tables_` vector remains the
query iteration order, and ordinary component access is unchanged. The large
mask toggle case improved substantially; the two-mask control costs an extra
291 ns per sample, or about 1.14 ns per tag transition across its 256
transitions. These focused cases do not establish whole-system or application
performance.
