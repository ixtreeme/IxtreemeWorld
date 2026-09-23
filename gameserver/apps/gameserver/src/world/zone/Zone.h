#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <flecs.h>

#include "common/Types.h"
#include "db/CharacterRepository.h"
#include "map/MapData.h"
#include "network/Session.h"

#include "../activity/ActivityTypes.h"
#include "../activity/LoadFieldPublisher.h"
#include "../activity/SpatialActivityField.h"
#include "../components/ComponentRegistration.h"
#include "../components/SimulationLod.h"
#include "../partition/PartitionTypes.h"
#include "../replication/ReplicationConfig.h"
#include "../spatial/SpatialGrid.h"
#include "../visibility/BorderSnapshot.h"
#include "../visibility/GhostSystem.h"
#include "ZoneCommandQueue.h"
#include "ZoneDiagnostics.h"

namespace gs::game {

class TerrainService;
class MobPrototypeRegistry;
class ZoneManager;
class MigrationQueue;

// Zone activity for sleeping/inactive support. Sleeping is observational:
// a zone with no players, no mobs and no pending commands performs no
// simulation work (the scheduler already skipped such zones); the flag only
// makes the state explicit, measurable and available for future multi-rate
// scheduling. Mob-bearing zones stay Active -- freezing them would lose
// gameplay time with no observer-independent justification.
enum class ZoneActivity : std::uint8_t {
    Active = 0,
    Sleeping = 1,
};

// Context for a single zone tick. Carries the shared services the zone's
// systems need WITHOUT giving the zone ownership of them. The zone never
// touches the network directly; outbound traffic flows through send.
struct ZoneTickContext {
    TerrainService& terrain;
    const mx::map::WorldLogic& world_logic;
    MobPrototypeRegistry& mob_types;
    ZoneManager& zones;
    // Border-crossing event sink for the migration queue. May be null in
    // unit-test contexts; MovementSystem checks before use.
    MigrationQueue* migration_queue = nullptr;
    std::uint32_t world_tick = 0;
    // Simulation LOD config (world-global, owned by WorldRuntime). Null or
    // disabled = legacy behavior: every entity integrates every tick.
    const LodConfig* lod = nullptr;
    // Phase 5B replication/AOI config (world-global, owned by WorldRuntime).
    // Null = optimized defaults.
    const ReplicationConfig* replication = nullptr;
    // World-space activity snapshot for this tick (immutable generation,
    // owned by WorldRuntime's field; null in unit-test contexts). Lets LOD
    // evaluation see players in ANY zone without cross-zone live reads.
    std::shared_ptr<const ActivityGrid> activity;
    std::function<void(std::shared_ptr<gs::network::Session>, std::vector<std::uint8_t>)> send;
    std::function<void(std::size_t spawn_point_index, float delay_sec)> respawn_later;
};

// A zone owns exactly one flecs::world, which is the SOLE authoritative
// store for resident gameplay state (Position, Hp, CombatStats, ...).
//
// What the zone additionally keeps is deliberately NOT gameplay state:
//  - entities_: net_id -> handle cache (index, not authority)
//  - players_: session association (socket, static character data) plus the
//    per-viewer visibility set (network/replication bookkeeping)
//  - mob_rng_: simulation RNG drivers (never snapshotted/replicated)
//  - ghosts_: read-only copies of NEIGHBOR snapshots for cross-zone visibility
//  - ghost_index_: net_id -> ghost slot (derived index for O(1) maintenance)
//  - publish_buffer_: this zone's outbound border snapshots (plain data),
//    maintained incrementally (phase 5A) with an exact content generation
//  - grid_: spatial index maintained incrementally by the systems
class Zone {
public:
    // Phase 6: per-recipient entity state. This IS the shadow client model:
    // the server tracks exactly what the recipient knows (version + the
    // replicated field values) and sends field-level deltas against it.
    struct RecipientEntity {
        std::uint32_t version = 0;        // entity TransformVersion last seen
        std::uint32_t next_due_tick = 0;  // Network LOD schedule
        std::uint32_t last_sent_tick = 0; // for starvation / state-age bounds
        float x = 0.0f;                   // client-known position
        float y = 0.0f;
        float z = 0.0f;
        std::uint16_t heading_q = 0;      // client-known quantized heading
        std::uint8_t move_state = 0;      // client-known move state
        std::uint8_t tier = 0;            // Network LOD tier (for diagnostics)
    };

