#include "ixtreemetree/tree.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace ixtreemetree
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;

Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Mul(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}
float Length(Vec3 v) { return std::sqrt(std::max(0.0f, Dot(v, v))); }
Vec3 Normalize(Vec3 v)
{
    const float len = Length(v);
    return len > 0.00001f ? Mul(v, 1.0f / len) : Vec3{0.0f, 1.0f, 0.0f};
}
Vec3 Mix(Vec3 a, Vec3 b, float t) { return Add(Mul(a, 1.0f - t), Mul(b, t)); }

class Rng
{
public:
    explicit Rng(std::uint64_t seed)
    {
        std::uint64_t state = seed;
        for (std::uint64_t& value : s_)
            value = splitmix64(state);
    }

    std::uint64_t next()
    {
        const std::uint64_t result = rotl(s_[1] * 5u, 7) * 9u;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    float uniform(float minValue, float maxValue)
    {
        constexpr double inv = 1.0 / static_cast<double>(UINT64_MAX);
        const float t = static_cast<float>(static_cast<double>(next()) * inv);
        return minValue + (maxValue - minValue) * t;
    }

    int uniformInt(int minValue, int maxValue)
    {
        if (maxValue <= minValue)
            return minValue;
        return minValue + static_cast<int>(next() % static_cast<std::uint64_t>(maxValue - minValue + 1));
    }

private:
    static std::uint64_t rotl(std::uint64_t x, int k)
    {
        return (x << k) | (x >> (64 - k));
    }

    static std::uint64_t splitmix64(std::uint64_t& x)
    {
        std::uint64_t z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    std::uint64_t s_[4]{};
};

void IncludeBounds(TreeMesh& mesh, Vec3 p)
{
    mesh.bboxMin.x = std::min(mesh.bboxMin.x, p.x);
    mesh.bboxMin.y = std::min(mesh.bboxMin.y, p.y);
    mesh.bboxMin.z = std::min(mesh.bboxMin.z, p.z);
    mesh.bboxMax.x = std::max(mesh.bboxMax.x, p.x);
    mesh.bboxMax.y = std::max(mesh.bboxMax.y, p.y);
    mesh.bboxMax.z = std::max(mesh.bboxMax.z, p.z);
}

struct BranchEnd
{
    Vec3 position{};
    Vec3 direction{0.0f, 1.0f, 0.0f};
};

void BuildFrame(Vec3 direction, Vec3& right, Vec3& up)
{
    const Vec3 axis = Normalize(direction);
    const Vec3 seed = std::fabs(axis.y) > 0.95f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    right = Normalize(Cross(seed, axis));
    up = Normalize(Cross(axis, right));
}

Vec3 RotateAroundAxis(Vec3 v, Vec3 axis, float radians)
{
    axis = Normalize(axis);
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return Add(Add(Mul(v, c), Mul(Cross(axis, v), s)), Mul(axis, Dot(axis, v) * (1.0f - c)));
}

BranchEnd GenerateBranch(const TreeOptions& options,
                         Rng& rng,
                         TreeMesh& mesh,
                         int level,
                         Vec3 origin,
                         Vec3 direction,
                         std::vector<BranchEnd>& terminalBranches)
{
    const int index = std::clamp(level, 0, kMaxBranchLevels - 1);
    const int sections = std::max(1, options.branch.sections[index]);
    const int radialSegments = std::max(3, options.branch.segments[index]);
    const float length = std::max(0.1f, options.branch.length[index]);
    const float baseRadius = std::max(0.01f, options.branch.radius[index]);
    const float taper = std::clamp(options.branch.taper[index], 0.05f, 1.0f);
    const float gnarliness = std::max(0.0f, options.branch.gnarliness[index]);
    const float twist = options.branch.twist[index] * kPi / 180.0f;
    const Vec3 forceDirection = Normalize(options.branch.forceDirection);
    const float forceStrength = options.branch.forceStrength;

    direction = Normalize(direction);
    std::vector<Vec3> centers;
    std::vector<Vec3> axes;
    centers.reserve(static_cast<std::size_t>(sections + 1));
    axes.reserve(static_cast<std::size_t>(sections + 1));
    centers.push_back(origin);
    axes.push_back(direction);

    Vec3 cursor = origin;
    Vec3 currentDirection = direction;
    for (int i = 1; i <= sections; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(sections);
        const Vec3 randomBend{
            rng.uniform(-gnarliness, gnarliness),
            rng.uniform(-gnarliness, gnarliness),
            rng.uniform(-gnarliness, gnarliness),
        };
        currentDirection = Normalize(Add(Add(currentDirection, Mul(randomBend, 0.12f)), Mul(forceDirection, forceStrength * t)));
        cursor = Add(cursor, Mul(currentDirection, length / static_cast<float>(sections)));
        centers.push_back(cursor);
        axes.push_back(currentDirection);
    }

    const std::uint32_t baseVertex = static_cast<std::uint32_t>(mesh.bark.vertices.size());
    for (int ring = 0; ring <= sections; ++ring)
    {
        const float t = static_cast<float>(ring) / static_cast<float>(sections);
        const float radius = baseRadius * (1.0f - (1.0f - taper) * t);
        Vec3 right{};
        Vec3 up{};
        BuildFrame(axes[static_cast<std::size_t>(ring)], right, up);
        right = RotateAroundAxis(right, axes[static_cast<std::size_t>(ring)], twist * static_cast<float>(ring));
        up = Normalize(Cross(axes[static_cast<std::size_t>(ring)], right));

        for (int seg = 0; seg <= radialSegments; ++seg)
        {
            const float u = static_cast<float>(seg) / static_cast<float>(radialSegments);
            const float a = u * 2.0f * kPi;
            const Vec3 normal = Normalize(Add(Mul(right, std::cos(a)), Mul(up, std::sin(a))));
            const Vec3 position = Add(centers[static_cast<std::size_t>(ring)], Mul(normal, radius));
            mesh.bark.vertices.push_back(Vertex{position, normal, Vec2{u * options.bark.textureScale.x, t * options.bark.textureScale.y}});
            IncludeBounds(mesh, position);
        }
    }

    const int stride = radialSegments + 1;
    for (int ring = 0; ring < sections; ++ring)
    {
        for (int seg = 0; seg < radialSegments; ++seg)
        {
            const std::uint32_t a = baseVertex + static_cast<std::uint32_t>(ring * stride + seg);
            const std::uint32_t b = a + 1u;
            const std::uint32_t c = a + static_cast<std::uint32_t>(stride);
            const std::uint32_t d = c + 1u;
            mesh.bark.indices.insert(mesh.bark.indices.end(), {a, c, b, b, c, d});
        }
    }

    const BranchEnd end{centers.back(), axes.back()};
    const int maxLevel = std::clamp(options.branch.levels, 0, kMaxBranchLevels - 1);
    if (level >= maxLevel)
    {
        terminalBranches.push_back(end);
        return end;
    }

    const int childLevel = level + 1;
    const int childCount = std::max(0, options.branch.children[childLevel]);
    const float start = std::clamp(options.branch.start[childLevel], 0.0f, 0.95f);
    const float angleRadians = options.branch.angle[childLevel] * kPi / 180.0f;
    for (int child = 0; child < childCount; ++child)
    {
        const float spanT = (static_cast<float>(child) + rng.uniform(0.15f, 0.85f)) / std::max(1.0f, static_cast<float>(childCount));
        const float t = std::clamp(start + (1.0f - start) * spanT, 0.0f, 1.0f);
        const int centerIndex = std::clamp(static_cast<int>(t * static_cast<float>(sections)), 0, sections);
        Vec3 right{};
        Vec3 up{};
        BuildFrame(axes[static_cast<std::size_t>(centerIndex)], right, up);
        const float around = rng.uniform(0.0f, 2.0f * kPi);
        const Vec3 radial = Normalize(Add(Mul(right, std::cos(around)), Mul(up, std::sin(around))));
        const Vec3 childDir = Normalize(Add(Mul(axes[static_cast<std::size_t>(centerIndex)], std::cos(angleRadians)),
            Mul(radial, std::sin(angleRadians))));
        GenerateBranch(options, rng, mesh, childLevel, centers[static_cast<std::size_t>(centerIndex)], childDir, terminalBranches);
    }

    return end;
}

void AddLeafQuad(TreeMesh& mesh, Vec3 center, Vec3 normal, Vec3 tangent, float size)
{
    normal = Normalize(normal);
    tangent = Normalize(tangent);
    Vec3 bitangent = Normalize(Cross(normal, tangent));
    if (Length(bitangent) < 0.001f)
        bitangent = {0.0f, 1.0f, 0.0f};
    const Vec3 x = Mul(tangent, size * 0.5f);
    const Vec3 y = Mul(bitangent, size * 0.5f);
    const std::uint32_t base = static_cast<std::uint32_t>(mesh.leaves.vertices.size());
    const Vec3 p0 = Sub(Sub(center, x), y);
    const Vec3 p1 = Add(Sub(center, y), x);
    const Vec3 p2 = Add(Add(center, x), y);
    const Vec3 p3 = Add(Sub(center, x), y);
    mesh.leaves.vertices.push_back(Vertex{p0, normal, Vec2{0.0f, 1.0f}});
    mesh.leaves.vertices.push_back(Vertex{p1, normal, Vec2{1.0f, 1.0f}});
    mesh.leaves.vertices.push_back(Vertex{p2, normal, Vec2{1.0f, 0.0f}});
    mesh.leaves.vertices.push_back(Vertex{p3, normal, Vec2{0.0f, 0.0f}});
    mesh.leaves.indices.insert(mesh.leaves.indices.end(), {base, base + 1u, base + 2u, base, base + 2u, base + 3u});
    IncludeBounds(mesh, p0);
    IncludeBounds(mesh, p1);
    IncludeBounds(mesh, p2);
    IncludeBounds(mesh, p3);
}

void GenerateLeaves(const TreeOptions& options, Rng& rng, TreeMesh& mesh, const std::vector<BranchEnd>& branches)
{
    for (const BranchEnd& branch : branches)
    {
        const int count = std::max(0, options.leaves.count);
        for (int i = 0; i < count; ++i)
        {
            const float along = options.leaves.start + (1.0f - options.leaves.start) *
                ((static_cast<float>(i) + rng.uniform(0.0f, 1.0f)) / std::max(1.0f, static_cast<float>(count)));
            const Vec3 center = Add(branch.position, Mul(branch.direction, rng.uniform(0.0f, 0.35f) * along));
            Vec3 right{};
            Vec3 up{};
            BuildFrame(branch.direction, right, up);
            const float angle = rng.uniform(0.0f, 2.0f * kPi);
            const Vec3 tangent = Normalize(Add(Mul(right, std::cos(angle)), Mul(up, std::sin(angle))));
            const Vec3 normal = Normalize(Mix(branch.direction, up, 0.65f));
            const float variance = std::clamp(options.leaves.sizeVariance, 0.0f, 1.0f);
            const float size = std::max(0.02f, options.leaves.size * (1.0f + rng.uniform(-variance, variance)));
            AddLeafQuad(mesh, center, normal, tangent, size);
            if (options.leaves.billboard == LeafBillboard::Double)
                AddLeafQuad(mesh, center, normal, RotateAroundAxis(tangent, normal, kPi * 0.5f), size);
        }
    }
}
}

TreeOptions defaultTreeOptions()
{
    TreeOptions options{};
    options.branch.levels = 3;
    options.branch.angle = {0.0f, 42.0f, 36.0f, 28.0f};
    options.branch.children = {1, 7, 4, 2};
    options.branch.gnarliness = {0.12f, 0.20f, 0.24f, 0.18f};
    options.branch.length = {7.5f, 3.6f, 1.7f, 0.9f};
    options.branch.radius = {0.36f, 0.16f, 0.07f, 0.035f};
    options.branch.sections = {10, 6, 4, 3};
    options.branch.segments = {10, 8, 7, 6};
    options.branch.start = {0.0f, 0.32f, 0.24f, 0.18f};
    options.branch.taper = {0.23f, 0.20f, 0.15f, 0.10f};
    options.branch.twist = {8.0f, 20.0f, 25.0f, 30.0f};
    options.leaves.count = 6;
    options.leaves.size = 0.55f;
    options.leaves.sizeVariance = 0.35f;
    return options;
}

TreeMesh Tree::generate() const
{
    const auto start = std::chrono::high_resolution_clock::now();
    TreeMesh mesh{};
    mesh.bboxMin = {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    mesh.bboxMax = {
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
    };

    Rng rng(options.seed);
    std::vector<BranchEnd> terminalBranches;
    GenerateBranch(options, rng, mesh, 0, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, terminalBranches);
    GenerateLeaves(options, rng, mesh, terminalBranches);

    if (mesh.bark.vertices.empty() && mesh.leaves.vertices.empty())
    {
        mesh.bboxMin = {};
        mesh.bboxMax = {};
    }

    mesh.stats.barkTriangles = static_cast<int>(mesh.bark.indices.size() / 3u);
    mesh.stats.leafTriangles = static_cast<int>(mesh.leaves.indices.size() / 3u);
    const auto end = std::chrono::high_resolution_clock::now();
    mesh.stats.generationMs = std::chrono::duration<float, std::milli>(end - start).count();
    return mesh;
}
}
