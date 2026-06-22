#include "AssetDatabase.h"

#include "Common.h"
#include "Debug.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace
{
using ixtreeme::common::EscapeJson;
using ixtreeme::common::JsonStringValue;
using ixtreeme::common::TimestampUtc;
using ixtreeme::common::ToLowerAscii;

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

std::optional<int> JsonIntValue(const std::string& object, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = object.find(needle);
    if (keyPos == std::string::npos)
        return std::nullopt;
    const size_t colon = object.find(':', keyPos + needle.size());
    if (colon == std::string::npos)
        return std::nullopt;
    const char* begin = object.c_str() + colon + 1;
    char* end = nullptr;
    const long value = std::strtol(begin, &end, 10);
    if (end == begin)
        return std::nullopt;
    return static_cast<int>(value);
}

std::vector<std::string> JsonStringArrayValue(const std::string& object, const std::string& key)
{
    std::vector<std::string> values;
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = object.find(needle);
    if (keyPos == std::string::npos)
        return values;
    const size_t begin = object.find('[', keyPos + needle.size());
    if (begin == std::string::npos)
        return values;

    bool inString = false;
    bool escaping = false;
    std::string current;
    for (size_t i = begin + 1; i < object.size(); ++i)
    {
        const char c = object[i];
        if (inString)
        {
            if (escaping)
            {
                current += c;
                escaping = false;
            }
            else if (c == '\\')
            {
                escaping = true;
            }
            else if (c == '"')
            {
                values.push_back(current);
                current.clear();
                inString = false;
            }
            else
            {
                current += c;
            }
            continue;
        }
        if (c == '"')
            inString = true;
        else if (c == ']')
            return values;
    }
    return values;
}

bool HasJsonObjectShape(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    const size_t last = text.find_last_not_of(" \t\r\n");
    return first != std::string::npos && last != std::string::npos && text[first] == '{' && text[last] == '}';
}

std::optional<AssetType> ParseAssetType(const std::string& value)
{
    if (value == "Model")
        return AssetType::Model;
    if (value == "Texture")
        return AssetType::Texture;
    if (value == "Material")
        return AssetType::Material;
    if (value == "Animation")
        return AssetType::Animation;
    if (value == "Scene")
        return AssetType::Scene;
    if (value == "Prefab")
        return AssetType::Prefab;
    if (value == "Project")
        return AssetType::Project;
    return std::nullopt;
}

bool IsIgnoredDirectoryName(const std::filesystem::path& path)
{
    const std::string name = ToLowerAscii(path.filename().string());
    static const std::unordered_set<std::string> ignored = {
        ".git",
        ".vs",
        ".vscode",
        "build",
        "bin",
        "out",
        "cmakefiles",
        ".cache",
    };
    return ignored.contains(name);
}

std::string WithDefaultMaterialsField(std::string text, const std::vector<Guid>& materials)
{
    const std::string fieldName = "\"defaultMaterials\"";
    const size_t fieldPos = text.find(fieldName);
    if (fieldPos != std::string::npos)
    {
        const size_t arrayBegin = text.find('[', fieldPos + fieldName.size());
        if (arrayBegin != std::string::npos)
        {
            int depth = 0;
            bool inString = false;
            bool escaping = false;
            for (size_t i = arrayBegin; i < text.size(); ++i)
            {
                const char c = text[i];
                if (inString)
                {
                    if (escaping) escaping = false;
                    else if (c == '\\') escaping = true;
                    else if (c == '"') inString = false;
                    continue;
                }
                if (c == '"')
                    inString = true;
                else if (c == '[')
                    ++depth;
                else if (c == ']' && --depth == 0)
                {
                    size_t lineEnd = text.find('\n', i);
                    if (lineEnd == std::string::npos)
                        lineEnd = i + 1;
                    text.erase(fieldPos, lineEnd - fieldPos + (lineEnd < text.size() ? 1 : 0));
                    break;
                }
            }
        }
    }

    std::ostringstream field;
    field << "  \"defaultMaterials\": [";
    for (size_t i = 0; i < materials.size(); ++i)
    {
        if (i)
            field << ", ";
        field << "\"" << materials[i].toString() << "\"";
    }
    field << "]";

    const size_t importedAt = text.find("\"importedAt\"");
    if (importedAt != std::string::npos)
    {
        const size_t lineBegin = text.rfind('\n', importedAt);
        const size_t insertPos = lineBegin == std::string::npos ? importedAt : lineBegin + 1;
        text.insert(insertPos, field.str() + ",\n");
        return text;
    }

    const size_t closing = text.find_last_of('}');
    if (closing != std::string::npos)
    {
        size_t insertPos = closing;
        while (insertPos > 0 && (text[insertPos - 1] == '\n' || text[insertPos - 1] == '\r' ||
                                text[insertPos - 1] == ' ' || text[insertPos - 1] == '\t'))
        {
            --insertPos;
        }
        if (insertPos > 0 && text[insertPos - 1] != '{')
            text.insert(insertPos, ",\n" + field.str());
        else
            text.insert(insertPos, "\n" + field.str() + "\n");
    }
    return text;
}

std::string RemoveJsonField(std::string text, const std::string& quotedFieldName)
{
    const size_t fieldPos = text.find(quotedFieldName);
    if (fieldPos == std::string::npos)
        return text;

    const size_t colon = text.find(':', fieldPos + quotedFieldName.size());
    if (colon == std::string::npos)
        return text;

    const size_t valueBegin = text.find_first_not_of(" \t\r\n", colon + 1);
    if (valueBegin == std::string::npos)
        return text;

    size_t valueEnd = valueBegin;
    const char opener = text[valueBegin];
    if (opener == '{' || opener == '[')
    {
        const char closer = opener == '{' ? '}' : ']';
        int depth = 0;
        bool inString = false;
        bool escaping = false;
        for (size_t i = valueBegin; i < text.size(); ++i)
        {
            const char c = text[i];
            if (inString)
            {
                if (escaping) escaping = false;
                else if (c == '\\') escaping = true;
                else if (c == '"') inString = false;
                continue;
            }
            if (c == '"')
                inString = true;
            else if (c == opener)
                ++depth;
            else if (c == closer && --depth == 0)
            {
                valueEnd = i + 1;
                break;
            }
        }
    }
    else if (opener == '"')
    {
        bool escaping = false;
        for (size_t i = valueBegin + 1; i < text.size(); ++i)
        {
            const char c = text[i];
            if (escaping)
            {
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
                valueEnd = i + 1;
                break;
            }
        }
    }
    else
    {
        const size_t comma = text.find(',', valueBegin);
        const size_t newline = text.find('\n', valueBegin);
        valueEnd = std::min(comma == std::string::npos ? text.size() : comma,
            newline == std::string::npos ? text.size() : newline);
    }

    size_t eraseBegin = fieldPos;
    while (eraseBegin > 0 && (text[eraseBegin - 1] == ' ' || text[eraseBegin - 1] == '\t'))
        --eraseBegin;
    if (eraseBegin > 0 && text[eraseBegin - 1] == '\n')
        --eraseBegin;

    size_t eraseEnd = valueEnd;
    while (eraseEnd < text.size() && (text[eraseEnd] == ' ' || text[eraseEnd] == '\t'))
        ++eraseEnd;
    if (eraseEnd < text.size() && text[eraseEnd] == ',')
        ++eraseEnd;
    if (eraseEnd < text.size() && text[eraseEnd] == '\r')
        ++eraseEnd;
    if (eraseEnd < text.size() && text[eraseEnd] == '\n')
        ++eraseEnd;

    text.erase(eraseBegin, eraseEnd - eraseBegin);
    return text;
}

std::string InsertJsonFieldBeforeImportedAt(std::string text, const std::string& field)
{
    const size_t importedAt = text.find("\"importedAt\"");
    if (importedAt != std::string::npos)
    {
        const size_t lineBegin = text.rfind('\n', importedAt);
        const size_t insertPos = lineBegin == std::string::npos ? importedAt : lineBegin + 1;
        text.insert(insertPos, field + ",\n");
        return text;
    }

    const size_t closing = text.find_last_of('}');
    if (closing != std::string::npos)
    {
        size_t insertPos = closing;
        while (insertPos > 0 && (text[insertPos - 1] == '\n' || text[insertPos - 1] == '\r' ||
                                text[insertPos - 1] == ' ' || text[insertPos - 1] == '\t'))
        {
            --insertPos;
        }
        if (insertPos > 0 && text[insertPos - 1] != '{')
            text.insert(insertPos, ",\n" + field);
        else
            text.insert(insertPos, "\n" + field + "\n");
    }
    return text;
}

std::string WithSkeletalAssetField(std::string text,
                                   const Guid& skeletonGuid,
                                   const std::vector<Guid>& animationGuids)
{
    text = RemoveJsonField(std::move(text), "\"skeletalAsset\"");

    std::ostringstream field;
    field << "  \"skeletalAsset\": {\n";
    field << "    \"skeletonGuid\": \"" << skeletonGuid.toString() << "\",\n";
    field << "    \"animationGuids\": [";
    for (size_t i = 0; i < animationGuids.size(); ++i)
    {
        if (i)
            field << ", ";
        field << "\"" << animationGuids[i].toString() << "\"";
    }
    field << "]\n";
    field << "  }";
    return InsertJsonFieldBeforeImportedAt(std::move(text), field.str());
}

std::string WithDependenciesField(std::string text, const std::vector<Guid>& dependencies)
{
    text = RemoveJsonField(std::move(text), "\"dependencies\"");

    std::ostringstream field;
    field << "  \"dependencies\": [";
    for (size_t i = 0; i < dependencies.size(); ++i)
    {
        if (i)
            field << ", ";
        field << "\"" << dependencies[i].toString() << "\"";
    }
    field << "]";
    return InsertJsonFieldBeforeImportedAt(std::move(text), field.str());
}
}

