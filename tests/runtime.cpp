#include "engine/math.hpp"
#include "engine/runtime/fixed_step_clock.hpp"
#include "engine/runtime/snapshot_exchange.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

struct alignas(64) AlignedValue {
    int value = 0;
};

static void require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}

static void clock_behavior() {
    engine::runtime::FixedStepClock clock;
    clock.beginFrame(.015);
    require(clock.consumeStep() && !clock.consumeStep(), "15ms produces one 120Hz tick");
    require(std::abs(clock.alpha() - .8) < 1e-6, "clock interpolates remaining time");
    clock.reset();
    clock.beginFrame(1.);
    int ticks = 0;
    while (clock.consumeStep())
        ++ticks;
    require(ticks == 12 && clock.discardedSeconds() > .89, "clock bounds suspension catch-up");
}

static void rotation_invariants() {
    using namespace engine;
    for (auto end : {rotateX(pi), rotateY(pi), rotateZ(pi), rotateY(1.5f) * rotateX(2.1f)}) {
        auto mid = rotation(slerp(quaternion(M4::identity()), quaternion(end), .5f));
        auto x = direction(mid, {1, 0, 0}), y = direction(mid, {0, 1, 0}),
             z = direction(mid, {0, 0, 1});
        require(std::abs(length(x) - 1) < 1e-5 && std::abs(length(y) - 1) < 1e-5 &&
                    std::abs(length(z) - 1) < 1e-5,
                "180-degree interpolation remains orthonormal");
        require(std::abs(dot(x, y)) < 1e-5 && dot(cross(x, y), z) > .9999f,
                "quaternion orientation preserves handedness");
        auto restored = rotation(quaternion(end));
        for (int i = 0; i < 16; ++i)
            require(std::abs(restored.m[i] - end.m[i]) < 1e-5, "matrix quaternion round trip");
    }
    auto near = rotation(
        slerp(quaternion(rotateY(179 * pi / 180)), quaternion(rotateY(-179 * pi / 180)), .5f));
    require(direction(near, {0, 0, 1}).z < -.999f, "orientation follows shortest arc across wrap");
}

static void bounded_exchange_and_lifetime() {
    using engine::runtime::SnapshotExchange;
    SnapshotExchange<std::uint64_t> exchange;
    require(exchange.reset(10), "snapshot initial seed");
    auto seed = exchange.acquire();
    require(exchange.publish(11, 1, 1. / 120.), "first tick published");
    auto first = exchange.acquire();
    require(exchange.publish(12, 2, 2. / 120.), "second tick published");
    auto second = exchange.acquire();
    require(std::abs(second.alpha(1.5 / 120.) - .5) < 1e-6, "timestamps interpolate");
    require(second.alpha(-1) == 0 && second.alpha(10) == 1, "presentation clamps history edges");
    require(!exchange.publish(13, 3, 3. / 120.), "three pinned generations bound publication");
    require(!exchange.reset(100, 0, 0), "reset fails while all slots are pinned");
    require(seed.current->value == 10 && first.current->value == 11,
            "old readers remain immutable");
    seed = {};
    first = {};
    require(exchange.reset(100, 0, 0), "reset retries after releasing old readers");
    auto reset = exchange.acquire();
    require(reset.current->discontinuity && reset.previous == reset.current &&
                reset.alpha(-10) == 1,
            "reset seeds both snapshots and disables interpolation");
    require(second.current->value == 12, "reader remains valid through reset");

    SnapshotExchange<int>::Pair afterDestruction;
    {
        auto shortLived = std::make_unique<SnapshotExchange<int>>();
        require(shortLived->reset(42), "seed exchange that will be destroyed");
        afterDestruction = shortLived->acquire();
    }
    require(afterDestruction.current->value == 42,
            "acquired immutable snapshots outlive their exchange");

    SnapshotExchange<AlignedValue> aligned;
    require(aligned.reset(AlignedValue{73}), "over-aligned snapshot seed");
    auto alignedPair = aligned.acquire();
    require(alignedPair.current->value.value == 73,
            "over-aligned payloads live outside the LFRC wrapper allocation");

    SnapshotExchange<int> discontinuity;
    require(discontinuity.reset(1), "seed discontinuity exchange");
    auto oldPair = discontinuity.acquire();
    require(discontinuity.publish(2, 1, 1.), "publish before discontinuity");
    auto currentPair = discontinuity.acquire();
    require(discontinuity.publish(3, 2, 2., true), "publish a discontinuity snapshot");
    auto teleported = discontinuity.acquire();
    require(teleported.previous == teleported.current && teleported.current->discontinuity &&
                teleported.alpha(0.) == 1 && oldPair.current->value == 1 &&
                currentPair.current->value == 2,
            "discontinuity publication reseeds both members and preserves older readers");
}

static void concurrent_reader_and_publisher() {
    using engine::runtime::SnapshotExchange;
    SnapshotExchange<std::uint64_t> exchange;
    require(exchange.reset(0), "seed concurrent exchange");
    std::atomic<bool> done{false};
    std::atomic<bool> valid{true};
    std::atomic<std::uint64_t> published{0};

    std::thread reader([&] {
        do {
            auto pair = exchange.acquire();
            if (!pair.current || pair.current->value != pair.current->tick ||
                pair.previous->value != pair.previous->tick ||
                pair.current->seconds != static_cast<double>(pair.current->value) ||
                pair.previous->seconds != static_cast<double>(pair.previous->value) ||
                pair.alpha(pair.current->seconds) < 0 || pair.alpha(pair.current->seconds) > 1)
                valid.store(false, std::memory_order_relaxed);
        } while (!done.load(std::memory_order_acquire));
    });

    auto producer = [&](std::uint64_t writer) {
        for (std::uint64_t sequence = 1; sequence <= 5000; ++sequence) {
            const std::uint64_t value = (writer << 32) | sequence;
            while (!exchange.publish(value, value, static_cast<double>(value)))
                std::this_thread::yield();
            published.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread firstProducer(producer, 1);
    std::thread secondProducer(producer, 2);
    firstProducer.join();
    secondProducer.join();
    done.store(true, std::memory_order_release);
    reader.join();
    require(valid.load(std::memory_order_relaxed),
            "concurrent readers see complete immutable pairs");
    require(published.load(std::memory_order_relaxed) == 10000,
            "concurrent publishers progress as readers release bounded slots");
}

int main() {
    clock_behavior();
    rotation_invariants();
    bounded_exchange_and_lifetime();
    concurrent_reader_and_publisher();
    std::puts("PASS portable clock and bounded lock-free snapshot exchange");
}
