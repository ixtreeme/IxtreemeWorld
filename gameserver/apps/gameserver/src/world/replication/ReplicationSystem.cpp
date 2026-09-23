#include "ReplicationSystem.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "../spatial/AoiSystem.h"
#include "../visibility/VisibilitySystem.h"
#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"
#include "ProtocolEncoder.h"
#include "SnapshotBuilder.h"

namespace gs::game {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t ElapsedUs(const Clock::time_point& start)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

// Caller-owned scratch reused across viewers on the same worker thread: the
// encoder consumes it before the next reconcile, so no per-viewer vector is
// allocated in steady state.
thread_local std::vector<BorderEntitySnapshot> t_updates;

} // namespace

std::size_t ReplicationSystem::BroadcastTransforms(Zone& zone,
                                                   const SendFn& send,
                                                   const ReplicationConfig* config)
{
    AssertZoneOwner(zone, "zone outgoing snapshot build");

    if (zone.Players().empty()) {
        return 0;
    }

    ReplicationConfig effective;
    if (config != nullptr) {
        effective = *config;
    }

    VisibilitySystem::SpawnCache spawn_cache;
    VisibilitySystem::SnapshotCache snapshot_cache;
    // Pre-size the per-tick caches: without this every tick pays a full
    // rehash ladder (0->512 buckets) on top of the inserts themselves.
    {
        const std::size_t residents =
            zone.Players().size() +
            zone.Diagnostics().mob_count.load(std::memory_order_relaxed) + zone.Ghosts().size();
        spawn_cache.reserve(64);
        snapshot_cache.reserve(residents + 16);
    }

    auto& diag = zone.Diagnostics();
    const std::uint32_t world_tick = zone.WorldTick();
    std::size_t transform_records_sent = 0;
    for (const auto& [viewer_net_id, binding] : zone.Players()) {
        if (!binding.session) {
            continue;
        }

        const auto viewer_entity = zone.FindEntity(viewer_net_id);
        if (!viewer_entity.is_valid()) {
            continue;
        }
        const auto viewer_position = viewer_entity.get<Position>();

        // ---- AOI: spatial prefilter (grid superset) + exact distance filter,
        // reduced to the nearest kAoiEntityCap candidates.
        const auto aoi_start = Clock::now();
        const auto& candidates =
            AoiSystem::QueryCandidates(zone,
                                       viewer_net_id,
                                       viewer_position,
                                       effective.aoi_partial_cap);
        diag.repl_aoi_us_since_diag.fetch_add(ElapsedUs(aoi_start), std::memory_order_relaxed);
        diag.aoi_queries_since_diag.fetch_add(1, std::memory_order_relaxed);

        // Staggered full refresh: each viewer refreshes once per
        // refresh_ticks, spread across ticks by NetId so no tick carries the
        // whole world's refresh at once. With dirty replication off this is
        // the legacy "send everything every tick" behavior.
        const bool refresh_all =
            !effective.dirty_enabled ||
            (effective.refresh_ticks > 0 &&
             ((world_tick + viewer_net_id) % effective.refresh_ticks) == 0);
        if (refresh_all && effective.dirty_enabled) {
            diag.repl_refresh_since_diag.fetch_add(1, std::memory_order_relaxed);
        }

        // ---- Visibility reconcile: ENTER/LEAVE lifecycle + dirty transforms.
        std::uint64_t send_us = 0;
        const SendFn timed_send = [&send, &send_us](std::shared_ptr<gs::network::Session> session,
                                                    std::vector<std::uint8_t> payload) {
            const auto start = Clock::now();
            send(session, std::move(payload));
            send_us += ElapsedUs(start);
        };
        ReconcileStats stats;
        const auto reconcile_start = Clock::now();
        VisibilitySystem::ReconcileViewer(zone,
                                          viewer_net_id,
                                          candidates,
                                          timed_send,
                                          spawn_cache,
                                          snapshot_cache,
                                          refresh_all,
                                          t_updates,
                                          stats);
        const std::uint64_t reconcile_us = ElapsedUs(reconcile_start) >= send_us
                                               ? ElapsedUs(reconcile_start) - send_us
                                               : 0;

        // ---- Frame encode + fanout. The viewer's own record is always sent
        // (its own authoritative state); visible records are the dirty set.
        const auto viewer_snapshot = BuildPlayerSnapshot(zone, viewer_entity);
        const auto encode_start = Clock::now();
        auto frame = EncodeTransformFrame(viewer_snapshot, t_updates, zone.TickIndex());
        const std::uint64_t encode_us = ElapsedUs(encode_start);
        const std::uint64_t frame_bytes = frame.size();
        const std::size_t records = 1 + t_updates.size();
        {
            const auto start = Clock::now();
            timed_send(binding.session, std::move(frame));
            send_us += ElapsedUs(start);
        }
        transform_records_sent += records;

        // ---- Diagnostics (measured, not derived).
        diag.repl_reconcile_us_since_diag.fetch_add(reconcile_us, std::memory_order_relaxed);
        diag.repl_encode_us_since_diag.fetch_add(encode_us, std::memory_order_relaxed);
        diag.repl_send_us_since_diag.fetch_add(send_us, std::memory_order_relaxed);
        diag.repl_spawn_since_diag.fetch_add(stats.spawns, std::memory_order_relaxed);
        diag.repl_despawn_since_diag.fetch_add(stats.despawns, std::memory_order_relaxed);
        diag.repl_update_since_diag.fetch_add(stats.updates, std::memory_order_relaxed);
        diag.repl_suppressed_since_diag.fetch_add(stats.suppressed, std::memory_order_relaxed);
        diag.repl_records_since_diag.fetch_add(records, std::memory_order_relaxed);
        diag.repl_fanout_relationships_since_diag.fetch_add(records, std::memory_order_relaxed);
        diag.repl_frame_bytes_since_diag.fetch_add(frame_bytes, std::memory_order_relaxed);
        diag.repl_payload_bytes_since_diag.fetch_add(stats.payload_bytes,
                                                     std::memory_order_relaxed);
        diag.aoi_visible_final_since_diag.fetch_add(stats.visible, std::memory_order_relaxed);
        diag.interest_enter_since_diag.fetch_add(stats.spawns, std::memory_order_relaxed);
        diag.interest_leave_since_diag.fetch_add(stats.despawns, std::memory_order_relaxed);
        diag.interest_keep_since_diag.fetch_add(stats.updates + stats.suppressed,
                                                std::memory_order_relaxed);

        // Load field attribution: replication is measured in BYTES (the only
        // measured channel) and attributed to the viewer position -- fanout
        // is included because every recipient's frame is encoded separately.
        if (auto* load = zone.LoadBins().CellFor(viewer_position.x, viewer_position.y)) {
            const std::uint64_t total_bytes = stats.payload_bytes + frame_bytes;
            load->repl_bytes += static_cast<std::uint32_t>(
                total_bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : total_bytes);
            load->repl_records += static_cast<std::uint32_t>(
                records > 0xFFFFFFFFu ? 0xFFFFFFFFu : records);
        }
    }
    return transform_records_sent;
}

} // namespace gs::game
