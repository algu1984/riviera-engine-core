#include "engine/scene/hierarchy.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

struct RenderMarker {
    int value = 7;
};
using World = engine::ecs::World<engine::scene::LocalTransform, engine::scene::GlobalTransform,
                                 engine::scene::ChildOf, RenderMarker>;

static void require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}

static engine::ecs::Entity add(World& world, engine::M4 local, engine::ecs::Entity parent = {}) {
    auto entity = world.create();
    auto transform = world.emplace<engine::scene::LocalTransform>(entity);
    auto global = world.emplace<engine::scene::GlobalTransform>(entity);
    auto relation = world.emplace<engine::scene::ChildOf>(entity);
    auto marker = world.emplace<RenderMarker>(entity);
    if (!entity || !transform || !global || !relation || !marker)
        return {};
    world.get<engine::scene::LocalTransform>(entity)->matrix = local;
    world.get<engine::scene::ChildOf>(entity)->parent = parent;
    return entity;
}

int main() {
    World world;
    const auto parent = add(world, engine::translate({3, 0, 0}));
    const auto child = add(world, engine::translate({0, 2, 0}), parent);
    require(parent && child, "hierarchy entities created in caller-owned world");

    engine::scene::HierarchyScratch scratch;
    const auto result = engine::scene::propagate_transforms(world, scratch);
    const auto* global = world.get<engine::scene::GlobalTransform>(child);
    const auto position = engine::point(global->matrix, {});
    require(result.transformed == 2 && std::abs(position.x - 3) < 1e-6 &&
                std::abs(position.y - 2) < 1e-6,
            "generic propagation composes parent and child transforms");
    require(bool(world.get<RenderMarker>(child)), "non-hierarchy components remain in same world");

    const auto staleChild = add(world, engine::translate({0, 0, 4}), engine::ecs::Entity{~0ull});
    const auto staleResult = engine::scene::propagate_transforms(world, scratch);
    const auto* staleGlobal = world.get<engine::scene::GlobalTransform>(staleChild);
    const auto stalePosition = engine::point(staleGlobal->matrix, {});
    require(staleResult.staleParents == 1 && std::abs(stalePosition.z - 4) < 1e-6,
            "missing parent falls back to local transform");

    const auto descendant = add(world, engine::translate({0, 0, 5}), child);
    require(bool(descendant), "descendant created before cycle mutation");
    auto* parentRelation = world.get<engine::scene::ChildOf>(parent);
    auto* childRelation = world.get<engine::scene::ChildOf>(child);
    parentRelation->parent = child;
    childRelation->parent = parent;
    const auto cycleResult = engine::scene::propagate_transforms(world, scratch);
    require(cycleResult.cycles > 0, "cycle in concurrently mutable hierarchy is detected");
    require(!world.get<engine::scene::GlobalTransform>(parent)->valid &&
                !world.get<engine::scene::GlobalTransform>(child)->valid &&
                !world.get<engine::scene::GlobalTransform>(descendant)->valid,
            "cyclic transforms and their descendants are marked invalid");
    std::puts("PASS generic hierarchy propagation on a shared extensible ECS world");
}
