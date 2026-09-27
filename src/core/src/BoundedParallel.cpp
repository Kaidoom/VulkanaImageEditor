#include "imageeditor/core/BoundedParallel.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace imageeditor::core {
namespace {

thread_local unsigned parallelDepth = 0;
struct DepthGuard {
    DepthGuard() { ++parallelDepth; }
    ~DepthGuard() { --parallelDepth; }
};

class Executor {
public:
    Executor()
    {
        const auto desired = std::clamp(std::thread::hardware_concurrency(), 1U, 8U);
        threads_.reserve(desired - 1);
        for (unsigned rank = 1; rank < desired; ++rank) {
            try {
                threads_.emplace_back([this, rank] { worker(rank); });
            } catch (const std::system_error&) {
                // Resource-constrained hosts retain a working smaller pool.
                break;
            } catch (const std::bad_alloc&) {
                // Already started workers must remain owned; unwinding a
                // partially constructed pool would strand their parked waits.
                break;
            }
        }
    }

    ~Executor()
    {
        {
            std::lock_guard lock(stateMutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        // Join before destroying the mutexes and condition variables.
        threads_.clear();
    }

    unsigned count(unsigned requested) const
    {
        const auto capacity = static_cast<unsigned>(threads_.size()) + 1;
        return requested ? std::clamp(requested, 1U, capacity) : capacity;
    }

    bool run(unsigned count, const std::function<void(unsigned, unsigned)>& function,
        const std::function<bool()>& cancelled, bool waitForPool = true)
    {
        // Separate ownership from worker state: no extra worker sets under
        // concurrent callers, and cancellation does not wait for another solve.
        {
            std::unique_lock lock(batchMutex_, std::defer_lock);
            if (waitForPool) lock.lock();
            else if (!lock.try_lock()) return false;
            if (busy_ && !waitForPool) return false;
            while (busy_) {
                lock.unlock();
                if (cancelled && cancelled()) return false;
                lock.lock();
                if (busy_) available_.wait_for(lock, std::chrono::milliseconds(5));
            }
            if (cancelled && cancelled()) return false;
            busy_ = true;
        }
        struct Release {
            Executor* self;
            ~Release()
            {
                { std::lock_guard lock(self->batchMutex_); self->busy_ = false; }
                self->available_.notify_all();
            }
        } release {this};
        {
            std::lock_guard lock(stateMutex_);
            function_ = &function;
            participants_ = count;
            remaining_ = count - 1;
            error_ = {};
            ++generation_;
        }
        wake_.notify_all();
        invoke(0, count, function);
        std::exception_ptr error;
        {
            std::unique_lock lock(stateMutex_);
            completed_.wait(lock, [&] { return remaining_ == 0; });
            error = error_;
            function_ = nullptr;
        }
        if (error) std::rethrow_exception(error);
        return true;
    }

private:
    void invoke(unsigned rank, unsigned participants,
        const std::function<void(unsigned, unsigned)>& function) noexcept
    {
        try {
            DepthGuard guard;
            function(rank, participants);
        } catch (...) {
            std::lock_guard lock(stateMutex_);
            if (!error_) error_ = std::current_exception();
        }
    }

    void worker(unsigned rank)
    {
        std::uint64_t seen = 0;
        for (;;) {
            const std::function<void(unsigned, unsigned)>* function;
            unsigned participants;
            {
                std::unique_lock lock(stateMutex_);
                wake_.wait(lock, [&] { return stopping_ || generation_ != seen; });
                if (stopping_) return;
                seen = generation_;
                participants = participants_;
                if (rank >= participants) continue;
                function = function_;
            }
            invoke(rank, participants, *function);
            {
                std::lock_guard lock(stateMutex_);
                --remaining_;
                if (!remaining_) completed_.notify_one();
            }
        }
    }

    std::vector<std::jthread> threads_;
    std::mutex batchMutex_, stateMutex_;
    std::condition_variable available_, wake_, completed_;
    bool busy_ {false}, stopping_ {false};
    std::uint64_t generation_ {0};
    unsigned participants_ {1}, remaining_ {0};
    const std::function<void(unsigned, unsigned)>* function_ {nullptr};
    std::exception_ptr error_;
};

Executor& executor()
{
    static Executor value;
    return value;
}
} // namespace

unsigned boundedParallelWorkerCount(unsigned requested)
{
    // Avoid starting the pool at all for explicitly serial work/nested batches.
    return parallelDepth || requested == 1 ? 1 : executor().count(requested);
}

bool boundedParallel(unsigned workers,
    const std::function<void(unsigned, unsigned)>& function,
    const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled()) return false;
    const unsigned count = boundedParallelWorkerCount(workers);
    if (count == 1) {
        DepthGuard guard;
        function(0, 1);
        return true;
    }
    return executor().run(count, function, cancelled);
}

bool tryBoundedParallel(unsigned workers,
    const std::function<void(unsigned, unsigned)>& function)
{
    const unsigned count = boundedParallelWorkerCount(workers);
    if (count == 1) {
        DepthGuard guard;
        function(0, 1);
        return true;
    }
    return executor().run(count, function, {}, false);
}

DiagonalWavefront::DiagonalWavefront(std::span<const int> pixels, int width)
{
    if (width <= 0) throw std::invalid_argument("Wavefront width must be positive");
    if (pixels.empty()) return;
    int previous = -1;
    std::size_t first = std::numeric_limits<std::size_t>::max(), last = 0;
    for (const int pixel : pixels) {
        if (pixel <= previous) throw std::invalid_argument("Wavefront pixels must be unique and in row-major order");
        previous = pixel;
        const auto diagonal = std::size_t(pixel / width) + std::size_t(pixel % width);
        first = std::min(first, diagonal);
        last = std::max(last, diagonal);
    }
    offsets_.resize(last - first + 2);
    for (const int pixel : pixels)
        ++offsets_[std::size_t(pixel / width) + std::size_t(pixel % width) - first + 1];
    for (std::size_t i = 1; i < offsets_.size(); ++i) offsets_[i] += offsets_[i - 1];
    auto cursor = offsets_;
    entries_.resize(pixels.size());
    for (std::size_t ordinal = 0; ordinal < pixels.size(); ++ordinal) {
        const int pixel = pixels[ordinal];
        const auto diagonal = std::size_t(pixel / width) + std::size_t(pixel % width) - first;
        entries_[cursor[diagonal]++] = {pixel, ordinal};
    }
}

bool DiagonalWavefront::run(unsigned workers, bool reverse,
    const std::function<void(int, std::size_t, unsigned)>& function,
    const std::function<bool()>& cancelled) const
{
    if (entries_.empty()) return !(cancelled && cancelled());
    const unsigned count = boundedParallelWorkerCount(workers);
    std::atomic<bool> stopped {false};
    bool waveStopped = false;
    auto finishWave = [&]() noexcept { waveStopped = stopped.load(std::memory_order_relaxed); };
    std::barrier barrier(static_cast<std::ptrdiff_t>(count), finishWave);
    std::exception_ptr error;
    std::mutex errorMutex;
    const bool started = boundedParallel(count, [&](unsigned rank, unsigned participants) {
        for (std::size_t ordinal = 0; ordinal < diagonalCount(); ++ordinal) {
            const auto diagonal = reverse ? diagonalCount() - 1 - ordinal : ordinal;
            try {
                if (!stopped.load(std::memory_order_relaxed)) {
                    // Only rank zero calls arbitrary cancellation callbacks;
                    // clients need not make their cancellation functor reentrant.
                    if (rank == 0 && cancelled && cancelled()) stopped.store(true, std::memory_order_relaxed);
                    for (auto i = offsets_[diagonal] + rank; i < offsets_[diagonal + 1]; i += participants) {
                        if (stopped.load(std::memory_order_relaxed)) break;
                        const auto& entry = entries_[i];
                        function(entry.pixel, reverse ? size() - 1 - entry.ordinal : entry.ordinal, rank);
                    }
                }
            } catch (...) {
                { std::lock_guard lock(errorMutex); if (!error) error = std::current_exception(); }
                stopped.store(true, std::memory_order_relaxed);
            }
            barrier.arrive_and_wait();
            if (waveStopped) break;
        }
    }, cancelled);
    if (error) std::rethrow_exception(error);
    return started && !stopped.load(std::memory_order_relaxed);
}

} // namespace imageeditor::core
