#include "ClientSession.h"
#include "AssetLibrary.h"
#include "MapEditorTypes.h"
#include "WaterBodyIO.h"

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
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

struct Options
{
    bool runNetwork = false;
    bool runAsset = false;
    bool runRender = false;
    std::string loginHost = "127.0.0.1";
    std::uint16_t loginPort = 11000;
    std::string username = "testuser";
    std::string password = "testpass";
    std::uint64_t characterId = 0;
    int timeoutSeconds = 15;
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

std::string GetEnvString(const char* name, const std::string& fallback)
{
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) == 0 && value)
    {
        std::string out(value);
        std::free(value);
        if (!out.empty())
            return out;
    }
    return fallback;
}

std::uint16_t ParsePort(const std::string& value)
{
    const int parsed = std::stoi(value);
    if (parsed <= 0 || parsed > 65535)
        throw std::runtime_error("invalid port: " + value);
    return static_cast<std::uint16_t>(parsed);
}

void PrintUsage()
{
    std::cout
        << "IwSelfTest options:\n"
        << "  --all                         Run asset/render/network tests\n"
        << "  --asset                       Run isolated AssetLibrary tests\n"
        << "  --render                      Run render asset/shader/config checks\n"
        << "  --network                     Connect to loginserver and enter world\n"
        << "  --login-host HOST             Default: 127.0.0.1\n"
        << "  --login-port PORT             Default: 11000\n"
        << "  --username USER               Default: IW_TEST_USER or testuser\n"
        << "  --password PASS               Default: IW_TEST_PASSWORD or testpass\n"
        << "  --character-id ID             Default: first character in list\n"
        << "  --timeout SECONDS             Default: 15\n"
        << "  --client-root PATH            Default: compiled Client source root\n"
        << "  --scratch-root PATH           Default: temp/IwSelfTest_<time>\n";
}

Options ParseOptions(int argc, char** argv)
{
    Options options;
    options.username = GetEnvString("IW_TEST_USER", options.username);
    options.password = GetEnvString("IW_TEST_PASSWORD", options.password);

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
            options.runNetwork = true;
            options.runAsset = true;
            options.runRender = true;
        }
        else if (arg == "--asset")
            options.runAsset = true;
        else if (arg == "--render")
            options.runRender = true;
        else if (arg == "--network")
            options.runNetwork = true;
        else if (arg == "--login-host")
            options.loginHost = needValue("--login-host");
        else if (arg == "--login-port")
            options.loginPort = ParsePort(needValue("--login-port"));
        else if (arg == "--username")
            options.username = needValue("--username");
        else if (arg == "--password")
            options.password = needValue("--password");
        else if (arg == "--character-id")
            options.characterId = std::stoull(needValue("--character-id"));
        else if (arg == "--timeout")
            options.timeoutSeconds = std::stoi(needValue("--timeout"));
        else if (arg == "--client-root")
            options.clientRoot = needValue("--client-root");
        else if (arg == "--scratch-root")
            options.scratchRoot = needValue("--scratch-root");
        else
            throw std::runtime_error("unknown option: " + arg);
    }

    if (!options.runNetwork && !options.runAsset && !options.runRender)
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

    std::cout << "[INFO] asset scratch kept at: " << scratch.generic_string() << "\n";
    return ctx.failed == 0;
}

