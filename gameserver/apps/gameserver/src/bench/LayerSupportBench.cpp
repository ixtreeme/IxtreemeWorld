#include "LayerSupportBench.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <boost/asio/io_context.hpp>

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
        std::printf("LAYERSUPPORT %s: %s\n", name, pass ? "PASS" : "FAIL");
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
            const auto candidate = parent / ("ixw_layersupport_" + std::to_string(suffix) + "_" +
                                             std::to_string(attempt));
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) {
                root = candidate;
                return;
            }
            if (ec) throw std::runtime_error("cannot create layer support scratch directory: " + ec.message());
        }
        throw std::runtime_error("cannot allocate a unique layer support scratch directory");
    }

    ~ScratchDirectory()
    {
        if (root.is_absolute() && root.parent_path() == parent &&
            root.filename().string().starts_with("ixw_layersupport_")) {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    }
};

bool Near(double actual, double expected)
{
    return std::isfinite(actual) && std::abs(actual - expected) < 1e-10;
}

bool SameNumber(double lhs, double rhs)
{
    return lhs == rhs || (std::isnan(lhs) && std::isnan(rhs));
}

bool SameState(const map::LayerGroundState& lhs, const map::LayerGroundState& rhs)
{
    return lhs.volume_id == rhs.volume_id && lhs.layer_id == rhs.layer_id &&
        SameNumber(lhs.x, rhs.x) && SameNumber(lhs.y, rhs.y) && SameNumber(lhs.z, rhs.z);
}

map::LayeredWorld LayerFixture()
{
    using namespace map;
    LayeredWorld world;
    world.volumes = {
        LayerVolume{101, 1, "lower", Rect{-128,-128,128,128}, -0.02f, 2,
                    VolumeKind::Ground, true, VolumeTagGround},
        LayerVolume{202, 2, "upper", Rect{-64,-64,64,64}, 7.98f, 10,
                    VolumeKind::Interior, true, VolumeTagBuilding | VolumeTagInterior},
        LayerVolume{303, 3, "negative-underpass", Rect{-128,-128,128,128}, -12.02f, -10,
                    VolumeKind::Connector, true, VolumeTagConnector},
        LayerVolume{404, 4, "ramp", Rect{-16,-16,16,16}, 17.98f, 32,
                    VolumeKind::Connector, true, VolumeTagRoad | VolumeTagConnector},
        LayerVolume{505, 5, "water", Rect{80,80,96,96}, 39.98f, 42,
                    VolumeKind::WaterSurface, false, VolumeTagWater},
        LayerVolume{606, 6, "legacy-metadata-without-support", Rect{80,80,96,96}, 49.98f, 52,
                    VolumeKind::Interior, true, VolumeTagInterior},
    };
    world.volumes[0].ground_support = LayerSupportPlane{10,1,0,0,0,0,0,0};
    world.volumes[1].ground_support = LayerSupportPlane{20,1,0,0,8,0,0,0};
    world.volumes[2].ground_support = LayerSupportPlane{30,1,0,0,-12,0,0,0};
    world.volumes[3].ground_support = LayerSupportPlane{40,1,0,0,24,0.125,-0.25,0.00001};
    // Water keeps volume metadata but never acquires ground support. The
    // separate movement-allowed legacy volume also has no invented plane.
    world.portals.push_back(LayerPortal{11,101,202,Rect{8,8,12,12},Rect{8,8,12,12},
                                       -0.02f,2,7.98f,10,true});
    return world;
}

