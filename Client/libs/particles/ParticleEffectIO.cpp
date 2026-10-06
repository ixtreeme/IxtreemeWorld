#include "ParticleEffectIO.h"

#include "Common.h"

#include <array>
#include <cstdint>
#include <sstream>

namespace ixparticle
{
namespace
{

std::string FloatArrayText(const float* values, std::size_t count)
{
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i > 0)
            out << ", ";
        out << values[i];
    }
    out << "]";
    return out.str();
}

} // namespace

std::string WriteParticleEffectJson(const ParticleSystemComponent& e,
                                    const std::string& id,
                                    const std::string& displayName)
{
    std::ostringstream out;
    out << "{\n";
    out << "  \"version\": 1,\n";
    out << "  \"type\": \"particle_effect\",\n";
    out << "  \"id\": \"" << ixtreeme::common::EscapeJson(id) << "\",\n";
    out << "  \"display_name\": \"" << ixtreeme::common::EscapeJson(displayName) << "\",\n";
    out << "  \"texture_asset_id\": \"" << ixtreeme::common::EscapeJson(e.textureAssetId) << "\",\n";
    out << "  \"blend_mode\": \"" << BlendModeName(e.blendMode) << "\",\n";
    out << "  \"duration\": " << e.duration << ",\n";
    out << "  \"emission_rate\": " << e.emissionRate << ",\n";
    out << "  \"burst_count\": " << e.burstCount << ",\n";
    out << "  \"max_particles\": " << e.maxParticles << ",\n";
    out << "  \"start_lifetime_min\": " << e.startLifetimeMin << ",\n";
    out << "  \"start_lifetime_max\": " << e.startLifetimeMax << ",\n";
    out << "  \"start_speed_min\": " << e.startSpeedMin << ",\n";
    out << "  \"start_speed_max\": " << e.startSpeedMax << ",\n";
    out << "  \"start_size_min\": " << e.startSizeMin << ",\n";
    out << "  \"start_size_max\": " << e.startSizeMax << ",\n";
    out << "  \"size_over_life\": " << FloatArrayText(e.sizeOverLife, 4) << ",\n";
    out << "  \"direction\": " << FloatArrayText(e.direction, 3) << ",\n";
    out << "  \"cone_angle\": " << e.coneAngle << ",\n";
    out << "  \"shape_radius\": " << e.shapeRadius << ",\n";
    out << "  \"shape\": \"" << ShapeName(e.shape) << "\",\n";
    out << "  \"shape_extents\": " << FloatArrayText(e.shapeExtents, 3) << ",\n";
    out << "  \"shape_arc\": " << e.shapeArc << ",\n";
    out << "  \"collide_with_ground\": " << (e.collideWithGround ? "true" : "false") << ",\n";
    out << "  \"collision_bounce\": " << e.collisionBounce << ",\n";
    out << "  \"collision_friction\": " << e.collisionFriction << ",\n";
    out << "  \"ground_plane_y\": " << e.groundPlaneY << ",\n";
    out << "  \"local_space\": " << (e.localSpace ? "true" : "false") << ",\n";
    out << "  \"gravity\": " << e.gravity << ",\n";
    out << "  \"drag\": " << e.drag << ",\n";
    out << "  \"rotation_speed\": " << e.rotationSpeed << ",\n";
    out << "  \"soft_particles\": " << (e.softParticles ? "true" : "false") << ",\n";
    out << "  \"soft_distance\": " << e.softDistance << ",\n";
    out << "  \"atlas_columns\": " << e.atlasColumns << ",\n";
    out << "  \"atlas_rows\": " << e.atlasRows << ",\n";
    out << "  \"start_color\": " << FloatArrayText(e.colorOverLife, 4) << ",\n";
    out << "  \"end_color\": " << FloatArrayText(e.colorOverLife + 12, 4) << ",\n";
    out << "  \"color_over_life\": " << FloatArrayText(e.colorOverLife, 16) << "\n";
    out << "}\n";
    return out.str();
}

