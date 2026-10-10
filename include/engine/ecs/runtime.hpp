#pragma once

#include "engine/ecs/system_scheduler.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace engine::ecs {

enum class Schedule : std::size_t { Startup, FixedUpdate, Update, RenderExtract, Count };

template <class World> class Runtime {
  public:
    Runtime(World& world, jobs::JobScheduler& jobs) : world_(world), jobs_(jobs) {}

    template <class Fn> SystemId addSystem(Schedule schedule, std::string name, Fn&& fn) {
        return schedulers_[index(schedule)].add(std::move(name), std::forward<Fn>(fn));
    }
    bool before(Schedule schedule, SystemId first, SystemId second) {
        return schedulers_[index(schedule)].before(first, second);
    }
    bool after(Schedule schedule, SystemId system, SystemId prerequisite) {
        return schedulers_[index(schedule)].after(system, prerequisite);
    }

    template <class Plugin> void addPlugin(Plugin&& plugin) {
        std::forward<Plugin>(plugin).build(*this);
    }
    template <class Plugin, class... Args> void addPlugin(Args&&... args) {
        Plugin plugin(std::forward<Args>(args)...);
        plugin.build(*this);
    }

    SystemRunResult run(Schedule schedule) {
        const auto slot = index(schedule);
        if (schedule == Schedule::Startup && startupComplete_)
            return {true, {}, {}};
        auto result = schedulers_[slot].run(world_, jobs_);
        if (schedule == Schedule::Startup && result.success)
            startupComplete_ = true;
        return result;
    }

    SystemScheduler<World>& systems(Schedule schedule) {
        return schedulers_[index(schedule)];
    }
    const SystemScheduler<World>& systems(Schedule schedule) const {
        return schedulers_[index(schedule)];
    }
    World& world() noexcept {
        return world_;
    }
    jobs::JobScheduler& jobs() noexcept {
        return jobs_;
    }

  private:
    static constexpr std::size_t index(Schedule schedule) {
        return static_cast<std::size_t>(schedule);
    }
    World& world_;
    jobs::JobScheduler& jobs_;
    std::array<SystemScheduler<World>, static_cast<std::size_t>(Schedule::Count)> schedulers_;
    bool startupComplete_ = false;
};

} // namespace engine::ecs
