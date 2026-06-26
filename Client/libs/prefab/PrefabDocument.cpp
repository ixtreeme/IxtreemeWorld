#include "PrefabDocument.h"

#include "Common.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace ixtreeme::prefab
{
namespace
{
std::uint32_t JsonU32Value(const std::string& object, const std::string& key, std::uint32_t fallback = 0)
{
    const float value = ixtreeme::common::JsonFloatValue(object, key, static_cast<float>(fallback));
    return value < 0.0f ? fallback : static_cast<std::uint32_t>(value);
}

std::size_t FindMatchingBracket(const std::string& text, std::size_t open, char openCh, char closeCh)
{
    bool inString = false;
    bool escaping = false;
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i)
    {
        const char ch = text[i];
        if (inString)
        {
            if (escaping)
            {
                escaping = false;
            }
            else if (ch == '\\')
            {
                escaping = true;
            }
            else if (ch == '"')
            {
                inString = false;
            }
            continue;
        }
        if (ch == '"')
        {
            inString = true;
            continue;
        }
        if (ch == openCh)
            ++depth;
        else if (ch == closeCh)
        {
            --depth;
            if (depth == 0)
                return i;
        }
    }
    return std::string::npos;
}

std::string ExtractNamedObject(const std::string& text, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    const std::size_t keyPos = text.find(needle);
    if (keyPos == std::string::npos)
        return {};
    const std::size_t open = text.find('{', keyPos + needle.size());
    if (open == std::string::npos)
        return {};
    const std::size_t close = FindMatchingBracket(text, open, '{', '}');
    return close == std::string::npos ? std::string{} : text.substr(open, close - open + 1);
}

std::vector<std::string> ExtractNamedArrayObjects(const std::string& text, const std::string& key)
{
    std::vector<std::string> objects;
    const std::string needle = "\"" + key + "\"";
    const std::size_t keyPos = text.find(needle);
    if (keyPos == std::string::npos)
        return objects;
    const std::size_t openArray = text.find('[', keyPos + needle.size());
    if (openArray == std::string::npos)
        return objects;
    const std::size_t closeArray = FindMatchingBracket(text, openArray, '[', ']');
    if (closeArray == std::string::npos)
        return objects;

    std::size_t cursor = openArray + 1;
    while (cursor < closeArray)
    {
        const std::size_t openObject = text.find('{', cursor);
        if (openObject == std::string::npos || openObject >= closeArray)
            break;
        const std::size_t closeObject = FindMatchingBracket(text, openObject, '{', '}');
        if (closeObject == std::string::npos || closeObject > closeArray)
            break;
        objects.push_back(text.substr(openObject, closeObject - openObject + 1));
        cursor = closeObject + 1;
    }
    return objects;
}

void WriteMaterials(std::ostream& out, const std::vector<std::string>& materials, const std::string& indent)
{
    if (materials.empty())
        return;
    out << ",\n";
    out << indent << "\"materials\": [";
    for (std::size_t i = 0; i < materials.size(); ++i)
    {
        if (i)
            out << ", ";
        out << "\"" << ixtreeme::common::EscapeJson(materials[i]) << "\"";
    }
    out << "]";
}

void WriteMaterialOverride(std::ostream& out,
                           const MeshSceneEntity::MaterialOverride& material,
                           const std::string& indent,
                           bool comma)
{
    out << indent << "{\n";
    out << indent << "  \"slot\": " << material.slot << ",\n";
    out << indent << "  \"enabled\": " << (material.enabled ? "true" : "false") << ",\n";
    out << indent << "  \"base_color\": " << FloatArray(material.baseColor, 4) << ",\n";
    out << indent << "  \"metallic\": " << material.metallic << ",\n";
    out << indent << "  \"roughness\": " << material.roughness << ",\n";
    out << indent << "  \"normal_strength\": " << material.normalStrength << ",\n";
    out << indent << "  \"ao_strength\": " << material.aoStrength << ",\n";
    out << indent << "  \"emissive\": " << FloatArray(material.emissive, 3) << ",\n";
    out << indent << "  \"emissive_intensity\": " << material.emissiveIntensity << ",\n";
    out << indent << "  \"uv_tiling\": " << FloatArray(material.uvTiling, 2) << ",\n";
    out << indent << "  \"uv_offset\": " << FloatArray(material.uvOffset, 2) << "\n";
    out << indent << "}" << (comma ? "," : "") << "\n";
}

void WriteMaterialOverrides(std::ostream& out,
                            const std::vector<MeshSceneEntity::MaterialOverride>& overrides,
                            const std::string& indent)
{
    if (overrides.empty())
        return;
    out << ",\n";
    out << indent << "\"material_overrides\": [\n";
    for (std::size_t i = 0; i < overrides.size(); ++i)
        WriteMaterialOverride(out, overrides[i], indent + "  ", i + 1 < overrides.size());
    out << indent << "]";
}

std::string BoolArray(const bool* values, std::size_t count)
{
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i)
            out << ", ";
        out << (values[i] ? "true" : "false");
    }
    out << "]";
    return out.str();
}

