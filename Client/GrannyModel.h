#pragma once

#include <string>

class GrannyModel
{
public:
    GrannyModel() = default;
    ~GrannyModel();

    bool LoadAndLog(const std::string& path);
    bool LoadAnimationAndCompare(const std::string& modelPath, const std::string& animationPath);
    bool ComputeStaticPoseAndLog(const std::string& modelPath, const std::string& animationPath, float timeSeconds);
    void Destroy();

private:
    struct Impl;
    Impl* m_impl = nullptr;
};
