#pragma once

#include "Matrix.h"
#include "Scalar.h"
#include "Types.h"
#include "Vector.h"

#include <math.h>
#include <cmath>
#include <limits>

namespace ixtreeme::math
{
struct RaycastHit
{
    bool hit = false;
    float distance = 0.0f;
    Vec3 point{};
    Vec3 normal{};
    Vec3 barycentric{};
};

enum class Containment
{
    Outside,
    Intersects,
    Inside
};

inline Vec3 GetPoint(const Ray& ray, float distance)
{
    return ray.origin + ray.direction * distance;
}

inline Ray NormalizeRay(Ray ray)
{
    ray.direction = SafeNormalize(ray.direction, {0.0f, 0.0f, 1.0f});
    return ray;
}

inline Plane MakePlane(Vec3 normal, float distance)
{
    const float length = Length(normal);
    if (length <= Epsilon)
        return {};
    return {normal / length, distance / length};
}

inline Plane PlaneFromPointNormal(Vec3 point, Vec3 normal)
{
    const Vec3 n = SafeNormalize(normal);
    return {n, -Dot(n, point)};
}

inline Plane PlaneFromTriangle(Vec3 a, Vec3 b, Vec3 c)
{
    return PlaneFromPointNormal(a, Cross(b - a, c - a));
}

inline Plane NormalizePlane(Plane plane)
{
    return MakePlane(plane.normal, plane.distance);
}

inline float SignedDistance(Plane plane, Vec3 point)
{
    return Dot(plane.normal, point) + plane.distance;
}

inline Vec3 ClosestPoint(Plane plane, Vec3 point)
{
    return point - plane.normal * SignedDistance(plane, point);
}

inline Aabb EmptyAabb()
{
    const float inf = std::numeric_limits<float>::infinity();
    return {{inf, inf, inf}, {-inf, -inf, -inf}};
}

inline Aabb MakeAabb(Vec3 minValue, Vec3 maxValue)
{
    return {Min(minValue, maxValue), Max(minValue, maxValue)};
}

inline bool IsValid(Aabb box)
{
    return box.min.x <= box.max.x && box.min.y <= box.max.y && box.min.z <= box.max.z;
}

inline Vec3 Center(Aabb box)
{
    return (box.min + box.max) * 0.5f;
}

inline Vec3 Size(Aabb box)
{
    return box.max - box.min;
}

inline Vec3 Extents(Aabb box)
{
    return Size(box) * 0.5f;
}

inline float SurfaceArea(Aabb box)
{
    const Vec3 size = Size(box);
    return 2.0f * (size.x * size.y + size.y * size.z + size.z * size.x);
}

inline float Volume(Aabb box)
{
    const Vec3 size = Size(box);
    return size.x * size.y * size.z;
}

inline Aabb Expand(Aabb box, Vec3 point)
{
    if (!IsValid(box))
        return {point, point};
    return {Min(box.min, point), Max(box.max, point)};
}

inline Aabb Expand(Aabb box, Aabb other)
{
    if (!IsValid(box))
        return other;
    if (!IsValid(other))
        return box;
    return {Min(box.min, other.min), Max(box.max, other.max)};
}

inline Aabb Inflate(Aabb box, Vec3 amount)
{
    return {box.min - amount, box.max + amount};
}

inline bool Contains(Aabb box, Vec3 point)
{
    return point.x >= box.min.x && point.x <= box.max.x &&
        point.y >= box.min.y && point.y <= box.max.y &&
        point.z >= box.min.z && point.z <= box.max.z;
}

inline bool Contains(Aabb outer, Aabb inner)
{
    return Contains(outer, inner.min) && Contains(outer, inner.max);
}

inline bool Overlaps(Aabb a, Aabb b)
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x &&
        a.min.y <= b.max.y && a.max.y >= b.min.y &&
        a.min.z <= b.max.z && a.max.z >= b.min.z;
}

inline Vec3 ClosestPoint(Aabb box, Vec3 point)
{
    return Clamp(point, box.min, box.max);
}

