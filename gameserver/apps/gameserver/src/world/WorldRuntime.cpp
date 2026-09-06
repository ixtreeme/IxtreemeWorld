#include "WorldRuntime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>

#include "common/Logging.h"

#include "components/CombatComponents.h"
#include "components/MigrationComponents.h"
#include "components/NetworkComponents.h"
#include "components/Tags.h"
#include "components/TransformComponents.h"
#include "migration/EntityTransfer.h"
#include "migration/MigrationSystem.h"
#include "replication/NetworkSend.h"
#include "replication/ProtocolEncoder.h"
#include "spatial/SpatialTypes.h"
#include "spawn/SpawnRandom.h"
#include "spawn/SpawnSystem.h"
#include "systems/CombatSystem.h"
#include "visibility/GhostSystem.h"
#include "zone/ZoneOwnership.h"
#include "WorldConstants.h"

#ifndef IXTREEME_DEFAULT_MOB_TYPES_CONFIG
#define IXTREEME_DEFAULT_MOB_TYPES_CONFIG "mob_types.conf"
#endif

namespace gs::game {
namespace {

float DbToMeters(std::int32_t value)
{
    return static_cast<float>(value) / kDbUnitsPerMeter;
}

mx::map::WorldLogic LoadWorldLogicFromMapRoot(const std::string& map_root)
{
    const std::filesystem::path root(map_root);
    auto logic = mx::map::LoadWorldLogic(
        [&root](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
            std::filesystem::path normalized(path);
            std::ifstream file(root / normalized.relative_path(), std::ios::binary | std::ios::ate);
            if (!file) {
                return std::nullopt;
            }
            const auto end = file.tellg();
            if (end < 0) {
                return std::nullopt;
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
            file.seekg(0);
            if (!bytes.empty()) {
                file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!file) {
                    return std::nullopt;
                }
            }
            return bytes;
        },
        ".");
    if (logic) {
        LOG_INFO("Game sim loaded worldlogic: zones={} spawns={} warps={}",
                 logic->zones.size(),
                 logic->spawns.size(),
                 logic->warps.size());
        return std::move(*logic);
    }
    LOG_WARN("Game sim worldlogic load failed from {}; using fallback single zone", map_root);
    return mx::map::WorldLogic{};
}

} // namespace

WorldRuntime::WorldRuntime(boost::asio::io_context& io)
    : io_(io)
    , workers_([this](std::size_t zone_index) {
        TickZone(zone_index);
    })
{
    const std::string map_root = IXTREEME_DEFAULT_MAP_ROOT;
    terrain_ = TerrainService::LoadFromMapRoot(map_root);
    world_logic_ = LoadWorldLogicFromMapRoot(map_root);

    zones_.BuildFromWorldLogic(world_logic_, terrain_.WorldExtentMeters());
    mob_types_.LoadFromFile(IXTREEME_DEFAULT_MOB_TYPES_CONFIG);
    spawn_points_ = SpawnLoader::LoadFromFile((std::filesystem::path(map_root) / "mob_spawns.conf").string());
    SpawnConfiguredMobs();
}

WorldRuntime::~WorldRuntime()
{
    Stop();
}

void WorldRuntime::Start()
{
    if (thread_.joinable()) {
        return;
    }

    stopping_ = false;
    thread_ = std::thread([this] {
        Run();
    });
}

void WorldRuntime::Stop()
{
    stopping_ = true;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    workers_.Stop();
}

void WorldRuntime::PostSpawn(std::shared_ptr<gs::network::Session> session,
                             gs::db::Character character,
                             std::optional<DebugSpawnOverride> debug_spawn)
{
    Enqueue([this,
             session = std::move(session),
             character = std::move(character),
             debug_spawn]() mutable {
        Spawn(std::move(session), std::move(character), debug_spawn);
    });
}

void WorldRuntime::PostDespawn(gs::common::SessionId session_id)
{
    Enqueue([this, session_id] {
        Despawn(session_id);
    });
}

void WorldRuntime::PostMoveInput(gs::common::SessionId session_id,
                                 std::uint32_t sequence,
                                 float dir_angle,
                                 MoveState state)
{
    {
        std::lock_guard lock(input_mutex_);
        pending_inputs_.push_back(MoveInput{session_id, sequence, dir_angle, state});
    }
    cv_.notify_one();
}

void WorldRuntime::PostAttackTarget(gs::common::SessionId session_id, std::uint32_t target_net_id)
{
    {
        std::lock_guard lock(attack_mutex_);
        pending_attacks_.push_back(AttackInput{session_id, target_net_id});
    }
    cv_.notify_one();
}

void WorldRuntime::Enqueue(std::function<void()> command)
{
    {
        std::lock_guard lock(mutex_);
        commands_.push(std::move(command));
    }
    cv_.notify_one();
}

void WorldRuntime::EnqueueZoneCommand(std::size_t zone_index, ZoneCommandQueue::Command command)
{
    if (zone_index >= zones_.ZoneCount()) {
        return;
    }
    zones_.GetZone(zone_index).Commands().Push(std::move(command));
    cv_.notify_one();
}

ZoneTickContext WorldRuntime::BuildZoneTickContext()
{
    ZoneTickContext ctx{terrain_,
                        world_logic_,
                        mob_types_,
                        zones_,
                        world_tick_.load(std::memory_order_relaxed),
                        [this](std::shared_ptr<gs::network::Session> session, std::vector<std::uint8_t> payload) {
                            SendToSession(io_, session, std::move(payload));
                        },
                        [this](std::size_t spawn_point_index, float delay_sec) {
                            respawns_.Push(spawn_point_index, delay_sec);
                        }};
    return ctx;
}

void WorldRuntime::TickZone(std::size_t zone_index)
{
    if (zone_index >= zones_.ZoneCount()) {
        return;
    }
    auto ctx = BuildZoneTickContext();
    zones_.GetZone(zone_index).Tick(kTickDtSeconds, ctx);
}

void WorldRuntime::Run()
{
    sim_thread_id_ = std::this_thread::get_id();

    workers_.Start(zones_.ZoneCount());

    LOG_INFO("Game sim supervisor started: zones={} workers={} aoi_radius={} aoi_cap={}",
             zones_.ZoneCount(),
             workers_.WorkerCount(),
             kAoiRadiusMeters,
             kAoiEntityCap);
    auto next_world_tick = std::chrono::steady_clock::now() + kTickDt;
    auto next_diagnostics = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    while (!stopping_) {
        DrainGlobalCommands();
        ProcessMigrations();
        ProcessRespawns(kTickDtSeconds);
        DrainMoveInputs();
        DrainAttackInputs();
        scheduler_.ScheduleOnce(zones_, workers_, std::chrono::steady_clock::now());

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_world_tick) {
            do {
                world_tick_.fetch_add(1, std::memory_order_relaxed);
                next_world_tick += kTickDt;
            } while (now >= next_world_tick);
        }

        if (now >= next_diagnostics) {
            std::uint64_t total_ticks = 0;
            std::uint64_t total_records = 0;
            std::uint64_t total_empty_skips = 0;
            std::uint64_t total_migrations = 0;
            const auto attacks_per_sec = attacks_since_diag_.exchange(0);
            std::size_t active_zones = 0;
            std::size_t active_sessions = 0;
            std::size_t active_ghosts = 0;
            for (std::size_t i = 0; i < zones_.ZoneCount(); ++i) {
                auto& zone = zones_.GetZone(i);
                const auto player_count = zone.Diagnostics().player_count.load();
                const auto mob_count = zone.Diagnostics().mob_count.load();
                active_sessions += player_count;
                active_ghosts += zone.Diagnostics().ghost_count.load();
                if (player_count > 0 || mob_count > 0) {
                    ++active_zones;
                }
                total_ticks += zone.Diagnostics().ticks_since_diag.exchange(0);
                total_records += zone.Diagnostics().transform_records_since_diag.exchange(0);
                total_empty_skips += zone.Diagnostics().empty_skips_since_diag.exchange(0);
                total_migrations += zone.Diagnostics().migrations_since_diag.exchange(0);
            }
            std::size_t active_mobs = 0;
            std::size_t wandering_mobs = 0;
            std::size_t idle_mobs = 0;
            for (std::size_t i = 0; i < zones_.ZoneCount(); ++i) {
                auto& zone = zones_.GetZone(i);
                active_mobs += zone.Diagnostics().mob_count.load();
                wandering_mobs += zone.Diagnostics().wandering_mob_count.load();
                idle_mobs += zone.Diagnostics().idle_mob_count.load();
            }
            const std::size_t respawns_pending = respawns_.PendingCount();
            LOG_INFO("Game sim diag: world_tick={} zones={} active_zones={} active_sessions={} active_mobs={} wandering_mobs={} idle_mobs={} ghosts={} zone_ticks={} empty_zone_skips={} transform_records_sent={} attacks_per_sec={} deaths_total={} respawns_pending={} respawns_total={} migrations={} workers={}",
                     world_tick_.load(),
                     zones_.ZoneCount(),
                     active_zones,
                     active_sessions,
                     active_mobs,
                     wandering_mobs,
                     idle_mobs,
                     active_ghosts,
                     total_ticks,
                     total_empty_skips,
                     total_records,
                     attacks_per_sec,
                     deaths_total_.load(),
                     respawns_pending,
                     respawns_total_.load(),
                     total_migrations,
                     workers_.WorkerCount());
            do {
                next_diagnostics += std::chrono::seconds(1);
            } while (now >= next_diagnostics);
        }

        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(5), [this] {
            return stopping_.load() || !commands_.empty();
        });
    }

    DrainGlobalCommands();
    while (zones_.AnyTickInProgress()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    zones_.Clear();
    owners_by_session_.clear();
    respawns_.Clear();
    LOG_INFO("Game sim supervisor stopped");
}

