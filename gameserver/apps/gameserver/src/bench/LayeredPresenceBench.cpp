#include "LayeredPresenceBench.h"

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "db/CharacterRepository.h"
#include "map/LayerClearance.h"
#include "map/LayeredWorldGeometry.h"
#include "map/WorldPackageWriter.h"
#include "network/Session.h"
#include "protocol/Serialization.h"

#include "../world/WorldRuntime.h"
#include "../world/components/ComponentRegistration.h"
#include "../world/components/AiComponents.h"
#include "../world/debug/WorldValidator.h"
#include "../world/partition/RegionDefinition.h"
#include "../world/spatial/SpatialValidator.h"
#include "../world/spawn/MobPrototypeRegistry.h"
#include "../world/spawn/SpawnSystem.h"
#include "../world/systems/MovementSystem.h"
#include "../world/terrain/TerrainService.h"
#include "../world/zone/Zone.h"
#include "../world/zone/ZoneManager.h"
#include "../world/zone/ZoneOwnership.h"
#include "../world/components/LayerComponents.h"
#include "../world/migration/EntityTransfer.h"
#include "../world/package/WorldPackageLoader.h"
#include "../world/partition/PartitionConfig.h"
#include "../world/replication/ProtocolEncoder.h"
#include "../world/spatial/SpatialGrid.h"
#include "BenchSnapshot.h"

namespace gs::bench {
namespace {

namespace fs = std::filesystem;
namespace asio = boost::asio;
namespace map = mx::map;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Checks {
    int passes = 0, failures = 0;

    void Report(const std::string& name, bool pass, const std::string& detail = {})
    {
        std::printf("LAYEREDPRESENCE %s: %s%s%s\n", name.c_str(), pass ? "PASS" : "FAIL",
                    detail.empty() ? "" : " -- ", detail.c_str());
        (pass ? passes : failures) += 1;
    }
};

std::string Fmt(const char* format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

struct ScratchDirectory {
    fs::path parent = fs::weakly_canonical(fs::temp_directory_path());
    fs::path root;

    ScratchDirectory()
    {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 10; ++attempt) {
            const auto candidate = parent / ("ixw_layeredpresence_" + std::to_string(suffix) + "_" +
                                             std::to_string(attempt));
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) {
                root = candidate;
                return;
            }
        }
        throw std::runtime_error("cannot allocate a unique layered presence scratch directory");
    }