void WriteRigidbody(std::ostream& out, const ixtreeme::physics::RigidbodyComponent& rigidbody, const std::string& indent)
{
    out << ",\n";
    out << indent << "\"rigidbody\": {\n";
    out << indent << "  \"enabled\": " << (rigidbody.enabled ? "true" : "false") << ",\n";
    out << indent << "  \"body_type\": \"" << ixtreeme::physics::ToString(rigidbody.bodyType) << "\",\n";
    out << indent << "  \"mass\": " << rigidbody.mass << ",\n";
    out << indent << "  \"linear_damping\": " << rigidbody.linearDamping << ",\n";
    out << indent << "  \"angular_damping\": " << rigidbody.angularDamping << ",\n";
    out << indent << "  \"use_gravity\": " << (rigidbody.useGravity ? "true" : "false") << ",\n";
    out << indent << "  \"allow_sleeping\": " << (rigidbody.allowSleeping ? "true" : "false") << ",\n";
    out << indent << "  \"continuous_collision\": " << (rigidbody.continuousCollision ? "true" : "false") << ",\n";
    out << indent << "  \"freeze_position\": " << BoolArray(rigidbody.freezePosition, 3) << ",\n";
    out << indent << "  \"freeze_rotation\": " << BoolArray(rigidbody.freezeRotation, 3) << "\n";
    out << indent << "}";
}

void WriteCollider(std::ostream& out, const ixtreeme::physics::ColliderComponent& collider, const std::string& indent)
{
    out << ",\n";
    out << indent << "\"collider\": {\n";
    out << indent << "  \"enabled\": " << (collider.enabled ? "true" : "false") << ",\n";
    out << indent << "  \"trigger\": " << (collider.trigger ? "true" : "false") << ",\n";
    out << indent << "  \"shape\": \"" << ixtreeme::physics::ToString(collider.shape) << "\",\n";
    out << indent << "  \"center\": " << FloatArray(collider.center, 3) << ",\n";
    out << indent << "  \"size\": " << FloatArray(collider.size, 3) << ",\n";
    out << indent << "  \"radius\": " << collider.radius << ",\n";
    out << indent << "  \"height\": " << collider.height << ",\n";
    out << indent << "  \"friction\": " << collider.friction << ",\n";
    out << indent << "  \"restitution\": " << collider.restitution << ",\n";
    out << indent << "  \"layer\": \"" << ixtreeme::physics::ToString(collider.layer) << "\",\n";
    out << indent << "  \"material_asset_id\": \"" << ixtreeme::common::EscapeJson(collider.materialAssetId) << "\"\n";
    out << indent << "}";
}

void WriteFixedJoint(std::ostream& out, const ixtreeme::physics::FixedJointComponent& joint, const std::string& indent)
{
    out << ",\n";
    out << indent << "\"fixed_joint\": {\n";
    out << indent << "  \"enabled\": " << (joint.enabled ? "true" : "false") << ",\n";
    out << indent << "  \"connected_entity_id\": " << joint.connectedEntityId << "\n";
    out << indent << "}";
}

void WriteHingeJoint(std::ostream& out, const ixtreeme::physics::HingeJointComponent& joint, const std::string& indent)
{
    out << ",\n";
    out << indent << "\"hinge_joint\": {\n";
    out << indent << "  \"enabled\": " << (joint.enabled ? "true" : "false") << ",\n";
    out << indent << "  \"connected_entity_id\": " << joint.connectedEntityId << ",\n";
    out << indent << "  \"anchor\": " << FloatArray(joint.anchor, 3) << ",\n";
    out << indent << "  \"axis\": " << FloatArray(joint.axis, 3) << ",\n";
    out << indent << "  \"limits_enabled\": " << (joint.limitsEnabled ? "true" : "false") << ",\n";
    out << indent << "  \"min_angle_deg\": " << joint.minAngleDegrees << ",\n";
    out << indent << "  \"max_angle_deg\": " << joint.maxAngleDegrees << ",\n";
    out << indent << "  \"friction_torque\": " << joint.frictionTorque << "\n";
    out << indent << "}";
}

