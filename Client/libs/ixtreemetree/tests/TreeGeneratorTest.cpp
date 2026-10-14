// The tree generator's leaf placement and the export's leaf card trim.
#include "ixtreemetree/leaf_trim.h"
#include "ixtreemetree/tree.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
int g_errors = 0;

void Check(bool ok, const char* message)
{
    if (!ok)
    {
        ++g_errors;
        std::cerr << "FAIL: " << message << '\n';
    }
}

ixtreemetree::Vec3 CardCenter(const ixtreemetree::LeafMesh& leaves, std::size_t card)
{
    ixtreemetree::Vec3 sum{};
    for (std::size_t i = 0; i < 4u; ++i)
    {
        const ixtreemetree::Vec3& p = leaves.vertices[card * 4u + i].position;
        sum = {sum.x + p.x, sum.y + p.y, sum.z + p.z};
    }
    return {sum.x * 0.25f, sum.y * 0.25f, sum.z * 0.25f};
}

void TestTrim()
{
    ixtreemetree::LeafMesh card;
    card.vertices = {{{0, 1, 0}, {0, 0, 1}, {0, 1}}, {{1, 1, 0}, {0, 0, 1}, {1, 1}},
        {{1, 0, 0}, {0, 0, 1}, {1, 0}}, {{0, 0, 0}, {0, 0, 1}, {0, 0}}};
    card.indices = {0, 1, 2, 0, 2, 3};
    const ixtreemetree::LeafMesh original = card;
    std::vector<std::uint8_t> rgba(64 * 64 * 4, 0);
    for (int y = 20; y < 44; ++y)
        for (int x = 20; x < 44; ++x)
            rgba[(y * 64 + x) * 4 + 3] = 255;
    const ixtreemetree::LeafTrimStats result = ixtreemetree::trimLeafCards(card, rgba, 64, 64, 2);
    Check(result.trimmedCards == 1 && result.areaAfter < 0.2 && result.areaBefore == 1, "the empty border is cut away");
    Check(card.indices == original.indices, "the indices stay");
    for (const ixtreemetree::Vertex& v : card.vertices)
    {
        Check(v.position.x == v.uv.x && v.position.y == v.uv.y, "the uvs move with the positions (no stretch)");
        Check(v.position.z == 0 && v.normal.z == 1, "the plane and the normal stay");
    }
    Check(card.vertices[3].uv.x < 20.0f / 64 && card.vertices[1].uv.x > 44.0f / 64, "the filtered edge keeps its padding");

    card = original;
    std::fill(rgba.begin(), rgba.end(), std::uint8_t{255});
    Check(ixtreemetree::trimLeafCards(card, rgba, 64, 64).trimmedCards == 0, "an opaque cell is left whole");
    card = original;
    std::fill(rgba.begin(), rgba.end(), std::uint8_t{0});
    Check(ixtreemetree::trimLeafCards(card, rgba, 64, 64).trimmedCards == 0, "an empty cell is left whole");
    card = original;
    card.indices[1] = 3;
    Check(ixtreemetree::trimLeafCards(card, rgba, 64, 64).trimmedCards == 0, "other topology is not touched");
    card = original;
    rgba.resize(1);
    Check(ixtreemetree::trimLeafCards(card, rgba, 64, 64).trimmedCards == 0, "a short image is refused");
}

void TestCutout()
{
    // One card on a 64 x 64 texture whose visible texels form a diamond: the card becomes a convex
    // polygon around it, smaller than its bounding square, holding every visible texel.
    ixtreemetree::LeafMesh card;
    card.vertices = {{{0, 1, 0}, {0, 0, 1}, {0, 1}}, {{1, 1, 0}, {0, 0, 1}, {1, 1}},
        {{1, 0, 0}, {0, 0, 1}, {1, 0}}, {{0, 0, 0}, {0, 0, 1}, {0, 0}}};
    card.indices = {0, 1, 2, 0, 2, 3};
    std::vector<std::uint8_t> rgba(64 * 64 * 4, 0);
    std::vector<std::pair<int, int>> visible;
    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            if (std::abs(x - 32) + std::abs(y - 32) <= 16)
            {
                rgba[(y * 64 + x) * 4 + 3] = 255;
                visible.push_back({x, y});
            }
        }
    }
    ixtreemetree::LeafMesh trimmed = card;
    const ixtreemetree::LeafTrimStats square = ixtreemetree::trimLeafCards(trimmed, rgba, 64, 64, 2);
    const ixtreemetree::LeafTrimStats cut = ixtreemetree::cutoutLeafCards(card, rgba, 64, 64, 2, 8);
    Check(cut.trimmedCards == 1 && cut.areaAfter < square.areaAfter * 0.8, "the cutout is smaller than the trimmed square");
    Check(card.vertices.size() >= 3 && card.vertices.size() <= 12 &&
            card.indices.size() == (card.vertices.size() - 2) * 3, "a triangle fan of a few corners");
    bool affine = true;
    for (const ixtreemetree::Vertex& v : card.vertices)
        affine = affine && std::abs(v.position.x - v.uv.x) < 1e-5f && std::abs(v.position.y - v.uv.y) < 1e-5f &&
            v.position.z == 0.0f && v.normal.z == 1.0f;
    Check(affine, "the uvs move with the positions, on the card's plane");
    // Every visible texel's centre inside the (convex) polygon.
    bool holdsAll = true;
    const std::size_t n = card.vertices.size();
    double orientation = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto& a = card.vertices[i].uv;
        const auto& b = card.vertices[(i + 1) % n].uv;
        orientation += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
    }
    for (const auto& [x, y] : visible)
    {
        const double px = (x + 0.5) / 64.0;
        const double py = (y + 0.5) / 64.0;
        for (std::size_t i = 0; i < n && holdsAll; ++i)
        {
            const auto& a = card.vertices[i].uv;
            const auto& b = card.vertices[(i + 1) % n].uv;
            const double side = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
            holdsAll = orientation > 0.0 ? side >= -1e-9 : side <= 1e-9;
        }
    }
    Check(holdsAll, "every visible texel is inside the cutout");

    ixtreemetree::LeafMesh other = card;
    std::fill(rgba.begin(), rgba.end(), std::uint8_t{0});
    Check(ixtreemetree::cutoutLeafCards(other, rgba, 64, 64).trimmedCards == 0, "a fan is not cut again");
}

