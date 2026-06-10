#include "AssetLibrary.h"

#include "Debug.h"

#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <ctime>
#include <unordered_set>

namespace
{
std::string ToLower(std::string value)
{
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::string SanitizeStem(std::string value)
{
    for (char& c : value)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '_';
    }
    while (!value.empty() && value.back() == '_')
        value.pop_back();
    return value.empty() ? "asset" : value;
}

std::string EscapeJson(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value)
    {
        switch (c)
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

bool JsonArrayBody(const std::string& text, const std::string& key, std::string& out);

std::string JsonStringValue(const std::string& object, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = object.find(needle);
    if (keyPos == std::string::npos)
        return {};
    const size_t colon = object.find(':', keyPos + needle.size());
    if (colon == std::string::npos)
        return {};
    size_t firstQuote = object.find('"', colon + 1);
    if (firstQuote == std::string::npos)
        return {};
    std::string out;
    bool escaping = false;
    for (size_t i = firstQuote + 1; i < object.size(); ++i)
    {
        const char c = object[i];
        if (escaping)
        {
            out += c;
            escaping = false;
            continue;
        }
        if (c == '\\')
        {
            escaping = true;
            continue;
        }
        if (c == '"')
            break;
        out += c;
    }
    return out;
}

float JsonFloatValue(const std::string& object, const std::string& key, float fallback)
{
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = object.find(needle);
    if (keyPos == std::string::npos)
        return fallback;
    const size_t colon = object.find(':', keyPos + needle.size());
    if (colon == std::string::npos)
        return fallback;
    const char* begin = object.c_str() + colon + 1;
    char* end = nullptr;
    const float value = std::strtof(begin, &end);
    return end != begin ? value : fallback;
}

bool JsonBoolValue(const std::string& object, const std::string& key, bool fallback)
{
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = object.find(needle);
    if (keyPos == std::string::npos)
        return fallback;
    const size_t colon = object.find(':', keyPos + needle.size());
    if (colon == std::string::npos)
        return fallback;
    size_t pos = colon + 1;
    while (pos < object.size() && std::isspace(static_cast<unsigned char>(object[pos])))
        ++pos;
    if (object.compare(pos, 4, "true") == 0)
        return true;
    if (object.compare(pos, 5, "false") == 0)
        return false;
    return fallback;
}

std::string JsonObjectValue(const std::string& object, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = object.find(needle);
    if (keyPos == std::string::npos)
        return {};
    const size_t begin = object.find('{', keyPos + needle.size());
    if (begin == std::string::npos)
        return {};
    int depth = 0;
    bool inString = false;
    bool escaping = false;
    for (size_t i = begin; i < object.size(); ++i)
    {
        const char c = object[i];
        if (inString)
        {
            if (escaping)
                escaping = false;
            else if (c == '\\')
                escaping = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"')
        {
            inString = true;
            continue;
        }
        if (c == '{')
            ++depth;
        else if (c == '}')
        {
            --depth;
            if (depth == 0)
                return object.substr(begin, i - begin + 1);
        }
    }
    return {};
}

bool JsonObjectAt(const std::string& text, size_t begin, std::string& out, size_t& endOut)
{
    if (begin >= text.size() || text[begin] != '{')
        return false;
    int depth = 0;
    bool inString = false;
    bool escaping = false;
    for (size_t i = begin; i < text.size(); ++i)
    {
        const char c = text[i];
        if (inString)
        {
            if (escaping)
                escaping = false;
            else if (c == '\\')
                escaping = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"')
        {
            inString = true;
            continue;
        }
        if (c == '{')
            ++depth;
        else if (c == '}')
        {
            --depth;
            if (depth == 0)
            {
                out = text.substr(begin, i - begin + 1);
                endOut = i + 1;
                return true;
            }
        }
    }
    return false;
}

std::string JsonNullableStringValue(const std::string& object, const std::string& key)
{
    return JsonStringValue(object, key);
}

std::vector<std::string> JsonStringArrayValue(const std::string& object, const std::string& key)
{
    std::vector<std::string> values;
    std::string body;
    if (!JsonArrayBody(object, key, body))
        return values;

    bool inString = false;
    bool escaping = false;
    std::string value;
    for (char c : body)
    {
        if (!inString)
        {
            if (c == '"')
            {
                inString = true;
                value.clear();
            }
            continue;
        }

        if (escaping)
        {
            value += c;
            escaping = false;
            continue;
        }
        if (c == '\\')
        {
            escaping = true;
            continue;
        }
        if (c == '"')
        {
            values.push_back(value);
            inString = false;
            continue;
        }
        value += c;
    }
    return values;
}

bool JsonArrayBody(const std::string& text, const std::string& key, std::string& out)
{
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = text.find(needle);
    if (keyPos == std::string::npos)
        return false;
    const size_t arrayBegin = text.find('[', keyPos + needle.size());
    if (arrayBegin == std::string::npos)
        return false;

    int depth = 0;
    bool inString = false;
    bool escaping = false;
    for (size_t i = arrayBegin; i < text.size(); ++i)
    {
        const char c = text[i];
        if (inString)
        {
            if (escaping)
                escaping = false;
            else if (c == '\\')
                escaping = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"')
        {
            inString = true;
            continue;
        }
        if (c == '[')
            ++depth;
        else if (c == ']')
        {
            --depth;
            if (depth == 0)
            {
                out = text.substr(arrayBegin + 1, i - arrayBegin - 1);
                return true;
            }
        }
    }
    return false;
}

bool IsDataUri(const std::string& uri)
{
    const std::string lower = ToLower(uri);
    return lower.rfind("data:", 0) == 0;
}

int HexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + c - 'a';
    if (c >= 'A' && c <= 'F')
        return 10 + c - 'A';
    return -1;
}

std::string PercentDecodeUriPath(std::string uri)
{
    const size_t fragment = uri.find('#');
    if (fragment != std::string::npos)
        uri.resize(fragment);
    const size_t query = uri.find('?');
    if (query != std::string::npos)
        uri.resize(query);

    std::string out;
    out.reserve(uri.size());
    for (size_t i = 0; i < uri.size(); ++i)
    {
        if (uri[i] == '%' && i + 2 < uri.size())
        {
            const int hi = HexValue(uri[i + 1]);
            const int lo = HexValue(uri[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(uri[i]);
    }
    return out;
}

bool NormalizeGltfDependencyUri(const std::string& uri, std::filesystem::path& outRelative, std::string& error)
{
    if (uri.empty() || IsDataUri(uri))
        return false;

    const std::string decoded = PercentDecodeUriPath(uri);
    if (decoded.find("://") != std::string::npos)
    {
        error = "remote glTF URI is not supported: " + uri;
        return false;
    }

    std::string slashPath = decoded;
    std::replace(slashPath.begin(), slashPath.end(), '\\', '/');
    std::filesystem::path candidate(slashPath);
    if (candidate.is_absolute() || !candidate.root_name().empty())
    {
        error = "absolute glTF URI is not supported: " + uri;
        return false;
    }

    std::filesystem::path normalized;
    for (const auto& part : candidate)
    {
        const std::string text = part.generic_string();
        if (text.empty() || text == ".")
            continue;
        if (text == "..")
        {
            error = "parent-relative glTF URI is not supported: " + uri;
            return false;
        }
        normalized /= part;
    }

    if (normalized.empty())
        return false;
    outRelative = normalized;
    return true;
}

void CollectGltfDependencyArray(const std::string& json,
                                const std::string& key,
                                std::vector<std::filesystem::path>& dependencies,
                                std::string& error)
{
    std::string body;
    if (!JsonArrayBody(json, key, body))
        return;

    size_t pos = 0;
    while ((pos = body.find('{', pos)) != std::string::npos)
    {
        std::string object;
        size_t end = 0;
        if (!JsonObjectAt(body, pos, object, end))
            break;
        pos = end;

        const std::string uri = JsonStringValue(object, "uri");
        std::filesystem::path relative;
        if (NormalizeGltfDependencyUri(uri, relative, error))
            dependencies.push_back(relative);
        if (!error.empty())
            return;
    }
}

bool CopyGltfExternalDependencies(const std::filesystem::path& sourceGltf,
                                  const std::filesystem::path& destinationGltf,
                                  std::vector<std::filesystem::path>& copiedFiles,
                                  std::string& error)
{
    copiedFiles.clear();
    if (ToLower(sourceGltf.extension().string()) != ".gltf")
        return true;

    std::ifstream file(sourceGltf, std::ios::binary);
    if (!file)
    {
        error = "failed to open glTF source";
        return false;
    }
    const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    std::vector<std::filesystem::path> dependencies;
    CollectGltfDependencyArray(json, "buffers", dependencies, error);
    if (error.empty())
        CollectGltfDependencyArray(json, "images", dependencies, error);
    if (!error.empty())
        return false;

    std::set<std::string> unique;
    std::vector<std::filesystem::path> uniqueDependencies;
    for (const auto& dependency : dependencies)
    {
        const std::string key = dependency.generic_string();
        if (unique.insert(key).second)
            uniqueDependencies.push_back(dependency);
    }

    const std::filesystem::path sourceDir = sourceGltf.parent_path();
    const std::filesystem::path destinationDir = destinationGltf.parent_path();
    for (const auto& dependency : uniqueDependencies)
    {
        const std::filesystem::path source = sourceDir / dependency;
        if (!std::filesystem::is_regular_file(source))
        {
            Tracenf("[ASSET-LIBRARY] glTF missing external dependency: %s", source.string().c_str());
            error = "missing glTF external dependency: " + dependency.generic_string();
            return false;
        }
    }

    std::error_code ec;
    for (const auto& dependency : uniqueDependencies)
    {
        const std::filesystem::path source = sourceDir / dependency;
        const std::filesystem::path destination = destinationDir / dependency;
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec)
        {
            error = ec.message();
            return false;
        }
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            error = ec.message();
            return false;
        }
        copiedFiles.push_back(destination);
        Tracenf("[ASSET-LIBRARY] glTF dependency imported: %s -> %s",
            dependency.generic_string().c_str(),
            destination.string().c_str());
    }

    return true;
}

std::string TimestampUtc()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

bool AtomicWriteText(const std::filesystem::path& path, const std::string& text, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            error = "failed to open temp file";
            return false;
        }
        file << text;
        if (!file)
        {
            error = "failed to write temp file";
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec)
    {
        error = ec.message();
        return false;
    }
    return true;
}

bool IsSubpathInside(const std::string& value, const std::string& parent)
{
    return value == parent || value.rfind(parent + "/", 0) == 0;
}

std::string ParentSubpath(const std::string& path)
{
    const std::string normalized = AssetLibrary::NormalizeSubpath(path);
    const size_t slash = normalized.find_last_of('/');
    return slash == std::string::npos ? std::string{} : normalized.substr(0, slash);
}

std::string ReplaceSubpathPrefix(const std::string& value,
                                 const std::string& oldPrefix,
                                 const std::string& newPrefix)
{
    if (value == oldPrefix)
        return newPrefix;
    if (value.rfind(oldPrefix + "/", 0) == 0)
        return newPrefix + value.substr(oldPrefix.size());
    return value;
}

AssetLibrary::MaterialData ClampMaterialData(AssetLibrary::MaterialData material)
{
    material.tilingScaleX = std::clamp(material.tilingScaleX, 0.1f, 10.0f);
    material.tilingScaleY = std::clamp(material.tilingScaleY, 0.1f, 10.0f);
    material.normalStrength = std::clamp(material.normalStrength, 0.0f, 3.0f);
    material.aoStrength = std::clamp(material.aoStrength, 0.0f, 2.0f);
    material.roughnessStrength = std::clamp(material.roughnessStrength, 0.0f, 2.0f);
    material.metallicStrength = std::clamp(material.metallicStrength, 0.0f, 2.0f);
    material.colorTint[0] = std::clamp(material.colorTint[0], 0.0f, 1.0f);
    material.colorTint[1] = std::clamp(material.colorTint[1], 0.0f, 1.0f);
    material.colorTint[2] = std::clamp(material.colorTint[2], 0.0f, 1.0f);
    return material;
}

WaterConfig ClampWaterConfig(WaterConfig config)
{
    config.waterLevelY = std::clamp(config.waterLevelY, -1000.0f, 1000.0f);
    for (float& value : config.baseColor) value = std::clamp(value, 0.0f, 1.0f);
    config.waveScaleSmall = std::clamp(config.waveScaleSmall, 0.001f, 2.0f);
    config.waveScaleLarge = std::clamp(config.waveScaleLarge, 0.001f, 2.0f);
    config.waveSpeedSmall = std::clamp(config.waveSpeedSmall, 0.0f, 5.0f);
    config.waveSpeedLarge = std::clamp(config.waveSpeedLarge, 0.0f, 5.0f);
    config.normalStrength = std::clamp(config.normalStrength, 0.0f, 4.0f);
    config.fresnelPower = std::clamp(config.fresnelPower, 0.1f, 20.0f);
    config.fresnelMin = std::clamp(config.fresnelMin, 0.0f, 1.0f);
    for (float& value : config.reflectionColor) value = std::clamp(value, 0.0f, 1.0f);
    config.reflectionDistortionStrength = std::clamp(config.reflectionDistortionStrength, 0.0f, 1.0f);
    for (float& value : config.shallowColor) value = std::clamp(value, 0.0f, 1.0f);
    for (float& value : config.deepColor) value = std::clamp(value, 0.0f, 1.0f);
    config.depthColorMin = std::clamp(config.depthColorMin, 0.0f, 100.0f);
    config.depthColorMax = std::max(config.depthColorMin + 0.001f, std::clamp(config.depthColorMax, 0.001f, 200.0f));
    config.depthFadeDistance = std::clamp(config.depthFadeDistance, 0.001f, 200.0f);
    config.refractionStrength = std::clamp(config.refractionStrength, 0.0f, 1.0f);
    config.refractionDepthStrength = std::clamp(config.refractionDepthStrength, 0.0f, 8.0f);
    config.foamDistance = std::clamp(config.foamDistance, 0.0f, 20.0f);
    config.foamSoftness = std::clamp(config.foamSoftness, 0.001f, 20.0f);
    config.foamIntensity = std::clamp(config.foamIntensity, 0.0f, 8.0f);
    config.foamScrollSpeed = std::clamp(config.foamScrollSpeed, 0.0f, 10.0f);
    config.foamScale = std::clamp(config.foamScale, 0.001f, 20.0f);
    config.foamTerrainThickness = std::clamp(config.foamTerrainThickness, 0.0f, 20.0f);
    config.causticIntensity = std::clamp(config.causticIntensity, 0.0f, 8.0f);
    config.causticScale = std::clamp(config.causticScale, 0.001f, 20.0f);
    config.causticSpeed = std::clamp(config.causticSpeed, 0.0f, 10.0f);
    config.causticMaxDepth = std::clamp(config.causticMaxDepth, 0.001f, 200.0f);
    config.edgeFadeDistance = std::clamp(config.edgeFadeDistance, 0.0f, 3.0f);
    config.edgeFadeCurve = static_cast<WaterConfig::EdgeFadeCurve>(
        std::clamp(static_cast<int>(config.edgeFadeCurve), 0, 2));
    return config;
}

WaterMaterialData ClampWaterMaterialData(WaterMaterialData material)
{
    material.config = ClampWaterConfig(material.config);
    material.scrollSpeedA[0] = std::clamp(material.scrollSpeedA[0], -10.0f, 10.0f);
    material.scrollSpeedA[1] = std::clamp(material.scrollSpeedA[1], -10.0f, 10.0f);
    material.scrollSpeedB[0] = std::clamp(material.scrollSpeedB[0], -10.0f, 10.0f);
    material.scrollSpeedB[1] = std::clamp(material.scrollSpeedB[1], -10.0f, 10.0f);
    material.normalTiling = std::clamp(material.normalTiling, 0.001f, 100.0f);
    material.formatVersion = std::max(1u, material.formatVersion);
    return material;
}

void WriteWaterConfigJson(std::ostringstream& json, const WaterConfig& config, const char* indent)
{
    json << indent << "\"enabled\": " << (config.enabled ? "true" : "false") << ",\n"
         << indent << "\"water_level_y\": " << config.waterLevelY << ",\n"
         << indent << "\"base_color\": [" << config.baseColor[0] << ", " << config.baseColor[1] << ", "
         << config.baseColor[2] << ", " << config.baseColor[3] << "],\n"
         << indent << "\"wave_scale_small\": " << config.waveScaleSmall << ",\n"
         << indent << "\"wave_scale_large\": " << config.waveScaleLarge << ",\n"
         << indent << "\"wave_speed_small\": " << config.waveSpeedSmall << ",\n"
         << indent << "\"wave_speed_large\": " << config.waveSpeedLarge << ",\n"
         << indent << "\"normal_strength\": " << config.normalStrength << ",\n"
         << indent << "\"fresnel_power\": " << config.fresnelPower << ",\n"
         << indent << "\"fresnel_min\": " << config.fresnelMin << ",\n"
         << indent << "\"reflection_color\": [" << config.reflectionColor[0] << ", " << config.reflectionColor[1]
         << ", " << config.reflectionColor[2] << "],\n"
         << indent << "\"reflection_enabled\": " << (config.reflectionEnabled ? "true" : "false") << ",\n"
         << indent << "\"reflection_quality\": " << static_cast<std::int32_t>(config.reflectionQuality) << ",\n"
         << indent << "\"reflection_distortion_strength\": " << config.reflectionDistortionStrength << ",\n"
         << indent << "\"refraction_enabled\": " << (config.refractionEnabled ? "true" : "false") << ",\n"
         << indent << "\"shallow_color\": [" << config.shallowColor[0] << ", " << config.shallowColor[1]
         << ", " << config.shallowColor[2] << "],\n"
         << indent << "\"deep_color\": [" << config.deepColor[0] << ", " << config.deepColor[1]
         << ", " << config.deepColor[2] << "],\n"
         << indent << "\"depth_color_min\": " << config.depthColorMin << ",\n"
         << indent << "\"depth_color_max\": " << config.depthColorMax << ",\n"
         << indent << "\"depth_fade_distance\": " << config.depthFadeDistance << ",\n"
         << indent << "\"refraction_strength\": " << config.refractionStrength << ",\n"
         << indent << "\"refraction_depth_strength\": " << config.refractionDepthStrength << ",\n"
         << indent << "\"foam_enabled\": " << (config.foamEnabled ? "true" : "false") << ",\n"
         << indent << "\"foam_distance\": " << config.foamDistance << ",\n"
         << indent << "\"foam_softness\": " << config.foamSoftness << ",\n"
         << indent << "\"foam_intensity\": " << config.foamIntensity << ",\n"
         << indent << "\"foam_scroll_speed\": " << config.foamScrollSpeed << ",\n"
         << indent << "\"foam_scale\": " << config.foamScale << ",\n"
         << indent << "\"foam_terrain_thickness\": " << config.foamTerrainThickness << ",\n"
         << indent << "\"caustic_mode\": " << static_cast<std::int32_t>(config.causticMode) << ",\n"
         << indent << "\"caustic_intensity\": " << config.causticIntensity << ",\n"
         << indent << "\"caustic_scale\": " << config.causticScale << ",\n"
         << indent << "\"caustic_speed\": " << config.causticSpeed << ",\n"
         << indent << "\"caustic_max_depth\": " << config.causticMaxDepth << ",\n"
         << indent << "\"edge_fade_distance\": " << config.edgeFadeDistance << ",\n"
         << indent << "\"edge_fade_curve\": " << static_cast<std::int32_t>(config.edgeFadeCurve);
}

void ReadFloatArray(const std::string& object, const std::string& key, float* values, std::size_t count)
{
    std::string body;
    if (!JsonArrayBody(object, key, body))
        return;
    const char* cursor = body.c_str();
    for (std::size_t i = 0; i < count && *cursor != '\0';)
    {
        char* end = nullptr;
        const float value = std::strtof(cursor, &end);
        if (end != cursor)
        {
            values[i++] = value;
            cursor = end;
            continue;
        }
        ++cursor;
    }
}

WaterConfig ReadWaterConfigJson(const std::string& object, WaterConfig fallback = {})
{
    WaterConfig config = fallback;
    config.enabled = JsonBoolValue(object, "enabled", config.enabled);
    config.waterLevelY = JsonFloatValue(object, "water_level_y", config.waterLevelY);
    ReadFloatArray(object, "base_color", config.baseColor, 4);
    config.waveScaleSmall = JsonFloatValue(object, "wave_scale_small", config.waveScaleSmall);
    config.waveScaleLarge = JsonFloatValue(object, "wave_scale_large", config.waveScaleLarge);
    config.waveSpeedSmall = JsonFloatValue(object, "wave_speed_small", config.waveSpeedSmall);
    config.waveSpeedLarge = JsonFloatValue(object, "wave_speed_large", config.waveSpeedLarge);
    config.normalStrength = JsonFloatValue(object, "normal_strength", config.normalStrength);
    config.fresnelPower = JsonFloatValue(object, "fresnel_power", config.fresnelPower);
    config.fresnelMin = JsonFloatValue(object, "fresnel_min", config.fresnelMin);
    ReadFloatArray(object, "reflection_color", config.reflectionColor, 3);
    config.reflectionEnabled = JsonBoolValue(object, "reflection_enabled", config.reflectionEnabled);
    config.reflectionQuality = static_cast<WaterConfig::ReflectionQuality>(
        std::clamp(static_cast<int>(JsonFloatValue(object, "reflection_quality", static_cast<float>(config.reflectionQuality))), 0, 2));
    config.reflectionDistortionStrength = JsonFloatValue(object, "reflection_distortion_strength", config.reflectionDistortionStrength);
    config.refractionEnabled = JsonBoolValue(object, "refraction_enabled", config.refractionEnabled);
    ReadFloatArray(object, "shallow_color", config.shallowColor, 3);
    ReadFloatArray(object, "deep_color", config.deepColor, 3);
    config.depthColorMin = JsonFloatValue(object, "depth_color_min", config.depthColorMin);
    config.depthColorMax = JsonFloatValue(object, "depth_color_max", config.depthColorMax);
    config.depthFadeDistance = JsonFloatValue(object, "depth_fade_distance", config.depthFadeDistance);
    config.refractionStrength = JsonFloatValue(object, "refraction_strength", config.refractionStrength);
    config.refractionDepthStrength = JsonFloatValue(object, "refraction_depth_strength", config.refractionDepthStrength);
    config.foamEnabled = JsonBoolValue(object, "foam_enabled", config.foamEnabled);
    config.foamDistance = JsonFloatValue(object, "foam_distance", config.foamDistance);
    config.foamSoftness = JsonFloatValue(object, "foam_softness", config.foamSoftness);
    config.foamIntensity = JsonFloatValue(object, "foam_intensity", config.foamIntensity);
    config.foamScrollSpeed = JsonFloatValue(object, "foam_scroll_speed", config.foamScrollSpeed);
    config.foamScale = JsonFloatValue(object, "foam_scale", config.foamScale);
    config.foamTerrainThickness = JsonFloatValue(object, "foam_terrain_thickness", config.foamTerrainThickness);
    config.causticMode = static_cast<WaterConfig::CausticMode>(
        std::clamp(static_cast<int>(JsonFloatValue(object, "caustic_mode", static_cast<float>(config.causticMode))), 0, 2));
    config.causticIntensity = JsonFloatValue(object, "caustic_intensity", config.causticIntensity);
    config.causticScale = JsonFloatValue(object, "caustic_scale", config.causticScale);
    config.causticSpeed = JsonFloatValue(object, "caustic_speed", config.causticSpeed);
    config.causticMaxDepth = JsonFloatValue(object, "caustic_max_depth", config.causticMaxDepth);
    config.edgeFadeDistance = JsonFloatValue(object, "edge_fade_distance", config.edgeFadeDistance);
    config.edgeFadeCurve = static_cast<WaterConfig::EdgeFadeCurve>(
        std::clamp(static_cast<int>(JsonFloatValue(object, "edge_fade_curve", static_cast<float>(config.edgeFadeCurve))), 0, 2));
    return ClampWaterConfig(config);
}

WaterMaterialData ReadWaterMaterialJson(const std::string& object, WaterMaterialData fallback = {})
{
    WaterMaterialData material = fallback;
    material.formatVersion = static_cast<std::uint32_t>(
        std::max(1.0f, JsonFloatValue(object, "format_version", static_cast<float>(material.formatVersion))));
    material.normalMapA = JsonNullableStringValue(object, "normal_map_a");
    material.normalMapB = JsonNullableStringValue(object, "normal_map_b");
    material.diffuseMap = JsonNullableStringValue(object, "diffuse_map");
    ReadFloatArray(object, "scroll_speed_a", material.scrollSpeedA, 2);
    ReadFloatArray(object, "scroll_speed_b", material.scrollSpeedB, 2);
    material.normalTiling = JsonFloatValue(object, "normal_tiling", material.normalTiling);
    const std::string configObject = JsonObjectValue(object, "water_config");
    material.config = ReadWaterConfigJson(configObject.empty() ? object : configObject, material.config);
    return ClampWaterMaterialData(material);
}

std::string WaterMaterialFileJson(const AssetLibrary::Entry& entry)
{
    std::ostringstream fileJson;
    fileJson << "{\n"
             << "  \"format_version\": " << entry.waterMaterial.formatVersion << ",\n"
             << "  \"id\": \"" << EscapeJson(entry.id) << "\",\n"
             << "  \"display_name\": \"" << EscapeJson(entry.displayName) << "\",\n"
             << "  \"normal_map_a\": \"" << EscapeJson(entry.waterMaterial.normalMapA) << "\",\n"
             << "  \"normal_map_b\": \"" << EscapeJson(entry.waterMaterial.normalMapB) << "\",\n"
             << "  \"diffuse_map\": \"" << EscapeJson(entry.waterMaterial.diffuseMap) << "\",\n"
             << "  \"scroll_speed_a\": [" << entry.waterMaterial.scrollSpeedA[0] << ", "
             << entry.waterMaterial.scrollSpeedA[1] << "],\n"
             << "  \"scroll_speed_b\": [" << entry.waterMaterial.scrollSpeedB[0] << ", "
             << entry.waterMaterial.scrollSpeedB[1] << "],\n"
             << "  \"normal_tiling\": " << entry.waterMaterial.normalTiling << ",\n"
             << "  \"water_config\": {\n";
    WriteWaterConfigJson(fileJson, entry.waterMaterial.config, "    ");
    fileJson << "\n  }\n}\n";
    return fileJson.str();
}

std::string MaterialFileJson(const AssetLibrary::Entry& entry)
{
    auto tintByte = [](float value) {
        return std::clamp(static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)), 0, 255);
    };
    char tint[16];
    std::snprintf(tint, sizeof(tint), "#%02x%02x%02x",
        tintByte(entry.material.colorTint[0]),
        tintByte(entry.material.colorTint[1]),
        tintByte(entry.material.colorTint[2]));

    std::ostringstream fileJson;
    fileJson << "{\n"
             << "  \"id\": \"" << EscapeJson(entry.id) << "\",\n"
             << "  \"display_name\": \"" << EscapeJson(entry.displayName) << "\",\n"
             << "  \"diffuse_texture_id\": \"" << EscapeJson(entry.material.diffuseTextureId) << "\",\n"
             << "  \"normal_texture_id\": \"" << EscapeJson(entry.material.normalTextureId) << "\",\n"
             << "  \"ao_texture_id\": \"" << EscapeJson(entry.material.aoTextureId) << "\",\n"
             << "  \"roughness_texture_id\": \"" << EscapeJson(entry.material.roughnessTextureId) << "\",\n"
             << "  \"metallic_texture_id\": \"" << EscapeJson(entry.material.metallicTextureId) << "\",\n"
             << "  \"height_texture_id\": \"" << EscapeJson(entry.material.heightTextureId) << "\",\n"
             << "  \"tiling_scale\": { \"x\": " << entry.material.tilingScaleX
             << ", \"y\": " << entry.material.tilingScaleY << " },\n"
             << "  \"color_tint\": \"" << tint << "\",\n"
             << "  \"normal_strength\": " << entry.material.normalStrength << ",\n"
             << "  \"ao_strength\": " << entry.material.aoStrength << ",\n"
             << "  \"roughness_strength\": " << entry.material.roughnessStrength << ",\n"
             << "  \"metallic_strength\": " << entry.material.metallicStrength << "\n"
             << "}\n";
    return fileJson.str();
}

bool HasAnyExtension(const std::filesystem::path& path, std::initializer_list<const char*> extensions)
{
    const std::string ext = ToLower(path.extension().string());
    for (const char* allowed : extensions)
    {
        if (ext == allowed)
            return true;
    }
    return false;
}

std::string GenericPath(const std::filesystem::path& path)
{
    return path.generic_string();
}

bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle)
{
    return needle.empty() || ToLower(haystack).find(ToLower(needle)) != std::string::npos;
}

std::string JoinTagsForSearch(const std::vector<std::string>& tags)
{
    std::string joined;
    for (const std::string& tag : tags)
    {
        if (!joined.empty())
            joined += ' ';
        joined += tag;
    }
    return joined;
}

bool JsonUintPairValue(const std::string& object, const std::string& key, std::uint32_t& x, std::uint32_t& y)
{
    std::string body;
    if (!JsonArrayBody(object, key, body))
        return false;

    std::vector<std::uint32_t> values;
    const char* cursor = body.c_str();
    while (*cursor != '\0' && values.size() < 2)
    {
        char* end = nullptr;
        const unsigned long value = std::strtoul(cursor, &end, 10);
        if (end != cursor)
        {
            values.push_back(static_cast<std::uint32_t>(value));
            cursor = end;
            continue;
        }
        ++cursor;
    }

    if (values.size() != 2)
        return false;
    x = values[0];
    y = values[1];
    return true;
}

bool ReadDdsResolution(const std::filesystem::path& path, std::uint32_t& width, std::uint32_t& height)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;

