#include "ixtreemetree/tree.h"

#include "math/IXMath.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace ixtreemetree
{
namespace
{
namespace xm = ixtreeme::math;

xm::Vec3 ToMath(Vec3 v)
{
    return {v.x, v.y, v.z};
}

Vec3 ToTree(xm::Vec3 v)
{
    return {v.x, v.y, v.z};
}

Vec3 Add(Vec3 a, Vec3 b) { return ToTree(ToMath(a) + ToMath(b)); }
Vec3 Sub(Vec3 a, Vec3 b) { return ToTree(ToMath(a) - ToMath(b)); }
Vec3 Mul(Vec3 v, float s) { return ToTree(ToMath(v) * s); }
float Dot(Vec3 a, Vec3 b) { return xm::Dot(ToMath(a), ToMath(b)); }
Vec3 Cross(Vec3 a, Vec3 b) { return ToTree(xm::Cross(ToMath(a), ToMath(b))); }
float Length(Vec3 v) { return xm::Length(ToMath(v)); }
Vec3 Normalize(Vec3 v)
{
    return ToTree(xm::SafeNormalize(ToMath(v), {0.0f, 1.0f, 0.0f}));
}
Vec3 Mix(Vec3 a, Vec3 b, float t) { return ToTree(xm::Lerp(ToMath(a), ToMath(b), t)); }

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

// A last-level branch (twig), which the leaves grow along: its section centres and directions, base first.
struct TerminalBranch
{
    std::vector<Vec3> centers;
    std::vector<Vec3> axes;
};

// The point a fraction t (0 base .. 1 tip) of the way along a twig, and the twig's direction there.
Vec3 PointAlong(const TerminalBranch& branch, float t, Vec3& direction)
{
    const std::size_t last = branch.centers.size() - 1u;
    const float position = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(last);
    const std::size_t index = std::min(static_cast<std::size_t>(position), last > 0u ? last - 1u : 0u);
    const std::size_t next = std::min(index + 1u, last);
    const float alpha = position - static_cast<float>(index);
    direction = Normalize(Mix(branch.axes[index], branch.axes[next], alpha));
    return Mix(branch.centers[index], branch.centers[next], alpha);
}

void BuildFrame(Vec3 direction, Vec3& right, Vec3& up)
{
    const Vec3 axis = Normalize(direction);
    const Vec3 seed = xm::Abs(axis.y) > 0.95f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    right = Normalize(Cross(seed, axis));
    up = Normalize(Cross(axis, right));
}

Vec3 RotateAroundAxis(Vec3 v, Vec3 axis, float radians)
{
    axis = Normalize(axis);
    const float c = xm::Cos(radians);
    const float s = xm::Sin(radians);
    return Add(Add(Mul(v, c), Mul(Cross(axis, v), s)), Mul(axis, Dot(axis, v) * (1.0f - c)));
}

// How long a branch grows, as a share of its level's length, for where it starts in its parent's
// crown: ratio 1 at the crown's base, 0 at its top (Weber and Penn's shape ratios).
float ShapeRatio(CrownShape shape, float ratio)
{
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    switch (shape)
    {
    case CrownShape::Conical: return 0.2f + 0.8f * ratio;
    case CrownShape::Spherical: return 0.2f + 0.8f * xm::Sin(xm::Pi * ratio);
    case CrownShape::Hemispherical: return 0.2f + 0.8f * xm::Sin(0.5f * xm::Pi * ratio);
    case CrownShape::TaperedCylinder: return 0.5f + 0.5f * ratio;
    case CrownShape::Flame: return ratio <= 0.7f ? ratio / 0.7f : (1.0f - ratio) / 0.3f;
    case CrownShape::InverseConical: return 1.0f - 0.8f * ratio;
    case CrownShape::TendFlame: return ratio <= 0.7f ? 0.5f + 0.5f * ratio / 0.7f : 0.5f + 0.5f * (1.0f - ratio) / 0.3f;
    case CrownShape::Uniform: break;
    }
    return 1.0f;
}

// lengthScale shrinks the branch (its length and radius) and everything growing from it: the crown
// shape of its own and its ancestors' levels.
BranchEnd GenerateBranch(const TreeOptions& options,
                         Rng& rng,
                         TreeMesh& mesh,
                         int level,
                         Vec3 origin,
                         Vec3 direction,
                         std::vector<TerminalBranch>& terminalBranches,
                         float lengthScale = 1.0f)
{
    const int index = std::clamp(level, 0, kMaxBranchLevels - 1);
    const int sections = std::max(1, options.branch.sections[index]);
    const int radialSegments = std::max(3, options.branch.segments[index]);
    const float length = std::max(0.1f, options.branch.length[index] * lengthScale);
    const float baseRadius = std::max(0.01f, options.branch.radius[index] * lengthScale);
    const float taper = std::clamp(options.branch.taper[index], 0.05f, 1.0f);
    const float gnarliness = std::max(0.0f, options.branch.gnarliness[index]);
    const float twist = xm::DegreesToRadians(options.branch.twist[index]);
    const Vec3 forceDirection = Normalize(options.branch.forceDirection);
    const float forceStrength = options.branch.forceStrength;
    // Droop is a bend a metre grown, so a branch arches the same however finely it is cut.
    const float droopStep = options.branch.droop[index] * length / static_cast<float>(sections);

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
        Vec3 bent = Add(Add(currentDirection, Mul(randomBend, 0.12f)), Mul(forceDirection, forceStrength * t));
        if (droopStep != 0.0f)
            bent = Add(bent, Vec3{0.0f, -droopStep, 0.0f});
        currentDirection = Normalize(bent);
        cursor = Add(cursor, Mul(currentDirection, length / static_cast<float>(sections)));
        centers.push_back(cursor);
        axes.push_back(currentDirection);
    }

    const int maxLevel = std::clamp(options.branch.levels, 0, kMaxBranchLevels - 1);
    // A frond's branch is its midrib: the leaf shows it, no bark is drawn.
    const bool frond = level >= maxLevel && options.leaves.style == LeafStyle::Fronds;
    const std::uint32_t baseVertex = static_cast<std::uint32_t>(mesh.bark.vertices.size());
    for (int ring = 0; ring <= sections && !frond; ++ring)
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
            const float a = u * xm::TwoPi;
            const Vec3 normal = Normalize(Add(Mul(right, xm::Cos(a)), Mul(up, xm::Sin(a))));
            const Vec3 position = Add(centers[static_cast<std::size_t>(ring)], Mul(normal, radius));
            mesh.bark.vertices.push_back(Vertex{position, normal, Vec2{u * options.bark.textureScale.x, t * options.bark.textureScale.y}});
            IncludeBounds(mesh, position);
        }
    }

    const int stride = radialSegments + 1;
    for (int ring = 0; ring < sections && !frond; ++ring)
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
    if (level >= maxLevel)
    {
        terminalBranches.push_back(TerminalBranch{centers, axes});
        return end;
    }

    const int childLevel = level + 1;
    const int childCount = std::max(0, options.branch.children[childLevel]);
    const float start = std::clamp(options.branch.start[childLevel], 0.0f, 0.95f);
    const float angleRadians = xm::DegreesToRadians(options.branch.angle[childLevel]);
    const BranchArrangement arrangement = options.branch.arrangement[childLevel];
    const CrownShape shape = options.branch.shape[childLevel];
    const int whorlSize = options.branch.whorlSize[childLevel] > 0 ? options.branch.whorlSize[childLevel] : 4;
    const int whorls = std::max(1, (childCount + whorlSize - 1) / whorlSize);
    constexpr float kGoldenAngle = 2.39996323f;  // 137.5 degrees: a palm's fronds, a sunflower's seeds
    const float spiralStart = arrangement == BranchArrangement::Spiral ? rng.uniform(0.0f, xm::TwoPi) : 0.0f;
    float whorlBase = 0.0f;
    for (int child = 0; child < childCount; ++child)
    {
        float t = 0.0f;
        if (arrangement == BranchArrangement::Whorled)
        {
            // Rings of whorlSize, evenly up the parent, each turned at random, its branches evenly round.
            if (child % whorlSize == 0)
                whorlBase = rng.uniform(0.0f, xm::TwoPi);
            t = start + (1.0f - start) * (static_cast<float>(child / whorlSize) + 0.5f) / static_cast<float>(whorls);
        }
        else if (arrangement == BranchArrangement::Spiral)
        {
            t = start + (1.0f - start) * (static_cast<float>(child) + 0.5f) / static_cast<float>(childCount);
        }
        else
        {
            const float spanT = (static_cast<float>(child) + rng.uniform(0.15f, 0.85f)) / std::max(1.0f, static_cast<float>(childCount));
            t = start + (1.0f - start) * spanT;
        }
        t = std::clamp(t, 0.0f, 1.0f);
        const int centerIndex = std::clamp(static_cast<int>(t * static_cast<float>(sections)), 0, sections);
        Vec3 right{};
        Vec3 up{};
        BuildFrame(axes[static_cast<std::size_t>(centerIndex)], right, up);
        float around = 0.0f;
        if (arrangement == BranchArrangement::Whorled)
            around = whorlBase + xm::TwoPi * static_cast<float>(child % whorlSize) / static_cast<float>(whorlSize) +
                rng.uniform(-0.15f, 0.15f);
        else if (arrangement == BranchArrangement::Spiral)
            around = spiralStart + kGoldenAngle * static_cast<float>(child);
        else if (arrangement == BranchArrangement::Alternating)
            around = xm::Pi * static_cast<float>(child % 2);
        else
            around = rng.uniform(0.0f, xm::TwoPi);
        const Vec3 radial = Normalize(Add(Mul(right, xm::Cos(around)), Mul(up, xm::Sin(around))));
        const Vec3 childDir = Normalize(Add(Mul(axes[static_cast<std::size_t>(centerIndex)], xm::Cos(angleRadians)),
            Mul(radial, xm::Sin(angleRadians))));
        float childScale = lengthScale;
        if (shape != CrownShape::Uniform)
            childScale *= std::max(0.05f, ShapeRatio(shape, start < 1.0f ? 1.0f - (t - start) / (1.0f - start) : 0.0f));
        GenerateBranch(options, rng, mesh, childLevel, centers[static_cast<std::size_t>(centerIndex)], childDir,
            terminalBranches, childScale);
    }

    return end;
}

