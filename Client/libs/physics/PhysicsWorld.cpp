#include "PhysicsWorld.h"

#include "Debug.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(IXENGINE_PHYSICS_WITH_JOLT)
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollector.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#endif

namespace ixtreeme::physics
{
namespace
{
constexpr BodyId kInvalidBodyId = 0;
constexpr ConstraintId kInvalidConstraintId = 0;
constexpr float kPi = 3.14159265358979323846f;

struct PhysicsSurfaceProperties
{
    float friction = 0.6f;
    float restitution = 0.0f;
    PhysicsMaterialCombineMode frictionCombine = PhysicsMaterialCombineMode::Average;
    PhysicsMaterialCombineMode restitutionCombine = PhysicsMaterialCombineMode::Maximum;
    bool customMaterial = false;
};

float CombinePhysicsValue(float a, float b, PhysicsMaterialCombineMode mode)
{
    switch (mode)
    {
    case PhysicsMaterialCombineMode::Minimum:
        return std::min(a, b);
    case PhysicsMaterialCombineMode::Maximum:
        return std::max(a, b);
    case PhysicsMaterialCombineMode::Multiply:
        return a * b;
    case PhysicsMaterialCombineMode::Average:
    default:
        return (a + b) * 0.5f;
    }
}

#if defined(IXENGINE_PHYSICS_WITH_JOLT)
JPH::Vec3 SafeDirection(const float values[3], JPH::Vec3Arg fallback)
{
    JPH::Vec3 direction(values[0], values[1], values[2]);
    if (direction.LengthSq() <= 0.000001f)
        return fallback;
    return direction.Normalized();
}

JPH::Vec3 PerpendicularNormal(JPH::Vec3Arg axis)
{
    const JPH::Vec3 candidate = std::abs(axis.Dot(JPH::Vec3::sAxisY())) < 0.85f
        ? JPH::Vec3::sAxisY()
        : JPH::Vec3::sAxisX();
    JPH::Vec3 normal = axis.Cross(candidate);
    if (normal.LengthSq() <= 0.000001f)
        normal = JPH::Vec3::sAxisZ();
    return normal.Normalized();
}

JPH::ShapeRefC CreateFallbackBoxShape(const ColliderComponent& collider, float sx, float sy, float sz)
{
    return new JPH::BoxShape(JPH::Vec3(
        std::max(0.001f, collider.size[0] * sx * 0.5f),
        std::max(0.001f, collider.size[1] * sy * 0.5f),
        std::max(0.001f, collider.size[2] * sz * 0.5f)));
}

JPH::Array<JPH::Vec3> BuildConvexHullPoints(
    const std::vector<std::array<float, 3>>& meshVertices,
    const ColliderComponent& collider,
    float sx,
    float sy,
    float sz)
{
    JPH::Array<JPH::Vec3> points;
    if (meshVertices.empty())
        return points;

    constexpr std::size_t kMaxInputPoints = 256;
    points.reserve(static_cast<JPH::uint>(std::min(meshVertices.size(), kMaxInputPoints)));
    const std::size_t stride = std::max<std::size_t>(1u, (meshVertices.size() + kMaxInputPoints - 1u) / kMaxInputPoints);
    for (std::size_t i = 0; i < meshVertices.size() && points.size() < kMaxInputPoints; i += stride)
    {
        const auto& vertex = meshVertices[i];
        points.emplace_back(
            vertex[0] * sx + collider.center[0],
            vertex[1] * sy + collider.center[1],
            vertex[2] * sz + collider.center[2]);
    }
    return points;
}

namespace Layers
{
static constexpr JPH::ObjectLayer Default = static_cast<JPH::ObjectLayer>(PhysicsLayer::Default);
static constexpr JPH::ObjectLayer StaticWorld = static_cast<JPH::ObjectLayer>(PhysicsLayer::StaticWorld);
static constexpr JPH::ObjectLayer DynamicObject = static_cast<JPH::ObjectLayer>(PhysicsLayer::DynamicObject);
static constexpr JPH::ObjectLayer Player = static_cast<JPH::ObjectLayer>(PhysicsLayer::Player);
static constexpr JPH::ObjectLayer Trigger = static_cast<JPH::ObjectLayer>(PhysicsLayer::Trigger);
static constexpr JPH::ObjectLayer Projectile = static_cast<JPH::ObjectLayer>(PhysicsLayer::Projectile);
static constexpr JPH::ObjectLayer Foliage = static_cast<JPH::ObjectLayer>(PhysicsLayer::Foliage);
static constexpr JPH::ObjectLayer NoCollision = static_cast<JPH::ObjectLayer>(PhysicsLayer::NoCollision);
static constexpr JPH::ObjectLayer Count = static_cast<JPH::ObjectLayer>(PhysicsLayer::Count);
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
        return IsMostlyStaticLayer(layer) ? BroadPhaseLayers::NonMoving : BroadPhaseLayers::Moving;
    }

    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return layer == BroadPhaseLayers::NonMoving ? "NonMoving" : "Moving";
    }

private:
    static bool IsMostlyStaticLayer(JPH::ObjectLayer layer)
    {
        return layer == Layers::StaticWorld ||
            layer == Layers::Foliage ||
            layer == Layers::NoCollision;
    }
};

class ObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhaseLayer) const override
    {
        if (layer == Layers::NoCollision)
            return false;
        if (broadPhaseLayer == BroadPhaseLayers::NonMoving)
            return layer != Layers::StaticWorld && layer != Layers::Foliage;
        return true;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter
{
public:
    void SetMatrix(const PhysicsCollisionMatrix& matrix)
    {
        m_matrix = matrix;
    }

    bool ShouldCollide(JPH::ObjectLayer object1, JPH::ObjectLayer object2) const override
    {
        if (object1 >= Layers::Count || object2 >= Layers::Count)
            return false;
        return m_matrix[object1][object2];
    }

private:
    PhysicsCollisionMatrix m_matrix = DefaultPhysicsCollisionMatrix();
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

JPH::ObjectLayer ToJoltLayer(PhysicsLayer layer)
{
    if (layer == PhysicsLayer::Count)
        return Layers::Default;
    return static_cast<JPH::ObjectLayer>(layer);
}

PhysicsLayer FromJoltLayer(JPH::ObjectLayer layer)
{
    if (layer >= Layers::Count)
        return PhysicsLayer::Default;
    return static_cast<PhysicsLayer>(layer);
}

class QueryObjectLayerFilter final : public JPH::ObjectLayerFilter
{
public:
    explicit QueryObjectLayerFilter(std::uint32_t layerMask)
        : m_layerMask(layerMask)
    {
    }

    bool ShouldCollide(JPH::ObjectLayer layer) const override
    {
        if (layer >= Layers::Count)
            return false;
        return (m_layerMask & (1u << static_cast<std::uint32_t>(layer))) != 0u;
    }

private:
    std::uint32_t m_layerMask = AllPhysicsLayerMask();
};

class QueryBodyFilter final : public JPH::BodyFilter
{
public:
    explicit QueryBodyFilter(const PhysicsQueryFilter& filter)
        : m_filter(filter)
    {
    }

    bool ShouldCollideLocked(const JPH::Body& body) const override
    {
        if (m_filter.ignoreBody != 0 && static_cast<BodyId>(body.GetUserData()) == m_filter.ignoreBody)
            return false;
        if (!m_filter.hitTriggers && body.IsSensor())
            return false;
        return true;
    }

private:
    PhysicsQueryFilter m_filter;
};

JPH::EAllowedDOFs ToJoltAllowedDofs(const RigidbodyComponent& rigidbody)
{
    JPH::EAllowedDOFs allowed = JPH::EAllowedDOFs::None;
    if (!rigidbody.freezePosition[0])
        allowed |= JPH::EAllowedDOFs::TranslationX;
    if (!rigidbody.freezePosition[1])
        allowed |= JPH::EAllowedDOFs::TranslationY;
    if (!rigidbody.freezePosition[2])
        allowed |= JPH::EAllowedDOFs::TranslationZ;
    if (!rigidbody.freezeRotation[0])
        allowed |= JPH::EAllowedDOFs::RotationX;
    if (!rigidbody.freezeRotation[1])
        allowed |= JPH::EAllowedDOFs::RotationY;
    if (!rigidbody.freezeRotation[2])
        allowed |= JPH::EAllowedDOFs::RotationZ;

    // Jolt does not allow a dynamic body with zero DOFs. Keep it valid and let users use Static for full lock.
    return allowed == JPH::EAllowedDOFs::None ? JPH::EAllowedDOFs::All : allowed;
}

JPH::ShapeRefC CreateShape(
    const ColliderComponent& input,
    const float scale[3],
    const std::vector<std::array<float, 3>>& meshVertices,
    const std::vector<std::uint32_t>& meshIndices)
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
    case ColliderShape::ConvexHull:
    {
        JPH::Array<JPH::Vec3> points = BuildConvexHullPoints(meshVertices, collider, sx, sy, sz);
        if (points.size() < 4)
        {
            Tracenf("[PHYSICS] convex hull collider fallback=box reason=not-enough-points points=%u",
                static_cast<unsigned>(points.size()));
            baseShape = CreateFallbackBoxShape(collider, sx, sy, sz);
            break;
        }

        JPH::ConvexHullShapeSettings hullSettings(points);
        JPH::ShapeSettings::ShapeResult shapeResult = hullSettings.Create();
        if (shapeResult.HasError())
        {
            Tracenf("[PHYSICS] convex hull collider fallback=box reason=%s points=%u",
                shapeResult.GetError().c_str(),
                static_cast<unsigned>(points.size()));
            baseShape = CreateFallbackBoxShape(collider, sx, sy, sz);
            break;
        }
        baseShape = shapeResult.Get();
        collider.center[0] = 0.0f;
        collider.center[1] = 0.0f;
        collider.center[2] = 0.0f;
        break;
    }
    case ColliderShape::Mesh:
    {
        if (meshVertices.empty() || meshIndices.size() < 3)
        {
            Tracenf("[PHYSICS] mesh collider fallback=box reason=missing-geometry verts=%zu indices=%zu",
                meshVertices.size(),
                meshIndices.size());
            baseShape = CreateFallbackBoxShape(collider, sx, sy, sz);
            break;
        }

        JPH::VertexList vertices;
        vertices.reserve(meshVertices.size());
        for (const auto& vertex : meshVertices)
        {
            vertices.emplace_back(
                vertex[0] * sx + collider.center[0],
                vertex[1] * sy + collider.center[1],
                vertex[2] * sz + collider.center[2]);
        }

        JPH::IndexedTriangleList triangles;
        triangles.reserve(meshIndices.size() / 3u);
        for (std::size_t i = 0; i + 2 < meshIndices.size(); i += 3)
        {
            const std::uint32_t a = meshIndices[i + 0u];
            const std::uint32_t b = meshIndices[i + 1u];
            const std::uint32_t c = meshIndices[i + 2u];
            if (a >= meshVertices.size() || b >= meshVertices.size() || c >= meshVertices.size())
                continue;
            if (a == b || b == c || a == c)
                continue;
            triangles.emplace_back(a, b, c, 0u);
        }

        if (triangles.empty())
        {
            Tracenf("[PHYSICS] mesh collider fallback=box reason=no-valid-triangles verts=%zu indices=%zu",
                meshVertices.size(),
                meshIndices.size());
            baseShape = CreateFallbackBoxShape(collider, sx, sy, sz);
            break;
        }

        JPH::MeshShapeSettings meshSettings(std::move(vertices), std::move(triangles));
        JPH::ShapeSettings::ShapeResult shapeResult = meshSettings.Create();
        if (shapeResult.HasError())
        {
            Tracenf("[PHYSICS] mesh collider fallback=box reason=%s", shapeResult.GetError().c_str());
            baseShape = CreateFallbackBoxShape(collider, sx, sy, sz);
            break;
        }
        baseShape = shapeResult.Get();
        collider.center[0] = 0.0f;
        collider.center[1] = 0.0f;
        collider.center[2] = 0.0f;
        break;
    }
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
        baseShape = CreateFallbackBoxShape(collider, sx, sy, sz);
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

class EngineContactListener final : public JPH::ContactListener
{
public:
    explicit EngineContactListener(const std::unordered_map<BodyId, PhysicsSurfaceProperties>* surfaces)
        : m_surfaces(surfaces)
    {
    }

    void OnContactAdded(
        const JPH::Body& body1,
        const JPH::Body& body2,
        const JPH::ContactManifold& manifold,
        JPH::ContactSettings& settings) override
    {
        ApplyMaterialCombine(body1, body2, settings);
        PushContact(PhysicsContactPhase::Started, body1, body2, manifold, settings);
    }

    void OnContactPersisted(
        const JPH::Body& body1,
        const JPH::Body& body2,
        const JPH::ContactManifold& manifold,
        JPH::ContactSettings& settings) override
    {
        ApplyMaterialCombine(body1, body2, settings);
        PushContact(PhysicsContactPhase::Stayed, body1, body2, manifold, settings);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
    {
        std::scoped_lock lock(m_mutex);
        const auto subShapeIt = m_subShapeContacts.find(pair);
        if (subShapeIt == m_subShapeContacts.end())
            return;

        const BodyPairKey bodyPair = subShapeIt->second;
        m_subShapeContacts.erase(subShapeIt);

        auto bodyPairIt = m_bodyPairContacts.find(bodyPair);
        if (bodyPairIt == m_bodyPairContacts.end())
            return;

        BodyPairState& state = bodyPairIt->second;
        if (state.subShapeContactCount > 0)
            --state.subShapeContactCount;
        if (state.subShapeContactCount > 0)
            return;

        PhysicsContactEvent event = state.lastEvent;
        event.phase = PhysicsContactPhase::Ended;
        m_pendingEvents.push_back(event);
        m_pendingStayedPairs.erase(bodyPair);
        m_bodyPairContacts.erase(bodyPairIt);
    }

    std::vector<PhysicsContactEvent> DrainEvents()
    {
        std::scoped_lock lock(m_mutex);
        std::vector<PhysicsContactEvent> result;
        result.swap(m_pendingEvents);
        m_pendingStayedPairs.clear();
        return result;
    }

private:
    const PhysicsSurfaceProperties* FindSurface(const JPH::Body& body) const
    {
        if (!m_surfaces)
            return nullptr;
        const auto it = m_surfaces->find(EngineBodyId(body));
        return it == m_surfaces->end() ? nullptr : &it->second;
    }

    void ApplyMaterialCombine(const JPH::Body& body1, const JPH::Body& body2, JPH::ContactSettings& settings) const
    {
        const PhysicsSurfaceProperties* surface1 = FindSurface(body1);
        const PhysicsSurfaceProperties* surface2 = FindSurface(body2);
        if ((!surface1 || !surface1->customMaterial) && (!surface2 || !surface2->customMaterial))
            return;

        const PhysicsSurfaceProperties fallback1{
            body1.GetFriction(),
            body1.GetRestitution(),
            PhysicsMaterialCombineMode::Average,
            PhysicsMaterialCombineMode::Maximum,
            false};
        const PhysicsSurfaceProperties fallback2{
            body2.GetFriction(),
            body2.GetRestitution(),
            PhysicsMaterialCombineMode::Average,
            PhysicsMaterialCombineMode::Maximum,
            false};
        const PhysicsSurfaceProperties& a = surface1 ? *surface1 : fallback1;
        const PhysicsSurfaceProperties& b = surface2 ? *surface2 : fallback2;
        const PhysicsMaterialCombineMode frictionMode = a.customMaterial ? a.frictionCombine : b.frictionCombine;
        const PhysicsMaterialCombineMode restitutionMode = a.customMaterial ? a.restitutionCombine : b.restitutionCombine;
        settings.mCombinedFriction = std::max(0.0f, CombinePhysicsValue(a.friction, b.friction, frictionMode));
        settings.mCombinedRestitution = std::clamp(CombinePhysicsValue(a.restitution, b.restitution, restitutionMode), 0.0f, 1.0f);
    }

    struct BodyPairKey
    {
        std::uint32_t bodyA = 0;
        std::uint32_t bodyB = 0;

        bool operator==(const BodyPairKey& other) const
        {
            return bodyA == other.bodyA && bodyB == other.bodyB;
        }
    };

    struct BodyPairKeyHash
    {
        std::size_t operator()(const BodyPairKey& key) const
        {
            const std::size_t a = std::hash<std::uint32_t>{}(key.bodyA);
            const std::size_t b = std::hash<std::uint32_t>{}(key.bodyB);
            return a ^ (b + 0x9e3779b97f4a7c15ull + (a << 6u) + (a >> 2u));
        }
    };

    struct BodyPairState
    {
        std::uint32_t subShapeContactCount = 0;
        PhysicsContactEvent lastEvent{};
    };

    static BodyId EngineBodyId(const JPH::Body& body)
    {
        return static_cast<BodyId>(body.GetUserData());
    }

    static BodyPairKey MakeBodyPairKey(JPH::BodyID body1, JPH::BodyID body2)
    {
        const std::uint32_t id1 = body1.GetIndexAndSequenceNumber();
        const std::uint32_t id2 = body2.GetIndexAndSequenceNumber();
        return id1 < id2 ? BodyPairKey{id1, id2} : BodyPairKey{id2, id1};
    }

    static void StoreVec3(const JPH::Vec3& source, float out[3])
    {
        out[0] = source.GetX();
        out[1] = source.GetY();
        out[2] = source.GetZ();
    }

    static void StoreRVec3(const JPH::RVec3& source, float out[3])
    {
        out[0] = static_cast<float>(source.GetX());
        out[1] = static_cast<float>(source.GetY());
        out[2] = static_cast<float>(source.GetZ());
    }

    void PushContact(
        PhysicsContactPhase phase,
        const JPH::Body& body1,
        const JPH::Body& body2,
        const JPH::ContactManifold& manifold,
        const JPH::ContactSettings& settings)
    {
        PhysicsContactEvent event{};
        event.phase = phase;
        event.kind = settings.mIsSensor || body1.IsSensor() || body2.IsSensor()
            ? PhysicsContactKind::Trigger
            : PhysicsContactKind::Collision;
        event.bodyA = EngineBodyId(body1);
        event.bodyB = EngineBodyId(body2);
        event.penetrationDepth = manifold.mPenetrationDepth;
        StoreVec3(manifold.mWorldSpaceNormal, event.normal);
        if (!manifold.mRelativeContactPointsOn1.empty())
            StoreRVec3(manifold.GetWorldSpaceContactPointOn1(0), event.point);

        std::scoped_lock lock(m_mutex);
        const JPH::SubShapeIDPair key(body1.GetID(), manifold.mSubShapeID1, body2.GetID(), manifold.mSubShapeID2);
        const BodyPairKey bodyPair = MakeBodyPairKey(body1.GetID(), body2.GetID());
        if (phase == PhysicsContactPhase::Started)
        {
            auto [contactIt, inserted] = m_subShapeContacts.emplace(key, bodyPair);
            if (!inserted)
            {
                m_bodyPairContacts[contactIt->second].lastEvent = event;
                return;
            }

            BodyPairState& state = m_bodyPairContacts[bodyPair];
            state.lastEvent = event;
            const bool bodyPairWasInactive = state.subShapeContactCount == 0;
            ++state.subShapeContactCount;
            if (bodyPairWasInactive)
                m_pendingEvents.push_back(event);
            return;
        }

        auto [subShapeIt, inserted] = m_subShapeContacts.emplace(key, bodyPair);
        if (inserted)
        {
            BodyPairState& state = m_bodyPairContacts[bodyPair];
            ++state.subShapeContactCount;
        }

        BodyPairState& state = m_bodyPairContacts[bodyPair];
        state.lastEvent = event;
        if (m_pendingStayedPairs.insert(bodyPair).second)
            m_pendingEvents.push_back(event);
    }

    std::mutex m_mutex;
    std::vector<PhysicsContactEvent> m_pendingEvents;
    std::unordered_map<JPH::SubShapeIDPair, BodyPairKey> m_subShapeContacts;
    std::unordered_map<BodyPairKey, BodyPairState, BodyPairKeyHash> m_bodyPairContacts;
    std::unordered_set<BodyPairKey, BodyPairKeyHash> m_pendingStayedPairs;
    const std::unordered_map<BodyId, PhysicsSurfaceProperties>* m_surfaces = nullptr;
};
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
    std::unordered_map<BodyId, PhysicsSurfaceProperties> bodySurfaces;
    std::unique_ptr<EngineContactListener> contactListener;
    JPH::PhysicsSystem system;
    std::unordered_map<BodyId, JPH::BodyID> bodies;
    std::unordered_map<ConstraintId, JPH::Ref<JPH::Constraint>> constraints;
#else
    std::unordered_map<BodyId, PhysicsTransform> bodies;
#endif
    std::vector<PhysicsContactEvent> frameContactEvents;
    float gravity[3] = {0.0f, -9.81f, 0.0f};
    BodyId nextBodyId = 1;
    ConstraintId nextConstraintId = 1;
    bool created = false;
};

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld()
{
    Destroy();
}

PhysicsCollisionMatrix DefaultPhysicsCollisionMatrix()
{
    PhysicsCollisionMatrix matrix{};
    for (std::size_t a = 0; a < static_cast<std::size_t>(PhysicsLayer::Count); ++a)
    {
        for (std::size_t b = 0; b < static_cast<std::size_t>(PhysicsLayer::Count); ++b)
        {
            matrix[a][b] = DefaultLayerCollision(
                static_cast<PhysicsLayer>(a),
                static_cast<PhysicsLayer>(b));
        }
    }
    return matrix;
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
    m_impl->system.SetGravity(JPH::Vec3(m_impl->gravity[0], m_impl->gravity[1], m_impl->gravity[2]));
    m_impl->contactListener = std::make_unique<EngineContactListener>(&m_impl->bodySurfaces);
    m_impl->system.SetContactListener(m_impl->contactListener.get());
    Tracenf("[PHYSICS] backend=Jolt created gravity=(%.2f,%.2f,%.2f) tempAllocatorMB=%u",
        m_impl->gravity[0],
        m_impl->gravity[1],
        m_impl->gravity[2],
        tempAllocatorSizeBytes / (1024u * 1024u));
#else
    Tracen("[PHYSICS] backend=Null created");
#endif
    m_impl->created = true;
    return true;
}

void PhysicsWorld::SetGravity(const float gravity[3])
{
    if (!m_impl)
        m_impl = std::make_unique<Impl>();
    m_impl->gravity[0] = std::clamp(gravity[0], -1000.0f, 1000.0f);
    m_impl->gravity[1] = std::clamp(gravity[1], -1000.0f, 1000.0f);
    m_impl->gravity[2] = std::clamp(gravity[2], -1000.0f, 1000.0f);
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    if (m_impl->created)
        m_impl->system.SetGravity(JPH::Vec3(m_impl->gravity[0], m_impl->gravity[1], m_impl->gravity[2]));
#endif
}

void PhysicsWorld::SetCollisionMatrix(const PhysicsCollisionMatrix& matrix)
{
    if (!m_impl)
        m_impl = std::make_unique<Impl>();
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    m_impl->objectLayerPairFilter.SetMatrix(matrix);
    std::uint32_t enabledPairs = 0;
    for (std::size_t a = 0; a < static_cast<std::size_t>(PhysicsLayer::Count); ++a)
    {
        for (std::size_t b = a; b < static_cast<std::size_t>(PhysicsLayer::Count); ++b)
        {
            if (matrix[a][b])
                ++enabledPairs;
        }
    }
    Tracenf("[PHYSICS-LAYER] matrix loaded layers=%u enabled_pairs=%u",
        PhysicsLayerCount(),
        enabledPairs);
#else
    (void)matrix;
#endif
}

void PhysicsWorld::Destroy()
{
    if (!m_impl)
        return;

#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    if (m_impl->created)
    {
        m_impl->system.SetContactListener(nullptr);
        for (const auto& entry : m_impl->constraints)
            m_impl->system.RemoveConstraint(entry.second);
        m_impl->constraints.clear();
        JPH::BodyInterface& bodies = m_impl->system.GetBodyInterface();
        for (const auto& entry : m_impl->bodies)
        {
            bodies.RemoveBody(entry.second);
            bodies.DestroyBody(entry.second);
        }
    }
#endif
    m_impl->bodies.clear();
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    m_impl->bodySurfaces.clear();
#endif
    m_impl->frameContactEvents.clear();
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
    if (collider.shape == ColliderShape::Mesh && desc.bodyType == BodyType::Dynamic)
    {
        Tracenf("[PHYSICS] mesh collider fallback=box reason=dynamic-body-not-supported");
        collider.shape = ColliderShape::Box;
    }
    JPH::ShapeRefC shape = CreateShape(collider, desc.scale, desc.meshVertices, desc.meshIndices);
    JPH::BodyCreationSettings settings(
        shape,
        JPH::RVec3(desc.transform.position[0],
            desc.transform.position[1],
            desc.transform.position[2]),
        JPH::Quat(desc.transform.rotation[0], desc.transform.rotation[1], desc.transform.rotation[2], desc.transform.rotation[3]),
        ToJoltMotion(desc.bodyType),
        ToJoltLayer(collider.layer));
    settings.mUserData = id;
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
    settings.mAllowSleeping = rigidbody.allowSleeping;
    settings.mMotionQuality = rigidbody.continuousCollision
        ? JPH::EMotionQuality::LinearCast
        : JPH::EMotionQuality::Discrete;
    settings.mAllowedDOFs = ToJoltAllowedDofs(rigidbody);
    const JPH::BodyID bodyId = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
    m_impl->bodies[id] = bodyId;
    m_impl->bodySurfaces[id] = PhysicsSurfaceProperties{
        collider.friction,
        collider.restitution,
        collider.frictionCombine,
        collider.restitutionCombine,
        !collider.materialAssetId.empty()};
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
        ToJoltLayer(desc.layer));
    bodySettings.mUserData = id;
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

ConstraintId PhysicsWorld::CreateFixedJoint(const FixedJointDesc& desc)
{
    if (!m_impl || !m_impl->created || desc.bodyA == 0 || desc.bodyB == 0 || desc.bodyA == desc.bodyB)
        return kInvalidConstraintId;
    auto bodyA = m_impl->bodies.find(desc.bodyA);
    auto bodyB = m_impl->bodies.find(desc.bodyB);
    if (bodyA == m_impl->bodies.end() || bodyB == m_impl->bodies.end())
        return kInvalidConstraintId;

    const ConstraintId id = m_impl->nextConstraintId++;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    JPH::FixedConstraintSettings settings;
    settings.mAutoDetectPoint = true;
    JPH::BodyInterface& bodies = m_impl->system.GetBodyInterface();
    JPH::TwoBodyConstraint* rawConstraint = bodies.CreateConstraint(&settings, bodyA->second, bodyB->second);
    if (!rawConstraint)
        return kInvalidConstraintId;
    JPH::Ref<JPH::Constraint> constraint = rawConstraint;
    m_impl->system.AddConstraint(constraint);
    bodies.ActivateConstraint(rawConstraint);
    m_impl->constraints[id] = constraint;
#else
    (void)desc;
    return kInvalidConstraintId;
#endif
    return id;
}

ConstraintId PhysicsWorld::CreateHingeJoint(const HingeJointDesc& desc)
{
    if (!m_impl || !m_impl->created || desc.bodyA == 0 || desc.bodyB == 0 || desc.bodyA == desc.bodyB)
        return kInvalidConstraintId;
    auto bodyA = m_impl->bodies.find(desc.bodyA);
    auto bodyB = m_impl->bodies.find(desc.bodyB);
    if (bodyA == m_impl->bodies.end() || bodyB == m_impl->bodies.end())
        return kInvalidConstraintId;

    const ConstraintId id = m_impl->nextConstraintId++;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    const JPH::Vec3 axis = SafeDirection(desc.axis, JPH::Vec3::sAxisY());
    const JPH::Vec3 normal = PerpendicularNormal(axis);
    JPH::HingeConstraintSettings settings;
    settings.mPoint1 = JPH::RVec3(desc.anchor[0], desc.anchor[1], desc.anchor[2]);
    settings.mPoint2 = settings.mPoint1;
    settings.mHingeAxis1 = axis;
    settings.mHingeAxis2 = axis;
    settings.mNormalAxis1 = normal;
    settings.mNormalAxis2 = normal;
    settings.mLimitsMin = desc.limitsEnabled ? std::clamp(desc.minAngleRadians, -kPi, 0.0f) : -kPi;
    settings.mLimitsMax = desc.limitsEnabled ? std::clamp(desc.maxAngleRadians, 0.0f, kPi) : kPi;
    if (settings.mLimitsMin > settings.mLimitsMax)
        std::swap(settings.mLimitsMin, settings.mLimitsMax);
    settings.mMaxFrictionTorque = std::max(0.0f, desc.frictionTorque);

    JPH::BodyInterface& bodies = m_impl->system.GetBodyInterface();
    JPH::TwoBodyConstraint* rawConstraint = bodies.CreateConstraint(&settings, bodyA->second, bodyB->second);
    if (!rawConstraint)
        return kInvalidConstraintId;
    JPH::Ref<JPH::Constraint> constraint = rawConstraint;
    m_impl->system.AddConstraint(constraint);
    bodies.ActivateConstraint(rawConstraint);
    m_impl->constraints[id] = constraint;
#else
    (void)desc;
    return kInvalidConstraintId;
#endif
    return id;
}

void PhysicsWorld::DestroyConstraint(ConstraintId id)
{
    if (!m_impl || id == 0)
        return;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    auto it = m_impl->constraints.find(id);
    if (it == m_impl->constraints.end())
        return;
    if (m_impl->created)
        m_impl->system.RemoveConstraint(it->second);
    m_impl->constraints.erase(it);
#else
    (void)id;
#endif
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
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    m_impl->bodySurfaces.erase(id);
#endif
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

bool PhysicsWorld::SetLinearVelocity(BodyId id, const float velocity[3])
{
    if (!m_impl || !m_impl->created)
        return false;
    auto it = m_impl->bodies.find(id);
    if (it == m_impl->bodies.end())
        return false;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    JPH::BodyInterface& bodies = m_impl->system.GetBodyInterface();
    if (bodies.GetMotionType(it->second) == JPH::EMotionType::Static)
        return false;
    bodies.SetLinearVelocity(it->second, JPH::Vec3(velocity[0], velocity[1], velocity[2]));
#else
    (void)velocity;
    return false;
#endif
    return true;
}

bool PhysicsWorld::GetLinearVelocity(BodyId id, float outVelocity[3]) const
{
    if (!outVelocity)
        return false;
    outVelocity[0] = 0.0f;
    outVelocity[1] = 0.0f;
    outVelocity[2] = 0.0f;
    if (!m_impl || !m_impl->created)
        return false;
    auto it = m_impl->bodies.find(id);
    if (it == m_impl->bodies.end())
        return false;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    const JPH::Vec3 velocity = m_impl->system.GetBodyInterface().GetLinearVelocity(it->second);
    outVelocity[0] = velocity.GetX();
    outVelocity[1] = velocity.GetY();
    outVelocity[2] = velocity.GetZ();
#else
    (void)it;
    return false;
#endif
    return true;
}

bool PhysicsWorld::GetAngularVelocity(BodyId id, float outVelocity[3]) const
{
    if (!outVelocity)
        return false;
    outVelocity[0] = 0.0f;
    outVelocity[1] = 0.0f;
    outVelocity[2] = 0.0f;
    if (!m_impl || !m_impl->created)
        return false;
    auto it = m_impl->bodies.find(id);
    if (it == m_impl->bodies.end())
        return false;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    const JPH::Vec3 velocity = m_impl->system.GetBodyInterface().GetAngularVelocity(it->second);
    outVelocity[0] = velocity.GetX();
    outVelocity[1] = velocity.GetY();
    outVelocity[2] = velocity.GetZ();
#else
    (void)it;
    return false;
#endif
    return true;
}

bool PhysicsWorld::IsBodyActive(BodyId id) const
{
    if (!m_impl || !m_impl->created)
        return false;
    auto it = m_impl->bodies.find(id);
    if (it == m_impl->bodies.end())
        return false;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    return m_impl->system.GetBodyInterface().IsActive(it->second);
#else
    (void)it;
    return false;
#endif
}

bool PhysicsWorld::MoveKinematic(BodyId id, const PhysicsTransform& targetTransform, float deltaSeconds)
{
    if (!m_impl || !m_impl->created)
        return false;
    auto it = m_impl->bodies.find(id);
    if (it == m_impl->bodies.end())
        return false;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    JPH::BodyInterface& bodies = m_impl->system.GetBodyInterface();
    if (bodies.GetMotionType(it->second) != JPH::EMotionType::Kinematic)
        return false;
    const float safeDelta = std::clamp(deltaSeconds, 1.0f / 240.0f, 1.0f / 15.0f);
    bodies.MoveKinematic(
        it->second,
        JPH::RVec3(targetTransform.position[0], targetTransform.position[1], targetTransform.position[2]),
        JPH::Quat(
            targetTransform.rotation[0],
            targetTransform.rotation[1],
            targetTransform.rotation[2],
            targetTransform.rotation[3]),
        safeDelta);
#else
    (void)targetTransform;
    (void)deltaSeconds;
    return false;
#endif
    return true;
}

bool PhysicsWorld::AddForce(BodyId id, const float force[3])
{
    if (!m_impl || !m_impl->created)
        return false;
    auto it = m_impl->bodies.find(id);
    if (it == m_impl->bodies.end())
        return false;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    JPH::BodyInterface& bodies = m_impl->system.GetBodyInterface();
    if (bodies.GetMotionType(it->second) != JPH::EMotionType::Dynamic)
        return false;
    bodies.AddForce(it->second, JPH::Vec3(force[0], force[1], force[2]), JPH::EActivation::Activate);
#else
    (void)force;
    return false;
#endif
    return true;
}

bool PhysicsWorld::AddImpulse(BodyId id, const float impulse[3])
{
    if (!m_impl || !m_impl->created)
        return false;
    auto it = m_impl->bodies.find(id);
    if (it == m_impl->bodies.end())
        return false;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    JPH::BodyInterface& bodies = m_impl->system.GetBodyInterface();
    if (bodies.GetMotionType(it->second) != JPH::EMotionType::Dynamic)
        return false;
    bodies.AddImpulse(it->second, JPH::Vec3(impulse[0], impulse[1], impulse[2]));
#else
    (void)impulse;
    return false;
#endif
    return true;
}

bool PhysicsWorld::AddAngularImpulse(BodyId id, const float angularImpulse[3])
{
    if (!m_impl || !m_impl->created)
        return false;
    auto it = m_impl->bodies.find(id);
    if (it == m_impl->bodies.end())
        return false;
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    JPH::BodyInterface& bodies = m_impl->system.GetBodyInterface();
    if (bodies.GetMotionType(it->second) != JPH::EMotionType::Dynamic)
        return false;
    bodies.AddAngularImpulse(it->second, JPH::Vec3(angularImpulse[0], angularImpulse[1], angularImpulse[2]));
#else
    (void)angularImpulse;
    return false;
#endif
    return true;
}

bool PhysicsWorld::Raycast(
    const float origin[3],
    const float direction[3],
    float maxDistance,
    PhysicsRaycastHit& outHit) const
{
    return Raycast(origin, direction, maxDistance, PhysicsQueryFilter{}, outHit);
}

bool PhysicsWorld::Raycast(
    const float origin[3],
    const float direction[3],
    float maxDistance,
    const PhysicsQueryFilter& filter,
    PhysicsRaycastHit& outHit) const
{
    outHit = {};
    if (!m_impl || !m_impl->created || maxDistance <= 0.0f || filter.layerMask == 0u)
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
    QueryObjectLayerFilter objectLayerFilter(filter.layerMask);
    QueryBodyFilter bodyFilter(filter);
    const JPH::RRayCast ray(
        JPH::RVec3(origin[0], origin[1], origin[2]),
        JPH::Vec3(rayDir[0], rayDir[1], rayDir[2]));
    JPH::RayCastResult result;
    if (!m_impl->system.GetNarrowPhaseQuery().CastRay(ray, result, {}, objectLayerFilter, bodyFilter))
        return false;

    const JPH::RVec3 hitPosition = ray.GetPointOnRay(result.mFraction);
    JPH::Vec3 hitNormal = JPH::Vec3::sAxisY();
    PhysicsLayer hitLayer = PhysicsLayer::Default;
    bool hitTrigger = false;
    {
        JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), result.mBodyID);
        if (lock.Succeeded())
        {
            const JPH::Body& body = lock.GetBody();
            hitNormal = body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, hitPosition);
            hitLayer = FromJoltLayer(body.GetObjectLayer());
            hitTrigger = body.IsSensor();
        }
    }

    outHit.hit = true;
    outHit.bodyId = FindEngineBodyId(m_impl->bodies, result.mBodyID);
    outHit.layer = hitLayer;
    outHit.trigger = hitTrigger;
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
    (void)filter;
    (void)outHit;
    return false;
#endif
}

