#pragma once

#include "PhysicsComponents.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ixtreeme::physics
{

using BodyId = std::uint64_t;

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
    float position[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 1.0f, 0.0f};
    float distance = 0.0f;
    float fraction = 0.0f;
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
};

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

    BodyId CreateBody(const PhysicsBodyDesc& desc);
    BodyId CreateTerrainCollider(const TerrainColliderDesc& desc);
    void DestroyBody(BodyId id);
    void SetBodyTransform(BodyId id, const PhysicsTransform& transform);
    bool GetBodyTransform(BodyId id, PhysicsTransform& outTransform) const;
    bool Raycast(const float origin[3], const float direction[3], float maxDistance, PhysicsRaycastHit& outHit) const;
    void Step(float deltaSeconds);
    PhysicsWorldStats Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ixtreeme::physics
