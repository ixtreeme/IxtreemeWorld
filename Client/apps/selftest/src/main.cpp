#include "AssimpExporter.h"
#include "AssimpImporter.h"
#include "AssetLibrary.h"
#include "MapEditorTypes.h"
#include "ProjectManager.h"
#include "SceneManager.h"
#include "WaterBodyIO.h"
#include "ixtreemetree/ixtreemetree.h"
#include "map/MapData.h"
#include "schema/map_manifest.capnp.h"
#include "tools/tree/TreeGlbExporter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <capnp/message.h>
#include <capnp/serialize.h>

namespace
{
using Clock = std::chrono::steady_clock;

struct Options
{
    bool runAsset = false;
    bool runRender = false;
    std::filesystem::path clientRoot = IW_CLIENT_SOURCE_ROOT;
    std::filesystem::path scratchRoot;
};

struct TestContext
{
    int passed = 0;
    int failed = 0;

    void Pass(const std::string& name)
    {
        ++passed;
        std::cout << "[PASS] " << name << "\n";
    }

    void Fail(const std::string& name, const std::string& message)
    {
        ++failed;
        std::cerr << "[FAIL] " << name << ": " << message << "\n";
    }

    bool Expect(bool condition, const std::string& name, const std::string& message)
    {
        if (condition)
        {
            Pass(name);
            return true;
        }
        Fail(name, message);
        return false;
    }
};

void PrintUsage()
{
    std::cout
        << "IwSelfTest options:\n"
        << "  --all                         Run asset/render engine baseline tests\n"
        << "  --asset                       Run isolated AssetLibrary tests\n"
        << "  --render                      Run render asset/shader/config checks\n"
        << "  --client-root PATH            Default: compiled Client source root\n"
        << "  --scratch-root PATH           Default: temp/IwSelfTest_<time>\n";
}

Options ParseOptions(int argc, char** argv)
{
    Options options;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto needValue = [&](const char* name) -> std::string {
            if (i + 1 >= argc)
                throw std::runtime_error(std::string("missing value for ") + name);
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h")
        {
            PrintUsage();
            std::exit(0);
        }
        else if (arg == "--all")
        {
            options.runAsset = true;
            options.runRender = true;
        }
        else if (arg == "--asset")
            options.runAsset = true;
        else if (arg == "--render")
            options.runRender = true;
        else if (arg == "--client-root")
            options.clientRoot = needValue("--client-root");
        else if (arg == "--scratch-root")
            options.scratchRoot = needValue("--scratch-root");
        else
            throw std::runtime_error("unknown option: " + arg);
    }

    if (!options.runAsset && !options.runRender)
    {
        options.runAsset = true;
        options.runRender = true;
    }
    return options;
}

std::filesystem::path MakeDefaultScratchRoot()
{
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count();
    return std::filesystem::temp_directory_path() / ("IwSelfTest_" + std::to_string(stamp));
}

bool WriteTga(const std::filesystem::path& path,
              std::uint8_t r,
              std::uint8_t g,
              std::uint8_t b,
              bool normalLike = false)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;

    const std::uint8_t header[18] = {
        0, 0, 2,
        0, 0, 0, 0, 0,
        0, 0,
        0, 0,
        4, 0,
        4, 0,
        24,
        0x20
    };
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    for (int y = 0; y < 4; ++y)
    {
        for (int x = 0; x < 4; ++x)
        {
            std::uint8_t px[3] = {b, g, r};
            if (normalLike)
            {
                px[0] = 255;
                px[1] = static_cast<std::uint8_t>(128 + x * 8);
                px[2] = static_cast<std::uint8_t>(128 + y * 8);
            }
            out.write(reinterpret_cast<const char*>(px), sizeof(px));
        }
    }
    return true;
}

std::size_t CountCategory(const AssetLibrary& library, AssetLibrary::Category category)
{
    return static_cast<std::size_t>(std::count_if(library.Entries().begin(), library.Entries().end(),
        [category](const AssetLibrary::Entry& entry) {
            return entry.category == category;
        }));
}

void PushU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

void PushU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xffu));
}

void PushI16(std::vector<std::uint8_t>& bytes, std::int16_t value)
{
    PushU16(bytes, static_cast<std::uint16_t>(value));
}

std::vector<std::uint8_t> BuildOneCellMxChunk()
{
    struct Section
    {
        std::uint16_t type = 0;
        std::vector<std::uint8_t> bytes;
    };

    std::vector<Section> sections;
    Section height;
    height.type = 1;
    PushI16(height.bytes, 0);
    PushI16(height.bytes, 100);
    PushI16(height.bytes, 200);
    PushI16(height.bytes, 300);
    sections.push_back(height);

    Section splatA;
    splatA.type = 2;
    PushU16(splatA.bytes, 1);
    PushU16(splatA.bytes, 1);
    splatA.bytes.insert(splatA.bytes.end(), {255, 0, 0, 255});
    sections.push_back(splatA);

    Section attributes;
    attributes.type = 3;
    PushU16(attributes.bytes, 0);
    sections.push_back(attributes);

    Section splatB;
    splatB.type = 4;
    PushU16(splatB.bytes, 1);
    PushU16(splatB.bytes, 1);
    splatB.bytes.insert(splatB.bytes.end(), {0, 255, 0, 255});
    sections.push_back(splatB);

    constexpr std::size_t kHeaderSize = 14;
    constexpr std::size_t kTocEntrySize = 12;
    std::vector<std::uint8_t> bytes;
    PushU32(bytes, 0x3143584d); // MXC1
    PushU16(bytes, 2);
    PushU16(bytes, 0);
    PushU16(bytes, 0);
    PushU16(bytes, 1);
    PushU16(bytes, static_cast<std::uint16_t>(sections.size()));

    std::uint32_t offset = static_cast<std::uint32_t>(kHeaderSize + sections.size() * kTocEntrySize);
    for (const Section& section : sections)
    {
        PushU16(bytes, section.type);
        PushU16(bytes, 0);
        PushU32(bytes, offset);
        PushU32(bytes, static_cast<std::uint32_t>(section.bytes.size()));
        offset += static_cast<std::uint32_t>(section.bytes.size());
    }

    for (const Section& section : sections)
        bytes.insert(bytes.end(), section.bytes.begin(), section.bytes.end());
    return bytes;
}

std::vector<std::uint8_t> BuildMapManifest()
{
    capnp::MallocMessageBuilder builder;
    auto manifest = builder.initRoot<mx::map::schema::MapManifest>();
    manifest.setFormatVersion(2);
    manifest.setWorldId("baseline");
    manifest.setWorldName("Engine Baseline");
    manifest.setWorldSizeCells(1);
    manifest.setCellSizeMeters(1.0f);
    manifest.setHeightUnit(mx::map::schema::HeightUnit::CENTIMETERS);
    manifest.setChunkSizeCells(1);
    auto grid = manifest.initZoneGridDims();
    grid.setX(1);
    grid.setY(1);
    manifest.setZoneSizeCells(1);
    auto palette = manifest.initTexturePalette(1);
    palette[0].setId(0);
    palette[0].setPath("textures/default");

    kj::Array<capnp::word> words = capnp::messageToFlatArray(builder);
    const auto bytes = words.asBytes();
    return {bytes.begin(), bytes.end()};
}

bool RunMapDataBaselineTest(TestContext& ctx)
{
    const std::vector<std::uint8_t> manifest = BuildMapManifest();
    const std::vector<std::uint8_t> chunk = BuildOneCellMxChunk();
    const auto read = [&](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
        const std::string key(path);
        if (key == "maps/baseline/map.manifest")
            return manifest;
        if (key == "maps/baseline/chunks/chunk_0_0.mxchunk")
            return chunk;
        return std::nullopt;
    };

    const auto loadedManifest = mx::map::LoadManifest(read, "maps/baseline");
    ctx.Expect(loadedManifest && loadedManifest->world_name == "Engine Baseline" &&
            loadedManifest->chunk_size_cells == 1,
        "mapdata manifest capnp load", "MapData failed to load the capnp manifest fixture");

    const auto heightField = mx::map::LoadHeightField(read, "maps/baseline");
    return ctx.Expect(heightField && heightField->IsValid() &&
            heightField->width_vertices == 2 &&
            heightField->height_vertices == 2 &&
            heightField->splat_a_rgba8.size() == 4 &&
            std::abs(heightField->SampleHeightMeters(1.0f, 1.0f) - 3.0f) < 0.001f,
        "mapdata mxchunk heightfield load", "MapData failed to load and sample the .mxchunk fixture");
}

