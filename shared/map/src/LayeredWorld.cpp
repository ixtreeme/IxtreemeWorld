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

std::size_t BitBytes(std::uint64_t bits)
{
    return static_cast<std::size_t>((bits + 7) / 8);
}

bool BitsetShapeValid(const std::vector<std::uint8_t>& bits, std::uint64_t count)
{
    if (bits.size() != BitBytes(count)) return false;
    const unsigned used = static_cast<unsigned>(count % 8);
    // Padding bits of the last byte must be zero: one canonical encoding.
    return used == 0 || bits.empty() || (bits.back() >> used) == 0;
}

bool BitSet(const std::vector<std::uint8_t>& bits, std::uint64_t index)
{
    const auto byte = static_cast<std::size_t>(index / 8);
    return byte >= bits.size() || ((bits[byte] >> (index % 8)) & 1u) != 0; // out of range = blocked
}

bool ProofGeometryValid(const LayerPortal& portal, const LayerVolume& source, const LayerVolume& target,
                        const LayerClearanceProfile& profile, std::string& why)
{
    const auto& proof = *portal.proof;
    if (proof.axis > 1) { why = "axis"; return false; }
    // The shared edge: one volume's maximum equals the other's minimum.
    const float s_min = proof.axis == 0 ? source.bounds.min_x : source.bounds.min_y;
    const float s_max = proof.axis == 0 ? source.bounds.max_x : source.bounds.max_y;
    const float t_min = proof.axis == 0 ? target.bounds.min_x : target.bounds.min_y;
    const float t_max = proof.axis == 0 ? target.bounds.max_x : target.bounds.max_y;
    if (!((s_max == proof.edge && t_min == proof.edge) || (s_min == proof.edge && t_max == proof.edge))) {
        why = "edge is not an exactly shared volume boundary";
        return false;
    }
    const float so_min = proof.axis == 0 ? source.bounds.min_y : source.bounds.min_x;
    const float so_max = proof.axis == 0 ? source.bounds.max_y : source.bounds.max_x;
    const float to_min = proof.axis == 0 ? target.bounds.min_y : target.bounds.min_x;
    const float to_max = proof.axis == 0 ? target.bounds.max_y : target.bounds.max_x;
    if (proof.span_min != std::max(so_min, to_min) || proof.span_max != std::min(so_max, to_max) ||
        !(proof.span_min < proof.span_max)) {
        why = "span is not the exact shared segment";
        return false;
    }
    double across0 = 0.0, across1 = 0.0;
    LayerPortalCorridorAcross(proof, source, target, profile, across0, across1);
    const std::uint64_t cells = static_cast<std::uint64_t>(proof.slots) * proof.across;
    if (proof.slots != LayerClearanceCellCount(proof.span_min, proof.span_max, profile.cell_size_m) ||
        proof.across != LayerClearanceCellCount(across0, across1, profile.cell_size_m) || cells == 0 ||
        cells > kMaxLayerClearanceCellsPerVolume || !BitsetShapeValid(proof.blocked, cells)) {
        why = "corridor grid does not match the shared segment and band";
        return false;
    }
    if (!std::isfinite(proof.max_step_m) || proof.max_step_m < 0.0 ||
        proof.max_step_m > static_cast<double>(profile.step_height_m)) {
        why = "step exceeds the profile";
        return false;
    }
    // The planes are linear: the difference is extreme at the segment ends.
    double measured = 0.0;
    for (const double along : {static_cast<double>(proof.span_min), static_cast<double>(proof.span_max)}) {
        const double x = proof.axis == 0 ? static_cast<double>(proof.edge) : along;
        const double y = proof.axis == 0 ? along : static_cast<double>(proof.edge);
        measured = std::max(measured,
            std::abs(source.ground_support->Height(x, y) - target.ground_support->Height(x, y)));
    }
    if (!std::isfinite(measured) || std::abs(measured - proof.max_step_m) > kLayerPortalStepToleranceMeters) {
        why = "stored step does not match the support planes";
        return false;
    }
    return true;
}

} // namespace

