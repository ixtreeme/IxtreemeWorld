#pragma once

#include "tree_mesh.h"

#include <cstdint>
#include <vector>

namespace ixtreemetree
{
struct LeafTrimStats
{
    std::uint32_t trimmedCards = 0;
    double areaBefore = 0.0;  // the cards' area, in mesh units squared
    double areaAfter = 0.0;
};

// Shrinks each leaf card (the generator's quads, each on one cell of the leaf atlas) to the part of its
// cell that holds any visible texel, plus paddingPixels around it for the filtered edge: the texture's
// fully transparent border is not rasterized (the leaf textures are 80-93% transparent). The uvs and
// positions move together, so the texture does not stretch; the indices and normals stay. A mesh that
// is not such quads is left as it is; so is a card whose cell is empty (filtering may reach a neighbour).
LeafTrimStats trimLeafCards(LeafMesh& mesh, const std::vector<std::uint8_t>& rgba,
    std::uint32_t width, std::uint32_t height, std::uint32_t paddingPixels = 16);

// Like trimLeafCards, but each card becomes a convex polygon around its cell's visible texels (their
// convex hull, grown by paddingPixels, of at most about maxVertices corners, still holding every
// visible texel; clipped to the cell), drawn as a triangle fan. A leaf drawn alone in its cell (the
// built-in textures) then rasterizes little more than the leaf: about half the trimmed card's area.
// The uvs and positions move together, so the texture does not stretch. A mesh that is not the
// generator's quads is left as it is; so is a card whose cell is empty.
LeafTrimStats cutoutLeafCards(LeafMesh& mesh, const std::vector<std::uint8_t>& rgba,
    std::uint32_t width, std::uint32_t height, std::uint32_t paddingPixels = 8, std::uint32_t maxVertices = 8);
}
