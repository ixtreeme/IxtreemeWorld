#include "MaterialAssetManager.h"

#include "Debug.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace
{
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

std::string SanitizeName(std::string value)
{
    for (char& c : value)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc))
            continue;
        c = '_';
    }
    while (!value.empty() && value.back() == '_')
        value.pop_back();
    return value.empty() ? "material" : value;
}

const char* AlphaModeName(MaterialAsset::AlphaMode mode)
{
    switch (mode)
    {
    case MaterialAsset::AlphaMode::Mask: return "mask";
    case MaterialAsset::AlphaMode::Blend: return "blend";
    default: return "opaque";
    }
}

MaterialAsset::AlphaMode ParseAlphaMode(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (value == "mask")
        return MaterialAsset::AlphaMode::Mask;
    if (value == "blend")
        return MaterialAsset::AlphaMode::Blend;
    return MaterialAsset::AlphaMode::Opaque;
}

std::string GuidOrNull(const std::optional<Guid>& guid)
{
    return guid ? ("\"" + guid->toString() + "\"") : "null";
}

std::string FloatValue(float value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;
    std::string text = out.str();
    while (text.size() > 1 && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text += '0';
    return text;
}

template <size_t N>
std::string FloatArray(const std::array<float, N>& values)
{
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i)
            out << ", ";
        out << FloatValue(values[i]);
    }
    out << "]";
    return out.str();
}

std::string MaterialJson(const MaterialAsset& material)
{
    std::ostringstream json;
    json << "{\n"
         << "  \"version\": 1,\n"
         << "  \"shader\": \"PBR-Standard\",\n"
         << "  \"name\": \"" << EscapeJson(material.name) << "\",\n"
         << "  \"baseColor\": " << FloatArray(material.baseColor) << ",\n"
         << "  \"metallic\": " << FloatValue(material.metallic) << ",\n"
         << "  \"roughness\": " << FloatValue(material.roughness) << ",\n"
         << "  \"normalStrength\": " << FloatValue(material.normalStrength) << ",\n"
         << "  \"aoStrength\": " << FloatValue(material.aoStrength) << ",\n"
         << "  \"emissive\": " << FloatArray(material.emissive) << ",\n"
         << "  \"uvTiling\": " << FloatArray(material.uvTiling) << ",\n"
         << "  \"uvOffset\": " << FloatArray(material.uvOffset) << ",\n"
         << "  \"alphaMode\": \"" << AlphaModeName(material.alphaMode) << "\",\n"
         << "  \"alphaCutoff\": " << FloatValue(material.alphaCutoff) << ",\n"
         << "  \"textures\": {\n"
         << "    \"baseColor\": " << GuidOrNull(material.baseColorTexture) << ",\n"
         << "    \"normal\": " << GuidOrNull(material.normalTexture) << ",\n"
         << "    \"metallicRoughness\": " << GuidOrNull(material.metallicRoughnessTexture) << ",\n"
         << "    \"ao\": " << GuidOrNull(material.aoTexture) << ",\n"
         << "    \"emissive\": " << GuidOrNull(material.emissiveTexture) << "\n"
         << "  }\n"
         << "}\n";
    return json.str();
}

std::string JsonStringValue(const std::string& object, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = object.find(needle);
    if (keyPos == std::string::npos)
        return {};
    const size_t colon = object.find(':', keyPos + needle.size());
    if (colon == std::string::npos)
        return {};
    const size_t firstQuote = object.find('"', colon + 1);
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
            return out;
        out += c;
    }
    return {};
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
            if (escaping) escaping = false;
            else if (c == '\\') escaping = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return object.substr(begin, i - begin + 1);
    }
    return {};
}

template <size_t N>
void ReadFloatArray(const std::string& object, const std::string& key, std::array<float, N>& values)
{
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = object.find(needle);
    if (keyPos == std::string::npos)
        return;
    const size_t begin = object.find('[', keyPos + needle.size());
    const size_t end = object.find(']', begin == std::string::npos ? keyPos : begin);
    if (begin == std::string::npos || end == std::string::npos)
        return;
    std::string body = object.substr(begin + 1, end - begin - 1);
    const char* cursor = body.c_str();
    for (float& value : values)
    {
        char* next = nullptr;
        const float parsed = std::strtof(cursor, &next);
        if (next == cursor)
            return;
        value = parsed;
        cursor = next;
        while (*cursor == ',' || std::isspace(static_cast<unsigned char>(*cursor)))
            ++cursor;
    }
}

