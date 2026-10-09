#include "TreeGlbExporter.h"

#include "AssetDatabase.h"
#include "Common.h"
#include "Debug.h"
#include "MaterialAssetManager.h"
#include <ixtreemetree/leaf_trim.h>
#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <vector>

namespace tree_tool
{
namespace
{
using ixtreeme::common::EscapeJson;

struct BufferView
{
    std::size_t offset = 0;
    std::size_t length = 0;
    std::size_t stride = 0;
    int target = 0;
};

std::string SanitizeAssetName(std::string value)
{
    for (char& c : value)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-')
            c = '_';
    }
    while (!value.empty() && (value.back() == '_' || value.back() == '-'))
        value.pop_back();
    return value.empty() ? "tree" : value;
}

bool PathFilenameEquals(const std::filesystem::path& path, const char* name)
{
    std::string filename = path.filename().string();
    std::string expected(name);
    std::transform(filename.begin(), filename.end(), filename.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::transform(expected.begin(), expected.end(), expected.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return filename == expected;
}

std::filesystem::path ResolvePathBestEffort(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(path, ec);
    if (!ec && !resolved.empty())
        return resolved;
    ec.clear();
    resolved = std::filesystem::absolute(path, ec);
    return ec ? path : resolved;
}

std::filesystem::path NormalizeProjectRoot(const std::filesystem::path& projectRoot)
{
    std::filesystem::path resolved = ResolvePathBestEffort(projectRoot);
    if (PathFilenameEquals(resolved, "Assets") && resolved.has_parent_path())
        resolved = resolved.parent_path();
    return resolved;
}

void Align4(std::vector<std::uint8_t>& bytes)
{
    while ((bytes.size() % 4u) != 0u)
        bytes.push_back(0);
}

template <typename T>
void AppendValue(std::vector<std::uint8_t>& bytes, const T& value)
{
    const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), src, src + sizeof(T));
}

BufferView AppendVertices(std::vector<std::uint8_t>& bytes, const std::vector<ixtreemetree::Vertex>& vertices)
{
    Align4(bytes);
    BufferView view{};
    view.offset = bytes.size();
    view.stride = sizeof(float) * 8u;
    view.target = 34962;
    for (const ixtreemetree::Vertex& vertex : vertices)
    {
        const float packed[8] = {
            vertex.position.x, vertex.position.y, vertex.position.z,
            vertex.normal.x, vertex.normal.y, vertex.normal.z,
            vertex.uv.x, vertex.uv.y,
        };
        const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(packed);
        bytes.insert(bytes.end(), src, src + sizeof(packed));
    }
    view.length = bytes.size() - view.offset;
    return view;
}

BufferView AppendIndices(std::vector<std::uint8_t>& bytes, const std::vector<std::uint32_t>& indices)
{
    Align4(bytes);
    BufferView view{};
    view.offset = bytes.size();
    view.target = 34963;
    for (std::uint32_t index : indices)
        AppendValue(bytes, index);
    view.length = bytes.size() - view.offset;
    return view;
}

void Bounds(const std::vector<ixtreemetree::Vertex>& vertices, ixtreemetree::Vec3& minOut, ixtreemetree::Vec3& maxOut)
{
    minOut = {999999.0f, 999999.0f, 999999.0f};
    maxOut = {-999999.0f, -999999.0f, -999999.0f};
    for (const auto& vertex : vertices)
    {
        minOut.x = std::min(minOut.x, vertex.position.x);
        minOut.y = std::min(minOut.y, vertex.position.y);
        minOut.z = std::min(minOut.z, vertex.position.z);
        maxOut.x = std::max(maxOut.x, vertex.position.x);
        maxOut.y = std::max(maxOut.y, vertex.position.y);
        maxOut.z = std::max(maxOut.z, vertex.position.z);
    }
}

std::string Vec3Json(ixtreemetree::Vec3 value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(6)
        << "[" << value.x << "," << value.y << "," << value.z << "]";
    return out.str();
}

bool WriteGlb(const ixtreemetree::TreeMesh& mesh,
              const std::filesystem::path& path,
              const std::string& name,
              const std::string& barkTextureUri,
              const std::string& leafTextureUri,
              float leafAlphaCutoff,
              std::uint64_t* outBinarySizeBytes)
{
    std::vector<BufferView> views;
    std::vector<std::uint8_t> bin;
    const BufferView barkVertices = AppendVertices(bin, mesh.bark.vertices);
    views.push_back(barkVertices);
    const BufferView barkIndices = AppendIndices(bin, mesh.bark.indices);
    views.push_back(barkIndices);
    const BufferView leafVertices = AppendVertices(bin, mesh.leaves.vertices);
    views.push_back(leafVertices);
    const BufferView leafIndices = AppendIndices(bin, mesh.leaves.indices);
    views.push_back(leafIndices);
    Align4(bin);

    ixtreemetree::Vec3 barkMin{}, barkMax{}, leafMin{}, leafMax{};
    Bounds(mesh.bark.vertices, barkMin, barkMax);
    Bounds(mesh.leaves.vertices, leafMin, leafMax);

    std::ostringstream json;
    json << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"ixtreemetree\"},"
         << "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
         << "\"nodes\":[{\"mesh\":0,\"name\":\"" << EscapeJson(name) << "\"}],"
         << "\"materials\":["
         << "{\"name\":\"bark\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.42,0.26,0.12,1],";
    if (!barkTextureUri.empty())
        json << "\"baseColorTexture\":{\"index\":0},";
    json << "\"metallicFactor\":0,\"roughnessFactor\":0.82}},"
         << "{\"name\":\"leaves\",\"alphaMode\":\"MASK\",\"alphaCutoff\":" << leafAlphaCutoff
         << ",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.22,0.56,0.22,1],";
    if (!leafTextureUri.empty())
        json << "\"baseColorTexture\":{\"index\":" << (barkTextureUri.empty() ? 0 : 1) << "},";
    json << "\"metallicFactor\":0,\"roughnessFactor\":0.7}}],";
    if (!barkTextureUri.empty() || !leafTextureUri.empty())
    {
        json << "\"textures\":[";
        int textureIndex = 0;
        if (!barkTextureUri.empty())
            json << "{\"source\":" << textureIndex++ << "}";
        if (!leafTextureUri.empty())
        {
            if (textureIndex > 0) json << ",";
            json << "{\"source\":" << textureIndex++ << "}";
        }
        json << "],\"images\":[";
        int imageIndex = 0;
        if (!barkTextureUri.empty())
        {
            (void)imageIndex++;
            json << "{\"uri\":\"" << EscapeJson(barkTextureUri) << "\"}";
        }
        if (!leafTextureUri.empty())
        {
            if (imageIndex > 0) json << ",";
            json << "{\"uri\":\"" << EscapeJson(leafTextureUri) << "\"}";
        }
        json << "],";
    }
    json
         << "\"meshes\":[{\"name\":\"" << EscapeJson(name) << "\",\"primitives\":["
         << "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"material\":0,\"mode\":4},"
         << "{\"attributes\":{\"POSITION\":4,\"NORMAL\":5,\"TEXCOORD_0\":6},\"indices\":7,\"material\":1,\"mode\":4}]}],"
         << "\"buffers\":[{\"byteLength\":" << bin.size() << "}],\"bufferViews\":[";
    for (std::size_t i = 0; i < views.size(); ++i)
    {
        if (i) json << ",";
        json << "{\"buffer\":0,\"byteOffset\":" << views[i].offset << ",\"byteLength\":" << views[i].length;
        if (views[i].stride) json << ",\"byteStride\":" << views[i].stride;
        json << ",\"target\":" << views[i].target << "}";
    }
    json << "],\"accessors\":["
         << "{\"bufferView\":0,\"byteOffset\":0,\"componentType\":5126,\"count\":" << mesh.bark.vertices.size() << ",\"type\":\"VEC3\",\"min\":" << Vec3Json(barkMin) << ",\"max\":" << Vec3Json(barkMax) << "},"
         << "{\"bufferView\":0,\"byteOffset\":12,\"componentType\":5126,\"count\":" << mesh.bark.vertices.size() << ",\"type\":\"VEC3\"},"
         << "{\"bufferView\":0,\"byteOffset\":24,\"componentType\":5126,\"count\":" << mesh.bark.vertices.size() << ",\"type\":\"VEC2\"},"
         << "{\"bufferView\":1,\"byteOffset\":0,\"componentType\":5125,\"count\":" << mesh.bark.indices.size() << ",\"type\":\"SCALAR\"},"
         << "{\"bufferView\":2,\"byteOffset\":0,\"componentType\":5126,\"count\":" << mesh.leaves.vertices.size() << ",\"type\":\"VEC3\",\"min\":" << Vec3Json(leafMin) << ",\"max\":" << Vec3Json(leafMax) << "},"
         << "{\"bufferView\":2,\"byteOffset\":12,\"componentType\":5126,\"count\":" << mesh.leaves.vertices.size() << ",\"type\":\"VEC3\"},"
         << "{\"bufferView\":2,\"byteOffset\":24,\"componentType\":5126,\"count\":" << mesh.leaves.vertices.size() << ",\"type\":\"VEC2\"},"
         << "{\"bufferView\":3,\"byteOffset\":0,\"componentType\":5125,\"count\":" << mesh.leaves.indices.size() << ",\"type\":\"SCALAR\"}]}";

    std::string jsonText = json.str();
    while ((jsonText.size() % 4u) != 0u)
        jsonText.push_back(' ');

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    const std::uint32_t magic = 0x46546C67u;
    const std::uint32_t version = 2u;
    const std::uint32_t totalLength = 12u + 8u + static_cast<std::uint32_t>(jsonText.size()) +
        8u + static_cast<std::uint32_t>(bin.size());
    const std::uint32_t jsonLength = static_cast<std::uint32_t>(jsonText.size());
    const std::uint32_t jsonType = 0x4E4F534Au;
    const std::uint32_t binLength = static_cast<std::uint32_t>(bin.size());
    const std::uint32_t binType = 0x004E4942u;
    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&totalLength), sizeof(totalLength));
    file.write(reinterpret_cast<const char*>(&jsonLength), sizeof(jsonLength));
    file.write(reinterpret_cast<const char*>(&jsonType), sizeof(jsonType));
    file.write(jsonText.data(), static_cast<std::streamsize>(jsonText.size()));
    file.write(reinterpret_cast<const char*>(&binLength), sizeof(binLength));
    file.write(reinterpret_cast<const char*>(&binType), sizeof(binType));
    file.write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
    if (outBinarySizeBytes)
        *outBinarySizeBytes = totalLength;
    return file.good();
}

