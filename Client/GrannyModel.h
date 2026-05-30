#pragma once

#include <string>

namespace client::asset {
class IAssetReader;
}

class GrannyModel
{
public:
    explicit GrannyModel(client::asset::IAssetReader& assets);
    ~GrannyModel();

    bool LoadAndLog(const std::string& path);
    bool LoadAnimationAndCompare(const std::string& modelPath, const std::string& animationPath);
    bool ComputeStaticPoseAndLog(const std::string& modelPath, const std::string& animationPath, float timeSeconds);
    void Destroy();

private:
    struct Impl;
    Impl* m_impl = nullptr;
    client::asset::IAssetReader* m_assets = nullptr;
};
