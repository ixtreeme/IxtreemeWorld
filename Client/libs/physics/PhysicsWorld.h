#pragma once

#include "PhysicsComponents.h"

#include <cstdint>
#include <memory>

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
};

struct PhysicsWorldStats
{
    std::uint32_t bodyCount = 0;
    bool backendReady = false;
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
    void DestroyBody(BodyId id);
    void SetBodyTransform(BodyId id, const PhysicsTransform& transform);
    bool GetBodyTransform(BodyId id, PhysicsTransform& outTransform) const;
    void Step(float deltaSeconds);
    PhysicsWorldStats Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ixtreeme::physics
