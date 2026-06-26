#pragma once

#include "physics/PhysicsComponents.h"

#include <cstdint>
#include <cstddef>
#include <array>
#include <string>
#include <vector>

using PhysicsLayerMatrix = std::array<std::array<bool, static_cast<std::size_t>(ixtreeme::physics::PhysicsLayer::Count)>, static_cast<std::size_t>(ixtreeme::physics::PhysicsLayer::Count)>;

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

struct PhysicsSceneSettings
{
    float gravity[3] = {0.0f, -9.81f, 0.0f};
    float fixedDeltaSeconds = 1.0f / 60.0f;
    std::uint32_t maxSubsteps = 4;
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

// A game camera that lives in the scene as a hierarchy entity (Unity/Unreal style).
// Every scene has at least one; the SceneData::mainCameraId selects which one drives
// the Game view. rotation is Euler radians (pitch=X, yaw=Y, roll=Z), matching the
// convention used by the editor free-fly camera (see WorldForwardFromYawPitch).
struct CameraEntity
{
    std::uint32_t id = 0;
    std::string name = "Main Camera";
    std::string prefabAssetId;
    PrefabInstanceState prefabInstance;
    SceneParentRef parent;
    float position[3] = {0.0f, 8.0f, -18.0f};
    float rotation[3] = {-0.4363323f, 0.0f, 0.0f};
    float fovDegrees = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    bool editorHidden = false;
};

// Persisted editor free-fly viewpoint, so reopening a scene/project restores the
// camera where you left off (matches the Unreal editor-viewport behavior).
struct EditorCameraState
{
    float eye[3] = {0.0f, 8.0f, -18.0f};
    float yaw = 0.0f;
    float pitch = -0.4363323f;
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
    bool hasRigidbody = false;
    ixtreeme::physics::RigidbodyComponent rigidbody;
    bool hasCollider = false;
    ixtreeme::physics::ColliderComponent collider;
    bool hasFixedJoint = false;
    ixtreeme::physics::FixedJointComponent fixedJoint;
    bool hasHingeJoint = false;
    ixtreeme::physics::HingeJointComponent hingeJoint;
    bool hasCharacterController = false;
    ixtreeme::physics::CharacterControllerComponent characterController;
    // Stage-3 temporary clip binding (id of an AnimationClip asset, or empty). Runtime-only —
    // not serialized; replaced by the real Animator component in Stage 4.
    std::string debugAnimationClipId;
    // Stage-4 Animator: id of an AnimatorController asset (or empty). Runtime-only for now (full
    // AnimatorComponent + serialization is a follow-up); takes priority over debugAnimationClipId.
    std::string animatorControllerId;
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
    MeshEntity,
    Camera
};

enum class SceneGizmoTargetKind
{
    Object,
    ColliderCenter
};

enum class EditorComponentType
{
    None,
    WaterBody,
    PointLight,
    SpotLight,
    MeshRenderer,
    Rigidbody,
    BoxCollider,
    SphereCollider,
    CapsuleCollider,
    TriggerBox,
    TriggerSphere,
    TriggerCapsule,
    FixedJoint,
    HingeJoint,
    CharacterController
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
    bool hasRigidbody = false;
    ixtreeme::physics::RigidbodyComponent rigidbody;
    bool physicsRuntimeValid = false;
    bool physicsRuntimeActive = false;
    std::uint64_t physicsRuntimeBodyId = 0;
    float physicsRuntimeLinearVelocity[3] = {0.0f, 0.0f, 0.0f};
    float physicsRuntimeAngularVelocity[3] = {0.0f, 0.0f, 0.0f};
    bool hasCollider = false;
    ixtreeme::physics::ColliderComponent collider;
    bool hasFixedJoint = false;
    ixtreeme::physics::FixedJointComponent fixedJoint;
    bool hasHingeJoint = false;
    ixtreeme::physics::HingeJointComponent hingeJoint;
    bool hasCharacterController = false;
    ixtreeme::physics::CharacterControllerComponent characterController;
    // Stage-3 temporary clip binding (id of an AnimationClip asset, or empty). Runtime-only —
    // not serialized; replaced by the real Animator component in Stage 4.
    std::string debugAnimationClipId;
    // Stage-4 Animator: id of an AnimatorController asset (or empty). Runtime-only for now (full
    // AnimatorComponent + serialization is a follow-up); takes priority over debugAnimationClipId.
    std::string animatorControllerId;
};

// Stage-6 read-only snapshot of an AnimatorController for the node-graph editor panel. Filled by
// EngineApplication (which owns the ixanim types — libs/render must NOT depend on apps/client) and
// pushed via EditorImGui::SetAnimatorGraphEditorState each frame. kAnyState sentinel = 0xFFFFFFFF.
// Stage 7: mirrors of ixanim::ConditionOp / ixanim::ParamType. MapEditorTypes.h (libs/render) must
// NOT include the apps/client AnimatorController.h (one-way layering), so these enums are duplicated
// here with IDENTICAL ordinals; EngineApplication static_asserts them against the real enums.
enum class AnimEditConditionOp { Greater, Less, Equals, NotEquals, If, IfNot };
enum class AnimEditParamType { Float, Int, Bool, Trigger };

struct AnimatorGraphCondition
{
    std::string param;
    AnimEditConditionOp op = AnimEditConditionOp::Greater;
    float value = 0.0f;
};
struct AnimatorGraphParameter
{
    std::string name;
    AnimEditParamType type = AnimEditParamType::Float;
    float defaultValue = 0.0f;
};

struct AnimatorGraphNode
{
    std::uint32_t id = 0;            // AnimatorState::id (or 0xFFFFFFFF for Any-State, 0 for Entry)
    std::string name;
    std::string clipLabel;          // clip basename, "(no clip)", or empty for synthetic nodes
    std::string clipId;             // backing AnimationClip asset id (for the clip combo selection)
    float graphPos[2] = {0.0f, 0.0f};
    float speed = 1.0f;
    bool loop = true;
    bool isDefault = false;
    bool isAnyState = false;
    bool isEntry = false;
    bool isActive = false;          // Stage 7 live highlight (== current state in Play)
};
struct AnimatorGraphEdge
{
    std::uint32_t fromStateId = 0;  // 0xFFFFFFFF for Any-State transitions
    std::uint32_t toStateId = 0;
    bool isAnyState = false;
    int conditionCount = 0;
    bool active = false;            // Stage 7 live highlight (currently-taken transition)
    bool hasExitTime = false;       // full data for the transition editor (Stage 7)
    float exitTime = 0.0f;
    float duration = 0.15f;
    bool canTransitionToSelf = false;
    std::vector<AnimatorGraphCondition> conditions;
};
struct AnimatorGraphEditorState
{
    bool hasController = false;
    std::string controllerId;
    std::string controllerDisplayName;
    std::vector<AnimatorGraphNode> nodes;
    std::vector<AnimatorGraphEdge> edges;
    std::vector<AnimatorGraphParameter> parameters;
    std::uint32_t defaultStateId = 0;
    std::uint32_t activeStateId = 0;
};

// Stage 7: one user edit emitted by the Animator graph panel, applied by EngineApplication to the
// real ixanim::AnimatorController (then saved + re-bound). MoveNode carries a centroid-invariant
// DELTA, not an absolute position, because the display snapshot is re-centered every frame.
enum class AnimatorGraphEditType
{
    MoveNode, AddState, DeleteState, RenameState, AssignClip, SetStateSpeed, SetStateLoop,
    SetDefaultState, CreateTransition, DeleteTransition, EditTransition,
    AddParameter, DeleteParameter, RenameParameter, SetParameterType, SetParameterDefault
};
struct AnimatorGraphEdit
{
    AnimatorGraphEditType type = AnimatorGraphEditType::MoveNode;
    std::uint32_t stateId = 0;       // primary state / transition source (0xFFFFFFFF = Any-State)
    std::uint32_t toStateId = 0;     // transition target
    float graphDeltaX = 0.0f;        // MoveNode: centroid-invariant delta (graph units)
    float graphDeltaY = 0.0f;
    float floatValue = 0.0f;         // SetStateSpeed / SetParameterDefault
    bool boolValue = false;          // SetStateLoop
    std::string text;                // AddState/RenameState name, AssignClip clipId, param name
    std::string text2;               // RenameParameter new name
    AnimEditParamType paramType = AnimEditParamType::Float;
    bool hasExitTime = false;        // EditTransition payload:
    float exitTime = 0.0f;
    float duration = 0.15f;
    bool canTransitionToSelf = false;
    std::vector<AnimatorGraphCondition> conditions;
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

struct CameraEditorState
{
    bool selected = false;
    bool isMain = false;
    std::uint32_t cameraCount = 0;
    CameraEntity camera;
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
    // Frame timing breakdown (previous frame's CPU profile + present mode), for the
    // Performance panel so the cost source is visible without a debug-logs build.
    bool presentUncapped = true;
    double cpuTotalMs = 0.0;
    double cpuSceneRenderMs = 0.0;
    double cpuEditorUiMs = 0.0;
    double cpuSubmitPresentMs = 0.0;
    double cpuEcsUpdateMs = 0.0;
    double cpuAssetWatcherMs = 0.0;
};

struct PhysicsEventEditorState
{
    std::string phase;
    std::string kind;
    std::uint32_t entityA = 0;
    std::uint32_t entityB = 0;
    std::string nameA;
    std::string nameB;
    std::uint64_t bodyA = 0;
    std::uint64_t bodyB = 0;
    float point[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 1.0f, 0.0f};
    float penetrationDepth = 0.0f;
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
    bool showPhysicsColliders = false;
    bool showPhysicsContacts = false;
    bool showPhysicsBodyCenters = false;
    bool physicsRaycastFromCamera = false;
    std::uint32_t physicsRaycastLayerMask = ixtreeme::physics::AllPhysicsLayerMask();
    bool physicsRaycastHitTriggers = true;
    float physicsRaycastDistance = 500.0f;
    bool physicsOverlapSphereFromCamera = false;
    bool physicsOverlapBoxFromCamera = false;
    bool physicsOverlapCapsuleFromCamera = false;
    std::uint32_t physicsOverlapLayerMask = ixtreeme::physics::AllPhysicsLayerMask();
    bool physicsOverlapHitTriggers = true;
    float physicsOverlapDistance = 20.0f;
    float physicsOverlapRadius = 2.0f;
    float physicsOverlapBoxHalfExtents[3] = {1.0f, 1.0f, 1.0f};
    float physicsOverlapCapsuleRadius = 0.5f;
    float physicsOverlapCapsuleHeight = 2.0f;
    bool physicsSetLinearVelocityForSelected = false;
    bool physicsApplyForceToSelected = false;
    bool physicsApplyImpulseToSelected = false;
    bool physicsApplyAngularImpulseToSelected = false;
    std::uint32_t physicsRuntimeEntityId = 0;
    float physicsLinearVelocity[3] = {0.0f, 0.0f, 0.0f};
    float physicsForce[3] = {0.0f, 20.0f, 0.0f};
    float physicsImpulse[3] = {0.0f, 5.0f, 0.0f};
    float physicsAngularImpulse[3] = {0.0f, 1.0f, 0.0f};
    bool physicsLayerMatrixChanged = false;
    PhysicsLayerMatrix physicsLayerMatrix{};
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
    bool fitSelectedColliderToMesh = false;
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
    bool selectedCameraChanged = false;
    CameraEntity selectedCamera;
    bool setMainCameraRequested = false;
    std::uint32_t setMainCameraId = 0;
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
    SceneGizmoTargetKind sceneGizmoTarget = SceneGizmoTargetKind::Object;
    HierarchyEntityType sceneGizmoEntityType = HierarchyEntityType::None;
    std::uint32_t sceneGizmoEntityId = 0;
    MapEditorGizmoOperation sceneGizmoOperation = MapEditorGizmoOperation::Translate;
    float sceneGizmoPosition[3] = {0.0f, 0.0f, 0.0f};
    float sceneGizmoRotation[3] = {0.0f, 0.0f, 0.0f};
    float sceneGizmoScale[3] = {1.0f, 1.0f, 1.0f};
    std::vector<AnimatorGraphEdit> animatorEdits;  // Stage 7: Animator graph edits (append-merged)
};