    char magic[4]{};
    file.read(magic, sizeof(magic));
    if (!file || std::memcmp(magic, "DDS ", 4) != 0)
        return false;

    std::uint32_t headerSize = 0;
    file.read(reinterpret_cast<char*>(&headerSize), sizeof(headerSize));
    if (!file || headerSize != 124)
        return false;

    std::uint32_t flags = 0;
    file.read(reinterpret_cast<char*>(&flags), sizeof(flags));
    file.read(reinterpret_cast<char*>(&height), sizeof(height));
    file.read(reinterpret_cast<char*>(&width), sizeof(width));
    return file && width > 0 && height > 0;
}

bool ReadImageResolution(const std::filesystem::path& path, std::uint32_t& width, std::uint32_t& height)
{
    const std::string ext = ToLower(path.extension().string());
    if (ext == ".dds")
        return ReadDdsResolution(path, width, height);

    int w = 0;
    int h = 0;
    int channels = 0;
    if (!stbi_info(path.string().c_str(), &w, &h, &channels))
        return false;
    if (w <= 0 || h <= 0)
        return false;
    width = static_cast<std::uint32_t>(w);
    height = static_cast<std::uint32_t>(h);
    return true;
}

std::vector<std::uint8_t> ResizeBilinearRgba(const std::uint8_t* pixels,
                                             std::uint32_t srcWidth,
                                             std::uint32_t srcHeight,
                                             std::uint32_t dstWidth,
                                             std::uint32_t dstHeight)
{
    std::vector<std::uint8_t> out(static_cast<size_t>(dstWidth) * dstHeight * 4u);
    if (!pixels || srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0)
        return out;

    const float scaleX = dstWidth > 1 ? static_cast<float>(srcWidth - 1) / static_cast<float>(dstWidth - 1) : 0.0f;
    const float scaleY = dstHeight > 1 ? static_cast<float>(srcHeight - 1) / static_cast<float>(dstHeight - 1) : 0.0f;
    for (std::uint32_t y = 0; y < dstHeight; ++y)
    {
        const float sy = scaleY * static_cast<float>(y);
        const std::uint32_t y0 = static_cast<std::uint32_t>(sy);
        const std::uint32_t y1 = std::min(y0 + 1, srcHeight - 1);
        const float fy = sy - static_cast<float>(y0);
        for (std::uint32_t x = 0; x < dstWidth; ++x)
        {
            const float sx = scaleX * static_cast<float>(x);
            const std::uint32_t x0 = static_cast<std::uint32_t>(sx);
            const std::uint32_t x1 = std::min(x0 + 1, srcWidth - 1);
            const float fx = sx - static_cast<float>(x0);

            const size_t dst = (static_cast<size_t>(y) * dstWidth + x) * 4u;
            const size_t p00 = (static_cast<size_t>(y0) * srcWidth + x0) * 4u;
            const size_t p10 = (static_cast<size_t>(y0) * srcWidth + x1) * 4u;
            const size_t p01 = (static_cast<size_t>(y1) * srcWidth + x0) * 4u;
            const size_t p11 = (static_cast<size_t>(y1) * srcWidth + x1) * 4u;
            for (size_t c = 0; c < 4; ++c)
            {
                const float top = static_cast<float>(pixels[p00 + c]) * (1.0f - fx) + static_cast<float>(pixels[p10 + c]) * fx;
                const float bottom = static_cast<float>(pixels[p01 + c]) * (1.0f - fx) + static_cast<float>(pixels[p11 + c]) * fx;
                out[dst + c] = static_cast<std::uint8_t>(std::clamp(std::lround(top * (1.0f - fy) + bottom * fy), 0l, 255l));
            }
        }
    }
    return out;
}

