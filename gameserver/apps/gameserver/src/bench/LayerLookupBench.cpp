#include "LayerLookupBench.h"

#include <chrono>
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

struct Checks {
    int passes = 0;
    int failures = 0;

    void Report(const char* name, bool pass)
    {
        std::printf("LAYERLOOKUP %s: %s\n", name, pass ? "PASS" : "FAIL");
        (pass ? passes : failures) += 1;
    }
};

// Own only a successfully created, unique child of the resolved temp root.
// No existing fixture or checked-in asset is removed or overwritten.
struct ScratchDirectory {
    fs::path root;

    ScratchDirectory()
    {
        const fs::path temp = fs::weakly_canonical(fs::temp_directory_path());
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 10; ++attempt) {
            const auto candidate = temp / ("ixw_layerlookup_" + std::to_string(suffix) + "_" +
                                          std::to_string(attempt));
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) {
                root = candidate;
                return;
            }
            if (ec) {
                throw std::runtime_error("cannot create layer lookup scratch directory: " + ec.message());
            }
        }
        throw std::runtime_error("cannot allocate a unique layer lookup scratch directory");
    }

    ~ScratchDirectory()
    {
        if (!root.empty()) {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    }
};

mx::map::LayeredWorld LayerFixture()
{
    using namespace mx::map;
    LayeredWorld world;
    // Nonsequential identities make lookup independent of vector indexing.
    world.volumes = {
        LayerVolume{101, 1, "ground", Rect{0, 0, 256, 256}, 0, 4, VolumeKind::Ground, true,
                    VolumeTagGround | VolumeTagRoad},
        LayerVolume{202, 2, "upper-floor", Rect{64, 64, 192, 192}, 6, 10, VolumeKind::Interior, true,
                    VolumeTagBuilding | VolumeTagInterior},
        LayerVolume{303, 3, "underpass", Rect{0, 0, 256, 256}, -10, -6, VolumeKind::Connector, false,
                    VolumeTagConnector | VolumeTagRoad},
        LayerVolume{404, 4, "water-surface", Rect{208, 208, 224, 224}, 4.5f, 5.5f,
                    VolumeKind::WaterSurface, false, VolumeTagWater},
    };
    world.portals = {
        LayerPortal{11, 101, 202, Rect{72, 72, 74, 74}, Rect{72, 72, 74, 74}, 0, 4, 6, 10, true},
        LayerPortal{12, 101, 303, Rect{20, 20, 22, 22}, Rect{20, 20, 22, 22}, 0, 4, -10, -6, true},
    };
    return world;
}

mx::map::PackageWriteSpec PackageSpec()
{
    using namespace mx::map;
    PackageWriteSpec spec;
    spec.world_id = "layerlookup_fixture";
    spec.world_name = "Immutable layered metadata fixture";
    // One initial leaf still meets the unchanged 2x-AOI partition floor.
    spec.size_cells_x = 256;
    spec.size_cells_y = 256;
    spec.cell_size_m = 1.0f;
    spec.chunk_size_cells = 128;
    spec.height_raw = [](std::uint32_t, std::uint32_t) { return 0; };
    spec.attributes = [](std::uint32_t, std::uint32_t) { return std::uint16_t{0}; };
    spec.logic.spawns.push_back(SpawnRegion{1, 0, Rect{2, 2, 4, 4}});
    return spec;
}

gs::game::WorldLoadRequest LoadRequest(const fs::path& root)
{
    gs::game::WorldLoadRequest request;
    request.package_root = root;
    request.mob_types_config = IXTREEME_DEFAULT_MOB_TYPES_CONFIG;
    request.depth = mx::map::ValidationDepth::Full;
    request.warp_policy = mx::map::WarpPolicy::Strict;
    return request;
}

gs::game::PartitionLayout SingleLeafLayout()
{
    gs::game::PartitionLayout layout;
    layout.regions_x = 1;
    layout.regions_y = 1;
    layout.leaves_x = 1;
    layout.leaves_y = 1;
    return layout;
}