    struct PlayerBinding {
        std::shared_ptr<gs::network::Session> session;
        gs::db::Character character;
        // Phase 5B/6 interest set: net_id -> recipient entity state. The key
        // set IS the viewer's visible set; a delta is only generated when the
        // entity's version moved ahead of the recipient's or a field differs.
        std::unordered_map<std::uint32_t, RecipientEntity> visible_net_versions;
        // Recipient-level lifecycle counters (phase 5B observability): they
        // travel with the binding across migrations/splits/merges, so churn
        // caused by topology changes is directly measurable per recipient.
        std::uint64_t spawn_events = 0;
        std::uint64_t despawn_events = 0;
        std::uint64_t update_events = 0;
        std::uint64_t suppressed_events = 0;

        bool IsVisible(std::uint32_t net_id) const
        {
            return visible_net_versions.find(net_id) != visible_net_versions.end();
        }
        void EraseVisible(std::uint32_t net_id)
        {
            visible_net_versions.erase(net_id);
        }
    };

    Zone(ZoneId id, std::string name, mx::map::Rect bounds);

    Zone(const Zone&) = delete;
    Zone& operator=(const Zone&) = delete;

    ZoneId Id() const noexcept
    {
        return id_;
    }
    const std::string& Name() const noexcept
    {
        return name_;
    }
    const mx::map::Rect& Bounds() const noexcept
    {
        return bounds_;
    }

    flecs::world& World() noexcept
    {
        return world_;
    }
    const flecs::world& World() const noexcept
    {
        return world_;
    }

