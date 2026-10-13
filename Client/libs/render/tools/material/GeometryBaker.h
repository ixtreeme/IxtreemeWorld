#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace material_tool {
using GeometryVec3 = std::array<float, 3>;
struct GeometryVertex {
    GeometryVec3 position{}, normal{0,1,0};
    std::array<float,2> uv{};
};
struct GeometryMesh {
    std::vector<GeometryVertex> vertices;
    std::vector<std::uint32_t> indices;
};
struct GeometryBakeSettings {
    unsigned resolution = 128, padding = 4, aoSamples = 16;
    float rayDistance = 10, rayBias = .0001f;
    std::uint32_t seed = 1;
    // Row-major affine matrix multiplying a column vector. Rays/distances are world-space.
    std::array<float,16> objectToWorld{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bool operator==(const GeometryBakeSettings&) const = default;
};
struct GeometryTexel {
    std::array<float,2> uv{}; // Original covered-texel UV, retained by padding.
    GeometryVec3 objectPosition{}, worldPosition{}, objectNormal{0,1,0}, worldNormal{0,1,0};
    GeometryVec3 worldTangent{1,0,0}, worldBitangent{0,0,-1};
    float ao = 1, curvature = .5f, cavity = 0, thickness = 0;
};
struct GeometryBake {
    GeometryBakeSettings settings;
    std::uint64_t meshHash = 0;
    std::vector<GeometryTexel> texels;
    // 0: empty, 1: real UV coverage, 2: padding copied from real coverage.
    std::vector<std::uint8_t> coverage;
    unsigned coveredTexels = 0, boundaryEdges = 0;
    bool Sample(float u, float v, GeometryTexel& texel) const;
};
struct GeometryBakeControl {
    std::function<bool()> cancelled;
    std::function<void(float)> progress;
};
// Why a bake stopped. Only a UvLayout failure (reused, out-of-range or degenerate UVs) can be
// repaired by generating a new atlas for the same surface.
enum class GeometryBakeFailure { None, Cancelled, UvLayout, Other };
std::uint64_t GeometryMeshHash(const GeometryMesh& mesh);
// Failure/cancellation preserves result. Limits: 512 square, 1M vertices/triangles.
bool BakeGeometry(const GeometryMesh& mesh, const GeometryBakeSettings& settings,
                  GeometryBake& result, std::string& error, const GeometryBakeControl& control = {},
                  GeometryBakeFailure* failure = nullptr);
bool ValidateGeometryBake(const GeometryBake& bake, std::string& error);
// Portable, bounded, checksummed little-endian v1 cache; failure preserves the destination.
bool EncodeGeometryBake(const GeometryBake& bake, std::string& bytes, std::string& error);
bool DecodeGeometryBake(const std::string& bytes, GeometryBake& bake, std::string& error);

class GeometryBakeJob {
public:
    ~GeometryBakeJob();
    bool Start(GeometryMesh mesh, GeometryBakeSettings settings);
    void Cancel();
    bool Running() const { return running_.load(); }
    float Progress() const { return progress_.load(); }
    // Returns false until finished. A completed failure reports error and preserves result.
    bool TakeResult(GeometryBake& result, std::string& error, GeometryBakeFailure* failure = nullptr);
private:
    std::jthread worker_;
    std::atomic<bool> running_{false}, ready_{false};
    std::atomic<float> progress_{0};
    GeometryBake result_;
    std::string error_;
    GeometryBakeFailure failure_ = GeometryBakeFailure::None;
    bool success_ = false;
};
}
