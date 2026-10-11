#include "engine/ecs/world.hpp"
#include "engine/jobs/job_scheduler.hpp"
#include "engine/runtime/requested_exchange.hpp"
#include "engine/runtime/snapshot_exchange.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/utsname.h>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
struct Position {
    float x{}, y{}, z{};
};
struct Velocity {
    float x{}, y{}, z{};
};
using World = engine::ecs::World<Position, Velocity>;
constexpr float dt = 1.0F / 120.0F;
constexpr int ticks_per_sample = 100;
volatile double sink = 0;

struct Stats {
    double median{}, p95{};
};
Stats stats(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto p95_index = (values.size() * 95 + 99) / 100 - 1;
    return {values[values.size() / 2], values[p95_index]};
}
template <class Fn> Stats measure(int warmup, int samples, Fn&& fn) {
    for (int i = 0; i < warmup; ++i)
        fn();
    std::vector<double> times;
    for (int i = 0; i < samples; ++i) {
        const auto start = Clock::now();
        fn();
        times.push_back(std::chrono::duration<double, std::nano>(Clock::now() - start).count());
    }
    return stats(std::move(times));
}
void print(const std::string& group, const std::string& name, std::size_t units, std::size_t bytes,
           const Stats& s, std::uint64_t checksum, int helpers = 0, double setup_ns = 0,
           int capacity_growths = 0, std::size_t work_iterations = 0) {
    std::cout << std::setprecision(12) << "{\"group\":\"" << group << "\",\"case\":\"" << name
              << "\",\"units\":" << units << ",\"payload_bytes\":" << bytes
              << ",\"median_ns\":" << s.median << ",\"p95_ns\":" << s.p95
              << ",\"units_per_second\":" << (s.median ? units * 1e9 / s.median : 0)
              << ",\"checksum\":" << checksum << ",\"workers\":" << helpers
              << ",\"setup_ns\":" << setup_ns << ",\"capacity_growths\":" << capacity_growths
              << ",\"work_iterations\":" << work_iterations << "}\n";
}
std::uint64_t checksum(const World& w) {
    double sum = 0;
    w.template for_each<Position>([&](auto, const Position& p) {
        sum += static_cast<double>(p.x) + static_cast<double>(p.y) + static_cast<double>(p.z);
    });
    return static_cast<std::uint64_t>(sum * 100000.0);
}
void ecs_bench(std::size_t n, int warmup, int samples) {
    const auto setup_start = Clock::now();
    World world;
    std::vector<Position> positions(n);
    std::vector<Velocity> velocities(n);
    for (std::size_t i = 0; i < n; ++i) {
        positions[i] = {float(i % 97), float(i % 53), float(i % 31)};
        velocities[i] = {0.5F, -0.25F, 1.0F};
        auto e = world.create();
        world.emplace<Position>(e, positions[i]);
        world.emplace<Velocity>(e, velocities[i]);
    }
    const auto setup_ns =
        std::chrono::duration<double, std::nano>(Clock::now() - setup_start).count();
    const auto ecs = measure(warmup, samples, [&] {
        for (int tick = 0; tick < ticks_per_sample; ++tick)
            world.template for_each<Position, Velocity>([](auto, Position& p, Velocity& v) {
                p.x += v.x * dt;
                p.y += v.y * dt;
                p.z += v.z * dt;
            });
    });
    const auto ecs_checksum = checksum(world);
    print("ecs_iteration", "core_cached", n, 0,
          {ecs.median / ticks_per_sample, ecs.p95 / ticks_per_sample}, ecs_checksum, 0, setup_ns);
    const auto direct = measure(warmup, samples, [&] {
        for (int tick = 0; tick < ticks_per_sample; ++tick)
            for (std::size_t i = 0; i < n; ++i) {
                positions[i].x += velocities[i].x * dt;
                positions[i].y += velocities[i].y * dt;
                positions[i].z += velocities[i].z * dt;
            }
    });
    double sum = 0;
    for (auto p : positions)
        sum += static_cast<double>(p.x) + static_cast<double>(p.y) + static_cast<double>(p.z);
    const auto direct_checksum = static_cast<std::uint64_t>(sum * 100000.0);
    if (ecs_checksum != direct_checksum)
        std::abort();
    print("ecs_iteration", "direct_arrays", n, 0,
          {direct.median / ticks_per_sample, direct.p95 / ticks_per_sample}, direct_checksum, 0,
          setup_ns);
}

