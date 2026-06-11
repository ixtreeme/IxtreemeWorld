#include "SpatialIndex.h"

#include <algorithm>
#include <cmath>

namespace
{
float Center(float a, float b)
{
    return (a + b) * 0.5f;
}

float Extent(float a, float b)
{
    return std::max(0.001f, (b - a) * 0.5f);
}
}

SpatialIndex::SpatialIndex(Config config)
    : m_config(config)
{
    m_config.maxDepth = std::max(1u, m_config.maxDepth);
    m_config.nodeCapacity = std::max(1u, m_config.nodeCapacity);
    m_config.looseFactor = std::max(1.0f, m_config.looseFactor);
    ResetRoot(m_config.worldBounds);
}

void SpatialIndex::Clear()
{
    m_objects.clear();
    ResetRoot(m_config.worldBounds);
}

void SpatialIndex::ResetRoot(const Aabb& bounds)
{
    m_nodes.clear();
    Node root{};
    root.tightBounds = bounds;
    root.looseBounds = MakeLoose(bounds);
    root.children.fill(std::numeric_limits<std::uint32_t>::max());
    root.depth = 0;
    m_nodes.push_back(std::move(root));
}

SpatialIndex::Aabb SpatialIndex::MakeLoose(const Aabb& tight) const
{
    const Vec3 center{
        Center(tight.min.x, tight.max.x),
        Center(tight.min.y, tight.max.y),
        Center(tight.min.z, tight.max.z)};
    const Vec3 extent{
        Extent(tight.min.x, tight.max.x) * m_config.looseFactor,
        Extent(tight.min.y, tight.max.y) * m_config.looseFactor,
        Extent(tight.min.z, tight.max.z) * m_config.looseFactor};
    return {{center.x - extent.x, center.y - extent.y, center.z - extent.z},
        {center.x + extent.x, center.y + extent.y, center.z + extent.z}};
}

bool SpatialIndex::Contains(const Aabb& outer, const Aabb& inner) const
{
    return inner.min.x >= outer.min.x && inner.max.x <= outer.max.x &&
        inner.min.y >= outer.min.y && inner.max.y <= outer.max.y &&
        inner.min.z >= outer.min.z && inner.max.z <= outer.max.z;
}

bool SpatialIndex::Intersects(const Aabb& a, const Aabb& b) const
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x &&
        a.min.y <= b.max.y && a.max.y >= b.min.y &&
        a.min.z <= b.max.z && a.max.z >= b.min.z;
}

void SpatialIndex::EnsureRootContains(const Aabb& bounds)
{
    if (Contains(m_nodes[0].looseBounds, bounds))
        return;

    Aabb next = m_nodes[0].tightBounds;
    while (!Contains(MakeLoose(next), bounds))
    {
        const Vec3 center{
            Center(next.min.x, next.max.x),
            Center(next.min.y, next.max.y),
            Center(next.min.z, next.max.z)};
        const Vec3 extent{
            Extent(next.min.x, next.max.x) * 2.0f,
            Extent(next.min.y, next.max.y) * 2.0f,
            Extent(next.min.z, next.max.z) * 2.0f};
        next = {{center.x - extent.x, center.y - extent.y, center.z - extent.z},
            {center.x + extent.x, center.y + extent.y, center.z + extent.z}};
    }

    std::vector<ObjectRecord> records;
    records.reserve(m_objects.size());
    for (const auto& [entity, record] : m_objects)
        records.push_back(record);
    ResetRoot(next);
    m_objects.clear();
    for (const ObjectRecord& record : records)
        Insert(record.entity, record.bounds);
}

void SpatialIndex::Insert(std::uint32_t entity, const Aabb& bounds)
{
    Remove(entity);
    EnsureRootContains(bounds);
    ObjectRecord record{};
    record.entity = entity;
    record.bounds = bounds;
    record.node = InsertIntoNode(0, entity, bounds);
    m_objects[entity] = record;
    ++m_mutations.inserts;
}

void SpatialIndex::Remove(std::uint32_t entity)
{
    auto it = m_objects.find(entity);
    if (it == m_objects.end())
        return;
    Node& node = m_nodes[it->second.node];
    node.objects.erase(std::remove(node.objects.begin(), node.objects.end(), entity), node.objects.end());
    m_objects.erase(it);
    ++m_mutations.removes;
}

void SpatialIndex::Update(std::uint32_t entity, const Aabb& bounds)
{
    auto it = m_objects.find(entity);
    if (it == m_objects.end())
    {
        Insert(entity, bounds);
        return;
    }
    Node& oldNode = m_nodes[it->second.node];
    oldNode.objects.erase(std::remove(oldNode.objects.begin(), oldNode.objects.end(), entity), oldNode.objects.end());
    m_objects.erase(it);
    EnsureRootContains(bounds);
    ObjectRecord record{};
    record.entity = entity;
    record.bounds = bounds;
    record.node = InsertIntoNode(0, entity, bounds);
    m_objects[entity] = record;
    ++m_mutations.updates;
}

std::uint32_t SpatialIndex::InsertIntoNode(std::uint32_t nodeIndex, std::uint32_t entity, const Aabb& bounds)
{
    Node& node = m_nodes[nodeIndex];
    const int child = ContainingChild(node, bounds);
    if (child >= 0 && node.depth < m_config.maxDepth)
        return InsertIntoNode(GetOrCreateChild(nodeIndex, static_cast<std::uint32_t>(child)), entity, bounds);

    node.objects.push_back(entity);
    return nodeIndex;
}

