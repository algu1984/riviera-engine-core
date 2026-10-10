# Riviera Engine Core

Riviera Engine Core is a portable C++20 library with dense archetype ECS storage,
signature-based system scheduling, ordinary and end-only structural command
buffers, generic transform relationships, simulation time, immutable snapshots,
and a general task graph/job scheduler. The job scheduler is independent of ECS;
applications and third-party services can keep their own object models. Public
headers retain the established `engine::` namespace and `engine/...` paths.

The core has no operating-system thread creation and no browser, Emscripten, WebGPU, renderer, or physics-engine dependency in its authored library headers and sources. Applications may provide an ECS world that includes both core hierarchy components and their own renderer/game components; hierarchy propagation operates directly on that same typed world.

See [docs/CORE_DESIGN.md](docs/CORE_DESIGN.md) for the high-level architecture and design decisions.

## Build and verify

`make build` builds `build/libriviera-engine-core.a`. `make test` runs dense ECS and range traversal, inferred system access and structural commands, runtime schedules and plugins, snapshot/math, hierarchy, general job scheduler, and Xenium portability tests. `make wasm-test` builds and runs pthread-enabled ECS, system scheduling and commands, snapshot, general scheduler, and Xenium portability tests under Node. Emscripten is only needed for that optional target.

`make deps` initializes the pinned Xenium submodule and applies its WASM portability patch. `make deps-check` verifies that local checkout offline. Builds and tests require the dependency to be initialized but never fetch it implicitly. `make boundary-check` checks authored library headers and sources for platform dependencies, blocking synchronization, and include paths outside the standard library, this SDK, and Xenium. `core.mk` exports absolute `CORE_SOURCES`, `CORE_HEADERS`, `CORE_XENIUM_HEADERS`, and `CORE_INCLUDE_FLAGS` for parent builds.

## Snapshot exchange

`SnapshotExchange<T>` publishes immutable previous/current pairs with Xenium lock-free reference-count guards. A separate lock-free quota limits it to three live snapshot values, including those retained by old reader pairs. Publication and reset can fail while those values remain pinned; callers may retry after releasing snapshots. Pair guards keep old snapshots readable after reset and exchange destruction. The implementation does not use a mutex or `std::atomic<std::shared_ptr>`; the library does not claim that every allocator or C++ runtime operation is lock-free.

## Third-party code

`third_party/xenium/` is the official Xenium submodule pinned at commit `1c449ae953ce2a440b0d16c5ed1181d2754860ab`. Its upstream MIT license and WASM32 portability patch are documented in [docs/XENIUM.md](docs/XENIUM.md). The patch is kept in the core repository and is applied by `make deps`.