void WriteCharacterController(std::ostream& out, const ixtreeme::physics::CharacterControllerComponent& cc, const std::string& indent)
{
    out << ",\n";
    out << indent << "\"character_controller\": {\n";
    out << indent << "  \"enabled\": " << (cc.enabled ? "true" : "false") << ",\n";
    out << indent << "  \"walk_speed\": " << cc.walkSpeed << ",\n";
    out << indent << "  \"run_speed\": " << cc.runSpeed << ",\n";
    out << indent << "  \"jump_height\": " << cc.jumpHeight << ",\n";
    out << indent << "  \"gravity_scale\": " << cc.gravityScale << ",\n";
    out << indent << "  \"slope_limit_deg\": " << cc.slopeLimitDegrees << ",\n";
    out << indent << "  \"step_height\": " << cc.stepHeight << ",\n";
    out << indent << "  \"capsule_radius\": " << cc.capsuleRadius << ",\n";
    out << indent << "  \"capsule_height\": " << cc.capsuleHeight << ",\n";
    out << indent << "  \"camera_mode\": \"" << ixtreeme::physics::ToString(cc.cameraMode) << "\",\n";
    out << indent << "  \"eye_height\": " << cc.eyeHeight << ",\n";
    out << indent << "  \"third_person_distance\": " << cc.thirdPersonDistance << ",\n";
    out << indent << "  \"third_person_height\": " << cc.thirdPersonHeight << ",\n";
    out << indent << "  \"third_person_pitch_deg\": " << cc.thirdPersonPitchDegrees << ",\n";
    out << indent << "  \"top_down_height\": " << cc.topDownHeight << ",\n";
    out << indent << "  \"top_down_pitch_deg\": " << cc.topDownPitchDegrees << ",\n";
    out << indent << "  \"mouse_sensitivity\": " << cc.mouseSensitivity << "\n";
    out << indent << "}";
}

void WriteAudioSource(std::ostream& out, const ixaudio::AudioSourceComponent& a, const std::string& indent)
{
    out << ",\n";
    out << indent << "\"audio_source\": {\n";
    out << indent << "  \"clip_asset_id\": \"" << ixtreeme::common::EscapeJson(a.clipAssetId) << "\",\n";
    out << indent << "  \"enabled\": " << (a.enabled ? "true" : "false") << ",\n";
    out << indent << "  \"loop\": " << (a.loop ? "true" : "false") << ",\n";
    out << indent << "  \"is_3d\": " << (a.is3d ? "true" : "false") << ",\n";
    out << indent << "  \"play_on_start\": " << (a.playOnStart ? "true" : "false") << ",\n";
    out << indent << "  \"volume\": " << a.volume << ",\n";
    out << indent << "  \"pitch\": " << a.pitch << ",\n";
    out << indent << "  \"min_distance\": " << a.minDistance << ",\n";
    out << indent << "  \"max_distance\": " << a.maxDistance << ",\n";
    out << indent << "  \"bus\": \"" << ixaudio::BusName(a.bus) << "\"\n";
    out << indent << "}";
}

ixtreeme::physics::RigidbodyComponent ReadRigidbody(const std::string& object)
{
    ixtreeme::physics::RigidbodyComponent rigidbody;
    const std::string component = ExtractNamedObject(object, "rigidbody");
    if (component.empty())
        return rigidbody;
    rigidbody.enabled = ixtreeme::common::JsonBoolValue(component, "enabled", rigidbody.enabled);
    rigidbody.bodyType = ixtreeme::physics::BodyTypeFromString(ixtreeme::common::JsonStringValue(component, "body_type"), rigidbody.bodyType);
    rigidbody.mass = ixtreeme::common::JsonFloatValue(component, "mass", rigidbody.mass);
    rigidbody.linearDamping = ixtreeme::common::JsonFloatValue(component, "linear_damping", rigidbody.linearDamping);
    rigidbody.angularDamping = ixtreeme::common::JsonFloatValue(component, "angular_damping", rigidbody.angularDamping);
    rigidbody.useGravity = ixtreeme::common::JsonBoolValue(component, "use_gravity", rigidbody.useGravity);
    rigidbody.allowSleeping = ixtreeme::common::JsonBoolValue(component, "allow_sleeping", rigidbody.allowSleeping);
    rigidbody.continuousCollision = ixtreeme::common::JsonBoolValue(component, "continuous_collision", rigidbody.continuousCollision);
    ixtreeme::common::JsonBoolArrayValue(component, "freeze_position", rigidbody.freezePosition, 3);
    ixtreeme::common::JsonBoolArrayValue(component, "freeze_rotation", rigidbody.freezeRotation, 3);
    ixtreeme::physics::Sanitize(rigidbody);
    return rigidbody;
}

