#include "PhysicsWorld.h"

#include "Debug.h"

#include <algorithm>
#include <cmath>
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
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
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

JPH::ShapeRefC CreateShape(const ColliderComponent& input, const float scale[3])
{
    ColliderComponent collider = input;
    Sanitize(collider);
    const float sx = std::max(0.001f, std::abs(scale[0]));
    const float sy = std::max(0.001f, std::abs(scale[1]));
    const float sz = std::max(0.001f, std::abs(scale[2]));
    collider.center[0] *= sx;
    collider.center[1] *= sy;
    collider.center[2] *= sz;
    JPH::ShapeRefC baseShape;
    switch (collider.shape)
    {
    case ColliderShape::Sphere:
        baseShape = new JPH::SphereShape(collider.radius * std::max({sx, sy, sz}));
        break;
    case ColliderShape::Capsule:
    {
        const float radius = collider.radius * std::max(sx, sz);
        const float height = std::max(radius * 2.0f, collider.height * sy);
        const float cylinderHalfHeight = std::max(0.001f, (height - radius * 2.0f) * 0.5f);
        baseShape = new JPH::CapsuleShape(cylinderHalfHeight, radius);
        break;
    }
    case ColliderShape::Box:
    default:
        baseShape = new JPH::BoxShape(JPH::Vec3(
            std::max(0.001f, collider.size[0] * sx * 0.5f),
            std::max(0.001f, collider.size[1] * sy * 0.5f),
            std::max(0.001f, collider.size[2] * sz * 0.5f)));
        break;
    }

    if (std::abs(collider.center[0]) > 0.0001f ||
        std::abs(collider.center[1]) > 0.0001f ||
        std::abs(collider.center[2]) > 0.0001f)
    {
        return JPH::RotatedTranslatedShapeSettings(
            JPH::Vec3(collider.center[0], collider.center[1], collider.center[2]),
            JPH::Quat::sIdentity(),
            baseShape).Create().Get();
    }
    return baseShape;
}