std::string Guid::toString() const
{
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::nouppercase;
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out << '-';
        out << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return out.str();
}

std::optional<Guid> Guid::fromString(const std::string& value)
{
    if (value.size() != 36)
        return std::nullopt;
    if (value[8] != '-' || value[13] != '-' || value[18] != '-' || value[23] != '-')
        return std::nullopt;

    Guid guid;
    size_t byteIndex = 0;
    for (size_t i = 0; i < value.size();)
    {
        if (value[i] == '-')
        {
            ++i;
            continue;
        }
        if (byteIndex >= guid.bytes.size() || i + 1 >= value.size())
            return std::nullopt;
        const int hi = HexValue(value[i]);
        const int lo = HexValue(value[i + 1]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        guid.bytes[byteIndex++] = static_cast<std::uint8_t>((hi << 4) | lo);
        i += 2;
    }
    if (byteIndex != guid.bytes.size())
        return std::nullopt;
    if ((guid.bytes[6] & 0xf0u) != 0x40u)
        return std::nullopt;
    if ((guid.bytes[8] & 0xc0u) != 0x80u)
        return std::nullopt;
    return guid;
}

Guid generateGuidV4()
{
    static thread_local std::mt19937_64 rng([] {
        std::random_device rd;
        std::seed_seq seed{
            rd(), rd(), rd(), rd(),
            static_cast<unsigned int>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count() & 0xffffffffu),
        };
        return std::mt19937_64(seed);
    }());

    Guid guid;
    for (size_t i = 0; i < guid.bytes.size(); i += 8)
    {
        const std::uint64_t value = rng();
        for (size_t j = 0; j < 8 && i + j < guid.bytes.size(); ++j)
            guid.bytes[i + j] = static_cast<std::uint8_t>((value >> (j * 8)) & 0xffu);
    }

    guid.bytes[6] = static_cast<std::uint8_t>((guid.bytes[6] & 0x0fu) | 0x40u);
    guid.bytes[8] = static_cast<std::uint8_t>((guid.bytes[8] & 0x3fu) | 0x80u);
    return guid;
}

