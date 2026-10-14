#include "import_export/tree/TreeGlbExporter.h"
#include "asset/FileAssetReader.h"
#include "asset/MaterialAssetManager.h"
#include "StaticMeshRenderer.h"
#include <ixtreemetree/tree.h>
#include <stb_image.h>
#include <stb_image_write.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
namespace fs = std::filesystem;
int checks = 0, failures = 0;
void Check(const char* name, bool value)
{
    ++checks;
    failures += !value;
    std::cout << name << ": " << (value ? "PASS" : "FAIL") << '\n';
}
void Require(bool value, const std::string& error)
{
    if (!value)
        throw std::runtime_error(error);
}
struct Workspace
{
    fs::path parent = fs::weakly_canonical(fs::temp_directory_path());
    fs::path root =
        parent / ("ixw_impostor_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Workspace()
    {
        Require(fs::create_directory(root), "create temporary project");
    }
    ~Workspace()
    {
        if (root.parent_path() == parent && root.filename().string().starts_with("ixw_impostor_test_"))
        {
            std::error_code error;
            fs::remove_all(root, error);
        }
    }
};
void Texture(const fs::path& path, bool cutout)
{
    fs::create_directories(path.parent_path());
    std::vector<unsigned char> pixels(32 * 32 * 4);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
        {
            auto* pixel = pixels.data() + (y * 32 + x) * 4;
            pixel[0] = cutout ? 80 : 150;
            pixel[1] = cutout ? 200 : 80;
            pixel[2] = cutout ? 60 : 40;
            pixel[3] = cutout && x >= 12 && x < 20 && y >= 12 && y < 20 ? 0 : 255;
        }
    Require(stbi_write_png(path.string().c_str(), 32, 32, 4, pixels.data(), 128) != 0, "write source texture");
}
tree_tool::TreeMaterialBinding Binding(const fs::path& root)
{
    Texture(root / "source_bark.png", false);
    Texture(root / "source_leaf.png", true);
    tree_tool::TreeMaterialBinding binding;
    binding.barkBaseColorTexturePath = root / "source_bark.png";
    binding.leafBaseColorTexturePath = root / "source_leaf.png";
    binding.trimTransparentLeafBorders = false;
    binding.impostor.resolution = 128;
    return binding;
}
ixtreemetree::TreeMesh Card()
{
    ixtreemetree::TreeMesh mesh;
    mesh.bboxMin = {-1, -1, -0.1f};
    mesh.bboxMax = {1, 1, 0};
    mesh.leaves.vertices = {{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
                            {{1, -1, 0}, {0, 0, 1}, {1, 1}},
                            {{1, 1, 0}, {0, 0, 1}, {1, 0}},
                            {{-1, 1, 0}, {0, 0, 1}, {0, 0}}};
    mesh.leaves.indices = {0, 1, 2, 0, 2, 3};
    mesh.bark.vertices = mesh.leaves.vertices;
    for (auto& vertex : mesh.bark.vertices)
        vertex.position.z = -0.1f;
    mesh.bark.indices = mesh.leaves.indices;
    return mesh;
}
void Test()
{
    using namespace tree_tool;
    constexpr float pi = 3.14159265358979323846f;
    Check("distance transition has exact endpoints",
          TreeImpostorWeight(170, 180, 30) == 0 && TreeImpostorWeight(210, 180, 30) == 1);
    Check("transition midpoint", std::abs(TreeImpostorWeight(195, 180, 30) - 0.5f) < 1e-6f);
    const auto wrapped = SelectTreeImpostorView(-pi / 8, 0, 8);
    Check("negative azimuth wraps across atlas seam",
          wrapped.first == 7 && wrapped.second == 0 && std::abs(wrapped.blend - 0.5f) < 1e-5f);
    Check("capture elevation rows",
          SelectTreeImpostorView(0, -pi / 6, 8).row == 0 && SelectTreeImpostorView(0, pi / 6, 8).row == 2);
    const auto uv = TreeImpostorUv(7, 2, 8, 128);
    Check("UV tile leaves guard texels", uv[2] > 7.0f / 8 && uv[2] + uv[0] < 1 && uv[3] + uv[1] < 1);
    Workspace workspace;
    auto binding = Binding(workspace.root);
    const auto mesh = Card();
    const auto result = TreeGlbExporter::SaveAsAsset(mesh, workspace.root, "Card", binding);
    Require(result.ok, result.error);
    Require(result.impostorBaked, result.impostorWarning);
    client::asset::FileAssetReader assets(workspace.root);
    std::string error;
    auto data = LoadTreeImpostor(assets, "Assets/Card/Card.glb", error);
    Require(data.has_value(), error);
    const auto metadataPath = result.modelPath.string() + ".impostor.json";
    const auto originalMetadata = assets.ReadText(metadataPath);
    Require(originalMetadata.has_value(), "read metadata fixture");
    const auto damagedMetadata = [&](const char* name, const std::string& from, const std::string& to) {
        auto text = *originalMetadata;
        const auto position = text.find(from);
        Require(position != std::string::npos, "metadata fixture key");
        text.replace(position, from.size(), to);
        {
            std::ofstream file(metadataPath, std::ios::binary | std::ios::trunc);
            file << text;
        }
        error.clear();
        Check(name, !LoadTreeImpostor(assets, "Assets/Card/Card.glb", error) && error.starts_with("Invalid"));
        {
            std::ofstream file(metadataPath, std::ios::binary | std::ios::trunc);
            file << *originalMetadata;
        }
    };
    damagedMetadata("out-of-range metadata refused before integer conversion", "\"azimuths\":8", "\"azimuths\":1e30");
    damagedMetadata("fractional atlas dimensions rejected", "\"azimuths\":8", "\"azimuths\":7.5");
    damagedMetadata("missing bounds center rejected", "\"center\"", "\"omittedCenter\"");
    Check("saved atlas has 24 directions", data->azimuths == 8 && data->elevations == 3 && data->resolution == 128);
    Check("baked source and output fingerprints match", ValidateTreeImpostor(assets, *data));
    const auto dependencies = AssetDatabase::Instance().loadDependencies(result.modelPath);
    Check("model metadata includes atlas dependencies", dependencies.size() >= 8);
    StaticMeshRenderer billboard;
    Check("generated glTF is a two-triangle static model", billboard.LoadCpu(assets, data->billboardPath) &&
                                                               billboard.VertexCount() == 4 &&
                                                               billboard.IndexCount() == 6);
    int width = 0, height = 0, channels = 0;
    const auto folder = workspace.root / fs::path(data->billboardPath).parent_path();
    auto* pixels = stbi_load((folder / "albedo.png").string().c_str(), &width, &height, &channels, 4);
    Require(pixels != nullptr, "load baked albedo");
    Check("atlas dimensions", width == 1024 && height == 384);
    const auto at = [&](int x, int y) { return pixels + (static_cast<std::size_t>(128 + y) * width + x) * 4; };
    Check("leaf alpha hole reveals bark behind it", at(64, 64)[0] > at(64, 64)[1] && at(64, 64)[3] == 255);
    Check("leaf survives away from alpha hole", at(40, 64)[1] > at(40, 64)[0] && at(40, 64)[3] == 255);
    Check("empty atlas corners stay transparent", at(3, 3)[3] == 0);
    bool filled = false;
    for (int x = 3; x < 32; ++x)
        filled |= at(x, 64)[3] == 0 && at(x, 64)[1] != 0;
    Check("transparent edge RGB is dilated without filling alpha", filled);
    stbi_image_free(pixels);
    pixels = stbi_load((folder / "normal.png").string().c_str(), &width, &height, &channels, 4);
    Require(pixels != nullptr, "load baked normal");
    const auto* normal = pixels + (static_cast<std::size_t>(192) * width + 40) * 4;
    Check("front surface normal preserves its direction",
          normal[0] >= 127 && normal[0] <= 128 && normal[1] >= 127 && normal[1] <= 128 && normal[2] == 255);
    stbi_image_free(pixels);
    // Packaging/relocation uses project-relative references, including material/texture GUID metadata.
    const auto relocated = workspace.root / "Relocated";
    fs::create_directories(relocated);
    fs::copy(workspace.root / "Assets", relocated / "Assets", fs::copy_options::recursive);
    client::asset::FileAssetReader relocatedAssets(relocated);
    Check("bake survives project relocation",
          LoadTreeImpostor(relocatedAssets, "Assets/Card/Card.glb", error).has_value());
    {
        std::ofstream change(folder / "normal.png", std::ios::binary | std::ios::app);
        change.put('x');
    }
    Check("edited atlas is rejected", !LoadTreeImpostor(assets, "Assets/Card/Card.glb", error));
    StaticMeshRenderer parent;
    Check("stale atlas still loads full tree",
          parent.LoadCpu(assets, "Assets/Card/Card.glb") && parent.IndexCount() == 12);
    fs::remove(relocated / fs::path(data->billboardPath).parent_path() / "albedo.png");
    Check("missing atlas is rejected", !LoadTreeImpostor(relocatedAssets, "Assets/Card/Card.glb", error));
    auto guid = Guid::fromString(data->materials[1]);
    Require(guid.has_value(), "leaf material GUID");
    auto* material = MaterialAssetManager::Instance().getOrLoad(*guid);
    Require(material != nullptr, "leaf material");
    const auto signature = TreeImpostorMaterialSignature(*material);
    auto modified = *material;
    modified.baseColor[1] *= 0.5f;
    Check("live material edit changes bake signature", TreeImpostorMaterialSignature(modified) != signature);
    modified = *material;
    modified.name = "Renamed";
    Check("renaming material keeps bake valid", TreeImpostorMaterialSignature(modified) == signature);
    const unsigned char tangentNormal[] = {255, 128, 128, 255};
    const auto normalPath = workspace.root / "Assets" / "tangent_normal.png";
    Require(stbi_write_png(normalPath.string().c_str(), 1, 1, 4, tangentNormal, 4) != 0, "normal map fixture");
    material->normalTexture = AssetDatabase::Instance().getOrCreateGuid(normalPath);
    Require(MaterialAssetManager::Instance().save(*material), "save normal material");
    auto normalBinding = binding;
    normalBinding.leafMaterial = *guid;
    const auto normalResult = TreeGlbExporter::SaveAsAsset(mesh, workspace.root, "Mapped", normalBinding);
    Require(normalResult.ok && normalResult.impostorBaked, normalResult.impostorWarning);
    pixels = stbi_load((normalResult.modelPath.parent_path() / "Mapped_impostor" / "normal.png").string().c_str(),
                       &width, &height, &channels, 4);
    Require(pixels != nullptr, "normal mapped atlas");
    normal = pixels + (static_cast<std::size_t>(192) * width + 40) * 4;
    Check("source tangent normal survives billboard re-encoding",
          normal[0] >= 254 && normal[1] >= 126 && normal[1] <= 129 && normal[2] >= 126 && normal[2] <= 129);
    stbi_image_free(pixels);
    binding.impostor.enabled = false;
    const auto disabled = TreeGlbExporter::SaveAsAsset(mesh, workspace.root, "Disabled", binding);
    Check("disabled bake saves only ordinary tree",
          disabled.ok && !disabled.impostorBaked && !fs::exists(disabled.modelPath.string() + ".impostor.json"));
    const auto beforeBake = assets.ReadAll(disabled.modelPath.generic_string());
    binding.impostor.enabled = true;
    Check("existing tree can be baked without exporting again",
          StaticMeshRenderer::BakeTreeImpostorAsset(assets, disabled.modelPath, binding.impostor, error));
    Check("existing-tree bake keeps original mesh bytes",
          assets.ReadAll(disabled.modelPath.generic_string()) == beforeBake);
    Check("existing-tree bake loads valid metadata",
          LoadTreeImpostor(assets, disabled.modelPath.generic_string(), error).has_value());
    const auto defaults = AssetDatabase::Instance().loadDefaultMaterials(disabled.modelPath);
    const std::array<MeshImpostorPart, 1> single{{{mesh.bark.vertices, mesh.bark.indices}}};
    std::vector<Guid> genericDependencies;
    Check("one-material ordinary model atlas",
          BakeMeshImpostor(single, disabled.modelPath, {defaults[0]}, binding.impostor, genericDependencies, error));
    auto genericData = LoadTreeImpostor(assets, disabled.modelPath.generic_string(), error);
    Check("one-material atlas metadata and source validation",
          genericData && genericData->materials.size() == 1 && ValidateTreeImpostor(assets, *genericData));
    auto thirdPart = mesh.bark;
    for (auto& vertex : thirdPart.vertices) vertex.position.x += 3;
    const std::array<MeshImpostorPart, 3> three{{{mesh.bark.vertices, mesh.bark.indices},
        {mesh.leaves.vertices, mesh.leaves.indices}, {thirdPart.vertices, thirdPart.indices}}};
    Check("multi-material ordinary model atlas",
          BakeMeshImpostor(three, disabled.modelPath, {defaults[0], defaults[1], defaults[0]}, binding.impostor, genericDependencies, error));
    genericData = LoadTreeImpostor(assets, disabled.modelPath.generic_string(), error);
    Check("multi-material atlas retains every material and full bounds",
          genericData && genericData->materials.size() == 3 && genericData->center[0] > 1 && ValidateTreeImpostor(assets, *genericData));
    Check("material/part mismatch is rejected",
          !BakeMeshImpostor(three, disabled.modelPath, {defaults[0]}, binding.impostor, genericDependencies, error));
    Check("generic bake preserves source model bytes", assets.ReadAll(disabled.modelPath.generic_string()) == beforeBake);
    binding.impostor.enabled = true;
    modified = *material;
    modified.alphaMode = MaterialAsset::AlphaMode::Blend;
    *material = modified;
    Require(MaterialAssetManager::Instance().save(modified), "save blend material");
    binding.leafMaterial = *guid;
    const auto unsupported = TreeGlbExporter::SaveAsAsset(mesh, workspace.root, "Blend", binding);
    Check("unsupported material saves tree and reports fallback",
          unsupported.ok && !unsupported.impostorBaked && !unsupported.impostorWarning.empty());
    std::vector<Guid> output;
    auto invalid = mesh;
    invalid.leaves.indices[0] = 999;
    const auto materials = AssetDatabase::Instance().loadDefaultMaterials(result.modelPath);
    Check("invalid geometry rejected before rasterization",
          !BakeTreeImpostor(invalid, result.modelPath, materials, binding.impostor, output, error));
    binding.impostor.distance = std::numeric_limits<float>::quiet_NaN();
    Check("nonfinite settings rejected",
          !BakeTreeImpostor(mesh, result.modelPath, materials, binding.impostor, output, error));
}
} // namespace
int main(int argc, char** argv)
{
    try
    {
        if (argc == 3 && std::string(argv[1]) == "--export")
        {
            const fs::path root = fs::absolute(argv[2]);
            fs::create_directories(root);
            ixtreemetree::Tree tree;
            auto binding = Binding(root);
            binding.impostor.resolution = 256;
            const auto mesh = tree.generate();
            const auto result = tree_tool::TreeGlbExporter::SaveAsAsset(mesh, root, "ForestOak", binding);
            Require(result.ok, result.error);
            Require(result.impostorBaked, result.impostorWarning);
            std::cout << "EXPORTED " << result.modelPath
                      << " triangles=" << (mesh.bark.indices.size() + mesh.leaves.indices.size()) / 3
                      << " bake_ms=" << result.durationMs << '\n';
            return 0;
        }
        Test();
    }
    catch (const std::exception& error)
    {
        ++failures;
        std::cerr << error.what() << '\n';
    }
    std::cout << checks - failures << '/' << checks << " checks passed\n";
    return failures ? 1 : 0;
}
