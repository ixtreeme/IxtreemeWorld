#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>

struct Guid
{
    std::array<std::uint8_t, 16> bytes{};

    std::string toString() const;
    static std::optional<Guid> fromString(const std::string& value);

    bool operator==(const Guid& other) const { return bytes == other.bytes; }
    bool operator!=(const Guid& other) const { return !(*this == other); }
};

namespace std
{
template<>
struct hash<Guid>
{
    std::size_t operator()(const Guid& guid) const noexcept
    {
        std::size_t value = 1469598103934665603ull;
        for (std::uint8_t byte : guid.bytes)
        {
            value ^= static_cast<std::size_t>(byte);
            value *= 1099511628211ull;
        }
        return value;
    }
};
}

Guid generateGuidV4();

enum class AssetType
{
    Model,
    Texture,
    Material,
    Animation,
    Scene,
    Prefab,
    Project,
    AnimationClip,
    AnimatorController,
    AudioClip,
    Unknown
};

AssetType detectAssetType(const std::filesystem::path& filePath);
const char* AssetTypeName(AssetType type);

class AssetDatabase
{
public:
    struct ScanStats
    {
        int assetsScanned = 0;
        int metasExisting = 0;
        int metasGenerated = 0;
        int metasCorrupted = 0;
    };

    static AssetDatabase& Instance();

    void scan(const std::filesystem::path& projectRoot);

    std::optional<std::filesystem::path> resolveGuid(const Guid& guid) const;
    std::optional<Guid> resolvePath(const std::filesystem::path& absPath) const;
    Guid getOrCreateGuid(const std::filesystem::path& absPath);
    std::vector<Guid> loadDefaultMaterials(const std::filesystem::path& modelPath) const;
    bool writeDefaultMaterials(const std::filesystem::path& modelPath, const std::vector<Guid>& materials) const;
    bool writeDependencies(const std::filesystem::path& assetPath, const std::vector<Guid>& dependencies) const;
    bool writeSkeletalAsset(const std::filesystem::path& modelPath,
                            const Guid& skeletonGuid,
                            const std::vector<Guid>& animationGuids) const;
    ScanStats lastScanStats() const { return lastStats_; }

    bool runtimeAdd(const std::filesystem::path& absPath);
    bool runtimeRemove(const std::filesystem::path& absPath);
    bool runtimeMove(const std::filesystem::path& oldAbsPath, const std::filesystem::path& newAbsPath);
    bool runtimeModified(const std::filesystem::path& absPath);

private:
    struct MetaRecord
    {
        Guid guid;
        AssetType assetType = AssetType::Unknown;
    };

    std::filesystem::path canonicalPath(const std::filesystem::path& path) const;
    std::filesystem::path metaPathFor(const std::filesystem::path& assetPath) const;
    std::filesystem::path assetPathForMeta(const std::filesystem::path& metaPath) const;
    std::string displayPath(const std::filesystem::path& path) const;
    std::optional<MetaRecord> loadMeta(const std::filesystem::path& metaPath,
                                       const std::filesystem::path& assetPath,
                                       std::string& error) const;
    bool writeMeta(const std::filesystem::path& assetPath, AssetType assetType, const Guid& guid) const;
    Guid writeNewMeta(const std::filesystem::path& assetPath, AssetType assetType);
    void registerAsset(const std::filesystem::path& assetPath, const Guid& guid);
    std::optional<Guid> unregisterAsset(const std::filesystem::path& assetPath);

    std::unordered_map<Guid, std::filesystem::path> guidToPath_;
    std::unordered_map<std::string, Guid> pathToGuid_;
    ScanStats lastStats_;
    std::filesystem::path scanRoot_;
};
