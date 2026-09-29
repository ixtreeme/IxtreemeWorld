#include "ClosureBench.h"
#include <cstdio>
#include <limits>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <boost/asio/io_context.hpp>
#include "../world/zone/ZoneManager.h"
#include "../world/zone/ZoneOwnership.h"
#include "../world/zone/ZoneScheduler.h"
#include "../world/zone/ZoneWorkerPool.h"
#include "../world/spawn/SpawnSystem.h"
#include "../world/spawn/SpawnLoader.h"
#include "../world/spawn/MobPrototypeRegistry.h"
#include "../world/systems/MovementSystem.h"
#include "../world/systems/LodSystem.h"
#include "../world/components/MovementComponents.h"
#include "../world/debug/WorldValidator.h"
#include "../world/terrain/TerrainService.h"
#include "../world/migration/EntityTransfer.h"

namespace gs::bench {
// Deterministic phase reproduction on the real publisher, immutable builder,
// movement, scheduler and LOD owner paths. No elapsed-time tolerance is
// introduced here. This mode confirms the N2 mismatch; it is not a closure
// acceptance test and never relabels the unmodified runtime as fixed.
int RunActivityTemporalReproduction(bool fixed) {
    using namespace gs::game; using namespace std::chrono_literals;
    using Clock=std::chrono::steady_clock;
    int failures=0;
    auto check=[&](const char* name,bool ok){std::printf("TEMPORAL-REPRO %s: %s\n",name,ok?"PASS":"FAIL"); std::fflush(stdout); failures+=!ok;};
    // Socket services must outlive the sessions owned by the zones.
    boost::asio::io_context io;
    ZoneManager zones; PartitionLayout layout; layout.regions_x=1; layout.regions_y=1; layout.leaves_x=2;
    InitialPartition initial; std::string error;
    if (!BuildInitialPartition({0,0,4000,2000},layout,240,initial,error)) return 2;
    zones.BuildInitialPartition(initial); auto& source=zones.GetZone(0); auto& target=zones.GetZone(1);
    auto terrain=TerrainService::Flat({0,0,4000,2000}); mx::map::WorldLogic logic; MobPrototypeRegistry types;
    LodConfig lod; ZoneTickContext ctx{terrain,logic,types,zones}; ctx.lod=&lod;
    SpatialActivityField field({500,{0,0,4000,2000}});
    const ActivityRadii radii{150,500,1500};
    ZoneScheduler scheduler; scheduler.SetLodEnabled(true);
    // No due tasks in this phase-only fixture: NextTick stays in the future.
    // The production scheduler still makes its sleep/wake decision before
    // testing due. Threaded execution is covered separately by schedulercontract.
    ZoneWorkerPool unused_pool([](std::size_t){});
    const auto now=Clock::now();
    source.NextTick()=now+50ms; target.NextTick()=now+50ms;
    {
        ZoneWriteGuard guard(target,"temporal fixture spawn");
        SpawnSystem::SpawnMob(target,{},0,MobTypeDefinition{},Position{2100,1000,0},9001);
        // Legitimate settled initial tier, derived by the production evaluator
        // with zero grace in this isolated fixture (runtime config unchanged).
        LodConfig initial_lod=lod; initial_lod.demote_full_sec=0; initial_lod.demote_reduced_sec=0; initial_lod.demote_low_sec=0;
        ctx.lod=&initial_lod; ctx.activity=field.Rebuild(zones,radii,true);
        LodSystem::Evaluate(target,ctx); LodSystem::Evaluate(target,ctx); LodSystem::Evaluate(target,ctx);
        ctx.lod=&lod;
    }
    auto old=field.Snapshot(); scheduler.ScheduleOnce(zones,unused_pool,now,old,500);
    check("initial-mob-zone-sleeps",target.Activity()==ZoneActivity::Sleeping);
    const auto pre_commit_cut=scheduler.CaptureWakeInput(zones,500);
    auto session=std::make_shared<gs::network::Session>(boost::asio::ip::tcp::socket(io),1);
    {
        ZoneWriteGuard guard(source,"temporal authoritative spawn commit");
        SpawnSystem::SpawnPlayer(source,session,{},Position{1700,1000,0},42);
    }
    auto trace=[&](const char* phase,const std::shared_ptr<const ActivityGrid>& grid) {
        const auto pos=source.FindEntity(42).get<Position>();
        const auto publication=source.ActivitySources().empty()?PlayerInfluenceSource{}:source.ActivitySources().front();
        const auto decision=scheduler.WakeDecision();
        auto brute=TierForPlayerDistanceSq((2100-pos.x)*(2100-pos.x)+(1000-pos.y)*(1000-pos.y),radii);
        const auto actual=grid->QueryPlayerInfluenceExact(2100,1000,target.Id()).tier;
        std::printf("TEMPORALTRACE phase=%s commit_seq=%llu commit_ns=%llu pending_since_ns=%llu decision_generation=%llu cut_ns=%llu source_zone=%u target_incarnation=%u net=42 x=%.9g y=%.9g field_epoch=%llu source_tick=%u target_tick=%u target_sleep=%d field_tier=%u current_tier=%u due_remaining_us=%lld\n",
            phase,publication.commit_sequence,publication.commit_steady_ns,publication.pending_since_ns,decision?decision->generation:0,decision?decision->cut_steady_ns:0,
            source.Id(),target.Id(),pos.x,pos.y,grid->epoch,source.TickIndex(),target.TickIndex(),target.Activity()==ZoneActivity::Sleeping,
            unsigned(actual),unsigned(brute),std::chrono::duration_cast<std::chrono::microseconds>(target.NextTick()-now).count());
    };
    trace("commit-before-publish",old);
    scheduler.ScheduleOnce(zones,unused_pool,now,old,500,{},fixed?pre_commit_cut:nullptr);
    check("old-generation-keeps-current-influence-asleep",target.Activity()==ZoneActivity::Sleeping);
    if(fixed) {
        check("post-cut-commit-is-legitimate-pending-phase",ValidateActivityWake(zones,scheduler.WakeDecision().get(),error));
        check("spawn-commit-published-synchronously",source.ActivitySources().size()==1 && source.ActivitySources().front().x==1700);
        scheduler.ScheduleOnce(zones,unused_pool,now,old,500);
        check("first-next-phase-wakes-with-old-field",target.Activity()==ZoneActivity::Active);
        check("wake-does-not-advance-due-or-run-extra-tick",target.NextTick()==now+50ms && !zones.AnyTickInProgress());
        check("completed-phase-current-state-oracle",ValidateActivityWake(zones,scheduler.WakeDecision().get(),error));
        check("old-field-still-correct-for-own-generation",ValidateActivityGeneration(zones,*old,error));
        target.SetActivity(ZoneActivity::Sleeping);
        check("negative-withheld-wake-detected",!ValidateActivityWake(zones,scheduler.WakeDecision().get(),error));
        target.SetActivity(ZoneActivity::Active);
        const auto good=target.WakeDecisionGeneration(); target.SetWakeDecisionGeneration(good-1);
        check("negative-stale-decision-generation-detected",!ValidateActivityWake(zones,scheduler.WakeDecision().get(),error));
        target.SetWakeDecisionGeneration(good);
        auto saved=source.ActivitySources(); source.ActivitySources().clear();
        check("negative-withheld-publication-detected",!ValidateActivityWake(zones,scheduler.WakeDecision().get(),error));
        source.ActivitySources()=saved;
    }
    {
        ZoneWriteGuard guard(source,"temporal source publish"); ActivityPublisher::Publish(source);
    }
    trace("source-published-grid-old",old);
    auto fresh=field.Rebuild(zones,radii,true);
    const std::vector<PlayerInfluenceSource> generation_input=source.ActivitySources();
    auto generation_oracle=[&](const ActivityGrid& grid) {
        // Inputs were copied from the actual publisher, not recovered from
        // the built field. Brute force is deliberately independent of cells.
        for (float x : {1600.f,1700.f,1850.f,1999.f,2100.f,2200.f,3200.f}) {
            auto brute=SimulationTier::Dormant;
            for(const auto& p:generation_input) {
                const float distance=(x-p.x)*(x-p.x)+(1000-p.y)*(1000-p.y);
                const auto tier=distance<150.f*150.f?SimulationTier::Full:
                    distance<500.f*500.f?SimulationTier::Reduced:
                    distance<1500.f*1500.f?SimulationTier::Low:SimulationTier::Dormant;
                brute=std::min(brute,tier);
            }
            if(grid.QueryPlayerTierFast(x,1000,target.Id()).tier!=brute ||
               grid.QueryPlayerInfluenceExact(x,1000,target.Id()).tier!=brute) return false;
        }
        return true;
    };
    check("generation-source-oracle",generation_oracle(*fresh));
    auto corrupt=*fresh; for(auto& cell:corrupt.cells) cell.players.clear();
    check("negative-dropped-source-is-detected",!generation_oracle(corrupt));
    trace("grid-published-before-schedule",fresh);
    check(fixed?"wake-precedes-periodic-field":"new-field-before-decision-still-sleeping",
        target.Activity()==(fixed?ZoneActivity::Active:ZoneActivity::Sleeping) && fresh->HasPlayerWithin(target.Bounds(),500));
    std::vector<ActivitySampleResult> samples;
    check("live-oracle-rejects-old-generation",!ValidateActivityFieldDetailed(zones,*old,samples,error));
    check("live-oracle-accepts-fresh-generation",ValidateActivityFieldDetailed(zones,*fresh,samples,error));
    scheduler.ScheduleOnce(zones,unused_pool,now,fresh,500);
    trace("next-schedule",fresh);
    check("fresh-schedule-wakes-without-tick-or-snapshot",target.Activity()==ZoneActivity::Active && !zones.AnyTickInProgress());
    {
        ZoneWriteGuard guard(source,"temporal real movement");
        auto entity=source.FindEntity(42); auto intent=entity.get<MoveIntent>();
        intent.state=MoveState::Running; intent.dir_angle=1.57079632679f; entity.set<MoveIntent>(intent);
        MovementSystem::Step(source,0.05f,ctx);
        check("real-movement-commits",entity.get<Position>().x>1700);
        ActivityPublisher::Publish(source);
    }
    trace("movement-published-grid-still-previous",fresh);
    if(fixed) {
        const auto oldest=source.ActivitySources().front().pending_since_ns;
        for(int repeat=0;repeat<8;++repeat) {
            ZoneWriteGuard guard(source,"temporal repeated real movement"); MovementSystem::Step(source,0.05f,ctx);
        }
        check("repeated-updates-preserve-oldest-pending-age",oldest!=0 && source.ActivitySources().front().pending_since_ns==oldest);
        target.SetActivity(ZoneActivity::Sleeping);
        check("repeated-updates-do-not-hide-missed-prior-wake",!ValidateActivityWake(zones,scheduler.WakeDecision().get(),error));
        scheduler.ScheduleOnce(zones,unused_pool,now,old,500);
        check("real-movement-next-phase-wake",target.Activity()==ZoneActivity::Active && ValidateActivityWake(zones,scheduler.WakeDecision().get(),error));
        auto broken=*scheduler.WakeDecision(); broken.generation+=1; broken.influenced.clear();
        target.SetActivity(ZoneActivity::Sleeping);
        scheduler.ScheduleOnce(zones,unused_pool,now,old,500,{},std::make_shared<ActivityWakeFrame>(broken));
        check("negative-disabled-fix-reproduces-missed-phase",!ValidateActivityWake(zones,scheduler.WakeDecision().get(),error));
        scheduler.ScheduleOnce(zones,unused_pool,now,fresh,500);
    }
    check("immutable-generation-position-retained",fresh->CellAt(fresh->ClampedCellY(1000)*fresh->dim_x+fresh->ClampedCellX(1700)).players.front().x==1700);
    // Exact representable boundary and adjacent floats: independent geometry.
    check("radius-boundary-exclusive",TierForPlayerDistanceSq(500.f*500.f,radii)==SimulationTier::Low);
    const auto inside=std::nextafter(500.f,0.f);
    check("radius-nextafter-inside",TierForPlayerDistanceSq(inside*inside,radii)==SimulationTier::Reduced);
    const auto outside=std::nextafter(500.f,std::numeric_limits<float>::infinity());
    check("radius-nextafter-outside",TierForPlayerDistanceSq(outside*outside,radii)==SimulationTier::Low);
    {
        ZoneWriteGuard guard(source,"temporal despawn commit");
        auto entity=source.FindEntity(42); source.Grid().Remove(42,entity.get<Position>());
        entity.destruct(); source.UnindexEntity(42); source.ErasePlayerBinding(42); source.RefreshResidentCounts();
    }
    check("despawn-clears-publisher-synchronously",source.ActivitySources().empty());
    auto removed=field.Rebuild(zones,radii,true);
    check("despawn-next-generation-drops-source",!removed->HasPlayerWithin(target.Bounds(),500));
    check("prior-generation-remains-immutable-after-despawn",generation_oracle(*fresh));
    if(fixed) {
        // Real production queue / Zone::Tick / movement commit. The controller
        // cuts the next phase while the owner is held just BEFORE commit,
        // then releases it. This is event ordered, never a sleep-based race.
        {
            ZoneWriteGuard guard(target,"temporal boundary fixture");
            auto mob=target.FindEntity(9001); target.Grid().Remove(9001,mob.get<Position>());
            mob.set<Position>({2000.1f,1000,0}); target.Grid().Insert(9001,mob.get<Position>(),mob);
        }
        {
            ZoneWriteGuard guard(source,"temporal boundary spawn");
            SpawnSystem::SpawnPlayer(source,session,{},Position{1499.9f,1000,0},43);
        }
        scheduler.ScheduleOnce(zones,unused_pool,now,removed,500);
        check("boundary-before-movement-sleeps",target.Activity()==ZoneActivity::Sleeping);
        std::mutex gate_mutex; std::condition_variable gate_cv; bool entered=false,release=false,done=false,timedout=false;
        source.Commands().Push([&](Zone& z){
            std::unique_lock lock(gate_mutex); entered=true; gate_cv.notify_all();
            if(!gate_cv.wait_for(lock,5s,[&]{return release;})) timedout=true;
            auto entity=z.FindEntity(43); auto intent=entity.get<MoveIntent>();
            intent.state=MoveState::Running; intent.dir_angle=1.57079632679f; entity.set<MoveIntent>(intent);
        });
        ctx.activity=removed;
        ctx.send=[](std::shared_ptr<gs::network::Session>,std::vector<std::uint8_t>){};
        ZoneWorkerPool live_pool([&](std::size_t index){
            zones.GetZone(index).Tick(0.05f,ctx);
            std::lock_guard lock(gate_mutex); done=true; gate_cv.notify_all();
        });
        live_pool.Start(4); source.NextTick()=now;
        scheduler.ScheduleOnce(zones,live_pool,now,removed,500);
        {
            std::unique_lock lock(gate_mutex);
            if(!gate_cv.wait_for(lock,5s,[&]{return entered;})) timedout=true;
        }
        const auto cut=scheduler.CaptureWakeInput(zones,500);
        {std::lock_guard lock(gate_mutex); release=true; gate_cv.notify_all();}
        {
            std::unique_lock lock(gate_mutex);
            if(!gate_cv.wait_for(lock,5s,[&]{return done;})) timedout=true;
        }
        live_pool.Stop();
        const auto publication=source.ActivitySources().front();
        std::printf("TEMPORALTHREAD phase=cut-before-authoritative-movement pool_workers=4 cut_generation=%llu cut_ns=%llu commit_seq=%llu commit_ns=%llu x=%.9g target_zone=%u original_due_ns=%lld\n",
            cut->generation,cut->cut_steady_ns,publication.commit_sequence,publication.commit_steady_ns,publication.x,target.Id(),
            std::chrono::duration_cast<std::chrono::nanoseconds>((now+50ms).time_since_epoch()).count());
        check("production-owner-movement-crosses-radius",!timedout && source.FindEntity(43).get<Position>().x>1500);
        scheduler.ScheduleOnce(zones,unused_pool,now,removed,500,{},cut);
        check("threaded-post-cut-commit-pending-not-false-fail",target.Activity()==ZoneActivity::Sleeping && ValidateActivityWake(zones,scheduler.WakeDecision().get(),error));
        scheduler.ScheduleOnce(zones,unused_pool,now,removed,500);
        check("threaded-next-phase-wakes-cross-zone",target.Activity()==ZoneActivity::Active && ValidateActivityWake(zones,scheduler.WakeDecision().get(),error));
        check("threaded-wake-preserves-nexttick",target.NextTick()==now+50ms && !zones.AnyTickInProgress());
        {
            ZoneWriteGuard guard(source,"temporal outward movement");
            auto e=source.FindEntity(43); auto intent=e.get<MoveIntent>(); intent.dir_angle=-1.57079632679f; e.set<MoveIntent>(intent);
            MovementSystem::Step(source,0.05f,ctx);
        }
        scheduler.ScheduleOnce(zones,unused_pool,now,removed,500);
        check("real-movement-outside-removes-positive-wake",target.Activity()==ZoneActivity::Sleeping && ValidateActivityWake(zones,scheduler.WakeDecision().get(),error));
        {
            ZoneWriteGuard guard(source,"temporal inward movement");
            auto e=source.FindEntity(43); auto intent=e.get<MoveIntent>(); intent.dir_angle=1.57079632679f; e.set<MoveIntent>(intent);
            MovementSystem::Step(source,0.05f,ctx);
        }
        // Transactional topology owner path, with a real player transfer.
        // No handles survive retirement/reclamation: only stable IDs below.
        auto transfer_player=[&](ZoneId from,ZoneId to) {
            auto& a=zones.GetZone(zones.FindIndexById(from)); auto& b=zones.GetZone(zones.FindIndexById(to));
            ZoneWriteGuard ga(a,"temporal transfer source"),gb(b,"temporal transfer destination");
            auto entity=a.FindEntity(43); auto value=BuildTransfer(entity,true);
            auto binding=a.ExtractPlayerBinding(43);
            a.Grid().Remove(43,value.position); entity.destruct(); a.UnindexEntity(43); a.RefreshResidentCounts();
            auto copy=ApplyTransfer(b.World(),value); b.IndexEntity(43,copy); b.Grid().Insert(43,value.position,copy);
            b.InsertPlayerBinding(43,std::move(binding)); b.RefreshResidentCounts();
        };
        auto child_for_player=[&](const std::vector<ZoneId>& children) {
            for(auto id:children) {
                const auto& b=zones.GetZone(zones.FindIndexById(id)).Bounds();
                if(1500.2f>=b.min_x && 1500.2f<b.max_x && 1000>=b.min_y && 1000<b.max_y) return id;
            }
            return ZoneId{0};
        };
        const auto original=source.Id();
        ZoneManager::SplitPlan split; std::vector<ZoneId> children;
        bool split_ok=zones.PlanSplit(original,split) && zones.CreateStagedSplit(split,children);
        ZoneId child=split_ok?child_for_player(children):0;
        if(child) {transfer_player(original,child);split_ok=zones.CommitSplit(original,children);} else split_ok=false;
        scheduler.ScheduleOnce(zones,unused_pool,now,removed,500);
        check("split-transfer-keeps-first-phase-cross-zone-wake",split_ok && target.Activity()==ZoneActivity::Active && ValidateActivityWake(zones,scheduler.WakeDecision().get(),error));
        ZoneManager::MergePlan merge; ZoneId merged=0;
        bool merge_ok=split_ok && zones.PlanMerge(original,merge) && zones.CreateStagedMergeTarget(merge,merged);
        if(merge_ok) {transfer_player(child,merged);merge_ok=zones.CommitMerge(merge,merged);}
        scheduler.ScheduleOnce(zones,unused_pool,now,removed,500);
        check("merge-transfer-publishes-new-incarnation",merge_ok && ValidateActivityWake(zones,scheduler.WakeDecision().get(),error));
        zones.ReclaimRetiredSlots(10,[](std::size_t){return false;});
        zones.ReclaimRetiredSlots(13,[](std::size_t){return false;});
        ZoneManager::SplitPlan reused_plan; std::vector<ZoneId> reused;
        bool reuse_ok=merge_ok && zones.PlanSplit(merged,reused_plan) && zones.CreateStagedSplit(reused_plan,reused);
        const auto reused_child=reuse_ok?child_for_player(reused):0;
        if(reused_child) {transfer_player(merged,reused_child);reuse_ok=zones.CommitSplit(merged,reused);} else reuse_ok=false;
        scheduler.ScheduleOnce(zones,unused_pool,now,removed,500);
        check("reclaimed-slot-wake-uses-current-incarnation",reuse_ok && reused_child!=original && reused_child!=child &&
            target.Activity()==ZoneActivity::Active && ValidateActivityWake(zones,scheduler.WakeDecision().get(),error));
    }
    std::printf("TEMPORAL-REPRO verdict=%s phase_mismatch=REPRODUCED\n",fixed?"APPROVED_PHASE_CONTRACT_TESTED":"SAVED_N2_REPRODUCTION");
    return failures?2:0;
}
}
