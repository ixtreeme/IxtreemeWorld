#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>

enum class MapEditorTool
{
    Raise,
    Lower,
    Smooth,
    Flatten,
    Paint
};

enum class MapEditorPaintMode
{
    Replace,
    Mix
};

enum class MapEditorToolMode
{
    None,
    WaterSculpt,
    Heightmap,
    SplatPaint
};

enum class EditorPlayMode
{
    Edit,
    Play,
    PlayPaused
};

struct EditorPlayModeState
{
    EditorPlayMode mode = EditorPlayMode::Edit;
    int frameCount = 0;
    double elapsedSeconds = 0.0;
};

struct MapEditorSettings
{
    MapEditorToolMode toolMode = MapEditorToolMode::None;
    MapEditorTool tool = MapEditorTool::Raise;
    float brushRadiusMeters = 5.0f;
    float brushStrength = 1.0f;
    float brushFalloff = 1.0f;
    float flattenTargetY = 0.0f;
    std::uint32_t textureSlot = 4;
    MapEditorPaintMode paintMode = MapEditorPaintMode::Replace;
    bool waterSculptActive = false;
    bool waterSculptAdd = true;
    float waterSculptRadiusMeters = 3.0f;
};

struct MapEditorPaletteSlot
{
    std::uint32_t slot = 0;
    std::string assetId;
    std::string displayName;
    std::string texturePath;
    std::string normalTexturePath;
    std::string aoTexturePath;
    std::string roughnessTexturePath;
    std::string metallicTexturePath;
    std::string heightTexturePath;
    float tilingScaleX = 1.0f;
    float tilingScaleY = 1.0f;
    float colorTint[3] = {1.0f, 1.0f, 1.0f};
    float normalStrength = 1.0f;
    float aoStrength = 1.0f;
    float roughnessStrength = 1.0f;
    float metallicStrength = 1.0f;
};

struct DirectionalLight
{
    float azimuthDegrees = 60.0f;
    float elevationDegrees = 60.0f;
    float intensity = 1.0f;
    float r = 1.0f;
    float g = 0.95f;
    float b = 0.85f;
    bool enabled = true;
};

struct AmbientLight
{
    float r = 0.15f;
    float g = 0.18f;
    float b = 0.25f;
    float intensity = 1.0f;
};

constexpr std::uint32_t kMaxDynamicPointLights = 16;
constexpr std::uint32_t kMaxDynamicSpotLights = 16;

struct PointLight
{
    std::uint32_t id = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float r = 1.0f;
    float g = 0.95f;
    float b = 0.75f;
    float intensity = 3.0f;
    float radius = 10.0f;
    bool enabled = true;
};

struct SpotLight
{
    std::uint32_t id = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[3] = {-1.5708f, 0.0f, 0.0f};
    float r = 1.0f;
    float g = 1.0f;
    float b = 0.9f;
    float intensity = 5.0f;
    float radius = 20.0f;
    float innerConeDegrees = 20.0f;
    float outerConeDegrees = 35.0f;
    bool enabled = true;
};

struct LightingState
{
    DirectionalLight directional;
    AmbientLight ambient;
    bool sunShadowsEnabled = true;
    std::uint32_t numPointLights = 0;
    std::uint32_t numSpotLights = 0;
    std::array<PointLight, kMaxDynamicPointLights> pointLights{};
    std::array<SpotLight, kMaxDynamicSpotLights> spotLights{};
};

struct WaterConfig
{
    enum class ReflectionQuality : std::int32_t
    {
        Quarter = 0,
        Half = 1,
        Full = 2
    };

    enum class CausticMode : std::int32_t
    {
        Off = 0,
        AnimatedTexture = 1,
        Procedural = 2
    };

    enum class EdgeFadeCurve : std::int32_t
    {
        Linear = 0,
        Smooth = 1,
        Exponential = 2
    };