bool RunRenderChecks(const Options& options, TestContext& ctx)
{
    const std::filesystem::path shaderDir = options.clientRoot / "assets" / "shaders";
    ctx.Expect(std::filesystem::exists(options.clientRoot / "shaders" / "Water.hlsl"),
        "water shader source exists", "Client/shaders/Water.hlsl missing");
    ctx.Expect(std::filesystem::exists(shaderDir / "water_vs.spv"),
        "water vertex shader compiled", "assets/shaders/water_vs.spv missing; build ClientShaders");
    ctx.Expect(std::filesystem::exists(shaderDir / "water_ps.spv"),
        "water pixel shader compiled", "assets/shaders/water_ps.spv missing; build ClientShaders");
    ctx.Expect(std::filesystem::exists(options.clientRoot / "shaders" / "Composite.hlsl"),
        "composite shader source exists", "Client/shaders/Composite.hlsl missing");
    ctx.Expect(std::filesystem::exists(shaderDir / "composite_vs.spv"),
        "composite vertex shader compiled", "assets/shaders/composite_vs.spv missing; build ClientShaders");
    ctx.Expect(std::filesystem::exists(shaderDir / "composite_ps.spv"),
        "composite pixel shader compiled", "assets/shaders/composite_ps.spv missing; build ClientShaders");

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

    const std::filesystem::path vulkanDevicePath = options.clientRoot / "libs" / "platform" / "VulkanDevice.cpp";
    std::ifstream vulkanDevice(vulkanDevicePath);
    std::stringstream vulkanDeviceText;
    vulkanDeviceText << vulkanDevice.rdbuf();
    const std::string vulkanDeviceSource = vulkanDeviceText.str();
    ctx.Expect(vulkanDeviceSource.find("supported.samplerAnisotropy") != std::string::npos &&
            vulkanDeviceSource.find("enabled.samplerAnisotropy = VK_TRUE") != std::string::npos,
        "vulkan sampler anisotropy feature", "VulkanDevice must enable samplerAnisotropy with graceful fallback");

    const std::filesystem::path warriorShaderPath = options.clientRoot / "shaders" / "Warrior.hlsl";
    std::ifstream warriorShader(warriorShaderPath);
    std::stringstream warriorShaderText;
    warriorShaderText << warriorShader.rdbuf();
    const std::string warriorSource = warriorShaderText.str();
    ctx.Expect(warriorSource.find("u_waterParams") != std::string::npos &&
            warriorSource.find("CausticPattern") != std::string::npos &&
            warriorSource.find("waterDepth > 0.0") != std::string::npos,
        "warrior water caustic shader controls", "Warrior shader is missing WATER-4 underwater caustic controls");

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

    const std::filesystem::path mainPath = options.clientRoot / "apps" / "client" / "src" / "main.cpp";
    std::ifstream mainFile(mainPath);
    std::stringstream mainText;
    mainText << mainFile.rdbuf();
    const std::string mainSource = mainText.str();
    ctx.Expect(mainSource.find("offscreenScene.BeginMainPass") != std::string::npos &&
            mainSource.find("offscreenScene.RenderComposite") != std::string::npos,
        "main render loop uses offscreen composite", "Main render loop does not route 3D through offscreen composite");
    ctx.Expect(mainSource.find("offscreenScene.SnapshotScene") != std::string::npos &&
            mainSource.find("terrain.SetWaterRefractionInputs") != std::string::npos &&
            mainSource.find("offscreenScene.BeginMainPass(device, false)") != std::string::npos,
        "main render loop uses water refraction snapshots", "Main render loop does not snapshot scene before water pass");

    ctx.Expect(warriorSource.find("u_lightPadding.x > 0.5") != std::string::npos &&
            warriorSource.find("input.worldPos.y < u_lightPadding.y") != std::string::npos,
        "warrior reflection clip shader", "Warrior shader is missing water-level reflection clipping");

    const std::filesystem::path warriorRendererPath = options.clientRoot / "libs" / "render" / "WarriorRenderer.cpp";
    std::ifstream warriorRenderer(warriorRendererPath);
    std::stringstream warriorRendererText;
    warriorRendererText << warriorRenderer.rdbuf();
    const std::string warriorRendererSource = warriorRendererText.str();
    ctx.Expect(warriorRendererSource.find("CreateReflectionPipeline") != std::string::npos &&
            warriorRendererSource.find("VK_CULL_MODE_FRONT_BIT") != std::string::npos &&
            warriorRendererSource.find("RenderInWorldReflection") != std::string::npos,
        "warrior reflection pipeline source", "WarriorRenderer reflection pipeline entry points are missing");

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
    const std::filesystem::path gameClientLayerPath = options.clientRoot / "libs" / "render" / "GameClientLayer.cpp";
    std::ifstream gameClientLayer(gameClientLayerPath);
    std::stringstream gameClientLayerText;
    gameClientLayerText << gameClientLayer.rdbuf();
    const std::string gameClientLayerSource = gameClientLayerText.str();
    const std::filesystem::path gameClientLayerHeaderPath = options.clientRoot / "libs" / "render" / "GameClientLayer.h";
    std::ifstream gameClientLayerHeader(gameClientLayerHeaderPath);
    std::stringstream gameClientLayerHeaderText;
    gameClientLayerHeaderText << gameClientLayerHeader.rdbuf();
    const std::string gameClientLayerHeaderSource = gameClientLayerHeaderText.str();
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
    const std::filesystem::path loginRmlPath = options.clientRoot / "assets" / "ui" / "login.rml";
    std::ifstream loginRml(loginRmlPath);
    std::stringstream loginRmlText;
    loginRmlText << loginRml.rdbuf();
    const std::string loginRmlSource = loginRmlText.str();
    const std::filesystem::path loginRcssPath = options.clientRoot / "assets" / "ui" / "login.rcss";
    std::ifstream loginRcss(loginRcssPath);
    std::stringstream loginRcssText;
    loginRcssText << loginRcss.rdbuf();
    const std::string loginRcssSource = loginRcssText.str();
    const std::filesystem::path lobbyRmlPath = options.clientRoot / "assets" / "ui" / "lobby.rml";
    std::ifstream lobbyRml(lobbyRmlPath);
    std::stringstream lobbyRmlText;
    lobbyRmlText << lobbyRml.rdbuf();
    const std::string lobbyRmlSource = lobbyRmlText.str();
    const std::filesystem::path lobbyRcssPath = options.clientRoot / "assets" / "ui" / "lobby.rcss";
    std::ifstream lobbyRcss(lobbyRcssPath);
    std::stringstream lobbyRcssText;
    lobbyRcssText << lobbyRcss.rdbuf();
    const std::string lobbyRcssSource = lobbyRcssText.str();
    const std::filesystem::path worldHudRmlPath = options.clientRoot / "assets" / "ui" / "worldhud.rml";
    std::ifstream worldHudRml(worldHudRmlPath);
    std::stringstream worldHudRmlText;
    worldHudRmlText << worldHudRml.rdbuf();
    const std::string worldHudRmlSource = worldHudRmlText.str();
    const std::filesystem::path worldHudRcssPath = options.clientRoot / "assets" / "ui" / "worldhud.rcss";
    std::ifstream worldHudRcss(worldHudRcssPath);
    std::stringstream worldHudRcssText;
    worldHudRcssText << worldHudRcss.rdbuf();
    const std::string worldHudRcssSource = worldHudRcssText.str();
    const std::filesystem::path menuRmlPath = options.clientRoot / "assets" / "ui" / "ingame_menu.rml";
    std::ifstream menuRml(menuRmlPath);
    std::stringstream menuRmlText;
    menuRmlText << menuRml.rdbuf();
    const std::string menuRmlSource = menuRmlText.str();
    const std::filesystem::path settingsRmlPath = options.clientRoot / "assets" / "ui" / "settings.rml";
    std::ifstream settingsRml(settingsRmlPath);
    std::stringstream settingsRmlText;
    settingsRmlText << settingsRml.rdbuf();
    const std::string settingsRmlSource = settingsRmlText.str();
    const std::filesystem::path inventoryRmlPath = options.clientRoot / "assets" / "ui" / "inventory.rml";
    std::ifstream inventoryRml(inventoryRmlPath);
    std::stringstream inventoryRmlText;
    inventoryRmlText << inventoryRml.rdbuf();
    const std::string inventoryRmlSource = inventoryRmlText.str();
    const std::filesystem::path creationRmlPath = options.clientRoot / "assets" / "ui" / "character_creation.rml";
    std::ifstream creationRml(creationRmlPath);
    std::stringstream creationRmlText;
    creationRmlText << creationRml.rdbuf();
    const std::string creationRmlSource = creationRmlText.str();
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
            renderCmakeSource.find("GameClientLayer.cpp") != std::string::npos &&
            clientMainSource.find("RmlUiLayer rmlUi") != std::string::npos &&
            clientMainSource.find("CreateRuntimeSession(kActiveRuntimeImplementation)") != std::string::npos &&
            clientMainSource.find("rmlUi.Render(device);") != std::string::npos &&
            clientMainSource.find("editorImGui.Render(device);") != std::string::npos,
        "rmlui build and z-order pipeline", "RMLUI-1 must link RmlUi 6.2, compile shaders, and render before ImGui");
    ctx.Expect(rmlUiLayerSource.find("class RmlAssetFileInterface") != std::string::npos &&
            rmlUiLayerSource.find("class RmlRenderInterface final : public Rml::RenderInterface") != std::string::npos &&
            rmlUiLayerSource.find("CreateDescriptorPool") != std::string::npos &&
            rmlUiLayerSource.find("ProcessMouseButtonDown") != std::string::npos &&
            rmlUiLayerSource.find("Event: type=%s element=login-button") != std::string::npos &&
            rmlUiLayerSource.find("assets/ui/login.rml") != std::string::npos &&
            rmlUiLayerSource.find("SetLoginSubmitCallback") != std::string::npos &&
            rmlUiLayerSource.find("Username and password required") != std::string::npos &&
            rmlUiLayerSource.find("[RMLUI-DIAG] CreateContext: viewport=") != std::string::npos &&
            rmlUiLayerSource.find("[RMLUI-DIAG] RenderGeometry: vertices=") != std::string::npos &&
            rmlUiLayerSource.find("[RMLUI-DIAG] vkCmdSetViewport") != std::string::npos &&
            rmlUiLayerSource.find("[RMLUI-DIAG] Pipeline primitive topology: TRIANGLE_LIST") != std::string::npos &&
            rmlUiLayerSource.find("PendingGeometryDelete") != std::string::npos &&
            rmlUiLayerSource.find("m_pendingGeometryDeletes") != std::string::npos &&
            rmlUiShaderSource.find("[[vk::push_constant]]") != std::string::npos &&
            rmlUiShaderSource.find("[[vk::binding(0, 0)]] Texture2D") != std::string::npos &&
            rmlUiShaderSource.find("(pixel.y / g_push.viewport.y) * 2.0f - 1.0f") != std::string::npos,
        "rmlui vulkan layer source", "RMLUI-2 must provide file, render, input, login event, and Vulkan shader integration");
    ctx.Expect(loginRmlSource.find("AURIGA GLOBAL") != std::string::npos &&
            loginRmlSource.find("login-username") != std::string::npos &&
            loginRmlSource.find("login-password") != std::string::npos &&
            loginRmlSource.find("login-remember") != std::string::npos &&
            loginRmlSource.find("login-button") != std::string::npos &&
            loginRmlSource.find("login-error") != std::string::npos &&
            loginRcssSource.find(".login-screen") != std::string::npos &&
            loginRcssSource.find(".login-panel") != std::string::npos &&
            loginRcssSource.find(".login-button") != std::string::npos &&
            loginRcssSource.find(".login-error") != std::string::npos &&
            runtimeUiAdapterSource.find("m_rmlUi.SetLoginSubmitCallback") != std::string::npos &&
            runtimeUiAdapterSource.find("runtime.SetLoginCallbacks") != std::string::npos,
        "rmlui login document and game handoff", "RMLUI-2 login must be RML/RCSS and call the existing backend login path");
    ctx.Expect(lobbyRmlSource.find("Lobby - AURIGA GLOBAL") != std::string::npos &&
            lobbyRmlSource.find("character-list") != std::string::npos &&
            lobbyRmlSource.find("enter-world-btn") != std::string::npos &&
            lobbyRmlSource.find("delete-char-btn") != std::string::npos &&
            lobbyRmlSource.find("logout-btn") != std::string::npos &&
            lobbyRmlSource.find("delete-confirm") != std::string::npos &&
            lobbyRcssSource.find(".lobby-screen") != std::string::npos &&
            lobbyRcssSource.find(".character-panel") != std::string::npos &&
            lobbyRcssSource.find(".action-panel") != std::string::npos &&
            lobbyRcssSource.find(".character-item.selected") != std::string::npos &&
            rmlUiLayerSource.find("assets/ui/lobby.rml") != std::string::npos &&
            rmlUiLayerSource.find("SetLobbyCharacters") != std::string::npos &&
            rmlUiLayerSource.find("[RMLUI-LOBBY] Character list populated") != std::string::npos &&
            runtimeUiAdapterSource.find("m_rmlUi.SetLobbyCallbacks") != std::string::npos &&
            runtimeUiAdapterSource.find("runtime.SetLobbyCallbacks") != std::string::npos,
        "rmlui lobby document and game handoff", "RMLUI-3 lobby must be RML/RCSS and populate character data from callbacks");
    ctx.Expect(worldHudRmlSource.find("HUD - AURIGA GLOBAL") != std::string::npos &&
            worldHudRmlSource.find("player-name") != std::string::npos &&
            worldHudRmlSource.find("hp-fill") != std::string::npos &&
            worldHudRmlSource.find("mp-fill") != std::string::npos &&
            worldHudRmlSource.find("xp-fill") != std::string::npos &&
            worldHudRmlSource.find("target-frame") != std::string::npos &&
            worldHudRmlSource.find("minimap-canvas") != std::string::npos &&
            worldHudRcssSource.find(".player-frame") != std::string::npos &&
            worldHudRcssSource.find(".target-frame") != std::string::npos &&
            worldHudRcssSource.find(".minimap-frame") != std::string::npos &&
            rmlUiLayerSource.find("assets/ui/worldhud.rml") != std::string::npos &&
            rmlUiLayerSource.find("CacheHudElements") != std::string::npos &&
            rmlUiLayerSource.find("void RmlUiLayer::UpdateHud") != std::string::npos &&
            rmlUiLayerSource.find("SetProperty(\"width\"") != std::string::npos &&
            runtimeUiAdapterSource.find("m_rmlUi.ShowHud()") != std::string::npos &&
            clientMainSource.find("runtimeUi->UpdateHud(hudData)") != std::string::npos,
        "rmlui hud document and per-frame update", "RMLUI-4 HUD must be RML/RCSS and update bar widths by CSS property");
    ctx.Expect(menuRmlSource.find("menu-resume-btn") != std::string::npos &&
            menuRmlSource.find("menu-settings-btn") != std::string::npos &&
            menuRmlSource.find("menu-logout-btn") != std::string::npos &&
            settingsRmlSource.find("settings-tab-video") != std::string::npos &&
            settingsRmlSource.find("settings-tab-audio") != std::string::npos &&
            inventoryRmlSource.find("inventory-grid") != std::string::npos &&
            creationRmlSource.find("character-name") != std::string::npos &&
            rmlUiLayerSource.find("assets/ui/ingame_menu.rml") != std::string::npos &&
            rmlUiLayerSource.find("SetInGameMenuCallbacks") != std::string::npos &&
            rmlUiLayerSource.find("ToggleInventory") != std::string::npos &&
            rmlUiLayerSource.find("ShowCharacterCreation") != std::string::npos &&
            clientMainSource.find("event.key == Key_Escape") != std::string::npos &&
            clientMainSource.find("event.key == Key_I") != std::string::npos,
        "rmlui gameplay panels", "RMLUI-5 must provide menu, settings, inventory, character creation, and keyboard toggles");
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
    ctx.Expect(editorImGuiSource.find("ApplyAaaImGuiStyle") != std::string::npos &&
            editorImGuiSource.find("WindowRounding = 6.0f") != std::string::npos &&
            editorImGuiSource.find("FramePadding = ImVec2(8.0f, 6.0f)") != std::string::npos &&
            editorImGuiSource.find("ImGuiCol_ButtonHovered") != std::string::npos &&
            editorImGuiSource.find("0.28f, 0.48f, 0.75f") != std::string::npos &&
            editorImGuiSource.find("[EDITOR-VISUAL] AAA-style ImGui colors applied") != std::string::npos,
        "editor aaa imgui style", "EDITOR-VISUAL-POLISH must apply deep backgrounds, rounded corners, padding, and blue hover accents");
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
            clientMainSource.find("AURIGA GLOBAL") != std::string::npos &&
            clientMainSource.find("Standalone Vulkan Clear - gameClient Overlay") == std::string::npos,
        "editor icon buttons and title", "EDITOR-VISUAL-POLISH must iconize editor controls and rename the window title");
    ctx.Expect(editorImGuiSource.find("ImGui::Button(label)") != std::string::npos &&
            editorImGuiSource.find("m_assetFilter = filter") != std::string::npos &&
            editorImGuiSource.find("m_assetSubpath.clear()") != std::string::npos &&
            editorImGuiSource.find("ImGuiTabItemFlags_SetSelected") == std::string::npos,
        "asset browser category tabs", "asset browser category buttons must drive the editor filter directly instead of forcing ImGui tab selection");
    ctx.Expect(editorImGuiSource.find("BeginTable(\"AssetGridTiles\"") != std::string::npos &&
            editorImGuiSource.find("TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, cellWidth)") != std::string::npos &&
            editorImGuiSource.find("ImGui::TableNextColumn()") != std::string::npos &&
            editorImGuiSource.find("ShortAssetFilename") != std::string::npos &&
            editorImGuiSource.find("constexpr size_t kVisibleCharacters = 10") != std::string::npos,
        "asset browser grid wrapping", "asset browser tiles must wrap inside fixed grid cells instead of extending horizontally past the Tags panel");
    ctx.Expect(editorImGuiSource.find("BeginChild(\"AssetFoldersScroll\"") != std::string::npos &&
            editorImGuiSource.find("BeginChild(\"AssetGridScroll\"") != std::string::npos &&
            editorImGuiSource.find("BeginChild(\"AssetTagsScroll\"") != std::string::npos &&
            editorImGuiSource.find("browserPanelHeight") != std::string::npos,
        "asset browser independent scroll zones", "asset browser folders, grid, and tags columns must scroll independently when content overflows");
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
            sceneManagerSource.find("\\\"terrain_ref\\\"") != std::string::npos &&
            sceneManagerSource.find("\\\"splat_ref\\\"") != std::string::npos &&
            sceneManagerSource.find("\\\"shape_mask_ref\\\"") != std::string::npos &&
            sceneManagerSource.find(".heightmap") != std::string::npos &&
            sceneManagerSource.find(".splat") != std::string::npos &&
            sceneManagerSource.find(".mask") != std::string::npos,
        "scene json sidecar format", "SCENE-1 must save readable .scene JSON and separate heightmap/splat/water-mask sidecar files");
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
            sceneManagerSource.find("AURIGA GLOBAL \\xE2\\x80\\x94 Editor") != std::string::npos &&
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
    ctx.Expect(sceneManagerHeaderSource.find("SetRuntimeUiCallbacks") != std::string::npos &&
            sceneManagerSource.find("ActivateSceneType") != std::string::npos &&
            sceneManagerSource.find("scene_type") != std::string::npos &&
            sceneManagerSource.find("type == \"login\"") != std::string::npos &&
            sceneManagerSource.find("type == \"lobby\"") != std::string::npos &&
            sceneManagerSource.find("type == \"world\"") != std::string::npos &&
            sceneManagerSource.find("type == \"empty\"") != std::string::npos,
        "scene runtime ui binding", "SCENE-2 must activate RmlUi views from scene_type values");
    ctx.Expect(runtimeSessionHeaderSource.find("class RuntimeSession") != std::string::npos &&
            runtimeSessionHeaderSource.find("Start(const SceneData& openScene)") != std::string::npos &&
            runtimeSessionHeaderSource.find("Stop()") != std::string::npos &&
            runtimeSessionSource.find("class EmptyRuntimeSession final") != std::string::npos &&
            runtimeSessionSource.find("class AurigaRuntimeSession final") != std::string::npos &&
            runtimeSessionSource.find("GameClientLayer m_gameClient") != std::string::npos &&
            runtimeSessionSource.find("std::unique_ptr<client::net::ClientSession>") != std::string::npos &&
            runtimeUiAdapterHeaderSource.find("class RuntimeUiAdapter") != std::string::npos &&
            runtimeUiAdapterSource.find("class NullRuntimeUiAdapter final") != std::string::npos &&
            runtimeUiAdapterSource.find("class AurigaRuntimeUiAdapter final") != std::string::npos &&
            clientMainSource.find("kActiveRuntimeImplementation = RuntimeImplementation::Auriga") != std::string::npos,
        "cleanup runtime adapters", "CLEANUP-1 must put runtime session and player UI routing behind default and AURIGA adapters");
    ctx.Expect(clientMainSource.find("StartupSceneFromConfig") != std::string::npos &&
            clientMainSource.find("startup_scene") != std::string::npos &&
            clientMainSource.find("scenes/Login.scene") != std::string::npos &&
            clientMainSource.find("#else\n    LoadRuntimeScene(assets, StartupSceneFromConfig(assets));") != std::string::npos,
        "scene release startup flow", "SCENE-2 release builds must load app_config.json startup_scene with Login.scene fallback");
    ctx.Expect(gameClientLayerHeaderSource.find("SetMapEditorOpen") != std::string::npos &&
            gameClientLayerSource.find("void GameClientLayer::SetMapEditorOpen(bool open)") != std::string::npos &&
            clientMainSource.find("runtimeSession->SetMapEditorOpen(true)") != std::string::npos &&
            clientMainSource.find("[BOOT] editor_open forced = 1") != std::string::npos &&
            clientMainSource.find("editorImGui.BeginFrame(runtimeSession->IsMapEditorOpen())") != std::string::npos,
        "editor boot opens imgui", "ENGINE-BOOT-FIX must keep the ImGui editor open on editor-build boot independent of scene_type/RmlUi routing");
    ctx.Expect(runtimeUiAdapterSource.find("scenes/Lobby.scene") != std::string::npos &&
            runtimeUiAdapterSource.find("scenes/World.scene") != std::string::npos &&
            runtimeUiAdapterSource.find("LoadRuntimeSceneOrFallback") != std::string::npos,
        "scene navigation callbacks", "SCENE-2 login/lobby/world transitions must route through scene loads");
    ctx.Expect(rmlUiLayerSource.find("void RmlUiLayer::HideAll") != std::string::npos &&
            rmlUiLayerSource.find("WarnSceneTypeMismatch") != std::string::npos &&
            rmlUiLayerSource.find("ShowLogin\", \"login") != std::string::npos &&
            rmlUiLayerSource.find("ShowLobby\", \"lobby") != std::string::npos &&
            rmlUiLayerSource.find("ShowHud\", \"world") != std::string::npos,
        "rmlui scene type warnings", "SCENE-2 must warn when runtime UI is shown outside its matching scene_type while remaining backwards compatible");
    ctx.Expect(editorImGuiSource.find("RenderSceneSettingsPanel") != std::string::npos &&
            editorImGuiSource.find("Scene Settings") != std::string::npos &&
            editorImGuiSource.find("Scene Type") != std::string::npos &&
            editorImGuiSource.find("\"login\", \"lobby\", \"loading\", \"world\"") != std::string::npos,
        "scene settings panel", "SCENE-2 must expose scene metadata and editable scene type in the editor");
    ctx.Expect(clientMainSource.find("playStartScenePath") != std::string::npos &&
            clientMainSource.find("Restored starting scene") != std::string::npos &&
            editorImGuiSource.find("Open a scene to Play") != std::string::npos &&
            editorImGuiSource.find("HasOpenScene()") != std::string::npos,
        "play scene restore", "SCENE-2 Play mode must store/restore the starting scene and disable Play with no open scene");
    ctx.Expect(clientMainSource.find("editorRuntimeFlowActive") != std::string::npos &&
            runtimeUiAdapterSource.find("suppressed (Edit mode)") != std::string::npos &&
            runtimeUiAdapterSource.find("[SCENE-FLOW] suppressed in Edit mode") != std::string::npos &&
            clientMainSource.find("SceneManager::Instance().ActivateCurrentSceneType()") != std::string::npos &&
            clientMainSource.find("Runtime UI/scene flow enabled for Play mode") != std::string::npos,
        "runtime ui play gate", "EDIT-PLAY-2 must keep scene_type RmlUi routing and login/lobby/world flow inactive in Edit mode and enable them only in Play");
    ctx.Expect(clientMainSource.find("injectDirectGameplayDevCharacter") != std::string::npos &&
            clientMainSource.find("playSceneType == \"world\" || playSceneType == \"gameplay\"") != std::string::npos &&
            clientMainSource.find("!hasOwnRuntimeCharacter()") != std::string::npos &&
            clientMainSource.find("Direct gameplay scene Play: dev character injected") != std::string::npos,
        "direct gameplay dev character", "EDIT-PLAY-2 must inject a dev character only when directly playing a gameplay scene without an existing character");
    ctx.Expect(sceneManagerHeaderSource.find("RestoreSceneSnapshot") != std::string::npos &&
            sceneManagerSource.find("void SceneManager::RestoreSceneSnapshot") != std::string::npos &&
            clientMainSource.find("playStartSceneSnapshot") != std::string::npos &&
            clientMainSource.find("playStartSceneDirty") != std::string::npos,
        "play snapshot restore", "EDIT-PLAY-2 Stop must restore the editor scene snapshot, including unsaved scenes, after Play");
    ctx.Expect(editorImGuiSource.find("RenderHierarchyPanel") != std::string::npos &&
            editorImGuiSource.find("Hierarchy") != std::string::npos &&
            editorImGuiSource.find("RenderHierarchyWaterBodies") != std::string::npos &&
            editorImGuiSource.find("RenderHierarchyPointLights") != std::string::npos &&
            editorImGuiSource.find("RenderHierarchySpotLights") != std::string::npos &&
            editorImGuiSource.find("DockBuilderDockWindow(ICON_FA_LIST_TREE \" Hierarchy\"") != std::string::npos,
        "hierarchy panel and dock", "HIERARCHY-1 must add a docked Scene Hierarchy panel grouped by entity type");
    ctx.Expect(editorImGuiSource.find("RenderHierarchyToolbar") != std::string::npos &&
            editorImGuiSource.find("Search entities") != std::string::npos &&
            editorImGuiSource.find("HierarchyPassesSearch") != std::string::npos &&
            editorImGuiSource.find("ContainsCaseInsensitive") != std::string::npos &&
            editorImGuiSource.find("Search filter") != std::string::npos,
        "hierarchy search", "HIERARCHY-1 must provide case-insensitive entity search with a clearable toolbar");
    ctx.Expect(editorImGuiSource.find("QueueHierarchySelection") != std::string::npos &&
            editorImGuiSource.find("QueueHierarchyFocus") != std::string::npos &&
            editorImGuiSource.find("SetScrollHereY") != std::string::npos &&
            clientMainSource.find("selectHierarchyEntity") != std::string::npos &&
            clientMainSource.find("editorImGui.SetHierarchySceneState") != std::string::npos,
        "hierarchy selection sync", "HIERARCHY-1 must sync hierarchy clicks with Inspector/gizmo selection and reverse-highlight selected items");
    ctx.Expect(editorImGuiSource.find("RenderHierarchyContextMenu") != std::string::npos &&
            editorImGuiSource.find("Focus Camera") != std::string::npos &&
            editorImGuiSource.find("Duplicate") != std::string::npos &&
            editorImGuiSource.find("Rename") != std::string::npos &&
            editorImGuiSource.find("Delete") != std::string::npos &&
            editorImGuiSource.find("InputTextFlags_EnterReturnsTrue") != std::string::npos,
        "hierarchy context menu", "HIERARCHY-1 must expose Focus/Duplicate/Rename/Delete and in-place rename");
    ctx.Expect(mapEditorTypesSource.find("HierarchyEntityType") != std::string::npos &&
            mapEditorTypesSource.find("hierarchyDuplicateEntity") != std::string::npos &&
            mapEditorTypesSource.find("hierarchyRenameEntity") != std::string::npos &&
            clientMainSource.find("duplicateHierarchyEntity") != std::string::npos &&
            clientMainSource.find("renameHierarchyEntity") != std::string::npos &&
            clientMainSource.find("deleteHierarchyEntity") != std::string::npos,
        "hierarchy commands", "HIERARCHY-1 hierarchy commands must be routed from ImGui to the editor runtime");
    ctx.Expect(mapEditorTypesSource.find("editorHidden") != std::string::npos &&
            editorImGuiSource.find("ICON_FA_EYE_SLASH") != std::string::npos &&
            clientMainSource.find("toggleHierarchyHidden") != std::string::npos &&
            clientMainSource.find("light.editorHidden") != std::string::npos &&
            clientMainSource.find("body.editorHidden") != std::string::npos &&
            sceneManagerSource.find("\\\"editor_hidden\\\"") == std::string::npos,
        "hierarchy editor visibility", "HIERARCHY-1 must add editor-only hide/show with eye icons and avoid saving editor_hidden into scene JSON");
    ctx.Expect(clientMainSource.find("void FocusOn(WorldVec3 target") != std::string::npos &&
            clientMainSource.find("focusHierarchyEntity") != std::string::npos &&
            editorImGuiSource.find("ImGuiKey_F") != std::string::npos &&
            clientMainSource.find("[HIERARCHY] Focused camera on entity") != std::string::npos,
        "hierarchy focus camera", "HIERARCHY-1 must focus the editor camera on selected hierarchy entities with F/double-click/context menu");
    ctx.Expect(editorImGuiSource.find("RenderEditorToolbar") != std::string::npos &&
            editorImGuiSource.find("HandleEditorHotkeys") != std::string::npos &&
            editorImGuiSource.find("ImGuiKey_F5") != std::string::npos &&
            editorImGuiSource.find("ImGuiKey_F6") != std::string::npos &&
            editorImGuiSource.find("enterPlayMode") != std::string::npos &&
            editorImGuiSource.find("pausePlayMode") != std::string::npos &&
            editorImGuiSource.find("Tools disabled in Play Mode") != std::string::npos &&
            editorImGuiSource.find("Read-only during Play Mode") != std::string::npos,
        "editor play toolbar and hotkeys", "EDIT-PLAY-1 needs Play/Stop/Pause toolbar controls, F5/F6 hotkeys, and disabled edit tools in Play Mode");
    ctx.Expect(gameClientLayerHeaderSource.find("EnterLocalPlayMode") != std::string::npos &&
            gameClientLayerHeaderSource.find("ExitLocalPlayMode") != std::string::npos &&
            gameClientLayerHeaderSource.find("UpdateLocalPlayPlayer") != std::string::npos &&
            gameClientLayerSource.find("localSavedStateValid") != std::string::npos &&
            gameClientLayerSource.find("[EDIT-PLAY] Runtime state cleared") != std::string::npos &&
            clientMainSource.find("EditorPlayRuntime") != std::string::npos &&
            clientMainSource.find("DevPlayer") != std::string::npos &&
            clientMainSource.find("injectDirectGameplayDevCharacter") != std::string::npos &&
            clientMainSource.find("player.level = 50") != std::string::npos &&
            clientMainSource.find("player.hpMax = 1000.0f") != std::string::npos &&
            clientMainSource.find("SceneManager::Instance().ActivateCurrentSceneType()") != std::string::npos &&
            clientMainSource.find("editorPlay.state.mode == EditorPlayMode::PlayPaused") != std::string::npos &&
            clientMainSource.find("[EDIT-PLAY] Snapshot restored") != std::string::npos,
        "editor local play mode runtime", "EDIT-PLAY-1/2 needs local play runtime state, controlled dev character spawn, HUD routing, pause, and Stop restore");
    ctx.Expect(!std::filesystem::exists(options.clientRoot / "libs" / "render" / ("Noe" "sisLayer.cpp")) &&
            !std::filesystem::exists(options.clientRoot / "libs" / "render" / ("Noe" "sisLayer.h")) &&
            renderCmakeSource.find(legacyUiName) == std::string::npos &&
            rootCmakeSource.find(legacyUiName) == std::string::npos &&
            clientCmakeSource.find(legacyUiName) == std::string::npos &&
            androidGradleSource.find(legacyUiName) == std::string::npos &&
            gameClientLayerSource.find(legacyUiName) == std::string::npos &&
            rmlUiLayerSource.find(legacyUiName) == std::string::npos &&
            clientMainSource.find(legacyUiName) == std::string::npos &&
            renderCmakeSource.find(legacyMarkupExt) == std::string::npos &&
            rootCmakeSource.find(legacyMarkupExt) == std::string::npos &&
            clientCmakeSource.find(legacyMarkupExt) == std::string::npos,
        "legacy ui removed", "RMLUI-5 must remove legacy UI source, CMake links, Android packaging, and markup assets");
    ctx.Expect(editorImGuiSource.find("RenderWaterSculptToolPanel") != std::string::npos &&
            editorImGuiSource.find("RenderHeightmapToolPanel") != std::string::npos &&
            editorImGuiSource.find("RenderSplatPaintToolPanel") != std::string::npos &&
            editorImGuiSource.find("ImportAssetWithDialog") != std::string::npos &&
            editorImGuiSource.find("PickAssetFileForImport") != std::string::npos &&
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
        "water object selection visuals removed", "Water-body selection must not render the legacy bbox or warrior/nameplate proxies");

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