std::string StemDisplayName(const std::string& filename)
{
    return std::filesystem::path(filename).stem().string();
}
} // namespace

AssetLibrary::AssetLibrary(std::filesystem::path clientRoot)
    : m_clientRoot(std::move(clientRoot))
    , m_libraryRoot(m_clientRoot / "assets" / "library")
{
}

AssetLibrary::AssetLibrary(std::filesystem::path clientRoot, std::filesystem::path libraryRoot)
    : m_clientRoot(std::move(clientRoot))
    , m_libraryRoot(std::move(libraryRoot))
{
}

bool AssetLibrary::Initialize()
{
    if (!EnsureDirectories())
        return false;
    return LoadManifest();
}

bool AssetLibrary::EnsureDirectories() const
{
    std::error_code ec;
    std::filesystem::create_directories(m_libraryRoot / "textures", ec);
    std::filesystem::create_directories(m_libraryRoot / "models", ec);
    std::filesystem::create_directories(m_libraryRoot / "animations", ec);
    std::filesystem::create_directories(m_libraryRoot / "materials", ec);
    std::filesystem::create_directories(m_libraryRoot / "materials" / "water", ec);
    std::filesystem::create_directories(m_libraryRoot / "scenes", ec);
    std::filesystem::create_directories(m_libraryRoot / "thumbnails", ec);
    return !ec;
}

const char* AssetLibrary::CategoryName(Category category)
{
    switch (category)
    {
    case Category::Texture: return "Textures";
    case Category::Model: return "Models";
    case Category::Animation: return "Animations";
    case Category::Material: return "Materials";
    case Category::WaterMaterial: return "Water Materials";
    case Category::Scene: return "Scenes";
    default: return "Assets";
    }
}

const char* AssetLibrary::TextureRoleName(TextureRole role)
{
    switch (role)
    {
    case TextureRole::Diffuse: return "diffuse";
    case TextureRole::Normal: return "normal";
    case TextureRole::Ao: return "ao";
    case TextureRole::Roughness: return "roughness";
    case TextureRole::Metallic: return "metallic";
    case TextureRole::Height: return "height";
    case TextureRole::ArmPacked: return "arm_packed";
    case TextureRole::Unknown: return "unknown";
    default: return "unknown";
    }
}

const char* AssetLibrary::TextureRoleBadge(TextureRole role)
{
    switch (role)
    {
    case TextureRole::Diffuse: return "D";
    case TextureRole::Normal: return "N";
    case TextureRole::Ao: return "AO";
    case TextureRole::Roughness: return "R";
    case TextureRole::Metallic: return "M";
    case TextureRole::Height: return "H";
    case TextureRole::ArmPacked: return "ARM";
    case TextureRole::Unknown: return "?";
    default: return "?";
    }
}