AssetType detectAssetType(const std::filesystem::path& filePath)
{
    const std::string ext = ToLowerAscii(filePath.extension().string());
    if (ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj")
        return AssetType::Model;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" ||
        ext == ".bmp" || ext == ".dds" || ext == ".ktx" || ext == ".ktx2" || ext == ".hdr")
        return AssetType::Texture;
    if (ext == ".material")
        return AssetType::Material;
    if (ext == ".anim" || ext == ".ozz")
        return AssetType::Animation;
    if (ext == ".scene")
        return AssetType::Scene;
    if (ext == ".ixprefab")
        return AssetType::Prefab;
    if (ext == ".ixproj")
        return AssetType::Project;
    return AssetType::Unknown;
}

const char* AssetTypeName(AssetType type)
{
    switch (type)
    {
    case AssetType::Model: return "Model";
    case AssetType::Texture: return "Texture";
    case AssetType::Material: return "Material";
    case AssetType::Animation: return "Animation";
    case AssetType::Scene: return "Scene";
    case AssetType::Prefab: return "Prefab";
    case AssetType::Project: return "Project";
    default: return "Unknown";
    }
}

AssetDatabase& AssetDatabase::Instance()
{
    static AssetDatabase database;
    return database;
}

void AssetDatabase::scan(const std::filesystem::path& projectRoot)
{
    const auto begin = std::chrono::steady_clock::now();
    guidToPath_.clear();
    pathToGuid_.clear();
    lastStats_ = {};
    scanRoot_ = canonicalPath(projectRoot);

    Tracenf("[ASSET-DB] scan_start root=%s", scanRoot_.generic_string().c_str());

    std::error_code ec;
    if (!std::filesystem::exists(scanRoot_, ec))
    {
        TraceError("[ASSET-DB] error path=%s reason=scan_root_missing regenerating=no",
            scanRoot_.generic_string().c_str());
        Tracen("[ASSET-DB] scan_done assets=0 metas_existing=0 metas_generated=0 metas_corrupted=0 duration_ms=0");
        return;
    }

    for (std::filesystem::recursive_directory_iterator it(
             scanRoot_,
             std::filesystem::directory_options::skip_permission_denied,
             ec),
         end;
         !ec && it != end;
         it.increment(ec))
    {
        const std::filesystem::directory_entry& entry = *it;
        std::error_code entryEc;
        if (entry.is_directory(entryEc))
        {
            if (IsIgnoredDirectoryName(entry.path()))
                it.disable_recursion_pending();
            continue;
        }

        if (!entry.is_regular_file(entryEc))
            continue;
        const std::filesystem::path assetPath = canonicalPath(entry.path());
        if (ToLowerAscii(assetPath.extension().string()) == ".meta")
            continue;

        const AssetType assetType = detectAssetType(assetPath);
        if (assetType == AssetType::Unknown)
            continue;

        ++lastStats_.assetsScanned;
        const std::filesystem::path metaPath = metaPathFor(assetPath);
        bool regenerate = false;
        Guid guid;
        if (std::filesystem::exists(metaPath, entryEc))
        {
            std::string error;
            const std::optional<MetaRecord> meta = loadMeta(metaPath, assetPath, error);
            if (meta)
            {
                if (meta->assetType != assetType)
                {
                    TraceError("[ASSET-DB] error path=%s reason=asset_type_mismatch regenerating=yes",
                        displayPath(metaPath).c_str());
                    regenerate = true;
                    ++lastStats_.metasCorrupted;
                }
                else if (guidToPath_.contains(meta->guid))
                {
                    TraceError("[ASSET-DB] error path=%s reason=duplicate_guid regenerating=yes",
                        displayPath(metaPath).c_str());
                    regenerate = true;
                    ++lastStats_.metasCorrupted;
                }
                else
                {
                    guid = meta->guid;
                    ++lastStats_.metasExisting;
                    registerAsset(assetPath, guid);
                    Tracenf("[ASSET-DB] guid_load path=%s guid=%s assetType=%s",
                        displayPath(assetPath).c_str(),
                        guid.toString().c_str(),
                        AssetTypeName(assetType));
                }
            }
            else
            {
                TraceError("[ASSET-DB] error path=%s reason=%s regenerating=yes",
                    displayPath(metaPath).c_str(),
                    error.c_str());
                regenerate = true;
                ++lastStats_.metasCorrupted;
            }
        }
        else
        {
            regenerate = true;
        }

        if (regenerate)
        {
            guid = writeNewMeta(assetPath, assetType);
            registerAsset(assetPath, guid);
            ++lastStats_.metasGenerated;
            Tracenf("[ASSET-DB] guid_assign path=%s guid=%s assetType=%s",
                displayPath(assetPath).c_str(),
                guid.toString().c_str(),
                AssetTypeName(assetType));
        }
    }

    if (ec)
    {
        TraceError("[ASSET-DB] error path=%s reason=%s regenerating=no",
            scanRoot_.generic_string().c_str(),
            ec.message().c_str());
    }

    const auto end = std::chrono::steady_clock::now();
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    Tracenf("[ASSET-DB] scan_done assets=%d metas_existing=%d metas_generated=%d metas_corrupted=%d duration_ms=%lld",
        lastStats_.assetsScanned,
        lastStats_.metasExisting,
        lastStats_.metasGenerated,
        lastStats_.metasCorrupted,
        static_cast<long long>(durationMs));
}

