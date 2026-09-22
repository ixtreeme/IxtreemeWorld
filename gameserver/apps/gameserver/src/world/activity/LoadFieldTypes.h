#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ActivityTypes.h"

// Continuous Multi-Channel Load Field: shared vocabulary.
//
// The activity field answers "how much PLAYER RELEVANCE radiates from here?";
// this field answers a different question: "how much actual SERVER WORK is
// produced here?". It is DERIVED DATA, never gameplay authority and never
// zone-owned truth: the world-space cells keep their meaning across split,
// merge, zone retirement, region borders and ownership migration. The
// partition tree is a CONSUMER of the field, never its owner (§25).
//
// CHANNELS STAY SEPARATE (§4-5): a single scalar would erase WHY a place is
// hot (AI-heavy vs replication-heavy vs boundary-unstable), and every later
// decision (LOD budget, boundary placement, split) depends on the cause.
//
// UNITS ARE NOT ADDED RAW (§15): each channel has its own documented unit
// (see LoadChannelMetadata); normalization to [0,1] happens through
// configurable per-second reference budgets. Composite load is a separate,
// explicitly weighted view; the raw breakdown always remains available.
//
// MEASURED vs EVENT/COUNT (§12) is declared per channel in
// LoadChannelMetadata::source. Nothing in this file fabricates a channel:
// unwired inputs stay zero and are documented as seams.
namespace gs::game {

// --- channels ---------------------------------------------------------------

enum class LoadChannel : std::uint8_t {
    Simulation = 0, // executed AI + movement work
    Replication,    // encoded outbound payload bytes (fanout work)
    AOI,            // interest queries + candidate entities
    Combat,         // attack/damage events
    Migration,      // ownership transfers / boundary crossings
    Count,
};

inline constexpr std::size_t kLoadChannelCount = static_cast<std::size_t>(LoadChannel::Count);

constexpr std::size_t LoadChannelIndex(LoadChannel channel) noexcept
{
    return static_cast<std::size_t>(channel);
}

// How a channel's raw value is produced TODAY. Measured = the number itself
// is a measurement (bytes); EventCount = measured work events weighted by a
// documented, configurable unit cost; Unavailable = explicit seam, always 0.
enum class LoadChannelSource : std::uint8_t {
    Measured = 0,
    EventCount = 1,
    Unavailable = 2,
};

struct LoadChannelInfo {
    const char* name = "";
    LoadChannelSource source = LoadChannelSource::Unavailable;
    const char* unit = "";
    const char* note = "";
};

inline const LoadChannelInfo& LoadChannelMetadata(LoadChannel channel) noexcept
{
    static const LoadChannelInfo kInfo[kLoadChannelCount] = {
        {"Simulation",
         LoadChannelSource::EventCount,
         "updates/window",
         "AI decisions + movement integrations actually executed (LOD-scaled); "
         "per-update cost is not measured per entity -- zone-level tick time "
         "remains the measured anchor in ZoneDiagnostics"},
        {"Replication",
         LoadChannelSource::Measured,
         "bytes/window",
         "encoded transform/spawn/despawn payload bytes handed to send() "
         "(measured, attributed to the viewer position); dirty-record count is "
         "counted but not yet a separate channel input"},
        {"AOI",
         LoadChannelSource::EventCount,
         "items/window",
         "viewer AOI queries + candidate entities considered BEFORE the "
         "entity cap (dense hotspots cost more than the same entities spread)"},
        {"Combat",
         LoadChannelSource::EventCount,
         "events/window",
         "successful attack/damage events (event-derived, decays through the "
         "asymmetric EMA: a fight 5 minutes ago is cold again)"},
        {"Migration",
         LoadChannelSource::EventCount,
         "events/window",
         "committed ownership transfers + first-time boundary-crossing "
         "transitions (split/merge internal transfers included)"},
    };
    return kInfo[LoadChannelIndex(channel)];
}

// --- raw cell counters ------------------------------------------------------
//
// Integer counters only: the hot path (zone tick, supervisor transfer) writes
// these with a single index computation + increments. No clocks, no
// allocation, no locks. Converted to channel values at aggregation time in
// exactly one place (ChannelsFromCounters).
//
// 32-bit is sufficient by construction: a zone publishes at the end of every
// tick, so a cell accumulates at most one aggregation window (<= 1s) of work,
// and even the byte counter stays far below 2^32 at the AOI cap (100
// recipients) and realistic payload sizes. The supervisor clamps per-entry
// values when converting.

struct LoadCellCounters {
    std::uint32_t sim_work = 0;
    std::uint32_t repl_bytes = 0;
    std::uint32_t repl_records = 0;
    std::uint32_t repl_dirty = 0;
    std::uint32_t aoi_queries = 0;
    std::uint32_t aoi_candidates = 0;
    std::uint32_t combat_events = 0;
    std::uint32_t migration_events = 0;