    ZoneCommandQueue& Commands() noexcept
    {
        return commands_;
    }
    const ZoneCommandQueue& Commands() const noexcept
    {
        return commands_;
    }
    SpatialGrid& Grid() noexcept
    {
        return grid_;
    }
    const SpatialGrid& Grid() const noexcept
    {
        return grid_;
    }
    std::vector<GhostRecord>& Ghosts() noexcept
    {
        return ghosts_;
    }
    const std::vector<GhostRecord>& Ghosts() const noexcept
    {
        return ghosts_;
    }
    // Derived lookup for ghost maintenance (net_id -> ghosts_ slot). Only
    // GhostSystem mutates it; it is kept in sync with ghosts_ (swap-erase
    // removes). Consumers that need a single ghost should prefer
    // FindGhost().
    std::unordered_map<std::uint32_t, std::size_t>& GhostIndex() noexcept
    {
        return ghost_index_;
    }
    const std::unordered_map<std::uint32_t, std::size_t>& GhostIndex() const noexcept
    {
        return ghost_index_;
    }
    const GhostRecord* FindGhost(std::uint32_t net_id) const noexcept
    {
        const auto it = ghost_index_.find(net_id);
        return it != ghost_index_.end() ? &ghosts_[it->second] : nullptr;
    }
    GhostMaintenanceState& GhostMaintenance() noexcept
    {
        return ghost_maintenance_;
    }
    const GhostMaintenanceState& GhostMaintenance() const noexcept
    {
        return ghost_maintenance_;
    }
    // Canonical outbound border snapshot buffer, guarded by PublishMutex.
    // Maintained incrementally: content changes bump PublishGeneration().
    std::vector<BorderEntitySnapshot>& PublishBuffer() noexcept
    {
        return publish_buffer_;
    }
    const std::vector<BorderEntitySnapshot>& PublishBuffer() const noexcept
    {
        return publish_buffer_;
    }
    // Publish generation: consumers compare it to skip a neighbor whose
    // border set did not change at all. Atomic so the reconcile fast path can
    // check it without taking the publish mutex; the buffer copy still locks.
    // Release on bump / acquire on read: a consumer that observes a new
    // generation and then locks sees the matching buffer content.
    std::uint64_t PublishGeneration() const noexcept
    {
        return publish_generation_.load(std::memory_order_acquire);
    }
    void BumpPublishGeneration() noexcept
    {
        publish_generation_.fetch_add(1, std::memory_order_release);
    }
    // Owner-only full-refill scratch + O(1) net->slot index for the canonical
    // publish buffer (guarded by PublishMutex).
    std::vector<BorderEntitySnapshot>& PublishScratch() noexcept
    {
        return publish_scratch_;
    }
    std::unordered_map<std::uint32_t, std::size_t>& PublishIndex() noexcept
    {
        return publish_index_;
    }
    // Delta publication (guarded by PublishMutex). Nets added or whose
    // snapshot changed, and nets removed, in the latest publish generation.
    // Valid only when PublishDeltaValid() is true: a full refill invalidates
    // the delta and consumers must rescan the buffer.
    const std::vector<std::uint32_t>& PublishDeltaNets() const noexcept
    {
        return publish_delta_nets_;
    }
    const std::vector<std::uint32_t>& PublishDeltaRemoved() const noexcept
    {
        return publish_delta_removed_;
    }
    bool PublishDeltaValid() const noexcept
    {
        return publish_delta_valid_;
    }
    std::vector<std::uint32_t>& PublishDeltaNetsScratch() noexcept
    {
        return publish_delta_nets_scratch_;
    }
    std::vector<std::uint32_t>& PublishDeltaRemovedScratch() noexcept
    {
        return publish_delta_removed_scratch_;
    }
    // Commits a staged incremental generation: the staged delta becomes the
    // latest-generation delta and the generation advances. Caller holds the
    // publish mutex.
    void CommitPublishDelta() noexcept
    {
        publish_delta_nets_.swap(publish_delta_nets_scratch_);
        publish_delta_removed_.swap(publish_delta_removed_scratch_);
        publish_delta_nets_scratch_.clear();
        publish_delta_removed_scratch_.clear();
        publish_delta_valid_ = true;
        BumpPublishGeneration();
    }
    void InvalidatePublishDelta() noexcept
    {
        publish_delta_valid_ = false;
    }
    // Producer bookkeeping: which resident-set generation was last published.
    std::uint64_t PublishedEntitySetGeneration() const noexcept
    {
        return published_entity_set_generation_;
    }
    void NotePublishedEntitySetGeneration(std::uint64_t generation) noexcept
    {
        published_entity_set_generation_ = generation;
    }
    // Producer-side: resident set generation (spawn/despawn/transfer bumps
    // it), so the publisher knows when a full refill is required.
    std::uint64_t EntitySetGeneration() const noexcept
    {
        return entity_set_generation_;
    }
    // Guards the publish buffer + generation only (see Thread-safety note).
    // Taken for the short buffer fill/copy; never held across system work and
    // never nested, so no lock ordering issues arise.
    std::mutex& PublishMutex() const noexcept
    {
        return publish_mutex_;
    }
    // --- phase 5A: dirty entities for incremental border publication ---
    // Movement / combat HP / move-intent changes mark the entity here; the
    // publisher re-evaluates only these instead of scanning every resident.
    // Owner-thread only (tick or guarded command); cleared at tick start.
    void MarkEntityDirty(flecs::entity entity)
    {
        if (entity.is_valid()) {
            dirty_publish_entities_.push_back(entity);
        }
    }
    std::vector<flecs::entity>& DirtyPublishEntities() noexcept
    {
        return dirty_publish_entities_;
    }
    const std::vector<flecs::entity>& DirtyPublishEntities() const noexcept
    {
        return dirty_publish_entities_;
    }
    // Repair seam: a validator-detected publisher staleness forces the next
    // tick's publish to be a full refill.
    void RequestForcePublish() noexcept
    {
        force_publish_ = true;
    }
    bool ConsumeForcePublish() noexcept
    {
        const bool requested = force_publish_;
        force_publish_ = false;
        return requested;
    }
    // --- spatial activity publication (derived data, never authority) ---
    // This zone's authoritative player sources for the world-space activity
    // field. Written by ActivityPublisher at the end of every tick under the
    // zone guard + this mutex; copied by the supervisor aggregator.
    std::vector<PlayerInfluenceSource>& ActivitySources() noexcept
    {
        return activity_sources_;
    }
    const std::vector<PlayerInfluenceSource>& ActivitySources() const noexcept
    {
        return activity_sources_;
    }
    // Guards both publish buffers below (activity sources + load bins): they
    // are filled by the tick thread at the end of a tick and drained by the
    // supervisor at its own cadence.
    std::mutex& ActivityMutex() const noexcept
    {
        return activity_mutex_;
    }
    // --- continuous load field publication (derived data, never authority) ---
    // Zone-local integer work counters, binned by world-space field cell.
    // Written by whoever holds this zone's write guard (tick systems, combat
    // commands, guarded transfers); never read by other threads directly.
    ZoneLoadBins& LoadBins() noexcept
    {
        return load_bins_;
    }
    const ZoneLoadBins& LoadBins() const noexcept
    {
        return load_bins_;
    }
    void ConfigureLoadBins(const LoadFieldMapping& mapping)
    {
        load_bins_.Configure(mapping, bounds_);
    }
    // Sparse published deltas, guarded by ActivityMutex. Filled at the end of
    // every tick (LoadFieldPublisher::Publish), drained by the supervisor's
    // load field aggregation.
    std::vector<LoadBinEntry>& PublishedLoadBins() noexcept
    {
        return published_load_bins_;
    }
    const std::vector<LoadBinEntry>& PublishedLoadBins() const noexcept
    {
        return published_load_bins_;
    }
    // Drops all published sources (zone sleep/retire path). Postcondition:
    // the next aggregation cannot see influence from here, so despawned or
    // retired players vanish deterministically within one rebuild period.
    void ClearActivitySources()
    {
        std::lock_guard lock(activity_mutex_);
        activity_sources_.clear();
    }
    std::uint32_t TickIndex() const noexcept
    {
        return zone_tick_;
    }
    // World-global tick of this zone's last tick (Phase 5B version domain).
    std::uint32_t WorldTick() const noexcept
    {
        return world_tick_;
    }
    // Phase 5C shadow/debug seam: retain the last tick's canonical transform
    // records so the quiescent-window validator can compare the shared
    // payloads against authority. Off by default (zero cost).
    bool ReplicationAuditEnabled() const noexcept
    {
        return replication_audit_;
    }
    void SetReplicationAudit(bool enabled) noexcept
    {
        replication_audit_ = enabled;
        if (!enabled) {
            replication_audit_records_.clear();
        }
    }
    void StoreReplicationAuditRecords(const std::vector<TransformRecord>& records)
    {
        replication_audit_records_ = records;
    }
    const std::vector<TransformRecord>& LastReplicationAuditRecords() const noexcept
    {
        return replication_audit_records_;
    }
    // Marks an entity's replicated transform (position/heading/move_state) as
    // changed at the current world tick. Callers must only invoke this when a
    // published transform field actually changed.
    void NoteTransformChanged(flecs::entity entity) const
    {
        if (entity.is_valid() && entity.has<TransformVersion>()) {
            entity.set<TransformVersion>({world_tick_});
        }
    }

