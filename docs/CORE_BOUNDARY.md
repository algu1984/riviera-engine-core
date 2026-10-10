# Core boundary and execution contract

Core owns portable C++20 data and execution services: dense ECS storage, system
scheduling, command buffers, transform relationships, simulation time, immutable
snapshot exchange, task graphs, and general job scheduling. It depends on the
standard library and pinned Xenium for the remaining concurrent mechanisms.
Authored library code has no platform, browser, graphics, physics, audio-device,
or operating-system thread-creation dependency.

## World access

Entity identities are world-local index/generation values. Component columns are
dense and ordinary fields are not atomic. Pointers and references are borrowed;
structural mutation, storage growth, and archetype migration can invalidate them.
No component handle keeps an old instance alive after removal or world destruction.

Scheduled read access is immutable. Scheduled write access is exclusive against
other accesses to the same component type. Different systems may access disjoint
component types concurrently. Direct world calls outside scheduling require the
caller to establish ownership and ordering. Structural changes cannot occur while
scheduled systems hold world borrows.

Commands are recorded per producer and applied at exclusive graph boundaries.
Ordinary commands are applied before explicitly dependent consumers; end-only
commands remain pending until schedule completion. Both use generation-checked
entity references. A queued spawn returns a buffer-scoped token, resolved after
application. Tokens expire at the next schedule run and must not outlive their
recording buffer; retain the resolved Entity for longer-lived references. Command
application is not transactional: a failed batch may retain earlier successful
changes, but its remaining payloads are cleared rather than replayed. World
destruction requires all users and schedules to finish.

## Scheduling and memory visibility

System signature analysis builds access sets. Logical order is expressed through
dependencies; ambiguous conflicting systems are diagnosed. Cached schedule topology
is invalidated by registration or dependency changes. Schedules on the same world
are initially ordered by the host, not executed concurrently without analysis.

The general task scheduler remains independent of ECS. Its dependency counters
provide release/acquire visibility of completed predecessor work. Host workers are
owned outside core, and must stop before scheduling state is destroyed. Task
captures and external objects must outlive their accepted work. External library
access is not inferred from a callback's captured objects.

Parallel systems partition a declared component domain into half-open row ranges.
The callback must restrict writes to its assigned entities; unrestricted view
access does not establish disjointness automatically. Other views are joined by
Entity, not by assuming equal row ordinals. All partitions join before successors
or structural application can proceed. Range jobs use the same scheduler and
worker budget, without nested synchronous waits or a second pool.

Authored core algorithms use no mutexes, spinlocks, condition variables, semaphores,
or blocking atomic waits. Atomics protect readiness, ownership, and publication
where needed; ordinary component data relies on scheduling. Required concurrent
atomic types must be lock-free on the target. Allocation, constructors, destructors,
and application callbacks are outside the algorithmic progress guarantee.

## Immutable snapshots

Snapshot publication is independent of ECS Commands. Readers acquire immutable
previous/current pairs and retain their storage, including after reset or exchange
teardown. Publication and reset may decline when the bounded live-value quota is
occupied. A slow reader does not require the producer to wait. Published payloads
cannot safely alias mutable ECS component storage.

The snapshot exchange and ready-job queue retain their Xenium lifetime mechanisms.
Xenium is no longer the component storage or lifetime model. Its upstream pin and
portability patch remain core-owned because those independent concurrent services
still depend on it. See [XENIUM.md](XENIUM.md).

## Integration

The engine imports core through its public build manifest. Sources can be compiled
into the same application or linked through a separate core archive. There is no
required process boundary or copy of the ECS. Generic transform propagation acts
on the same world as application components; renderer extraction and physics
integration remain consumer responsibilities.

`make boundary-check` audits authored library code for platform dependencies,
blocking synchronization, and includes outside the allowed boundary. Tests and
vendored implementation code are outside that authored-library scan.