    void Add(const LoadCellCounters& other) noexcept
    {
        sim_work += other.sim_work;
        repl_bytes += other.repl_bytes;
        repl_records += other.repl_records;
        repl_dirty += other.repl_dirty;
        aoi_queries += other.aoi_queries;
        aoi_candidates += other.aoi_candidates;
        combat_events += other.combat_events;
        migration_events += other.migration_events;
    }

    bool IsZero() const noexcept
    {
        return sim_work == 0 && repl_bytes == 0 && repl_records == 0 && repl_dirty == 0 &&
               aoi_queries == 0 && aoi_candidates == 0 && combat_events == 0 &&
               migration_events == 0;
    }
};

// One published sparse delta: global field cell + the counters accumulated in
// that cell since the previous publish. Zones emit these; the supervisor
// sums them into the generation.
struct LoadBinEntry {
    std::uint32_t gx = 0;
    std::uint32_t gy = 0;
    LoadCellCounters counters{};
};

// --- raw channel values -----------------------------------------------------

struct LoadChannels {
    float values[kLoadChannelCount] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    float& operator[](LoadChannel channel) noexcept
    {
        return values[LoadChannelIndex(channel)];
    }
    float operator[](LoadChannel channel) const noexcept
    {
        return values[LoadChannelIndex(channel)];
    }

    LoadChannels& operator+=(const LoadChannels& other) noexcept
    {
        for (std::size_t i = 0; i < kLoadChannelCount; ++i) {
            values[i] += other.values[i];
        }
        return *this;
    }

    bool IsFiniteNonNegative() const noexcept
    {
        for (std::size_t i = 0; i < kLoadChannelCount; ++i) {
            if (!std::isfinite(values[i]) || values[i] < 0.0f) {
                return false;
            }
        }
        return true;
    }

    bool IsZero(float epsilon = 0.0f) const noexcept
    {
        for (std::size_t i = 0; i < kLoadChannelCount; ++i) {
            if (values[i] > epsilon) {
                return false;
            }
        }
        return true;
    }
};

// Single conversion point counters -> channel values. Every raw formula is
// documented here and nowhere else:
//   Simulation  = executed AI decisions + movement integrations
//   Replication = measured encoded bytes (fanout included: every recipient's
//                 frame is encoded and sent separately)
//   AOI         = queries + pre-cap candidates
//   Combat      = attack/damage events
//   Migration   = committed transfers + boundary crossings
inline LoadChannels ChannelsFromCounters(const LoadCellCounters& counters) noexcept
{
    LoadChannels out;
    out[LoadChannel::Simulation] = static_cast<float>(counters.sim_work);
    out[LoadChannel::Replication] = static_cast<float>(counters.repl_bytes);
    out[LoadChannel::AOI] = static_cast<float>(counters.aoi_queries) +
                            static_cast<float>(counters.aoi_candidates);
    out[LoadChannel::Combat] = static_cast<float>(counters.combat_events);
    out[LoadChannel::Migration] = static_cast<float>(counters.migration_events);
    return out;
}

// --- cells / generations ----------------------------------------------------

struct LoadCell {
    LoadChannels current;   // raw channel values of the last aggregation window
    LoadChannels fast;      // asymmetric short EMA (fast rise, medium fall)
    LoadChannels slow;      // asymmetric long EMA (sustained hotspot detection)
    LoadChannels predicted; // §37 seam: equals slow today (no trajectory model)
};

enum class LoadTimescale : std::uint8_t {
    Current = 0,
    Fast = 1,
    Slow = 2,
    Predicted = 3,
};

inline const LoadChannels& LoadChannelsFor(const LoadCell& cell, LoadTimescale scale) noexcept
{
    switch (scale) {
    case LoadTimescale::Fast:
        return cell.fast;
    case LoadTimescale::Slow:
        return cell.slow;
    case LoadTimescale::Predicted:
        return cell.predicted;
    case LoadTimescale::Current:
    default:
        return cell.current;
    }
}

inline bool IsLoadCellActive(const LoadCell& cell, float epsilon = 1e-6f) noexcept
{
    return !cell.current.IsZero(epsilon) || !cell.fast.IsZero(epsilon) ||
           !cell.slow.IsZero(epsilon);
}

// Raw work totals of one aggregation window (diagnostics/report, §12). These
// are the exact integers the field consumed; no interpretation happens here.
struct LoadTotals {
    std::uint64_t sim_work = 0;
    std::uint64_t repl_bytes = 0;
    std::uint64_t repl_records = 0;
    std::uint64_t repl_dirty = 0;
    std::uint64_t aoi_queries = 0;
    std::uint64_t aoi_candidates = 0;
    std::uint64_t combat_events = 0;
    std::uint64_t migration_events = 0;
    std::uint64_t windows = 0;

