#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace ixtreeme::physics
{

enum class BodyType : std::uint8_t
{
    Static,
    Dynamic,
    Kinematic
};

enum class ColliderShape : std::uint8_t
{
    Box,
    Sphere,
    Capsule,
    Mesh,
    ConvexHull
};

enum class PhysicsLayer : std::uint8_t
{
    Default,
    StaticWorld,
    DynamicObject,
    Player,
    Trigger,
    Projectile,
    Foliage,
    NoCollision,
    Count
};

enum class PhysicsMaterialCombineMode : std::uint8_t
{
    Average,
    Minimum,
    Maximum,
    Multiply
};

struct RigidbodyComponent
{
    bool enabled = true;
    BodyType bodyType = BodyType::Dynamic;
    float mass = 1.0f;
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    bool useGravity = true;
    bool allowSleeping = true;
    bool continuousCollision = false;
    bool freezePosition[3] = {false, false, false};
    bool freezeRotation[3] = {false, false, false};
};

struct ColliderComponent
{
    bool enabled = true;
    bool trigger = false;
    ColliderShape shape = ColliderShape::Box;
    float center[3] = {0.0f, 0.0f, 0.0f};
    float size[3] = {1.0f, 1.0f, 1.0f};
    float radius = 0.5f;
    float height = 2.0f;
    float friction = 0.6f;
    float restitution = 0.0f;
    PhysicsMaterialCombineMode frictionCombine = PhysicsMaterialCombineMode::Average;
    PhysicsMaterialCombineMode restitutionCombine = PhysicsMaterialCombineMode::Maximum;
    PhysicsLayer layer = PhysicsLayer::Default;
    std::string materialAssetId;
};

struct FixedJointComponent
{
    bool enabled = true;
    std::uint32_t connectedEntityId = 0;
};

struct HingeJointComponent
{
    bool enabled = true;
    std::uint32_t connectedEntityId = 0;
    float anchor[3] = {0.0f, 0.0f, 0.0f};
    float axis[3] = {0.0f, 1.0f, 0.0f};
    bool limitsEnabled = false;
    float minAngleDegrees = -90.0f;
    float maxAngleDegrees = 90.0f;
    float frictionTorque = 0.0f;
};

inline const char* ToString(BodyType type)
{
    switch (type)
    {
    case BodyType::Static: return "static";
    case BodyType::Dynamic: return "dynamic";
    case BodyType::Kinematic: return "kinematic";
    default: return "dynamic";
    }
}

inline BodyType BodyTypeFromString(const std::string& text, BodyType fallback = BodyType::Dynamic)
{
    if (text == "static")
        return BodyType::Static;
    if (text == "dynamic")
        return BodyType::Dynamic;
    if (text == "kinematic")
        return BodyType::Kinematic;
    return fallback;
}

inline const char* ToString(ColliderShape shape)
{
    switch (shape)
    {
    case ColliderShape::Box: return "box";
    case ColliderShape::Sphere: return "sphere";
    case ColliderShape::Capsule: return "capsule";
    case ColliderShape::Mesh: return "mesh";
    case ColliderShape::ConvexHull: return "convex_hull";
    default: return "box";
    }
}

inline ColliderShape ColliderShapeFromString(const std::string& text, ColliderShape fallback = ColliderShape::Box)
{
    if (text == "box")
        return ColliderShape::Box;
    if (text == "sphere")
        return ColliderShape::Sphere;
    if (text == "capsule")
        return ColliderShape::Capsule;
    if (text == "mesh")
        return ColliderShape::Mesh;
    if (text == "convex_hull" || text == "convex-hull")
        return ColliderShape::ConvexHull;
    return fallback;
}

inline const char* ToString(PhysicsLayer layer)
{
    switch (layer)
    {
    case PhysicsLayer::Default: return "default";
    case PhysicsLayer::StaticWorld: return "static_world";
    case PhysicsLayer::DynamicObject: return "dynamic_object";
    case PhysicsLayer::Player: return "player";
    case PhysicsLayer::Trigger: return "trigger";
    case PhysicsLayer::Projectile: return "projectile";
    case PhysicsLayer::Foliage: return "foliage";
    case PhysicsLayer::NoCollision: return "no_collision";
    case PhysicsLayer::Count:
    default: return "default";
    }
}

inline const char* DisplayName(PhysicsLayer layer)
{
    switch (layer)
    {
    case PhysicsLayer::Default: return "Default";
    case PhysicsLayer::StaticWorld: return "Static World";
    case PhysicsLayer::DynamicObject: return "Dynamic Object";
    case PhysicsLayer::Player: return "Player";
    case PhysicsLayer::Trigger: return "Trigger";
    case PhysicsLayer::Projectile: return "Projectile";
    case PhysicsLayer::Foliage: return "Foliage";
    case PhysicsLayer::NoCollision: return "No Collision";
    case PhysicsLayer::Count:
    default: return "Default";
    }
}

inline const char* ToString(PhysicsMaterialCombineMode mode)
{
    switch (mode)
    {
    case PhysicsMaterialCombineMode::Average: return "average";
    case PhysicsMaterialCombineMode::Minimum: return "minimum";
    case PhysicsMaterialCombineMode::Maximum: return "maximum";
    case PhysicsMaterialCombineMode::Multiply: return "multiply";
    default: return "average";
    }
}

inline const char* DisplayName(PhysicsMaterialCombineMode mode)
{
    switch (mode)
    {
    case PhysicsMaterialCombineMode::Average: return "Average";
    case PhysicsMaterialCombineMode::Minimum: return "Minimum";
    case PhysicsMaterialCombineMode::Maximum: return "Maximum";
    case PhysicsMaterialCombineMode::Multiply: return "Multiply";
    default: return "Average";
    }
}

inline PhysicsMaterialCombineMode PhysicsMaterialCombineModeFromString(
    const std::string& text,
    PhysicsMaterialCombineMode fallback = PhysicsMaterialCombineMode::Average)
{
    if (text == "average" || text == "avg")
        return PhysicsMaterialCombineMode::Average;
    if (text == "minimum" || text == "min")
        return PhysicsMaterialCombineMode::Minimum;
    if (text == "maximum" || text == "max")
        return PhysicsMaterialCombineMode::Maximum;
    if (text == "multiply" || text == "mult")
        return PhysicsMaterialCombineMode::Multiply;
    return fallback;
}

inline PhysicsLayer PhysicsLayerFromString(const std::string& text, PhysicsLayer fallback = PhysicsLayer::Default)
{
    if (text == "default")
        return PhysicsLayer::Default;
    if (text == "static_world")
        return PhysicsLayer::StaticWorld;
    if (text == "dynamic_object")
        return PhysicsLayer::DynamicObject;
    if (text == "player")
        return PhysicsLayer::Player;
    if (text == "trigger")
        return PhysicsLayer::Trigger;
    if (text == "projectile")
        return PhysicsLayer::Projectile;
    if (text == "foliage")
        return PhysicsLayer::Foliage;
    if (text == "no_collision")
        return PhysicsLayer::NoCollision;
    return fallback;
}

inline constexpr std::uint8_t PhysicsLayerIndex(PhysicsLayer layer)
{
    return static_cast<std::uint8_t>(layer);
}

inline constexpr std::uint8_t PhysicsLayerCount()
{
    return static_cast<std::uint8_t>(PhysicsLayer::Count);
}

inline constexpr std::uint32_t PhysicsLayerMask(PhysicsLayer layer)
{
    return layer == PhysicsLayer::Count
        ? 0u
        : (1u << static_cast<std::uint32_t>(PhysicsLayerIndex(layer)));
}

inline constexpr std::uint32_t AllPhysicsLayerMask()
{
    return (1u << static_cast<std::uint32_t>(PhysicsLayer::Count)) - 1u;
}

inline bool DefaultLayerCollision(PhysicsLayer a, PhysicsLayer b)
{
    if (a == PhysicsLayer::NoCollision || b == PhysicsLayer::NoCollision)
        return false;
    if (a == PhysicsLayer::Foliage || b == PhysicsLayer::Foliage)
        return false;
    if (a == PhysicsLayer::Projectile && b == PhysicsLayer::Projectile)
        return false;
    return true;
}

inline void Sanitize(RigidbodyComponent& rigidbody)
{
    rigidbody.mass = std::max(0.001f, rigidbody.mass);
    rigidbody.linearDamping = std::clamp(rigidbody.linearDamping, 0.0f, 100.0f);
    rigidbody.angularDamping = std::clamp(rigidbody.angularDamping, 0.0f, 100.0f);
}

inline void Sanitize(ColliderComponent& collider)
{
    collider.size[0] = std::max(0.001f, collider.size[0]);
    collider.size[1] = std::max(0.001f, collider.size[1]);
    collider.size[2] = std::max(0.001f, collider.size[2]);
    collider.radius = std::max(0.001f, collider.radius);
    collider.height = std::max(collider.radius * 2.0f, collider.height);
    collider.friction = std::clamp(collider.friction, 0.0f, 4.0f);
    collider.restitution = std::clamp(collider.restitution, 0.0f, 1.0f);
}

} // namespace ixtreeme::physics