ixtreeme::physics::ColliderComponent ReadCollider(const std::string& object)
{
    ixtreeme::physics::ColliderComponent collider;
    const std::string component = ExtractNamedObject(object, "collider");
    if (component.empty())
        return collider;
    collider.enabled = ixtreeme::common::JsonBoolValue(component, "enabled", collider.enabled);
    collider.trigger = ixtreeme::common::JsonBoolValue(component, "trigger", collider.trigger);
    collider.shape = ixtreeme::physics::ColliderShapeFromString(ixtreeme::common::JsonStringValue(component, "shape"), collider.shape);
    ixtreeme::common::JsonFloatArrayValue(component, "center", collider.center, 3);
    ixtreeme::common::JsonFloatArrayValue(component, "size", collider.size, 3);
    collider.radius = ixtreeme::common::JsonFloatValue(component, "radius", collider.radius);
    collider.height = ixtreeme::common::JsonFloatValue(component, "height", collider.height);
    collider.friction = ixtreeme::common::JsonFloatValue(component, "friction", collider.friction);
    collider.restitution = ixtreeme::common::JsonFloatValue(component, "restitution", collider.restitution);
    collider.layer = ixtreeme::physics::PhysicsLayerFromString(
        ixtreeme::common::JsonStringValue(component, "layer"),
        collider.layer);
    collider.materialAssetId = ixtreeme::common::JsonStringValue(component, "material_asset_id");
    ixtreeme::physics::Sanitize(collider);
    return collider;
}

ixtreeme::physics::FixedJointComponent ReadFixedJoint(const std::string& object)
{
    ixtreeme::physics::FixedJointComponent joint;
    const std::string component = ExtractNamedObject(object, "fixed_joint");
    if (component.empty())
        return joint;
    joint.enabled = ixtreeme::common::JsonBoolValue(component, "enabled", joint.enabled);
    joint.connectedEntityId = JsonU32Value(component, "connected_entity_id", joint.connectedEntityId);
    return joint;
}

ixtreeme::physics::HingeJointComponent ReadHingeJoint(const std::string& object)
{
    ixtreeme::physics::HingeJointComponent joint;
    const std::string component = ExtractNamedObject(object, "hinge_joint");
    if (component.empty())
        return joint;
    joint.enabled = ixtreeme::common::JsonBoolValue(component, "enabled", joint.enabled);
    joint.connectedEntityId = JsonU32Value(component, "connected_entity_id", joint.connectedEntityId);
    ixtreeme::common::JsonFloatArrayValue(component, "anchor", joint.anchor, 3);
    ixtreeme::common::JsonFloatArrayValue(component, "axis", joint.axis, 3);
    joint.limitsEnabled = ixtreeme::common::JsonBoolValue(component, "limits_enabled", joint.limitsEnabled);
    joint.minAngleDegrees = ixtreeme::common::JsonFloatValue(component, "min_angle_deg", joint.minAngleDegrees);
    joint.maxAngleDegrees = ixtreeme::common::JsonFloatValue(component, "max_angle_deg", joint.maxAngleDegrees);
    joint.frictionTorque = std::max(0.0f, ixtreeme::common::JsonFloatValue(component, "friction_torque", joint.frictionTorque));
    return joint;
}

ixtreeme::physics::CharacterControllerComponent ReadCharacterController(const std::string& object)
{
    ixtreeme::physics::CharacterControllerComponent cc;
    const std::string component = ExtractNamedObject(object, "character_controller");
    if (component.empty())
        return cc;
    cc.enabled = ixtreeme::common::JsonBoolValue(component, "enabled", cc.enabled);
    cc.walkSpeed = ixtreeme::common::JsonFloatValue(component, "walk_speed", cc.walkSpeed);
    cc.runSpeed = ixtreeme::common::JsonFloatValue(component, "run_speed", cc.runSpeed);
    cc.jumpHeight = ixtreeme::common::JsonFloatValue(component, "jump_height", cc.jumpHeight);
    cc.gravityScale = ixtreeme::common::JsonFloatValue(component, "gravity_scale", cc.gravityScale);
    cc.slopeLimitDegrees = ixtreeme::common::JsonFloatValue(component, "slope_limit_deg", cc.slopeLimitDegrees);
    cc.stepHeight = ixtreeme::common::JsonFloatValue(component, "step_height", cc.stepHeight);
    cc.capsuleRadius = ixtreeme::common::JsonFloatValue(component, "capsule_radius", cc.capsuleRadius);
    cc.capsuleHeight = ixtreeme::common::JsonFloatValue(component, "capsule_height", cc.capsuleHeight);
    cc.cameraMode = ixtreeme::physics::CameraModeFromString(ixtreeme::common::JsonStringValue(component, "camera_mode"), cc.cameraMode);
    cc.eyeHeight = ixtreeme::common::JsonFloatValue(component, "eye_height", cc.eyeHeight);
    cc.thirdPersonDistance = ixtreeme::common::JsonFloatValue(component, "third_person_distance", cc.thirdPersonDistance);
    cc.thirdPersonHeight = ixtreeme::common::JsonFloatValue(component, "third_person_height", cc.thirdPersonHeight);
    cc.thirdPersonPitchDegrees = ixtreeme::common::JsonFloatValue(component, "third_person_pitch_deg", cc.thirdPersonPitchDegrees);
    cc.topDownHeight = ixtreeme::common::JsonFloatValue(component, "top_down_height", cc.topDownHeight);
    cc.topDownPitchDegrees = ixtreeme::common::JsonFloatValue(component, "top_down_pitch_deg", cc.topDownPitchDegrees);
    cc.mouseSensitivity = ixtreeme::common::JsonFloatValue(component, "mouse_sensitivity", cc.mouseSensitivity);
    ixtreeme::physics::Sanitize(cc);
    return cc;
}

