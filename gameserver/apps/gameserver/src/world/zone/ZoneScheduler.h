#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

// Decides WHEN each zone ticks. Gameplay-agnostic: it only looks at resident
// counts, pending commands and tick deadlines, then hands due zones to the
// worker pool. Empty zones are skipped (diagnostic counter, as before) and
// their deadline is pushed forward so they stay idle.
namespace gs::game {

class ZoneManager;
class ZoneWorkerPool;
struct ZonePartition;
struct ActivityGrid;

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
    void ScheduleOnce(ZoneManager& zones,
                      ZoneWorkerPool& pool,
                      std::chrono::steady_clock::time_point now,
                      const std::shared_ptr<const ActivityGrid>& activity,
                      float wake_radius_m);

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

private:
    bool lod_enabled_ = true;
};

} // namespace gs::game