inline Aabb TransformAabb(Aabb box, const Mat4& transform)
{
    if (!IsValid(box))
        return EmptyAabb();

    Aabb result = EmptyAabb();
    for (int x = 0; x < 2; ++x)
    {
        for (int y = 0; y < 2; ++y)
        {
            for (int z = 0; z < 2; ++z)
            {
                const Vec3 corner{
                    x == 0 ? box.min.x : box.max.x,
                    y == 0 ? box.min.y : box.max.y,
                    z == 0 ? box.min.z : box.max.z};
                result = Expand(result, TransformPoint(transform, corner));
            }
        }
    }
    return result;
}

inline Sphere SphereFromAabb(Aabb box)
{
    const Vec3 center = Center(box);
    return {center, Distance(center, box.max)};
}

inline bool Contains(Sphere sphere, Vec3 point)
{
    return DistanceSquared(sphere.center, point) <= sphere.radius * sphere.radius;
}

inline bool Overlaps(Sphere a, Sphere b)
{
    const float radius = a.radius + b.radius;
    return DistanceSquared(a.center, b.center) <= radius * radius;
}

inline bool Overlaps(Sphere sphere, Aabb box)
{
    return DistanceSquared(sphere.center, ClosestPoint(box, sphere.center)) <= sphere.radius * sphere.radius;
}

inline bool IntersectRayPlane(Ray ray, Plane plane, float& distance)
{
    const float denom = Dot(plane.normal, ray.direction);
    if (Abs(denom) <= Epsilon)
        return false;

    distance = -SignedDistance(plane, ray.origin) / denom;
    return distance >= 0.0f;
}

inline bool IntersectRaySphere(Ray ray, Sphere sphere, float& distance)
{
    const Vec3 oc = ray.origin - sphere.center;
    const float a = Dot(ray.direction, ray.direction);
    const float b = 2.0f * Dot(oc, ray.direction);
    const float c = Dot(oc, oc) - sphere.radius * sphere.radius;
    const float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f || Abs(a) <= Epsilon)
        return false;

    const float sqrtDisc = Sqrt(discriminant);
    const float invDenom = 1.0f / (2.0f * a);
    const float t0 = (-b - sqrtDisc) * invDenom;
    const float t1 = (-b + sqrtDisc) * invDenom;
    if (t0 >= 0.0f)
    {
        distance = t0;
        return true;
    }
    if (t1 >= 0.0f)
    {
        distance = t1;
        return true;
    }
    return false;
}

inline bool IntersectRayAabb(Ray ray, Aabb box, float& distanceMin, float& distanceMax)
{
    distanceMin = 0.0f;
    distanceMax = std::numeric_limits<float>::infinity();

    for (int axis = 0; axis < 3; ++axis)
    {
        const float origin = ray.origin[axis];
        const float direction = ray.direction[axis];
        const float minValue = box.min[axis];
        const float maxValue = box.max[axis];

        if (Abs(direction) <= Epsilon)
        {
            if (origin < minValue || origin > maxValue)
                return false;
            continue;
        }

        const float invDirection = 1.0f / direction;
        float t0 = (minValue - origin) * invDirection;
        float t1 = (maxValue - origin) * invDirection;
        if (t0 > t1)
            std::swap(t0, t1);

        distanceMin = Max(distanceMin, t0);
        distanceMax = Min(distanceMax, t1);
        if (distanceMin > distanceMax)
            return false;
    }

    return true;
}

inline bool IntersectRayAabb(Ray ray, Aabb box, float& distance)
{
    float distanceMax = 0.0f;
    if (!IntersectRayAabb(ray, box, distance, distanceMax))
        return false;
    return distanceMax >= 0.0f;
}

