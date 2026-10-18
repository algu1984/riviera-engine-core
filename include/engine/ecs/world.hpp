#pragma once

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace engine::ecs {

struct Entity {
    std::uint64_t value = 0;
    constexpr explicit operator bool() const noexcept {
        return value != 0;
    }
    constexpr std::uint32_t index() const noexcept {
        return static_cast<std::uint32_t>(value);
    }
    constexpr std::uint32_t generation() const noexcept {
        return static_cast<std::uint32_t>(value >> 32);
    }
    friend constexpr bool operator==(Entity, Entity) = default;
    static constexpr Entity null() noexcept {
        return {};
    }
};

namespace detail {
template <class...> inline constexpr bool unique_types_v = true;
template <class T, class... Rest>
inline constexpr bool unique_types_v<T, Rest...> =
    ((!std::is_same_v<T, Rest>) && ...) && unique_types_v<Rest...>;

template <class T, class... Ts> consteval std::size_t type_index() {
    static_assert((std::is_same_v<T, Ts> || ...), "component type is not registered");
    constexpr bool matches[] = {std::is_same_v<T, Ts>...};
    for (std::size_t i = 0; i < sizeof...(Ts); ++i)
        if (matches[i])
            return i;
    return sizeof...(Ts);
}

template <class T> struct Column {
    std::vector<T> values;
    void erase_swap(std::size_t row) noexcept {
        if (row + 1 != values.size())
            values[row] = std::move(values.back());
        values.pop_back();
    }
};
} // namespace detail

// Typed, compile-time component universe. Structural changes are exclusive;
// component pointers are borrowed and invalidated by any structural mutation.
template <class... Components> class World {
    static_assert(sizeof...(Components) > 0, "register at least one component");
    static_assert(detail::unique_types_v<Components...>, "component types must be unique");
    static_assert((std::is_nothrow_destructible_v<Components> && ...),
                  "component destructors must be nothrow");
    static_assert((std::is_nothrow_move_constructible_v<Components> && ...),
                  "components must be nothrow move constructible");
    static_assert((std::is_nothrow_move_assignable_v<Components> && ...),
                  "components must be nothrow move assignable");

    using Columns = std::tuple<std::unique_ptr<detail::Column<Components>>...>;
    struct Archetype {
        explicit Archetype(std::bitset<sizeof...(Components)> typeMask) : mask(typeMask) {
            make_columns(std::index_sequence_for<Components...>{});
        }
        template <std::size_t... I> void make_columns(std::index_sequence<I...>) {
            ((mask.test(I)
                  ? (void)(std::get<I>(columns) = std::make_unique<detail::Column<Components>>())
                  : (void)0),
             ...);
        }
        std::bitset<sizeof...(Components)> mask;
        std::vector<Entity> entities;
        Columns columns;
    };
    struct Location {
        Archetype* table = nullptr;
        std::size_t row = 0;
        std::uint32_t generation = 1;
        bool alive = false;
    };

  public:
    using component_types = std::tuple<Components...>;
    template <class T> static constexpr bool registered = (std::is_same_v<T, Components> || ...);
    World() {
        (void)table(std::bitset<sizeof...(Components)>{});
    }
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    Entity create() {
        if (executing_)
            return {};
        auto& empty = table(std::bitset<sizeof...(Components)>{});
        reserve_for_one(empty.entities);
        const bool reuse = !free_.empty();
        std::uint32_t index = reuse ? free_.back() : 0;
        if (!reuse) {
            if (locations_.size() >= UINT32_MAX)
                return {};
            index = static_cast<std::uint32_t>(locations_.size());
            locations_.emplace_back();
        }
        auto& location = locations_[index];
        const Entity entity{(std::uint64_t(location.generation) << 32) | index};
        empty.entities.push_back(entity);
        if (reuse)
            free_.pop_back();
        location.table = &empty;
        location.row = empty.entities.size() - 1;
        location.alive = true;
        return entity;
    }
    Entity spawn() {
        return create();
    }

    bool despawn(Entity entity) {
        if (executing_)
            return false;
        auto* location = find(entity);
        if (!location)
            return false;
        reserve_for_one(free_);
        remove_row(*location->table, location->row, std::index_sequence_for<Components...>{});
        retire(entity);
        return true;
    }

    template <class T, class... A> T* emplace(Entity entity, A&&... args) {
        static_assert(registered<T>, "component type not registered");
        if (executing_)
            return nullptr;
        auto* location = find(entity);
        if (!location)
            return nullptr;
        constexpr auto index = detail::type_index<T, Components...>();
        if (location->table->mask.test(index)) {
            T replacement(std::forward<A>(args)...);
            auto* current = get<T>(entity);
            std::destroy_at(current);
            return std::construct_at(current, std::move(replacement));
        }
        T value(std::forward<A>(args)...);
        auto signature = location->table->mask;
        signature.set(index);
        auto& destination = table(signature);
        migrate<T>(entity, *location, destination, &value,
                   std::index_sequence_for<Components...>{});
        return get<T>(entity);
    }

    template <class T> T* get(Entity entity) noexcept {
        static_assert(registered<T>, "component type not registered");
        auto* location = find(entity);
        if (!location)
            return nullptr;
        constexpr auto i = detail::type_index<T, Components...>();
        if (!location->table->mask.test(i))
            return nullptr;
        return std::get<i>(location->table->columns)->values.data() + location->row;
    }
    template <class T> const T* get(Entity entity) const noexcept {
        return const_cast<World*>(this)->template get<T>(entity);
    }

    template <class T> bool remove(Entity entity) {
        static_assert(registered<T>, "component type not registered");
        if (executing_)
            return false;
        auto* location = find(entity);
        if (!location)
            return false;
        constexpr auto index = detail::type_index<T, Components...>();
        if (!location->table->mask.test(index))
            return false;
        auto signature = location->table->mask;
        signature.reset(index);
        auto& destination = table(signature);
        migrate<T>(entity, *location, destination, nullptr,
                   std::index_sequence_for<Components...>{});
        return true;
    }

    template <class... T> std::size_t count() const {
        static_assert(sizeof...(T) > 0 && (registered<T> && ...), "query registered components");
        const auto required = query_mask<T...>();
        std::size_t result = 0;
        for (const auto& owned : tables_)
            if ((owned->mask & required) == required)
                result += owned->entities.size();
        return result;
    }

    template <class... T, class Fn>
    void for_each_range(std::size_t first, std::size_t last, Fn&& fn) {
        static_assert(sizeof...(T) > 0 && (registered<T> && ...), "query registered components");
        if (first >= last)
            return;
        const auto required = query_mask<T...>();
        std::size_t ordinal = 0;
        for (auto& owned : tables_) {
            auto& t = *owned;
            if ((t.mask & required) != required)
                continue;
            const auto tableEnd = ordinal + t.entities.size();
            if (first < tableEnd && last > ordinal) {
                const auto rowBegin = first > ordinal ? first - ordinal : 0;
                const auto rowEnd = std::min(last, tableEnd) - ordinal;
                for (auto row = rowBegin; row < rowEnd; ++row)
                    std::invoke(fn, t.entities[row],
                                std::get<detail::type_index<T, Components...>()>(t.columns)
                                    ->values[row]...);
            }
            ordinal = tableEnd;
            if (ordinal >= last)
                break;
        }
    }
    template <class... T, class Fn>
    void for_each_range(std::size_t first, std::size_t last, Fn&& fn) const {
        const_cast<World*>(this)->template for_each_range<T...>(
            first, last,
            [&](Entity e, T&... values) { std::invoke(fn, e, static_cast<const T&>(values)...); });
    }
    template <class... T, class Fn> void for_each(Fn&& fn) {
        for_each_range<T...>(0, std::numeric_limits<std::size_t>::max(), std::forward<Fn>(fn));
    }
    template <class... T, class Fn> void for_each(Fn&& fn) const {
        for_each_range<T...>(0, std::numeric_limits<std::size_t>::max(), std::forward<Fn>(fn));
    }

    // The scheduler brackets system execution with these methods. Queries are
    // allowed during the phase; direct structural changes fail immediately.
    bool begin_execution() {
        if (executing_)
            return false;
        executing_ = true;
        return true;
    }
    void end_execution() {
        executing_ = false;
    }

  private:
    template <class... T> static std::bitset<sizeof...(Components)> query_mask() {
        std::bitset<sizeof...(Components)> result;
        (result.set(detail::type_index<T, Components...>()), ...);
        return result;
    }

    Archetype& table(std::bitset<sizeof...(Components)> mask) {
        for (auto& t : tables_)
            if (t->mask == mask)
                return *t;
        tables_.push_back(std::make_unique<Archetype>(mask));
        return *tables_.back();
    }
    template <class T> static void reserve_for_one(std::vector<T>& values) {
        if (values.size() < values.capacity())
            return;
        const auto maximum = values.max_size();
        const auto required = values.size() + 1;
        const auto doubled = values.capacity() == 0            ? std::size_t{1}
                             : values.capacity() > maximum / 2 ? maximum
                                                               : values.capacity() * 2;
        values.reserve(std::max(required, doubled));
    }
    Location* find(Entity entity) noexcept {
        if (!entity || entity.index() >= locations_.size())
            return nullptr;
        auto& location = locations_[entity.index()];
        return location.alive && location.generation == entity.generation() ? &location : nullptr;
    }
    void retire(Entity entity) noexcept {
        auto& location = locations_[entity.index()];
        location.alive = false;
        location.table = nullptr;
        if (++location.generation != 0)
            free_.push_back(entity.index());
    }
    template <class T, std::size_t... I>
    void migrate(Entity entity, Location& location, Archetype& destination, T* inserted,
                 std::index_sequence<I...>) {
        auto& source = *location.table;
        const auto oldRow = location.row;
        const auto newRow = destination.entities.size();
        reserve_for_one(destination.entities);
        (
            [&] {
                if (destination.mask.test(I))
                    reserve_for_one(std::get<I>(destination.columns)->values);
            }(),
            ...);
        destination.entities.push_back(entity);
        (
            [&] {
                if (!destination.mask.test(I))
                    return;
                auto& out = std::get<I>(destination.columns)->values;
                if constexpr (std::is_same_v<T, Components>) {
                    if (inserted) {
                        out.emplace_back(std::move(*inserted));
                        return;
                    }
                }
                auto& in = std::get<I>(source.columns)->values;
                out.emplace_back(std::move(in[oldRow]));
            }(),
            ...);
        remove_row(source, oldRow);
        location.table = &destination;
        location.row = newRow;
    }
    template <std::size_t... I>
    void remove_row(Archetype& source, std::size_t row, std::index_sequence<I...>) noexcept {
        const auto last = source.entities.size() - 1;
        const Entity moved = source.entities[last];
        (
            [&] {
                if (source.mask.test(I))
                    std::get<I>(source.columns)->erase_swap(row);
            }(),
            ...);
        if (row != last) {
            source.entities[row] = moved;
            auto& location = locations_[moved.index()];
            location.table = &source;
            location.row = row;
        }
        source.entities.pop_back();
    }
    void remove_row(Archetype& source, std::size_t row) noexcept {
        remove_row(source, row, std::index_sequence_for<Components...>{});
    }
    std::vector<Location> locations_;
    std::vector<std::uint32_t> free_;
    std::vector<std::unique_ptr<Archetype>> tables_;
    bool executing_ = false;
};
} // namespace engine::ecs