bool VerifyBinaryGlb(const std::filesystem::path& path, std::uint64_t expectedSize, std::string& error)
{
    std::error_code ec;
    const std::uint64_t fileSize = static_cast<std::uint64_t>(std::filesystem::file_size(path, ec));
    if (ec || fileSize < 20u)
    {
        error = "written GLB is missing or too small";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        error = "failed to reopen written GLB";
        return false;
    }

    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t totalLength = 0;
    std::uint32_t jsonLength = 0;
    std::uint32_t jsonType = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(&totalLength), sizeof(totalLength));
    file.read(reinterpret_cast<char*>(&jsonLength), sizeof(jsonLength));
    file.read(reinterpret_cast<char*>(&jsonType), sizeof(jsonType));

    if (magic != 0x46546C67u || version != 2u || jsonType != 0x4E4F534Au)
    {
        error = "written file is not a binary GLB v2";
        return false;
    }
    if (static_cast<std::uint64_t>(totalLength) != fileSize || (expectedSize != 0u && expectedSize != fileSize))
    {
        error = "written GLB size does not match header";
        return false;
    }
    return true;
}

std::filesystem::path CopyTextureDependency(const std::filesystem::path& source,
                                            const std::filesystem::path& textureDir,
                                            const std::string& stem,
                                            std::string& error)
{
    if (source.empty())
        return {};
    std::error_code ec;
    if (!std::filesystem::exists(source, ec))
    {
        error = "texture dependency missing: " + source.generic_string();
        return {};
    }
    std::filesystem::create_directories(textureDir, ec);
    if (ec)
    {
        error = "failed to create texture dependency folder";
        return {};
    }
    std::string extension = source.extension().string();
    if (extension.empty())
        extension = ".png";
    const std::filesystem::path destination = textureDir / (stem + extension);
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        error = "failed to copy texture dependency: " + source.generic_string();
        return {};
    }
    return destination;
}

