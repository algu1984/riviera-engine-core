#include "engine/jobs/job_scheduler.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using namespace engine::jobs;

static void test_async_dag_and_owned_lifetime() {
    JobScheduler scheduler;
    std::atomic<int> order{0};
    int payload = 0;
    JobScheduler::Ticket ticket;
    {
        TaskGraph graph;
        const auto first = graph.add("first", [&] {
            payload = 42;
            order.store(1, std::memory_order_relaxed);
            return true;
        });
        const auto left = graph.add("left",
                                    [&] {
                                        assert(order.load(std::memory_order_relaxed) & 1);
                                        order.fetch_or(2, std::memory_order_relaxed);
                                        return true;
                                    },
                                    {first});
        const auto right = graph.add("right",
                                     [&] {
                                         assert(order.load(std::memory_order_relaxed) & 1);
                                         order.fetch_or(4, std::memory_order_relaxed);
                                         return true;
                                     },
                                     {first});
        graph.add("join",
                  [&] { return order.load(std::memory_order_relaxed) == 7 && payload == 42; },
                  {left, right});
        ticket = scheduler.submit(graph);
    }
    assert(!scheduler.poll(ticket).complete);
    while (scheduler.helpOne()) {
    }
    const auto result = scheduler.poll(ticket);
    assert(result.complete && result.success && result.completed == 4);
    assert(order.load(std::memory_order_relaxed) == 7);
}

static void test_failed_branch_does_not_release_dependents() {
    JobScheduler scheduler;
    TaskGraph graph;
    const auto fail = graph.add("expected failure", [] { return false; });
    bool dependentRan = false;
    graph.add("cancelled dependent",
              [&] {
                  dependentRan = true;
                  return true;
              },
              {fail});
    auto ticket = scheduler.submit(graph);
    while (scheduler.helpOne()) {
    }
    const auto result = scheduler.poll(ticket);
    assert(result.complete && !result.success && result.completed == 0);
    assert(result.error == "Task failed: expected failure");
    assert(!dependentRan);
}

static void test_validation_and_empty_graph() {
    JobScheduler scheduler;
    TaskGraph cycle;
    const auto first = cycle.add("first", [] { return true; });
    const auto second = cycle.add("second", [] { return true; }, {first});
    assert(cycle.depends(first, second));
    auto invalid = scheduler.submit(cycle);
    assert(scheduler.poll(invalid).complete && !scheduler.poll(invalid).success);

    TaskGraph empty;
    auto done = scheduler.submit(empty);
    const auto result = scheduler.poll(done);
    assert(result.complete && result.success && result.completed == 0);
}

static void test_ticket_can_be_dropped_while_queued() {
    JobScheduler scheduler;
    std::atomic<int> calls{0};
    {
        TaskGraph graph;
        graph.add("owned closure", [&] {
            calls.fetch_add(1, std::memory_order_relaxed);
            return true;
        });
        auto ticket = scheduler.submit(graph);
        assert(ticket);
    }
    assert(scheduler.hasOutstanding());
    while (scheduler.helpOne()) {
    }
    assert(!scheduler.hasOutstanding());
    assert(calls.load(std::memory_order_relaxed) == 1);
}

static void test_multiple_submit_producers() {
    JobScheduler scheduler;
    constexpr int producerCount = 4;
    constexpr int graphsPerProducer = 40;
    std::atomic<int> producersLeft{producerCount};
    std::atomic<int> calls{0};
    std::vector<std::vector<JobScheduler::Ticket>> tickets(producerCount);
    std::vector<std::thread> helpers;
    for (int i = 0; i < producerCount; ++i) {
        helpers.emplace_back([&] {
            while (producersLeft.load(std::memory_order_acquire) != 0 ||
                   scheduler.hasOutstanding()) {
                if (!scheduler.helpOne())
                    std::this_thread::yield();
            }
        });
    }
    std::vector<std::thread> producers;
    for (int producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&, producer] {
            auto& ownedTickets = tickets[producer];
            ownedTickets.reserve(graphsPerProducer);
            for (int i = 0; i < graphsPerProducer; ++i) {
                TaskGraph graph;
                graph.add("producer task", [&calls] {
                    calls.fetch_add(1, std::memory_order_relaxed);
                    return true;
                });
                ownedTickets.push_back(scheduler.submit(graph));
            }
            producersLeft.fetch_sub(1, std::memory_order_release);
        });
    }
    for (auto& producer : producers)
        producer.join();
    for (auto& helper : helpers)
        helper.join();
    assert(calls.load(std::memory_order_relaxed) == producerCount * graphsPerProducer);
    for (const auto& group : tickets)
        for (const auto& ticket : group) {
            const auto result = scheduler.poll(ticket);
            assert(result.complete && result.success && result.completed == 1);
        }
}

static void test_submit_races_with_helpers() {
    JobScheduler scheduler;
    constexpr int graphCount = 80;
    constexpr int tasksPerGraph = 64;
    std::atomic<bool> submitting{true};
    std::atomic<int> executed{0};
    std::thread helper([&] {
        while (submitting.load(std::memory_order_acquire) || scheduler.hasOutstanding()) {
            if (!scheduler.helpOne())
                std::this_thread::yield();
        }
    });
    std::vector<JobScheduler::Ticket> tickets;
    tickets.reserve(graphCount);
    for (int graphIndex = 0; graphIndex < graphCount; ++graphIndex) {
        TaskGraph graph;
        TaskId previous = graph.add("start", [&executed] {
            executed.fetch_add(1, std::memory_order_relaxed);
            return true;
        });
        for (int task = 1; task < tasksPerGraph; ++task) {
            previous = graph.add("chain",
                                 [&executed] {
                                     executed.fetch_add(1, std::memory_order_relaxed);
                                     return true;
                                 },
                                 {previous});
        }
        tickets.push_back(scheduler.submit(graph));
    }
    submitting.store(false, std::memory_order_release);
    helper.join();
    assert(executed.load(std::memory_order_relaxed) == graphCount * tasksPerGraph);
    for (const auto& ticket : tickets) {
        const auto result = scheduler.poll(ticket);
        assert(result.complete && result.success && result.completed == tasksPerGraph);
    }
}

static void test_concurrent_graphs_and_workers() {
    JobScheduler scheduler;
    constexpr int graphCount = 64;
    constexpr int tasksPerGraph = 32;
    std::atomic<int> calls{0};
    std::vector<JobScheduler::Ticket> tickets;
    tickets.reserve(graphCount);
    for (int graphIndex = 0; graphIndex < graphCount; ++graphIndex) {
        TaskGraph graph;
        for (int task = 0; task < tasksPerGraph; ++task)
            graph.add("concurrent", [&] {
                calls.fetch_add(1, std::memory_order_relaxed);
                return true;
            });
        tickets.push_back(scheduler.submit(graph));
    }
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&] {
            while (scheduler.hasOutstanding()) {
                if (!scheduler.helpOne())
                    std::this_thread::yield();
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    assert(calls.load(std::memory_order_relaxed) == graphCount * tasksPerGraph);
    for (const auto& ticket : tickets) {
        const auto result = scheduler.poll(ticket);
        assert(result.complete && result.success && result.completed == tasksPerGraph);
    }
}

int main() {
    test_async_dag_and_owned_lifetime();
    test_failed_branch_does_not_release_dependents();
    test_validation_and_empty_graph();
    test_ticket_can_be_dropped_while_queued();
    test_concurrent_graphs_and_workers();
    test_submit_races_with_helpers();
    test_multiple_submit_producers();
    std::puts("job scheduler checks passed");
}
