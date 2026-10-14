#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>
#include <algorithm>
#include "TerrainRequest.h"

// MAP-3: per-zone terrain demand. During its tick a zone records the chunks
// its consumers need (moving entities: current + lookahead position; every
// query that answered NotResident) and the query outcomes; at the end of the
// tick it publishes them for the supervisor, which aggregates every zone's
// demand into the world's terrain streamer. Plain data, no world pointers:
// a zone that retires just stops publishing.
namespace gs::game {

struct TerrainQueryCounters {
    std::uint64_t ok = 0;
    std::uint64_t not_resident = 0;
    std::uint64_t invalid = 0;       // movement steps refused on data published INVALID (no wait, no demand)
    std::uint64_t outside = 0;
    std::uint64_t steps_attempted = 0;
    std::uint64_t steps_waiting = 0; // movement steps refused for NOT RESIDENT data (demanded, retried next tick)
    std::uint64_t steps_blocked = 0; // refused by collision (blocked / slope / water)
    std::uint64_t warps_completed = 0;
    std::uint64_t warps_refused = 0;
    std::uint64_t warps_cancelled = 0;
    std::uint64_t warps_timed_out = 0;
    double warp_wait_seconds = 0; // sum of authoritative NotResident wait time, all players

    void Add(const TerrainQueryCounters& o) noexcept
    {
        ok += o.ok;
        not_resident += o.not_resident;
        invalid += o.invalid;
        outside += o.outside;
        steps_attempted += o.steps_attempted;
        steps_waiting += o.steps_waiting;
        steps_blocked += o.steps_blocked;
        warps_completed += o.warps_completed;
        warps_refused += o.warps_refused;
        warps_cancelled += o.warps_cancelled;
        warps_timed_out += o.warps_timed_out;
        warp_wait_seconds += o.warp_wait_seconds;
    }
};

struct TerrainDemand { std::uint32_t chunk; TerrainPriority priority; };

class TerrainDemandBuffer {
public:
    static constexpr std::uint32_t kNone = 0xffffffffu;

    // Zone tick (write-guard holder): distinct within the tick.
    void Add(std::uint32_t chunk_index, TerrainPriority priority = TerrainPriority::Active) noexcept
    {
        if (chunk_index == kNone || local_.size() >= kMaxPublished) {
            return;
        }
        std::uint32_t h = (chunk_index * 2654435761u) >> (32 - kBits);
        for (std::uint32_t probe = 0; probe < 8; ++probe, h = (h + 1) & (kSlots - 1)) {
            if (stamps_[h] != stamp_) {
                stamps_[h] = stamp_;
                keys_[h] = chunk_index;
                positions_[h]=local_.size();
                local_.push_back({chunk_index,priority});
                return;
            }
            if (keys_[h] == chunk_index) {
                auto& previous=local_[positions_[h]].priority;
                previous=std::min(previous,priority);
                return;
            }
        }
        local_.push_back({chunk_index,priority}); // bounded overflow, duplicates are harmless
    }
    TerrainQueryCounters& Counters() noexcept
    {
        return counters_;
    }
    // End of the zone tick: this tick's set becomes visible to the supervisor.
    void Publish()
    {
        {
            std::lock_guard lock(mutex_);
            if (published_.size() < kMaxPublished) {
                const auto count=std::min(local_.size(),kMaxPublished-published_.size());
                published_.insert(published_.end(), local_.begin(), local_.begin()+count);
            }
            published_counters_.Add(counters_);
        }
        local_.clear();
        counters_ = {};
        if (++stamp_ == 0) {
            stamps_.fill(0);
            stamp_ = 1;
        }
    }
    // Supervisor: everything published since the last Take.
    void Take(std::vector<TerrainDemand>& out, TerrainQueryCounters& counters)
    {
        std::lock_guard lock(mutex_);
        out.insert(out.end(), published_.begin(), published_.end());
        published_.clear();
        counters.Add(published_counters_);
        published_counters_ = {};
    }

private:
    static constexpr std::uint32_t kBits = 8;
    static constexpr std::uint32_t kSlots = 1u << kBits;
    // Bounded between two supervisor passes (demand is re-issued every tick
    // anyway, so dropping an overflowing tail only delays it).
    static constexpr std::size_t kMaxPublished = 1u << 16;

    std::array<std::uint32_t, kSlots> keys_{};
    std::array<std::uint32_t, kSlots> stamps_{};
    std::array<std::size_t, kSlots> positions_{};
    std::uint32_t stamp_ = 1;
    std::vector<TerrainDemand> local_;
    TerrainQueryCounters counters_;
    std::mutex mutex_;
    std::vector<TerrainDemand> published_;
    TerrainQueryCounters published_counters_;
};

} // namespace gs::game
