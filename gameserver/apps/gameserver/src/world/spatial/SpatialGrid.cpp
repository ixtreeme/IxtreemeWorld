#include "SpatialGrid.h"

namespace gs::game {

void SpatialGrid::Clear()
{
    cells_.clear();
}

bool SpatialGrid::Empty() const noexcept
{
    return cells_.empty();
}

void SpatialGrid::Insert(std::uint32_t net_id, const Position& position)
{
    const int cell_x = SpatialCellCoord(position.x);
    const int cell_y = SpatialCellCoord(position.y);
    cells_[SpatialCellKey(cell_x, cell_y)].push_back(GridEntry{net_id});
}

} // namespace gs::game