bool LayerClearanceProfile::Valid() const noexcept
{
    return std::isfinite(cell_size_m) && cell_size_m >= 0.05f && cell_size_m <= 4.0f &&
           std::isfinite(actor_radius_m) && actor_radius_m > 0.0f && actor_radius_m <= 10.0f &&
           std::isfinite(actor_height_m) && actor_height_m >= 2.0f * actor_radius_m && actor_height_m <= 20.0f &&
           std::isfinite(step_height_m) && step_height_m >= 0.0f && step_height_m < actor_height_m &&
           std::isfinite(floor_contact_m) && floor_contact_m >= 0.0f && floor_contact_m < actor_height_m;
}

bool LayerClearanceGrid::Blocked(std::uint32_t x, std::uint32_t y) const noexcept
{
    if (x >= cells_x || y >= cells_y) return true;
    return BitSet(blocked, static_cast<std::uint64_t>(y) * cells_x + x);
}

std::uint64_t LayerClearanceGrid::BlockedCount() const noexcept
{
    std::uint64_t count = 0;
    for (const auto byte : blocked) {
        for (unsigned bit = 0; bit < 8; ++bit) count += (byte >> bit) & 1u;
    }
    return count;
}

bool LayerPortalProof::CellBlocked(std::uint32_t slot, std::uint32_t across_cell) const noexcept
{
    if (slot >= slots || across_cell >= across) return true;
    return BitSet(blocked, static_cast<std::uint64_t>(slot) * across + across_cell);
}

void LayerPortalCorridorAcross(const LayerPortalProof& proof,
                               const LayerVolume& a,
                               const LayerVolume& b,
                               const LayerClearanceProfile& profile,
                               double& across0,
                               double& across1) noexcept
{
    const double reach = static_cast<double>(profile.actor_radius_m) + static_cast<double>(profile.cell_size_m);
    const double edge = proof.edge;
    across0 = edge - reach;
    across1 = edge + reach;
    for (const LayerVolume* volume : {&a, &b}) {
        const double min = proof.axis == 0 ? volume->bounds.min_x : volume->bounds.min_y;
        const double max = proof.axis == 0 ? volume->bounds.max_x : volume->bounds.max_y;
        if (max <= edge) across0 = std::max(across0, min); // volume on the low side
        if (min >= edge) across1 = std::min(across1, max); // volume on the high side
    }
}

