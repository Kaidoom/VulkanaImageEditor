#include "imageeditor/core/BoundedParallel.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <latch>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace imageeditor::core;
int failures = 0;
void check(bool value, std::string_view message)
{
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

void executorTests()
{
    const auto capacity = boundedParallelWorkerCount();
    check(capacity >= 1 && capacity <= 8, "pool bounds participant count including the caller");
    check(boundedParallelWorkerCount(1) == 1, "explicit serial execution stays serial");
    std::array<std::thread::id, 8> first {}, second {};
    std::atomic<unsigned> visits {0};
    auto invoke = [&](auto& ids) {
        return boundedParallel(99, [&](unsigned rank, unsigned count) {
            ids[rank] = std::this_thread::get_id();
            visits.fetch_add(1, std::memory_order_relaxed);
            if (count != capacity || rank >= count) throw std::runtime_error("bad rank/count");
            if (!boundedParallel(8, [&](unsigned nestedRank, unsigned nestedCount) {
                    if (nestedRank != 0 || nestedCount != 1 || boundedParallelWorkerCount() != 1)
                        throw std::runtime_error("nested executor did not become serial");
                })) throw std::runtime_error("unexpected nested cancellation");
        });
    };
    check(invoke(first) && invoke(second), "consecutive batches complete");
    check(first == second, "worker identities are reused across batches");
    check(visits == capacity * 2, "every participant invoked once per batch");
    bool caught = false;
    try {
        (void)boundedParallel(8, [&](unsigned rank, unsigned) {
            if (rank == capacity - 1) throw std::runtime_error("intentional worker failure");
        });
    } catch (const std::runtime_error&) { caught = true; }
    check(caught, "worker exceptions return to the invoking thread");
    check(invoke(second), "executor remains usable after a failed batch");
    bool ran = false;
    check(!boundedParallel(8, [&](unsigned, unsigned) { ran = true; }, [] { return true; }) && !ran,
        "pre-cancelled work never executes");

    if (capacity > 1) {
        std::latch acquired(1), release(1);
        auto owner = std::async(std::launch::async, [&] {
            return boundedParallel(2, [&](unsigned rank, unsigned) {
                if (rank == 0) acquired.count_down();
                release.wait();
            });
        });
        acquired.wait();
        std::atomic<unsigned> unexpectedCalls {0};
        auto interactive = std::async(std::launch::async, [&] {
            return tryBoundedParallel(8, [&](unsigned, unsigned) { ++unexpectedCalls; });
        });
        const auto interactiveReady = interactive.wait_for(std::chrono::milliseconds(250));
        std::atomic<bool> cancel {false};
        auto pending = std::async(std::launch::async, [&] {
            return boundedParallel(2, [](unsigned, unsigned) { throw std::runtime_error("cancelled queued batch ran"); },
                [&] { return cancel.load(); });
        });
        cancel.store(true);
        const auto ready = pending.wait_for(std::chrono::milliseconds(250));
        release.count_down();
        check(interactiveReady == std::future_status::ready && !interactive.get() && unexpectedCalls == 0,
            "interactive work declines a busy pool without callbacks or waiting for its owner");
        check(ready == std::future_status::ready && !pending.get(), "waiting for a busy pool is promptly cancellable");
        check(owner.get(), "queued cancellation cannot interrupt the pool owner");
        check(tryBoundedParallel(8, [](unsigned, unsigned) {}), "nonblocking pool is reusable after contention");
    }
}

void wavefrontTests()
{
    constexpr int width = 73, height = 47;
    std::vector<int> pixels;
    for (int p = 0; p < width * height; ++p)
        if ((p * 17) % 23 > 3) pixels.push_back(p);
    const DiagonalWavefront schedule(pixels, width);
    check(schedule.size() == pixels.size(), "schedule retains all target pixels");
    const auto seed = std::uint64_t(0x123456789abcdefULL);
    for (bool reverse : {false, true}) {
        auto apply = [&](std::vector<std::uint64_t>& values, int p, std::size_t ordinal) {
            const int x = p % width, y = p / width, step = reverse ? 1 : -1;
            auto value = seed + std::uint64_t(ordinal) * 0x9e3779b97f4a7c15ULL;
            if (x + step >= 0 && x + step < width) value ^= values[std::size_t(p + step)] * 11;
            if (y + step >= 0 && y + step < height) value ^= values[std::size_t(p + step * width)] * 19;
            values[std::size_t(p)] = value;
        };
        std::vector<std::uint64_t> reference(width * height, 42);
        for (std::size_t ordinal = 0; ordinal < pixels.size(); ++ordinal)
            apply(reference, pixels[reverse ? pixels.size() - 1 - ordinal : ordinal], ordinal);
        for (const unsigned workers : {1U, 2U, 4U, 8U}) {
            std::vector<std::uint64_t> actual(width * height, 42);
            check(schedule.run(workers, reverse, [&](int p, std::size_t ordinal, unsigned) { apply(actual, p, ordinal); }),
                "wavefront completes at each bounded worker count");
            check(actual == reference, "sparse diagonal execution preserves serial dependency and RNG-ordinal results exactly");
        }
    }
    bool caught = false;
    try {
        (void)schedule.run(8, false, [](int, std::size_t ordinal, unsigned) {
            if (ordinal == 100) throw std::runtime_error("intentional diagonal failure");
        });
    } catch (const std::runtime_error&) { caught = true; }
    check(caught, "wavefront exception drains all participants without a stuck barrier");
    std::atomic<unsigned> visits {0};
    unsigned polls = 0;
    check(!schedule.run(8, false, [&](int, std::size_t, unsigned) { visits.fetch_add(1, std::memory_order_relaxed); },
              [&] { return ++polls >= 12; }),
        "wavefront cancellation stops at a completed diagonal");
    check(visits > 0 && visits < pixels.size(), "cancellation does not process the remaining schedule");
    check(schedule.run(8, false, [](int, std::size_t, unsigned) {}), "executor is reusable after a cancelled wavefront");
    bool bad = false;
    try { const std::array duplicates {2, 2}; const DiagonalWavefront invalid(duplicates, 4); }
    catch (const std::invalid_argument&) { bad = true; }
    check(bad, "duplicate target IDs cannot cause concurrent writes");
    check(DiagonalWavefront().run(8, false, [](int, std::size_t, unsigned) {}), "empty schedule succeeds without work");
}
} // namespace

int main()
{
    executorTests();
    wavefrontTests();
    if (failures) return 1;
    std::cout << "Bounded executor and exact diagonal dependency tests passed\n";
}
