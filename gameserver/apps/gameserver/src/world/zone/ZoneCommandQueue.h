#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>

// Thread-safe inbound command queue for a single zone. Producers are the
// supervisor thread (spawn/despawn/input routing) and any input path;
// the single consumer is the worker currently owning the zone, draining
// inside Zone::Tick. Commands execute under zone ownership.
namespace gs::game {

class Zone;

class ZoneCommandQueue {
public:
    using Command = std::function<void(Zone&)>;

    void Push(Command command)
    {
        std::lock_guard lock(mutex_);
        commands_.push(std::move(command));
        pushed_total_.fetch_add(1, std::memory_order_relaxed);
        // Backpressure observability (§38): high-water mark only. The local
        // path never refuses (lossless FIFO); the mark tells a future
        // balancer/transport when a zone falls behind.
        const std::size_t depth = commands_.size();
        std::size_t observed = max_depth_observed_.load(std::memory_order_relaxed);
        while (depth > observed &&
               !max_depth_observed_.compare_exchange_weak(observed,
                                                          depth,
                                                          std::memory_order_relaxed)) {
        }
    }

    bool Empty() const
    {
        std::lock_guard lock(mutex_);
        return commands_.empty();
    }

    std::size_t Depth() const
    {
        std::lock_guard lock(mutex_);
        return commands_.size();
    }

    std::size_t MaxDepthObserved() const noexcept
    {
        return max_depth_observed_.load(std::memory_order_relaxed);
    }

    std::queue<Command> TakeAll()
    {
        std::lock_guard lock(mutex_);
        std::queue<Command> taken;
        taken.swap(commands_);
        const std::size_t count = taken.size();
        drained_total_.fetch_add(count, std::memory_order_relaxed);
        std::size_t observed = max_drained_per_take_.load(std::memory_order_relaxed);
        while (count > observed &&
               !max_drained_per_take_.compare_exchange_weak(observed,
                                                            count,
                                                            std::memory_order_relaxed)) {
        }
        return taken;
    }

    // Cumulative, never reset (diagnostic resets must not touch these): the
    // tick-rate audit derives commands/sec and commands drained per tick
    // from deltas over its own window.
    std::uint64_t PushedTotal() const noexcept
    {
        return pushed_total_.load(std::memory_order_relaxed);
    }
    std::uint64_t DrainedTotal() const noexcept
    {
        return drained_total_.load(std::memory_order_relaxed);
    }
    std::size_t MaxDrainedPerTake() const noexcept
    {
        return max_drained_per_take_.load(std::memory_order_relaxed);
    }

private:
    mutable std::mutex mutex_;
    std::queue<Command> commands_;
    std::atomic<std::size_t> max_depth_observed_{0};
    std::atomic<std::uint64_t> pushed_total_{0};
    std::atomic<std::uint64_t> drained_total_{0};
    std::atomic<std::size_t> max_drained_per_take_{0};
};

} // namespace gs::game
