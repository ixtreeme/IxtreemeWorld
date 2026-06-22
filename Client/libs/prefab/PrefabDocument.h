#pragma once

#include "MapEditorTypes.h"

#include <cstddef>
#include <cstdint>
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

} // namespace ixtreeme::prefab
