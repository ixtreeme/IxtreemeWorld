#include "PhysicsWorld.h"

#include "Debug.h"

#include <algorithm>
#include <thread>
#include <unordered_map>

#if defined(IXENGINE_PHYSICS_WITH_JOLT)
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#endif

namespace ixtreeme::physics
{
namespace
{
constexpr BodyId kInvalidBodyId = 0;

#if defined(IXENGINE_PHYSICS_WITH_JOLT)
namespace Layers
{
static constexpr JPH::ObjectLayer NonMoving = 0;
static constexpr JPH::ObjectLayer Moving = 1;
static constexpr JPH::ObjectLayer Count = 2;
}

namespace BroadPhaseLayers
{
static constexpr JPH::BroadPhaseLayer NonMoving(0);
static constexpr JPH::BroadPhaseLayer Moving(1);
static constexpr std::uint32_t Count = 2;
}

class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface
{
public:
    std::uint32_t GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::Count; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return layer == Layers::NonMoving ? BroadPhaseLayers::NonMoving : BroadPhaseLayers::Moving;
    }

    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return layer == BroadPhaseLayers::NonMoving ? "NonMoving" : "Moving";
    }
};

class ObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhaseLayer) const override
    {
        if (layer == Layers::NonMoving)
            return broadPhaseLayer == BroadPhaseLayers::Moving;
        return true;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer object1, JPH::ObjectLayer object2) const override
    {
        return object1 == Layers::Moving || object2 == Layers::Moving;
    }
};

JPH::EMotionType ToJoltMotion(BodyType type)
{
    switch (type)
    {
    case BodyType::Static: return JPH::EMotionType::Static;
    case BodyType::Kinematic: return JPH::EMotionType::Kinematic;
    case BodyType::Dynamic:
    default: return JPH::EMotionType::Dynamic;
    }
}

JPH::ObjectLayer ToJoltLayer(BodyType type)
{
    return type == BodyType::Static ? Layers::NonMoving : Layers::Moving;
}

JPH::ShapeRefC CreateShape(const ColliderComponent& input)
{
    ColliderComponent collider = input;
    Sanitize(collider);
    switch (collider.shape)
    {
    case ColliderShape::Sphere:
        return new JPH::SphereShape(collider.radius);
    case ColliderShape::Capsule:
    {
        const float cylinderHalfHeight = std::max(0.001f, (collider.height - collider.radius * 2.0f) * 0.5f);
        return new JPH::CapsuleShape(cylinderHalfHeight, collider.radius);
    }
    case ColliderShape::Box:
    default:
        return new JPH::BoxShape(JPH::Vec3(
            std::max(0.001f, collider.size[0] * 0.5f),
            std::max(0.001f, collider.size[1] * 0.5f),
            std::max(0.001f, collider.size[2] * 0.5f)));
    }
}
#endif
}

struct PhysicsWorld::Impl
{
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    BroadPhaseLayerInterface broadPhaseLayers;
    ObjectVsBroadPhaseLayerFilter objectVsBroadPhaseLayerFilter;
    ObjectLayerPairFilter objectLayerPairFilter;
    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> jobSystem;
    JPH::PhysicsSystem system;
    std::unordered_map<BodyId, JPH::BodyID> bodies;
#else
    std::unordered_map<BodyId, PhysicsTransform> bodies;
#endif
    BodyId nextBodyId = 1;
    bool created = false;
};

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld()
{
    Destroy();
}

bool PhysicsWorld::Create()
{
    if (!m_impl)
        m_impl = std::make_unique<Impl>();
    if (m_impl->created)
        return true;

#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    JPH::RegisterDefaultAllocator();
    if (JPH::Factory::sInstance == nullptr)
    {
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

    constexpr std::uint32_t maxBodies = 65536;
    constexpr std::uint32_t bodyMutexes = 0;
    constexpr std::uint32_t maxBodyPairs = 65536;
    constexpr std::uint32_t maxContactConstraints = 20480;
    m_impl->tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
    m_impl->jobSystem = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs,
        JPH::cMaxPhysicsBarriers,
        std::max(1u, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1u));
    m_impl->system.Init(maxBodies,
        bodyMutexes,
        maxBodyPairs,
        maxContactConstraints,
        m_impl->broadPhaseLayers,
        m_impl->objectVsBroadPhaseLayerFilter,
        m_impl->objectLayerPairFilter);
    Tracen("[PHYSICS] backend=Jolt created");
#else
    Tracen("[PHYSICS] backend=Null created");
#endif
    m_impl->created = true;
    return true;
}

void PhysicsWorld::Destroy()
{
    if (!m_impl)
        return;

#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    if (m_impl->created)
    {
        JPH::BodyInterface& bodies = m_impl->system.GetBodyInterface();
        for (const auto& entry : m_impl->bodies)
        {
            bodies.RemoveBody(entry.second);
            bodies.DestroyBody(entry.second);
        }
    }
#endif
    m_impl->bodies.clear();
    m_impl->created = false;
}

