#include "engine/ecs/world.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <vector>
#if defined(__cpp_exceptions)
#include <stdexcept>
#endif

struct Position {
    int x = 0;
    int y = 0;
};
struct Velocity {
    int x = 0;
};
struct Tag {};
struct Tracked {
    static inline int live = 0;
    int value = 0;
    explicit Tracked(int initial = 0) noexcept : value(initial) {
        ++live;
    }
    Tracked(Tracked&& other) noexcept : value(other.value) {
        ++live;
    }
    Tracked& operator=(Tracked&& other) noexcept {
        value = other.value;
        return *this;
    }
    ~Tracked() noexcept {
        --live;
    }
};
#if defined(__cpp_exceptions)
struct Throwing {
    static inline bool fail = false;
    int value = 0;
    explicit Throwing(int initial = 0) : value(initial) {
        if (fail)
            throw std::runtime_error("component construction failed");
    }
    Throwing(Throwing&&) noexcept = default;
    Throwing& operator=(Throwing&&) noexcept = default;
};
#endif
struct alignas(64) OverAligned {
    int value = 0;
    OverAligned() noexcept = default;
    OverAligned(OverAligned&&) noexcept = default;
    OverAligned& operator=(OverAligned&&) noexcept = default;
};
template <std::size_t I> struct IndexedComponent {
    int value = static_cast<int>(I);
};
template <class Sequence> struct WorldFor;
template <std::size_t... I> struct WorldFor<std::index_sequence<I...>> {
    using type = engine::ecs::World<IndexedComponent<I>...>;
};
using TestWorld = engine::ecs::World<Position, Velocity, Tag>;

static void require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}

