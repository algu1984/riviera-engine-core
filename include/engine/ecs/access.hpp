#pragma once

#include "engine/ecs/world.hpp"

#include <memory>
#include <type_traits>
#include <utility>

namespace engine::ecs {

struct Range {
    std::size_t first = 0;
    std::size_t last = 0;
};

template <class T> class Read {
  public:
    const T* get(Entity entity) const {
        return get_(world_, entity);
    }

    template <class Fn> void for_each(Fn&& fn) const {
        using Callback = std::remove_reference_t<Fn>;
        auto* callback = const_cast<void*>(static_cast<const void*>(std::addressof(fn)));
        each_(world_, callback, [](void* context, Entity entity, const T& value) {
            (*static_cast<Callback*>(context))(entity, value);
        });
    }
    template <class Fn> void for_each_range(Range range, Fn&& fn) const {
        using Callback = std::remove_reference_t<Fn>;
        auto* callback = const_cast<void*>(static_cast<const void*>(std::addressof(fn)));
        eachRange_(world_, range.first, range.last, callback,
                   [](void* context, Entity entity, const T& value) {
                       (*static_cast<Callback*>(context))(entity, value);
                   });
    }
    template <class Fn> void for_each_range(std::size_t first, std::size_t last, Fn&& fn) const {
        for_each_range(Range{first, last}, std::forward<Fn>(fn));
    }

  private:
    template <class U, class World> friend Read<U> read_access(const World&);
    Read(const void* world, const T* (*get)(const void*, Entity),
         void (*each)(const void*, void*, void (*)(void*, Entity, const T&)),
         void (*eachRange)(const void*, std::size_t, std::size_t, void*,
                           void (*)(void*, Entity, const T&)))
        : world_(world), get_(get), each_(each), eachRange_(eachRange) {}

    const void* world_;
    const T* (*get_)(const void*, Entity);
    void (*each_)(const void*, void*, void (*)(void*, Entity, const T&));
    void (*eachRange_)(const void*, std::size_t, std::size_t, void*,
                       void (*)(void*, Entity, const T&));
};

template <class T> class Write {
  public:
    T* get(Entity entity) const {
        return get_(world_, entity);
    }

    template <class Fn> void for_each(Fn&& fn) const {
        using Callback = std::remove_reference_t<Fn>;
        auto* callback = const_cast<void*>(static_cast<const void*>(std::addressof(fn)));
        each_(world_, callback, [](void* context, Entity entity, T& value) {
            (*static_cast<Callback*>(context))(entity, value);
        });
    }
    template <class Fn> void for_each_range(Range range, Fn&& fn) const {
        using Callback = std::remove_reference_t<Fn>;
        auto* callback = const_cast<void*>(static_cast<const void*>(std::addressof(fn)));
        eachRange_(world_, range.first, range.last, callback,
                   [](void* context, Entity entity, T& value) {
                       (*static_cast<Callback*>(context))(entity, value);
                   });
    }
    template <class Fn> void for_each_range(std::size_t first, std::size_t last, Fn&& fn) const {
        for_each_range(Range{first, last}, std::forward<Fn>(fn));
    }

  private:
    template <class U, class World> friend Write<U> write_access(World&);
    Write(void* world, T* (*get)(void*, Entity),
          void (*each)(void*, void*, void (*)(void*, Entity, T&)),
          void (*eachRange)(void*, std::size_t, std::size_t, void*, void (*)(void*, Entity, T&)))
        : world_(world), get_(get), each_(each), eachRange_(eachRange) {}

    void* world_;
    T* (*get_)(void*, Entity);
    void (*each_)(void*, void*, void (*)(void*, Entity, T&));
    void (*eachRange_)(void*, std::size_t, std::size_t, void*, void (*)(void*, Entity, T&));
};

template <class T, class World> Read<T> read_access(const World& world) {
    return Read<T>(
        &world,
        [](const void* context, Entity entity) {
            return static_cast<const World*>(context)->template get<T>(entity);
        },
        [](const void* context, void* callback, void (*invoke)(void*, Entity, const T&)) {
            static_cast<const World*>(context)->template for_each<T>(
                [callback, invoke](Entity entity, const T& value) {
                    invoke(callback, entity, value);
                });
        },
        [](const void* context, std::size_t first, std::size_t last, void* callback,
           void (*invoke)(void*, Entity, const T&)) {
            static_cast<const World*>(context)->template for_each_range<T>(
                first, last, [callback, invoke](Entity entity, const T& value) {
                    invoke(callback, entity, value);
                });
        });
}

template <class T, class World> Write<T> write_access(World& world) {
    return Write<T>(
        &world,
        [](void* context, Entity entity) {
            return static_cast<World*>(context)->template get<T>(entity);
        },
        [](void* context, void* callback, void (*invoke)(void*, Entity, T&)) {
            static_cast<World*>(context)->template for_each<T>(
                [callback, invoke](Entity entity, T& value) { invoke(callback, entity, value); });
        },
        [](void* context, std::size_t first, std::size_t last, void* callback,
           void (*invoke)(void*, Entity, T&)) {
            static_cast<World*>(context)->template for_each_range<T>(
                first, last,
                [callback, invoke](Entity entity, T& value) { invoke(callback, entity, value); });
        });
}

} // namespace engine::ecs
