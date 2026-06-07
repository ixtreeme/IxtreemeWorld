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

    const std::filesystem::path noesisLayerPath = options.clientRoot / "libs" / "render" / "NoesisLayer.cpp";
    std::ifstream noesisLayer(noesisLayerPath);
    std::stringstream noesisLayerText;
    noesisLayerText << noesisLayer.rdbuf();
    const std::string noesisLayerSource = noesisLayerText.str();
    const std::filesystem::path noesisLayerHeaderPath = options.clientRoot / "libs" / "render" / "NoesisLayer.h";
    std::ifstream noesisLayerHeader(noesisLayerHeaderPath);
    std::stringstream noesisLayerHeaderText;
    noesisLayerHeaderText << noesisLayerHeader.rdbuf();
    const std::string noesisLayerHeaderSource = noesisLayerHeaderText.str();
    const std::filesystem::path editorImGuiPath = options.clientRoot / "libs" / "render" / "EditorImGui.cpp";
    std::ifstream editorImGui(editorImGuiPath);
    std::stringstream editorImGuiText;
    editorImGuiText << editorImGui.rdbuf();
    const std::string editorImGuiSource = editorImGuiText.str();
    const std::filesystem::path rmlUiLayerPath = options.clientRoot / "libs" / "render" / "RmlUiLayer.cpp";
    std::ifstream rmlUiLayer(rmlUiLayerPath);
    std::stringstream rmlUiLayerText;
    rmlUiLayerText << rmlUiLayer.rdbuf();
    const std::string rmlUiLayerSource = rmlUiLayerText.str();
    const std::filesystem::path rmlUiShaderPath = options.clientRoot / "shaders" / "RmlUi.hlsl";
    std::ifstream rmlUiShader(rmlUiShaderPath);
    std::stringstream rmlUiShaderText;
    rmlUiShaderText << rmlUiShader.rdbuf();
    const std::string rmlUiShaderSource = rmlUiShaderText.str();
    const std::filesystem::path helloRmlPath = options.clientRoot / "assets" / "ui" / "hello.rml";
    std::ifstream helloRml(helloRmlPath);
    std::stringstream helloRmlText;
    helloRmlText << helloRml.rdbuf();
    const std::string helloRmlSource = helloRmlText.str();
    const std::filesystem::path helloRcssPath = options.clientRoot / "assets" / "ui" / "hello.rcss";
    std::ifstream helloRcss(helloRcssPath);
    std::stringstream helloRcssText;
    helloRcssText << helloRcss.rdbuf();
    const std::string helloRcssSource = helloRcssText.str();
    const std::filesystem::path editorPanelPath = options.clientRoot / "assets" / "xaml" / "EditorPanel.xaml";
    std::ifstream editorPanel(editorPanelPath);
    std::stringstream editorPanelText;
    editorPanelText << editorPanel.rdbuf();
    const std::string editorPanelSource = editorPanelText.str();
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
            renderCmakeSource.find("RmlUi::RmlUi") != std::string::npos &&
            clientMainSource.find("RmlUiLayer rmlUi") != std::string::npos &&
            clientMainSource.find("noesis.RenderOnscreen(device);\n            rmlUi.Render(device);") != std::string::npos &&
            clientMainSource.find("editorImGui.Render(device);") != std::string::npos,
        "rmlui build and z-order pipeline", "RMLUI-1 must link RmlUi 6.2, compile shaders, and render between Noesis and ImGui");
    ctx.Expect(rmlUiLayerSource.find("class RmlAssetFileInterface") != std::string::npos &&
            rmlUiLayerSource.find("class RmlRenderInterface final : public Rml::RenderInterface") != std::string::npos &&
            rmlUiLayerSource.find("CreateDescriptorPool") != std::string::npos &&
            rmlUiLayerSource.find("ProcessMouseButtonDown") != std::string::npos &&
            rmlUiLayerSource.find("Event: type=%s element=test-button") != std::string::npos &&
            rmlUiLayerSource.find("assets/ui/hello.rml") != std::string::npos &&
            rmlUiLayerSource.find("[RMLUI-DIAG] CreateContext: viewport=") != std::string::npos &&
            rmlUiLayerSource.find("[RMLUI-DIAG] RenderGeometry: vertices=") != std::string::npos &&
            rmlUiLayerSource.find("[RMLUI-DIAG] vkCmdSetViewport") != std::string::npos &&
            rmlUiLayerSource.find("[RMLUI-DIAG] Pipeline primitive topology: TRIANGLE_LIST") != std::string::npos &&
            rmlUiLayerSource.find("PendingGeometryDelete") != std::string::npos &&
            rmlUiLayerSource.find("m_pendingGeometryDeletes") != std::string::npos &&
            rmlUiShaderSource.find("[[vk::push_constant]]") != std::string::npos &&
            rmlUiShaderSource.find("[[vk::binding(0, 0)]] Texture2D") != std::string::npos &&
            rmlUiShaderSource.find("(pixel.y / g_push.viewport.y) * 2.0f - 1.0f") != std::string::npos,
        "rmlui vulkan layer source", "RMLUI-1 must provide file, render, input, click-event, and Vulkan shader integration");
    ctx.Expect(helloRmlSource.find("screen-root") != std::string::npos &&
            helloRmlSource.find("RmlUi v6.2 active") != std::string::npos &&
            helloRmlSource.find("test-button") != std::string::npos &&
            helloRcssSource.find(".screen-root") != std::string::npos &&
            helloRcssSource.find("right: 0") != std::string::npos &&
            helloRcssSource.find("bottom: 0") != std::string::npos &&
            helloRcssSource.find("background-color: transparent") != std::string::npos &&
            helloRcssSource.find("background-color: rgba(24, 28, 44, 0.96)") != std::string::npos &&
            helloRcssSource.find("linear-gradient") != std::string::npos &&
            helloRcssSource.find("box-shadow") != std::string::npos &&
            helloRcssSource.find("border-radius") != std::string::npos &&
            helloRcssSource.find("transition: background") != std::string::npos,
        "rmlui hello document styling", "RMLUI-1 hello panel must prove RML/RCSS styling and button markup");
    ctx.Expect(editorPanelSource.find("AddWaterBodyButton") != std::string::npos &&
            editorPanelSource.find("SelectedWaterBodySection") != std::string::npos &&
            noesisLayerSource.find("OnAddWaterBodyClicked") != std::string::npos &&
            noesisLayerSource.find("SetWaterBodyEditorState") != std::string::npos,
        "water object editor UI source", "WATER-OBJ-3 editor button/inspector binding is missing");
    ctx.Expect(editorPanelSource.find("SelectedWaterSculptButton") == std::string::npos &&
            editorPanelSource.find("BrushRadiusSlider") == std::string::npos &&
            editorPanelSource.find("ToolPaint") == std::string::npos &&
            editorPanelSource.find("Slot0Button") == std::string::npos &&
            editorImGuiSource.find("RenderWaterSculptToolPanel") != std::string::npos &&
            editorImGuiSource.find("RenderHeightmapToolPanel") != std::string::npos &&
            editorImGuiSource.find("RenderSplatPaintToolPanel") != std::string::npos &&
            editorImGuiSource.find("ImportAssetWithDialog") != std::string::npos &&
            editorImGuiSource.find("PickAssetFileForImport") != std::string::npos &&
            clientMainSource.find("editorSettings.toolMode == MapEditorToolMode::Heightmap") != std::string::npos &&
            clientMainSource.find("editorSettings.toolMode == MapEditorToolMode::SplatPaint") != std::string::npos,
        "editor tool ui moved to imgui", "EDITOR-IMGUI-5 tool panels/import routing are missing or Noesis tool XAML remains");
    ctx.Expect(editorImGuiSource.find("RenderWaterMaterialEdgeFadeSection") != std::string::npos &&
            editorImGuiSource.find("SliderFloat(\"Edge Fade Distance\"") != std::string::npos &&
            editorImGuiSource.find("Combo(\"Edge Fade Curve\"") != std::string::npos &&
            editorImGuiSource.find("WaterConfig::EdgeFadeCurve") != std::string::npos,
        "water edge fade imgui material editor source", "WATER-OBJ-6 Edge Fade material controls are missing from ImGui");
    const auto selectedWaterSectionPos = editorPanelSource.find("SelectedWaterBodySection");
    const auto dynamicLightsSectionPos = editorPanelSource.find("DynamicLightsSectionButton");
    ctx.Expect(selectedWaterSectionPos != std::string::npos &&
            dynamicLightsSectionPos != std::string::npos &&
            editorPanelSource.find("WaterMaterialEditorSection") == std::string::npos &&
            editorPanelSource.find("WaterBaseSectionButton") == std::string::npos &&
            editorPanelSource.find("MaterialDiffuseButton") == std::string::npos &&
            editorImGuiSource.find("RenderWaterMaterialEditor") != std::string::npos &&
            editorImGuiSource.find("RenderPbrMaterialEditor") != std::string::npos &&
            editorImGuiSource.find("RenderWaterTextureSlot") != std::string::npos &&
            editorImGuiSource.find("AcceptDragDropPayload(kAssetPayloadType)") != std::string::npos &&
            dynamicLightsSectionPos < selectedWaterSectionPos &&
            clientMainSource.find("editorImGui.OpenWaterMaterialEditor(materialId)") != std::string::npos,
        "material editors moved to imgui", "Noesis material editor UI must be removed and ImGui material editors must handle water/PBR editing");
    ctx.Expect(noesisLayerSource.find("WaterConfig waterConfig;") == std::string::npos &&
            noesisLayerSource.find("EditedWaterConfig()") != std::string::npos &&
            noesisLayerHeaderSource.find("GetWaterConfig") == std::string::npos,
        "legacy global water UI state removed", "NoesisLayer still exposes or stores global WaterConfig state");
    ctx.Expect(noesisLayerSource.find("BuildAssetLibrarySignature") != std::string::npos &&
            noesisLayerSource.find("PollAssetLibraryChanges") != std::string::npos &&
            noesisLayerSource.find("assets.RootPath()") != std::string::npos &&
            noesisLayerSource.find("assetReaderRoot.empty() ? FindClientRoot() : assetReaderRoot") != std::string::npos &&
            noesisLayerSource.find("assetLibrary->Refresh(error)") != std::string::npos &&
            noesisLayerSource.find("RefreshAssetBrowser()") != std::string::npos,
        "asset browser realtime filesystem refresh", "Asset browser must poll assets/library and refresh when files or folders change");
    ctx.Expect(noesisLayerSource.find("entry.category != AssetLibrary::Category::WaterMaterial &&") != std::string::npos &&
            noesisLayerSource.find("entry.category != AssetLibrary::Category::Material") != std::string::npos &&
            noesisLayerSource.find("converted.normalMapA = texturePathForId(entry.material.normalTextureId)") != std::string::npos &&
            noesisLayerSource.find("materials.push_back({entry.id, converted})") != std::string::npos,
        "water body accepts material assets", "Water bodies must accept PBR material assets and convert their textures for water rendering");
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