int main() {
    using namespace engine::ecs;
    TestWorld world;
    const auto first = world.create();
    const auto second = world.spawn();
    const auto third = world.create();
    require(first && second && third, "world creates packed generational entities");
    require(first.index() != second.index() && first.generation() != 0,
            "entity exposes packed index and generation");

    require(world.emplace<Position>(first, Position{10, 11}) &&
                world.emplace<Position>(second, Position{20, 21}),
            "components are emplaced through typed access");
    auto* p0 = world.get<Position>(first);
    auto* p1 = world.get<Position>(second);
    require(p0 && p1, "component lookup returns borrowed pointers");
    require(p1 == p0 + 1, "same archetype component values occupy dense contiguous rows");
    auto* v0 = world.emplace<Velocity>(first, Velocity{7});
    require(v0 != nullptr, "adding a component migrates an entity to another archetype");

    require(world.despawn(first), "despawn moves rows out of the source archetype");
    require(!world.get<Position>(first), "despawn invalidates the old entity identity");
    require(world.get<Position>(second)->x == 20, "swap removal preserves the other entity value");
    const auto reused = world.create();
    require(reused.index() == first.index() && reused.generation() != first.generation(),
            "reused entity slots receive a new generation");
    require(!world.get<Position>(first), "stale generation never resolves to a reused entity");

    auto* position = world.get<Position>(second);
    auto* velocity = world.emplace<Velocity>(second, Velocity{9});
    require(velocity && velocity->x == 9, "emplace adds a missing component to the entity");
    velocity = world.emplace<Velocity>(second, Velocity{12});
    require(velocity && velocity->x == 12, "emplace replaces an existing component value");
    require(world.remove<Position>(second), "remove migrates to the matching archetype");
    require(!world.get<Position>(second) && world.get<Velocity>(second),
            "component removal updates archetype membership");
    require(world.emplace<Position>(second, Position{30, 31}) != nullptr,
            "component can be added again after removal");
    (void)position; // borrowed pointers may be invalidated by structural mutation

    std::size_t queried = 0;
    world.for_each<Position, Velocity>([&](Entity, Position& pos, Velocity& vel) {
        require(pos.x == 30 && vel.x == 12, "query yields matching component references");
        ++queried;
    });
    require(queried == 1, "archetype query skips rows missing requested components");

    {
        TestWorld ranged;
        const auto p0 = ranged.create();
        const auto p1 = ranged.create();
        const auto p2 = ranged.create();
        const auto p3 = ranged.create();
        const auto velocityOnly = ranged.create();
        ranged.emplace<Position>(p0, Position{10, 0});
        ranged.emplace<Position>(p1, Position{20, 0});
        ranged.emplace<Position>(p2, Position{30, 0});
        ranged.emplace<Position>(p3, Position{40, 0});
        ranged.emplace<Velocity>(velocityOnly, Velocity{5});
        ranged.emplace<Velocity>(p2, Velocity{3});
        ranged.emplace<Velocity>(p3, Velocity{4});
        require(ranged.count<Position>() == 4 && ranged.count<Position, Velocity>() == 2,
                "query counts span matching rows in multiple archetypes");

        std::vector<int> covered;
        ranged.for_each_range<Position>(1, 3, [&](Entity, Position& position) {
            covered.push_back(position.x);
            position.x += 1;
        });
        require(covered == std::vector<int>{20, 30},
                "range crossing archetype boundary covers the exact ordinal interval");
        covered.clear();
        ranged.for_each_range<Position>(
            0, 1, [&](Entity, Position& position) { covered.push_back(position.x); });
        ranged.for_each_range<Position>(
            3, 4, [&](Entity, Position& position) { covered.push_back(position.x); });
        require(covered == std::vector<int>{10, 40}, "partial ranges select table-edge rows");

        covered.clear();
        ranged.for_each_range<Position>(
            3, 9, [&](Entity, Position& position) { covered.push_back(position.x); });
        require(covered == std::vector<int>{40},
                "range ending beyond the match count clips at the final matching row");

        std::size_t calls = 0;
        ranged.for_each_range<Position>(4, 9, [&](Entity, Position&) { ++calls; });
        ranged.for_each_range<Position>(2, 2, [&](Entity, Position&) { ++calls; });
        require(calls == 0, "empty and out-of-bounds ranges invoke no callbacks");

        const auto& constRanged = ranged;
        require(constRanged.count<Position>() == 4, "const count matches mutable count");
        constRanged.for_each_range<Position>(1, 3, [&](Entity, const Position& position) {
            static_assert(std::is_const_v<std::remove_reference_t<decltype(position)>>);
            ++calls;
        });
        require(calls == 2, "const range query supplies const component references");

        ranged.emplace<Velocity>(p0, Velocity{6});
        require(ranged.count<Position>() == 4, "adding another component preserves Position count");
        require(ranged.remove<Position>(p1) && ranged.count<Position>() == 3,
                "removing a queried component reduces the count");
        ranged.emplace<Position>(p1, Position{21, 0});
        require(ranged.count<Position>() == 4,
                "adding a removed component restores the count after archetype migration");
    }

    require(world.begin_execution(), "execution phase starts");
    require(!world.begin_execution(), "execution phase cannot nest");
    require(world.create() == Entity::null(), "creation is rejected during execution");
    require(world.emplace<Tag>(second) == nullptr, "component insertion is rejected in phase");
    require(!world.remove<Velocity>(second) && !world.despawn(second),
            "component removal and despawn are rejected in phase");
    require(world.get<Position>(second) != nullptr, "queries remain available during execution");
    world.end_execution();
    require(world.emplace<Tag>(second) != nullptr, "structural changes resume after execution");

    {
        World<Tracked> lifecycle;
        auto tracked = lifecycle.create();
        require(lifecycle.emplace<Tracked>(tracked, 1) != nullptr, "tracked component created");
        require(Tracked::live == 1, "one tracked component is live");
        require(lifecycle.emplace<Tracked>(tracked, 2)->value == 2,
                "replacement destroys the old component exactly once");
        require(Tracked::live == 1, "replacement preserves exact component lifetime");
        require(lifecycle.remove<Tracked>(tracked), "tracked component removed");
        require(Tracked::live == 0, "remove ends the component lifetime");
        require(lifecycle.emplace<Tracked>(tracked, 3) != nullptr, "tracked component reinserted");
        require(lifecycle.despawn(tracked), "tracked entity despawned");
        require(Tracked::live == 0, "despawn ends the component lifetime");
    }
    require(Tracked::live == 0, "all component lifetimes end after remove and despawn");

#if defined(__cpp_exceptions)
    {
        World<Tracked, Throwing> construction;
        const auto entity = construction.create();
        construction.emplace<Tracked>(entity, 41);
        Throwing::fail = true;
        try {
            construction.emplace<Throwing>(entity, 1);
            require(false, "throwing component constructor propagates its exception");
        } catch (const std::runtime_error&) {
        }
        require(construction.get<Tracked>(entity)->value == 41 &&
                    !construction.get<Throwing>(entity),
                "failed insertion leaves the source archetype intact");
        Throwing::fail = false;
        construction.emplace<Throwing>(entity, 5);
        Throwing::fail = true;
        try {
            construction.emplace<Throwing>(entity, 7);
            require(false, "throwing replacement constructor propagates its exception");
        } catch (const std::runtime_error&) {
        }
        Throwing::fail = false;
        require(construction.get<Throwing>(entity)->value == 5,
                "failed replacement preserves the old component");
    }
    require(Tracked::live == 0, "throwing-construction test leaves no live component");
#endif

    {
        engine::ecs::World<OverAligned> aligned;
        const auto entity = aligned.create();
        const auto other = aligned.create();
        aligned.emplace<OverAligned>(entity);
        aligned.emplace<OverAligned>(other);
        auto* value = aligned.get<OverAligned>(other);
        require(reinterpret_cast<std::uintptr_t>(value) % alignof(OverAligned) == 0,
                "dense component columns preserve over-alignment");
    }

    {
        using ManyComponents = typename WorldFor<std::make_index_sequence<70>>::type;
        ManyComponents many;
        const auto entity = many.create();
        require(many.emplace<IndexedComponent<69>>(entity) != nullptr &&
                    many.get<IndexedComponent<69>>(entity)->value == 69,
                "archetype signatures support more than 64 registered component types");
    }

    const auto empty = world.create();
    require(world.despawn(empty), "entity without components can be despawned directly");
    const auto emptyReuse = world.create();
    require(emptyReuse.index() == empty.index() && emptyReuse.generation() != empty.generation(),
            "empty-archetype despawn leaves no live entity row behind");

    std::puts("PASS dense archetype rows, generation reuse, typed query and execution phase");
}