    std::atomic<std::thread::id>& OwnerThreadId() noexcept
    {
        return owner_thread_id_;
    }
    const std::atomic<std::thread::id>& OwnerThreadId() const noexcept
    {
        return owner_thread_id_;
    }
    std::atomic<bool>& TickInProgress() noexcept
    {
        return tick_in_progress_;
    }
    const std::atomic<bool>& TickInProgress() const noexcept
    {
        return tick_in_progress_;
    }
    std::chrono::steady_clock::time_point& NextTick() noexcept
    {
        return next_tick_;
    }
    ZoneDiagnostics& Diagnostics() noexcept
    {
        return diagnostics_;
    }
    const ZoneDiagnostics& Diagnostics() const noexcept
    {
        return diagnostics_;
    }

    // --- entity index (non-authoritative handle cache) ---
    bool HasEntity(std::uint32_t net_id) const;
    flecs::entity FindEntity(std::uint32_t net_id) const;
    void IndexEntity(std::uint32_t net_id, flecs::entity entity);
    void UnindexEntity(std::uint32_t net_id);
    bool IsResident(std::uint32_t net_id) const;

    // --- player session bindings (network association, not gameplay) ---
    PlayerBinding* FindPlayer(std::uint32_t net_id);
    const PlayerBinding* FindPlayer(std::uint32_t net_id) const;
    PlayerBinding* FindPlayerBySession(gs::common::SessionId session_id);
    std::unordered_map<std::uint32_t, PlayerBinding>& Players() noexcept
    {
        return players_;
    }
    const std::unordered_map<std::uint32_t, PlayerBinding>& Players() const noexcept
    {
        return players_;
    }
    void InsertPlayerBinding(std::uint32_t net_id, PlayerBinding binding);
    PlayerBinding ExtractPlayerBinding(std::uint32_t net_id);
    void ErasePlayerBinding(std::uint32_t net_id);

