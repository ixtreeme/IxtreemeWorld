#include "map/LayeredWorld.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
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

double LayerSupportPlane::Height(double x, double y) const noexcept
{
    return anchor_z + slope_x * (x - anchor_x) + slope_y * (y - anchor_y);
}

bool LayerVolume::AllowsGroundMovement() const noexcept
{
    return supports_ground_movement &&
           static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(VolumeKind::Connector) &&
           kind != VolumeKind::WaterSurface && kind != VolumeKind::Underwater &&
           (tags & ~kKnownVolumeTags) == 0 && !HasVolumeTag(tags, VolumeTagWater) &&
           !HasVolumeTag(tags, VolumeTagUnderwater);
}

bool LayerVolume::HasValidGroundSupport() const noexcept
{
    if (!ground_support || !AllowsGroundMovement() || id == 0 || layer_id == kLegacyLayerId ||
        !ValidRect(bounds) || !Finite(min_z) || !Finite(max_z) || !(min_z < max_z)) {
        return false;
    }
    const auto& plane = *ground_support;
    if (plane.source_id == 0 || plane.component_id == 0 ||
        !std::isfinite(plane.anchor_x) || !std::isfinite(plane.anchor_y) ||
        !std::isfinite(plane.anchor_z) || !std::isfinite(plane.slope_x) ||
        !std::isfinite(plane.slope_y) || !std::isfinite(plane.max_height_error_m) ||
        plane.max_height_error_m < 0.0) {
        return false;
    }
    // Cooker anchors are actual component vertices. Closed XY bounds are
    // intentional: boundary vertices may lie on the half-open footprint's
    // maximum edge. Remote anchors can hide huge finite cancellation in the
    // corner evaluation and must never be accepted as surface proof.
    if (plane.anchor_x < static_cast<double>(bounds.min_x) ||
        plane.anchor_x > static_cast<double>(bounds.max_x) ||
        plane.anchor_y < static_cast<double>(bounds.min_y) ||
        plane.anchor_y > static_cast<double>(bounds.max_y) ||
        !std::isfinite(plane.anchor_z - plane.max_height_error_m) ||
        !std::isfinite(plane.anchor_z + plane.max_height_error_m) ||
        plane.anchor_z - plane.max_height_error_m < static_cast<double>(min_z) ||
        plane.anchor_z + plane.max_height_error_m >= static_cast<double>(max_z)) {
        return false;
    }
    const double vertical_error_limit = kLayerSupportCoplanarToleranceMeters *
        std::hypot(1.0, std::hypot(plane.slope_x, plane.slope_y));
    if (!std::isfinite(vertical_error_limit) || plane.max_height_error_m > vertical_error_limit) {
        return false;
    }
    for (const double x : {static_cast<double>(bounds.min_x), static_cast<double>(bounds.max_x)}) {
        for (const double y : {static_cast<double>(bounds.min_y), static_cast<double>(bounds.max_y)}) {
            const double height = plane.Height(x, y);
            const double lower = height - plane.max_height_error_m;
            const double upper = height + plane.max_height_error_m;
            if (!std::isfinite(height) || !std::isfinite(lower) || !std::isfinite(upper) ||
                lower < static_cast<double>(min_z) || upper >= static_cast<double>(max_z)) {
                return false;
            }
        }
    }
    return true;
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
    if (volumes.size() > kMaxLayeredWorldVolumes || portals.size() > kMaxLayeredWorldPortals) {
        error = "layered-world record count exceeds the limit";
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
        if (static_cast<std::uint8_t>(volume.kind) > static_cast<std::uint8_t>(VolumeKind::Connector) ||
            (volume.tags & ~kKnownVolumeTags) != 0 || !ValidRect(volume.bounds) ||
            !InsideRect(volume.bounds, world_bounds) || !Finite(volume.min_z) ||
            !Finite(volume.max_z) || !(volume.min_z < volume.max_z)) {
            error = "layered-world volume " + std::to_string(volume.id) + " has invalid bounds";
            return false;
        }
        if (volume.ground_support && !volume.HasValidGroundSupport()) {
            error = "layered-world volume " + std::to_string(volume.id) + " has invalid ground support";
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

namespace {

void PutU8(std::vector<std::uint8_t>& out, std::uint8_t value)
{
    out.push_back(value);
}

void PutU16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
}

void PutU64(std::vector<std::uint8_t>& out, std::uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
}

void PutF64(std::vector<std::uint8_t>& out, double value)
{
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    PutU64(out, bits);
}

void PutF32(std::vector<std::uint8_t>& out, float value)
{
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    PutU32(out, bits);
}

void PutRect(std::vector<std::uint8_t>& out, const Rect& rect)
{
    PutF32(out, rect.min_x);
    PutF32(out, rect.min_y);
    PutF32(out, rect.max_x);
    PutF32(out, rect.max_y);
}

class ByteCursor {
public:
    explicit ByteCursor(const std::vector<std::uint8_t>& bytes)
        : bytes_(bytes)
    {
    }

    std::size_t Offset() const noexcept
    {
        return offset_;
    }

    std::size_t Remaining() const noexcept
    {
        return bytes_.size() - offset_;
    }

    bool U8(std::uint8_t& out) noexcept
    {
        if (Remaining() < 1) {
            return false;
        }
        out = bytes_[offset_++];
        return true;
    }

    bool U16(std::uint16_t& out) noexcept
    {
        if (Remaining() < 2) {
            return false;
        }
        out = static_cast<std::uint16_t>(bytes_[offset_]) |
              static_cast<std::uint16_t>(bytes_[offset_ + 1] << 8);
        offset_ += 2;
        return true;
    }

    bool U32(std::uint32_t& out) noexcept
    {
        if (Remaining() < 4) {
            return false;
        }
        out = static_cast<std::uint32_t>(bytes_[offset_]) |
              (static_cast<std::uint32_t>(bytes_[offset_ + 1]) << 8) |
              (static_cast<std::uint32_t>(bytes_[offset_ + 2]) << 16) |
              (static_cast<std::uint32_t>(bytes_[offset_ + 3]) << 24);
        offset_ += 4;
        return true;
    }

    bool F32(float& out) noexcept
    {
        std::uint32_t bits = 0;
        if (!U32(bits)) {
            return false;
        }
        static_assert(sizeof(bits) == sizeof(out));
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }

    bool U64(std::uint64_t& out) noexcept
    {
        if (Remaining() < 8) return false;
        out = 0;
        for (int shift = 0; shift < 64; shift += 8) {
            out |= static_cast<std::uint64_t>(bytes_[offset_++]) << shift;
        }
        return true;
    }

    bool F64(double& out) noexcept
    {
        std::uint64_t bits = 0;
        if (!U64(bits)) return false;
        static_assert(sizeof(bits) == sizeof(out));
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }

    bool Text(std::size_t length, std::string& out)
    {
        if (Remaining() < length) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), length);
        offset_ += length;
        return true;
    }

private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t offset_ = 0;
};

bool ReadRect(ByteCursor& cursor, Rect& rect)
{
    return cursor.F32(rect.min_x) && cursor.F32(rect.min_y) && cursor.F32(rect.max_x) && cursor.F32(rect.max_y);
}

} // namespace