BodyId FindEngineBodyId(const std::unordered_map<BodyId, JPH::BodyID>& bodies, JPH::BodyID joltBodyId)
{
    for (const auto& entry : bodies)
    {
        if (entry.second == joltBodyId)
            return entry.first;
    }
    return kInvalidBodyId;
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
    constexpr std::uint32_t tempAllocatorSizeBytes = 128u * 1024u * 1024u;
    m_impl->tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(tempAllocatorSizeBytes);
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
    m_impl->system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    Tracenf("[PHYSICS] backend=Jolt created gravity=(0.00,-9.81,0.00) tempAllocatorMB=%u",
        tempAllocatorSizeBytes / (1024u * 1024u));
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
    m_impl.reset();
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
    JPH::ShapeRefC shape = CreateShape(collider, desc.scale);
    JPH::BodyCreationSettings settings(
        shape,
        JPH::RVec3(desc.transform.position[0],
            desc.transform.position[1],
            desc.transform.position[2]),
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
    settings.mFriction = collider.friction;
    settings.mRestitution = collider.restitution;
    const JPH::BodyID bodyId = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
    m_impl->bodies[id] = bodyId;
#else
    m_impl->bodies[id] = desc.transform;
#endif
    return id;
}

BodyId PhysicsWorld::CreateTerrainCollider(const TerrainColliderDesc& desc)
{
    if (!Create())
        return kInvalidBodyId;
    if (desc.cellsX == 0 || desc.cellsZ == 0 || desc.widthMeters <= 0.0f || desc.depthMeters <= 0.0f)
    {
        Tracenf("[PHYSICS] terrain collider skipped reason=invalid-dims cells=%ux%u size=%.2fx%.2f",
            desc.cellsX,
            desc.cellsZ,
            desc.widthMeters,
            desc.depthMeters);
        return kInvalidBodyId;
    }

    const std::uint32_t vertsX = desc.cellsX + 1u;
    const std::uint32_t vertsZ = desc.cellsZ + 1u;
    const std::size_t expectedHeights = static_cast<std::size_t>(vertsX) * static_cast<std::size_t>(vertsZ);
    const bool hasHeightGrid = desc.heightCmGrid.size() >= expectedHeights;
    const float cellX = desc.widthMeters / static_cast<float>(desc.cellsX);
    const float cellZ = desc.depthMeters / static_cast<float>(desc.cellsZ);

    const BodyId id = m_impl->nextBodyId++;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    JPH::VertexList vertices;
    vertices.reserve(expectedHeights);
    for (std::uint32_t z = 0; z < vertsZ; ++z)
    {
        const float worldZ = desc.depthMeters * 0.5f - static_cast<float>(z) * cellZ;
        for (std::uint32_t x = 0; x < vertsX; ++x)
        {
            const float worldX = -desc.widthMeters * 0.5f + static_cast<float>(x) * cellX;
            const std::size_t index = static_cast<std::size_t>(z) * vertsX + x;
            const float worldY = hasHeightGrid ? desc.heightCmGrid[index] * 0.01f : 0.0f;
            vertices.emplace_back(worldX, worldY, worldZ);
        }
    }

    JPH::IndexedTriangleList triangles;
    triangles.reserve(static_cast<std::size_t>(desc.cellsX) * static_cast<std::size_t>(desc.cellsZ) * 2u);
    for (std::uint32_t z = 0; z < desc.cellsZ; ++z)
    {
        for (std::uint32_t x = 0; x < desc.cellsX; ++x)
        {
            const std::uint32_t v00 = z * vertsX + x;
            const std::uint32_t v10 = v00 + 1u;
            const std::uint32_t v01 = (z + 1u) * vertsX + x;
            const std::uint32_t v11 = v01 + 1u;
            triangles.emplace_back(v00, v10, v01, 0u);
            triangles.emplace_back(v10, v11, v01, 0u);
        }
    }

    JPH::MeshShapeSettings shapeSettings(std::move(vertices), std::move(triangles));
    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (shapeResult.HasError())
    {
        Tracenf("[PHYSICS] terrain collider failed reason=%s", shapeResult.GetError().c_str());
        return kInvalidBodyId;
    }

    JPH::BodyCreationSettings bodySettings(
        shapeResult.Get(),
        JPH::RVec3::sZero(),
        JPH::Quat::sIdentity(),
        JPH::EMotionType::Static,
        Layers::NonMoving);
    bodySettings.mFriction = std::clamp(desc.friction, 0.0f, 4.0f);
    bodySettings.mRestitution = std::clamp(desc.restitution, 0.0f, 1.0f);
    JPH::BodyInterface& bodies = m_impl->system.GetBodyInterface();
    const JPH::BodyID bodyId = bodies.CreateAndAddBody(bodySettings, JPH::EActivation::DontActivate);
    m_impl->bodies[id] = bodyId;
#else
    PhysicsTransform transform{};
    m_impl->bodies[id] = transform;
#endif

    Tracenf("[PHYSICS] terrain collider created body=%llu cells=%ux%u verts=%zu tris=%zu size=%.2fx%.2f heightGrid=%s",
        static_cast<unsigned long long>(id),
        desc.cellsX,
        desc.cellsZ,
        expectedHeights,
        static_cast<std::size_t>(desc.cellsX) * static_cast<std::size_t>(desc.cellsZ) * 2u,
        desc.widthMeters,
        desc.depthMeters,
        hasHeightGrid ? "yes" : "flat");
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

bool PhysicsWorld::Raycast(
    const float origin[3],
    const float direction[3],
    float maxDistance,
    PhysicsRaycastHit& outHit) const
{
    outHit = {};
    if (!m_impl || !m_impl->created || maxDistance <= 0.0f)
        return false;

    const float lengthSq =
        direction[0] * direction[0] +
        direction[1] * direction[1] +
        direction[2] * direction[2];
    if (lengthSq <= 0.0000001f)
        return false;

    const float invLength = 1.0f / std::sqrt(lengthSq);
    const float rayDir[3] = {
        direction[0] * invLength * maxDistance,
        direction[1] * invLength * maxDistance,
        direction[2] * invLength * maxDistance,
    };

#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    const JPH::RRayCast ray(
        JPH::RVec3(origin[0], origin[1], origin[2]),
        JPH::Vec3(rayDir[0], rayDir[1], rayDir[2]));
    JPH::RayCastResult result;
    if (!m_impl->system.GetNarrowPhaseQuery().CastRay(ray, result))
        return false;

    const JPH::RVec3 hitPosition = ray.GetPointOnRay(result.mFraction);
    JPH::Vec3 hitNormal = JPH::Vec3::sAxisY();
    {
        JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), result.mBodyID);
        if (lock.Succeeded())
            hitNormal = lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, hitPosition);
    }

    outHit.hit = true;
    outHit.bodyId = FindEngineBodyId(m_impl->bodies, result.mBodyID);
    outHit.position[0] = static_cast<float>(hitPosition.GetX());
    outHit.position[1] = static_cast<float>(hitPosition.GetY());
    outHit.position[2] = static_cast<float>(hitPosition.GetZ());
    outHit.normal[0] = hitNormal.GetX();
    outHit.normal[1] = hitNormal.GetY();
    outHit.normal[2] = hitNormal.GetZ();
    outHit.fraction = result.mFraction;
    outHit.distance = result.mFraction * maxDistance;
    return true;
#else
    (void)origin;
    (void)rayDir;
    (void)outHit;
    return false;
#endif
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
