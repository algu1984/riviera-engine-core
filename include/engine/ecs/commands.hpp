#pragma once

#include "engine/ecs/world.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <tuple>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

namespace engine::ecs {

namespace detail {
struct CommandTarget {
    Entity entity{};
    const void* tokenOwner = nullptr;
    std::size_t tokenIndex = 0;
    std::uint64_t tokenEpoch = 0;
    Entity (*resolve)(const void*, std::size_t, std::uint64_t) = nullptr;
};
template <class T> struct CommandPayload {
    static void moveConstruct(void* destination, void* source) {
        ::new (destination) T(std::move(*static_cast<T*>(source)));
    }
    static void destroy(void* value) {
        static_cast<T*>(value)->~T();
    }
};
template <class World> class CommandBuffer;
} // namespace detail

class EntityToken {
  public:
    Entity entity() const {
        return resolve_ ? resolve_(owner_, index_, epoch_) : Entity{};
    }
    bool ready() const {
        return static_cast<bool>(entity());
    }
    explicit operator bool() const {
        return ready();
    }

  private:
    template <class> friend class detail::CommandBuffer;
    friend class Commands;
    using Resolve = Entity (*)(const void*, std::size_t, std::uint64_t);
    EntityToken(const void* owner, std::size_t index, std::uint64_t epoch, Resolve resolve)
        : owner_(owner), index_(index), epoch_(epoch), resolve_(resolve) {}
    const void* owner_ = nullptr;
    std::size_t index_ = 0;
    std::uint64_t epoch_ = 0;
    Resolve resolve_ = nullptr;
};

class Commands {
  public:
    Commands() = default;
    template <class... T> EntityToken spawn() {
        const std::array<std::type_index, sizeof...(T)> types{std::type_index(typeid(T))...};
        return spawn_(context_, types.data(), types.size());
    }
    void despawn(Entity entity) {
        append_(context_, Kind::despawn, {entity}, typeid(void), nullptr, nullptr, nullptr, 0, 0);
    }
    void despawn(EntityToken token) {
        append_(context_, Kind::despawn, target(token), typeid(void), nullptr, nullptr, nullptr, 0,
                0);
    }
    template <class T> void add(Entity entity) {
        append_(context_, Kind::addDefault, {entity}, typeid(T), nullptr, nullptr, nullptr, 0, 0);
    }
    template <class T> void add(EntityToken token) {
        append_(context_, Kind::addDefault, target(token), typeid(T), nullptr, nullptr, nullptr, 0,
                0);
    }
    template <class T> void add(Entity entity, T value) {
        appendValue<T>({entity}, std::move(value));
    }
    template <class T> void add(EntityToken token, T value) {
        appendValue<T>(target(token), std::move(value));
    }
    template <class T> void remove(Entity entity) {
        append_(context_, Kind::remove, {entity}, typeid(T), nullptr, nullptr, nullptr, 0, 0);
    }
    template <class T> void remove(EntityToken token) {
        append_(context_, Kind::remove, target(token), typeid(T), nullptr, nullptr, nullptr, 0, 0);
    }

  protected:
    enum class Kind { spawn, despawn, add, addDefault, remove };
    using Spawn = EntityToken (*)(void*, const std::type_index*, std::size_t);
    using Append = void (*)(void*, Kind, detail::CommandTarget, std::type_index, void*,
                            void (*)(void*, void*), void (*)(void*), std::size_t, std::size_t);
    Commands(void* context, Spawn spawn, Append append)
        : context_(context), spawn_(spawn), append_(append) {}

  private:
    template <class> friend class detail::CommandBuffer;
    static detail::CommandTarget target(EntityToken token) {
        return {{}, token.owner_, token.index_, token.epoch_, token.resolve_};
    }
    template <class T> void appendValue(detail::CommandTarget to, T value) {
        append_(context_, Kind::add, to, typeid(T), &value,
                &detail::CommandPayload<T>::moveConstruct, &detail::CommandPayload<T>::destroy,
                sizeof(T), alignof(T));
    }
    void* context_ = nullptr;
    Spawn spawn_ = nullptr;
    Append append_ = nullptr;
};

class DeferredCommands : public Commands {
  public:
    DeferredCommands() = default;