    bool enabled = true;
    float waterLevelY = 0.0f;
    float baseColor[4] = {0.10f, 0.35f, 0.55f, 0.85f};
    float waveScaleSmall = 0.08f;
    float waveScaleLarge = 0.03f;
    float waveSpeedSmall = 0.03f;
    float waveSpeedLarge = 0.015f;
    float normalStrength = 0.6f;
    float fresnelPower = 5.0f;
    float fresnelMin = 0.05f;
    float reflectionColor[3] = {0.55f, 0.70f, 0.85f};
    bool reflectionEnabled = true;
    ReflectionQuality reflectionQuality = ReflectionQuality::Half;
    float reflectionDistortionStrength = 0.04f;
    bool refractionEnabled = true;
    float shallowColor[3] = {0.36f, 0.88f, 0.78f};
    float deepColor[3] = {0.04f, 0.17f, 0.35f};
    float depthColorMin = 0.5f;
    float depthColorMax = 8.0f;
    float depthFadeDistance = 12.0f;
    float refractionStrength = 0.018f;
    float refractionDepthStrength = 1.0f;
    bool foamEnabled = true;
    float foamDistance = 0.08f;
    float foamSoftness = 0.035f;
    float foamIntensity = 0.65f;
    float foamScrollSpeed = 0.02f;
    float foamScale = 2.0f;
    float foamTerrainThickness = 0.08f;
    CausticMode causticMode = CausticMode::AnimatedTexture;
    float causticIntensity = 0.7f;
    float causticScale = 0.3f;
    float causticSpeed = 0.5f;
    float causticMaxDepth = 8.0f;
    float edgeFadeDistance = 1.0f;
    EdgeFadeCurve edgeFadeCurve = EdgeFadeCurve::Smooth;
};

struct WaterMaterialData
{
    WaterConfig config;
    std::string normalMapA;
    std::string normalMapB;
    std::string diffuseMap;
    float scrollSpeedA[2] = {0.0f, 0.0f};
    float scrollSpeedB[2] = {0.0f, 0.0f};
    float normalTiling = 1.0f;
    std::uint32_t formatVersion = 1;
};

struct WaterBody
{
    std::uint32_t id = 0;
    std::string name;
    float bboxMin[2] = {0.0f, 0.0f};
    float bboxMax[2] = {0.0f, 0.0f};
    float waterLevelY = 0.0f;
    std::uint32_t maskWidth = 0;
    std::uint32_t maskHeight = 0;
    std::vector<std::uint8_t> shapeMask;
    std::string materialId;
    WaterConfig config;
};

struct WaterBodyEditorState
{
    bool selected = false;
    std::uint32_t id = 0;
    std::uint32_t count = 0;
    std::string name;
    float center[3] = {0.0f, 0.0f, 0.0f};
    float width = 10.0f;
    float depth = 10.0f;
    std::string materialId;
    std::string materialName;
    WaterConfig config;
};

enum class DynamicLightType
{
    None,
    Point,
    Spot
};

enum class MapEditorGizmoOperation
{
    Translate,
    Rotate,
    Scale
};

struct DynamicLightEditorState
{
    DynamicLightType type = DynamicLightType::None;
    std::uint32_t id = 0;
    std::uint32_t pointCount = 0;
    std::uint32_t spotCount = 0;
    PointLight point;
    SpotLight spot;
};

struct MapEditorCommands
{
    bool save = false;
    bool reload = false;
    bool undo = false;
    bool enterPlayMode = false;
    bool exitPlayMode = false;
    bool pausePlayMode = false;
    bool resumePlayMode = false;
    bool addWaterBody = false;
    bool deleteSelectedWaterBody = false;
    bool selectedWaterBodyChanged = false;
    bool openSelectedWaterMaterialEditor = false;
    bool waterMaterialDeleted = false;
    std::string deletedWaterMaterialId;
    WaterBodyEditorState selectedWaterBody;
    bool addPointLight = false;
    bool addSpotLight = false;
    bool deleteSelectedLight = false;
    bool selectedLightChanged = false;
    DynamicLightEditorState selectedLight;
    bool paletteSlotChanged = false;
    std::uint32_t paletteSlot = 0;
    std::string paletteAssetId;
    std::string paletteTexturePath;
    MapEditorPaletteSlot paletteSlotData;
    bool gizmoSettingsChanged = false;
    MapEditorGizmoOperation gizmoOperation = MapEditorGizmoOperation::Translate;
    bool gizmoSnapEnabled = false;
    float gizmoSnapValue = 1.0f;
};