AssetLibrary::TextureRole AssetLibrary::DetectTextureRole(const std::string& filename, std::string* normalConvention)
{
    struct Pattern
    {
        const char* suffix;
        TextureRole role;
        const char* convention;
    };
    static constexpr Pattern patterns[] = {
        {"_ambient_occlusion", TextureRole::Ao, ""},
        {"_ambientocclusion", TextureRole::Ao, ""},
        {"_base_color", TextureRole::Diffuse, ""},
        {"_basecolor", TextureRole::Diffuse, ""},
        {"_displacement", TextureRole::Height, ""},
        {"_normal_gl", TextureRole::Normal, "gl"},
        {"_normalgl", TextureRole::Normal, "gl"},
        {"_normal_dx", TextureRole::Normal, "dx"},
        {"_normaldx", TextureRole::Normal, "dx"},
        {"_occlusion", TextureRole::Ao, ""},
        {"_roughness", TextureRole::Roughness, ""},
        {"_metallic", TextureRole::Metallic, ""},
        {"_diffuse", TextureRole::Diffuse, ""},
        {"_albedo", TextureRole::Diffuse, ""},
        {"_color", TextureRole::Diffuse, ""},
        {"_nor_gl", TextureRole::Normal, "gl"},
        {"_norgl", TextureRole::Normal, "gl"},
        {"_nor_dx", TextureRole::Normal, "dx"},
        {"_nordx", TextureRole::Normal, "dx"},
        {"_normal", TextureRole::Normal, "gl"},
        {"_rough", TextureRole::Roughness, ""},
        {"_metal", TextureRole::Metallic, ""},
        {"_height", TextureRole::Height, ""},
        {"_diff", TextureRole::Diffuse, ""},
        {"_disp", TextureRole::Height, ""},
        {"_hght", TextureRole::Height, ""},
        {"_nrm", TextureRole::Normal, "gl"},
        {"_nor", TextureRole::Normal, "gl"},
        {"_rgh", TextureRole::Roughness, ""},
        {"_met", TextureRole::Metallic, ""},
        {"_col", TextureRole::Diffuse, ""},
        {"_arm", TextureRole::ArmPacked, ""},
        {"_orm", TextureRole::ArmPacked, ""},
        {"_ao", TextureRole::Ao, ""},
        {"_nm", TextureRole::Normal, "gl"},
    };

    const std::string stem = ToLower(std::filesystem::path(filename).stem().string());
    TextureRole role = TextureRole::Unknown;
    std::string convention;
    size_t bestPos = std::string::npos;
    size_t bestLen = 0;
    for (const Pattern& pattern : patterns)
    {
        const std::string suffix(pattern.suffix);
        const size_t pos = stem.rfind(suffix);
        if (pos == std::string::npos)
            continue;
        if (bestPos == std::string::npos || pos > bestPos || (pos == bestPos && suffix.size() > bestLen))
        {
            bestPos = pos;
            bestLen = suffix.size();
            role = pattern.role;
            convention = pattern.convention;
        }
    }

    if (normalConvention)
        *normalConvention = role == TextureRole::Normal ? convention : std::string{};
    return role;
}

bool AssetLibrary::IsValidRenameName(const std::string& name, std::string* error)
{
    if (name.empty())
    {
        if (error) *error = "name is empty";
        return false;
    }
    if (name.size() > 200)
    {
        if (error) *error = "name is too long";
        return false;
    }
    if (name.front() == '.')
    {
        if (error) *error = "name cannot start with a dot";
        return false;
    }
    if (name.back() == '.' || name.back() == ' ')
    {
        if (error) *error = "name cannot end with a dot or space";
        return false;
    }

    for (char c : name)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-' && c != '.')
        {
            if (error) *error = "only A-Z, 0-9, underscore, dash and dot are allowed";
            return false;
        }
    }

    const std::string lower = ToLower(name);
    static const std::unordered_set<std::string> reserved = {
        "con", "prn", "aux", "nul",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
    };
    if (reserved.contains(lower))
    {
        if (error) *error = "reserved Windows device name";
        return false;
    }
    return true;
}

std::string AssetLibrary::NormalizeSubpath(const std::string& value)
{
    std::filesystem::path path;
    std::string segment;
    auto flushSegment = [&]() {
        if (segment.empty() || segment == "." || segment == "..")
        {
            segment.clear();
            return;
        }
        for (char& c : segment)
        {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (!std::isalnum(uc) && c != '_' && c != '-' && c != '.')
                c = '_';
        }
        path /= segment;
        segment.clear();
    };

    for (char c : value)
    {
        if (c == '/' || c == '\\')
            flushSegment();
        else
            segment += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    flushSegment();
    return path.generic_string();
}

std::vector<std::string> AssetLibrary::NormalizeTags(const std::vector<std::string>& tags)
{
    std::vector<std::string> normalized;
    std::unordered_set<std::string> seen;
    for (std::string tag : tags)
    {
        std::string out;
        bool lastWasSeparator = false;
        for (char c : tag)
        {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc))
            {
                out += static_cast<char>(std::tolower(uc));
                lastWasSeparator = false;
            }
            else if (c == '_' || c == '-' || std::isspace(uc))
            {
                if (!lastWasSeparator && !out.empty())
                    out += c == '-' ? '-' : '_';
                lastWasSeparator = true;
            }
        }
        while (!out.empty() && (out.back() == '_' || out.back() == '-'))
            out.pop_back();
        if (!out.empty() && !seen.contains(out))
        {
            seen.insert(out);
            normalized.push_back(out);
        }
    }
    return normalized;
}

std::vector<std::string> AssetLibrary::TagsFromCsv(const std::string& csv)
{
    std::vector<std::string> tags;
    std::string tag;
    for (char c : csv)
    {
        if (c == ',')
        {
            tags.push_back(tag);
            tag.clear();
        }
        else
        {
            tag += c;
        }
    }
    tags.push_back(tag);
    return NormalizeTags(tags);
}

std::string AssetLibrary::TagsToCsv(const std::vector<std::string>& tags)
{
    std::string csv;
    for (const std::string& tag : tags)
    {
        if (!csv.empty())
            csv += ", ";
        csv += tag;
    }
    return csv;
}

std::string AssetLibrary::CategoryString(Category category)
{
    switch (category)
    {
    case Category::Texture: return "texture";
    case Category::Model: return "model";
    case Category::Animation: return "animation";
    case Category::Material: return "material";
    case Category::WaterMaterial: return "water_material";
    case Category::Scene: return "scene";
    default: return "texture";
    }
}

std::optional<AssetLibrary::Category> AssetLibrary::ParseCategory(const std::string& value)
{
    if (value == "texture") return Category::Texture;
    if (value == "model") return Category::Model;
    if (value == "animation") return Category::Animation;
    if (value == "material") return Category::Material;
    if (value == "water_material" || value == "watermaterial") return Category::WaterMaterial;
    if (value == "scene") return Category::Scene;
    return std::nullopt;
}

std::optional<AssetLibrary::TextureRole> AssetLibrary::ParseTextureRole(const std::string& value)
{
    if (value == "diffuse") return TextureRole::Diffuse;
    if (value == "normal") return TextureRole::Normal;
    if (value == "ao") return TextureRole::Ao;
    if (value == "roughness") return TextureRole::Roughness;
    if (value == "metallic") return TextureRole::Metallic;
    if (value == "height") return TextureRole::Height;
    if (value == "arm_packed") return TextureRole::ArmPacked;
    if (value == "unknown") return TextureRole::Unknown;
    return std::nullopt;
}

std::filesystem::path AssetLibrary::CategoryDirectory(Category category) const
{
    switch (category)
    {
    case Category::Texture: return m_libraryRoot / "textures";
    case Category::Model: return m_libraryRoot / "models";
    case Category::Animation: return m_libraryRoot / "animations";
    case Category::Material: return m_libraryRoot / "materials";
    case Category::WaterMaterial: return m_libraryRoot / "materials" / "water";
    case Category::Scene: return m_libraryRoot / "scenes";
    default: return m_libraryRoot / "textures";
    }
}

bool AssetLibrary::GenerateTextureThumbnail(const Entry& entry, std::string& thumbnail, std::string* error) const
{
    if (entry.category != Category::Texture)
        return false;

    const std::filesystem::path source = AbsolutePath(entry);
    const std::filesystem::path thumbnailPath = m_libraryRoot / "thumbnails" / (entry.id + ".png");
    std::error_code ec;
    std::filesystem::create_directories(thumbnailPath.parent_path(), ec);
    if (ec)
    {
        if (error) *error = ec.message();
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(source.string().c_str(), &width, &height, &channels, 4);
    std::uint32_t outWidth = 96;
    std::uint32_t outHeight = 96;
    std::vector<std::uint8_t> output;

    if (decoded && width > 0 && height > 0 && width <= 16384 && height <= 16384)
    {
        const float scale = std::min(256.0f / static_cast<float>(width), 256.0f / static_cast<float>(height));
        outWidth = std::max(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(width) * std::min(scale, 1.0f))));
        outHeight = std::max(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(height) * std::min(scale, 1.0f))));
        output = ResizeBilinearRgba(decoded,
            static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height),
            outWidth,
            outHeight);
        stbi_image_free(decoded);
    }
    else
    {
        if (decoded)
            stbi_image_free(decoded);

        std::array<std::uint8_t, 4> color{130, 140, 150, 255};
        switch (entry.textureRole)
        {
        case TextureRole::Diffuse: color = {80, 170, 100, 255}; break;
        case TextureRole::Normal: color = {115, 95, 210, 255}; break;
        case TextureRole::Ao: color = {125, 125, 125, 255}; break;
        case TextureRole::Roughness: color = {190, 160, 70, 255}; break;
        case TextureRole::Metallic: color = {105, 115, 125, 255}; break;
        case TextureRole::Height: color = {70, 145, 210, 255}; break;
        case TextureRole::ArmPacked: color = {175, 115, 70, 255}; break;
        case TextureRole::Unknown: color = {105, 105, 115, 255}; break;
        }

        output.assign(static_cast<size_t>(outWidth) * outHeight * 4u, 255);
        for (std::uint32_t y = 0; y < outHeight; ++y)
        {
            for (std::uint32_t x = 0; x < outWidth; ++x)
            {
                const bool checker = ((x / 12u) + (y / 12u)) % 2u == 0;
                const size_t dst = (static_cast<size_t>(y) * outWidth + x) * 4u;
                output[dst + 0] = checker ? color[0] : static_cast<std::uint8_t>(color[0] / 2u);
                output[dst + 1] = checker ? color[1] : static_cast<std::uint8_t>(color[1] / 2u);
                output[dst + 2] = checker ? color[2] : static_cast<std::uint8_t>(color[2] / 2u);
                output[dst + 3] = color[3];
            }
        }
    }

    if (!stbi_write_png(thumbnailPath.string().c_str(),
            static_cast<int>(outWidth),
            static_cast<int>(outHeight),
            4,
            output.data(),
            static_cast<int>(outWidth * 4u)))
    {
        Tracenf("[THUMBNAIL] FAILED to write path=%s", thumbnailPath.string().c_str());
        if (error) *error = "failed to write thumbnail";
        return false;
    }

    std::error_code sizeEc;
    const bool exists = std::filesystem::exists(thumbnailPath, sizeEc);
    const auto size = exists ? std::filesystem::file_size(thumbnailPath, sizeEc) : 0;
    Tracenf("[THUMBNAIL] generated path=%s exists=%d size=%zu bytes",
        thumbnailPath.string().c_str(),
        exists ? 1 : 0,
        static_cast<size_t>(sizeEc ? 0 : size));

    thumbnail = GenericPath(std::filesystem::relative(thumbnailPath, m_libraryRoot, ec));
    if (ec)
        thumbnail = std::string("thumbnails/") + entry.id + ".png";
    return true;
}

bool AssetLibrary::PopulateTextureMetadata(Entry& entry, bool generateThumbnail, std::string* error) const
{
    if (entry.category != Category::Texture)
        return false;

    bool changed = false;
    std::string convention;
    const TextureRole detectedRole = DetectTextureRole(entry.filename, &convention);
    if (entry.textureRole != detectedRole)
    {
        entry.textureRole = detectedRole;
        changed = true;
    }
    if (entry.roleDetectedFrom.empty())
    {
        entry.roleDetectedFrom = "filename_suffix";
        changed = true;
    }
    if (entry.normalConvention != convention)
    {
        entry.normalConvention = convention;
        changed = true;
    }

    const std::string stem = StemDisplayName(entry.filename);
    if (!stem.empty() && entry.displayName != stem)
    {
        entry.displayName = stem;
        changed = true;
    }

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (ReadImageResolution(AbsolutePath(entry), width, height))
    {
        if (entry.resolutionWidth != width || entry.resolutionHeight != height)
        {
            entry.resolutionWidth = width;
            entry.resolutionHeight = height;
            changed = true;
        }
    }

    if (generateThumbnail || entry.thumbnail.empty() || !std::filesystem::exists(m_libraryRoot / entry.thumbnail))
    {
        std::string thumbnail;
        if (GenerateTextureThumbnail(entry, thumbnail, error) && entry.thumbnail != thumbnail)
        {
            entry.thumbnail = thumbnail;
            changed = true;
        }
    }

    return changed;
}