std::vector<PhysicsOverlapHit> PhysicsWorld::OverlapSphere(
    const float center[3],
    float radius,
    const PhysicsQueryFilter& filter,
    std::size_t maxHits) const
{
    std::vector<PhysicsOverlapHit> hits;
    if (!m_impl || !m_impl->created || radius <= 0.0f || filter.layerMask == 0u || maxHits == 0)
        return hits;

#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    JPH::SphereShape sphere(radius);
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    QueryObjectLayerFilter objectLayerFilter(filter.layerMask);
    QueryBodyFilter bodyFilter(filter);
    const JPH::RVec3 baseOffset(center[0], center[1], center[2]);
    m_impl->system.GetNarrowPhaseQuery().CollideShape(
        &sphere,
        JPH::Vec3::sOne(),
        JPH::RMat44::sTranslation(baseOffset),
        settings,
        JPH::RVec3::sZero(),
        collector,
        {},
        objectLayerFilter,
        bodyFilter);

    hits.reserve(std::min<std::size_t>(collector.mHits.size(), maxHits));
    for (const JPH::CollideShapeResult& result : collector.mHits)
    {
        if (hits.size() >= maxHits)
            break;

        PhysicsOverlapHit hit{};
        hit.bodyId = FindEngineBodyId(m_impl->bodies, result.mBodyID2);
        hit.position[0] = result.mContactPointOn2.GetX();
        hit.position[1] = result.mContactPointOn2.GetY();
        hit.position[2] = result.mContactPointOn2.GetZ();
        const JPH::Vec3 normal = result.mPenetrationAxis.LengthSq() > 0.000001f
            ? -result.mPenetrationAxis.Normalized()
            : JPH::Vec3::sAxisY();
        hit.normal[0] = normal.GetX();
        hit.normal[1] = normal.GetY();
        hit.normal[2] = normal.GetZ();
        hit.penetrationDepth = result.mPenetrationDepth;

        JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), result.mBodyID2);
        if (lock.Succeeded())
        {
            const JPH::Body& body = lock.GetBody();
            hit.layer = FromJoltLayer(body.GetObjectLayer());
            hit.trigger = body.IsSensor();
        }
        hits.push_back(hit);
    }
