#include "engine/ecs/runtime.hpp"
#include "engine/scene/transform_plugin.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <latch>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

struct Value {
    int value = 0;
};
struct Other {
    int value = 0;
};
struct alignas(64) AlignedPayload {
    int value = 0;
};
struct NonDefaultPayload {
    explicit NonDefaultPayload(int initial) : value(initial) {}
    NonDefaultPayload(const NonDefaultPayload&) = delete;
    NonDefaultPayload& operator=(const NonDefaultPayload&) = delete;
    NonDefaultPayload(NonDefaultPayload&&) noexcept = default;
    NonDefaultPayload& operator=(NonDefaultPayload&&) noexcept = default;
    int value;
};
struct MoveOnlyPayload {
    std::unique_ptr<int> value;
    MoveOnlyPayload() = default;
    explicit MoveOnlyPayload(std::unique_ptr<int> initial) : value(std::move(initial)) {}
    MoveOnlyPayload(const MoveOnlyPayload&) = delete;
    MoveOnlyPayload& operator=(const MoveOnlyPayload&) = delete;
    MoveOnlyPayload(MoveOnlyPayload&&) noexcept = default;
    MoveOnlyPayload& operator=(MoveOnlyPayload&&) noexcept = default;
};
struct LargePayloadA {
    std::array<std::byte, 3000> data{};
};
struct LargePayloadB {
    std::array<std::byte, 3000> data{};
};
struct Position {
    static inline std::atomic<bool> queryActive{false};
    static inline std::atomic<bool> raced{false};
    int value = 0;
    Position() {
        if (queryActive.load(std::memory_order_relaxed))
            raced.store(true, std::memory_order_relaxed);
    }
};
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
struct Bomb {
    static inline bool failConstruction = false;
    Bomb() {
        if (failConstruction)
            throw std::runtime_error("intentional queued construction failure");
    }
    Bomb(const Bomb&) = delete;
    Bomb& operator=(const Bomb&) = delete;
    Bomb(Bomb&&) noexcept = default;
    Bomb& operator=(Bomb&&) noexcept = default;
};
#endif
using World = engine::ecs::World<Value, Other, Position>;

static void require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}

static void runWithHelpers(engine::ecs::SystemScheduler<World>& systems, World& world,
                           engine::jobs::JobScheduler& jobs,
                           const std::function<void()>& beforeRun = {}) {
    std::atomic<bool> done{false};
    std::vector<std::thread> helpers;
    for (int i = 0; i < 2; ++i)
        helpers.emplace_back([&] {
            while (!done.load(std::memory_order_acquire))
                (void)jobs.helpOne();
        });
    if (beforeRun)
        beforeRun();
    const auto result = systems.run(world, jobs);
    done.store(true, std::memory_order_release);
    for (auto& helper : helpers)
        helper.join();
    require(result.success, result.error.empty() ? "system schedule ran" : result.error.c_str());
}

static void access_inference_and_parallel_queries() {
    World world;
    auto entity = world.create();
    world.emplace<Value>(entity, Value{5});
    engine::jobs::JobScheduler jobs;
    engine::ecs::SystemScheduler<World> systems;
    std::latch rendezvous(2);
    std::atomic<int> readCount{0};
    systems.add("read one", [&](engine::ecs::Read<Value> values) {
        static_assert(std::is_same_v<decltype(values.get(entity)), const Value*>);
        require(values.get(entity)->value == 5, "Read<T>::get returns const component pointer");
        const auto callback = [](engine::ecs::Entity, const Value&) {};
        values.for_each(callback);
        values.for_each([&](engine::ecs::Entity, const Value&) { readCount.fetch_add(1); });
        rendezvous.count_down();
        rendezvous.wait();
    });
    systems.add("read two", [&](engine::ecs::Read<Value> values) {
        values.for_each([&](engine::ecs::Entity, const Value&) { readCount.fetch_add(1); });
        rendezvous.count_down();
        rendezvous.wait();
    });
    runWithHelpers(systems, world, jobs);
    require(readCount == 2, "read-only access is inferred and matching systems can overlap");
    require(systems.schedule_cached(), "compiled system graph is cached after first run");
    systems.add("read other", [](engine::ecs::Read<Other>) {});
    require(!systems.schedule_cached(), "registration invalidates prepared graph cache");
}

