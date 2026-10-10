#pragma once

#include "engine/ecs/runtime.hpp"
#include "engine/scene/hierarchy.hpp"

#include <cstddef>
#include <memory>

namespace engine::scene {

class TransformPlugin {
  public:
    explicit TransformPlugin(std::size_t initialReserve = 0) : initialReserve_(initialReserve) {}

    template <class World> void build(ecs::Runtime<World>& runtime) const {
        auto* world = &runtime.world();
        auto scratch = std::make_shared<HierarchyScratch>(initialReserve_);
        runtime.addSystem(ecs::Schedule::FixedUpdate, "Propagate transforms",
                          [world, scratch](ecs::Read<LocalTransform>, ecs::Read<ChildOf>,
                                           ecs::Write<GlobalTransform>) {
                              (void)propagate_transforms(*world, *scratch);
                          });
    }

  private:
    std::size_t initialReserve_ = 0;
};

} // namespace engine::scene
