// The tree generator's leaf placement and the export's leaf card trim.
#include "ixtreemetree/leaf_trim.h"
#include "ixtreemetree/preset.h"
#include "ixtreemetree/tree.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
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

// A straight 10 m trunk with one level of straight, level branches and one leaf card at each tip: a
// card's centre is where its branch ends.
ixtreemetree::TreeOptions LevelBranches(int children)
{
    ixtreemetree::TreeOptions options = ixtreemetree::defaultTreeOptions();
    options.branch.levels = 1;
    options.branch.gnarliness = {0.0f, 0.0f, 0.0f, 0.0f};
    options.branch.forceStrength = 0.0f;
    options.branch.length[0] = 10.0f;
    options.branch.sections[0] = 40;
    options.branch.children[1] = children;
    options.branch.angle[1] = 90.0f;
    options.branch.start[1] = 0.2f;
    options.branch.length[1] = 3.0f;
    options.branch.sections[1] = 12;
    options.leaves.count = 1;
    options.leaves.cardsPerCluster = 1;
    options.leaves.start = 0.95f;
    options.leaves.sizeVariance = 0.0f;
    return options;
}

std::vector<ixtreemetree::Vec3> TipCards(const ixtreemetree::TreeOptions& options)
{
    ixtreemetree::Tree tree;
    tree.options = options;
    const ixtreemetree::TreeMesh mesh = tree.generate();
    std::vector<ixtreemetree::Vec3> centers;
    for (std::size_t card = 0; card < mesh.leaves.vertices.size() / 4u; ++card)
        centers.push_back(CardCenter(mesh.leaves, card));
    return centers;
}

// The farthest a tip reaches from the trunk among the branches starting between y0 and y1.
float Reach(const std::vector<ixtreemetree::Vec3>& tips, float y0, float y1)
{
    float reach = 0.0f;
    for (const ixtreemetree::Vec3& p : tips)
        if (p.y >= y0 && p.y <= y1)
            reach = std::max(reach, std::sqrt(p.x * p.x + p.z * p.z));
    return reach;
}

void TestCrownShape()
{
    // Level branches from 2 m to 10 m: a cone's top ones are short, a uniform crown's all alike.
    ixtreemetree::TreeOptions options = LevelBranches(40);
    const std::vector<ixtreemetree::Vec3> uniform = TipCards(options);
    options.branch.shape[1] = ixtreemetree::CrownShape::Conical;
    const std::vector<ixtreemetree::Vec3> cone = TipCards(options);
    Check(Reach(uniform, 8.4f, 10.0f) > 0.8f * Reach(uniform, 2.0f, 3.6f), "a uniform crown is as wide at the top");
    Check(Reach(cone, 8.4f, 10.0f) < 0.5f * Reach(cone, 2.0f, 3.6f) && Reach(cone, 2.0f, 3.6f) > 2.0f,
        "a conical crown narrows to the top");
    options.branch.shape[1] = ixtreemetree::CrownShape::Spherical;
    const std::vector<ixtreemetree::Vec3> sphere = TipCards(options);
    Check(Reach(sphere, 5.0f, 7.0f) > 1.5f * Reach(sphere, 9.0f, 10.0f) && Reach(sphere, 5.0f, 7.0f) > 1.5f * Reach(sphere, 2.0f, 2.8f),
        "a spherical crown is widest in the middle");
}

void TestOtherCrownShapes()
{
    using ixtreemetree::CrownShape;
    auto options = LevelBranches(40);
    options.branch.arrangement[1] = ixtreemetree::BranchArrangement::Spiral;
    const auto reaches = [&](CrownShape shape) {
        options.branch.shape[1] = shape;
        const auto tips = TipCards(options);
        return std::array<float, 3>{Reach(tips, 2.0f, 2.8f), Reach(tips, 4.0f, 5.2f), Reach(tips, 9.0f, 10.0f)};
    };
    auto r = reaches(CrownShape::Hemispherical);
    Check(r[0] > 2.8f && r[2] < 1.2f, "a hemispherical crown narrows from a broad base");
    r = reaches(CrownShape::TaperedCylinder);
    Check(r[0] > 2.8f && r[2] > 1.4f && r[2] < 1.9f, "a tapered cylinder keeps half its width at the top");
    r = reaches(CrownShape::Flame);
    Check(r[1] > 2.7f && r[0] < 1.2f && r[2] < 0.7f, "a flame crown is widest above its base");
    r = reaches(CrownShape::InverseConical);
    Check(r[0] < 0.9f && r[2] > 2.6f, "an inverse cone widens toward the top");
    r = reaches(CrownShape::TendFlame);
    Check(r[1] > 2.8f && r[0] < 2.1f && r[2] > 1.4f && r[2] < 1.9f, "a tended flame retains width at both ends");
}