static void conflicting_writes_are_ordered() {
    World world;
    const auto entity = world.create();
    world.emplace<Value>(entity, Value{});
    engine::jobs::JobScheduler jobs;
    engine::ecs::SystemScheduler<World> systems;
    std::atomic<int> active{0};
    std::atomic<int> overlaps{0};
    for (int i = 0; i < 2; ++i) {
        systems.add("write value", [&](engine::ecs::Write<Value> values) {
            if (active.fetch_add(1) != 0)
                overlaps.fetch_add(1);
            values.for_each([](engine::ecs::Entity, Value& value) { ++value.value; });
            active.fetch_sub(1);
        });
    }
    const auto result = systems.run(world, jobs);
    require(result.success && overlaps == 0, "write access to one component is serialized");
    require(!result.warnings.empty(), "stable schedule-order conflict resolution is diagnosed");
    require(world.get<Value>(entity)->value == 2,
            "serialized writes preserve ordinary component updates");
}

static void within_system_range_fork_join() {
    World world;
    constexpr int initialCount = 10000;
    int expectedWithValue = 0;
    for (int i = 0; i < initialCount; ++i) {
        const auto entity = world.create();
        world.emplace<Position>(entity, Position{})->value = 0;
        if (i % 2 == 0) {
            world.emplace<Value>(entity, Value{1});
            ++expectedWithValue;
        }
    }
    engine::jobs::JobScheduler jobs;
    engine::ecs::SystemScheduler<World> systems;
    std::atomic<int> activeRanges{0};
    std::atomic<int> maximumRanges{0};
    std::size_t downstreamRows = 0;
    const auto update = systems.add_parallel<Position>(
        "partitioned update", 4,
        [&](engine::ecs::Range range, engine::ecs::Write<Position> positions,
            engine::ecs::Read<Value> values) {
            const int active = activeRanges.fetch_add(1) + 1;
            int maximum = maximumRanges.load();
            while (maximum < active && !maximumRanges.compare_exchange_weak(maximum, active)) {
            }
            positions.for_each_range(range, [&](engine::ecs::Entity entity, Position& position) {
                if (const auto* value = values.get(entity))
                    position.value += value->value;
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            activeRanges.fetch_sub(1);
        });
    const auto observe =
        systems.add("join after ranges", [&](engine::ecs::Read<Position> positions) {
            downstreamRows = 0;
            positions.for_each([&](engine::ecs::Entity, const Position&) { ++downstreamRows; });
        });
    require(systems.after(observe, update), "ordinary systems can depend on all ranges completing");
    runWithHelpers(systems, world, jobs);
    require(maximumRanges.load() > 1, "one system dispatches independent ranges to shared workers");
    require(downstreamRows == initialCount,
            "system completion joins every range before successors");
    require(world.count<Position>() == initialCount, "domain rows include matching archetypes");
    int sum = 0;
    world.for_each<Position>(
        [&](engine::ecs::Entity, Position& position) { sum += position.value; });
    require(sum == expectedWithValue, "each partition updates its disjoint domain rows once");
    require(systems.schedule_cached(), "range partition topology is cached");

    const auto additional = world.create();
    world.emplace<Position>(additional, Position{});
    require(systems.schedule_cached(),
            "changing row counts does not invalidate fixed range topology");
    runWithHelpers(systems, world, jobs);
    require(downstreamRows == initialCount + 1,
            "cached range jobs use the current domain row count on each schedule run");

    World spawned;
    engine::ecs::SystemScheduler<World> afterStructuralChange;
    const auto spawn =
        afterStructuralChange.add("spawn domain row", [](engine::ecs::Commands commands) {
            (void)commands.spawn<Position>();
        });
    afterStructuralChange.add_parallel<Position>(
        "update newly spawned rows", 3,
        [](engine::ecs::Range range, engine::ecs::Write<Position> positions) {
            positions.for_each_range(
                range, [](engine::ecs::Entity, Position& position) { ++position.value; });
        });
    std::size_t spawnedRows = 0;
    const auto spawnedObserver = afterStructuralChange.add(
        "observe range result", [&](engine::ecs::Read<Position> positions) {
            spawnedRows = 0;
            positions.for_each([&](engine::ecs::Entity, const Position& position) {
                require(position.value >= 1, "parallel range runs after normal spawn flush");
                ++spawnedRows;
            });
        });
    require(afterStructuralChange.after(engine::ecs::SystemId{1}, spawn) &&
                afterStructuralChange.after(spawnedObserver, engine::ecs::SystemId{1}),
            "range system is ordered after spawning and before its observer");
    require(afterStructuralChange.run(spawned, jobs).success && spawnedRows == 1,
            "parallel domain count sees rows created by an earlier command barrier");
    require(afterStructuralChange.schedule_cached(),
            "structural count update leaves cached graph intact");
    require(afterStructuralChange.run(spawned, jobs).success && spawnedRows == 2,
            "cached range graph recounts rows after each structural flush");
}

static void command_barriers_and_deferred_commands() {
    World world;
    const auto existing = world.create();
    world.emplace<Position>(existing);
    engine::jobs::JobScheduler jobs;
    engine::ecs::SystemScheduler<World> systems;
    std::atomic<bool> releaseReader{false};
    std::atomic<bool> readerStarted{false};
    Position::queryActive = false;
    Position::raced = false;
    std::optional<engine::ecs::EntityToken> token;
    const auto producer = systems.add("queue normal spawn", [&](engine::ecs::Commands commands) {
        token = commands.spawn<Position>();
        commands.add(token.value(), Value{7});
    });
    systems.add("independent active reader", [&](engine::ecs::Read<Position> positions) {
        positions.for_each([&](engine::ecs::Entity, const Position&) {
            Position::queryActive.store(true, std::memory_order_release);
            readerStarted.store(true, std::memory_order_release);
            while (!releaseReader.load(std::memory_order_acquire))
                std::this_thread::yield();
            Position::queryActive.store(false, std::memory_order_release);
        });
    });
    std::size_t dependentCount = 0;
    const auto dependent =
        systems.add("normal spawn dependent", [&](engine::ecs::Read<Position> positions) {
            dependentCount = 0;
            positions.for_each([&](engine::ecs::Entity, const Position&) { ++dependentCount; });
        });
    require(systems.after(dependent, producer), "explicit system dependency is recorded");
    std::thread releaser([&] {
        while (!readerStarted.load(std::memory_order_acquire))
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        releaseReader.store(true, std::memory_order_release);
    });
    runWithHelpers(systems, world, jobs);
    releaser.join();
    require(!Position::raced.load(), "structural flush waits for every active query");
    require(dependentCount == 2,
            "normal commands become visible before explicitly dependent systems");
    require(token && token->ready(),
            "spawn token resolves after its normal command buffer applies");
    const auto previousToken = token.value();
    runWithHelpers(systems, world, jobs);
    require(!previousToken.ready(), "entity tokens expire at the next schedule run");

    engine::ecs::SystemScheduler<World> deferred;
    std::size_t intermediateCount = 99;
    deferred.add("queue deferred spawn",
                 [](engine::ecs::DeferredCommands commands) { (void)commands.spawn<Position>(); });
    const auto observer =
        deferred.add("deferred observer", [&](engine::ecs::Read<Position> positions) {
            intermediateCount = 0;
            positions.for_each([&](engine::ecs::Entity, const Position&) { ++intermediateCount; });
        });
    const auto deferredWriter = engine::ecs::SystemId{0};
    require(deferred.after(observer, deferredWriter), "deferred observer dependency is registered");
    const auto result = deferred.run(world, jobs);
    require(result.success && intermediateCount == 3,
            "deferred commands stay hidden between systems and apply at schedule end");
    std::size_t finalCount = 0;
    world.for_each<Position>([&](engine::ecs::Entity, Position&) { ++finalCount; });
    require(finalCount == 4, "final deferred flush creates its entity");

    World mixedWorld;
    engine::ecs::SystemScheduler<World> mixed;
    std::optional<engine::ecs::EntityToken> mixedToken;
    mixed.add("normal and deferred commands",
              [&](engine::ecs::Commands commands, engine::ecs::DeferredCommands deferredCommands) {
                  mixedToken = commands.spawn<Position>();
                  commands.add(mixedToken.value(), Value{11});
                  deferredCommands.add(mixedToken.value(), Other{12});
              });
    int transitiveValue = 0;
    bool deferredWasHidden = false;
    const auto direct = mixed.add("direct dependent", [&](engine::ecs::Read<Position> positions,
                                                          engine::ecs::Read<Other> others) {
        std::size_t seen = 0;
        positions.for_each([&](engine::ecs::Entity, const Position&) { ++seen; });
        std::size_t deferredSeen = 0;
        others.for_each([&](engine::ecs::Entity, const Other&) { ++deferredSeen; });
        deferredWasHidden = seen == 1 && deferredSeen == 0;
    });
    const auto transitive = mixed.add("transitive dependent", [&](engine::ecs::Read<Value> values) {
        values.for_each([&](engine::ecs::Entity entity, const Value& value) {
            if (entity == mixedToken->entity())
                transitiveValue = value.value;
        });
    });
    require(mixed.after(direct, engine::ecs::SystemId{0}) && mixed.after(transitive, direct),
            "normal command visibility follows transitive logical dependencies");
    require(mixed.run(mixedWorld, jobs).success && deferredWasHidden && transitiveValue == 11,
            "same-producer Commands and DeferredCommands keep their separate visibility windows");
    require(mixedToken && mixedWorld.get<Other>(mixedToken->entity())->value == 12,
            "deferred queue can resolve an entity token from the normal queue");
}

static void dependency_validation_and_runtime_plugins() {
    World world;
    engine::jobs::JobScheduler jobs;
    engine::ecs::SystemScheduler<World> cyclic;
    const auto first = cyclic.add("first", [](engine::ecs::Read<Value>) {});
    const auto second = cyclic.add("second", [](engine::ecs::Read<Other>) {});
    require(cyclic.before(first, second) && cyclic.before(second, first),
            "cycle dependencies are accepted for validation");
    const auto cycle = cyclic.run(world, jobs);
    require(!cycle.success && cycle.error.find("cycle") != std::string::npos,
            "cyclic schedule is rejected with a diagnostic");

    using TransformWorld =
        engine::ecs::World<engine::scene::LocalTransform, engine::scene::GlobalTransform,
                           engine::scene::ChildOf>;
    TransformWorld transforms;
    const auto parent = transforms.create();
    const auto child = transforms.create();
    transforms.emplace<engine::scene::LocalTransform>(parent)->matrix =
        engine::translate({2, 0, 0});
    transforms.emplace<engine::scene::GlobalTransform>(parent);
    transforms.emplace<engine::scene::ChildOf>(parent);
    transforms.emplace<engine::scene::LocalTransform>(child)->matrix = engine::translate({0, 3, 0});
    transforms.emplace<engine::scene::GlobalTransform>(child);
    transforms.emplace<engine::scene::ChildOf>(child)->parent = parent;
    engine::ecs::Runtime<TransformWorld> runtime(transforms, jobs);
    runtime.addPlugin<engine::scene::TransformPlugin>(4);
    require(runtime.run(engine::ecs::Schedule::FixedUpdate).success,
            "transform plugin runs as a declared-access system");
    const auto location =
        engine::point(transforms.get<engine::scene::GlobalTransform>(child)->matrix, {});
    require(std::abs(location.x - 2) < 1e-6 && std::abs(location.y - 3) < 1e-6,
            "runtime transform plugin updates hierarchy globals");

    int startupRuns = 0;
    engine::ecs::Runtime<World> startup(world, jobs);
    startup.addSystem(engine::ecs::Schedule::Startup, "startup once",
                      [&](engine::ecs::Read<Value>) { ++startupRuns; });
    require(startup.run(engine::ecs::Schedule::Startup).success &&
                startup.run(engine::ecs::Schedule::Startup).success && startupRuns == 1,
            "Runtime executes Startup once");
}

static void command_lifecycle_failure_and_stale_tokens() {
    World world;
    const auto stale = world.create();
    world.emplace<Value>(stale, Value{2});
    require(world.despawn(stale), "old generation entity is retired");
    const auto current = world.create();
    world.emplace<Value>(current, Value{6});
    require(current.index() == stale.index() && current.generation() != stale.generation(),
            "entity slot reuse increments its generation");
    engine::jobs::JobScheduler jobs;
    engine::ecs::SystemScheduler<World> systems;
    bool firstRun = true;
    systems.add("stale queued add", [&](engine::ecs::Commands commands) {
        if (firstRun) {
            commands.add(stale, Other{8});
            firstRun = false;
        }
    });
    const auto failed = systems.run(world, jobs);
    require(!failed.success && !world.get<Other>(current),
            "queued structural command rejects a stale generational entity");
    const auto recovered = systems.run(world, jobs);
    require(recovered.success,
            "failed command flush clears buffers and restores schedule usability");

    engine::ecs::SystemScheduler<World> lifecycle;
    lifecycle.add("remove and add component", [current](engine::ecs::Commands commands) {
        commands.remove<Value>(current);
        commands.add(current, Other{3});
    });
    require(lifecycle.run(world, jobs).success && !world.get<Value>(current) &&
                world.get<Other>(current) && world.get<Other>(current)->value == 3,
            "typed remove and add operations apply in buffer order");
    engine::ecs::SystemScheduler<World> destruction;
    destruction.add("despawn entity",
                    [current](engine::ecs::Commands commands) { commands.despawn(current); });
    require(destruction.run(world, jobs).success && !world.get<Other>(current),
            "despawn command retires the selected entity");

    using PayloadWorld = engine::ecs::World<AlignedPayload, NonDefaultPayload, MoveOnlyPayload,
                                            LargePayloadA, LargePayloadB>;
    PayloadWorld payloadWorld;
    const auto payloadEntity = payloadWorld.create();
    engine::ecs::SystemScheduler<PayloadWorld> payloadSystems;
    payloadSystems.add("aligned and move-only payloads",
                       [payloadEntity](engine::ecs::Commands commands) {
                           commands.add(payloadEntity, AlignedPayload{64});
                           commands.add(payloadEntity, NonDefaultPayload{7});
                           commands.add(payloadEntity, MoveOnlyPayload{std::make_unique<int>(9)});
                           commands.add(payloadEntity, LargePayloadA{});
                           commands.add(payloadEntity, LargePayloadB{});
                       });
    require(payloadSystems.run(payloadWorld, jobs).success,
            "reusable command arena supports aligned, non-default, and move-only payloads");
    require(reinterpret_cast<std::uintptr_t>(payloadWorld.get<AlignedPayload>(payloadEntity)) %
                        64 ==
                    0 &&
                payloadWorld.get<NonDefaultPayload>(payloadEntity)->value == 7 &&
                *payloadWorld.get<MoveOnlyPayload>(payloadEntity)->value == 9,
            "payload alignment and ownership survive arena growth");

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    using BombWorld = engine::ecs::World<Bomb>;
    BombWorld bombWorld;
    engine::ecs::SystemScheduler<BombWorld> bombSystems;
    bombSystems.add("throwing queued default construction",
                    [](engine::ecs::Commands commands) { (void)commands.spawn<Bomb>(); });
    Bomb::failConstruction = true;
    const auto bombFailure = bombSystems.run(bombWorld, jobs);
    Bomb::failConstruction = false;
    require(!bombFailure.success, "throwing component construction produces a failed flush result");
    const auto bombRecovery = bombSystems.run(bombWorld, jobs);
    require(bombRecovery.success && bombWorld.count<Bomb>() == 1,
            "throwing deferred construction leaves buffers and execution guard reusable");
#endif
}

int main() {
    access_inference_and_parallel_queries();
    conflicting_writes_are_ordered();
    within_system_range_fork_join();
    command_barriers_and_deferred_commands();
    dependency_validation_and_runtime_plugins();
    command_lifecycle_failure_and_stale_tokens();
    std::puts("PASS inferred ECS systems, structural command barriers, and runtime schedules");
}
