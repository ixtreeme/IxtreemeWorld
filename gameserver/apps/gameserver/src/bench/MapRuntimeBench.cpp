#include "HardeningBench.h"
#include "ReadinessBench.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "db/CharacterRepository.h"
#include "network/Session.h"

#include "map/MapData.h"
#include "map/ServerTerrain.h"
#include "map/WorldPackage.h"
#include "map/WorldPackageWriter.h"

#include "../world/WorldConstants.h"
#include "../world/WorldRuntime.h"
#include "../world/components/Tags.h"
#include "../world/components/WarpState.h"
#include "../world/components/MobComponents.h"
#include "../world/migration/EntityTransfer.h"
#include "../world/spawn/SpawnSystem.h"
#include "../world/systems/MovementSystem.h"
#include "../world/zone/ZoneOwnership.h"
#include "../world/components/TransformComponents.h"
#include "../world/package/WorldPackageLoader.h"
#include "../world/partition/PartitionConfig.h"
#include "../world/partition/RegionDefinition.h"
#include "../world/terrain/TerrainService.h"
#include "BenchSnapshot.h"
#include "BenchWorld.h"

// MAP-2 runtime checks on file-backed worlds written to a scratch directory
// and loaded through the production loader (gs::game::LoadWorldPackage):
//  - terrain:  heights against an oracle that never touches the provider (the
//              generator function + an independent bilinear evaluation, and
//              hand-computed samples), seams, partial edge chunks, the world
//              edge, NotResident vs a legitimate 0 m, and the runtime rules at
//              the edge (movement without clamp, spawn fallback, mobs inside).
//  - mapsplit: forced split 1 -> 4 -> 16 and merge back on a non-flat,
//              negative-origin, non-square world with partial chunks; at every
//              step the topology, authority, population, mob heights and
//              partition-independent world queries are checked, and the
//              package is never loaded again. A zone at the min-size floor
//              stays unsplittable with a diagnostic. Plus the checked-in test
//              map under the approved R2 partitioning.
namespace gs::bench {
namespace {

namespace fs = std::filesystem;
namespace asio = boost::asio;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::string Fmt(const char* format, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

struct Checks {
    const char* tag;
    int failures = 0;
    int passes = 0;
    void Report(const std::string& name, bool pass, const std::string& detail)
    {
        std::printf("%s %s %s: %s\n", tag, name.c_str(), detail.c_str(), pass ? "PASS" : "FAIL");
        std::fflush(stdout);
        (pass ? passes : failures) += 1;
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
    character.id = gs::db::CharacterId{77000 + index};
    character.account_id = gs::db::AccountId{78000 + index};
    character.name = "MapRuntime" + std::to_string(index);
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

fs::path FixtureDir(const std::string& name)
{
    const fs::path dir = fs::temp_directory_path() / "ixw_mapruntime" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir;
}

std::optional<gs::game::LoadedWorld> WriteAndLoad(const fs::path& dir,
                                                  const mx::map::PackageWriteSpec& spec,
                                                  std::string& error, mx::map::WarpPolicy policy = mx::map::WarpPolicy::Strict)
{
    const auto written = mx::map::WritePackage(dir, spec);
    if (!written.ok) {
        error = "write: " + written.error;
        return std::nullopt;
    }
    mx::map::PackageReport report;
    gs::game::WorldLoadRequest request;
    request.package_root = dir;
    request.warp_policy=policy;
    request.mob_types_config = IXTREEME_DEFAULT_MOB_TYPES_CONFIG;
    auto world = gs::game::LoadWorldPackage(request, report);
    if (!world) {
        const auto* first = report.FirstError();
        error = first != nullptr ? first->Format() : std::string("refused without an error issue");
    }
    return world;
}

// ---- independent height oracle ---------------------------------------------------------
//
// Evaluates the GENERATOR function (the raw sample the fixture was written
// from) with its own cell lookup and bilinear blend in double. It shares no
// code and no data with mx::map::ServerTerrain: a wrong chunk lookup, a wrong
// partial-chunk stride, a swapped axis or a wrong scale/offset in the
// provider shows up as a mismatch.
struct OracleGrid {
    double origin_x = 0.0;
    double origin_y = 0.0;
    double cell = 1.0;
    std::uint32_t cells_x = 0;
    std::uint32_t cells_y = 0;
    double meters_per_unit = 0.01;
    double offset_m = 0.0;
    std::function<std::int32_t(std::uint32_t, std::uint32_t)> raw;

    double Meters(std::uint32_t vx, std::uint32_t vy) const
    {
        return offset_m + static_cast<double>(raw(vx, vy)) * meters_per_unit;
    }
    // Only for points inside the half-open world.
    double At(double x, double y) const
    {
        const double gx = (x - origin_x) / cell;
        const double gy = (y - origin_y) / cell;
        const auto ix = std::min<std::uint32_t>(static_cast<std::uint32_t>(std::floor(gx)), cells_x - 1);
        const auto iy = std::min<std::uint32_t>(static_cast<std::uint32_t>(std::floor(gy)), cells_y - 1);
        const double fx = gx - ix;
        const double fy = gy - iy;
        const double south = Meters(ix, iy) * (1.0 - fx) + Meters(ix + 1, iy) * fx;
        const double north = Meters(ix, iy + 1) * (1.0 - fx) + Meters(ix + 1, iy + 1) * fx;
        return south * (1.0 - fy) + north * fy;
    }
};

// Fixture A ("oracle world"): 70 x 45 cells of 8 m at origin (-150, -90), so
// the world is [-150, 410) x [-90, 270). 32-cell chunks: 3 x 2 chunks, the
// last column 6 cells wide, the last row 13 cells high. Bumpy, mostly
// negative relief plus two exact plateaus (0 m and 5 m) and a 3x3 blocked
// cell patch. Height layer v1 (int16 cm).
constexpr double kAOriginX = -150.0;
constexpr double kAOriginY = -90.0;
constexpr double kACell = 8.0;
constexpr std::uint32_t kACellsX = 70;
constexpr std::uint32_t kACellsY = 45;

std::int32_t RawA(std::uint32_t vx, std::uint32_t vy)
{
    if (vx >= 40 && vx <= 50 && vy >= 5 && vy <= 15) {
        return 0; // plateau at exactly 0 m (a legitimate height, not "no data")
    }
    if (vx >= 10 && vx <= 20 && vy >= 30 && vy <= 40) {
        return 500; // plateau at 5 m
    }
    return -1500 + 37 * static_cast<std::int32_t>(vx) - 23 * static_cast<std::int32_t>(vy) +
           static_cast<std::int32_t>((vx * 7 + vy * 13) % 11) * 9;
}

mx::map::PackageWriteSpec SpecA()
{
    mx::map::PackageWriteSpec spec;
    spec.world_id = "map2-oracle";
    spec.world_name = "MAP-2 oracle world";
    spec.size_cells_x = kACellsX;
    spec.size_cells_y = kACellsY;
    spec.origin_x = kAOriginX;
    spec.origin_y = kAOriginY;
    spec.cell_size_m = static_cast<float>(kACell);
    spec.chunk_size_cells = 32;
    spec.height_raw = RawA;
    spec.attributes = [](std::uint32_t cx, std::uint32_t cy) -> std::uint16_t {
        return cx >= 20 && cx <= 22 && cy >= 20 && cy <= 22 ? 0x0001 : 0;
    };
    spec.logic.zones = {{1, "all", {-150.0f, -90.0f, 410.0f, 270.0f}}};
    spec.logic.spawns = {{1, 1, {90.0f, 80.0f, 110.0f, 100.0f}}}; // centre (100, 90)
    // Two mob spawn circles that spill over the world edge (east edge, and the
    // negative south-west corner): every mob must still land inside.
    spec.mob_spawns = std::string("mob_type_id=1 x=400 y=100 count=40 radius=30\n"
                                  "mob_type_id=2 x=-145 y=-85 count=40 radius=20\n");
    spec.overwrite = true;
    return spec;
}

OracleGrid OracleA()
{
    OracleGrid o;
    o.origin_x = kAOriginX;
    o.origin_y = kAOriginY;
    o.cell = kACell;
    o.cells_x = kACellsX;
    o.cells_y = kACellsY;
    o.raw = RawA;
    return o;
}

// Fixture B: height layer v2, int32 samples, 1 mm per unit, offset -500 m: a
// ~3.3 km relief (version 1 stops at +-327 m). 64 x 64 cells of 4 m at origin
// (-2000, 3000).
std::int32_t RawB(std::uint32_t vx, std::uint32_t vy)
{
    return static_cast<std::int32_t>(vx * 50000u + vy * 1000u) - static_cast<std::int32_t>((vx * vy) % 7u) * 333;
}

// Fixture S ("split world"): 320 x 256 cells of 8 m at origin (-1280, -1024):
// [-1280, 1280) x [-1024, 1024), 96-cell chunks = 4 x 3 chunks, the last
// column 32 cells, the last row 64 cells. Rolling hills (+-~40 m).
constexpr double kSOriginX = -1280.0;
constexpr double kSOriginY = -1024.0;
constexpr double kSCell = 8.0;
constexpr std::uint32_t kSCellsX = 320;
constexpr std::uint32_t kSCellsY = 256;

std::int32_t RawS(std::uint32_t vx, std::uint32_t vy)
{
    const double h = 1200.0 * std::sin(vx * 0.07) + 800.0 * std::cos(vy * 0.05) + 4.0 * vx - 3.0 * vy;
    return static_cast<std::int32_t>(std::lround(h));
}

OracleGrid OracleS()
{
    OracleGrid o;
    o.origin_x = kSOriginX;
    o.origin_y = kSOriginY;
    o.cell = kSCell;
    o.cells_x = kSCellsX;
    o.cells_y = kSCellsY;
    o.raw = RawS;
    return o;
}

mx::map::PackageWriteSpec SpecS()
{
    mx::map::PackageWriteSpec spec;
    spec.world_id = "map2-split";
    spec.world_name = "MAP-2 split world";
    spec.size_cells_x = kSCellsX;
    spec.size_cells_y = kSCellsY;
    spec.origin_x = kSOriginX;
    spec.origin_y = kSOriginY;
    spec.cell_size_m = static_cast<float>(kSCell);
    spec.chunk_size_cells = 96;
    spec.height_raw = RawS;
    spec.attributes = [](std::uint32_t cx, std::uint32_t cy) -> std::uint16_t {
        return cx >= 100 && cx <= 103 && cy >= 100 && cy <= 103 ? 0x0001 : 0;
    };
    // Areas are metadata (AreaId namespace); the server partition below is
    // configured independently of them.
    spec.logic.zones = {{7, "west-march", {-1280.0f, -1024.0f, 0.0f, 1024.0f}},
                        {9000, "east-march", {0.0f, -1024.0f, 1280.0f, 1024.0f}}};
    spec.logic.spawns = {{1, 9000, {0.0f, 0.0f, 10.0f, 10.0f}}}; // centre (5, 5)
    std::string mobs;
    // One cluster per final 640 x 512 leaf, alternating mob types ...
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            mobs += Fmt("mob_type_id=%d x=%d y=%d count=20 radius=200\n", 1 + (i + j) % 2, -960 + 640 * i,
                        -768 + 512 * j);
        }
    }
    // ... plus clusters straddling the first and the second level cuts.
    mobs += "mob_type_id=1 x=0 y=0 count=30 radius=60\n";
    mobs += "mob_type_id=2 x=-640 y=512 count=20 radius=40\n";
    spec.mob_spawns = mobs;
    spec.overwrite = true;
    return spec;
}

// ---- snapshot probes ---------------------------------------------------------------------

struct TopoLeaf {
    gs::game::ZoneId id = 0;
    gs::game::RegionId region = 0;
    mx::map::Rect bounds;
    unsigned depth = 0;
};

struct TopoProbe {
    std::uint64_t epoch = 0;
    std::vector<TopoLeaf> leaves; // sorted by id
    std::size_t owners = 0;
    std::size_t players = 0;
    std::size_t mobs = 0;
    std::vector<std::uint32_t> net_ids; // every authoritative entity, sorted
    float max_mob_z_err = 0.0f;         // |z - terrain height| over all mobs
    std::size_t mobs_without_height = 0;
    std::string violation; // first violated invariant, empty = OK
};

// Exact at a quiescent supervisor point: unique authority, no staged/retired
// leftovers, the active leaves (reached from the partition roots, i.e. what
// the load monitor walks) tile the world, sit inside their own region, and
// equal the set of simulating leaf zones (nothing escapes the monitor), every
// entity is inside the world and every mob stands on the terrain.
TopoProbe ProbeTopology(const WorldSnapshot& snap, const gs::game::TerrainService& terrain)
{
    TopoProbe p;
    p.epoch = snap.epoch;
    p.owners = snap.owners.size();
    auto fail = [&p](std::string what) {
        if (p.violation.empty()) {
            p.violation = std::move(what);
        }
    };
    const auto world = terrain.Bounds();
    std::unordered_set<std::uint32_t> seen;
    std::unordered_set<gs::game::ZoneId> simulating_leaf_zones;
    std::size_t bound_players = 0;
    for (std::size_t i = 0; i < snap.zones.ZoneCount(); ++i) {
        const auto& zone = snap.zones.GetZone(i);
        if (zone.Partition() == gs::game::PartitionState::Staging) {
            fail(Fmt("staged zone=%u visible", zone.Id()));
        }
        if (zone.Partition() == gs::game::PartitionState::Retired &&
            (!zone.Entities().empty() || !zone.Players().empty())) {
            fail(Fmt("retired zone=%u holds entities", zone.Id()));
        }
        if (zone.Partition() == gs::game::PartitionState::Leaf && zone.SimulationEnabled()) {
            simulating_leaf_zones.insert(zone.Id());
        }
        bound_players += zone.Players().size();
        for (const auto& [net_id, entity] : zone.Entities()) {
            if (!seen.insert(net_id).second) {
                fail(Fmt("net=%u authoritative twice", net_id));
            }
            p.net_ids.push_back(net_id);
            if (!entity.is_valid() || !entity.has<gs::game::Position>()) {
                fail(Fmt("net=%u has no position", net_id));
                continue;
            }
            const auto pos = entity.get<gs::game::Position>();
            if (!terrain.Contains(pos.x, pos.y)) {
                fail(Fmt("net=%u at (%.3f, %.3f) outside the world", net_id, pos.x, pos.y));
            }
            if (entity.has<gs::game::PlayerTag>()) {
                ++p.players;
            } else if (entity.has<gs::game::MobTag>() && !entity.has<gs::game::GhostTag>()) {
                ++p.mobs;
                const auto ground = terrain.Height(pos.x, pos.y);
                if (!ground.Ok()) {
                    ++p.mobs_without_height;
                } else {
                    p.max_mob_z_err = std::max(p.max_mob_z_err, std::abs(pos.z - ground.meters));
                }
            }
        }
    }
    std::sort(p.net_ids.begin(), p.net_ids.end());
    if (bound_players != p.owners) {
        fail(Fmt("player bindings %zu != owners %zu", bound_players, p.owners));
    }
    double area = 0.0;
    for (const auto* leaf : snap.zones.GetActiveLeaves()) {
        TopoLeaf l{leaf->zone_id, leaf->region_id, leaf->bounds, leaf->depth};
        p.leaves.push_back(l);
        area += static_cast<double>(l.bounds.max_x - l.bounds.min_x) * (l.bounds.max_y - l.bounds.min_y);
        if (simulating_leaf_zones.erase(l.id) == 0) {
            fail(Fmt("active leaf %u is not a simulating leaf zone", l.id));
        }
        const gs::game::RegionDefinition* region = nullptr;
        for (const auto& r : snap.zones.Regions()) {
            if (r.id == l.region) {
                region = &r;
            }
        }
        if (region == nullptr || l.bounds.min_x < region->bounds.min_x || l.bounds.min_y < region->bounds.min_y ||
            l.bounds.max_x > region->bounds.max_x || l.bounds.max_y > region->bounds.max_y) {
            fail(Fmt("leaf %u not inside its region %u", l.id, l.region));
        }
    }
    if (!simulating_leaf_zones.empty()) {
        fail(Fmt("%zu simulating leaf zone(s) unreachable from the partition roots (escape the load monitor)",
                 simulating_leaf_zones.size()));
    }
    for (std::size_t a = 0; a < p.leaves.size(); ++a) {
        for (std::size_t b = a + 1; b < p.leaves.size(); ++b) {
            const auto& x = p.leaves[a].bounds;
            const auto& y = p.leaves[b].bounds;
            if (x.min_x < y.max_x && y.min_x < x.max_x && x.min_y < y.max_y && y.min_y < x.max_y) {
                fail(Fmt("leaves %u and %u overlap", p.leaves[a].id, p.leaves[b].id));
            }
        }
    }
    const double world_area = static_cast<double>(world.ExtentX()) * world.ExtentY();
    if (std::abs(area - world_area) > world_area * 1e-6) {
        fail(Fmt("leaves cover %.0f of %.0f m2", area, world_area));
    }
    std::sort(p.leaves.begin(), p.leaves.end(),
              [](const TopoLeaf& a, const TopoLeaf& b) { return a.id < b.id; });
    return p;
}

// Partition-independent world queries: the terrain answer at a world point
// never depends on the zone topology (it is not a per-zone copy), and the
// point's owner is exactly one active leaf (half-open), resolved the same way
// by FindIndexForPosition; points outside the world have no owner.
struct QueryPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct TerrainAnswer {
    mx::map::TerrainStatus height_status = mx::map::TerrainStatus::OutsideWorld;
    float meters = 0.0f;
    mx::map::TerrainStatus cell_status = mx::map::TerrainStatus::OutsideWorld;
    std::uint16_t attributes = 0;
    bool operator==(const TerrainAnswer& o) const
    {
        return height_status == o.height_status && cell_status == o.cell_status &&
               (height_status != mx::map::TerrainStatus::Ok || std::memcmp(&meters, &o.meters, sizeof(float)) == 0) &&
               (cell_status != mx::map::TerrainStatus::Ok || attributes == o.attributes);
    }
};

std::vector<TerrainAnswer> AskTerrain(const gs::game::TerrainService& terrain, const std::vector<QueryPoint>& points)
{
    std::vector<TerrainAnswer> out;
    out.reserve(points.size());
    for (const auto& q : points) {
        const auto h = terrain.Height(q.x, q.y);
        const auto c = terrain.Cell(q.x, q.y);
        out.push_back({h.status, h.meters, c.status, c.attributes});
    }
    return out;
}

struct OwnerCheck {
    std::size_t inside = 0;
    std::size_t outside = 0;
    std::string violation;
};

OwnerCheck CheckOwners(const WorldSnapshot& snap, const gs::game::TerrainService& terrain,
                       const std::vector<QueryPoint>& points)
{
    OwnerCheck out;
    const auto leaves = snap.zones.GetActiveLeaves();
    for (const auto& q : points) {
        std::size_t owners = 0;
        gs::game::ZoneId owner = 0;
        for (const auto* leaf : leaves) {
            if (leaf->bounds.ContainsHalfOpen(q.x, q.y)) {
                ++owners;
                owner = leaf->zone_id;
            }
        }
        const std::size_t index = snap.zones.FindIndexForPosition(q.x, q.y);
        const bool inside = terrain.Contains(q.x, q.y);
        (inside ? out.inside : out.outside) += 1;
        std::string v;
        if (inside && owners != 1) {
            v = Fmt("(%.3f, %.3f) inside the world owned by %zu leaves", q.x, q.y, owners);
        } else if (!inside && owners != 0) {
            v = Fmt("(%.3f, %.3f) outside the world owned by leaf %u", q.x, q.y, owner);
        } else if (inside && (index >= snap.zones.ZoneCount() || snap.zones.GetZone(index).Id() != owner)) {
            v = Fmt("(%.3f, %.3f) FindIndexForPosition disagrees with the leaf scan", q.x, q.y);
        } else if (!inside && index < snap.zones.ZoneCount()) {
            v = Fmt("(%.3f, %.3f) outside the world resolved to zone %u", q.x, q.y, snap.zones.GetZone(index).Id());
        }
        if (!v.empty() && out.violation.empty()) {
            out.violation = v;
        }
    }
    return out;
}

std::vector<QueryPoint> QueryPoints(const gs::game::WorldBounds& b, const std::vector<float>& cuts_x,
                                    const std::vector<float>& cuts_y, std::uint32_t seed, int random_count)
{
    std::vector<QueryPoint> points;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> ux(b.min_x, b.max_x);
    std::uniform_real_distribution<float> uy(b.min_y, b.max_y);
    for (int i = 0; i < random_count; ++i) {
        QueryPoint q{ux(rng), uy(rng)};
        if (q.x < b.max_x && q.y < b.max_y) {
            points.push_back(q);
        }
    }
    const float below_max_x = std::nextafter(b.max_x, b.min_x);
    const float below_max_y = std::nextafter(b.max_y, b.min_y);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // Corners, outer edges (max is outside), just inside / outside.
    for (const QueryPoint q : {QueryPoint{b.min_x, b.min_y}, QueryPoint{below_max_x, below_max_y},
                               QueryPoint{b.max_x, 0.5f * (b.min_y + b.max_y)},
                               QueryPoint{0.5f * (b.min_x + b.max_x), b.max_y}, QueryPoint{b.max_x, b.max_y},
                               QueryPoint{std::nextafter(b.min_x, -1e9f), b.min_y},
                               QueryPoint{b.min_x, std::nextafter(b.min_y, -1e9f)}, QueryPoint{nan, 0.0f},
                               QueryPoint{0.0f, nan}, QueryPoint{b.min_x - 500.0f, b.min_y - 500.0f}}) {
        points.push_back(q);
    }
    // Every cut line (exactly on it and one float step either side), sampled
    // along its whole length.
    for (const float cx : cuts_x) {
        for (int k = 0; k < 64; ++k) {
            const float y = b.min_y + (b.max_y - b.min_y) * (static_cast<float>(k) + 0.37f) / 64.0f;
            points.push_back({cx, y});
            points.push_back({std::nextafter(cx, -1e9f), y});
            points.push_back({std::nextafter(cx, 1e9f), y});
        }
        for (const float cy : cuts_y) {
            points.push_back({cx, cy}); // where four leaves meet
        }
    }
    for (const float cy : cuts_y) {
        for (int k = 0; k < 64; ++k) {
            const float x = b.min_x + (b.max_x - b.min_x) * (static_cast<float>(k) + 0.61f) / 64.0f;
            points.push_back({x, cy});
            points.push_back({x, std::nextafter(cy, -1e9f)});
            points.push_back({x, std::nextafter(cy, 1e9f)});
        }
    }
    return points;
}

struct PlayerPos {
    bool found = false;
    gs::game::Position pos;
};

PlayerPos ReadPlayer(gs::game::WorldRuntime& sim, gs::common::SessionId session)
{
    return ReadWorld(sim, [session](const WorldSnapshot& snap) {
        PlayerPos out;
        const auto it = snap.owners.find(session);
        if (it == snap.owners.end() || it->second.zone_index >= snap.zones.ZoneCount()) {
            return out;
        }
        const auto entity = snap.zones.GetZone(it->second.zone_index).FindEntity(it->second.net_id);
        if (entity.is_valid() && entity.has<gs::game::Position>()) {
            out.found = true;
            out.pos = entity.get<gs::game::Position>();
        }
        return out;
    });
}

gs::game::ZoneId LeafAt(gs::game::WorldRuntime& sim, float x, float y)
{
    return ReadWorld(sim, [x, y](const WorldSnapshot& snap) -> gs::game::ZoneId {
        for (const auto* leaf : snap.zones.GetActiveLeaves()) {
            if (leaf->bounds.ContainsHalfOpen(x, y)) {
                return leaf->zone_id;
            }
        }
        return 0;
    });
}

std::size_t CountSplitRefusals(gs::game::WorldRuntime& sim, gs::game::ZoneId zone, std::string& detail,
                               gs::game::PartitionNoopReason& reason)
{
    std::size_t n = 0;
    for (const auto& record : sim.PartitionDecisionLog()) {
        if (record.kind == gs::game::PartitionDecisionKind::Split && record.zone_id == zone && !record.executed) {
            ++n;
            detail = record.detail != nullptr ? record.detail : "";
            reason = record.noop_reason;
        }
    }
    return n;
}

// Forced topology steps with their completion wait (the transactions run on
// the supervisor; commits are counted by the partition metrics).
bool ForceSplit(gs::game::WorldRuntime& sim, gs::game::ZoneId zone)
{
    const auto before = sim.PartitionMetricsSnapshot().split_commits;
    sim.PostForceSplit(zone);
    return WaitFor(10000ms, [&] { return sim.PartitionMetricsSnapshot().split_commits > before; });
}

bool ForceMerge(gs::game::WorldRuntime& sim, gs::game::ZoneId parent)
{
    const auto before = sim.PartitionMetricsSnapshot().merge_commits;
    sim.PostForceMerge(parent);
    return WaitFor(10000ms, [&] { return sim.PartitionMetricsSnapshot().merge_commits > before; });
}

// A forced split the rules must refuse: waits for the why-not record, then
// makes sure nothing committed.
struct Refusal {
    bool refused = false;
    std::string detail;
    gs::game::PartitionNoopReason reason = gs::game::PartitionNoopReason::None;
    bool committed = false;
};

Refusal ForceSplitExpectRefusal(gs::game::WorldRuntime& sim, gs::game::ZoneId zone)
{
    Refusal out;
    std::string detail;
    gs::game::PartitionNoopReason reason{};
    const std::size_t before = CountSplitRefusals(sim, zone, detail, reason);
    const auto commits = sim.PartitionMetricsSnapshot().split_commits;
    sim.PostForceSplit(zone);
    out.refused = WaitFor(10000ms, [&] { return CountSplitRefusals(sim, zone, detail, reason) > before; });
    std::this_thread::sleep_for(300ms);
    out.detail = detail;
    out.reason = reason;
    out.committed = sim.PartitionMetricsSnapshot().split_commits != commits;
    return out;
}

std::string LeafSummary(const TopoProbe& p)
{
    if (p.leaves.empty()) {
        return "none";
    }
    float min_w = 1e30f;
    float max_w = 0.0f;
    float min_h = 1e30f;
    float max_h = 0.0f;
    for (const auto& l : p.leaves) {
        min_w = std::min(min_w, l.bounds.max_x - l.bounds.min_x);
        max_w = std::max(max_w, l.bounds.max_x - l.bounds.min_x);
        min_h = std::min(min_h, l.bounds.max_y - l.bounds.min_y);
        max_h = std::max(max_h, l.bounds.max_y - l.bounds.min_y);
    }
    return Fmt("%zu leaves %.0fx%.0f..%.0fx%.0f m", p.leaves.size(), min_w, min_h, max_w, max_h);
}

} // namespace

// ============================================================================
// MAP-2: terrain oracle + runtime edge rules
// ============================================================================
int RunTerrainScenario()
{
    Checks c{"TERRAIN"};
    // ---- A: static queries against the independent oracle ----
    std::string error;
    auto loaded = WriteAndLoad(FixtureDir("oracle_world"), SpecA(), error);
    c.Report("fixture-load", loaded.has_value(),
             loaded ? Fmt("world [(%.0f,%.0f),(%.0f,%.0f)) cells=%ux%u chunks=%zu", loaded->terrain.Geometry().MinX(),
                          loaded->terrain.Geometry().MinY(), loaded->terrain.Geometry().MaxX(),
                          loaded->terrain.Geometry().MaxY(), loaded->terrain.Geometry().cells_x,
                          loaded->terrain.Geometry().cells_y, loaded->terrain.ChunkCount())
                    : "REFUSED: " + error);
    if (!loaded) {
        std::printf("TERRAIN-DONE failures=%d\n", c.failures);
        return c.failures;
    }
    const mx::map::ServerTerrain& t = loaded->terrain;
    const OracleGrid oracle = OracleA();
    {
        std::mt19937 rng(20260925);
        std::uniform_real_distribution<double> ux(-150.0, 410.0);
        std::uniform_real_distribution<double> uy(-90.0, 270.0);
        double max_err = 0.0;
        int not_ok = 0;
        int negative = 0;
        int n = 0;
        for (; n < 20000; ++n) {
            const double x = ux(rng);
            const double y = uy(rng);
            const auto h = t.Height(x, y);
            if (!h.Ok()) {
                ++not_ok;
                continue;
            }
            negative += h.meters < 0.0f ? 1 : 0;
            max_err = std::max(max_err, std::abs(h.meters - oracle.At(x, y)));
        }
        c.Report("oracle-random", not_ok == 0 && max_err < 1e-4 && negative > 0,
                 Fmt("points=%d not_ok=%d negative=%d max_abs_err=%.2e m", n, not_ok, negative, max_err));
    }
    {
        // Hand-computed from RawA (cm):
        //  vertex (0,0)   = -1500                                    -> -15.00 m
        //  vertex (70,45) = -1500+2590-1035+((490+585)%11)*9 = 127   ->   1.27 m (outer corner sample)
        //  centre of cell (0,0) (-146,-86): corners -1500, -1400 (x+1), -1505 (y+1), -1405
        //                   -> mean -1452.5                          -> -14.525 m
        //  (212,-6): cell (45,10), all corners on the 0 m plateau     ->   0.00 m
        //  (-26,194): cell (15,35), 5 m plateau                       ->   5.00 m
        //  (254,-10): cell (50,10) x-mid, y on the cell edge: corners 0 (vx=50) and
        //             RawA(51,10) = -1500+1887-230+((357+130)%11)*9 = 184 -> 0.92 m
        struct Hand {
            double x, y, expect;
        };
        const Hand samples[] = {{-150.0, -90.0, -15.00}, {-146.0, -86.0, -14.525}, {212.0, -6.0, 0.0},
                                {-26.0, 194.0, 5.0},     {254.0, -10.0, 0.92}};
        double max_err = 0.0;
        bool all_ok = true;
        std::string detail;
        for (const auto& s : samples) {
            const auto h = t.Height(s.x, s.y);
            all_ok = all_ok && h.Ok();
            max_err = std::max(max_err, std::abs(h.meters - s.expect));
            detail += Fmt("(%.0f,%.0f)=%.3f ", s.x, s.y, h.meters);
        }
        const auto corner = t.Vertex(70, 45);
        const bool zero_exact = t.Height(212.0, -6.0).Ok() && t.Height(212.0, -6.0).meters == 0.0f;
        c.Report("oracle-hand-checked",
                 all_ok && max_err < 1e-4 && corner.Ok() && std::abs(corner.meters - 1.27f) < 1e-4f && zero_exact,
                 detail + Fmt("vertex(70,45)=%.3f max_abs_err=%.2e zero_plateau_exact=%d", corner.meters, max_err,
                              zero_exact ? 1 : 0));
    }
    {
        // Levels: plateaus are flat to the last bit, the step between the
        // 0 m plateau and the relief is a linear ramp inside one cell.
        double max_plateau_dev = 0.0;
        for (int i = 0; i < 400; ++i) {
            const double fx = (i % 20) / 20.0;
            const double fy = (i / 20) / 20.0;
            max_plateau_dev = std::max<double>(max_plateau_dev, std::abs(t.Height(170.0 + 80.0 * fx, -50.0 + 80.0 * fy).meters));
            max_plateau_dev =
                std::max<double>(max_plateau_dev, std::abs(t.Height(-70.0 + 80.0 * fx, 150.0 + 80.0 * fy).meters - 5.0));
        }
        // Ramp in cell (50,10) on its south edge: 0 m at x=250, 1.84 m at x=258.
        const double ramp_err = std::max({std::abs(t.Height(252.0, -10.0).meters - 0.46),
                                          std::abs(t.Height(254.0, -10.0).meters - 0.92),
                                          std::abs(t.Height(256.0, -10.0).meters - 1.38)});
        c.Report("levels-and-step", max_plateau_dev == 0.0 && ramp_err < 1e-4,
                 Fmt("plateau_max_dev=%.2e m ramp(x=252,254,256)=%.3f,%.3f,%.3f (expect 0.460, 0.920, 1.380)",
                     max_plateau_dev, t.Height(252.0, -10.0).meters, t.Height(254.0, -10.0).meters,
                     t.Height(256.0, -10.0).meters));
    }
    {
        // Slope: height differences over 0.25 m steps follow the oracle.
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> ux(-149.0, 409.0);
        std::uniform_real_distribution<double> uy(-89.0, 269.0);
        double max_err = 0.0;
        double steepest = 0.0;
        for (int i = 0; i < 2000; ++i) {
            const double x = ux(rng);
            const double y = uy(rng);
            const double dx = t.Height(x + 0.25, y).meters - t.Height(x, y).meters;
            const double dy = t.Height(x, y + 0.25).meters - t.Height(x, y).meters;
            max_err = std::max({max_err, std::abs(dx - (oracle.At(x + 0.25, y) - oracle.At(x, y))),
                                std::abs(dy - (oracle.At(x, y + 0.25) - oracle.At(x, y)))});
            steepest = std::max({steepest, std::abs(dx) / 0.25, std::abs(dy) / 0.25});
        }
        c.Report("slope", max_err < 2e-4 && steepest > 0.01,
                 Fmt("pairs=2000 max_abs_diff_err=%.2e m steepest=%.3f m/m", max_err, steepest));
    }
    {
        // Seams: chunk borders at x = -150+256 = 106, x = 362, y = -90+256 = 166.
        // Exactly on the line, one step either side, and the 4-chunk corner.
        double max_err = 0.0;
        double max_jump = 0.0;
        int not_ok = 0;
        auto probe = [&](double x, double y) {
            const auto h = t.Height(x, y);
            if (!h.Ok()) {
                ++not_ok;
                return;
            }
            max_err = std::max(max_err, std::abs(h.meters - oracle.At(x, y)));
        };
        for (int k = 0; k < 200; ++k) {
            const double along_y = -90.0 + 360.0 * (k + 0.5) / 200.0;
            const double along_x = -150.0 + 560.0 * (k + 0.5) / 200.0;
            for (const double sx : {106.0, 362.0}) {
                probe(sx, along_y);
                probe(sx - 1e-3, along_y);
                probe(sx + 1e-3, along_y);
                max_jump = std::max<double>(max_jump, std::abs(t.Height(sx - 1e-6, along_y).meters -
                                                       t.Height(sx, along_y).meters));
            }
            probe(along_x, 166.0);
            probe(along_x, 166.0 - 1e-3);
            probe(along_x, 166.0 + 1e-3);
            max_jump = std::max<double>(max_jump, std::abs(t.Height(along_x, 166.0 - 1e-6).meters -
                                                   t.Height(along_x, 166.0).meters));
        }
        probe(106.0, 166.0);
        probe(362.0, 166.0);
        // Duplicated border samples are identical in both chunks.
        const auto* a = t.ChunkAt(0, 0);
        const auto* b = t.ChunkAt(1, 0);
        bool dup_equal = a != nullptr && b != nullptr;
        for (std::uint32_t row = 0; dup_equal && row <= a->cells_y; ++row) {
            dup_equal = a->heights16[row * (a->cells_x + 1) + a->cells_x] == b->heights16[row * (b->cells_x + 1)];
        }
        c.Report("seams", not_ok == 0 && max_err < 1e-4 && max_jump < 1e-4 && dup_equal,
                 Fmt("not_ok=%d max_abs_err=%.2e continuity_jump=%.2e border_samples_equal=%d", not_ok, max_err,
                     max_jump, dup_equal ? 1 : 0));
    }
    {
        // Partial edge chunks: column 2 (cells 64..69, x >= 362) and row 1
        // (cells 32..44, y >= 166) hold only their in-world cells.
        const auto* corner = t.ChunkAt(2, 1);
        double max_err = 0.0;
        int not_ok = 0;
        std::mt19937 rng(11);
        std::uniform_real_distribution<double> ux(362.0, 410.0);
        std::uniform_real_distribution<double> uy(166.0, 270.0);
        for (int i = 0; i < 2000; ++i) {
            const double x = ux(rng);
            const double y = uy(rng);
            const auto h = t.Height(x, y);
            if (!h.Ok()) {
                ++not_ok;
                continue;
            }
            max_err = std::max(max_err, std::abs(h.meters - oracle.At(x, y)));
        }
        c.Report("partial-edge-chunk",
                 corner != nullptr && corner->cells_x == 6 && corner->cells_y == 13 &&
                     corner->heights16.size() == 7u * 14u && not_ok == 0 && max_err < 1e-4,
                 Fmt("chunk(2,1)=%ux%u cells samples=%zu not_ok=%d max_abs_err=%.2e",
                     corner != nullptr ? corner->cells_x : 0, corner != nullptr ? corner->cells_y : 0,
                     corner != nullptr ? corner->heights16.size() : 0, not_ok, max_err));
    }
    {
        // World edge: [min, max) on both axes, non-finite input outside.
        using mx::map::TerrainStatus;
        const double inf = std::numeric_limits<double>::infinity();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        struct Edge {
            const char* what;
            double x, y;
            TerrainStatus expect;
        };
        const Edge edges[] = {
            {"x=max", 410.0, 100.0, TerrainStatus::OutsideWorld},
            {"x=below-max", std::nextafter(410.0, 0.0), 100.0, TerrainStatus::Ok},
            {"x=min", -150.0, 100.0, TerrainStatus::Ok},
            {"x=below-min", std::nextafter(-150.0, -inf), 100.0, TerrainStatus::OutsideWorld},
            {"y=max", 100.0, 270.0, TerrainStatus::OutsideWorld},
            {"y=below-max", 100.0, std::nextafter(270.0, 0.0), TerrainStatus::Ok},
            {"y=min", 100.0, -90.0, TerrainStatus::Ok},
            {"y=below-min", 100.0, std::nextafter(-90.0, -inf), TerrainStatus::OutsideWorld},
            {"corner=max", 410.0, 270.0, TerrainStatus::OutsideWorld},
            {"nan", nan, 0.0, TerrainStatus::OutsideWorld},
            {"inf", inf, 0.0, TerrainStatus::OutsideWorld},
            {"-inf", 0.0, -inf, TerrainStatus::OutsideWorld},
        };
        bool ok = true;
        std::string bad;
        for (const auto& e : edges) {
            const auto h = t.Height(e.x, e.y);
            const auto cell = t.Cell(e.x, e.y);
            const bool match = h.status == e.expect && cell.status == e.expect &&
                               (e.expect != TerrainStatus::Ok ||
                                std::abs(h.meters - oracle.At(e.x, e.y)) < 1e-4) &&
                               (e.expect == TerrainStatus::Ok || !cell.Walkable());
            if (!match) {
                ok = false;
                bad += Fmt(" %s:%s/%s", e.what, mx::map::ToString(h.status), mx::map::ToString(cell.status));
            }
        }
        // The same through the runtime (f32) service.
        const gs::game::TerrainService svc(t.Clone());
        const auto b = svc.Bounds();
        const bool svc_ok = b.min_x == -150.0f && b.min_y == -90.0f && b.max_x == 410.0f && b.max_y == 270.0f &&
                            !svc.Contains(410.0f, 0.0f) && svc.Contains(std::nextafter(410.0f, 0.0f), 0.0f) &&
                            !svc.Height(410.0f, 0.0f).Ok() && !svc.IsWalkable(410.0f, 0.0f) &&
                            svc.Height(-150.0f, -90.0f).Ok();
        c.Report("world-edge-half-open", ok && svc_ok,
                 Fmt("cases=%zu mismatches=[%s] service_bounds=[(%.0f,%.0f),(%.0f,%.0f)) service_ok=%d",
                     std::size(edges), bad.c_str(), b.min_x, b.min_y, b.max_x, b.max_y, svc_ok ? 1 : 0));
    }
    {
        // Unavailable data vs a legitimate 0 m: the 0 m plateau lies in chunk
        // (1,0). Evicted, the same point answers NotResident, never 0 m /
        // walkable. Also proves single ownership of the seam: x = 106 (the
        // border sample column) belongs to chunk 1 only.
        using mx::map::TerrainStatus;
        mx::map::ServerTerrain evicted = t.Clone();
        evicted.EvictChunkForTest(1, 0);
        const auto zero = t.Height(212.0, -6.0);
        const auto gone = evicted.Height(212.0, -6.0);
        const auto gone_cell = evicted.Cell(212.0, -6.0);
        const auto seam = evicted.Height(106.0, 0.0);
        const auto west_of_seam = evicted.Height(std::nextafter(106.0, 0.0), 0.0);
        // North seam y = 166: just below it is still row 0 (the evicted
        // chunk), on it is row 1 (resident).
        const auto south_of_seam = evicted.Height(200.0, std::nextafter(166.0, 0.0));
        const auto on_north_seam = evicted.Height(200.0, 166.0);
        const auto vertex = evicted.Vertex(45, 10);
        const gs::game::TerrainService svc(evicted.Clone());
        const bool ok = zero.Ok() && zero.meters == 0.0f && gone.status == TerrainStatus::NotResident &&
                        gone_cell.status == TerrainStatus::NotResident && !gone_cell.Walkable() &&
                        seam.status == TerrainStatus::NotResident && west_of_seam.Ok() &&
                        south_of_seam.status == TerrainStatus::NotResident && on_north_seam.Ok() &&
                        vertex.status == TerrainStatus::NotResident && !svc.IsWalkable(212.0f, -6.0f) &&
                        svc.Height(212.0f, -6.0f).status == TerrainStatus::NotResident &&
                        evicted.ResidentChunkCount() + 1 == t.ResidentChunkCount();
        c.Report("not-resident-vs-zero-meters", ok,
                 Fmt("loaded=%s %.2f m | evicted=%s cell=%s walkable=%d | seam x=106 -> %s, x=106- -> %s, "
                     "y=166- -> %s, y=166 -> %s | resident %zu -> %zu",
                     mx::map::ToString(zero.status), zero.meters, mx::map::ToString(gone.status),
                     mx::map::ToString(gone_cell.status), gone_cell.Walkable() ? 1 : 0,
                     mx::map::ToString(seam.status), mx::map::ToString(west_of_seam.status),
                     mx::map::ToString(south_of_seam.status), mx::map::ToString(on_north_seam.status),
                     t.ResidentChunkCount(), evicted.ResidentChunkCount()));
    }
    // ---- B: height layer v2 (int32, scaled, offset) against the oracle ----
    {
        mx::map::PackageWriteSpec spec;
        spec.world_id = "map2-int32";
        spec.world_name = "MAP-2 int32 heights";
        spec.size_cells_x = 64;
        spec.origin_x = -2000.0;
        spec.origin_y = 3000.0;
        spec.cell_size_m = 4.0f;
        spec.chunk_size_cells = 32;
        mx::map::HeightEncoding enc;
        enc.layer_version = 2;
        enc.int32_samples = true;
        enc.meters_per_unit = 0.001;
        enc.offset_m = -500.0;
        spec.height_encoding = enc;
        spec.height_raw = RawB;
        spec.logic.zones = {{1, "all", {-2000.0f, 3000.0f, -1744.0f, 3256.0f}}};
        spec.logic.spawns = {{1, 1, {-1900.0f, 3100.0f, -1890.0f, 3110.0f}}};
        spec.overwrite = true;
        std::string err;
        const auto world = WriteAndLoad(FixtureDir("int32_world"), spec, err);
        OracleGrid ob;
        ob.origin_x = -2000.0;
        ob.origin_y = 3000.0;
        ob.cell = 4.0;
        ob.cells_x = 64;
        ob.cells_y = 64;
        ob.meters_per_unit = 0.001;
        ob.offset_m = -500.0;
        ob.raw = RawB;
        double max_err = 0.0;
        float top = 0.0f;
        int not_ok = 0;
        if (world) {
            std::mt19937 rng(3);
            std::uniform_real_distribution<double> ux(-2000.0, -1744.0);
            std::uniform_real_distribution<double> uy(3000.0, 3256.0);
            for (int i = 0; i < 5000; ++i) {
                const double x = ux(rng);
                const double y = uy(rng);
                const auto h = world->terrain.Height(x, y);
                if (!h.Ok()) {
                    ++not_ok;
                    continue;
                }
                max_err = std::max(max_err, std::abs(h.meters - ob.At(x, y)));
            }
            // vertex (64,64): 64*50000 + 64*1000 - (4096 % 7 = 1)*333 = 3263667 mm -> 2763.667 m
            top = world->terrain.Vertex(64, 64).meters;
        }
        c.Report("height-v2-int32-oracle", world && not_ok == 0 && max_err < 1e-3 && std::abs(top - 2763.667f) < 1e-3f,
                 world ? Fmt("points=5000 not_ok=%d max_abs_err=%.2e m vertex(64,64)=%.3f m (expect 2763.667)",
                             not_ok, max_err, top)
                       : "REFUSED: " + err);
    }

    // ---- C: runtime rules at the world edge (fixture A, file-backed) ----
    {
        const std::size_t spawn_lines = loaded->spawn_points.size();
        IoRunner runner;
        gs::game::PartitionLayout layout;
        layout.regions_x = layout.regions_y = 1;
        gs::game::WorldRuntime sim(runner.io, {}, std::move(*loaded), layout);
        gs::game::PartitionConfig partition;
        partition.scoring.adaptive_enabled = false;
        sim.ConfigurePartition(partition);
        const auto& terrain = sim.Terrain();

        struct Player {
            gs::common::SessionId session;
            std::optional<gs::game::DebugSpawnOverride> debug;
            float heading; // radians: 0 = north (+Y), pi/2 = east (+X)
            bool moves;
        };
        constexpr float kPi = 3.14159265f;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const Player players[] = {
            {92001, gs::game::DebugSpawnOverride{406.0f, 100.0f}, 0.5f * kPi, true},     // east edge
            {92002, gs::game::DebugSpawnOverride{-146.0f, -86.0f}, -0.75f * kPi, true}, // SW corner, negative
            {92003, gs::game::DebugSpawnOverride{200.0f, 266.0f}, 0.25f * kPi, true},   // north edge, slides east
            {92004, gs::game::DebugSpawnOverride{410.0f, 100.0f}, 0.0f, false},         // exactly on max: outside
            {92005, gs::game::DebugSpawnOverride{1000.0f, 1000.0f}, 0.0f, false},       // far outside
            {92006, gs::game::DebugSpawnOverride{nan, 0.0f}, 0.0f, false},              // non-finite
            {92007, gs::game::DebugSpawnOverride{22.0f, 82.0f}, 0.0f, false},           // blocked cell
            {92008, std::nullopt, 0.0f, false},                                         // normal rule
        };
        std::uint64_t index = 0;
        for (const auto& p : players) {
            auto character = MakeCharacter(index++);
            if (p.session == 92008) {
                // A valid, in-world stored DB position (300, 200): it must NOT
                // take precedence over the player spawn region.
                character.pos_x = static_cast<std::int32_t>(300.0f * gs::game::kDbUnitsPerMeter);
                character.pos_y = static_cast<std::int32_t>(200.0f * gs::game::kDbUnitsPerMeter);
            }
            sim.PostSpawn(DetachedSession(runner.io, p.session), std::move(character), p.debug);
        }
        sim.Start();
        const bool entered = WaitFor(10000ms, [&] {
            return ReadWorld(sim, [](const WorldSnapshot& snap) { return snap.owners.size(); }) == std::size(players);
        });
        std::uint32_t seq = 0;
        const auto run_until = Clock::now() + 2500ms;
        while (Clock::now() < run_until) {
            for (const auto& p : players) {
                if (p.moves) {
                    sim.PostMoveInput(p.session, ++seq, p.heading, gs::game::MoveState::Running);
                }
            }
            std::this_thread::sleep_for(50ms);
        }
        for (const auto& p : players) {
            if (p.moves) {
                sim.PostMoveInput(p.session, ++seq, p.heading, gs::game::MoveState::Idle);
            }
        }
        std::this_thread::sleep_for(300ms);
        const auto east = ReadPlayer(sim, 92001);
        const auto sw = ReadPlayer(sim, 92002);
        const auto north = ReadPlayer(sim, 92003);
        constexpr float kStep = 6.0f * 0.05f; // run speed x tick
        auto z_err = [&](const PlayerPos& p) {
            return p.found ? std::abs(p.pos.z - static_cast<float>(oracle.At(p.pos.x, p.pos.y))) : 1e9f;
        };
        c.Report("runtime-players-entered", entered, Fmt("owners=%zu/%zu", ReadWorld(sim, [](const WorldSnapshot& s) {
                                                             return s.owners.size();
                                                         }),
                                                         std::size(players)));
        c.Report("move-east-edge-no-clamp",
                 east.found && east.pos.x < 410.0f && east.pos.x > 410.0f - kStep - 1e-3f && std::abs(east.pos.y - 100.0f) < 1e-3f &&
                     z_err(east) < 1e-3f,
                 Fmt("start (406,100) -> (%.4f, %.4f) z=%.4f oracle_z_err=%.2e (max x is outside; a clamp would sit "
                     "on/after the edge)",
                     east.pos.x, east.pos.y, east.pos.z, z_err(east)));
        c.Report("move-southwest-negative-corner",
                 sw.found && sw.pos.x >= -150.0f && sw.pos.y >= -90.0f && sw.pos.x < -150.0f + kStep &&
                     sw.pos.y < -90.0f + kStep && z_err(sw) < 1e-3f,
                 Fmt("start (-146,-86) -> (%.4f, %.4f) z=%.4f oracle_z_err=%.2e", sw.pos.x, sw.pos.y, sw.pos.z,
                     z_err(sw)));
        c.Report("move-north-edge-per-axis",
                 north.found && north.pos.y < 270.0f && north.pos.y > 270.0f - kStep && north.pos.x > 205.0f &&
                     z_err(north) < 1e-3f,
                 Fmt("start (200,266) heading NE -> (%.4f, %.4f): y refused at the edge, x keeps moving", north.pos.x,
                     north.pos.y));
        bool fallback_ok = true;
        std::string fallback;
        for (const gs::common::SessionId s : {92004, 92005, 92006, 92007, 92008}) {
            const auto p = ReadPlayer(sim, s);
            fallback_ok = fallback_ok && p.found && p.pos.x == 100.0f && p.pos.y == 90.0f && z_err(p) < 1e-3f;
            fallback += Fmt(" %llu:(%.1f,%.1f)", static_cast<unsigned long long>(s), p.pos.x, p.pos.y);
        }
        c.Report("spawn-invalid-debug-falls-back-to-spawn-region", fallback_ok,
                 "max-edge/outside/NaN/blocked/none(valid DB pos (300,200)) ->" + fallback +
                     " (spawn region centre (100,90); the DB position is not a candidate)");
        struct MobStats {
            std::size_t mobs = 0;
            std::size_t outside = 0;
            std::size_t on_edge = 0;
            float max_z_err = 0.0f;
            float max_x = -1e9f;
            float min_x = 1e9f;
            float min_y = 1e9f;
        };
        const auto mobs = ReadWorld(sim, [&](const WorldSnapshot& snap) {
            MobStats m;
            for (std::size_t i = 0; i < snap.zones.ZoneCount(); ++i) {
                for (const auto& [net_id, entity] : snap.zones.GetZone(i).Entities()) {
                    (void)net_id;
                    if (!entity.has<gs::game::MobTag>() || entity.has<gs::game::GhostTag>()) {
                        continue;
                    }
                    const auto pos = entity.get<gs::game::Position>();
                    ++m.mobs;
                    m.outside += terrain.Contains(pos.x, pos.y) ? 0 : 1;
                    m.on_edge += pos.x == std::nextafter(410.0f, 0.0f) || pos.x == -150.0f || pos.y == -90.0f ? 1 : 0;
                    m.max_z_err = std::max(m.max_z_err, std::abs(pos.z - static_cast<float>(oracle.At(pos.x, pos.y))));
                    m.max_x = std::max(m.max_x, pos.x);
                    m.min_x = std::min(m.min_x, pos.x);
                    m.min_y = std::min(m.min_y, pos.y);
                }
            }
            return m;
        });
        c.Report("mobs-inside-on-terrain",
                 spawn_lines == 2 && mobs.mobs >= 70 && mobs.outside == 0 && mobs.on_edge == 0 && mobs.max_z_err < 1e-3f,
                 Fmt("mobs=%zu/80 outside=%zu piled_on_edge=%zu max_x=%.3f min_x=%.3f min_y=%.3f max_z_err=%.2e",
                     mobs.mobs, mobs.outside, mobs.on_edge, mobs.max_x, mobs.min_x, mobs.min_y, mobs.max_z_err));
        // R3: every world-level consumer uses the loaded bounds.
        WaitFor(5000ms, [&] { return sim.ActivitySnapshot() != nullptr && sim.LoadFieldSnapshot() != nullptr; });
        const auto activity = sim.ActivitySnapshot();
        const auto load = sim.LoadFieldSnapshot();
        const auto lf = sim.EffectiveLoadFieldConfig().bounds;
        const auto regions = ReadWorld(sim, [](const WorldSnapshot& snap) { return snap.zones.Regions(); });
        auto same = [](const gs::game::WorldBounds& a) {
            return a.min_x == -150.0f && a.min_y == -90.0f && a.max_x == 410.0f && a.max_y == 270.0f;
        };
        const bool region_ok = regions.size() == 1 && regions[0].bounds.min_x == -150.0f &&
                               regions[0].bounds.min_y == -90.0f && regions[0].bounds.max_x == 410.0f &&
                               regions[0].bounds.max_y == 270.0f;
        c.Report("world-consumers-use-loaded-bounds",
                 same(terrain.Bounds()) && same(lf) && activity && same(activity->bounds) && load &&
                     same(load->bounds) && region_ok,
                 Fmt("terrain/load-field-config/activity/load-grid/region = [(-150,-90),(410,270)): %d/%d/%d/%d/%d",
                     same(terrain.Bounds()) ? 1 : 0, same(lf) ? 1 : 0, activity && same(activity->bounds) ? 1 : 0,
                     load && same(load->bounds) ? 1 : 0, region_ok ? 1 : 0));
        const std::string audit = AuditNow(sim);
        c.Report("runtime-validator", audit == "OK", "WorldValidator: " + audit);
        sim.Stop();
    }

    // ---- D: no usable player spawn region -> the enter is refused ----
    // The spawn region centre (100, 90) lies in chunk (0,0); with that chunk
    // not resident the centre has no height. A valid stored DB position and
    // no debug override: the enter must be REFUSED (SERVER_ERROR), not placed
    // at the DB position or anywhere else. A valid debug override (dev
    // builds) still enters. Mob spawn circles in the missing chunk are
    // skipped, never placed on guessed ground.
    {
        std::string err;
        auto world = WriteAndLoad(FixtureDir("oracle_world_no_spawn_chunk"), SpecA(), err);
        if (!world) {
            c.Report("spawn-refused-without-usable-spawn-region", false, "REFUSED: " + err);
        } else {
            world->terrain.EvictChunkForTest(0, 0);
            IoRunner runner;
            gs::game::PartitionLayout layout;
            layout.regions_x = layout.regions_y = 1;
            gs::game::WorldRuntime sim(runner.io, {}, std::move(*world), layout);
            gs::game::PartitionConfig partition;
            partition.scoring.adaptive_enabled = false;
            sim.ConfigurePartition(partition);
            const std::size_t mobs = ReadWorld(sim, [](const WorldSnapshot& snap) {
                std::size_t n = 0;
                for (std::size_t i = 0; i < snap.zones.ZoneCount(); ++i) {
                    for (const auto& [net_id, entity] : snap.zones.GetZone(i).Entities()) {
                        (void)net_id;
                        n += entity.has<gs::game::MobTag>() && !entity.has<gs::game::GhostTag>() ? 1 : 0;
                    }
                }
                return n;
            });
            sim.Start();
            auto refused = MakeCharacter(20);
            refused.pos_x = static_cast<std::int32_t>(300.0f * gs::game::kDbUnitsPerMeter); // valid, resident
            refused.pos_y = static_cast<std::int32_t>(200.0f * gs::game::kDbUnitsPerMeter);
            sim.PostSpawn(DetachedSession(runner.io, 92101), std::move(refused), std::nullopt);
            const bool counted = WaitFor(5000ms, [&] { return sim.PlayerSpawnRefusals() == 1; });
            sim.PostSpawn(DetachedSession(runner.io, 92102), MakeCharacter(21),
                          gs::game::DebugSpawnOverride{300.0f, 200.0f});
            WaitFor(5000ms, [&] { return ReadPlayer(sim, 92102).found; });
            std::this_thread::sleep_for(200ms);
            const auto refused_pos = ReadPlayer(sim, 92101);
            const auto debug_pos = ReadPlayer(sim, 92102);
            const auto presence = sim.PresenceStats();
            const auto owners = ReadWorld(sim, [](const WorldSnapshot& snap) { return snap.owners.size(); });
            c.Report("spawn-refused-without-usable-spawn-region",
                     counted && !refused_pos.found && debug_pos.found && debug_pos.pos.x == 300.0f &&
                         debug_pos.pos.y == 200.0f && owners == 1 && sim.PlayerSpawnRefusals() == 1,
                     Fmt("spawn centre (100,90) height=%s: session 92101 (DB pos (300,200) valid) refused=%d "
                         "entered=%d | session 92102 debug (300,200) entered=%d at (%.1f,%.1f) | owners=%zu "
                         "refusals=%llu presence_claims=%llu",
                         mx::map::ToString(sim.Terrain().Height(100.0f, 90.0f).status), counted ? 1 : 0,
                         refused_pos.found ? 1 : 0, debug_pos.found ? 1 : 0, debug_pos.pos.x, debug_pos.pos.y,
                         owners, static_cast<unsigned long long>(sim.PlayerSpawnRefusals()),
                         static_cast<unsigned long long>(presence.claims)));
            c.Report("mob-spawn-skips-missing-chunk", mobs == 40,
                     Fmt("mobs=%zu (east circle 40 of 40; the south-west circle lies in the missing chunk "
                         "(0,0): 0 of 40, skipped)",
                         mobs));
            sim.Stop();
        }
    }
    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / "ixw_mapruntime", ec);
    std::printf("TERRAIN-DONE passes=%d failures=%d\n", c.passes, c.failures);
    return c.failures;
}

// ============================================================================
// MAP-2: split/merge from the real loader
// ============================================================================
namespace {

struct Ladder {
    Checks& c;
    gs::game::WorldRuntime& sim;
    const std::vector<QueryPoint>& points;
    std::vector<TerrainAnswer> terrain0;
    TopoProbe base;
    std::uint64_t loads0 = 0;
    std::size_t resident0 = 0;