inline Vec3 Barycentric(Vec3 point, Vec3 a, Vec3 b, Vec3 c)
{
    const Vec3 v0 = b - a;
    const Vec3 v1 = c - a;
    const Vec3 v2 = point - a;
    const float d00 = Dot(v0, v0);
    const float d01 = Dot(v0, v1);
    const float d11 = Dot(v1, v1);
    const float d20 = Dot(v2, v0);
    const float d21 = Dot(v2, v1);
    const float denom = d00 * d11 - d01 * d01;
    if (Abs(denom) <= Epsilon)
        return {};
    const float v = (d11 * d20 - d01 * d21) / denom;
    const float w = (d00 * d21 - d01 * d20) / denom;
    const float u = 1.0f - v - w;
    return {u, v, w};
}

inline bool ContainsTrianglePoint(Vec3 point, Vec3 a, Vec3 b, Vec3 c)
{
    const Vec3 bary = Barycentric(point, a, b, c);
    return bary.x >= -Epsilon && bary.y >= -Epsilon && bary.z >= -Epsilon;
}

inline bool IntersectRayTriangle(Ray ray, Vec3 a, Vec3 b, Vec3 c, RaycastHit& hit, bool cullBackfaces = false)
{
    const Vec3 edge1 = b - a;
    const Vec3 edge2 = c - a;
    const Vec3 pvec = Cross(ray.direction, edge2);
    const float det = Dot(edge1, pvec);

    if (cullBackfaces)
    {
        if (det <= Epsilon)
            return false;
    }
    else if (Abs(det) <= Epsilon)
    {
        return false;
    }

    const float invDet = 1.0f / det;
    const Vec3 tvec = ray.origin - a;
    const float u = Dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f)
        return false;

    const Vec3 qvec = Cross(tvec, edge1);
    const float v = Dot(ray.direction, qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f)
        return false;

    const float t = Dot(edge2, qvec) * invDet;
    if (t < 0.0f)
        return false;

    hit.hit = true;
    hit.distance = t;
    hit.point = GetPoint(ray, t);
    hit.normal = SafeNormalize(Cross(edge1, edge2));
    hit.barycentric = {1.0f - u - v, u, v};
    return true;
}

inline Vec3 ClosestPointOnSegment(Vec3 point, Vec3 a, Vec3 b)
{
    const Vec3 ab = b - a;
    const float denom = LengthSquared(ab);
    if (denom <= Epsilon)
        return a;
    const float t = Saturate(Dot(point - a, ab) / denom);
    return a + ab * t;
}

inline Vec3 ClosestPointOnTriangle(Vec3 point, Vec3 a, Vec3 b, Vec3 c)
{
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 ap = point - a;

    const float d1 = Dot(ab, ap);
    const float d2 = Dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f)
        return a;

    const Vec3 bp = point - b;
    const float d3 = Dot(ab, bp);
    const float d4 = Dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3)
        return b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
    {
        const float v = d1 / (d1 - d3);
        return a + ab * v;
    }

    const Vec3 cp = point - c;
    const float d5 = Dot(ab, cp);
    const float d6 = Dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6)
        return c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
    {
        const float w = d2 / (d2 - d6);
        return a + ac * w;
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
    {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + (c - b) * w;
    }

    const float denom = 1.0f / (va + vb + vc);
    const float v = vb * denom;
    const float w = vc * denom;
    return a + ab * v + ac * w;
}

inline Plane PlaneFromClipRow(const Mat4& matrix, float row0, float row1, float row2, float row3)
{
    Plane plane{
        {
            row0 * Mat4At(matrix, 0, 0) + row1 * Mat4At(matrix, 1, 0) + row2 * Mat4At(matrix, 2, 0) + row3 * Mat4At(matrix, 3, 0),
            row0 * Mat4At(matrix, 0, 1) + row1 * Mat4At(matrix, 1, 1) + row2 * Mat4At(matrix, 2, 1) + row3 * Mat4At(matrix, 3, 1),
            row0 * Mat4At(matrix, 0, 2) + row1 * Mat4At(matrix, 1, 2) + row2 * Mat4At(matrix, 2, 2) + row3 * Mat4At(matrix, 3, 2),
        },
        row0 * Mat4At(matrix, 0, 3) + row1 * Mat4At(matrix, 1, 3) + row2 * Mat4At(matrix, 2, 3) + row3 * Mat4At(matrix, 3, 3)};
    return NormalizePlane(plane);
}

