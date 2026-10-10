#pragma once

#include <xenium/reclamation/lock_free_ref_count.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace engine::runtime {

// Immutable snapshots and their paired publication use Xenium LFRC guards.
// At most three Snapshot objects may remain live, including values pinned by
// slow readers. Publication is multi-writer; it can fail while that quota is
// occupied. Pair guards keep values alive through reset and exchange teardown.
template <class T> class SnapshotExchange {
  public:
    struct Snapshot {
        T value;
        std::uint64_t tick;
        double seconds;
        bool discontinuity;
    };

  private:
    using Reclaimer = xenium::reclamation::lock_free_ref_count<
        xenium::reclamation::lock_free_ref_count_traits<true, 0>>;

    struct Quota {
        std::atomic<unsigned> live{0};
        std::atomic<unsigned> references{1}; // held by the exchange and each live snapshot node

        static_assert(std::atomic<unsigned>::is_always_lock_free,
                      "snapshot quota requires lock-free unsigned atomics");

        void retain() noexcept {
            references.fetch_add(1, std::memory_order_relaxed);
        }
        void release_reference() noexcept {
            if (references.fetch_sub(1, std::memory_order_acq_rel) == 1)
                delete this;
        }

        bool reserve() noexcept {
            auto current = live.load(std::memory_order_relaxed);
            while (current < 3) {
                if (live.compare_exchange_weak(current, current + 1, std::memory_order_acquire,
                                               std::memory_order_relaxed))
                    return true;
            }
            return false;
        }
        void release_snapshot() noexcept {
            live.fetch_sub(1, std::memory_order_release);
        }
    };

    struct SnapshotNode : Reclaimer::template enable_concurrent_ptr<SnapshotNode, 0> {
        Quota* quota;
        std::unique_ptr<Snapshot> snapshot;

        SnapshotNode(Quota* owner, std::unique_ptr<Snapshot> value) noexcept
            : quota(owner), snapshot(std::move(value)) {
            quota->retain();
        }
        ~SnapshotNode() {
            snapshot.reset();
            quota->release_snapshot();
            quota->release_reference();
        }
    };

    using SnapshotPtr = Reclaimer::template concurrent_ptr<SnapshotNode, 0>;
    using SnapshotGuard = typename SnapshotPtr::guard_ptr;
    using SnapshotMarked = typename SnapshotPtr::marked_ptr;

    struct PublicationNode : Reclaimer::template enable_concurrent_ptr<PublicationNode, 0> {
        SnapshotGuard previous;
        SnapshotGuard current;

        PublicationNode(const SnapshotGuard& oldSnapshot, const SnapshotGuard& newSnapshot)
            : previous(oldSnapshot), current(newSnapshot) {}
    };

    using PublicationPtr = Reclaimer::template concurrent_ptr<PublicationNode, 0>;
    using PublicationGuard = typename PublicationPtr::guard_ptr;
    using PublicationMarked = typename PublicationPtr::marked_ptr;

    static_assert(std::atomic<SnapshotMarked>::is_always_lock_free,
                  "snapshot guards require lock-free pointer atomics");
    static_assert(std::atomic<PublicationMarked>::is_always_lock_free,
                  "publication requires lock-free pointer atomics");
    static_assert(alignof(SnapshotNode) <= alignof(std::max_align_t),
                  "LFRC snapshot wrappers must use ordinary alignment");
    static_assert(alignof(PublicationNode) <= alignof(std::max_align_t),
                  "LFRC publication wrappers must use ordinary alignment");

  public:
    class SnapshotHandle {
      public:
        SnapshotHandle() noexcept = default;
        SnapshotHandle(const SnapshotHandle&) noexcept = default;
        SnapshotHandle(SnapshotHandle&&) noexcept = default;
        SnapshotHandle& operator=(const SnapshotHandle&) noexcept = default;
        SnapshotHandle& operator=(SnapshotHandle&&) noexcept = default;

        explicit operator bool() const noexcept {
            return static_cast<bool>(guard_);
        }
        const Snapshot* get() const noexcept {
            return guard_ ? guard_->snapshot.get() : nullptr;
        }
        const Snapshot* operator->() const noexcept {
            return get();
        }
        const Snapshot& operator*() const noexcept {
            return *get();
        }
        friend bool operator==(const SnapshotHandle& a, const SnapshotHandle& b) noexcept {
            return a.guard_.get() == b.guard_.get();
        }

      private:
        friend class SnapshotExchange;
        explicit SnapshotHandle(SnapshotGuard guard) noexcept : guard_(std::move(guard)) {}
        SnapshotGuard guard_;
    };

    struct Pair {
        SnapshotHandle previous, current;

        double alpha(double presentationSeconds) const {
            if (!previous || !current || current->discontinuity ||
                current->seconds <= previous->seconds)
                return 1;
            const double a =
                (presentationSeconds - previous->seconds) / (current->seconds - previous->seconds);
            return a < 0 ? 0 : a > 1 ? 1 : a;
        }
    };

    SnapshotExchange() : quota_(new Quota) {}
    SnapshotExchange(const SnapshotExchange&) = delete;
    SnapshotExchange& operator=(const SnapshotExchange&) = delete;

    ~SnapshotExchange() {
        PublicationGuard old;
        old.acquire(published_, std::memory_order_acquire);
        published_.store(PublicationMarked{}, std::memory_order_release);
        if (old)
            old.reclaim();
        quota_->release_reference();
    }

    bool publish(T value, std::uint64_t tick, double seconds, bool discontinuity = false) {
        return publish_impl(std::move(value), tick, seconds, discontinuity, false);
    }

    bool reset(T value, std::uint64_t tick = 0, double seconds = 0) {
        return publish_impl(std::move(value), tick, seconds, true, true);
    }

    Pair acquire() const {
        PublicationGuard publication;
        publication.acquire(published_, std::memory_order_acquire);
        if (!publication)
            return {};
        return {SnapshotHandle{publication->previous}, SnapshotHandle{publication->current}};
    }

  private:
    bool publish_impl(T value, std::uint64_t tick, double seconds, bool discontinuity,
                      bool resetHead) {
        if (!quota_->reserve())
            return false;

        SnapshotNode* rawSnapshot = nullptr;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
        try {
            auto snapshot = std::make_unique<Snapshot>(
                Snapshot{std::move(value), tick, seconds, discontinuity});
            rawSnapshot = new SnapshotNode(quota_, std::move(snapshot));
        } catch (...) {
            quota_->release_snapshot();
            throw;
        }
#else
        auto snapshot =
            std::make_unique<Snapshot>(Snapshot{std::move(value), tick, seconds, discontinuity});
        rawSnapshot = new SnapshotNode(quota_, std::move(snapshot));
#endif
        SnapshotGuard initial(rawSnapshot);
        SnapshotGuard next(initial);
        initial.reclaim(); // consume the allocation's baseline while next remains guarded

        for (;;) {
            PublicationGuard observed;
            observed.acquire(published_, std::memory_order_acquire);
            const SnapshotGuard previous =
                discontinuity || resetHead || !observed ? next : observed->current;
            PublicationGuard replacement(new PublicationNode(previous, next));
            auto expected = static_cast<PublicationMarked>(observed);
            if (published_.compare_exchange_weak(expected, PublicationMarked(replacement),
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                if (observed)
                    observed.reclaim(); // release the root's old baseline reference
                return true;
            }
            replacement.reclaim();
        }
    }

    Quota* quota_;
    PublicationPtr published_;
};

} // namespace engine::runtime