std::optional<Guid> JsonGuidOrNull(const std::string& object, const std::string& key)
{
    const std::string text = JsonStringValue(object, key);
    if (text.empty())
        return std::nullopt;
    return Guid::fromString(text);
}

std::optional<Guid> ResolveTextureGuid(AssetDatabase& db,
                                       const std::filesystem::path& path,
                                       const char* slot,
                                       const std::string& materialName)
{
    if (path.empty())
        return std::nullopt;
    if (const std::optional<Guid> guid = db.resolvePath(path))
        return guid;
    Tracenf("[MATERIAL-ASSET] warn texture_not_in_db path=%s slot=%s materialName=%s",
        path.generic_string().c_str(),
        slot,
        materialName.c_str());
    return std::nullopt;
}

bool SameTextFile(const std::filesystem::path& path, const std::string& expected)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    const std::string existing((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return existing == expected;
}
}

MaterialAssetManager& MaterialAssetManager::Instance()
{
    static MaterialAssetManager manager(AssetDatabase::Instance());
    return manager;
}

MaterialAssetManager::MaterialAssetManager(AssetDatabase& db)
    : db_(db)
{
}

MaterialAsset* MaterialAssetManager::getOrLoad(const Guid& guid)
{
    if (auto it = cache_.find(guid); it != cache_.end())
        return it->second.get();

    const std::optional<std::filesystem::path> path = db_.resolveGuid(guid);
    if (!path)
        return nullptr;

    std::ifstream file(*path, std::ios::binary);
    if (!file)
    {
        TraceError("[MATERIAL-ASSET] error path=%s reason=open_failed", path->generic_string().c_str());
        return nullptr;
    }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    auto material = std::make_unique<MaterialAsset>();
    material->guid = guid;
    material->path = *path;
    material->name = JsonStringValue(text, "name");
    ReadFloatArray(text, "baseColor", material->baseColor);
    material->metallic = JsonFloatValue(text, "metallic", material->metallic);
    material->roughness = JsonFloatValue(text, "roughness", material->roughness);
    material->normalStrength = JsonFloatValue(text, "normalStrength", material->normalStrength);
    material->aoStrength = JsonFloatValue(text, "aoStrength", material->aoStrength);
    ReadFloatArray(text, "emissive", material->emissive);
    ReadFloatArray(text, "uvTiling", material->uvTiling);
    ReadFloatArray(text, "uvOffset", material->uvOffset);
    material->alphaMode = ParseAlphaMode(JsonStringValue(text, "alphaMode"));
    material->alphaCutoff = JsonFloatValue(text, "alphaCutoff", material->alphaCutoff);

    const std::string textures = JsonObjectValue(text, "textures");
    material->baseColorTexture = JsonGuidOrNull(textures, "baseColor");
    material->normalTexture = JsonGuidOrNull(textures, "normal");
    material->metallicRoughnessTexture = JsonGuidOrNull(textures, "metallicRoughness");
    material->aoTexture = JsonGuidOrNull(textures, "ao");
    material->emissiveTexture = JsonGuidOrNull(textures, "emissive");

    Tracenf("[MATERIAL-ASSET] load OK path=%s guid=%s",
        path->generic_string().c_str(),
        guid.toString().c_str());
    MaterialAsset* result = material.get();
    cache_[guid] = std::move(material);
    return result;
}

bool MaterialAssetManager::save(const MaterialAsset& material)
{
    std::error_code ec;
    std::filesystem::create_directories(material.path.parent_path(), ec);
    std::ofstream file(material.path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        TraceError("[MATERIAL-ASSET] error path=%s reason=open_write_failed",
            material.path.generic_string().c_str());
        return false;
    }
    file << MaterialJson(material);
    Tracenf("[MATERIAL-ASSET] save OK path=%s guid=%s",
        material.path.generic_string().c_str(),
        material.guid.toString().c_str());
    return true;
}