inline Frustum ExtractFrustumVulkan(const Mat4& viewProjection)
{
    Frustum frustum{};
    frustum.left = PlaneFromClipRow(viewProjection, 1.0f, 0.0f, 0.0f, 1.0f);
    frustum.right = PlaneFromClipRow(viewProjection, -1.0f, 0.0f, 0.0f, 1.0f);
    frustum.bottom = PlaneFromClipRow(viewProjection, 0.0f, 1.0f, 0.0f, 1.0f);
    frustum.top = PlaneFromClipRow(viewProjection, 0.0f, -1.0f, 0.0f, 1.0f);
    frustum.nearPlane = PlaneFromClipRow(viewProjection, 0.0f, 0.0f, 1.0f, 0.0f);
    frustum.farPlane = PlaneFromClipRow(viewProjection, 0.0f, 0.0f, -1.0f, 1.0f);
    return frustum;
}

inline Containment Classify(Plane plane, Aabb box)
{
    const Vec3 positive{
        plane.normal.x >= 0.0f ? box.max.x : box.min.x,
        plane.normal.y >= 0.0f ? box.max.y : box.min.y,
        plane.normal.z >= 0.0f ? box.max.z : box.min.z};
    if (SignedDistance(plane, positive) < 0.0f)
        return Containment::Outside;

    const Vec3 negative{
        plane.normal.x >= 0.0f ? box.min.x : box.max.x,
        plane.normal.y >= 0.0f ? box.min.y : box.max.y,
        plane.normal.z >= 0.0f ? box.min.z : box.max.z};
    if (SignedDistance(plane, negative) >= 0.0f)
        return Containment::Inside;

    return Containment::Intersects;
}

inline Containment Classify(Plane plane, Sphere sphere)
{
    const float distance = SignedDistance(plane, sphere.center);
    if (distance < -sphere.radius)
        return Containment::Outside;
    if (distance > sphere.radius)
        return Containment::Inside;
    return Containment::Intersects;
}

inline bool Intersects(Frustum frustum, Aabb box)
{
    return Classify(frustum.left, box) != Containment::Outside &&
        Classify(frustum.right, box) != Containment::Outside &&
        Classify(frustum.bottom, box) != Containment::Outside &&
        Classify(frustum.top, box) != Containment::Outside &&
        Classify(frustum.nearPlane, box) != Containment::Outside &&
        Classify(frustum.farPlane, box) != Containment::Outside;
}

inline bool Intersects(Frustum frustum, Sphere sphere)
{
    return Classify(frustum.left, sphere) != Containment::Outside &&
        Classify(frustum.right, sphere) != Containment::Outside &&
        Classify(frustum.bottom, sphere) != Containment::Outside &&
        Classify(frustum.top, sphere) != Containment::Outside &&
        Classify(frustum.nearPlane, sphere) != Containment::Outside &&
        Classify(frustum.farPlane, sphere) != Containment::Outside;
}

inline bool Contains(Frustum frustum, Aabb box)
{
    return Classify(frustum.left, box) == Containment::Inside &&
        Classify(frustum.right, box) == Containment::Inside &&
        Classify(frustum.bottom, box) == Containment::Inside &&
        Classify(frustum.top, box) == Containment::Inside &&
        Classify(frustum.nearPlane, box) == Containment::Inside &&
        Classify(frustum.farPlane, box) == Containment::Inside;
}

inline bool Contains(Frustum frustum, Sphere sphere)
{
    return Classify(frustum.left, sphere) == Containment::Inside &&
        Classify(frustum.right, sphere) == Containment::Inside &&
        Classify(frustum.bottom, sphere) == Containment::Inside &&
        Classify(frustum.top, sphere) == Containment::Inside &&
        Classify(frustum.nearPlane, sphere) == Containment::Inside &&
        Classify(frustum.farPlane, sphere) == Containment::Inside;
}
}
