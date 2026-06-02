#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <unordered_map>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <flecs.h>

#include "db/CharacterRepository.h"
#include "map/MapData.h"
#include "network/Session.h"

namespace gs::game {

struct Position {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Heading {
    float angle = 0.0f;
};

struct Velocity {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

enum class MoveState : std::uint8_t {
    Idle = 0,
    Walking = 1,
    Running = 2,
};

struct MoveIntent {
    float dir_angle = 0.0f;
    MoveState state = MoveState::Idle;
    std::uint32_t last_input_seq = 0;
};

struct MoveSpeed {
    float walk = 3.0f;
    float run = 6.0f;
};

struct NetId {
    std::uint32_t value = 0;
};

struct SessionRef {
    gs::common::SessionId session = 0;
};

struct PlayerTag {
};

struct MobTag {
};

struct MobTypeRef {
    std::uint32_t id = 0;
};

struct Hp {
    float current = 1.0f;
    float max = 1.0f;
};

struct CombatStats {
    float damage = 1.0f;
    float defense = 0.0f;
    float attack_range = 2.0f;
    float attack_cooldown = 1.0f;
};

struct AttackCooldown {
    float remaining = 0.0f;
};

struct WanderState {
    enum class Mode : std::uint8_t {
        Idle = 0,
        Moving = 1,
    };

    Mode mode = Mode::Idle;
    Vec2 target;
    float timer = 0.0f;
    Vec2 spawn_center;
    float spawn_radius = 0.0f;
};

struct GhostTag {
};

struct MigrateTo {
    std::uint32_t target_zone = 0;
};

struct DebugSpawnOverride {
    float x = 0.0f;
    float y = 0.0f;
};

struct SimPlayer;
struct SimMob;

class SimWorld {
public:
    explicit SimWorld(boost::asio::io_context& io);
    ~SimWorld();

    SimWorld(const SimWorld&) = delete;
    SimWorld& operator=(const SimWorld&) = delete;

    void Start();
    void Stop();

    void PostSpawn(std::shared_ptr<gs::network::Session> session,
                   gs::db::Character character,
                   std::optional<DebugSpawnOverride> debug_spawn = std::nullopt);
    void PostDespawn(gs::common::SessionId session_id);
    void PostMoveInput(gs::common::SessionId session_id,
                       std::uint32_t sequence,
                       float dir_angle,
                       MoveState state);
    void PostAttackTarget(gs::common::SessionId session_id,
                          std::uint32_t target_net_id);

private:
    struct ZoneRuntime;
    struct OwnerInfo;
    struct ZoneTickScope;
    struct AoiEntityRef;

    struct MoveInput {
        gs::common::SessionId session_id = 0;
        std::uint32_t sequence = 0;
        float dir_angle = 0.0f;
        MoveState state = MoveState::Idle;
    };

    struct AttackInput {
        gs::common::SessionId session_id = 0;
        std::uint32_t target_net_id = 0;
    };