bool ParseParticleEffectJson(const std::string& text, ParticleSystemComponent& out)
{
    const std::size_t open = text.find('{');
    if (open == std::string::npos)
        return false;

    out.textureAssetId = ixtreeme::common::JsonStringValue(text, "texture_asset_id");
    out.blendMode = ParseBlendMode(ixtreeme::common::JsonStringValue(text, "blend_mode"));
    out.duration = ixtreeme::common::JsonFloatValue(text, "duration", out.duration);
    out.emissionRate = ixtreeme::common::JsonFloatValue(text, "emission_rate", out.emissionRate);
    out.burstCount = static_cast<int>(ixtreeme::common::JsonFloatValue(
        text, "burst_count", static_cast<float>(out.burstCount)));
    out.maxParticles = static_cast<int>(ixtreeme::common::JsonFloatValue(
        text, "max_particles", static_cast<float>(out.maxParticles)));
    out.startLifetimeMin = ixtreeme::common::JsonFloatValue(text, "start_lifetime_min", out.startLifetimeMin);
    out.startLifetimeMax = ixtreeme::common::JsonFloatValue(text, "start_lifetime_max", out.startLifetimeMax);
    out.startSpeedMin = ixtreeme::common::JsonFloatValue(text, "start_speed_min", out.startSpeedMin);
    out.startSpeedMax = ixtreeme::common::JsonFloatValue(text, "start_speed_max", out.startSpeedMax);
    out.startSizeMin = ixtreeme::common::JsonFloatValue(text, "start_size_min", out.startSizeMin);
    out.startSizeMax = ixtreeme::common::JsonFloatValue(text, "start_size_max", out.startSizeMax);
    // Backward compatibility: pre-curve files stored a linear start/end pair + an end size scale.
    {
        float startColor[4] = {out.colorOverLife[0], out.colorOverLife[1], out.colorOverLife[2], out.colorOverLife[3]};
        float endColor[4] = {out.colorOverLife[12], out.colorOverLife[13], out.colorOverLife[14], out.colorOverLife[15]};
        ixtreeme::common::JsonFloatArrayValue(text, "start_color", startColor, 4);
        ixtreeme::common::JsonFloatArrayValue(text, "end_color", endColor, 4);
        for (int c = 0; c < 4; ++c)
        {
            out.colorOverLife[c] = startColor[c];
            out.colorOverLife[12 + c] = endColor[c];
            out.colorOverLife[4 + c] = startColor[c] + (endColor[c] - startColor[c]) * (1.0f / 3.0f);
            out.colorOverLife[8 + c] = startColor[c] + (endColor[c] - startColor[c]) * (2.0f / 3.0f);
        }
        ixtreeme::common::JsonFloatArrayValue(text, "color_over_life", out.colorOverLife, 16);

        const float endSizeScale = ixtreeme::common::JsonFloatValue(text, "end_size_scale", out.sizeOverLife[3]);
        out.sizeOverLife[0] = 1.0f;
        out.sizeOverLife[1] = 1.0f;
        out.sizeOverLife[2] = 1.0f;
        out.sizeOverLife[3] = endSizeScale;
        ixtreeme::common::JsonFloatArrayValue(text, "size_over_life", out.sizeOverLife, 4);
    }
    ixtreeme::common::JsonFloatArrayValue(text, "direction", out.direction, 3);
    out.coneAngle = ixtreeme::common::JsonFloatValue(text, "cone_angle", out.coneAngle);
    out.shapeRadius = ixtreeme::common::JsonFloatValue(text, "shape_radius", out.shapeRadius);
    out.shape = ParseShape(ixtreeme::common::JsonStringValue(text, "shape"));
    ixtreeme::common::JsonFloatArrayValue(text, "shape_extents", out.shapeExtents, 3);
    out.shapeArc = ixtreeme::common::JsonFloatValue(text, "shape_arc", out.shapeArc);
    out.collideWithGround = ixtreeme::common::JsonBoolValue(text, "collide_with_ground", out.collideWithGround);
    out.collisionBounce = ixtreeme::common::JsonFloatValue(text, "collision_bounce", out.collisionBounce);
    out.collisionFriction = ixtreeme::common::JsonFloatValue(text, "collision_friction", out.collisionFriction);
    out.groundPlaneY = ixtreeme::common::JsonFloatValue(text, "ground_plane_y", out.groundPlaneY);
    out.localSpace = ixtreeme::common::JsonBoolValue(text, "local_space", out.localSpace);
    out.gravity = ixtreeme::common::JsonFloatValue(text, "gravity", out.gravity);
    out.drag = ixtreeme::common::JsonFloatValue(text, "drag", out.drag);
    out.rotationSpeed = ixtreeme::common::JsonFloatValue(text, "rotation_speed", out.rotationSpeed);
    out.softParticles = ixtreeme::common::JsonBoolValue(text, "soft_particles", out.softParticles);
    out.softDistance = ixtreeme::common::JsonFloatValue(text, "soft_distance", out.softDistance);
    out.atlasColumns = static_cast<int>(ixtreeme::common::JsonFloatValue(
        text, "atlas_columns", static_cast<float>(out.atlasColumns)));
    out.atlasRows = static_cast<int>(ixtreeme::common::JsonFloatValue(
        text, "atlas_rows", static_cast<float>(out.atlasRows)));
    Sanitize(out);
    return true;
}

} // namespace ixparticle
