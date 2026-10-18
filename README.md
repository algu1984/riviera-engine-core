# Riviera Engine Core

Riviera Engine Core is a portable C++20 library with dense archetype ECS storage,
signature-based system scheduling, ordinary and end-only structural command
buffers, generic transform relationships, simulation time, immutable snapshots,
and a general task graph/job scheduler. The job scheduler is independent of ECS;
applications and third-party services can keep their own object models. Public
headers retain the established `engine::` namespace and `engine/...` paths.

The core has no operating-system thread creation and no browser, Emscripten, WebGPU, renderer, or physics-engine dependency in its authored library headers and sources. Applications may provide an ECS world that includes both core hierarchy components and their own renderer/game components; hierarchy propagation operates directly on that same typed world.

See [docs/BUILD.md](docs/BUILD.md) for build and verification instructions.
See [docs/CORE_DESIGN.md](docs/CORE_DESIGN.md) for the high-level architecture and design decisions.
See [docs/PERFORMANCE.md](docs/PERFORMANCE.md) for reproducible native benchmarks and their scope.
The latest Core-only native result sample is available at [benchmarks/results/native-sample.json](benchmarks/results/native-sample.json).

## Included modules

Core provides **reusable C++20 engine infrastructure**, not a complete renderer
or game engine. The following pieces are implemented:

| Module | What Core provides |
| --- | --- |
| **ECS — entities and components** | Generational entity IDs, dense archetype storage, component insertion/removal, entity destruction, typed lookups, and component queries ([`world.hpp`](include/engine/ecs/world.hpp)). |
| **ECS — systems and schedules** | Typed `Read<T>` / `Write<T>` access, inferred read/write conflicts, explicit system dependencies, cycle checking, cached task graphs, and parallel range processing ([`access.hpp`](include/engine/ecs/access.hpp), [`system_scheduler.hpp`](include/engine/ecs/system_scheduler.hpp)). |
| **ECS — structural commands** | Per-system `Commands` and `DeferredCommands` for safe entity/component structural changes at scheduling boundaries ([`commands.hpp`](include/engine/ecs/commands.hpp)). |
| **ECS — runtime and plugins** | `Startup`, `FixedUpdate`, `Update`, and `RenderExtract` schedules; system and plugin registration ([`runtime.hpp`](include/engine/ecs/runtime.hpp)). |
| **Job system** | General-purpose task graphs, dependencies, job submission, completion polling, and host-driven worker execution; usable without ECS ([`task_graph.hpp`](include/engine/jobs/task_graph.hpp), [`job_scheduler.hpp`](include/engine/jobs/job_scheduler.hpp)). |
| **Simulation time** | Configurable fixed-step accumulator, step limits, interpolation alpha, and discarded-time tracking ([`fixed_step_clock.hpp`](include/engine/runtime/fixed_step_clock.hpp)). |
| **Simulation ↔ render handoff** | Request-driven reusable two-slot payload exchange for one simulation producer and one render consumer ([`requested_exchange.hpp`](include/engine/runtime/requested_exchange.hpp)); alternatively, immutable previous/current snapshot publication using Xenium guards ([`snapshot_exchange.hpp`](include/engine/runtime/snapshot_exchange.hpp)). |
| **Scene transforms** | Local/global transforms, parent relationships, hierarchy propagation, cycle/stale-parent handling, and an ECS transform plugin ([`hierarchy.hpp`](include/engine/scene/hierarchy.hpp), [`transform_plugin.hpp`](include/engine/scene/transform_plugin.hpp)). |
| **Math** | Small vector, quaternion, and matrix primitives and common transform operations ([`math.hpp`](include/engine/math.hpp)). |

**Boundaries:** Core does **not** create OS threads or Web Workers, own the
application loop, render with WebGPU/Metal, or implement a physics engine. Those
parts belong to the host/application. The ECS component type set is defined at
compile time by `World<Components...>`; systems run when their schedules are
invoked, not through automatic per-object `update()` calls. The render exchanges
provide data handoff primitives, not an automatically launched rendering pipeline.

## Snapshot exchange

`SnapshotExchange<T>` publishes immutable previous/current pairs with Xenium lock-free reference-count guards. A separate lock-free quota limits it to three live snapshot values, including those retained by old reader pairs. Publication and reset can fail while those values remain pinned; callers may retry after releasing snapshots. Pair guards keep old snapshots readable after reset and exchange destruction. The implementation does not use a mutex or `std::atomic<std::shared_ptr>`; the library does not claim that every allocator or C++ runtime operation is lock-free.

## License

Riviera Engine Core's authored code is licensed under the [MIT License](LICENSE), copyright 2026 Alexandr Hmyriak. Third-party dependencies retain their own license terms; see the notices and documentation for each dependency.

## Third-party code

`third_party/xenium/` is the official Xenium submodule pinned at commit `1c449ae953ce2a440b0d16c5ed1181d2754860ab`. Its upstream MIT license and WASM32 portability patch are documented in [docs/XENIUM.md](docs/XENIUM.md). The patch is kept in the core repository and is applied by `make deps`.