class NetworkProbe final : public client::net::IClientHandler
{
public:
    enum class Phase
    {
        LoginConnect,
        LoginAuthenticated,
        WaitingCharacterList,
        WaitingToken,
        GameConnect,
        EnteringWorld,
        InWorld,
        Failed
    };

    explicit NetworkProbe(const Options& opts)
        : options(opts)
    {
    }

    void Attach(client::net::ClientSession* value)
    {
        session = value;
    }

    bool Done() const
    {
        return phase == Phase::InWorld || phase == Phase::Failed;
    }

    bool Success() const
    {
        return phase == Phase::InWorld;
    }

    std::string FailureReason() const
    {
        return failure;
    }

    void Start()
    {
        Log("connect login " + options.loginHost + ":" + std::to_string(options.loginPort));
        phase = Phase::LoginConnect;
        session->Connect(options.loginHost, options.loginPort);
    }

    void OnConnectionFailed(const std::string& reason) override
    {
        Fail("connection failed: " + reason);
    }

    void OnDisconnected() override
    {
        if (handoffDisconnectExpected)
        {
            handoffDisconnectExpected = false;
            phase = Phase::GameConnect;
            Log("connect game " + gameHost + ":" + std::to_string(gamePort));
            session->Connect(gameHost, gamePort);
            return;
        }
        if (phase == Phase::GameConnect)
        {
            Log("ignoring late login disconnect during game handoff");
            return;
        }
        if (phase != Phase::InWorld && phase != Phase::Failed)
            Fail("unexpected disconnect");
    }

