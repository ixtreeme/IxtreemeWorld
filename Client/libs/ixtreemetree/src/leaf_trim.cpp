#include "ixtreemetree/leaf_trim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace ixtreemetree
{
namespace
{
// A card's uv rectangle: u0, v0 (its corner 3), u1, v1 (its corner 1).
using UvRect = std::array<float, 4>;

// The part of an atlas cell holding visible texels, padded (the cell itself when it holds none).
UvRect VisibleRect(const UvRect& cell, const std::vector<std::uint8_t>& rgba, std::uint32_t width,
    std::uint32_t height, std::uint32_t paddingPixels)
{
    const int x0 = std::clamp(static_cast<int>(std::floor(cell[0] * width)), 0, static_cast<int>(width));
    const int y0 = std::clamp(static_cast<int>(std::floor(cell[1] * height)), 0, static_cast<int>(height));
    const int x1 = std::clamp(static_cast<int>(std::ceil(cell[2] * width)), x0, static_cast<int>(width));
    const int y1 = std::clamp(static_cast<int>(std::ceil(cell[3] * height)), y0, static_cast<int>(height));
    int minX = x1;
    int minY = y1;
    int maxX = x0 - 1;
    int maxY = y0 - 1;
    for (int y = y0; y < y1; ++y)
    {
        for (int x = x0; x < x1; ++x)
        {
            if (rgba[(static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4u + 3u] == 0)
                continue;
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }
    if (maxX < minX || maxY < minY)
        return cell;
    const float padding = static_cast<float>(paddingPixels);
    return {std::max(cell[0], (static_cast<float>(minX) - padding) / static_cast<float>(width)),
        std::max(cell[1], (static_cast<float>(minY) - padding) / static_cast<float>(height)),
        std::min(cell[2], (static_cast<float>(maxX + 1) + padding) / static_cast<float>(width)),
        std::min(cell[3], (static_cast<float>(maxY + 1) + padding) / static_cast<float>(height))};
}
struct Point
{
    double x = 0.0;
    double y = 0.0;
};

double Cross(const Point& o, const Point& a, const Point& b)
{
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

// Counter-clockwise (in pixel space: x right, y down) convex hull, without collinear points.
std::vector<Point> ConvexHull(std::vector<Point> points)
{
    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) {
        return a.x != b.x ? a.x < b.x : a.y < b.y;
    });
    points.erase(std::unique(points.begin(), points.end(), [](const Point& a, const Point& b) {
        return a.x == b.x && a.y == b.y;
    }), points.end());
    if (points.size() < 3)
        return points;
    std::vector<Point> hull(points.size() * 2);
    std::size_t k = 0;
    for (const Point& p : points)
    {
        while (k >= 2 && Cross(hull[k - 2], hull[k - 1], p) <= 0.0)
            --k;
        hull[k++] = p;
    }
    for (std::size_t i = points.size() - 1, lower = k + 1; i-- > 0;)
    {
        while (k >= lower && Cross(hull[k - 2], hull[k - 1], points[i]) <= 0.0)
            --k;
        hull[k++] = points[i];
    }
    hull.resize(k - 1);
    return hull;
}

double PolygonArea(const std::vector<Point>& polygon)
{
    double area = 0.0;
    for (std::size_t i = 0; i < polygon.size(); ++i)
    {
        const Point& a = polygon[i];
        const Point& b = polygon[(i + 1) % polygon.size()];
        area += a.x * b.y - b.x * a.y;
    }
    return std::abs(area) * 0.5;
}

// Fewer corners for a convex polygon that still holds it: an edge goes, its two neighbours extended
// to meet, the one whose triangle adds the least area each time, down to maxVertices.
void ReduceEnclosing(std::vector<Point>& polygon, std::size_t maxVertices)
{
    while (polygon.size() > std::max<std::size_t>(maxVertices, 3u))
    {
        const std::size_t n = polygon.size();
        double bestArea = std::numeric_limits<double>::max();
        std::size_t best = n;
        Point bestPoint{};
        for (std::size_t i = 0; i < n; ++i)
        {
            // Edge a-b removed: the lines prev-a and b-next meet past a and b.
            const Point& prev = polygon[(i + n - 1) % n];
            const Point& a = polygon[i];
            const Point& b = polygon[(i + 1) % n];
            const Point& next = polygon[(i + 2) % n];
            const double d1x = a.x - prev.x, d1y = a.y - prev.y;
            const double d2x = b.x - next.x, d2y = b.y - next.y;
            const double denominator = d1x * d2y - d1y * d2x;
            if (std::abs(denominator) < 1e-12)
                continue;
            const double t = ((b.x - a.x) * d2y - (b.y - a.y) * d2x) / denominator;
            const double s = ((b.x - a.x) * d1y - (b.y - a.y) * d1x) / denominator;
            if (t <= 0.0 || s <= 0.0)
                continue;  // the lines meet behind: not an enclosing corner
            const Point meet{a.x + d1x * t, a.y + d1y * t};
            const double added = std::abs(Cross(a, meet, b)) * 0.5;
            if (added < bestArea)
            {
                bestArea = added;
                best = i;
                bestPoint = meet;
            }
        }
        if (best == n)
            return;
        polygon[best] = bestPoint;
        polygon.erase(polygon.begin() + static_cast<std::ptrdiff_t>((best + 1) % n));
    }
}

// The polygon cut to an axis-aligned rectangle (Sutherland-Hodgman, one side at a time).
std::vector<Point> ClipToRect(const std::vector<Point>& polygon, double x0, double y0, double x1, double y1)
{
    std::vector<Point> current = polygon;
    const auto clip = [&](auto inside, auto intersect) {
        std::vector<Point> out;
        for (std::size_t i = 0; i < current.size(); ++i)
        {
            const Point& a = current[i];
            const Point& b = current[(i + 1) % current.size()];
            const bool inA = inside(a);
            const bool inB = inside(b);
            if (inA)
                out.push_back(a);
            if (inA != inB)
                out.push_back(intersect(a, b));
        }
        current = std::move(out);
    };
    const auto atX = [](double x) {
        return [x](const Point& a, const Point& b) {
            const double t = (x - a.x) / (b.x - a.x);
            return Point{x, a.y + (b.y - a.y) * t};
        };
    };
    const auto atY = [](double y) {
        return [y](const Point& a, const Point& b) {
            const double t = (y - a.y) / (b.y - a.y);
            return Point{a.x + (b.x - a.x) * t, y};
        };
    };
    clip([&](const Point& p) { return p.x >= x0; }, atX(x0));
    if (!current.empty())
        clip([&](const Point& p) { return p.x <= x1; }, atX(x1));
    if (!current.empty())
        clip([&](const Point& p) { return p.y >= y0; }, atY(y0));
    if (!current.empty())
        clip([&](const Point& p) { return p.y <= y1; }, atY(y1));
    return current;
}

// A cell's visible texels as a convex polygon in uv (see cutoutLeafCards); empty when it has none.
std::vector<Vec2> VisiblePolygon(const UvRect& cell, const std::vector<std::uint8_t>& rgba, std::uint32_t width,
    std::uint32_t height, std::uint32_t paddingPixels, std::uint32_t maxVertices)
{
    const int x0 = std::clamp(static_cast<int>(std::floor(cell[0] * width)), 0, static_cast<int>(width));
    const int y0 = std::clamp(static_cast<int>(std::floor(cell[1] * height)), 0, static_cast<int>(height));
    const int x1 = std::clamp(static_cast<int>(std::ceil(cell[2] * width)), x0, static_cast<int>(width));
    const int y1 = std::clamp(static_cast<int>(std::ceil(cell[3] * height)), y0, static_cast<int>(height));
    // Each row's leftmost and rightmost visible texel, as their outer corners grown by the padding
    // (a square around each: the hull then holds every texel within the padding of a visible one).
    const double pad = static_cast<double>(paddingPixels);
    std::vector<Point> points;
    for (int y = y0; y < y1; ++y)
    {
        int minX = x1;
        int maxX = x0 - 1;
        const std::size_t row = static_cast<std::size_t>(y) * width;
        for (int x = x0; x < x1; ++x)
        {
            if (rgba[(row + static_cast<std::size_t>(x)) * 4u + 3u] == 0)
                continue;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
        }
        if (maxX < minX)
            continue;
        for (const double px : {static_cast<double>(minX) - pad, static_cast<double>(maxX + 1) + pad})
        {
            points.push_back({px, static_cast<double>(y) - pad});
            points.push_back({px, static_cast<double>(y + 1) + pad});
        }
    }
    if (points.empty())
        return {};
    std::vector<Point> polygon = ConvexHull(std::move(points));
    if (polygon.size() < 3)
        return {};
    ReduceEnclosing(polygon, maxVertices);
    polygon = ClipToRect(polygon, x0, y0, x1, y1);
    if (polygon.size() < 3 || PolygonArea(polygon) <= 0.0)
        return {};
    std::vector<Vec2> uv;
    uv.reserve(polygon.size());
    for (const Point& p : polygon)
        uv.push_back({static_cast<float>(p.x / width), static_cast<float>(p.y / height)});
    return uv;
}
} // namespace

LeafTrimStats trimLeafCards(LeafMesh& mesh, const std::vector<std::uint8_t>& rgba,
    std::uint32_t width, std::uint32_t height, std::uint32_t paddingPixels)
{
    LeafTrimStats stats;
    if (width == 0 || height == 0 || static_cast<std::uint64_t>(width) * height > rgba.size() / 4u ||
        mesh.vertices.size() % 4u != 0 || mesh.indices.size() != mesh.vertices.size() / 4u * 6u)
        return stats;
    // Only the generator's quads (AddLeafQuad): four vertices each, drawn as (0, 1, 2) and (0, 2, 3).
    for (std::size_t base = 0; base < mesh.vertices.size(); base += 4u)
    {
        const std::uint32_t b = static_cast<std::uint32_t>(base);
        const std::array<std::uint32_t, 6> expected{b, b + 1u, b + 2u, b, b + 2u, b + 3u};
        if (!std::equal(expected.begin(), expected.end(), mesh.indices.begin() + static_cast<std::ptrdiff_t>(base / 4u * 6u)))
            return stats;
    }
    // Cards share atlas cells: each cell's visible part is worked out once.
    std::map<UvRect, UvRect> visible;
    for (std::size_t base = 0; base < mesh.vertices.size(); base += 4u)
    {
        Vertex* quad = mesh.vertices.data() + base;
        const float u0 = quad[3].uv.x;
        const float v0 = quad[3].uv.y;
        const float u1 = quad[1].uv.x;
        const float v1 = quad[1].uv.y;
        if (!(u0 >= 0.0f && v0 >= 0.0f && u1 <= 1.0f && v1 <= 1.0f && u1 > u0 && v1 > v0) ||
            quad[0].uv.x != u0 || quad[0].uv.y != v1 || quad[2].uv.x != u1 || quad[2].uv.y != v0)
            continue;
        const UvRect cell{u0, v0, u1, v1};
        auto found = visible.find(cell);
        if (found == visible.end())
            found = visible.emplace(cell, VisibleRect(cell, rgba, width, height, paddingPixels)).first;
        const UvRect& crop = found->second;

        // The card's plane: corner 3 at (u0, v0), its u edge to corner 2, its v edge to corner 0.
        const Vec3 origin = quad[3].position;
        const Vec3 edgeU{quad[2].position.x - origin.x, quad[2].position.y - origin.y, quad[2].position.z - origin.z};
        const Vec3 edgeV{quad[0].position.x - origin.x, quad[0].position.y - origin.y, quad[0].position.z - origin.z};
        const double cx = static_cast<double>(edgeU.y) * edgeV.z - static_cast<double>(edgeU.z) * edgeV.y;
        const double cy = static_cast<double>(edgeU.z) * edgeV.x - static_cast<double>(edgeU.x) * edgeV.z;
        const double cz = static_cast<double>(edgeU.x) * edgeV.y - static_cast<double>(edgeU.y) * edgeV.x;
        const double area = std::sqrt(cx * cx + cy * cy + cz * cz);
        stats.areaBefore += area;
        stats.areaAfter += area * ((crop[2] - crop[0]) / (u1 - u0)) * ((crop[3] - crop[1]) / (v1 - v0));
        if (crop == cell)
            continue;
        ++stats.trimmedCards;
        const std::array<Vec2, 4> uv{Vec2{crop[0], crop[3]}, Vec2{crop[2], crop[3]}, Vec2{crop[2], crop[1]},
            Vec2{crop[0], crop[1]}};
        for (int i = 0; i < 4; ++i)
        {
            const float u = (uv[i].x - u0) / (u1 - u0);
            const float v = (uv[i].y - v0) / (v1 - v0);
            quad[i].position = {origin.x + edgeU.x * u + edgeV.x * v,
                origin.y + edgeU.y * u + edgeV.y * v,
                origin.z + edgeU.z * u + edgeV.z * v};
            quad[i].uv = uv[i];
        }
    }
    return stats;
}

LeafTrimStats cutoutLeafCards(LeafMesh& mesh, const std::vector<std::uint8_t>& rgba,
    std::uint32_t width, std::uint32_t height, std::uint32_t paddingPixels, std::uint32_t maxVertices)
{
    LeafTrimStats stats;
    if (width == 0 || height == 0 || static_cast<std::uint64_t>(width) * height > rgba.size() / 4u ||
        mesh.vertices.size() % 4u != 0 || mesh.indices.size() != mesh.vertices.size() / 4u * 6u)
        return stats;
    for (std::size_t base = 0; base < mesh.vertices.size(); base += 4u)
    {
        const std::uint32_t b = static_cast<std::uint32_t>(base);
        const std::array<std::uint32_t, 6> expected{b, b + 1u, b + 2u, b, b + 2u, b + 3u};
        if (!std::equal(expected.begin(), expected.end(), mesh.indices.begin() + static_cast<std::ptrdiff_t>(base / 4u * 6u)))
            return stats;
    }
    std::map<UvRect, std::vector<Vec2>> polygons;  // per atlas cell
    LeafMesh out;
    out.vertices.reserve(mesh.vertices.size() * 2u);
    out.indices.reserve(mesh.indices.size() * 3u);
    for (std::size_t base = 0; base < mesh.vertices.size(); base += 4u)
    {
        const Vertex* quad = mesh.vertices.data() + base;
        const float u0 = quad[3].uv.x;
        const float v0 = quad[3].uv.y;
        const float u1 = quad[1].uv.x;
        const float v1 = quad[1].uv.y;
        const Vec3 origin = quad[3].position;
        const Vec3 edgeU{quad[2].position.x - origin.x, quad[2].position.y - origin.y, quad[2].position.z - origin.z};
        const Vec3 edgeV{quad[0].position.x - origin.x, quad[0].position.y - origin.y, quad[0].position.z - origin.z};
        const double cx = static_cast<double>(edgeU.y) * edgeV.z - static_cast<double>(edgeU.z) * edgeV.y;
        const double cy = static_cast<double>(edgeU.z) * edgeV.x - static_cast<double>(edgeU.x) * edgeV.z;
        const double cz = static_cast<double>(edgeU.x) * edgeV.y - static_cast<double>(edgeU.y) * edgeV.x;
        const double cardArea = std::sqrt(cx * cx + cy * cy + cz * cz);
        stats.areaBefore += cardArea;

        const bool generatorQuad = u0 >= 0.0f && v0 >= 0.0f && u1 <= 1.0f && v1 <= 1.0f && u1 > u0 && v1 > v0 &&
            quad[0].uv.x == u0 && quad[0].uv.y == v1 && quad[2].uv.x == u1 && quad[2].uv.y == v0;
        const std::vector<Vec2>* polygon = nullptr;
        if (generatorQuad)
        {
            const UvRect cell{u0, v0, u1, v1};
            auto found = polygons.find(cell);
            if (found == polygons.end())
                found = polygons.emplace(cell, VisiblePolygon(cell, rgba, width, height, paddingPixels, maxVertices)).first;
            if (!found->second.empty())
                polygon = &found->second;
        }
        const std::uint32_t first = static_cast<std::uint32_t>(out.vertices.size());
        if (!polygon)
        {
            // Left as it is.
            out.vertices.insert(out.vertices.end(), quad, quad + 4);
            out.indices.insert(out.indices.end(), {first, first + 1u, first + 2u, first, first + 2u, first + 3u});
            stats.areaAfter += cardArea;
            continue;
        }
        // The polygon's corners on the card's plane (corner 3 at (u0, v0), u along edgeU, v along edgeV).
        double uvArea = 0.0;
        for (std::size_t i = 0; i < polygon->size(); ++i)
        {
            const Vec2& a = (*polygon)[i];
            const Vec2& b = (*polygon)[(i + 1) % polygon->size()];
            uvArea += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
            const float u = (a.x - u0) / (u1 - u0);
            const float v = (a.y - v0) / (v1 - v0);
            Vertex vertex = quad[0];
            vertex.position = {origin.x + edgeU.x * u + edgeV.x * v,
                origin.y + edgeU.y * u + edgeV.y * v,
                origin.z + edgeU.z * u + edgeV.z * v};
            vertex.uv = a;
            out.vertices.push_back(vertex);
        }
        for (std::uint32_t i = 1; i + 1 < polygon->size(); ++i)
            out.indices.insert(out.indices.end(), {first, first + i, first + i + 1u});
        stats.areaAfter += cardArea * (std::abs(uvArea) * 0.5) / (static_cast<double>(u1 - u0) * (v1 - v0));
        ++stats.trimmedCards;
    }
    if (stats.trimmedCards != 0)
        mesh = std::move(out);
    return stats;
}
}