Guid MaterialAssetManager::createFromGltfMaterial(const GltfMaterialSource& gltfMat,
                                                  const std::filesystem::path& materialFolder,
                                                  const std::string& materialName,
                                                  ImportSummary* summary)
{
    MaterialAsset material{};
    material.name = materialName.empty() ? gltfMat.name : materialName;
    material.name = SanitizeName(material.name.empty() ? "material" : material.name);
    material.baseColor = gltfMat.baseColor;
    material.metallic = gltfMat.metallic;
    material.roughness = gltfMat.roughness;
    material.normalStrength = gltfMat.normalStrength;
    material.aoStrength = gltfMat.aoStrength;
    material.emissive = gltfMat.emissive;
    material.alphaMode = ParseAlphaMode(gltfMat.alphaMode);
    material.alphaCutoff = gltfMat.alphaCutoff;
    material.baseColorTexture = ResolveTextureGuid(db_, gltfMat.baseColorTexturePath, "baseColor", material.name);
    material.normalTexture = ResolveTextureGuid(db_, gltfMat.normalTexturePath, "normal", material.name);
    material.metallicRoughnessTexture =
        ResolveTextureGuid(db_, gltfMat.metallicRoughnessTexturePath, "metallicRoughness", material.name);
    material.aoTexture = ResolveTextureGuid(db_, gltfMat.aoTexturePath, "ao", material.name);
    material.emissiveTexture = ResolveTextureGuid(db_, gltfMat.emissiveTexturePath, "emissive", material.name);

    std::error_code ec;
    std::filesystem::create_directories(materialFolder, ec);
    const std::string baseFilename = SanitizeName(material.name);
    material.path = materialFolder / (baseFilename + ".material");

    std::string content = MaterialJson(material);
    if (std::filesystem::exists(material.path) && SameTextFile(material.path, content))
    {
        material.guid = db_.getOrCreateGuid(material.path);
        if (summary) ++summary->reused;
        Tracenf("[MATERIAL-ASSET] reused existing path=%s guid=%s (content matches)",
            material.path.generic_string().c_str(),
            material.guid.toString().c_str());
        return material.guid;
    }

    if (std::filesystem::exists(material.path))
    {
        for (int version = 2; version < 10000; ++version)
        {
            const std::filesystem::path candidate =
                materialFolder / (baseFilename + "_v" + std::to_string(version) + ".material");
            material.path = candidate;
            content = MaterialJson(material);
            if (!std::filesystem::exists(candidate))
            {
                if (summary) ++summary->conflicts;
                Tracenf("[MATERIAL-ASSET] conflict existing=%s generated=%s reason=content_differs",
                    (baseFilename + ".material").c_str(),
                    candidate.filename().generic_string().c_str());
                break;
            }
            if (SameTextFile(candidate, content))
            {
                material.guid = db_.getOrCreateGuid(candidate);
                if (summary) ++summary->reused;
                Tracenf("[MATERIAL-ASSET] reused existing path=%s guid=%s (content matches)",
                    candidate.generic_string().c_str(),
                    material.guid.toString().c_str());
                return material.guid;
            }
        }
    }

    material.guid = db_.getOrCreateGuid(material.path);
    if (!save(material))
        return {};
    if (summary) ++summary->generated;

    const std::string baseColorTexture = material.baseColorTexture ? material.baseColorTexture->toString() : "null";
    const std::string normalTexture = material.normalTexture ? material.normalTexture->toString() : "null";
    const std::string metallicRoughnessTexture =
        material.metallicRoughnessTexture ? material.metallicRoughnessTexture->toString() : "null";
    const std::string aoTexture = material.aoTexture ? material.aoTexture->toString() : "null";
    const std::string emissiveTexture = material.emissiveTexture ? material.emissiveTexture->toString() : "null";
    Tracenf("[MATERIAL-ASSET] generated path=%s guid=%s textures=[baseColor=%s,normal=%s,metallicRoughness=%s,ao=%s,emissive=%s]",
        material.path.generic_string().c_str(),
        material.guid.toString().c_str(),
        baseColorTexture.c_str(),
        normalTexture.c_str(),
        metallicRoughnessTexture.c_str(),
        aoTexture.c_str(),
        emissiveTexture.c_str());
    return material.guid;
}

void MaterialAssetManager::invalidate(const Guid& guid)
{
    cache_.erase(guid);
}
