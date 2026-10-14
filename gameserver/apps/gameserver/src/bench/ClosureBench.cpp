#include "ClosureBench.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "../world/zone/ZoneManager.h"
#include "../world/zone/ZoneOwnership.h"
#include "../world/zone/ZoneScheduler.h"
#include "../world/zone/ZoneWorkerPool.h"

namespace gs::bench {
namespace {
using namespace gs::game;
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
double Us(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double,std::micro>(b-a).count();
}
std::uint64_t Cpu100ns() {
#ifdef _WIN32
    FILETIME c,e,k,u;
    if (GetThreadTimes(GetCurrentThread(),&c,&e,&k,&u)) {
        return (static_cast<std::uint64_t>(k.dwHighDateTime)<<32)+k.dwLowDateTime+
               (static_cast<std::uint64_t>(u.dwHighDateTime)<<32)+u.dwLowDateTime;
    }
#endif
    return 0;
}
std::uint64_t Work(std::uint64_t x,std::uint64_t iterations) {
    for (std::uint64_t i=0;i<iterations;++i) {
        x^=x>>12; x^=x<<25; x^=x>>27; x*=2685821657736338717ULL;
    }
    return x;
}
}

// Version 1. An explicit finite workload through the unchanged production
// ScheduleOnce -> FIFO pool -> ZoneWriteGuard path. No gameplay simulation,
// no artificial sleep in the performance window, no assumption that every
// existing zone is eligible. Stable zone IDs are incarnation IDs (not reused).
int RunSchedulerContract(std::size_t requested,bool fixed_work,std::uint64_t iterations) {
    if ((requested!=1 && requested!=2 && requested!=4) || !iterations || iterations>100000000) return 2;
    const std::size_t count=fixed_work?64:8;
    ZoneManager zones;
    PartitionLayout layout; layout.regions_x=1; layout.regions_y=1;
    layout.leaves_x=static_cast<std::uint32_t>(count); layout.leaves_y=1;
    InitialPartition initial; std::string error;
    if (!BuildInitialPartition({0,0,float(count*1000),1000},layout,240,initial,error)) return 2;
    zones.BuildInitialPartition(initial);
    struct Record {
        ZoneId id=0; std::uint64_t calls=0,output=0,cpu=0;
        Clock::time_point due{},enqueue{},start{},end{}; std::thread::id worker;
    };
    std::vector<Record> records(count);
    // Expected results are evaluated outside the measured window.
    std::vector<std::uint64_t> expected(count);
    for (std::size_t i=0;i<count;++i) {
        expected[i]=Work(i+1,fixed_work?iterations:1);
        zones.GetZone(i).Diagnostics().mob_count=1;
        zones.GetZone(i).Diagnostics().lod_full=1;
    }
    std::mutex mutex; std::condition_variable cv;
    std::size_t entered=0,completed=0,active=0,overlap=0;
    bool release=fixed_work,watchdog=false; std::atomic<bool> guard_ok{true};
    ZoneWorkerPool pool([&](std::size_t i) {
        auto& zone=zones.GetZone(i); auto& r=records[i];
        {
            ZoneWriteGuard guard(zone,"schedulercontract finite work");
            if (!zone.TickInProgress().load() || !zone.SimulationEnabled() || r.id!=zone.Id()) guard_ok=false;
            r.worker=std::this_thread::get_id(); r.start=Clock::now(); ++r.calls;
            {
                std::unique_lock lock(mutex); ++entered; ++active; overlap=std::max(overlap,active); cv.notify_all();
                if (!cv.wait_for(lock,5s,[&]{return release;})) watchdog=true;
            }
            const auto cpu=Cpu100ns(); r.output=Work(i+1,fixed_work?iterations:1); r.cpu=Cpu100ns()-cpu;
            r.end=Clock::now();
            {std::lock_guard lock(mutex); --active;}
        }
        zone.TickInProgress().store(false,std::memory_order_release);
        {std::lock_guard lock(mutex); ++completed; cv.notify_all();}
    });
    pool.Start(requested);
    ZoneScheduler scheduler;
    const auto start=Clock::now();
    for (std::size_t i=0;i<count;++i) {
        auto& zone=zones.GetZone(i); zone.NextTick()=start;
        records[i].id=zone.Id(); records[i].due=start;
    }
    scheduler.ScheduleOnce(zones,pool,start,{},500,[&](const std::vector<std::size_t>& indices){
        for (auto i:indices) records[i].enqueue=Clock::now();
    });
    int failures=0;
    auto check=[&](const char* label,bool ok){std::printf("SCHEDCONTRACT %s: %s\n",label,ok?"PASS":"FAIL"); failures+=!ok;};
    if (!fixed_work) {
        std::unique_lock lock(mutex);
        const bool arrived=cv.wait_for(lock,5s,[&]{return entered>=requested;});
        check("production-dispatch-overlap",arrived && overlap==requested);
        lock.unlock();
        // All claims remain held. A second due wave must not duplicate them;
        // this exercises the actual scheduler CAS, not a model of it.
        scheduler.ScheduleOnce(zones,pool,start+50ms,{},500);
        check("second-wave-guard-rejects-duplicates",scheduler.GetCounters().enqueued==count && scheduler.GetCounters().cas_failures==count);
        bool claimed=true;
        for (std::size_t i=0;i<count;++i) claimed &= zones.GetZone(i).TickInProgress().load();
        check("claims-retained-while-queued",claimed);
        check("runtime-drain-gate-sees-live-work",zones.AnyTickInProgress());
        lock.lock(); release=true; lock.unlock(); cv.notify_all();
    }
    {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock,30s,[&]{return completed==count;})) {watchdog=true; release=true; cv.notify_all();}
    }
    // Production runtime drains TickInProgress before stopping the pool.
    // Stop joins active workers; its standalone cancellation semantics are
    // intentionally not replaced with a fabricated queue-drain guarantee.
    pool.Stop();
    const auto capture=Clock::now();
    auto end=start; bool exactly_once=true,outputs=true; std::uint64_t cpu=0,checksum=0,misses=0;
    std::map<std::thread::id,std::size_t> worker_counts;
    double min_us=1e100,max_us=0,sum_us=0,queue_us=0,due_enqueue_us=0;
    for (std::size_t i=0;i<count;++i) {
        auto& r=records[i]; end=std::max(end,r.end); cpu+=r.cpu; checksum^=r.output;
        exactly_once &= r.calls==1 && !zones.GetZone(i).TickInProgress().load(); outputs &= r.output==expected[i];
        ++worker_counts[r.worker]; const auto elapsed=Us(r.start,r.end);
        min_us=std::min(min_us,elapsed); max_us=std::max(max_us,elapsed); sum_us+=elapsed;
        queue_us+=Us(r.enqueue,r.start); due_enqueue_us+=Us(r.due,r.enqueue); misses+=r.end>r.due+50ms;
        std::printf("SCHEDTASK id=%u due_us=%.3f before_enqueue_hook_us=%.3f callback_start_us=%.3f callback_end_us=%.3f calls=%llu output=%llu cpu_100ns=%llu\n",
            r.id,Us(start,r.due),Us(start,r.enqueue),Us(start,r.start),Us(start,r.end),r.calls,r.output,r.cpu);
    }
    check("exactly-once-and-drained",exactly_once && pool.GetUtilization().tasks_completed==count && !watchdog);
    check("guard-incarnation-and-output",guard_ok && outputs);
    const auto delays=pool.GetDelays();
    check("production-delay-population",delays.samples==count);
    // After drain the unchanged sleep eligibility excludes every quiet zone.
    for(std::size_t i=0;i<count;++i) zones.GetZone(i).Diagnostics().mob_count=0;
    scheduler.ScheduleOnce(zones,pool,end+50ms,{},500);
    check("sleep-does-not-enqueue",!zones.AnyTickInProgress());
    if (!fixed_work) {
        // Empty topology transactions use the same manager commit/reclaim
        // gates as production, after the scheduler's issued work has drained.
        // Old stable IDs must never name the new incarnation of a reused slot.
        const auto original_id=zones.GetZone(0).Id();
        ZoneManager::SplitPlan split; std::vector<ZoneId> children;
        bool split_ok=zones.PlanSplit(original_id,split) && zones.CreateStagedSplit(split,children) && zones.CommitSplit(original_id,children);
        check("split-after-drain",split_ok);
        ZoneManager::MergePlan merge; ZoneId merged=0;
        bool merge_ok=split_ok && zones.PlanMerge(original_id,merge) && zones.CreateStagedMergeTarget(merge,merged) && zones.CommitMerge(merge,merged);
        check("merge-after-drain",merge_ok && merged!=original_id);
        zones.ReclaimRetiredSlots(10,[](std::size_t){return false;});
        zones.ReclaimRetiredSlots(13,[](std::size_t){return false;});
        ZoneManager::SplitPlan again; std::vector<ZoneId> reused;
        const bool reused_ok=merge_ok && zones.PlanSplit(merged,again) && zones.CreateStagedSplit(again,reused) && zones.CommitSplit(merged,reused);
        bool distinct=reused_ok;
        for (auto id:reused) distinct &= id!=original_id && std::find(children.begin(),children.end(),id)==children.end();
        check("slot-reuse-keeps-distinct-incarnation",distinct);
        const auto before=scheduler.GetCounters().enqueued;
        // Even stale high gauges on retired slots cannot make them eligible.
        for(std::size_t i=0;i<zones.ZoneCount();++i) if(!zones.GetZone(i).SimulationEnabled()) {
            zones.GetZone(i).Diagnostics().mob_count=1; zones.GetZone(i).Diagnostics().lod_full=1;
        }
        scheduler.ScheduleOnce(zones,pool,end+100ms,{},500);
        check("retired-not-dispatched",scheduler.GetCounters().enqueued==before && !zones.AnyTickInProgress());
    }
    const double wall=Us(start,end);
    std::printf("SCHEDWORK version=1 workers=%zu tasks=%zu iterations=%llu total_iterations=%llu checksum=%llu window_us=%.3f capture_us=%.3f throughput_iterations_s=%.3f task_elapsed_min_us=%.3f avg_us=%.3f max_us=%.3f sum_us=%.3f worker_cpu_us=%.3f participating=%zu overlap=%zu due_enqueue_us=%llu queue_us=%llu due_start_us=%llu deadline_misses=%llu pending_start=0 pending_end=0 overflow=0 mode=%s\n",
        requested,count,iterations,count*(fixed_work?iterations:1),checksum,wall,Us(start,capture),count*(fixed_work?iterations:1)*1e6/wall,
        min_us,sum_us/count,max_us,sum_us,cpu/10.0,worker_counts.size(),overlap,delays.due_to_enqueue_us,delays.queue_us,delays.due_to_start_us,delays.finish_deadline_misses,fixed_work?"fixed-work":"gate-correctness");
    std::size_t worker=0;
    for (auto [id,tasks]:worker_counts) std::printf("SCHEDWORKER ordinal=%zu tasks=%zu\n",worker++,tasks);
#ifndef _WIN32
    std::printf("SCHEDWORK CPU: NOT MEASURED\n");
#endif
    return failures?2:0;
}
}