#else
    (void)center;
    (void)radius;
    (void)filter;
    (void)maxHits;
#endif
    return hits;
}

std::vector<PhysicsOverlapHit> PhysicsWorld::OverlapBox(
    const float center[3],
    const float halfExtents[3],
    const PhysicsQueryFilter& filter,
    std::size_t maxHits) const
{
    std::vector<PhysicsOverlapHit> hits;
    if (!m_impl || !m_impl->created || filter.layerMask == 0u || maxHits == 0)
        return hits;
    if (halfExtents[0] <= 0.0f || halfExtents[1] <= 0.0f || halfExtents[2] <= 0.0f)
        return hits;

#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    JPH::BoxShape box(JPH::Vec3(halfExtents[0], halfExtents[1], halfExtents[2]));
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    QueryObjectLayerFilter objectLayerFilter(filter.layerMask);
    QueryBodyFilter bodyFilter(filter);
    const JPH::RVec3 baseOffset(center[0], center[1], center[2]);
    m_impl->system.GetNarrowPhaseQuery().CollideShape(
        &box,
        JPH::Vec3::sOne(),
        JPH::RMat44::sTranslation(baseOffset),
        settings,
        JPH::RVec3::sZero(),
        collector,
        {},
        objectLayerFilter,
        bodyFilter);

    hits.reserve(std::min<std::size_t>(collector.mHits.size(), maxHits));
    for (const JPH::CollideShapeResult& result : collector.mHits)
    {
        if (hits.size() >= maxHits)
            break;

        PhysicsOverlapHit hit{};
        hit.bodyId = FindEngineBodyId(m_impl->bodies, result.mBodyID2);
        hit.position[0] = result.mContactPointOn2.GetX();
        hit.position[1] = result.mContactPointOn2.GetY();
        hit.position[2] = result.mContactPointOn2.GetZ();
        const JPH::Vec3 normal = result.mPenetrationAxis.LengthSq() > 0.000001f
            ? -result.mPenetrationAxis.Normalized()
            : JPH::Vec3::sAxisY();
        hit.normal[0] = normal.GetX();
        hit.normal[1] = normal.GetY();
        hit.normal[2] = normal.GetZ();
        hit.penetrationDepth = result.mPenetrationDepth;

        JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), result.mBodyID2);
        if (lock.Succeeded())
        {
            const JPH::Body& body = lock.GetBody();
            hit.layer = FromJoltLayer(body.GetObjectLayer());
            hit.trigger = body.IsSensor();
        }
        hits.push_back(hit);
    }
