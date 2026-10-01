#include "SpawnCoordinator.h"

#include <cmath>
#include <random>

#include "common/Logging.h"

#include "../distributed/WorldDirectory.h"
#include "../distributed/WorldMessageRouter.h"
#include "../replication/NetworkSend.h"
#include "../replication/ProtocolEncoder.h"
#include "../spawn/SpawnRandom.h"
#include "../spawn/SpawnSystem.h"
#include "../zone/Zone.h"
#include "../zone/ZoneManager.h"
#include "../zone/ZoneOwnership.h"
#include "../terrain/TerrainService.h"
#include "../WorldConstants.h"

namespace gs::game {

SpawnCoordinator::SpawnCoordinator(boost::asio::io_context& io,
                                   ZoneManager& zones,
                                   TerrainService& terrain,
                                   const mx::map::WorldLogic& world_logic,
                                   OwnerMap& owners,
                                   WorldMessageRouter& router,
                                   WakeFn wake,
                                   SendFn send,
                                   SendFn send_and_close,
                                   RuntimeIdentity identity,
                                   WorldDirectory& directory)
    : io_(io)
    , zones_(zones)
    , terrain_(terrain)
    , world_logic_(world_logic)
    , owners_(owners)
    , router_(router)
    , wake_(std::move(wake))
    , send_(std::move(send))
    , send_and_close_(std::move(send_and_close))
    , identity_(identity)
    , directory_(directory)
{
}

void SpawnCoordinator::PostToOwner(std::size_t zone_index, std::function<void(Zone&)> command)
{
    router_.RouteZoneCommand(zone_index, std::move(command));
    wake_();
}

void SpawnCoordinator::Initialize(std::vector<MobSpawnPoint> spawn_points,
                                  const std::string& mob_types_config,
                                  bool spawn_now)
{
    LoadMobTypes(mob_types_config);
    {
        std::lock_guard lock(spawn_points_mutex_);
        spawn_points_ = std::move(spawn_points);
    }
    if (spawn_now) {
        (void)SpawnAllConfiguredMobs();
    }
}

void SpawnCoordinator::LoadMobTypes(const std::string& mob_types_config)
{
    mob_types_.LoadFromFile(mob_types_config);
}

void SpawnCoordinator::ClearSpawnPoints()
{
    std::lock_guard lock(spawn_points_mutex_);
    spawn_points_.clear();
    initially_spawned_.clear();
}

void SpawnCoordinator::Spawn(std::shared_ptr<gs::network::Session> session,
                             gs::db::Character character,
                             std::optional<DebugSpawnOverride> debug_spawn,
                             std::uint32_t world_tick, TerrainRequestHandle terrain_request)
{
    const auto session_id = session->Id();
    Despawn(session_id);

    // World presence invariant (H4): one character, at most one authoritative
    // presence. First presence wins; the newcomer is refused before anything
    // is allocated or routed, so no second entity can ever exist.
    if (!presence_.Admit(character.id, session_id)) {
        const auto* holder = presence_.Find(character.id);
        LOG_WARN("Session {} enter world refused: character {} already present via session {}",
                 session_id,
                 gs::db::ToUint64(character.id),
                 holder != nullptr ? holder->session_id : 0);
        send_and_close_(session, MakeEnterWorldRejectAlreadyInWorld());
        return;
    }

    const auto resolved = ResolveSpawnPosition(debug_spawn, session_id);
    const std::size_t zone_index =
        resolved ? zones_.FindIndexForPosition(resolved->position.x, resolved->position.y) : zones_.ZoneCount();
    if (!resolved || zone_index >= zones_.ZoneCount()) {
        // No valid place to put the character: refuse the enter instead of
        // dropping it into some edge zone (MAP-2 spawn rule).
        LOG_ERROR("Session {} enter world refused: no valid spawn position for character {}",
                  session_id,
                  gs::db::ToUint64(character.id));
        player_spawn_refusals_.fetch_add(1, std::memory_order_relaxed);
        presence_.ReleaseBySession(session_id);
        send_and_close_(session, MakeEnterWorldRejectServerError());
        return;
    }
    const Position position = resolved->position;
    const LayerPresence layer = resolved->layer;
    const auto net_id = net_ids_.AllocatePlayerNetId();

    // The routing location comes from the directory, not from the fact
    // that the entity physically spawns here: under logical-distribution
    // emulation (or a future balancer) this zone may be assigned elsewhere
    // while still simulated locally.
    const ZoneId spawn_zone_id = zones_.GetZone(zone_index).Id();
    const auto spawn_location =
        directory_.ResolveZone(spawn_zone_id).value_or(LocalZoneLocation(identity_, spawn_zone_id));
    OwnerInfo owner;
    owner.entity = ToGlobalEntityId(net_id, NamespaceFor(identity_));
    owner.location = spawn_location;
    owner.zone_index = zone_index;
    owner.net_id = net_id;
    owners_[session_id] = owner;
    presence_.Claim(character.id, session_id, net_id, world_tick);
    send_(session, MakeEnterWorldAccept(net_id, position, world_tick, layer.volume_id, layer.layer_id));

    LOG_INFO("Session {} assigned to zone {} ('{}') as net_id {} at {}, {}, ground_z={} volume={}",
             session_id,
             zones_.GetZone(zone_index).Id(),
             zones_.GetZone(zone_index).Name(),
             net_id,
             position.x,
             position.y,
             position.z,
             layer.volume_id);

    PostToOwner(zone_index,
                [session = std::move(session),
                character = std::move(character),
                position,
                layer,
                net_id, terrain_request=std::move(terrain_request)](Zone& zone) mutable {
                   AssertZoneOwner(zone, "zone spawn command");
                   SpawnSystem::SpawnPlayer(zone,
                                            std::move(session),
                                            std::move(character),
                                            position,
                                            net_id,
                                            layer);
                   if(terrain_request) terrain_request->Effect();
               });
}

void SpawnCoordinator::Despawn(gs::common::SessionId session_id)
{
    // Only this session's own presence can be released here (a refused
    // duplicate session's disconnect never touches the holder's record).
    presence_.ReleaseBySession(session_id);
    const auto owner_it = owners_.find(session_id);
    if (owner_it == owners_.end()) {
        LOG_INFO("Sim despawn requested for session {}, but no in-world player was found", session_id);
        return;
    }

    const auto zone_index = owner_it->second.zone_index;
    const auto net_id = owner_it->second.net_id;
    owners_.erase(owner_it);

    PostToOwner(zone_index, [this, session_id, net_id](Zone& zone) {
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
            if (viewer.IsVisible(net_id)) {
                viewer.EraseVisible(net_id);
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

bool SpawnCoordinator::SpawnMobFromSpawnPoint(std::size_t spawn_point_index, bool* needs_terrain)
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

    // A candidate on a chunk that is not resident is not "blocked": its chunk
    // is demanded and the spawn is retried later by the caller (MAP-3). A
    // candidate on a chunk published INVALID is refused like a blocked cell
    // but counted apart (unusable data, not a map rule).
    bool missing_terrain = false;
    bool invalid_terrain = false;
    auto usable = [&](const Position& p) {
        const auto cell = terrain_.Cell(p.x, p.y);
        invalid_terrain = cell.status == mx::map::TerrainStatus::InvalidData; // of the last candidate
        if (cell.status == mx::map::TerrainStatus::NotResident) {
            missing_terrain = true; // any candidate: the circle is not fully known yet -> retry later
            if (terrain_demand_) {
                terrain_demand_(terrain_.ChunkIndexOf(p.x, p.y));
            }
        }
        return cell.Walkable();
    };
    const auto net_id = net_ids_.AllocateMobNetId();
    std::mt19937 position_rng(MakeMobSeed(net_id, spawn.mob_type_id) ^ 0x4d3561u);
    Vec2 spawn_position = RandomPointInCircle(position_rng, Vec2{spawn.x, spawn.y}, spawn.radius);
    Position position{spawn_position.x, spawn_position.y, 0.0f};
    auto zone_index = zones_.FindIndexForPosition(position.x, position.y);
    for (int attempt = 0; (zone_index >= zones_.ZoneCount() || !usable(position)) && attempt < 8; ++attempt) {
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

    if (!usable(position)) {
        if (missing_terrain) {
            if (needs_terrain != nullptr) {
                *needs_terrain = true;
            }
            LOG_DEBUG("Mob spawn deferred: terrain not resident at spawn point {}", spawn_point_index);
            return false;
        }
        if (invalid_terrain) {
            mob_spawns_invalid_terrain_.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("Skipping mob spawn on invalid terrain data (chunk failed to load): type={} spawn_point={} "
                     "pos=({}, {})",
                     spawn.mob_type_id,
                     spawn_point_index,
                     position.x,
                     position.y);
            return false;
        }
        LOG_WARN("Skipping mob spawn on blocked terrain: type={} spawn_point={} pos=({}, {})",
                 spawn.mob_type_id,
                 spawn_point_index,
                 position.x,
                 position.y);
        return false;
    }

    const auto ground = terrain_.Height(position.x, position.y);
    if (!ground.Ok()) {
        LOG_WARN("Skipping mob spawn without terrain height ({}): type={} spawn_point={} pos=({}, {})",
                 mx::map::ToString(ground.status),
                 spawn.mob_type_id,
                 spawn_point_index,
                 position.x,
                 position.y);
        return false;
    }
    position.z = ground.meters;
    auto& zone = zones_.GetZone(zone_index);
    ZoneWriteGuard guard(zone, "mob spawn");
    SpawnSystem::SpawnMob(zone, spawn, spawn_point_index, *type, position, net_id);
    return true;
}

std::size_t SpawnCoordinator::SpawnPointMobs(std::size_t spawn_point_index)
{
    std::uint32_t count = 0;
    {
        std::lock_guard lock(spawn_points_mutex_);
        if (spawn_point_index >= spawn_points_.size()) {
            return 0;
        }
        count = spawn_points_[spawn_point_index].count;
    }
    std::size_t spawned = 0;
    auto& activated = initially_spawned_[spawn_point_index];
    const auto remaining = count > activated ? count - activated : 0;
    for (std::uint32_t i = 0; i < remaining; ++i) {
        if (SpawnMobFromSpawnPoint(spawn_point_index)) {
            ++activated;
            ++spawned;
        }
    }
    return spawned;
}

std::vector<MobSpawnPoint> SpawnCoordinator::SpawnPointsSnapshot() const
{
    std::lock_guard lock(spawn_points_mutex_);
    return spawn_points_;
}

void SpawnCoordinator::ProcessRespawns(float dt)
{
    if (zones_.AnyTickInProgress()) {
        return;
    }

    const auto ready = respawns_.TakeReady(dt);
    for (const auto spawn_point_index : ready) {
        bool needs_terrain = false;
        if (SpawnMobFromSpawnPoint(spawn_point_index, &needs_terrain)) {
            ++respawns_total_;
            terrain_deferrals_.erase(spawn_point_index);
            LOG_INFO("respawn: spawn_point_id={} total_respawns={}", spawn_point_index, respawns_total_);
        } else if (needs_terrain) {
            // Bounded wait for the demanded chunk: retry every 0.5 s, give up
            // after kMaxTerrainDeferrals consecutive misses of this point.
            auto& deferrals = terrain_deferrals_[spawn_point_index];
            if (++deferrals <= kMaxTerrainDeferrals) {
                respawns_.Push(spawn_point_index, 0.5f);
            } else {
                deferrals = 0;
                respawns_dropped_no_terrain_.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN("respawn dropped: spawn_point_id={} terrain not resident after {} retries",
                         spawn_point_index, kMaxTerrainDeferrals);
            }
        }
    }
}

std::optional<SpawnPlacement> SpawnCoordinator::ResolveSpawnPosition(std::optional<DebugSpawnOverride> debug_spawn,
                                                                     gs::common::SessionId session_id)
{
    auto with_height = [this](float x, float y) -> std::optional<SpawnPlacement> {
        const auto ground = terrain_.Height(x, y);
        if (!ground.Ok()) {
            return std::nullopt;
        }
        return SpawnPlacement{Position{x, y, ground.meters}, LayerPresence{}};
    };
#if MMO_DEBUG_SPAWN_OVERRIDE
    if (debug_spawn && debug_spawn->volume_id != 0) {
        // 3D-5A explicit layered admission. A refusal never falls back to
        // the terrain at the same x/y (that would be a different floor).
        if (auto layered = ResolveLayeredSpawn(*debug_spawn)) {
            LOG_INFO("Layered debug spawn honored for session {}: volume {} ({}, {}) z={}", session_id,
                     debug_spawn->volume_id, debug_spawn->x, debug_spawn->y, layered->position.z);
            return layered;
        }
        LOG_WARN("Layered debug spawn rejected for session {}: volume {} ({}, {}) - using the player spawn region",
                 session_id, debug_spawn->volume_id, debug_spawn->x, debug_spawn->y);
    } else if (debug_spawn) {
        if (IsValidDebugSpawnOverride(*debug_spawn)) {
            LOG_INFO("Debug spawn override honored for session {}: ({}, {})", session_id, debug_spawn->x,
                     debug_spawn->y);
            return with_height(debug_spawn->x, debug_spawn->y);
        }
        LOG_WARN("Debug spawn override rejected for session {}: ({}, {}) - using the player spawn region",
                 session_id,
                 debug_spawn->x,
                 debug_spawn->y);
        if (terrain_demand_ && std::isfinite(debug_spawn->x) && std::isfinite(debug_spawn->y) &&
            terrain_.Cell(debug_spawn->x, debug_spawn->y).status == mx::map::TerrainStatus::NotResident) {
            // Streaming: the point was only not loaded yet -- demand it so a
            // later attempt can use it (no waiting here: the spawn region
            // centre is permanently resident).
            terrain_demand_(terrain_.ChunkIndexOf(debug_spawn->x, debug_spawn->y));
        }
    }
#else
    (void)debug_spawn;
#endif
    const auto* spawn = world_logic_.FirstSpawn();
    if (spawn == nullptr) {
        LOG_ERROR("Session {}: the world has no player spawn region", session_id);
        return std::nullopt;
    }
    const float x = spawn->bounds.CenterX();
    const float y = spawn->bounds.CenterY();
    if (!IsValidSpawnPoint(x, y)) {
        LOG_ERROR("Session {}: player spawn region {} centre ({}, {}) is not usable (height={}, cell={})",
                  session_id,
                  spawn->id,
                  x,
                  y,
                  mx::map::ToString(terrain_.Height(x, y).status),
                  mx::map::ToString(terrain_.Cell(x, y).status));
        return std::nullopt;
    }
    LOG_INFO("Using player spawn region {} at {}, {}", spawn->id, x, y);
    return with_height(x, y);
}

bool SpawnCoordinator::IsValidSpawnPoint(float x, float y) const
{
    return terrain_.IsWalkable(x, y) && terrain_.Height(x, y).Ok() &&
           zones_.FindIndexForPosition(x, y) < zones_.ZoneCount();
}

std::optional<SpawnPlacement> SpawnCoordinator::ResolveLayeredSpawn(const DebugSpawnOverride& debug_spawn) const
{
    if (layered_ == nullptr || !std::isfinite(debug_spawn.x) || !std::isfinite(debug_spawn.y) ||
        zones_.FindIndexForPosition(debug_spawn.x, debug_spawn.y) >= zones_.ZoneCount()) {
        return std::nullopt;
    }
    const auto placed = mx::map::ResolveLayerActorPlacement(
        *layered_, layer_actor_, debug_spawn.volume_id, debug_spawn.x, debug_spawn.y);
    if (!placed.Ok()) {
        LOG_WARN("Layered spawn volume {} ({}, {}) refused: {}", debug_spawn.volume_id, debug_spawn.x,
                 debug_spawn.y, mx::map::ToString(placed.status));
        return std::nullopt;
    }
    return SpawnPlacement{Position{debug_spawn.x, debug_spawn.y, static_cast<float>(placed.state.z)},
                          LayerPresence{placed.state.volume_id, placed.state.layer_id}};
}

bool SpawnCoordinator::IsValidDebugSpawnOverride(const DebugSpawnOverride& debug_spawn)
{
    return std::isfinite(debug_spawn.x) && std::isfinite(debug_spawn.y) &&
           IsValidSpawnPoint(debug_spawn.x, debug_spawn.y);
}

std::size_t SpawnCoordinator::SpawnAllConfiguredMobs()
{
    std::vector<std::pair<MobSpawnPoint, std::size_t>> points;
    {
        std::lock_guard lock(spawn_points_mutex_);
        points.reserve(spawn_points_.size());
        for (std::size_t i = 0; i < spawn_points_.size(); ++i) {
            points.emplace_back(spawn_points_[i], i);
        }
    }
    if (mob_types_.Empty() || points.empty() || zones_.ZoneCount() == 0) {
        LOG_INFO("Mob spawn skipped: types={} spawn_points={} zones={}",
                 mob_types_.Size(),
                 points.size(),
                 zones_.ZoneCount());
        return 0;
    }

    std::size_t total = 0;
    for (const auto& [spawn, spawn_index] : points) {
        total += SpawnPointMobs(spawn_index);
    }

    LOG_INFO("spawn complete: total_mobs={}", total);
    return total;
}

} // namespace gs::game