    void OnHandshakeAccepted() override
    {
        if (phase == Phase::LoginConnect)
        {
            Log("login handshake accepted");
            session->SendLogin(options.username, options.password);
            return;
        }
        if (phase == Phase::GameConnect)
        {
            Log("game handshake accepted; sending enter-world token");
            phase = Phase::EnteringWorld;
            session->SendEnterWorld(token);
            return;
        }
        Fail("handshake accepted in unexpected phase");
    }

    void OnHandshakeRejected(const std::string& reason) override
    {
        Fail("handshake rejected: " + reason);
    }

    void OnLoginAccepted(std::uint64_t accountId) override
    {
        Log("login accepted account_id=" + std::to_string(accountId));
        phase = Phase::WaitingCharacterList;
        session->SendCharacterListRequest();
    }

    void OnLoginRejected(const std::string& reason) override
    {
        Fail("login rejected: " + reason);
    }

    void OnCharacterList(const std::vector<client::net::CharacterListItem>& characters) override
    {
        Log("character list count=" + std::to_string(characters.size()));
        if (characters.empty())
        {
            Fail("character list is empty");
            return;
        }

        std::uint64_t selected = options.characterId;
        if (selected == 0)
            selected = characters.front().id;

        const auto it = std::find_if(characters.begin(), characters.end(),
            [selected](const client::net::CharacterListItem& item) {
                return item.id == selected;
            });
        if (it == characters.end())
        {
            Fail("requested character id not found: " + std::to_string(selected));
            return;
        }

        Log("select character id=" + std::to_string(selected) + " name=" + it->name);
        phase = Phase::WaitingToken;
        session->SendCharacterSelect(selected);
    }

