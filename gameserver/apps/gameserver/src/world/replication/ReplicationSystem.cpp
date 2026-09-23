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

// Phase 5C per-zone-tick caches, reused across ticks on the same worker
// thread (clear keeps buckets/capacity: no steady-state allocation). All of
// them are recipient-independent: spawn snapshots/payloads, despawn payloads
// and the canonical 19-byte transform records are produced once per net per
// tick and shared by every interested recipient.
thread_local VisibilitySystem::SpawnCache t_spawn_cache;
thread_local VisibilitySystem::SnapshotCache t_snapshot_cache;
thread_local VisibilitySystem::DespawnCache t_despawn_cache;
thread_local RecordCache t_record_cache;
thread_local std::vector<std::uint32_t> t_record_slots;
thread_local std::vector<std::uint8_t> t_delta_payload;

// SpawnCache and DespawnCache are the same underlying type; one template
// covers both.
template <typename Cache>
std::uint64_t PayloadCacheBytes(const Cache& cache)
{
    std::uint64_t bytes = 0;
    for (const auto& [net_id, payload] : cache) {
        (void)net_id;
        bytes += payload.size();
    }
    return bytes;
}

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

    const std::size_t residents =
        zone.Players().size() +
        zone.Diagnostics().mob_count.load(std::memory_order_relaxed) + zone.Ghosts().size();

    t_spawn_cache.clear();
    t_snapshot_cache.clear();
    t_despawn_cache.clear();
    t_record_cache.Clear();
    // First-use sizing only: clear() keeps capacity, so this is a no-op on
    // steady-state ticks.
    if (t_spawn_cache.bucket_count() < 64) {
        t_spawn_cache.reserve(64);
    }
    if (t_snapshot_cache.bucket_count() < residents + 16) {
        t_snapshot_cache.reserve(residents + 16);
    }
    if (t_record_cache.index.bucket_count() < residents + 16) {
        t_record_cache.index.reserve(residents + 16);
    }
    if (t_record_cache.records.capacity() < residents) {
        t_record_cache.records.reserve(residents);
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
                                       effective.aoi_partial_cap,
                                       effective.aoi_reference_positions,
                                       effective.aoi_nth_element);
        diag.repl_aoi_us_since_diag.fetch_add(ElapsedUs(aoi_start), std::memory_order_relaxed);
        diag.aoi_queries_since_diag.fetch_add(1, std::memory_order_relaxed);

        // Staggered full refresh/resync: each viewer refreshes once per
        // refresh_ticks (v2: resync_ticks when configured), spread across
        // ticks by NetId so no tick carries the whole world's refresh at
        // once. With dirty replication off this is the legacy "send
        // everything every tick" behavior.
        const std::uint32_t refresh_period =
            effective.v2_enabled && effective.resync_ticks > 0 ? effective.resync_ticks
                                                               : effective.refresh_ticks;
        const bool refresh_all =
            !effective.dirty_enabled ||
            (refresh_period > 0 && ((world_tick + viewer_net_id) % refresh_period) == 0);
        if (refresh_all && effective.dirty_enabled) {
            diag.repl_refresh_since_diag.fetch_add(1, std::memory_order_relaxed);
        }

        // ---- Visibility reconcile: ENTER/LEAVE lifecycle + dirty transforms
        // through the shared canonical record cache.
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
                                          t_spawn_cache,
                                          t_snapshot_cache,
                                          t_despawn_cache,
                                          t_record_cache,
                                          effective,
                                          world_tick,
                                          refresh_all,
                                          t_record_slots,
                                          t_delta_payload,
                                          stats);
        const std::uint64_t reconcile_us = ElapsedUs(reconcile_start) >= send_us
                                               ? ElapsedUs(reconcile_start) - send_us
                                               : 0;

        // ---- Frame assembly: the viewer's own record (recipient-specific)
        // plus memcpy'd canonical records for the requested slots. No
        // per-recipient field encoding, no snapshot copies.
        const auto encode_start = Clock::now();
        const TransformRecord viewer_record =
            EncodeTransformRecord(viewer_net_id,
                                  viewer_position,
                                  viewer_entity.get<Heading>(),
                                  viewer_entity.get<MoveIntent>().state);
        auto frame = effective.v2_enabled
                         ? EncodeTransformFrameV2(viewer_record,
                                                  t_delta_payload,
                                                  static_cast<std::uint32_t>(stats.delta_records +
                                                                             stats.full_records),
                                                  zone.TickIndex())
                         : EncodeTransformFrameFromRecords(viewer_record,
                                                           t_record_cache.records,
                                                           t_record_slots,
                                                           zone.TickIndex());
        const std::uint64_t encode_us = ElapsedUs(encode_start);
        const std::uint64_t frame_bytes = frame.size();
        const std::size_t records = 1 + (effective.v2_enabled
                                             ? stats.delta_records + stats.full_records
                                             : t_record_slots.size());
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
        diag.repl_bytes_copied_since_diag.fetch_add(
            stats.payload_copied_bytes + records * kTransformRecordSize,
            std::memory_order_relaxed);
        diag.repl_wire_bytes_since_diag.fetch_add(stats.payload_bytes + frame_bytes,
                                                  std::memory_order_relaxed);
        diag.repl_despawn_cache_hits_since_diag.fetch_add(stats.despawn_cache_hits,
                                                          std::memory_order_relaxed);
        diag.repl_despawn_cache_misses_since_diag.fetch_add(stats.despawn_cache_misses,
                                                            std::memory_order_relaxed);
        diag.repl_v2_full_records_since_diag.fetch_add(stats.full_records,
                                                       std::memory_order_relaxed);
        diag.repl_v2_delta_records_since_diag.fetch_add(stats.delta_records,
                                                        std::memory_order_relaxed);
        diag.repl_v2_deferred_since_diag.fetch_add(stats.deferred, std::memory_order_relaxed);
        diag.repl_v2_starvation_since_diag.fetch_add(stats.starvation_bypasses,
                                                     std::memory_order_relaxed);
        diag.repl_v2_budget_hits_since_diag.fetch_add(stats.budget_hits,
                                                      std::memory_order_relaxed);
        diag.repl_v2_critical_since_diag.fetch_add(stats.critical_records,
                                                   std::memory_order_relaxed);
        diag.repl_v2_delta_bytes_since_diag.fetch_add(stats.delta_bytes,
                                                      std::memory_order_relaxed);
        {
            std::uint64_t observed =
                diag.repl_v2_max_defer_ticks.load(std::memory_order_relaxed);
            while (stats.max_defer_ticks > observed &&
                   !diag.repl_v2_max_defer_ticks.compare_exchange_weak(
                       observed, stats.max_defer_ticks, std::memory_order_relaxed)) {
            }
        }
        diag.repl_v2_tier_critical_since_diag.fetch_add(stats.tier_counts[0],
                                                        std::memory_order_relaxed);
        diag.repl_v2_tier_near_since_diag.fetch_add(stats.tier_counts[1],
                                                    std::memory_order_relaxed);
        diag.repl_v2_tier_normal_since_diag.fetch_add(stats.tier_counts[2],
                                                      std::memory_order_relaxed);
        diag.repl_v2_tier_reduced_since_diag.fetch_add(stats.tier_counts[3],
                                                       std::memory_order_relaxed);
        diag.aoi_visible_final_since_diag.fetch_add(stats.visible, std::memory_order_relaxed);
        diag.interest_enter_since_diag.fetch_add(stats.spawns, std::memory_order_relaxed);
        diag.interest_leave_since_diag.fetch_add(stats.despawns, std::memory_order_relaxed);
        diag.interest_keep_since_diag.fetch_add(stats.updates + stats.suppressed,
                                                std::memory_order_relaxed);

        // Load field attribution: replication is measured in BYTES (the only
        // measured channel) and attributed to the viewer position -- fanout
        // is included because every recipient's frame is assembled separately.
        if (auto* load = zone.LoadBins().CellFor(viewer_position.x, viewer_position.y)) {
            const std::uint64_t total_bytes = stats.payload_bytes + frame_bytes;
            load->repl_bytes += static_cast<std::uint32_t>(
                total_bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : total_bytes);
            load->repl_records += static_cast<std::uint32_t>(
                records > 0xFFFFFFFFu ? 0xFFFFFFFFu : records);
        }
    }

    // ---- Per-tick shared-payload accounting: requests vs unique
    // serializations, and generated (unique) vs copied vs wire bytes.
    const std::uint64_t record_generated = t_record_cache.serializations * kTransformRecordSize;
    const std::uint64_t spawn_generated = PayloadCacheBytes(t_spawn_cache);
    const std::uint64_t despawn_generated = PayloadCacheBytes(t_despawn_cache);
    diag.repl_record_requests_since_diag.fetch_add(t_record_cache.requests,
                                                   std::memory_order_relaxed);
    diag.repl_record_serializations_since_diag.fetch_add(t_record_cache.serializations,
                                                         std::memory_order_relaxed);
    diag.repl_bytes_generated_since_diag.fetch_add(
        record_generated + spawn_generated + despawn_generated, std::memory_order_relaxed);

    // Audit retention (shadow/debug only): the canonical records of this tick
    // stay readable for the quiescent-window validator.
    if (zone.ReplicationAuditEnabled()) {
        zone.StoreReplicationAuditRecords(t_record_cache.records);
    }
    return transform_records_sent;
}

} // namespace gs::game
