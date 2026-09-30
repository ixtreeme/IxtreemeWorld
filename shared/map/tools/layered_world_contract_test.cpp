#include "map/LayeredWorld.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void Check(const char* name, bool value)
{
    std::printf("LAYERED3D %s: %s\n", name, value ? "PASS" : "FAIL");
    if (!value) {
        ++failures;
    }
}

} // namespace

int main()
{
    using namespace mx::map;

    const Rect world{0.0f, 0.0f, 100.0f, 100.0f};
    LayeredWorld stacked;
    stacked.volumes = {
        LayerVolume{1, 1, "street", Rect{0, 0, 100, 100}, 0, 4, VolumeKind::Ground, true},
        LayerVolume{2, 2, "upper-floor", Rect{0, 0, 100, 100}, 6, 10, VolumeKind::Interior, true},
    };
    stacked.portals = {
        LayerPortal{1, 1, 2, Rect{10, 10, 12, 12}, Rect{10, 10, 12, 12}, 0, 4, 6, 10, true},
    };
    std::string error;
    Check("stacked-valid", stacked.Validate(world, error) && error.empty());
    Check("ground-lookup", stacked.FindVolume(50, 50, 1).value_or(0) == 1);
    Check("upper-lookup", stacked.FindVolume(50, 50, 7).value_or(0) == 2);
    Check("gap-uncovered", !stacked.FindVolume(50, 50, 5).has_value());
    Check("portal-traverse", stacked.CanTraverse(stacked.portals.front(), 11, 11, 1, 11, 11, 7));

    LayeredWorld overlap = stacked;
    overlap.volumes[1].min_z = 3.0f;
    Check("overlap-rejected", !overlap.Validate(world, error) && error.find("overlap") != std::string::npos);

    LayeredWorld bad_portal = stacked;
    bad_portal.portals.front().target_bounds = Rect{99, 99, 101, 101};
    Check("portal-outside-rejected", !bad_portal.Validate(world, error));

    LayeredWorld legacy;
    Check("legacy-empty-contract", legacy.Validate(world, error));
    Check("legacy-no-implicit-volume", !legacy.FindVolume(50, 50, 0).has_value());

    std::printf("LAYERED3D summary: failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
