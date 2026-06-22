#pragma once

#include <cstdint>
#include <cstddef>
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
    float metallicStrength = 0.0f;
    float uvOffset[2] = {0.0f, 0.0f};
    float uvRotationDegrees = 0.0f;
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

struct PrefabInstanceState
{
    bool linked = false;
    std::string assetId;
    std::uint32_t localId = 1;
    bool preserveTransform = true;
    bool nameOverride = true;
};

struct SceneParentRef
{
    std::string type;
    std::uint32_t id = 0;

    bool IsValid() const { return !type.empty() && id != 0; }
};

struct PrefabAssetEntityNameEdit
{
    std::uint32_t localId = 0;
    std::uint32_t parentLocalId = 0;
    std::string type;
    std::string lightType;
    std::string name;
    std::string meshAssetId;
    std::string meshAssetPath;
    std::string prefabAssetId;
    std::vector<std::string> materialSlots;
    bool transformValid = false;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[3] = {0.0f, 0.0f, 0.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};
    bool lightValid = false;
    float color[3] = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float radius = 1.0f;
    float innerConeDegrees = 20.0f;
    float outerConeDegrees = 35.0f;
    bool enabled = true;
};

inline PrefabInstanceState MakePrefabInstanceState(const std::string& assetId)
{
    PrefabInstanceState state;
    state.linked = !assetId.empty();
    state.assetId = assetId;
    return state;
}

struct PointLight
{
    std::uint32_t id = 0;
    std::string name;
    std::string prefabAssetId;
    PrefabInstanceState prefabInstance;
    SceneParentRef parent;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float r = 1.0f;
    float g = 0.95f;
    float b = 0.75f;
    float intensity = 3.0f;
    float radius = 10.0f;
    bool enabled = true;
    bool editorHidden = false;
};

struct SpotLight
{
    std::uint32_t id = 0;
    std::string name;
    std::string prefabAssetId;
    PrefabInstanceState prefabInstance;
    SceneParentRef parent;
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
    bool editorHidden = false;
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
    bool editorHidden = false;
};

struct EditorAttachedComponent
{
    std::string type;
    std::string displayName;
    std::string category;
    std::string note;
};

struct LodConfig
{
    static constexpr std::uint32_t MaxLevels = 4;

    std::uint32_t levelCount = 4;
    float targetRatios[MaxLevels] = {1.0f, 0.5f, 0.2f, 0.06f};
    float distances[MaxLevels] = {0.0f, 30.0f, 80.0f, 200.0f};
    float hysteresisMeters = 5.0f;
};

struct LodComponent
{
    bool enabled = false;
    bool overrideAssetDefault = false;
    LodConfig config;
};

struct MeshSceneEntity
{
    std::uint32_t id = 0;
    std::string name;
    std::string prefabAssetId;
    PrefabInstanceState prefabInstance;
    SceneParentRef parent;
    std::string meshAssetId;
    std::string meshAssetPath;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[3] = {0.0f, 0.0f, 0.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};
    bool skinned = false;
    bool editorHidden = false;

    struct MaterialOverride
    {
        std::uint32_t slot = 0;
        bool enabled = false;
        float baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float metallic = 1.0f;
        float roughness = 1.0f;
        float normalStrength = 1.0f;
        float aoStrength = 1.0f;
        float emissive[3] = {0.0f, 0.0f, 0.0f};
        float emissiveIntensity = 0.0f;
        float uvTiling[2] = {1.0f, 1.0f};
        float uvOffset[2] = {0.0f, 0.0f};
    };
    std::vector<std::string> materialSlots;
    std::vector<MaterialOverride> materialOverrides;
    std::vector<EditorAttachedComponent> editorComponents;
    LodComponent lod;
};

