#include "ClientSession.h"
#include "AssetLibrary.h"
#include "MapEditorTypes.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

    std::string newSubpath;
    if (!ctx.Expect(library.RenameFolder(AssetLibrary::Category::Texture,
            "terrain/rock", "pebbles", newSubpath, error),
            "folder rename", error))
        return false;
    const auto renamedTexture = library.FindById(renamedDiffuse.id);
    ctx.Expect(renamedTexture && renamedTexture->subpath == "terrain/pebbles",
        "folder rename updates subpath", "texture subpath was not updated");

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
    ctx.Expect(waterSource.find("u_foamParams") != std::string::npos &&
            waterSource.find("FoamNoise") != std::string::npos,
        "water foam shader controls", "Water shader is missing WATER-4 foam controls");
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
    ctx.Expect(waterSource.find("finalColor = finalColor / (finalColor + 1.0.xxx)") == std::string::npos,
        "water local tone-map removed", "Water shader still performs local Reinhard tone-mapping");

    const std::filesystem::path terrainShaderPath = options.clientRoot / "shaders" / "Terrain.hlsl";
    std::ifstream terrainShader(terrainShaderPath);
    std::stringstream terrainShaderText;
    terrainShaderText << terrainShader.rdbuf();
    const std::string terrainSource = terrainShaderText.str();
    ctx.Expect(terrainSource.find("u_waterParams1") != std::string::npos &&
            terrainSource.find("CausticPattern") != std::string::npos,
        "terrain water caustic shader controls", "Terrain shader is missing WATER-4 water/caustic controls");
    ctx.Expect(terrainSource.find("terrainFoam") != std::string::npos &&
            terrainSource.find("terrainFoamDepth > 0.0") != std::string::npos,
        "terrain shore foam constrained", "Terrain foam must exist only in a water-under-surface shoreline band");
    ctx.Expect(terrainSource.find("float4(1.0, 0.92, 0.15") != std::string::npos,
        "terrain editor brush ring present", "Terrain shader is missing the yellow editor brush ring");
    ctx.Expect(terrainSource.find("float4(0.1, 0.55, 1.0") == std::string::npos &&
            terrainSource.find("float4(0.0, 1.0, 0.25") == std::string::npos &&
            terrainSource.find("float4(1.0, 0.08, 0.04") == std::string::npos,
        "terrain debug color layers removed", "Terrain shader still contains old blue/green/red debug layers");
    ctx.Expect(terrainSource.find("finalColor = finalColor / (finalColor + 1.0.xxx)") == std::string::npos,
        "terrain local tone-map removed", "Terrain shader still performs local Reinhard tone-mapping");

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
    std::string gameHost = "127.0.0.1";
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
