#pragma once
#include "GeometryBaker.h"

namespace material_tool {
bool GenerateGeometryUvAtlas(const GeometryMesh& mesh, unsigned resolution, GeometryMesh& result,
    std::string& error, const GeometryBakeControl& control = {});
class GeometryUvAtlasJob {
public:
    ~GeometryUvAtlasJob();
    bool Start(GeometryMesh mesh, unsigned resolution);
    void Cancel();
    bool Running() const { return running_.load(); }
    float Progress() const { return progress_.load(); }
    bool TakeResult(GeometryMesh& result, std::string& error);
private:
    std::jthread worker_;
    std::atomic<bool> running_{false}, ready_{false};
    std::atomic<float> progress_{0};
    GeometryMesh result_;
    std::string error_;
    bool success_ = false;
};
}
