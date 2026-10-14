#pragma once

#include <string>

// Debug/test consistency check for the incremental spatial index: every
// authoritative resident and every ghost must be indexed exactly once, at
// the cell derived from its current position, and the index must contain
// nothing else. Never called on the production hot path (O(entities)).
namespace gs::game {

class Zone;
class SpatialGrid;

bool ValidateSpatialIndex(Zone& zone, const SpatialGrid& grid, std::string& out_error);

} // namespace gs::game