std::string RelativeUri(const std::filesystem::path& path, const std::filesystem::path& base)
{
    if (path.empty())
        return {};
    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(path, base, ec);
    if (ec || relative.empty())
        relative = path.filename();
    return relative.generic_string();
}
}

bool TreeGlbExporter::IsValidAssetName(const std::string& name, std::string* error)
{
    if (name.empty())
    {
        if (error) *error = "name is empty";
        return false;
    }
    constexpr const char* invalid = "<>:\"/\\|?*";
    if (name.find_first_of(invalid) != std::string::npos)
    {
        if (error) *error = "name contains invalid filename characters";
        return false;
    }
    return true;
}

TreeExportResult TreeGlbExporter::SaveAsAsset(const ixtreemetree::TreeMesh& mesh,
                                              const std::filesystem::path& projectRoot,
                                              const std::string& assetName,
                                              const TreeMaterialBinding& materialBinding)
{
    const auto start = std::chrono::high_resolution_clock::now();
    TreeExportResult result{};
    std::string validationError;
    if (!IsValidAssetName(assetName, &validationError))
    {
        result.error = validationError;
        return result;
    }
    const std::string safeName = SanitizeAssetName(assetName);
    const std::filesystem::path normalizedProjectRoot = NormalizeProjectRoot(projectRoot);
    const std::filesystem::path assetRoot = normalizedProjectRoot / "Assets";
    // One folder per tree, holding the model with its textures and materials (no per-type folders).
    const std::filesystem::path modelDir = assetRoot / safeName;
    const std::filesystem::path textureDir = modelDir / (safeName + "_textures");
    const std::filesystem::path materialDir = modelDir / (safeName + "_materials");
    std::error_code ec;
    std::filesystem::create_directories(modelDir, ec);
    std::filesystem::create_directories(materialDir, ec);
    if (ec)
    {
        result.error = "failed to create asset directories";
        return result;
    }

    const std::filesystem::path modelPath = modelDir / (safeName + ".glb");
    if (std::filesystem::exists(modelPath, ec))
    {
        result.error = "model asset already exists";
        return result;
    }
    // A given material's base colour texture is what the .glb points at (where it is in the project);
    // without one, the built-in texture is copied beside the model.
    const MaterialAsset* barkMaterial =
        materialBinding.barkMaterial ? MaterialAssetManager::Instance().getOrLoad(*materialBinding.barkMaterial) : nullptr;
    const MaterialAsset* leafMaterial =
        materialBinding.leafMaterial ? MaterialAssetManager::Instance().getOrLoad(*materialBinding.leafMaterial) : nullptr;
    const auto materialTexture = [](const MaterialAsset* material) -> std::filesystem::path {
        if (!material || !material->baseColorTexture)
            return {};
        const std::optional<std::filesystem::path> path = AssetDatabase::Instance().resolveGuid(*material->baseColorTexture);
        std::error_code exists;
        return path && std::filesystem::exists(*path, exists) ? *path : std::filesystem::path{};
    };
    std::string dependencyError;
    std::filesystem::path barkTexturePath = materialTexture(barkMaterial);
    if (barkTexturePath.empty())
        barkTexturePath =
            CopyTextureDependency(materialBinding.barkBaseColorTexturePath, textureDir, "bark_basecolor", dependencyError);
    if (!dependencyError.empty())
    {
        result.error = dependencyError;
        return result;
    }
    std::filesystem::path leafTexturePath = materialTexture(leafMaterial);
    if (leafTexturePath.empty())
        leafTexturePath =
            CopyTextureDependency(materialBinding.leafBaseColorTexturePath, textureDir, "leaves_basecolor", dependencyError);
    if (!dependencyError.empty())
    {
        result.error = dependencyError;
        return result;
    }
    const float leafAlphaCutoff = leafMaterial ? leafMaterial->alphaCutoff : materialBinding.leafAlphaCutoff;
    std::uint64_t binarySizeBytes = 0;
    // The leaf cards cut to the visible part of their atlas cells (a convex polygon around the leaf):
    // the texture's transparent parts are then not rasterized in every view and shadow cascade (the
    // same look, a fraction of the area).
    ixtreemetree::TreeMesh trimmedMesh;
    const ixtreemetree::TreeMesh* outputMesh = &mesh;
    // (Only for alpha-masked leaves, and where the leaf texture maps 1:1 onto the cards: an opaque
    // material draws the whole card, and one that tiles or offsets its uvs shows another part of the
    // texture than the cards' uvs say.)
    const bool leafUvsAsGenerated = !leafMaterial ||
        (leafMaterial->alphaMode == MaterialAsset::AlphaMode::Mask &&
         leafMaterial->uvTiling == std::array<float, 2>{1.0f, 1.0f} && leafMaterial->uvOffset == std::array<float, 2>{0.0f, 0.0f});
    if (materialBinding.trimTransparentLeafBorders && leafUvsAsGenerated && !leafTexturePath.empty())
    {
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load(leafTexturePath.string().c_str(), &width, &height, &channels, 4);
        if (pixels && width > 0 && height > 0)
        {
            trimmedMesh = mesh;
            const std::vector<std::uint8_t> rgba(pixels, pixels + static_cast<std::size_t>(width) * height * 4u);
            const ixtreemetree::LeafTrimStats trim = ixtreemetree::cutoutLeafCards(trimmedMesh.leaves, rgba,
                static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
            if (trim.areaBefore > 0.0)
                result.leafAreaReduction = static_cast<float>(1.0 - trim.areaAfter / trim.areaBefore);
            Tracenf("[TREE-PERF] leaf cards cut to their visible texels: cards=%u area -%.1f%%",
                trim.trimmedCards,
                result.leafAreaReduction * 100.0f);
            outputMesh = &trimmedMesh;
        }
        if (pixels)
            stbi_image_free(pixels);
    }
    if (!WriteGlb(*outputMesh,
            modelPath,
            safeName,
            RelativeUri(barkTexturePath, modelDir),
            RelativeUri(leafTexturePath, modelDir),
            leafAlphaCutoff,
            &binarySizeBytes))
    {
        result.error = "failed to write glb";
        return result;
    }
    const std::filesystem::path resolvedModelPath = ResolvePathBestEffort(modelPath);
    std::error_code fileSizeError;
    const std::uint64_t actualFileSize = static_cast<std::uint64_t>(std::filesystem::file_size(modelPath, fileSizeError));
    if (!fileSizeError)
        binarySizeBytes = actualFileSize;
    Tracenf("[TREE-1-DEBUG] save_attempt name=%s resolvedPath=%s fastgltf_method=manual_glb_writer buffer_uri_mode=embedded binary_size_bytes=%llu",
        safeName.c_str(),
        resolvedModelPath.string().c_str(),
        static_cast<unsigned long long>(binarySizeBytes));
    std::string glbVerifyError;
    if (!VerifyBinaryGlb(modelPath, binarySizeBytes, glbVerifyError))
    {
        result.error = glbVerifyError;
        return result;
    }
    if (!barkTexturePath.empty())
        AssetDatabase::Instance().getOrCreateGuid(barkTexturePath);
    if (!leafTexturePath.empty())
        AssetDatabase::Instance().getOrCreateGuid(leafTexturePath);

    // The model's materials: the given ones, or ones made from the built-in textures.
    std::vector<Guid> defaultMaterials;
    defaultMaterials.reserve(2);
    MaterialAssetManager::ImportSummary summary{};
    if (barkMaterial)
    {
        defaultMaterials.push_back(*materialBinding.barkMaterial);
    }
    else
    {
        GltfMaterialSource bark{};
        bark.name = "bark";
        bark.baseColor = {0.42f, 0.26f, 0.12f, 1.0f};
        bark.roughness = 0.82f;
        bark.emissive = {0.0f, 0.0f, 0.0f, 1.0f};  // as a glTF material reads (strength 1)
        bark.baseColorTexturePath = barkTexturePath;
        defaultMaterials.push_back(MaterialAssetManager::Instance().createFromGltfMaterial(bark, materialDir, "bark", &summary));
    }
    if (leafMaterial)
    {
        defaultMaterials.push_back(*materialBinding.leafMaterial);
    }
    else
    {
        GltfMaterialSource leaves{};
        leaves.name = "leaves";
        leaves.baseColor = {0.22f, 0.56f, 0.22f, 1.0f};
        leaves.roughness = 0.7f;
        leaves.emissive = {0.0f, 0.0f, 0.0f, 1.0f};
        leaves.alphaMode = "mask";
        leaves.alphaCutoff = materialBinding.leafAlphaCutoff;
        leaves.baseColorTexturePath = leafTexturePath;
        defaultMaterials.push_back(MaterialAssetManager::Instance().createFromGltfMaterial(leaves, materialDir, "leaves", &summary));
    }

    AssetDatabase::Instance().scan(normalizedProjectRoot);
    AssetDatabase::Instance().writeDefaultMaterials(modelPath, defaultMaterials);
    if (!barkTexturePath.empty())
        AssetDatabase::Instance().getOrCreateGuid(barkTexturePath);
    if (!leafTexturePath.empty())
        AssetDatabase::Instance().getOrCreateGuid(leafTexturePath);
    result.ok = true;
    result.modelPath = modelPath;
    result.materialFolder = materialDir;
    result.durationMs = std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - start).count();
    Tracenf("[TREE-1] saved asset path=Assets/models/%s.glb triangles=%d materials=2 durationMs=%.3f",
        safeName.c_str(),
        mesh.stats.barkTriangles + mesh.stats.leafTriangles,
        result.durationMs);
    Tracenf("[TREE-3] export material textures bark=%s leaves=%s",
        barkTexturePath.generic_string().c_str(),
        leafTexturePath.generic_string().c_str());
    return result;
}
}
