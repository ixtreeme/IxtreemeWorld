#pragma once
#include "MaterialGraphImages.h"
#include "AssetLibrary.h"
#include <filesystem>

namespace material_tool {
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
    unsigned resolution, const std::string& name, AssetLibrary::Entry& result, std::string& error, const std::filesystem::path& graphFolder = {});
}
