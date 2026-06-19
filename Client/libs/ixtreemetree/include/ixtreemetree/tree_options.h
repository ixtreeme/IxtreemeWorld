#pragma once

#include <array>
#include <cstdint>

namespace ixtreemetree
{
constexpr int kMaxBranchLevels = 4;
constexpr const char* kVersion = "0.1.0";

struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

enum class TreeType : std::uint8_t { Deciduous, Evergreen };
enum class BarkType : std::uint8_t { Oak, Birch, Pine, Willow, Ash };
enum class LeafType : std::uint8_t { Oak, Ash, Pine, Willow, Birch };

struct TreeOptions
{
    std::uint32_t seed = 12345;
    TreeType type = TreeType::Deciduous;

    struct Bark
    {
        BarkType type = BarkType::Oak;
        std::uint32_t tint = 0xFFFFFFFFu;
        bool flatShading = false;
        bool textured = true;
        Vec2 textureScale{1.0f, 1.0f};
    } bark;

    struct Branch
    {
        int levels = 3;
        std::array<float, kMaxBranchLevels> angle{};
        std::array<int, kMaxBranchLevels> children{};
        std::array<float, kMaxBranchLevels> gnarliness{};
        std::array<float, kMaxBranchLevels> length{};
        std::array<float, kMaxBranchLevels> radius{};
        std::array<int, kMaxBranchLevels> sections{};
        std::array<int, kMaxBranchLevels> segments{};
        std::array<float, kMaxBranchLevels> start{};
        std::array<float, kMaxBranchLevels> taper{};
        std::array<float, kMaxBranchLevels> twist{};

        Vec3 forceDirection{0.0f, 1.0f, 0.0f};
        float forceStrength = 0.05f;
    } branch;

    struct Leaves
    {
        LeafType type = LeafType::Oak;
        int cardsPerCluster = 3;
        int atlasGridX = 2;
        int atlasGridY = 2;
        float angle = 30.0f;
        int count = 5;
        float start = 0.5f;
        float size = 1.0f;
        float sizeVariance = 0.2f;
        std::uint32_t tint = 0xFFFFFFFFu;
        float alphaTest = 0.5f;
    } leaves;
};

TreeOptions defaultTreeOptions();
}