std::size_t DistinctHeights(const std::vector<ixtreemetree::Vec3>& tips, float step)
{
    std::vector<long> heights;
    for (const ixtreemetree::Vec3& p : tips)
        heights.push_back(std::lround(p.y / step));
    std::sort(heights.begin(), heights.end());
    return static_cast<std::size_t>(std::unique(heights.begin(), heights.end()) - heights.begin());
}

void TestArrangements()
{
    ixtreemetree::TreeOptions options = LevelBranches(12);
    Check(DistinctHeights(TipCards(options), 0.25f) >= 8u, "random branches start at many heights");
    options.branch.arrangement[1] = ixtreemetree::BranchArrangement::Whorled;
    options.branch.whorlSize[1] = 4;
    const std::vector<ixtreemetree::Vec3> whorls = TipCards(options);
    Check(whorls.size() == 12u && DistinctHeights(whorls, 0.25f) == 3u, "twelve branches in rings of four: three heights");

    // A spiral turns by the golden angle from one branch to the next (around the vertical trunk, a
    // branch's azimuth is atan2(x, z)).
    options = LevelBranches(8);
    options.branch.arrangement[1] = ixtreemetree::BranchArrangement::Spiral;
    const std::vector<ixtreemetree::Vec3> spiral = TipCards(options);
    bool golden = spiral.size() == 8u;
    for (std::size_t i = 1; golden && i < spiral.size(); ++i)
    {
        const double a = std::atan2(spiral[i - 1].x, spiral[i - 1].z);
        const double b = std::atan2(spiral[i].x, spiral[i].z);
        double turn = (b - a) * 180.0 / 3.14159265358979 - 137.5;
        turn -= 360.0 * std::round(turn / 360.0);
        golden = std::abs(turn) < 3.0;
    }
    Check(golden, "spiral branches turn by 137.5 degrees each");
    Check(DistinctHeights(spiral, 0.25f) == 8u, "spiral branches climb the trunk one by one");
}

void TestDroop()
{
    ixtreemetree::TreeOptions options = LevelBranches(12);
    const auto meanY = [](const std::vector<ixtreemetree::Vec3>& tips) {
        double sum = 0.0;
        for (const ixtreemetree::Vec3& p : tips)
            sum += p.y;
        return tips.empty() ? 0.0 : sum / static_cast<double>(tips.size());
    };
    const double level = meanY(TipCards(options));
    options.branch.droop[1] = 0.2f;
    const std::vector<ixtreemetree::Vec3> drooping = TipCards(options);
    Check(meanY(drooping) < level - 0.5, "drooping branches bend toward the ground");
    options.branch.droop[1] = -0.2f;
    Check(meanY(TipCards(options)) > level + 0.5, "negative droop bends them up");
}

void TestTrunks()
{
    ixtreemetree::TreeOptions options = LevelBranches(0);
    options.branch.levels = 0;
    options.branch.trunks = 5;
    options.branch.trunkSpread = 30.0f;
    const std::vector<ixtreemetree::Vec3> tips = TipCards(options);
    float widest = 0.0f;
    for (const ixtreemetree::Vec3& a : tips)
        for (const ixtreemetree::Vec3& b : tips)
            widest = std::max(widest, std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)));
    Check(tips.size() == 5u && widest > 4.0f, "five stems lean out from one base");
    bool lowered = !tips.empty();
    for (const ixtreemetree::Vec3& p : tips)
        lowered = lowered && p.y < 9.6f && p.y > 5.0f;
    Check(lowered, "a leaning stem's tip is lower than an upright one's");
}

