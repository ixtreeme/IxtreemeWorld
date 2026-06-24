#pragma once

#include "PhysicsComponents.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace ixtreeme::physics
{

using BodyId = std::uint64_t;
using ConstraintId = std::uint64_t;
using PhysicsCollisionMatrix = std::array<std::array<bool, static_cast<std::size_t>(PhysicsLayer::Count)>, static_cast<std::size_t>(PhysicsLayer::Count)>;

struct PhysicsTransform
{
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

struct PhysicsBodyDesc
{
    BodyType bodyType = BodyType::Dynamic;
    RigidbodyComponent rigidbody;
    ColliderComponent collider;
    PhysicsTransform transform;
    float scale[3] = {1.0f, 1.0f, 1.0f};
    std::vector<std::array<float, 3>> meshVertices;
    std::vector<std::uint32_t> meshIndices;
};

struct PhysicsWorldStats
{
    std::uint32_t bodyCount = 0;
    bool backendReady = false;
};

struct PhysicsRaycastHit
{
    bool hit = false;
    BodyId bodyId = 0;
    PhysicsLayer layer = PhysicsLayer::Default;
    bool trigger = false;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 1.0f, 0.0f};
    float distance = 0.0f;
    float fraction = 0.0f;
};

struct PhysicsOverlapHit
{
    BodyId bodyId = 0;
    PhysicsLayer layer = PhysicsLayer::Default;
    bool trigger = false;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 1.0f, 0.0f};
    float penetrationDepth = 0.0f;
};

struct PhysicsQueryFilter
{
    std::uint32_t layerMask = AllPhysicsLayerMask();
    bool hitTriggers = true;
    BodyId ignoreBody = 0;
};

enum class PhysicsContactPhase : std::uint8_t
{
    Started,
    Stayed,
    Ended
};

enum class PhysicsContactKind : std::uint8_t
{
    Collision,
    Trigger
};

struct PhysicsContactEvent
{
    PhysicsContactPhase phase = PhysicsContactPhase::Started;
    PhysicsContactKind kind = PhysicsContactKind::Collision;
    BodyId bodyA = 0;
    BodyId bodyB = 0;
    float point[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 1.0f, 0.0f};
    float penetrationDepth = 0.0f;
};

struct TerrainColliderDesc
{
    float widthMeters = 0.0f;
    float depthMeters = 0.0f;
    float cellSizeMeters = 1.0f;
    std::uint32_t cellsX = 0;
    std::uint32_t cellsZ = 0;
    std::vector<float> heightCmGrid;
    float friction = 0.8f;
    float restitution = 0.0f;
    PhysicsLayer layer = PhysicsLayer::StaticWorld;
};

struct FixedJointDesc
{
    BodyId bodyA = 0;
    BodyId bodyB = 0;
};

struct HingeJointDesc
{
    BodyId bodyA = 0;
    BodyId bodyB = 0;
    float anchor[3] = {0.0f, 0.0f, 0.0f};
    float axis[3] = {0.0f, 1.0f, 0.0f};
    bool limitsEnabled = false;
    float minAngleRadians = -1.57079632679f;
    float maxAngleRadians = 1.57079632679f;
    float frictionTorque = 0.0f;
};

PhysicsCollisionMatrix DefaultPhysicsCollisionMatrix();

class PhysicsWorld
{
public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    bool Create();
    void Destroy();
    bool IsCreated() const;
    void SetCollisionMatrix(const PhysicsCollisionMatrix& matrix);
    void SetGravity(const float gravity[3]);

    BodyId CreateBody(const PhysicsBodyDesc& desc);
    BodyId CreateTerrainCollider(const TerrainColliderDesc& desc);
    ConstraintId CreateFixedJoint(const FixedJointDesc& desc);
    ConstraintId CreateHingeJoint(const HingeJointDesc& desc);
    void DestroyConstraint(ConstraintId id);
    void DestroyBody(BodyId id);
    void SetBodyTransform(BodyId id, const PhysicsTransform& transform);
    bool GetBodyTransform(BodyId id, PhysicsTransform& outTransform) const;
    bool SetLinearVelocity(BodyId id, const float velocity[3]);
    bool GetLinearVelocity(BodyId id, float outVelocity[3]) const;
    bool GetAngularVelocity(BodyId id, float outVelocity[3]) const;
    bool IsBodyActive(BodyId id) const;
    bool MoveKinematic(BodyId id, const PhysicsTransform& targetTransform, float deltaSeconds);
    bool AddForce(BodyId id, const float force[3]);
    bool AddImpulse(BodyId id, const float impulse[3]);
    bool AddAngularImpulse(BodyId id, const float angularImpulse[3]);
    bool Raycast(const float origin[3], const float direction[3], float maxDistance, PhysicsRaycastHit& outHit) const;
    bool Raycast(const float origin[3], const float direction[3], float maxDistance, const PhysicsQueryFilter& filter, PhysicsRaycastHit& outHit) const;
    std::vector<PhysicsOverlapHit> OverlapSphere(
        const float center[3],
        float radius,
        const PhysicsQueryFilter& filter,
        std::size_t maxHits = 64) const;
    std::vector<PhysicsOverlapHit> OverlapBox(
        const float center[3],
        const float halfExtents[3],
        const PhysicsQueryFilter& filter,
        std::size_t maxHits = 64) const;
    std::vector<PhysicsOverlapHit> OverlapCapsule(
        const float center[3],
        float halfHeight,
        float radius,
        const PhysicsQueryFilter& filter,
        std::size_t maxHits = 64) const;
    void Step(float deltaSeconds);
    const std::vector<PhysicsContactEvent>& ContactEvents() const;
    PhysicsWorldStats Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ixtreeme::physics