// The organic model samples a fixed-resolution growth path, independently of the
// render tessellation. Changing tube resolution must not move branch attachments.
void GenerateOrganicBranch(const TreeOptions& o, Rng& rng, TreeMesh& mesh, int level,
    Vec3 origin, Vec3 direction, std::vector<TerminalBranch>& terminals,
    int& branchBudget, float scale = 1.0f)
{
    if (--branchBudget < 0 || mesh.bark.vertices.size() > 1500000u)
    {
        mesh.stats.budgetLimited = true;
        return;
    }
    const auto& b = o.branch;
    const int lastLevel = std::clamp(b.levels, 0, kMaxBranchLevels - 1);
    const int sections = std::clamp(b.sections[level], 2, 64);
    const int sides = std::clamp(b.segments[level], 3, 32);
    const float length = std::max(0.02f, b.length[level] * scale *
        (1 + rng.uniform(-1, 1) * std::clamp(b.lengthVariance[level], 0.0f, 0.95f)));
    const float radius = std::max(0.0005f, b.radius[level] * std::sqrt(scale) *
        (1 + rng.uniform(-1, 1) * std::clamp(b.radiusVariance[level], 0.0f, 0.95f)));
    direction = Normalize(direction);
    Vec3 side{}, rise{};
    BuildFrame(direction, side, rise);
    // Smooth, low-frequency wandering: one set of coefficients per branch.
    const float noiseA = rng.uniform(-1, 1) * b.gnarliness[level];
    const float noiseB = rng.uniform(-1, 1) * b.gnarliness[level];
    const float phase = rng.uniform(0, xm::TwoPi);
    auto axisAt = [&](float t) {
        const float elevation = xm::DegreesToRadians(b.bendProfile[level].evaluate(t)) - b.droop[level] * length * t;
        Vec3 axis = RotateAroundAxis(direction, side, -elevation);
        axis = Add(axis, Add(Mul(side, noiseA * std::sin(t * xm::TwoPi + phase) * t),
                            Mul(rise, noiseB * std::sin(t * xm::Pi) * t)));
        return Normalize(Add(axis, Mul(Normalize(b.forceDirection), b.forceStrength * length * t)));
    };
    TerminalBranch path;
    constexpr int samples = 64;
    path.centers.push_back(origin);
    path.axes.push_back(axisAt(0));
    for (int i = 1; i <= samples; ++i)
    {
        const float t = static_cast<float>(i) / samples;
        path.centers.push_back(Add(path.centers.back(), Mul(axisAt(t - 0.5f / samples), length / samples)));
        path.axes.push_back(axisAt(t));
    }
    const bool frond = level == lastLevel && o.leaves.style == LeafStyle::Fronds;
    const auto base = static_cast<std::uint32_t>(mesh.bark.vertices.size());
    Vec3 previousRight = side;
    for (int ring = 0; ring <= sections && !frond; ++ring)
    {
        const float t = static_cast<float>(ring) / sections;
        Vec3 axis{};
        const Vec3 center = PointAlong(path, t, axis);
        // Parallel transport avoids the visible frame flips of world-up frames.
        Vec3 right = Normalize(Sub(previousRight, Mul(axis, Dot(previousRight, axis))));
        previousRight = right;
        right = RotateAroundAxis(right, axis, xm::DegreesToRadians(b.twist[level]) * t);
        const Vec3 up = Normalize(Cross(axis, right));
        const float r = radius * std::max(0.001f, b.radiusProfile[level].evaluate(t)) *
            (1 - (1 - std::clamp(b.taper[level], 0.001f, 1.0f)) * t);
        for (int j = 0; j <= sides; ++j)
        {
            const float u = static_cast<float>(j) / sides;
            const Vec3 normal = Add(Mul(right, std::cos(u * xm::TwoPi)), Mul(up, std::sin(u * xm::TwoPi)));
            const Vec3 p = Add(center, Mul(normal, r));
            mesh.bark.vertices.push_back({p, normal, {u * o.bark.textureScale.x, t * o.bark.textureScale.y}});
            IncludeBounds(mesh, p);
        }
    }
    for (int ring = 0; ring < sections && !frond; ++ring)
        for (int j = 0; j < sides; ++j)
        {
            const auto a = base + ring * (sides + 1) + j;
            const auto c = a + sides + 1;
            mesh.bark.indices.insert(mesh.bark.indices.end(), {a,c,a+1,a+1,c,c+1});
        }
    if (!frond)
    {
        // Close the small tip; no open tube is visible when the camera looks down a branch.
        const auto cap = static_cast<std::uint32_t>(mesh.bark.vertices.size());
        mesh.bark.vertices.push_back({path.centers.back(), path.axes.back(), {0.5f, 1}});
        const auto tip = base + sections * (sides + 1);
        for (int j = 0; j < sides; ++j)
            mesh.bark.indices.insert(mesh.bark.indices.end(), {tip+j, cap, tip+j+1});
    }
    if (level >= lastLevel)
    {
        if (frond)
        {
            TerminalBranch sampled;
            for (int ring=0; ring<=sections; ++ring)
            {
                Vec3 axis{};
                sampled.centers.push_back(PointAlong(path,float(ring)/sections,axis));
                sampled.axes.push_back(axis);
            }
            path=std::move(sampled);
        }
        terminals.push_back(std::move(path));
        return;
    }
    const int childLevel = level + 1;
    const int count = std::clamp(static_cast<int>(b.children[childLevel] * std::sqrt(scale)), 0, 128);
    if (count == 0)
    {
        // An adaptive hierarchy can stop before the last configured level.
        // Such a shoot is still a leaf-bearing terminal, including at the crown tip.
        terminals.push_back(std::move(path));
        return;
    }
    const float start = std::clamp(b.start[childLevel], 0.0f, 0.98f);
    const int whorlSize = std::clamp(b.whorlSize[childLevel] > 0 ? b.whorlSize[childLevel] : 4, 1, 16);
    const int whorls = std::max(1, (count + whorlSize - 1) / whorlSize);
    const float rotation = rng.uniform(0, xm::TwoPi);
    for (int child = 0; child < count && branchBudget > 0; ++child)
    {
        const auto arrangement = b.arrangement[childLevel];
        float q = (child + rng.uniform(0.18f, 0.82f)) / std::max(1, count);
        float around = rng.uniform(0, xm::TwoPi);
        if (arrangement == BranchArrangement::Whorled)
        {
            q = (child / whorlSize + 0.5f + rng.uniform(-0.12f, 0.12f)) / whorls;
            around = rotation + (child / whorlSize) * 2.39996323f + (child % whorlSize) * xm::TwoPi / whorlSize;
        }
        else if (arrangement == BranchArrangement::Spiral)
            around = rotation + child * 2.39996323f;
        else if (arrangement == BranchArrangement::Alternating)
            around = (child % 2) * xm::Pi;
        q = std::pow(std::clamp(q, 0.001f, 0.999f), std::clamp(b.distribution[childLevel], 0.2f, 5.0f));
        const float t = start + (0.985f - start) * q;
        Vec3 parentAxis{};
        const Vec3 attachment = PointAlong(path, t, parentAxis);
        Vec3 right{}, up{};
        BuildFrame(parentAxis, right, up);
        around += xm::DegreesToRadians(rng.uniform(-1, 1) * b.azimuthVariance[childLevel]);
        const Vec3 radial = Add(Mul(right, std::cos(around)), Mul(up, std::sin(around)));
        const float angle = xm::DegreesToRadians(std::clamp(b.angle[childLevel] * b.angleProfile[childLevel].evaluate(q) +
            rng.uniform(-1, 1) * b.angleVariance[childLevel], 0.0f, 175.0f));
        const Vec3 childDirection = Normalize(Add(Mul(parentAxis, std::cos(angle)), Mul(radial, std::sin(angle))));
        const float inheritedScale = std::pow(scale, std::clamp(b.scaleInheritance[childLevel],0.0f,1.0f));
        const float childScale = std::max(0.025f, inheritedScale * ShapeRatio(b.shape[childLevel], 1-q) *
            std::max(0.025f, b.lengthProfile[childLevel].evaluate(q)));
        GenerateOrganicBranch(o, rng, mesh, childLevel, attachment, childDirection, terminals, branchBudget, childScale);
    }
    if (branchBudget <= 0) mesh.stats.budgetLimited = true;
}