std::uint32_t SpatialIndex::GetOrCreateChild(std::uint32_t nodeIndex, std::uint32_t child)
{
    Node& node = m_nodes[nodeIndex];
    if (node.children[child] != std::numeric_limits<std::uint32_t>::max())
        return node.children[child];

    Node next{};
    next.tightBounds = ChildBounds(node.tightBounds, child);
    next.looseBounds = MakeLoose(next.tightBounds);
    next.children.fill(std::numeric_limits<std::uint32_t>::max());
    next.depth = node.depth + 1u;
    const std::uint32_t created = static_cast<std::uint32_t>(m_nodes.size());
    m_nodes.push_back(std::move(next));
    m_nodes[nodeIndex].children[child] = created;
    return created;
}

int SpatialIndex::ContainingChild(const Node& node, const Aabb& bounds) const
{
    for (std::uint32_t child = 0; child < 8; ++child)
    {
        const Aabb loose = MakeLoose(ChildBounds(node.tightBounds, child));
        if (Contains(loose, bounds))
            return static_cast<int>(child);
    }
    return -1;
}

SpatialIndex::Aabb SpatialIndex::ChildBounds(const Aabb& parent, std::uint32_t child) const
{
    const Vec3 center{
        Center(parent.min.x, parent.max.x),
        Center(parent.min.y, parent.max.y),
        Center(parent.min.z, parent.max.z)};
    Aabb out = parent;
    if (child & 1u)
        out.min.x = center.x;
    else
        out.max.x = center.x;
    if (child & 2u)
        out.min.y = center.y;
    else
        out.max.y = center.y;
    if (child & 4u)
        out.min.z = center.z;
    else
        out.max.z = center.z;
    return out;
}

bool SpatialIndex::OutsideFrustum(const Frustum& frustum, const Aabb& bounds) const
{
    bool outsideLeft = true;
    bool outsideRight = true;
    bool outsideBottom = true;
    bool outsideTop = true;
    bool outsideNear = true;
    bool outsideFar = true;
    const float* m = frustum.viewProjection;
    for (int z = 0; z < 2; ++z)
    {
        for (int y = 0; y < 2; ++y)
        {
            for (int x = 0; x < 2; ++x)
            {
                const Vec3 p{
                    x == 0 ? bounds.min.x : bounds.max.x,
                    y == 0 ? bounds.min.y : bounds.max.y,
                    z == 0 ? bounds.min.z : bounds.max.z};
                const float clipX = p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12];
                const float clipY = p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13];
                const float clipZ = p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14];
                const float clipW = p.x * m[3] + p.y * m[7] + p.z * m[11] + m[15];
                outsideLeft = outsideLeft && (clipX < -clipW);
                outsideRight = outsideRight && (clipX > clipW);
                outsideBottom = outsideBottom && (clipY < -clipW);
                outsideTop = outsideTop && (clipY > clipW);
                outsideNear = outsideNear && (clipZ < 0.0f);
                outsideFar = outsideFar && (clipZ > clipW);
            }
        }
    }
    return outsideLeft || outsideRight || outsideBottom || outsideTop || outsideNear || outsideFar;
}

std::vector<std::uint32_t> SpatialIndex::QueryFrustum(const Frustum& frustum, QueryStats* stats) const
{
    QueryStats local{};
    local.totalObjects = ObjectCount();
    std::vector<std::uint32_t> results;
    if (m_nodes.empty())
    {
        if (stats)
            *stats = local;
        return results;
    }

    std::vector<std::uint32_t> stack;
    stack.push_back(0);
    while (!stack.empty())
    {
        const std::uint32_t nodeIndex = stack.back();
        stack.pop_back();
        const Node& node = m_nodes[nodeIndex];
        ++local.nodesVisited;
        if (OutsideFrustum(frustum, node.looseBounds))
            continue;

        for (std::uint32_t entity : node.objects)
            results.push_back(entity);
        for (std::uint32_t child : node.children)
        {
            if (child != std::numeric_limits<std::uint32_t>::max())
                stack.push_back(child);
        }
    }
    local.candidates = static_cast<std::uint32_t>(results.size());
    if (stats)
        *stats = local;
    return results;
}

std::vector<std::uint32_t> SpatialIndex::QueryAabb(const Aabb& bounds, QueryStats* stats) const
{
    QueryStats local{};
    local.totalObjects = ObjectCount();
    std::vector<std::uint32_t> results;
    std::vector<std::uint32_t> stack;
    stack.push_back(0);
    while (!stack.empty())
    {
        const std::uint32_t nodeIndex = stack.back();
        stack.pop_back();
        const Node& node = m_nodes[nodeIndex];
        ++local.nodesVisited;
        if (!Intersects(node.looseBounds, bounds))
            continue;
        for (std::uint32_t entity : node.objects)
            results.push_back(entity);
        for (std::uint32_t child : node.children)
        {
            if (child != std::numeric_limits<std::uint32_t>::max())
                stack.push_back(child);
        }
    }
    local.candidates = static_cast<std::uint32_t>(results.size());
    if (stats)
        *stats = local;
    return results;
}

std::vector<std::uint32_t> SpatialIndex::QueryRay(Vec3, Vec3, float, QueryStats* stats) const
{
    if (stats)
        *stats = {0, 0, ObjectCount()};
    return {};
}

std::uint32_t SpatialIndex::CountNodes(std::uint32_t nodeIndex) const
{
    if (nodeIndex >= m_nodes.size())
        return 0;
    std::uint32_t count = 1;
    for (std::uint32_t child : m_nodes[nodeIndex].children)
    {
        if (child != std::numeric_limits<std::uint32_t>::max())
            count += CountNodes(child);
    }
    return count;
}

SpatialIndex::MutationStats SpatialIndex::ConsumeMutationStats()
{
    MutationStats out = m_mutations;
    m_mutations = {};
    return out;
}