// A palm: a straight trunk, 12 fronds at its top in a spiral, arching down.
ixtreemetree::TreeOptions PalmOptions()
{
    ixtreemetree::TreeOptions options = LevelBranches(12);
    options.branch.arrangement[1] = ixtreemetree::BranchArrangement::Spiral;
    options.branch.start[1] = 0.95f;
    options.branch.angle[1] = 70.0f;
    options.branch.length[1] = 4.0f;
    options.branch.sections[1] = 8;
    options.branch.droop[1] = 0.2f;
    options.leaves.style = ixtreemetree::LeafStyle::Fronds;
    options.leaves.atlasGridX = 2;
    options.leaves.atlasGridY = 1;
    options.leaves.frondWidth = 1.5f;
    return options;
}

float Distance(ixtreemetree::Vec3 a, ixtreemetree::Vec3 b)
{
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

void TestFronds()
{
    ixtreemetree::Tree tree;
    tree.options = PalmOptions();
    const ixtreemetree::TreeMesh mesh = tree.generate();
    const std::vector<ixtreemetree::Vertex>& v = mesh.leaves.vertices;
    const std::size_t quads = v.size() / 4u;
    Check(quads == 12u * 8u * 2u && mesh.leaves.indices.size() == quads * 6u, "a frond is two quads a segment");

    bool flat = true;
    bool inCell = true;
    bool joined = true;
    bool topology = true;
    bool uvRunsToTip = true;
    bool upwardNormals = true;
    float lowestTip = std::numeric_limits<float>::max();
    for (std::size_t q = 0; q < quads; ++q)
    {
        const ixtreemetree::Vertex* quad = v.data() + q * 4u;
        const auto base = static_cast<std::uint32_t>(q * 4u);
        const std::array<std::uint32_t, 6> expectedIndices{base, base + 1u, base + 2u, base, base + 2u, base + 3u};
        topology = topology && std::equal(expectedIndices.begin(), expectedIndices.end(), mesh.leaves.indices.begin() + q * 6u);
        upwardNormals = upwardNormals && quad[0].normal.y >= 0.0f;
        const auto segmentIndex = (q / 2u) % 8u;
        const float vBase = 1.0f - static_cast<float>(segmentIndex) / 8.0f;
        const float vTip = 1.0f - static_cast<float>(segmentIndex + 1u) / 8.0f;
        uvRunsToTip = uvRunsToTip && std::abs(quad[0].uv.y - vBase) < 1e-6f &&
            std::abs(quad[1].uv.y - vBase) < 1e-6f && std::abs(quad[2].uv.y - vTip) < 1e-6f &&
            std::abs(quad[3].uv.y - vTip) < 1e-6f;
        // A parallelogram: corner 1 is corner 2 plus corner 0 minus corner 3 (what the trim assumes).
        const ixtreemetree::Vec3 expected{quad[2].position.x + quad[0].position.x - quad[3].position.x,
            quad[2].position.y + quad[0].position.y - quad[3].position.y,
            quad[2].position.z + quad[0].position.z - quad[3].position.z};
        flat = flat && Distance(expected, quad[1].position) < 1e-4f;
        const float cellX = v[(q / 16u) * 64u].uv.x;
        for (int i = 0; i < 4; ++i)
            inCell = inCell && quad[i].uv.x >= cellX - 1e-6f && quad[i].uv.x <= cellX + 0.5f + 1e-6f &&
                quad[i].uv.y >= -1e-6f && quad[i].uv.y <= 1.0f + 1e-6f;
        // The same half's next segment (two quads on) starts where this one ends.
        const std::size_t segment = (q / 2u) % 8u;
        if (segment + 1u < 8u)
        {
            const ixtreemetree::Vertex* next = quad + 8;
            joined = joined && Distance(next[0].position, quad[3].position) < 1e-5f &&
                Distance(next[1].position, quad[2].position) < 1e-5f;
        }
        else
        {
            lowestTip = std::min(lowestTip, quad[2].position.y);
        }
    }
    Check(flat, "every frond segment is flat");
    Check(inCell, "a frond shows one atlas cell");
    Check(joined, "a frond's segments meet without cracks");
    Check(topology, "fronds use the leaf trim's exact quad index convention");
    Check(uvRunsToTip, "frond UVs run continuously from the cell's bottom to its top");
    Check(upwardNormals, "frond face normals point upward");
    Check(lowestTip < 9.5f, "the fronds arch down below the crown");
    if (v.size() >= 8u)
    {
        const auto& l = v[0].position;
        const auto& c = v[1].position;
        const auto& r = v[5].position;
        const float half = Distance(l, c);
        const float foldedDot = ((l.x - c.x) * (r.x - c.x) + (l.y - c.y) * (r.y - c.y) +
            (l.z - c.z) * (r.z - c.z)) / (half * half);
        Check(std::abs(half - 0.75f) < 1e-5f, "frond width sets both half widths");
        Check(std::abs(foldedDot + 0.8660254f) < 1e-4f, "frond fold raises both halves by fifteen degrees");
    }

    // The last level has no bark: fewer bark triangles than the same tree with leaf cards.
    ixtreemetree::Tree cards;
    cards.options = PalmOptions();
    cards.options.leaves.style = ixtreemetree::LeafStyle::Cards;
    Check(mesh.stats.barkTriangles < cards.generate().stats.barkTriangles && mesh.stats.barkTriangles > 0,
        "fronds draw no bark of their own");

    // The export's trim takes them: a texture visible only in a narrow band down each cell's middle.
    ixtreemetree::LeafMesh trimmed = mesh.leaves;
    std::vector<std::uint8_t> rgba(64 * 32 * 4, 0);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x)
            if (x % 32 >= 10 && x % 32 < 22)
                rgba[(y * 64 + x) * 4 + 3] = 255;
    const ixtreemetree::LeafTrimStats trim = ixtreemetree::trimLeafCards(trimmed, rgba, 64, 32, 1);
    Check(trim.trimmedCards == quads && trim.areaAfter < trim.areaBefore * 0.9, "the leaf trim fits each frond segment");
    bool trimPreservesPlane = true;
    for (std::size_t q = 0; q < quads; ++q)
    {
        const auto& origin = v[q * 4u].position;
        const auto& normal = v[q * 4u].normal;
        for (std::size_t corner = 0; corner < 4u; ++corner)
        {
            const auto& p = trimmed.vertices[q * 4u + corner].position;
            const float distance = (p.x - origin.x) * normal.x + (p.y - origin.y) * normal.y + (p.z - origin.z) * normal.z;
            trimPreservesPlane = trimPreservesPlane && std::abs(distance) < 1e-4f;
        }
    }
    Check(trimPreservesPlane, "trimming keeps every frond segment in its original plane");
}