ixaudio::AudioSourceComponent ReadAudioSource(const std::string& object)
{
    ixaudio::AudioSourceComponent a;
    const std::string component = ExtractNamedObject(object, "audio_source");
    if (component.empty())
        return a;
    a.clipAssetId = ixtreeme::common::JsonStringValue(component, "clip_asset_id");
    a.enabled = ixtreeme::common::JsonBoolValue(component, "enabled", a.enabled);
    a.loop = ixtreeme::common::JsonBoolValue(component, "loop", a.loop);
    a.is3d = ixtreeme::common::JsonBoolValue(component, "is_3d", a.is3d);
    a.playOnStart = ixtreeme::common::JsonBoolValue(component, "play_on_start", a.playOnStart);
    a.volume = ixtreeme::common::JsonFloatValue(component, "volume", a.volume);
    a.pitch = ixtreeme::common::JsonFloatValue(component, "pitch", a.pitch);
    a.minDistance = ixtreeme::common::JsonFloatValue(component, "min_distance", a.minDistance);
    a.maxDistance = ixtreeme::common::JsonFloatValue(component, "max_distance", a.maxDistance);
    a.bus = ixaudio::ParseBus(ixtreeme::common::JsonStringValue(component, "bus"));
    ixaudio::Sanitize(a);
    return a;
}

MeshSceneEntity::MaterialOverride ReadMaterialOverride(const std::string& object)
{
    MeshSceneEntity::MaterialOverride material;
    material.slot = JsonU32Value(object, "slot", material.slot);
    material.enabled = ixtreeme::common::JsonBoolValue(object, "enabled", material.enabled);
    ixtreeme::common::JsonFloatArrayValue(object, "base_color", material.baseColor, 4);
    material.metallic = ixtreeme::common::JsonFloatValue(object, "metallic", material.metallic);
    material.roughness = ixtreeme::common::JsonFloatValue(object, "roughness", material.roughness);
    material.normalStrength = ixtreeme::common::JsonFloatValue(object, "normal_strength", material.normalStrength);
    material.aoStrength = ixtreeme::common::JsonFloatValue(object, "ao_strength", material.aoStrength);
    ixtreeme::common::JsonFloatArrayValue(object, "emissive", material.emissive, 3);
    material.emissiveIntensity = ixtreeme::common::JsonFloatValue(object, "emissive_intensity", material.emissiveIntensity);
    ixtreeme::common::JsonFloatArrayValue(object, "uv_tiling", material.uvTiling, 2);
    ixtreeme::common::JsonFloatArrayValue(object, "uv_offset", material.uvOffset, 2);
    return material;
}

std::string PrefabAssetIdForSave(const std::string& legacyAssetId, const PrefabInstanceState& prefab)
{
    return !prefab.assetId.empty() ? prefab.assetId : legacyAssetId;
}

void WritePrefabInstance(std::ostream& out,
                         const std::string& legacyAssetId,
                         const PrefabInstanceState& prefab,
                         const std::string& indent)
{
    const std::string assetId = PrefabAssetIdForSave(legacyAssetId, prefab);
    if (assetId.empty())
        return;

    out << indent << "\"prefab_asset_id\": \"" << ixtreeme::common::EscapeJson(assetId) << "\",\n";
    out << indent << "\"prefab_instance\": {\n";
    out << indent << "  \"linked\": " << (prefab.linked || !assetId.empty() ? "true" : "false") << ",\n";
    out << indent << "  \"asset_id\": \"" << ixtreeme::common::EscapeJson(assetId) << "\",\n";
    out << indent << "  \"local_id\": " << (prefab.localId == 0 ? 1u : prefab.localId) << ",\n";
    out << indent << "  \"preserve_transform\": " << (prefab.preserveTransform ? "true" : "false") << ",\n";
    out << indent << "  \"name_override\": " << (prefab.nameOverride ? "true" : "false") << "\n";
    out << indent << "},\n";
}

PrefabInstanceState ReadPrefabInstance(const std::string& object, const std::string& legacyAssetId)
{
    PrefabInstanceState prefab;
    prefab.assetId = legacyAssetId;
    const std::string instanceObject = ExtractNamedObject(object, "prefab_instance");
    if (!instanceObject.empty())
    {
        prefab.linked = ixtreeme::common::JsonBoolValue(instanceObject, "linked", !prefab.assetId.empty());
        prefab.assetId = ixtreeme::common::JsonStringValue(instanceObject, "asset_id");
        if (prefab.assetId.empty())
            prefab.assetId = legacyAssetId;
        prefab.localId = JsonU32Value(instanceObject, "local_id", 1);
        prefab.preserveTransform = ixtreeme::common::JsonBoolValue(instanceObject, "preserve_transform", true);
        prefab.nameOverride = ixtreeme::common::JsonBoolValue(instanceObject, "name_override", true);
    }
    else
    {
        prefab.linked = !prefab.assetId.empty();
    }
    return prefab;
}

