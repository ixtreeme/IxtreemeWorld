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

#include "../components/ComponentRegistration.h"
#include "../components/SimulationLod.h"
#include "../partition/PartitionTypes.h"
#include "../spatial/SpatialGrid.h"
#include "../visibility/BorderSnapshot.h"
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
//  - publish_buffers_: this zone's outbound border snapshots (plain data)
//  - grid_: spatial index rebuilt from flecs state every tick
class Zone {
public:
    struct PlayerBinding {
        std::shared_ptr<gs::network::Session> session;
        gs::db::Character character;
        std::unordered_set<std::uint32_t> visible_net_ids;
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
    std::array<std::vector<BorderEntitySnapshot>, 2>& PublishBuffers() noexcept
    {
        return publish_buffers_;
    }
    // Guards the publish buffers only (see Thread-safety note in the report).
    // Taken for the short buffer fill/copy; never held across system work and
    // never nested, so no lock ordering issues arise.
    std::mutex& PublishMutex() noexcept
    {
        return publish_mutex_;
    }
    std::uint32_t TickIndex() const noexcept
    {
        return zone_tick_;
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
    std::array<std::vector<BorderEntitySnapshot>, 2> publish_buffers_;
    std::mutex publish_mutex_;
    std::unordered_map<std::uint32_t, flecs::entity> entities_;
    std::unordered_map<std::uint32_t, PlayerBinding> players_;
    std::unordered_map<gs::common::SessionId, std::uint32_t> net_by_session_;
    std::unordered_map<std::uint32_t, std::mt19937> mob_rng_;
    std::uint32_t zone_tick_ = 0;
    std::atomic<ZoneActivity> activity_{ZoneActivity::Active};
    std::atomic<std::thread::id> owner_thread_id_{std::thread::id{}};
    std::atomic<bool> tick_in_progress_{false};
    std::chrono::steady_clock::time_point next_tick_{};
    ZoneDiagnostics diagnostics_;
};

} // namespace gs::game