    ~ScratchDirectory()
    {
        if (root.is_absolute() && root.parent_path() == parent &&
            root.filename().string().starts_with("ixw_layeredpresence_")) {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    }
};

struct IoRunner {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work{asio::make_work_guard(io)};
    std::thread thread{[this] { io.run(); }};
    ~IoRunner()
    {
        work.reset();
        io.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }
};

std::shared_ptr<gs::network::Session> DetachedSession(asio::io_context& io, gs::common::SessionId id)
{
    asio::ip::tcp::socket socket(io);
    return std::make_shared<gs::network::Session>(std::move(socket), id);
}

gs::db::Character MakeCharacter(std::uint64_t index)
{
    gs::db::Character character;
    character.id = gs::db::CharacterId{95000 + index};
    character.account_id = gs::db::AccountId{96000 + index};
    character.name = "Layered" + std::to_string(index);
    character.level = 1;
    character.class_id = 1;
    character.created_at = std::chrono::system_clock::now();
    character.last_played_at = character.created_at;
    return character;
}

bool WaitFor(std::chrono::milliseconds timeout, const std::function<bool()>& condition)
{
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return condition();
}

std::string AuditNow(gs::game::WorldRuntime& sim)
{
    sim.RequestValidation();
    std::string result;
    for (int i = 0; i < 400; ++i) {
        if (sim.TryTakeValidationResult(result)) {
            return result;
        }
        std::this_thread::sleep_for(25ms);
    }
    return "TIMEOUT";
}

// Closed box in canonical metres (Z up), outward winding.
map::LayerCollisionMesh Box(std::uint32_t id, float x0, float y0, float z0, float x1, float y1, float z1)
{
    map::LayerCollisionMesh mesh;
    mesh.source_id = id;
    mesh.name = "box_" + std::to_string(id);
    mesh.tags = map::VolumeTagGround;
    mesh.supports_ground_movement = true;
    mesh.vertices = {{x0, y0, z0}, {x1, y0, z0}, {x0, y1, z0}, {x1, y1, z0},
                     {x0, y0, z1}, {x1, y0, z1}, {x0, y1, z1}, {x1, y1, z1}};
    mesh.indices = {4, 5, 7, 4, 7, 6, 0, 2, 3, 0, 3, 1, 0, 1, 5, 0, 5, 4,
                    2, 6, 7, 2, 7, 3, 0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5};
    return mesh;
}

constexpr float kWorldSize = 512.0f;

// Floor (top 1 m) across the x = 256 zone cut, a non-walkable wall on it, a
// stair of four 0.75 m treads onto a 2 m landing at its east edge, and a
// separate bridge deck (top 6 m) stacked above part of the floor.
bool CookFixture(map::LayeredWorld& world, std::string& error)
{
    std::vector<map::LayerCollisionMesh> walkable{Box(1, 200, 236, 0, 300, 276, 1)};
    for (std::uint32_t k = 0; k < 4; ++k) {
        const float x0 = 300 + 0.75f * static_cast<float>(k);
        walkable.push_back(Box(2 + k, x0, 250, 0, x0 + 0.75f, 254, 1.25f + 0.25f * static_cast<float>(k)));
    }
    walkable.push_back(Box(6, 303, 248, 0, 310, 256, 2));
    walkable.push_back(Box(7, 240, 240, 5, 270, 270, 6));
    const auto wall = Box(9, 230, 240, 1, 230.5f, 260, 4);

    std::vector<map::LayerSourceSurface> surfaces;
    map::LayerGeometryReport geometry;
    if (!map::ExtractLayerSourceSurfaces(walkable, {}, surfaces, geometry)) {
        error = geometry.errors.empty() ? "extract" : geometry.errors.front();
        return false;
    }
    map::LayerGenerationOptions options;
    options.world_bounds = map::Rect{0, 0, kWorldSize, kWorldSize};
    options.require_exact_footprints = true;
    map::LayerGenerationReport generation;
    if (!map::GenerateLayeredWorld(surfaces, options, world, generation)) {
        error = generation.errors.empty() ? "generate" : generation.errors.front();
        return false;
    }
    std::vector<map::LayerObstructionMesh> obstructions;
    for (const auto& mesh : walkable) obstructions.push_back({mesh.source_id, mesh.vertices, mesh.indices});
    obstructions.push_back({wall.source_id, wall.vertices, wall.indices});
    map::LayerClearanceReport report;
    if (!map::CookLayerClearance(world, options.world_bounds, obstructions, nullptr, map::LayerClearanceProfile{},
                                 report)) {
        error = report.errors.empty() ? "cook" : report.errors.front();
        return false;
    }
    return true;
}

const map::LayerVolume* WalkableAt(const map::LayeredWorld& world, double x, double y, double min_z, double max_z)
{
    for (const auto& volume : world.volumes) {
        if (volume.ground_support && x >= volume.bounds.min_x && x < volume.bounds.max_x &&
            y >= volume.bounds.min_y && y < volume.bounds.max_y && volume.min_z >= min_z && volume.min_z < max_z) {
            return &volume;
        }
    }
    return nullptr;
}

// ---- unit checks (no runtime) ---------------------------------------------

void UnitChecks(Checks& checks)
{
    using gs::game::GridSlot;
    using gs::game::LayerPresence;
    using gs::game::Position;
    flecs::world world;
    gs::game::RegisterWorldComponents(world);
    gs::game::SpatialGrid grid;
    auto ground = world.entity();
    auto upper = world.entity().set<LayerPresence>({7, 2});
    const Position below{10.5f, 20.5f, 1.0f};
    const Position above{10.5f, 20.5f, 6.0f};
    grid.Insert(1, below, ground, 0);
    grid.Insert(2, above, upper, 7);
    const auto entries = grid.SnapshotEntries();
    bool distinct = entries.size() == 2;
    for (const auto& [net, key] : entries) {
        distinct = distinct && key.volume_id == (net == 1 ? 0u : 7u) && key.cell_x == 0 && key.cell_y == 0;
    }
    checks.Report("grid-stacked-entries-have-distinct-layered-buckets",
                  distinct && !(entries[0].second == entries[1].second));
    std::size_t seen = 0;
    grid.ForEachInRadius(below, 120.0f, [&](const gs::game::GridEntry&) { ++seen; });
    checks.Report("grid-radius-scan-reads-every-volume-of-the-column", seen == 2);
    checks.Report("grid-contains-in-is-volume-exact", grid.ContainsIn(2, above, 7) && !grid.ContainsIn(2, above, 0) &&
                                                         grid.ContainsIn(1, below, 0) && grid.Contains(2, above));
    // A portal crossing in place: same cell, other volume.
    upper.set<LayerPresence>({8, 3});
    const bool changed = grid.Move(upper, 2, gs::game::SpatialCellKey(0, 0), above, 8);
    const auto* slot = upper.try_get<GridSlot>();
    checks.Report("grid-volume-change-moves-the-bucket", changed && grid.ContainsIn(2, above, 8) &&
                                                             !grid.ContainsIn(2, above, 7) && slot &&
                                                             slot->volume_id == 8 && grid.Size() == 2);
    grid.Remove(2, above);
    grid.Remove(1, below);
    checks.Report("grid-remove-finds-entries-of-any-volume", grid.Size() == 0 && grid.Empty());

    // Transfer DTO round trip keeps presence (and terrain entities stay plain).
    auto layered = world.entity()
                       .set<Position>({300.0f, 252.0f, 1.25f})
                       .set<gs::game::Heading>({})
                       .set<gs::game::Velocity>({})
                       .set<gs::game::MoveIntent>({})
                       .set<gs::game::MoveSpeed>({})
                       .set<gs::game::Hp>({})
                       .set<gs::game::CombatStats>({})
                       .set<gs::game::AttackCooldown>({})
                       .set<gs::game::NetId>({4242})
                       .set<gs::game::SessionRef>({1})
                       .set<LayerPresence>({3, 3});
    const auto transfer = gs::game::BuildTransfer(layered, true);
    flecs::world other;
    gs::game::RegisterWorldComponents(other);
    const auto applied = gs::game::ApplyTransfer(other, transfer);
    const auto* moved = applied.try_get<LayerPresence>();
    checks.Report("transfer-carries-layered-presence", transfer.IsLayered() && moved && moved->volume_id == 3 &&
                                                         moved->layer_id == 3 &&
                                                         applied.get<Position>().z == 1.25f);
    layered.remove<LayerPresence>();
    const auto plain = gs::game::ApplyTransfer(other, gs::game::BuildTransfer(layered, true));
    checks.Report("terrain-transfer-stays-without-presence", !plain.has<LayerPresence>());

    // Additive protocol fields.
    gs::game::BorderEntitySnapshot snapshot;
    snapshot.net_id = 77;
    snapshot.volume_id = 12;
    snapshot.layer_id = 5;
    const auto spawn = gs::protocol::ParsePacket(gs::game::MakeSpawn(snapshot));
    const auto accept =
        gs::protocol::ParsePacket(gs::game::MakeEnterWorldAccept(9, {1.0f, 2.0f, 3.0f}, 44, 12, 5));
    const auto legacy = gs::protocol::ParsePacket(gs::game::MakeEnterWorldAccept(9, {1.0f, 2.0f, 3.0f}, 44));
    checks.Report("spawn-packet-carries-volume-and-layer",
                  spawn && spawn->packet.isEntitySpawn() && spawn->packet.getEntitySpawn().getVolumeId() == 12 &&
                      spawn->packet.getEntitySpawn().getLayerId() == 5);
    checks.Report("enter-accept-carries-volume-legacy-defaults-zero",
                  accept && accept->packet.isEnterWorldAccept() &&
                      accept->packet.getEnterWorldAccept().getSpawnVolumeId() == 12 &&
                      accept->packet.getEnterWorldAccept().getSpawnLayerId() == 5 && legacy &&
                      legacy->packet.getEnterWorldAccept().getSpawnVolumeId() == 0);
}

// ---- deterministic zone-level checks (production systems, no scheduler) ----

struct ZoneFixture {
    asio::io_context io; // sessions' sockets must outlive the zone
    gs::game::ZoneManager zones;
    gs::game::TerrainService terrain = gs::game::TerrainService::Flat({0, 0, kWorldSize, kWorldSize});
    map::WorldLogic logic;
    gs::game::MobPrototypeRegistry types;

