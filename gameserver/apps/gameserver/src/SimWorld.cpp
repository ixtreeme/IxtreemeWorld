#include "SimWorld.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

#include <boost/asio/post.hpp>
#include <capnp/message.h>

#include "common/Logging.h"
#include "db/Types.h"
#include "protocol/Serialization.h"
#include "schema/packet.capnp.h"

namespace gs::game {
namespace {

constexpr float kDbUnitsPerMeter = 1000.0f;
constexpr float kAoiRadiusMeters = 120.0f;
constexpr float kAoiRadiusSqMeters = kAoiRadiusMeters * kAoiRadiusMeters;
constexpr float kSpatialCellSizeMeters = kAoiRadiusMeters;
constexpr std::size_t kAoiEntityCap = 100;
constexpr float kMigrationHysteresisMeters = 5.0f;
constexpr auto kTickDt = std::chrono::milliseconds(50);
constexpr float kTickDtSeconds = 0.05f;

#ifndef MMO_ZONE_OWNER_CHECK
#define MMO_ZONE_OWNER_CHECK 1
#endif

#ifndef MMO_DEBUG_SPAWN_OVERRIDE
#define MMO_DEBUG_SPAWN_OVERRIDE 0
#endif

struct BorderEntitySnapshot {
    std::uint32_t net_id = 0;
    Position position;
    Heading heading;
    MoveState move_state = MoveState::Idle;
    std::string name;
    std::uint16_t class_id = 0;
};

float DbToMeters(std::int32_t value)
{
    return static_cast<float>(value) / kDbUnitsPerMeter;
}

int SpatialCellCoord(float value)
{
    return static_cast<int>(std::floor(value / kSpatialCellSizeMeters));
}

std::int64_t SpatialCellKey(int x, int y)
{
    const auto packed = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
                        static_cast<std::uint32_t>(y);
    return static_cast<std::int64_t>(packed);
}

bool RectsTouchOrOverlap(const mx::map::Rect& lhs, const mx::map::Rect& rhs)
{
    constexpr float kEpsilon = 0.001f;
    return lhs.min_x <= rhs.max_x + kEpsilon && lhs.max_x + kEpsilon >= rhs.min_x &&
           lhs.min_y <= rhs.max_y + kEpsilon && lhs.max_y + kEpsilon >= rhs.min_y;
}

float DistanceOutsideRect(const mx::map::Rect& rect, const Position& position)
{
    float distance = 0.0f;
    if (position.x < rect.min_x) {
        distance = std::max(distance, rect.min_x - position.x);
    } else if (position.x > rect.max_x) {
        distance = std::max(distance, position.x - rect.max_x);
    }

    if (position.y < rect.min_y) {
        distance = std::max(distance, rect.min_y - position.y);
    } else if (position.y > rect.max_y) {
        distance = std::max(distance, position.y - rect.max_y);
    }
    return distance;
}

std::uint16_t QuantizeHeading(float angle)
{
    constexpr float kTwoPi = 6.28318530717958647692f;
    while (angle < 0.0f) {
        angle += kTwoPi;
    }
    while (angle >= kTwoPi) {
        angle -= kTwoPi;
    }
    return static_cast<std::uint16_t>(std::lround((angle / kTwoPi) * 65535.0f));
}

void FillVec3(gs::protocol::Vec3::Builder out, const Position& pos)
{
    out.setX(pos.x);
    out.setY(pos.y);
    out.setZ(pos.z);
}

void WriteU16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void WriteU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

void WriteF32(std::vector<std::uint8_t>& out, float value)
{
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    WriteU32(out, bits);
}

std::vector<std::uint8_t> MakeEnterWorldAccept(std::uint32_t net_id,
                                               const Position& pos,
                                               std::uint32_t world_tick)
{
    capnp::MallocMessageBuilder msg;
    auto packet = msg.initRoot<gs::protocol::Packet>();
    auto accept = packet.initEnterWorldAccept();
    accept.setYourNetId(net_id);
    FillVec3(accept.initSpawnPos(), pos);
    accept.setServerTick(world_tick);
    return gs::protocol::SerializeToBytes(msg);
}

std::vector<std::uint8_t> MakeSpawn(const SimPlayer& player);

std::vector<std::uint8_t> MakeDespawn(std::uint32_t net_id)
{
    capnp::MallocMessageBuilder msg;
    auto packet = msg.initRoot<gs::protocol::Packet>();
    auto despawn = packet.initEntityDespawn();
    despawn.setNetId(net_id);
    return gs::protocol::SerializeToBytes(msg);
}

void Send(boost::asio::io_context& io,
          const std::shared_ptr<gs::network::Session>& session,
          std::vector<std::uint8_t> payload)
{
    boost::asio::post(io, [session, payload = std::move(payload)]() mutable {
        session->SendPayload(std::move(payload));
    });
}

} // namespace

struct SimPlayer {
    flecs::entity entity;
    std::shared_ptr<gs::network::Session> session;
    gs::db::Character character;
    Position position;
    Heading heading;
    Velocity velocity;
    MoveIntent move_intent;
    MoveSpeed move_speed;
    std::uint32_t net_id = 0;
    std::unordered_set<std::uint32_t> visible_net_ids;
};

struct GhostEntity {
    flecs::entity entity;
    BorderEntitySnapshot snapshot;
};

struct SimWorld::AoiEntityRef {
    bool ghost = false;
    std::size_t index = 0;
};

struct SimWorld::OwnerInfo {
    std::size_t zone_index = 0;
    std::uint32_t net_id = 0;
};

struct SimWorld::ZoneRuntime {
    std::uint32_t id = 0;
    std::string name;
    mx::map::Rect bounds;
    std::unique_ptr<flecs::world> world;
    std::vector<SimPlayer> players;
    std::vector<GhostEntity> ghosts;
    std::unordered_map<std::int64_t, std::vector<AoiEntityRef>> spatial_grid;
    std::array<std::vector<BorderEntitySnapshot>, 2> publish_buffers;
    std::vector<std::size_t> neighbor_indices;