    void Add(const LoadCellCounters& counters) noexcept
    {
        sim_work += counters.sim_work;
        repl_bytes += counters.repl_bytes;
        repl_records += counters.repl_records;
        repl_dirty += counters.repl_dirty;
        aoi_queries += counters.aoi_queries;
        aoi_candidates += counters.aoi_candidates;
        combat_events += counters.combat_events;
        migration_events += counters.migration_events;
    }
};

// --- configuration ----------------------------------------------------------

// Default L0 cell size. Deliberately independent of kActivityCellSizeMeters
// and of the spatial grid: the load field's resolution is a config decision
// (benchmarked; see docs/adaptive-simulation-fabric.md).
inline constexpr float kLoadCellSizeMeters = 500.0f;

struct LoadFieldConfig {
    bool enabled = true;
    float cell_size_m = kLoadCellSizeMeters;
    float aggregation_hz = 1.0f;
    bool l1_enabled = true;
    std::uint32_t l1_ratio = 4; // one L1 cell = l1_ratio^2 L0 cells

    // Normalization reference budgets: raw units per SECOND per cell that
    // map to 1.0. Configurable per deployment; invalid values warn + fall
    // back (ValidateLoadFieldConfig), never undefined behavior.
    float simulation_budget = 5000.0f;   // updates/s
    float replication_budget = 1000000.0f; // bytes/s
    float aoi_budget = 20000.0f;         // items/s
    float combat_budget = 100.0f;        // events/s
    float migration_budget = 50.0f;      // events/s

    // Composite load weights (§18). The raw/normalized breakdown always
    // remains available; the composite is an explicit, configured view.
    float weight_simulation = 1.0f;
    float weight_replication = 1.0f;
    float weight_aoi = 1.0f;
    float weight_combat = 1.0f;
    float weight_migration = 1.0f;

    // Asymmetric two-timescale EMA time constants in seconds (§19):
    // fast rise + slow fall on each timescale, so a real hotspot is picked up
    // quickly, a one-window spike cannot thrash the topology, and a dead
    // hotspot cools down instead of staying hot forever.
    float fast_rise_tau_s = 1.5f;
    float fast_fall_tau_s = 8.0f;
    float slow_rise_tau_s = 8.0f;
    float slow_fall_tau_s = 25.0f;