void TestLeavesAlongTwig()
{
    // A bare trunk is the one twig: its leaf clusters spread from `start` of it to its tip.
    ixtreemetree::TreeOptions options = ixtreemetree::defaultTreeOptions();
    options.branch.levels = 0;
    options.branch.length[0] = 10.0f;
    options.branch.gnarliness[0] = 0.0f;
    options.branch.forceStrength = 0.0f;
    options.leaves.start = 0.2f;
    options.leaves.count = 10;
    options.leaves.cardsPerCluster = 1;
    ixtreemetree::Tree tree;
    tree.options = options;
    const ixtreemetree::TreeMesh mesh = tree.generate();
    const std::size_t cards = mesh.leaves.vertices.size() / 4u;
    Check(cards == 10u, "one card per cluster");
    float lowest = std::numeric_limits<float>::max();
    float highest = -std::numeric_limits<float>::max();
    for (std::size_t card = 0; card < cards; ++card)
    {
        const float y = CardCenter(mesh.leaves, card).y;
        lowest = std::min(lowest, y);
        highest = std::max(highest, y);
    }
    Check(lowest > 1.5f && highest > 8.5f && highest - lowest > 5.0f, "the clusters spread along the twig, not at its tip");

    const ixtreemetree::TreeMesh again = tree.generate();
    bool same = again.leaves.vertices.size() == mesh.leaves.vertices.size();
    for (std::size_t i = 0; same && i < mesh.leaves.vertices.size(); ++i)
        same = again.leaves.vertices[i].position.x == mesh.leaves.vertices[i].position.x &&
            again.leaves.vertices[i].position.y == mesh.leaves.vertices[i].position.y;
    Check(same, "the same seed makes the same tree");
}

void TestDoubleCardsCross()
{
    // "Double" (two cards a cluster) and any even count: no two cards of a cluster in one plane.
    for (const int perCluster : {2, 4})
    {
        ixtreemetree::TreeOptions options = ixtreemetree::defaultTreeOptions();
        options.leaves.cardsPerCluster = perCluster;
        ixtreemetree::Tree tree;
        tree.options = options;
        const ixtreemetree::TreeMesh mesh = tree.generate();
        const std::size_t cards = mesh.leaves.vertices.size() / 4u;
        float worst = 0.0f;
        for (std::size_t first = 0; first + static_cast<std::size_t>(perCluster) <= cards; first += static_cast<std::size_t>(perCluster))
        {
            for (int a = 0; a < perCluster; ++a)
            {
                for (int b = a + 1; b < perCluster; ++b)
                {
                    const ixtreemetree::Vec3 na = mesh.leaves.vertices[(first + a) * 4u].normal;
                    const ixtreemetree::Vec3 nb = mesh.leaves.vertices[(first + b) * 4u].normal;
                    worst = std::max(worst, std::abs(na.x * nb.x + na.y * nb.y + na.z * nb.z));
                }
            }
        }
        Check(cards > 0u && worst < 0.95f, "the cards of a cluster cross each other");
    }
}
} // namespace

int main()
{
    TestTrim();
    TestCutout();
    TestLeavesAlongTwig();
    TestDoubleCardsCross();
    if (g_errors == 0)
        std::cout << "TreeGeneratorTest: all checks passed\n";
    return g_errors == 0 ? 0 : 1;
}
