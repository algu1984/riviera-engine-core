#include "engine/ecs/world.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

struct Counter {
    int value = 0;
};
struct Marker {};
using World = engine::ecs::World<Counter, Marker>;

static void require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::abort();
    }
}

int main() {
    World world;
    const auto entity = world.create();
    auto* counter = world.emplace<Counter>(entity, Counter{4});
    require(counter && counter->value == 4, "seed component before systems execute");
    for (int i = 0; i < 64; ++i) {
        auto e = world.create();
        require(world.emplace<Counter>(e, Counter{i}) != nullptr, "populate query rows");
        if (i % 2 == 0)
            require(world.emplace<Marker>(e) != nullptr, "populate second table");
    }

    require(world.begin_execution(), "scheduler opens the read/query phase");
    std::atomic<int> oneComponent{0}, twoComponents{0};
    std::thread first([&] {
        world.for_each<Counter>([&](engine::ecs::Entity, const Counter& value) {
            oneComponent.fetch_add(value.value, std::memory_order_relaxed);
        });
    });
    std::thread second([&] {
        world.for_each<Counter, Marker>(
            [&](engine::ecs::Entity, const Counter& value, const Marker&) {
                twoComponents.fetch_add(value.value, std::memory_order_relaxed);
            });
    });
    first.join();
    second.join();
    require(oneComponent == 4 + (63 * 64) / 2 && twoComponents > 0 &&
                world.get<Counter>(entity)->value == 4,
            "queries and component access are allowed in the execution phase");
    require(!world.create() && !world.emplace<Counter>(entity) && !world.remove<Counter>(entity) &&
                !world.despawn(entity),
            "all structural mutation is rejected until the phase ends");
    world.end_execution();
    require(world.remove<Counter>(entity), "exclusive structural work resumes after the phase");
    std::puts("PASS ECS execution phase excludes structural mutation");
}