    // Runtime-owned: WorldRuntime overwrites this with the real terrain
    // extent (operator config never changes world geometry).
    WorldBounds bounds = WorldBounds::FromExtent(100000.0f);
};

struct ValidatedLoadFieldConfig {
    LoadFieldConfig effective;
    std::vector<std::string> warnings;
};

// Pure: clamps/repairs invalid fields, one warning per correction. Follows
// the surrounding config philosophy (warn + safe fallback, never fail-fast,
// never undefined behavior). NaN/Inf are treated as invalid.
inline ValidatedLoadFieldConfig ValidateLoadFieldConfig(const LoadFieldConfig& in)
{
    ValidatedLoadFieldConfig out;
    out.effective = in;

    auto warn = [&out](const char* message) {
        out.warnings.emplace_back(std::string("loadfield: ") + message);
    };
    auto finite_positive = [](float value) { return std::isfinite(value) && value > 0.0f; };

    if (!finite_positive(out.effective.cell_size_m)) {
        warn("cell_size_m must be finite and > 0, using 500");
        out.effective.cell_size_m = kLoadCellSizeMeters;
    }
    if (!std::isfinite(out.effective.aggregation_hz) || out.effective.aggregation_hz < 0.1f ||
        out.effective.aggregation_hz > 20.0f) {
        warn("aggregation_hz outside [0.1, 20], using 1.0");
        out.effective.aggregation_hz = 1.0f;
    }
    if (out.effective.l1_ratio < 2 || out.effective.l1_ratio > 16) {
        warn("l1_ratio outside [2, 16], using 4");
        out.effective.l1_ratio = 4;
    }
    if (!finite_positive(out.effective.simulation_budget)) {
        warn("simulation_budget must be finite and > 0, using 5000");
        out.effective.simulation_budget = 5000.0f;
    }
    if (!finite_positive(out.effective.replication_budget)) {
        warn("replication_budget must be finite and > 0, using 1000000");
        out.effective.replication_budget = 1000000.0f;
    }
    if (!finite_positive(out.effective.aoi_budget)) {
        warn("aoi_budget must be finite and > 0, using 20000");
        out.effective.aoi_budget = 20000.0f;
    }
    if (!finite_positive(out.effective.combat_budget)) {
        warn("combat_budget must be finite and > 0, using 100");
        out.effective.combat_budget = 100.0f;
    }
    if (!finite_positive(out.effective.migration_budget)) {
        warn("migration_budget must be finite and > 0, using 50");
        out.effective.migration_budget = 50.0f;
    }
    auto check_weight = [&](float& weight, const char* name) {
        if (!std::isfinite(weight) || weight < 0.0f) {
            warn((std::string(name) + " must be finite and >= 0, using 1.0").c_str());
            weight = 1.0f;
        }
    };
    check_weight(out.effective.weight_simulation, "weight_simulation");
    check_weight(out.effective.weight_replication, "weight_replication");
    check_weight(out.effective.weight_aoi, "weight_aoi");
    check_weight(out.effective.weight_combat, "weight_combat");
    check_weight(out.effective.weight_migration, "weight_migration");
    auto check_tau = [&](float& tau, const char* name) {
        if (!finite_positive(tau)) {
            warn((std::string(name) + " must be finite and > 0, using 1.0").c_str());
            tau = 1.0f;
        }
    };
    check_tau(out.effective.fast_rise_tau_s, "fast_rise_tau_s");
    check_tau(out.effective.fast_fall_tau_s, "fast_fall_tau_s");
    check_tau(out.effective.slow_rise_tau_s, "slow_rise_tau_s");
    check_tau(out.effective.slow_fall_tau_s, "slow_fall_tau_s");
    if (!out.effective.bounds.IsValid()) {
        warn("bounds must have positive extents, using a 1x1m safe grid");
        out.effective.bounds = WorldBounds::FromExtent(1.0f);
    }
    return out;
}

// --- world-space mapping ----------------------------------------------------
//
// Same origin-aware primitive as the activity field (ClampedAxisCellFor), so
// negative origins and non-square worlds work identically in both fields and
// the coordinate logic is not duplicated.

struct LoadFieldMapping {
    bool valid = false;
    WorldBounds bounds{};
    float cell_size_m = kLoadCellSizeMeters;
    std::uint32_t dim_x = 0;
    std::uint32_t dim_y = 0;

    static LoadFieldMapping FromBounds(const WorldBounds& bounds_in, float cell_size_in) noexcept
    {
        LoadFieldMapping mapping;
        mapping.cell_size_m =
            (std::isfinite(cell_size_in) && cell_size_in > 0.0f) ? cell_size_in : kLoadCellSizeMeters;
        mapping.bounds = bounds_in.IsValid() ? bounds_in : WorldBounds::FromExtent(1.0f);
        // Division (not reciprocal multiplication): a 100km extent at 500m
        // must come out as exactly 200 cells, never 201 from a rounding tail.
        mapping.dim_x = std::max<std::uint32_t>(
            1,
            static_cast<std::uint32_t>(
                std::ceil(mapping.bounds.ExtentX() / mapping.cell_size_m)));
        mapping.dim_y = std::max<std::uint32_t>(
            1,
            static_cast<std::uint32_t>(
                std::ceil(mapping.bounds.ExtentY() / mapping.cell_size_m)));
        mapping.valid = true;
        return mapping;
    }

    static LoadFieldMapping FromConfig(const LoadFieldConfig& config) noexcept
    {
        return FromBounds(config.bounds, config.cell_size_m);
    }

    std::uint32_t CellX(float x) const noexcept
    {
        return ClampedAxisCellFor(x, bounds.min_x, cell_size_m, dim_x);
    }
    std::uint32_t CellY(float y) const noexcept
    {
        return ClampedAxisCellFor(y, bounds.min_y, cell_size_m, dim_y);
    }
};

// --- normalization ----------------------------------------------------------

// Reference-budget normalization: raw counts are converted to per-second
// rates first, so the result is independent of the aggregation cadence.
// Guards: non-finite/negative raw -> 0; non-finite ratio -> 0; result is
// clamped to [0,1].
inline float NormalizeLoadChannel(float raw, float budget_per_second, float window_seconds) noexcept
{
    if (!std::isfinite(raw) || raw <= 0.0f) {
        return 0.0f;
    }
    const float window = (std::isfinite(window_seconds) && window_seconds > 0.0f) ? window_seconds : 1.0f;
    const float rate = raw / window;
    const float normalized = rate / budget_per_second;
    if (!std::isfinite(normalized) || normalized <= 0.0f) {
        return 0.0f;
    }
    return std::min(normalized, 1.0f);
}

struct NormalizedLoad {
    float values[kLoadChannelCount] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float composite = 0.0f;