void TestPresets(bool print)
{
    std::vector<std::string> errors;
    const std::vector<ixtreemetree::Preset> presets = ixtreemetree::loadAllPresets(IXTREEMETREE_PRESET_DIR, &errors);
    for (const auto& error : errors) std::cerr << error << '\n';
    Check(errors.empty(), "every preset parses without errors");
    const auto find = [&](const char* name) -> const ixtreemetree::Preset* {
        const auto it = std::find_if(presets.begin(), presets.end(), [&](const ixtreemetree::Preset& p) { return p.name == name; });
        return it == presets.end() ? nullptr : &*it;
    };
    bool sane = !presets.empty();
    for (const ixtreemetree::Preset& preset : presets)
    {
        ixtreemetree::Tree tree;
        tree.options = preset.options;
        const ixtreemetree::TreeMesh mesh = tree.generate();
        const float height = mesh.bboxMax.y - mesh.bboxMin.y;
        const float width = std::max(mesh.bboxMax.x - mesh.bboxMin.x, mesh.bboxMax.z - mesh.bboxMin.z);
        if (print)
            std::cout << preset.name << ": height " << height << " m, width " << width << " m, bark " << mesh.stats.barkTriangles
                      << " / leaf " << mesh.stats.leafTriangles << " triangles\n";
        sane = sane && !mesh.bark.vertices.empty() && std::isfinite(height) && std::isfinite(width) && height > 0.5f && height < 120.0f;
        for (float coordinate : {mesh.bboxMin.x, mesh.bboxMin.y, mesh.bboxMin.z, mesh.bboxMax.x, mesh.bboxMax.y, mesh.bboxMax.z})
            sane = sane && std::isfinite(coordinate);
    }
    Check(sane, "every preset grows a tree of a sane size");

    const ixtreemetree::Preset* fir = find("Fir Tree");
    Check(fir && fir->options.branch.arrangement[1] == ixtreemetree::BranchArrangement::Whorled &&
            fir->options.branch.whorlSize[1] == 6 && fir->options.branch.shape[1] == ixtreemetree::CrownShape::Conical &&
            fir->options.branch.droop[1] > 0.0f, "the fir preset reads its whorls, cone and droop");
    const ixtreemetree::Preset* shrub = find("Round Shrub");
    Check(shrub && shrub->options.branch.trunks == 6 && shrub->options.branch.trunkSpread == 40.0f &&
            shrub->options.branch.shape[1] == ixtreemetree::CrownShape::Spherical, "the shrub preset reads its stems and sphere");
    const ixtreemetree::Preset* willow = find("Weeping Willow");
    Check(willow && willow->options.branch.droop[2] > 0.3f && willow->options.branch.droop[1] < 0.0f,
        "the willow preset reads its droop both ways");
    const ixtreemetree::Preset* dead = find("Dead Tree");
    if (dead)
    {
        ixtreemetree::Tree tree;
        tree.options = dead->options;
        Check(tree.generate().leaves.vertices.empty(), "the dead tree has no leaves");
    }
    Check(dead != nullptr, "the dead tree preset loads");
    const auto* palm = find("Palm Tall");
    Check(palm && palm->options.leaves.style == ixtreemetree::LeafStyle::Fronds &&
        palm->options.leaves.type == ixtreemetree::LeafType::Palm && palm->options.bark.type == ixtreemetree::BarkType::Palm &&
        palm->options.branch.arrangement[1] == ixtreemetree::BranchArrangement::Spiral &&
        palm->options.leaves.atlasGridX == 2 && palm->options.leaves.atlasGridY == 1 &&
        palm->options.leaves.frondWidth == 1.6f && palm->options.leaves.frondFold == 15.0f,
        "the palm preset reads frond style, dimensions, textures and spiral");
    const auto* curved = find("Palm Curved");
    Check(curved && curved->options.branch.trunkSpread == 28.0f && curved->options.branch.droop[0] == -0.05f,
        "the curved palm reads trunk lean and upward curve");
    const auto* snowy = find("Snowy Fir");
    Check(snowy && snowy->options.leaves.type == ixtreemetree::LeafType::SnowyPine &&
        snowy->options.branch.shape[1] == ixtreemetree::CrownShape::Conical, "the snowy fir reads snow and cone");
    const auto* blossom = find("Blossom Tree");
    Check(blossom && blossom->options.leaves.type == ixtreemetree::LeafType::Blossom &&
        blossom->options.branch.shape[1] == ixtreemetree::CrownShape::Spherical, "the blossom tree reads flowers and sphere");
    const auto* blossomShrub = find("Blossom Shrub");
    Check(blossomShrub && blossomShrub->options.leaves.type == ixtreemetree::LeafType::Blossom &&
        blossomShrub->options.branch.trunks == 5 && blossomShrub->options.branch.trunkSpread == 35.0f,
        "the blossom shrub reads flowers and several leaning stems");
}