    TopoProbe Probe()
    {
        const auto& terrain = sim.Terrain();
        return ReadWorld(sim, [&terrain](const WorldSnapshot& snap) { return ProbeTopology(snap, terrain); });
    }

    // Everything that must hold after each topology step.
    void Step(const std::string& name, std::size_t expect_leaves)
    {
        const auto& terrain = sim.Terrain();
        const TopoProbe p = Probe();
        const OwnerCheck owners =
            ReadWorld(sim, [&](const WorldSnapshot& snap) { return CheckOwners(snap, terrain, points); });
        const auto answers = AskTerrain(terrain, points);
        std::size_t changed = 0;
        for (std::size_t i = 0; i < answers.size(); ++i) {
            changed += answers[i] == terrain0[i] ? 0 : 1;
        }
        const bool population = p.net_ids == base.net_ids && p.owners == base.owners && p.mobs == base.mobs &&
                                p.players == base.players;
        const std::uint64_t loads = mx::map::PackageLoadCount();
        const std::size_t resident = terrain.ResidentBytes();
        const std::string audit = AuditNow(sim);
        c.Report(name,
                 p.violation.empty() && p.leaves.size() == expect_leaves && owners.violation.empty() && changed == 0 &&
                     population && p.mobs_without_height == 0 && p.max_mob_z_err < 1e-3f && loads == loads0 &&
                     resident == resident0 && audit == "OK",
                 Fmt("%s | entities=%zu (players=%zu mobs=%zu) same_set=%d | points inside=%zu outside=%zu owner=\"%s\" "
                     "terrain_changed=%zu | mob_z_err=%.1e no_height=%zu | package_loads=%llu (start %llu) "
                     "resident=%zu B | invariants=\"%s\" validator=%s",
                     LeafSummary(p).c_str(), p.net_ids.size(), p.players, p.mobs, population ? 1 : 0, owners.inside,
                     owners.outside, owners.violation.c_str(), changed, p.max_mob_z_err, p.mobs_without_height,
                     static_cast<unsigned long long>(loads), static_cast<unsigned long long>(loads0), resident,
                     p.violation.c_str(), audit.c_str()));
    }
};

// 1 region / 1 initial leaf over `world`, then ForceSplit to 4 and to 16,
// a refused split below the floor, and merges back. `splittable_levels` = how
// many levels the min-zone-size floor allows (2 for 1->4->16).
void RunLadder(Checks& c, gs::game::WorldRuntime& sim, const std::string& prefix, const std::vector<QueryPoint>& points,
               std::uint64_t loads0, int splittable_levels, gs::game::ZoneId& refused_zone,
               std::chrono::milliseconds dwell = 0ms)
{
    Ladder l{c, sim, points, {}, {}, loads0, sim.Terrain().ResidentBytes()};
    l.terrain0 = AskTerrain(sim.Terrain(), points);
    l.base = l.Probe();
    const std::size_t leaves0 = l.base.leaves.size();
    l.Step(prefix + "-initial", leaves0);

    // Level by level: split every current leaf.
    std::vector<std::vector<gs::game::ZoneId>> split_levels;
    std::size_t expect = leaves0;
    for (int level = 0; level < splittable_levels; ++level) {
        std::vector<gs::game::ZoneId> parents;
        for (const auto& leaf : l.Probe().leaves) {
            parents.push_back(leaf.id);
        }
        bool all = true;
        for (const auto id : parents) {
            all = ForceSplit(sim, id) && all;
        }
        split_levels.push_back(parents);
        expect *= 4;
        c.Report(Fmt("%s-split-to-%zu-committed", prefix.c_str(), expect), all,
                 Fmt("forced splits=%zu", parents.size()));
        std::this_thread::sleep_for(dwell); // let movers cross the new cuts
        l.Step(Fmt("%s-after-split-%zu", prefix.c_str(), expect), expect);
    }
    // The floor: one more split must be refused with a diagnostic.
    const auto leaves = l.Probe().leaves;
    refused_zone = leaves.empty() ? 0 : leaves.front().id;
    const auto refusal = ForceSplitExpectRefusal(sim, refused_zone);
    const auto after = l.Probe();
    c.Report(prefix + "-too-small-refused-with-diagnostic",
             refusal.refused && !refusal.committed && refusal.detail == "min-size" &&
                 refusal.reason == gs::game::PartitionNoopReason::SplitMinSize && after.leaves.size() == expect,
             Fmt("zone=%u %.0fx%.0f m: refused=%d detail=\"%s\" committed=%d leaves=%zu", refused_zone,
                 leaves.empty() ? 0.0f : leaves.front().bounds.max_x - leaves.front().bounds.min_x,
                 leaves.empty() ? 0.0f : leaves.front().bounds.max_y - leaves.front().bounds.min_y,
                 refusal.refused ? 1 : 0, refusal.detail.c_str(), refusal.committed ? 1 : 0, after.leaves.size()));
    // Merge back, deepest level first.
    for (auto it = split_levels.rbegin(); it != split_levels.rend(); ++it) {
        bool all = true;
        for (const auto parent : *it) {
            all = ForceMerge(sim, parent) && all;
        }
        expect /= 4;
        c.Report(Fmt("%s-merge-to-%zu-committed", prefix.c_str(), expect), all, Fmt("forced merges=%zu", it->size()));
        std::this_thread::sleep_for(dwell);
        l.Step(Fmt("%s-after-merge-%zu", prefix.c_str(), expect), expect);
    }
}

} // namespace

int RunMapSplitScenario()
{
    Checks c{"MAPSPLIT"};
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    // ---- S: non-flat, negative origin, non-square, partial chunks ----
    {
        std::string error;
        auto loaded = WriteAndLoad(FixtureDir("split_world"), SpecS(), error);
        const std::uint64_t loads0 = mx::map::PackageLoadCount();
        if (!loaded) {
            c.Report("fixture-load", false, "REFUSED: " + error);
        } else {
            const auto& g = loaded->terrain.Geometry();
            const auto* last = loaded->terrain.ChunkAt(3, 2);
            // Non-flat and oracle-true: the fixture really is the terrain the
            // runtime will query.
            const OracleGrid oracle = OracleS();
            double max_err = 0.0;
            float lo = 1e9f;
            float hi = -1e9f;
            std::mt19937 rng(5);
            std::uniform_real_distribution<double> ux(-1280.0, 1280.0);
            std::uniform_real_distribution<double> uy(-1024.0, 1024.0);
            for (int i = 0; i < 4000; ++i) {
                const double x = ux(rng);
                const double y = uy(rng);
                const auto h = loaded->terrain.Height(x, y);
                max_err = std::max(max_err, h.Ok() ? std::abs(h.meters - oracle.At(x, y)) : 1e9);
                lo = std::min(lo, h.meters);
                hi = std::max(hi, h.meters);
            }
            c.Report("fixture-load",
                     g.chunks_x == 4 && g.chunks_y == 3 && last != nullptr && last->cells_x == 32 &&
                         last->cells_y == 64 && max_err < 1e-4 && hi - lo > 20.0f,
                     Fmt("world [(%.0f,%.0f),(%.0f,%.0f)) cells=%ux%u chunks=%ux%u last=%ux%u heights %.1f..%.1f m "
                         "oracle_err=%.1e mob_lines=%zu",
                         g.MinX(), g.MinY(), g.MaxX(), g.MaxY(), g.cells_x, g.cells_y, g.chunks_x, g.chunks_y,
                         last != nullptr ? last->cells_x : 0, last != nullptr ? last->cells_y : 0, lo, hi, max_err,
                         loaded->spawn_points.size()));

            IoRunner runner;
            gs::game::PartitionLayout layout;
            layout.regions_x = layout.regions_y = 1; // one region, one initial leaf
            gs::game::WorldRuntime sim(runner.io, {}, std::move(*loaded), layout);
            gs::game::PartitionConfig partition;
            partition.scoring.adaptive_enabled = false; // forced topology only (min size 500 m, depth 4 kept)
            sim.ConfigurePartition(partition);
            const auto initial = ReadWorld(sim, [](const WorldSnapshot& snap) {
                std::vector<gs::game::ZoneId> ids;
                for (const auto* leaf : snap.zones.GetActiveLeaves()) {
                    ids.push_back(leaf->zone_id);
                }
                return std::make_pair(ids, snap.zones.Regions().empty() ? std::string() : snap.zones.Regions()[0].name);
            });
            c.Report("ids-areas-vs-zones", initial.first.size() == 1 && initial.second == "World",
                     Fmt("areas {7, 9000} (package metadata) | server zones {%u} in region \"%s\" (server config 1x1)",
                         initial.first.empty() ? 0u : initial.first.front(), initial.second.c_str()));

            // Players cross the first cut (x = 0), a second-level cut
            // (x = 640) and (y = 512) back and forth during the whole ladder.
            const struct {
                gs::common::SessionId session;
                std::optional<gs::game::DebugSpawnOverride> debug;
                float heading;
            } movers[] = {{93001, gs::game::DebugSpawnOverride{-6.0f, 3.0f}, 1.5707963f},
                          {93002, gs::game::DebugSpawnOverride{634.0f, -300.0f}, 1.5707963f},
                          {93003, gs::game::DebugSpawnOverride{-100.0f, 506.0f}, 0.0f},
                          {93004, std::nullopt, 1.5707963f}};
            std::uint64_t index = 0;
            for (const auto& m : movers) {
                sim.PostSpawn(DetachedSession(runner.io, m.session), MakeCharacter(100 + index++), m.debug);
            }
            sim.Start();
            const std::size_t workers = [&] {
                std::size_t w = 0;
                WaitFor(5000ms, [&] { return (w = sim.SchedulerStats().workers) > 0; });
                return w;
            }();
            const std::size_t hw_workers = hw > 1 ? hw - 1 : 1;
            c.Report("workers-not-capped-by-initial-zones", workers == hw_workers,
                     Fmt("initial zones=1 workers=%zu expected=%zu (hardware_concurrency=%u)", workers, hw_workers, hw));
            WaitFor(10000ms, [&] {
                return ReadWorld(sim, [](const WorldSnapshot& snap) { return snap.owners.size(); }) == std::size(movers);
            });
            std::atomic<bool> moving{true};
            std::thread mover([&] {
                std::uint32_t seq = 0;
                int step = 0;
                while (moving.load(std::memory_order_acquire)) {
                    const float sign = (step / 40) % 2 == 0 ? 1.0f : -1.0f;
                    for (const auto& m : movers) {
                        // 40 ticks x 0.3 m = 12 m each way from 6 m before a cut:
                        // 6 m past it, beyond the 5 m migration hysteresis.
                        sim.PostMoveInput(m.session, ++seq, sign > 0 ? m.heading : m.heading + 3.14159265f,
                                          gs::game::MoveState::Running);
                    }
                    ++step;
                    std::this_thread::sleep_for(50ms);
                }
            });
            std::this_thread::sleep_for(500ms);
            // Movement goes on through every step: the population is checked
            // by identity (net ids), not by position.
            const auto b = sim.Terrain().Bounds();
            const auto points =
                QueryPoints(b, {-640.0f, 0.0f, 640.0f, -960.0f, -320.0f, 320.0f, 960.0f, -1280.0f + 768.0f},
                            {-512.0f, 0.0f, 512.0f, -768.0f, -256.0f, 256.0f, 768.0f, -1024.0f + 768.0f}, 20260925, 3000);
            const auto mig0 = sim.MigrationMetrics();
            gs::game::ZoneId refused = 0;
            RunLadder(c, sim, "world", points, loads0, 2, refused, 2500ms);
            const auto mig1 = sim.MigrationMetrics();
            moving.store(false, std::memory_order_release);
            mover.join();
            // Coverage: the population checks above ran while players really
            // migrated across the new cuts (not a static world).
            c.Report("world-players-crossed-cuts", mig1.committed - mig0.committed >= 3,
                     Fmt("player migrations during the ladder=%llu (stale=%llu retries=%llu)",
                         static_cast<unsigned long long>(mig1.committed - mig0.committed),
                         static_cast<unsigned long long>(mig1.dropped_stale - mig0.dropped_stale),
                         static_cast<unsigned long long>(mig1.retries - mig0.retries)));
            sim.Stop();
        }
    }
    // ---- T: the checked-in test map (1 km) under the approved R2 rule ----
    // Default production layout (2x2 regions, one initial leaf each = 500 m).
    {
        const std::uint64_t loads0 = mx::map::PackageLoadCount() + 1;
        IoRunner runner;
        gs::game::WorldRuntime sim(runner.io, {}, LoadBenchTestWorld(), gs::game::PartitionLayout{});
        gs::game::PartitionConfig partition;
        partition.scoring.adaptive_enabled = false;
        partition.min_zone_size_m = 240.0f; // = 2 x AOI radius, the safe floor
        sim.ConfigurePartition(partition);
        sim.Start();
        std::this_thread::sleep_for(300ms);
        const auto b = sim.Terrain().Bounds();
        const auto points = QueryPoints(b, {250.0f, 500.0f, 750.0f}, {250.0f, 500.0f, 750.0f}, 99, 2000);
        gs::game::ZoneId refused = 0;
        // 4 region leaves of 500 m -> 16 of 250 m; 125 m would break the floor.
        RunLadder(c, sim, "testmap-min240", points, loads0, 1, refused);
        sim.Stop();
    }
    {
        // Same map, default min zone size 500 m: a 500 m region leaf is
        // legitimately too small to split (children would be 250 m).
        IoRunner runner;
        gs::game::WorldRuntime sim(runner.io, {}, LoadBenchTestWorld(), gs::game::PartitionLayout{});
        gs::game::PartitionConfig partition;
        partition.scoring.adaptive_enabled = false;
        sim.ConfigurePartition(partition);
        sim.Start();
        std::this_thread::sleep_for(300ms);
        const auto leaf = LeafAt(sim, 100.0f, 100.0f);
        const auto refusal = ForceSplitExpectRefusal(sim, leaf);
        const auto leaves =
            ReadWorld(sim, [](const WorldSnapshot& snap) { return snap.zones.GetActiveLeaves().size(); });
        c.Report("testmap-min500-refused-with-diagnostic",
                 refusal.refused && !refusal.committed && refusal.detail == "min-size" && leaves == 4,
                 Fmt("zone=%u (500x500 m): refused=%d detail=\"%s\" committed=%d leaves=%zu", leaf,
                     refusal.refused ? 1 : 0, refusal.detail.c_str(), refusal.committed ? 1 : 0, leaves));
        sim.Stop();
    }
    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / "ixw_mapruntime", ec);
    std::printf("MAPSPLIT-DONE passes=%d failures=%d\n", c.passes, c.failures);
    return c.failures;
}

