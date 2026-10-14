#include "LayerClearanceBench.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/asio/io_context.hpp>

#include "map/LayerClearance.h"
#include "map/LayeredWorldGeometry.h"
#include "map/WorldPackageWriter.h"

#include "../world/WorldRuntime.h"
#include "../world/package/WorldPackageLoader.h"

namespace gs::bench {
namespace {

namespace fs = std::filesystem;
namespace map = mx::map;

struct Checks {
    int passes = 0, failures = 0;

    void Report(const char* name, bool pass)
    {
        std::printf("LAYERCLEARANCE %s: %s\n", name, pass ? "PASS" : "FAIL");
        (pass ? passes : failures) += 1;
    }
};

struct ScratchDirectory {
    fs::path parent = fs::weakly_canonical(fs::temp_directory_path());
    fs::path root;

    ScratchDirectory()
    {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 10; ++attempt) {
            const auto candidate = parent / ("ixw_layerclearance_" + std::to_string(suffix) + "_" +
                                             std::to_string(attempt));
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) {
                root = candidate;
                return;
            }
            if (ec) throw std::runtime_error("cannot create layer clearance scratch directory: " + ec.message());
        }
        throw std::runtime_error("cannot allocate a unique layer clearance scratch directory");
    }

    ~ScratchDirectory()
    {
        if (root.is_absolute() && root.parent_path() == parent &&
            root.filename().string().starts_with("ixw_layerclearance_")) {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    }
};

bool SameState(const map::LayerGroundState& lhs, const map::LayerGroundState& rhs)
{
    return lhs.volume_id == rhs.volume_id && lhs.layer_id == rhs.layer_id &&
        lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
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

// Floor (top 1 m) with a non-walkable wall, four 0.75 m treads rising
// 0.25 m each onto a 2 m landing. Every edge of the stair touches exactly.
bool CookFixture(map::LayeredWorld& world, map::LayerClearanceReport& report, std::string& error)
{
    std::vector<map::LayerCollisionMesh> walkable{Box(1, 4, 4, 0, 28, 28, 1)};
    for (std::uint32_t k = 0; k < 4; ++k) {
        const float x0 = 28 + 0.75f * static_cast<float>(k);
        walkable.push_back(Box(2 + k, x0, 14, 0, x0 + 0.75f, 18, 1.25f + 0.25f * static_cast<float>(k)));
    }
    walkable.push_back(Box(6, 31, 13, 0, 36, 19, 2));
    const auto wall = Box(9, 14, 8, 1, 14.5f, 20, 4);

    std::vector<map::LayerSourceSurface> surfaces;
    map::LayerGeometryReport geometry;
    if (!map::ExtractLayerSourceSurfaces(walkable, {}, surfaces, geometry)) {
        error = geometry.errors.empty() ? "extract" : geometry.errors.front();
        return false;
    }
    map::LayerGenerationOptions options;
    options.world_bounds = map::Rect{0, 0, 256, 256};
    options.require_exact_footprints = true;
    map::LayerGenerationReport generation;
    if (!map::GenerateLayeredWorld(surfaces, options, world, generation)) {
        error = generation.errors.empty() ? "generate" : generation.errors.front();
        return false;
    }
    std::vector<map::LayerObstructionMesh> obstructions;
    for (const auto& mesh : walkable) obstructions.push_back({mesh.source_id, mesh.vertices, mesh.indices});
    obstructions.push_back({wall.source_id, wall.vertices, wall.indices});
    if (!map::CookLayerClearance(world, options.world_bounds, obstructions, nullptr, map::LayerClearanceProfile{},
                                 report)) {
        error = report.errors.empty() ? "cook" : report.errors.front();
        return false;
    }
    return true;
}

map::PackageWriteSpec PackageSpec()
{
    map::PackageWriteSpec spec;
    spec.world_id = "layerclearance_fixture";
    spec.world_name = "Offline clearance/portal fixture";
    spec.size_cells_x = 256;
    spec.size_cells_y = 256;
    spec.origin_x = 0;
    spec.origin_y = 0;
    spec.cell_size_m = 1;
    spec.chunk_size_cells = 128;
    spec.height_raw = [](std::uint32_t, std::uint32_t) { return 0; };
    spec.attributes = [](std::uint32_t, std::uint32_t) { return std::uint16_t{0}; };
    spec.logic.spawns.push_back(map::SpawnRegion{1, 0, map::Rect{8, 8, 10, 10}});
    return spec;
}

gs::game::WorldLoadRequest LoadRequest(const fs::path& root)
{
    gs::game::WorldLoadRequest request;
    request.package_root = root;
    request.mob_types_config = IXTREEME_DEFAULT_MOB_TYPES_CONFIG;
    request.depth = map::ValidationDepth::Full;
    request.warp_policy = map::WarpPolicy::Strict;
    return request;
}

gs::game::PartitionLayout SingleLeafLayout()
{
    gs::game::PartitionLayout layout;
    layout.regions_x = layout.regions_y = layout.leaves_x = layout.leaves_y = 1;
    return layout;
}

const map::LayerVolume* FootprintAt(const map::LayeredWorld& world, double x, double y)
{
    for (const auto& volume : world.volumes) {
        if (volume.ground_support && x >= volume.bounds.min_x && x < volume.bounds.max_x &&
            y >= volume.bounds.min_y && y < volume.bounds.max_y) {
            return &volume;
        }
    }
    return nullptr;
}

std::vector<std::uint8_t> ReadBytes(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

bool WriteBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

// Writes a copy of the valid package whose sidecar f32 at `offset` is
// replaced, and returns whether the strict loader refuses it as invalid.
bool TamperedProfileRefused(const fs::path& source, const fs::path& copy, std::size_t offset, float value)
{
    std::error_code ec;
    fs::copy(source, copy, fs::copy_options::recursive, ec);
    if (ec) return false;
    auto bytes = ReadBytes(copy / map::kLayeredWorldFile);
    if (bytes.size() < offset + 4) return false;
    std::memcpy(bytes.data() + offset, &value, 4);
    if (!WriteBytes(copy / map::kLayeredWorldFile, bytes)) return false;
    map::PackageReport report;
    const auto loaded = gs::game::LoadWorldPackage(LoadRequest(copy), report);
    return !loaded && !report.Ok() && report.FirstError() &&
        report.FirstError()->code == map::PackageErrorCode::LayeredWorldInvalid;
}

void RunChecks(Checks& checks)
{
    using map::GroundSupportStatus;
    ScratchDirectory scratch;
    map::LayeredWorld world;
    map::LayerClearanceReport cooked;
    std::string error;
    const bool built = CookFixture(world, cooked, error);
    checks.Report("cook-floor-wall-stairs-landing", built);
    if (!built) {
        std::printf("LAYERCLEARANCE cook-error: %s\n", error.c_str());
        return;
    }
    const auto proven = std::count_if(world.portals.begin(), world.portals.end(),
                                      [](const auto& portal) { return portal.proof.has_value(); });
    checks.Report("five-proven-step-portals", proven == 5 && world.volumes.size() == 6);
    const auto encoded = map::EncodeLayeredWorld(world);
    std::uint32_t version = 0;
    if (encoded.size() >= 8) std::memcpy(&version, encoded.data() + 4, 4);
    checks.Report("sidecar-is-mx3d-v4", version == map::kLayeredWorldFileVersion && version == 4);

    auto spec = PackageSpec();
    spec.layered_world = world;
    const auto package_root = scratch.root / "clearance";
    const auto written = map::WritePackage(package_root, spec);
    checks.Report("strict-writer-accepts-v4-sidecar", written.ok);
    if (!written.ok) {
        std::printf("LAYERCLEARANCE writer-error: %s\n", written.error.c_str());
        return;
    }
    checks.Report("written-sidecar-bytes-are-the-encoded-world",
                  ReadBytes(package_root / map::kLayeredWorldFile) == encoded);

    map::PackageReport report;
    auto loaded = gs::game::LoadWorldPackage(LoadRequest(package_root), report);
    checks.Report("strict-full-production-package-load", loaded && report.Ok());
    if (!loaded) {
        gs::game::LogPackageReport(report);
        return;
    }
    checks.Report("loaded-metadata-bit-identical-to-cooked", loaded->layered_world &&
        loaded->layered_world->clearance_profile && map::EncodeLayeredWorld(*loaded->layered_world) == encoded);

    boost::asio::io_context io;
    const auto layout = SingleLeafLayout();
    // Construct the production orchestrator, but never Start it or create
    // sessions/entities. The queried state is an offline value only.
    gs::game::WorldRuntime runtime(io, {}, std::move(*loaded), layout);
    const auto* metadata = runtime.LayeredMetadata();
    const auto* floor = metadata ? FootprintAt(*metadata, 10, 16) : nullptr;
    const auto* landing = metadata ? FootprintAt(*metadata, 33, 16) : nullptr;
    if (!floor || !landing) {
        checks.Report("runtime-floor-and-landing-volumes", false);
        return;
    }
    const map::LayerActorProfile actor{};
    const auto start = runtime.PlaceLayerActor(actor, floor->id, 10, 16);
    checks.Report("actor-placed-on-floor", start.Ok() && start.state.z == 1);

    const auto wall = runtime.MoveLayerActor(actor, start.state, floor->id, 18, 16);
    checks.Report("move-through-wall-blocked-keeps-state",
                  wall.status == GroundSupportStatus::Blocked && SameState(wall.state, start.state));
    checks.Report("actor-cannot-stand-against-wall",
                  runtime.PlaceLayerActor(actor, floor->id, 13.8, 16).status == GroundSupportStatus::Blocked);
    checks.Report("support-only-query-still-answers-against-wall",
                  runtime.PlaceLayerGround(floor->id, 13.8, 16).Ok());

    auto state = start.state;
    bool around = true;
    for (const auto& point : {std::pair{10.0, 22.0}, std::pair{18.0, 22.0}, std::pair{20.0, 16.0}}) {
        const auto step = runtime.MoveLayerActor(actor, state, floor->id, point.first, point.second);
        around = around && step.Ok();
        if (step.Ok()) state = step.state;
    }
    checks.Report("move-around-wall-ok", around);

    int crossings = 0;
    bool climbed = around;
    for (int k = 0; k <= 70 && climbed; ++k) {
        const double x = (100 + k) * 0.2;
        const auto* target = FootprintAt(*metadata, x, 16);
        const auto step = target ? runtime.MoveLayerActor(actor, state, target->id, x, 16)
                                 : map::LayerGroundResult{};
        climbed = step.Ok();
        if (!climbed) std::printf("LAYERCLEARANCE stair walk stopped at x=%.2f status=%s\n", x, map::ToString(step.status));
        if (step.portal_id != 0) ++crossings;
        if (climbed) state = step.state;
    }
    checks.Report("actor-walks-stairs-via-five-proven-portals",
                  climbed && crossings == 5 && state.volume_id == landing->id && state.z == 2);
    const auto down = runtime.MoveLayerActor(actor, state, landing->id, 34, 16);
    checks.Report("landing-move-keeps-height", down.Ok() && down.state.z == 2);

    const auto edge = runtime.PlaceLayerActor(actor, floor->id, 27, 16);
    const auto skip = edge.Ok() ? runtime.MoveLayerActor(actor, edge.state, landing->id, 33, 16)
                                : map::LayerGroundResult{};
    checks.Report("skipping-stairs-requires-transition", edge.Ok() &&
        skip.status == GroundSupportStatus::TransitionRequired && SameState(skip.state, edge.state));
    const auto support_only = edge.Ok() ? runtime.MoveLayerGround(edge.state, landing->id, 33, 16)
                                        : map::LayerGroundResult{};
    checks.Report("support-only-move-still-never-changes-volume",
                  support_only.status == GroundSupportStatus::TransitionRequired);
    checks.Report("larger-actor-not-covered-by-bake",
                  runtime.PlaceLayerActor(map::LayerActorProfile{0.5f, 1.8f}, floor->id, 10, 16).status ==
                      GroundSupportStatus::ActorNotCovered);

    // Tampered profiles no longer match the stored proofs: refused at load.
    checks.Report("tampered-radius-refused-at-load",
                  TamperedProfileRefused(package_root, scratch.root / "tamper_radius", 20, 0.5f));
    checks.Report("tampered-step-height-refused-at-load",
                  TamperedProfileRefused(package_root, scratch.root / "tamper_step", 28, 0.1f));
    checks.Report("nonfinite-profile-refused-at-load",
                  TamperedProfileRefused(package_root, scratch.root / "tamper_nan", 16, std::nanf("")));

    // A support-only (v3) package keeps 3D-4A answers and fails closed for actors.
    auto support_world = world;
    support_world.clearance_profile.reset();
    for (auto& volume : support_world.volumes) volume.clearance.reset();
    support_world.portals.clear();
    auto support_spec = PackageSpec();
    support_spec.layered_world = support_world;
    const auto support_root = scratch.root / "support_only";
    const auto support_written = map::WritePackage(support_root, support_spec);
    map::PackageReport support_report;
    auto support_loaded = support_written.ok
        ? gs::game::LoadWorldPackage(LoadRequest(support_root), support_report) : std::nullopt;
    const auto support_bytes = ReadBytes(support_root / map::kLayeredWorldFile);
    std::uint32_t support_version = 0;
    if (support_bytes.size() >= 8) std::memcpy(&support_version, support_bytes.data() + 4, 4);
    checks.Report("support-only-package-stays-v3", support_loaded && support_report.Ok() &&
                  support_version == map::kLayeredWorldSupportFileVersion);
    if (support_loaded) {
        gs::game::WorldRuntime support_runtime(io, {}, std::move(*support_loaded), layout);
        checks.Report("support-only-world-actor-queries-fail-closed",
            support_runtime.PlaceLayerGround(floor->id, 10, 16).Ok() &&
            support_runtime.PlaceLayerActor(actor, floor->id, 10, 16).status ==
                GroundSupportStatus::NoClearanceProof);
    }

    gs::game::WorldRuntime::SyntheticWorldConfig synthetic;
    synthetic.extent_m = 256;
    synthetic.zones_x = synthetic.zones_y = 1;
    gs::game::WorldRuntime synthetic_runtime(io, {}, synthetic);
    const auto synthetic_move = synthetic_runtime.MoveLayerActor(actor, start.state, floor->id, 12, 16);
    checks.Report("synthetic-world-no-invented-clearance",
        synthetic_runtime.PlaceLayerActor(actor, floor->id, 10, 16).status == GroundSupportStatus::NotAvailable &&
        synthetic_move.status == GroundSupportStatus::NotAvailable && SameState(synthetic_move.state, start.state));

    const auto& zone = runtime.Zones().GetZone(0);
    checks.Report("offline-actor-queries-create-no-production-presence", runtime.Owners().empty() &&
        zone.Entities().empty() && zone.Players().empty() && zone.Ghosts().empty() && zone.Grid().Size() == 0);
    std::printf("LAYERCLEARANCE cooked cells=%llu blocked=%llu corridor=%llu corridorBlocked=%llu bytes=%zu\n",
                static_cast<unsigned long long>(cooked.cells_total),
                static_cast<unsigned long long>(cooked.cells_blocked),
                static_cast<unsigned long long>(cooked.corridor_slots),
                static_cast<unsigned long long>(cooked.corridor_slots_blocked), encoded.size());
}

} // namespace

int RunLayerClearanceScenario()
{
    Checks checks;
    try {
        RunChecks(checks);
    } catch (const std::exception& error) {
        checks.Report("unexpected-exception", false);
        std::printf("LAYERCLEARANCE exception: %s\n", error.what());
    }
    std::printf("LAYERCLEARANCE summary passes=%d failures=%d scope=offline-actor-clearance-only\n",
                checks.passes, checks.failures);
    return checks.failures;
}

} // namespace gs::bench
