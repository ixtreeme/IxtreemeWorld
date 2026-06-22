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
    Capsule
};

struct RigidbodyComponent
{
    bool enabled = true;
    BodyType bodyType = BodyType::Dynamic;
    float mass = 1.0f;
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    bool useGravity = true;
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
    std::string materialAssetId;
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
    return fallback;
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
