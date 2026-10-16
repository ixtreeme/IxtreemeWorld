// The tree generator's leaf placement and the export's leaf card trim.
#include "ixtreemetree/leaf_trim.h"
#include "ixtreemetree/preset.h"
#include "ixtreemetree/tree.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
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

// FNV-1a over every byte of the mesh: its vertices (position, normal, uv) and indices.
std::uint64_t MeshHash(const ixtreemetree::TreeMesh& mesh)
{
    std::uint64_t hash = 1469598103934665603ull;
    const auto add = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i)
            hash = (hash ^ bytes[i]) * 1099511628211ull;
    };
    for (const auto* part : {&mesh.bark.vertices, &mesh.leaves.vertices})
        for (const ixtreemetree::Vertex& v : *part)
        {
            const float values[8] = {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z, v.uv.x, v.uv.y};
            add(values, sizeof(values));
        }
    add(mesh.bark.indices.data(), mesh.bark.indices.size() * sizeof(std::uint32_t));
    add(mesh.leaves.indices.data(), mesh.leaves.indices.size() * sizeof(std::uint32_t));
    return hash;
}

std::uint64_t HashOf(const ixtreemetree::TreeOptions& options)
{
    ixtreemetree::Tree tree;
    tree.options = options;
    return MeshHash(tree.generate());
}

// The trees made before the generator learned crown shapes, arrangements, droop, several trunks and
// fronds: their new settings default to the old behaviour, so every one must come out bit for bit
// the same.
void TestExistingTreesUnchanged(bool print)
{
    struct Golden
    {
        const char* name;
        std::uint64_t hash;
    };
    static const Golden kGolden[] = {
        {"default", 0x2351d9504fe5e02bull},
        {"Ash Large", 0x397b3dd627be20dull},
        {"Ash Medium", 0x6a5b32221451eebull},
        {"Ash Small", 0xfac3f447583161c0ull},
        {"Aspen Large", 0x859b461e919522f7ull},
        {"Aspen Medium", 0xa189f3ec1450ca33ull},
        {"Aspen Small", 0x9830c30db9baccc9ull},
        {"Bush 1", 0xc8d451d7d2370172ull},
        {"Bush 2", 0x4c4af74bd925e36bull},
        {"Bush 3", 0x322e0a9f31227a8bull},
        {"Oak Large", 0x2ced4a2153ad78d9ull},
        {"Oak Medium", 0xe302c548475c4a3full},
        {"Oak Small", 0x5a24bdeffa31bfc4ull},
        {"Pine Large", 0x318d2c4c77a2e221ull},
        {"Pine Medium", 0x8bf49ff498544c8full},
        {"Pine Small", 0x4ccd31966451d811ull},
        {"Trellis", 0xd3dd19e13437a81eull},
    };
    std::vector<std::pair<std::string, std::uint64_t>> actual{{"default", HashOf(ixtreemetree::defaultTreeOptions())}};
    for (const ixtreemetree::Preset& preset : ixtreemetree::loadAllPresets(IXTREEMETREE_PRESET_DIR))
        actual.push_back({preset.name, HashOf(preset.options)});
    for (const auto& [name, hash] : actual)
    {
        if (print)
            std::cout << "        {\"" << name << "\", 0x" << std::hex << hash << std::dec << "ull},\n";
        const auto golden = std::find_if(std::begin(kGolden), std::end(kGolden),
            [&name = name](const Golden& g) { return name == g.name; });
        if (golden != std::end(kGolden))
            Check(golden->hash == hash, ("an existing tree is unchanged: " + name).c_str());
    }
    Check(actual.size() >= std::size(kGolden), "every golden tree still loads");
}
} // namespace

int main(int argc, char** argv)
{
    TestTrim();
    TestCutout();
    TestLeavesAlongTwig();
    TestDoubleCardsCross();
    TestExistingTreesUnchanged(argc > 1 && std::string(argv[1]) == "--print-golden");
    if (g_errors == 0)
        std::cout << "TreeGeneratorTest: all checks passed\n";
    return g_errors == 0 ? 0 : 1;
}