void AddLeafQuad(TreeMesh& mesh, Vec3 center, Vec3 normal, Vec3 tangent, float size, Vec2 uvMin, Vec2 uvMax, float aspect = 1.0f)
{
    normal = Normalize(normal);
    tangent = Normalize(tangent);
    Vec3 bitangent = Normalize(Cross(normal, tangent));
    if (Length(bitangent) < 0.001f)
        bitangent = {0.0f, 1.0f, 0.0f};
    const Vec3 x = Mul(tangent, size * 0.5f * aspect);
    const Vec3 y = Mul(bitangent, size * 0.5f);
    const std::uint32_t base = static_cast<std::uint32_t>(mesh.leaves.vertices.size());
    const Vec3 p0 = Sub(Sub(center, x), y);
    const Vec3 p1 = Add(Sub(center, y), x);
    const Vec3 p2 = Add(Add(center, x), y);
    const Vec3 p3 = Add(Sub(center, x), y);
    mesh.leaves.vertices.push_back(Vertex{p0, normal, Vec2{uvMin.x, uvMax.y}});
    mesh.leaves.vertices.push_back(Vertex{p1, normal, Vec2{uvMax.x, uvMax.y}});
    mesh.leaves.vertices.push_back(Vertex{p2, normal, Vec2{uvMax.x, uvMin.y}});
    mesh.leaves.vertices.push_back(Vertex{p3, normal, Vec2{uvMin.x, uvMin.y}});
    mesh.leaves.indices.insert(mesh.leaves.indices.end(), {base, base + 1u, base + 2u, base, base + 2u, base + 3u});
    IncludeBounds(mesh, p0);
    IncludeBounds(mesh, p1);
    IncludeBounds(mesh, p2);
    IncludeBounds(mesh, p3);
}

