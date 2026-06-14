#pragma once

#include "tree_options.h"

#include <cstdint>
#include <vector>

namespace ixtreemetree
{
struct Vertex
{
    Vec3 position{};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    Vec2 uv{};
};

struct BarkMesh
{
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

struct LeafMesh
{
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

struct TreeMesh
{
    BarkMesh bark;
    LeafMesh leaves;
    Vec3 bboxMin{0.0f, 0.0f, 0.0f};
    Vec3 bboxMax{0.0f, 0.0f, 0.0f};

    struct Stats
    {
        int barkTriangles = 0;
        int leafTriangles = 0;
        float generationMs = 0.0f;
    } stats;
};
}