    bool Build()
    {
        gs::game::PartitionLayout layout;
        layout.regions_x = layout.regions_y = 1;
        gs::game::InitialPartition initial;
        std::string error;
        if (!gs::game::BuildInitialPartition({0, 0, kWorldSize, kWorldSize}, layout, 240, initial, error)) {
            return false;
        }
        zones.BuildInitialPartition(initial);
        return zones.ZoneCount() == 1;
    }
};

void DeterministicChecks(Checks& checks)
{
    using namespace gs::game;
    map::LayeredWorld layered;
    std::string error;
    if (!CookFixture(layered, error)) {
        checks.Report("deterministic-fixture-cooked", false, error);
        return;
    }
    const auto* floor = WalkableAt(layered, 240, 256, -1, 2);
    const auto* landing = WalkableAt(layered, 305, 252, 1.5, 3);
    const auto* bridge = WalkableAt(layered, 250, 250, 4, 7);
    ZoneFixture fixture;
    if (!floor || !landing || !bridge || !fixture.Build()) {
        checks.Report("deterministic-fixture-zone", false);
        return;
    }
    auto& zone = fixture.zones.GetZone(0);
    const map::LayerActorProfile actor{layered.clearance_profile->actor_radius_m,
                                       layered.clearance_profile->actor_height_m};
    ZoneTickContext ctx{fixture.terrain, fixture.logic, fixture.types, fixture.zones};
    ctx.layered = &layered;
    ctx.layer_actor = actor;

    auto spawn = [&](std::uint32_t net, float x, float y, const map::LayerVolume& volume) {
        ZoneWriteGuard guard(zone, "layered fixture spawn");
        const auto z = static_cast<float>(volume.ground_support->Height(x, y));
        SpawnSystem::SpawnPlayer(zone, DetachedSession(fixture.io, net), MakeCharacter(net), Position{x, y, z}, net,
                                 LayerPresence{volume.id, volume.layer_id});
        return zone.FindEntity(net);
    };
    auto steer = [&](flecs::entity entity, float heading, MoveState state) {
        ZoneWriteGuard guard(zone, "layered fixture input");
        entity.set<MoveIntent>({heading, state, 0});
    };
    auto tick = [&](int ticks, float dt = 0.05f) {
        for (int i = 0; i < ticks; ++i) {
            ZoneWriteGuard guard(zone, "layered fixture tick");
            MovementSystem::Step(zone, dt, ctx);
        }
    };
    constexpr float kEast = 1.5707963f;
    auto& diag = zone.Diagnostics();

    // Stairs: the volume sequence is exactly floor -> 4 treads -> landing,
    // one proven portal at a time, z always the current support plane.
    const auto climber = spawn(501, 296.0f, 252.0f, *floor);
    steer(climber, kEast, MoveState::Running);
    std::vector<std::uint32_t> sequence{floor->id};
    bool z_on_plane = true;
    for (int i = 0; i < 60; ++i) {
        tick(1);
        const auto presence = climber.get<LayerPresence>();
        const auto position = climber.get<Position>();
        if (presence.volume_id != sequence.back()) sequence.push_back(presence.volume_id);
        const auto* volume = WalkableAt(layered, position.x, position.y, -1, 10);
        z_on_plane = z_on_plane && volume != nullptr &&
                     position.z == static_cast<float>(volume->ground_support->Height(position.x, position.y));
    }
    std::string order;
    for (const auto id : sequence) order += std::to_string(id) + " ";
    checks.Report("deterministic-stair-volume-sequence-one-portal-at-a-time",
                  sequence.size() == 6 && sequence.front() == floor->id && sequence.back() == landing->id &&
                      diag.layered_portal_crossings_total.load() == 5 && z_on_plane,
                  "volumes " + order);
    const auto stopped = climber.get<Position>();
    checks.Report("deterministic-landing-edge-refuses-leaving-the-volume-system",
                  stopped.x < 310.0f && stopped.x > 309.0f && climber.get<LayerPresence>().volume_id == landing->id &&
                      diag.layered_moves_transition_total.load() > 0,
                  Fmt("x=%.4f", stopped.x));

    // Wall: the exact last accepted piece is the last clear cell.
    const auto walker = spawn(502, 226.0f, 250.0f, *floor);
    steer(walker, kEast, MoveState::Running);
    const auto blocked_before = diag.layered_moves_blocked_total.load();
    tick(30);
    const auto at_wall = walker.get<Position>();
    checks.Report("deterministic-wall-blocks-before-actor-radius",
                  at_wall.x <= 230.0f - 0.35f && at_wall.x > 229.0f &&
                      diag.layered_moves_blocked_total.load() > blocked_before,
                  Fmt("x=%.4f", at_wall.x));

    // A low-LOD-sized long step (1.5 m) is split into <= 0.5 m pieces, so it
    // may still cross treads one portal at a time.
    const auto stepper = spawn(503, 299.0f, 251.0f, *floor);
    {
        ZoneWriteGuard guard(zone, "layered fixture speed");
        stepper.set<MoveSpeed>({1.5f, 1.5f});
        stepper.set<MoveIntent>({kEast, MoveState::Walking, 0});
    }
    const auto crossings_before = diag.layered_portal_crossings_total.load();
    tick(1, 1.0f);
    const auto long_step = stepper.get<Position>();
    checks.Report("deterministic-long-step-is-sub-stepped-across-portals",
                  long_step.x == 300.5f && diag.layered_portal_crossings_total.load() == crossings_before + 1 &&
                      stepper.get<LayerPresence>().volume_id != floor->id && long_step.z == 1.25f,
                  Fmt("x=%.4f z=%.4f", long_step.x, long_step.z));

    // Without layered metadata in the tick context a layered entity cannot
    // move at all (never falls back to terrain movement).
    const auto frozen = spawn(504, 250.0f, 260.0f, *floor);
    steer(frozen, 0.0f, MoveState::Running);
    ctx.layered = nullptr;
    const auto invalid_before = diag.layered_moves_invalid_total.load();
    tick(3);
    ctx.layered = &layered;
    checks.Report("deterministic-no-layered-metadata-freezes-layered-entity",
                  frozen.get<Position>().y == 260.0f && frozen.get<Position>().z == 1.0f &&
                      diag.layered_moves_invalid_total.load() > invalid_before);
    steer(frozen, 0.0f, MoveState::Idle);

    // Audit + spatial index: positive, then one corruption at a time.
    std::string audit_error;
    checks.Report("audit-accepts-valid-layered-residents",
                  ValidateLayeredPresence(fixture.zones, &layered, actor, audit_error) &&
                      ValidateSpatialIndex(zone, zone.Grid(), audit_error),
                  audit_error);
    const auto victim = spawn(505, 250.0f, 250.0f, *bridge);
    auto corrupt = [&](const char* name, const std::function<void()>& damage, const std::function<void()>& repair,
                       bool spatial) {
        {
            ZoneWriteGuard guard(zone, "layered fixture corruption");
            damage();
        }
        std::string message;
        const bool caught = spatial ? !ValidateSpatialIndex(zone, zone.Grid(), message)
                                    : !ValidateLayeredPresence(fixture.zones, &layered, actor, message);
        {
            ZoneWriteGuard guard(zone, "layered fixture repair");
            repair();
        }
        checks.Report(name, caught, message);
    };
    const Position good = victim.get<Position>();
    auto place = [&](const Position& p) {
        const auto current = victim.get<Position>();
        const auto old_cell = SpatialCellKey(SpatialCellCoord(current.x), SpatialCellCoord(current.y));
        victim.set<Position>(p);
        zone.Grid().Move(victim, 505, old_cell, p, SpatialVolumeOf(victim));
    };
    corrupt("negative-audit-catches-off-plane-z", [&] { place({good.x, good.y, good.z + 0.01f}); },
            [&] { place(good); }, false);
    corrupt("negative-audit-catches-unknown-volume", [&] { victim.set<LayerPresence>({999, bridge->layer_id}); },
            [&] { victim.set<LayerPresence>({bridge->id, bridge->layer_id}); }, false);
    corrupt("negative-audit-catches-wrong-layer", [&] { victim.set<LayerPresence>({bridge->id, bridge->layer_id + 1}); },
            [&] { victim.set<LayerPresence>({bridge->id, bridge->layer_id}); }, false);
    const auto* wall_floor = floor;
    corrupt("negative-audit-catches-resident-inside-blocked-cell",
            [&] {
                victim.set<LayerPresence>({wall_floor->id, wall_floor->layer_id});
                place({229.9f, 250.0f, 1.0f});
            },
            [&] {
                victim.set<LayerPresence>({bridge->id, bridge->layer_id});
                place(good);
            },
            false);
    {
        std::string message;
        checks.Report("negative-audit-null-world-refuses-layered-residents",
                      !ValidateLayeredPresence(fixture.zones, nullptr, actor, message), message);
    }
    corrupt("negative-spatial-index-catches-stale-volume-bucket",
            [&] { victim.set<LayerPresence>({floor->id, floor->layer_id}); },
            [&] { victim.set<LayerPresence>({bridge->id, bridge->layer_id}); }, true);
    checks.Report("audit-accepts-after-repairs",
                  ValidateLayeredPresence(fixture.zones, &layered, actor, audit_error) &&
                      ValidateSpatialIndex(zone, zone.Grid(), audit_error),
                  audit_error);
}

// ---- running runtime ------------------------------------------------------

struct PlayerView {
    bool found = false;
    gs::game::ZoneId zone = 0;
    std::uint32_t net_id = 0;
    gs::game::Position position;
    bool layered = false;
    gs::game::LayerPresence presence;
};

PlayerView ReadPlayer(gs::game::WorldRuntime& sim, gs::common::SessionId session)
{
    return ReadWorld(sim, [session](const WorldSnapshot& snap) {
        PlayerView out;
        const auto it = snap.owners.find(session);
        if (it == snap.owners.end() || it->second.zone_index >= snap.zones.ZoneCount()) {
            return out;
        }
        const auto& zone = snap.zones.GetZone(it->second.zone_index);
        const auto entity = zone.FindEntity(it->second.net_id);
        if (!entity.is_valid() || !entity.has<gs::game::Position>()) {
            return out;
        }
        out.found = true;
        out.zone = zone.Id();
        out.net_id = it->second.net_id;
        out.position = entity.get<gs::game::Position>();
        if (const auto* presence = entity.try_get<gs::game::LayerPresence>()) {
            out.layered = true;
            out.presence = *presence;
        }
        return out;
    });
}

std::string Describe(const PlayerView& p)
{
    return Fmt("zone=%u net=%u pos=(%.3f, %.3f, %.3f) volume=%u layer=%u", p.zone, p.net_id, p.position.x,
               p.position.y, p.position.z, p.presence.volume_id, p.presence.layer_id);
}

void RuntimeChecks(Checks& checks)
{
    ScratchDirectory scratch;
    map::LayeredWorld layered;
    std::string error;
    const bool cooked = CookFixture(layered, error);
    const auto proven = std::count_if(layered.portals.begin(), layered.portals.end(),
                                      [](const auto& portal) { return portal.proof.has_value(); });
    checks.Report("cook-floor-wall-stairs-landing-bridge", cooked && proven == 5, error);
    if (!cooked) return;
    const auto* floor = WalkableAt(layered, 240, 256, -1, 2);
    const auto* landing = WalkableAt(layered, 305, 252, 1.5, 3);
    const auto* bridge = WalkableAt(layered, 250, 250, 4, 7);
    if (!floor || !landing || !bridge) {
        checks.Report("fixture-volumes-found", false);
        return;
    }
    const std::uint32_t floor_id = floor->id, landing_id = landing->id, bridge_id = bridge->id;

    map::PackageWriteSpec spec;
    spec.world_id = "layeredpresence_fixture";
    spec.world_name = "3D-5A layered presence fixture";
    spec.size_cells_x = static_cast<std::uint32_t>(kWorldSize);
    spec.size_cells_y = static_cast<std::uint32_t>(kWorldSize);
    spec.origin_x = 0;
    spec.origin_y = 0;
    spec.cell_size_m = 1;
    spec.chunk_size_cells = 128;
    spec.height_raw = [](std::uint32_t, std::uint32_t) { return 0; };
    spec.attributes = [](std::uint32_t, std::uint32_t) { return std::uint16_t{0}; };
    spec.logic.spawns.push_back(map::SpawnRegion{1, 0, map::Rect{96, 96, 104, 104}});
    spec.layered_world = layered;
    const auto root = scratch.root / "package";
    const auto written = map::WritePackage(root, spec);
    checks.Report("strict-writer-accepts-layered-fixture", written.ok, written.error);
    if (!written.ok) return;
    gs::game::WorldLoadRequest request;
    request.package_root = root;
    request.mob_types_config = IXTREEME_DEFAULT_MOB_TYPES_CONFIG;
    request.depth = map::ValidationDepth::Full;
    request.warp_policy = map::WarpPolicy::Strict;
    map::PackageReport report;
    auto loaded = gs::game::LoadWorldPackage(request, report);
    checks.Report("strict-full-load", loaded && report.Ok());
    if (!loaded) return;

    IoRunner runner;
    gs::game::PartitionLayout layout;
    layout.regions_x = 2;
    layout.regions_y = 1;
    gs::game::WorldRuntime sim(runner.io, {}, std::move(*loaded), layout);
    gs::game::PartitionConfig partition;
    partition.scoring.adaptive_enabled = false;
    sim.ConfigurePartition(partition);

    struct Player {
        gs::common::SessionId session;
        gs::game::DebugSpawnOverride spawn;
        float heading; // 0 = north (+Y), pi/2 = east (+X)
        bool moves;
    };
    constexpr float kEast = 1.5707963f;
    const Player players[] = {
        {95101, {240.0f, 256.0f, floor_id}, kEast, true},   // A: crosses the x=256 zone cut on the floor
        {95102, {296.0f, 252.0f, floor_id}, kEast, true},   // B: up the stairs onto the landing
        {95103, {226.0f, 250.0f, floor_id}, kEast, true},   // C: runs into the wall at x=230
        {95104, {250.0f, 250.0f, bridge_id}, 0.0f, false},  // D: bridge deck, stacked above E
        {95105, {250.0f, 250.0f, floor_id}, 0.0f, false},   // E: floor under the bridge
        {95106, {100.0f, 100.0f, 0}, 0.0f, true},           // F: legacy terrain player
        {95107, {230.25f, 250.0f, floor_id}, 0.0f, false},  // G: inside the wall -> refused
        {95108, {240.0f, 256.0f, 999}, 0.0f, false},        // H: unknown volume -> refused
    };
    std::uint64_t index = 0;
    for (const auto& p : players) {
        sim.PostSpawn(DetachedSession(runner.io, p.session), MakeCharacter(index++), p.spawn);
    }
    sim.Start();
    const bool entered = WaitFor(10000ms, [&] {
        return ReadWorld(sim, [](const WorldSnapshot& snap) { return snap.owners.size(); }) == std::size(players);
    });
    checks.Report("all-players-entered", entered);

    // Admission (before any movement).
    const auto d0 = ReadPlayer(sim, 95104);
    const auto e0 = ReadPlayer(sim, 95105);
    const auto a0 = ReadPlayer(sim, 95101);
    checks.Report("layered-admission-explicit-volume-and-support-z",
                  a0.found && a0.layered && a0.presence.volume_id == floor_id && a0.position.z == 1.0f,
                  Describe(a0));
    checks.Report("stacked-admission-same-xy-different-floors",
                  d0.found && e0.found && d0.layered && e0.layered && d0.presence.volume_id == bridge_id &&
                      e0.presence.volume_id == floor_id && d0.position.z == 6.0f && e0.position.z == 1.0f &&
                      d0.position.x == e0.position.x && d0.position.y == e0.position.y,
                  Describe(d0) + " / " + Describe(e0));
    for (const auto session : {gs::common::SessionId{95107}, gs::common::SessionId{95108}}) {
        const auto refused = ReadPlayer(sim, session);
        checks.Report(session == 95107 ? "layered-admission-in-wall-refused-to-terrain-spawn"
                                       : "layered-admission-unknown-volume-refused-to-terrain-spawn",
                      refused.found && !refused.layered && refused.position.x == 100.0f &&
                          refused.position.y == 100.0f && refused.position.z == 0.0f,
                      Describe(refused));
    }
    const auto stacked_keys = ReadWorld(sim, [&](const WorldSnapshot& snap) {
        std::vector<mx::map::LayeredSpatialCellKey> keys;
        for (std::size_t i = 0; i < snap.zones.ZoneCount(); ++i) {
            for (const auto& [net, key] : snap.zones.GetZone(i).Grid().SnapshotEntries()) {
                if (net == d0.net_id || net == e0.net_id) keys.push_back(key);
            }
        }
        return keys;
    });
    checks.Report("stacked-players-indexed-in-distinct-volume-buckets",
                  stacked_keys.size() == 2 && !(stacked_keys[0] == stacked_keys[1]) &&
                      stacked_keys[0].cell_x == stacked_keys[1].cell_x &&
                      stacked_keys[0].cell_y == stacked_keys[1].cell_y);

    std::uint32_t seq = 0;
    const auto run_until = Clock::now() + 4000ms;
    while (Clock::now() < run_until) {
        for (const auto& p : players) {
            if (p.moves) sim.PostMoveInput(p.session, ++seq, p.heading, gs::game::MoveState::Running);
        }
        std::this_thread::sleep_for(50ms);
    }
    for (const auto& p : players) {
        if (p.moves) sim.PostMoveInput(p.session, ++seq, p.heading, gs::game::MoveState::Idle);
    }
    std::this_thread::sleep_for(400ms);

    const auto a = ReadPlayer(sim, 95101);
    const auto b = ReadPlayer(sim, 95102);
    const auto c = ReadPlayer(sim, 95103);
    const auto f = ReadPlayer(sim, 95106);
    const auto zone_of = [&](float x) {
        return ReadWorld(sim, [x](const WorldSnapshot& snap) -> gs::game::ZoneId {
            for (const auto* leaf : snap.zones.GetActiveLeaves()) {
                if (leaf->bounds.ContainsHalfOpen(x, 256.0f)) return leaf->zone_id;
            }
            return 0;
        });
    };
    checks.Report("layered-player-migrates-across-zone-cut-keeping-presence-and-z",
                  a.found && a.layered && a.presence.volume_id == floor_id && a.position.x > 256.5f &&
                      a.zone == zone_of(a.position.x) && a.zone != a0.zone && a.position.z == 1.0f,
                  Describe(a0) + " -> " + Describe(a));
    checks.Report("layered-player-climbs-stairs-onto-landing-via-portals",
                  b.found && b.layered && b.presence.volume_id == landing_id && b.position.z == 2.0f &&
                      b.position.x > 303.0f && b.position.x < 310.0f,
                  Describe(b));
    checks.Report("layered-player-stops-before-unopted-wall",
                  c.found && c.layered && c.presence.volume_id == floor_id && c.position.x > 227.0f &&
                      c.position.x <= 230.0f - 0.35f && c.position.z == 1.0f,
                  Describe(c));
    checks.Report("terrain-player-moves-on-terrain-without-presence",
                  f.found && !f.layered && f.position.y > 110.0f && f.position.z == 0.0f, Describe(f));

    struct Counters {
        std::uint64_t ok = 0, blocked = 0, transition = 0, invalid = 0, portals = 0;
    };
    const auto counters = ReadWorld(sim, [](const WorldSnapshot& snap) {
        Counters out;
        for (std::size_t i = 0; i < snap.zones.ZoneCount(); ++i) {
            const auto& diag = snap.zones.GetZone(i).Diagnostics();
            out.ok += diag.layered_moves_ok_total.load();
            out.blocked += diag.layered_moves_blocked_total.load();
            out.transition += diag.layered_moves_transition_total.load();
            out.invalid += diag.layered_moves_invalid_total.load();
            out.portals += diag.layered_portal_crossings_total.load();
        }
        return out;
    });
    checks.Report("portal-crossings-counted-no-invalid-layered-state",
                  counters.portals == 5 && counters.invalid == 0 && counters.ok > 0 && counters.blocked > 0,
                  Fmt("ok=%llu blocked=%llu transition=%llu invalid=%llu portals=%llu",
                      static_cast<unsigned long long>(counters.ok),
                      static_cast<unsigned long long>(counters.blocked),
                      static_cast<unsigned long long>(counters.transition),
                      static_cast<unsigned long long>(counters.invalid),
                      static_cast<unsigned long long>(counters.portals)));

    // Ghost of the migrated player in its former zone carries the volume.
    struct GhostView {
        bool found = false;
        std::uint32_t snapshot_volume = 0;
        std::uint32_t entity_volume = 0;
    };
    const auto ghost = ReadWorld(sim, [&](const WorldSnapshot& snap) {
        GhostView out;
        for (std::size_t i = 0; i < snap.zones.ZoneCount(); ++i) {
            const auto& zone = snap.zones.GetZone(i);
            if (zone.Id() != a0.zone) continue;
            if (const auto* record = zone.FindGhost(a.net_id)) {
                out.found = true;
                out.snapshot_volume = record->snapshot.volume_id;
                out.entity_volume = gs::game::SpatialVolumeOf(record->entity);
            }
        }
        return out;
    });
    checks.Report("neighbour-ghost-carries-layered-presence",
                  ghost.found && ghost.snapshot_volume == floor_id && ghost.entity_volume == floor_id,
                  Fmt("found=%d snapshot=%u entity=%u", ghost.found, ghost.snapshot_volume, ghost.entity_volume));

    // AOI -> visibility across stacked floors (horizontal interest radius).
    const auto mutual = ReadWorld(sim, [&](const WorldSnapshot& snap) {
        bool d_sees_e = false, e_sees_d = false;
        for (std::size_t i = 0; i < snap.zones.ZoneCount(); ++i) {
            const auto& zone = snap.zones.GetZone(i);
            if (const auto* binding = zone.FindPlayer(d0.net_id)) d_sees_e = binding->IsVisible(e0.net_id);
            if (const auto* binding = zone.FindPlayer(e0.net_id)) e_sees_d = binding->IsVisible(d0.net_id);
        }
        return d_sees_e && e_sees_d;
    });
    checks.Report("stacked-players-see-each-other", mutual);

    const auto audit = AuditNow(sim);
    checks.Report("world-audit-including-layered-presence", audit == "OK", audit);
    sim.Stop();
}

} // namespace

int RunLayeredPresenceScenario()
{
    Checks checks;
    try {
        UnitChecks(checks);
        DeterministicChecks(checks);
        RuntimeChecks(checks);
    } catch (const std::exception& error) {
        checks.Report("unexpected-exception", false, error.what());
    }
    std::printf("LAYEREDPRESENCE summary passes=%d failures=%d scope=server-layered-presence\n", checks.passes,
                checks.failures);
    return checks.failures;
}

} // namespace gs::bench