struct TerrainSceneData
{
    bool exists = false;
    std::string name = "Terrain";
    float widthMeters = 100.0f;
    float depthMeters = 100.0f;
    float cellSizeMeters = 1.0f;
    std::uint32_t cellsX = 100;
    std::uint32_t cellsZ = 100;
    std::uint32_t chunkSizeCells = 64;
    std::string chunkManifestRef;
    std::string heightmapRef;
    std::string splatRef;
    std::string maskRef;
    std::vector<float> heightCmGrid;
    std::vector<std::uint8_t> splatABytes;
    std::vector<std::uint8_t> splatBBytes;
    bool triplanarEnabled = false;
    float triplanarSharpness = 4.0f;
    float triplanarSlopeThreshold = 0.18f;
    float triplanarSlopeTransition = 0.20f;
    bool editorHidden = false;
};

enum class HierarchyEntityType
{
    None,
    Terrain,
    WaterBody,
    PointLight,
    SpotLight,
    MeshEntity
};

enum class EditorComponentType
{
    None,
    WaterBody,
    PointLight,
    SpotLight,
    MeshRenderer
};

struct HierarchySceneEntity
{
    std::uint64_t entity = 0;
    std::uint64_t parent = 0;
    HierarchyEntityType type = HierarchyEntityType::None;
    std::uint32_t objectId = 0;
    std::string name;
    bool prefabRoot = false;
    std::string prefabAssetId;
    bool editorHidden = false;
    bool selected = false;
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

struct MeshRendererEditorState
{
    bool selected = false;
    std::uint32_t id = 0;
    std::uint32_t count = 0;
    std::string name;
    std::string prefabAssetId;
    PrefabInstanceState prefabInstance;
    std::vector<std::string> prefabOverrides;
    std::string meshAssetId;
    std::string meshAssetPath;
    std::string meshDisplayName;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[3] = {0.0f, 0.0f, 0.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};
    bool skinned = false;
    std::uint32_t materialSlotCount = 1;
    std::uint32_t selectedMaterialSlot = 0;
    std::vector<std::string> materialSlots;
    std::vector<MeshSceneEntity::MaterialOverride> materialOverrides;
    std::vector<EditorAttachedComponent> editorComponents;
    LodComponent lod;
};

struct TerrainEditorState
{
    bool selected = false;
    bool exists = false;
    std::string name;
    float widthMeters = 0.0f;
    float depthMeters = 0.0f;
    float cellSizeMeters = 1.0f;
    std::uint32_t cellsX = 0;
    std::uint32_t cellsZ = 0;
    bool triplanarEnabled = false;
    float triplanarSharpness = 4.0f;
    float triplanarSlopeThreshold = 0.18f;
    float triplanarSlopeTransition = 0.20f;
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
    std::vector<std::string> prefabOverrides;
    PointLight point;
    SpotLight spot;
};

struct EngineStats
{
    double fps = 0.0;
    double frameMs = 0.0;
    double averageFrameMs = 0.0;
    double minFrameMs = 0.0;
    double maxFrameMs = 0.0;
    double frameBudgetPercent = 0.0;
    double processCpuPercent = 0.0;
    std::uint32_t swapchainWidth = 0;
    std::uint32_t swapchainHeight = 0;
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    std::uint64_t frameNumber = 0;
    std::size_t sceneEntityCount = 0;
    std::size_t staticMeshSubmitted = 0;
    std::size_t staticMeshDrawCalls = 0;
};

struct MapEditorCommands
{
    bool save = false;
    bool reload = false;
    bool undo = false;
    bool dumpMaterialState = false;
    bool captureGpuFrame = false;
    bool dumpFrameProfile = false;
    bool debugPerfTogglesChanged = false;
    bool disableShadowPass = false;
    bool disableWaterReflectionPass = false;
    bool disableAssetLibraryDiscovery = false;
    bool disableAssetWatcherPoll = false;
    bool disableHierarchyIteration = false;
    bool renderResolutionChanged = false;
    bool renderResolutionUseNative = true;
    std::uint32_t renderResolutionWidth = 0;
    std::uint32_t renderResolutionHeight = 0;
    bool enterPlayMode = false;
    bool exitPlayMode = false;
    bool pausePlayMode = false;
    bool resumePlayMode = false;
    bool addWaterBody = false;
    bool createTerrain = false;
    TerrainSceneData terrainCreate;
    bool addMeshEntity = false;
    std::string meshAssetId;
    bool meshDropScreenPositionValid = false;
    float meshDropScreenPosition[2] = {0.0f, 0.0f};
    bool addPrimitiveEntity = false;
    std::string primitiveType;
    bool addPrefabInstance = false;
    std::string prefabAssetId;
    bool prefabDropScreenPositionValid = false;
    float prefabDropScreenPosition[2] = {0.0f, 0.0f};
    bool createPrefabFromSelection = false;
    bool refreshSelectedPrefabInstance = false;
    bool refreshAllPrefabInstances = false;
    bool revertSelectedPrefabInstance = false;
    bool applySelectedPrefabToAsset = false;
    bool revertSelectedPrefabOverride = false;
    bool applySelectedPrefabOverrideToAsset = false;
    std::string selectedPrefabOverrideName;
    bool unpackSelectedPrefabInstance = false;
    bool savePrefabAssetEdit = false;
    std::string editPrefabAssetId;
    std::string editPrefabName;
    std::vector<PrefabAssetEntityNameEdit> editPrefabEntityNames;
    bool addComponentToSelectedEntity = false;
    EditorComponentType addComponentType = EditorComponentType::None;
    std::string addComponentTypeId;
    bool removeComponentFromSelectedEntity = false;
    std::string removeComponentTypeId;
    bool lodQualityCommitRequested = false;
    std::uint32_t lodQualityCommitEntityId = 0;
    LodConfig lodQualityCommitConfig;
    bool assignMeshAssetToSelectedEntity = false;
    std::string assignMeshAssetId;
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
    bool deleteSelectedMeshEntity = false;
    bool selectedMeshEntityChanged = false;
    MeshRendererEditorState selectedMeshEntity;
    bool hierarchySelectEntity = false;
    bool hierarchyFocusEntity = false;
    bool hierarchyDeleteEntity = false;
    bool hierarchyDuplicateEntity = false;
    bool hierarchyRenameEntity = false;
    bool hierarchyToggleHidden = false;
    bool hierarchyReparentEntity = false;
    HierarchyEntityType hierarchyEntityType = HierarchyEntityType::None;
    std::uint32_t hierarchyEntityId = 0;
    std::uint64_t hierarchyEntityHandle = 0;
    HierarchyEntityType hierarchyParentType = HierarchyEntityType::None;
    std::uint32_t hierarchyParentId = 0;
    std::string hierarchyRenameValue;
    bool exportMeshEntityToFbx = false;
    std::uint32_t exportMeshEntityId = 0;
    std::string exportFbxOutputPath;
    bool exportFbxEmbedTextures = true;
    bool exportFbxMaterials = true;
    bool exportFbxAnimations = true;
    bool paletteSlotChanged = false;
    bool paletteSlotParamsChanged = false;
    std::uint32_t paletteSlot = 0;
    std::string paletteAssetId;
    std::string paletteTexturePath;
    MapEditorPaletteSlot paletteSlotData;
    bool terrainTriplanarChanged = false;
    bool terrainTriplanarEnabled = false;
    float terrainTriplanarSharpness = 4.0f;
    float terrainTriplanarSlopeThreshold = 0.18f;
    float terrainTriplanarSlopeTransition = 0.20f;
    bool gizmoSettingsChanged = false;
    MapEditorGizmoOperation gizmoOperation = MapEditorGizmoOperation::Translate;
    bool gizmoSnapEnabled = false;
    float gizmoSnapValue = 1.0f;
    bool sceneGizmoTransformChanged = false;
    HierarchyEntityType sceneGizmoEntityType = HierarchyEntityType::None;
    std::uint32_t sceneGizmoEntityId = 0;
    float sceneGizmoPosition[3] = {0.0f, 0.0f, 0.0f};
    float sceneGizmoRotation[3] = {0.0f, 0.0f, 0.0f};
    float sceneGizmoScale[3] = {1.0f, 1.0f, 1.0f};
};