void TestPresetDefaultsAndNumericEnums()
{
    const auto path = std::filesystem::temp_directory_path() / ("ixw_tree_options_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{path};
    {
        std::ofstream file(path);
        file << "{}";
    }
    const auto defaults = ixtreemetree::loadPresetFile(path);
    Check(defaults && HashOf(defaults->options) == HashOf(ixtreemetree::defaultTreeOptions()) &&
        defaults->options.leaves.style == ixtreemetree::LeafStyle::Cards && defaults->options.leaves.frondWidth == 1.2f &&
        defaults->options.leaves.frondFold == 15.0f, "absent keys retain all generator defaults");
    {
        std::ofstream file(path);
        file << R"({"branch":{"shape":[0,1,2,7],"arrangement":[0,1,2],"whorlSize":[0,5],"droop":[-0.05,0.22],
            "trunks":5,"trunkSpread":30},"leaves":{"style":"Fronds","frondWidth":2.3,"frondFold":25}})";
    }
    const auto numeric = ixtreemetree::loadPresetFile(path);
    Check(numeric && numeric->options.branch.shape[1] == ixtreemetree::CrownShape::Conical &&
        numeric->options.branch.shape[2] == ixtreemetree::CrownShape::Spherical &&
        numeric->options.branch.shape[3] == ixtreemetree::CrownShape::TendFlame &&
        numeric->options.branch.arrangement[1] == ixtreemetree::BranchArrangement::Whorled &&
        numeric->options.branch.arrangement[2] == ixtreemetree::BranchArrangement::Spiral &&
        numeric->options.branch.whorlSize[1] == 5 && numeric->options.branch.trunks == 5 &&
        numeric->options.branch.trunkSpread == 30.0f && numeric->options.branch.droop[0] == -0.05f &&
        numeric->options.leaves.frondWidth == 2.3f && numeric->options.leaves.frondFold == 25.0f,
        "numeric enums and every new numeric field load");
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
    for (const auto& golden : kGolden)
        Check(std::count_if(actual.begin(), actual.end(), [&](const auto& tree) { return tree.first == golden.name; }) == 1,
            (std::string("golden tree still loads exactly once: ") + golden.name).c_str());
}
void TestOrganicGrowth()
{
    using namespace ixtreemetree;
    Tree tree;
    tree.options=defaultTreeOptions();
    auto& o=tree.options;
    o.branch.organic=true; o.branch.levels=1;
    o.branch.forceStrength=0; o.branch.gnarliness.fill(0); o.branch.twist.fill(0);
    o.branch.sections={2,4,3,3}; o.branch.segments={6,4,3,3};
    o.branch.children[1]=9; o.branch.start[1]=0.2f;
    o.branch.angle[1]=90; o.branch.lengthVariance[1]=0.23f;
    o.leaves.twigAligned=true; o.leaves.angle=0; o.leaves.density=4; o.leaves.aspect=0.6f;
    const auto coarse=tree.generate();
    const std::size_t trunkVertices=3*7+1, branchVertices=5*5+1;
    std::vector<float> heights;
    for (int child=0; child<9; ++child)
    {
        float height=0;
        for (int side=0;side<4;++side) height+=coarse.bark.vertices[trunkVertices+child*branchVertices+side].position.y;
        heights.push_back(height/4);
    }
    std::sort(heights.begin(),heights.end());
    Check(std::adjacent_find(heights.begin(),heights.end(),[](float a,float b){return std::abs(a-b)<1e-4f;})==heights.end(),
        "organic branches attach continuously instead of snapping to trunk rings");
    o.branch.sections[0]=32; o.branch.sections[1]=24;
    const auto fine=tree.generate();
    bool same=coarse.leaves.vertices.size()==fine.leaves.vertices.size();
    for (std::size_t i=0;same && i<coarse.leaves.vertices.size();++i)
    {
        const auto a=coarse.leaves.vertices[i].position,b=fine.leaves.vertices[i].position;
        same = a.x==b.x && a.y==b.y && a.z==b.z;
    }
    Check(same,"tessellation does not move organic twigs, random variation or leaves");
    const auto& leaves=coarse.leaves.vertices;
    Check(!leaves.empty() && std::abs(leaves[3].position.y-leaves[0].position.y)<1e-5f,
        "twig-aligned cards follow a horizontal twig instead of world up");
    const auto original=HashOf(o);
    o.branch.lengthProfile[1].values={1,0.8f,0.5f,0.2f,0.05f};
    Check(HashOf(o)!=original,"parent length profiles change the generated crown");
    const auto straight=HashOf(o);
    o.branch.bendProfile[1].values={0,-20,-30,10,40};
    Check(HashOf(o)!=straight,"bend profiles affect growth");
    const GrowthCurve curve{{0,1,0,2,1}};
    Check(curve.evaluate(0.125f)==0.5f && curve.evaluate(0.75f)==2 && curve.evaluate(1)==1,
        "growth curves interpolate and include both endpoints");

    const auto path=std::filesystem::temp_directory_path() / ("ixw_tree_roundtrip_"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".json");
    std::string error;
    auto presets=loadAllPresets(IXTREEMETREE_PRESET_DIR);
    presets.push_back({"Edited \"organic\" tree",o});
    for (const auto& preset:presets)
    {
        Check(savePresetFile(path,preset,&error),"growth preset saves");
        const auto loaded=loadPresetFile(path,&error);
        Check(loaded && loaded->name==preset.name && HashOf(loaded->options)==HashOf(preset.options),
            "saving and reloading every preset reproduces exactly the same mesh");
        if (loaded)
            Check(loaded->options.bark.tint==preset.options.bark.tint && loaded->options.leaves.tint==preset.options.leaves.tint &&
                loaded->options.leaves.alphaTest==preset.options.leaves.alphaTest,"round trip retains material settings");
    }
    std::error_code ec; std::filesystem::remove(path,ec);
    o.branch.levels=3; o.branch.children.fill(128); o.branch.sections.fill(2); o.branch.segments.fill(3);
    o.leaves.density=0; o.leaves.count=0;
    const auto limited=tree.generate();
    Check(limited.stats.budgetLimited && limited.bark.vertices.size()<2000000,
        "organic authoring enforces and reports its generation budget");
    o.branch.levels=2; o.branch.children[1]=1; o.branch.children[2]=0; o.leaves.count=2;
    Check(!tree.generate().leaves.vertices.empty(),"shoots that stop early in the hierarchy still carry leaves");
    o.branch.levels=1; o.branch.children[1]=3; o.branch.sections[1]=5;
    o.leaves.style=ixtreemetree::LeafStyle::Fronds;
    const auto fronds=tree.generate();
    Check(fronds.leaves.vertices.size()==3u*5u*2u*4u,"organic fronds respect the authored segment count");
}