    void OnEnterWorldToken(std::vector<std::uint8_t> newToken,
                           const std::string& host,
                           std::uint16_t port) override
    {
        token = std::move(newToken);
        gameHost = host.empty() ? "127.0.0.1" : host;
        gamePort = port;
        if (token.empty())
        {
            Fail("empty handoff token");
            return;
        }
        handoffDisconnectExpected = true;
        Log("received handoff token bytes=" + std::to_string(token.size()));
        session->Disconnect();
    }

    void OnEnterWorldAccepted(std::uint32_t netId, client::net::Vec3 spawnPos) override
    {
        phase = Phase::InWorld;
        Log("enter world accepted net_id=" + std::to_string(netId) +
            " spawn=(" + std::to_string(spawnPos.x) + ", " +
            std::to_string(spawnPos.y) + ", " + std::to_string(spawnPos.z) + ")");
        session->SendMoveInput(0.0f, client::net::MoveState::Walking);
    }

    void OnEnterWorldRejected(const std::string& reason) override
    {
        Fail("enter world rejected: " + reason);
    }

    void OnEntitySpawn(const client::net::EntitySpawnInfo& entity) override
    {
        ++spawns;
        Log("entity spawn net_id=" + std::to_string(entity.netId) + " name=" + entity.name);
    }