bool AssetLibrary::LoadManifest()
{
    m_entries.clear();
    const auto path = m_libraryRoot / "manifest.json";
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::string error;
        return SaveManifest(error);
    }

    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string assetsText;
    if (!JsonArrayBody(text, "assets", assetsText))
        return true;

    bool metadataChanged = false;
    size_t pos = 0;
    while ((pos = assetsText.find('{', pos)) != std::string::npos)
    {
        std::string object;
        size_t end = 0;
        if (!JsonObjectAt(assetsText, pos, object, end))
            break;
        pos = end;

        Entry entry;
        entry.id = JsonStringValue(object, "id");
        const auto category = ParseCategory(JsonStringValue(object, "category"));
        if (!category || entry.id.empty())
            continue;
        entry.category = *category;
        entry.displayName = JsonStringValue(object, "display_name");
        entry.subpath = NormalizeSubpath(JsonStringValue(object, "subpath"));
        entry.filename = JsonStringValue(object, "filename");
        entry.originalPath = JsonStringValue(object, "original_path");
        entry.thumbnail = JsonStringValue(object, "thumbnail");
        entry.importedAt = JsonStringValue(object, "imported_at");
        entry.tags = NormalizeTags(JsonStringArrayValue(object, "tags"));
        if (auto role = ParseTextureRole(JsonStringValue(object, "texture_role")))
            entry.textureRole = *role;
        entry.roleDetectedFrom = JsonStringValue(object, "role_detected_from");
        entry.normalConvention = JsonStringValue(object, "normal_convention");
        JsonUintPairValue(object, "resolution", entry.resolutionWidth, entry.resolutionHeight);
        const std::string materialObject = JsonObjectValue(object, "material_data");
        if (!materialObject.empty())
        {
            entry.material.diffuseTextureId = JsonNullableStringValue(materialObject, "diffuse_texture_id");
            entry.material.normalTextureId = JsonNullableStringValue(materialObject, "normal_texture_id");
            entry.material.aoTextureId = JsonNullableStringValue(materialObject, "ao_texture_id");
            entry.material.roughnessTextureId = JsonNullableStringValue(materialObject, "roughness_texture_id");
            entry.material.metallicTextureId = JsonNullableStringValue(materialObject, "metallic_texture_id");
            entry.material.heightTextureId = JsonNullableStringValue(materialObject, "height_texture_id");
            const std::string tiling = JsonObjectValue(materialObject, "tiling_scale");
            entry.material.tilingScaleX = JsonFloatValue(tiling, "x", 1.0f);
            entry.material.tilingScaleY = JsonFloatValue(tiling, "y", 1.0f);
            entry.material.normalStrength = JsonFloatValue(materialObject, "normal_strength", 1.0f);
            entry.material.aoStrength = JsonFloatValue(materialObject, "ao_strength", 1.0f);
            entry.material.roughnessStrength = JsonFloatValue(materialObject, "roughness_strength", 1.0f);
            entry.material.metallicStrength = JsonFloatValue(materialObject, "metallic_strength", 1.0f);
            const std::string tint = JsonStringValue(materialObject, "color_tint");
            if (tint.size() == 7 && tint[0] == '#')
            {
                const auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return 15;
                };
                entry.material.colorTint[0] = static_cast<float>(hex(tint[1]) * 16 + hex(tint[2])) / 255.0f;
                entry.material.colorTint[1] = static_cast<float>(hex(tint[3]) * 16 + hex(tint[4])) / 255.0f;
                entry.material.colorTint[2] = static_cast<float>(hex(tint[5]) * 16 + hex(tint[6])) / 255.0f;
            }
        }
        if (entry.category == Category::Texture)
            metadataChanged = PopulateTextureMetadata(entry, false) || metadataChanged;
        if (entry.category == Category::WaterMaterial)
        {
            const std::string waterObject = JsonObjectValue(object, "water_material_data");
            if (!waterObject.empty())
                entry.waterMaterial = ReadWaterMaterialJson(waterObject, entry.waterMaterial);
        }
        m_entries.push_back(std::move(entry));
    }

    std::string error;
    ReconcileFilesystem(error);
    if (metadataChanged)
        SaveManifest(error);
    return true;
}

bool AssetLibrary::SaveManifest(std::string& error) const
{
    std::ostringstream json;
    json << "{\n  \"version\": 1,\n  \"assets\": [\n";
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        const Entry& entry = m_entries[i];
        json << "    {\n"
             << "      \"id\": \"" << EscapeJson(entry.id) << "\",\n"
             << "      \"category\": \"" << CategoryString(entry.category) << "\",\n"
             << "      \"display_name\": \"" << EscapeJson(entry.displayName) << "\",\n"
             << "      \"subpath\": \"" << EscapeJson(entry.subpath) << "\",\n"
             << "      \"filename\": \"" << EscapeJson(entry.filename) << "\",\n"
             << "      \"original_path\": \"" << EscapeJson(entry.originalPath) << "\",\n"
             << "      \"thumbnail\": \"" << EscapeJson(entry.thumbnail) << "\",\n"
             << "      \"imported_at\": \"" << EscapeJson(entry.importedAt) << "\",\n"
             << "      \"tags\": [";
        for (size_t tagIndex = 0; tagIndex < entry.tags.size(); ++tagIndex)
        {
            json << "\"" << EscapeJson(entry.tags[tagIndex]) << "\""
                 << (tagIndex + 1 < entry.tags.size() ? ", " : "");
        }
        json << "]\n";
        if (entry.category == Category::Texture)
        {
            json << ",\n"
                 << "      \"texture_role\": \"" << TextureRoleName(entry.textureRole) << "\",\n"
                 << "      \"role_detected_from\": \"" << EscapeJson(entry.roleDetectedFrom.empty() ? "filename_suffix" : entry.roleDetectedFrom) << "\",\n";
            if (entry.normalConvention.empty())
                json << "      \"normal_convention\": null,\n";
            else
                json << "      \"normal_convention\": \"" << EscapeJson(entry.normalConvention) << "\",\n";
            json << "      \"resolution\": [" << entry.resolutionWidth << ", " << entry.resolutionHeight << "]\n";
        }
        if (entry.category == Category::Material)
        {
            auto tintByte = [](float value) {
                return std::clamp(static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)), 0, 255);
            };
            char tint[16];
            std::snprintf(tint, sizeof(tint), "#%02x%02x%02x",
                tintByte(entry.material.colorTint[0]),
                tintByte(entry.material.colorTint[1]),
                tintByte(entry.material.colorTint[2]));
            auto nullable = [&json](const std::string& value) {
                if (value.empty())
                    json << "null";
                else
                    json << "\"" << EscapeJson(value) << "\"";
            };
            json << ",\n"
                 << "      \"material_data\": {\n"
                 << "        \"diffuse_texture_id\": ";
            nullable(entry.material.diffuseTextureId);
            json << ",\n        \"normal_texture_id\": ";
            nullable(entry.material.normalTextureId);
            json << ",\n        \"ao_texture_id\": ";
            nullable(entry.material.aoTextureId);
            json << ",\n        \"roughness_texture_id\": ";
            nullable(entry.material.roughnessTextureId);
            json << ",\n        \"metallic_texture_id\": ";
            nullable(entry.material.metallicTextureId);
            json << ",\n        \"height_texture_id\": ";
            nullable(entry.material.heightTextureId);
            json << ",\n        \"tiling_scale\": { \"x\": " << entry.material.tilingScaleX
                 << ", \"y\": " << entry.material.tilingScaleY << " },\n"
                 << "        \"color_tint\": \"" << tint << "\",\n"
                 << "        \"normal_strength\": " << entry.material.normalStrength << ",\n"
                 << "        \"ao_strength\": " << entry.material.aoStrength << ",\n"
                 << "        \"roughness_strength\": " << entry.material.roughnessStrength << ",\n"
                 << "        \"metallic_strength\": " << entry.material.metallicStrength << "\n"
                 << "      }\n";
        }
        if (entry.category == Category::WaterMaterial)
        {
            json << ",\n"
                 << "      \"water_material_data\": {\n"
                 << "        \"format_version\": " << entry.waterMaterial.formatVersion << ",\n"
                 << "        \"normal_map_a\": \"" << EscapeJson(entry.waterMaterial.normalMapA) << "\",\n"
                 << "        \"normal_map_b\": \"" << EscapeJson(entry.waterMaterial.normalMapB) << "\",\n"
                 << "        \"diffuse_map\": \"" << EscapeJson(entry.waterMaterial.diffuseMap) << "\",\n"
                 << "        \"scroll_speed_a\": [" << entry.waterMaterial.scrollSpeedA[0] << ", "
                 << entry.waterMaterial.scrollSpeedA[1] << "],\n"
                 << "        \"scroll_speed_b\": [" << entry.waterMaterial.scrollSpeedB[0] << ", "
                 << entry.waterMaterial.scrollSpeedB[1] << "],\n"
                 << "        \"normal_tiling\": " << entry.waterMaterial.normalTiling << ",\n"
                 << "        \"water_config\": {\n";
            WriteWaterConfigJson(json, entry.waterMaterial.config, "          ");
            json << "\n        }\n"
                 << "      }\n";
        }
        json << "    }" << (i + 1 < m_entries.size() ? "," : "") << "\n";
    }
    json << "  ]\n}\n";
    return AtomicWriteText(m_libraryRoot / "manifest.json", json.str(), error);
}

bool AssetLibrary::ReconcileFilesystem(std::string& error)
{
    bool changed = false;
    std::vector<Entry> reconciled;
    reconciled.reserve(m_entries.size());

    for (Entry entry : m_entries)
    {
        if (std::filesystem::exists(AbsolutePath(entry)))
        {
            if (entry.category == Category::Texture)
                changed = PopulateTextureMetadata(entry, false, &error) || changed;
            reconciled.push_back(std::move(entry));
            continue;
        }

        std::vector<std::filesystem::path> matches;
        std::error_code ec;
        const auto categoryDir = CategoryDirectory(entry.category);
        if (std::filesystem::exists(categoryDir, ec))
        {
            for (std::filesystem::recursive_directory_iterator it(categoryDir, ec), end; it != end && !ec; it.increment(ec))
            {
                if (it->is_regular_file(ec) && it->path().filename() == entry.filename)
                    matches.push_back(it->path());
            }
        }

        if (matches.size() == 1)
        {
            std::filesystem::path parentRel = std::filesystem::relative(matches.front().parent_path(), categoryDir, ec);
            if (!ec)
                entry.subpath = NormalizeSubpath(parentRel.generic_string());
            if (entry.category == Category::Texture)
                PopulateTextureMetadata(entry, false, &error);
            reconciled.push_back(std::move(entry));
            changed = true;
            continue;
        }

        Tracenf("[ASSET-LIBRARY] removing missing manifest entry id=%s file=%s",
            entry.id.c_str(),
            entry.filename.c_str());
        changed = true;
    }

    if (changed)
    {
        m_entries = std::move(reconciled);
        return SaveManifest(error);
    }
    return true;
}

std::vector<AssetLibrary::Entry> AssetLibrary::EntriesFor(Category category, const std::string& filter) const
{
    std::vector<Entry> result;
    const std::string lowerFilter = ToLower(filter);
    for (const Entry& entry : m_entries)
    {
        if (entry.category != category)
            continue;
        if (!lowerFilter.empty() && ToLower(entry.displayName).find(lowerFilter) == std::string::npos)
            continue;
        result.push_back(entry);
    }
    return result;
}

std::vector<AssetLibrary::Entry> AssetLibrary::QueryEntries(Category category,
                                                            const std::string& subpath,
                                                            bool showAll,
                                                            const std::vector<std::string>& activeTags,
                                                            const std::string& search) const
{
    const std::string normalizedSubpath = NormalizeSubpath(subpath);
    const std::vector<std::string> normalizedTags = NormalizeTags(activeTags);
    const std::string normalizedSearch = ToLower(search);
    std::vector<Entry> result;

    for (const Entry& entry : m_entries)
    {
        if (entry.category != category)
            continue;
        if (!showAll && entry.subpath != normalizedSubpath)
            continue;

        bool hasAllTags = true;
        for (const std::string& tag : normalizedTags)
        {
            if (std::find(entry.tags.begin(), entry.tags.end(), tag) == entry.tags.end())
            {
                hasAllTags = false;
                break;
            }
        }
        if (!hasAllTags)
            continue;

        if (!normalizedSearch.empty() &&
            !ContainsCaseInsensitive(entry.displayName, normalizedSearch) &&
            !ContainsCaseInsensitive(entry.filename, normalizedSearch) &&
            !ContainsCaseInsensitive(TextureRoleName(entry.textureRole), normalizedSearch) &&
            !ContainsCaseInsensitive(JoinTagsForSearch(entry.tags), normalizedSearch))
        {
            continue;
        }

        result.push_back(entry);
    }

    std::sort(result.begin(), result.end(), [](const Entry& a, const Entry& b) {
        if (a.subpath != b.subpath)
            return a.subpath < b.subpath;
        return ToLower(a.displayName) < ToLower(b.displayName);
    });
    return result;
}