void WriteMeshObject(std::ostream& out, const MeshSceneEntity& mesh, const std::string& displayName, const std::string& indent)
{
    WritePrefabInstance(out, mesh.prefabAssetId, mesh.prefabInstance, indent);
    out << indent << "\"type\": \"mesh_entity\",\n";
    out << indent << "\"name\": \"" << ixtreeme::common::EscapeJson(displayName) << "\",\n";
    out << indent << "\"mesh_asset_id\": \"" << ixtreeme::common::EscapeJson(mesh.meshAssetId) << "\",\n";
    out << indent << "\"mesh_asset_path\": \"" << ixtreeme::common::EscapeJson(mesh.meshAssetPath) << "\",\n";
    out << indent << "\"position\": " << FloatArray(mesh.position, 3) << ",\n";
    out << indent << "\"rotation\": " << FloatArray(mesh.rotation, 3) << ",\n";
    out << indent << "\"scale\": " << FloatArray(mesh.scale, 3) << ",\n";
    out << indent << "\"skinned\": " << (mesh.skinned ? "true" : "false");
    WriteMaterials(out, mesh.materialSlots, indent);
    WriteMaterialOverrides(out, mesh.materialOverrides, indent);
    if (mesh.hasRigidbody)
        WriteRigidbody(out, mesh.rigidbody, indent);
    if (mesh.hasCollider)
        WriteCollider(out, mesh.collider, indent);
    if (mesh.hasFixedJoint)
        WriteFixedJoint(out, mesh.fixedJoint, indent);
    if (mesh.hasHingeJoint)
        WriteHingeJoint(out, mesh.hingeJoint, indent);
    if (mesh.hasCharacterController)
        WriteCharacterController(out, mesh.characterController, indent);
    if (mesh.hasAudioSource)
        WriteAudioSource(out, mesh.audioSource, indent);
    if (mesh.hasAudioListener)
    {
        out << ",\n";
        out << indent << "\"audio_listener\": {\n";
        out << indent << "  \"enabled\": " << (mesh.audioListener.enabled ? "true" : "false") << "\n";
        out << indent << "}";
    }
    out << "\n";
}

void WritePointObject(std::ostream& out, const PointLight& light, const std::string& displayName, const std::string& indent)
{
    const float color[3] = {light.r, light.g, light.b};
    WritePrefabInstance(out, light.prefabAssetId, light.prefabInstance, indent);
    out << indent << "\"type\": \"dynamic_light\",\n";
    out << indent << "\"light_type\": \"point\",\n";
    out << indent << "\"name\": \"" << ixtreeme::common::EscapeJson(displayName) << "\",\n";
    out << indent << "\"position\": " << FloatArray(light.position, 3) << ",\n";
    out << indent << "\"color\": " << FloatArray(color, 3) << ",\n";
    out << indent << "\"intensity\": " << light.intensity << ",\n";
    out << indent << "\"radius\": " << light.radius << ",\n";
    out << indent << "\"enabled\": " << (light.enabled ? "true" : "false") << "\n";
}

void WriteSpotObject(std::ostream& out, const SpotLight& light, const std::string& displayName, const std::string& indent)
{
    const float color[3] = {light.r, light.g, light.b};
    WritePrefabInstance(out, light.prefabAssetId, light.prefabInstance, indent);
    out << indent << "\"type\": \"dynamic_light\",\n";
    out << indent << "\"light_type\": \"spot\",\n";
    out << indent << "\"name\": \"" << ixtreeme::common::EscapeJson(displayName) << "\",\n";
    out << indent << "\"position\": " << FloatArray(light.position, 3) << ",\n";
    out << indent << "\"rotation\": " << FloatArray(light.rotation, 3) << ",\n";
    out << indent << "\"color\": " << FloatArray(color, 3) << ",\n";
    out << indent << "\"intensity\": " << light.intensity << ",\n";
    out << indent << "\"radius\": " << light.radius << ",\n";
    out << indent << "\"inner_cone_deg\": " << light.innerConeDegrees << ",\n";
    out << indent << "\"outer_cone_deg\": " << light.outerConeDegrees << ",\n";
    out << indent << "\"enabled\": " << (light.enabled ? "true" : "false") << "\n";
}

