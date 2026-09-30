#include "ZoneScheduler.h"
#include "../activity/WakeCaptureProfile.h"

#include <algorithm>
#include <memory>
#include <vector>
#include <tuple>

#include "common/Logging.h"

#include "../WorldConstants.h"
#include "../activity/SpatialActivityField.h"
#include "../partition/ZonePartition.h"
#include "Zone.h"
#include "ZoneLoadMetrics.h"
#include "ZoneManager.h"
#include "ZoneWorkerPool.h"

namespace gs::game {
namespace {

// Load score for dispatch ordering: players dominate (their ticks carry
// replication), then residents, then recent tick cost. Ordering only --
// every due zone still ticks every interval.
std::uint64_t LoadScore(const Zone& zone)
{
    const auto& diag = zone.Diagnostics();
    const std::uint64_t players = diag.player_count.load(std::memory_order_relaxed);
    const std::uint64_t mobs = diag.mob_count.load(std::memory_order_relaxed);
    // Last tick cost (H5): independent of the diagnostics reset cadence, so
    // dispatch order does not flip with the logger's phase.
    const std::uint64_t tick_cost = diag.last_tick_micros.load(std::memory_order_relaxed);
    return players * 1'000'000 + (players + mobs) * 1'000 + tick_cost;
}

} // namespace

std::shared_ptr<const ActivityWakeFrame> ZoneScheduler::CaptureWakeInput(ZoneManager& zones,float radius)
{
    const auto capture_start=std::chrono::steady_clock::now();
    const bool profiling=wake_profile::enabled.load(std::memory_order_relaxed);
    wake_profile::Values prof{};
    const auto ns=[&](){return profiling?wake_profile::Now():0;};
    const auto begin=ns();
    prof[wake_profile::phases]=1;prof[wake_profile::zones]=zones.ZoneCount();
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(zones.ZoneCount());
    prof[wake_profile::container_growths]+=!locks.empty() || locks.capacity()>0;
    prof[wake_profile::prep_ns]=ns()-begin;
    std::uint64_t first_lock=0;
    for(std::size_t i=0;i<zones.ZoneCount();++i) {
        if(!profiling) {locks.emplace_back(zones.GetZone(i).ActivityMutex());continue;}
        const auto a=ns();
        locks.emplace_back(zones.GetZone(i).ActivityMutex(),std::defer_lock);
        if(!locks.back().try_lock()) {++prof[wake_profile::blocked_locks];locks.back().lock();}
        const auto b=ns();if(i==0)first_lock=b;
        prof[wake_profile::lock_wait_ns]+=b-a;
        prof[wake_profile::max_lock_wait_ns]=std::max(prof[wake_profile::max_lock_wait_ns],b-a);
    }
    const auto cut=std::chrono::steady_clock::now();
    const auto revision_start=ns();
    std::vector<std::pair<std::uint32_t,std::uint64_t>> revisions;
    for(std::size_t i=0;i<zones.ZoneCount();++i) {
        const auto& z=zones.GetZone(i);
        if(z.SimulationEnabled()) {
            const auto cap=revisions.capacity();revisions.emplace_back(z.Id(),z.ActivityRevisionLocked());
            prof[wake_profile::container_growths]+=cap!=revisions.capacity();
            if(profiling) {
                prof[wake_profile::source_capacity_bytes]+=z.ActivitySources().capacity()*sizeof(PlayerInfluenceSource);
                const auto index=revisions.size()-1;
                prof[wake_profile::changed_zones]+=!wake_input_ || index>=wake_input_->revisions.size() || wake_input_->revisions[index]!=revisions.back();
            }
        }
    }
    const bool reused=wake_input_ && wake_input_->radius==radius && wake_input_->revisions==revisions;
    prof[wake_profile::revision_ns]=ns()-revision_start;
    const auto finish=[&]() {
        wake_capture_us_.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-capture_start).count()),std::memory_order_relaxed);
        if(profiling) {
            prof[wake_profile::total_ns]=ns()-begin;
            const auto classified=prof[wake_profile::prep_ns]+prof[wake_profile::lock_wait_ns]+prof[wake_profile::revision_ns]+prof[wake_profile::copy_ns]+prof[wake_profile::query_ns]+prof[wake_profile::output_ns];
            prof[wake_profile::other_ns]=prof[wake_profile::total_ns]>classified?prof[wake_profile::total_ns]-classified:0;
            prof[wake_profile::scratch_capacity_bytes]=locks.capacity()*sizeof(locks[0]);
            const auto bytes=[](const auto& f)->std::uint64_t {return f?sizeof(ActivityWakeFrame)+f->sources.capacity()*sizeof(PlayerInfluenceSource)+f->revisions.capacity()*sizeof(std::pair<std::uint32_t,std::uint64_t>)+(f->influenced.bucket_count()+f->leaves.bucket_count())*sizeof(void*)+(f->influenced.size()+f->leaves.size())*(sizeof(std::uint32_t)+sizeof(void*)):0;};
            prof[wake_profile::frame_capacity_bytes]=bytes(wake_input_)+(wake_decision_!=wake_input_?bytes(wake_decision_):0);
            wake_profile::Add(prof);
        }
    };
    if(reused) {
        locks.clear();prof[wake_profile::lock_window_ns]=ns()-first_lock;
        prof[wake_profile::reuse_hits]=1;finish();return wake_input_;
    }
    prof[wake_profile::reuse_misses]=1;
    const auto copy_start=ns();
    auto frame=std::make_shared<ActivityWakeFrame>();++prof[wake_profile::container_growths];
    frame->generation=wake_input_?wake_input_->generation+1:1;
    frame->cut_steady_ns=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(cut.time_since_epoch()).count());
    frame->radius=radius; frame->revisions=std::move(revisions);
    for(std::size_t i=0;i<zones.ZoneCount();++i) {
        auto& z=zones.GetZone(i);
        if(z.SimulationEnabled()) {
            const auto cap=frame->sources.capacity();
            frame->sources.insert(frame->sources.end(),z.ActivitySources().begin(),z.ActivitySources().end());
            prof[wake_profile::container_growths]+=cap!=frame->sources.capacity();
            for(auto& p:z.ActivitySources()) p.pending_since_ns=0;
        }
    }
    locks.clear();prof[wake_profile::lock_window_ns]=ns()-first_lock;
    prof[wake_profile::copy_ns]=ns()-copy_start;
    prof[wake_profile::copied_sources]=frame->sources.size();prof[wake_profile::copied_bytes]=frame->sources.size()*sizeof(PlayerInfluenceSource);
    const auto output_start=ns();
    for(const auto* leaf:zones.GetActiveLeaves()) frame->leaves.insert(leaf->zone_id);
    prof[wake_profile::container_growths]+=frame->leaves.size();
    prof[wake_profile::output_ns]=ns()-output_start;
    const auto query_start=ns();
    // The result is an existential union over coherent sources. Once one
    // source influences a leaf, remaining sources cannot change membership.
    // Keep all sources/stamps for the independent audit and age accounting;
    // runtime resident eligibility is evaluated separately in ScheduleOnce.
    const auto output_before=prof[wake_profile::output_ns];
    for(const auto* leaf:zones.GetActiveLeaves()) {
        for(const auto& source:frame->sources) {
            if(profiling) {++prof[wake_profile::nodes];++prof[wake_profile::leaf_tests];}
            const float dx=source.x-std::clamp(source.x,leaf->bounds.min_x,leaf->bounds.max_x);
            const float dy=source.y-std::clamp(source.y,leaf->bounds.min_y,leaf->bounds.max_y);
            if(dx*dx+dy*dy>radius*radius) continue;
            const auto a=ns();const auto result=frame->influenced.insert(leaf->zone_id);
            if(profiling) {prof[wake_profile::output_ns]+=ns()-a;++prof[wake_profile::positive_leaves];prof[wake_profile::inserts]+=result.second;prof[wake_profile::container_growths]+=result.second;}
            break;
        }
    }
    prof[wake_profile::query_sources]=frame->sources.size();
    prof[wake_profile::query_ns]=ns()-query_start-(prof[wake_profile::output_ns]-output_before);
    wake_input_=frame;wake_inputs_.fetch_add(1,std::memory_order_relaxed);
    finish();return frame;
}

