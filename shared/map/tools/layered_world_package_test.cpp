#include "map/WorldPackage.h"
#include "map/WorldPackageWriter.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

int failures = 0;

void Check(const char* name, bool value)
{
    std::cout << "LAYERED3D PACKAGE " << name << ": " << (value ? "PASS" : "FAIL") << '\n';
    if (!value) {
        ++failures;
    }
}

mx::map::LayeredWorld Fixture()
{
    using namespace mx::map;
    LayeredWorld world;
    world.volumes = {
        LayerVolume{1, 1, "ground", Rect{0, 0, 32, 32}, 0, 4, VolumeKind::Ground, true},
        LayerVolume{2, 2, "upper-floor", Rect{8, 8, 24, 24}, 6, 10, VolumeKind::Interior, true},
        LayerVolume{3, 3, "underpass", Rect{0, 0, 32, 32}, -10, -6, VolumeKind::Connector, false},
        LayerVolume{4, 4, "water-surface", Rect{20, 20, 28, 28}, 4.5f, 5.5f, VolumeKind::WaterSurface, false},
    };
    world.portals = {
        LayerPortal{1, 1, 2, Rect{10, 10, 12, 12}, Rect{10, 10, 12, 12}, 0, 4, 6, 10, true},
        LayerPortal{2, 1, 3, Rect{4, 4, 6, 6}, Rect{4, 4, 6, 6}, 0, 4, -10, -6, true},
    };
    return world;
}

} // namespace

int main()
{
    using namespace mx::map;
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() / ("ixw_layered_world_" + std::to_string(suffix));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    PackageWriteSpec spec;
    spec.world_id = "layered_fixture";
    spec.world_name = "Layered 3D strict fixture";
    spec.size_cells_x = 32;
    spec.size_cells_y = 32;
    spec.cell_size_m = 1.0f;
    spec.chunk_size_cells = 16;
    spec.height_raw = [](std::uint32_t, std::uint32_t) { return 0; };
    spec.attributes = [](std::uint32_t, std::uint32_t) { return std::uint16_t{0}; };
    spec.logic.spawns.push_back(SpawnRegion{1, 0, Rect{2, 2, 4, 4}});
    spec.layered_world = Fixture();
    spec.overwrite = true;

    const auto write = WritePackage(root, spec);
    Check("write-fixture", write.ok);
    Check("sidecar-present", std::filesystem::is_regular_file(root / kLayeredWorldFile, ec));

    PackageReport report;
    const auto loaded = LoadServerWorld(root, ValidationDepth::Startup, report);
    Check("strict-load", loaded.has_value() && report.Ok());
    Check("layered-world-loaded", loaded.has_value() && loaded->layered_world.has_value());
    Check("upper-volume-lookup",
          loaded.has_value() && loaded->layered_world &&
              loaded->layered_world->FindVolume(10, 10, 7).value_or(0) == 2);
    Check("underpass-lookup",
          loaded.has_value() && loaded->layered_world &&
              loaded->layered_world->FindVolume(5, 5, -8).value_or(0) == 3);
    Check("water-lookup",
          loaded.has_value() && loaded->layered_world &&
              loaded->layered_world->FindVolume(22, 22, 5).value_or(0) == 4);

    {
        std::ofstream append(root / kLayeredWorldFile, std::ios::binary | std::ios::app);
        append.put('\x7f');
    }
    PackageReport malformed_report;
    const auto malformed = LoadServerWorld(root, ValidationDepth::Startup, malformed_report);
    Check("trailing-byte-rejected", !malformed.has_value() && !malformed_report.Ok() &&
                                         malformed_report.FirstError() != nullptr &&
                                         malformed_report.FirstError()->code == PackageErrorCode::LayeredWorldTrailingData);

    std::filesystem::remove_all(root, ec);
    std::cout << "LAYERED3D PACKAGE summary: failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
