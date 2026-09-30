#include "map/LayeredWorldGenerator.h"

#include <iostream>

namespace {

int failures = 0;

void Check(const char* name, bool value)
{
    std::cout << "LAYERED3D GENERATOR " << name << ": " << (value ? "PASS" : "FAIL") << '\n';
    if (!value) {
        ++failures;
    }
}

} // namespace

int main()
{
    using namespace mx::map;
    const Rect world_bounds{0, 0, 16, 16};
    const std::vector<LayerSourceSurface> surfaces = {
        LayerSourceSurface{1, "road_a", Rect{0, 0, 8, 16}, 0, 4, VolumeTagGround | VolumeTagRoad, true},
        LayerSourceSurface{2, "road_b", Rect{8, 0, 16, 16}, 0, 4, VolumeTagGround | VolumeTagRoad, true},
        LayerSourceSurface{3, "building_floor", Rect{2, 2, 12, 12}, 6, 10,
                           VolumeTagBuilding | VolumeTagInterior, true},
        LayerSourceSurface{4, "underpass", Rect{0, 0, 16, 16}, -10, -6,
                           VolumeTagConnector | VolumeTagRoad, false},
        LayerSourceSurface{5, "water", Rect{12, 12, 16, 16}, 4.5f, 5.5f, VolumeTagWater, false},
    };
    LayeredWorld generated;
    LayerGenerationReport report;
    const bool ok = GenerateLayeredWorld(surfaces, LayerGenerationOptions{world_bounds, 0.05f, 0.25f},
                                         generated, report);
    Check("fixture-generated", ok && report.Ok());
    Check("adjacent-surfaces-merged", report.surfaces_merged == 1 && generated.volumes.size() == 4);
    const auto building = generated.FindVolume(4, 4, 8);
    Check("building-kind-and-tag", building.has_value() &&
                                      generated.volumes[*building - 1].kind == VolumeKind::Interior &&
                                      HasVolumeTag(generated.volumes[*building - 1].tags, VolumeTagBuilding));
    Check("underpass-kind", generated.FindVolume(4, 4, -8).has_value() &&
                                generated.volumes[0].kind == VolumeKind::Connector);
    const auto water = generated.FindVolume(14, 14, 5);
    Check("water-kind-and-tag", water.has_value() &&
                                   HasVolumeTag(generated.volumes[*water - 1].tags, VolumeTagWater));

    auto invalid = surfaces;
    invalid[0].tags |= (1u << 31);
    LayeredWorld rejected;
    LayerGenerationReport rejected_report;
    Check("unknown-tag-rejected",
          !GenerateLayeredWorld(invalid, LayerGenerationOptions{world_bounds}, rejected, rejected_report) &&
              !rejected_report.errors.empty());

    std::cout << "LAYERED3D GENERATOR summary: failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