    // --- mob RNG drivers (simulation input, not replicated state) ---
    std::mt19937& MobRng(std::uint32_t net_id, std::uint32_t mob_type_id);
    void EraseMobRng(std::uint32_t net_id);
    std::optional<std::mt19937> ExtractMobRng(std::uint32_t net_id);
    void InsertMobRng(std::uint32_t net_id, std::mt19937 rng);

    // --- read-only views for validators/benchmarks (no gameplay writes) ---
    const std::unordered_map<std::uint32_t, flecs::entity>& Entities() const noexcept
    {
        return entities_;
    }
    const std::unordered_map<gs::common::SessionId, std::uint32_t>& NetBySession() const noexcept
    {
        return net_by_session_;
    }
    std::vector<std::uint32_t> MobRngKeys() const
    {
        std::vector<std::uint32_t> keys;
        keys.reserve(mob_rng_.size());
        for (const auto& [net_id, rng] : mob_rng_) {
            (void)rng;
            keys.push_back(net_id);
        }
        return keys;
    }
    std::optional<std::uint32_t> NetIdForSession(gs::common::SessionId session_id) const
    {
        const auto it = net_by_session_.find(session_id);
        return it != net_by_session_.end() ? std::optional<std::uint32_t>(it->second) : std::nullopt;
    }

    ZoneActivity Activity() const noexcept
    {
        return activity_.load(std::memory_order_relaxed);
    }
    void SetActivity(ZoneActivity activity) noexcept
    {
        activity_.store(activity, std::memory_order_relaxed);
    }

    // Stable logical geography (region lifetime == world lifetime).
    RegionId Region() const noexcept
    {
        return region_id_;
    }
    void SetRegion(RegionId region) noexcept
    {
        region_id_ = region;
    }

    // Simulation-topology role. Named Partition()/SetPartition() (not
    // PartitionState()) so the accessors never hide the PartitionState type
    // in class scope.
    PartitionState Partition() const noexcept
    {
        return partition_state_;
    }
    void SetPartition(PartitionState state) noexcept
    {
        partition_state_ = state;
    }

    // Execution gate: retired/splitting parents stop simulating while
    // keeping metadata for the tree. The scheduler only ticks simulating
    // active leaves.
    bool SimulationEnabled() const noexcept
    {
        return simulation_enabled_;
    }
    void SetSimulationEnabled(bool enabled) noexcept
    {
        simulation_enabled_ = enabled;
    }

    void RefreshResidentCounts();

