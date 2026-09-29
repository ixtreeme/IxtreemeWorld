#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <atomic>

// Decides WHEN each zone ticks. Gameplay-agnostic: it only looks at resident
// counts, pending commands and tick deadlines, then hands due zones to the
// worker pool. Empty zones are skipped (diagnostic counter, as before) and
// their deadline is pushed forward so they stay idle.
namespace gs::game {

class ZoneManager;
class ZoneWorkerPool;
struct ZonePartition;
struct ActivityGrid;
struct ActivityWakeFrame;

class ZoneScheduler {
public:
    // Split/merge gating. Scores are normalized load fractions (1.0 == at
    // budget); merge sits far below split (value hysteresis) plus a much
    // longer sustained window (time hysteresis) and directional cooldowns, so
    // the topology cannot flap split/merge/split.
    struct Config {
        float split_load_threshold = 0.9f;
        float merge_load_threshold = 0.25f;
        std::chrono::seconds sustained_window{10};
        std::chrono::seconds split_cooldown{30};
        std::chrono::seconds merge_cooldown{60};
        std::uint8_t max_depth = 4;
        // Phase-3 stability: a group created by a split cannot merge until
        // this cooldown expires (prevents immediate un-splitting), and a
        // merged leaf cannot split again until this one expires (prevents a
        // small spike from re-fragmenting a just-simplified parent).
        std::chrono::seconds split_to_merge_cooldown{120};
        std::chrono::seconds merge_to_split_cooldown{90};
        // Sustained-low window for the whole sibling group (value + time
        // hysteresis; benchmarked, see docs §8).
        std::chrono::seconds merge_sustained_low{90};
        // Emergency split bypass seam: when the merge-to-split cooldown would
        // block an otherwise-passing split, a MEASURED dual signal may
        // override it -- p99 tick at/above this multiple of the tick budget
        // (the combined load score gate has already passed). Off = no bypass.
        bool emergency_split_bypass = true;
        float emergency_p99_multiplier = 2.0f;
        float tick_budget_ms = 16.0f;
    };

    // World-space activity snapshot for sleep/wake (may be null in
    // unit-test contexts: external influence then reads as absent) and the
    // wake radius in meters (derived from the LOD reduced radius; a player
    // inside it keeps the zone awake so residents simulate at the right
    // tier, including wake-before-entry).
    //
    // before_dispatch (optional) is called once per pass with the due zones,
    // already claimed (TickInProgress set) but not yet handed to the pool:
    // the supervisor delivers tick-aligned input into exactly the zones that
    // are about to drain their queue (hardening H1).
    using DueHook = std::function<void(const std::vector<std::size_t>& due_zone_indices)>;
    void ScheduleOnce(ZoneManager& zones,
                      ZoneWorkerPool& pool,
                      std::chrono::steady_clock::time_point now,
                      const std::shared_ptr<const ActivityGrid>& activity,
                      float wake_radius_m,
                      const DueHook& before_dispatch = {},
                      const std::shared_ptr<const ActivityWakeFrame>& phase_input = {});

    // Supervisor only. The last acquired publication mutex is the input cut:
    // all earlier completed commits are included; later commits belong to the
    // next phase. Exposed for deterministic phase-order tests, never from audit.
    std::shared_ptr<const ActivityWakeFrame> CaptureWakeInput(ZoneManager& zones,float radius);
    std::shared_ptr<const ActivityWakeFrame> WakeDecision() const { return wake_decision_; }
    struct WakeMetrics { std::uint64_t inputs=0,sources=0,capture_us=0,decision_age_us=0,max_decision_age_us=0; };
    WakeMetrics GetWakeMetrics() const { return {wake_inputs_.load(),wake_sources_.load(),wake_capture_us_.load(),wake_age_us_.load(),wake_age_max_us_.load()}; }

    // Structured split gate: which condition blocks a split right now. The
    // monitor uses it for why-not diagnostics; ShouldSplit is exactly
    // "gate == Pass".
    enum class SplitGate : std::uint8_t {
        Pass = 0,
        NotLeaf,
        MaxDepth,
        BelowThreshold,
        NotSustained,
        Cooldown,
        MergeToSplitCooldown,
    };

    // Pure predicates over leaf state (const: timers are owned/updated by
    // ZoneLoadMonitor::Update). Execution re-validates in ZoneManager.
    // `out_emergency` (optional) reports that the merge-to-split cooldown was
    // overridden by the measured emergency signal.
    SplitGate EvaluateSplitGate(const ZonePartition* leaf,
                                std::chrono::steady_clock::time_point now,
                                bool* out_emergency = nullptr) const;
    bool ShouldSplit(const ZonePartition* leaf, std::chrono::steady_clock::time_point now) const;

    // Structured merge gate over a whole sibling GROUP (the parent node).
    // Merge is only ever considered for a real quadtree parent with exactly
    // four authoritative active leaves -- one geometry, one executor.
    enum class MergeGate : std::uint8_t {
        Pass = 0,
        NoParent,
        Root,             // roots never merge (region floor)
        NotFourChildren,  // quadtree groups only
        ChildNotLeaf,     // staging/retired/non-simulating child
        NotSustained,     // group sustained-low window not matured
        SplitToMergeCooldown,
        MergeCooldown,    // merge-to-merge cooldown
    };

    MergeGate EvaluateMergeGate(const ZonePartition* parent,
                                std::chrono::steady_clock::time_point now) const;

    static const char* SplitGateName(SplitGate gate) noexcept;
    static const char* MergeGateName(MergeGate gate) noexcept;

    // Simulation LOD master switch (mirrors LodConfig::enabled, set by
    // WorldRuntime::ConfigureSimulationLod). Off = legacy sleep rule.
    void SetLodEnabled(bool enabled) noexcept
    {
        lod_enabled_ = enabled;
    }

    Config config;

    // Phase 7 scheduler audit counters (cumulative; the supervisor samples
    // deltas per diagnostics window).
    struct Counters {
        std::uint64_t waves = 0;          // ScheduleOnce calls
        std::uint64_t due_zones = 0;      // zones that were due
        std::uint64_t enqueued = 0;       // work items handed to the pool
        std::uint64_t sleeping_skips = 0; // sleeping zones skipped
        std::uint64_t cas_failures = 0;   // zone already ticking (guarded)
        std::uint64_t schedule_micros = 0;
    };
    Counters GetCounters() const noexcept
    {
        return {counters_.waves.load(),counters_.due_zones.load(),counters_.enqueued.load(),
                counters_.sleeping_skips.load(),counters_.cas_failures.load(),counters_.schedule_micros.load()};
    }
    Counters TakeCounters() noexcept
    {
        return {counters_.waves.exchange(0),counters_.due_zones.exchange(0),counters_.enqueued.exchange(0),
                counters_.sleeping_skips.exchange(0),counters_.cas_failures.exchange(0),counters_.schedule_micros.exchange(0)};
    }

private:
    std::shared_ptr<const ActivityWakeFrame> wake_input_, wake_decision_;
    std::atomic<std::uint64_t> wake_inputs_{0},wake_sources_{0},wake_capture_us_{0},wake_age_us_{0},wake_age_max_us_{0};
    bool lod_enabled_ = true;
    struct AtomicCounters {
        std::atomic<std::uint64_t> waves{0},due_zones{0},enqueued{0},sleeping_skips{0},cas_failures{0},schedule_micros{0};
    } counters_;
};

} // namespace gs::game