std::uint32_t LayerClearanceCellCount(double min, double max, float cell_size) noexcept
{
    const double length = max - min;
    const double cell = static_cast<double>(cell_size);
    if (!std::isfinite(length) || !std::isfinite(cell) || !(length > 0.0) || !(cell > 0.0)) return 0;
    const double cells = std::ceil(length / cell);
    if (!(cells >= 1.0) || cells > 4294967295.0) return 0;
    return static_cast<std::uint32_t>(cells);
}

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
        if (!portals.empty() || clearance_profile) {
            error = "layered-world portals and clearance require at least one volume";
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

    // ---- 3D-4B clearance and transition proofs ----
    const bool any_proof =
        std::any_of(volumes.begin(), volumes.end(), [](const LayerVolume& v) { return v.clearance.has_value(); }) ||
        std::any_of(portals.begin(), portals.end(), [](const LayerPortal& p) { return p.proof.has_value(); });
    if (clearance_profile.has_value() != any_proof) {
        error = clearance_profile ? "layered-world clearance profile has no clearance data"
                                  : "layered-world clearance data requires a clearance profile";
        return false;
    }
    if (!any_proof) {
        return true;
    }
    const auto& profile = *clearance_profile;
    if (!profile.Valid()) {
        error = "layered-world clearance profile is invalid";
        return false;
    }
    std::uint64_t total_cells = 0;
    for (const auto& volume : volumes) {
        if (!volume.clearance) continue;
        const auto& grid = *volume.clearance;
        if (!volume.ground_support || !volume.HasValidGroundSupport()) {
            error = "layered-world volume " + std::to_string(volume.id) + " has clearance without ground support";
            return false;
        }
        const auto cells_x = LayerClearanceCellCount(volume.bounds.min_x, volume.bounds.max_x, profile.cell_size_m);
        const auto cells_y = LayerClearanceCellCount(volume.bounds.min_y, volume.bounds.max_y, profile.cell_size_m);
        const std::uint64_t cells = static_cast<std::uint64_t>(grid.cells_x) * grid.cells_y;
        if (cells_x == 0 || cells_y == 0 || grid.cells_x != cells_x || grid.cells_y != cells_y ||
            cells > kMaxLayerClearanceCellsPerVolume || !BitsetShapeValid(grid.blocked, cells)) {
            error = "layered-world volume " + std::to_string(volume.id) + " clearance grid does not match its footprint";
            return false;
        }
        total_cells += cells;
        if (total_cells > kMaxLayerClearanceTotalCells) {
            error = "layered-world clearance grids exceed the total cell limit";
            return false;
        }
    }
    for (const auto& portal : portals) {
        if (!portal.proof) continue;
        const auto* source = FindById(volumes, portal.source_volume);
        const auto* target = FindById(volumes, portal.target_volume);
        std::string why;
        if (source == nullptr || target == nullptr || !source->clearance || !target->clearance ||
            !source->AllowsGroundMovement() || !target->AllowsGroundMovement()) {
            why = "both endpoints need walkable support with clearance";
        }
        if (!why.empty() || !ProofGeometryValid(portal, *source, *target, profile, why)) {
            error = "layered-world portal " + std::to_string(portal.id) + " transition proof is invalid: " + why;
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
        if (volume.clearance &&
            (!world.clearance_profile ||
             !BitsetShapeValid(volume.clearance->blocked,
                               static_cast<std::uint64_t>(volume.clearance->cells_x) * volume.clearance->cells_y))) {
            return {};
        }
    }
    for (const auto& portal : world.portals) {
        if (portal.proof && (!world.clearance_profile || portal.proof->axis > 1 ||
                             !BitsetShapeValid(portal.proof->blocked,
                                              static_cast<std::uint64_t>(portal.proof->slots) * portal.proof->across))) {
            return {};
        }
    }
    // Version 4 only when the world carries 3D-4B proofs; otherwise the exact
    // version-3 layout, so support-only worlds keep their previous bytes.
    const bool v4 = world.clearance_profile.has_value();
    if (v4 && !world.clearance_profile->Valid()) {
        return {};
    }

    std::vector<std::uint8_t> out;
    out.reserve(16 + world.volumes.size() * 40 + world.portals.size() * 60);
    PutU32(out, kLayeredWorldFileMagic);
    PutU32(out, v4 ? kLayeredWorldFileVersion : kLayeredWorldSupportFileVersion);
    PutU32(out, static_cast<std::uint32_t>(world.volumes.size()));
    PutU32(out, static_cast<std::uint32_t>(world.portals.size()));
    if (v4) {
        const auto& profile = *world.clearance_profile;
        PutF32(out, profile.cell_size_m);
        PutF32(out, profile.actor_radius_m);
        PutF32(out, profile.actor_height_m);
        PutF32(out, profile.step_height_m);
        PutF32(out, profile.floor_contact_m);
    }
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
        if (v4) {
            PutU8(out, volume.clearance ? 1 : 0);
            PutU8(out, 0);
            PutU8(out, 0);
            PutU8(out, 0);
            if (volume.clearance) {
                PutU32(out, volume.clearance->cells_x);
                PutU32(out, volume.clearance->cells_y);
                out.insert(out.end(), volume.clearance->blocked.begin(), volume.clearance->blocked.end());
            }
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
        if (v4) {
            PutU8(out, portal.proof ? 1 : 0);
            PutU8(out, portal.proof ? portal.proof->axis : 0);
            PutU8(out, 0);
            PutU8(out, 0);
            if (portal.proof) {
                const auto& proof = *portal.proof;
                PutF32(out, proof.edge);
                PutF32(out, proof.span_min);
                PutF32(out, proof.span_max);
                PutF64(out, proof.max_step_m);
                PutU32(out, proof.slots);
                PutU32(out, proof.across);
                out.insert(out.end(), proof.blocked.begin(), proof.blocked.end());
            }
        }
    }
    if (out.size() > kMaxLayeredWorldFileBytes) {
        return {};
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
    if (version >= 4) {
        LayerClearanceProfile profile;
        if (!cursor.F32(profile.cell_size_m) || !cursor.F32(profile.actor_radius_m) ||
            !cursor.F32(profile.actor_height_m) || !cursor.F32(profile.step_height_m) ||
            !cursor.F32(profile.floor_contact_m)) {
            error = "layered-world clearance profile is truncated";
            return false;
        }
        if (!profile.Valid()) {
            error = "layered-world clearance profile is invalid";
            return false;
        }
        decoded.clearance_profile = profile;
    }
    const std::uint64_t minimum_volume_bytes = version >= 4 ? 48 : (version >= 3 ? 44 : (version >= 2 ? 40 : 36));
    const std::uint64_t minimum_portal_bytes = version >= 4 ? 68 : 64;
    if (minimum_volume_bytes * volume_count + minimum_portal_bytes * portal_count > cursor.Remaining()) {
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
        if (version >= 4) {
            std::uint8_t has_clearance = 0, reserved0 = 0, reserved1 = 0, reserved2 = 0;
            if (!cursor.U8(has_clearance) || !cursor.U8(reserved0) || !cursor.U8(reserved1) ||
                !cursor.U8(reserved2)) {
                error = "layered-world clearance header " + std::to_string(i) + " is truncated";
                return false;
            }
            if (has_clearance > 1 || reserved0 != 0 || reserved1 != 0 || reserved2 != 0) {
                error = "layered-world clearance header " + std::to_string(i) + " has unsupported flags";
                return false;
            }
            if (has_clearance != 0) {
                LayerClearanceGrid grid;
                if (!cursor.U32(grid.cells_x) || !cursor.U32(grid.cells_y)) {
                    error = "layered-world clearance grid " + std::to_string(i) + " is truncated";
                    return false;
                }
                const std::uint64_t cells = static_cast<std::uint64_t>(grid.cells_x) * grid.cells_y;
                if (cells == 0 || cells > kMaxLayerClearanceCellsPerVolume || BitBytes(cells) > cursor.Remaining()) {
                    error = "layered-world clearance grid " + std::to_string(i) + " has an invalid size";
                    return false;
                }
                std::string raw;
                cursor.Text(BitBytes(cells), raw);
                grid.blocked.assign(raw.begin(), raw.end());
                volume.clearance = std::move(grid);
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
        if (version >= 4) {
            std::uint8_t has_proof = 0, axis = 0, proof_reserved0 = 0, proof_reserved1 = 0;
            if (!cursor.U8(has_proof) || !cursor.U8(axis) || !cursor.U8(proof_reserved0) ||
                !cursor.U8(proof_reserved1)) {
                error = "layered-world portal proof header " + std::to_string(i) + " is truncated";
                return false;
            }
            if (has_proof > 1 || axis > 1 || (has_proof == 0 && axis != 0) || proof_reserved0 != 0 ||
                proof_reserved1 != 0) {
                error = "layered-world portal proof header " + std::to_string(i) + " has unsupported flags";
                return false;
            }
            if (has_proof != 0) {
                LayerPortalProof proof;
                proof.axis = axis;
                if (!cursor.F32(proof.edge) || !cursor.F32(proof.span_min) || !cursor.F32(proof.span_max) ||
                    !cursor.F64(proof.max_step_m) || !cursor.U32(proof.slots) || !cursor.U32(proof.across)) {
                    error = "layered-world portal proof " + std::to_string(i) + " is truncated";
                    return false;
                }
                const std::uint64_t corridor_cells = static_cast<std::uint64_t>(proof.slots) * proof.across;
                if (corridor_cells == 0 || corridor_cells > kMaxLayerClearanceCellsPerVolume ||
                    BitBytes(corridor_cells) > cursor.Remaining()) {
                    error = "layered-world portal proof " + std::to_string(i) + " has an invalid size";
                    return false;
                }
                std::string raw;
                cursor.Text(BitBytes(corridor_cells), raw);
                proof.blocked.assign(raw.begin(), raw.end());
                portal.proof = std::move(proof);
            }
        }
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