std::vector<std::string> AssetLibrary::SubpathsFor(Category category) const
{
    std::set<std::string> paths;
    paths.insert("");
    for (const Entry& entry : m_entries)
    {
        if (entry.category != category)
            continue;
        paths.insert(entry.subpath);
    }
    return {paths.begin(), paths.end()};
}

std::vector<std::string> AssetLibrary::FolderSubpathsFor(Category category) const
{
    std::set<std::string> paths;
    paths.insert("");

    const std::filesystem::path root = CategoryDirectory(category);
    std::error_code ec;
    if (std::filesystem::exists(root, ec))
    {
        for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        {
            if (!it->is_directory(ec))
                continue;
            const std::filesystem::path relative = std::filesystem::relative(it->path(), root, ec);
            if (ec)
                continue;
            const std::string normalized = NormalizeSubpath(relative.generic_string());
            if (!normalized.empty())
                paths.insert(normalized);
        }
    }

    for (const Entry& entry : m_entries)
    {
        if (entry.category != category)
            continue;
        std::string path = NormalizeSubpath(entry.subpath);
        while (!path.empty())
        {
            paths.insert(path);
            path = ParentSubpath(path);
        }
    }

    return {paths.begin(), paths.end()};
}

std::vector<std::pair<std::string, std::uint32_t>> AssetLibrary::TagsFor(Category category) const
{
    std::map<std::string, std::uint32_t> counts;
    for (const Entry& entry : m_entries)
    {
        if (entry.category != category)
            continue;
        for (const std::string& tag : entry.tags)
            ++counts[tag];
    }

    std::vector<std::pair<std::string, std::uint32_t>> tags(counts.begin(), counts.end());
    std::sort(tags.begin(), tags.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second)
            return a.second > b.second;
        return a.first < b.first;
    });
    return tags;
}

std::uint32_t AssetLibrary::CountAssetsIn(Category category, const std::string& subpath) const
{
    const std::string normalizedSubpath = NormalizeSubpath(subpath);
    return static_cast<std::uint32_t>(std::count_if(m_entries.begin(), m_entries.end(), [&](const Entry& entry) {
        return entry.category == category && entry.subpath == normalizedSubpath;
    }));
}

std::optional<AssetLibrary::Entry> AssetLibrary::FindById(const std::string& id) const
{
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&id](const Entry& entry) {
        return entry.id == id;
    });
    if (it == m_entries.end())
        return std::nullopt;
    return *it;
}

bool AssetLibrary::ValidateFile(Category category, const std::filesystem::path& path, std::string& error) const
{
    if (!std::filesystem::is_regular_file(path))
    {
        error = "selected file does not exist";
        return false;
    }

    switch (category)
    {
    case Category::Texture:
        if (!HasAnyExtension(path, {".png", ".jpg", ".jpeg", ".dds", ".tga"}))
        {
            error = "textures must be PNG, JPG, DDS or TGA";
            return false;
        }
        break;
    case Category::Model:
        if (!HasAnyExtension(path, {".gltf", ".glb"}))
        {
            error = "models must be GLTF or GLB";
            return false;
        }
        break;
    case Category::Animation:
        if (!HasAnyExtension(path, {".gltf", ".glb", ".ozz"}))
        {
            error = "animations must be GLTF, GLB, or OZZ";
            return false;
        }
        break;
    case Category::Material:
        if (!HasAnyExtension(path, {".material.json", ".json"}))
        {
            error = "materials must be JSON";
            return false;
        }
        break;
    case Category::WaterMaterial:
        if (!HasAnyExtension(path, {".watermat", ".json"}))
        {
            error = "water materials must be WATERMAT or JSON";
            return false;
        }
        break;
    case Category::Scene:
        if (!HasAnyExtension(path, {".scene"}))
        {
            error = "scenes must be SCENE files";
            return false;
        }
        break;
    }
    return true;
}

std::string AssetLibrary::MakeUniqueId(Category category, const std::filesystem::path& sourcePath) const
{
    const std::string prefix = category == Category::Texture ? "tex_" :
        (category == Category::Model ? "model_" :
            (category == Category::Animation ? "anim_" :
                (category == Category::WaterMaterial ? "watermat_" :
                    (category == Category::Scene ? "scene_" : "mat_"))));
    const std::string base = prefix + SanitizeStem(sourcePath.stem().string());
    std::unordered_set<std::string> existing;
    for (const Entry& entry : m_entries)
        existing.insert(entry.id);
    if (!existing.contains(base))
        return base;
    for (uint32_t i = 2; i < 10000; ++i)
    {
        const std::string candidate = base + "_" + std::to_string(i);
        if (!existing.contains(candidate))
            return candidate;
    }
    return base + "_" + std::to_string(m_entries.size() + 1);
}

std::filesystem::path AssetLibrary::MakeUniqueDestination(Category category,
                                                          const std::string& subpath,
                                                          const std::filesystem::path& sourcePath) const
{
    const auto dir = CategoryDirectory(category) / NormalizeSubpath(subpath);
    const std::string stem = SanitizeStem(sourcePath.stem().string());
    const std::string ext = ToLower(sourcePath.extension().string());
    auto candidate = dir / (stem + ext);
    for (uint32_t i = 2; std::filesystem::exists(candidate); ++i)
        candidate = dir / (stem + "_" + std::to_string(i) + ext);
    return candidate;
}

bool AssetLibrary::Import(Category category, const std::filesystem::path& sourcePath, Entry& outEntry, std::string& error)
{
    return Import(category, sourcePath, ImportOptions{}, outEntry, error);
}

bool AssetLibrary::Import(Category category,
                          const std::filesystem::path& sourcePath,
                          const ImportOptions& options,
                          Entry& outEntry,
                          std::string& error)
{
    if (!ValidateFile(category, sourcePath, error))
        return false;

    const std::string subpath = NormalizeSubpath(options.subpath);
    const auto destination = MakeUniqueDestination(category, subpath, sourcePath);
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }
    std::filesystem::copy_file(sourcePath, destination, std::filesystem::copy_options::none, ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }

    std::vector<std::filesystem::path> copiedDependencyFiles;
    if ((category == Category::Model || category == Category::Animation) &&
        ToLower(sourcePath.extension().string()) == ".gltf" &&
        !CopyGltfExternalDependencies(sourcePath, destination, copiedDependencyFiles, error))
    {
        std::filesystem::remove(destination, ec);
        for (const auto& dependency : copiedDependencyFiles)
            std::filesystem::remove(dependency, ec);
        return false;
    }

    Entry entry;
    entry.id = MakeUniqueId(category, sourcePath);
    entry.category = category;
    entry.displayName = category == Category::Texture
        ? destination.stem().string()
        : (options.displayName.empty() ? sourcePath.stem().string() : options.displayName);
    entry.subpath = subpath;
    entry.filename = destination.filename().generic_string();
    entry.originalPath = GenericPath(sourcePath);
    entry.importedAt = TimestampUtc();
    entry.thumbnail = category == Category::Texture ? "" :
        (category == Category::Model ? "model_icon" :
            (category == Category::Animation ? "animation_icon" :
                (category == Category::WaterMaterial ? "water_material_icon" : "material_icon")));
    entry.tags = NormalizeTags(options.tags);
    if (category == Category::Texture)
    {
        std::string thumbnailError;
        PopulateTextureMetadata(entry, true, &thumbnailError);
        if (entry.thumbnail.empty())
        {
            std::filesystem::remove(destination, ec);
            error = thumbnailError.empty() ? "failed to generate thumbnail" : thumbnailError;
            return false;
        }
    }
    else if (category == Category::WaterMaterial)
    {
        std::ifstream importedFile(destination, std::ios::binary);
        std::string importedText((std::istreambuf_iterator<char>(importedFile)), std::istreambuf_iterator<char>());
        entry.waterMaterial = ReadWaterMaterialJson(importedText);
    }
    m_entries.push_back(entry);

    if (!SaveManifest(error))
    {
        std::filesystem::remove(destination, ec);
        for (const auto& dependency : copiedDependencyFiles)
            std::filesystem::remove(dependency, ec);
        if (category == Category::Texture && !entry.thumbnail.empty())
            std::filesystem::remove(m_libraryRoot / entry.thumbnail, ec);
        m_entries.pop_back();
        return false;
    }

    outEntry = entry;
    return true;
}

bool AssetLibrary::CreateMaterial(const ImportOptions& options,
                                  const MaterialData& material,
                                  Entry& outEntry,
                                  std::string& error)
{
    const std::string displayName = options.displayName.empty() ? "material" : options.displayName;
    const std::string subpath = NormalizeSubpath(options.subpath);
    Entry entry;
    entry.id = MakeUniqueId(Category::Material, displayName);
    entry.category = Category::Material;
    entry.displayName = displayName;
    entry.subpath = subpath;
    entry.filename = SanitizeStem(displayName) + ".material.json";
    entry.originalPath.clear();
    entry.importedAt = TimestampUtc();
    entry.tags = NormalizeTags(options.tags);
    entry.thumbnail = "material_icon";
    entry.material = ClampMaterialData(material);

    std::filesystem::path destination = AbsolutePath(entry);
    for (uint32_t i = 2; std::filesystem::exists(destination); ++i)
    {
        entry.filename = SanitizeStem(displayName) + "_" + std::to_string(i) + ".material.json";
        destination = AbsolutePath(entry);
    }

    if (!AtomicWriteText(destination, MaterialFileJson(entry), error))
        return false;

    m_entries.push_back(entry);
    if (!SaveManifest(error))
    {
        std::error_code ec;
        std::filesystem::remove(destination, ec);
        m_entries.pop_back();
        return false;
    }

    Tracenf("[ASSET-LIBRARY] material saved id=%s diffuse=%s normal=%s",
        entry.id.c_str(),
        entry.material.diffuseTextureId.c_str(),
        entry.material.normalTextureId.c_str());
    outEntry = entry;
    return true;
}

bool AssetLibrary::UpdateMaterial(const std::string& id,
                                  const MaterialData& material,
                                  Entry& outEntry,
                                  std::string& error)
{
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&id](const Entry& entry) {
        return entry.id == id;
    });
    if (it == m_entries.end())
    {
        error = "asset not found";
        return false;
    }
    if (it->category != Category::Material)
    {
        error = "asset is not a material";
        return false;
    }

    const Entry oldEntry = *it;
    Entry updated = oldEntry;
    updated.material = ClampMaterialData(material);
    const std::filesystem::path destination = AbsolutePath(updated);

    std::string oldFileText;
    const bool hadOldFile = std::filesystem::exists(destination);
    if (hadOldFile)
    {
        std::ifstream oldFile(destination, std::ios::binary);
        oldFileText.assign(std::istreambuf_iterator<char>(oldFile), std::istreambuf_iterator<char>());
    }

    if (!AtomicWriteText(destination, MaterialFileJson(updated), error))
        return false;

    *it = updated;
    if (!SaveManifest(error))
    {
        const std::string manifestError = error;
        *it = oldEntry;

        std::string rollbackError;
        bool rolledBack = false;
        if (hadOldFile)
            rolledBack = AtomicWriteText(destination, oldFileText, rollbackError);
        else
        {
            std::error_code ec;
            std::filesystem::remove(destination, ec);
            rolledBack = !ec;
            if (ec)
                rollbackError = ec.message();
        }

        error = "manifest save failed: " + manifestError;
        if (!rolledBack)
            error += "; rollback failed: " + rollbackError;
        return false;
    }

    Tracenf("[ASSET-LIBRARY] material updated id=%s diffuse=%s normal=%s",
        updated.id.c_str(),
        updated.material.diffuseTextureId.c_str(),
        updated.material.normalTextureId.c_str());
    outEntry = updated;
    return true;
}

