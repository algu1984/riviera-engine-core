#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace engine::runtime {

// A single simulation producer and a single render consumer exchange reusable
// extraction payloads. Construct the exchange before starting either worker.
// Destruction requires both workers to be quiescent and all handles released.
template <class T> class RequestedExchange {
  private:
    enum class State : unsigned char { free, writing, ready, reading };
    static constexpr int no_slot = -1;

    static_assert(std::atomic<int>::is_always_lock_free,
                  "requested exchange requires lock-free int atomics");
    static_assert(std::atomic<bool>::is_always_lock_free,
                  "requested exchange requires lock-free bool atomics");
    static_assert(std::atomic<State>::is_always_lock_free,
                  "requested exchange requires lock-free slot-state atomics");
    static_assert(std::is_default_constructible_v<T>, "payload must be default constructible");

  public:
    class ReadHandle {
      public:
        ReadHandle() noexcept = default;
        ReadHandle(const ReadHandle&) = delete;
        ReadHandle& operator=(const ReadHandle&) = delete;

        ReadHandle(ReadHandle&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), index_(other.index_) {}
        ReadHandle& operator=(ReadHandle&& other) noexcept {
            if (this != &other) {
                release();
                owner_ = std::exchange(other.owner_, nullptr);
                index_ = other.index_;
            }
            return *this;
        }
        ~ReadHandle() {
            release();
        }

        explicit operator bool() const noexcept {
            return owner_ != nullptr;
        }
        const T* get() const noexcept {
            return owner_ ? &owner_->payloads_[index_] : nullptr;
        }
        const T* operator->() const noexcept {
            return get();
        }
        const T& operator*() const noexcept {
            return *get();
        }

      private:
        friend class RequestedExchange;
        ReadHandle(RequestedExchange* owner, int index) noexcept : owner_(owner), index_(index) {}
        void release() noexcept {
            if (owner_) {
                owner_->states_[index_].store(State::free, std::memory_order_release);
                owner_ = nullptr;
            }
        }
        RequestedExchange* owner_ = nullptr;
        int index_ = no_slot;
    };

    RequestedExchange() = default;
    RequestedExchange(const RequestedExchange&) = delete;
    RequestedExchange& operator=(const RequestedExchange&) = delete;

    // Returns true only when this call creates a new outstanding request. A
    // request while a ready payload is waiting is satisfied by that payload.
    bool request() noexcept {
        if (ready_.load(std::memory_order_acquire) != no_slot)
            return false;
        bool expected = false;
        return pending_.compare_exchange_strong(expected, true, std::memory_order_release,
                                                std::memory_order_relaxed);
    }

    // True while a request is pending, including when both payloads are busy.
    bool requested() const noexcept {
        return pending_.load(std::memory_order_acquire);
    }

    // Called only by the simulation owner at a safe tick boundary. The fill
    // callback must finish before returning and must not retain references to T.
    // If it throws, the slot and request are restored before the exception escapes.
    template <class Fill> bool produce(Fill&& fill) {
        if (ready_.load(std::memory_order_acquire) != no_slot)
            return false;
        if (!pending_.exchange(false, std::memory_order_acq_rel))
            return false;

        int index = no_slot;
        for (int candidate = 0; candidate < 2; ++candidate) {
            State expected = State::free;
            if (states_[candidate].compare_exchange_strong(expected, State::writing,
                                                           std::memory_order_acquire,
                                                           std::memory_order_relaxed)) {
                index = candidate;
                break;
            }
        }
        if (index == no_slot) {
            pending_.store(true, std::memory_order_release);
            return false;
        }

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
        try {
            std::forward<Fill>(fill)(payloads_[index]);
        } catch (...) {
            states_[index].store(State::free, std::memory_order_release);
            pending_.store(true, std::memory_order_release);
            throw;
        }
#else
        std::forward<Fill>(fill)(payloads_[index]);
#endif
        states_[index].store(State::ready, std::memory_order_release);
        ready_.store(index, std::memory_order_release);
        return true;
    }

    // Called only by the render owner. Empty means no payload is ready now.
    ReadHandle acquire() noexcept {
        const int index = ready_.exchange(no_slot, std::memory_order_acquire);
        if (index == no_slot)
            return {};
        State expected = State::ready;
        const bool changed = states_[index].compare_exchange_strong(
            expected, State::reading, std::memory_order_acquire, std::memory_order_relaxed);
        assert(changed && "ready slot must transition to reader ownership");
        return ReadHandle(this, index);
    }

  private:
    std::array<T, 2> payloads_{};
    std::array<std::atomic<State>, 2> states_{{State::free, State::free}};
    std::atomic<int> ready_{no_slot};
    std::atomic<bool> pending_{false};
};

} // namespace engine::runtime