std::vector<std::uint8_t> EncodeLayeredWorld(const LayeredWorld& world)
{
    if (world.volumes.size() > kMaxLayeredWorldVolumes || world.portals.size() > kMaxLayeredWorldPortals) {
        return {};
    }
    for (const auto& volume : world.volumes) {
        if (volume.name.size() > kMaxLayeredWorldNameBytes ||
            (volume.ground_support && !volume.HasValidGroundSupport())) {
            return {};
        }
    }

    std::vector<std::uint8_t> out;
    out.reserve(16 + world.volumes.size() * 40 + world.portals.size() * 60);
    PutU32(out, kLayeredWorldFileMagic);
    PutU32(out, kLayeredWorldFileVersion);
    PutU32(out, static_cast<std::uint32_t>(world.volumes.size()));
    PutU32(out, static_cast<std::uint32_t>(world.portals.size()));
    for (const auto& volume : world.volumes) {
        PutU32(out, volume.id);
        PutU32(out, volume.layer_id);
        PutU32(out, volume.tags);
        PutU8(out, static_cast<std::uint8_t>(volume.kind));
        PutU8(out, volume.supports_ground_movement ? 1 : 0);
        PutU16(out, static_cast<std::uint16_t>(volume.name.size()));
        PutRect(out, volume.bounds);
        PutF32(out, volume.min_z);
        PutF32(out, volume.max_z);
        out.insert(out.end(), volume.name.begin(), volume.name.end());
        PutU8(out, volume.ground_support ? 1 : 0);
        PutU8(out, 0);
        PutU8(out, 0);
        PutU8(out, 0);
        if (volume.ground_support) {
            const auto& plane = *volume.ground_support;
            PutU32(out, plane.source_id);
            PutU32(out, plane.component_id);
            PutF64(out, plane.anchor_x);
            PutF64(out, plane.anchor_y);
            PutF64(out, plane.anchor_z);
            PutF64(out, plane.slope_x);
            PutF64(out, plane.slope_y);
            PutF64(out, plane.max_height_error_m);
        }
    }
    for (const auto& portal : world.portals) {
        PutU32(out, portal.id);
        PutU32(out, portal.source_volume);
        PutU32(out, portal.target_volume);
        PutRect(out, portal.source_bounds);
        PutRect(out, portal.target_bounds);
        PutF32(out, portal.source_min_z);
        PutF32(out, portal.source_max_z);
        PutF32(out, portal.target_min_z);
        PutF32(out, portal.target_max_z);
        PutU8(out, portal.bidirectional ? 1 : 0);
        PutU8(out, 0);
        PutU8(out, 0);
        PutU8(out, 0);
    }
    return out;
}

