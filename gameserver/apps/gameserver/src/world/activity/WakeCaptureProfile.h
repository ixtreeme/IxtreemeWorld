#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace gs::game::wake_profile {
// Opt-in, fixed-size diagnostic counters. No callbacks, per-player logs or
// retained history. OFF does not perform the additional timing reads.
inline std::atomic<bool> enabled{false};
enum Counter : std::size_t {
    phases, zones, reuse_hits, reuse_misses, changed_zones, copied_sources,
    copied_bytes, query_sources, nodes, leaf_tests, positive_leaves, inserts,
    container_growths, blocked_locks, lock_wait_ns, max_lock_wait_ns,
    lock_window_ns, prep_ns, revision_ns, copy_ns, query_ns, output_ns,
    other_ns, total_ns, writer_calls, writer_wait_ns, writer_hold_ns,
    writer_max_wait_ns, writer_max_hold_ns, worker_cpu_us,
    source_capacity_bytes, frame_capacity_bytes, scratch_capacity_bytes,
    count
};
inline constexpr const char* names[count]={
    "phases","zones","reuse_hits","reuse_misses","changed_zones","copied_sources",
    "copied_bytes","query_sources","nodes","leaf_tests","positive_leaves","inserts",
    "container_growths","blocked_locks","lock_wait_ns","max_lock_wait_ns",
    "lock_window_ns","prep_ns","revision_ns","copy_ns","query_ns","output_ns",
    "other_ns","total_ns","writer_calls","writer_wait_ns","writer_hold_ns",
    "writer_max_wait_ns","writer_max_hold_ns","worker_cpu_us",
    "source_capacity_bytes","frame_capacity_bytes","scratch_capacity_bytes"};
using Values=std::array<std::uint64_t,count>;
inline std::array<std::atomic<std::uint64_t>,count> counters{};
inline std::uint64_t Now() {return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
inline bool IsMaximum(std::size_t i) {return i==max_lock_wait_ns || i==writer_max_wait_ns || i==writer_max_hold_ns;}
inline bool IsGauge(std::size_t i) {return i>=source_capacity_bytes;}
inline void Maximum(Counter i,std::uint64_t value) {
    auto old=counters[i].load(std::memory_order_relaxed);
    while(old<value && !counters[i].compare_exchange_weak(old,value,std::memory_order_relaxed)) {}
}
inline void Add(const Values& v) {
    for(std::size_t i=0;i<count;++i) {
        if(IsMaximum(i)) Maximum(static_cast<Counter>(i),v[i]);
        else if(IsGauge(i)) {if(v[phases]) counters[i].store(v[i],std::memory_order_relaxed);}
        else if(v[i]) counters[i].fetch_add(v[i],std::memory_order_relaxed);
    }
}
inline Values Snapshot() {Values v{};for(std::size_t i=0;i<count;++i)v[i]=counters[i].load(std::memory_order_relaxed);return v;}
inline void Print(const char* phase,const Values& before,const Values& after) {
    std::printf("TC4PROFILE phase=%s enabled=%d",phase,enabled.load()?1:0);
    for(std::size_t i=0;i<count;++i) std::printf(" %s=%llu",names[i],static_cast<unsigned long long>(IsMaximum(i)||IsGauge(i)?after[i]:after[i]-before[i]));
    std::printf(" maxima=lifetime gauges=last_capture capacity=container_estimate_not_heap_cpu\n");
}
class CommitLock {
public:
    explicit CommitLock(std::mutex& mutex):profiling_(enabled.load(std::memory_order_relaxed)),lock_(mutex,std::defer_lock) {
        if(profiling_) start_=Now();
        lock_.lock();
        if(profiling_) acquired_=Now();
    }
    ~CommitLock() {
        if(!profiling_) return;
        const auto end=Now();lock_.unlock();
        counters[writer_calls].fetch_add(1,std::memory_order_relaxed);
        counters[writer_wait_ns].fetch_add(acquired_-start_,std::memory_order_relaxed);
        counters[writer_hold_ns].fetch_add(end-acquired_,std::memory_order_relaxed);
        Maximum(writer_max_wait_ns,acquired_-start_);Maximum(writer_max_hold_ns,end-acquired_);
    }
private:
    bool profiling_;std::unique_lock<std::mutex> lock_;std::uint64_t start_=0,acquired_=0;
};
} // namespace gs::game::wake_profile
