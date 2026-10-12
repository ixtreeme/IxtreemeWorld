#pragma once
#include "MaterialGraphCompiler.h"
#include "GeometryBaker.h"
#include <cstdint>

namespace material_tool
{
enum class PreviewChannel { Shaded, BaseColor, Roughness, Metallic, Normal, AO, Height, Emissive, SelectedOutput,
    GeometryAO, Curvature, Cavity, Thickness, ObjectNormal, WorldNormal, ObjectPosition, WorldPosition };
struct PreviewSettings {
    PreviewChannel channel = PreviewChannel::Shaded;
    bool sphere = true;
    float worldSize = 10, heightOffset = 0;
    EvaluationContext surfaceData;
    PinType selectedType = PinType::Scalar;
    bool operator==(const PreviewSettings&) const = default;
};
struct BakedMaterialImages {
    unsigned size = 0;
    std::vector<std::uint8_t> color, normal, ao, roughness, metallic, height, emissive;
};
std::vector<std::uint8_t> RenderMaterialPreview(const MaterialGraphProgram& program, const ParameterOverrides& parameters,
    const TextureSampler& sample, const PreviewSettings& settings, unsigned size = 128, const GeometryBake* geometry = nullptr);
// Only UV-dependent graphs can be flattened into an ordinary material without losing geometric rules.
bool BakeMaterialUV(const MaterialGraphProgram& program, const ParameterOverrides& parameters,
    const TextureSampler& sample, unsigned size, BakedMaterialImages& images, std::string& error);
// Object-specific atlas: geometry/world rules use real cached surface samples, including padding.
bool BakeMaterialOnGeometry(const MaterialGraphProgram& program, const ParameterOverrides& parameters,
    const TextureSampler& sample, const GeometryBake& geometry, BakedMaterialImages& images, std::string& error);
}
