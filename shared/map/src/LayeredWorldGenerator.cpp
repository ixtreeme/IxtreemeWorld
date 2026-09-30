#include "map/LayeredWorldGenerator.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace mx::map {
namespace {

bool Finite(float value)
{
    return std::isfinite(value);
}

bool ValidSurface(const LayerSourceSurface& surface)
{
    return surface.source_id != 0 &&
           Finite(surface.bounds.min_x) && Finite(surface.bounds.min_y) &&
           Finite(surface.bounds.max_x) && Finite(surface.bounds.max_y) &&
           surface.bounds.min_x < surface.bounds.max_x && surface.bounds.min_y < surface.bounds.max_y &&
           Finite(surface.min_z) && Finite(surface.max_z) && surface.min_z < surface.max_z &&
           (surface.tags & ~kKnownVolumeTags) == 0;
}

bool NearOrOverlap(float lhs_min, float lhs_max, float rhs_min, float rhs_max, float gap)
{
    return lhs_min <= rhs_max + gap && rhs_min <= lhs_max + gap;
}

int Family(std::uint32_t tags)
{
    if (HasVolumeTag(tags, VolumeTagWater)) return 4;
    if (HasVolumeTag(tags, VolumeTagUnderwater)) return 3;
    if (HasVolumeTag(tags, VolumeTagBridge) || HasVolumeTag(tags, VolumeTagConnector) ||
        HasVolumeTag(tags, VolumeTagStairs) || HasVolumeTag(tags, VolumeTagLift)) return 2;
    if (HasVolumeTag(tags, VolumeTagBuilding) || HasVolumeTag(tags, VolumeTagInterior) ||
        HasVolumeTag(tags, VolumeTagDungeon)) return 1;
    return 0;
}

VolumeKind KindFor(std::uint32_t tags)
{
    if (HasVolumeTag(tags, VolumeTagWater)) return VolumeKind::WaterSurface;
    if (HasVolumeTag(tags, VolumeTagUnderwater)) return VolumeKind::Underwater;
    if (HasVolumeTag(tags, VolumeTagBridge) || HasVolumeTag(tags, VolumeTagConnector) ||
        HasVolumeTag(tags, VolumeTagStairs) || HasVolumeTag(tags, VolumeTagLift)) return VolumeKind::Connector;
    if (HasVolumeTag(tags, VolumeTagBuilding) || HasVolumeTag(tags, VolumeTagInterior) ||
        HasVolumeTag(tags, VolumeTagDungeon)) return VolumeKind::Interior;
    return VolumeKind::Ground;
}

bool ParseTags(const std::string& text, std::uint32_t& tags)
{
    tags = VolumeTagNone;
    if (text == "-" || text == "none") return true;
    std::istringstream stream(text);
    std::string token;
    while (std::getline(stream, token, '|')) {
        if (token == "ground") tags |= VolumeTagGround;
        else if (token == "building") tags |= VolumeTagBuilding;
        else if (token == "bridge") tags |= VolumeTagBridge;
        else if (token == "water") tags |= VolumeTagWater;
        else if (token == "underwater") tags |= VolumeTagUnderwater;
        else if (token == "dungeon") tags |= VolumeTagDungeon;
        else if (token == "interior") tags |= VolumeTagInterior;
        else if (token == "connector") tags |= VolumeTagConnector;
        else if (token == "road") tags |= VolumeTagRoad;
        else if (token == "stairs") tags |= VolumeTagStairs;
        else if (token == "lift") tags |= VolumeTagLift;
        else if (token == "dock") tags |= VolumeTagDock;
        else return false;
    }
    return true;
}

struct Component {
    std::vector<std::size_t> members;
    Rect bounds;
    float min_z = 0.0f;
    float max_z = 0.0f;
    std::uint32_t tags = VolumeTagNone;
    bool supports_ground_movement = true;
    int family = 0;
    std::uint32_t min_source_id = 0;
};

} // namespace