bool PhysicsWorld::IsCreated() const
{
    return m_impl && m_impl->created;
}

BodyId PhysicsWorld::CreateBody(const PhysicsBodyDesc& desc)
{
    if (!Create())
        return kInvalidBodyId;

    RigidbodyComponent rigidbody = desc.rigidbody;
    ColliderComponent collider = desc.collider;
    Sanitize(rigidbody);
    Sanitize(collider);

    const BodyId id = m_impl->nextBodyId++;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    JPH::BodyInterface& bodies = m_impl->system.GetBodyInterface();
    JPH::ShapeRefC shape = CreateShape(collider);
    JPH::BodyCreationSettings settings(
        shape,
        JPH::RVec3(desc.transform.position[0] + collider.center[0],
            desc.transform.position[1] + collider.center[1],
            desc.transform.position[2] + collider.center[2]),
        JPH::Quat(desc.transform.rotation[0], desc.transform.rotation[1], desc.transform.rotation[2], desc.transform.rotation[3]),
        ToJoltMotion(desc.bodyType),
        ToJoltLayer(desc.bodyType));
    settings.mIsSensor = collider.trigger;
    settings.mOverrideMassProperties = desc.bodyType == BodyType::Dynamic
        ? JPH::EOverrideMassProperties::CalculateInertia
        : JPH::EOverrideMassProperties::MassAndInertiaProvided;
    settings.mMassPropertiesOverride.mMass = rigidbody.mass;
    settings.mLinearDamping = rigidbody.linearDamping;
    settings.mAngularDamping = rigidbody.angularDamping;
    settings.mGravityFactor = rigidbody.useGravity ? 1.0f : 0.0f;
    const JPH::BodyID bodyId = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
    m_impl->bodies[id] = bodyId;
#else
    m_impl->bodies[id] = desc.transform;
#endif
    return id;
}

void PhysicsWorld::DestroyBody(BodyId id)
{
    if (!m_impl)
        return;
    auto it = m_impl->bodies.find(id);
    if (it == m_impl->bodies.end())
        return;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    JPH::BodyInterface& bodies = m_impl->system.GetBodyInterface();
    bodies.RemoveBody(it->second);
    bodies.DestroyBody(it->second);
#endif
    m_impl->bodies.erase(it);
}

void PhysicsWorld::SetBodyTransform(BodyId id, const PhysicsTransform& transform)
{
    if (!m_impl)
        return;
    auto it = m_impl->bodies.find(id);
    if (it == m_impl->bodies.end())
        return;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    JPH::BodyInterface& bodies = m_impl->system.GetBodyInterface();
    bodies.SetPositionAndRotation(
        it->second,
        JPH::RVec3(transform.position[0], transform.position[1], transform.position[2]),
        JPH::Quat(transform.rotation[0], transform.rotation[1], transform.rotation[2], transform.rotation[3]),
        JPH::EActivation::Activate);
#else
    it->second = transform;
#endif
}

bool PhysicsWorld::GetBodyTransform(BodyId id, PhysicsTransform& outTransform) const
{
    if (!m_impl)
        return false;
    auto it = m_impl->bodies.find(id);
    if (it == m_impl->bodies.end())
        return false;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    const JPH::RVec3 position = m_impl->system.GetBodyInterface().GetPosition(it->second);
    const JPH::Quat rotation = m_impl->system.GetBodyInterface().GetRotation(it->second);
    outTransform.position[0] = static_cast<float>(position.GetX());
    outTransform.position[1] = static_cast<float>(position.GetY());
    outTransform.position[2] = static_cast<float>(position.GetZ());
    outTransform.rotation[0] = rotation.GetX();
    outTransform.rotation[1] = rotation.GetY();
    outTransform.rotation[2] = rotation.GetZ();
    outTransform.rotation[3] = rotation.GetW();
#else
    outTransform = it->second;
#endif
    return true;
}

void PhysicsWorld::Step(float deltaSeconds)
{
    if (!m_impl || !m_impl->created || deltaSeconds <= 0.0f)
        return;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    constexpr int collisionSteps = 1;
    m_impl->system.Update(deltaSeconds, collisionSteps, m_impl->tempAllocator.get(), m_impl->jobSystem.get());
#else
    (void)deltaSeconds;
#endif
}

PhysicsWorldStats PhysicsWorld::Stats() const
{
    PhysicsWorldStats stats{};
    if (!m_impl)
        return stats;
    stats.bodyCount = static_cast<std::uint32_t>(m_impl->bodies.size());
    stats.backendReady = m_impl->created;
    return stats;
}

} // namespace ixtreeme::physics