// A quad in AddLeafQuad's corner order: p0 (uMin, vMax), p1 (uMax, vMax), p2 (uMax, vMin), p3 (uMin, vMin),
// drawn as (0, 1, 2) and (0, 2, 3), so the export's leaf trim works on it too. Its normal faces up.
void AddLeafQuadCorners(TreeMesh& mesh, Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, Vec2 uvMin, Vec2 uvMax)
{
    Vec3 normal = Normalize(Cross(Sub(p1, p0), Sub(p3, p0)));
    if (normal.y < 0.0f)
        normal = Mul(normal, -1.0f);
    const std::uint32_t base = static_cast<std::uint32_t>(mesh.leaves.vertices.size());
    mesh.leaves.vertices.push_back(Vertex{p0, normal, Vec2{uvMin.x, uvMax.y}});
    mesh.leaves.vertices.push_back(Vertex{p1, normal, Vec2{uvMax.x, uvMax.y}});
    mesh.leaves.vertices.push_back(Vertex{p2, normal, Vec2{uvMax.x, uvMin.y}});
    mesh.leaves.vertices.push_back(Vertex{p3, normal, Vec2{uvMin.x, uvMin.y}});
    mesh.leaves.indices.insert(mesh.leaves.indices.end(), {base, base + 1u, base + 2u, base, base + 2u, base + 3u});
    for (const Vec3& p : {p0, p1, p2, p3})
        IncludeBounds(mesh, p);
}