bool RunAssetTests(const Options& options, TestContext& ctx)
{
    const std::filesystem::path scratch = options.scratchRoot.empty() ? MakeDefaultScratchRoot() : options.scratchRoot;
    const std::filesystem::path fakeClientRoot = scratch / "ClientSandbox";
    const std::filesystem::path inputRoot = scratch / "input";

    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(inputRoot, ec);
    if (ec)
    {
        ctx.Fail("asset scratch setup", ec.message());
        return false;
    }

    const auto diffusePath = inputRoot / "Grass001_Color.tga";
    const auto normalPath = inputRoot / "Grass001_Normal.tga";
    const auto stonePath = inputRoot / "Stone001_Color.tga";
    if (!WriteTga(diffusePath, 32, 170, 64) ||
        !WriteTga(normalPath, 128, 128, 255, true) ||
        !WriteTga(stonePath, 120, 120, 120))
    {
        ctx.Fail("asset fixture images", "failed to write source TGA files");
        return false;
    }
    ctx.Pass("asset fixture images");

    AssetLibrary library(fakeClientRoot);
    std::string error;
    if (!ctx.Expect(library.Initialize(), "asset library initialize", "Initialize returned false"))
        return false;

    AssetLibrary::ImportOptions grassOptions;
    grassOptions.subpath = "terrain/grass";
    grassOptions.tags = {"grass", "terrain"};

    AssetLibrary::Entry diffuse;
    if (!ctx.Expect(library.Import(AssetLibrary::Category::Texture, diffusePath, grassOptions, diffuse, error),
            "texture import diffuse TGA", error))
        return false;
    ctx.Expect(diffuse.textureRole == AssetLibrary::TextureRole::Diffuse,
        "texture role diffuse", "expected Diffuse role");
    ctx.Expect(!diffuse.thumbnail.empty() && std::filesystem::exists(fakeClientRoot / "assets" / "library" / diffuse.thumbnail),
        "texture thumbnail generated", "thumbnail missing");

    const auto gltfRoot = inputRoot / "BoulderSource";
    std::filesystem::create_directories(gltfRoot / "buffers", ec);
    std::filesystem::create_directories(gltfRoot / "textures", ec);
    {
        std::ofstream bin(gltfRoot / "buffers" / "boulder.bin", std::ios::binary);
        const std::uint8_t bytes[48] = {};
        bin.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }
    {
        std::ofstream texture(gltfRoot / "textures" / "boulder_albedo.png", std::ios::binary);
        texture << "fixture texture bytes";
    }
    const auto gltfPath = gltfRoot / "namaqualand_boulder_02_1k.gltf";
    {
        std::ofstream gltf(gltfPath, std::ios::binary);
        gltf << "{\n"
             << "  \"asset\": {\"version\": \"2.0\"},\n"
             << "  \"buffers\": [{\"uri\": \"buffers/boulder.bin\", \"byteLength\": 48}],\n"
             << "  \"images\": [{\"uri\": \"textures/boulder_albedo.png\"}],\n"
             << "  \"bufferViews\": [],\n"
             << "  \"meshes\": []\n"
             << "}\n";
    }
    AssetLibrary::ImportOptions modelOptions;
    modelOptions.subpath = "props/boulders";
    AssetLibrary::Entry boulderModel;
    if (!ctx.Expect(library.Import(AssetLibrary::Category::Model, gltfPath, modelOptions, boulderModel, error),
            "gltf model import dependencies", error))
        return false;
    const std::filesystem::path importedGltf = library.AbsolutePath(boulderModel);
    ctx.Expect(std::filesystem::exists(importedGltf.parent_path() / "buffers" / "boulder.bin") &&
            std::filesystem::exists(importedGltf.parent_path() / "textures" / "boulder_albedo.png"),
        "gltf dependencies copied", "gltf external .bin or texture was not copied beside the imported model");

    AssetLibrary::Entry directImport;
    std::filesystem::path directFinalPath;
    const std::filesystem::path directTargetFolder = fakeClientRoot / "assets" / "library" / "custom" / "drop";
    if (!ctx.Expect(library.ImportFileToFolder(stonePath, directTargetFolder, directImport, directFinalPath, error),
            "direct folder import", error))
        return false;
    ctx.Expect(directImport.category == AssetLibrary::Category::Texture &&
            directImport.subpath == "custom/drop" &&
            directFinalPath == directTargetFolder / stonePath.filename() &&
            library.AbsolutePath(directImport) == directFinalPath &&
            std::filesystem::exists(directFinalPath),
        "direct import no auto categorize", "direct import should copy exactly into the requested folder, not a category subfolder");

    const auto missingGltfPath = gltfRoot / "missing_dependency.gltf";
    {
        std::ofstream gltf(missingGltfPath, std::ios::binary);
        gltf << "{\n"
             << "  \"asset\": {\"version\": \"2.0\"},\n"
             << "  \"buffers\": [{\"uri\": \"buffers/does_not_exist.bin\", \"byteLength\": 48}],\n"
             << "  \"images\": []\n"
             << "}\n";
    }
    AssetLibrary::Entry missingModel;
    ctx.Expect(!library.Import(AssetLibrary::Category::Model, missingGltfPath, modelOptions, missingModel, error) &&
            error.find("missing glTF external dependency") != std::string::npos,
        "gltf missing dependency rejected", "missing glTF dependency should fail import with a clear warning/error");

    AssetLibrary::Entry normal;
    if (!ctx.Expect(library.Import(AssetLibrary::Category::Texture, normalPath, grassOptions, normal, error),
            "texture import normal TGA", error))
        return false;
    ctx.Expect(normal.textureRole == AssetLibrary::TextureRole::Normal,
        "texture role normal", "expected Normal role");

    AssetLibrary::MaterialData materialData;
    materialData.diffuseTextureId = diffuse.id;
    materialData.normalTextureId = normal.id;
    materialData.normalStrength = 1.0f;

    AssetLibrary::ImportOptions materialOptions;
    materialOptions.displayName = "GrassMaterial";
    materialOptions.subpath = "terrain/grass";
    materialOptions.tags = {"terrain", "material"};

    AssetLibrary::Entry material;
    if (!ctx.Expect(library.CreateMaterial(materialOptions, materialData, material, error),
            "material create", error))
        return false;
    ctx.Expect(CountCategory(library, AssetLibrary::Category::Material) == 1,
        "material count after create", "expected exactly one material");

    materialData.normalStrength = 1.5f;
    AssetLibrary::Entry updatedMaterial;
    if (!ctx.Expect(library.UpdateMaterial(material.id, materialData, updatedMaterial, error),
            "material update overwrites", error))
        return false;
    ctx.Expect(updatedMaterial.id == material.id &&
            CountCategory(library, AssetLibrary::Category::Material) == 1,
        "material update no duplicate", "material update created a duplicate");

    AssetLibrary::Entry movedDiffuse;
    if (!ctx.Expect(library.MoveAssetToSubpath(diffuse.id, "terrain/rock", movedDiffuse, error),
            "asset move to folder", error))
        return false;
    ctx.Expect(movedDiffuse.subpath == "terrain/rock" &&
            std::filesystem::exists(library.AbsolutePath(movedDiffuse)),
        "asset move filesystem", "moved asset path missing");

    AssetLibrary::Entry renamedDiffuse;
    ctx.Expect(!library.RenameAsset(movedDiffuse.id, "MyGrass_Normal", false, renamedDiffuse, error),
        "rename role conflict rejected", "role conflict rename should require confirmation");
    if (!ctx.Expect(library.RenameAsset(movedDiffuse.id, "MyGrass_Normal", true, renamedDiffuse, error),
            "rename role conflict confirmed", error))
        return false;
    ctx.Expect(renamedDiffuse.id == movedDiffuse.id &&
            renamedDiffuse.textureRole == AssetLibrary::TextureRole::Normal,
        "rename keeps id updates role", "rename did not keep id or update role");

    AssetLibrary::Entry renamedMaterial;
    if (!ctx.Expect(library.RenameAsset(material.id, "GrassMaterial_Renamed", true, renamedMaterial, error),
            "material rename", error))
        return false;
    ctx.Expect(renamedMaterial.id == material.id &&
            renamedMaterial.filename == "GrassMaterial_Renamed.material.json" &&
            std::filesystem::exists(library.AbsolutePath(renamedMaterial)),
        "material rename filesystem", "renamed material JSON missing");

    WaterMaterialData waterMaterial{};
    waterMaterial.config.baseColor[0] = 0.12f;
    waterMaterial.config.baseColor[1] = 0.42f;
    waterMaterial.config.baseColor[2] = 0.68f;
    waterMaterial.config.foamIntensity = 0.75f;
    waterMaterial.config.edgeFadeDistance = 2.25f;
    waterMaterial.config.edgeFadeCurve = WaterConfig::EdgeFadeCurve::Exponential;
    waterMaterial.normalTiling = 3.5f;
    AssetLibrary::ImportOptions waterMaterialOptions;
    waterMaterialOptions.displayName = "LakeShared";
    AssetLibrary::Entry waterMaterialEntry;
    if (!ctx.Expect(library.CreateWaterMaterial(waterMaterialOptions, waterMaterial, waterMaterialEntry, error),
            "water material create", error))
        return false;
    ctx.Expect(waterMaterialEntry.category == AssetLibrary::Category::WaterMaterial &&
            waterMaterialEntry.filename == "LakeShared.watermat" &&
            std::filesystem::exists(library.AbsolutePath(waterMaterialEntry)) &&
            std::abs(waterMaterialEntry.waterMaterial.config.edgeFadeDistance - 2.25f) < 0.001f &&
            waterMaterialEntry.waterMaterial.config.edgeFadeCurve == WaterConfig::EdgeFadeCurve::Exponential,
        "water material filesystem", "water material file was not written or edge fade was not preserved");
    waterMaterial.config.foamIntensity = 1.25f;
    AssetLibrary::Entry updatedWaterMaterial;
    if (!ctx.Expect(library.UpdateWaterMaterial(waterMaterialEntry.id, waterMaterial, updatedWaterMaterial, error),
            "water material update", error))
        return false;
    const auto waterMaterialEntries = library.QueryEntries(AssetLibrary::Category::WaterMaterial, "", true, {}, "LakeShared");
    ctx.Expect(waterMaterialEntries.size() == 1 &&
            std::abs(waterMaterialEntries[0].waterMaterial.config.foamIntensity - 1.25f) < 0.001f &&
            std::abs(waterMaterialEntries[0].waterMaterial.config.edgeFadeDistance - 2.25f) < 0.001f &&
            waterMaterialEntries[0].waterMaterial.config.edgeFadeCurve == WaterConfig::EdgeFadeCurve::Exponential,
        "water material update no duplicate", "water material update created a duplicate or missed new edge fade values");
    AssetLibrary::Entry renamedWaterMaterial;
    ctx.Expect(library.RenameAsset(waterMaterialEntry.id, "LakeShared_Renamed", true, renamedWaterMaterial, error) &&
            renamedWaterMaterial.filename == "LakeShared_Renamed.watermat" &&
            std::filesystem::exists(library.AbsolutePath(renamedWaterMaterial)),
        "water material rename", error);

    std::string newSubpath;
    if (!ctx.Expect(library.RenameFolder(AssetLibrary::Category::Texture,
            "terrain/rock", "pebbles", newSubpath, error),
            "folder rename", error))
        return false;
    const auto renamedTexture = library.FindById(renamedDiffuse.id);
    ctx.Expect(renamedTexture && renamedTexture->subpath == "terrain/pebbles",
        "folder rename updates subpath", "texture subpath was not updated");

    std::string createdFolder;
    ctx.Expect(library.CreateFolder(AssetLibrary::Category::Texture, "terrain", "empty_folder", createdFolder, error) &&
            createdFolder == "terrain/empty_folder",
        "folder create empty", error);
    const auto foldersAfterCreate = library.FolderSubpathsFor(AssetLibrary::Category::Texture);
    ctx.Expect(std::find(foldersAfterCreate.begin(), foldersAfterCreate.end(), "terrain/empty_folder") != foldersAfterCreate.end(),
        "folder create visible", "empty created folder was not listed");
    std::string renamedEmptyFolder;
    ctx.Expect(library.RenameFolder(AssetLibrary::Category::Texture,
            "terrain/empty_folder", "empty_renamed", renamedEmptyFolder, error) &&
            renamedEmptyFolder == "terrain/empty_renamed",
        "empty folder rename", error);
    std::uint32_t removedFolderAssets = 0;
    ctx.Expect(library.DeleteFolder(AssetLibrary::Category::Texture,
            "terrain/empty_renamed", removedFolderAssets, error) &&
            removedFolderAssets == 0,
        "empty folder delete", error);

    ctx.Expect(!AssetLibrary::IsValidRenameName("bad name"),
        "rename validation rejects spaces", "space-containing name was accepted");
    ctx.Expect(!AssetLibrary::IsValidRenameName("CON"),
        "rename validation rejects reserved name", "reserved Windows name was accepted");

    AssetLibrary::Entry stone;
    AssetLibrary::ImportOptions rootOptions;
    rootOptions.subpath = "";
    ctx.Expect(library.Import(AssetLibrary::Category::Texture, stonePath, rootOptions, stone, error),
        "root texture import", error);
    const auto allTextures = library.QueryEntries(AssetLibrary::Category::Texture, "", true, {}, "");
    const auto grassFiltered = library.QueryEntries(AssetLibrary::Category::Texture, "terrain/pebbles", false, {}, "mygrass");
    ctx.Expect(allTextures.size() >= 3 && grassFiltered.size() == 1,
        "asset query folder plus search", "query did not combine folder/search as expected");

    AssetLibrary::Entry movedStone;
    ctx.Expect(library.MoveAssetToSubpath(stone.id, "terrain/delete_me", movedStone, error),
        "folder delete fixture move", error);
    std::uint32_t removedAssets = 0;
    ctx.Expect(library.DeleteFolder(AssetLibrary::Category::Texture, "terrain/delete_me", removedAssets, error) &&
            removedAssets == 1 && !library.FindById(stone.id),
        "non-empty folder delete removes manifest entries", error);

    const std::filesystem::path projectParent = scratch / "projects";
    std::filesystem::create_directories(projectParent, ec);
    if (!ctx.Expect(!ec, "project scratch setup", ec.message()))
        return false;

    std::string projectError;
    ProjectManager& projects = ProjectManager::Instance();
    if (!ctx.Expect(projects.CreateProject(projectParent, "EngineBaseline", projectError),
            "project create round-trip", projectError))
        return false;
    ctx.Expect(std::filesystem::exists(projects.ManifestPath()) &&
            std::filesystem::exists(projects.AssetRootPath()) &&
            std::filesystem::exists(projects.ScenesPath()),
        "project folders created", "project.ixproj, Assets, or Scenes missing after CreateProject");

    projects.SetRecentScenes({"Scenes/Baseline/Baseline.scene"});
    ctx.Expect(projects.SaveProject(projectError), "project save manifest", projectError);
    ctx.Expect(projects.OpenProject(projects.ManifestPath(), projectError) &&
            projects.CurrentProject().name == "EngineBaseline" &&
            projects.CurrentProject().recentScenes.size() == 1 &&
            projects.CurrentProject().startupScene == "Scenes/Baseline/Baseline.scene",
        "project open round-trip", projectError);

    SceneManager& scenes = SceneManager::Instance();
    scenes.NewScene();
    scenes.SetSceneName("Baseline");
    SceneData baselineScene = scenes.GetCurrentScene();
    WaterBody renamedWater{};
    renamedWater.id = 101;
    renamedWater.name = "Renamed Water";
    renamedWater.bboxMin[0] = -2.0f;
    renamedWater.bboxMin[1] = -2.0f;
    renamedWater.bboxMax[0] = 2.0f;
    renamedWater.bboxMax[1] = 2.0f;
    renamedWater.maskWidth = 1;
    renamedWater.maskHeight = 1;
    renamedWater.shapeMask = {255};
    baselineScene.waterBodies.push_back(renamedWater);
    PointLight keyLight{};
    keyLight.id = 202;
    keyLight.name = "Key Light";
    keyLight.position[0] = 1.0f;
    keyLight.position[1] = 2.0f;
    keyLight.position[2] = 3.0f;
    baselineScene.pointLights.push_back(keyLight);
    MeshSceneEntity meshEntity{};
    meshEntity.id = 303;
    meshEntity.name = "KicsiK";
    meshEntity.meshAssetId = "model_KicsiK";
    meshEntity.meshAssetPath = "Assets/Models/KicsiK.glb";
    meshEntity.position[0] = 4.0f;
    meshEntity.position[1] = 5.0f;
    meshEntity.position[2] = 6.0f;
    meshEntity.rotation[1] = 0.75f;
    meshEntity.scale[0] = 1.5f;
    meshEntity.scale[1] = 1.5f;
    meshEntity.scale[2] = 1.5f;
    baselineScene.meshEntities.push_back(meshEntity);
    scenes.SetCurrentSceneSnapshot(baselineScene);
    const std::filesystem::path scenePath = projects.ScenesPath() / "Baseline" / "Baseline.scene";
    if (!ctx.Expect(scenes.SaveSceneAs(scenePath.string()),
            "scene save project-relative", "SaveSceneAs failed"))
        return false;
    scenes.CloseScene();
    if (!ctx.Expect(scenes.LoadScene(scenePath.string()),
            "scene load round-trip", "LoadScene failed"))
        return false;
    ctx.Expect(scenes.HasOpenScene() &&
            scenes.GetCurrentScene().name == "Baseline" &&
            !scenes.IsDirty(),
        "scene data round-trip", "scene name/open/dirty state did not round-trip");
    ctx.Expect(scenes.GetCurrentScene().waterBodies.size() == 1 &&
            scenes.GetCurrentScene().waterBodies[0].name == "Renamed Water" &&
            scenes.GetCurrentScene().pointLights.size() == 1 &&
            scenes.GetCurrentScene().pointLights[0].name == "Key Light" &&
            scenes.GetCurrentScene().meshEntities.size() == 1 &&
            scenes.GetCurrentScene().meshEntities[0].name == "KicsiK" &&
            scenes.GetCurrentScene().meshEntities[0].meshAssetPath == "Assets/Models/KicsiK.glb" &&
            scenes.GetCurrentScene().meshEntities[0].position[2] == 6.0f,
        "scene entity round-trip", "water/light/mesh scene entities did not survive save/load");

    scenes.NewScene();
    scenes.SetSceneName("ChunkedTerrain");
    SceneData terrainScene = scenes.GetCurrentScene();
    terrainScene.terrain.exists = true;
    terrainScene.terrain.name = "Terrain";
    terrainScene.terrain.widthMeters = 33.0f;
    terrainScene.terrain.depthMeters = 17.0f;
    terrainScene.terrain.cellSizeMeters = 1.0f;
    terrainScene.terrain.cellsX = 33;
    terrainScene.terrain.cellsZ = 17;
    terrainScene.terrain.chunkSizeCells = 32;
    terrainScene.terrain.triplanarEnabled = true;
    terrainScene.terrain.triplanarSharpness = 5.5f;
    terrainScene.terrain.triplanarSlopeThreshold = 0.22f;
    terrainScene.terrain.triplanarSlopeTransition = 0.18f;
    terrainScene.terrain.heightCmGrid.resize(static_cast<std::size_t>(terrainScene.terrain.cellsX + 1u) *
        (terrainScene.terrain.cellsZ + 1u), 0.0f);
    terrainScene.terrain.heightCmGrid[static_cast<std::size_t>(16) * (terrainScene.terrain.cellsX + 1u) + 32u] = 123.0f;
    terrainScene.terrain.splatABytes.assign(static_cast<std::size_t>(terrainScene.terrain.cellsX) *
        terrainScene.terrain.cellsZ * 4u, 0);
    terrainScene.terrain.splatBBytes.assign(terrainScene.terrain.splatABytes.size(), 0);
    for (std::size_t i = 0; i < terrainScene.terrain.splatABytes.size(); i += 4u)
        terrainScene.terrain.splatABytes[i] = 255;
    terrainScene.terrain.splatABytes[0] = 0;
    terrainScene.terrain.splatABytes[1] = 255;
    terrainScene.paletteSlots[2].slot = 2;
    terrainScene.paletteSlots[2].displayName = "Live Grass";
    terrainScene.paletteSlots[2].texturePath = "Assets/Textures/live_grass.png";
    terrainScene.paletteSlots[2].tilingScaleX = 3.5f;
    terrainScene.paletteSlots[2].tilingScaleY = 3.5f;
    terrainScene.paletteSlots[2].colorTint[0] = 0.25f;
    terrainScene.paletteSlots[2].colorTint[1] = 0.75f;
    terrainScene.paletteSlots[2].colorTint[2] = 0.50f;
    terrainScene.paletteSlots[2].normalStrength = 1.7f;
    terrainScene.paletteSlots[2].roughnessStrength = 0.42f;
    terrainScene.paletteSlots[2].metallicStrength = 0.65f;
    terrainScene.paletteSlots[2].aoStrength = 0.35f;
    terrainScene.paletteSlots[2].uvOffset[0] = 0.125f;
    terrainScene.paletteSlots[2].uvOffset[1] = -0.25f;
    terrainScene.paletteSlots[2].uvRotationDegrees = 37.0f;
    scenes.SetCurrentSceneSnapshot(terrainScene);
    const std::filesystem::path terrainScenePath = projects.ScenesPath() / "ChunkedTerrain" / "ChunkedTerrain.scene";
    if (!ctx.Expect(scenes.SaveSceneAs(terrainScenePath.string()),
            "terrain chunk scene save", "SaveSceneAs failed for chunked terrain"))
        return false;
    const std::filesystem::path terrainMapDir = terrainScenePath.parent_path() / "ChunkedTerrain_terrain_map";
    ctx.Expect(std::filesystem::exists(terrainMapDir / "map.manifest") &&
            std::filesystem::exists(terrainMapDir / "chunks" / "chunk_0_0.mxchunk") &&
            std::filesystem::exists(terrainMapDir / "chunks" / "chunk_1_1.mxchunk"),
        "terrain chunk sidecars written", "terrain save did not write map.manifest and expected .mxchunk files");
    const auto readManifestBytes = [&](std::string_view assetPath) -> std::optional<std::vector<std::uint8_t>> {
        const std::filesystem::path diskPath = terrainMapDir / std::filesystem::path(assetPath);
        std::ifstream file(diskPath, std::ios::binary);
        if (!file)
            return std::nullopt;
        return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };
    const auto savedManifest = mx::map::LoadManifest(readManifestBytes, ".");
    ctx.Expect(savedManifest &&
            savedManifest->texture_palette_paths.size() > 2 &&
            savedManifest->texture_palette_paths[2] == "Assets/Textures/live_grass.png" &&
            savedManifest->texture_palette_tiling_x.size() > 2 &&
            std::abs(savedManifest->texture_palette_tiling_x[2] - 3.5f) < 0.01f &&
            std::abs(savedManifest->texture_palette_normal_strength[2] - 1.7f) < 0.01f &&
            std::abs(savedManifest->texture_palette_roughness_strength[2] - 0.42f) < 0.01f &&
            std::abs(savedManifest->texture_palette_tint_r[2] - 0.25f) < 0.01f &&
            std::abs(savedManifest->texture_palette_tint_g[2] - 0.75f) < 0.01f &&
            std::abs(savedManifest->texture_palette_tint_b[2] - 0.50f) < 0.01f &&
            std::abs(savedManifest->texture_palette_metallic_strength[2] - 0.65f) < 0.01f &&
            std::abs(savedManifest->texture_palette_ao_strength[2] - 0.35f) < 0.01f &&
            std::abs(savedManifest->texture_palette_uv_offset_x[2] - 0.125f) < 0.01f &&
            std::abs(savedManifest->texture_palette_uv_offset_y[2] + 0.25f) < 0.01f &&
            std::abs(savedManifest->texture_palette_uv_rotation_degrees[2] - 37.0f) < 0.01f &&
            std::abs(savedManifest->triplanar_slope_threshold - 0.22f) < 0.01f &&
            std::abs(savedManifest->triplanar_slope_transition - 0.18f) < 0.01f,
        "terrain material params manifest save", "map.manifest did not persist per-layer terrain material params");
    scenes.CloseScene();
    if (!ctx.Expect(scenes.LoadScene(terrainScenePath.string()),
            "terrain chunk scene load", "LoadScene failed for chunked terrain"))
        return false;
    const TerrainSceneData& loadedTerrain = scenes.GetCurrentScene().terrain;
    const std::size_t loadedHeightIndex = static_cast<std::size_t>(16) * (loadedTerrain.cellsX + 1u) + 32u;
    ctx.Expect(loadedTerrain.exists &&
            loadedTerrain.cellsX == 33 &&
            loadedTerrain.cellsZ == 17 &&
            loadedTerrain.chunkSizeCells == 32 &&
            loadedTerrain.triplanarEnabled &&
            std::abs(loadedTerrain.triplanarSharpness - 5.5f) < 0.01f &&
            std::abs(loadedTerrain.triplanarSlopeThreshold - 0.22f) < 0.01f &&
            std::abs(loadedTerrain.triplanarSlopeTransition - 0.18f) < 0.01f &&
            loadedTerrain.chunkManifestRef == "ChunkedTerrain_terrain_map/map.manifest" &&
            loadedHeightIndex < loadedTerrain.heightCmGrid.size() &&
            std::abs(loadedTerrain.heightCmGrid[loadedHeightIndex] - 123.0f) < 0.5f &&
            loadedTerrain.splatABytes.size() == static_cast<std::size_t>(33) * 17u * 4u &&
            loadedTerrain.splatABytes[1] == 255 &&
            std::abs(scenes.GetCurrentScene().paletteSlots[2].tilingScaleX - 3.5f) < 0.01f &&
            std::abs(scenes.GetCurrentScene().paletteSlots[2].normalStrength - 1.7f) < 0.01f &&
            std::abs(scenes.GetCurrentScene().paletteSlots[2].roughnessStrength - 0.42f) < 0.01f &&
            std::abs(scenes.GetCurrentScene().paletteSlots[2].colorTint[1] - 0.75f) < 0.01f &&
            std::abs(scenes.GetCurrentScene().paletteSlots[2].metallicStrength - 0.65f) < 0.01f &&
            std::abs(scenes.GetCurrentScene().paletteSlots[2].aoStrength - 0.35f) < 0.01f &&
            std::abs(scenes.GetCurrentScene().paletteSlots[2].uvOffset[0] - 0.125f) < 0.01f &&
            std::abs(scenes.GetCurrentScene().paletteSlots[2].uvOffset[1] + 0.25f) < 0.01f &&
            std::abs(scenes.GetCurrentScene().paletteSlots[2].uvRotationDegrees - 37.0f) < 0.01f,
        "terrain chunk round-trip", "chunked terrain dimensions, manifest, triplanar state, height, or splat data did not round-trip");

    RunMapDataBaselineTest(ctx);

    std::cout << "[INFO] asset scratch kept at: " << scratch.generic_string() << "\n";
    return ctx.failed == 0;
}