    void OnEntityDespawn(std::uint32_t) override {}
    void OnEntityHealthUpdate(const client::net::EntityHealthInfo&) override {}
    void OnEntityDeath(std::uint32_t, std::uint32_t) override {}

    void OnEntityTransforms(std::uint32_t serverTick,
                            const std::vector<client::net::EntityTransform>& transforms) override
    {
        lastTransformTick = serverTick;
        transformPackets += 1;
        transformRecords += static_cast<int>(transforms.size());
    }

    int SpawnCount() const { return spawns; }
    int TransformPackets() const { return transformPackets; }
    int TransformRecords() const { return transformRecords; }
    std::uint32_t LastTransformTick() const { return lastTransformTick; }

private:
    void Log(const std::string& message)
    {
        std::cout << "[NETTEST] " << message << "\n";
    }

    void Fail(const std::string& message)
    {
        failure = message;
        phase = Phase::Failed;
        std::cerr << "[NETTEST] " << message << "\n";
    }

    const Options& options;
    client::net::ClientSession* session = nullptr;
    Phase phase = Phase::LoginConnect;
    std::string failure;
    std::vector<std::uint8_t> token;
    std::string gameHost = "159.195.56.82";
    std::uint16_t gamePort = 11020;
    bool handoffDisconnectExpected = false;
    int spawns = 0;
    int transformPackets = 0;
    int transformRecords = 0;
    std::uint32_t lastTransformTick = 0;
};

bool RunNetworkTest(const Options& options, TestContext& ctx)
{
    NetworkProbe probe(options);
    client::net::ClientSession session(probe);
    probe.Attach(&session);
    probe.Start();

    const auto deadline = Clock::now() + std::chrono::seconds(options.timeoutSeconds);
    while (!probe.Done() && Clock::now() < deadline)
    {
        session.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    session.Update();

    if (!probe.Success())
    {
        const std::string reason = probe.FailureReason().empty() ? "timed out" : probe.FailureReason();
        ctx.Fail("network login enter-world", reason);
        return false;
    }

    ctx.Pass("network login enter-world");
    std::cout << "[NETTEST] post-enter stats: spawns=" << probe.SpawnCount()
              << " transform_packets=" << probe.TransformPackets()
              << " transform_records=" << probe.TransformRecords()
              << " last_tick=" << probe.LastTransformTick() << "\n";
    return true;
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
        if (options.runNetwork)
            RunNetworkTest(options, ctx);

        std::cout << "[SUMMARY] passed=" << ctx.passed << " failed=" << ctx.failed << "\n";
        return ctx.failed == 0 ? 0 : 1;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FATAL] " << exception.what() << "\n";
        return 2;
    }
}
