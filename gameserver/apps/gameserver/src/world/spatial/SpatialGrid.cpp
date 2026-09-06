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

std::size_t SpatialGrid::Size() const noexcept
{
    std::size_t total = 0;
    for (const auto& [key, cell] : cells_) {
        (void)key;
        total += cell.size();
    }
    return total;
}

void SpatialGrid::Insert(std::uint32_t net_id, const Position& position)
{
    const int cell_x = SpatialCellCoord(position.x);
    const int cell_y = SpatialCellCoord(position.y);
    cells_[SpatialCellKey(cell_x, cell_y)].push_back(GridEntry{net_id});
}

void SpatialGrid::Remove(std::uint32_t net_id, const Position& position)
{
    const int cell_x = SpatialCellCoord(position.x);
    const int cell_y = SpatialCellCoord(position.y);
    const auto cell_it = cells_.find(SpatialCellKey(cell_x, cell_y));
    if (cell_it == cells_.end()) {
        return;
    }
    EraseOne(cell_it->second, net_id);
    if (cell_it->second.empty()) {
        cells_.erase(cell_it);
    }
}

bool SpatialGrid::Move(std::uint32_t net_id,
                       std::int64_t old_cell_key,
                       const Position& new_position)
{
    const int cell_x = SpatialCellCoord(new_position.x);
    const int cell_y = SpatialCellCoord(new_position.y);
    const std::int64_t new_cell_key = SpatialCellKey(cell_x, cell_y);
    if (new_cell_key == old_cell_key) {
        return false;
    }
    const auto old_it = cells_.find(old_cell_key);
    if (old_it != cells_.end()) {
        EraseOne(old_it->second, net_id);
        if (old_it->second.empty()) {
            cells_.erase(old_it);
        }
    }
    cells_[new_cell_key].push_back(GridEntry{net_id});
    return true;
}

bool SpatialGrid::Contains(std::uint32_t net_id, const Position& position) const
{
    const int cell_x = SpatialCellCoord(position.x);
    const int cell_y = SpatialCellCoord(position.y);
    const auto cell_it = cells_.find(SpatialCellKey(cell_x, cell_y));
    if (cell_it == cells_.end()) {
        return false;
    }
    return std::any_of(cell_it->second.begin(),
                       cell_it->second.end(),
                       [net_id](const GridEntry& entry) {
                           return entry.net_id == net_id;
                       });
}

std::vector<std::pair<std::uint32_t, std::int64_t>> SpatialGrid::SnapshotEntries() const
{
    std::vector<std::pair<std::uint32_t, std::int64_t>> entries;
    entries.reserve(Size());
    for (const auto& [key, cell] : cells_) {
        for (const auto& entry : cell) {
            entries.emplace_back(entry.net_id, key);
        }
    }
    return entries;
}

} // namespace gs::game