map::PackageWriteSpec PackageSpec()
{
    map::PackageWriteSpec spec;
    spec.world_id = "layersupport_fixture";
    spec.world_name = "Offline volume-ground-state fixture";
    spec.size_cells_x = 256;
    spec.size_cells_y = 256;
    spec.origin_x = -128;
    spec.origin_y = -128;
    spec.cell_size_m = 1;
    spec.chunk_size_cells = 128;
    spec.height_raw = [](std::uint32_t, std::uint32_t) { return 0; };
    spec.attributes = [](std::uint32_t, std::uint32_t) { return std::uint16_t{0}; };
    spec.logic.spawns.push_back(map::SpawnRegion{1,0,map::Rect{-101,-101,-99,-99}});
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

void RunChecks(Checks& checks)
{
    using map::GroundSupportStatus;
    ScratchDirectory scratch;
    auto spec = PackageSpec();
    spec.layered_world = LayerFixture();
    const auto package_root = scratch.root / "supported";
    const auto written = map::WritePackage(package_root, spec);
    checks.Report("write-unique-supported-fixture", written.ok);
    if (!written.ok) {
        std::printf("LAYERSUPPORT writer-error: %s\n", written.error.c_str());
        return;
    }

    map::PackageReport report;
    auto loaded = gs::game::LoadWorldPackage(LoadRequest(package_root), report);
    checks.Report("strict-full-production-package-load", loaded && report.Ok());
    if (!loaded) {
        gs::game::LogPackageReport(report);
        return;
    }
    checks.Report("package-retains-source-component-and-error-bound", loaded->layered_world &&
        loaded->layered_world->volumes[3].ground_support &&
        loaded->layered_world->volumes[3].ground_support->source_id == 40 &&
        loaded->layered_world->volumes[3].ground_support->component_id == 1 &&
        loaded->layered_world->volumes[3].ground_support->max_height_error_m == 0.00001);

    boost::asio::io_context io;
    const auto layout = SingleLeafLayout();
    // Construct the actual production orchestrator, but never Start it or
    // create sessions/entities. The queried state is an offline value only.
    gs::game::WorldRuntime runtime(io, {}, std::move(*loaded), layout);
    const auto original_bounds = runtime.Terrain().Bounds();
    const auto original_zone_count = runtime.Zones().ZoneCount();
    const auto original_zone_id = runtime.Zones().GetZone(0).Id();
    const auto lower = runtime.PlaceLayerGround(101, 10, 10);
    const auto upper = runtime.PlaceLayerGround(202, 10, 10);
    checks.Report("same-xy-two-explicit-ground-states", lower.Ok() && upper.Ok() &&
        lower.state.volume_id == 101 && lower.state.layer_id == 1 && Near(lower.state.z,0) &&
        upper.state.volume_id == 202 && upper.state.layer_id == 2 && Near(upper.state.z,8));
    const auto below = runtime.PlaceLayerGround(303, -80, -90);
    checks.Report("negative-world-xy-and-support-height", below.Ok() &&
        below.state.x == -80 && below.state.y == -90 && Near(below.state.z,-12));
    const auto ramp = runtime.PlaceLayerGround(404, -8, 4);
    checks.Report("ramp-placement-independent-plane-oracle", ramp.Ok() &&
        Near(ramp.state.z,22) && ramp.max_height_error_m == 0.00001);
    const auto ramp_move = runtime.MoveLayerGround(ramp.state,404,8,-4);
    checks.Report("same-volume-ramp-move-independent-plane-oracle", ramp_move.Ok() &&
        ramp_move.state.volume_id == 404 && ramp_move.state.layer_id == 4 && Near(ramp_move.state.z,26));
    const auto upper_move = runtime.MoveLayerGround(upper.state,202,-12,-16);
    checks.Report("same-volume-upper-move-keeps-height", upper_move.Ok() && Near(upper_move.state.z,8));

    const auto fail_move = [&](const char* name, const map::LayerGroundState& state,
                               map::VolumeId target, double x, double y, GroundSupportStatus expected) {
        const auto result = runtime.MoveLayerGround(state,target,x,y);
        checks.Report(name, !result.Ok() && result.status == expected && SameState(result.state,state));
    };
    fail_move("cross-volume-needs-separate-transition-even-at-declared-portal",lower.state,202,10,10,
              GroundSupportStatus::TransitionRequired);
    fail_move("same-volume-outside-keeps-current-state",upper.state,202,64,10,
              GroundSupportStatus::OutsideVolume);
    auto wrong_layer = upper.state;
    wrong_layer.layer_id = 999;
    fail_move("mismatched-current-layer-rejected",wrong_layer,202,12,12,GroundSupportStatus::InvalidState);
    auto wrong_height = upper.state;
    wrong_height.z += 0.5;
    fail_move("off-support-current-height-rejected",wrong_height,202,12,12,GroundSupportStatus::InvalidState);
    auto nonfinite_current = upper.state;
    nonfinite_current.x = std::numeric_limits<double>::quiet_NaN();
    fail_move("nonfinite-current-state-rejected",nonfinite_current,202,12,12,GroundSupportStatus::InvalidState);
    checks.Report("unknown-explicit-volume-no-fallback",runtime.PlaceLayerGround(999,10,10).status ==
        GroundSupportStatus::UnknownVolume);
    checks.Report("half-open-upper-footprint",runtime.PlaceLayerGround(202,64,10).status ==
        GroundSupportStatus::OutsideVolume);
    const double inside_upper_edge = std::nextafter(64.0,0.0);
    const auto near_edge = runtime.PlaceLayerGround(202,inside_upper_edge,10);
    checks.Report("double-query-does-not-round-inner-point-onto-excluded-edge",near_edge.Ok() &&
        near_edge.state.x == inside_upper_edge && Near(near_edge.state.z,8));
    checks.Report("water-volume-does-not-admit-ground-movement",runtime.PlaceLayerGround(505,88,88).status ==
        GroundSupportStatus::UnsupportedMovement);
    checks.Report("metadata-without-support-no-terrain-fallback",runtime.PlaceLayerGround(606,88,88).status ==
        GroundSupportStatus::NotAvailable);
    const auto terrain_height = runtime.Terrain().Height(10,10);
    checks.Report("legacy-terrain-query-is-unchanged",terrain_height.Ok() && terrain_height.meters == 0 &&
        upper.Ok() && upper.state.z != terrain_height.meters);

    // No activity, partition, grid, entity or presence write is caused by
    // these read-only offline state queries.
    const auto& after_bounds = runtime.Terrain().Bounds();
    checks.Report("queries-leave-runtime-bounds-and-partition-unchanged",
        original_bounds.min_x == after_bounds.min_x && original_bounds.min_y == after_bounds.min_y &&
        original_bounds.max_x == after_bounds.max_x && original_bounds.max_y == after_bounds.max_y &&
        original_zone_count == 1 && runtime.Zones().ZoneCount() == original_zone_count &&
        runtime.Zones().GetZone(0).Id() == original_zone_id);
    const auto& zone = runtime.Zones().GetZone(0);
    checks.Report("offline-ground-states-create-no-production-presence",runtime.Owners().empty() &&
        zone.Entities().empty() && zone.Players().empty() && zone.Ghosts().empty() && zone.Grid().Size() == 0);

    // Public LoadedWorld constructors must also refuse unchecked metadata,
    // before InitializeWorld can publish any runtime/partition state.
    map::PackageReport manual_report;
    auto manual = gs::game::LoadWorldPackage(LoadRequest(package_root),manual_report);
    bool rejected_manual = false;
    if (manual && manual->layered_world) {
        manual->layered_world->volumes[1].ground_support->anchor_z = 1000;
        try {
            gs::game::WorldRuntime invalid(io,{},std::move(*manual),layout);
        } catch (const std::invalid_argument& error) {
            rejected_manual = std::string(error.what()).find("invalid layered metadata") != std::string::npos;
        }
    }
    checks.Report("unchecked-loaded-world-invalid-support-refused-before-initialize",rejected_manual);

    // Bound the manually assembled path before Validate's pairwise overlap
    // work, just as strict decoding bounds its incoming record count.
    map::PackageReport oversized_report;
    auto oversized = gs::game::LoadWorldPackage(LoadRequest(package_root),oversized_report);
    bool rejected_oversized = false;
    if (oversized && oversized->layered_world) {
        const auto prototype = oversized->layered_world->volumes.front();
        oversized->layered_world->volumes.assign(map::kMaxLayeredWorldVolumes + 1,prototype);
        try {
            gs::game::WorldRuntime invalid(io,{},std::move(*oversized),layout);
        } catch (const std::invalid_argument& error) {
            rejected_oversized = std::string(error.what()).find("record limits exceeded") != std::string::npos;
        }
    }
    checks.Report("unchecked-loaded-world-record-limit-before-overlap-validation",rejected_oversized);

    // The runtime's immutable data must not depend on source-file lifetime.
    {
        std::ofstream append(package_root / map::kLayeredWorldFile,std::ios::binary | std::ios::app);
        append.put('\x7f');
        checks.Report("strict-malformed-sidecar-control-created",static_cast<bool>(append));
    }
    map::PackageReport malformed_report;
    const auto malformed = gs::game::LoadWorldPackage(LoadRequest(package_root),malformed_report);
    checks.Report("malformed-sidecar-refused-before-runtime",!malformed && !malformed_report.Ok() &&
        malformed_report.FirstError() && malformed_report.FirstError()->code == map::PackageErrorCode::LayeredWorldTrailingData);
    checks.Report("runtime-support-independent-of-source-file-changes",
        SameState(runtime.PlaceLayerGround(202,10,10).state,upper.state));

    for (const bool empty_sidecar : {false,true}) {
        auto legacy_spec = PackageSpec();
        if (empty_sidecar) legacy_spec.layered_world = map::LayeredWorld{};
        const auto root = scratch.root / (empty_sidecar ? "empty_sidecar" : "absent_sidecar");
        const auto legacy_written = map::WritePackage(root,legacy_spec);
        map::PackageReport legacy_report;
        auto legacy = legacy_written.ok
            ? gs::game::LoadWorldPackage(LoadRequest(root),legacy_report) : std::nullopt;
        checks.Report(empty_sidecar ? "strict-empty-sidecar-fixture" : "strict-absent-sidecar-fixture",
                      legacy && legacy_report.Ok());
        if (!legacy) continue;
        gs::game::WorldRuntime legacy_runtime(io,{},std::move(*legacy),layout);
        const auto unavailable = legacy_runtime.PlaceLayerGround(101,10,10);
        const auto unmoved = legacy_runtime.MoveLayerGround(upper.state,202,12,12);
        checks.Report(empty_sidecar ? "empty-sidecar-no-offline-support" : "absent-sidecar-no-offline-support",
            unavailable.status == GroundSupportStatus::NotAvailable &&
            unmoved.status == GroundSupportStatus::NotAvailable && SameState(unmoved.state,upper.state));
    }
    gs::game::WorldRuntime::SyntheticWorldConfig synthetic;
    synthetic.extent_m = 256;
    synthetic.zones_x = synthetic.zones_y = 1;
    gs::game::WorldRuntime synthetic_runtime(io,{},synthetic);
    const auto synthetic_move = synthetic_runtime.MoveLayerGround(upper.state,202,12,12);
    checks.Report("synthetic-world-no-invented-support",synthetic_runtime.PlaceLayerGround(101,10,10).status ==
        GroundSupportStatus::NotAvailable && synthetic_move.status == GroundSupportStatus::NotAvailable &&
        SameState(synthetic_move.state,upper.state));
}

} // namespace

int RunLayerSupportScenario()
{
    Checks checks;
    try {
        RunChecks(checks);
    } catch (const std::exception& error) {
        checks.Report("unexpected-exception",false);
        std::printf("LAYERSUPPORT exception: %s\n",error.what());
    }
    std::printf("LAYERSUPPORT summary passes=%d failures=%d scope=offline-ground-state-only\n",
                checks.passes,checks.failures);
    return checks.failures;
}

} // namespace gs::bench