bool GenerateLayeredWorld(const std::vector<LayerSourceSurface>& surfaces,
                          const LayerGenerationOptions& options,
                          LayeredWorld& output,
                          LayerGenerationReport& report)
{
    output = LayeredWorld{};
    report = LayerGenerationReport{};
    report.surfaces_seen = surfaces.size();
    std::unordered_set<std::uint32_t> ids;
    if (!std::isfinite(options.merge_xy_gap) || options.merge_xy_gap < 0.0f ||
        !std::isfinite(options.merge_z_gap) || options.merge_z_gap < 0.0f) {
        report.errors.push_back("merge gaps must be finite and non-negative");
        return false;
    }
    for (const auto& surface : surfaces) {
        if (!ValidSurface(surface)) {
            report.errors.push_back("invalid surface record " + std::to_string(surface.source_id));
            continue;
        }
        if (!ids.insert(surface.source_id).second) {
            report.errors.push_back("duplicate surface source id " + std::to_string(surface.source_id));
        }
        if (surface.bounds.min_x < options.world_bounds.min_x || surface.bounds.min_y < options.world_bounds.min_y ||
            surface.bounds.max_x > options.world_bounds.max_x || surface.bounds.max_y > options.world_bounds.max_y) {
            report.errors.push_back("surface " + std::to_string(surface.source_id) + " is outside world bounds");
        }
    }
    if (!report.errors.empty()) {
        return false;
    }
    if (surfaces.empty()) {
        report.errors.push_back("no source surfaces");
        return false;
    }

    std::vector<std::size_t> parent(surfaces.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](std::size_t value) {
        std::size_t root = value;
        while (parent[root] != root) root = parent[root];
        while (parent[value] != value) {
            const std::size_t next = parent[value];
            parent[value] = root;
            value = next;
        }
        return root;
    };
    auto unite = [&](std::size_t lhs, std::size_t rhs) {
        lhs = find(lhs);
        rhs = find(rhs);
        if (lhs != rhs) parent[rhs] = lhs;
    };
    for (std::size_t i = 0; i < surfaces.size(); ++i) {
        for (std::size_t j = i + 1; j < surfaces.size(); ++j) {
            if (Family(surfaces[i].tags) != Family(surfaces[j].tags)) continue;
            if (!NearOrOverlap(surfaces[i].bounds.min_x, surfaces[i].bounds.max_x,
                               surfaces[j].bounds.min_x, surfaces[j].bounds.max_x, options.merge_xy_gap) ||
                !NearOrOverlap(surfaces[i].bounds.min_y, surfaces[i].bounds.max_y,
                               surfaces[j].bounds.min_y, surfaces[j].bounds.max_y, options.merge_xy_gap) ||
                !NearOrOverlap(surfaces[i].min_z, surfaces[i].max_z,
                               surfaces[j].min_z, surfaces[j].max_z, options.merge_z_gap)) {
                continue;
            }
            unite(i, j);
        }
    }

    std::vector<Component> components;
    std::unordered_map<std::size_t, std::size_t> component_indices;
    for (std::size_t i = 0; i < surfaces.size(); ++i) {
        const std::size_t root = find(i);
        const auto component_it = component_indices.find(root);
        if (component_it == component_indices.end()) {
            Component component;
            component.members.push_back(i);
            component.bounds = surfaces[i].bounds;
            component.min_z = surfaces[i].min_z;
            component.max_z = surfaces[i].max_z;
            component.tags = surfaces[i].tags;
            component.supports_ground_movement = surfaces[i].supports_ground_movement;
            component.family = Family(surfaces[i].tags);
            component.min_source_id = surfaces[i].source_id;
            components.push_back(std::move(component));
            component_indices.emplace(root, components.size() - 1);
        } else {
            auto& component = components[component_it->second];
            component.members.push_back(i);
            component.bounds.min_x = std::min(component.bounds.min_x, surfaces[i].bounds.min_x);
            component.bounds.min_y = std::min(component.bounds.min_y, surfaces[i].bounds.min_y);
            component.bounds.max_x = std::max(component.bounds.max_x, surfaces[i].bounds.max_x);
            component.bounds.max_y = std::max(component.bounds.max_y, surfaces[i].bounds.max_y);
            component.min_z = std::min(component.min_z, surfaces[i].min_z);
            component.max_z = std::max(component.max_z, surfaces[i].max_z);
            component.tags |= surfaces[i].tags;
            component.supports_ground_movement = component.supports_ground_movement || surfaces[i].supports_ground_movement;
            component.min_source_id = std::min(component.min_source_id, surfaces[i].source_id);
        }
    }
    std::sort(components.begin(), components.end(), [](const Component& lhs, const Component& rhs) {
        if (lhs.min_z != rhs.min_z) return lhs.min_z < rhs.min_z;
        return lhs.min_source_id < rhs.min_source_id;
    });

    output.volumes.reserve(components.size());
    for (std::size_t i = 0; i < components.size(); ++i) {
        LayerVolume volume;
        volume.id = static_cast<VolumeId>(i + 1);
        volume.layer_id = static_cast<LayerId>(i + 1);
        volume.name = "generated_" + std::to_string(volume.id);
        volume.bounds = components[i].bounds;
        volume.min_z = components[i].min_z;
        volume.max_z = components[i].max_z;
        volume.kind = KindFor(components[i].tags);
        volume.supports_ground_movement = components[i].supports_ground_movement;
        volume.tags = components[i].tags;
        output.volumes.push_back(std::move(volume));
        report.surfaces_merged += components[i].members.size() > 1 ? components[i].members.size() - 1 : 0;
    }
    report.volumes_generated = output.volumes.size();
    std::string validation_error;
    if (!output.Validate(options.world_bounds, validation_error)) {
        report.errors.push_back("generated world failed validation: " + validation_error);
        output = LayeredWorld{};
        return false;
    }
    return true;
}

bool ReadLayeredSurfaceSource(const std::filesystem::path& path,
                              std::vector<LayerSourceSurface>& surfaces,
                              std::vector<std::string>& errors)
{
    surfaces.clear();
    errors.clear();
    std::ifstream input(path);
    if (!input) {
        errors.push_back("cannot open source file " + path.string());
        return false;
    }
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        std::istringstream row(line.substr(first));
        std::string type;
        LayerSourceSurface surface;
        std::string tags;
        std::string movement;
        if (!(row >> type >> surface.source_id >> surface.name >> surface.bounds.min_x >> surface.bounds.min_y >>
              surface.bounds.max_x >> surface.bounds.max_y >> surface.min_z >> surface.max_z >> tags >> movement) ||
            type != "surface") {
            errors.push_back("line " + std::to_string(line_number) + ": expected surface record");
            continue;
        }
        std::string trailing;
        if (row >> trailing || !ParseTags(tags, surface.tags) ||
            (movement != "0" && movement != "1" && movement != "true" && movement != "false")) {
            errors.push_back("line " + std::to_string(line_number) + ": invalid tags, movement or trailing fields");
            continue;
        }
        surface.supports_ground_movement = movement == "1" || movement == "true";
        surfaces.push_back(std::move(surface));
    }
    return errors.empty();
}

} // namespace mx::map