    float operator[](LoadChannel channel) const noexcept
    {
        return values[LoadChannelIndex(channel)];
    }
};

inline NormalizedLoad NormalizeLoad(const LoadChannels& raw,
                                    const LoadFieldConfig& config,
                                    float window_seconds) noexcept
{
    NormalizedLoad out;
    out.values[LoadChannelIndex(LoadChannel::Simulation)] =
        NormalizeLoadChannel(raw[LoadChannel::Simulation], config.simulation_budget, window_seconds);
    out.values[LoadChannelIndex(LoadChannel::Replication)] =
        NormalizeLoadChannel(raw[LoadChannel::Replication], config.replication_budget, window_seconds);
    out.values[LoadChannelIndex(LoadChannel::AOI)] =
        NormalizeLoadChannel(raw[LoadChannel::AOI], config.aoi_budget, window_seconds);
    out.values[LoadChannelIndex(LoadChannel::Combat)] =
        NormalizeLoadChannel(raw[LoadChannel::Combat], config.combat_budget, window_seconds);
    out.values[LoadChannelIndex(LoadChannel::Migration)] =
        NormalizeLoadChannel(raw[LoadChannel::Migration], config.migration_budget, window_seconds);
    const float composite = config.weight_simulation * out.values[LoadChannelIndex(LoadChannel::Simulation)] +
                            config.weight_replication * out.values[LoadChannelIndex(LoadChannel::Replication)] +
                            config.weight_aoi * out.values[LoadChannelIndex(LoadChannel::AOI)] +
                            config.weight_combat * out.values[LoadChannelIndex(LoadChannel::Combat)] +
                            config.weight_migration * out.values[LoadChannelIndex(LoadChannel::Migration)];
    out.composite = (std::isfinite(composite) && composite > 0.0f)
                        ? std::min(composite, 1.0f)
                        : 0.0f;
    return out;
}

// --- temporal smoothing -----------------------------------------------------

// dt-aware EWMA coefficient from a time constant: response is cadence
// independent (a 0.5s rebuild and a 2s rebuild produce the same curve).
inline float EwmaAlpha(float dt_seconds, float tau_seconds) noexcept
{
    if (!(dt_seconds > 0.0f) || !(tau_seconds > 0.0f)) {
        return 1.0f;
    }
    const float alpha = 1.0f - std::exp(-dt_seconds / tau_seconds);
    return std::clamp(alpha, 0.0f, 1.0f);
}

// Asymmetric EMA: rising samples use rise_tau (fast pickup), falling samples
// use fall_tau (slow cooldown). NaN/Inf never enter the field.
inline float AsymmetricEwma(float previous,
                            float sample,
                            float dt_seconds,
                            float rise_tau_s,
                            float fall_tau_s) noexcept
{
    const float prev = (std::isfinite(previous) && previous > 0.0f) ? previous : 0.0f;
    const float s = (std::isfinite(sample) && sample > 0.0f) ? sample : 0.0f;
    const float tau = s >= prev ? rise_tau_s : fall_tau_s;
    const float alpha = EwmaAlpha(dt_seconds, tau);
    const float next = prev + (s - prev) * alpha;
    if (!std::isfinite(next)) {
        return s; // last-resort guard: never poison a generation with NaN
    }
    return next > 0.0f ? next : 0.0f;
}

// Advances one cell by one aggregation window. `raw` is the window's channel
// values (already converted from counters); the cell's current/fast/slow/
// predicted are updated in place. Pure and deterministic.
inline void AdvanceLoadCell(LoadCell& cell,
                            const LoadChannels& raw,
                            const LoadFieldConfig& config,
                            float dt_seconds) noexcept
{
    for (std::size_t i = 0; i < kLoadChannelCount; ++i) {
        const float sample = (std::isfinite(raw.values[i]) && raw.values[i] > 0.0f)
                                 ? raw.values[i]
                                 : 0.0f;
        cell.current.values[i] = sample;
        cell.fast.values[i] = AsymmetricEwma(cell.fast.values[i], sample, dt_seconds,
                                             config.fast_rise_tau_s, config.fast_fall_tau_s);
        cell.slow.values[i] = AsymmetricEwma(cell.slow.values[i], sample, dt_seconds,
                                             config.slow_rise_tau_s, config.slow_fall_tau_s);
        cell.predicted.values[i] = cell.slow.values[i]; // §37 seam
    }
}

} // namespace gs::game