void WorldRuntime::DrainGlobalCommands()
{
    for (;;) {
        std::function<void()> command;
        {
            std::lock_guard lock(mutex_);
            if (commands_.empty()) {
                return;
            }
            command = std::move(commands_.front());
            commands_.pop();
        }
        command();
    }
}

void WorldRuntime::ProcessMigrations()
{
    if (zones_.AnyTickInProgress()) {
        return;
    }

    struct MigrationPlan {
        std::uint32_t net_id = 0;
        std::size_t source_zone_index = 0;
        std::size_t target_zone_index = 0;
    };

    std::vector<MigrationPlan> plans;
    for (std::size_t zone_index = 0; zone_index < zones_.ZoneCount(); ++zone_index) {
        auto& zone = zones_.GetZone(zone_index);
        ZoneWriteGuard guard(zone, "migration scan");
        for (const auto& [net_id, binding] : zone.Players()) {
            if (!binding.session) {
                continue;
            }
            const auto owner_it = owners_by_session_.find(binding.session->Id());
            if (owner_it == owners_by_session_.end() || owner_it->second.net_id != net_id ||
                owner_it->second.zone_index != zone_index) {
                continue;
            }
            const auto entity = zone.FindEntity(net_id);
            if (!entity.is_valid()) {
                continue;
            }
            const auto migration = entity.get<MigrateTo>();
            if (migration.target_zone == 0) {
                continue;
            }
            const std::size_t target_index = zones_.FindIndexById(migration.target_zone);
            if (target_index >= zones_.ZoneCount() || target_index == zone_index) {
                continue;
            }
            plans.push_back(MigrationPlan{net_id, zone_index, target_index});
        }
        zone.World().query<const MobTag, const NetId, const MigrateTo>().each(
            [&](const MobTag&, const NetId& id, const MigrateTo& migration) {
                if (migration.target_zone == 0) {
                    return;
                }
                const std::size_t target_index = zones_.FindIndexById(migration.target_zone);
                if (target_index >= zones_.ZoneCount() || target_index == zone_index) {
                    return;
                }
                plans.push_back(MigrationPlan{id.value, zone_index, target_index});
            });
    }

    std::sort(plans.begin(), plans.end(), [](const MigrationPlan& lhs, const MigrationPlan& rhs) {
        return lhs.net_id < rhs.net_id;
    });

    for (const auto& plan : plans) {
        ExecuteMigration(plan.source_zone_index, plan.target_zone_index, plan.net_id);
    }
}