bool AssetLibrary::CreateWaterMaterial(const ImportOptions& options,
                                       const WaterMaterialData& material,
                                       Entry& outEntry,
                                       std::string& error)
{
    const std::string displayName = options.displayName.empty() ? "Default_Water" : options.displayName;
    const std::string subpath = NormalizeSubpath(options.subpath);
    Entry entry;
    entry.id = MakeUniqueId(Category::WaterMaterial, displayName);
    entry.category = Category::WaterMaterial;
    entry.displayName = displayName;
    entry.subpath = subpath;
    entry.filename = SanitizeStem(displayName) + ".watermat";
    entry.originalPath.clear();
    entry.importedAt = TimestampUtc();
    entry.tags = NormalizeTags(options.tags.empty() ? std::vector<std::string>{"water", "material"} : options.tags);
    entry.thumbnail = "water_material_icon";
    entry.waterMaterial = ClampWaterMaterialData(material);

    std::filesystem::path destination = AbsolutePath(entry);
    for (uint32_t i = 2; std::filesystem::exists(destination); ++i)
    {
        entry.filename = SanitizeStem(displayName) + "_" + std::to_string(i) + ".watermat";
        destination = AbsolutePath(entry);
    }

    if (!AtomicWriteText(destination, WaterMaterialFileJson(entry), error))
        return false;

    m_entries.push_back(entry);
    if (!SaveManifest(error))
    {
        std::error_code ec;
        std::filesystem::remove(destination, ec);
        m_entries.pop_back();
        return false;
    }

    Tracenf("[ASSET-LIBRARY] water material saved id=%s base=(%.2f %.2f %.2f)",
        entry.id.c_str(),
        entry.waterMaterial.config.baseColor[0],
        entry.waterMaterial.config.baseColor[1],
        entry.waterMaterial.config.baseColor[2]);
    outEntry = entry;
    return true;
}

bool AssetLibrary::UpdateWaterMaterial(const std::string& id,
                                       const WaterMaterialData& material,
                                       Entry& outEntry,
                                       std::string& error)
{
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&id](const Entry& entry) {
        return entry.id == id;
    });
    if (it == m_entries.end())
    {
        error = "asset not found";
        return false;
    }
    if (it->category != Category::WaterMaterial)
    {
        error = "asset is not a water material";
        return false;
    }

    const Entry oldEntry = *it;
    Entry updated = oldEntry;
    updated.waterMaterial = ClampWaterMaterialData(material);
    const std::filesystem::path destination = AbsolutePath(updated);

    std::string oldFileText;
    const bool hadOldFile = std::filesystem::exists(destination);
    if (hadOldFile)
    {
        std::ifstream oldFile(destination, std::ios::binary);
        oldFileText.assign(std::istreambuf_iterator<char>(oldFile), std::istreambuf_iterator<char>());
    }

    if (!AtomicWriteText(destination, WaterMaterialFileJson(updated), error))
        return false;

    *it = updated;
    if (!SaveManifest(error))
    {
        const std::string manifestError = error;
        *it = oldEntry;

        std::string rollbackError;
        bool rolledBack = false;
        if (hadOldFile)
            rolledBack = AtomicWriteText(destination, oldFileText, rollbackError);
        else
        {
            std::error_code ec;
            std::filesystem::remove(destination, ec);
            rolledBack = !ec;
            if (ec)
                rollbackError = ec.message();
        }

        error = "manifest save failed: " + manifestError;
        if (!rolledBack)
            error += "; rollback failed: " + rollbackError;
        return false;
    }

    Tracenf("[ASSET-LIBRARY] water material updated id=%s", updated.id.c_str());
    outEntry = updated;
    return true;
}

bool AssetLibrary::Remove(const std::string& id, std::string& error)
{
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&id](const Entry& entry) {
        return entry.id == id;
    });
    if (it == m_entries.end())
    {
        error = "asset not found";
        return false;
    }

    std::error_code ec;
    std::filesystem::remove(AbsolutePath(*it), ec);
    m_entries.erase(it);
    return SaveManifest(error);
}

bool AssetLibrary::UpdateAssetMetadata(const std::string& id,
                                       const std::string& displayName,
                                       const std::vector<std::string>& tags,
                                       std::string& error)
{
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&id](const Entry& entry) {
        return entry.id == id;
    });
    if (it == m_entries.end())
    {
        error = "asset not found";
        return false;
    }

    if (!displayName.empty())
        it->displayName = displayName;
    it->tags = NormalizeTags(tags);
    return SaveManifest(error);
}

bool AssetLibrary::MoveAssetToSubpath(const std::string& id,
                                      const std::string& subpath,
                                      Entry& outEntry,
                                      std::string& error)
{
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&id](const Entry& entry) {
        return entry.id == id;
    });
    if (it == m_entries.end())
    {
        error = "asset not found";
        return false;
    }

    const std::string targetSubpath = NormalizeSubpath(subpath);
    if (it->subpath == targetSubpath)
    {
        outEntry = *it;
        return true;
    }

    const Entry oldEntry = *it;
    Entry moved = oldEntry;
    moved.subpath = targetSubpath;
    const std::filesystem::path source = AbsolutePath(oldEntry);
    const std::filesystem::path destination = AbsolutePath(moved);

    if (!std::filesystem::exists(source))
    {
        error = "source file does not exist";
        return false;
    }
    if (std::filesystem::exists(destination))
    {
        error = "destination already exists";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }

    std::filesystem::rename(source, destination, ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }

    *it = moved;
    if (!SaveManifest(error))
    {
        const std::string manifestError = error;
        std::error_code rollbackEc;
        std::filesystem::create_directories(source.parent_path(), rollbackEc);
        if (!rollbackEc)
            std::filesystem::rename(destination, source, rollbackEc);
        *it = oldEntry;
        error = "manifest save failed: " + manifestError;
        if (rollbackEc)
            error += "; rollback failed: " + rollbackEc.message();
        return false;
    }

    outEntry = moved;
    return true;
}

bool AssetLibrary::RenameAsset(const std::string& id,
                               const std::string& newBaseName,
                               bool allowTextureRoleChange,
                               Entry& outEntry,
                               std::string& error)
{
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&id](const Entry& entry) {
        return entry.id == id;
    });
    if (it == m_entries.end())
    {
        error = "asset not found";
        return false;
    }

    std::string baseName = newBaseName;
    const std::string filenameLower = ToLower(it->filename);
    const std::string oldExtension = it->category == Category::Material && filenameLower.ends_with(".material.json")
        ? ".material.json"
        : (it->category == Category::WaterMaterial && filenameLower.ends_with(".watermat")
            ? ".watermat"
            : ToLower(std::filesystem::path(it->filename).extension().string()));
    std::filesystem::path typedName(baseName);
    const std::string typedNameLower = ToLower(baseName);
    if (oldExtension == ".material.json" && typedNameLower.ends_with(oldExtension))
    {
        baseName.resize(baseName.size() - oldExtension.size());
    }
    else if (ToLower(typedName.extension().string()) == oldExtension && !typedName.stem().string().empty())
    {
        baseName = typedName.stem().string();
    }

    if (!IsValidRenameName(baseName, &error))
        return false;

    const Entry oldEntry = *it;
    Entry renamed = oldEntry;
    renamed.displayName = baseName;
    renamed.filename = baseName + oldExtension;

    if (renamed.filename == oldEntry.filename && renamed.displayName == oldEntry.displayName)
    {
        outEntry = oldEntry;
        return true;
    }

    if (renamed.category == Category::Texture)
    {
        std::string detectedConvention;
        const TextureRole detected = DetectTextureRole(renamed.filename, &detectedConvention);
        if (detected != TextureRole::Unknown && detected != oldEntry.textureRole && !allowTextureRoleChange)
        {
            error = "role suffix suggests " + std::string(TextureRoleName(detected)) +
                "; press Enter again to confirm role change";
            return false;
        }
        if (detected != TextureRole::Unknown)
        {
            renamed.textureRole = detected;
            renamed.normalConvention = detected == TextureRole::Normal ? detectedConvention : std::string{};
            renamed.roleDetectedFrom = renamed.filename;
        }
    }

    const std::string newFilenameLower = ToLower(renamed.filename);
    for (const Entry& entry : m_entries)
    {
        if (entry.id == oldEntry.id ||
            entry.category != oldEntry.category ||
            NormalizeSubpath(entry.subpath) != NormalizeSubpath(oldEntry.subpath))
        {
            continue;
        }
        if (ToLower(entry.filename) == newFilenameLower)
        {
            error = "an asset with this filename already exists in this folder";
            return false;
        }
    }

    const std::filesystem::path source = AbsolutePath(oldEntry);
    const std::filesystem::path destination = AbsolutePath(renamed);
    if (!std::filesystem::exists(source))
    {
        error = "source file does not exist";
        return false;
    }
    if (std::filesystem::exists(destination))
    {
        error = "destination already exists";
        return false;
    }

    std::string oldStructuredText;
    if (oldEntry.category == Category::Material || oldEntry.category == Category::WaterMaterial)
    {
        std::ifstream oldFile(source, std::ios::binary);
        oldStructuredText.assign(std::istreambuf_iterator<char>(oldFile), std::istreambuf_iterator<char>());
    }

    std::error_code ec;
    std::filesystem::rename(source, destination, ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }

    if (renamed.category == Category::Material)
    {
        if (!AtomicWriteText(destination, MaterialFileJson(renamed), error))
        {
            std::error_code rollbackEc;
            std::filesystem::rename(destination, source, rollbackEc);
            if (!oldStructuredText.empty())
            {
                std::string ignored;
                AtomicWriteText(source, oldStructuredText, ignored);
            }
            if (rollbackEc)
                error += "; rollback failed: " + rollbackEc.message();
            return false;
        }
    }
    else if (renamed.category == Category::WaterMaterial)
    {
        if (!AtomicWriteText(destination, WaterMaterialFileJson(renamed), error))
        {
            std::error_code rollbackEc;
            std::filesystem::rename(destination, source, rollbackEc);
            if (!oldStructuredText.empty())
            {
                std::string ignored;
                AtomicWriteText(source, oldStructuredText, ignored);
            }
            if (rollbackEc)
                error += "; rollback failed: " + rollbackEc.message();
            return false;
        }
    }

    *it = renamed;
    if (!SaveManifest(error))
    {
        const std::string manifestError = error;
        std::error_code rollbackEc;
        std::filesystem::rename(destination, source, rollbackEc);
        if ((oldEntry.category == Category::Material || oldEntry.category == Category::WaterMaterial) &&
            !oldStructuredText.empty())
        {
            std::string ignored;
            AtomicWriteText(source, oldStructuredText, ignored);
        }
        *it = oldEntry;
        error = "manifest save failed: " + manifestError;
        if (rollbackEc)
            error += "; rollback failed: " + rollbackEc.message();
        return false;
    }

    outEntry = renamed;
    return true;
}

bool AssetLibrary::RenameFolder(Category category,
                                const std::string& oldSubpath,
                                const std::string& newName,
                                std::string& newSubpath,
                                std::string& error)
{
    const std::string oldPath = NormalizeSubpath(oldSubpath);
    if (oldPath.empty())
    {
        error = "root folder cannot be renamed";
        return false;
    }
    if (!IsValidRenameName(newName, &error))
        return false;

    const std::string parent = ParentSubpath(oldPath);
    const std::string normalizedNewName = NormalizeSubpath(newName);
    if (normalizedNewName.empty() || normalizedNewName.find('/') != std::string::npos)
    {
        error = "invalid folder name";
        return false;
    }
    const std::string targetPath = parent.empty() ? normalizedNewName : parent + "/" + normalizedNewName;
    if (targetPath == oldPath)
    {
        newSubpath = oldPath;
        return true;
    }

    for (const Entry& entry : m_entries)
    {
        if (entry.category == category && IsSubpathInside(NormalizeSubpath(entry.subpath), targetPath))
        {
            error = "a folder with this name already exists";
            return false;
        }
    }

    std::vector<Entry> backup = m_entries;
    std::uint32_t affected = 0;
    for (Entry& entry : m_entries)
    {
        if (entry.category != category)
            continue;
        const std::string subpath = NormalizeSubpath(entry.subpath);
        if (!IsSubpathInside(subpath, oldPath))
            continue;
        entry.subpath = ReplaceSubpathPrefix(subpath, oldPath, targetPath);
        ++affected;
    }
    const std::filesystem::path source = CategoryDirectory(category) / oldPath;
    const std::filesystem::path destination = CategoryDirectory(category) / targetPath;
    if (!std::filesystem::exists(source))
    {
        m_entries = backup;
        error = "source folder does not exist";
        return false;
    }
    if (std::filesystem::exists(destination))
    {
        m_entries = backup;
        error = "destination folder already exists";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec)
    {
        m_entries = backup;
        error = ec.message();
        return false;
    }

    std::filesystem::rename(source, destination, ec);
    if (ec)
    {
        m_entries = backup;
        error = ec.message();
        return false;
    }

    if (!SaveManifest(error))
    {
        const std::string manifestError = error;
        std::error_code rollbackEc;
        std::filesystem::create_directories(source.parent_path(), rollbackEc);
        if (!rollbackEc)
            std::filesystem::rename(destination, source, rollbackEc);
        m_entries = backup;
        error = "manifest save failed: " + manifestError;
        if (rollbackEc)
            error += "; rollback failed: " + rollbackEc.message();
        return false;
    }

    newSubpath = targetPath;
    return true;
}