// Each last-level branch becomes a frond: a ribbon along it, its two halves rising from the midrib by
// the fold angle, the atlas cell's bottom at its base and its top at its tip. Both halves keep one
// offset from the midrib the whole way, so every segment is a parallelogram (flat: the trim stays
// exact) and neighbours share their edges (no cracks).
void GenerateFronds(const TreeOptions& options, Rng& rng, TreeMesh& mesh, const std::vector<TerminalBranch>& branches)
{
    const int atlasGridX = std::clamp(options.leaves.atlasGridX, 1, 8);
    const int atlasGridY = std::clamp(options.leaves.atlasGridY, 1, 8);
    const float fold = xm::DegreesToRadians(std::clamp(options.leaves.frondFold, -80.0f, 80.0f));
    const float variance = std::clamp(options.leaves.sizeVariance, 0.0f, 1.0f);
    for (const TerminalBranch& branch : branches)
    {
        if (branch.centers.size() < 2)
            continue;
        const int cellX = rng.uniformInt(0, atlasGridX - 1);
        const int cellY = rng.uniformInt(0, atlasGridY - 1);
        const float u0 = static_cast<float>(cellX) / static_cast<float>(atlasGridX);
        const float u1 = static_cast<float>(cellX + 1) / static_cast<float>(atlasGridX);
        const float uMid = 0.5f * (u0 + u1);
        const float vTop = static_cast<float>(cellY) / static_cast<float>(atlasGridY);
        const float vBottom = static_cast<float>(cellY + 1) / static_cast<float>(atlasGridY);
        const float width = std::max(0.02f, options.leaves.frondWidth * (1.0f + rng.uniform(-variance, variance)));

        const Vec3 axis = Normalize(branch.axes.front());
        Vec3 side = Cross(axis, Vec3{0.0f, 1.0f, 0.0f});
        if (Length(side) < 0.05f)
        {
            Vec3 up{};
            BuildFrame(axis, side, up);
        }
        side = Normalize(side);
        const Vec3 rise = Normalize(Cross(side, axis));
        const float half = 0.5f * width;
        const Vec3 left = Add(Mul(side, -half * xm::Cos(fold)), Mul(rise, half * xm::Sin(fold)));
        const Vec3 right = Add(Mul(side, half * xm::Cos(fold)), Mul(rise, half * xm::Sin(fold)));

        const std::size_t rings = branch.centers.size();
        for (std::size_t i = 0; i + 1 < rings; ++i)
        {
            const Vec3 a = branch.centers[i];
            const Vec3 b = branch.centers[i + 1];
            const float vA = vBottom + (vTop - vBottom) * static_cast<float>(i) / static_cast<float>(rings - 1);
            const float vB = vBottom + (vTop - vBottom) * static_cast<float>(i + 1) / static_cast<float>(rings - 1);
            AddLeafQuadCorners(mesh, Add(a, left), a, b, Add(b, left), Vec2{u0, vB}, Vec2{uMid, vA});
            AddLeafQuadCorners(mesh, a, Add(a, right), Add(b, right), b, Vec2{uMid, vB}, Vec2{u1, vA});
        }
    }
}