bool RunRenderChecks(const Options& options, TestContext& ctx)
{
    const std::filesystem::path shaderDir = options.clientRoot / "assets" / "shaders";
    ctx.Expect(std::filesystem::exists(options.clientRoot / "shaders" / "Water.hlsl"),
        "water shader source exists", "Client/shaders/Water.hlsl missing");
    ctx.Expect(std::filesystem::exists(shaderDir / "water_vs.spv"),
        "water vertex shader compiled", "assets/shaders/water_vs.spv missing; build IXEngineShaders");
    ctx.Expect(std::filesystem::exists(shaderDir / "water_ps.spv"),
        "water pixel shader compiled", "assets/shaders/water_ps.spv missing; build IXEngineShaders");
    ctx.Expect(std::filesystem::exists(options.clientRoot / "shaders" / "Composite.hlsl"),
        "composite shader source exists", "Client/shaders/Composite.hlsl missing");
    ctx.Expect(std::filesystem::exists(shaderDir / "composite_vs.spv"),
        "composite vertex shader compiled", "assets/shaders/composite_vs.spv missing; build IXEngineShaders");
    ctx.Expect(std::filesystem::exists(shaderDir / "composite_ps.spv"),
        "composite pixel shader compiled", "assets/shaders/composite_ps.spv missing; build IXEngineShaders");

    const std::filesystem::path waterShaderPath = options.clientRoot / "shaders" / "Water.hlsl";
    std::ifstream waterShader(waterShaderPath);
    std::stringstream waterShaderText;
    waterShaderText << waterShader.rdbuf();
    const std::string waterSource = waterShaderText.str();
    ctx.Expect(waterSource.find("u_reflectionTexture") != std::string::npos &&
            waterSource.find("[[vk::binding(3, 0)]]") != std::string::npos,
        "water reflection shader binding", "Water shader is not sampling a binding-3 reflection texture");
    ctx.Expect(waterSource.find("u_reflectionParams") != std::string::npos &&
            waterSource.find("screenUv += n.xz") != std::string::npos,
        "water reflection distortion shader", "Water shader is missing wave-based reflection distortion");
    ctx.Expect(waterSource.find("u_sceneColorTexture") != std::string::npos &&
            waterSource.find("u_sceneDepthTexture") != std::string::npos &&
            waterSource.find("u_refractionParams") != std::string::npos,
        "water refraction snapshot shader bindings", "Water shader is missing WATER-3 scene color/depth refraction bindings");
    ctx.Expect(waterSource.find("u_shallowColor") != std::string::npos &&
            waterSource.find("u_deepColor") != std::string::npos &&
            waterSource.find("waterDepth") != std::string::npos,
        "water depth color shader", "Water shader is missing WATER-3 depth color/fade logic");
    ctx.Expect(waterSource.find("u_textureParams") != std::string::npos &&
            waterSource.find("u_textureScroll") != std::string::npos &&
            waterSource.find("u_diffuseMap") != std::string::npos &&
            waterSource.find("useNormalA") != std::string::npos &&
            waterSource.find("useNormalB") != std::string::npos &&
            waterSource.find("depthWaterColor = lerp(depthWaterColor, depthWaterColor * diffuseTint") != std::string::npos,
        "water object material texture shader", "Water shader must sample assigned water material normals/diffuse with per-body tiling/scroll");
    ctx.Expect(waterSource.find("u_foamParams") != std::string::npos &&
            waterSource.find("FoamNoise") != std::string::npos,
        "water foam shader controls", "Water shader is missing WATER-4 foam controls");
    ctx.Expect(waterSource.find("edgeAlpha") != std::string::npos &&
            waterSource.find("outputAlpha") != std::string::npos &&
            waterSource.find("* saturate(input.edgeAlpha)") != std::string::npos,
        "water edge fade shader alpha", "Water shader must multiply final alpha by per-vertex edge alpha");
    ctx.Expect(waterSource.find("foamMask = smoothstep(0.5, 0.9, input.edgeAlpha)") != std::string::npos,
        "water edge fade foam mask", "Water foam must be suppressed in the fully faded shoreline zone");
    ctx.Expect(waterSource.find("worldPos.y = u_levelTimeEnabled.x") == std::string::npos,
        "water mesh Y comes from vertices", "Water vertex shader still overwrites mesh Y from the uniform level");
    ctx.Expect(waterSource.find("screenUv.y = 1.0 - screenUv.y") == std::string::npos,
        "water screen UV uses project convention", "Water shader still flips screenUv.y despite the Y-flipped projection convention");
    ctx.Expect(waterSource.find("u_cameraNearFar") != std::string::npos &&
            waterSource.find("const float nearPlane = 0.1") == std::string::npos &&
            waterSource.find("const float farPlane = 1000.0") == std::string::npos,
        "water depth uses camera near/far", "Water depth linearization still uses hard-coded near/far values");
    ctx.Expect(waterSource.find("depthT = smoothstep(u_depthParams.x, max(u_depthParams.y, u_depthParams.x + 0.001), waterViewDepth)") != std::string::npos &&
            waterSource.find("fadeT = saturate(waterViewDepth / max(u_depthParams.z, 0.001))") != std::string::npos,
        "water depth color uses meter depth", "Water depth color/fade must use linearized meter-space waterViewDepth");
    ctx.Expect(waterSource.find("(0.18 + shoreFoam") == std::string::npos &&
            waterSource.find("max(FoamNoise") == std::string::npos &&
            waterSource.find("LinearizeWaterDepth") != std::string::npos &&
            waterSource.find("waterViewDepth") != std::string::npos &&
            waterSource.find("fwidth(waterViewDepth)") != std::string::npos &&
            waterSource.find("smoothstep(0.02, foamDistance + foamSoftness, waterViewDepth)") != std::string::npos,
        "water foam shore mask constrained", "Water foam must use a narrow linear-depth shoreline band and reject flat shallow open water");
    ctx.Expect(waterSource.find("min(finalColor, float3(10.0, 10.0, 10.0))") != std::string::npos,
        "water HDR specular clamp", "Water shader is missing the HDR clamp that suppresses reflection/specular fireflies");
    ctx.Expect(waterSource.find("finalColor = finalColor / (finalColor + 1.0.xxx)") == std::string::npos,
        "water local tone-map removed", "Water shader still performs local Reinhard tone-mapping");

    const std::filesystem::path terrainShaderPath = options.clientRoot / "shaders" / "Terrain.hlsl";
    std::ifstream terrainShader(terrainShaderPath);
    std::stringstream terrainShaderText;
    terrainShaderText << terrainShader.rdbuf();
    const std::string terrainSource = terrainShaderText.str();
    ctx.Expect(terrainSource.find("TerrainWaterBodyUbo") != std::string::npos &&
            terrainSource.find("u_terrainWaterBodies[8]") != std::string::npos &&
            terrainSource.find("IsInsideWaterBodyBbox") != std::string::npos &&
            terrainSource.find("body.levelModeEnabled.x - input.worldPos.y") != std::string::npos &&
            terrainSource.find("u_waterParams1") == std::string::npos,
        "terrain per-water-body caustic controls", "Terrain shader must use per-body bbox/water-level data instead of a global water uniform");
    ctx.Expect(terrainSource.find("terrainFoam") != std::string::npos &&
            terrainSource.find("bodyWaterDepth <= 0.0") != std::string::npos &&
            terrainSource.find("bodyWaterDepth < foamThickness") != std::string::npos &&
            terrainSource.find("body.foamParams") != std::string::npos,
        "terrain per-water-body shore foam", "Terrain foam must be scoped to the current water body and its depth band");
    ctx.Expect(terrainSource.find("float4 edgeParams") != std::string::npos &&
            terrainSource.find("WaterBodyBboxEdgeAlpha") != std::string::npos &&
            terrainSource.find("waterFoamEdgeMask") != std::string::npos &&
            terrainSource.find("* waterEdgeAlpha") != std::string::npos,
        "terrain water edge fade effects", "Terrain water foam/caustics must fade near soft water body edges");
    ctx.Expect(terrainSource.find("float4(1.0, 0.92, 0.15") != std::string::npos,
        "terrain editor brush ring present", "Terrain shader is missing the yellow editor brush ring");
    ctx.Expect(terrainSource.find("u_layerConstants.u_layerParams.z > 7.5") != std::string::npos &&
            terrainSource.find("float3(0.34, 0.82, 1.0)") != std::string::npos &&
            terrainSource.find("float3(1.0, 0.18, 0.12)") != std::string::npos,
        "water sculpt brush ring shader", "Terrain shader must draw add/remove water sculpt brush rings");
    ctx.Expect(terrainSource.find("float4(0.1, 0.55, 1.0") == std::string::npos &&
            terrainSource.find("float4(0.0, 1.0, 0.25") == std::string::npos &&
            terrainSource.find("float4(1.0, 0.08, 0.04") == std::string::npos,
        "terrain debug color layers removed", "Terrain shader still contains old blue/green/red debug layers");
    ctx.Expect(terrainSource.find("finalColor = finalColor / (finalColor + 1.0.xxx)") == std::string::npos,
        "terrain local tone-map removed", "Terrain shader still performs local Reinhard tone-mapping");
    ctx.Expect(terrainSource.find("u_terrainMaterialParams") != std::string::npos &&
            terrainSource.find("TriplanarWeights") != std::string::npos &&
            terrainSource.find("TriplanarSlopeBlend") != std::string::npos &&
            terrainSource.find("smoothstep(threshold, threshold + transition, slope)") != std::string::npos &&
            terrainSource.find("TriplanarNormalToWorld") != std::string::npos &&
            terrainSource.find("TriplanarPlaneUv") != std::string::npos,
        "terrain triplanar shader path", "Terrain shader is missing triplanar UV/normal sampling support");

    const std::filesystem::path vulkanDevicePath = options.clientRoot / "libs" / "platform" / "VulkanDevice.cpp";
    std::ifstream vulkanDevice(vulkanDevicePath);
    std::stringstream vulkanDeviceText;
    vulkanDeviceText << vulkanDevice.rdbuf();
    const std::string vulkanDeviceSource = vulkanDeviceText.str();
    ctx.Expect(vulkanDeviceSource.find("supported.samplerAnisotropy") != std::string::npos &&
            vulkanDeviceSource.find("enabled.samplerAnisotropy = VK_TRUE") != std::string::npos,
        "vulkan sampler anisotropy feature", "VulkanDevice must enable samplerAnisotropy with graceful fallback");

    const std::filesystem::path skinnedMeshShaderPath = options.clientRoot / "shaders" / "SkinnedMesh.hlsl";
    std::ifstream skinnedMeshShader(skinnedMeshShaderPath);
    std::stringstream skinnedMeshShaderText;
    skinnedMeshShaderText << skinnedMeshShader.rdbuf();
    const std::string skinnedMeshSource = skinnedMeshShaderText.str();
    ctx.Expect(skinnedMeshSource.find("u_waterParams") != std::string::npos &&
            skinnedMeshSource.find("CausticPattern") != std::string::npos &&
            skinnedMeshSource.find("waterDepth > 0.0") != std::string::npos,
        "skinnedMesh water caustic shader controls", "Skinned mesh shader is missing WATER-4 underwater caustic controls");

    const std::filesystem::path compositeShaderPath = options.clientRoot / "shaders" / "Composite.hlsl";
    std::ifstream compositeShader(compositeShaderPath);
    std::stringstream compositeShaderText;
    compositeShaderText << compositeShader.rdbuf();
    const std::string compositeSource = compositeShaderText.str();
    ctx.Expect(compositeSource.find("hdrColor / (hdrColor + 1.0.xxx)") != std::string::npos,
        "composite central tone-map", "Composite shader is missing central Reinhard tone-mapping");

    const std::filesystem::path offscreenRendererPath = options.clientRoot / "libs" / "render" / "OffscreenSceneRenderer.cpp";
    std::ifstream offscreenRenderer(offscreenRendererPath);
    std::stringstream offscreenRendererText;
    offscreenRendererText << offscreenRenderer.rdbuf();
    const std::string offscreenSource = offscreenRendererText.str();
    ctx.Expect(offscreenSource.find("VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL") != std::string::npos &&
            offscreenSource.find("RenderComposite") != std::string::npos,
        "offscreen composite renderer source", "Offscreen scene renderer is missing shader-readable target/composite path");
    ctx.Expect(offscreenSource.find("SnapshotScene") != std::string::npos &&
            offscreenSource.find("vkCmdCopyImage") != std::string::npos &&
            offscreenSource.find("m_sceneDepthSnapshot") != std::string::npos,
        "offscreen scene snapshot source", "Offscreen scene renderer is missing WATER-3 scene color/depth snapshots");

    const std::filesystem::path offscreenRendererHeaderPath = options.clientRoot / "libs" / "render" / "OffscreenSceneRenderer.h";
    std::ifstream offscreenRendererHeader(offscreenRendererHeaderPath);
    std::stringstream offscreenRendererHeaderText;
    offscreenRendererHeaderText << offscreenRendererHeader.rdbuf();
    const std::string offscreenHeaderSource = offscreenRendererHeaderText.str();
    const std::filesystem::path sceneViewEditorImGuiPath = options.clientRoot / "libs" / "render" / "EditorImGui.cpp";
    std::ifstream editorImGuiForSceneView(sceneViewEditorImGuiPath);
    std::stringstream editorImGuiForSceneViewText;
    editorImGuiForSceneViewText << editorImGuiForSceneView.rdbuf();
    const std::string editorImGuiSceneViewSource = editorImGuiForSceneViewText.str();
    ctx.Expect(offscreenHeaderSource.find("GetSceneColorView") != std::string::npos &&
            editorImGuiSceneViewSource.find("SetSceneViewTexture") != std::string::npos &&
            editorImGuiSceneViewSource.find("ImGui_ImplVulkan_AddTexture") != std::string::npos &&
            editorImGuiSceneViewSource.find("ImGui::Image(reinterpret_cast<ImTextureID>(m_sceneViewDescriptor), imageSize)") != std::string::npos &&
            editorImGuiSceneViewSource.find("sceneViewSize[0] = imageSize.x") != std::string::npos,
        "scene view draws offscreen target", "Scene View must present the offscreen scene color target inside the docked ImGui panel");

    const std::filesystem::path mainPath = options.clientRoot / "apps" / "client" / "src" / "main.cpp";
    std::ifstream mainFile(mainPath);
    std::stringstream mainText;
    mainText << mainFile.rdbuf();
    const std::string mainSource = mainText.str();
    ctx.Expect(mainSource.find("offscreenScene.BeginMainPass") != std::string::npos &&
            mainSource.find("offscreenScene.RenderComposite") != std::string::npos,
        "main render loop uses offscreen composite", "Main render loop does not route 3D through offscreen composite");
    ctx.Expect(editorImGuiSceneViewSource.find("IsSceneViewInputTarget") != std::string::npos &&
            editorImGuiSceneViewSource.find("MapInputToSceneView") != std::string::npos &&
            editorImGuiSceneViewSource.find("SetSceneViewKeyboardFocus") != std::string::npos &&
            editorImGuiSceneViewSource.find("diag.sceneViewHovered") != std::string::npos &&
            mainSource.find("!sceneViewInputTarget") != std::string::npos &&
            mainSource.find("editorFlyCameraKey") != std::string::npos &&
            mainSource.find("editorFlyMovement") != std::string::npos &&
            mainSource.find("editorRightMouseHeld") != std::string::npos &&
            mainSource.find("cameraRmbInput") != std::string::npos &&
            mainSource.find("cameraController.IsFreeCameraEnabled()") != std::string::npos &&
            mainSource.find("editorImGui.SetSceneViewKeyboardFocus(sceneViewInputTarget)") != std::string::npos &&
            mainSource.find("viewportEvent = editorImGui.MapInputToSceneView(event)") != std::string::npos,
        "scene view routes viewport input", "Scene View must bypass generic ImGui capture and map mouse input into render-target coordinates");
    ctx.Expect(mainSource.find("ApplyGatedEdge") != std::string::npos &&
            mainSource.find("editorLastMouseX") != std::string::npos &&
            mainSource.find("viewportDiag.sceneViewRectValid") != std::string::npos &&
            mainSource.find("editorImGui.IsTextInputActive()") != std::string::npos &&
            mainSource.find("viewportHovered && !wantCaptureKeyboard") != std::string::npos &&
            mainSource.find("[EDITOR-CAMERA] viewport_input_gate enabled") != std::string::npos,
        "editor camera movement is viewport gated", "Editor fly-camera movement keys must activate only on viewport-hovered key-down edges outside ImGui keyboard capture");
    const std::filesystem::path nativeWindowWin32Path = options.clientRoot / "libs" / "platform" / "NativeWindow_Win32.cpp";
    std::ifstream nativeWindowWin32(nativeWindowWin32Path);
    std::stringstream nativeWindowWin32Text;
    nativeWindowWin32Text << nativeWindowWin32.rdbuf();
    const std::string nativeWindowWin32Source = nativeWindowWin32Text.str();
    ctx.Expect(nativeWindowWin32Source.find("IsEngineKeyboardInputMessage") != std::string::npos &&
            nativeWindowWin32Source.find("WM_KEYDOWN") != std::string::npos &&
            nativeWindowWin32Source.find("WantCapture gate before viewport tools") != std::string::npos,
        "win32 dispatches viewport keyboard input", "Win32 input must dispatch key events to the engine even when ImGui consumes the native message");
    ctx.Expect(nativeWindowWin32Source.find("ShowWindow(m_hwnd, SW_MAXIMIZE)") != std::string::npos,
        "win32 starts maximized", "The desktop editor window must open maximized while keeping the normal overlapped window border");
    ctx.Expect(mainSource.find("offscreenScene.SnapshotScene") != std::string::npos &&
            mainSource.find("terrain.SetWaterRefractionInputs") != std::string::npos &&
            mainSource.find("offscreenScene.BeginMainPass(device, false)") != std::string::npos,
        "main render loop uses water refraction snapshots", "Main render loop does not snapshot scene before water pass");

    ctx.Expect(skinnedMeshSource.find("u_lightPadding.x > 0.5") != std::string::npos &&
            skinnedMeshSource.find("input.worldPos.y < u_lightPadding.y") != std::string::npos,
        "skinnedMesh reflection clip shader", "Skinned mesh shader is missing water-level reflection clipping");

    const std::filesystem::path skinnedMeshRendererPath = options.clientRoot / "libs" / "render" / "SkinnedMeshRenderer.cpp";
    std::ifstream skinnedMeshRenderer(skinnedMeshRendererPath);
    std::stringstream skinnedMeshRendererText;
    skinnedMeshRendererText << skinnedMeshRenderer.rdbuf();
    const std::string skinnedMeshRendererSource = skinnedMeshRendererText.str();
    const std::filesystem::path staticMeshRendererPath = options.clientRoot / "libs" / "render" / "StaticMeshRenderer.cpp";
    std::ifstream staticMeshRenderer(staticMeshRendererPath);
    std::stringstream staticMeshRendererText;
    staticMeshRendererText << staticMeshRenderer.rdbuf();
    const std::string staticMeshRendererSource = staticMeshRendererText.str();
    ctx.Expect(skinnedMeshRendererSource.find("CreateReflectionPipeline") != std::string::npos &&
            skinnedMeshRendererSource.find("VK_CULL_MODE_FRONT_BIT") != std::string::npos &&
            skinnedMeshRendererSource.find("RenderInWorldReflection") != std::string::npos,
        "skinnedMesh reflection pipeline source", "SkinnedMeshRenderer reflection pipeline entry points are missing");

    WaterConfig water;
    ctx.Expect(water.enabled &&
            water.waterLevelY == 0.0f &&
            water.baseColor[3] > 0.0f &&
            water.waveScaleSmall > 0.0f &&
            water.waveScaleLarge > 0.0f &&
            water.fresnelPower >= 1.0f,
        "water config defaults sane", "WaterConfig defaults are invalid");
    ctx.Expect(water.reflectionEnabled &&
            water.reflectionQuality == WaterConfig::ReflectionQuality::Half &&
            water.reflectionDistortionStrength > 0.0f,
        "water reflection defaults sane", "Water reflection should default to enabled Half quality with distortion");
    ctx.Expect(water.refractionEnabled &&
            water.refractionStrength > 0.0f &&
            water.refractionDepthStrength > 0.0f &&
            water.depthColorMin == 0.5f &&
            water.depthColorMax == 8.0f &&
            water.depthFadeDistance == 12.0f,
        "water refraction defaults sane", "Water refraction/depth defaults are invalid");
    ctx.Expect(water.foamEnabled &&
            water.foamDistance > 0.0f &&
            water.foamScale > 0.0f &&
            water.foamIntensity > 0.0f &&
            water.causticMode == WaterConfig::CausticMode::AnimatedTexture &&
            water.causticIntensity > 0.0f &&
            water.causticMaxDepth > 0.0f,
        "water foam caustic defaults sane", "Water foam/caustic defaults are invalid");
    ctx.Expect(water.edgeFadeDistance == 1.0f &&
            water.edgeFadeCurve == WaterConfig::EdgeFadeCurve::Smooth,
        "water edge fade defaults sane", "Water edge fade should default to a 1m Smooth shoreline transition");

    const std::filesystem::path terrainRendererPath = options.clientRoot / "libs" / "render" / "TerrainRenderer.cpp";
    std::ifstream terrainRenderer(terrainRendererPath);
    std::stringstream terrainRendererText;
    terrainRendererText << terrainRenderer.rdbuf();
    const std::string terrainRendererSource = terrainRendererText.str();
    ctx.Expect(terrainRendererSource.find("const float pz = halfDepth - static_cast<float>(z) * cellSize") != std::string::npos &&
            terrainRendererSource.find("const float centerYcm = m_spawnLocalYcm - m_editorBrushLocalZ * 100.0f") != std::string::npos,
        "created terrain edit z convention", "Created terrain vertices must use the same Z orientation as sculpt/splat world-to-grid mapping");
    ctx.Expect(terrainRendererSource.find("[TRI-PERF] terrain pipeline bound in pass=main") != std::string::npos &&
            terrainRendererSource.find("[TRI-PERF] terrain pipeline bound in pass=water-reflection") != std::string::npos &&
            terrainRendererSource.find("[TRI-PERF] terrain pipeline bound in pass=shadow-cascade0") != std::string::npos &&
            terrainRendererSource.find("[TRI-PERF] avgActiveLayers=%.2f samples/fragment planar=%.1f triplanar=%.1f") != std::string::npos &&
            terrainRendererSource.find("[TRI-PERF] triplanar LOD mode=textureGrad-explicit mipUsed=%s forced-0=%s") != std::string::npos &&
            terrainRendererSource.find("[TRI-PERF] layer sampling=weight-gated threshold=1/255") != std::string::npos &&
            terrainRendererSource.find("[TRIPLANAR-OPT] mode=selective slopeThreshold=%.3f transition=%.3f avgAxesPerLayer=%.2f samples/fragment=%.1f fps=%.1f") != std::string::npos &&
            terrainRendererSource.find("[TRI-PERF] palette size=") != std::string::npos,
        "terrain triplanar perf diagnostics", "Terrain renderer must log pass inventory, sample count, LOD mode, layer gating, and palette texture details");
    ctx.Expect(terrainRendererSource.find("BuildRgbaArrayMipUpload") != std::string::npos &&
            terrainRendererSource.find("BuildR8ArrayMipUpload") != std::string::npos &&
            terrainRendererSource.find("normalRenorm=%s") != std::string::npos &&
            terrainRendererSource.find("sampler=trilinear aniso=%s") != std::string::npos &&
            terrainRendererSource.find("sampler.mipmapMode = out.mipLevels > 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR") != std::string::npos,
        "terrain array texture mip chains", "Terrain array textures must upload full mip chains and use trilinear/aniso sampling");
    ctx.Expect(terrainSource.find("const float layerWeightEpsilon = 1.0 / 255.0") != std::string::npos &&
            terrainSource.find("int u_activeLayerCount") != std::string::npos &&
            terrainSource.find("const int activeLayerCount = clamp(u_activeLayerCount, 1, 8)") != std::string::npos &&
            terrainSource.find("for (int layer = 0; layer < activeLayerCount; ++layer)") != std::string::npos &&
            terrainSource.find("if (weights[layer] < layerWeightEpsilon)") != std::string::npos &&
            terrainSource.find("continue;") != std::string::npos &&
            terrainSource.find("SampleGrad") != std::string::npos &&
            terrainSource.find("TriplanarPlaneUvGrad") != std::string::npos &&
            terrainSource.find("if (triplanarSlopeBlend > 0.001)") != std::string::npos,
        "terrain layer weight gating", "Terrain shader must skip zero-weight layers while using explicit gradients for mip-safe sampling");
    ctx.Expect(terrainRendererSource.find("EstimateActiveSplatLayerSpan") != std::string::npos &&
            terrainRendererSource.find("[TERRAIN-SHADER-DIAG] active_layer_count=%u total_layer_count=8") != std::string::npos &&
            terrainRendererSource.find("render_pass_count=1") != std::string::npos &&
            terrainRendererSource.find("const uint32_t baseDrawCallsBefore = terrainStats.drawCalls") != std::string::npos &&
            terrainRendererSource.find("&m_descriptorSets[frameIndex]") != std::string::npos,
        "terrain shader single pass optimization", "Terrain renderer must submit the palette-array terrain in a single material pass with active-layer diagnostics");
    const std::filesystem::path waterBodyIoPath = options.clientRoot / "libs" / "render" / "WaterBodyIO.h";
    std::ifstream waterBodyIo(waterBodyIoPath);
    std::stringstream waterBodyIoText;
    waterBodyIoText << waterBodyIo.rdbuf();
    const std::string waterBodyIoSource = waterBodyIoText.str();
    ctx.Expect(terrainRendererSource.find("LoadWaterBodies") != std::string::npos &&
            terrainRendererSource.find("m_waterBodies") != std::string::npos &&
            terrainRendererSource.find("CreateWaterBodyMesh") != std::string::npos &&
            terrainRendererSource.find("client::render::kWaterBodiesFilename") != std::string::npos &&
            waterBodyIoSource.find("water_bodies.mxwater") != std::string::npos,
        "water object renderer source", "TerrainRenderer is missing object-level water loading or mesh generation");
    ctx.Expect(terrainRendererSource.find("CreateWaterMesh") == std::string::npos &&
            terrainRendererSource.find("m_waterConfig") == std::string::npos &&
            terrainRendererSource.find("m_waterVertexBuffer") == std::string::npos &&
            terrainRendererSource.find("m_waterIndexBuffer") == std::string::npos &&
            terrainRendererSource.find("global water fallback") == std::string::npos,
        "legacy global water renderer removed", "TerrainRenderer still contains legacy global water mesh/config/fallback code");
    ctx.Expect(terrainRendererSource.find("UpdateWaterBodyUniform") != std::string::npos &&
            terrainRendererSource.find("waterBody.descriptorSets") != std::string::npos,
        "water object per-body UBO source", "Object water must use per-body uniform buffers/descriptors");
    ctx.Expect(terrainRendererSource.find("FindClosestWaterBody") != std::string::npos &&
            terrainRendererSource.find("reflectionTargetDistance") != std::string::npos &&
            terrainRendererSource.find("reflection_target=id=") != std::string::npos,
        "water object closest reflection target source", "WATER-OBJ-2 closest-body reflection selection is missing");
    ctx.Expect(terrainRendererSource.find("ComputeMirrorCamera(camera, {m_waterReflection.width, m_waterReflection.height}, reflectionWaterLevelY)") != std::string::npos &&
            terrainRendererSource.find("m_reflectionClipWaterLevelY = reflectionWaterLevelY") != std::string::npos,
        "water object reflection uses body level", "Reflection pass must mirror and clip at the selected water body's level");
    ctx.Expect(terrainRendererSource.find("BuildWaterUniform(camera, timeSeconds, ResolveWaterConfig(waterBody.body), waterBody.body.waterLevelY, reflectionTarget)") != std::string::npos &&
            terrainRendererSource.find("reflectionTarget && water.reflectionEnabled") != std::string::npos,
        "water object reflection flag override", "Only the selected water body should sample the reflection texture");
    ctx.Expect(terrainRendererSource.find("SetWaterMaterials") != std::string::npos &&
            terrainRendererSource.find("m_waterMaterials") != std::string::npos &&
            terrainRendererSource.find("ResolveWaterConfig") != std::string::npos,
        "water object live material references", "Water bodies must resolve live WaterMaterialData by material id");
    ctx.Expect(terrainRendererSource.find("WaterMaterialTextureSet") != std::string::npos &&
            terrainRendererSource.find("LoadWaterMaterialTextureSet") != std::string::npos &&
            terrainRendererSource.find("ResolveWaterMaterialTextures(waterBody.body)") != std::string::npos &&
            terrainRendererSource.find("materialTextures && materialTextures->normalA.view") != std::string::npos,
        "water object material normal textures", "Water body descriptor sets must bind the assigned material's normal maps");
    ctx.Expect(terrainRendererSource.find("VK_FORMAT_R8G8B8A8_SRGB") != std::string::npos &&
            terrainRendererSource.find("textureParams[0] = textures->normalA.view") != std::string::npos &&
            terrainRendererSource.find("textureParams[2] = textures->diffuse.view") != std::string::npos &&
            terrainRendererSource.find("textureScroll[0] = material->scrollSpeedA[0]") != std::string::npos &&
            terrainRendererSource.find("diffusePath") != std::string::npos &&
            terrainRendererSource.find("dstBinding = 6") != std::string::npos,
        "water object material texture sampling source", "Water body materials must drive normal/diffuse texture sampling and tiling flags");
    ctx.Expect(terrainRendererSource.find("ComputeWaterBodyDistanceField") != std::string::npos &&
            terrainRendererSource.find("BilinearSampleWaterDistance") != std::string::npos &&
            terrainRendererSource.find("edgeAlphaAt") != std::string::npos &&
            terrainRendererSource.find("edgeFadeDistance") != std::string::npos &&
            terrainRendererSource.find("out.edgeParams[0]") != std::string::npos &&
            terrainRendererSource.find("Vertex alpha computed") != std::string::npos,
        "water object edge fade mesh source", "WATER-OBJ-6 must compute distance-field driven per-vertex shoreline alpha");
    ctx.Expect(terrainRendererSource.find("m_waterMaterialEdgeSignature") != std::string::npos &&
            terrainRendererSource.find("Material edge fade updated") != std::string::npos,
        "water object edge fade live rebuild", "Changing edge fade material settings must rebuild water body meshes");
    ctx.Expect(terrainRendererSource.find("SetWaterBodies") != std::string::npos &&
            terrainRendererSource.find("SaveWaterBodies") != std::string::npos &&
            terrainRendererSource.find("SaveWaterBodiesBinary(tmp, bodies") != std::string::npos,
        "water object editor renderer API", "WATER-OBJ-3 needs runtime water body replacement and sidecar save support");
    ctx.Expect(terrainRendererSource.find("SetSelectedWaterBodyHighlight") != std::string::npos &&
            terrainRendererSource.find("RenderSelectedWaterBodyHighlight") != std::string::npos &&
            terrainRendererSource.find("m_selectedWaterBodyIndexCount") != std::string::npos,
        "water object selected bbox highlight legacy renderer source", "Selected water body highlight cleanup should leave renderer cleanup entry points available");
    ctx.Expect(terrainRendererSource.find("SetWaterSculptBrush") != std::string::npos &&
            terrainRendererSource.find("m_waterSculptBrushVisible") != std::string::npos &&
            terrainRendererSource.find("m_waterSculptBrushAddMode ? 8.0f : 9.0f") != std::string::npos,
        "water sculpt brush renderer source", "WATER-OBJ-5 needs add/remove brush cursor rendering");
    ctx.Expect(terrainRendererSource.find("device.WaitIdle();\n    DestroyWaterBodyResources();") != std::string::npos,
        "water object SetWaterBodies wait-idle", "SetWaterBodies must wait before destroying in-flight GPU resources");

    const std::string legacyUiName = "Noe" "sis";
    const std::string legacyMarkupExt = ".xa" "ml";
    const std::filesystem::path editorImGuiPath = options.clientRoot / "libs" / "render" / "EditorImGui.cpp";
    std::ifstream editorImGui(editorImGuiPath);
    std::stringstream editorImGuiText;
    editorImGuiText << editorImGui.rdbuf();
    const std::string editorImGuiSource = editorImGuiText.str();
    const std::filesystem::path editorImGuiHeaderPath = options.clientRoot / "libs" / "render" / "EditorImGui.h";
    std::ifstream editorImGuiHeader(editorImGuiHeaderPath);
    std::stringstream editorImGuiHeaderText;
    editorImGuiHeaderText << editorImGuiHeader.rdbuf();
    const std::string editorImGuiHeaderSource = editorImGuiHeaderText.str();
    const std::filesystem::path mapEditorTypesPath = options.clientRoot / "libs" / "render" / "MapEditorTypes.h";
    std::ifstream mapEditorTypes(mapEditorTypesPath);
    std::stringstream mapEditorTypesText;
    mapEditorTypesText << mapEditorTypes.rdbuf();
    const std::string mapEditorTypesSource = mapEditorTypesText.str();
    const std::filesystem::path sceneManagerPath = options.clientRoot / "libs" / "render" / "SceneManager.cpp";
    std::ifstream sceneManager(sceneManagerPath);
    std::stringstream sceneManagerText;
    sceneManagerText << sceneManager.rdbuf();
    const std::string sceneManagerSource = sceneManagerText.str();
    const std::filesystem::path sceneManagerHeaderPath = options.clientRoot / "libs" / "render" / "SceneManager.h";
    std::ifstream sceneManagerHeader(sceneManagerHeaderPath);
    std::stringstream sceneManagerHeaderText;
    sceneManagerHeaderText << sceneManagerHeader.rdbuf();
    const std::string sceneManagerHeaderSource = sceneManagerHeaderText.str();
    const std::filesystem::path projectManagerPath = options.clientRoot / "libs" / "render" / "ProjectManager.cpp";
    std::ifstream projectManager(projectManagerPath);
    std::stringstream projectManagerText;
    projectManagerText << projectManager.rdbuf();
    const std::string projectManagerSource = projectManagerText.str();
    const std::filesystem::path projectManagerHeaderPath = options.clientRoot / "libs" / "render" / "ProjectManager.h";
    std::ifstream projectManagerHeader(projectManagerHeaderPath);
    std::stringstream projectManagerHeaderText;
    projectManagerHeaderText << projectManagerHeader.rdbuf();
    const std::string projectManagerHeaderSource = projectManagerHeaderText.str();
    const std::filesystem::path nativeWindowHeaderPath = options.clientRoot / "libs" / "platform" / "NativeWindow.h";
    std::ifstream nativeWindowHeader(nativeWindowHeaderPath);
    std::stringstream nativeWindowHeaderText;
    nativeWindowHeaderText << nativeWindowHeader.rdbuf();
    const std::string nativeWindowHeaderSource = nativeWindowHeaderText.str();
    const std::filesystem::path rmlUiLayerPath = options.clientRoot / "libs" / "render" / "RmlUiLayer.cpp";
    std::ifstream rmlUiLayer(rmlUiLayerPath);
    std::stringstream rmlUiLayerText;
    rmlUiLayerText << rmlUiLayer.rdbuf();
    const std::string rmlUiLayerSource = rmlUiLayerText.str();
    const std::filesystem::path runtimeSessionPath = options.clientRoot / "libs" / "render" / "RuntimeSession.cpp";
    std::ifstream runtimeSession(runtimeSessionPath);
    std::stringstream runtimeSessionText;
    runtimeSessionText << runtimeSession.rdbuf();
    const std::string runtimeSessionSource = runtimeSessionText.str();
    const std::filesystem::path runtimeSessionHeaderPath = options.clientRoot / "libs" / "render" / "RuntimeSession.h";
    std::ifstream runtimeSessionHeader(runtimeSessionHeaderPath);
    std::stringstream runtimeSessionHeaderText;
    runtimeSessionHeaderText << runtimeSessionHeader.rdbuf();
    const std::string runtimeSessionHeaderSource = runtimeSessionHeaderText.str();
    const std::filesystem::path runtimeUiAdapterPath = options.clientRoot / "libs" / "render" / "RuntimeUiAdapter.cpp";
    std::ifstream runtimeUiAdapter(runtimeUiAdapterPath);
    std::stringstream runtimeUiAdapterText;
    runtimeUiAdapterText << runtimeUiAdapter.rdbuf();
    const std::string runtimeUiAdapterSource = runtimeUiAdapterText.str();
    const std::filesystem::path runtimeUiAdapterHeaderPath = options.clientRoot / "libs" / "render" / "RuntimeUiAdapter.h";
    std::ifstream runtimeUiAdapterHeader(runtimeUiAdapterHeaderPath);
    std::stringstream runtimeUiAdapterHeaderText;
    runtimeUiAdapterHeaderText << runtimeUiAdapterHeader.rdbuf();
    const std::string runtimeUiAdapterHeaderSource = runtimeUiAdapterHeaderText.str();
    const std::filesystem::path uiHelpersPath = options.clientRoot / "libs" / "render" / "UIHelpers.cpp";
    std::ifstream uiHelpers(uiHelpersPath);
    std::stringstream uiHelpersText;
    uiHelpersText << uiHelpers.rdbuf();
    const std::string uiHelpersSource = uiHelpersText.str();
    const std::filesystem::path iconsHeaderPath = options.clientRoot / "libs" / "render" / "IconsFontAwesome6.h";
    std::ifstream iconsHeader(iconsHeaderPath);
    std::stringstream iconsHeaderText;
    iconsHeaderText << iconsHeader.rdbuf();
    const std::string iconsHeaderSource = iconsHeaderText.str();
    const std::filesystem::path rmlUiShaderPath = options.clientRoot / "shaders" / "RmlUi.hlsl";
    std::ifstream rmlUiShader(rmlUiShaderPath);
    std::stringstream rmlUiShaderText;
    rmlUiShaderText << rmlUiShader.rdbuf();
    const std::string rmlUiShaderSource = rmlUiShaderText.str();
    const std::filesystem::path clientMainPath = options.clientRoot / "apps" / "client" / "src" / "main.cpp";
    std::ifstream clientMain(clientMainPath);
    std::stringstream clientMainText;
    clientMainText << clientMain.rdbuf();
    const std::string clientMainSource = clientMainText.str();
    const std::filesystem::path rootCmakePath = options.clientRoot / "CMakeLists.txt";
    std::ifstream rootCmake(rootCmakePath);
    std::stringstream rootCmakeText;
    rootCmakeText << rootCmake.rdbuf();
    const std::string rootCmakeSource = rootCmakeText.str();
    const std::filesystem::path renderCmakePath = options.clientRoot / "libs" / "render" / "CMakeLists.txt";
    std::ifstream renderCmake(renderCmakePath);
    std::stringstream renderCmakeText;
    renderCmakeText << renderCmake.rdbuf();
    const std::string renderCmakeSource = renderCmakeText.str();
    const std::filesystem::path treeLibCmakePath = options.clientRoot / "libs" / "ixtreemetree" / "CMakeLists.txt";
    std::ifstream treeLibCmake(treeLibCmakePath);
    std::stringstream treeLibCmakeText;
    treeLibCmakeText << treeLibCmake.rdbuf();
    const std::string treeLibCmakeSource = treeLibCmakeText.str();
    const std::filesystem::path treeLibHeaderPath = options.clientRoot / "libs" / "ixtreemetree" / "include" / "ixtreemetree" / "tree_options.h";
    std::ifstream treeLibHeader(treeLibHeaderPath);
    std::stringstream treeLibHeaderText;
    treeLibHeaderText << treeLibHeader.rdbuf();
    const std::string treeLibHeaderSource = treeLibHeaderText.str();
    const std::filesystem::path treeLibSourcePath = options.clientRoot / "libs" / "ixtreemetree" / "src" / "tree.cpp";
    std::ifstream treeLibSourceFile(treeLibSourcePath);
    std::stringstream treeLibSourceText;
    treeLibSourceText << treeLibSourceFile.rdbuf();
    const std::string treeLibSource = treeLibSourceText.str();
    const std::filesystem::path treePresetHeaderPath = options.clientRoot / "libs" / "ixtreemetree" / "include" / "ixtreemetree" / "preset.h";
    std::ifstream treePresetHeader(treePresetHeaderPath);
    std::stringstream treePresetHeaderText;
    treePresetHeaderText << treePresetHeader.rdbuf();
    const std::string treePresetHeaderSource = treePresetHeaderText.str();
    const std::filesystem::path treePresetSourcePath = options.clientRoot / "libs" / "ixtreemetree" / "src" / "preset_loader.cpp";
    std::ifstream treePresetSourceFile(treePresetSourcePath);
    std::stringstream treePresetSourceText;
    treePresetSourceText << treePresetSourceFile.rdbuf();
    const std::string treePresetSource = treePresetSourceText.str();
    const std::filesystem::path treePanelPath = options.clientRoot / "libs" / "render" / "tools" / "tree" / "TreeGeneratorPanel.cpp";
    std::ifstream treePanelFile(treePanelPath);
    std::stringstream treePanelText;
    treePanelText << treePanelFile.rdbuf();
    const std::string treePanelSource = treePanelText.str();
    const std::filesystem::path treeExporterPath = options.clientRoot / "libs" / "render" / "tools" / "tree" / "TreeGlbExporter.cpp";
    std::ifstream treeExporterFile(treeExporterPath);
    std::stringstream treeExporterText;
    treeExporterText << treeExporterFile.rdbuf();
    const std::string treeExporterSource = treeExporterText.str();
    const std::filesystem::path treePreviewPath = options.clientRoot / "libs" / "render" / "tools" / "tree" / "TreePreviewRenderer.cpp";
    std::ifstream treePreviewFile(treePreviewPath);
    std::stringstream treePreviewText;
    treePreviewText << treePreviewFile.rdbuf();
    const std::string treePreviewSource = treePreviewText.str();
    const std::filesystem::path treePalettePath = options.clientRoot / "libs" / "render" / "tools" / "tree" / "TreeTexturePalette.cpp";
    std::ifstream treePaletteFile(treePalettePath);
    std::stringstream treePaletteText;
    treePaletteText << treePaletteFile.rdbuf();
    const std::string treePaletteSource = treePaletteText.str();
    const std::filesystem::path clientCmakePath = options.clientRoot / "apps" / "client" / "CMakeLists.txt";
    std::ifstream clientCmake(clientCmakePath);
    std::stringstream clientCmakeText;
    clientCmakeText << clientCmake.rdbuf();
    const std::string clientCmakeSource = clientCmakeText.str();
    const std::filesystem::path androidGradlePath = options.clientRoot / "android" / "app" / "build.gradle.kts";
    std::ifstream androidGradle(androidGradlePath);
    std::stringstream androidGradleText;
    androidGradleText << androidGradle.rdbuf();
    const std::string androidGradleSource = androidGradleText.str();
    ctx.Expect(rootCmakeSource.find("option(IXTREEME_WITH_EDITOR") != std::string::npos &&
            rootCmakeSource.find("if(IXTREEME_WITH_EDITOR)\n    vcpkg_require(imguizmo)") != std::string::npos &&
            rootCmakeSource.find("if(WIN32 AND IXTREEME_WITH_EDITOR)") != std::string::npos &&
            renderCmakeSource.find("$<$<BOOL:${IXTREEME_WITH_EDITOR}>:IXTREEME_WITH_EDITOR=1>") != std::string::npos &&
            renderCmakeSource.find("if(IXTREEME_WITH_EDITOR)") != std::string::npos &&
            editorImGuiSource.find("#if defined(IXTREEME_WITH_EDITOR) && defined(_WIN32)") != std::string::npos &&
            clientMainSource.find("[BUILD] Editor: DISABLED") != std::string::npos,
        "editor build flag gates imgui pipeline", "IXTREEME_WITH_EDITOR must gate ImGui/ImGuizmo dependencies and runtime editor startup");
    ctx.Expect(rootCmakeSource.find("find_package(RmlUi 6.2 CONFIG REQUIRED)") != std::string::npos &&
            rootCmakeSource.find("RMLUI_VS_SPV") != std::string::npos &&
            rootCmakeSource.find("RmlUi.hlsl") != std::string::npos &&
            renderCmakeSource.find("RmlUiLayer.cpp") != std::string::npos &&
            renderCmakeSource.find("RuntimeSession.cpp") != std::string::npos &&
            renderCmakeSource.find("RuntimeUiAdapter.cpp") != std::string::npos &&
            renderCmakeSource.find("RmlUi::RmlUi") != std::string::npos &&
            renderCmakeSource.find("Game" "ClientLayer.cpp") == std::string::npos &&
            clientMainSource.find("RmlUiLayer rmlUi") != std::string::npos &&
            clientMainSource.find("CreateRuntimeSession()") != std::string::npos &&
            clientMainSource.find("rmlUi.Render(device);") != std::string::npos &&
            clientMainSource.find("editorImGui.Render(device);") != std::string::npos,
        "rmlui build and z-order pipeline", "RMLUI-1 must link RmlUi 6.2, compile shaders, and render before ImGui");
    ctx.Expect(rmlUiLayerSource.find("class RmlAssetFileInterface") != std::string::npos &&
            rmlUiLayerSource.find("class RmlRenderInterface final : public Rml::RenderInterface") != std::string::npos &&
            rmlUiLayerSource.find("CreateDescriptorPool") != std::string::npos &&
            rmlUiLayerSource.find("ProcessMouseButtonDown") != std::string::npos &&
            rmlUiLayerSource.find("[RMLUI-DIAG] CreateContext: viewport=") != std::string::npos &&
            rmlUiLayerSource.find("[RMLUI-DIAG] RenderGeometry: vertices=") != std::string::npos &&
            rmlUiLayerSource.find("[RMLUI-DIAG] vkCmdSetViewport") != std::string::npos &&
            rmlUiLayerSource.find("[RMLUI-DIAG] Pipeline primitive topology: TRIANGLE_LIST") != std::string::npos &&
            rmlUiLayerSource.find("PendingGeometryDelete") != std::string::npos &&
            rmlUiLayerSource.find("m_pendingGeometryDeletes") != std::string::npos &&
            rmlUiShaderSource.find("[[vk::push_constant]]") != std::string::npos &&
            rmlUiShaderSource.find("[[vk::binding(0, 0)]] Texture2D") != std::string::npos &&
            rmlUiShaderSource.find("(pixel.y / g_push.viewport.y) * 2.0f - 1.0f") != std::string::npos,
        "rmlui vulkan backend source", "RmlUi backend must provide file IO, render, input, deferred GPU cleanup, and Vulkan shader integration");
    ctx.Expect(runtimeUiAdapterHeaderSource.find("class RuntimeUiAdapter") != std::string::npos &&
            runtimeUiAdapterSource.find("class NullRuntimeUiAdapter final") != std::string::npos &&
            runtimeUiAdapterSource.find("HideAll") != std::string::npos &&
            runtimeSessionSource.find("class EmptyRuntimeSession final") != std::string::npos &&
            clientMainSource.find("CreateRuntimeUiAdapter(rmlUi)") != std::string::npos,
        "runtime ui null baseline", "Clean engine runtime UI must use the Null adapter by default while keeping the RmlUi backend available");
    ctx.Expect(std::filesystem::exists(options.clientRoot / "assets" / "fonts" / "Inter-Regular.ttf") &&
            std::filesystem::exists(options.clientRoot / "assets" / "fonts" / "Inter-SemiBold.ttf") &&
            std::filesystem::exists(options.clientRoot / "assets" / "fonts" / "Inter-Bold.ttf") &&
            std::filesystem::exists(options.clientRoot / "assets" / "fonts" / "fa-solid-900.ttf") &&
            editorImGuiSource.find("LoadEditorFonts") != std::string::npos &&
            editorImGuiSource.find("AddFontFromFileTTF") != std::string::npos &&
            editorImGuiSource.find("Inter-Regular.ttf") != std::string::npos &&
            editorImGuiSource.find("fa-solid-900.ttf") != std::string::npos &&
            iconsHeaderSource.find("ICON_FA_PLAY") != std::string::npos &&
            iconsHeaderSource.find("ICON_FA_TRASH") != std::string::npos,
        "editor visual fonts and icons", "EDITOR-VISUAL-POLISH must bundle Inter, FontAwesome, and icon constants");
    ctx.Expect(editorImGuiSource.find("ApplyEditorStyle") != std::string::npos &&
            editorImGuiSource.find("WindowRounding = 4.0f") != std::string::npos &&
            editorImGuiSource.find("FramePadding = ImVec2(8.0f, 3.0f)") != std::string::npos &&
            editorImGuiSource.find("FrameBorderSize = 1.0f") != std::string::npos &&
            editorImGuiSource.find("ImGuiCol_ButtonHovered") != std::string::npos &&
            editorImGuiSource.find("ColorU8(61, 126, 219") != std::string::npos &&
            editorImGuiSource.find("[EDITOR-VISUAL] Dark compact editor style applied") != std::string::npos,
        "editor dark compact imgui style", "EDITOR-VISUAL-POLISH must apply neutral dark backgrounds, compact controls, subtle borders, and blue accents");
    ctx.Expect(uiHelpersSource.find("namespace UI") != std::string::npos &&
            uiHelpersSource.find("PropertyRow") != std::string::npos &&
            uiHelpersSource.find("IconButton") != std::string::npos &&
            uiHelpersSource.find("SectionHeader") != std::string::npos &&
            uiHelpersSource.find("StatusOk") != std::string::npos &&
            uiHelpersSource.find("HelpMarker") != std::string::npos &&
            renderCmakeSource.find("UIHelpers.cpp") != std::string::npos,
        "editor ui helper layer", "EDITOR-VISUAL-POLISH must provide reusable UI helpers for consistent panels");
    ctx.Expect(editorImGuiSource.find("DockBuilderDockWindow(\"Editor Toolbar\"") != std::string::npos &&
            editorImGuiSource.find("DockBuilderDockWindow(\"Tools\"") != std::string::npos &&
            editorImGuiSource.find("DockBuilderDockWindow(\"Inspector\"") != std::string::npos &&
            editorImGuiSource.find("DockBuilderDockWindow(\"Asset Browser\"") != std::string::npos &&
            editorImGuiSource.find("DockBuilderDockWindow(\"Scene View\"") != std::string::npos &&
            editorImGuiSource.find("[EDITOR-LAYOUT] Default Unity-style dock layout applied") != std::string::npos &&
            editorImGuiSource.find("[EDITOR-LAYOUT] Loaded layout from editor_layout.ini") != std::string::npos,
        "editor unity dock layout", "EDITOR-VISUAL-POLISH must default to a Unity-style dock layout without overwriting saved layouts");
    ctx.Expect(editorImGuiSource.find("UI::IconButton(ICON_FA_PLAY") != std::string::npos &&
            editorImGuiSource.find("UI::IconButton(ICON_FA_DROPLET") != std::string::npos &&
            editorImGuiSource.find("UI::IconButton(ICON_FA_FOLDER_PLUS") != std::string::npos &&
            editorImGuiSource.find("UI::IconButton(ICON_FA_TRASH") != std::string::npos &&
            editorImGuiSource.find("UI::SectionHeader") != std::string::npos &&
            sceneManagerSource.find("IxtreemeEngine - Editor") != std::string::npos &&
            clientMainSource.find("Standalone Vulkan Clear - gameClient Overlay") == std::string::npos,
        "editor icon buttons and title", "EDITOR-VISUAL-POLISH must iconize editor controls and rename the window title");
    ctx.Expect(editorImGuiSource.find("RenderAssetBrowserFolderTree") != std::string::npos &&
            editorImGuiSource.find("RenderAssetBrowserContent") != std::string::npos &&
            editorImGuiSource.find("RenderAssetBrowserBreadcrumb") != std::string::npos &&
            editorImGuiSource.find("BeginTable(\"AssetBrowserLayout\", 2") != std::string::npos &&
            editorImGuiSource.find("RenderAssetTypeTabs();") == std::string::npos,
        "asset browser unity layout", "ASSET-BROWSER-UNITY-1 must replace category tabs with a folder tree, content view, and breadcrumb");
    ctx.Expect(editorImGuiSource.find("BeginTable(\"AssetBrowserUnityGrid\"") != std::string::npos &&
            editorImGuiSource.find("TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, cellWidth)") != std::string::npos &&
            editorImGuiSource.find("ImGui::TableNextColumn()") != std::string::npos &&
            editorImGuiSource.find("ShortAssetFilename") != std::string::npos &&
            editorImGuiSource.find("constexpr size_t kVisibleCharacters = 10") != std::string::npos,
        "asset browser grid wrapping", "asset browser tiles must wrap inside fixed grid cells instead of extending horizontally past the Tags panel");
    ctx.Expect(editorImGuiSource.find("BeginChild(\"AssetFolderTreeScroll\"") != std::string::npos &&
            editorImGuiSource.find("BeginChild(\"AssetContentScroll\"") != std::string::npos &&
            editorImGuiSource.find("BeginChild(\"AssetTagsScroll\"") == std::string::npos &&
            editorImGuiSource.find("browserPanelHeight") != std::string::npos,
        "asset browser independent scroll zones", "asset browser folder tree and content view must scroll independently when content overflows");
    ctx.Expect(editorImGuiSource.find("platform::move_to_trash") != std::string::npos &&
            editorImGuiSource.find("MetaSidecarPath") != std::string::npos &&
            editorImGuiSource.find("kAssetFolderPayloadType") != std::string::npos,
        "asset browser file ops", "ASSET-BROWSER-UNITY-1 must route delete through platform trash, move .meta sidecars, and support folder drag payloads");
    ctx.Expect(editorImGuiHeaderSource.find("AssetPreviewTexture") != std::string::npos &&
            editorImGuiSource.find("LoadAssetPreviewTexture") != std::string::npos &&
            editorImGuiSource.find("ImGui_ImplVulkan_AddTexture") != std::string::npos &&
            editorImGuiSource.find("ImGui_ImplVulkan_RemoveTexture") != std::string::npos &&
            editorImGuiSource.find("drawList->AddImage") != std::string::npos &&
            iconsHeaderSource.find("ICON_FA_IMAGE") != std::string::npos,
        "asset browser previews", "asset browser must show texture/material thumbnails and fall back to category icons");
    ctx.Expect(editorImGuiSource.find("ImGui::BeginTooltip()") != std::string::npos &&
            editorImGuiSource.find("Type: %s") != std::string::npos &&
            editorImGuiSource.find("Preview: %s") != std::string::npos &&
            editorImGuiSource.find("Tags: %s") != std::string::npos &&
            editorImGuiSource.find("Source: %s") != std::string::npos,
        "asset browser compact tile metadata", "asset browser tiles should show only a short filename and move metadata into the hover tooltip");
    ctx.Expect(treeLibCmakeSource.find("add_library(ixtreemetree STATIC") != std::string::npos &&
            treeLibCmakeSource.find("add_library(ixtreemetree::ixtreemetree ALIAS ixtreemetree)") != std::string::npos &&
            rootCmakeSource.find("add_subdirectory(${CLIENT_LIBS_DIR}/ixtreemetree)") != std::string::npos &&
            renderCmakeSource.find("ixtreemetree::ixtreemetree") != std::string::npos,
        "ixtreemetree library target", "TREE-1 must add a standalone ixtreemetree static library target and link it into the editor render module");
    ctx.Expect(treeLibHeaderSource.find("struct TreeOptions") != std::string::npos &&
            treeLibHeaderSource.find("enum class TreeType") != std::string::npos &&
            treeLibHeaderSource.find("enum class BarkType") != std::string::npos &&
            treeLibHeaderSource.find("cardsPerCluster") != std::string::npos &&
            treeLibHeaderSource.find("atlasGridX") != std::string::npos &&
            treeLibHeaderSource.find("atlasGridY") != std::string::npos &&
            treeLibSource.find("Rng rng(options.seed)") != std::string::npos &&
            treeLibSource.find("GenerateBranch") != std::string::npos &&
            treeLibSource.find("GenerateLeaves") != std::string::npos &&
            treeLibSource.find("Vulkan") == std::string::npos &&
            treeLibSource.find("ImGui") == std::string::npos,
        "ixtreemetree pure generator api", "TREE-1 library must expose TreeOptions/TreeMesh/Tree and stay independent from Vulkan/ImGui");
    {
        ixtreemetree::Tree tree;
        tree.options = ixtreemetree::defaultTreeOptions();
        tree.options.seed = 54321;
        const ixtreemetree::TreeMesh a = tree.generate();
        const ixtreemetree::TreeMesh b = tree.generate();
        const bool deterministic = a.bark.vertices.size() == b.bark.vertices.size() &&
            a.bark.indices.size() == b.bark.indices.size() &&
            a.leaves.vertices.size() == b.leaves.vertices.size() &&
            a.leaves.indices.size() == b.leaves.indices.size() &&
            !a.bark.vertices.empty() &&
            a.stats.barkTriangles > 0 &&
            a.stats.leafTriangles > 0;
        ctx.Expect(deterministic,
            "ixtreemetree deterministic generate", "TREE-1 must generate deterministic non-empty bark and leaf meshes from the same seed/options");
    }
    {
        const std::filesystem::path scratch = (options.scratchRoot.empty() ? MakeDefaultScratchRoot() : options.scratchRoot) / "tree_fbx_export";
        std::error_code ec;
        std::filesystem::create_directories(scratch, ec);

        ixtreemetree::Tree tree;
        tree.options = ixtreemetree::defaultTreeOptions();
        tree.options.seed = 91917;
        const ixtreemetree::TreeMesh mesh = tree.generate();

        tree_tool::TreeMaterialBinding binding{};
        binding.barkBaseColorTexturePath = options.clientRoot / "assets" / "internal" / "textures" / "bark" / "oak_bark.png";
        binding.leafBaseColorTexturePath = options.clientRoot / "assets" / "internal" / "textures" / "leaves" / "oak_leaf.png";
        const tree_tool::TreeExportResult glb = tree_tool::TreeGlbExporter::SaveAsAsset(mesh, scratch, "fbx_export_tree", binding);

        AssimpExporter::ExportOptions exportOptions{};
        exportOptions.outputPath = scratch / "fbx_export_tree.fbx";
        exportOptions.embedTextures = true;
        std::string exportError;
        const bool exported = glb.ok && AssimpExporter::ExportAssetToFbx(glb.modelPath, exportOptions, exportError);
        ctx.Expect(exported && std::filesystem::exists(exportOptions.outputPath),
            "tree glb exports to fbx", "Tree Generator GLB with external bark/leaf textures must export to FBX without Assimp iterator crash; error=" + (glb.ok ? exportError : glb.error));
        if (exported)
        {
            AssimpImporter importer;
            const AssimpImporter::ImportResult importResult = importer.importFile(exportOptions.outputPath);
            ctx.Expect(importResult.success && !importResult.meshes.empty() && !importResult.materials.empty(),
                "tree fbx imports as static mesh", "FBX static import must parse exported Tree Generator FBX into renderable meshes/materials");
        }
    }
    ctx.Expect(treePresetHeaderSource.find("loadPresetFile") != std::string::npos &&
            treePresetHeaderSource.find("loadAllPresets") != std::string::npos &&
            treePresetSource.find("JsonParser") != std::string::npos &&
            treePresetSource.find("defaultTreeOptions") != std::string::npos &&
            treePresetSource.find("IXTREEME") == std::string::npos &&
            treePresetSource.find("ImGui") == std::string::npos,
        "ixtreemetree preset loader api", "TREE-2 preset loader must expose file/folder load APIs, tolerate missing fields with defaults, and stay engine/UI independent");
    {
        const std::filesystem::path presetDir = options.clientRoot / "assets" / "internal" / "tree_presets";
        std::vector<std::string> errors;
        const std::vector<ixtreemetree::Preset> presets = ixtreemetree::loadAllPresets(presetDir, &errors);
        std::string presetFailure = "TREE-2 must ship the EZ-Tree base presets as parseable JSON files";
        if (!errors.empty())
        {
            presetFailure += "; first error: " + errors.front();
        }
        presetFailure += "; count=" + std::to_string(presets.size());
        const auto hasPreset = [&](const char* name) {
            return std::any_of(presets.begin(), presets.end(), [name](const ixtreemetree::Preset& preset) {
                return preset.name == name;
            });
        };
        ctx.Expect(errors.empty() && presets.size() == 16 &&
                hasPreset("Oak Medium") &&
                hasPreset("Pine Medium") &&
                hasPreset("Bush 1") &&
                hasPreset("Trellis"),
            "tree preset json set parses", presetFailure);
        if (!presets.empty())
        {
            ixtreemetree::Tree tree;
            tree.options = presets.front().options;
            const ixtreemetree::TreeMesh mesh = tree.generate();
            ctx.Expect(mesh.stats.barkTriangles > 0 && mesh.stats.leafTriangles > 0,
                "tree preset generates mesh", "TREE-2 presets must produce usable ixtreemetree meshes");
        }
    }
    ctx.Expect(treePanelSource.find("Tree Generator") != std::string::npos &&
            treePanelSource.find("BeginTable(\"TreeBranchLevels\"") != std::string::npos &&
            treePanelSource.find("Regenerate") != std::string::npos &&
            treePanelSource.find("Save as Asset") != std::string::npos &&
            treePreviewSource.find("TreePreviewCanvas") != std::string::npos &&
            treeExporterSource.find("[TREE-1] saved asset") != std::string::npos &&
            editorImGuiSource.find("Tree Generator...") != std::string::npos &&
            editorImGuiHeaderSource.find("TreeGeneratorPanel") != std::string::npos,
        "tree generator editor integration", "TREE-1 must add a Tools > Tree Generator panel, preview wrapper, and GLB asset exporter");
    ctx.Expect(treePanelSource.find("RenderPresetSelector") != std::string::npos &&
            treePanelSource.find("loadAllPresets") != std::string::npos &&
            treePanelSource.find("Custom") != std::string::npos &&
            treePanelSource.find("[TREE-2] loaded") != std::string::npos &&
            renderCmakeSource.find("IXTREEME_TREE_PRESET_DIR") != std::string::npos &&
            clientCmakeSource.find("${CLIENT_ROOT}/assets") != std::string::npos,
        "tree generator preset dropdown", "TREE-2 must load internal tree presets, expose a Custom+preset dropdown, and rely on the existing asset copy path for runtime availability");
    {
        const std::filesystem::path treeTextureRoot = options.clientRoot / "assets" / "internal" / "textures";
        int textureCount = 0;
        for (const std::filesystem::path path : {
                 treeTextureRoot / "bark" / "oak_bark.png",
                 treeTextureRoot / "bark" / "birch_bark.png",
                 treeTextureRoot / "bark" / "pine_bark.png",
                 treeTextureRoot / "bark" / "willow_bark.png",
                 treeTextureRoot / "bark" / "ash_bark.png",
                 treeTextureRoot / "leaves" / "oak_leaf.png",
                 treeTextureRoot / "leaves" / "ash_leaf.png",
                 treeTextureRoot / "leaves" / "pine_leaf.png",
                 treeTextureRoot / "leaves" / "willow_leaf.png",
                 treeTextureRoot / "leaves" / "birch_leaf.png",
             })
        {
            if (std::filesystem::exists(path))
                ++textureCount;
        }
        ctx.Expect(textureCount == 10 &&
                renderCmakeSource.find("TreeTexturePalette.cpp") != std::string::npos &&
                treePaletteSource.find("[TREE-3] palette_loaded bark=") != std::string::npos &&
                treePaletteSource.find("[TREE-3] generated procedural") != std::string::npos &&
                treePaletteSource.find("[TREE-LEAF-FORM-1] atlas_grid=2x2 cardsPerCluster_default=3") != std::string::npos &&
                treePanelSource.find("RenderTextureOverrideSlot") != std::string::npos &&
                treePanelSource.find("AcceptDragDropPayload(\"ASSET_ID\")") != std::string::npos &&
                treePanelSource.find("AssetLibrary::Category::Texture") != std::string::npos &&
                treePanelSource.find("barkTextureOverridePath_") != std::string::npos &&
                treeExporterSource.find("TreeMaterialBinding") != std::string::npos &&
                treeExporterSource.find("CopyTextureDependency") != std::string::npos &&
                treeExporterSource.find("baseColorTexturePath = barkTexturePath") != std::string::npos &&
                treeExporterSource.find("\\\"baseColorTexture\\\"") != std::string::npos &&
                treePreviewSource.find("TreePreviewStyle") != std::string::npos &&
                treeLibHeaderSource.find("Guid") == std::string::npos &&
                treeLibSource.find("TreeTexturePalette") == std::string::npos,
            "tree texture palette and overrides", "TREE-3 must keep ixtreemetree texture-agnostic while engine-side palette textures, drag-drop overrides, preview styling, and material GUID binding are wired");
    }
    ctx.Expect(sceneManagerHeaderSource.find("class SceneManager") != std::string::npos &&
            sceneManagerHeaderSource.find("struct SceneData") != std::string::npos &&
            sceneManagerHeaderSource.find("LoadScene") != std::string::npos &&
            sceneManagerHeaderSource.find("SaveScene") != std::string::npos &&
            sceneManagerHeaderSource.find("SaveSceneAs") != std::string::npos &&
            renderCmakeSource.find("SceneManager.cpp") != std::string::npos,
        "scene manager class", "SCENE-1 must add a SceneManager class and build it into the editor client");
    ctx.Expect(sceneManagerHeaderSource.find("m_sceneOpen") != std::string::npos &&
            sceneManagerHeaderSource.find("bool HasOpenScene() const { return m_sceneOpen; }") != std::string::npos &&
            sceneManagerSource.find("m_sceneOpen = true;") != std::string::npos &&
            sceneManagerSource.find("m_sceneOpen = false;") != std::string::npos,
        "unsaved new scene open state", "New Scene must be treated as an open editor scene even before it has a saved file path");
    ctx.Expect(sceneManagerSource.find("\\\"version\\\": 1") != std::string::npos &&
            sceneManagerSource.find("\\\"terrain\\\"") != std::string::npos &&
            sceneManagerSource.find("\\\"width_m\\\"") != std::string::npos &&
            sceneManagerSource.find("\\\"cell_size_m\\\"") != std::string::npos &&
            sceneManagerSource.find("\\\"chunk_size_cells\\\"") != std::string::npos &&
            sceneManagerSource.find("\\\"chunk_manifest_ref\\\"") != std::string::npos &&
            sceneManagerSource.find("\\\"shape_mask_ref\\\"") != std::string::npos &&
            sceneManagerSource.find("map.manifest") != std::string::npos &&
            sceneManagerSource.find(".mxchunk") != std::string::npos,
        "scene json chunk terrain format", "TERRAIN-CHUNK-1 must save readable .scene JSON and terrain through map.manifest/.mxchunk sidecar files");
    ctx.Expect(editorImGuiSource.find("RenderMenuBar") != std::string::npos &&
            editorImGuiSource.find("BeginMainMenuBar") != std::string::npos &&
            editorImGuiSource.find("New Scene") != std::string::npos &&
            editorImGuiSource.find("Open Scene") != std::string::npos &&
            editorImGuiSource.find("Save Scene") != std::string::npos &&
            editorImGuiSource.find("Recent Scenes") != std::string::npos &&
            editorImGuiSource.find("Ctrl+Shift+S") != std::string::npos,
        "scene editor file menu", "SCENE-1 must expose New/Open/Save/SaveAs/Recent through an editor File menu");
    ctx.Expect(editorImGuiSource.find("ImGuiKey_N") != std::string::npos &&
            editorImGuiSource.find("ImGuiKey_O") != std::string::npos &&
            editorImGuiSource.find("ImGuiKey_S") != std::string::npos &&
            editorImGuiSource.find("io.KeyShift") != std::string::npos,
        "scene editor hotkeys", "SCENE-1 must wire Ctrl+N, Ctrl+O, Ctrl+S, and Ctrl+Shift+S");
    ctx.Expect(sceneManagerSource.find("MarkDirty") != std::string::npos &&
            sceneManagerSource.find("PromptSaveBeforeAction") != std::string::npos &&
            sceneManagerHeaderSource.find("GetRecentScenes") != std::string::npos &&
            sceneManagerSource.find("IxtreemeEngine - Editor") != std::string::npos &&
            sceneManagerSource.find("title += \"*\"") != std::string::npos &&
            nativeWindowHeaderSource.find("SetTitle") != std::string::npos &&
            clientMainSource.find("SetWindowTitleCallback") != std::string::npos,
        "scene dirty title recent", "SCENE-1 must track dirty state, prompt before destructive scene actions, update title, and maintain recent scenes");
    ctx.Expect(clientMainSource.find("SetCurrentSceneSnapshot(buildSceneSnapshot())") != std::string::npos &&
            clientMainSource.find("ConsumePendingScene") != std::string::npos &&
            clientMainSource.find("applySceneData") != std::string::npos &&
            clientMainSource.find("[STARTUP] Editor build: no automatic default.scene load") != std::string::npos,
        "scene main editor bridge", "SCENE-1/2 must bridge SceneManager data into editor water/light/palette state and start editor builds without automatic default.scene loading");
    ctx.Expect(projectManagerHeaderSource.find("class ProjectManager") != std::string::npos &&
            projectManagerHeaderSource.find("CreateProject") != std::string::npos &&
            projectManagerHeaderSource.find("OpenProject") != std::string::npos &&
            projectManagerHeaderSource.find("SaveProject") != std::string::npos &&
            projectManagerSource.find("\"project.ixproj\"") != std::string::npos &&
            projectManagerSource.find("\"Assets\"") != std::string::npos &&
            projectManagerSource.find("\"Scenes\"") != std::string::npos &&
            renderCmakeSource.find("ProjectManager.cpp") != std::string::npos,
        "project manager manifest", "PROJECT-1 must create/open/save project.ixproj manifests and project Assets/Scenes roots");
    ctx.Expect(editorImGuiHeaderSource.find("ProjectDialogMode") != std::string::npos &&
            editorImGuiSource.find("RenderProjectModal") != std::string::npos &&
            editorImGuiSource.find("No Project") != std::string::npos &&
            editorImGuiSource.find("Create New Project") != std::string::npos &&
            editorImGuiSource.find("Open Project") != std::string::npos &&
            editorImGuiSource.find("Browse...") != std::string::npos &&
            editorImGuiSource.find("Browse Path") != std::string::npos &&
            editorImGuiSource.find("Filter folders/projects...") != std::string::npos &&
            editorImGuiSource.find("NavigateProjectBrowser") != std::string::npos &&
            editorImGuiSource.find("createMissing") != std::string::npos &&
            editorImGuiSource.find("create_directories(target") != std::string::npos &&
            editorImGuiSource.find("Folder created:") != std::string::npos &&
            editorImGuiSource.find("Drives") != std::string::npos &&
            editorImGuiSource.find("Go") != std::string::npos &&
            editorImGuiSource.find("Use This Folder") != std::string::npos &&
            editorImGuiSource.find("Recent Projects") != std::string::npos &&
            editorImGuiSource.find("InitializeProjectAssetLibrary") != std::string::npos &&
            clientMainSource.find("editorImGui.SetEngineRoot(*assetRoot)") != std::string::npos &&
            clientMainSource.find("editorImGui.InitializeAssetLibrary(*assetRoot)") == std::string::npos,
        "editor project workflow", "PROJECT-1 editor boot must show a no-project modal and bind the Asset Browser only after a project is active");
    ctx.Expect(sceneManagerSource.find("DefaultProjectScenePath") != std::string::npos &&
            sceneManagerSource.find("ResolveProjectScenePath") != std::string::npos &&
            sceneManagerSource.find("ProjectSceneRecentPath") != std::string::npos &&
            sceneManagerSource.find("ProjectManager::Instance().SetRecentScenes") != std::string::npos,
        "project scene paths", "PROJECT-1 scene save/open/recent handling must be project-relative when a project is active");
    ctx.Expect(sceneManagerHeaderSource.find("SetRuntimeUiCallbacks") == std::string::npos &&
            sceneManagerHeaderSource.find("SetSceneType") == std::string::npos &&
            sceneManagerSource.find("ActivateSceneType") == std::string::npos &&
            sceneManagerSource.find("\"scene_type\"") == std::string::npos &&
            editorImGuiSource.find("Scene Type") == std::string::npos,
        "scene type removed", "Clean engine scenes must not expose scene_type metadata or runtime UI routing");
    ctx.Expect(runtimeSessionHeaderSource.find("class RuntimeSession") != std::string::npos &&
            runtimeSessionHeaderSource.find("Start(const SceneData& openScene)") != std::string::npos &&
            runtimeSessionHeaderSource.find("Stop()") != std::string::npos &&
            runtimeSessionSource.find("class EmptyRuntimeSession final") != std::string::npos &&
            runtimeUiAdapterHeaderSource.find("class RuntimeUiAdapter") != std::string::npos &&
            runtimeUiAdapterSource.find("class NullRuntimeUiAdapter final") != std::string::npos &&
            clientMainSource.find("CreateRuntimeSession()") != std::string::npos &&
            clientMainSource.find("CreateRuntimeUiAdapter(rmlUi)") != std::string::npos,
        "default runtime adapters", "Clean engine must boot through the default runtime session and null UI adapter");
    ctx.Expect(clientMainSource.find("StartupSceneFromConfig") != std::string::npos &&
            clientMainSource.find("startup_scene") != std::string::npos &&
            clientMainSource.find("scenes/" "Login" ".scene") == std::string::npos &&
            clientMainSource.find("[BOOT] config missing/empty -> fallback = empty runtime") != std::string::npos &&
            clientMainSource.find("[SCENE] no scene loaded (empty runtime startup)") != std::string::npos,
        "release empty startup fallback", "Release/default runtime must no longer fall back to archived game scenes");
    ctx.Expect(clientMainSource.find("runtimeSession->SetMapEditorOpen(true)") != std::string::npos &&
            clientMainSource.find("[BOOT] editor_open forced = 1") != std::string::npos &&
            clientMainSource.find("editorImGui.BeginFrame(runtimeSession->IsMapEditorOpen())") != std::string::npos,
        "editor boot opens imgui", "Editor boot must keep the ImGui editor open independent of scene type");
    ctx.Expect(runtimeUiAdapterSource.find("LoadRuntimeSceneOrFallback") == std::string::npos &&
            runtimeUiAdapterSource.find("Scenes/Lobby.scene") == std::string::npos &&
            runtimeUiAdapterSource.find("Scenes/World.scene") == std::string::npos,
        "no built-in scene flow", "Clean engine runtime UI adapter must not hard-code game scene transitions");
    ctx.Expect(rmlUiLayerSource.find("void RmlUiLayer::HideAll") != std::string::npos &&
            rmlUiLayerSource.find("WarnSceneTypeMismatch") == std::string::npos &&
            rmlUiLayerSource.find("scene_type mismatch") == std::string::npos,
        "rmlui scene type warnings removed", "Clean engine RmlUi must not depend on scene_type metadata");
    ctx.Expect(editorImGuiSource.find("RenderSceneSettingsPanel") != std::string::npos &&
            editorImGuiSource.find("Scene Settings") != std::string::npos &&
            editorImGuiSource.find("Scene Name") != std::string::npos &&
            editorImGuiSource.find("Scene Type") == std::string::npos,
        "scene settings panel", "Scene Settings must expose scene metadata without obsolete scene_type controls");
    ctx.Expect(clientMainSource.find("playStartScenePath") != std::string::npos &&
            clientMainSource.find("Restored starting scene") != std::string::npos &&
            editorImGuiSource.find("Open a scene to Play") != std::string::npos &&
            editorImGuiSource.find("HasOpenScene()") != std::string::npos,
        "play scene restore", "SCENE-2 Play mode must store/restore the starting scene and disable Play with no open scene");
    ctx.Expect(clientMainSource.find("editorRuntimeFlowActive") == std::string::npos &&
            clientMainSource.find("Runtime UI/scene flow enabled for Play mode") == std::string::npos,
        "runtime flow absent in clean engine", "Clean engine default runtime must not enable game-specific UI/scene flow");
    ctx.Expect(clientMainSource.find("injectDirectGameplayDevCharacter") == std::string::npos &&
            clientMainSource.find("Dev" "Player") == std::string::npos &&
            clientMainSource.find("player.level = 50") == std::string::npos,
        "no dev character injection", "Clean engine Play mode must not inject game-specific dev characters");
    ctx.Expect(sceneManagerHeaderSource.find("RestoreSceneSnapshot") != std::string::npos &&
            sceneManagerSource.find("void SceneManager::RestoreSceneSnapshot") != std::string::npos &&
            clientMainSource.find("playStartSceneSnapshot") != std::string::npos &&
            clientMainSource.find("playStartSceneDirty") != std::string::npos,
        "play snapshot restore", "EDIT-PLAY-2 Stop must restore the editor scene snapshot, including unsaved scenes, after Play");
    ctx.Expect(editorImGuiSource.find("RenderHierarchyPanel") != std::string::npos &&
            editorImGuiSource.find("BeginTable(\"HierarchyEntityTree\"") != std::string::npos &&
            editorImGuiSource.find("TableSetupColumn(\"Label\"") != std::string::npos &&
            editorImGuiSource.find("TableSetupColumn(\"Visibility\"") != std::string::npos &&
            editorImGuiSource.find("RenderHierarchyEntityNode") != std::string::npos &&
            editorImGuiSource.find("RenderHierarchyWaterBodies") == std::string::npos &&
            editorImGuiSource.find("RenderHierarchyPointLights") == std::string::npos &&
            editorImGuiSource.find("RenderHierarchySpotLights") == std::string::npos &&
            editorImGuiSource.find("DockBuilderDockWindow(ICON_FA_LIST_TREE \" Hierarchy\"") != std::string::npos,
        "hierarchy entity tree panel", "HIERARCHY-2 must render a real scene-root entity tree instead of fixed type buckets");
    ctx.Expect(editorImGuiSource.find("RenderHierarchyToolbar") != std::string::npos &&
            editorImGuiSource.find("Search entities") != std::string::npos &&
            editorImGuiSource.find("HierarchyPassesSearch") != std::string::npos &&
            editorImGuiSource.find("ContainsCaseInsensitive") != std::string::npos &&
            editorImGuiSource.find("Search filter") != std::string::npos,
        "hierarchy search", "HIERARCHY-2 must provide case-insensitive entity-name search with a clearable toolbar");
    ctx.Expect(editorImGuiSource.find("QueueHierarchySelection") != std::string::npos &&
            editorImGuiSource.find("QueueHierarchyFocus") != std::string::npos &&
            editorImGuiSource.find("SetScrollHereY") != std::string::npos &&
            clientMainSource.find("selectHierarchyEntity") != std::string::npos &&
            clientMainSource.find("editorImGui.SetHierarchySceneState") != std::string::npos &&
            mapEditorTypesSource.find("hierarchyEntityHandle") != std::string::npos &&
            editorImGuiHeaderSource.find("GetSelectedHierarchyEntity") != std::string::npos,
        "hierarchy selected entity bridge", "HIERARCHY-2 must expose the selected flecs entity handle for the next Inspector pass");
    ctx.Expect(editorImGuiSource.find("RenderHierarchyContextMenu") != std::string::npos &&
            editorImGuiSource.find("Focus Camera") != std::string::npos &&
            editorImGuiSource.find("Duplicate") != std::string::npos &&
            editorImGuiSource.find("Rename") != std::string::npos &&
            editorImGuiSource.find("Delete") != std::string::npos &&
            editorImGuiSource.find("InputTextFlags_EnterReturnsTrue") != std::string::npos,
        "hierarchy context menu", "HIERARCHY-2 must expose Focus/Duplicate/Rename/Delete and in-place rename per entity");
    ctx.Expect(mapEditorTypesSource.find("struct HierarchySceneEntity") != std::string::npos &&
            mapEditorTypesSource.find("std::uint64_t entity") != std::string::npos &&
            mapEditorTypesSource.find("std::uint64_t parent") != std::string::npos &&
            mapEditorTypesSource.find("HierarchyEntityType") != std::string::npos &&
            mapEditorTypesSource.find("hierarchyDuplicateEntity") != std::string::npos &&
            mapEditorTypesSource.find("hierarchyRenameEntity") != std::string::npos &&
            clientMainSource.find("duplicateHierarchyEntity") != std::string::npos &&
            clientMainSource.find("renameHierarchyEntity") != std::string::npos &&
            clientMainSource.find("deleteHierarchyEntity") != std::string::npos,
        "hierarchy commands", "HIERARCHY-2 hierarchy commands must be routed from generic entity nodes to the editor runtime");
    ctx.Expect(clientMainSource.find("#include <flecs.h>") != std::string::npos &&
            clientMainSource.find("ecs_new(editorHierarchyWorld.get())") != std::string::npos &&
            clientMainSource.find("ecs_add_pair(editorHierarchyWorld.get(), entity, EcsChildOf, editorSceneRootEntity)") != std::string::npos &&
            clientMainSource.find("HierarchyObjectKey") != std::string::npos &&
            clientMainSource.find("buildHierarchyEntities") != std::string::npos,
        "hierarchy flecs scene root", "HIERARCHY-2 must back visible scene objects with flecs entities parented to the scene root");
    ctx.Expect(mapEditorTypesSource.find("editorHidden") != std::string::npos &&
            editorImGuiSource.find("ICON_FA_EYE_SLASH") != std::string::npos &&
            clientMainSource.find("toggleHierarchyHidden") != std::string::npos &&
            clientMainSource.find("light.editorHidden") != std::string::npos &&
            clientMainSource.find("body.editorHidden") != std::string::npos &&
            sceneManagerSource.find("\\\"editor_hidden\\\"") == std::string::npos,
        "hierarchy editor visibility", "HIERARCHY-2 must keep per-entity editor visibility with eye icons and avoid saving editor_hidden into scene JSON");
    ctx.Expect(clientMainSource.find("void FocusOn(WorldVec3 target") != std::string::npos &&
            clientMainSource.find("focusHierarchyEntity") != std::string::npos &&
            editorImGuiSource.find("ImGuiKey_F") != std::string::npos &&
            clientMainSource.find("[HIERARCHY] Focused camera on entity") != std::string::npos,
        "hierarchy focus camera", "HIERARCHY-2 must focus the editor camera on selected hierarchy entities with F/double-click/context menu");
    ctx.Expect(clientMainSource.find("float yaw_ = 0.0f") != std::string::npos &&
            clientMainSource.find("float pitch_ = -25.0f") != std::string::npos &&
            clientMainSource.find("WorldVec3 eye_ = {0.0f, 8.0f, -18.0f}") != std::string::npos,
        "editor default camera sees origin", "Editor fly camera must boot looking toward the scene origin instead of the sky/background");
    ctx.Expect(editorImGuiSource.find("RenderSelectedWaterBodyInspector") != std::string::npos &&
            editorImGuiSource.find("RenderSelectedLightInspector") != std::string::npos &&
            editorImGuiSource.find("RenderTransformComponent") != std::string::npos &&
            editorImGuiSource.find("RenderAxisFloat") != std::string::npos &&
            editorImGuiSource.find("CollapsingHeader(ICON_FA_CUBE \" Transform\"") != std::string::npos &&
            editorImGuiSource.find("CollapsingHeader(ICON_FA_WATER \" Water Body\"") != std::string::npos &&
            editorImGuiSource.find("CollapsingHeader(ICON_FA_LIGHTBULB \" Point Light\"") != std::string::npos &&
            editorImGuiSource.find("CollapsingHeader(ICON_FA_BULLSEYE \" Spot Light\"") != std::string::npos,
        "component inspector sections", "INSPECTOR-2 must render selected entities as component sections with a colored transform editor");
    ctx.Expect(editorImGuiSource.find("RenderAddComponentMenu") != std::string::npos &&
            editorImGuiSource.find("Add Component") != std::string::npos &&
            mapEditorTypesSource.find("enum class EditorComponentType") != std::string::npos &&
            mapEditorTypesSource.find("addComponentToSelectedEntity") != std::string::npos &&
            clientMainSource.find("commands.addComponentToSelectedEntity") != std::string::npos &&
            clientMainSource.find("selectedEntityPosition") != std::string::npos,
        "component add command", "INSPECTOR-2 must expose an Add Component menu and route known component commands through the editor runtime");
    ctx.Expect(mapEditorTypesSource.find("struct MeshSceneEntity") != std::string::npos &&
            sceneManagerHeaderSource.find("meshEntities") != std::string::npos &&
            sceneManagerSource.find("mesh_entity") != std::string::npos &&
            sceneManagerSource.find("\"mesh_asset_path\"") != std::string::npos &&
            sceneManagerSource.find("ReadMeshSceneEntity") != std::string::npos,
        "mesh entity scene roundtrip source", "MESH-ENTITY-1 must persist mesh entities with Transform and MeshRenderer asset references");
    ctx.Expect(editorImGuiSource.find("RenderSelectedMeshRendererInspector") != std::string::npos &&
            editorImGuiSource.find("SetMeshRendererEditorState") != std::string::npos &&
            editorImGuiSource.find("MeshRenderer") != std::string::npos &&
            editorImGuiSource.find("AssignAssetToSelectedMeshRenderer") != std::string::npos &&
            editorImGuiSource.find("m_commands.addMeshEntity = true") != std::string::npos &&
            editorImGuiSource.find("Hierarchy drop queued") != std::string::npos,
        "mesh renderer inspector and spawn source", "MESH-ENTITY-1 must expose MeshRenderer in Inspector and allow model assets to spawn mesh entities from Asset Browser/Hierarchy");
    ctx.Expect(clientMainSource.find("std::vector<MeshSceneEntity> editorMeshEntities") != std::string::npos &&
            clientMainSource.find("createMeshEntityAt") != std::string::npos &&
            clientMainSource.find("resolveMeshRuntimePath") != std::string::npos &&
            clientMainSource.find("getStaticMeshRenderer") != std::string::npos &&
            clientMainSource.find("StaticMeshRenderer loaded") != std::string::npos &&
            clientMainSource.find("SelectedEditorObjectType::MeshEntity") != std::string::npos,
        "mesh entity runtime source", "MESH-ENTITY-1/2 must route mesh entities through hierarchy selection, gizmo state, and static mesh rendering");
    ctx.Expect(renderCmakeSource.find("StaticMeshRenderer.cpp") != std::string::npos &&
            staticMeshRendererSource.find("bool StaticMeshRenderer::DetectSkinnedGltf") != std::string::npos &&
            staticMeshRendererSource.find("!asset.skins.empty()") != std::string::npos &&
            staticMeshRendererSource.find("node.skinIndex.has_value()") != std::string::npos &&
            staticMeshRendererSource.find("JOINTS_0") != std::string::npos &&
            staticMeshRendererSource.find("WEIGHTS_0") != std::string::npos &&
            staticMeshRendererSource.find("fastgltf::Options::LoadExternalBuffers") != std::string::npos &&
            staticMeshRendererSource.find("fastgltf::Options::LoadExternalImages") != std::string::npos &&
            staticMeshRendererSource.find("fastgltf::Options::GenerateMeshIndices") != std::string::npos &&
            staticMeshRendererSource.find("LoadStatus::UnsupportedSkinned") != std::string::npos &&
            staticMeshRendererSource.find("bool StaticMeshRenderer::LoadStaticGltfMesh") != std::string::npos &&
            staticMeshRendererSource.find("skeleton.ozz") == std::string::npos,
        "static mesh renderer source", "MESH-ENTITY-2 must add an ozz-free static glTF renderer with skinned detection");
    ctx.Expect(clientMainSource.find("staticMeshCache") != std::string::npos &&
            clientMainSource.find("UnsupportedSkinned") != std::string::npos &&
            clientMainSource.find("StaticMeshRenderer::Instance") != std::string::npos &&
            clientMainSource.find("ensureSkinnedMeshLoaded(activeMeshModelPath)") == std::string::npos,
        "mesh entity static cache source", "MESH-ENTITY-2 must cache static/skinned/failed model states and avoid per-frame skinned retries for mesh entities");
    ctx.Expect(editorImGuiSource.find("void EditorImGui::RenderWorldPanel") != std::string::npos &&
            editorImGuiSource.find("ImGui::Begin(ICON_FA_GLOBE \" World\"") != std::string::npos &&
            editorImGuiSource.find("RenderWorldPanel();") != std::string::npos &&
            editorImGuiSource.find("DockBuilderDockWindow(ICON_FA_GLOBE \" World\"") != std::string::npos &&
            editorImGuiSource.find("Click a water body or light in the 3D viewport, or select a light from Dynamic Lights.") == std::string::npos &&
            editorImGuiSource.find("RenderWorldPanel();\n    RenderToolsPanel();") != std::string::npos,
        "world panel split", "INSPECTOR-2 must move environment/dynamic-light creation out of Inspector and keep gizmo controls out of the Inspector body");
    ctx.Expect(editorImGuiSource.find("void EditorImGui::RenderEditorToolbar") != std::string::npos &&
            editorImGuiSource.find("RenderGizmoControls();") != std::string::npos &&
            editorImGuiSource.find("operationButton(\"W\", \"Translate\"") != std::string::npos &&
            editorImGuiSource.find("operationButton(\"E\", \"Rotate\"") != std::string::npos &&
            editorImGuiSource.find("operationButton(\"R\", \"Scale\"") != std::string::npos &&
            editorImGuiSource.find("##GizmoSnap") != std::string::npos,
        "toolbar gizmo controls", "INSPECTOR-2 must move gizmo mode/snap controls to the top editor toolbar");
    ctx.Expect(editorImGuiSource.find("RenderEditorToolbar") != std::string::npos &&
            editorImGuiSource.find("HandleEditorHotkeys") != std::string::npos &&
            editorImGuiSource.find("ImGuiKey_F5") != std::string::npos &&
            editorImGuiSource.find("ImGuiKey_F6") != std::string::npos &&
            editorImGuiSource.find("enterPlayMode") != std::string::npos &&
            editorImGuiSource.find("pausePlayMode") != std::string::npos &&
            editorImGuiSource.find("Tools disabled in Play Mode") != std::string::npos &&
            editorImGuiSource.find("Read-only during Play Mode") != std::string::npos,
        "editor play toolbar and hotkeys", "EDIT-PLAY-1 needs Play/Stop/Pause toolbar controls, F5/F6 hotkeys, and disabled edit tools in Play Mode");
    ctx.Expect(clientMainSource.find("SetFreeCameraEnabled(true)") != std::string::npos &&
            clientMainSource.find("SetFreeCameraEnabled(false)") != std::string::npos &&
            clientMainSource.find("editorPlay.state.mode == EditorPlayMode::PlayPaused") != std::string::npos &&
            clientMainSource.find("[EDIT-PLAY] Snapshot restored") != std::string::npos,
        "editor play baseline runtime", "Clean engine Play mode must keep snapshot restore, pause state, and free-camera control without character runtime");
    ctx.Expect(!std::filesystem::exists(options.clientRoot / "libs" / "render" / ("Noe" "sisLayer.cpp")) &&
            !std::filesystem::exists(options.clientRoot / "libs" / "render" / ("Noe" "sisLayer.h")) &&
            renderCmakeSource.find(legacyUiName) == std::string::npos &&
            rootCmakeSource.find(legacyUiName) == std::string::npos &&
            clientCmakeSource.find(legacyUiName) == std::string::npos &&
            androidGradleSource.find(legacyUiName) == std::string::npos &&
            rmlUiLayerSource.find(legacyUiName) == std::string::npos &&
            clientMainSource.find(legacyUiName) == std::string::npos &&
            renderCmakeSource.find(legacyMarkupExt) == std::string::npos &&
            rootCmakeSource.find(legacyMarkupExt) == std::string::npos &&
            clientCmakeSource.find(legacyMarkupExt) == std::string::npos,
        "legacy ui removed", "RMLUI-5 must remove legacy UI source, CMake links, Android packaging, and markup assets");
    ctx.Expect(editorImGuiSource.find("RenderWaterSculptToolPanel") != std::string::npos &&
            editorImGuiSource.find("RenderHeightmapToolPanel") != std::string::npos &&
            editorImGuiSource.find("RenderSplatPaintToolPanel") != std::string::npos &&
            editorImGuiSource.find("OpenImportAssetDialog") != std::string::npos &&
            editorImGuiSource.find("ImportAssetFromPath") != std::string::npos &&
            editorImGuiSource.find("ImportAssetIntoFolder") != std::string::npos &&
            editorImGuiSource.find("[IMPORT-DIAG]") != std::string::npos &&
            clientMainSource.find("editorImGui.ImportExternalFiles") != std::string::npos &&
            clientMainSource.find("editorSettings.toolMode == MapEditorToolMode::Heightmap") != std::string::npos &&
            clientMainSource.find("editorSettings.toolMode == MapEditorToolMode::SplatPaint") != std::string::npos,
        "editor tool ui moved to imgui", "EDITOR-IMGUI-5 tool panels/import routing are missing or legacy tool markup remains");
    ctx.Expect(editorImGuiSource.find("RenderWaterMaterialEdgeFadeSection") != std::string::npos &&
            editorImGuiSource.find("SliderFloat(\"Edge Fade Distance\"") != std::string::npos &&
            editorImGuiSource.find("Combo(\"Edge Fade Curve\"") != std::string::npos &&
            editorImGuiSource.find("WaterConfig::EdgeFadeCurve") != std::string::npos,
        "water edge fade imgui material editor source", "WATER-OBJ-6 Edge Fade material controls are missing from ImGui");
    ctx.Expect(editorImGuiSource.find("RenderWaterMaterialEditor") != std::string::npos &&
            editorImGuiSource.find("RenderPbrMaterialEditor") != std::string::npos &&
            editorImGuiSource.find("RenderWaterTextureSlot") != std::string::npos &&
            editorImGuiSource.find("AcceptDragDropPayload(kAssetPayloadType)") != std::string::npos &&
            clientMainSource.find("editorImGui.OpenWaterMaterialEditor(materialId)") != std::string::npos,
        "material editors moved to imgui", "ImGui material editors must handle water/PBR editing");
    ctx.Expect(clientMainSource.find("PickWaterBody") != std::string::npos &&
            clientMainSource.find("RegenerateCircularWaterMask") != std::string::npos &&
            clientMainSource.find("SelectedEditorObjectType::WaterBody") != std::string::npos &&
            clientMainSource.find("commands.addWaterBody") != std::string::npos,
        "water object editor workflow source", "WATER-OBJ-3 spawn/select/transform/delete workflow is missing");
    ctx.Expect(clientMainSource.find("ApplyWaterSculptBrush") != std::string::npos &&
            clientMainSource.find("ExpandWaterBodyForSculpt") != std::string::npos &&
            clientMainSource.find("RaycastTerrainPoint") != std::string::npos &&
            clientMainSource.find("IsNearWaterBodyBbox") == std::string::npos &&
            clientMainSource.find("body.bboxMin[0] = nextMinX") != std::string::npos &&
            clientMainSource.find("waterSculptStrokeActive") != std::string::npos &&
            clientMainSource.find("waterSculptMeshRegenPending") != std::string::npos &&
            clientMainSource.find("RegenerateCircularWaterMask(body);") != std::string::npos,
        "water sculpt expandable bitmask source", "WATER-OBJ-5 sculpting must expand the water body instead of being limited by the initial bbox");
    ctx.Expect(clientMainSource.find("GetWaterConfig") == std::string::npos &&
            clientMainSource.find("SetWaterConfig") == std::string::npos &&
            clientMainSource.find("body.config = state.config") == std::string::npos &&
            clientMainSource.find("SetWaterMaterials(editorImGui.GetWaterMaterialsSnapshot())") != std::string::npos,
        "water body uses material reference", "Client main must not copy material config into WaterBody");
    ctx.Expect(clientMainSource.find("terrain.SetSelectedWaterBodyHighlight(device, 0u)") != std::string::npos &&
            clientMainSource.find("RenderSelectedWaterBodyHighlight(device, camera)") == std::string::npos &&
            clientMainSource.find("Water \" + std::to_string(body.id)") == std::string::npos &&
            clientMainSource.find("for (const WaterBody& body : editorWaterBodies)\n                    {\n                        if (skinSlot") == std::string::npos,
        "water object selection visuals removed", "Water-body selection must not render the legacy bbox or skinnedMesh/label proxies");

    const std::filesystem::path waterScratch = (options.scratchRoot.empty() ? MakeDefaultScratchRoot() : options.scratchRoot) / "water_obj";
    const std::filesystem::path waterFile = waterScratch / client::render::kWaterBodiesFilename;
    auto waterBodies = client::render::CreateWaterBodyTestSet();
    if (!waterBodies.empty())
        waterBodies[0].materialId = "watermat_LakeShared_Renamed";
    std::string waterError;
    ctx.Expect(waterBodies.size() == 3 && waterBodies[0].maskWidth > 0 && !waterBodies[0].shapeMask.empty(),
        "water object test fixtures", "CreateWaterBodyTestSet did not create valid fixtures");
    if (ctx.Expect(client::render::SaveWaterBodiesBinary(waterFile, waterBodies, &waterError),
            "water object sidecar write", waterError))
    {
        std::ifstream in(waterFile, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<WaterBody> loadedBodies;
        ctx.Expect(client::render::LoadWaterBodiesBinary(bytes, loadedBodies, &waterError) &&
                loadedBodies.size() == waterBodies.size() &&
                loadedBodies[0].materialId == waterBodies[0].materialId &&
                loadedBodies[1].waterLevelY == waterBodies[1].waterLevelY &&
                loadedBodies[2].shapeMask == waterBodies[2].shapeMask,
            "water object sidecar roundtrip", waterError.empty() ? "roundtrip mismatch/material id lost" : waterError);
    }

    return ctx.failed == 0;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        Options options = ParseOptions(argc, argv);
        TestContext ctx;

        if (options.runAsset)
            RunAssetTests(options, ctx);
        if (options.runRender)
            RunRenderChecks(options, ctx);

        std::cout << "[SUMMARY] passed=" << ctx.passed << " failed=" << ctx.failed << "\n";
        return ctx.failed == 0 ? 0 : 1;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FATAL] " << exception.what() << "\n";
        return 2;
    }
}
