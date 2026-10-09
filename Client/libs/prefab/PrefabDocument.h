#pragma once

#include "MapEditorTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace ixtreeme::prefab
{

struct PrefabTemplate
{
    enum class Kind
    {
        Unsupported,
        Mesh,
        PointLight,
        SpotLight
    };

    Kind kind = Kind::Unsupported;
    std::string name;
    MeshSceneEntity mesh;
    PointLight point;
    SpotLight spot;
};

struct PrefabEntity
{
    std::uint32_t localId = 0;
    std::uint32_t parentLocalId = 0;
    PrefabTemplate::Kind kind = PrefabTemplate::Kind::Unsupported;
    std::string name;
    MeshSceneEntity mesh;
    PointLight point;
    SpotLight spot;
};

struct PrefabDocument
{
    std::string name;
    std::vector<PrefabEntity> entities;
};

std::string FloatArray(const float* values, std::size_t count);

void WriteMesh(std::ostream& out, const MeshSceneEntity& mesh, const std::string& displayName);
void WritePointLight(std::ostream& out, const PointLight& light, const std::string& displayName);
void WriteSpotLight(std::ostream& out, const SpotLight& light, const std::string& displayName);
void WriteDocument(std::ostream& out, const PrefabDocument& document);

PrefabTemplate ParseTemplate(const std::string& text, const std::string& fallbackName);
PrefabDocument ParseDocument(const std::string& text, const std::string& fallbackName);

// Validates a mesh-rooted runtime prefab before allocating IDs. The returned copies carry world
// positions, remapped parents/joints and prefab links; no live world is changed here.
struct RuntimePrefabInstance
{
    std::vector<MeshSceneEntity> meshes;
    std::vector<PointLight> points;
    std::vector<SpotLight> spots;
};
bool InstantiateRuntime(const PrefabDocument& document, const std::string& assetId,
    std::uint32_t rootId, const float position[3],
    const std::function<std::uint32_t()>& allocateMeshId,
    const std::function<std::uint32_t()>& allocateLightId,
    RuntimePrefabInstance& result, std::string& error);

} // namespace ixtreeme::prefab
