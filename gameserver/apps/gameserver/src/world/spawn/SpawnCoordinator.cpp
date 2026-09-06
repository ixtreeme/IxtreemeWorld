#include "SpawnCoordinator.h"

#include <cmath>
#include <filesystem>
#include <random>

#include "common/Logging.h"

#include "../replication/NetworkSend.h"
#include "../replication/ProtocolEncoder.h"
#include "../spawn/SpawnRandom.h"
#include "../spawn/SpawnSystem.h"
#include "../zone/Zone.h"
#include "../zone/ZoneManager.h"
#include "../zone/ZoneOwnership.h"
#include "../terrain/TerrainService.h"
#include "../WorldConstants.h"

#ifndef IXTREEME_DEFAULT_MOB_TYPES_CONFIG
#define IXTREEME_DEFAULT_MOB_TYPES_CONFIG "mob_types.conf"
#endif

namespace gs::game {
namespace {

float DbToMeters(std::int32_t value)
{
    return static_cast<float>(value) / kDbUnitsPerMeter;
}

} // namespace

SpawnCoordinator::SpawnCoordinator(boost::asio::io_context& io,
                                   ZoneManager& zones,
                                   TerrainService& terrain,
                                   const mx::map::WorldLogic& world_logic,
                                   OwnerMap& owners,
                                   PostZoneFn post_zone,
                                   SendFn send)
    : io_(io)
    , zones_(zones)
    , terrain_(terrain)
    , world_logic_(world_logic)
    , owners_(owners)
    , post_zone_(std::move(post_zone))
    , send_(std::move(send))
{
}

void SpawnCoordinator::Initialize(const std::string& map_root, const std::string& mob_types_config)
{
    mob_types_.LoadFromFile(mob_types_config);
    {
        std::lock_guard lock(spawn_points_mutex_);
        spawn_points_ =
            SpawnLoader::LoadFromFile((std::filesystem::path(map_root) / "mob_spawns.conf").string());
    }
    SpawnConfiguredMobs();
}

void SpawnCoordinator::Spawn(std::shared_ptr<gs::network::Session> session,
                             gs::db::Character character,
                             std::optional<DebugSpawnOverride> debug_spawn,
                             std::uint32_t world_tick)
{
    const auto session_id = session->Id();
    Despawn(session_id);

    auto position = ResolveSpawnPosition(character, debug_spawn, session_id);
    std::size_t zone_index = zones_.FindIndexForPosition(position.x, position.y);
    if (zone_index >= zones_.ZoneCount()) {
        LOG_WARN("Resolved spawn at {}, {} is outside all zones; falling back to zone 0",
                 position.x,
                 position.y);
        zone_index = 0;
        position.x = zones_.GetZone(zone_index).Bounds().CenterX();
        position.y = zones_.GetZone(zone_index).Bounds().CenterY();
    }
    position.z = terrain_.SampleGroundHeight(position.x, position.y);
    const auto net_id = net_ids_.AllocatePlayerNetId();

    owners_[session_id] = OwnerInfo{zone_index, net_id};
    send_(session, MakeEnterWorldAccept(net_id, position, world_tick));

    LOG_INFO("Session {} assigned to zone {} ('{}') as net_id {} at {}, {}, ground_z={}",
             session_id,
             zones_.GetZone(zone_index).Id(),
             zones_.GetZone(zone_index).Name(),
             net_id,
             position.x,
             position.y,
             position.z);

    post_zone_(zone_index,
               [session = std::move(session),
                character = std::move(character),
                position,
                net_id](Zone& zone) mutable {
                   AssertZoneOwner(zone, "zone spawn command");
                   SpawnSystem::SpawnPlayer(zone,
                                            std::move(session),
                                            std::move(character),
                                            position,
                                            net_id);
               });
}

void SpawnCoordinator::Despawn(gs::common::SessionId session_id)
{
    const auto owner_it = owners_.find(session_id);
    if (owner_it == owners_.end()) {
        LOG_INFO("Sim despawn requested for session {}, but no in-world player was found", session_id);
        return;
    }

    const auto zone_index = owner_it->second.zone_index;
    const auto net_id = owner_it->second.net_id;
    owners_.erase(owner_it);

    post_zone_(zone_index, [this, session_id, net_id](Zone& zone) {
        AssertZoneOwner(zone, "zone despawn command");
        // O(1) session -> net lookup via the zone's reverse index.
        const auto found = zone.NetIdForSession(session_id);
        if (!found) {
            return;
        }
        const std::uint32_t found_net = *found;

        // Authoritative entity leaves exactly one index: the zone's. The
        // global owner record was already erased above, so no stale routing
        // can reach it after this point.
        const auto entity = zone.FindEntity(found_net);
        if (entity.is_valid()) {
            zone.Grid().Remove(found_net, entity.get<Position>());
            entity.destruct();
        }
        zone.UnindexEntity(found_net);
        zone.ErasePlayerBinding(found_net);
        LOG_INFO("Session {} despawned net_id {} from zone {}", session_id, net_id, zone.Id());

        auto payload = MakeDespawn(net_id);
        for (auto& [viewer_net, viewer] : zone.Players()) {
            (void)viewer_net;
            if (viewer.visible_net_ids.erase(net_id) > 0) {
                send_(viewer.session, payload);
            }
        }
        zone.RefreshResidentCounts();
    });
}

void SpawnCoordinator::AddSpawnPoint(const MobSpawnPoint& point)
{
    std::lock_guard lock(spawn_points_mutex_);
    spawn_points_.push_back(point);
}

bool SpawnCoordinator::SpawnMobFromSpawnPoint(std::size_t spawn_point_index)
{
    MobSpawnPoint spawn;
    {
        std::lock_guard lock(spawn_points_mutex_);
        if (spawn_point_index >= spawn_points_.size()) {
            return false;
        }
        spawn = spawn_points_[spawn_point_index];
    }

    const auto* type = mob_types_.Find(spawn.mob_type_id);
    if (type == nullptr) {
        LOG_WARN("Skipping mob spawn: unknown mob_type_id={}", spawn.mob_type_id);
        return false;
    }

    const auto net_id = net_ids_.AllocateMobNetId();
    std::mt19937 position_rng(MakeMobSeed(net_id, spawn.mob_type_id) ^ 0x4d3561u);
    Vec2 spawn_position = RandomPointInCircle(position_rng, Vec2{spawn.x, spawn.y}, spawn.radius);
    Position position{spawn_position.x, spawn_position.y, 0.0f};
    auto zone_index = zones_.FindIndexForPosition(position.x, position.y);
    for (int attempt = 0;
         (zone_index >= zones_.ZoneCount() || !terrain_.IsWalkable(position.x, position.y)) && attempt < 8;
         ++attempt) {
        spawn_position = RandomPointInCircle(position_rng, Vec2{spawn.x, spawn.y}, spawn.radius);
        position = Position{spawn_position.x, spawn_position.y, 0.0f};
        zone_index = zones_.FindIndexForPosition(position.x, position.y);
    }
    if (zone_index >= zones_.ZoneCount()) {
        LOG_WARN("Skipping mob spawn outside zones: type={} spawn_point={} pos=({}, {})",
                 spawn.mob_type_id,
                 spawn_point_index,
                 position.x,
                 position.y);
        return false;
    }

    if (!terrain_.IsWalkable(position.x, position.y)) {
        LOG_WARN("Skipping mob spawn on blocked terrain: type={} spawn_point={} pos=({}, {})",
                 spawn.mob_type_id,
                 spawn_point_index,
                 position.x,
                 position.y);
        return false;
    }

    position.z = terrain_.SampleGroundHeight(position.x, position.y);
    auto& zone = zones_.GetZone(zone_index);
    ZoneWriteGuard guard(zone, "mob spawn");
    SpawnSystem::SpawnMob(zone, spawn, spawn_point_index, *type, position, net_id);
    return true;
}

void SpawnCoordinator::ProcessRespawns(float dt)
{
    if (zones_.AnyTickInProgress()) {
        return;
    }

    const auto ready = respawns_.TakeReady(dt);
    for (const auto spawn_point_index : ready) {
        if (SpawnMobFromSpawnPoint(spawn_point_index)) {
            ++respawns_total_;
            LOG_INFO("respawn: spawn_point_id={} total_respawns={}", spawn_point_index, respawns_total_);
        }
    }
}

Position SpawnCoordinator::ResolveSpawnPosition(const gs::db::Character& character,
                                                std::optional<DebugSpawnOverride> debug_spawn,
                                                gs::common::SessionId session_id)
{
    (void)session_id;
    Position pos{DbToMeters(character.pos_x), DbToMeters(character.pos_y), 0.0f};
#if MMO_DEBUG_SPAWN_OVERRIDE
    if (debug_spawn) {
        if (IsValidDebugSpawnOverride(*debug_spawn)) {
            pos.x = debug_spawn->x;
            pos.y = debug_spawn->y;
            pos.z = terrain_.SampleGroundHeight(pos.x, pos.y);
            LOG_INFO("Debug spawn override honored for session {}: ({}, {})", session_id, pos.x, pos.y);
            return pos;
        }

        LOG_WARN("Debug spawn override rejected for session {}: ({}, {}) - fallback to normal spawn",
                 session_id,
                 debug_spawn->x,
                 debug_spawn->y);
    }
#else
    (void)debug_spawn;
#endif

    if (const auto* spawn = world_logic_.FirstSpawn()) {
        pos.x = spawn->bounds.CenterX();
        pos.y = spawn->bounds.CenterY();
        LOG_INFO("Using worldlogic spawn region {} at {}, {}", spawn->id, pos.x, pos.y);
    }
    if (!terrain_.IsWalkable(pos.x, pos.y)) {
        LOG_WARN("Resolved spawn at {}, {} is blocked; falling back to DB position", pos.x, pos.y);
        pos.x = DbToMeters(character.pos_x);
        pos.y = DbToMeters(character.pos_y);
    }
    pos.z = terrain_.SampleGroundHeight(pos.x, pos.y);
    return pos;
}

bool SpawnCoordinator::IsValidDebugSpawnOverride(const DebugSpawnOverride& debug_spawn)
{
    if (!std::isfinite(debug_spawn.x) || !std::isfinite(debug_spawn.y)) {
        return false;
    }

    const float max_extent = terrain_.WorldExtentMeters();
    if (debug_spawn.x < 0.0f || debug_spawn.y < 0.0f || debug_spawn.x > max_extent ||
        debug_spawn.y > max_extent) {
        return false;
    }

    if (zones_.FindIndexForPosition(debug_spawn.x, debug_spawn.y) >= zones_.ZoneCount()) {
        return false;
    }

    return terrain_.IsWalkable(debug_spawn.x, debug_spawn.y);
}

void SpawnCoordinator::SpawnConfiguredMobs()
{
    std::vector<std::pair<MobSpawnPoint, std::size_t>> points;
    {
        std::lock_guard lock(spawn_points_mutex_);
        for (std::size_t i = 0; i < spawn_points_.size(); ++i) {
            points.emplace_back(spawn_points_[i], i);
        }
    }
    if (mob_types_.Empty() || points.empty() || zones_.ZoneCount() == 0) {
        LOG_INFO("Mob spawn skipped: types={} spawn_points={} zones={}",
                 mob_types_.Size(),
                 points.size(),
                 zones_.ZoneCount());
        return;
    }

    std::uint32_t total = 0;
    for (const auto& [spawn, spawn_index] : points) {
        for (std::uint32_t i = 0; i < spawn.count; ++i) {
            if (SpawnMobFromSpawnPoint(spawn_index)) {
                ++total;
            }
        }
    }

    LOG_INFO("spawn complete: total_mobs={}", total);
}

} // namespace gs::game
