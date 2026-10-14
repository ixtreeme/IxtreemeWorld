#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <condition_variable>

namespace gs::game {

// Operation classes, not entity classes. A mob collision is Active too.
enum class TerrainPriority : std::uint8_t { Admission, Active, Prefetch };
enum class TerrainRequestStatus : std::uint8_t {
    Pending, Ready, Consumed, Cancelled, TimedOut, CapacityRejected, InvalidData, OutsideWorld
};

// A world-owned request entry retains this small state until its next pass,
// detecting the last consumer drop by shared ownership count. Consume then
// drop still preserves the acknowledgement. No entity/session is retained;
// cancelling one consumer does not cancel another for the same chunk.
struct TerrainRequest {
    using Clock = std::chrono::steady_clock;
    explicit TerrainRequest(Clock::time_point now, double timeout_seconds = 5.0)
        : id(next_id.fetch_add(1, std::memory_order_relaxed) + 1), started(now),
          deadline(now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timeout_seconds))) {}
    const std::uint64_t id;
    const Clock::time_point started;
    const Clock::time_point deadline;
    std::atomic<TerrainRequestStatus> status{TerrainRequestStatus::Pending};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> consumed{false};
    // Monotonic microseconds since request creation; zero means not observed.
    // Timestamp publication precedes status/acknowledgement publication.
    std::atomic<std::uint64_t> registered_us{0}, ready_us{0}, consumed_us{0}, effect_us{0}, terminal_us{0};
    std::atomic<TerrainPriority> priority{TerrainPriority::Admission};
    std::uint64_t ElapsedUs() const noexcept {
        return 1 + static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-started).count());
    }
    void MarkOnce(std::atomic<std::uint64_t>& timestamp) noexcept {
        std::uint64_t zero=0;
        timestamp.compare_exchange_strong(zero,ElapsedUs(),std::memory_order_release);
    }
    void SetStatus(TerrainRequestStatus value) noexcept {
        std::unique_lock lock(wait_mutex_);
        if(value==TerrainRequestStatus::Ready) MarkOnce(ready_us);
        else if(value!=TerrainRequestStatus::Pending) MarkOnce(terminal_us);
        status.store(value,std::memory_order_release);
        lock.unlock();
        wait_cv_.notify_all();
    }
    // The request owns both synchronization objects. Its existing deadline is
    // never extended; timeout here does not mutate the world's request state.
    TerrainRequestStatus WaitUntilReadyOrTerminal() const {
        return WaitImpl([]{});
    }
    void Effect() noexcept { MarkOnce(effect_us); }
    TerrainRequestStatus Status() const noexcept { return status.load(std::memory_order_acquire); }
    void Cancel() noexcept { cancelled.store(true, std::memory_order_release); }
    void Consume() noexcept { MarkOnce(consumed_us); consumed.store(true, std::memory_order_release); }
private:
    friend struct TerrainRequestWaitTestAccess;
    template<class OnPending> TerrainRequestStatus WaitImpl(OnPending on_pending) const {
        std::unique_lock lock(wait_mutex_);
        wait_cv_.wait_until(lock,deadline,[&]{
            const bool done=Status()!=TerrainRequestStatus::Pending;
            if(!done) on_pending(); // Empty/inlined in the production call.
            return done;
        });
        return Status();
    }
    mutable std::mutex wait_mutex_;
    mutable std::condition_variable wait_cv_;
    inline static std::atomic<std::uint64_t> next_id{0};
};
using TerrainRequestHandle = std::shared_ptr<TerrainRequest>;

} // namespace gs::game
