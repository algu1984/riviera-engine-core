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
31 timed samples and 2 warmups. ECS latency is normalized to one entity update
tick. The matching checksums are shown to make the work comparable.

| Entities | Core ECS ns/tick | Direct arrays ns/tick | Checksum |
| ---: | ---: | ---: | ---: |
| 1,000 | 198.75 | 334.59 | 12,211,798,566 |
| 10,000 | 3,493.75 | 3,725.00 | 123,294,019,222 |
| 100,000 | 34,754.58 | 36,180.84 | 1,233,697,109,572 |

The exchange p50/p95 latency medians were 250/4,250 ns for a 64-float payload
and 458/3,958 ns for a 1,024-float payload. Corresponding vector-copy reference
times were 4.56/5.53 ns and 52.73/53.06 ns per copy. No payload capacity growth
occurred during the measured handoffs.

For eight jobs, serial versus 1, 2, and 4 total workers took 41/792/792/3,709 ns
for one recurrence step per task, and 76,500/77,042/43,625/23,041 ns for 10,000
steps per task. All worker counts produced the same expected checksum in every
sample. These are machine-specific observations; the exchange's long p95 tail
and scheduler variability make the raw JSONL more useful than a single ratio.
The three source runs are retained under the ignored `build` directory when
the benchmark is run locally.

The ECS cases create 1,000, 10,000, and 100,000 entities with three-float
`Position` and `Velocity` components. Initial values are position
`{i%97, i%53, i%31}` and velocity `{0.5, -0.25, 1.0}`. A sample applies 100
ticks of `position += velocity * (1/120)` to the cached Core world or to
equivalent direct `std::vector` arrays. Setup is outside the timed region;
`setup_ns` covers world and array initialization. Reported median and p95 are
normalized to one entity update pass, and throughput is entity updates per
second. The final checksum sums each coordinate after conversion to double and truncates the total times
100,000 to an unsigned integer. The harness aborts if Core and direct-array
checksums differ.

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