void ZoneScheduler::ScheduleOnce(ZoneManager& zones,
                                 ZoneWorkerPool& pool,
                                 std::chrono::steady_clock::time_point now,
                                 const std::shared_ptr<const ActivityGrid>& activity,
                                 float wake_radius_m,
                                 const DueHook& before_dispatch,
                                 const std::shared_ptr<const ActivityWakeFrame>& phase_input)
{
    const auto schedule_start = std::chrono::steady_clock::now();
    ++counters_.waves;
    const auto wake=phase_input?phase_input:CaptureWakeInput(zones,wake_radius_m);
    std::vector<std::tuple<std::uint64_t, std::size_t, std::chrono::steady_clock::time_point>> due;
    due.reserve(zones.ZoneCount());

    // Only simulating active leaves tick. Retired/split parents keep their
    // slots (index stability) but are skipped here, so the scheduler never
    // assumes a fixed zone count.
    for (ZonePartition* leaf : zones.GetActiveLeaves()) {
        const std::size_t i = zones.FindIndexById(leaf->zone_id);
        if (i >= zones.ZoneCount()) {
            continue;
        }
        auto& zone = zones.GetZone(i);
        if (!zone.SimulationEnabled()) {
            continue;
        }
        const bool has_commands = !zone.Commands().Empty();

        // Sleeping: no players, no pending commands, and nothing that needs
        // full-rate simulation. With LOD on, Low/Dormant-only mob zones may
        // sleep (their mobs freeze unobservably and resume on wake); without
        // LOD the legacy mob-bearing rule applies unchanged.
        // Wake is implicit: spawn/migration bump the counts synchronously,
        // queued commands flip has_commands, and external (cross-zone)
        // player influence keeps/wakes the zone via the activity field.
        auto& diag = zone.Diagnostics();
        zone.SetWakeDecisionGeneration(wake->generation);
        const bool lod_quiet = lod_enabled_ && diag.lod_full.load(std::memory_order_relaxed) == 0 &&
                               diag.lod_reduced.load(std::memory_order_relaxed) == 0;
        const bool legacy_quiet = diag.mob_count.load(std::memory_order_relaxed) == 0;
        // World-space external influence (§18-19): a player near the zone —
        // even across a zone or region border — keeps it awake so residents
        // simulate at the right tier. Exact predicate (no approximation),
        // so it can neither over- nor under-sleep. Empty zones sleep
        // regardless: with no residents nothing needs simulating.
        const bool external = diag.mob_count.load(std::memory_order_relaxed) > 0 &&
                              (wake->influenced.contains(zone.Id()) ||
                               (activity && activity->HasPlayerWithin(zone.Bounds(), wake_radius_m)));
        const bool locally_quiet = diag.player_count.load(std::memory_order_relaxed) == 0 &&
                                   !has_commands &&
                                   (legacy_quiet || (lod_enabled_ && lod_quiet));
        if (locally_quiet && !external) {
            if (zone.Activity() != ZoneActivity::Sleeping) {
                zone.SetActivity(ZoneActivity::Sleeping);
                // Leak-freedom: a sleeping zone never ticks again, so wipe
                // its last published sources now (despawn/retire races
                // included). Next aggregation drops them deterministically.
                zone.ClearActivitySources();
                LOG_DEBUG("Zone {} ('{}') sleeping", zone.Id(), zone.Name());
            }
            zone.NextTick() = now + kTickDt;
            zone.Diagnostics().empty_skips_since_diag.fetch_add(1, std::memory_order_relaxed);
            ++counters_.sleeping_skips;
            continue;
        }
        if (zone.Activity() == ZoneActivity::Sleeping) {
            // Wake transition. Count it as external when outside influence
            // is present and nothing local explains the wake (spawned/
            // migrated players bump counts synchronously; commands flip
            // has_commands) — predictive/external wake signal for §29.
            if (external && diag.player_count.load(std::memory_order_relaxed) == 0 &&
                !has_commands) {
                diag.wake_external_since_diag.fetch_add(1, std::memory_order_relaxed);
            }
            zone.SetActivity(ZoneActivity::Active);
            LOG_DEBUG("Zone {} ('{}') active", zone.Id(), zone.Name());
        }
        if (locally_quiet && external) {
            // Awake purely on outside influence (0 local players/commands):
            // observable §29 signal, distinct from normal activity.
            diag.sleep_blocked_external_since_diag.fetch_add(1, std::memory_order_relaxed);
        }

        // Authoritative clock (hardening H1): a zone ticks ONLY on its 20 Hz
        // cadence. Pending commands keep a zone awake (the sleep test above)
        // and are drained by the next due tick, but they never schedule an
        // early tick: every tick integrates a fixed kTickDt, so an early tick
        // would run simulation time faster than wall time (input rate would
        // become the tick rate -- a client sending per rendered frame sped
        // the whole zone up ~7x at 144 FPS, and an input flood starved it).
        // Model: command -> queue -> wake if sleeping -> wait NextTick -> tick
        // -> drain. A woken zone keeps the NextTick the sleep branch
        // maintained (<= one kTickDt away), so wake latency stays bounded.
        if (now < zone.NextTick()) {
            continue;
        }

        bool expected = false;
        if (!zone.TickInProgress().compare_exchange_strong(expected, true)) {
            ++counters_.cas_failures;
            continue;
        }
        ++counters_.due_zones;
        const auto scheduled_due=zone.NextTick();

        do {
            zone.NextTick() += kTickDt;
        } while (zone.NextTick() <= now);
        due.emplace_back(LoadScore(zone), i, scheduled_due);
    }

    // Heaviest zones first so one slow zone cannot starve behind a queue of
    // light ones on the same worker; assignment still stripes naturally via
    // the FIFO task queue.
    std::sort(due.begin(), due.end(), [](const auto& lhs, const auto& rhs) {
        return std::get<0>(lhs) > std::get<0>(rhs);
    });
    if (before_dispatch && !due.empty()) {
        // Zones are claimed and not yet running: whatever the hook posts is
        // drained at the top of exactly this tick.
        std::vector<std::size_t> indices;
        indices.reserve(due.size());
        for (const auto& [score, index, scheduled_due] : due) {
            (void)score;
            indices.push_back(index);
        }
        before_dispatch(indices);
    }
    for (const auto& [score, index, scheduled_due] : due) {
        (void)score;
        pool.Enqueue(index,scheduled_due);
    }
    counters_.enqueued += due.size();
    if(!wake_decision_ || wake_decision_->generation!=wake->generation) {
        const auto stamp=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        for(const auto& p:wake->sources) if(p.pending_since_ns && stamp>=p.pending_since_ns) {
            const auto age=(stamp-p.pending_since_ns)/1000;
            wake_sources_.fetch_add(1,std::memory_order_relaxed); wake_age_us_.fetch_add(age,std::memory_order_relaxed);
            wake_age_max_us_.store(std::max(wake_age_max_us_.load(),age));
        }
    }
    wake_decision_=wake;
    counters_.schedule_micros += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - schedule_start)
            .count());
}