std::optional<std::filesystem::path> AssetDatabase::resolveGuid(const Guid& guid) const
{
    const auto it = guidToPath_.find(guid);
    if (it == guidToPath_.end())
        return std::nullopt;
    return it->second;
}

std::optional<Guid> AssetDatabase::resolvePath(const std::filesystem::path& absPath) const
{
    const auto it = pathToGuid_.find(canonicalPath(absPath).generic_string());
    if (it == pathToGuid_.end())
        return std::nullopt;
    return it->second;
}

bool AssetDatabase::runtimeAdd(const std::filesystem::path& absPath)
{
    const std::filesystem::path assetPath = canonicalPath(absPath);
    if (ToLowerAscii(assetPath.extension().string()) == ".meta")
        return false;

    const AssetType assetType = detectAssetType(assetPath);
    if (assetType == AssetType::Unknown)
        return false;

    const bool alreadyRegistered = resolvePath(assetPath).has_value();
    const std::filesystem::path metaPath = metaPathFor(assetPath);
    const bool metaWasExisting = std::filesystem::exists(metaPath);
    Guid guid{};
    bool regenerate = !metaWasExisting;
    if (metaWasExisting)
    {
        std::string error;
        const std::optional<MetaRecord> meta = loadMeta(metaPath, assetPath, error);
        if (!meta || meta->assetType != assetType)
        {
            TraceError("[ASSET-DB] error path=%s reason=%s regenerating=yes",
                displayPath(metaPath).c_str(),
                meta ? "asset_type_mismatch" : error.c_str());
            regenerate = true;
        }
        else
        {
            const auto collision = guidToPath_.find(meta->guid);
            const std::filesystem::path existingPath =
                collision == guidToPath_.end() ? std::filesystem::path{} : canonicalPath(collision->second);
            if (collision != guidToPath_.end() && existingPath != assetPath)
            {
                TraceError("[ASSET-DB] error path=%s reason=guid_collision_with=%s regenerating_guid=yes",
                    displayPath(assetPath).c_str(),
                    displayPath(existingPath).c_str());
                regenerate = true;
            }
            else
            {
                guid = meta->guid;
            }
        }
    }

    if (regenerate)
        guid = writeNewMeta(assetPath, assetType);

    registerAsset(assetPath, guid);
    Tracenf("[ASSET-DB] %s path=%s guid=%s assetType=%s metaWasExisting=%s",
        alreadyRegistered ? "re_added" : "runtime_added",
        displayPath(assetPath).c_str(),
        guid.toString().c_str(),
        AssetTypeName(assetType),
        metaWasExisting ? "yes" : "no");
    return true;
}