// ============================================================================
// MAP-2 review: aggregate bootstrap resources
// ============================================================================
int RunBootstrapScenario(int max_grid)
{
    Checks c{"BOOTSTRAP"};
    // (1) Cost of the initial partition itself: an empty 100 km world with an
    // N = g x g initial leaf grid (every leaf a live zone: flecs world,
    // spatial grid, load bins, queues). Working set is the OS view (process
    // RSS); the delta includes allocator retention, so it is an upper bound.
    struct Row {
        std::uint32_t zones = 0;
        double construct_ms = 0.0;
        double start_ms = 0.0;
        std::int64_t rss_construct = 0;
        std::int64_t rss_running = 0;
        double supervisor_avg_ms = 0.0;
        std::size_t workers = 0;
    };
    std::vector<Row> rows;
    for (std::uint32_t g = 1; g <= static_cast<std::uint32_t>(max_grid); g *= 2) {
        Row row;
        row.zones = g * g;
        const auto rss0 = static_cast<std::int64_t>(ProcessWorkingSetBytes());
        {
            IoRunner runner;
            const auto t0 = Clock::now();
            gs::game::WorldRuntime sim(runner.io, {}, gs::game::WorldRuntime::SyntheticWorldConfig{100000.0f, g, g, {}});
            const auto t1 = Clock::now();
            row.rss_construct = static_cast<std::int64_t>(ProcessWorkingSetBytes()) - rss0;
            gs::game::PartitionConfig partition;
            partition.scoring.adaptive_enabled = false;
            sim.ConfigurePartition(partition);
            sim.Start();
            WaitFor(10000ms, [&] { return (row.workers = sim.SchedulerStats().workers) > 0; });
            row.start_ms = std::chrono::duration<double, std::milli>(Clock::now() - t1).count();
            std::this_thread::sleep_for(3s);
            row.rss_running = static_cast<std::int64_t>(ProcessWorkingSetBytes()) - rss0;
            row.supervisor_avg_ms = sim.SupervisorAvgMs();
            row.construct_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            sim.Stop();
        }
        std::printf("BOOTSTRAP zones=%u construct_ms=%.1f start_ms=%.1f rss_after_construct=%.1fMB "
                    "rss_running=%.1fMB per_zone=%.1fKB supervisor_avg_ms=%.3f workers=%zu\n",
                    row.zones, row.construct_ms, row.start_ms, row.rss_construct / 1048576.0,
                    row.rss_running / 1048576.0, row.rss_running / 1024.0 / row.zones, row.supervisor_avg_ms,
                    row.workers);
        std::fflush(stdout);
        rows.push_back(row);
    }
    // At the cap: the documented bootstrap budget (<= 2 GB working set for the
    // empty initial zones, <= 5 s to construct, idle supervisor pass <= 10% of
    // the 50 ms tick).
    const Row* at_cap = nullptr;
    for (const auto& row : rows) {
        if (row.zones == gs::game::kMaxInitialLeafZones) {
            at_cap = &row;
        }
    }
    if (at_cap == nullptr) {
        std::printf("BOOTSTRAP at-cap: not measured (--grid %d < %u): SKIPPED\n", max_grid,
                    static_cast<unsigned>(std::sqrt(static_cast<double>(gs::game::kMaxInitialLeafZones))));
    } else {
        c.Report("bootstrap-at-cap-within-budget",
                 at_cap->rss_running < (2ll << 30) && at_cap->construct_ms < 5000.0 && at_cap->supervisor_avg_ms < 5.0,
                 Fmt("zones=%u rss=%.0fMB (<= 2048) construct=%.0fms (<= 5000) supervisor_idle=%.2fms (<= 5.0)",
                     at_cap->zones, at_cap->rss_running / 1048576.0, at_cap->construct_ms, at_cap->supervisor_avg_ms));
    }
    // (2) The aggregate cap is enforced wherever an initial partition is built.
    {
        const auto world = gs::game::WorldBounds::FromExtent(100000.0f);
        auto build = [&](std::uint32_t rx, std::uint32_t ry, std::uint32_t lx, std::uint32_t ly, std::string& error) {
            gs::game::PartitionLayout layout;
            layout.regions_x = rx;
            layout.regions_y = ry;
            layout.leaves_x = lx;
            layout.leaves_y = ly;
            gs::game::InitialPartition out;
            return gs::game::BuildInitialPartition(world, layout, 240.0f, out, error) ? out.leaves.size() : 0u;
        };
        std::string e_ok, e_over, e_max;
        const std::size_t ok = build(2, 2, 16, 16, e_ok);        // 1024
        const std::size_t over = build(1, 1, 33, 32, e_over);    // 1056
        const std::size_t former = build(16, 16, 64, 64, e_max); // 1048576: the former per-axis maximum
        c.Report("initial-zone-cap-enforced",
                 ok == 1024 && over == 0 && former == 0 && e_over.find("bootstrap cap") != std::string::npos &&
                     e_max.find("bootstrap cap") != std::string::npos,
                 Fmt("2x2 regions x 16x16 leaves -> %zu zones; 33x32 -> \"%s\"; 16x16 x 64x64 -> \"%s\"", ok,
                     e_over.c_str(), e_max.c_str()));
        gs::game::PartitionLayout explicit_layout;
        for (std::uint32_t i = 0; i <= gs::game::kMaxInitialLeafZones; ++i) {
            const float x = static_cast<float>(i) * 97.0f;
            explicit_layout.explicit_leaves.push_back(mx::map::Rect{x, 0.0f, x + 97.0f, 100000.0f});
        }
        gs::game::InitialPartition out;
        std::string e_explicit;
        const bool explicit_ok = gs::game::BuildInitialPartition(world, explicit_layout, 0.0f, out, e_explicit);
        c.Report("explicit-leaves-cap-enforced", !explicit_ok && e_explicit.find("bootstrap cap") != std::string::npos,
                 Fmt("%zu explicit leaves -> \"%s\"", explicit_layout.explicit_leaves.size(), e_explicit.c_str()));
        auto synthetic_throws = [](gs::game::WorldRuntime::SyntheticWorldConfig config) {
            IoRunner runner;
            try {
                gs::game::WorldRuntime sim(runner.io, {}, config);
                return std::string("constructed");
            } catch (const std::invalid_argument& error) {
                return std::string(error.what());
            }
        };
        const std::string over_zones = synthetic_throws({100000.0f, 33, 32, {}});
        const std::string over_extent = synthetic_throws({200000.0f, 2, 2, {}});
        c.Report("synthetic-world-caps-enforced",
                 over_zones.find("bootstrap cap") != std::string::npos &&
                     over_extent.find("exceeds") != std::string::npos,
                 Fmt("33x32 zones -> \"%s\"; 200 km extent -> \"%s\"", over_zones.c_str(), over_extent.c_str()));
    }
    std::printf("BOOTSTRAP-DONE passes=%d failures=%d\n", c.passes, c.failures);
    return c.failures;
}