ZoneScheduler::SplitGate ZoneScheduler::EvaluateSplitGate(
    const ZonePartition* leaf,
    std::chrono::steady_clock::time_point now,
    bool* out_emergency) const
{
    if (out_emergency != nullptr) {
        *out_emergency = false;
    }
    if (leaf == nullptr || !leaf->IsLeaf()) {
        return SplitGate::NotLeaf;
    }
    if (leaf->depth >= config.max_depth) {
        return SplitGate::MaxDepth;
    }
    if (leaf->load_score < config.split_load_threshold) {
        return SplitGate::BelowThreshold;
    }
    if (leaf->sustained_breach_since == std::chrono::steady_clock::time_point{}) {
        return SplitGate::NotSustained; // timer not started yet (monitor owns it)
    }
    if (now - leaf->sustained_breach_since < config.sustained_window) {
        return SplitGate::NotSustained;
    }
    if (now - leaf->last_split_time < config.split_cooldown) {
        return SplitGate::Cooldown;
    }
    if (now - leaf->last_merge_time < config.merge_to_split_cooldown) {
        // The leaf was just produced by a merge. The normal path stays
        // suppressed; the MEASURED emergency signal (p99 tick at/above the
        // configured multiple of the budget, on top of the already-passed
        // load-score gate) may override it. No magic: both signals are
        // measured, both thresholds are config, and the bypass is countable.
        const bool emergency =
            config.emergency_split_bypass && config.tick_budget_ms > 0.0f &&
            config.emergency_p99_multiplier > 0.0f &&
            leaf->p99_tick_us >=
                config.emergency_p99_multiplier * config.tick_budget_ms * 1000.0f;
        if (!emergency) {
            return SplitGate::MergeToSplitCooldown;
        }
        if (out_emergency != nullptr) {
            *out_emergency = true;
        }
    }
    return SplitGate::Pass;
}