void TestAlternatingShoots()
{
    for (bool organic : {false,true})
    {
        ixtreemetree::Tree tree;
        tree.options=ixtreemetree::defaultTreeOptions();
        auto& o=tree.options;
        o.branch.organic=organic; o.branch.levels=1;
        o.branch.children[1]=8; o.branch.angle[1]=90;
        o.branch.gnarliness.fill(0); o.branch.twist.fill(0); o.branch.forceStrength=0;
        o.branch.arrangement[1]=ixtreemetree::BranchArrangement::Alternating;
        o.leaves.twigAligned=true; o.leaves.angle=0; o.leaves.count=1;
        o.leaves.cardsPerCluster=1; o.leaves.start=0.98f; o.leaves.size=0.1f;
        const auto mesh=tree.generate();
        Check(mesh.leaves.vertices.size()==32,"alternating shoots carry one card per twig");
        for (std::size_t i=0; i<mesh.leaves.vertices.size()/4; ++i)
        {
            const auto p=CardCenter(mesh.leaves,i);
            Check(std::abs(p.x)<1e-4f && (i%2==0 ? p.z>1 : p.z<-1),
                "alternating shoots grow on opposite sides in both growth models");
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    TestTrim();
    TestCutout();
    TestLeavesAlongTwig();
    TestDoubleCardsCross();
    TestCrownShape();
    TestOtherCrownShapes();
    TestArrangements();
    TestDroop();
    TestTrunks();
    TestFronds();
    TestExistingTreesUnchanged(argc > 1 && std::string(argv[1]) == "--print-golden");
    TestPresets(argc > 1 && std::string(argv[1]) == "--print-golden");
    TestPresetDefaultsAndNumericEnums();
    TestOrganicGrowth();
    TestAlternatingShoots();
    if (g_errors == 0)
        std::cout << "TreeGeneratorTest: all checks passed\n";
    return g_errors == 0 ? 0 : 1;
}
