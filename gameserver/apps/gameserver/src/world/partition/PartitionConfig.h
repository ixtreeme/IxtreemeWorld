#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../WorldConstants.h"

// Runtime-bound partition configuration (§14-15). Field defaults match the
// gameserver.conf.example keys and the in-code Config defaults they replace.
// Validation follows the surrounding config philosophy (see main.cpp
// ResolvePort/ResolveIoThreads): never fail-fast on operator input, warn +
// fall back per field, and always log the effective configuration.
namespace gs::game {

struct PartitionConfig {
    float split_load_threshold = 0.9f;
    float merge_load_threshold = 0.25f;
    int sustained_window_seconds = 10;
    int split_cooldown_seconds = 30;
    int merge_cooldown_seconds = 60;
    float tick_budget_ms = 16.0f;
    float resident_budget = 2000.0f;
    int max_partition_depth = 4;
    float min_zone_size_m = 500.0f;
};

struct ValidatedPartitionConfig {
    PartitionConfig effective;
    std::vector<std::string> warnings;
};

// Pure: clamps invalid fields to sane values, records one warning per
// correction. Rules:
//  - merge_threshold < split_threshold (else merge := split * 0.25)
//  - min_zone_size >= 2 * AOI radius (ghost border needs a non-degenerate
//    interior; a zone narrower than that is all border)
//  - max_depth in [1, 8]
//  - tick/resident budgets > 0, sustained window >= 1s, cooldowns >= 0s.
inline ValidatedPartitionConfig ValidatePartitionConfig(const PartitionConfig& in)
{
    ValidatedPartitionConfig out;
    out.effective = in;

    if (!(out.effective.split_load_threshold > 0.0f)) {
        out.warnings.emplace_back("partition: split_load_threshold must be > 0, using 0.9");
        out.effective.split_load_threshold = 0.9f;
    }
    if (!(out.effective.merge_load_threshold < out.effective.split_load_threshold)) {
        out.warnings.emplace_back(
            "partition: merge_load_threshold must be < split_load_threshold, using split*0.25");
        out.effective.merge_load_threshold = out.effective.split_load_threshold * 0.25f;
    }
    if (out.effective.merge_load_threshold < 0.0f) {
        out.warnings.emplace_back("partition: merge_load_threshold must be >= 0, using 0.0");
        out.effective.merge_load_threshold = 0.0f;
    }
    if (out.effective.sustained_window_seconds < 1) {
        out.warnings.emplace_back("partition: sustained_window_seconds < 1, using 1");
        out.effective.sustained_window_seconds = 1;
    }
    if (out.effective.split_cooldown_seconds < 0) {
        out.warnings.emplace_back("partition: split_cooldown_seconds < 0, using 0");
        out.effective.split_cooldown_seconds = 0;
    }
    if (out.effective.merge_cooldown_seconds < 0) {
        out.warnings.emplace_back("partition: merge_cooldown_seconds < 0, using 0");
        out.effective.merge_cooldown_seconds = 0;
    }
    if (!(out.effective.tick_budget_ms > 0.0f)) {
        out.warnings.emplace_back("partition: tick_budget_ms must be > 0, using 16.0");
        out.effective.tick_budget_ms = 16.0f;
    }
    if (!(out.effective.resident_budget > 0.0f)) {
        out.warnings.emplace_back("partition: resident_budget must be > 0, using 2000");
        out.effective.resident_budget = 2000.0f;
    }
    if (out.effective.max_partition_depth < 1 || out.effective.max_partition_depth > 8) {
        out.warnings.emplace_back("partition: max_partition_depth outside [1,8], using 4");
        out.effective.max_partition_depth = 4;
    }
    const float kMinSafeZone = 2.0f * kAoiRadiusMeters;
    if (!(out.effective.min_zone_size_m >= kMinSafeZone)) {
        out.warnings.emplace_back("partition: min_zone_size_m below 2x AOI radius, clamping");
        out.effective.min_zone_size_m = kMinSafeZone;
    }
    return out;
}

} // namespace gs::game
