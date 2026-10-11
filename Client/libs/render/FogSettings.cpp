#include "FogSettings.h"
#include <ostream>

namespace {
void WriteCommon(std::ostream& out, bool enabled, const float* color, float density, const std::string& indent)
{
    out << indent << "\"enabled\": " << (enabled ? "true" : "false") << ",\n";
    out << indent << "\"color\": [" << color[0] << ", " << color[1] << ", " << color[2] << "],\n";
    out << indent << "\"density\": " << density << ",\n";
}
}

void WriteFogFields(std::ostream& out, GlobalFogSettings fog, const std::string& indent)
{
    SanitizeFog(fog);
    WriteCommon(out, fog.enabled, fog.color, fog.density, indent);
    out << indent << "\"start_distance\": " << fog.startDistance << ",\n";
    out << indent << "\"max_distance\": " << fog.maxDistance << ",\n";
    out << indent << "\"max_opacity\": " << fog.maxOpacity << ",\n";
    out << indent << "\"height_enabled\": " << (fog.heightEnabled ? "true" : "false") << ",\n";
    out << indent << "\"base_height\": " << fog.baseHeight << ",\n";
    out << indent << "\"height_falloff\": " << fog.heightFalloff << ",\n";
    out << indent << "\"affect_sky\": " << (fog.affectSky ? "true" : "false") << "\n";
}

void WriteFogFields(std::ostream& out, FogVolumeComponent fog, const std::string& indent)
{
    SanitizeFog(fog);
    WriteCommon(out, fog.enabled, fog.color, fog.density, indent);
    out << indent << "\"shape\": \"" << (fog.shape == FogVolumeComponent::Shape::Sphere ? "sphere" : "box") << "\",\n";
    out << indent << "\"half_extents\": [" << fog.halfExtents[0] << ", " << fog.halfExtents[1] << ", " << fog.halfExtents[2] << "],\n";
    out << indent << "\"radius\": " << fog.radius << ",\n";
    out << indent << "\"edge_fade\": " << fog.edgeFade << "\n";
}
