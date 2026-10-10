#include <xenium/harris_michael_hash_map.hpp>
#include <xenium/marked_ptr.hpp>
#include <xenium/reclamation/lock_free_ref_count.hpp>
#include <xenium/utils.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <future>
#include <thread>

namespace {

using AtomicMarkedPtr = std::atomic<xenium::marked_ptr<int, 2>>;
static_assert(AtomicMarkedPtr::is_always_lock_free,
              "the selected Xenium marked pointer must remain lock-free");

void test_marked_ptr_round_trip() {
    constexpr std::uintptr_t address = 0xf2345670u;
    auto* synthetic = reinterpret_cast<int*>(address);
    xenium::marked_ptr<int, 4> value(synthetic, 0xbu);
    assert(value.get() == synthetic);
    assert(value.mark() == 0xbu);

    xenium::marked_ptr<int, 4> null_with_mark(nullptr, 0x5u);
    assert(null_with_mark.get() == nullptr);
    assert(null_with_mark.mark() == 0x5u);
    assert(static_cast<bool>(null_with_mark));

    null_with_mark.reset();
    assert(null_with_mark.get() == nullptr);
    assert(null_with_mark.mark() == 0);
    assert(!static_cast<bool>(null_with_mark));

    int target = 0;
    AtomicMarkedPtr atomic_value{xenium::marked_ptr<int, 2>(&target, 1)};
    auto expected = atomic_value.load();
    assert(atomic_value.compare_exchange_strong(expected, xenium::marked_ptr<int, 2>(&target, 2)));
    assert(atomic_value.load().get() == &target);
    assert(atomic_value.load().mark() == 2);
}

void test_rotate_matches_uintptr_width() {
    constexpr std::uintptr_t value = static_cast<std::uintptr_t>(0x123456789abcdef0ull);
    const auto rotated = xenium::utils::rotate<7>::left(value);
    assert(xenium::utils::rotate<7>::right(rotated) == value);
}

using LfrcTraits = xenium::reclamation::lock_free_ref_count_traits<true, 0>;
using LfrcReclaimer = xenium::reclamation::lock_free_ref_count<LfrcTraits>;

struct alignas(8) LfrcNode : LfrcReclaimer::enable_concurrent_ptr<LfrcNode, 2> {
    explicit LfrcNode(std::atomic<int>& destruction_count) : destruction_count(destruction_count) {}
    ~LfrcNode() override {
        destruction_count.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<int>& destruction_count;
    std::uint64_t alignment_probe = 0;
};

void test_lfrc_guard_ownership_across_threads() {
    using ConcurrentPtr = LfrcReclaimer::concurrent_ptr<LfrcNode>;

    std::atomic<int> destruction_count{0};
    auto* node = new LfrcNode(destruction_count);
    assert(reinterpret_cast<std::uintptr_t>(node) % alignof(LfrcNode) == 0);
    typename ConcurrentPtr::marked_ptr marked(node, 3);
    typename ConcurrentPtr::guard_ptr owner(marked);
    std::promise<void> child_has_guard;
    std::promise<void> release_child;
    auto release_future = release_child.get_future();

    std::thread child([guard = typename ConcurrentPtr::guard_ptr(owner), &child_has_guard,
                       release_future = std::move(release_future)]() mutable {
        assert(guard.get() != nullptr);
        assert(guard.mark() == 3);
        child_has_guard.set_value();
        release_future.wait();
        guard.reset();
    });

    child_has_guard.get_future().wait();
    owner.reclaim();
    assert(destruction_count.load(std::memory_order_relaxed) == 0);
    release_child.set_value();
    child.join();
    assert(destruction_count.load(std::memory_order_relaxed) == 1);
}

void test_harris_michael_map_operations_and_iteration() {
    using Map = xenium::harris_michael_hash_map<std::uint64_t, std::uint64_t,
                                                xenium::policy::reclaimer<LfrcReclaimer>,
                                                xenium::policy::buckets<8>>;

    Map map;
    {
        typename Map::reclaimer::region_guard guard;
        assert(map.emplace(1, 10));
        assert(map.emplace(2, 20));

        auto found = map.find(1u);
        assert(found != map.end());
        assert(found->first == 1 && found->second == 10);
        assert(reinterpret_cast<std::uintptr_t>(&*found) % alignof(Map::value_type) == 0);
        found.reset();

        int count = 0;
        int sum = 0;
        for (const auto& entry : map) {
            ++count;
            sum += entry.second;
        }
        assert(count == 2 && sum == 30);

        assert(map.erase(1));
        assert(!map.contains(1));
        assert(map.contains(2));
    }
}

} // namespace

int main() {
    test_marked_ptr_round_trip();
    test_rotate_matches_uintptr_width();
    test_lfrc_guard_ownership_across_threads();
    test_harris_michael_map_operations_and_iteration();
    std::puts("Xenium portability checks passed");
}