#else
    (void)center;
    (void)halfExtents;
    (void)filter;
    (void)maxHits;
#endif
    return hits;
}

std::vector<PhysicsOverlapHit> PhysicsWorld::OverlapCapsule(
    const float center[3],
    float halfHeight,
    float radius,
    const PhysicsQueryFilter& filter,
    std::size_t maxHits) const
{
    std::vector<PhysicsOverlapHit> hits;
    if (!m_impl || !m_impl->created || radius <= 0.0f || halfHeight < 0.0f || filter.layerMask == 0u || maxHits == 0)
        return hits;

#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    JPH::CapsuleShape capsule(halfHeight, radius);
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    QueryObjectLayerFilter objectLayerFilter(filter.layerMask);
    QueryBodyFilter bodyFilter(filter);
    const JPH::RVec3 baseOffset(center[0], center[1], center[2]);
    m_impl->system.GetNarrowPhaseQuery().CollideShape(
        &capsule,
        JPH::Vec3::sOne(),
        JPH::RMat44::sTranslation(baseOffset),
        settings,
        JPH::RVec3::sZero(),
        collector,
        {},
        objectLayerFilter,
        bodyFilter);

    hits.reserve(std::min<std::size_t>(collector.mHits.size(), maxHits));
    for (const JPH::CollideShapeResult& result : collector.mHits)
    {
        if (hits.size() >= maxHits)
            break;

        PhysicsOverlapHit hit{};
        hit.bodyId = FindEngineBodyId(m_impl->bodies, result.mBodyID2);
        hit.position[0] = result.mContactPointOn2.GetX();
        hit.position[1] = result.mContactPointOn2.GetY();
        hit.position[2] = result.mContactPointOn2.GetZ();
        const JPH::Vec3 normal = result.mPenetrationAxis.LengthSq() > 0.000001f
            ? -result.mPenetrationAxis.Normalized()
            : JPH::Vec3::sAxisY();
        hit.normal[0] = normal.GetX();
        hit.normal[1] = normal.GetY();
        hit.normal[2] = normal.GetZ();
        hit.penetrationDepth = result.mPenetrationDepth;

        JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), result.mBodyID2);
        if (lock.Succeeded())
        {
            const JPH::Body& body = lock.GetBody();
            hit.layer = FromJoltLayer(body.GetObjectLayer());
            hit.trigger = body.IsSensor();
        }
        hits.push_back(hit);
    }
#else
    (void)center;
    (void)halfHeight;
    (void)radius;
    (void)filter;
    (void)maxHits;
#endif
    return hits;
}

void PhysicsWorld::Step(float deltaSeconds)
{
    if (!m_impl || !m_impl->created || deltaSeconds <= 0.0f)
        return;
    m_impl->frameContactEvents.clear();
#if defined(IXENGINE_PHYSICS_WITH_JOLT)
    constexpr int collisionSteps = 1;
    m_impl->system.Update(deltaSeconds, collisionSteps, m_impl->tempAllocator.get(), m_impl->jobSystem.get());
    if (m_impl->contactListener)
        m_impl->frameContactEvents = m_impl->contactListener->DrainEvents();
#else
    (void)deltaSeconds;
#endif
}

const std::vector<PhysicsContactEvent>& PhysicsWorld::ContactEvents() const
{
    static const std::vector<PhysicsContactEvent> empty;
    if (!m_impl)
        return empty;
    return m_impl->frameContactEvents;
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