bool DecodeLayeredWorld(const std::vector<std::uint8_t>& bytes, LayeredWorld& world, std::string& error)
{
    error.clear();
    world = LayeredWorld{};
    LayeredWorld decoded;
    if (bytes.size() > kMaxLayeredWorldFileBytes) {
        error = "layered-world sidecar exceeds the size limit";
        return false;
    }
    ByteCursor cursor(bytes);
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t volume_count = 0;
    std::uint32_t portal_count = 0;
    if (!cursor.U32(magic) || !cursor.U32(version) || !cursor.U32(volume_count) || !cursor.U32(portal_count)) {
        error = "layered-world sidecar header is truncated";
        return false;
    }
    if (magic != kLayeredWorldFileMagic) {
        error = "layered-world sidecar magic is invalid";
        return false;
    }
    if (version < kLayeredWorldFileMinVersion || version > kLayeredWorldFileVersion) {
        error = "layered-world sidecar version " + std::to_string(version) + " is unsupported";
        return false;
    }
    if (volume_count > kMaxLayeredWorldVolumes || portal_count > kMaxLayeredWorldPortals) {
        error = "layered-world sidecar record count exceeds the limit";
        return false;
    }
    const std::uint64_t minimum_volume_bytes = version >= 3 ? 44 : (version >= 2 ? 40 : 36);
    if (minimum_volume_bytes * volume_count + 64ull * portal_count > cursor.Remaining()) {
        error = "layered-world sidecar records are truncated";
        return false;
    }
    decoded.volumes.reserve(volume_count);
    decoded.portals.reserve(portal_count);
    for (std::uint32_t i = 0; i < volume_count; ++i) {
        LayerVolume volume;
        std::uint8_t raw_kind = 0;
        std::uint8_t movement = 0;
        std::uint16_t name_length = 0;
        if (!cursor.U32(volume.id) || !cursor.U32(volume.layer_id) ||
            (version >= 2 && !cursor.U32(volume.tags)) || !cursor.U8(raw_kind) ||
            !cursor.U8(movement) || !cursor.U16(name_length) || !ReadRect(cursor, volume.bounds) ||
            !cursor.F32(volume.min_z) || !cursor.F32(volume.max_z)) {
            error = "layered-world volume record " + std::to_string(i) + " is truncated";
            return false;
        }
        if (raw_kind > static_cast<std::uint8_t>(VolumeKind::Connector) || movement > 1 ||
            name_length > kMaxLayeredWorldNameBytes || (volume.tags & ~kKnownVolumeTags) != 0) {
            error = "layered-world volume record " + std::to_string(i) + " has an invalid field";
            return false;
        }
        volume.kind = static_cast<VolumeKind>(raw_kind);
        if (version == 1) {
            switch (volume.kind) {
            case VolumeKind::Ground: volume.tags = VolumeTagGround; break;
            case VolumeKind::Interior: volume.tags = VolumeTagInterior; break;
            case VolumeKind::WaterSurface: volume.tags = VolumeTagWater; break;
            case VolumeKind::Underwater: volume.tags = VolumeTagUnderwater; break;
            case VolumeKind::Connector: volume.tags = VolumeTagConnector; break;
            }
        }
        volume.supports_ground_movement = movement != 0;
        if (!cursor.Text(name_length, volume.name)) {
            error = "layered-world volume record " + std::to_string(i) + " name is truncated";
            return false;
        }
        if (version >= 3) {
            std::uint8_t support = 0, reserved0 = 0, reserved1 = 0, reserved2 = 0;
            if (!cursor.U8(support) || !cursor.U8(reserved0) || !cursor.U8(reserved1) ||
                !cursor.U8(reserved2)) {
                error = "layered-world support header " + std::to_string(i) + " is truncated";
                return false;
            }
            if (support > 1 || reserved0 != 0 || reserved1 != 0 || reserved2 != 0) {
                error = "layered-world support header " + std::to_string(i) + " has unsupported flags";
                return false;
            }
            if (support != 0) {
                LayerSupportPlane plane;
                if (!cursor.U32(plane.source_id) || !cursor.U32(plane.component_id) ||
                    !cursor.F64(plane.anchor_x) || !cursor.F64(plane.anchor_y) ||
                    !cursor.F64(plane.anchor_z) || !cursor.F64(plane.slope_x) ||
                    !cursor.F64(plane.slope_y) || !cursor.F64(plane.max_height_error_m)) {
                    error = "layered-world support record " + std::to_string(i) + " is truncated";
                    return false;
                }
                volume.ground_support = plane;
                if (!volume.HasValidGroundSupport()) {
                    error = "layered-world support record " + std::to_string(i) + " is invalid";
                    return false;
                }
            }
        }
        decoded.volumes.push_back(std::move(volume));
    }
    for (std::uint32_t i = 0; i < portal_count; ++i) {
        LayerPortal portal;
        std::uint8_t direction = 0;
        std::uint8_t reserved0 = 0, reserved1 = 0, reserved2 = 0;
        if (!cursor.U32(portal.id) || !cursor.U32(portal.source_volume) || !cursor.U32(portal.target_volume) ||
            !ReadRect(cursor, portal.source_bounds) || !ReadRect(cursor, portal.target_bounds) ||
            !cursor.F32(portal.source_min_z) || !cursor.F32(portal.source_max_z) ||
            !cursor.F32(portal.target_min_z) || !cursor.F32(portal.target_max_z) || !cursor.U8(direction) ||
            !cursor.U8(reserved0) || !cursor.U8(reserved1) || !cursor.U8(reserved2)) {
            error = "layered-world portal record " + std::to_string(i) + " is truncated";
            return false;
        }
        if (direction > 1 || (version >= 3 && (reserved0 != 0 || reserved1 != 0 || reserved2 != 0))) {
            error = "layered-world portal record " + std::to_string(i) + " has an invalid direction flag";
            return false;
        }
        portal.bidirectional = direction != 0;
        decoded.portals.push_back(std::move(portal));
    }
    if (cursor.Remaining() != 0) {
        error = "layered-world sidecar has trailing bytes at offset " + std::to_string(cursor.Offset());
        return false;
    }
    world = std::move(decoded);
    return true;
}

} // namespace mx::map