void GenerateLeaves(const TreeOptions& options, Rng& rng, TreeMesh& mesh, const std::vector<TerminalBranch>& branches)
{
    const int cardsPerCluster = std::clamp(options.leaves.cardsPerCluster, 1, 7);
    const int atlasGridX = std::clamp(options.leaves.atlasGridX, 1, 8);
    const int atlasGridY = std::clamp(options.leaves.atlasGridY, 1, 8);
    const float start = std::clamp(options.leaves.start, 0.0f, 0.98f);
    const float leafTilt = xm::DegreesToRadians(options.leaves.angle);

    for (const TerminalBranch& branch : branches)
    {
        if (branch.centers.empty())
            continue;
        const int count = std::max(0, options.leaves.count);
        for (int i = 0; i < count; ++i)
        {
            // The clusters spread along the twig, from `start` of its length to its tip, each moved a
            // little (they all sat at the tip before, on each other).
            const float clusterT = (static_cast<float>(i) + 0.5f) / std::max(1.0f, static_cast<float>(count));
            const float spacing = (1.0f - start) / std::max(1.0f, static_cast<float>(count));
            float along = start + (1.0f - start) * clusterT + rng.uniform(-0.05f, 0.05f);
            along = std::clamp(along + rng.uniform(-0.5f, 0.5f) * spacing, start, 1.0f);
            Vec3 direction{};
            Vec3 center = PointAlong(branch, along, direction);
            Vec3 right{};
            Vec3 up{};
            BuildFrame(direction, right, up);
            const float variance = std::clamp(options.leaves.sizeVariance, 0.0f, 1.0f);
            const float size = std::max(0.02f, options.leaves.size * (1.0f + rng.uniform(-variance, variance)));
            center = Add(center, Add(Mul(right, rng.uniform(-0.05f, 0.05f) * size), Mul(up, rng.uniform(-0.05f, 0.05f) * size)));

            const Vec3 clusterAxis = Normalize(Mix(Vec3{0.0f, 1.0f, 0.0f}, direction, 0.25f));
            const float baseAngle = rng.uniform(0.0f, xm::TwoPi);
            const Vec3 baseTangent = Normalize(Add(Mul(right, xm::Cos(baseAngle)), Mul(up, xm::Sin(baseAngle))));
            // A card turned by half a turn lies in the plane of the first (a "Double" cluster was two
            // cards in one plane): an even count spreads over half a turn, crossing each other.
            const float cardTurn = cardsPerCluster % 2 == 0 ? xm::Pi : xm::TwoPi;
            for (int card = 0; card < cardsPerCluster; ++card)
            {
                const float cardAngle = (static_cast<float>(card) / static_cast<float>(cardsPerCluster)) * cardTurn;
                Vec3 tangent = RotateAroundAxis(baseTangent, clusterAxis, cardAngle);
                tangent = RotateAroundAxis(tangent, Normalize(Cross(clusterAxis, tangent)), leafTilt * 0.15f);
                Vec3 normal = Normalize(Cross(clusterAxis, tangent));
                if (Length(normal) < 0.001f)
                    normal = Normalize(Mix(direction, up, 0.65f));

                const int cellX = rng.uniformInt(0, atlasGridX - 1);
                const int cellY = rng.uniformInt(0, atlasGridY - 1);
                const Vec2 uvMin{
                    static_cast<float>(cellX) / static_cast<float>(atlasGridX),
                    static_cast<float>(cellY) / static_cast<float>(atlasGridY),
                };
                const Vec2 uvMax{
                    static_cast<float>(cellX + 1) / static_cast<float>(atlasGridX),
                    static_cast<float>(cellY + 1) / static_cast<float>(atlasGridY),
                };
                const Vec3 cardCenter = Add(center, Add(Mul(right, rng.uniform(-0.025f, 0.025f) * size), Mul(up, rng.uniform(-0.025f, 0.025f) * size)));
                AddLeafQuad(mesh, cardCenter, normal, tangent, size, uvMin, uvMax);
            }
        }
    }
}

