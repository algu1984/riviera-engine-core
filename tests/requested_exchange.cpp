#include "engine/runtime/requested_exchange.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

struct Extraction {
    unsigned sequence = 0;
    std::vector<unsigned> entities;
};

static void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}

static void request_and_reuse_contract() {
    engine::runtime::RequestedExchange<Extraction> exchange;
    unsigned fills = 0;
    auto fill = [&](Extraction& payload) {
        ++fills;
        payload.sequence = fills;
        payload.entities.clear();
        for (unsigned i = 0; i < 64; ++i)
            payload.entities.push_back(fills + i);
    };

    require(!exchange.requested(), "exchange starts without a request");
    require(!exchange.produce(fill) && fills == 0, "producer does not fill unsolicited payloads");
    require(exchange.request(), "first request becomes pending");
    require(!exchange.request(), "repeated requests coalesce");
    require(exchange.produce(fill), "requested payload is produced");
    require(!exchange.request(), "ready payload satisfies a request while waiting");
    auto first = exchange.acquire();
    require(first && first->sequence == 1 && first->entities.size() == 64,
            "consumer reads complete payload");
    const auto firstCapacity = first->entities.capacity();
    require(!exchange.produce(fill) && fills == 1,
            "request coalesced into ready payload does not cause another fill");

    require(exchange.request(), "consumer can request while retaining a payload");
    require(exchange.produce(fill), "second slot is writable while first is held");
    auto second = exchange.acquire();
    require(second && second->sequence == 2, "second payload remains independently readable");

    require(exchange.request(), "request remains pending while both slots are held");
    require(!exchange.produce(fill) && exchange.requested(),
            "producer returns immediately and preserves request when no slot is free");
    first = {};
    require(exchange.produce(fill), "released slot services the pending request");
    auto third = exchange.acquire();
    require(third && third->sequence == 3 && third->entities.size() == 64,
            "pending request publishes a complete payload");
    require(third->entities.capacity() >= firstCapacity,
            "reused payload storage retains vector capacity");
    require(second->sequence == 2, "held payload is not overwritten during reuse");
}

static void concurrent_handoff() {
    engine::runtime::RequestedExchange<Extraction> exchange;
    constexpr unsigned count = 20000;
    std::atomic<bool> producerDone{false};
    std::atomic<bool> valid{true};

    std::thread producer([&] {
        unsigned sequence = 1;
        while (sequence <= count) {
            if (exchange.produce([&](Extraction& payload) {
                    payload.sequence = sequence;
                    payload.entities.clear();
                    payload.entities.push_back(sequence);
                    payload.entities.push_back(sequence ^ 0xA5A5u);
                }))
                ++sequence;
            else
                std::this_thread::yield();
        }
        producerDone.store(true, std::memory_order_release);
    });

    unsigned expected = 1;
    do {
        auto handle = exchange.acquire();
        if (handle) {
            if (handle->sequence != expected || handle->entities.size() != 2 ||
                handle->entities[0] != expected || handle->entities[1] != (expected ^ 0xA5A5u))
                valid.store(false, std::memory_order_relaxed);
            ++expected;
            // Keeping each handle through validation stresses slot ownership while the
            // simulation thread continues trying to service coalesced requests.
            std::this_thread::yield();
        } else {
            exchange.request();
            std::this_thread::yield();
        }
    } while (expected <= count || !producerDone.load(std::memory_order_acquire));
    producer.join();
    require(valid.load(std::memory_order_relaxed) && expected == count + 1,
            "concurrent producer and reader exchange untorn payloads in order");
}

static void request_survives_ready_acquire_race() {
    engine::runtime::RequestedExchange<Extraction> exchange;
    constexpr unsigned count = 5000;
    std::atomic<bool> stopProducer{false};
    std::atomic<unsigned> sequence{0};

    require(exchange.request(), "race test starts with one request");
    std::thread producer([&] {
        unsigned next = 1;
        while (!stopProducer.load(std::memory_order_acquire)) {
            exchange.produce([&](Extraction& payload) { payload.sequence = next++; });
        }
    });

    bool valid = true;
    for (unsigned expected = 1; expected <= count; ++expected) {
        engine::runtime::RequestedExchange<Extraction>::ReadHandle handle;
        unsigned attempts = 0;
        while (!(handle = exchange.acquire())) {
            if (++attempts == 1000000) {
                valid = false;
                break;
            }
            std::this_thread::yield();
        }
        if (!valid)
            break;
        if (handle->sequence != expected)
            valid = false;
        sequence.store(expected, std::memory_order_relaxed);

        // The producer repeatedly takes the ready-slot fast path. The request is
        // made immediately after acquire removes the ready index, stressing the
        // boundary where a stale producer observation must not clear this request.
        exchange.request();
        handle = {};
    }

    stopProducer.store(true, std::memory_order_release);
    producer.join();
    require(valid && sequence.load(std::memory_order_relaxed) == count,
            "request racing ready removal is not erased by producer fast path");
}

int main() {
    request_and_reuse_contract();
    concurrent_handoff();
    request_survives_ready_acquire_race();
    std::puts("PASS request-driven reusable extraction exchange");
}