void WorldRuntime::ExecuteMigration(std::size_t source_zone_index,
                                    std::size_t target_zone_index,
                                    std::uint32_t net_id)
{
    if (source_zone_index >= zones_.ZoneCount() || target_zone_index >= zones_.ZoneCount() ||
        source_zone_index == target_zone_index || zones_.AnyTickInProgress()) {
        return;
    }

    auto migrate = [&]() {
        auto& source_zone = zones_.GetZone(source_zone_index);
        auto& target_zone = zones_.GetZone(target_zone_index);

        if (auto* binding = source_zone.FindPlayer(net_id)) {
            if (!binding->session) {
                return;
            }
            const auto owner_it = owners_by_session_.find(binding->session->Id());
            if (owner_it == owners_by_session_.end() || owner_it->second.net_id != net_id ||
                owner_it->second.zone_index != source_zone_index) {
                return;
            }

            const auto entity = source_zone.FindEntity(net_id);
            if (!entity.is_valid()) {
                return;
            }
            const auto migration = entity.get<MigrateTo>();
            if (migration.target_zone != target_zone.Id()) {
                return;
            }

            const auto position = entity.get<Position>();
            const std::size_t actual_target = zones_.FindIndexForPosition(position.x, position.y);
            if (actual_target != target_zone_index ||
                DistanceOutsideRect(source_zone.Bounds(), position) <= kMigrationHysteresisMeters) {
                entity.set<MigrateTo>({0});
                return;
            }

            GhostSystem::RemoveByNetId(target_zone, net_id);

            auto transfer = BuildTransfer(entity, true);
            auto moved_binding = source_zone.ExtractPlayerBinding(net_id);
            if (entity.is_valid()) {
                entity.destruct();
            }
            source_zone.UnindexEntity(net_id);
            source_zone.RefreshResidentCounts();

            transfer.position.z = terrain_.SampleGroundHeight(transfer.position.x, transfer.position.y);
            auto new_entity = ApplyTransfer(target_zone.World(), transfer);
            target_zone.IndexEntity(net_id, new_entity);
            const auto session_id = moved_binding.session ? moved_binding.session->Id() : 0;
            target_zone.InsertPlayerBinding(net_id, std::move(moved_binding));
            target_zone.RefreshResidentCounts();
            if (session_id != 0) {
                owners_by_session_[session_id] = OwnerInfo{target_zone_index, net_id};
            }

            source_zone.Diagnostics().migrations_since_diag.fetch_add(1, std::memory_order_relaxed);
            LOG_INFO("migration: net_id={} (player) from_zone={} to_zone={} tick={}",
                     net_id,
                     source_zone.Id(),
                     target_zone.Id(),
                     world_tick_.load(std::memory_order_relaxed));
            return;
        }

        const auto entity = source_zone.FindEntity(net_id);
        if (!entity.is_valid() || entity.has<PlayerTag>()) {
            return;
        }

        const auto migration = entity.get<MigrateTo>();
        if (migration.target_zone != target_zone.Id()) {
            return;
        }

        const auto position = entity.get<Position>();
        const std::size_t actual_target = zones_.FindIndexForPosition(position.x, position.y);
        if (actual_target != target_zone_index ||
            DistanceOutsideRect(source_zone.Bounds(), position) <= kMigrationHysteresisMeters) {
            entity.set<MigrateTo>({0});
            return;
        }

        GhostSystem::RemoveByNetId(target_zone, net_id);

        auto transfer = BuildTransfer(entity, false);
        auto moved_rng = source_zone.ExtractMobRng(net_id);
        if (entity.is_valid()) {
            entity.destruct();
        }
        source_zone.UnindexEntity(net_id);
        source_zone.EraseMobRng(net_id);
        source_zone.RefreshResidentCounts();

        transfer.position.z = terrain_.SampleGroundHeight(transfer.position.x, transfer.position.y);
        auto new_entity = ApplyTransfer(target_zone.World(), transfer);
        target_zone.IndexEntity(net_id, new_entity);
        if (moved_rng) {
            target_zone.InsertMobRng(net_id, std::move(*moved_rng));
        }
        target_zone.RefreshResidentCounts();

        source_zone.Diagnostics().migrations_since_diag.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO("migration: net_id={} (mob, type={}) from_zone={} to_zone={} tick={}",
                 net_id,
                 transfer.mob_type_id,
                 source_zone.Id(),
                 target_zone.Id(),
                 world_tick_.load(std::memory_order_relaxed));
    };

    if (source_zone_index < target_zone_index) {
        ZoneWriteGuard source_guard(zones_.GetZone(source_zone_index), "migration source");
        ZoneWriteGuard target_guard(zones_.GetZone(target_zone_index), "migration target");
        migrate();
    } else {
        ZoneWriteGuard target_guard(zones_.GetZone(target_zone_index), "migration target");
        ZoneWriteGuard source_guard(zones_.GetZone(source_zone_index), "migration source");
        migrate();
    }
}