  private:
    template <class> friend class detail::CommandBuffer;
    DeferredCommands(void* context, Spawn spawn, Append append)
        : Commands(context, spawn, append) {}
};

namespace detail {
template <class World> class CommandBuffer {
    using Tuple = typename World::component_types;
    struct Operation {
        Commands::Kind kind;
        CommandTarget target;
        std::size_t component = 0;
        void* payload = nullptr;
        void (*destroy)(void*) = nullptr;
        bool constructed = false;
    };

  public:
    ~CommandBuffer() {
        clear();
        for (const auto& block : blocks_)
            ::operator delete(block.data, std::align_val_t(block.alignment));
    }
    void beginRun() {
        ++epoch_;
        if (epoch_ == 0)
            ++epoch_;
        entities_.clear();
    }
    Commands commands() {
        return Commands(this, &spawnThunk, &appendThunk);
    }
    DeferredCommands deferredCommands() {
        return DeferredCommands(this, &spawnThunk, &appendThunk);
    }

    bool apply(World& world) {
        bool success = true;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
        try {
#endif
            for (const auto& op : operations_) {
                if (op.kind == Commands::Kind::spawn) {
                    entities_[op.target.tokenIndex] = world.create();
                    success = static_cast<bool>(entities_[op.target.tokenIndex]) && success;
                    continue;
                }
                Entity entity = op.target.entity;
                if (op.target.tokenOwner) {
                    entity = op.target.resolve
                                 ? op.target.resolve(op.target.tokenOwner, op.target.tokenIndex,
                                                     op.target.tokenEpoch)
                                 : Entity{};
                    if (!entity) {
                        success = false;
                        continue;
                    }
                }
                switch (op.kind) {
                case Commands::Kind::spawn:
                    break;
                case Commands::Kind::despawn:
                    success = world.despawn(entity) && success;
                    break;
                case Commands::Kind::add:
                    success =
                        op.constructed && addAt(world, op.component, entity, op.payload) && success;
                    break;
                case Commands::Kind::addDefault:
                    success = addAt(world, op.component, entity, nullptr) && success;
                    break;
                case Commands::Kind::remove:
                    success = removeAt(world, op.component, entity) && success;
                    break;
                }
            }
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
        } catch (...) {
            success = false;
        }
#endif
        clear();
        return success;
    }

  private:
    static Entity resolveThunk(const void* context, std::size_t index, std::uint64_t epoch) {
        const auto& buffer = *static_cast<const CommandBuffer*>(context);
        return epoch == buffer.epoch_ && index < buffer.entities_.size() ? buffer.entities_[index]
                                                                         : Entity{};
    }
    static EntityToken spawnThunk(void* context, const std::type_index* types, std::size_t count) {
        auto& buffer = *static_cast<CommandBuffer*>(context);
        const auto index = buffer.entities_.size();
        buffer.entities_.push_back({});
        buffer.operations_.push_back(
            {Commands::Kind::spawn, {{}, &buffer, index, buffer.epoch_, &resolveThunk}});
        for (std::size_t i = 0; i < count; ++i)
            buffer.operations_.push_back({Commands::Kind::addDefault,
                                          {{}, &buffer, index, buffer.epoch_, &resolveThunk},
                                          buffer.find(types[i])});
        return EntityToken(&buffer, index, buffer.epoch_, &resolveThunk);
    }
    static void appendThunk(void* context, Commands::Kind kind, CommandTarget target,
                            std::type_index type, void* value, void (*construct)(void*, void*),
                            void (*destroy)(void*), std::size_t size, std::size_t alignment) {
        auto& buffer = *static_cast<CommandBuffer*>(context);
        const auto component = (kind == Commands::Kind::despawn) ? 0 : buffer.find(type);
        void* payload = nullptr;
        bool constructed = false;
        if (kind == Commands::Kind::add && component < std::tuple_size_v<Tuple>) {
            // Reserve the operation first so allocation/constructor failure cannot
            // strand a live payload without a registered destructor.
            buffer.operations_.push_back({kind, target, component, nullptr, destroy, false});
            payload = buffer.allocate(size, alignment);
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
            try {
                construct(payload, value);
                constructed = true;
            } catch (...) {
                buffer.operations_.pop_back();
                throw;
            }
#else
            construct(payload, value);
            constructed = true;
#endif
            buffer.operations_.back().payload = payload;
            buffer.operations_.back().constructed = true;
            return;
        }
        buffer.operations_.push_back({kind, target, component, payload, nullptr, constructed});
    }
    template <std::size_t I = 0> std::size_t find(std::type_index type) const {
        if constexpr (I == std::tuple_size_v<Tuple>)
            return I;
        else if (type == typeid(std::tuple_element_t<I, Tuple>))
            return I;
        else
            return find<I + 1>(type);
    }
    template <std::size_t I = 0>
    bool addAt(World& world, std::size_t component, Entity entity, const void* value) {
        if constexpr (I == std::tuple_size_v<Tuple>)
            return false;
        else if (component == I) {
            using T = std::tuple_element_t<I, Tuple>;
            if (value)
                return world.template emplace<T>(
                           entity, std::move(*const_cast<T*>(static_cast<const T*>(value)))) !=
                       nullptr;
            if constexpr (std::is_default_constructible_v<T>)
                return world.template emplace<T>(entity) != nullptr;
            else
                return false;
        } else
            return addAt<I + 1>(world, component, entity, value);
    }
    template <std::size_t I = 0> bool removeAt(World& world, std::size_t component, Entity entity) {
        if constexpr (I == std::tuple_size_v<Tuple>)
            return false;
        else if (component == I)
            return world.template remove<std::tuple_element_t<I, Tuple>>(entity);
        else
            return removeAt<I + 1>(world, component, entity);
    }
    struct ArenaBlock {
        std::byte* data = nullptr;
        std::size_t capacity = 0;
        std::size_t used = 0;
        std::size_t alignment = alignof(std::max_align_t);
    };
    void* allocate(std::size_t size, std::size_t alignment) {
        alignment = std::max(alignment, alignof(std::max_align_t));
        auto fits = [&](ArenaBlock& block) {
            const auto offset = (block.used + alignment - 1) & ~(alignment - 1);
            if (!block.data || block.alignment < alignment || offset + size > block.capacity)
                return false;
            block.used = offset + size;
            return true;
        };
        for (std::size_t i = blockCursor_; i < blocks_.size(); ++i) {
            if (fits(blocks_[i])) {
                blockCursor_ = i;
                const auto end = blocks_[i].used;
                return blocks_[i].data + end - size;
            }
        }
        if (blockCursor_ > 0) {
            for (std::size_t i = 0; i < blockCursor_; ++i) {
                if (fits(blocks_[i])) {
                    blockCursor_ = i;
                    const auto end = blocks_[i].used;
                    return blocks_[i].data + end - size;
                }
            }
        }
        {
            const auto capacity = std::max<std::size_t>(4096, size + alignment);
            blocks_.reserve(blocks_.size() + 1);
            ArenaBlock block;
            block.data =
                static_cast<std::byte*>(::operator new(capacity, std::align_val_t(alignment)));
            block.capacity = capacity;
            block.alignment = alignment;
            blocks_.push_back(block);
            blockCursor_ = blocks_.size() - 1;
            blocks_.back().used = size;
            return blocks_.back().data;
        }
    }
    void clear() {
        for (const auto& op : operations_)
            if (op.constructed && op.destroy)
                op.destroy(op.payload);
        operations_.clear();
        for (auto& block : blocks_)
            block.used = 0;
        blockCursor_ = 0;
    }
    std::vector<Operation> operations_;
    std::vector<Entity> entities_;
    std::vector<ArenaBlock> blocks_;
    std::uint64_t epoch_ = 1;
    std::size_t blockCursor_ = 0;
};
} // namespace detail

} // namespace engine::ecs
