#include "map/LayerGroundSupport.h"
#include "map/LayeredWorldGeometry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
using namespace mx::map;
int failures = 0;
int checks = 0;

void Check(const char* name, bool condition)
{
    ++checks;
    std::cout << "LAYER GROUND " << name << ": " << (condition ? "PASS" : "FAIL") << '\n';
    if (!condition) ++failures;
}

bool Near(double value, double expected, double error = 1e-12)
{
    return std::isfinite(value) && std::abs(value - expected) <= error;
}

std::uint64_t Bits(double value)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool SameState(const LayerGroundState& lhs, const LayerGroundState& rhs)
{
    return lhs.volume_id == rhs.volume_id && lhs.layer_id == rhs.layer_id &&
        Bits(lhs.x) == Bits(rhs.x) && Bits(lhs.y) == Bits(rhs.y) && Bits(lhs.z) == Bits(rhs.z);
}

void U8(std::vector<std::uint8_t>& bytes, std::uint8_t value) { bytes.push_back(value); }
void U32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (unsigned shift = 0; shift != 32; shift += 8) U8(bytes, static_cast<std::uint8_t>(value >> shift));
}
void F32(std::vector<std::uint8_t>& bytes, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    U32(bytes, bits);
}
void U32At(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (unsigned shift = 0; shift != 32; shift += 8) bytes.at(offset++) = static_cast<std::uint8_t>(value >> shift);
}
void F64At(std::vector<std::uint8_t>& bytes, std::size_t offset, double value)
{
    const auto bits = Bits(value);
    for (unsigned shift = 0; shift != 64; shift += 8) bytes.at(offset++) = static_cast<std::uint8_t>(bits >> shift);
}

// Independent old-layout fixture encoder: deliberately cannot serialize a
// support plane. Do not obtain old-format controls by patching a v3 header.
std::vector<std::uint8_t> LegacyBytes(const LayeredWorld& world, std::uint32_t version)
{
    std::vector<std::uint8_t> bytes;
    U32(bytes, kLayeredWorldFileMagic); U32(bytes, version);
    U32(bytes, static_cast<std::uint32_t>(world.volumes.size())); U32(bytes, 0);
    for (const auto& volume : world.volumes) {
        U32(bytes, volume.id); U32(bytes, volume.layer_id);
        if (version >= 2) U32(bytes, volume.tags);
        U8(bytes, static_cast<std::uint8_t>(volume.kind)); U8(bytes, volume.supports_ground_movement ? 1 : 0);
        U8(bytes, static_cast<std::uint8_t>(volume.name.size())); U8(bytes, 0);
        F32(bytes, volume.bounds.min_x); F32(bytes, volume.bounds.min_y);
        F32(bytes, volume.bounds.max_x); F32(bytes, volume.bounds.max_y);
        F32(bytes, volume.min_z); F32(bytes, volume.max_z);
        bytes.insert(bytes.end(), volume.name.begin(), volume.name.end());
    }
    return bytes;
}

LayerCollisionMesh Mesh(std::uint32_t id, float intercept = 0, float slope_x = 0, float slope_y = 0)
{
    LayerCollisionMesh mesh;
    mesh.source_id = id;
    mesh.name = "support";
    mesh.tags = VolumeTagGround;
    for (const auto point : {std::array<float, 2>{-4,-2}, {4,-2}, {4,2}, {-4,2}}) {
        mesh.vertices.push_back({point[0], point[1], intercept + slope_x * point[0] + slope_y * point[1]});
    }
    mesh.indices = {0,1,2, 0,2,3};
    return mesh;
}

bool Bake(const std::vector<LayerCollisionMesh>& meshes, LayeredWorld& world,
          std::vector<LayerSourceSurface>* kept = nullptr)
{
    std::vector<LayerSourceSurface> sources;
    LayerGeometryReport extraction;
    LayerGenerationReport generation;
    LayerGenerationOptions options{Rect{-10,-10,10,10}};
    options.require_exact_footprints = true;
    if (!ExtractLayerSourceSurfaces(meshes, {}, sources, extraction)) return false;
    if (kept != nullptr) *kept = sources;
    return GenerateLayeredWorld(sources, options, world, generation);
}

bool RejectedBytes(const std::vector<std::uint8_t>& bytes)
{
    LayeredWorld output;
    output.volumes.push_back(LayerVolume{});
    std::string error;
    return !DecodeLayeredWorld(bytes, output, error) && !error.empty() &&
        output.volumes.empty() && output.portals.empty();
}

} // namespace