bool AssetDatabase::runtimeRemove(const std::filesystem::path& absPath)
{
    const std::filesystem::path path = canonicalPath(absPath);
    if (ToLowerAscii(path.extension().string()) == ".meta")
    {
        const std::filesystem::path assetPath = assetPathForMeta(path);
        if (std::filesystem::exists(assetPath))
        {
            getOrCreateGuid(assetPath);
            Tracenf("[ASSET-DB] meta_orphaned_regenerated path=%s", displayPath(assetPath).c_str());
            return true;
        }
        return false;
    }

    if (detectAssetType(path) == AssetType::Unknown)
        return false;

    const std::optional<Guid> guid = unregisterAsset(path);
    std::error_code ec;
    std::filesystem::remove(metaPathFor(path), ec);
    if (guid)
    {
        Tracenf("[ASSET-DB] runtime_removed path=%s guid=%s",
            displayPath(path).c_str(),
            guid->toString().c_str());
        return true;
    }
    return false;
}

bool AssetDatabase::runtimeMove(const std::filesystem::path& oldAbsPath, const std::filesystem::path& newAbsPath)
{
    const std::filesystem::path oldPath = canonicalPath(oldAbsPath);
    const std::filesystem::path newPath = canonicalPath(newAbsPath);
    if (ToLowerAscii(newPath.extension().string()) == ".meta" ||
        ToLowerAscii(oldPath.extension().string()) == ".meta")
    {
        return false;
    }

    const AssetType assetType = detectAssetType(newPath);
    if (assetType == AssetType::Unknown)
    {
        runtimeRemove(oldPath);
        return false;
    }

    std::optional<Guid> guid = unregisterAsset(oldPath);
    if (!guid)
    {
        if (const std::optional<Guid> existing = resolvePath(newPath))
            guid = existing;
    }
    if (!guid)
    {
        const std::filesystem::path newMetaPath = metaPathFor(newPath);
        if (std::filesystem::exists(newMetaPath))
        {
            std::string error;
            if (const std::optional<MetaRecord> meta = loadMeta(newMetaPath, newPath, error))
                guid = meta->guid;
        }
    }
    if (!guid)
        guid = generateGuidV4();

    const std::filesystem::path oldMetaPath = metaPathFor(oldPath);
    const std::filesystem::path newMetaPath = metaPathFor(newPath);
    std::error_code ec;
    if (!std::filesystem::exists(newMetaPath, ec) && std::filesystem::exists(oldMetaPath, ec))
    {
        std::filesystem::create_directories(newMetaPath.parent_path(), ec);
        ec.clear();
        std::filesystem::rename(oldMetaPath, newMetaPath, ec);
    }
    if (!std::filesystem::exists(newMetaPath, ec))
        writeMeta(newPath, assetType, *guid);

    registerAsset(newPath, *guid);
    Tracenf("[ASSET-DB] runtime_moved guid=%s oldPath=%s newPath=%s",
        guid->toString().c_str(),
        displayPath(oldPath).c_str(),
        displayPath(newPath).c_str());
    return true;
}

