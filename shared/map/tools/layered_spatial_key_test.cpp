#include "map/LayeredSpatialKey.h"

#include <iostream>
#include <unordered_map>

namespace {

int failures = 0;

void Check(const char* name, bool value)
{
    std::cout << "LAYERED3D SPATIAL " << name << ": " << (value ? "PASS" : "FAIL") << '\n';
    if (!value) {
        ++failures;
    }
}

} // namespace

int main()
{
    using mx::map::LayeredSpatialCellKey;
    using mx::map::LayeredSpatialCellKeyHash;

    const LayeredSpatialCellKey ground{1, 4, -2};
    const LayeredSpatialCellKey upper{2, 4, -2};
    const LayeredSpatialCellKey legacy{mx::map::kLegacyLayerId, 4, -2};
    const LayeredSpatialCellKey ground_same{1, 4, -2};

    Check("same-volume-equal", ground == ground_same);
    Check("stacked-volumes-distinct", !(ground == upper));
    Check("legacy-distinct", !(ground == legacy) && !(upper == legacy));

    std::unordered_map<LayeredSpatialCellKey, int, LayeredSpatialCellKeyHash> cells;
    cells.emplace(ground, 10);
    cells.emplace(upper, 20);
    cells.emplace(legacy, 30);
    Check("unordered-map-three-entries", cells.size() == 3);
    Check("ground-value", cells.at(ground) == 10);
    Check("upper-value", cells.at(upper) == 20);
    Check("legacy-value", cells.at(legacy) == 30);

    std::cout << "LAYERED3D SPATIAL summary: failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