void GenerateTwigLeaves(const TreeOptions& o, Rng& rng, TreeMesh& mesh, const std::vector<TerminalBranch>& branches)
{
    const auto& l = o.leaves;
    const int columns = std::clamp(l.atlasGridX, 1, 8), rows = std::clamp(l.atlasGridY, 1, 8);
    const int cards = std::clamp(l.cardsPerCluster, 1, 7);
    const float start = std::clamp(l.start, 0.0f, 0.98f);
    for (const auto& branch : branches)
    {
        float length = 0;
        for (std::size_t j = 1; j < branch.centers.size(); ++j)
            length += Length(Sub(branch.centers[j], branch.centers[j-1]));
        const int count = std::clamp(l.density > 0 ? static_cast<int>(std::ceil(length * (1-start) * l.density)) : l.count, 0, 256);
        for (int i = 0; i < count && mesh.leaves.vertices.size() < 400000u; ++i)
        {
            const float t = start + (1-start) * (i + rng.uniform(0.15f, 0.85f)) / count;
            Vec3 axis{};
            const Vec3 attachment = PointAlong(branch, t, axis);
            Vec3 right{}, up{};
            BuildFrame(axis, right, up);
            const float size = std::max(0.01f, std::min(l.size, length * 1.4f) * (1 + rng.uniform(-1,1) * std::clamp(l.sizeVariance,0.0f,0.95f)) *
                (1 + (std::clamp(l.tipScale,0.05f,2.0f)-1) * t));
            const float rotation = rng.uniform(0, xm::Pi);
            for (int card = 0; card < cards; ++card)
            {
                const Vec3 tangent = RotateAroundAxis(right, axis, rotation + xm::Pi * card / cards);
                const Vec3 leafAxis = RotateAroundAxis(axis, tangent, xm::DegreesToRadians(l.angle) * rng.uniform(-1,1));
                const Vec3 normal = Normalize(Cross(tangent, leafAxis));
                const int x = rng.uniformInt(0,columns-1), y = rng.uniformInt(0,rows-1);
                // Atlas bottom is the stalk: it emerges from the twig, instead of
                // floating around an unrelated world-up cluster axis.
                const Vec3 center = Add(attachment, Mul(leafAxis, size * 0.35f));
                AddLeafQuad(mesh, center, normal, tangent, size,
                    {float(x)/columns,float(y)/rows}, {float(x+1)/columns,float(y+1)/rows}, std::clamp(l.aspect,0.1f,3.0f));
            }
        }
        if (mesh.leaves.vertices.size() >= 400000u) mesh.stats.budgetLimited = true;
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
    std::vector<TerminalBranch> terminalBranches;
    int branchBudget = 12000;
    auto grow = [&](Vec3 origin, Vec3 direction, float scale = 1.0f) {
        if (options.branch.organic)
            GenerateOrganicBranch(options, rng, mesh, 0, origin, direction, terminalBranches, branchBudget, scale);
        else
            GenerateBranch(options, rng, mesh, 0, origin, direction, terminalBranches, scale);
    };
    const int trunks = std::clamp(options.branch.trunks, 1, 16);
    const float spread = xm::DegreesToRadians(std::clamp(options.branch.trunkSpread, 0.0f, 89.0f));
    if (trunks == 1 && spread == 0.0f)
    {
        grow(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
    }
    else
    {
        // Stems evenly round, each leaning out by about the spread; several are a little shorter and
        // stand a little apart, so they do not grow through each other at the base.
        const float firstAzimuth = rng.uniform(0.0f, xm::TwoPi);
        for (int trunk = 0; trunk < trunks; ++trunk)
        {
            const float azimuth = firstAzimuth + xm::TwoPi * static_cast<float>(trunk) / static_cast<float>(trunks) +
                (trunks > 1 ? rng.uniform(-0.3f, 0.3f) : 0.0f);
            const float lean = spread * (trunks > 1 ? rng.uniform(0.6f, 1.0f) : 1.0f);
            const Vec3 outward{xm::Cos(azimuth), 0.0f, xm::Sin(azimuth)};
            const Vec3 direction{outward.x * xm::Sin(lean), xm::Cos(lean), outward.z * xm::Sin(lean)};
            const Vec3 origin = trunks > 1 ? Mul(outward, std::max(0.01f, options.branch.radius[0]) * 0.8f) : Vec3{};
            const float scale = trunks > 1 ? rng.uniform(0.75f, 1.0f) : 1.0f;
            grow(origin, direction, scale);
        }
    }
    if (options.leaves.style == LeafStyle::Fronds)
        GenerateFronds(options, rng, mesh, terminalBranches);
    else if (options.leaves.twigAligned)
        GenerateTwigLeaves(options, rng, mesh, terminalBranches);
    else
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