void WorldRuntime::ProcessRespawns(float dt)
{
    if (zones_.AnyTickInProgress()) {
        return;
    }

    const auto ready = respawns_.TakeReady(dt);
    for (const auto spawn_point_index : ready) {
        if (SpawnMobFromSpawnPoint(spawn_point_index)) {
            respawns_total_.fetch_add(1, std::memory_order_relaxed);
            LOG_INFO("respawn: spawn_point_id={} total_respawns={}",
                     spawn_point_index,
                     respawns_total_.load(std::memory_order_relaxed));
        }
    }
}

void WorldRuntime::DrainMoveInputs()
{
    std::vector<MoveInput> inputs;
    {
        std::lock_guard lock(input_mutex_);
        inputs.swap(pending_inputs_);
    }

    for (const auto& input : inputs) {
        const auto owner_it = owners_by_session_.find(input.session_id);
        if (owner_it == owners_by_session_.end()) {
            continue;
        }
        const auto zone_index = owner_it->second.zone_index;
        EnqueueZoneCommand(zone_index, [input](Zone& zone) {
            AssertZoneOwner(zone, "zone input command");
            std::uint32_t net_id = 0;
            for (const auto& [candidate_net, binding] : zone.Players()) {
                if (binding.session && binding.session->Id() == input.session_id) {
                    net_id = candidate_net;
                    break;
                }
            }
            if (net_id == 0) {
                return;
            }
            const auto entity = zone.FindEntity(net_id);
            if (!entity.is_valid()) {
                return;
            }
            auto intent = entity.get<MoveIntent>();
            if (input.sequence < intent.last_input_seq) {
                return;
            }
            intent.dir_angle = input.dir_angle;
            intent.state = input.state;
            intent.last_input_seq = input.sequence;
            entity.set<MoveIntent>(intent);
        });
    }
}

