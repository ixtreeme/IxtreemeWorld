#include "RendererGeometry.h"

#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/tools.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace material_tool {
namespace {
// Area-weighted vertex normals for a primitive that stores none. The renderer only has a lighting
// fallback for these; geometry tools need the surface's real orientation.
void GenerateNormals(AssimpImporter::StaticMeshData& mesh)
{
    for (auto& vertex : mesh.vertices) std::fill_n(vertex.normal, 3, 0.0f);
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
    {
        const auto& a = mesh.vertices[mesh.indices[i]].position;
        const auto& b = mesh.vertices[mesh.indices[i + 1]].position;
        const auto& c = mesh.vertices[mesh.indices[i + 2]].position;
        const float u[] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float face[] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        for (unsigned corner = 0; corner < 3; ++corner)
            for (unsigned axis = 0; axis < 3; ++axis)
                mesh.vertices[mesh.indices[i + corner]].normal[axis] += face[axis];
    }
    for (auto& vertex : mesh.vertices)
    {
        const float length = std::sqrt(vertex.normal[0] * vertex.normal[0] + vertex.normal[1] * vertex.normal[1] + vertex.normal[2] * vertex.normal[2]);
        if (length > 0.0f)
            for (float& axis : vertex.normal) axis /= length;
        else
            vertex.normal[1] = 1.0f;
    }
}

// Mirrors StaticMeshRenderer::LoadStaticGltfMesh: every triangle primitive of every mesh, in file
// order, without node transforms, drawn with the slot of its glTF material index.
AssimpImporter::ImportResult ImportGltf(const std::filesystem::path& path)
{
    AssimpImporter::ImportResult result;
    result.assetName = path.stem().string();
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None)
    {
        result.errorMessage = std::string(fastgltf::getErrorMessage(data.error()));
        return result;
    }
    fastgltf::Parser parser;
    auto parsed = parser.loadGltf(data.get(), path.parent_path(),
        fastgltf::Options::DecomposeNodeMatrices | fastgltf::Options::LoadExternalBuffers | fastgltf::Options::GenerateMeshIndices);
    if (parsed.error() != fastgltf::Error::None)
    {
        result.errorMessage = std::string(fastgltf::getErrorMessage(parsed.error()));
        return result;
    }
    const fastgltf::Asset& asset = parsed.get();
    result.hasSkeletal = !asset.skins.empty() ||
        std::any_of(asset.nodes.begin(), asset.nodes.end(), [](const auto& node) { return node.skinIndex.has_value(); });
    for (const auto& material : asset.materials)
    {
        GltfMaterialSource source;
        source.name = std::string(material.name);
        result.materials.push_back(std::move(source));
    }
    if (result.materials.empty())
        result.materials.push_back(GltfMaterialSource{});

    for (std::size_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex)
    {
        const auto& mesh = asset.meshes[meshIndex];
        for (std::size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
        {
            const auto& primitive = mesh.primitives[primitiveIndex];
            if (primitive.findAttribute("JOINTS_0") != primitive.attributes.end() ||
                primitive.findAttribute("WEIGHTS_0") != primitive.attributes.end())
                result.hasSkeletal = true;
            if (primitive.type != fastgltf::PrimitiveType::Triangles)
                continue;
            const auto position = primitive.findAttribute("POSITION");
            if (position == primitive.attributes.end() || !primitive.indicesAccessor.has_value())
                continue;

            AssimpImporter::StaticMeshData out;
            out.name = mesh.name.empty() ? "mesh_" + std::to_string(meshIndex) : std::string(mesh.name);
            if (mesh.primitives.size() > 1)
                out.name += "-" + std::to_string(primitiveIndex);
            const std::size_t material = primitive.materialIndex.has_value() ? primitive.materialIndex.value() : 0u;
            out.materialIndex = material < result.materials.size() ? static_cast<std::uint32_t>(material) : 0u;
            out.vertices.resize(asset.accessors[position->accessorIndex].count);
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, asset.accessors[position->accessorIndex],
                [&](fastgltf::math::fvec3 value, std::size_t index) {
                    out.vertices[index].position[0] = value.x();
                    out.vertices[index].position[1] = value.y();
                    out.vertices[index].position[2] = value.z();
                });
            const auto normal = primitive.findAttribute("NORMAL");
            if (normal != primitive.attributes.end())
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, asset.accessors[normal->accessorIndex],
                    [&](fastgltf::math::fvec3 value, std::size_t index) {
                        if (index >= out.vertices.size()) return;
                        out.vertices[index].normal[0] = value.x();
                        out.vertices[index].normal[1] = value.y();
                        out.vertices[index].normal[2] = value.z();
                    });
            const auto uv = primitive.findAttribute("TEXCOORD_0");
            if (uv != primitive.attributes.end())
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(asset, asset.accessors[uv->accessorIndex],
                    [&](fastgltf::math::fvec2 value, std::size_t index) {
                        if (index >= out.vertices.size()) return;
                        out.vertices[index].uv[0] = value.x();
                        out.vertices[index].uv[1] = value.y();
                    });
            bool indicesValid = true;
            fastgltf::iterateAccessor<std::uint32_t>(asset, asset.accessors[primitive.indicesAccessor.value()],
                [&](std::uint32_t index) {
                    indicesValid = indicesValid && index < out.vertices.size();
                    out.indices.push_back(index);
                });
            out.indices.resize(out.indices.size() - out.indices.size() % 3);
            if (!indicesValid || out.vertices.empty() || out.indices.empty())
                continue;
            if (normal == primitive.attributes.end())
                GenerateNormals(out);
            result.meshes.push_back(std::move(out));
        }
    }
    result.success = !result.meshes.empty();
    if (!result.success && result.errorMessage.empty())
        result.errorMessage = "glTF contains no renderable static triangle geometry";
    return result;
}
}

AssimpImporter::ImportResult ImportRendererGeometry(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".glb" || extension == ".gltf")
        return ImportGltf(path);
    return AssimpImporter{}.importFile(path);
}
}
