#pragma once

#include "../ecs/world.hpp"
#include "../math.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::scene {

using M4 = engine::M4;
using V3 = engine::V3;
using Entity = engine::ecs::Entity;

struct LocalTransform {
    M4 matrix = M4::identity();
};

struct GlobalTransform {
    M4 matrix = M4::identity();
    bool valid = false;
};

struct ChildOf {
    Entity parent{};
};

struct PropagationResult {
    std::size_t transformed = 0;
    std::size_t staleParents = 0;
    std::size_t cycles = 0;
};

enum class ParentPolicy { MissingParentUsesLocalTransform };
inline constexpr ParentPolicy staleParentPolicy = ParentPolicy::MissingParentUsesLocalTransform;

class HierarchyScratch;
namespace detail {
bool resolve_hierarchy_node(HierarchyScratch&, std::size_t, PropagationResult&);
}

class HierarchyScratch {
  public:
    explicit HierarchyScratch(std::size_t initialReserve = 0) {
        nodes_.reserve(initialReserve);
        indices_.reserve(initialReserve);
        path_.reserve(initialReserve);
    }
    HierarchyScratch(const HierarchyScratch&) = delete;
    HierarchyScratch& operator=(const HierarchyScratch&) = delete;

  private:
    template <class World> friend PropagationResult propagate_transforms(World&, HierarchyScratch&);
    friend bool detail::resolve_hierarchy_node(HierarchyScratch&, std::size_t, PropagationResult&);

    struct Node {
        Entity entity{};
        Entity parent{};
        M4 local = M4::identity();
        M4 global = M4::identity();
        std::uint8_t state = 0;
    };
    std::vector<Node> nodes_;
    std::vector<std::pair<std::uint64_t, std::size_t>> indices_;
    std::vector<std::size_t> path_;
};

using PropagationScratch = HierarchyScratch;

namespace detail {
inline bool resolve_hierarchy_node(HierarchyScratch& scratch, std::size_t index,
                                   PropagationResult& result) {
    auto& nodes = scratch.nodes_;
    auto& path = scratch.path_;
    if (nodes[index].state == 2)
        return true;
    if (nodes[index].state == 3)
        return false;
    path.clear();
    std::size_t current = index;

    while (path.size() < nodes.size()) {
        auto& node = nodes[current];
        if (node.state == 2 || node.state == 3)
            break;
        if (node.state == 1) {
            for (std::size_t pathIndex : path)
                nodes[pathIndex].state = 3;
            ++result.cycles;
            path.clear();
            return false;
        }
        node.state = 1;
        path.push_back(current);
        if (!node.parent) {
            node.global = node.local;
            node.state = 2;
            break;
        }
        auto parent =
            std::lower_bound(scratch.indices_.begin(), scratch.indices_.end(), node.parent.value,
                             [](const auto& entry, std::uint64_t id) { return entry.first < id; });
        if (parent == scratch.indices_.end() || parent->first != node.parent.value) {
            ++result.staleParents;
            node.global = node.local;
            node.state = 2;
            break;
        }
        current = parent->second;
    }

    if (path.empty())
        return nodes[index].state == 2;
    if (path.size() == nodes.size() && nodes[current].state == 1) {
        for (std::size_t pathIndex : path)
            nodes[pathIndex].state = 3;
        ++result.cycles;
        return false;
    }

    bool validChain = true;
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        auto& node = nodes[*it];
        if (node.state != 1) {
            validChain = node.state == 2 && validChain;
            continue;
        }
        if (!node.parent) {
            node.global = node.local;
            node.state = 2;
        } else {
            auto parent = std::lower_bound(
                scratch.indices_.begin(), scratch.indices_.end(), node.parent.value,
                [](const auto& entry, std::uint64_t id) { return entry.first < id; });
            if (parent == scratch.indices_.end() || parent->first != node.parent.value) {
                ++result.staleParents;
                node.global = node.local;
                node.state = 2;
            } else if (nodes[parent->second].state == 2) {
                node.global = nodes[parent->second].global * node.local;
                node.state = 2;
            } else {
                node.state = 3;
                validChain = false;
            }
        }
        validChain = node.state == 2 && validChain;
    }
    return validChain && nodes[index].state == 2;
}
} // namespace detail

// This operates on any ECS world that registers these three components. Other
// components in that world (including renderer-facing data) are untouched.
template <class World>
PropagationResult propagate_transforms(World& world, HierarchyScratch& scratch) {
    auto& nodes = scratch.nodes_;
    auto& indices = scratch.indices_;
    nodes.clear();
    indices.clear();
    world.template for_each<LocalTransform, GlobalTransform, ChildOf>(
        [&](Entity entity, LocalTransform& local, GlobalTransform&, ChildOf& relationship) {
            const std::size_t index = nodes.size();
            HierarchyScratch::Node node;
            node.entity = entity;
            node.parent = relationship.parent;
            node.local = local.matrix;
            nodes.push_back(node);
            indices.emplace_back(entity.value, index);
        });
    std::sort(indices.begin(), indices.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    PropagationResult result;
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].state != 3 && detail::resolve_hierarchy_node(scratch, i, result))
            ++result.transformed;

    // Reacquire because component pointers are borrowed and invalidated by
    // structural changes between the scan and this write pass.
    for (const auto& node : nodes) {
        auto* global = world.template get<GlobalTransform>(node.entity);
        if (!global)
            continue;
        const bool validTransform = node.state == 2;
        global->valid = validTransform;
        if (validTransform)
            global->matrix = node.global;
    }
    return result;
}

} // namespace engine::scene
