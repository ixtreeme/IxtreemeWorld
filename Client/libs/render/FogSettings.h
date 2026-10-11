#pragma once

#include <algorithm>
#include <cmath>
#include <iosfwd>
#include <string>

// Density is extinction per metre: transmission through distance d is exp(-density * d).
struct GlobalFogSettings
{
    bool enabled = false;
    float color[3] = {0.65f, 0.72f, 0.80f};
    float density = 0.01f;
    float startDistance = 0.0f;
    float maxDistance = 1000.0f;
    float maxOpacity = 0.98f;
    bool heightEnabled = false;
    float baseHeight = 0.0f;
    float heightFalloff = 0.05f;
    bool affectSky = true;
};

struct FogVolumeComponent
{
    enum class Shape { Box, Sphere };
    bool enabled = true;
    Shape shape = Shape::Box;
    float color[3] = {0.65f, 0.72f, 0.80f};
    float density = 0.08f;
    // World-space metres, independent of the host object's scale (including zero).
    float halfExtents[3] = {10.0f, 3.0f, 10.0f};
    float radius = 8.0f;
    float edgeFade = 1.0f;
};

inline float FogFiniteClamp(float value, float fallback, float low, float high)
{
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

inline void SanitizeFog(GlobalFogSettings& fog)
{
    for (float& color : fog.color) color = FogFiniteClamp(color, 0.7f, 0.0f, 1.0f);
    fog.density = FogFiniteClamp(fog.density, 0.01f, 0.0f, 10.0f);
    fog.maxDistance = FogFiniteClamp(fog.maxDistance, 1000.0f, 0.1f, 100000.0f);
    fog.startDistance = FogFiniteClamp(fog.startDistance, 0.0f, 0.0f, fog.maxDistance);
    fog.maxOpacity = FogFiniteClamp(fog.maxOpacity, 0.98f, 0.0f, 1.0f);
    fog.baseHeight = FogFiniteClamp(fog.baseHeight, 0.0f, -100000.0f, 100000.0f);
    fog.heightFalloff = FogFiniteClamp(fog.heightFalloff, 0.05f, 0.0f, 10.0f);
}

inline void SanitizeFog(FogVolumeComponent& fog)
{
    for (float& color : fog.color) color = FogFiniteClamp(color, 0.7f, 0.0f, 1.0f);
    fog.density = FogFiniteClamp(fog.density, 0.08f, 0.0f, 10.0f);
    for (float& extent : fog.halfExtents) extent = FogFiniteClamp(extent, 1.0f, 0.01f, 100000.0f);
    fog.radius = FogFiniteClamp(fog.radius, 8.0f, 0.01f, 100000.0f);
    const float smallest = fog.shape == FogVolumeComponent::Shape::Sphere ? fog.radius :
        std::min({fog.halfExtents[0], fog.halfExtents[1], fog.halfExtents[2]});
    fog.edgeFade = FogFiniteClamp(fog.edgeFade, 0.0f, 0.0f, smallest);
}

// Fields only, so scene and prefab files share exactly the same representation.
void WriteFogFields(std::ostream& out, GlobalFogSettings fog, const std::string& indent);
void WriteFogFields(std::ostream& out, FogVolumeComponent fog, const std::string& indent);
