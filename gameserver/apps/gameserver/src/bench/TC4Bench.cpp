#include "ClosureBench.h"
#include <cstdio>
#include <future>
#include <thread>
#include <latch>
#include <boost/asio/io_context.hpp>
#include "../world/terrain/TerrainRequest.h"
#include "../world/zone/ZoneManager.h"
#include "../world/zone/ZoneOwnership.h"
#include "../world/zone/ZoneScheduler.h"
#include "../world/zone/ZoneWorkerPool.h"
#include "../world/spawn/SpawnSystem.h"
#include "../world/spawn/SpawnLoader.h"
#include "../world/spawn/MobPrototypeRegistry.h"
#include "../world/debug/WorldValidator.h"
#include "../world/systems/MovementSystem.h"
#include "../world/systems/LodSystem.h"
#include "../world/terrain/TerrainService.h"

namespace gs::game {
struct TerrainRequestWaitTestAccess {
    static auto Lock(TerrainRequest& r) { return std::unique_lock(r.wait_mutex_); }
    static void Spurious(TerrainRequest& r) { r.wait_cv_.notify_all(); }
    static auto AtPending(TerrainRequest& r,std::latch& checked,std::latch& release) {
        bool first=true;
        return r.WaitImpl([&]{if(first){first=false;checked.count_down();release.wait();}});
    }
};
}
namespace gs::bench {
int RunTerrainWaitTests() {
    using namespace gs::game;using namespace std::chrono_literals;
    using S=TerrainRequestStatus;using Clock=TerrainRequest::Clock;
    int failures=0;
    const auto check=[&](const char* n,bool ok){std::printf("TERRAINWAIT %s: %s\n",n,ok?"PASS":"FAIL");failures+=!ok;};
    TerrainRequest ready(Clock::now(),1);ready.SetStatus(S::Ready);
    check("ready-before-wait",ready.WaitUntilReadyOrTerminal()==S::Ready);
    ready.Consume();const auto ack=ready.consumed_us.load();ready.Consume();
    check("double-consume-idempotent",ready.consumed.load() && ready.consumed_us.load()==ack);
    for(auto status:{S::Consumed,S::Cancelled,S::TimedOut,S::CapacityRejected,S::InvalidData,S::OutsideWorld}) {
        TerrainRequest r(Clock::now(),1);r.SetStatus(status);
        check("terminal-before-wait",r.WaitUntilReadyOrTerminal()==status);
    }
    bool races=true;
    for(int i=0;i<64;++i) {
        TerrainRequest r(Clock::now(),1);
        // Hold the same predicate mutex while both waiter and producer start.
        // Both acquisition orders must see publication; no timed polling.
        auto gate=TerrainRequestWaitTestAccess::Lock(r);std::latch started(2);
        auto waiter=std::async(std::launch::async,[&]{started.count_down();return r.WaitUntilReadyOrTerminal();});
        std::thread producer([&]{started.count_down();r.SetStatus(S::Ready);});
        started.wait();gate.unlock();producer.join();races=races && waiter.get()==S::Ready;
    }
    check("publication-wait-enrollment-races",races);
    TerrainRequest gap(Clock::now(),1);std::latch checked(1),release(1),setter_entered(1);
    auto gap_wait=std::async(std::launch::async,[&]{return TerrainRequestWaitTestAccess::AtPending(gap,checked,release);});
    checked.wait();
    std::thread gap_setter([&]{setter_entered.count_down();gap.SetStatus(S::Ready);});
    setter_entered.wait();release.count_down();gap_setter.join();
    check("ready-between-predicate-and-sleep-no-lost-wake",gap_wait.get()==S::Ready && Clock::now()<gap.deadline);
    TerrainRequest withheld(Clock::now(),0.03);
    auto waiter=std::async(std::launch::async,[&]{return withheld.WaitUntilReadyOrTerminal();});
    TerrainRequestWaitTestAccess::Spurious(withheld);
    check("spurious-no-early-success",waiter.wait_for(1ms)==std::future_status::timeout);
    check("negative-withheld-completion-deadline",waiter.get()==S::Pending && Clock::now()>=withheld.deadline && !withheld.consumed.load());
    TerrainRequest silent(Clock::now(),0.03);std::latch silent_checked(1),silent_release(1);
    auto silent_wait=std::async(std::launch::async,[&]{return TerrainRequestWaitTestAccess::AtPending(silent,silent_checked,silent_release);});
    silent_checked.wait();silent_release.count_down();
    {auto lock=TerrainRequestWaitTestAccess::Lock(silent);silent.status.store(S::Ready,std::memory_order_release);}
    const auto silent_result=silent_wait.get();
    check("negative-withheld-notification-deadline-detected",silent_result==S::Ready && Clock::now()>=silent.deadline);
    TerrainRequest cancelled(Clock::now(),1);cancelled.Cancel();cancelled.SetStatus(S::Cancelled);
    check("owner-cancel-terminal-wakes",cancelled.WaitUntilReadyOrTerminal()==S::Cancelled && !cancelled.consumed.load());
    auto request=std::make_shared<TerrainRequest>(Clock::now(),1);std::weak_ptr<TerrainRequest> weak=request;
    std::promise<void> entered;
    auto retained=std::async(std::launch::async,[held=request,&entered]{entered.set_value();return held->WaitUntilReadyOrTerminal();});
    entered.get_future().wait();request.reset();auto owner=weak.lock();
    const bool alive=bool(owner);if(owner)owner->SetStatus(S::Cancelled);owner.reset();
    check("drop-consumer-wait-lifetime",alive && retained.get()==S::Cancelled && weak.expired());
    return failures?2:0;
}

int RunTC4TemporalTests() {
    using namespace gs::game;using namespace std::chrono_literals;
    using Clock=std::chrono::steady_clock;int failures=0;
    const auto check=[&](const char* n,bool ok){std::printf("TC4TEMPORAL %s: %s\n",n,ok?"PASS":"FAIL");failures+=!ok;};
    boost::asio::io_context io;
    ZoneManager zones;PartitionLayout layout;layout.regions_x=1;layout.regions_y=1;layout.leaves_x=2;
    InitialPartition initial;std::string error;
    if(!BuildInitialPartition({0,0,4000,2000},layout,240,initial,error))return 2;
    zones.BuildInitialPartition(initial);auto& a=zones.GetZone(0);auto& b=zones.GetZone(1);
    SpatialActivityField field({500,{0,0,4000,2000}});
    auto empty_grid=field.Rebuild(zones,{150,500,1500},true);
    auto session=std::make_shared<gs::network::Session>(boost::asio::ip::tcp::socket(io),1);
    {ZoneWriteGuard guard(a,"TC4 spawn");SpawnSystem::SpawnPlayer(a,session,{},Position{1500,1000,0},42);}
    ZoneScheduler scheduler;scheduler.SetLodEnabled(true);ZoneWorkerPool pool([](std::size_t){});
    const auto now=Clock::now();a.NextTick()=b.NextTick()=now+50ms;
    auto first=scheduler.CaptureWakeInput(zones,500);
    check("exact-radius-empty-target-in-geometric-set",first->influenced.contains(b.Id()));
    scheduler.ScheduleOnce(zones,pool,now,{},500);
    check("empty-target-still-sleeps",b.Activity()==ZoneActivity::Sleeping);
    auto terrain=TerrainService::Flat({0,0,4000,2000});mx::map::WorldLogic logic;MobPrototypeRegistry types;
    logic.warps.push_back({1,{1499,999,1501,1001},1700,1000});
    ZoneTickContext ctx{terrain,logic,types,zones};
    {ZoneWriteGuard guard(a,"TC4 real warp");MovementSystem::Step(a,0.05f,ctx);}
    check("real-warp-authoritative-publication",a.FindEntity(42).get<Position>().x==1700 && a.ActivitySources().front().x==1700);
    auto warped=scheduler.CaptureWakeInput(zones,500);
    {
        ZoneWriteGuard guard(b,"TC4 late resident");SpawnSystem::SpawnMob(b,{},0,MobTypeDefinition{},Position{2100,1000,0},9001);
        // Settle against the retained empty generation using the real evaluator.
        // Fixture-only zero grace; production hysteresis remains unchanged.
        LodConfig fixture_lod;fixture_lod.demote_full_sec=fixture_lod.demote_reduced_sec=fixture_lod.demote_low_sec=0;
        ctx.lod=&fixture_lod;ctx.activity=empty_grid;
        LodSystem::Evaluate(b,ctx);LodSystem::Evaluate(b,ctx);LodSystem::Evaluate(b,ctx);ctx.lod=nullptr;
    }
    auto unchanged=scheduler.CaptureWakeInput(zones,500);
    check("mob-eligibility-does-not-invalidate-geometric-input",warped==unchanged);
    scheduler.ScheduleOnce(zones,pool,now,{},500);
    check("unchanged-input-new-mob-wakes-without-early-tick",b.Activity()==ZoneActivity::Active && b.NextTick()==now+50ms && !zones.AnyTickInProgress());
    auto broken=std::make_shared<ActivityWakeFrame>(*unchanged);broken->influenced.erase(b.Id());
    b.SetActivity(ZoneActivity::Sleeping);scheduler.ScheduleOnce(zones,pool,now,{},500,{},broken);
    check("negative-missing-positive-leaf-detected",!ValidateActivityWake(zones,scheduler.WakeDecision().get(),error));
    scheduler.ScheduleOnce(zones,pool,now,{},500);
    // Publication primitive concurrency: capture holds A while waiting for B.
    // B1 publishes before the common cut; A1 can publish only after it.
    auto b_lock=std::unique_lock(b.ActivityMutex());
    auto capture=std::async(std::launch::async,[&]{return scheduler.CaptureWakeInput(zones,500);});
    bool a_held=false;const auto deadline=Clock::now()+2s;
    while(Clock::now()<deadline) {
        if(a.ActivityMutex().try_lock()) {a.ActivityMutex().unlock();std::this_thread::yield();}
        else {a_held=true;break;}
    }
    b.UpsertActivitySourceLocked(43,Position{3000,1000,0});
    auto producer=std::async(std::launch::async,[&]{std::lock_guard lock(a.ActivityMutex());a.UpsertActivitySourceLocked(42,Position{1600,1000,0});});
    b_lock.unlock();auto cut=capture.get();producer.get();
    const auto source_x=[](const auto& f,std::uint32_t id){for(const auto& p:f->sources)if(p.net_id==id)return p.x;return -1.f;};
    check("two-zone-coherent-cut",a_held && source_x(cut,42)==1700 && source_x(cut,43)==3000);
    auto next=scheduler.CaptureWakeInput(zones,500);
    check("post-cut-publication-next-frame",source_x(next,42)==1600 && source_x(next,43)==3000 && next->generation>cut->generation);
    check("negative-incoherent-pair-excluded",!(source_x(cut,42)==1600 && source_x(cut,43)!=3000));
    check("retained-frame-is-immutable",source_x(first,42)==1500 && first->sources.size()==1);
    {
        ZoneManager topology;topology.BuildInitialPartition(initial);ZoneScheduler capture_scheduler;
        auto& source=topology.GetZone(0);const auto parent=topology.GetZone(1).Id();
        {std::lock_guard lock(source.ActivityMutex());source.UpsertActivitySourceLocked(1,Position{1700,1000,0});}
        const auto revision=source.ActivityRevisionLocked();
        auto before=capture_scheduler.CaptureWakeInput(topology,500);
        ZoneManager::SplitPlan split;std::vector<ZoneId> children;
        bool split_ok=topology.PlanSplit(parent,split) && topology.CreateStagedSplit(split,children) && topology.CommitSplit(parent,children);
        auto divided=capture_scheduler.CaptureWakeInput(topology,500);
        bool geometric=true;
        for(const auto* leaf:topology.GetActiveLeaves()) {
            const auto& bounds=leaf->bounds;
            const float dx=std::max({bounds.min_x-1700.f,0.f,1700.f-bounds.max_x});
            const float dy=std::max({bounds.min_y-1000.f,0.f,1000.f-bounds.max_y});
            geometric &= divided->influenced.contains(leaf->zone_id)==(dx*dx+dy*dy<=250000.f);
        }
        check("unchanged-source-split-invalidates-topology",split_ok && geometric && !divided->leaves.contains(parent) && divided!=before && source.ActivityRevisionLocked()==revision);
        ZoneManager::MergePlan merge;ZoneId merged=0;
        bool merge_ok=topology.PlanMerge(parent,merge) && topology.CreateStagedMergeTarget(merge,merged) && topology.CommitMerge(merge,merged);
        auto combined=capture_scheduler.CaptureWakeInput(topology,500);
        bool children_gone=true;for(auto child:children)children_gone &= !combined->leaves.contains(child);
        check("unchanged-source-merge-new-incarnation",merge_ok && children_gone && combined->influenced.contains(merged) && combined!=divided && source.ActivityRevisionLocked()==revision);
        check("negative-reused-pre-topology-frame-detected",before->leaves!=combined->leaves && before->influenced!=combined->influenced);
    }
    return failures?2:0;
}
}
