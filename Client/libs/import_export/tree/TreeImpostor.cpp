#include "TreeImpostor.h"
#include "asset/ExrImage.h"
#include "MaterialAssetManager.h"
#include "Common.h"
#include "math/IXMath.h"
#include <stb_image.h>
#include <stb_image_write.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>

namespace tree_tool
{
namespace
{
namespace xm = ixtreeme::math;
namespace fs = std::filesystem;
using ixtreeme::common::EscapeJson;
using xm::Vec3;
constexpr float pi = 3.14159265358979323846f;
std::string Hash(const std::vector<std::uint8_t>& bytes)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (auto byte : bytes)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream text;
    text << std::hex << std::setw(16) << std::setfill('0') << hash;
    return text.str();
}
std::vector<std::uint8_t> Read(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
float VDot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 VAdd(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 VSub(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 VMul(Vec3 a, float f)
{
    return {a.x * f, a.y * f, a.z * f};
}
Vec3 VCross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 VNormal(Vec3 a)
{
    return xm::SafeNormalize(a, {0, 1, 0});
}
std::uint8_t Byte(float value)
{
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}
float Linear(float value)
{
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}
float Srgb(float value)
{
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(std::max(value, 0.0f), 1.0f / 2.4f) - 0.055f;
}
struct Image
{
    int width = 0, height = 0;
    std::vector<std::uint8_t> bytes;
    std::array<float, 4> Sample(float u, float v, std::array<float, 4> fallback) const
    {
        if (bytes.empty())
            return fallback;
        u -= std::floor(u);
        v -= std::floor(v);
        const int x = std::min(width - 1, static_cast<int>(u * width));
        const int y = std::min(height - 1, static_cast<int>(v * height));
        const auto* p = bytes.data() + (static_cast<std::size_t>(y) * width + x) * 4;
        return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
    }
};
struct Surface
{
    MaterialAsset material;
    Image color, normal, orm, ao;
};
bool LoadImage(const std::optional<Guid>& guid, Image& image, bool color, TreeImpostorData& data,
               const fs::path& modelFolder, std::string& error)
{
    if (!guid)
        return true;
    const auto path = AssetDatabase::Instance().resolveGuid(*guid);
    if (!path)
    {
        error = "Impostor source texture cannot be resolved";
        return false;
    }
    int channels = 0;
    if (client::asset::IsExrPath(*path))
    {
        auto exr = client::asset::LoadExr(*path, error);
        if (!exr)
            return false;
        image.width = exr->width;
        image.height = exr->height;
        image.bytes = client::asset::ExrRgba8(*exr, color ? client::asset::ExrByteMode::SrgbColor
                                                          : client::asset::ExrByteMode::LinearData);
    }
    else
    {
        auto* pixels = stbi_load(path->string().c_str(), &image.width, &image.height, &channels, 4);
        if (!pixels)
        {
            error = "Impostor texture cannot be decoded: " + path->filename().string();
            return false;
        }
        image.bytes.assign(pixels, pixels + static_cast<std::size_t>(image.width) * image.height * 4);
        stbi_image_free(pixels);
    }
    data.sourcePaths.push_back(path->lexically_relative(modelFolder).generic_string());
    data.sourceHashes.push_back(Hash(Read(*path)));
    return true;
}
struct Projected
{
    float x, y, z;
    Vec3 normal;
    float u, v;
};
float Edge(const Projected& a, const Projected& b, float x, float y)
{
    return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
}
template <typename Mesh>
void Raster(const Mesh& mesh, const Surface& surface, Vec3 center, float radius, Vec3 right, Vec3 up, Vec3 direction,
            int resolution, int column, int row, int atlasWidth, std::vector<std::uint8_t>& color,
            std::vector<std::uint8_t>& normals, std::vector<std::uint8_t>& orm, std::vector<float>& depth)
{
    std::vector<Projected> vertices;
    vertices.reserve(mesh.vertices.size());
    // A guard texel keeps each atlas cell clear for filtering. UVs address only its inner image.
    const float scale = (resolution - 4) / (2.0f * radius);
    for (const auto& vertex : mesh.vertices)
    {
        const Vec3 p = VSub({vertex.position.x, vertex.position.y, vertex.position.z}, center);
        vertices.push_back({resolution * 0.5f + VDot(p, right) * scale,
                            resolution * 0.5f - VDot(p, up) * scale,
                            VDot(p, direction),
                            {vertex.normal.x, vertex.normal.y, vertex.normal.z},
                            vertex.uv.x,
                            vertex.uv.y});
    }
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
    {
        const auto& a = vertices[mesh.indices[i]];
        const auto& b = vertices[mesh.indices[i + 1]];
        const auto& c = vertices[mesh.indices[i + 2]];
        const float area = Edge(a, b, c.x, c.y);
        if (std::abs(area) < 1e-6f)
            continue;
        const int x0 = std::clamp(static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))), 0, resolution - 1);
        const int x1 = std::clamp(static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))), 0, resolution - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))), 0, resolution - 1);
        const int y1 = std::clamp(static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))), 0, resolution - 1);
        const auto& va = mesh.vertices[mesh.indices[i]];
        const auto& vb = mesh.vertices[mesh.indices[i + 1]];
        const auto& vc = mesh.vertices[mesh.indices[i + 2]];
        const Vec3 e1 =
            VSub({vb.position.x, vb.position.y, vb.position.z}, {va.position.x, va.position.y, va.position.z});
        const Vec3 e2 =
            VSub({vc.position.x, vc.position.y, vc.position.z}, {va.position.x, va.position.y, va.position.z});
        const float du1 = vb.uv.x - va.uv.x, dv1 = vb.uv.y - va.uv.y;
        const float du2 = vc.uv.x - va.uv.x, dv2 = vc.uv.y - va.uv.y;
        // Vulkan's left-handed camera mirrors our capture X: Edge() has the actual screen determinant's sign.
        Vec3 tangent = VSub(VMul(e1, dv2), VMul(e2, dv1));
        if (area < 0)
            tangent = VMul(tangent, -1.0f);
        tangent = VNormal(tangent);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const float wa = Edge(b, c, x + 0.5f, y + 0.5f) / area;
                const float wb = Edge(c, a, x + 0.5f, y + 0.5f) / area;
                const float wc = 1 - wa - wb;
                if (wa < -1e-5f || wb < -1e-5f || wc < -1e-5f)
                    continue;
                const float z = wa * a.z + wb * b.z + wc * c.z;
                const std::size_t local = static_cast<std::size_t>(y) * resolution + x;
                if (z < depth[local])
                    continue;
                const float u =
                    (wa * a.u + wb * b.u + wc * c.u) * surface.material.uvTiling[0] + surface.material.uvOffset[0];
                const float v =
                    (wa * a.v + wb * b.v + wc * c.v) * surface.material.uvTiling[1] + surface.material.uvOffset[1];
                const auto albedo = surface.color.Sample(u, v, {1, 1, 1, 1});
                const float alpha = albedo[3] * surface.material.baseColor[3];
                if (surface.material.alphaMode == MaterialAsset::AlphaMode::Mask &&
                    alpha < surface.material.alphaCutoff)
                    continue;
                depth[local] = z;
                const std::size_t at =
                    (static_cast<std::size_t>(row * resolution + y) * atlasWidth + column * resolution + x) * 4;
                for (int channel = 0; channel < 3; ++channel)
                    color[at + channel] = Byte(Srgb(Linear(albedo[channel]) * surface.material.baseColor[channel]));
                color[at + 3] = 255;
                Vec3 n = VNormal(VAdd(VAdd(VMul(a.normal, wa), VMul(b.normal, wb)), VMul(c.normal, wc)));
                if (!surface.normal.bytes.empty())
                {
                    const auto map = surface.normal.Sample(u, v, {0.5f, 0.5f, 1, 1});
                    const Vec3 bitangent = VNormal(VCross(n, tangent));
                    n = VNormal(VAdd(VAdd(VMul(tangent, (map[0] * 2 - 1) * surface.material.normalStrength),
                                          VMul(bitangent, (map[1] * 2 - 1) * surface.material.normalStrength)),
                                     VMul(n, map[2] * 2 - 1)));
                }
                // A front-facing +Z billboard's derivative tangent/bitangent are -right/-up.
                normals[at] = Byte(-VDot(n, right) * 0.5f + 0.5f);
                normals[at + 1] = Byte(-VDot(n, up) * 0.5f + 0.5f);
                normals[at + 2] = Byte(VDot(n, direction) * 0.5f + 0.5f);
                normals[at + 3] = 255;
                const auto packed = surface.orm.Sample(u, v, {1, 1, 1, 1});
                const auto ambient = surface.ao.Sample(u, v, {packed[0], 1, 1, 1});
                orm[at] = Byte(ambient[0] * surface.material.aoStrength);
                orm[at + 1] = Byte(packed[1] * surface.material.roughness);
                orm[at + 2] = Byte(packed[2] * surface.material.metallic);
                orm[at + 3] = 255;
            }
    }
}
void Dilate(std::vector<std::uint8_t>& color, std::vector<std::uint8_t>& normals, std::vector<std::uint8_t>& orm,
            int resolution, int columns, int rows)
{
    const int width = columns * resolution;
    std::vector<std::uint8_t> filled(color.size() / 4);
    for (std::size_t pixel = 0; pixel < filled.size(); ++pixel)
        filled[pixel] = color[pixel * 4 + 3] != 0;
    for (int pass = 0; pass < 4; ++pass)
    {
        const auto previous = filled;
        const auto previousColor = color, previousNormals = normals, previousOrm = orm;
        for (int row = 0; row < rows; ++row)
            for (int col = 0; col < columns; ++col)
                for (int y = 1; y < resolution - 1; ++y)
                    for (int x = 1; x < resolution - 1; ++x)
                    {
                        const std::size_t at =
                            (static_cast<std::size_t>(row * resolution + y) * width + col * resolution + x) * 4;
                        if (previous[at / 4])
                            continue;
                        for (const auto offset :
                             {-static_cast<std::ptrdiff_t>(width) * 4, static_cast<std::ptrdiff_t>(width) * 4,
                              std::ptrdiff_t(-4), std::ptrdiff_t(4)})
                        {
                            const std::size_t from = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(at) + offset);
                            if (!previous[from / 4])
                                continue;
                            for (int channel = 0; channel < 3; ++channel)
                            {
                                color[at + channel] = previousColor[from + channel];
                                normals[at + channel] = previousNormals[from + channel];
                                orm[at + channel] = previousOrm[from + channel];
                            }
                            filled[at / 4] = 1;
                            // Coverage stays zero: extend RGB only, preventing dark filtered fringes.
                            break;
                        }
                    }
    }
}
} // namespace
std::string TreeImpostorMaterialSignature(const MaterialAsset& material)
{
    std::ostringstream text;
    text << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (float value : material.baseColor)
        text << value << ',';
    for (float value : material.emissive)
        text << value << ',';
    for (float value : material.uvTiling)
        text << value << ',';
    for (float value : material.uvOffset)
        text << value << ',';
    text << material.metallic << ',' << material.roughness << ',' << material.normalStrength << ','
         << material.aoStrength << ',' << material.alphaCutoff << ',' << static_cast<int>(material.alphaMode) << ','
         << static_cast<int>(material.shadingMode);
    for (const auto& guid : {material.baseColorTexture, material.normalTexture, material.metallicRoughnessTexture,
                             material.aoTexture, material.emissiveTexture})
        text << ',' << (guid ? guid->toString() : "");
    const auto value = text.str();
    return Hash({value.begin(), value.end()});
}
TreeImpostorView SelectTreeImpostorView(float azimuth, float elevation, int azimuths)
{
    if (!std::isfinite(azimuth) || !std::isfinite(elevation))
        return {};
    azimuths = std::clamp(azimuths, 4, 16);
    float frame = std::fmod(azimuth / (2 * pi) * azimuths, static_cast<float>(azimuths));
    if (frame < 0)
        frame += azimuths;
    const int first = static_cast<int>(std::floor(frame)) % azimuths;
    const int row = std::clamp(static_cast<int>(std::lround(elevation / (pi / 6))) + 1, 0, 2);
    return {first, (first + 1) % azimuths, row, frame - std::floor(frame)};
}
float TreeImpostorWeight(float distance, float begin, float transition)
{
    return std::clamp((distance - begin) / std::max(transition, 1.0f), 0.0f, 1.0f);
}
std::array<float, 4> TreeImpostorUv(int column, int row, int azimuths, int resolution)
{
    const float inner = static_cast<float>(resolution - 4) / resolution;
    return {inner / azimuths, inner / 3.0f, (column + 2.0f / resolution) / azimuths, (row + 2.0f / resolution) / 3.0f};
}
std::optional<TreeImpostorData> LoadTreeImpostor(const client::asset::IAssetReader& assets,
                                                 const std::string& modelPath, std::string& error)
{
    using namespace ixtreeme::common;
    const auto text = assets.ReadText(modelPath + ".impostor.json");
    if (!text)
        return {};
    TreeImpostorData data;
    const float azimuths = JsonFloatValue(*text, "azimuths", 0), elevations = JsonFloatValue(*text, "elevations", 0),
                resolution = JsonFloatValue(*text, "resolution", 0);
    if (!(azimuths >= 4 && azimuths <= 16 && azimuths == std::floor(azimuths)) || elevations != 3 ||
        !(resolution >= 64 && resolution <= 512 && resolution == std::floor(resolution)))
    {
        error = "Invalid impostor atlas dimensions";
        return {};
    }
    data.azimuths = static_cast<int>(azimuths);
    data.elevations = static_cast<int>(elevations);
    data.resolution = static_cast<int>(resolution);
    data.radius = JsonFloatValue(*text, "radius", 0);
    data.distance = JsonFloatValue(*text, "distance", 0);
    data.transition = JsonFloatValue(*text, "transition", 0);
    data.center.fill(std::numeric_limits<float>::quiet_NaN());
    JsonFloatArrayValue(*text, "center", data.center.data(), 3);
    data.materials = JsonStringArrayValue(*text, "materials");
    data.materialSignatures = JsonStringArrayValue(*text, "materialSignatures");
    const auto folder = fs::path(modelPath).parent_path();
    const auto relative = fs::path(JsonStringValue(*text, "billboard"));
    if (JsonFloatValue(*text, "version", 0) != 1 || relative.empty() || relative.has_root_path() || data.azimuths < 4 ||
        data.azimuths > 16 || data.elevations != 3 || data.resolution < 64 || data.resolution > 512 ||
        !(data.radius > 0 && std::isfinite(data.radius)) || !(data.distance >= 10 && std::isfinite(data.distance)) ||
        !(data.transition >= 1 && std::isfinite(data.transition)) || data.materials.empty() || data.materials.size() > 128 ||
        data.materialSignatures.size() != data.materials.size() ||
        !std::all_of(data.center.begin(), data.center.end(), [](float x) { return std::isfinite(x); }))
    {
        error = "Invalid tree impostor metadata";
        return {};
    }
    data.billboardPath = (folder / relative).lexically_normal().generic_string();
    data.sourcePaths = JsonStringArrayValue(*text, "sources");
    data.sourceHashes = JsonStringArrayValue(*text, "hashes");
    if (data.sourcePaths.empty() || data.sourcePaths.size() != data.sourceHashes.size())
    {
        error = "Invalid impostor sources";
        return {};
    }
    for (auto& path : data.sourcePaths)
        path = (folder / fs::path(path)).lexically_normal().generic_string();
    if (!ValidateTreeImpostor(assets, data))
    {
        error = "Tree impostor is stale or incomplete; save the tree again";
        return {};
    }
    return data;
}
bool ValidateTreeImpostor(const client::asset::IAssetReader& assets, const TreeImpostorData& data)
{
    if (data.sourcePaths.size() != data.sourceHashes.size())
        return false;
    for (std::size_t i = 0; i < data.sourcePaths.size(); ++i)
    {
        const auto bytes = assets.ReadAll(data.sourcePaths[i]);
        if (!bytes || Hash(*bytes) != data.sourceHashes[i])
            return false;
    }
    return assets.ReadAll(data.billboardPath).has_value();
}
bool BakeTreeImpostor(const ixtreemetree::TreeMesh& mesh, const fs::path& modelPath, const std::vector<Guid>& materials,
                      const TreeImpostorSettings& settings, std::vector<Guid>& dependencies, std::string& error)
{
    const std::array<MeshImpostorPart, 2> parts{{{mesh.bark.vertices, mesh.bark.indices}, {mesh.leaves.vertices, mesh.leaves.indices}}};
    return BakeMeshImpostor(parts, modelPath, materials, settings, dependencies, error);
}
bool BakeMeshImpostor(std::span<const MeshImpostorPart> parts, const fs::path& modelPath, const std::vector<Guid>& materials,
                      const TreeImpostorSettings& settings, std::vector<Guid>& dependencies, std::string& error)
{
    if (!settings.enabled)
        return true;
    if (materials.empty() || materials.size() > 128 || materials.size() != parts.size())
    {
        error = "Mesh impostor needs one material per source part (1-128)";
        return false;
    }
    if (!std::isfinite(settings.distance) || !std::isfinite(settings.transition))
    {
        error = "Invalid impostor distance";
        return false;
    }
    const auto validMesh = [](const auto& part) {
        return part.indices.size() % 3 == 0 &&
               std::all_of(part.indices.begin(), part.indices.end(),
                           [&](auto index) { return index < part.vertices.size(); }) &&
               std::all_of(part.vertices.begin(), part.vertices.end(), [](const auto& vertex) {
                   return std::isfinite(vertex.position.x) && std::isfinite(vertex.position.y) &&
                          std::isfinite(vertex.position.z) && std::isfinite(vertex.normal.x) &&
                          std::isfinite(vertex.normal.y) && std::isfinite(vertex.normal.z) &&
                          std::isfinite(vertex.uv.x) && std::isfinite(vertex.uv.y);
               });
    };
    if (!std::all_of(parts.begin(), parts.end(), validMesh) ||
        std::all_of(parts.begin(), parts.end(), [](const auto& part) { return part.indices.empty(); }))
    {
        error = "Invalid impostor source geometry";
        return false;
    }
    TreeImpostorData data;
    data.azimuths = std::clamp(settings.azimuths, 4, 16);
    data.resolution = std::clamp(settings.resolution, 64, 512);
    data.distance = std::clamp(settings.distance, 10.0f, 2000.0f);
    data.transition = std::clamp(settings.transition, 1.0f, 200.0f);
    Vec3 minimum{INFINITY, INFINITY, INFINITY}, maximum{-INFINITY, -INFINITY, -INFINITY};
    for (const auto& part : parts)
        for (const auto& vertex : part.vertices)
        {
            minimum.x = std::min(minimum.x, vertex.position.x);
            minimum.y = std::min(minimum.y, vertex.position.y);
            minimum.z = std::min(minimum.z, vertex.position.z);
            maximum.x = std::max(maximum.x, vertex.position.x);
            maximum.y = std::max(maximum.y, vertex.position.y);
            maximum.z = std::max(maximum.z, vertex.position.z);
        }
    data.center = {(minimum.x + maximum.x) * .5f, (minimum.y + maximum.y) * .5f, (minimum.z + maximum.z) * .5f};
    if (!std::all_of(data.center.begin(), data.center.end(), [](float value) { return std::isfinite(value); }))
    {
        error = "Invalid tree bounds";
        return false;
    }
    const Vec3 center{data.center[0], data.center[1], data.center[2]};
    for (const auto& part : parts)
        for (const auto& vertex : part.vertices)
            data.radius = std::max(
                data.radius, std::sqrt(VDot(VSub({vertex.position.x, vertex.position.y, vertex.position.z}, center),
                                            VSub({vertex.position.x, vertex.position.y, vertex.position.z}, center))));
    data.radius = std::max(data.radius * 1.01f, 0.1f);
    if (!std::isfinite(data.radius))
    {
        error = "Tree bounds exceed impostor precision";
        return false;
    }
    const auto folder = modelPath.parent_path();
    std::vector<Surface> surfaces(parts.size());
    data.sourcePaths.push_back(modelPath.filename().generic_string());
    data.sourceHashes.push_back(Hash(Read(modelPath)));
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        const auto* material = MaterialAssetManager::Instance().getOrLoad(materials[i]);
        if (!material)
        {
            error = "Cannot load tree material for impostor";
            return false;
        }
        auto& surface = surfaces[i];
        surface.material = *material;
        const auto finite = [](const auto& values) {
            return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
        };
        if (!finite(material->baseColor) || !finite(material->emissive) || !finite(material->uvTiling) ||
            !finite(material->uvOffset) || !std::isfinite(material->normalStrength) ||
            !std::isfinite(material->aoStrength) || !std::isfinite(material->metallic) ||
            !std::isfinite(material->roughness) || !std::isfinite(material->alphaCutoff))
        {
            error = "Tree material has nonfinite values";
            return false;
        }
        if (material->alphaMode == MaterialAsset::AlphaMode::Blend ||
            material->shadingMode == MaterialAsset::ShadingMode::Unlit || material->emissive[0] != 0 ||
            material->emissive[1] != 0 || material->emissive[2] != 0 || material->emissiveTexture)
        {
            error = "Impostors require lit opaque/masked tree materials without emissive maps";
            return false;
        }
        data.materials.push_back(materials[i].toString());
        data.materialSignatures.push_back(TreeImpostorMaterialSignature(*material));
        data.sourcePaths.push_back(material->path.lexically_relative(folder).generic_string());
        data.sourceHashes.push_back(Hash(Read(material->path)));
        if (!LoadImage(material->baseColorTexture, surface.color, true, data, folder, error) ||
            !LoadImage(material->normalTexture, surface.normal, false, data, folder, error) ||
            !LoadImage(material->metallicRoughnessTexture, surface.orm, false, data, folder, error) ||
            !LoadImage(material->aoTexture, surface.ao, false, data, folder, error))
            return false;
    }
    const int width = data.azimuths * data.resolution, height = 3 * data.resolution;
    const std::size_t size = static_cast<std::size_t>(width) * height * 4;
    std::vector<std::uint8_t> color(size, 0), normals(size, 128), orm(size, 255);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < data.azimuths; ++col)
        {
            const float yaw = 2 * pi * col / data.azimuths, elevation = (row - 1) * pi / 6;
            const Vec3 direction{std::sin(yaw) * std::cos(elevation), std::sin(elevation),
                                 std::cos(yaw) * std::cos(elevation)};
            const Vec3 right{std::cos(yaw), 0, -std::sin(yaw)}, up = VCross(direction, right);
            std::vector<float> depth(static_cast<std::size_t>(data.resolution) * data.resolution,
                                     -std::numeric_limits<float>::infinity());
            for (std::size_t i = 0; i < parts.size(); ++i)
                Raster(parts[i], surfaces[i], center, data.radius, right, up, direction, data.resolution, col, row, width,
                       color, normals, orm, depth);
        }
    Dilate(color, normals, orm, data.resolution, data.azimuths, 3);
    const auto directory = folder / (modelPath.stem().string() + "_impostor");
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }
    for (const auto& item :
         {std::pair{"albedo.png", &color}, std::pair{"normal.png", &normals}, std::pair{"orm.png", &orm}})
    {
        const auto path = directory / item.first;
        if (!stbi_write_png(path.string().c_str(), width, height, 4, item.second->data(), width * 4))
        {
            error = "Cannot write impostor atlas";
            return false;
        }
        dependencies.push_back(AssetDatabase::Instance().getOrCreateGuid(path));
    }
    const float r = data.radius;
    const float vertices[] = {-r, -r, 0, 0, 0, 1, 0, 1, r,  -r, 0, 0, 0, 1, 1, 1,
                              r,  r,  0, 0, 0, 1, 1, 0, -r, r,  0, 0, 0, 1, 0, 0};
    const std::uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    std::ofstream binary(directory / "billboard.bin", std::ios::binary | std::ios::trunc);
    binary.write(reinterpret_cast<const char*>(vertices), sizeof(vertices));
    binary.write(reinterpret_cast<const char*>(indices), sizeof(indices));
    if (!binary.good())
    {
        error = "Cannot write impostor billboard";
        return false;
    }
    std::ofstream gltf(directory / "billboard.gltf", std::ios::binary | std::ios::trunc);
    gltf << std::setprecision(9)
         << "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"billboard.bin\",\"byteLength\":152}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":128,\"byteStride\":32,\"target\":34962},{"
            "\"buffer\":0,\"byteOffset\":128,\"byteLength\":24,\"target\":34963}],"
            "\"accessors\":[{\"bufferView\":0,\"byteOffset\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\","
            "\"min\":["
         << -r << "," << -r << ",0],\"max\":[" << r << "," << r
         << ",0]},"
            "{\"bufferView\":0,\"byteOffset\":12,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},{\"bufferView\":"
            "0,\"byteOffset\":24,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
            "{\"bufferView\":1,\"componentType\":5125,\"count\":6,\"type\":\"SCALAR\"}],"
            "\"images\":[{\"uri\":\"albedo.png\"},{\"uri\":\"normal.png\"},{\"uri\":\"orm.png\"}],\"textures\":[{"
            "\"source\":0},{\"source\":1},{\"source\":2}],"
            "\"materials\":[{\"name\":\"tree_impostor\",\"doubleSided\":true,\"alphaMode\":\"MASK\",\"alphaCutoff\":0."
            "35,\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1],\"metallicFactor\":1,\"roughnessFactor\":1,"
            "\"baseColorTexture\":{\"index\":0},\"metallicRoughnessTexture\":{\"index\":2}},\"normalTexture\":{"
            "\"index\":1},\"occlusionTexture\":{\"index\":2}}],"
            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,"
            "\"material\":0}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    if (!gltf.good())
    {
        error = "Cannot write impostor glTF";
        return false;
    }
    gltf.close();
    binary.close();
    data.billboardPath = (directory / "billboard.gltf").lexically_relative(folder).generic_string();
    // Atlas/billboard loss or edits also invalidate the bake, instead of drawing a fallback-white quad.
    for (const char* filename : {"albedo.png", "normal.png", "orm.png", "billboard.gltf", "billboard.bin"})
    {
        const auto path = directory / filename;
        data.sourcePaths.push_back(path.lexically_relative(folder).generic_string());
        data.sourceHashes.push_back(Hash(Read(path)));
    }
    const auto billboardGuid = AssetDatabase::Instance().getOrCreateGuid(directory / "billboard.gltf");
    dependencies.push_back(billboardGuid);
    std::ofstream metadata(modelPath.string() + ".impostor.json", std::ios::binary | std::ios::trunc);
    metadata << std::setprecision(9) << "{\n\"version\":1,\"billboard\":\"" << EscapeJson(data.billboardPath)
             << "\",\"azimuths\":" << data.azimuths << ",\"elevations\":3,\"resolution\":" << data.resolution
             << ",\"center\":[" << data.center[0] << "," << data.center[1] << "," << data.center[2]
             << "],\"radius\":" << r << ",\"distance\":" << data.distance << ",\"transition\":" << data.transition;
    const auto array = [&](const char* key, const std::vector<std::string>& values) {
        metadata << ",\n\"" << key << "\":[";
        for (std::size_t i = 0; i < values.size(); ++i)
            metadata << (i ? "," : "") << "\"" << EscapeJson(values[i]) << "\"";
        metadata << "]";
    };
    array("materials", data.materials);
    array("materialSignatures", data.materialSignatures);
    array("sources", data.sourcePaths);
    array("hashes", data.sourceHashes);
    metadata << "\n}\n";
    if (!metadata.good())
    {
        error = "Cannot write impostor metadata";
        return false;
    }
    return true;
}
} // namespace tree_tool
