#pragma once

#include <cstddef>
#include <cstdint>

namespace ixtreeme::math
{
struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;

    constexpr float& operator[](std::size_t index) { return index == 0 ? x : y; }
    constexpr const float& operator[](std::size_t index) const { return index == 0 ? x : y; }
};

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr float& operator[](std::size_t index) { return index == 0 ? x : (index == 1 ? y : z); }
    constexpr const float& operator[](std::size_t index) const { return index == 0 ? x : (index == 1 ? y : z); }
};

struct Vec4
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    constexpr float& operator[](std::size_t index)
    {
        return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w));
    }

    constexpr const float& operator[](std::size_t index) const
    {
        return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w));
    }
};

struct IVec2
{
    int32_t x = 0;
    int32_t y = 0;
};

struct IVec3
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

struct IVec4
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int32_t w = 0;
};

struct UVec2
{
    uint32_t x = 0;
    uint32_t y = 0;
};

struct UVec3
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
};

struct UVec4
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
    uint32_t w = 0;
};

struct Mat3
{
    float m[9]{};
};

struct Mat4
{
    float m[16]{};
};

struct Quat
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct Transform
{
    Vec3 position{};
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

struct Plane
{
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float distance = 0.0f;
};

struct Ray
{
    Vec3 origin{};
    Vec3 direction{0.0f, 0.0f, 1.0f};
};

struct Aabb
{
    Vec3 min{};
    Vec3 max{};
};

struct Sphere
{
    Vec3 center{};
    float radius = 0.0f;
};

struct Frustum
{
    Plane left{};
    Plane right{};
    Plane bottom{};
    Plane top{};
    Plane nearPlane{};
    Plane farPlane{};
};

constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
constexpr Vec2 operator-(Vec2 v) { return {-v.x, -v.y}; }
constexpr Vec2 operator*(Vec2 v, float s) { return {v.x * s, v.y * s}; }
constexpr Vec2 operator*(float s, Vec2 v) { return v * s; }
constexpr Vec2 operator/(Vec2 v, float s) { return {v.x / s, v.y / s}; }

constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec3 operator-(Vec3 v) { return {-v.x, -v.y, -v.z}; }
constexpr Vec3 operator*(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
constexpr Vec3 operator*(float s, Vec3 v) { return v * s; }
constexpr Vec3 operator/(Vec3 v, float s) { return {v.x / s, v.y / s, v.z / s}; }

constexpr Vec4 operator+(Vec4 a, Vec4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
constexpr Vec4 operator-(Vec4 a, Vec4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
constexpr Vec4 operator-(Vec4 v) { return {-v.x, -v.y, -v.z, -v.w}; }
constexpr Vec4 operator*(Vec4 v, float s) { return {v.x * s, v.y * s, v.z * s, v.w * s}; }
constexpr Vec4 operator*(float s, Vec4 v) { return v * s; }
constexpr Vec4 operator/(Vec4 v, float s) { return {v.x / s, v.y / s, v.z / s, v.w / s}; }
}
