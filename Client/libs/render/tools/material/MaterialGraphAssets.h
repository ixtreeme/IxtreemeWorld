#pragma once
#include "MaterialGraphImages.h"
#include "AssetLibrary.h"
#include <filesystem>

namespace material_tool {
class GraphSubgraphCache
{
public:
    void Clear() { entries_.clear(); used_.clear(); }
    void BeginCompilation() { used_.clear(); }
    void FinishCompilation();
    bool Changed() const;
    bool Resolve(const std::string& guid, MaterialSubgraph& asset, std::string& error);
    MaterialSubgraphResolver Resolver() { return [this](const auto& guid, auto& asset, auto& error) { return Resolve(guid, asset, error); }; }
private:
    struct Stamp {
        std::filesystem::path path;
        std::filesystem::file_time_type modified{};
        std::uintmax_t bytes = 0;
        bool exists = false;
        bool operator==(const Stamp&) const = default;
    };
    struct Entry { Stamp stamp; MaterialSubgraph asset; std::string error; };
    static Stamp Current(const std::string& guid);
    std::unordered_map<std::string, Entry> entries_;
    std::vector<std::string> used_;
};
class GraphTextureCache {
public:
    void Configure(AssetLibrary* library, const std::filesystem::path& graphFolder);
    void Clear();
    void BeginEvaluation() { error_.clear(); }
    SampledTexture Sample(const GraphNode& node, const std::string& overridePath, float u, float v);
    const std::string& Error() const { return error_; }
private:
    struct Image { int width=0,height=0; std::vector<float> pixels; std::string error; };
    struct Binding { std::string resource, guid, assetId, overridePath, key; bool exr=false; };
    std::filesystem::path Resolve(const GraphNode& node, const std::string& overridePath) const;
    std::unordered_map<std::string, Image> images_;
    std::unordered_map<std::uint32_t, Binding> bindings_;
    AssetLibrary* library_ = nullptr;
    std::filesystem::path graphFolder_;
    std::string error_;
    std::size_t bytes_ = 0;
};
bool PublishBakedMaterial(AssetLibrary& library, const MaterialGraphDocument& document, const ParameterOverrides& parameters,
    unsigned resolution, const std::string& name, AssetLibrary::Entry& result, std::string& error, const std::filesystem::path& graphFolder = {},
    const GeometryBake* geometry = nullptr, const std::string& geometryGuid = {});
}