void WorldRuntime::DrainAttackInputs()
{
    std::vector<AttackInput> inputs;
    {
        std::lock_guard lock(attack_mutex_);
        inputs.swap(pending_attacks_);
    }

    for (const auto& input : inputs) {
        const auto owner_it = owners_by_session_.find(input.session_id);
        if (owner_it == owners_by_session_.end()) {
            continue;
        }
        const auto zone_index = owner_it->second.zone_index;
        EnqueueZoneCommand(zone_index, [this, input](Zone& zone) {
            auto ctx = BuildZoneTickContext();
            const auto result = CombatSystem::ProcessAttack(zone, input.session_id, input.target_net_id, ctx);
            if (result.attacked) {
                attacks_since_diag_.fetch_add(1, std::memory_order_relaxed);
            }
            if (result.killed) {
                deaths_total_.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
}

Position WorldRuntime::ResolveSpawnPosition(const gs::db::Character& character,
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

bool WorldRuntime::IsValidDebugSpawnOverride(const DebugSpawnOverride& debug_spawn)
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

void WorldRuntime::Spawn(std::shared_ptr<gs::network::Session> session,
                         gs::db::Character character,
                         std::optional<DebugSpawnOverride> debug_spawn)
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

    owners_by_session_[session_id] = OwnerInfo{zone_index, net_id};
    SendToSession(io_,
                  session,
                  MakeEnterWorldAccept(net_id, position, world_tick_.load(std::memory_order_relaxed)));

    LOG_INFO("Session {} assigned to zone {} ('{}') as net_id {} at {}, {}, ground_z={}",
             session_id,
             zones_.GetZone(zone_index).Id(),
             zones_.GetZone(zone_index).Name(),
             net_id,
             position.x,
             position.y,
             position.z);

    EnqueueZoneCommand(zone_index,
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

void WorldRuntime::Despawn(gs::common::SessionId session_id)
{
    const auto owner_it = owners_by_session_.find(session_id);
    if (owner_it == owners_by_session_.end()) {
        LOG_INFO("Sim despawn requested for session {}, but no in-world player was found", session_id);
        return;
    }

    const auto zone_index = owner_it->second.zone_index;
    const auto net_id = owner_it->second.net_id;
    owners_by_session_.erase(owner_it);

    EnqueueZoneCommand(zone_index, [this, session_id, net_id](Zone& zone) {
        AssertZoneOwner(zone, "zone despawn command");
        std::uint32_t found_net = 0;
        for (const auto& [candidate_net, binding] : zone.Players()) {
            if (binding.session && binding.session->Id() == session_id) {
                found_net = candidate_net;
                break;
            }
        }
        if (found_net == 0) {
            return;
        }

        const auto entity = zone.FindEntity(found_net);
        if (entity.is_valid()) {
            entity.destruct();
        }
        zone.UnindexEntity(found_net);
        zone.ErasePlayerBinding(found_net);
        LOG_INFO("Session {} despawned net_id {} from zone {}", session_id, net_id, zone.Id());

        auto ctx = BuildZoneTickContext();
        auto payload = MakeDespawn(net_id);
        for (auto& [viewer_net, viewer] : zone.Players()) {
            (void)viewer_net;
            if (viewer.visible_net_ids.erase(net_id) > 0) {
                ctx.send(viewer.session, payload);
            }
        }
        zone.RefreshResidentCounts();
    });
}

void WorldRuntime::SpawnConfiguredMobs()
{
    if (mob_types_.Empty() || spawn_points_.empty() || zones_.ZoneCount() == 0) {
        LOG_INFO("Mob spawn skipped: types={} spawn_points={} zones={}",
                 mob_types_.Size(),
                 spawn_points_.size(),
                 zones_.ZoneCount());
        return;
    }

    std::uint32_t total = 0;
    for (std::size_t spawn_index = 0; spawn_index < spawn_points_.size(); ++spawn_index) {
        const auto& spawn = spawn_points_[spawn_index];
        for (std::uint32_t i = 0; i < spawn.count; ++i) {
            if (SpawnMobFromSpawnPoint(spawn_index)) {
                ++total;
            }
        }
    }

    LOG_INFO("spawn complete: total_mobs={}", total);
}

bool WorldRuntime::SpawnMobFromSpawnPoint(std::size_t spawn_point_index)
{
    if (spawn_point_index >= spawn_points_.size()) {
        return false;
    }

    const auto& spawn = spawn_points_[spawn_point_index];
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

} // namespace gs::game
