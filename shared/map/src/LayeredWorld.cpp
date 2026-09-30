#include "map/LayeredWorld.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_set>

namespace mx::map {
namespace {

bool Finite(float value)
{
    return std::isfinite(value);
}

bool ValidRect(const Rect& rect)
{
    return Finite(rect.min_x) && Finite(rect.min_y) && Finite(rect.max_x) && Finite(rect.max_y) &&
           rect.min_x < rect.max_x && rect.min_y < rect.max_y;
}

bool OpenIntervalOverlap(float lhs_min, float lhs_max, float rhs_min, float rhs_max)
{
    return lhs_min < rhs_max && rhs_min < lhs_max;
}

bool AabbOverlap(const LayerVolume& lhs, const LayerVolume& rhs)
{
    return OpenIntervalOverlap(lhs.bounds.min_x, lhs.bounds.max_x, rhs.bounds.min_x, rhs.bounds.max_x) &&
           OpenIntervalOverlap(lhs.bounds.min_y, lhs.bounds.max_y, rhs.bounds.min_y, rhs.bounds.max_y) &&
           OpenIntervalOverlap(lhs.min_z, lhs.max_z, rhs.min_z, rhs.max_z);
}

bool InsideRect(const Rect& inner, const Rect& outer)
{
    return inner.min_x >= outer.min_x && inner.max_x <= outer.max_x && inner.min_y >= outer.min_y &&
           inner.max_y <= outer.max_y;
}

const LayerVolume* FindById(const std::vector<LayerVolume>& volumes, VolumeId id)
{
    const auto it = std::find_if(volumes.begin(), volumes.end(), [id](const LayerVolume& volume) {
        return volume.id == id;
    });
    return it == volumes.end() ? nullptr : &*it;
}

} // namespace

const char* ToString(VolumeKind kind) noexcept
{
    switch (kind) {
    case VolumeKind::Ground:
        return "ground";
    case VolumeKind::Interior:
        return "interior";
    case VolumeKind::WaterSurface:
        return "water_surface";
    case VolumeKind::Underwater:
        return "underwater";
    case VolumeKind::Connector:
        return "connector";
    }
    return "unknown";
}

bool LayerVolume::Contains(float x, float y, float z) const noexcept
{
    return bounds.ContainsHalfOpen(x, y) && z >= min_z && z < max_z;
}

bool LayerPortal::SourceContains(float x, float y, float z) const noexcept
{
    return source_bounds.ContainsHalfOpen(x, y) && z >= source_min_z && z < source_max_z;
}

bool LayeredWorld::Validate(const Rect& world_bounds, std::string& error) const
{
    error.clear();
    if (!ValidRect(world_bounds)) {
        error = "layered-world world bounds are invalid";
        return false;
    }
    if (volumes.empty()) {
        if (!portals.empty()) {
            error = "layered-world portals require at least one volume";
            return false;
        }
        return true;
    }

    std::unordered_set<VolumeId> volume_ids;
    for (const auto& volume : volumes) {
        if (volume.id == 0) {
            error = "layered-world volume id must be non-zero";
            return false;
        }
        if (!volume_ids.insert(volume.id).second) {
            error = "layered-world duplicate volume id " + std::to_string(volume.id);
            return false;
        }
        if (!ValidRect(volume.bounds) || !InsideRect(volume.bounds, world_bounds) || !Finite(volume.min_z) ||
            !Finite(volume.max_z) || !(volume.min_z < volume.max_z)) {
            error = "layered-world volume " + std::to_string(volume.id) + " has invalid bounds";
            return false;
        }
    }
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        for (std::size_t j = i + 1; j < volumes.size(); ++j) {
            if (AabbOverlap(volumes[i], volumes[j])) {
                error = "layered-world volumes " + std::to_string(volumes[i].id) + " and " +
                        std::to_string(volumes[j].id) + " overlap in 3D";
                return false;
            }
        }
    }

    std::unordered_set<std::uint32_t> portal_ids;
    for (const auto& portal : portals) {
        if (portal.id == 0 || !portal_ids.insert(portal.id).second) {
            error = "layered-world portal id must be non-zero and unique";
            return false;
        }
        const auto* source = FindById(volumes, portal.source_volume);
        const auto* target = FindById(volumes, portal.target_volume);
        if (source == nullptr || target == nullptr || source == target) {
            error = "layered-world portal " + std::to_string(portal.id) + " references invalid volumes";
            return false;
        }
        if (!ValidRect(portal.source_bounds) || !ValidRect(portal.target_bounds) ||
            !InsideRect(portal.source_bounds, source->bounds) || !InsideRect(portal.target_bounds, target->bounds) ||
            !Finite(portal.source_min_z) || !Finite(portal.source_max_z) || !Finite(portal.target_min_z) ||
            !Finite(portal.target_max_z) || !(portal.source_min_z < portal.source_max_z) ||
            !(portal.target_min_z < portal.target_max_z) || portal.source_min_z < source->min_z ||
            portal.source_max_z > source->max_z || portal.target_min_z < target->min_z ||
            portal.target_max_z > target->max_z) {
            error = "layered-world portal " + std::to_string(portal.id) + " has invalid endpoint bounds";
            return false;
        }
    }
    return true;
}

std::optional<VolumeId> LayeredWorld::FindVolume(float x, float y, float z) const noexcept
{
    std::optional<VolumeId> result;
    for (const auto& volume : volumes) {
        if (!volume.Contains(x, y, z)) {
            continue;
        }
        if (result.has_value()) {
            return std::nullopt; // caller used an unvalidated/ambiguous contract
        }
        result = volume.id;
    }
    return result;
}

bool LayeredWorld::CanTraverse(const LayerPortal& portal,
                               float source_x,
                               float source_y,
                               float source_z,
                               float target_x,
                               float target_y,
                               float target_z) const noexcept
{
    const auto* source = FindById(volumes, portal.source_volume);
    const auto* target = FindById(volumes, portal.target_volume);
    return source != nullptr && target != nullptr && portal.SourceContains(source_x, source_y, source_z) &&
           target_z >= portal.target_min_z && target_z < portal.target_max_z &&
           target->Contains(target_x, target_y, target_z) && portal.target_bounds.ContainsHalfOpen(target_x, target_y);
}

} // namespace mx::map