const char* ZoneScheduler::SplitGateName(SplitGate gate) noexcept
{
    switch (gate) {
    case SplitGate::NotLeaf:
        return "not-leaf";
    case SplitGate::MaxDepth:
        return "max-depth";
    case SplitGate::BelowThreshold:
        return "below-threshold";
    case SplitGate::NotSustained:
        return "not-sustained";
    case SplitGate::Cooldown:
        return "cooldown";
    case SplitGate::MergeToSplitCooldown:
        return "merge-cooldown";
    case SplitGate::Pass:
    default:
        return "pass";
    }
}

ZoneScheduler::MergeGate ZoneScheduler::EvaluateMergeGate(
    const ZonePartition* parent,
    std::chrono::steady_clock::time_point now) const
{
    if (parent == nullptr) {
        return MergeGate::NoParent;
    }
    if (parent->parent == nullptr) {
        return MergeGate::Root; // region roots never merge
    }
    // Quadtree merge semantics: exactly the four sibling leaves of one real
    // parent. No arbitrary-neighbor or partial-set merges.
    if (parent->children.size() != 4) {
        return MergeGate::NotFourChildren;
    }
    for (const auto& child : parent->children) {
        if (!child->IsLeaf() || child->state != PartitionState::Leaf ||
            !child->simulation_enabled) {
            return MergeGate::ChildNotLeaf;
        }
    }
    if (parent->group_low_since == std::chrono::steady_clock::time_point{} ||
        now - parent->group_low_since < config.merge_sustained_low) {
        return MergeGate::NotSustained;
    }
    if (now - parent->last_split_time < config.split_to_merge_cooldown) {
        return MergeGate::SplitToMergeCooldown;
    }
    if (now - parent->last_merge_time < config.merge_cooldown) {
        return MergeGate::MergeCooldown;
    }
    return MergeGate::Pass;
}