int RunMap4Scenario()
{
    using namespace gs::game;
    Checks c{"MAP4"};
    auto spec = SpecS();
    spec.mob_spawns_version = 2;
    spec.mob_spawns_required = true;
    spec.mob_spawns = "spawn_id=51 area_id=7 mob_type_id=1 x=-700 y=-500 count=12 radius=5\n";
    // Non-adjacent source and target with a chained target trigger.
    spec.logic.warps = {{17, {-910,-510,-890,-490}, 900,500},
                        {29, {890,490,910,510}, 700,500}};
    std::string error;
    auto world = WriteAndLoad(FixtureDir("map4-main"), spec, error, mx::map::WarpPolicy::Legacy);
    c.Report("v2-spawn-identity-and-valid-chain", world && world->spawn_points[0].spawn_id == 51 &&
             world->spawn_points[0].area_id == 7, error);
    if (!world) return c.failures;

    auto refused = [&](const char* name, const std::string& text, unsigned version = 2) {
        auto bad = spec;
        bad.mob_spawns_version = static_cast<std::uint16_t>(version);
        bad.mob_spawns = text;
        std::string why;
        auto loaded = WriteAndLoad(FixtureDir(std::string("map4-") + name), bad, why, mx::map::WarpPolicy::Legacy);
        c.Report(name, !loaded, why);
    };
    refused("spawn-missing-id", "area_id=0 mob_type_id=1 x=0 y=0 count=1 radius=0\n");
    refused("spawn-missing-area", "spawn_id=1 mob_type_id=1 x=0 y=0 count=1 radius=0\n");
    refused("spawn-id-overflow", "spawn_id=4294967296 area_id=0 mob_type_id=1 x=0 y=0 count=1 radius=0\n");
    refused("spawn-zero-id", "spawn_id=0 area_id=0 mob_type_id=1 x=0 y=0 count=1 radius=0\n");
    refused("spawn-duplicate-id", *spec.mob_spawns + *spec.mob_spawns);
    refused("spawn-unknown-area", "spawn_id=1 area_id=999999 mob_type_id=1 x=0 y=0 count=1 radius=0\n");
    refused("spawn-outside-referenced-area", "spawn_id=1 area_id=7 mob_type_id=1 x=500 y=0 count=1 radius=0\n");
    refused("spawn-unknown-type", "spawn_id=1 area_id=0 mob_type_id=999999 x=0 y=0 count=1 radius=0\n");
    refused("spawn-outside", "spawn_id=1 area_id=0 mob_type_id=1 x=999999 y=0 count=1 radius=0\n");
    refused("v1-rejects-v2-fields", *spec.mob_spawns, 1);
    {
        auto legacy = spec;
        legacy.mob_spawns_version = 1;
        legacy.mob_spawns = "# comment\nmob_type_id=1 x=0 y=0 count=1 radius=0\n";
        auto loaded = WriteAndLoad(FixtureDir("map4-legacy"), legacy, error, mx::map::WarpPolicy::Legacy);
        c.Report("legacy-ordinal-identity", loaded && loaded->spawn_points[0].spawn_id == 1, error);
        auto cycle = spec;
        cycle.logic.warps[1].target_x = -900;
        cycle.logic.warps[1].target_y = -500;
        auto rejected = WriteAndLoad(FixtureDir("map4-cycle"), cycle, error, mx::map::WarpPolicy::Legacy);
        c.Report("package-rejects-A-B-cycle", !rejected, error);
    }
    // R6 strict-valid package through the same authoritative movement system.
    {
        auto strict_spec=spec;
        strict_spec.logic.warps.resize(1); // target is outside every trigger
        auto strict_world=WriteAndLoad(FixtureDir("map4-strict-runtime"),strict_spec,error);
        bool once=false;
        if(strict_world) {
            IoRunner runner;
            TerrainService terrain(std::move(strict_world->terrain));
            ZoneManager zones; InitialPartition partition; PartitionLayout layout;
            layout.regions_x=layout.regions_y=1;
            if(BuildInitialPartition(terrain.Bounds(),layout,240,partition,error)) {
                zones.BuildInitialPartition(partition);
                auto& zone=zones.GetZone(0); MobPrototypeRegistry types;
                ZoneTickContext ctx{terrain,strict_world->logic,types,zones};
                ZoneWriteGuard guard(zone,"R6 strict authoritative warp");
                SpawnSystem::SpawnPlayer(zone,DetachedSession(runner.io,1),MakeCharacter(1),{-900,-500,0},1);
                for(int tick=0;tick<60;++tick) MovementSystem::Step(zone,0.05f,ctx);
                const auto entity=zone.FindEntity(1);
                once=entity.get<WarpState>().completed==1 && entity.get<Position>().x==900;
            }
        }
        c.Report("strict-valid-single-warp",once,"policy=strict; 60 authoritative ticks, exactly one warp");
    }
    // Direct authoritative ticks: deterministic timeout/cancel/failure cases,
    // with actual file-loaded terrain. Mutations below are test faults only.
    {
        IoRunner runner;
        TerrainService terrain(std::move(world->terrain));
        ZoneManager zones;
        InitialPartition partition;
        PartitionLayout layout;
        layout.regions_x = layout.regions_y = 1;
        BuildInitialPartition(terrain.Bounds(), layout, 240, partition, error);
        zones.BuildInitialPartition(partition);
        auto& zone = zones.GetZone(0);
        MobPrototypeRegistry types;
        ZoneTickContext ctx{terrain, world->logic, types, zones};
        ZoneWriteGuard guard(zone, "MAP4 direct tick test");
        SpawnSystem::SpawnPlayer(zone, DetachedSession(runner.io, 1), MakeCharacter(1), {-900,-500,0}, 1);
        auto entity = zone.FindEntity(1);
        auto step = [&](int ticks) { for (int i=0;i<ticks;++i) MovementSystem::Step(zone, 0.05f, ctx); };
        auto reset = [&](float x, float y) {
            auto old = entity.get<Position>();
            entity.set<Position>({x,y,terrain.Height(x,y).meters});
            entity.set<WarpState>({});
            zone.Grid().Move(entity, 1, SpatialCellKey(SpatialCellCoord(old.x),SpatialCellCoord(old.y)), entity.get<Position>());
        };
        step(60);
        c.Report("chain-no-retrigger", entity.get<WarpState>().completed == 1 && entity.get<Position>().x == 900,
                 "60 ticks inside destination source: exactly one teleport");
        auto transfer = BuildTransfer(entity, true);
        flecs::world destination;
        auto copy = ApplyTransfer(destination, transfer);
        c.Report("handoff-preserves-trigger", !copy.get<WarpState>().armed && copy.get<WarpState>().completed == 1,
                 "same transfer payload used by migration and partition transactions");
        entity.set<Position>({920,500,0}); step(1);
        entity.set<Position>({900,500,0}); step(1);
        c.Report("leave-reenter-rearms", entity.get<WarpState>().completed == 2 && entity.get<Position>().x == 700, "explicit rearm");

        reset(-900,-500);
        const auto chunk = terrain.ChunkIndexOf(900,500);
        auto retained = terrain.MutableTerrain()->Unpublish(chunk);
        step(2);
        c.Report("pending-no-partial-teleport", entity.get<WarpState>().pending_id == 17 && entity.get<Position>().x == -900,
                 "target chunk absent, source position retained");
        entity.set<Position>({-880,-500,0}); step(1);
        c.Report("leave-cancels-pending", entity.get<WarpState>().pending_id == 0 && entity.get<WarpState>().cancelled == 1, "no callback");
        reset(-900,-500); step(102);
        c.Report("pending-bounded-timeout", entity.get<WarpState>().timed_out == 1 && entity.get<WarpState>().pending_id == 0,
                 "five simulation seconds");
        step(120);
        c.Report("timeout-no-retry-storm", entity.get<WarpState>().timed_out == 1, "must leave to rearm");
        reset(-900,-500); step(1);
        (void)terrain.MutableTerrain()->Publish(chunk, retained);
        step(2);
        c.Report("pending-completes-on-residency", entity.get<WarpState>().completed == 1 && entity.get<Position>().x == 900, "height available before teleport");
        world->logic.warps[0].target_x = 999999;
        reset(-900,-500); step(100);
        c.Report("invalid-target-one-refusal", entity.get<WarpState>().refused == 1 && entity.get<Position>().x == -900, "no partial teleport, no per-tick warning");
        world->logic.warps[0].target_x = -470;
        world->logic.warps[0].target_y = -214; // blocked cell (101,101) from SpecS
        reset(-900,-500); step(100);
        c.Report("blocked-target-one-refusal", entity.get<WarpState>().refused == 1 && entity.get<Position>().x == -900,
                 "blocked is not a pending load");
        world->logic.warps[0].target_x = 900;
        world->logic.warps[0].target_y = 500;
        reset(-890,-500); step(1);
        c.Report("half-open-source-max", entity.get<WarpState>().completed == 0, "source max belongs outside");

        reset(-900,-500);
        retained = terrain.MutableTerrain()->Unpublish(chunk);
        step(1);
        const bool was_pending = entity.get<WarpState>().pending_id == 17;
        zone.Grid().Remove(1, entity.get<Position>());
        entity.destruct();
        zone.UnindexEntity(1);
        zone.ErasePlayerBinding(1);
        (void)terrain.MutableTerrain()->Publish(chunk, retained);
        // Reuse the network id after a late terrain completion. A new player
        // outside all sources must not inherit the old entity's request.
        SpawnSystem::SpawnPlayer(zone, DetachedSession(runner.io, 2), MakeCharacter(2), {-880,-500,0}, 1);
        entity = zone.FindEntity(1);
        step(2);
        c.Report("despawn-pending-late-residency", was_pending && entity.get<WarpState>().pending_id == 0 &&
                 entity.get<WarpState>().completed == 0 && entity.get<Position>().x == -880,
                 "despawn destroys the request; late publication cannot teleport a replacement entity");
    }
    // Real runtime, ownership map, non-neighbour migration, ASF and spawn
    // identity. No direct entity mutation while the scheduler is running.
    for (const int workers : {1,4}) {
        auto loaded = WriteAndLoad(FixtureDir("map4-runtime-" + std::to_string(workers)), spec, error, mx::map::WarpPolicy::Legacy);
        if (!loaded) { c.Report("runtime-package", false, error); continue; }
        IoRunner runner;
        PartitionLayout layout;
        layout.regions_x = 4; layout.regions_y = 2;
        WorldRuntime sim(runner.io, {}, std::move(*loaded), layout);
        sim.ConfigureWorkers(workers);
        PartitionConfig cfg; cfg.scoring.adaptive_enabled = false; cfg.min_zone_size_m = 240; sim.ConfigurePartition(cfg);
        c.Report("initial-spawn-idempotent-" + std::to_string(workers), sim.SpawnConfiguredMobsNow() == 0,
                 "already activated by constructor");
        sim.PostSpawn(DetachedSession(runner.io, 99001), MakeCharacter(99001), DebugSpawnOverride{-900,-500});
        sim.Start();
        const bool landed = WaitFor(5000ms, [&] { auto p=ReadPlayer(sim,99001); return p.found && p.pos.x == 900; });
        auto state = [&] { return ReadWorld(sim, [&](const WorldSnapshot& s) {
            const auto it=s.owners.find(99001);
            if(it==s.owners.end()) return WarpState{};
            const auto& z=s.zones.GetZone(it->second.zone_index);
            const auto e=z.FindEntity(it->second.net_id);
            return e.is_valid() && e.has<WarpState>() ? e.get<WarpState>() : WarpState{};
        }); };
        const bool migrated = WaitFor(5000ms,[&] { return ReadWorld(sim,[&](const WorldSnapshot& s) {
            auto it=s.owners.find(99001); return it!=s.owners.end() &&
                s.zones.GetZone(it->second.zone_index).Bounds().ContainsHalfOpen(900,500);
        }); });
        c.Report("non-neighbour-warp-" + std::to_string(workers), landed && migrated && state().completed == 1,
                 AuditNow(sim));
        const auto leaf=LeafAt(sim,900,500);
        const bool split=ForceSplit(sim,leaf);
        const bool merge=split && ForceMerge(sim,leaf);
        c.Report("trigger-through-split-merge-" + std::to_string(workers), merge && state().completed == 1 && !state().armed,
                 AuditNow(sim));
        const auto mob_leaf = LeafAt(sim, -700, -500);
        const bool mobs_transferred = ForceSplit(sim, mob_leaf) && ForceMerge(sim, mob_leaf);
        c.Report("spawn-source-through-split-merge-" + std::to_string(workers), mobs_transferred,
                 "the populated mob leaf also crosses both ownership transactions");
        const auto mobs=ReadWorld(sim,[](const WorldSnapshot& s) {
            std::size_t n=0; bool ids=true;
            for(std::size_t i=0;i<s.zones.ZoneCount();++i) s.zones.GetZone(i).World().query<const MobTag,const MobSpawnRef>()
                .each([&](flecs::entity e,const MobTag&,const MobSpawnRef& ref) { if(!e.has<GhostTag>()) {++n; ids &= ref.spawn_id==51;} });
            return std::pair{n,ids};
        });
        c.Report("spawn-identity-no-duplicates-" + std::to_string(workers), mobs.first==12 && mobs.second, Fmt("mobs=%zu",mobs.first));
        sim.Stop();
    }
    {
        gs::game::WorldLoadRequest request;
        request.package_root = FixtureDir("map4-demand");
        request.mob_types_config = IXTREEME_DEFAULT_MOB_TYPES_CONFIG;
        request.residency = mx::map::ResidencyMode::Streaming;
        auto demand_spec=spec; demand_spec.logic.warps.clear(); demand_spec.mob_spawns.reset();
        (void)mx::map::WritePackage(request.package_root,demand_spec);
        mx::map::PackageReport report;
        auto loaded=LoadWorldPackage(request,report);
        if (loaded) {
            IoRunner runner;
            WorldRuntime sim(runner.io,{},std::move(*loaded));
            sim.Start();
            const float x=-511.5f,y=-255.5f; // 0.5m above both chunk seams
            const bool ready=WaitFor(3000ms,[&] {
                sim.PostTerrainDemand(x,y,1);
                return ReadWorld(sim,[&](const WorldSnapshot&) {
                    return sim.Terrain().Height(x,y).Ok() && sim.Terrain().Height(x-1,y-1).Ok();
                });
            });
            c.Report("demand-includes-centre-across-seams",ready,"radius 1m intersects four chunks");
            sim.Stop();
        } else c.Report("demand-fixture",false,"loader rejected");
    }
    std::printf("MAP4-DONE passes=%d failures=%d\n", c.passes,c.failures);
    return c.failures;
}

} // namespace gs::bench
