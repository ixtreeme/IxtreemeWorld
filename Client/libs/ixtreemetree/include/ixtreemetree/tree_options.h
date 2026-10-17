#pragma once

#include <array>
#include <cstdint>
#include <algorithm>

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
enum class BarkType : std::uint8_t { Oak, Birch, Pine, Willow, Ash, Palm, Larch };
// Palm: frond images (2 x 1 atlas, see LeafStyle::Fronds); Blossom: flower clusters; SnowyPine: needles
// with snow on top.
enum class LeafType : std::uint8_t { Oak, Ash, Pine, Willow, Birch, Palm, Blossom, SnowyPine, Larch };
constexpr int kBarkTypeCount = static_cast<int>(BarkType::Larch) + 1;
constexpr int kLeafTypeCount = static_cast<int>(LeafType::Larch) + 1;
// A crown's outline: how long a branch grows for where it starts on its parent (the shape ratios of
// Weber and Penn, "Creation and Rendering of Realistic Trees", 1995). Uniform: every one the same.
enum class CrownShape : std::uint8_t { Uniform, Conical, Spherical, Hemispherical, TaperedCylinder, Flame, InverseConical, TendFlame };
// Where a level's branches sit around their parent: anywhere, in rings (a fir's whorls) or one after
// another at the golden angle (a palm's crown).
enum class BranchArrangement : std::uint8_t { Random, Whorled, Spiral, Alternating };

// Five equally spaced control points. Parent profiles use attachment position;
// branch profiles use distance from the base. Piecewise linear, no overshoot.
struct GrowthCurve
{
    std::array<float, 5> values{1, 1, 1, 1, 1};
    float evaluate(float t) const
    {
        const float p = std::clamp(t, 0.0f, 1.0f) * 4.0f;
        const int i = std::min(3, static_cast<int>(p));
        return values[i] + (values[i + 1] - values[i]) * (p - i);
    }
};
// Cards: leaf cards in clusters along the last level's branches. Fronds: each last-level branch is one
// long leaf (a palm's or a fern's), a ribbon along it showing one atlas cell, its base at the cell's
// bottom; those branches then have no bark.
enum class LeafStyle : std::uint8_t { Cards, Fronds };

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
        // Per level (the trunk's entry unused for shape and arrangement).
        std::array<CrownShape, kMaxBranchLevels> shape{};
        std::array<BranchArrangement, kMaxBranchLevels> arrangement{};
        std::array<int, kMaxBranchLevels> whorlSize{};  // branches a ring (Whorled); 0: 4
        std::array<float, kMaxBranchLevels> droop{};    // bend toward the ground a metre grown; negative bends up
        // Opt-in authoring model. Legacy presets retain their original geometry and RNG sequence.
        bool organic = false;
        std::array<float, kMaxBranchLevels> lengthVariance{};
        std::array<float, kMaxBranchLevels> radiusVariance{};
        std::array<float, kMaxBranchLevels> angleVariance{}; // degrees
        std::array<float, kMaxBranchLevels> azimuthVariance{}; // degrees
        std::array<GrowthCurve, kMaxBranchLevels> lengthProfile{};
        std::array<GrowthCurve, kMaxBranchLevels> angleProfile{};
        std::array<GrowthCurve, kMaxBranchLevels> radiusProfile{};
        std::array<GrowthCurve, kMaxBranchLevels> bendProfile{
            GrowthCurve{{0,0,0,0,0}}, GrowthCurve{{0,0,0,0,0}},
            GrowthCurve{{0,0,0,0,0}}, GrowthCurve{{0,0,0,0,0}}}; // elevation in degrees
        std::array<float, kMaxBranchLevels> distribution{1,1,1,1}; // >1 concentrates near parent base
        std::array<float, kMaxBranchLevels> scaleInheritance{1,1,1,1}; // influence of ancestors' length scales
        std::array<float, kMaxBranchLevels> tipFoliageLength{}; // additional foliage on parent shoot tips, metres
        int trunks = 1;            // stems from the ground (a shrub has several)
        float trunkSpread = 0.0f;  // how far the stems lean from upright, degrees

        Vec3 forceDirection{0.0f, 1.0f, 0.0f};
        float forceStrength = 0.05f;
    } branch;

    // Optional world-space pruning envelope for organic branches. The trunk is retained.
    struct Crown
    {
        bool enabled = false;
        float width = 4.0f; // half-width along X, metres
        float depth = 4.0f; // half-width along Z, metres
        float base = 0.2f;  // fraction of authored trunk height
        Vec2 offset{};      // horizontal X/Z offset, metres
        GrowthCurve profile{{1,1,0.8f,0.45f,0.05f}}; // radius from crown base to top
    } crown;

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
        LeafStyle style = LeafStyle::Cards;
        float frondWidth = 1.2f;  // m (sizeVariance varies it)
        float frondFold = 15.0f;  // degrees each half rises from the midrib
        bool twigAligned = false;
        float aspect = 1.0f; // card width / length
        float density = 0.0f; // clusters / metre; zero uses count per twig
        float tipScale = 1.0f;
        GrowthCurve densityProfile{}; // relative clusters along a twig (0..1), twigAligned only
        GrowthCurve sizeProfile{};    // relative card size along a twig (0..2), twigAligned only
    } leaves;
};

TreeOptions defaultTreeOptions();
}
