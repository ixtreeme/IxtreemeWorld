#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

class SpatialIndex
{
public:
    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct Aabb
    {
        Vec3 min;
        Vec3 max;
    };

    struct Frustum
    {
        float viewProjection[16]{};
    };

    struct Config
    {
        Aabb worldBounds = {{-2048.0f, -512.0f, -2048.0f}, {2048.0f, 512.0f, 2048.0f}};
        std::uint32_t maxDepth = 8;
        std::uint32_t nodeCapacity = 16;
        float looseFactor = 1.5f;
    };

    struct QueryStats
    {
        std::uint32_t nodesVisited = 0;
        std::uint32_t candidates = 0;
        std::uint32_t totalObjects = 0;
    };

    struct MutationStats
    {
        std::uint32_t inserts = 0;
        std::uint32_t removes = 0;
        std::uint32_t updates = 0;
    };

    explicit SpatialIndex(Config config = {});

    void Clear();
    void Insert(std::uint32_t entity, const Aabb& bounds);
    void Remove(std::uint32_t entity);
    void Update(std::uint32_t entity, const Aabb& bounds);

    std::vector<std::uint32_t> QueryFrustum(const Frustum& frustum, QueryStats* stats = nullptr) const;
    std::vector<std::uint32_t> QueryAabb(const Aabb& bounds, QueryStats* stats = nullptr) const;
    std::vector<std::uint32_t> QueryRay(Vec3 origin, Vec3 direction, float maxDistance, QueryStats* stats = nullptr) const;

    std::uint32_t ObjectCount() const { return static_cast<std::uint32_t>(m_objects.size()); }
    std::uint32_t NodeCount() const { return CountNodes(0); }
    std::uint32_t MaxDepth() const { return m_config.maxDepth; }
    const Aabb& WorldBounds() const { return m_nodes[0].tightBounds; }
    MutationStats ConsumeMutationStats();

private:
    struct ObjectRecord
    {
        std::uint32_t entity = 0;
        Aabb bounds{};
        std::uint32_t node = 0;
    };

    struct Node
    {
        Aabb tightBounds{};
        Aabb looseBounds{};
        std::array<std::uint32_t, 8> children{};
        std::vector<std::uint32_t> objects;
        std::uint32_t depth = 0;
    };

    Config m_config;
    std::vector<Node> m_nodes;
    std::unordered_map<std::uint32_t, ObjectRecord> m_objects;
    MutationStats m_mutations;

    void ResetRoot(const Aabb& bounds);
    void EnsureRootContains(const Aabb& bounds);
    std::uint32_t InsertIntoNode(std::uint32_t nodeIndex, std::uint32_t entity, const Aabb& bounds);
    std::uint32_t GetOrCreateChild(std::uint32_t nodeIndex, std::uint32_t child);
    int ContainingChild(const Node& node, const Aabb& bounds) const;
    Aabb ChildBounds(const Aabb& parent, std::uint32_t child) const;
    Aabb MakeLoose(const Aabb& tight) const;
    bool Contains(const Aabb& outer, const Aabb& inner) const;
    bool Intersects(const Aabb& a, const Aabb& b) const;
    bool OutsideFrustum(const Frustum& frustum, const Aabb& bounds) const;
    std::uint32_t CountNodes(std::uint32_t nodeIndex) const;
};
