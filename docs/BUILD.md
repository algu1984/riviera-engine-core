# Build and verify

`make build` builds `build/libriviera-engine-core.a`. `make test` runs dense ECS and range traversal, inferred system access and structural commands, runtime schedules and plugins, snapshot/math, hierarchy, general job scheduler, and Xenium portability tests. `make wasm-test` builds and runs pthread-enabled ECS, system scheduling and commands, snapshot, general scheduler, and Xenium portability tests under Node. Emscripten is only needed for that optional target.

`make deps` initializes the pinned Xenium submodule and applies its WASM portability patch. `make deps-check` verifies that local checkout offline. Builds and tests require the dependency to be initialized but never fetch it implicitly. `make boundary-check` checks authored library headers and sources for platform dependencies, blocking synchronization, and include paths outside the standard library, this SDK, and Xenium. `core.mk` exports absolute `CORE_SOURCES`, `CORE_HEADERS`, `CORE_XENIUM_HEADERS`, and `CORE_INCLUDE_FLAGS` for parent builds.

See the [project README](../README.md) for the list of Core modules.