bool AssetDatabase::runtimeModified(const std::filesystem::path& absPath)
{
    const std::filesystem::path path = canonicalPath(absPath);
    if (ToLowerAscii(path.extension().string()) == ".meta")
        return false;
    if (detectAssetType(path) == AssetType::Unknown)
        return false;

    const std::optional<Guid> guid = resolvePath(path);
    if (!guid)
        return runtimeAdd(path);

    Tracenf("[ASSET-DB] content_modified path=%s guid=%s (ignored, no DB change)",
        displayPath(path).c_str(),
        guid->toString().c_str());
    return false;
}

Guid AssetDatabase::getOrCreateGuid(const std::filesystem::path& absPath)
{
    const std::filesystem::path assetPath = canonicalPath(absPath);
    if (const std::optional<Guid> existing = resolvePath(assetPath))
        return *existing;

    const AssetType assetType = detectAssetType(assetPath);
    if (assetType == AssetType::Unknown)
        return {};

    std::string error;
    const std::filesystem::path metaPath = metaPathFor(assetPath);
    if (std::filesystem::exists(metaPath))
    {
        if (const std::optional<MetaRecord> meta = loadMeta(metaPath, assetPath, error))
        {
            registerAsset(assetPath, meta->guid);
            return meta->guid;
        }
        TraceError("[ASSET-DB] error path=%s reason=%s regenerating=yes",
            displayPath(metaPath).c_str(),
            error.c_str());
    }

    const Guid guid = writeNewMeta(assetPath, assetType);
    registerAsset(assetPath, guid);
    Tracenf("[ASSET-DB] guid_assign path=%s guid=%s assetType=%s",
        displayPath(assetPath).c_str(),
        guid.toString().c_str(),
        AssetTypeName(assetType));
    return guid;
}

std::vector<Guid> AssetDatabase::loadDefaultMaterials(const std::filesystem::path& modelPath) const
{
    std::vector<Guid> materials;
    const std::filesystem::path assetPath = canonicalPath(modelPath);
    const std::filesystem::path metaPath = metaPathFor(assetPath);
    std::ifstream file(metaPath, std::ios::binary);
    if (!file)
        return materials;
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    for (const std::string& value : JsonStringArrayValue(text, "defaultMaterials"))
    {
        if (const std::optional<Guid> guid = Guid::fromString(value))
            materials.push_back(*guid);
    }
    return materials;
}

