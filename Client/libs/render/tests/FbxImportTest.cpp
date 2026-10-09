#include "asset/AssetDatabase.h"
#include "asset/AssetLibrary.h"
#include "asset/MaterialAssetManager.h"
#include "import_export/AssimpImporter.h"
#include "import_export/FbxAssetSidecars.h"

#include <assimp/Exporter.hpp>
#include <assimp/material.h>
#include <assimp/scene.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace
{
namespace fs = std::filesystem;
int checks = 0, failures = 0;
void Check(const char* name, bool value)
{
    ++checks;
    failures += !value;
    std::cout << "FBX " << name << ": " << (value ? "PASS" : "FAIL") << '\n';
}
void Require(bool value, const std::string& error)
{
    if (!value) throw std::runtime_error(error);
}
struct Workspace
{
    fs::path parent = fs::weakly_canonical(fs::temp_directory_path());
    fs::path root = parent / ("ixw_fbx_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Workspace() { Require(fs::create_directory(root), "cannot create FBX test directory"); }
    ~Workspace()
    {
        AssetLibrary::SetFbxSidecarProcessor(nullptr);
        if (root.parent_path() == parent && root.filename().string().starts_with("ixw_fbx_test_"))
        {
            std::error_code error;
            fs::remove_all(root, error);
        }
    }
};
std::vector<unsigned char> Read(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void Image(const fs::path& path, unsigned char r, unsigned char g, unsigned char b)
{
    fs::create_directories(path.parent_path());
    const unsigned char pixels[] = {r, g, b, 255, r, g, b, 255, r, g, b, 255, r, g, b, 255};
    Require(stbi_write_png(path.string().c_str(), 2, 2, 4, pixels, 8) != 0, "cannot create PNG");
}
using Maps = std::vector<std::pair<aiTextureType, std::string>>;
void Model(const fs::path& path, const std::vector<Maps>& maps, const fs::path& embedded = {})
{
    aiScene scene;
    scene.mRootNode = new aiNode("Root");
    scene.mNumMaterials = scene.mNumMeshes = static_cast<unsigned int>(maps.size());
    scene.mMaterials = new aiMaterial*[maps.size()]{};
    scene.mMeshes = new aiMesh*[maps.size()]{};
    scene.mRootNode->mNumChildren = scene.mNumMeshes;
    scene.mRootNode->mChildren = new aiNode*[maps.size()]{};
    for (unsigned int i = 0; i < maps.size(); ++i)
    {
        auto* material = scene.mMaterials[i] = new aiMaterial;
        aiString name("Bark_" + std::to_string(i));
        material->AddProperty(&name, AI_MATKEY_NAME);
        const aiColor4D color(0.8f, 0.7f, 0.6f, 1.0f);
        material->AddProperty(&color, 1, AI_MATKEY_COLOR_DIFFUSE);
        for (const auto& [type, reference] : maps[i])
        {
            const aiString texture(reference);
            material->AddProperty(&texture, AI_MATKEY_TEXTURE(type, 0));
        }
        auto* mesh = scene.mMeshes[i] = new aiMesh;
        mesh->mName = aiString("Triangle_" + std::to_string(i));
        mesh->mMaterialIndex = i;
        mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
        mesh->mNumVertices = 3;
        mesh->mVertices = new aiVector3D[3]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        mesh->mNormals = new aiVector3D[3]{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
        mesh->mTextureCoords[0] = new aiVector3D[3]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        mesh->mNumUVComponents[0] = 2;
        mesh->mNumFaces = 1;
        mesh->mFaces = new aiFace[1];
        mesh->mFaces[0].mNumIndices = 3;
        mesh->mFaces[0].mIndices = new unsigned int[3]{0, 1, 2};
        auto* node = scene.mRootNode->mChildren[i] = new aiNode(mesh->mName.C_Str());
        node->mParent = scene.mRootNode;
        node->mNumMeshes = 1;
        node->mMeshes = new unsigned int[1]{i};
    }
    if (!embedded.empty())
    {
        const auto bytes = Read(embedded);
        scene.mNumTextures = 1;
        scene.mTextures = new aiTexture*[1]{new aiTexture};
        auto* texture = scene.mTextures[0];
        texture->mFilename = aiString("embedded albedo.png");
        std::memcpy(texture->achFormatHint, "png", 4);
        texture->mWidth = static_cast<unsigned int>(bytes.size());
        texture->pcData = new aiTexel[(bytes.size() + sizeof(aiTexel) - 1) / sizeof(aiTexel)]{};
        std::memcpy(texture->pcData, bytes.data(), bytes.size());
    }
    else
    {
        // Assimp 6's exporter dereferences an absent path-table entry for external textures.
        // Empty Video payloads populate that table while remaining external references on import.
        std::set<std::string> references;
        for (const auto& materialMaps : maps)
            for (const auto& [type, reference] : materialMaps) references.insert(reference);
        scene.mNumTextures = static_cast<unsigned int>(references.size());
        scene.mTextures = new aiTexture*[references.size()]{};
        std::unordered_map<std::string, unsigned int> indices;
        unsigned int index = 0;
        for (const auto& reference : references)
        {
            indices.emplace(reference, index);
            auto* texture = scene.mTextures[index++] = new aiTexture;
            texture->mFilename = aiString(reference);
            std::memcpy(texture->achFormatHint, "png", 4);
        }
        for (unsigned int i = 0; i < maps.size(); ++i)
            for (const auto& [type, reference] : maps[i])
            {
                const aiString texture("*" + std::to_string(indices.at(reference)));
                scene.mMaterials[i]->AddProperty(&texture, AI_MATKEY_TEXTURE(type, 0));
            }
    }
    Assimp::Exporter exporter;
    Require(exporter.Export(&scene, embedded.empty() ? "fbxa" : "fbx", path.string()) == aiReturn_SUCCESS, exporter.GetErrorString());
}
std::size_t Count(const fs::path& root, const std::string& extension)
{
    std::size_t count = 0;
    if (fs::exists(root))
        for (const auto& file : fs::recursive_directory_iterator(root))
            count += file.is_regular_file() && file.path().extension() == extension;
    return count;
}
AssetLibrary::Entry Import(AssetLibrary& library, const fs::path& source, const fs::path& target)
{
    AssetLibrary::Entry model;
    fs::path finalPath;
    std::string error;
    Require(library.ImportFileToFolder(source, target, model, finalPath, error), error);
    Require(library.Refresh(error), error);
    return library.FindById(model.id).value_or(model);
}
MaterialAsset Material(const fs::path& model, std::size_t index = 0)
{
    const auto materials = AssetDatabase::Instance().loadDefaultMaterials(model);
    Require(index < materials.size(), "missing material slot");
    auto* material = MaterialAssetManager::Instance().getOrLoad(materials[index]);
    Require(material != nullptr, "cannot load native material");
    return *material;
}
fs::path Texture(const std::optional<Guid>& guid)
{
    Require(guid.has_value(), "missing texture GUID");
    auto path = AssetDatabase::Instance().resolveGuid(*guid);
    Require(path.has_value(), "unresolved texture GUID");
    return *path;
}
}

int main(int argc, char** argv)
{
    try
    {
        Workspace workspace;
        const auto source = workspace.root / "download";
        const auto project = workspace.root / "project";
        const auto assets = project / "Assets";
        fs::create_directories(source);
        fs::create_directories(assets);
        auto& db = AssetDatabase::Instance();
        auto& manager = MaterialAssetManager::Instance();
        db.scan(project);
        AssetLibrary::SetFbxSidecarProcessor(&ProcessImportedFbxAsset);
        AssetLibrary library(project, assets);
        Require(library.Initialize(), "library initialization failed");

        Image(source / "Textures/albedo.png", 180, 70, 20);
        Image(source / "Textures/normal.png", 128, 128, 255);
        Image(source / "Textures/height.png", 64, 64, 64);
        Image(source / "Textures/specular.png", 220, 220, 220);
        const Maps maps{{aiTextureType_DIFFUSE, "Textures\\albedo.png"}, {aiTextureType_NORMALS, "Textures/normal.png"},
                        {aiTextureType_HEIGHT, "Textures/height.png"}, {aiTextureType_SPECULAR, "Textures/specular.png"}};
        Model(source / "shared-tree.fbx", {maps, maps});
        auto entry = Import(library, source / "shared-tree.fbx", assets);
        const auto model = library.AbsolutePath(entry);
        const auto initialDefaults = db.loadDefaultMaterials(model);
        Check("two FBX material slots become native materials", initialDefaults.size() == 2);
        const auto first = Material(model), second = Material(model, 1);
        Check("original relative texture path survives copying FBX", Read(Texture(first.baseColorTexture)) == Read(source / "Textures/albedo.png"));
        Check("shared albedo has one GUID across materials", first.baseColorTexture == second.baseColorTexture);
        Check("normal texture assigned", Read(Texture(first.normalTexture)) == Read(source / "Textures/normal.png"));
        Check("bump kept as height rather than normal", first.heightTexture && first.heightTexture != first.normalTexture);
        Check("all referenced textures copied including specular", Count(assets / "shared-tree_textures", ".png") == 4);
        Check("source color preserved", std::abs(first.baseColor[0] - 0.8f) < 0.001f);
        auto contents = library.QueryModelContents(entry);
        Check("expanded model exposes its two materials and four textures", contents.assets.size() == 6);
        Check("materials appear before textures", contents.assets.size() >= 2 && contents.assets[0].category == AssetLibrary::Category::Material && contents.assets[1].category == AssetLibrary::Category::Material);
        std::set<std::string> ids;
        bool usable = true;
        for (const auto& asset : contents.assets)
            usable &= ids.insert(asset.id).second && library.FindById(asset.id).has_value() && fs::is_regular_file(library.AbsolutePath(asset));
        Check("children are distinct real browser assets", usable);
        Check("model records native dependencies", db.loadDependencies(model).size() == 6);

        entry = Import(library, source / "shared-tree.fbx", assets);
        Check("repeat import keeps material GUIDs", db.loadDefaultMaterials(model) == initialDefaults);
        Check("repeat import creates no duplicate textures/materials", Count(assets / "shared-tree_textures", ".png") == 4 && Count(assets / "shared-tree_materials", ".material") == 2);
        const auto loaded = AssimpImporter{}.importFile(model);
        Check("runtime reload finds extracted texture folder", loaded.success && loaded.materials.size() == 2 && loaded.materials[0].baseColorTexturePath == Texture(first.baseColorTexture));

        auto edited = first;
        edited.baseColor[0] = 0.1f;
        Require(manager.save(edited), "failed material edit");
        manager.invalidate(edited.guid);
        entry = Import(library, source / "shared-tree.fbx", assets);
        Check("reimport preserves user material edits", std::abs(manager.getOrLoad(first.guid)->baseColor[0] - 0.1f) < 0.001f);
        Check("changed material receives a separate native version", Material(model).guid != first.guid && Material(model, 1).guid == second.guid);
        const auto versionDefaults = db.loadDefaultMaterials(model);
        entry = Import(library, source / "shared-tree.fbx", assets);
        Check("identical reimport reuses conflict version", db.loadDefaultMaterials(model) == versionDefaults && Count(assets / "shared-tree_materials", ".material") == 3);
        const auto editedTexture = Texture(first.baseColorTexture);
        Image(editedTexture, 1, 2, 3);
        const auto editedBytes = Read(editedTexture);
        entry = Import(library, source / "shared-tree.fbx", assets);
        Check("reimport preserves edited texture bytes", Read(editedTexture) == editedBytes);
        Check("original imported image receives a new shared texture GUID", Material(model).baseColorTexture != first.baseColorTexture && Material(model).baseColorTexture == Material(model, 1).baseColorTexture && Read(Texture(Material(model).baseColorTexture)) == Read(source / "Textures/albedo.png"));

        Image(source / "A/same.png", 10, 20, 30);
        Image(source / "B/same.png", 220, 210, 200);
        Model(source / "collision.fbx", {{{aiTextureType_DIFFUSE, "A/same.png"}}, {{aiTextureType_DIFFUSE, "B/same.png"}}});
        auto collision = Import(library, source / "collision.fbx", assets);
        const auto collisionPath = library.AbsolutePath(collision);
        const auto collisionA = Material(collisionPath), collisionB = Material(collisionPath, 1);
        Check("same basename with different bytes gets separate GUIDs", collisionA.baseColorTexture != collisionB.baseColorTexture);
        Check("same basename retains both original images", Read(Texture(collisionA.baseColorTexture)) == Read(source / "A/same.png") && Read(Texture(collisionB.baseColorTexture)) == Read(source / "B/same.png"));
        collision = Import(library, source / "collision.fbx", assets);
        Check("basename collision is stable on reimport", Count(assets / "collision_textures", ".png") == 2 && Material(collisionPath).baseColorTexture == collisionA.baseColorTexture);

        Model(source / "missing.fbx", {{{aiTextureType_DIFFUSE, "Missing/same.png"}, {aiTextureType_NORMALS, "Missing/absent.png"}}});
        auto missing = Import(library, source / "missing.fbx", assets);
        Check("ambiguous basename is not guessed", !Material(library.AbsolutePath(missing)).baseColorTexture);
        Check("missing texture warnings survive as model contents", library.QueryModelContents(missing).missingTextures.size() == 2);
        std::string renameError;
        AssetLibrary::Entry renamedMissing;
        Require(library.RenameAsset(missing.id, "missing-renamed", false, renamedMissing, renameError), renameError);
        missing = renamedMissing;
        Check("model rename keeps its import diagnostics", library.QueryModelContents(missing).missingTextures.size() == 2);
        Require(library.MoveAssetToSubpath(missing.id, "Moved", renamedMissing, renameError), renameError);
        missing = renamedMissing;
        Check("model move keeps its import diagnostics", library.QueryModelContents(missing).missingTextures.size() == 2);

        Model(source / "embedded.fbx", {{{aiTextureType_DIFFUSE, "*0"}}, {{aiTextureType_DIFFUSE, "*0"}}}, source / "Textures/albedo.png");
        auto embedded = Import(library, source / "embedded.fbx", assets);
        const auto embeddedPath = library.AbsolutePath(embedded);
        Check("binary FBX embedded PNG extracted correctly", Read(Texture(Material(embeddedPath).baseColorTexture)) == Read(source / "Textures/albedo.png"));
        Check("shared embedded image extracted once", Count(assets / "embedded_textures", ".png") == 1 && Material(embeddedPath).baseColorTexture == Material(embeddedPath, 1).baseColorTexture);
        const auto runtimeEmbedded = AssimpImporter{}.importFile(embeddedPath);
        Check("embedded runtime reload uses extracted native image", runtimeEmbedded.success && runtimeEmbedded.materials[0].baseColorTexturePath == Texture(Material(embeddedPath).baseColorTexture));
        embedded = Import(library, source / "embedded.fbx", assets);
        Check("embedded reimport is idempotent", Count(assets / "embedded_textures", ".png") == 1 && Count(assets / "embedded_materials", ".material") == 2);

        Image(source / "PBR/ao.png", 128, 128, 128);
        Image(source / "PBR/rough.png", 64, 64, 64);
        Image(source / "PBR/metal.png", 192, 192, 192);
        const auto modernSource = source / "modern.fbx";
        Model(modernSource, {{{aiTextureType_DIFFUSE, "Textures/albedo.png"}, {aiTextureType_AMBIENT, "PBR/ao.png"},
            {aiTextureType_SPECULAR, "PBR/metal.png"}, {aiTextureType_SHININESS, "PBR/rough.png"}}});
        const auto modernBytes = Read(modernSource);
        std::string modernText(modernBytes.begin(), modernBytes.end());
        for (const auto& [legacy, modern] : std::vector<std::pair<std::string, std::string>>{
                {"DiffuseColor", "Maya|TEX_color_map"}, {"AmbientColor", "Maya|TEX_ao_map"},
                {"SpecularColor", "Maya|TEX_metallic_map"}, {"ShininessExponent", "Maya|TEX_roughness_map"}})
        {
            const auto at = modernText.rfind(", \"" + legacy + "\"");
            Require(at != std::string::npos, "missing FBX texture connection");
            modernText.replace(at + 3, legacy.size(), modern);
        }
        {
            std::ofstream file(modernSource, std::ios::binary | std::ios::trunc);
            file << modernText;
        }
        auto modernEntry = Import(library, modernSource, assets);
        const auto modernMaterial = Material(library.AbsolutePath(modernEntry));
        Check("Maya FBX PBR maps become native material references", modernMaterial.baseColorTexture && modernMaterial.aoTexture && modernMaterial.roughnessTexture && modernMaterial.metallicTexture && modernMaterial.metallicRoughnessTexture);
        Check("single metallic/roughness images are not mistaken for packed maps", modernMaterial.metallicRoughnessTexture != modernMaterial.metallicTexture && modernMaterial.metallicRoughnessTexture != modernMaterial.roughnessTexture);
        int modernW = 0, modernH = 0, modernChannels = 0;
        auto* modernPixels = stbi_load(Texture(modernMaterial.metallicRoughnessTexture).string().c_str(), &modernW, &modernH, &modernChannels, 4);
        Check("FBX PBR import packs correct linear RGB channels", modernPixels && modernPixels[0] == 128 && modernPixels[1] == 64 && modernPixels[2] == 192);
        stbi_image_free(modernPixels);
        Check("packed map appears in expanded FBX model contents", library.QueryModelContents(modernEntry).assets.size() == 6);

        Image(assets / "pbr/ao.png", 128, 128, 128);
        Image(assets / "pbr/rough.png", 64, 64, 64);
        Image(assets / "pbr/metal.png", 192, 192, 192);
        db.runtimeAdd(assets / "pbr/ao.png");
        db.runtimeAdd(assets / "pbr/rough.png");
        db.runtimeAdd(assets / "pbr/metal.png");
        GltfMaterialSource pbr;
        pbr.name = "PBR";
        pbr.aoTexturePath = assets / "pbr/ao.png";
        pbr.roughnessTexturePath = assets / "pbr/rough.png";
        pbr.metallicTexturePath = assets / "pbr/metal.png";
        pbr.roughness = 0.35f;
        pbr.metallic = 0.65f;
        const auto pbrGuid = manager.createFromGltfMaterial(pbr, assets / "pbr", pbr.name);
        auto nativePbr = *manager.getOrLoad(pbrGuid);
        const auto oldPack = Texture(nativePbr.metallicRoughnessTexture);
        int width = 0, height = 0, channels = 0;
        auto* pixels = stbi_load(oldPack.string().c_str(), &width, &height, &channels, 4);
        Check("separate PBR maps packed into AO/Roughness/Metallic RGB", pixels && width == 2 && height == 2 && pixels[0] == 128 && pixels[1] == 64 && pixels[2] == 192);
        stbi_image_free(pixels);
        Check("PBR scalar factors preserved", nativePbr.roughness == pbr.roughness && nativePbr.metallic == pbr.metallic);
        Image(assets / "pbr/rough-new.png", 32, 32, 32);
        db.runtimeAdd(assets / "pbr/rough-new.png");
        pbr.roughnessTexturePath = assets / "pbr/rough-new.png";
        const auto newPbr = manager.createFromGltfMaterial(pbr, assets / "pbr", pbr.name);
        Check("reimported PBR material keeps original version and packed image", newPbr != pbrGuid && fs::is_regular_file(oldPack));

        // Runtime dependency loading must work after the downloaded source has disappeared and the project moved.
        const auto relocated = workspace.root / "relocated";
        fs::copy(project, relocated, fs::copy_options::recursive);
        fs::rename(source, workspace.root / "offline-download");
        db.scan(relocated);
        for (const auto& guid : db.loadDefaultMaterials(relocated / "Assets/shared-tree.fbx")) manager.invalidate(guid);
        AssetLibrary reopened(relocated, relocated / "Assets");
        Require(reopened.InitializeReadOnly(), "read-only library reload failed");
        const auto relocatedEntry = reopened.FindById(entry.id);
        Check("model survives read-only project relocation", relocatedEntry.has_value());
        const auto movedMaterial = Material(relocated / "Assets/shared-tree.fbx");
        Check("material textures resolve inside relocated project", Texture(movedMaterial.baseColorTexture).string().starts_with(relocated.string()));
        Check("expanded contents work without original download", relocatedEntry && reopened.QueryModelContents(*relocatedEntry).assets.size() == 6);
        const auto movedMissing = reopened.FindById(missing.id);
        Check("missing warnings persist after reopen", movedMissing && reopened.QueryModelContents(*movedMissing).missingTextures.size() == 2);

        if (argc > 1)
        {
            Require(!fs::exists(argv[1]), "fixture output already exists");
            fs::copy(relocated, fs::path(argv[1]), fs::copy_options::recursive);
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "FBX exception: " << error.what() << '\n';
        ++failures;
    }
    std::cout << "FBX checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