PrefabEntity ParseEntityObject(const std::string& object, const std::string& fallbackName)
{
    PrefabEntity entity;
    entity.localId = JsonU32Value(object, "local_id", 1);
    entity.parentLocalId = JsonU32Value(object, "parent_local_id", 0);
    entity.name = ixtreeme::common::JsonStringValue(object, "name");
    if (entity.name.empty())
        entity.name = fallbackName;

    const std::string type = ixtreeme::common::JsonStringValue(object, "type");
    if (type == "mesh_entity")
    {
        entity.kind = PrefabTemplate::Kind::Mesh;
        entity.mesh.name = entity.name;
        entity.mesh.prefabAssetId = ixtreeme::common::JsonStringValue(object, "prefab_asset_id");
        entity.mesh.prefabInstance = ReadPrefabInstance(object, entity.mesh.prefabAssetId);
        entity.mesh.meshAssetId = ixtreeme::common::JsonStringValue(object, "mesh_asset_id");
        entity.mesh.meshAssetPath = ixtreeme::common::JsonStringValue(object, "mesh_asset_path");
        if (entity.mesh.meshAssetPath.empty())
            entity.mesh.meshAssetPath = entity.mesh.meshAssetId;
        ixtreeme::common::JsonFloatArrayValue(object, "position", entity.mesh.position, 3);
        ixtreeme::common::JsonFloatArrayValue(object, "rotation", entity.mesh.rotation, 3);
        ixtreeme::common::JsonFloatArrayValue(object, "scale", entity.mesh.scale, 3);
        entity.mesh.skinned = ixtreeme::common::JsonBoolValue(object, "skinned", entity.mesh.skinned);
        entity.mesh.materialSlots = ixtreeme::common::JsonStringArrayValue(object, "materials");
        const std::vector<std::string> materialOverrides = ExtractNamedArrayObjects(object, "material_overrides");
        for (const std::string& materialOverride : materialOverrides)
            entity.mesh.materialOverrides.push_back(ReadMaterialOverride(materialOverride));
        if (!ExtractNamedObject(object, "rigidbody").empty())
        {
            entity.mesh.hasRigidbody = true;
            entity.mesh.rigidbody = ReadRigidbody(object);
        }
        if (!ExtractNamedObject(object, "collider").empty())
        {
            entity.mesh.hasCollider = true;
            entity.mesh.collider = ReadCollider(object);
        }
        if (!ExtractNamedObject(object, "fixed_joint").empty())
        {
            entity.mesh.hasFixedJoint = true;
            entity.mesh.fixedJoint = ReadFixedJoint(object);
        }
        if (!ExtractNamedObject(object, "hinge_joint").empty())
        {
            entity.mesh.hasHingeJoint = true;
            entity.mesh.hingeJoint = ReadHingeJoint(object);
        }
        if (!ExtractNamedObject(object, "character_controller").empty())
        {
            entity.mesh.hasCharacterController = true;
            entity.mesh.characterController = ReadCharacterController(object);
        }
        if (!ExtractNamedObject(object, "audio_source").empty())
        {
            entity.mesh.hasAudioSource = true;
            entity.mesh.audioSource = ReadAudioSource(object);
        }
        if (const std::string listenerObj = ExtractNamedObject(object, "audio_listener"); !listenerObj.empty())
        {
            entity.mesh.hasAudioListener = true;
            entity.mesh.audioListener.enabled = ixtreeme::common::JsonBoolValue(listenerObj, "enabled", true);
        }
        return entity;
    }

    if (type == "dynamic_light")
    {
        const std::string lightType = ixtreeme::common::JsonStringValue(object, "light_type");
        if (lightType == "point")
        {
            entity.kind = PrefabTemplate::Kind::PointLight;
            entity.point.name = entity.name;
            entity.point.prefabAssetId = ixtreeme::common::JsonStringValue(object, "prefab_asset_id");
            entity.point.prefabInstance = ReadPrefabInstance(object, entity.point.prefabAssetId);
            ixtreeme::common::JsonFloatArrayValue(object, "position", entity.point.position, 3);
            float color[3] = {entity.point.r, entity.point.g, entity.point.b};
            ixtreeme::common::JsonFloatArrayValue(object, "color", color, 3);
            entity.point.r = color[0];
            entity.point.g = color[1];
            entity.point.b = color[2];
            entity.point.intensity = ixtreeme::common::JsonFloatValue(object, "intensity", entity.point.intensity);
            entity.point.radius = ixtreeme::common::JsonFloatValue(object, "radius", entity.point.radius);
            entity.point.enabled = ixtreeme::common::JsonBoolValue(object, "enabled", entity.point.enabled);
            return entity;
        }
        if (lightType == "spot")
        {
            entity.kind = PrefabTemplate::Kind::SpotLight;
            entity.spot.name = entity.name;
            entity.spot.prefabAssetId = ixtreeme::common::JsonStringValue(object, "prefab_asset_id");
            entity.spot.prefabInstance = ReadPrefabInstance(object, entity.spot.prefabAssetId);
            ixtreeme::common::JsonFloatArrayValue(object, "position", entity.spot.position, 3);
            ixtreeme::common::JsonFloatArrayValue(object, "rotation", entity.spot.rotation, 3);
            float color[3] = {entity.spot.r, entity.spot.g, entity.spot.b};
            ixtreeme::common::JsonFloatArrayValue(object, "color", color, 3);
            entity.spot.r = color[0];
            entity.spot.g = color[1];
            entity.spot.b = color[2];
            entity.spot.intensity = ixtreeme::common::JsonFloatValue(object, "intensity", entity.spot.intensity);
            entity.spot.radius = ixtreeme::common::JsonFloatValue(object, "radius", entity.spot.radius);
            entity.spot.innerConeDegrees = ixtreeme::common::JsonFloatValue(object, "inner_cone_deg", entity.spot.innerConeDegrees);
            entity.spot.outerConeDegrees = ixtreeme::common::JsonFloatValue(object, "outer_cone_deg", entity.spot.outerConeDegrees);
            entity.spot.enabled = ixtreeme::common::JsonBoolValue(object, "enabled", entity.spot.enabled);
            return entity;
        }
    }

    return entity;
}
} // namespace