    std::mutex commands_mutex;
    std::queue<std::function<void(ZoneRuntime&)>> commands;

    std::atomic<std::thread::id> owner_thread_id{std::thread::id{}};
    std::atomic<bool> tick_in_progress{false};
    std::chrono::steady_clock::time_point next_tick{};
    std::uint32_t zone_tick = 0;

    std::atomic<std::uint32_t> player_count{0};
    std::atomic<std::uint32_t> ghost_count{0};
    std::atomic<std::uint64_t> ticks_since_diag{0};
    std::atomic<std::uint64_t> transform_records_since_diag{0};
    std::atomic<std::uint64_t> empty_skips_since_diag{0};
    std::atomic<std::uint64_t> migrations_since_diag{0};
};

struct SimWorld::ZoneTickScope {
    SimWorld& world;
    ZoneRuntime& zone;

    ZoneTickScope(SimWorld& owner, ZoneRuntime& runtime)
        : world(owner)
        , zone(runtime)
    {
#if MMO_ZONE_OWNER_CHECK
        const auto caller = std::this_thread::get_id();
        auto expected_owner = std::thread::id{};
        if (!zone.owner_thread_id.compare_exchange_strong(expected_owner,
                                                          caller,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
            world.FailZoneOwnerCheck(zone, "ZoneTickScope acquire", expected_owner, caller);
        }
        world.AssertZoneOwner(zone, "ZoneTickScope acquired");
#endif
    }

    ZoneTickScope(const ZoneTickScope&) = delete;
    ZoneTickScope& operator=(const ZoneTickScope&) = delete;

    ~ZoneTickScope()
    {
#if MMO_ZONE_OWNER_CHECK
        world.AssertZoneOwner(zone, "ZoneTickScope release");
        zone.owner_thread_id.store(std::thread::id{}, std::memory_order_release);
#endif
    }
};

namespace {

BorderEntitySnapshot SnapshotFromPlayer(const SimPlayer& player)
{
    return BorderEntitySnapshot{player.net_id,
                                player.position,
                                player.heading,
                                player.move_intent.state,
                                player.character.name,
                                player.character.class_id};
}

std::vector<std::uint8_t> MakeSpawn(const BorderEntitySnapshot& snapshot)
{
    capnp::MallocMessageBuilder msg;
    auto packet = msg.initRoot<gs::protocol::Packet>();
    auto spawn = packet.initEntitySpawn();
    spawn.setNetId(snapshot.net_id);
    spawn.setName(snapshot.name);
    spawn.setClassId(snapshot.class_id);
    FillVec3(spawn.initSpawnPos(), snapshot.position);
    spawn.setHeading(QuantizeHeading(snapshot.heading.angle));
    return gs::protocol::SerializeToBytes(msg);
}

std::vector<std::uint8_t> MakeSpawn(const SimPlayer& player)
{
    return MakeSpawn(SnapshotFromPlayer(player));
}

void WriteTransformRecord(std::vector<std::uint8_t>& payload, const BorderEntitySnapshot& snapshot)
{
    WriteU32(payload, snapshot.net_id);
    WriteF32(payload, snapshot.position.x);
    WriteF32(payload, snapshot.position.y);
    WriteF32(payload, snapshot.position.z);
    WriteU16(payload, QuantizeHeading(snapshot.heading.angle));
    payload.push_back(static_cast<std::uint8_t>(snapshot.move_state));
}

void WriteTransformRecord(std::vector<std::uint8_t>& payload, const SimPlayer& player)
{
    WriteTransformRecord(payload, SnapshotFromPlayer(player));
}

void RegisterFlecsComponents(flecs::world& world)
{
    world.component<Position>();
    world.component<Heading>();
    world.component<NetId>();
    world.component<SessionRef>();
    world.component<PlayerTag>();
    world.component<GhostTag>();
    world.component<MigrateTo>();
}

} // namespace

SimWorld::SimWorld(boost::asio::io_context& io)
    : io_(io)
{
    const std::filesystem::path map_root = IXTREEME_DEFAULT_MAP_ROOT;
    auto field = mx::map::LoadHeightField(
        [&map_root](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
            std::filesystem::path normalized(path);
            std::ifstream file(map_root / normalized.relative_path(), std::ios::binary | std::ios::ate);
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
    if (field) {
        terrain_ = std::move(*field);
        LOG_INFO("Game sim loaded map '{}' cells={} cellSize={}m from {}",
                 terrain_.manifest.world_id,
                 terrain_.manifest.world_size_cells,
                 terrain_.manifest.cell_size_meters,
                 map_root.string());
    } else {
        LOG_WARN("Game sim map load failed from {}; using flat 1000m fallback", map_root.string());
    }

    auto logic = mx::map::LoadWorldLogic(
        [&map_root](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
            std::filesystem::path normalized(path);
            std::ifstream file(map_root / normalized.relative_path(), std::ios::binary | std::ios::ate);
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
        world_logic_ = std::move(*logic);
        LOG_INFO("Game sim loaded worldlogic: zones={} spawns={} warps={}",
                 world_logic_.zones.size(),
                 world_logic_.spawns.size(),
                 world_logic_.warps.size());
    } else {
        LOG_WARN("Game sim worldlogic load failed from {}; using fallback single zone", map_root.string());
    }

    BuildZones();
}

SimWorld::~SimWorld()
{
    Stop();
}

void SimWorld::Start()
{
    if (thread_.joinable()) {
        return;
    }

    stopping_ = false;
    thread_ = std::thread([this] {
        Run();
    });
}

void SimWorld::Stop()
{
    stopping_ = true;
    cv_.notify_all();
    worker_cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    StopWorkers();
}

void SimWorld::PostSpawn(std::shared_ptr<gs::network::Session> session,
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

void SimWorld::PostDespawn(gs::common::SessionId session_id)
{
    Enqueue([this, session_id] {
        Despawn(session_id);
    });
}

void SimWorld::PostMoveInput(gs::common::SessionId session_id,
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

void SimWorld::Enqueue(std::function<void()> command)
{
    {
        std::lock_guard lock(mutex_);
        commands_.push(std::move(command));
    }
    cv_.notify_one();
}

void SimWorld::BuildZones()
{
    zones_.clear();
    if (!world_logic_.zones.empty()) {
        zones_.reserve(world_logic_.zones.size());
        for (const auto& logic_zone : world_logic_.zones) {
            auto zone = std::make_unique<ZoneRuntime>();
            zone->id = logic_zone.id;
            zone->name = logic_zone.name;
            zone->bounds = logic_zone.bounds;
            zone->world = std::make_unique<flecs::world>();
            RegisterFlecsComponents(*zone->world);
            zones_.push_back(std::move(zone));
        }
    } else {
        auto zone = std::make_unique<ZoneRuntime>();
        zone->id = 1;
        zone->name = "fallback";
        const float extent = WorldExtentMeters();
        zone->bounds = mx::map::Rect{0.0f, 0.0f, extent, extent};
        zone->world = std::make_unique<flecs::world>();
        RegisterFlecsComponents(*zone->world);
        zones_.push_back(std::move(zone));
    }

    const auto now = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < zones_.size(); ++i) {
        zones_[i]->neighbor_indices.clear();
        for (std::size_t j = 0; j < zones_.size(); ++j) {
            if (i == j) {
                continue;
            }
            if (RectsTouchOrOverlap(zones_[i]->bounds, zones_[j]->bounds)) {
                zones_[i]->neighbor_indices.push_back(j);
            }
        }
    }

    for (auto& zone : zones_) {
        zone->next_tick = now;
        LOG_INFO("Game sim zone registered: id={} name='{}' bounds=({}, {})-({}, {}) neighbors={}",
                 zone->id,
                 zone->name,
                 zone->bounds.min_x,
                 zone->bounds.min_y,
                 zone->bounds.max_x,
                 zone->bounds.max_y,
                 zone->neighbor_indices.size());
    }
}

void SimWorld::Run()
{
    sim_thread_id_ = std::this_thread::get_id();

    const unsigned int hardware_threads = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t worker_count =
        std::max<std::size_t>(1, std::min<std::size_t>(zones_.size(), hardware_threads > 1 ? hardware_threads - 1 : 1));
    workers_.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this] {
            WorkerLoop();
        });
    }

    LOG_INFO("Game sim supervisor started: zones={} workers={} aoi_radius={} aoi_cap={}",
             zones_.size(),
             workers_.size(),
             kAoiRadiusMeters,
             kAoiEntityCap);
    auto next_world_tick = std::chrono::steady_clock::now() + kTickDt;
    auto next_diagnostics = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    while (!stopping_) {
        DrainGlobalCommands();
        ProcessMigrations();
        DrainMoveInputs();
        ScheduleZones();

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
            std::size_t active_zones = 0;
            std::size_t active_sessions = 0;
            std::size_t active_ghosts = 0;
            for (const auto& zone : zones_) {
                const auto player_count = zone->player_count.load();
                active_sessions += player_count;
                active_ghosts += zone->ghost_count.load();
                if (player_count > 0) {
                    ++active_zones;
                }
                total_ticks += zone->ticks_since_diag.exchange(0);
                total_records += zone->transform_records_since_diag.exchange(0);
                total_empty_skips += zone->empty_skips_since_diag.exchange(0);
                total_migrations += zone->migrations_since_diag.exchange(0);
            }
            LOG_INFO("Game sim diag: world_tick={} zones={} active_zones={} active_sessions={} ghosts={} zone_ticks={} empty_zone_skips={} transform_records_sent={} migrations={} workers={}",
                     world_tick_.load(),
                     zones_.size(),
                     active_zones,
                     active_sessions,
                     active_ghosts,
                     total_ticks,
                     total_empty_skips,
                     total_records,
                     total_migrations,
                     workers_.size());
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
    for (auto& zone : zones_) {
        while (zone->tick_in_progress.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        zone->players.clear();
        zone->ghosts.clear();
        zone->spatial_grid.clear();
        for (auto& buffer : zone->publish_buffers) {
            buffer.clear();
        }
        zone->player_count = 0;
        zone->ghost_count = 0;
    }
    owners_by_session_.clear();
    LOG_INFO("Game sim supervisor stopped");
}

void SimWorld::StopWorkers()
{
    worker_cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

void SimWorld::WorkerLoop()
{
    while (!stopping_) {
        std::size_t zone_index = 0;
        {
            std::unique_lock lock(worker_mutex_);
            worker_cv_.wait(lock, [this] {
                return stopping_.load() || !worker_tasks_.empty();
            });
            if (stopping_.load()) {
                return;
            }
            zone_index = worker_tasks_.front();
            worker_tasks_.pop();
        }

        if (zone_index >= zones_.size()) {
            continue;
        }
        TickZone(*zones_[zone_index], kTickDtSeconds);
    }
}

void SimWorld::DrainGlobalCommands()
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

bool SimWorld::AnyZoneTickInProgress() const
{
    return std::any_of(zones_.begin(), zones_.end(), [](const auto& zone) {
        return zone->tick_in_progress.load(std::memory_order_acquire);
    });
}

void SimWorld::ProcessMigrations()
{
    if (AnyZoneTickInProgress()) {
        return;
    }

    struct MigrationPlan {
        std::uint32_t net_id = 0;
        std::size_t source_zone_index = 0;
        std::size_t target_zone_index = 0;
    };

    std::vector<MigrationPlan> plans;
    for (std::size_t zone_index = 0; zone_index < zones_.size(); ++zone_index) {
        auto& zone = *zones_[zone_index];
        ZoneTickScope scope(*this, zone);
        for (const auto& player : zone.players) {
            if (!player.session) {
                continue;
            }
            const auto owner_it = owners_by_session_.find(player.session->Id());
            if (owner_it == owners_by_session_.end() || owner_it->second.net_id != player.net_id ||
                owner_it->second.zone_index != zone_index) {
                continue;
            }
            const auto migration = player.entity.get<MigrateTo>();
            if (migration.target_zone == 0) {
                continue;
            }
            const std::size_t target_index = FindZoneIndexById(migration.target_zone);
            if (target_index >= zones_.size() || target_index == zone_index) {
                continue;
            }
            plans.push_back(MigrationPlan{player.net_id, zone_index, target_index});
        }
    }

    std::sort(plans.begin(), plans.end(), [](const MigrationPlan& lhs, const MigrationPlan& rhs) {
        return lhs.net_id < rhs.net_id;
    });

    for (const auto& plan : plans) {
        ExecuteMigration(plan.source_zone_index, plan.target_zone_index, plan.net_id);
    }
}

void SimWorld::DrainMoveInputs()
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
        EnqueueZoneCommand(zone_index, [this, input](ZoneRuntime& zone) {
            AssertZoneOwner(zone, "zone input command");
            auto it = std::find_if(zone.players.begin(), zone.players.end(), [&input](const SimPlayer& player) {
                return player.session && player.session->Id() == input.session_id;
            });
            if (it == zone.players.end()) {
                return;
            }
            if (input.sequence < it->move_intent.last_input_seq) {
                return;
            }
            it->move_intent.dir_angle = input.dir_angle;
            it->move_intent.state = input.state;
            it->move_intent.last_input_seq = input.sequence;
        });
    }
}

void SimWorld::ScheduleZones()
{
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < zones_.size(); ++i) {
        auto& zone = *zones_[i];
        bool has_commands = false;
        {
            std::lock_guard lock(zone.commands_mutex);
            has_commands = !zone.commands.empty();
        }

        if (zone.player_count.load() == 0 && !has_commands) {
            zone.next_tick = now + kTickDt;
            zone.empty_skips_since_diag.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        if (now < zone.next_tick && !has_commands) {
            continue;
        }

        bool expected = false;
        if (!zone.tick_in_progress.compare_exchange_strong(expected, true)) {
            continue;
        }

        do {
            zone.next_tick += kTickDt;
        } while (zone.next_tick <= now);
        EnqueueWorkerTask(i);
    }
}

void SimWorld::EnqueueWorkerTask(std::size_t zone_index)
{
    {
        std::lock_guard lock(worker_mutex_);
        worker_tasks_.push(zone_index);
    }
    worker_cv_.notify_one();
}

void SimWorld::AssertZoneOwner(const ZoneRuntime& zone, const char* operation) const
{
#if MMO_ZONE_OWNER_CHECK
    const auto owner = zone.owner_thread_id.load(std::memory_order_acquire);
    const auto caller = std::this_thread::get_id();
    if (owner != caller) {
        FailZoneOwnerCheck(zone, operation, owner, caller);
    }
#else
    (void)zone;
    (void)operation;
#endif
}

[[noreturn]] void SimWorld::FailZoneOwnerCheck(const ZoneRuntime& zone,
                                               const char* operation,
                                               std::thread::id owner,
                                               std::thread::id caller) const
{
    std::ostringstream owner_stream;
    std::ostringstream caller_stream;
    owner_stream << owner;
    caller_stream << caller;
    LOG_ERROR("ZoneWorld accessed by non-owner thread (M3a invariant violation): zone={} ('{}') operation={} owner={} caller={}",
              zone.id,
              zone.name,
              operation,
              owner_stream.str(),
              caller_stream.str());
    std::abort();
}

void SimWorld::TickZone(ZoneRuntime& zone, float dt)
{
    {
        ZoneTickScope owner_scope(*this, zone);
        DrainZoneCommands(zone);
        if (zone.players.empty()) {
            ClearGhosts(zone);
            PublishBorderSnapshot(zone);
            zone.ticks_since_diag.fetch_add(1, std::memory_order_relaxed);
            ++zone.zone_tick;
        } else {
            StepMovement(zone, dt);
            AssertZoneOwner(zone, "flecs world progress");
            zone.world->progress(dt);

            // Phase 3 is the supervisor sync gap before the next worker tick: this
            // worker only marks MigrateTo; the supervisor performs ownership transfer.
            // Phase 4-5: publish this zone's border residents, then rebuild read-only ghosts
            // from the previous tick buffers of neighboring zones.
            PublishBorderSnapshot(zone);
            RebuildGhosts(zone);

            const auto records = BroadcastTransforms(zone);
            zone.transform_records_since_diag.fetch_add(records, std::memory_order_relaxed);
            zone.ticks_since_diag.fetch_add(1, std::memory_order_relaxed);
            ++zone.zone_tick;
        }
        zone.player_count = static_cast<std::uint32_t>(zone.players.size());
    }
    zone.tick_in_progress = false;
}

void SimWorld::DrainZoneCommands(ZoneRuntime& zone)
{
    AssertZoneOwner(zone, "zone command drain");

    std::queue<std::function<void(ZoneRuntime&)>> commands;
    {
        std::lock_guard lock(zone.commands_mutex);
        commands.swap(zone.commands);
    }

    while (!commands.empty()) {
        auto command = std::move(commands.front());
        commands.pop();
        command(zone);
    }
}

void SimWorld::StepMovement(ZoneRuntime& zone, float dt)
{
    AssertZoneOwner(zone, "zone movement integration");

    const float max_extent = WorldExtentMeters();
    for (auto& player : zone.players) {
        const auto state = player.move_intent.state;
        const float speed = state == MoveState::Running
                                ? player.move_speed.run
                                : (state == MoveState::Walking ? player.move_speed.walk : 0.0f);
        player.heading.angle = player.move_intent.dir_angle;
        player.velocity.x = std::sin(player.move_intent.dir_angle) * speed;
        player.velocity.y = std::cos(player.move_intent.dir_angle) * speed;
        player.velocity.z = 0.0f;

        const float dx = player.velocity.x * dt;
        const float dy = player.velocity.y * dt;
        const float next_x = std::clamp(player.position.x + dx, 0.0f, max_extent);
        if (IsWalkable(next_x, player.position.y)) {
            player.position.x = next_x;
        }

        const float next_y = std::clamp(player.position.y + dy, 0.0f, max_extent);
        if (IsWalkable(player.position.x, next_y)) {
            player.position.y = next_y;
        }
        TryApplyWarp(zone, player);
        player.position.z = SampleGroundHeight(player.position.x, player.position.y);
        UpdateMigrationMarker(zone, player);

        player.entity.set<Position>(player.position)
            .set<Heading>(player.heading)
            .set<Velocity>(player.velocity)
            .set<MoveIntent>(player.move_intent);
    }
}

float SimWorld::SampleGroundHeight(float world_x, float world_y) const
{
    if (!terrain_.IsValid()) {
        return 0.0f;
    }
    return terrain_.SampleHeightMeters(world_x, world_y);
}

bool SimWorld::IsWalkable(float world_x, float world_y) const
{
    if (!terrain_.IsValid()) {
        return true;
    }
    return terrain_.IsWalkable(world_x, world_y);
}

Position SimWorld::ResolveSpawnPosition(const gs::db::Character& character,
                                        std::optional<DebugSpawnOverride> debug_spawn,
                                        gs::common::SessionId session_id) const
{
    (void)session_id;
    Position pos{DbToMeters(character.pos_x), DbToMeters(character.pos_y), 0.0f};
#if MMO_DEBUG_SPAWN_OVERRIDE
    if (debug_spawn) {
        if (IsValidDebugSpawnOverride(*debug_spawn)) {
            pos.x = debug_spawn->x;
            pos.y = debug_spawn->y;
            pos.z = SampleGroundHeight(pos.x, pos.y);
            LOG_INFO("Debug spawn override honored for session {}: ({}, {})",
                     session_id,
                     pos.x,
                     pos.y);
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
    if (!IsWalkable(pos.x, pos.y)) {
        LOG_WARN("Resolved spawn at {}, {} is blocked; falling back to DB position", pos.x, pos.y);
        pos.x = DbToMeters(character.pos_x);
        pos.y = DbToMeters(character.pos_y);
    }
    pos.z = SampleGroundHeight(pos.x, pos.y);
    return pos;
}

bool SimWorld::IsValidDebugSpawnOverride(const DebugSpawnOverride& debug_spawn) const
{
    if (!std::isfinite(debug_spawn.x) || !std::isfinite(debug_spawn.y)) {
        return false;
    }

    const float max_extent = WorldExtentMeters();
    if (debug_spawn.x < 0.0f || debug_spawn.y < 0.0f ||
        debug_spawn.x > max_extent || debug_spawn.y > max_extent) {
        return false;
    }

    if (FindZoneIndexForPosition(debug_spawn.x, debug_spawn.y) >= zones_.size()) {
        return false;
    }

    return IsWalkable(debug_spawn.x, debug_spawn.y);
}

void SimWorld::TryApplyWarp(ZoneRuntime& zone, SimPlayer& player)
{
    AssertZoneOwner(zone, "zone warp application");

    const auto* warp = world_logic_.FindWarp(player.position.x, player.position.y);
    if (!warp) {
        return;
    }

    if (!IsWalkable(warp->target_x, warp->target_y)) {
        LOG_WARN("Warp {} target {}, {} is blocked; ignoring warp",
                 warp->id,
                 warp->target_x,
                 warp->target_y);
        return;
    }

    const std::size_t target_zone_index = FindZoneIndexForPosition(warp->target_x, warp->target_y);
    if (target_zone_index >= zones_.size() || zones_[target_zone_index]->id != zone.id) {
        LOG_WARN("Warp {} target {}, {} leaves home zone {}; cross-zone warp is not enabled in M4",
                 warp->id,
                 warp->target_x,
                 warp->target_y,
                 zone.id);
        return;
    }

    LOG_INFO("Session {} triggered warp {} in home zone {} from {}, {} to {}, {}",
             player.session ? player.session->Id() : 0,
             warp->id,
             zone.id,
             player.position.x,
             player.position.y,
             warp->target_x,
             warp->target_y);
    player.position.x = warp->target_x;
    player.position.y = warp->target_y;
}

void SimWorld::UpdateMigrationMarker(ZoneRuntime& zone, SimPlayer& player)
{
    AssertZoneOwner(zone, "zone migration marker update");

    const std::size_t current_zone_index = FindZoneIndexById(zone.id);
    const std::size_t target_zone_index = FindZoneIndexForPosition(player.position.x, player.position.y);
    std::uint32_t target_zone_id = 0;

    if (target_zone_index < zones_.size() && target_zone_index != current_zone_index &&
        DistanceOutsideRect(zone.bounds, player.position) > kMigrationHysteresisMeters) {
        const auto neighbor_it =
            std::find(zone.neighbor_indices.begin(), zone.neighbor_indices.end(), target_zone_index);
        if (neighbor_it != zone.neighbor_indices.end()) {
            target_zone_id = zones_[target_zone_index]->id;
        }
    }

    player.entity.set<MigrateTo>({target_zone_id});
}

std::size_t SimWorld::FindZoneIndexById(std::uint32_t zone_id) const
{
    for (std::size_t i = 0; i < zones_.size(); ++i) {
        if (zones_[i]->id == zone_id) {
            return i;
        }
    }
    return zones_.size();
}

void SimWorld::ExecuteMigration(std::size_t source_zone_index,
                                std::size_t target_zone_index,
                                std::uint32_t net_id)
{
    if (source_zone_index >= zones_.size() || target_zone_index >= zones_.size() ||
        source_zone_index == target_zone_index || AnyZoneTickInProgress()) {
        return;
    }

    auto migrate = [&]() {
        auto& source_zone = *zones_[source_zone_index];
        auto& target_zone = *zones_[target_zone_index];
        auto it = std::find_if(source_zone.players.begin(),
                               source_zone.players.end(),
                               [net_id](const SimPlayer& player) {
                                   return player.net_id == net_id;
                               });
        if (it == source_zone.players.end()) {
            return;
        }
        if (!it->session) {
            return;
        }
        const auto owner_it = owners_by_session_.find(it->session->Id());
        if (owner_it == owners_by_session_.end() || owner_it->second.net_id != net_id ||
            owner_it->second.zone_index != source_zone_index) {
            return;
        }

        const auto migration = it->entity.get<MigrateTo>();
        if (migration.target_zone != target_zone.id) {
            return;
        }

        const std::size_t actual_target = FindZoneIndexForPosition(it->position.x, it->position.y);
        if (actual_target != target_zone_index ||
            DistanceOutsideRect(source_zone.bounds, it->position) <= kMigrationHysteresisMeters) {
            it->entity.set<MigrateTo>({0});
            return;
        }

        RemoveGhostByNetId(target_zone, net_id);

        SimPlayer player = std::move(*it);
        if (player.entity.is_valid()) {
            player.entity.destruct();
        }
        source_zone.players.erase(it);
        source_zone.player_count = static_cast<std::uint32_t>(source_zone.players.size());

        player.position.z = SampleGroundHeight(player.position.x, player.position.y);
        player.entity = target_zone.world->entity()
                            .set<Position>(player.position)
                            .set<Heading>(player.heading)
                            .set<Velocity>(player.velocity)
                            .set<MoveIntent>(player.move_intent)
                            .set<MoveSpeed>(player.move_speed)
                            .set<NetId>({player.net_id})
                            .set<SessionRef>({player.session ? player.session->Id() : 0})
                            .set<MigrateTo>({0})
                            .add<PlayerTag>();

        const auto session_id = player.session ? player.session->Id() : 0;
        target_zone.players.push_back(std::move(player));
        target_zone.player_count = static_cast<std::uint32_t>(target_zone.players.size());
        if (session_id != 0) {
            owners_by_session_[session_id] = OwnerInfo{target_zone_index, net_id};
        }

        source_zone.migrations_since_diag.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO("migration: net_id={} from_zone={} to_zone={} tick={}",
                 net_id,
                 source_zone.id,
                 target_zone.id,
                 world_tick_.load(std::memory_order_relaxed));
    };

    if (source_zone_index < target_zone_index) {
        ZoneTickScope source_scope(*this, *zones_[source_zone_index]);
        ZoneTickScope target_scope(*this, *zones_[target_zone_index]);
        migrate();
    } else {
        ZoneTickScope target_scope(*this, *zones_[target_zone_index]);
        ZoneTickScope source_scope(*this, *zones_[source_zone_index]);
        migrate();
    }
}

void SimWorld::RemoveGhostByNetId(ZoneRuntime& zone, std::uint32_t net_id)
{
    AssertZoneOwner(zone, "zone ghost dedup removal");

    const auto it = std::find_if(zone.ghosts.begin(), zone.ghosts.end(), [net_id](const GhostEntity& ghost) {
        return ghost.snapshot.net_id == net_id;
    });
    if (it == zone.ghosts.end()) {
        return;
    }

    if (it->entity.is_valid()) {
        it->entity.destruct();
    }
    zone.ghosts.erase(it);
    zone.ghost_count = static_cast<std::uint32_t>(zone.ghosts.size());
}

float SimWorld::WorldExtentMeters() const
{
    if (!terrain_.IsValid()) {
        return 1000.0f;
    }
    return static_cast<float>(terrain_.manifest.world_size_cells) * terrain_.manifest.cell_size_meters;
}

std::size_t SimWorld::FindZoneIndexForPosition(float world_x, float world_y) const
{
    for (std::size_t i = 0; i < zones_.size(); ++i) {
        if (zones_[i]->bounds.Contains(world_x, world_y)) {
            return i;
        }
    }
    return zones_.size();
}

void SimWorld::PublishBorderSnapshot(ZoneRuntime& zone)
{
    AssertZoneOwner(zone, "zone border publish");

    auto& out = zone.publish_buffers[zone.zone_tick % zone.publish_buffers.size()];
    out.clear();
    out.reserve(zone.players.size());

    for (const auto& player : zone.players) {
        if (IsInBorderBand(zone, player.position)) {
            out.push_back(SnapshotFromPlayer(player));
        }
    }
}

void SimWorld::RebuildGhosts(ZoneRuntime& zone)
{
    AssertZoneOwner(zone, "zone ghost rebuild");

    ClearGhosts(zone);
    if (zone.neighbor_indices.empty()) {
        return;
    }

    std::unordered_set<std::uint32_t> added_net_ids;
    for (const std::size_t neighbor_index : zone.neighbor_indices) {
        if (neighbor_index >= zones_.size()) {
            continue;
        }

        const auto& neighbor = *zones_[neighbor_index];
        const auto& snapshots = neighbor.publish_buffers[(zone.zone_tick + 1) % neighbor.publish_buffers.size()];
        for (const auto& snapshot : snapshots) {
            if (snapshot.net_id == 0 || IsResidentInZone(zone, snapshot.net_id) ||
                !added_net_ids.insert(snapshot.net_id).second) {
                continue;
            }

            auto entity = zone.world->entity()
                              .set<Position>(snapshot.position)
                              .set<Heading>(snapshot.heading)
                              .set<NetId>({snapshot.net_id})
                              .add<GhostTag>();
            zone.ghosts.push_back(GhostEntity{entity, snapshot});
        }
    }
    zone.ghost_count = static_cast<std::uint32_t>(zone.ghosts.size());
}

void SimWorld::ClearGhosts(ZoneRuntime& zone)
{
    AssertZoneOwner(zone, "zone ghost clear");

    for (auto& ghost : zone.ghosts) {
        if (ghost.entity.is_valid()) {
            ghost.entity.destruct();
        }
    }
    zone.ghosts.clear();
    zone.ghost_count = 0;
}

bool SimWorld::IsInBorderBand(const ZoneRuntime& zone, const Position& position) const
{
    return position.x - zone.bounds.min_x <= kAoiRadiusMeters ||
           zone.bounds.max_x - position.x <= kAoiRadiusMeters ||
           position.y - zone.bounds.min_y <= kAoiRadiusMeters ||
           zone.bounds.max_y - position.y <= kAoiRadiusMeters;
}

bool SimWorld::IsResidentInZone(const ZoneRuntime& zone, std::uint32_t net_id) const
{
    return std::any_of(zone.players.begin(), zone.players.end(), [net_id](const SimPlayer& player) {
        return player.net_id == net_id;
    });
}

void SimWorld::RebuildSpatialGrid(ZoneRuntime& zone)
{
    AssertZoneOwner(zone, "zone spatial grid rebuild");

    zone.spatial_grid.clear();
    for (std::size_t i = 0; i < zone.players.size(); ++i) {
        const auto& player = zone.players[i];
        const int cell_x = SpatialCellCoord(player.position.x);
        const int cell_y = SpatialCellCoord(player.position.y);
        zone.spatial_grid[SpatialCellKey(cell_x, cell_y)].push_back(AoiEntityRef{false, i});
    }
    for (std::size_t i = 0; i < zone.ghosts.size(); ++i) {
        const auto& snapshot = zone.ghosts[i].snapshot;
        const int cell_x = SpatialCellCoord(snapshot.position.x);
        const int cell_y = SpatialCellCoord(snapshot.position.y);
        zone.spatial_grid[SpatialCellKey(cell_x, cell_y)].push_back(AoiEntityRef{true, i});
    }
}

std::vector<SimWorld::AoiEntityRef> SimWorld::QueryAoiCandidates(const ZoneRuntime& zone,
                                                                 const SimPlayer& viewer) const
{
    AssertZoneOwner(zone, "zone AOI query");

    struct Candidate {
        float distance_sq = std::numeric_limits<float>::max();
        AoiEntityRef ref;
    };

    std::vector<Candidate> candidates;
    const int center_x = SpatialCellCoord(viewer.position.x);
    const int center_y = SpatialCellCoord(viewer.position.y);
    const int search_radius =
        static_cast<int>(std::ceil(kAoiRadiusMeters / kSpatialCellSizeMeters));

    for (int y = center_y - search_radius; y <= center_y + search_radius; ++y) {
        for (int x = center_x - search_radius; x <= center_x + search_radius; ++x) {
            const auto cell_it = zone.spatial_grid.find(SpatialCellKey(x, y));
            if (cell_it == zone.spatial_grid.end()) {
                continue;
            }

            for (const auto ref : cell_it->second) {
                std::uint32_t candidate_net_id = 0;
                Position candidate_position;
                if (ref.ghost) {
                    if (ref.index >= zone.ghosts.size()) {
                        continue;
                    }
                    const auto& snapshot = zone.ghosts[ref.index].snapshot;
                    candidate_net_id = snapshot.net_id;
                    candidate_position = snapshot.position;
                } else {
                    if (ref.index >= zone.players.size()) {
                        continue;
                    }
                    const auto& player = zone.players[ref.index];
                    candidate_net_id = player.net_id;
                    candidate_position = player.position;
                }

                if (candidate_net_id == 0 || candidate_net_id == viewer.net_id) {
                    continue;
                }

                const float dx = candidate_position.x - viewer.position.x;
                const float dy = candidate_position.y - viewer.position.y;
                const float distance_sq = dx * dx + dy * dy;
                if (distance_sq <= kAoiRadiusSqMeters) {
                    candidates.push_back(Candidate{distance_sq, ref});
                }
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
        return lhs.distance_sq < rhs.distance_sq;
    });
    if (candidates.size() > kAoiEntityCap) {
        candidates.resize(kAoiEntityCap);
    }

    std::vector<AoiEntityRef> refs;
    refs.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        refs.push_back(candidate.ref);
    }
    return refs;
}

std::size_t SimWorld::BroadcastTransforms(ZoneRuntime& zone)
{
    AssertZoneOwner(zone, "zone outgoing snapshot build");

    if (zone.players.empty()) {
        return 0;
    }

    RebuildSpatialGrid(zone);

    std::size_t transform_records_sent = 0;
    for (std::size_t viewer_index = 0; viewer_index < zone.players.size(); ++viewer_index) {
        auto& viewer = zone.players[viewer_index];
        if (!viewer.session) {
            continue;
        }

        const auto visible_indices = QueryAoiCandidates(zone, viewer);
        std::unordered_set<std::uint32_t> new_visible;
        new_visible.reserve(visible_indices.size());

        auto snapshot_for_ref = [&zone](AoiEntityRef ref) -> std::optional<BorderEntitySnapshot> {
            if (ref.ghost) {
                if (ref.index >= zone.ghosts.size()) {
                    return std::nullopt;
                }
                return zone.ghosts[ref.index].snapshot;
            }
            if (ref.index >= zone.players.size()) {
                return std::nullopt;
            }
            return SnapshotFromPlayer(zone.players[ref.index]);
        };

        std::vector<BorderEntitySnapshot> visible_snapshots;
        visible_snapshots.reserve(visible_indices.size());
        for (const auto ref : visible_indices) {
            auto snapshot = snapshot_for_ref(ref);
            if (!snapshot) {
                continue;
            }
            new_visible.insert(snapshot->net_id);
            visible_snapshots.push_back(std::move(*snapshot));
        }

        for (const auto& visible_entity : visible_snapshots) {
            if (!viewer.visible_net_ids.contains(visible_entity.net_id)) {
                Send(io_, viewer.session, MakeSpawn(visible_entity));
            }
        }

        for (const std::uint32_t old_net_id : viewer.visible_net_ids) {
            if (!new_visible.contains(old_net_id)) {
                Send(io_, viewer.session, MakeDespawn(old_net_id));
            }
        }
        viewer.visible_net_ids = std::move(new_visible);

        const std::size_t record_count = 1 + visible_snapshots.size();
        std::vector<std::uint8_t> payload;
        payload.reserve(1 + 1 + 4 + 2 + record_count * 19);
        payload.push_back(gs::protocol::kCodecBinary);
        payload.push_back(0x10);
        WriteU32(payload, zone.zone_tick);
        WriteU16(payload, static_cast<std::uint16_t>(record_count));
        WriteTransformRecord(payload, viewer);
        for (const auto& visible_entity : visible_snapshots) {
            WriteTransformRecord(payload, visible_entity);
        }

        Send(io_, viewer.session, std::move(payload));
        transform_records_sent += record_count;
    }
    return transform_records_sent;
}

void SimWorld::EnqueueZoneCommand(std::size_t zone_index, std::function<void(ZoneRuntime&)> command)
{
    if (zone_index >= zones_.size()) {
        return;
    }
    {
        std::lock_guard lock(zones_[zone_index]->commands_mutex);
        zones_[zone_index]->commands.push(std::move(command));
    }
    cv_.notify_one();
}

void SimWorld::Spawn(std::shared_ptr<gs::network::Session> session,
                     gs::db::Character character,
                     std::optional<DebugSpawnOverride> debug_spawn)
{
    const auto session_id = session->Id();
    Despawn(session_id);

    SimPlayer player;
    player.session = std::move(session);
    player.character = std::move(character);
    player.position = ResolveSpawnPosition(player.character, debug_spawn, session_id);
    std::size_t zone_index = FindZoneIndexForPosition(player.position.x, player.position.y);
    if (zone_index >= zones_.size()) {
        LOG_WARN("Resolved spawn at {}, {} is outside all zones; falling back to zone 0",
                 player.position.x,
                 player.position.y);
        zone_index = 0;
        player.position.x = zones_[zone_index]->bounds.CenterX();
        player.position.y = zones_[zone_index]->bounds.CenterY();
    }
    player.position.z = SampleGroundHeight(player.position.x, player.position.y);
    player.heading = Heading{0.0f};
    player.move_speed = MoveSpeed{3.0f, 6.0f};
    player.net_id = next_net_id_++;

    owners_by_session_[session_id] = OwnerInfo{zone_index, player.net_id};
    const auto incoming_session = player.session;
    Send(io_, incoming_session, MakeEnterWorldAccept(player.net_id,
                                                     player.position,
                                                     world_tick_.load(std::memory_order_relaxed)));

    LOG_INFO("Session {} assigned to zone {} ('{}') as net_id {} at {}, {}, ground_z={}",
             session_id,
             zones_[zone_index]->id,
             zones_[zone_index]->name,
             player.net_id,
             player.position.x,
             player.position.y,
             player.position.z);

    EnqueueZoneCommand(zone_index, [this, player = std::move(player)](ZoneRuntime& zone) mutable {
        AssertZoneOwner(zone, "zone spawn command");
        player.entity = zone.world->entity()
                            .set<Position>(player.position)
                            .set<Heading>(player.heading)
                            .set<Velocity>(player.velocity)
                            .set<MoveIntent>(player.move_intent)
                            .set<MoveSpeed>(player.move_speed)
                            .set<NetId>({player.net_id})
                            .set<SessionRef>({player.session->Id()})
                            .set<MigrateTo>({0})
                            .add<PlayerTag>();
        zone.players.push_back(std::move(player));
        zone.player_count = static_cast<std::uint32_t>(zone.players.size());
    });
}

void SimWorld::Despawn(gs::common::SessionId session_id)
{
    const auto owner_it = owners_by_session_.find(session_id);
    if (owner_it == owners_by_session_.end()) {
        LOG_INFO("Sim despawn requested for session {}, but no in-world player was found", session_id);
        return;
    }

    const auto zone_index = owner_it->second.zone_index;
    const auto net_id = owner_it->second.net_id;
    owners_by_session_.erase(owner_it);

    EnqueueZoneCommand(zone_index, [this, session_id, net_id](ZoneRuntime& zone) {
        AssertZoneOwner(zone, "zone despawn command");
        const auto it = std::find_if(zone.players.begin(), zone.players.end(), [session_id](const SimPlayer& player) {
            return player.session && player.session->Id() == session_id;
        });
        if (it == zone.players.end()) {
            return;
        }

        if (it->entity.is_valid()) {
            it->entity.destruct();
        }
        LOG_INFO("Session {} despawned net_id {} from zone {}", session_id, net_id, zone.id);
        zone.players.erase(it);

        auto payload = MakeDespawn(net_id);
        for (auto& player : zone.players) {
            if (player.visible_net_ids.erase(net_id) > 0) {
                Send(io_, player.session, payload);
            }
        }
        zone.player_count = static_cast<std::uint32_t>(zone.players.size());
    });
}

} // namespace gs::game