    void Enqueue(std::function<void()> command);
    void Run();
    void WorkerLoop();
    void StopWorkers();
    void DrainGlobalCommands();
    void ProcessMigrations();
    void ProcessRespawns(float dt);
    bool AnyZoneTickInProgress() const;
    void DrainMoveInputs();
    void DrainAttackInputs();
    void ScheduleZones();
    void TickZone(ZoneRuntime& zone, float dt);
    void DrainZoneCommands(ZoneRuntime& zone);
    void StepCooldowns(ZoneRuntime& zone, float dt);
    void StepWanderAi(ZoneRuntime& zone, float dt);
    void StepMovement(ZoneRuntime& zone, float dt);
    void ProcessAttackCommand(ZoneRuntime& zone,
                              gs::common::SessionId attacker_session_id,
                              std::uint32_t target_net_id);
    float SampleGroundHeight(float world_x, float world_y) const;
    bool IsWalkable(float world_x, float world_y) const;
    Position ResolveSpawnPosition(const gs::db::Character& character,
                                  std::optional<DebugSpawnOverride> debug_spawn,
                                  gs::common::SessionId session_id) const;
    bool IsValidDebugSpawnOverride(const DebugSpawnOverride& debug_spawn) const;
    void TryApplyWarp(ZoneRuntime& zone, SimPlayer& player);
    void UpdateMigrationMarker(ZoneRuntime& zone, SimPlayer& player);
    void UpdateMigrationMarker(ZoneRuntime& zone, SimMob& mob);
    std::size_t FindZoneIndexById(std::uint32_t zone_id) const;
    void ExecuteMigration(std::size_t source_zone_index,
                          std::size_t target_zone_index,
                          std::uint32_t net_id);
    void RemoveGhostByNetId(ZoneRuntime& zone, std::uint32_t net_id);
    float WorldExtentMeters() const;
    void BuildZones();
    void LoadMobTypes();
    void LoadMobSpawns();
    void SpawnConfiguredMobs();
    bool SpawnMobFromSpawnPoint(std::size_t spawn_point_index);
    std::uint32_t AllocatePlayerNetId();
    std::uint32_t AllocateMobNetId();
    std::size_t FindZoneIndexForPosition(float world_x, float world_y) const;
    void PublishBorderSnapshot(ZoneRuntime& zone);
    void RebuildGhosts(ZoneRuntime& zone);
    void ClearGhosts(ZoneRuntime& zone);
    bool IsInBorderBand(const ZoneRuntime& zone, const Position& position) const;
    bool IsResidentInZone(const ZoneRuntime& zone, std::uint32_t net_id) const;
    void RebuildSpatialGrid(ZoneRuntime& zone);
    std::vector<AoiEntityRef> QueryAoiCandidates(const ZoneRuntime& zone, const SimPlayer& viewer) const;
    std::size_t BroadcastTransforms(ZoneRuntime& zone);
    void Spawn(std::shared_ptr<gs::network::Session> session,
               gs::db::Character character,
               std::optional<DebugSpawnOverride> debug_spawn);
    void Despawn(gs::common::SessionId session_id);
    void EnqueueZoneCommand(std::size_t zone_index, std::function<void(ZoneRuntime&)> command);
    void EnqueueWorkerTask(std::size_t zone_index);
    void AssertZoneOwner(const ZoneRuntime& zone, const char* operation) const;
    [[noreturn]] void FailZoneOwnerCheck(const ZoneRuntime& zone,
                                         const char* operation,
                                         std::thread::id owner,
                                         std::thread::id caller) const;

    boost::asio::io_context& io_;
    std::thread thread_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
    std::thread::id sim_thread_id_{};

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> commands_;

    std::mutex input_mutex_;
    std::vector<MoveInput> pending_inputs_;
    std::mutex attack_mutex_;
    std::vector<AttackInput> pending_attacks_;

    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    std::queue<std::size_t> worker_tasks_;

    std::vector<std::unique_ptr<ZoneRuntime>> zones_;
    std::unordered_map<gs::common::SessionId, OwnerInfo> owners_by_session_;
    mx::map::HeightField terrain_;
    mx::map::WorldLogic world_logic_;
    std::uint32_t next_net_id_ = 1;
    std::uint32_t next_mob_net_id_ = 1'000'000;
    struct MobTypeDefinition {
        std::uint32_t id = 0;
        std::string name;
        std::uint32_t model_id = 0;
        std::uint32_t hp_max = 1;
        std::uint32_t damage = 0;
        float speed = 0.0f;
        float wander_speed = 1.5f;
        float wander_idle_min = 3.0f;
        float wander_idle_max = 8.0f;
        float defense = 0.0f;
        float attack_range = 2.0f;
        float attack_cooldown = 1.5f;
        float respawn_time_sec = 30.0f;
    };
    struct MobSpawnPoint {
        std::uint32_t mob_type_id = 0;
        float x = 0.0f;
        float y = 0.0f;
        std::uint32_t count = 0;
        float radius = 0.0f;
    };
    struct RespawnPending {
        std::size_t spawn_point_index = 0;
        float remaining_sec = 0.0f;
    };
    std::unordered_map<std::uint32_t, MobTypeDefinition> mob_types_;
    std::vector<MobSpawnPoint> mob_spawn_points_;
    std::mutex respawn_mutex_;
    std::vector<RespawnPending> respawns_pending_;
    std::atomic<std::uint64_t> attacks_since_diag_{0};
    std::atomic<std::uint64_t> deaths_total_{0};
    std::atomic<std::uint64_t> respawns_total_{0};
    std::atomic<std::uint32_t> world_tick_{0};
};

} // namespace gs::game