struct Payload {
    std::vector<float> values;
};
void exchange_bench(std::size_t size, int samples) {
    engine::runtime::RequestedExchange<Payload> exchange;
    std::atomic<int> capacity_growths{0};
    auto fill = [&](Payload& p) {
        const auto before = p.values.capacity();
        p.values.resize(size);
        if (p.values.capacity() != before)
            capacity_growths.fetch_add(1, std::memory_order_relaxed);
        for (std::size_t j = 0; j < size; ++j)
            p.values[j] = static_cast<float>(j);
    };
    // Prime and retain both slots at once so steady-state payload storage is reused.
    exchange.request();
    while (!exchange.produce(fill))
        std::this_thread::yield();
    auto first = exchange.acquire();
    exchange.request();
    while (!exchange.produce(fill))
        std::this_thread::yield();
    auto second = exchange.acquire();
    if (!first || !second)
        std::abort();
    first = {};
    second = {};
    capacity_growths.store(0, std::memory_order_relaxed);
    // The consumer submits requests directly; the producer only observes and fills them.
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        while (!stop.load(std::memory_order_acquire)) {
            if (!exchange.produce(fill))
                std::this_thread::yield();
        }
    });
    std::vector<double> ns;
    std::uint64_t sum = 0;
    for (int i = 0; i < samples; ++i) {
        const auto start = Clock::now();
        if (!exchange.request())
            std::abort();
        engine::runtime::RequestedExchange<Payload>::ReadHandle h;
        const auto deadline = start + std::chrono::seconds(5);
        while (!(h = exchange.acquire())) {
            if (Clock::now() > deadline)
                std::abort();
            std::this_thread::yield();
        }
        if (!h || h->values.size() != size)
            std::abort();
        sum += static_cast<std::uint64_t>(h->values.back());
        ns.push_back(std::chrono::duration<double, std::nano>(Clock::now() - start).count());
    }
    stop.store(true, std::memory_order_release);
    producer.join();
    print("requested_exchange", "two_thread_handoff", size, size * sizeof(float),
          stats(std::move(ns)), sum, 2, 0, capacity_growths.load(std::memory_order_relaxed));
    std::vector<float> source(size), destination(size, -1.0F);
    for (std::size_t i = 0; i < size; ++i)
        source[i] = static_cast<float>(i + 1);
    std::size_t generation = 0;
    constexpr int copies_per_sample = 128;
    const auto copy = measure(2, samples, [&] {
        for (int copy_index = 0; copy_index < copies_per_sample; ++copy_index) {
            source[0] = static_cast<float>(++generation);
            std::atomic_signal_fence(std::memory_order_seq_cst);
            std::copy(source.begin(), source.end(), destination.begin());
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    });
    sink += destination.empty() ? 0 : destination.back();
    double copy_sum = 0;
    for (const auto value : destination)
        copy_sum += value;
    sink += copy_sum;
    print("requested_exchange", "vector_copy_baseline", size, size * sizeof(float),
          {copy.median / copies_per_sample, copy.p95 / copies_per_sample},
          static_cast<std::uint64_t>(copy_sum), 0);
}

[[gnu::noinline]] std::uint64_t job_work(std::uint64_t seed, std::size_t work) {
    std::uint64_t x = seed;
    for (std::size_t i = 0; i < work; ++i)
        x = x * 1664525ULL + 1013904223ULL;
    return x;
}
void jobs_bench(std::size_t task_count, std::size_t work, int workers_count, int warmup,
                int samples) {
    engine::jobs::JobScheduler scheduler;
    engine::jobs::TaskGraph graph;
    std::atomic<std::uint64_t> total{0};
    volatile std::uint64_t seeds[8] = {11, 23, 37, 41, 53, 67, 71, 89};
    for (std::size_t t = 0; t < task_count; ++t) {
        const auto task_index = t;
        graph.add("independent", [&, task_index] {
            total.fetch_add(job_work(seeds[task_index], work), std::memory_order_relaxed);
            return true;
        });
    }
    const auto prepared = graph.prepare();
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int i = 1; i < workers_count; ++i)
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                if (!scheduler.helpOne())
                    std::this_thread::yield();
            }
        });
    std::vector<double> ns;
    std::uint64_t expected = 0;
    for (std::size_t t = 0; t < task_count; ++t)
        expected += job_work(static_cast<std::uint64_t>(seeds[t]), work);
    auto run_once = [&] {
        total.store(0, std::memory_order_relaxed);
        const auto start = Clock::now();
        if (workers_count == 0) {
            for (std::size_t t = 0; t < task_count; ++t)
                total.fetch_add(job_work(seeds[t], work), std::memory_order_relaxed);
        } else {
            auto ticket = scheduler.submit(prepared);
            const auto deadline = start + std::chrono::seconds(5);
            while (!scheduler.complete(ticket)) {
                if (Clock::now() > deadline)
                    std::abort();
                if (!scheduler.helpOne())
                    std::this_thread::yield();
            }
        }
        const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        if (total.load(std::memory_order_relaxed) != expected)
            std::abort();
        return elapsed;
    };
    for (int s = 0; s < warmup; ++s)
        (void)run_once();
    for (int s = 0; s < samples; ++s)
        ns.push_back(run_once());
    stop.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();
    print("job_scheduler", workers_count == 0 ? "serial_same_work" : "prepared_independent",
          task_count, 0, stats(std::move(ns)), expected, workers_count == 0 ? 1 : workers_count, 0,
          0, work);
}
} // namespace

int main(int argc, char** argv) {
    const int samples = argc > 1 ? std::clamp(std::atoi(argv[1]), 1, 10000) : 31;
    const int warmup = argc > 2 ? std::clamp(std::atoi(argv[2]), 0, 10000) : 2;
    struct utsname machine {};
    uname(&machine);
    std::cout << "{\"schema_version\":1,\"metadata\":{\"compiler\":\"" << __VERSION__
              << "\",\"cplusplus\":202002,\"hardware_threads\":"
              << std::thread::hardware_concurrency() << ",\"machine\":\"" << machine.machine
              << "\",\"os\":\"" << machine.sysname << "\",\"warmup\":" << warmup
              << ",\"samples\":" << samples << ",\"ticks_per_sample\":" << ticks_per_sample
              << ",\"copies_per_sample\":128"
              << ",\"flags\":\"-O3 -DNDEBUG -ffp-contract=off\"},\"type\":\"header\"}\n";
    for (auto n : {1000U, 10000U, 100000U})
        ecs_bench(n, warmup, samples);
    exchange_bench(64, samples);
    exchange_bench(1024, samples);
    for (int workers : {0, 1, 2, 4}) {
        jobs_bench(8, 1, workers, warmup, samples);
        jobs_bench(8, 10000, workers, warmup, samples);
    }
    return sink == -1 ? 1 : 0;
}