void RunChecks(Checks& checks)
{
    using namespace mx::map;
    ScratchDirectory scratch;
    auto spec = PackageSpec();
    spec.layered_world = LayerFixture();
    const auto layered_root = scratch.root / "layered";
    const auto written = WritePackage(layered_root, spec);
    checks.Report("write-unique-layered-fixture", written.ok);
    if (!written.ok) {
        std::printf("LAYERLOOKUP writer-error: %s\n", written.error.c_str());
        return;
    }

    auto request = LoadRequest(layered_root);
    PackageReport report;
    auto loaded = gs::game::LoadWorldPackage(request, report);
    checks.Report("strict-full-production-load", loaded.has_value() && report.Ok());
    if (!loaded) {
        gs::game::LogPackageReport(report);
        return;
    }
    checks.Report("loaded-sidecar-present", loaded->layered_world.has_value());

    boost::asio::io_context io;
    // Construct actual production WorldRuntime, but never Start a supervisor,
    // worker or network session. These checks concern metadata only.
    const auto layout = SingleLeafLayout();
    gs::game::WorldRuntime runtime(io, {}, std::move(*loaded), layout);
    const auto* metadata = runtime.LayeredMetadata();
    checks.Report("runtime-retains-sidecar", metadata != nullptr && metadata->volumes.size() == 4 &&
                                                  metadata->portals.size() == 2);
    const auto* lower = runtime.FindLayerVolume(80, 80, 1);
    const auto* upper = runtime.FindLayerVolume(80, 80, 7);
    checks.Report("same-xy-distinct-lower-upper", lower != nullptr && upper != nullptr &&
                                                    lower->id == 101 && upper->id == 202 &&
                                                    lower->layer_id != upper->layer_id);
    checks.Report("upper-semantic-tags-preserved", upper != nullptr &&
                                                       HasVolumeTag(upper->tags, VolumeTagBuilding) &&
                                                       HasVolumeTag(upper->tags, VolumeTagInterior));
    const auto* underpass = runtime.FindLayerVolume(80, 80, -8);
    checks.Report("underpass-negative-height", underpass != nullptr && underpass->id == 303);
    const auto* water = runtime.FindLayerVolume(220, 220, 5);
    checks.Report("water-volume-query", water != nullptr && water->id == 404 &&
                                             water->kind == VolumeKind::WaterSurface);
    checks.Report("uncovered-height-null", runtime.FindLayerVolume(80, 80, 5) == nullptr);
    checks.Report("half-open-volume-max-z", runtime.FindLayerVolume(80, 80, 4) == nullptr);
    checks.Report("half-open-volume-max-xy", runtime.FindLayerVolume(192, 80, 7) == nullptr);
    checks.Report("half-open-world-max", runtime.FindLayerVolume(256, 80, 1) == nullptr);
    checks.Report("outside-world-null", runtime.FindLayerVolume(-1, 80, 1) == nullptr);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    checks.Report("nonfinite-xyz-null", runtime.FindLayerVolume(nan, 80, 1) == nullptr &&
                                             runtime.FindLayerVolume(80, nan, 1) == nullptr &&
                                             runtime.FindLayerVolume(80, 80, nan) == nullptr &&
                                             runtime.FindLayerVolume(infinity, 80, 1) == nullptr &&
                                             runtime.FindLayerVolume(80, infinity, 1) == nullptr &&
                                             runtime.FindLayerVolume(80, 80, infinity) == nullptr);
    checks.Report("portal-metadata-preserved", metadata != nullptr && !metadata->portals.empty() &&
                                                   metadata->CanTraverse(metadata->portals.front(),
                                                                         73, 73, 1, 73, 73, 7));
    const auto height = runtime.Terrain().Height(80, 80);
    checks.Report("terrain-height-unchanged", height.Ok() && height.meters == 0.0f);

    // Negative control uses the same strict loader, before any malformed
    // runtime could be constructed. Existing runtime data is independently
    // owned and must not reload or depend on the modified source files.
    {
        std::ofstream append(layered_root / kLayeredWorldFile, std::ios::binary | std::ios::app);
        append.put('\x7f');
        checks.Report("malformed-control-created", static_cast<bool>(append));
    }
    PackageReport malformed_report;
    const auto malformed = gs::game::LoadWorldPackage(request, malformed_report);
    checks.Report("malformed-refused-before-runtime", !malformed && !malformed_report.Ok() &&
                                                         malformed_report.FirstError() != nullptr &&
                                                         malformed_report.FirstError()->code ==
                                                             PackageErrorCode::LayeredWorldTrailingData);
    checks.Report("runtime-metadata-independent-of-files", runtime.LayeredMetadata() == metadata &&
                                                              runtime.FindLayerVolume(80, 80, 7) == upper);

    for (const bool empty_sidecar : {false, true}) {
        auto legacy_spec = PackageSpec();
        if (empty_sidecar) {
            legacy_spec.layered_world = LayeredWorld{};
        }
        const auto legacy_root = scratch.root / (empty_sidecar ? "empty_sidecar" : "absent_sidecar");
        const auto legacy_written = WritePackage(legacy_root, legacy_spec);
        checks.Report(empty_sidecar ? "write-empty-sidecar-fixture" : "write-absent-sidecar-fixture",
                      legacy_written.ok);
        if (!legacy_written.ok) {
            continue;
        }
        PackageReport legacy_report;
        auto legacy = gs::game::LoadWorldPackage(LoadRequest(legacy_root), legacy_report);
        checks.Report(empty_sidecar ? "strict-empty-sidecar-load" : "strict-absent-sidecar-load",
                      legacy.has_value() && legacy_report.Ok());
        if (!legacy) {
            gs::game::LogPackageReport(legacy_report);
            continue;
        }
        gs::game::WorldRuntime legacy_runtime(io, {}, std::move(*legacy), layout);
        checks.Report(empty_sidecar ? "empty-sidecar-legacy-null" : "absent-sidecar-legacy-null",
                      legacy_runtime.LayeredMetadata() == nullptr &&
                          legacy_runtime.FindLayerVolume(80, 80, 1) == nullptr);
    }
    gs::game::WorldRuntime::SyntheticWorldConfig synthetic;
    synthetic.extent_m = 256;
    synthetic.zones_x = 1;
    synthetic.zones_y = 1;
    gs::game::WorldRuntime synthetic_runtime(io, {}, synthetic);
    checks.Report("synthetic-legacy-null", synthetic_runtime.LayeredMetadata() == nullptr &&
                                               synthetic_runtime.FindLayerVolume(80, 80, 1) == nullptr);
}

} // namespace

int RunLayerLookupScenario()
{
    Checks checks;
    try {
        RunChecks(checks);
    } catch (const std::exception& exception) {
        checks.Report("unexpected-exception", false);
        std::printf("LAYERLOOKUP exception: %s\n", exception.what());
    }
    std::printf("LAYERLOOKUP summary passes=%d failures=%d scope=metadata-only\n", checks.passes,
                checks.failures);
    return checks.failures;
}

} // namespace gs::bench