const char* ZoneScheduler::MergeGateName(MergeGate gate) noexcept
{
    switch (gate) {
    case MergeGate::NoParent:
        return "no-parent";
    case MergeGate::Root:
        return "root";
    case MergeGate::NotFourChildren:
        return "not-four-children";
    case MergeGate::ChildNotLeaf:
        return "child-not-leaf";
    case MergeGate::NotSustained:
        return "not-sustained";
    case MergeGate::SplitToMergeCooldown:
        return "split-cooldown";
    case MergeGate::MergeCooldown:
        return "merge-cooldown";
    case MergeGate::Pass:
    default:
        return "pass";
    }
}

bool ZoneScheduler::ShouldSplit(const ZonePartition* leaf,
                                std::chrono::steady_clock::time_point now) const
{
    return EvaluateSplitGate(leaf, now) == SplitGate::Pass;
}

void CollectZoneLoadMetrics(const ZoneManager& zones, std::vector<ZoneLoadMetrics>& out)
{
    out.clear();
    out.reserve(zones.ZoneCount());
    for (std::size_t i = 0; i < zones.ZoneCount(); ++i) {
        const auto& zone = zones.GetZone(i);
        const auto& diag = zone.Diagnostics();
        ZoneLoadMetrics metrics;
        metrics.zone_id = zone.Id();
        metrics.players = diag.player_count.load(std::memory_order_relaxed);
        metrics.mobs = diag.mob_count.load(std::memory_order_relaxed);
        metrics.ghosts = diag.ghost_count.load(std::memory_order_relaxed);
        const std::uint64_t ticks = diag.ticks_since_diag.load(std::memory_order_relaxed);
        metrics.avg_tick_us = ticks > 0
                                  ? diag.tick_micros_since_diag.load(std::memory_order_relaxed) / ticks
                                  : 0;
        metrics.aoi_queries = diag.aoi_queries_since_diag.load(std::memory_order_relaxed);
        metrics.migrations = diag.migrations_since_diag.load(std::memory_order_relaxed);
        metrics.repl_records = diag.transform_records_since_diag.load(std::memory_order_relaxed);
        metrics.queue_depth = zone.Commands().Depth();
        metrics.max_queue_depth = zone.Commands().MaxDepthObserved();
        metrics.activity = zone.Activity();
        metrics.gameplay_us = diag.gameplay_micros_since_diag.load(std::memory_order_relaxed);
        metrics.ai_us = diag.ai_micros_since_diag.load(std::memory_order_relaxed);
        metrics.movement_us = diag.movement_micros_since_diag.load(std::memory_order_relaxed);
        metrics.aoi_us = diag.aoi_micros_since_diag.load(std::memory_order_relaxed);
        metrics.ghost_us = diag.ghost_micros_since_diag.load(std::memory_order_relaxed);
        metrics.activity_publish_us =
            diag.activity_publish_micros_since_diag.load(std::memory_order_relaxed);
        metrics.load_publish_us =
            diag.load_publish_micros_since_diag.load(std::memory_order_relaxed);
        metrics.replication_us = diag.replication_micros_since_diag.load(std::memory_order_relaxed);
        metrics.lod_eval_us = diag.lod_eval_us_since_diag.load(std::memory_order_relaxed);
        metrics.ghost_entities = diag.ghost_entities_since_diag.load(std::memory_order_relaxed);
        out.push_back(metrics);
    }
}

} // namespace gs::game