int main()
{
    using namespace mx::map;
    const Rect bounds{-10,-10,10,10};
    std::string error;
    LayeredWorld ascending, descending;
    Check("opposing-planes-cook", Bake({Mesh(41,0,.25f)}, ascending) && Bake({Mesh(41,0,-.25f)}, descending));
    if (ascending.volumes.empty() || descending.volumes.empty()) return 1;
    Check("same-v2-band-and-bytes", LegacyBytes(ascending,2) == LegacyBytes(descending,2));
    Check("v3-preserves-different-floor", EncodeLayeredWorld(ascending) != EncodeLayeredWorld(descending));
    const auto up = ResolveLayerGroundPlacement(ascending,1,.5,.5);
    const auto down = ResolveLayerGroundPlacement(descending,1,.5,.5);
    Check("independent-opposing-height-oracle", up.Ok() && down.Ok() && Near(up.state.z,.125) && Near(down.state.z,-.125));
    Check("independent-slope-oracle", Near(ascending.volumes[0].ground_support->slope_x,.25) &&
        Near(ascending.volumes[0].ground_support->slope_y,0) && Near(descending.volumes[0].ground_support->slope_x,-.25));
    Check("bake-source-identity-retained", ascending.volumes[0].ground_support->source_id == 41 &&
        ascending.volumes[0].ground_support->component_id == 1);
    Check("double-query-below-float-max", ResolveLayerGroundPlacement(ascending,1,std::nextafter(4.0,0.0),0).Ok());
    Check("lower-xy-boundary-included", ResolveLayerGroundPlacement(ascending,1,-4,-2).Ok());
    Check("upper-x-boundary-excluded", ResolveLayerGroundPlacement(ascending,1,4,0).status == GroundSupportStatus::OutsideVolume);
    Check("upper-y-boundary-excluded", ResolveLayerGroundPlacement(ascending,1,0,2).status == GroundSupportStatus::OutsideVolume);
    Check("outside-footprint-rejected", ResolveLayerGroundPlacement(ascending,1,-4.01,0).status == GroundSupportStatus::OutsideVolume);
    Check("unknown-volume-rejected", ResolveLayerGroundPlacement(ascending,999,0,0).status == GroundSupportStatus::UnknownVolume);
    Check("zero-volume-no-legacy-fallback", ResolveLayerGroundPlacement(ascending,0,0,0).status == GroundSupportStatus::UnknownVolume);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    Check("nonfinite-query-rejected", ResolveLayerGroundPlacement(ascending,1,nan,0).status == GroundSupportStatus::InvalidState &&
        ResolveLayerGroundPlacement(ascending,1,0,infinity).status == GroundSupportStatus::InvalidState);
    Check("empty-no-terrain-fallback", ResolveLayerGroundPlacement({},1,0,0).status == GroundSupportStatus::NotAvailable);

    LayeredWorld stack;
    Check("stacked-negative-cook", Bake({Mesh(10,-8),Mesh(20,0),Mesh(30,4)},stack));
    const auto under = ResolveLayerGroundPlacement(stack,1,1,1);
    const auto ground = ResolveLayerGroundPlacement(stack,2,1,1);
    const auto upper = ResolveLayerGroundPlacement(stack,3,1,1);
    Check("explicit-same-xy-three-floors", under.Ok() && ground.Ok() && upper.Ok() &&
        under.state.z == -8 && ground.state.z == 0 && upper.state.z == 4 &&
        under.state.volume_id != upper.state.volume_id);
    Check("grounded-same-volume-move", ResolveLayerGroundMove(ascending,up.state,1,2,1).Ok() &&
        Near(ResolveLayerGroundMove(ascending,up.state,1,2,1).state.z,.5));
    stack.portals.push_back(LayerPortal{1,2,3,{-1,-1,2,2},{-1,-1,2,2},-.01f,1,4,5,true});
    Check("portal-fixture-valid", stack.Validate(bounds,error));
    const auto transition = ResolveLayerGroundMove(stack,ground.state,3,1,1);
    Check("portal-does-not-auto-transition", transition.status == GroundSupportStatus::TransitionRequired && SameState(transition.state,ground.state));
    const auto outside = ResolveLayerGroundMove(ascending,up.state,1,5,1);
    Check("failed-move-transactional", outside.status == GroundSupportStatus::OutsideVolume && SameState(outside.state,up.state));
    auto forged = up.state;
    forged.layer_id = 99;
    Check("wrong-layer-invalid", ResolveLayerGroundMove(ascending,forged,1,1,1).status == GroundSupportStatus::InvalidState &&
        SameState(ResolveLayerGroundMove(ascending,forged,1,1,1).state,forged));
    forged = up.state; forged.z += .01;
    Check("floating-pose-not-snapped", ResolveLayerGroundMove(ascending,forged,1,1,1).status == GroundSupportStatus::InvalidState);
    forged = up.state; forged.z = nan;
    Check("nan-current-retained", ResolveLayerGroundMove(ascending,forged,1,1,1).status == GroundSupportStatus::InvalidState &&
        SameState(ResolveLayerGroundMove(ascending,forged,1,1,1).state,forged));
    const auto bad_target = ResolveLayerGroundMove(ascending,up.state,1,nan,0);
    Check("nan-target-retains-current", bad_target.status == GroundSupportStatus::InvalidState && SameState(bad_target.state,up.state));

    auto policy = ascending;
    policy.volumes[0].supports_ground_movement = false;
    Check("false-movement-policy-rejected", ResolveLayerGroundPlacement(policy,1,0,0).status == GroundSupportStatus::UnsupportedMovement);
    Check("non-ground-plane-validation-rejected", !policy.Validate(bounds,error) && EncodeLayeredWorld(policy).empty());
    policy = ascending; policy.volumes[0].kind = VolumeKind::WaterSurface;
    Check("water-kind-rejected-even-true-flag", ResolveLayerGroundPlacement(policy,1,0,0).status == GroundSupportStatus::UnsupportedMovement);
    policy = ascending; policy.volumes[0].tags |= VolumeTagWater;
    Check("water-tag-rejected-even-ground-kind", ResolveLayerGroundPlacement(policy,1,0,0).status == GroundSupportStatus::UnsupportedMovement);
    policy = ascending; policy.volumes[0].kind = VolumeKind::Underwater;
    Check("underwater-kind-rejected", ResolveLayerGroundPlacement(policy,1,0,0).status == GroundSupportStatus::UnsupportedMovement);
    auto water = Mesh(50,0); water.tags = VolumeTagWater; water.supports_ground_movement = false;
    Check("water-cooker-does-not-create-ground-support", Bake({water},policy) && !policy.volumes[0].ground_support &&
        ResolveLayerGroundPlacement(policy,1,0,0).status == GroundSupportStatus::UnsupportedMovement);
    water.supports_ground_movement = true;
    Check("water-tag-cooker-still-no-support", Bake({water},policy) && !policy.volumes[0].ground_support);
    auto non_ground = Mesh(51,0); non_ground.supports_ground_movement = false;
    Check("non-ground-cooker-no-support", Bake({non_ground},policy) && !policy.volumes[0].ground_support);
    policy = ascending; policy.volumes[0].ground_support.reset();
    Check("unknown-plane-not-a-floor", ResolveLayerGroundPlacement(policy,1,0,0).status == GroundSupportStatus::NotAvailable && policy.Validate(bounds,error));

    std::vector<LayerSourceSurface> sources;
    Check("exact-generator-carries-plane", Bake({Mesh(41,0,.25f)},policy,&sources) && policy.volumes[0].ground_support.has_value());
    LayerGenerationReport generation;
    Check("legacy-generator-drops-plane", GenerateLayeredWorld(sources,LayerGenerationOptions{bounds},policy,generation) &&
        !policy.volumes[0].ground_support && ResolveLayerGroundPlacement(policy,1,0,0).status == GroundSupportStatus::NotAvailable);
    for (const std::uint32_t version : {1u,2u}) {
        LayeredWorld legacy;
        Check(version == 1 ? "v1-decodes-support-unknown" : "v2-decodes-support-unknown",
            DecodeLayeredWorld(LegacyBytes(ascending,version),legacy,error) && legacy.Validate(bounds,error) &&
            legacy.volumes.size() == 1 && !legacy.volumes[0].ground_support &&
            ResolveLayerGroundPlacement(legacy,1,0,0).status == GroundSupportStatus::NotAvailable);
        LayeredWorld upgraded;
        Check(version == 1 ? "v1-reencode-does-not-invent-floor" : "v2-reencode-does-not-invent-floor",
            DecodeLayeredWorld(EncodeLayeredWorld(legacy),upgraded,error) && !upgraded.volumes[0].ground_support);
    }
    const auto bytes = EncodeLayeredWorld(ascending);
    LayeredWorld decoded;
    Check("v3-roundtrip-exact", DecodeLayeredWorld(bytes,decoded,error) && decoded.Validate(bounds,error) &&
        EncodeLayeredWorld(decoded) == bytes && Near(ResolveLayerGroundPlacement(decoded,1,.5,.5).state.z,.125));
    Check("writer-emits-v3", bytes.size() >= 8 && bytes[4] == 3 && bytes[5] == 0);
    const std::size_t support_header = 16 + 40 + ascending.volumes[0].name.size();
    auto malformed = bytes; malformed.at(support_header) = 2;
    Check("unknown-support-flag-rejected", RejectedBytes(malformed));
    malformed = bytes; malformed.at(support_header+1) = 1;
    Check("reserved-support-flags-rejected", RejectedBytes(malformed));
    malformed = bytes; malformed.pop_back();
    Check("truncated-support-rejected-empty", RejectedBytes(malformed));
    malformed = bytes; malformed.push_back(0);
    Check("trailing-data-rejected-empty", RejectedBytes(malformed));
    malformed = bytes; U32At(malformed,8,kMaxLayeredWorldVolumes+1);
    Check("volume-count-limit", RejectedBytes(malformed));
    malformed = bytes; U32At(malformed,12,kMaxLayeredWorldPortals+1);
    Check("portal-count-limit", RejectedBytes(malformed));
    malformed = bytes; U32At(malformed,8,2);
    Check("missing-record-count-rejected", RejectedBytes(malformed));
    malformed = bytes; U32At(malformed,support_header+4,0);
    Check("zero-source-id-rejected", RejectedBytes(malformed));
    malformed = bytes; U32At(malformed,support_header+8,0);
    Check("zero-component-id-rejected", RejectedBytes(malformed));
    malformed = bytes; F64At(malformed,support_header+4+24,nan);
    Check("nan-plane-rejected", RejectedBytes(malformed));
    malformed = bytes; F64At(malformed,support_header+4+32,infinity);
    Check("infinite-slope-rejected", RejectedBytes(malformed));
    malformed = bytes; F64At(malformed,support_header+4+48,-1);
    Check("negative-error-rejected", RejectedBytes(malformed));
    malformed = bytes; F64At(malformed,support_header+4+48,.1);
    Check("deviation-outside-band-rejected", RejectedBytes(malformed));
    malformed = bytes; F64At(malformed,support_header+4+48,.001);
    Check("unproven-large-error-within-band-rejected", RejectedBytes(malformed));
    malformed = bytes; F64At(malformed,support_header+4+24,100);
    Check("floor-outside-band-rejected", RejectedBytes(malformed));
    malformed = bytes; F64At(malformed,support_header+4+8,5);
    F64At(malformed,support_header+4+24,1.25);
    Check("equivalent-plane-remote-anchor-rejected", RejectedBytes(malformed));
    malformed = bytes; F64At(malformed,support_header+4+8,1e308);
    F64At(malformed,support_header+4+24,1e308);
    F64At(malformed,support_header+4+32,1);
    Check("finite-anchor-cancellation-rejected", RejectedBytes(malformed));
    auto boundary_anchor = ascending;
    boundary_anchor.volumes[0].ground_support->anchor_x = 4;
    boundary_anchor.volumes[0].ground_support->anchor_y = 2;
    boundary_anchor.volumes[0].ground_support->anchor_z = 1;
    LayeredWorld boundary_decoded;
    Check("closed-max-boundary-anchor-valid", boundary_anchor.Validate(bounds,error) &&
        DecodeLayeredWorld(EncodeLayeredWorld(boundary_anchor),boundary_decoded,error) &&
        Near(ResolveLayerGroundPlacement(boundary_decoded,1,.5,.5).state.z,.125));
    malformed = bytes; F64At(malformed,support_header+4+32,std::numeric_limits<double>::max());
    Check("finite-plane-overflow-rejected", RejectedBytes(malformed));
    malformed = bytes; U32At(malformed,20,0);
    Check("supported-legacy-layer-id-rejected", RejectedBytes(malformed));
    malformed = bytes; malformed.at(28) = static_cast<std::uint8_t>(VolumeKind::WaterSurface);
    Check("encoded-water-ground-support-rejected", RejectedBytes(malformed));
    malformed = bytes; malformed.at(29) = 2;
    Check("unsupported-movement-flag-rejected", RejectedBytes(malformed));
    malformed = EncodeLayeredWorld(stack); malformed.back() = 1;
    Check("v3-portal-reserved-flag-rejected", RejectedBytes(malformed));
    malformed.assign(static_cast<std::size_t>(kMaxLayeredWorldFileBytes)+1,0);
    Check("file-byte-limit", RejectedBytes(malformed));

    auto ambiguous = ascending;
    ambiguous.volumes.push_back(ascending.volumes[0]);
    Check("unchecked-duplicate-id-query-fails-closed", ResolveLayerGroundPlacement(ambiguous,1,0,0).status == GroundSupportStatus::InvalidState);
    ambiguous.volumes.back().id = 2;
    Check("unchecked-overlap-query-fails-closed", ResolveLayerGroundPlacement(ambiguous,1,0,0).status == GroundSupportStatus::InvalidState);
    auto invalid = ascending; invalid.volumes[0].ground_support->max_height_error_m = nan;
    Check("unchecked-invalid-plane-fails-closed", ResolveLayerGroundPlacement(invalid,1,0,0).status == GroundSupportStatus::InvalidState);
    invalid = ascending; invalid.volumes[0].ground_support->source_id = 0;
    Check("invalid-support-validate-and-encode-reject", !invalid.Validate(bounds,error) && EncodeLayeredWorld(invalid).empty());

    auto almost_planar = Mesh(61,0,.25f);
    almost_planar.vertices[2][2] += .000001f;
    Check("measured-component-deviation-cooks", Bake({almost_planar},policy));
    double measured = 0;
    if (!policy.volumes.empty() && policy.volumes[0].ground_support) {
        const auto& plane = *policy.volumes[0].ground_support;
        for (const auto& vertex : almost_planar.vertices) {
            measured = std::max(measured,std::abs(plane.Height(vertex[0],vertex[1])-vertex[2]));
        }
        Check("deviation-is-actual-vertex-residual", measured > 0 && measured == plane.max_height_error_m);
        auto residual_pose = ResolveLayerGroundPlacement(policy,1,0,0).state;
        residual_pose.z += measured*.5;
        Check("measured-residual-pose-accepted", ResolveLayerGroundMove(policy,residual_pose,1,0,0).Ok());
        residual_pose.z += measured*2;
        Check("beyond-measured-residual-rejected", ResolveLayerGroundMove(policy,residual_pose,1,0,0).status == GroundSupportStatus::InvalidState);
    } else {
        Check("deviation-is-actual-vertex-residual",false);
        Check("measured-residual-pose-accepted",false);
        Check("beyond-measured-residual-rejected",false);
    }

    auto reordered = Mesh(41,0,.25f);
    std::reverse(reordered.vertices.begin(),reordered.vertices.end());
    for (auto& index : reordered.indices) index = 3-index;
    std::swap_ranges(reordered.indices.begin(),reordered.indices.begin()+3,reordered.indices.begin()+3);
    LayeredWorld reordered_world;
    Check("plane-bake-reorder-byte-determinism", Bake({reordered},reordered_world) && EncodeLayeredWorld(reordered_world) == bytes);
    auto islands = Mesh(71,0);
    const auto duplicate = Mesh(71,0);
    for (auto vertex : duplicate.vertices) { vertex[0] += 20; islands.vertices.push_back(vertex); }
    for (const auto index : duplicate.indices) islands.indices.push_back(index+4);
    std::vector<LayerSourceSurface> components;
    LayerGeometryReport extraction;
    Check("components-retain-source-and-unique-bake-id", ExtractLayerSourceSurfaces({islands},{},components,extraction) &&
        components.size() == 2 && components[0].ground_support && components[1].ground_support &&
        components[0].ground_support->source_id == 71 && components[1].ground_support->source_id == 71 &&
        components[0].ground_support->component_id != components[1].ground_support->component_id);
    Check("status-labels", std::string(ToString(GroundSupportStatus::TransitionRequired)) == "transition_required");
    std::cout << "LAYER GROUND summary: checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
