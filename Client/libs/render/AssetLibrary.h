#pragma once

#include "MapEditorTypes.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class AssetLibrary
{
public:
    enum class Category
    {
        Texture,
        Model,
        Animation,
        Material,
        WaterMaterial
    };

    enum class TextureRole
    {
        Diffuse,
        Normal,
        Ao,
        Roughness,
        Metallic,
        Height,
        ArmPacked,
        Unknown
    };

    struct MaterialData
    {
        std::string diffuseTextureId;
        std::string normalTextureId;
        std::string aoTextureId;
        std::string roughnessTextureId;
        std::string metallicTextureId;
        std::string heightTextureId;
        float tilingScaleX = 1.0f;
        float tilingScaleY = 1.0f;
        float colorTint[3] = {1.0f, 1.0f, 1.0f};
        float normalStrength = 1.0f;
        float aoStrength = 1.0f;
        float roughnessStrength = 1.0f;
        float metallicStrength = 1.0f;
    };

    struct Entry
    {
        std::string id;
        Category category = Category::Texture;
        std::string displayName;
        std::string subpath;
        std::string filename;
        std::string originalPath;
        std::string thumbnail;
        std::string importedAt;
        std::vector<std::string> tags;
        TextureRole textureRole = TextureRole::Unknown;
        std::string roleDetectedFrom;
        std::string normalConvention;
        std::uint32_t resolutionWidth = 0;
        std::uint32_t resolutionHeight = 0;
        MaterialData material;
        WaterMaterialData waterMaterial;
    };

    struct ImportOptions
    {
        std::string displayName;
        std::string subpath;
        std::vector<std::string> tags;
    };

    explicit AssetLibrary(std::filesystem::path clientRoot);

    bool Initialize();
    const std::vector<Entry>& Entries() const { return m_entries; }
    std::vector<Entry> EntriesFor(Category category, const std::string& filter = {}) const;
    std::vector<Entry> QueryEntries(Category category,
                                    const std::string& subpath,
                                    bool showAll,
                                    const std::vector<std::string>& activeTags,
                                    const std::string& search) const;
    std::optional<Entry> FindById(const std::string& id) const;
    std::vector<std::string> SubpathsFor(Category category) const;
    std::vector<std::string> FolderSubpathsFor(Category category) const;
    std::vector<std::pair<std::string, std::uint32_t>> TagsFor(Category category) const;
    std::uint32_t CountAssetsIn(Category category, const std::string& subpath) const;

    bool Import(Category category, const std::filesystem::path& sourcePath, Entry& outEntry, std::string& error);
    bool Import(Category category,
                const std::filesystem::path& sourcePath,
                const ImportOptions& options,
                Entry& outEntry,
                std::string& error);
    bool CreateMaterial(const ImportOptions& options,
                        const MaterialData& material,
                        Entry& outEntry,
                        std::string& error);
    bool UpdateMaterial(const std::string& id,
                        const MaterialData& material,
                        Entry& outEntry,
                        std::string& error);
    bool CreateWaterMaterial(const ImportOptions& options,
                             const WaterMaterialData& material,
                             Entry& outEntry,
                             std::string& error);
    bool UpdateWaterMaterial(const std::string& id,
                             const WaterMaterialData& material,
                             Entry& outEntry,
                             std::string& error);
    bool Remove(const std::string& id, std::string& error);
    bool UpdateAssetMetadata(const std::string& id,
                             const std::string& displayName,
                             const std::vector<std::string>& tags,
                             std::string& error);
    bool MoveAssetToSubpath(const std::string& id, const std::string& subpath, Entry& outEntry, std::string& error);
    bool RenameAsset(const std::string& id,
                     const std::string& newBaseName,
                     bool allowTextureRoleChange,
                     Entry& outEntry,
                     std::string& error);
    bool RenameFolder(Category category,
                      const std::string& oldSubpath,
                      const std::string& newName,
                      std::string& newSubpath,
                      std::string& error);
    bool CreateFolder(Category category,
                      const std::string& parentSubpath,
                      const std::string& name,
                      std::string& outSubpath,
                      std::string& error);
    bool DeleteFolder(Category category,
                      const std::string& subpath,
                      std::uint32_t& removedAssets,
                      std::string& error);
    bool Refresh(std::string& error);

    std::filesystem::path AbsolutePath(const Entry& entry) const;
    std::string AssetRelativePath(const Entry& entry) const;
    const std::filesystem::path& LibraryRoot() const { return m_libraryRoot; }

    std::array<MapEditorPaletteSlot, 8> LoadWorldPalette(const std::string& mapDirectory,
                                                         const std::array<MapEditorPaletteSlot, 8>& defaults) const;
    bool SaveWorldPalette(const std::string& mapDirectory,
                          const std::array<MapEditorPaletteSlot, 8>& slots,
                          std::string& error) const;

    static const char* CategoryName(Category category);
    static const char* TextureRoleName(TextureRole role);
    static const char* TextureRoleBadge(TextureRole role);
    static TextureRole DetectTextureRole(const std::string& filename, std::string* normalConvention = nullptr);
    static bool IsValidRenameName(const std::string& name, std::string* error = nullptr);
    static std::string NormalizeSubpath(const std::string& value);
    static std::vector<std::string> NormalizeTags(const std::vector<std::string>& tags);
    static std::vector<std::string> TagsFromCsv(const std::string& csv);
    static std::string TagsToCsv(const std::vector<std::string>& tags);

private:
    std::filesystem::path CategoryDirectory(Category category) const;
    static std::string CategoryString(Category category);
    static std::optional<Category> ParseCategory(const std::string& value);
    static std::optional<TextureRole> ParseTextureRole(const std::string& value);

    bool LoadManifest();
    bool SaveManifest(std::string& error) const;
    bool ReconcileFilesystem(std::string& error);
    bool PopulateTextureMetadata(Entry& entry, bool generateThumbnail, std::string* error = nullptr) const;
    bool GenerateTextureThumbnail(const Entry& entry, std::string& thumbnail, std::string* error = nullptr) const;
    bool EnsureDirectories() const;
    bool ValidateFile(Category category, const std::filesystem::path& path, std::string& error) const;
    std::string MakeUniqueId(Category category, const std::filesystem::path& sourcePath) const;
    std::filesystem::path MakeUniqueDestination(Category category,
                                                const std::string& subpath,
                                                const std::filesystem::path& sourcePath) const;

    std::filesystem::path m_clientRoot;
    std::filesystem::path m_libraryRoot;
    std::vector<Entry> m_entries;
};