bool AssetLibrary::CreateFolder(Category category,
                                const std::string& parentSubpath,
                                const std::string& name,
                                std::string& outSubpath,
                                std::string& error)
{
    if (!IsValidRenameName(name, &error))
        return false;

    const std::string parent = NormalizeSubpath(parentSubpath);
    const std::string normalizedName = NormalizeSubpath(name);
    if (normalizedName.empty() || normalizedName.find('/') != std::string::npos)
    {
        error = "invalid folder name";
        return false;
    }

    const std::string target = parent.empty() ? normalizedName : parent + "/" + normalizedName;
    const std::filesystem::path directory = CategoryDirectory(category) / target;
    if (std::filesystem::exists(directory))
    {
        error = "folder already exists";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }

    outSubpath = target;
    return true;
}

bool AssetLibrary::DeleteFolder(Category category,
                                const std::string& subpath,
                                std::uint32_t& removedAssets,
                                std::string& error)
{
    removedAssets = 0;
    const std::string target = NormalizeSubpath(subpath);
    if (target.empty())
    {
        error = "root folder cannot be deleted";
        return false;
    }

    const std::filesystem::path source = CategoryDirectory(category) / target;
    if (!std::filesystem::exists(source))
    {
        error = "folder does not exist";
        return false;
    }

    std::filesystem::path trash = source;
    trash += ".delete_tmp";
    for (int i = 0; std::filesystem::exists(trash) && i < 100; ++i)
    {
        trash = source;
        trash += ".delete_tmp_" + std::to_string(i);
    }
    if (std::filesystem::exists(trash))
    {
        error = "could not reserve temporary delete path";
        return false;
    }

    const std::vector<Entry> backup = m_entries;
    std::vector<Entry> removedEntries;
    auto writeIt = m_entries.begin();
    for (auto readIt = m_entries.begin(); readIt != m_entries.end(); ++readIt)
    {
        if (readIt->category == category && IsSubpathInside(NormalizeSubpath(readIt->subpath), target))
        {
            removedEntries.push_back(*readIt);
            continue;
        }
        if (writeIt != readIt)
            *writeIt = *readIt;
        ++writeIt;
    }
    m_entries.erase(writeIt, m_entries.end());
    removedAssets = static_cast<std::uint32_t>(removedEntries.size());

    std::error_code ec;
    std::filesystem::rename(source, trash, ec);
    if (ec)
    {
        m_entries = backup;
        error = ec.message();
        return false;
    }

    if (!SaveManifest(error))
    {
        const std::string manifestError = error;
        std::error_code rollbackEc;
        std::filesystem::rename(trash, source, rollbackEc);
        m_entries = backup;
        error = "manifest save failed: " + manifestError;
        if (rollbackEc)
            error += "; rollback failed: " + rollbackEc.message();
        return false;
    }

    std::filesystem::remove_all(trash, ec);
    for (const Entry& entry : removedEntries)
    {
        if (!entry.thumbnail.empty())
        {
            std::error_code thumbEc;
            std::filesystem::remove(m_libraryRoot / entry.thumbnail, thumbEc);
        }
    }
    return true;
}

bool AssetLibrary::Refresh(std::string& error)
{
    if (!EnsureDirectories())
    {
        error = "failed to create library directories";
        return false;
    }
    if (!LoadManifest())
    {
        error = "failed to load manifest";
        return false;
    }
    return ReconcileFilesystem(error);
}

std::filesystem::path AssetLibrary::AbsolutePath(const Entry& entry) const
{
    return CategoryDirectory(entry.category) / NormalizeSubpath(entry.subpath) / entry.filename;
}

std::string AssetLibrary::AssetRelativePath(const Entry& entry) const
{
    std::filesystem::path relative = std::filesystem::relative(AbsolutePath(entry), m_clientRoot);
    return relative.generic_string();
}

std::array<MapEditorPaletteSlot, 8> AssetLibrary::LoadWorldPalette(
    const std::string& mapDirectory,
    const std::array<MapEditorPaletteSlot, 8>& defaults) const
{
    auto slots = defaults;
    std::ifstream file(m_clientRoot / mapDirectory / "world_palette.json", std::ios::binary);
    if (!file)
        return slots;

    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string slotsText;
    if (!JsonArrayBody(text, "slots", slotsText))
        return slots;

    size_t pos = 0;
    uint32_t slotIndex = 0;
    while (slotIndex < slots.size() && (pos = slotsText.find('{', pos)) != std::string::npos)
    {
        const size_t end = slotsText.find('}', pos + 1);
        if (end == std::string::npos)
            break;
        const std::string object = slotsText.substr(pos, end - pos + 1);
        pos = end + 1;
        MapEditorPaletteSlot slot = slots[slotIndex];
        slot.slot = slotIndex;
        const std::string assetId = JsonStringValue(object, "asset_id");
        const std::string path = JsonStringValue(object, "texture_path");
        if (!assetId.empty())
        {
            slot.assetId = assetId;
            if (auto entry = FindById(assetId))
            {
                slot.displayName = entry->displayName;
                if (entry->category == Category::Material)
                {
                    if (auto diffuse = FindById(entry->material.diffuseTextureId))
                        slot.texturePath = AssetRelativePath(*diffuse);
                    if (auto normal = FindById(entry->material.normalTextureId))
                        slot.normalTexturePath = AssetRelativePath(*normal);
                    if (auto ao = FindById(entry->material.aoTextureId))
                        slot.aoTexturePath = AssetRelativePath(*ao);
                    if (auto roughness = FindById(entry->material.roughnessTextureId))
                        slot.roughnessTexturePath = AssetRelativePath(*roughness);
                    if (auto metallic = FindById(entry->material.metallicTextureId))
                        slot.metallicTexturePath = AssetRelativePath(*metallic);
                    if (auto height = FindById(entry->material.heightTextureId))
                        slot.heightTexturePath = AssetRelativePath(*height);
                    slot.tilingScaleX = entry->material.tilingScaleX;
                    slot.tilingScaleY = entry->material.tilingScaleY;
                    slot.colorTint[0] = entry->material.colorTint[0];
                    slot.colorTint[1] = entry->material.colorTint[1];
                    slot.colorTint[2] = entry->material.colorTint[2];
                    slot.normalStrength = entry->material.normalStrength;
                    slot.aoStrength = entry->material.aoStrength;
                    slot.roughnessStrength = entry->material.roughnessStrength;
                    slot.metallicStrength = entry->material.metallicStrength;
                }
                else
                {
                    slot.texturePath = AssetRelativePath(*entry);
                    slot.normalTexturePath.clear();
                    slot.aoTexturePath.clear();
                    slot.roughnessTexturePath.clear();
                    slot.metallicTexturePath.clear();
                    slot.heightTexturePath.clear();
                    slot.tilingScaleX = 1.0f;
                    slot.tilingScaleY = 1.0f;
                    slot.colorTint[0] = 1.0f;
                    slot.colorTint[1] = 1.0f;
                    slot.colorTint[2] = 1.0f;
                    slot.normalStrength = 0.0f;
                    slot.aoStrength = 1.0f;
                    slot.roughnessStrength = 1.0f;
                    slot.metallicStrength = 0.0f;
                    slot.uvOffset[0] = 0.0f;
                    slot.uvOffset[1] = 0.0f;
                    slot.uvRotationDegrees = 0.0f;
                }
            }
        }
        if (!path.empty())
            slot.texturePath = path;
        const std::string normalPath = JsonStringValue(object, "normal_texture_path");
        if (!normalPath.empty())
            slot.normalTexturePath = normalPath;
        const std::string aoPath = JsonStringValue(object, "ao_texture_path");
        if (!aoPath.empty())
            slot.aoTexturePath = aoPath;
        const std::string roughnessPath = JsonStringValue(object, "roughness_texture_path");
        if (!roughnessPath.empty())
            slot.roughnessTexturePath = roughnessPath;
        const std::string metallicPath = JsonStringValue(object, "metallic_texture_path");
        if (!metallicPath.empty())
            slot.metallicTexturePath = metallicPath;
        const std::string heightPath = JsonStringValue(object, "height_texture_path");
        if (!heightPath.empty())
            slot.heightTexturePath = heightPath;
        slot.tilingScaleX = JsonFloatValue(object, "tiling_scale_x", slot.tilingScaleX);
        slot.tilingScaleY = JsonFloatValue(object, "tiling_scale_y", slot.tilingScaleY);
        slot.colorTint[0] = JsonFloatValue(object, "tint_r", slot.colorTint[0]);
        slot.colorTint[1] = JsonFloatValue(object, "tint_g", slot.colorTint[1]);
        slot.colorTint[2] = JsonFloatValue(object, "tint_b", slot.colorTint[2]);
        slot.normalStrength = JsonFloatValue(object, "normal_strength", slot.normalStrength);
        slot.aoStrength = JsonFloatValue(object, "ao_strength", slot.aoStrength);
        slot.roughnessStrength = JsonFloatValue(object, "roughness_strength", slot.roughnessStrength);
        slot.metallicStrength = JsonFloatValue(object, "metallic_strength", 0.0f);
        slot.uvOffset[0] = JsonFloatValue(object, "uv_offset_x", slot.uvOffset[0]);
        slot.uvOffset[1] = JsonFloatValue(object, "uv_offset_y", slot.uvOffset[1]);
        slot.uvRotationDegrees = JsonFloatValue(object, "uv_rotation_degrees", slot.uvRotationDegrees);
        const std::string display = JsonStringValue(object, "display_name");
        if (!display.empty())
            slot.displayName = display;
        slots[slotIndex] = slot;
        ++slotIndex;
    }
    return slots;
}

bool AssetLibrary::SaveWorldPalette(const std::string& mapDirectory,
                                    const std::array<MapEditorPaletteSlot, 8>& slots,
                                    std::string& error) const
{
    std::ostringstream json;
    json << "{\n  \"version\": 1,\n  \"slots\": [\n";
    for (size_t i = 0; i < slots.size(); ++i)
    {
        const auto& slot = slots[i];
        json << "    { \"slot\": " << i
             << ", \"asset_id\": \"" << EscapeJson(slot.assetId) << "\""
             << ", \"display_name\": \"" << EscapeJson(slot.displayName) << "\""
             << ", \"texture_path\": \"" << EscapeJson(slot.texturePath) << "\""
             << ", \"normal_texture_path\": \"" << EscapeJson(slot.normalTexturePath) << "\""
             << ", \"ao_texture_path\": \"" << EscapeJson(slot.aoTexturePath) << "\""
             << ", \"roughness_texture_path\": \"" << EscapeJson(slot.roughnessTexturePath) << "\""
             << ", \"metallic_texture_path\": \"" << EscapeJson(slot.metallicTexturePath) << "\""
             << ", \"height_texture_path\": \"" << EscapeJson(slot.heightTexturePath) << "\""
             << ", \"tiling_scale_x\": " << slot.tilingScaleX
             << ", \"tiling_scale_y\": " << slot.tilingScaleY
             << ", \"tint_r\": " << slot.colorTint[0]
             << ", \"tint_g\": " << slot.colorTint[1]
             << ", \"tint_b\": " << slot.colorTint[2]
             << ", \"normal_strength\": " << slot.normalStrength
             << ", \"ao_strength\": " << slot.aoStrength
             << ", \"roughness_strength\": " << slot.roughnessStrength
             << ", \"metallic_strength\": " << slot.metallicStrength
             << ", \"uv_offset_x\": " << slot.uvOffset[0]
             << ", \"uv_offset_y\": " << slot.uvOffset[1]
             << ", \"uv_rotation_degrees\": " << slot.uvRotationDegrees << " }"
             << (i + 1 < slots.size() ? "," : "") << "\n";
    }
    json << "  ]\n}\n";
    return AtomicWriteText(m_clientRoot / mapDirectory / "world_palette.json", json.str(), error);
}