std::string FloatArray(const float* values, std::size_t count)
{
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i)
            out << ", ";
        out << values[i];
    }
    out << "]";
    return out.str();
}

void WriteMesh(std::ostream& out, const MeshSceneEntity& mesh, const std::string& displayName)
{
    out << "  \"entity\": {\n";
    WriteMeshObject(out, mesh, displayName, "    ");
    out << "  }\n";
}

void WritePointLight(std::ostream& out, const PointLight& light, const std::string& displayName)
{
    const float color[3] = {light.r, light.g, light.b};
    out << "  \"entity\": {\n";
    (void)color;
    WritePointObject(out, light, displayName, "    ");
    out << "  }\n";
}

void WriteSpotLight(std::ostream& out, const SpotLight& light, const std::string& displayName)
{
    const float color[3] = {light.r, light.g, light.b};
    out << "  \"entity\": {\n";
    (void)color;
    WriteSpotObject(out, light, displayName, "    ");
    out << "  }\n";
}

void WriteDocument(std::ostream& out, const PrefabDocument& document)
{
    out << "{\n";
    out << "  \"version\": 2,\n";
    out << "  \"name\": \"" << ixtreeme::common::EscapeJson(document.name.empty() ? "Prefab" : document.name) << "\",\n";
    out << "  \"entities\": [\n";
    for (std::size_t i = 0; i < document.entities.size(); ++i)
    {
        const PrefabEntity& entity = document.entities[i];
        out << "    {\n";
        out << "      \"local_id\": " << (entity.localId == 0 ? static_cast<std::uint32_t>(i + 1) : entity.localId) << ",\n";
        out << "      \"parent_local_id\": " << entity.parentLocalId << ",\n";
        if (entity.kind == PrefabTemplate::Kind::Mesh)
            WriteMeshObject(out, entity.mesh, entity.name.empty() ? entity.mesh.name : entity.name, "      ");
        else if (entity.kind == PrefabTemplate::Kind::PointLight)
            WritePointObject(out, entity.point, entity.name.empty() ? entity.point.name : entity.name, "      ");
        else if (entity.kind == PrefabTemplate::Kind::SpotLight)
            WriteSpotObject(out, entity.spot, entity.name.empty() ? entity.spot.name : entity.name, "      ");
        out << "    }" << (i + 1 < document.entities.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

PrefabDocument ParseDocument(const std::string& text, const std::string& fallbackName)
{
    PrefabDocument document;
    document.name = ixtreeme::common::JsonStringValue(text, "name");
    if (document.name.empty())
        document.name = fallbackName;

    std::vector<std::string> entityObjects = ExtractNamedArrayObjects(text, "entities");
    if (entityObjects.empty())
    {
        std::string legacy = ExtractNamedObject(text, "entity");
        if (legacy.empty())
            legacy = text;
        entityObjects.push_back(std::move(legacy));
    }

    for (std::string& object : entityObjects)
    {
        PrefabEntity entity = ParseEntityObject(object, document.name);
        if (entity.kind != PrefabTemplate::Kind::Unsupported)
            document.entities.push_back(std::move(entity));
    }
    return document;
}

PrefabTemplate ParseTemplate(const std::string& text, const std::string& fallbackName)
{
    PrefabTemplate prefab{};
    const PrefabDocument document = ParseDocument(text, fallbackName);
    prefab.name = document.name.empty() ? fallbackName : document.name;
    if (document.entities.empty())
        return prefab;

    const PrefabEntity& entity = document.entities.front();
    prefab.kind = entity.kind;
    prefab.name = entity.name.empty() ? prefab.name : entity.name;
    prefab.mesh = entity.mesh;
    prefab.point = entity.point;
    prefab.spot = entity.spot;
    return prefab;
}

} // namespace ixtreeme::prefab
