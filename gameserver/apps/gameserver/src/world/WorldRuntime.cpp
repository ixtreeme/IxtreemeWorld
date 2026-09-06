#include "WorldRuntime.h"

#include <chrono>
#include <filesystem>
#include <fstream>

#include "common/Logging.h"

#include "replication/NetworkSend.h"
#include "systems/CombatSystem.h"
#include "zone/ZoneOwnership.h"
#include "WorldConstants.h"

namespace gs::game {
namespace {

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
    , spawn_(io_,
             zones_,
             terrain_,
             world_logic_,
             owners_by_session_,
             [this](std::size_t zone_index, ZoneCommandQueue::Command command) {
                 zones_.PostCommand(zone_index, std::move(command));
                 cv_.notify_one();
             },
             [this](std::shared_ptr<gs::network::Session> session, std::vector<std::uint8_t> payload) {
                 SendToSession(io_, session, std::move(payload));
             })
    , migration_(zones_, terrain_, owners_by_session_)
    , inputs_(
          [this](std::size_t zone_index, ZoneCommandQueue::Command command) {
              zones_.PostCommand(zone_index, std::move(command));
              cv_.notify_one();
          },
          [this] {
              cv_.notify_one();
          })
{
    const std::string map_root = IXTREEME_DEFAULT_MAP_ROOT;
    terrain_ = TerrainService::LoadFromMapRoot(map_root);
    world_logic_ = LoadWorldLogicFromMapRoot(map_root);

    zones_.BuildFromWorldLogic(world_logic_, terrain_.WorldExtentMeters());
    spawn_.Initialize(map_root, IXTREEME_DEFAULT_MOB_TYPES_CONFIG);
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
        spawn_.Spawn(std::move(session), std::move(character), debug_spawn, world_tick_.load());
    });
}

void WorldRuntime::PostDespawn(gs::common::SessionId session_id)
{
    Enqueue([this, session_id] {
        spawn_.Despawn(session_id);
    });
}

void WorldRuntime::PostMoveInput(gs::common::SessionId session_id,
                                 std::uint32_t sequence,
                                 float dir_angle,
                                 MoveState state)
{
    inputs_.PostMoveInput(session_id, sequence, dir_angle, state);
}

void WorldRuntime::PostAttackTarget(gs::common::SessionId session_id, std::uint32_t target_net_id)
{
    inputs_.PostAttackTarget(session_id, target_net_id);
}

void WorldRuntime::Enqueue(std::function<void()> command)
{
    {
        std::lock_guard lock(mutex_);
        commands_.push(std::move(command));
    }
    cv_.notify_one();
}

ZoneTickContext WorldRuntime::BuildZoneTickContext()
{
    ZoneTickContext ctx{terrain_,
                        world_logic_,
                        spawn_.MobTypes(),
                        zones_,
                        world_tick_.load(std::memory_order_relaxed),
                        [this](std::shared_ptr<gs::network::Session> session, std::vector<std::uint8_t> payload) {
                            SendToSession(io_, session, std::move(payload));
                        },
                        [this](std::size_t spawn_point_index, float delay_sec) {
                            spawn_.ScheduleRespawn(spawn_point_index, delay_sec);
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
        migration_.ProcessMigrations(world_tick_.load(std::memory_order_relaxed));
        spawn_.ProcessRespawns(kTickDtSeconds);
        inputs_.DrainMoves(owners_by_session_);
        inputs_.DrainAttacks(owners_by_session_,
                             [this](Zone& zone,
                                     gs::common::SessionId attacker,
                                     std::uint32_t target_net_id) {
                                 auto ctx = BuildZoneTickContext();
                                 const auto result =
                                     CombatSystem::ProcessAttack(zone, attacker, target_net_id, ctx);
                                 if (result.attacked) {
                                     attacks_since_diag_.fetch_add(1, std::memory_order_relaxed);
                                 }
                                 if (result.killed) {
                                     deaths_total_.fetch_add(1, std::memory_order_relaxed);
                                 }
                             });
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
            std::uint64_t total_tick_micros = 0;
            std::uint64_t total_aoi_queries = 0;
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
                total_tick_micros += zone.Diagnostics().tick_micros_since_diag.exchange(0);
                total_aoi_queries += zone.Diagnostics().aoi_queries_since_diag.exchange(0);
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
            const double avg_tick_ms =
                total_ticks > 0 ? static_cast<double>(total_tick_micros) / total_ticks / 1000.0 : 0.0;
            LOG_INFO("Game sim diag: world_tick={} zones={} active_zones={} active_sessions={} active_mobs={} wandering_mobs={} idle_mobs={} ghosts={} zone_ticks={} empty_zone_skips={} transform_records_sent={} attacks_per_sec={} deaths_total={} respawns_pending={} respawns_total={} migrations={} workers={} avg_zone_tick_ms={:.3f} aoi_queries={}",
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
                     spawn_.RespawnsPending(),
                     spawn_.RespawnsTotal(),
                     total_migrations,
                     workers_.WorkerCount(),
                     avg_tick_ms,
                     total_aoi_queries);
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
    spawn_.ClearRespawns();
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
        // A failing global command must not kill the supervisor loop.
        try {
            command();
        } catch (const std::exception& error) {
            LOG_ERROR("Game sim global command failed: {}", error.what());
        } catch (...) {
            LOG_ERROR("Game sim global command failed with unknown exception");
        }
    }
}

} // namespace gs::game