bool AssetDatabase::writeDefaultMaterials(const std::filesystem::path& modelPath, const std::vector<Guid>& materials) const
{
    const std::filesystem::path assetPath = canonicalPath(modelPath);
    if (detectAssetType(assetPath) != AssetType::Model)
        return false;

    Guid modelGuid{};
    if (const std::optional<Guid> existing = resolvePath(assetPath))
        modelGuid = *existing;
    else
        modelGuid = const_cast<AssetDatabase*>(this)->getOrCreateGuid(assetPath);

    const std::filesystem::path metaPath = metaPathFor(assetPath);
    std::string text;
    {
        std::ifstream file(metaPath, std::ios::binary);
        if (file)
            text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    if (text.empty())
    {
        std::ostringstream json;
        json << "{\n"
             << "  \"guid\": \"" << modelGuid.toString() << "\",\n"
             << "  \"version\": 1,\n"
             << "  \"assetType\": \"" << AssetTypeName(AssetType::Model) << "\",\n"
             << "  \"importedAt\": \"" << EscapeJson(TimestampUtc()) << "\"\n"
             << "}\n";
        text = json.str();
    }
    text = WithDefaultMaterialsField(text, materials);

    std::ofstream file(metaPath, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        TraceError("[ASSET-DB] error path=%s reason=meta_write_open_failed regenerating=no",
            displayPath(metaPath).c_str());
        return false;
    }
    file << text;
    return true;
}

bool AssetDatabase::writeDependencies(const std::filesystem::path& assetPathInput,
                                      const std::vector<Guid>& dependencies) const
{
    const std::filesystem::path assetPath = canonicalPath(assetPathInput);
    const AssetType assetType = detectAssetType(assetPath);
    if (assetType == AssetType::Unknown)
        return false;

    Guid assetGuid{};
    if (const std::optional<Guid> existing = resolvePath(assetPath))
        assetGuid = *existing;
    else
        assetGuid = const_cast<AssetDatabase*>(this)->getOrCreateGuid(assetPath);

    const std::filesystem::path metaPath = metaPathFor(assetPath);
    std::string text;
    {
        std::ifstream file(metaPath, std::ios::binary);
        if (file)
            text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    if (text.empty())
    {
        std::ostringstream json;
        json << "{\n"
             << "  \"guid\": \"" << assetGuid.toString() << "\",\n"
             << "  \"version\": 1,\n"
             << "  \"assetType\": \"" << AssetTypeName(assetType) << "\",\n"
             << "  \"importedAt\": \"" << EscapeJson(TimestampUtc()) << "\"\n"
             << "}\n";
        text = json.str();
    }

    std::vector<Guid> uniqueDependencies;
    std::unordered_set<Guid> seen;
    uniqueDependencies.reserve(dependencies.size());
    for (const Guid& dependency : dependencies)
    {
        if (dependency == assetGuid || seen.contains(dependency))
            continue;
        seen.insert(dependency);
        uniqueDependencies.push_back(dependency);
    }

    text = WithDependenciesField(std::move(text), uniqueDependencies);
    std::ofstream file(metaPath, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        TraceError("[ASSET-DB] error path=%s reason=meta_write_open_failed regenerating=no",
            displayPath(metaPath).c_str());
        return false;
    }
    file << text;
    Tracenf("[ASSET-DB] dependencies_written path=%s count=%zu",
        displayPath(assetPath).c_str(),
        uniqueDependencies.size());
    return true;
}

bool AssetDatabase::writeSkeletalAsset(const std::filesystem::path& modelPath,
                                       const Guid& skeletonGuid,
                                       const std::vector<Guid>& animationGuids) const
{
    const std::filesystem::path assetPath = canonicalPath(modelPath);
    if (detectAssetType(assetPath) != AssetType::Model)
        return false;

    Guid modelGuid{};
    if (const std::optional<Guid> existing = resolvePath(assetPath))
        modelGuid = *existing;
    else
        modelGuid = const_cast<AssetDatabase*>(this)->getOrCreateGuid(assetPath);

    const std::filesystem::path metaPath = metaPathFor(assetPath);
    std::string text;
    {
        std::ifstream file(metaPath, std::ios::binary);
        if (file)
            text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    if (text.empty())
    {
        std::ostringstream json;
        json << "{\n"
             << "  \"guid\": \"" << modelGuid.toString() << "\",\n"
             << "  \"version\": 1,\n"
             << "  \"assetType\": \"" << AssetTypeName(AssetType::Model) << "\",\n"
             << "  \"importedAt\": \"" << EscapeJson(TimestampUtc()) << "\"\n"
             << "}\n";
        text = json.str();
    }
    text = WithSkeletalAssetField(text, skeletonGuid, animationGuids);

    std::ofstream file(metaPath, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        TraceError("[ASSET-DB] error path=%s reason=meta_write_open_failed regenerating=no",
            displayPath(metaPath).c_str());
        return false;
    }
    file << text;
    return true;
}

std::filesystem::path AssetDatabase::canonicalPath(const std::filesystem::path& path) const
{
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec)
        absolute = path;
    ec.clear();
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, ec);
    return (ec ? absolute : canonical).lexically_normal();
}

std::filesystem::path AssetDatabase::metaPathFor(const std::filesystem::path& assetPath) const
{
    return std::filesystem::path(assetPath.generic_string() + ".meta");
}

std::filesystem::path AssetDatabase::assetPathForMeta(const std::filesystem::path& metaPath) const
{
    std::string value = metaPath.generic_string();
    constexpr const char* suffix = ".meta";
    if (value.size() >= 5 && ToLowerAscii(value.substr(value.size() - 5)) == suffix)
        value.resize(value.size() - 5);
    return canonicalPath(value);
}

std::string AssetDatabase::displayPath(const std::filesystem::path& path) const
{
    if (scanRoot_.empty())
        return path.generic_string();
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(path, scanRoot_, ec);
    return ec ? path.generic_string() : relative.generic_string();
}

std::optional<AssetDatabase::MetaRecord> AssetDatabase::loadMeta(const std::filesystem::path& metaPath,
                                                                 const std::filesystem::path&,
                                                                 std::string& error) const
{
    std::ifstream file(metaPath, std::ios::binary);
    if (!file)
    {
        error = "open_failed";
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!HasJsonObjectShape(text))
    {
        error = "json_parse_failed";
        return std::nullopt;
    }

    const std::string guidText = JsonStringValue(text, "guid");
    const std::optional<Guid> guid = Guid::fromString(guidText);
    if (!guid)
    {
        error = guidText.empty() ? "missing_guid" : "invalid_guid";
        return std::nullopt;
    }

    const std::optional<int> version = JsonIntValue(text, "version");
    if (!version || *version != 1)
    {
        error = version ? "unsupported_version" : "missing_version";
        return std::nullopt;
    }

    const std::string assetTypeText = JsonStringValue(text, "assetType");
    const std::optional<AssetType> assetType = ParseAssetType(assetTypeText);
    if (!assetType)
    {
        error = assetTypeText.empty() ? "missing_assetType" : "invalid_assetType";
        return std::nullopt;
    }

    if (JsonStringValue(text, "importedAt").empty())
    {
        error = "missing_importedAt";
        return std::nullopt;
    }

    return MetaRecord{*guid, *assetType};
}

bool AssetDatabase::writeMeta(const std::filesystem::path& assetPath, AssetType assetType, const Guid& guid) const
{
    const std::filesystem::path metaPath = metaPathFor(assetPath);
    std::error_code ec;
    std::filesystem::create_directories(metaPath.parent_path(), ec);

    std::ostringstream json;
    json << "{\n"
         << "  \"guid\": \"" << guid.toString() << "\",\n"
         << "  \"version\": 1,\n"
         << "  \"assetType\": \"" << AssetTypeName(assetType) << "\",\n"
         << "  \"importedAt\": \"" << EscapeJson(TimestampUtc()) << "\"\n"
         << "}\n";

    std::ofstream file(metaPath, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        TraceError("[ASSET-DB] error path=%s reason=meta_write_open_failed regenerating=no",
            displayPath(metaPath).c_str());
        return false;
    }
    file << json.str();
    return true;
}

Guid AssetDatabase::writeNewMeta(const std::filesystem::path& assetPath, AssetType assetType)
{
    Guid guid = generateGuidV4();
    while (guidToPath_.contains(guid))
        guid = generateGuidV4();

    writeMeta(assetPath, assetType, guid);
    return guid;
}

void AssetDatabase::registerAsset(const std::filesystem::path& assetPath, const Guid& guid)
{
    const std::filesystem::path canonical = canonicalPath(assetPath);
    const std::string key = canonical.generic_string();
    const auto oldPathIt = pathToGuid_.find(key);
    if (oldPathIt != pathToGuid_.end() && oldPathIt->second != guid)
    {
        const auto oldGuidIt = guidToPath_.find(oldPathIt->second);
        if (oldGuidIt != guidToPath_.end() && canonicalPath(oldGuidIt->second) == canonical)
            guidToPath_.erase(oldGuidIt);
    }

    const auto oldGuidIt = guidToPath_.find(guid);
    if (oldGuidIt != guidToPath_.end() && canonicalPath(oldGuidIt->second) != canonical)
        pathToGuid_.erase(canonicalPath(oldGuidIt->second).generic_string());

    guidToPath_[guid] = canonical;
    pathToGuid_[key] = guid;
}

std::optional<Guid> AssetDatabase::unregisterAsset(const std::filesystem::path& assetPath)
{
    const std::filesystem::path canonical = canonicalPath(assetPath);
    const auto pathIt = pathToGuid_.find(canonical.generic_string());
    if (pathIt == pathToGuid_.end())
        return std::nullopt;

    const Guid guid = pathIt->second;
    pathToGuid_.erase(pathIt);
    const auto guidIt = guidToPath_.find(guid);
    if (guidIt != guidToPath_.end() && canonicalPath(guidIt->second) == canonical)
        guidToPath_.erase(guidIt);
    return guid;
}