    // Synchronous tier-gauge bump for freshly inserted mobs (spawn,
    // migration/transfer apply). The 1 Hz evaluation recounts exactly, so
    // this only needs to prevent stale-zero sleep decisions in between.
    void NoteLodInsert(SimulationTier tier) noexcept
    {
        auto& diag = diagnostics_;
        if (tier == SimulationTier::Full) {
            diag.lod_full.fetch_add(1, std::memory_order_relaxed);
        } else if (tier == SimulationTier::Reduced) {
            diag.lod_reduced.fetch_add(1, std::memory_order_relaxed);
        } else if (tier == SimulationTier::Low) {
            diag.lod_low.fetch_add(1, std::memory_order_relaxed);
        } else {
            diag.lod_dormant.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Thin tick: drain commands, run domain steps, publish visibility.
    // All gameplay detail lives in the systems called from here.
    void Tick(float dt, ZoneTickContext& ctx);
    void DrainCommands();

private:
    ZoneId id_;
    std::string name_;
    mx::map::Rect bounds_;
    RegionId region_id_ = 0;
    PartitionState partition_state_ = PartitionState::Leaf;
    bool simulation_enabled_ = true;
    flecs::world world_;
    ZoneCommandQueue commands_;
    SpatialGrid grid_;
    std::vector<GhostRecord> ghosts_;
    // Incremental border publication (phase 5A). `publish_buffer_` is the
    // canonical content; `publish_scratch_` is the owner-only full-refill
    // scratch (compared against the canonical buffer to decide whether the
    // publish generation advances). `publish_index_` mirrors the buffer for
    // O(1) dirty-entity updates. All three are guarded by publish_mutex_.
    std::vector<BorderEntitySnapshot> publish_buffer_;
    std::vector<BorderEntitySnapshot> publish_scratch_;
    std::unordered_map<std::uint32_t, std::size_t> publish_index_;
    // Phase 5A delta publication: nets added/changed and nets removed by the
    // LATEST publish generation. Consumers that are exactly one generation
    // behind apply only the delta instead of rescanning the whole buffer
    // (KEEP becomes O(1)); `publish_delta_valid_` is false when the latest
    // generation was a full refill (consumers fall back to a full scan).
    // Scratch vectors are owner-only staging so a no-op publish never
    // destroys the previous generation's delta.
    std::vector<std::uint32_t> publish_delta_nets_;
    std::vector<std::uint32_t> publish_delta_removed_;
    std::vector<std::uint32_t> publish_delta_nets_scratch_;
    std::vector<std::uint32_t> publish_delta_removed_scratch_;
    bool publish_delta_valid_ = false;
    std::atomic<std::uint64_t> publish_generation_{0};
    std::uint64_t entity_set_generation_ = 0;
    std::uint64_t published_entity_set_generation_ = 0;
    mutable std::mutex publish_mutex_;
    std::vector<flecs::entity> dirty_publish_entities_;
    bool force_publish_ = false;
    std::unordered_map<std::uint32_t, std::size_t> ghost_index_;
    GhostMaintenanceState ghost_maintenance_;
    std::vector<PlayerInfluenceSource> activity_sources_;
    std::vector<LoadBinEntry> published_load_bins_;
    ZoneLoadBins load_bins_;
    mutable std::mutex activity_mutex_;
    std::unordered_map<std::uint32_t, flecs::entity> entities_;
    std::unordered_map<std::uint32_t, PlayerBinding> players_;
    std::unordered_map<gs::common::SessionId, std::uint32_t> net_by_session_;
    std::unordered_map<std::uint32_t, std::mt19937> mob_rng_;
    std::uint32_t zone_tick_ = 0;
    // World-global tick of the last tick this zone ran (Phase 5B): the
    // domain shared by TransformVersion stamps and recipient last-sent
    // values, so migration across zones never invalidates comparisons.
    std::uint32_t world_tick_ = 0;
    // Phase 5C audit retention (shadow/debug only).
    bool replication_audit_ = false;
    std::vector<TransformRecord> replication_audit_records_;
    // Phase 5D spatial-index maintenance delta tracking (see Zone::Tick).
    SpatialGrid::MaintenanceCounters grid_maintenance_snapshot_;
    std::atomic<ZoneActivity> activity_{ZoneActivity::Active};
    std::atomic<std::thread::id> owner_thread_id_{std::thread::id{}};
    std::atomic<bool> tick_in_progress_{false};
    std::chrono::steady_clock::time_point next_tick_{};
    ZoneDiagnostics diagnostics_;
};

} // namespace gs::game
