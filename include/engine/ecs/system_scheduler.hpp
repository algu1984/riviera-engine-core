#pragma once

#include "engine/ecs/access.hpp"
#include "engine/ecs/commands.hpp"
#include "engine/jobs/job_scheduler.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

namespace engine::ecs {

using SystemId = std::size_t;

namespace detail {
template <class T> struct function_traits : function_traits<decltype(&T::operator())> {};
template <class C, class R, class... A> struct function_traits<R (C::*)(A...) const> {
    using args = std::tuple<A...>;
};
template <class C, class R, class... A> struct function_traits<R (C::*)(A...)> {
    using args = std::tuple<A...>;
};
template <class C, class R, class... A> struct function_traits<R (C::*)(A...) const noexcept> {
    using args = std::tuple<A...>;
};
template <class C, class R, class... A> struct function_traits<R (C::*)(A...) noexcept> {
    using args = std::tuple<A...>;
};
template <class R, class... A> struct function_traits<R (*)(A...)> {
    using args = std::tuple<A...>;
};
template <class R, class... A> struct function_traits<R (*)(A...) noexcept> {
    using args = std::tuple<A...>;
};
template <class R, class... A> struct function_traits<R(A...)> {
    using args = std::tuple<A...>;
};

template <class T> struct is_read : std::false_type {};
template <class T> struct is_read<Read<T>> : std::true_type {
    using component = T;
};
template <class T> struct is_write : std::false_type {};
template <class T> struct is_write<Write<T>> : std::true_type {
    using component = T;
};
template <class T> using bare_param = std::remove_cv_t<std::remove_reference_t<T>>;
template <class T> struct is_range : std::is_same<bare_param<T>, Range> {};

struct Access {
    std::type_index type{typeid(void)};
    bool write = false;
};

template <class World, class Fn, class Tuple, std::size_t... I>
void invoke_system(Fn& fn, World& world, CommandBuffer<World>& commands,
                   CommandBuffer<World>& deferred, Range range, std::index_sequence<I...>) {
    fn(make_param<std::tuple_element_t<I, Tuple>>(world, commands, deferred, range)...);
}

template <class Param, class World>
auto make_param(World& world, CommandBuffer<World>& commands, CommandBuffer<World>& deferred,
                Range range) {
    using P = bare_param<Param>;
    if constexpr (is_range<P>::value) {
        return range;
    } else if constexpr (is_read<P>::value) {
        return read_access<typename is_read<P>::component>(world);
    } else if constexpr (is_write<P>::value) {
        return write_access<typename is_write<P>::component>(world);
    } else if constexpr (std::is_same_v<P, Commands>) {
        return commands.commands();
    } else if constexpr (std::is_same_v<P, DeferredCommands>) {
        return deferred.deferredCommands();
    } else {
        static_assert(!sizeof(P),
                      "System parameters must be Read<T>, Write<T>, Commands, or DeferredCommands");
    }
}

template <class Tuple, std::size_t... I>
std::vector<Access> access_list(std::index_sequence<I...>) {
    std::vector<Access> result;
    (append_access<std::tuple_element_t<I, Tuple>>(result), ...);
    return result;
}
template <class Tuple, std::size_t... I>
constexpr bool has_normal_commands(std::index_sequence<I...>) {
    return (std::is_same_v<bare_param<std::tuple_element_t<I, Tuple>>, Commands> || ... || false);
}
template <class Tuple, std::size_t... I>
constexpr bool has_command_params(std::index_sequence<I...>) {
    return ((std::is_same_v<bare_param<std::tuple_element_t<I, Tuple>>, Commands> ||
             std::is_same_v<bare_param<std::tuple_element_t<I, Tuple>>, DeferredCommands>) ||
            ... || false);
}
template <class Tuple> struct first_is_range : std::false_type {};
template <class First, class... Rest>
struct first_is_range<std::tuple<First, Rest...>> : is_range<First> {};
template <class Param> void append_access(std::vector<Access>& result) {
    using P = bare_param<Param>;
    if constexpr (is_read<P>::value)
        result.push_back({typeid(typename is_read<P>::component), false});
    else if constexpr (is_write<P>::value)
        result.push_back({typeid(typename is_write<P>::component), true});
    else if constexpr (is_range<P>::value)
        return;
    else
        static_assert(std::is_same_v<P, Commands> || std::is_same_v<P, DeferredCommands>,
                      "System parameters must be Read<T>, Write<T>, Commands, or DeferredCommands");
}

} // namespace detail

struct SystemRunResult {
    bool success = false;
    std::string error;
    std::vector<std::string> warnings;
};

template <class World> class SystemScheduler {
    using Buffer = detail::CommandBuffer<World>;
    struct System {
        std::string name;
        std::vector<detail::Access> access;
        Buffer normal;
        Buffer deferred;
        bool hasNormal = false;
        bool parallel = false;
        std::size_t partitions = 1;
        std::size_t rangeTotal = 1;
        std::function<std::size_t(World&)> countDomain;
        std::function<void(World&, Range)> run;
    };

  public:
    SystemScheduler() = default;
    SystemScheduler(const SystemScheduler&) = delete;
    SystemScheduler& operator=(const SystemScheduler&) = delete;
    SystemScheduler(SystemScheduler&&) = delete;
    SystemScheduler& operator=(SystemScheduler&&) = delete;

    template <class Fn> SystemId add(std::string name, Fn&& callable) {
        if (running_)
            return std::numeric_limits<SystemId>::max();
        using Function = std::decay_t<Fn>;
        using Args = typename detail::function_traits<Function>::args;
        auto system = std::make_unique<System>();
        system->name = std::move(name);
        system->access =
            detail::access_list<Args>(std::make_index_sequence<std::tuple_size_v<Args>>{});
        system->hasNormal =
            detail::has_normal_commands<Args>(std::make_index_sequence<std::tuple_size_v<Args>>{});
        auto function = std::make_shared<Function>(std::forward<Fn>(callable));
        auto* entry = system.get();
        system->run = [entry, function](World& world, Range range) {
            detail::invoke_system<World, Function, Args>(
                *function, world, entry->normal, entry->deferred, range,
                std::make_index_sequence<std::tuple_size_v<Args>>{});
        };
        const auto id = systems_.size();
        systems_.push_back(std::move(system));
        invalidate();
        return id;
    }

    template <class Domain, class Fn>
    SystemId add_parallel(std::string name, std::size_t partitions, Fn&& callable) {
        if (running_)
            return std::numeric_limits<SystemId>::max();
        if (partitions == 0) {
            registrationError_ = "Parallel system partition count must be greater than zero";
            invalidate();
            return std::numeric_limits<SystemId>::max();
        }
        using Function = std::decay_t<Fn>;
        using Args = typename detail::function_traits<Function>::args;
        static_assert(detail::first_is_range<Args>::value,
                      "Parallel system callable must take ecs::Range as its first parameter");
        static_assert(World::template registered<Domain>,
                      "parallel domain must be registered in World");
        auto system = std::make_unique<System>();
        system->name = std::move(name);
        system->access =
            detail::access_list<Args>(std::make_index_sequence<std::tuple_size_v<Args>>{});
        system->hasNormal =
            detail::has_normal_commands<Args>(std::make_index_sequence<std::tuple_size_v<Args>>{});
        system->parallel = true;
        system->partitions = partitions;
        if (detail::has_command_params<Args>(std::make_index_sequence<std::tuple_size_v<Args>>{}))
            registrationError_ = "Parallel systems cannot declare Commands or DeferredCommands";
        if (std::none_of(
                system->access.begin(), system->access.end(),
                [](const detail::Access& access) { return access.type == typeid(Domain); }))
            registrationError_ = "Parallel system domain must appear as a Read or Write parameter";
        system->countDomain = [](World& world) { return world.template count<Domain>(); };
        auto function = std::make_shared<Function>(std::forward<Fn>(callable));
        auto* entry = system.get();
        system->run = [entry, function](World& world, Range range) {
            detail::invoke_system<World, Function, Args>(
                *function, world, entry->normal, entry->deferred, range,
                std::make_index_sequence<std::tuple_size_v<Args>>{});
        };
        const auto id = systems_.size();
        systems_.push_back(std::move(system));
        invalidate();
        return id;
    }

    bool before(SystemId first, SystemId second) {
        if (running_ || first >= systems_.size() || second >= systems_.size() || first == second)
            return false;
        explicitEdges_.emplace_back(first, second);
        invalidate();
        return true;
    }
    bool after(SystemId system, SystemId prerequisite) {
        return before(prerequisite, system);
    }
    void clear() {
        if (running_)
            return;
        systems_.clear();
        explicitEdges_.clear();
        registrationError_.clear();
        invalidate();
    }

    SystemRunResult run(World& world, jobs::JobScheduler& jobs) {
        SystemRunResult result;
        if (running_ || jobs::JobScheduler::inTask()) {
            result.error = "SystemScheduler::run cannot be nested inside a scheduler task";
            return result;
        }
        running_ = true;
        jobs::JobScheduler::Ticket ticket;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
        try {
#endif
            if (!prepared_) {
                result = prepare();
                if (!result.success) {
                    running_ = false;
                    return result;
                }
            }
            result.warnings = warnings_;
            if (!world.begin_execution()) {
                result.success = false;
                result.error = "World is already executing systems";
                running_ = false;
                return result;
            }
            activeWorld_ = &world;
            for (auto& system : systems_) {
                system->normal.beginRun();
                system->deferred.beginRun();
            }
            ticket = jobs.submit(*prepared_);
            while (!jobs.complete(ticket)) {
                (void)jobs.helpOne();
            }
            auto status = jobs.poll(ticket);
            world.end_execution();
            const bool normalFlushed = flushNormal(world);
            const bool deferredFlushed = flushDeferred(world);
            const bool flushed = normalFlushed && deferredFlushed;
            activeWorld_ = nullptr;
            result.success = status.success && flushed;
            if (!status.success)
                result.error = status.error;
            else if (!flushed)
                result.error = "One or more deferred structural commands failed";
            running_ = false;
            return result;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
        } catch (...) {
            while (ticket && !jobs.complete(ticket))
                (void)jobs.helpOne();
            if (activeWorld_) {
                activeWorld_->end_execution();
                (void)flushNormal(*activeWorld_);
                (void)flushDeferred(*activeWorld_);
                activeWorld_ = nullptr;
            }
            running_ = false;
            result.success = false;
            result.error = "System scheduler failed while preparing or applying work";
            return result;
        }
#endif
    }

    std::size_t system_count() const noexcept {
        return systems_.size();
    }
    bool schedule_cached() const noexcept {
        return prepared_.has_value();
    }

  private:
    struct PlanResult {
        jobs::TaskGraph graph;
        std::vector<std::string> warnings;
        std::string error;
    };
    void invalidate() {
        prepared_.reset();
        warnings_.clear();
    }

    SystemRunResult prepare() {
        SystemRunResult result;
        PlanResult plan = buildPlan();
        if (!plan.error.empty()) {
            result.error = std::move(plan.error);
            return result;
        }
        auto prepared = plan.graph.prepare();
        if (!prepared.valid()) {
            result.error = prepared.error();
            return result;
        }
        warnings_ = std::move(plan.warnings);
        prepared_ = std::move(prepared);
        result.success = true;
        return result;
    }

    PlanResult buildPlan() {
        PlanResult result;
        if (!registrationError_.empty()) {
            result.error = registrationError_;
            return result;
        }
        const auto count = systems_.size();
        std::vector<std::vector<SystemId>> edges(count);
        for (const auto [from, to] : explicitEdges_) {
            if (from >= count || to >= count) {
                result.error = "System dependency references an invalid system";
                return result;
            }
            edges[from].push_back(to);
        }
        auto explicitOrder = topological(edges, result.error);
        if (!result.error.empty())
            return result;
        std::vector<SystemId> rank(count);
        for (std::size_t i = 0; i < explicitOrder.size(); ++i)
            rank[explicitOrder[i]] = i;
        std::vector<std::pair<SystemId, SystemId>> conflicts;
        for (SystemId a = 0; a < count; ++a)
            for (SystemId b = a + 1; b < count; ++b)
                if (conflictsWith(*systems_[a], *systems_[b]))
                    conflicts.emplace_back(a, b);
        for (const auto [a, b] : conflicts) {
            if (reachable(edges, a, b) || reachable(edges, b, a))
                continue;
            const auto first = rank[a] <= rank[b] ? a : b;
            const auto second = first == a ? b : a;
            edges[first].push_back(second);
            result.warnings.push_back("Conflicting systems '" + systems_[first]->name + "' and '" +
                                      systems_[second]->name +
                                      "' were ordered by stable schedule order");
        }
        auto order = topological(edges, result.error);
        if (!result.error.empty())
            return result;
        std::vector<std::size_t> depth(count, 0);
        std::vector<std::vector<SystemId>> incoming(count);
        for (SystemId from = 0; from < count; ++from)
            for (const auto to : edges[from])
                incoming[to].push_back(from);
        for (const auto id : order)
            for (const auto before : incoming[id])
                depth[id] = std::max(depth[id], depth[before] + 1);
        const auto maxDepth = depth.empty() ? 0 : *std::max_element(depth.begin(), depth.end());
        std::vector<std::vector<SystemId>> waves(maxDepth + (count ? 1 : 0));
        for (SystemId id = 0; id < count; ++id)
            waves[depth[id]].push_back(id);
        for (const auto& system : systems_) {
            for (std::size_t a = 0; a < system->access.size(); ++a)
                for (std::size_t b = a + 1; b < system->access.size(); ++b)
                    if (system->access[a].type == system->access[b].type &&
                        (system->access[a].write || system->access[b].write)) {
                        result.error = "System '" + system->name +
                                       "' declares duplicate incompatible access to one component";
                        return result;
                    }
        }
        std::vector<jobs::TaskId> taskForSystem(count);
        std::vector<std::optional<jobs::TaskId>> barrierForWave(waves.size());
        std::vector<std::vector<jobs::TaskId>> priorTasks(waves.size());
        for (std::size_t wave = 0; wave < waves.size(); ++wave) {
            for (const auto id : waves[wave]) {
                std::vector<jobs::TaskId> prereqs;
                for (const auto before : incoming[id])
                    prereqs.push_back(taskForSystem[before]);
                if (wave > 0 && barrierForWave[wave - 1])
                    prereqs.push_back(*barrierForWave[wave - 1]);
                if (systems_[id]->parallel) {
                    const auto countTask = result.graph.add(
                        systems_[id]->name + " count domain",
                        [this, id]() {
                            auto& system = *systems_[id];
                            system.rangeTotal = system.countDomain(*activeWorld_);
                            return true;
                        },
                        prereqs);
                    std::vector<jobs::TaskId> ranges;
                    for (std::size_t part = 0; part < systems_[id]->partitions; ++part) {
                        auto rangePrerequisites = std::vector<jobs::TaskId>{countTask};
                        ranges.push_back(result.graph.add(
                            systems_[id]->name + " range " + std::to_string(part),
                            [this, id, part]() {
                                auto& system = *systems_[id];
                                const auto base = system.rangeTotal / system.partitions;
                                const auto extra = system.rangeTotal % system.partitions;
                                const auto first = part * base + std::min(part, extra);
                                const auto last = first + base + (part < extra ? 1 : 0);
                                system.run(*activeWorld_, Range{first, last});
                                return true;
                            },
                            std::move(rangePrerequisites)));
                    }
                    taskForSystem[id] = result.graph.add(
                        systems_[id]->name + " complete", [] { return true; }, std::move(ranges));
                } else {
                    taskForSystem[id] = result.graph.add(
                        systems_[id]->name,
                        [this, id]() {
                            systems_[id]->run(*activeWorld_, Range{0, 1});
                            return true;
                        },
                        std::move(prereqs));
                }
                priorTasks[wave].push_back(taskForSystem[id]);
            }
            const bool hasNormal =
                std::any_of(waves[wave].begin(), waves[wave].end(),
                            [this](SystemId id) { return systems_[id]->hasNormal; });
            {
                auto prerequisites = priorTasks[wave];
                const auto barrier = result.graph.add(
                    hasNormal ? "Apply Commands" : "System Phase Barrier",
                    [this, wave, hasNormal]() {
                        activeWorld_->end_execution();
                        const bool success = !hasNormal || flushNormal(*activeWorld_);
                        const bool entered = activeWorld_->begin_execution();
                        completedWave_ = wave;
                        return success && entered;
                    },
                    std::move(prerequisites));
                barrierForWave[wave] = barrier;
                priorTasks[wave].push_back(barrier);
            }
        }
        std::vector<jobs::TaskId> finalPrerequisites;
        for (SystemId id = 0; id < count; ++id)
            finalPrerequisites.push_back(taskForSystem[id]);
        for (const auto barrier : barrierForWave)
            if (barrier)
                finalPrerequisites.push_back(*barrier);
        // A terminal node makes final queue application part of normal completion;
        // run() repeats it as recovery if an upstream task failed and canceled it.
        result.graph.add(
            "Apply Deferred Commands",
            [this]() {
                activeWorld_->end_execution();
                const bool normal = flushNormal(*activeWorld_);
                const bool deferred = flushDeferred(*activeWorld_);
                return normal && deferred;
            },
            std::move(finalPrerequisites));
        return result;
    }

    static bool conflictsWith(const System& first, const System& second) {
        for (const auto& a : first.access)
            for (const auto& b : second.access)
                if (a.type == b.type && (a.write || b.write))
                    return true;
        return false;
    }
    static std::vector<SystemId> topological(const std::vector<std::vector<SystemId>>& edges,
                                             std::string& error) {
        std::vector<std::size_t> incoming(edges.size(), 0);
        for (const auto& successors : edges)
            for (const auto to : successors)
                ++incoming[to];
        std::vector<SystemId> ready;
        for (SystemId i = 0; i < edges.size(); ++i)
            if (!incoming[i])
                ready.push_back(i);
        std::vector<SystemId> order;
        for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) {
            const auto id = ready[cursor];
            order.push_back(id);
            for (const auto next : edges[id])
                if (--incoming[next] == 0)
                    ready.push_back(next);
        }
        if (order.size() != edges.size())
            error = "System dependency cycle";
        return order;
    }
    static bool reachable(const std::vector<std::vector<SystemId>>& edges, SystemId from,
                          SystemId to) {
        std::vector<bool> seen(edges.size(), false);
        std::vector<SystemId> stack{from};
        while (!stack.empty()) {
            const auto current = stack.back();
            stack.pop_back();
            if (current == to)
                return true;
            if (seen[current])
                continue;
            seen[current] = true;
            for (const auto next : edges[current])
                stack.push_back(next);
        }
        return false;
    }
    bool flushNormal(World& world) {
        bool success = true;
        for (auto& system : systems_)
            success = system->normal.apply(world) && success;
        return success;
    }
    bool flushDeferred(World& world) {
        bool success = true;
        for (auto& system : systems_)
            success = system->deferred.apply(world) && success;
        return success;
    }

    std::vector<std::unique_ptr<System>> systems_;
    std::vector<std::pair<SystemId, SystemId>> explicitEdges_;
    std::optional<jobs::PreparedTaskGraph> prepared_;
    std::vector<std::string> warnings_;
    std::string registrationError_;
    World* activeWorld_ = nullptr;
    std::size_t completedWave_ = 0;
    bool running_ = false;
};

} // namespace engine::ecs
