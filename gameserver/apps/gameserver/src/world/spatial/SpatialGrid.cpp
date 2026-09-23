#include "SpatialGrid.h"

#include "../components/GridSlot.h"

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

void SpatialGrid::Insert(std::uint32_t net_id, const Position& position, flecs::entity entity)
{
    const int cell_x = SpatialCellCoord(position.x);
    const int cell_y = SpatialCellCoord(position.y);
    const std::int64_t cell_key = SpatialCellKey(cell_x, cell_y);
    auto& cell = cells_[cell_key];
    cell.push_back(GridEntry{net_id, position.x, position.y, position.z, entity});
    if (entity.is_valid()) {
        entity.set<GridSlot>({cell_key, static_cast<std::uint32_t>(cell.size() - 1)});
    }
    ++maintenance_.inserts;
}

void SpatialGrid::Remove(std::uint32_t net_id, const Position& position)
{
    const int cell_x = SpatialCellCoord(position.x);
    const int cell_y = SpatialCellCoord(position.y);
    const std::int64_t cell_key = SpatialCellKey(cell_x, cell_y);
    const auto cell_it = cells_.find(cell_key);
    if (cell_it == cells_.end()) {
        return;
    }
    auto& cell = cell_it->second;
    for (std::size_t i = 0; i < cell.size(); ++i) {
        if (cell[i].net_id != net_id) {
            continue;
        }
        // Swap-erase keeps the GridSlot indices of the remaining entries
        // valid with one fix-up write.
        cell[i] = cell.back();
        cell.pop_back();
        if (i < cell.size() && cell[i].entity.is_valid()) {
            cell[i].entity.set<GridSlot>({cell_key, static_cast<std::uint32_t>(i)});
        }
        if (cell.empty()) {
            cells_.erase(cell_it);
        }
        ++maintenance_.removes;
        return;
    }
}

bool SpatialGrid::Move(flecs::entity entity,
                       std::uint32_t net_id,
                       std::int64_t old_cell_key,
                       const Position& new_position)
{
    const int cell_x = SpatialCellCoord(new_position.x);
    const int cell_y = SpatialCellCoord(new_position.y);
    const std::int64_t new_cell_key = SpatialCellKey(cell_x, cell_y);
    if (new_cell_key == old_cell_key) {
        // In-cell move: one O(1) write through the entity's grid slot. The
        // stored position is what the AOI radius scan reads.
        const auto* slot = entity.is_valid() ? entity.try_get<GridSlot>() : nullptr;
        const auto cell_it = cells_.find(old_cell_key);
        if (slot != nullptr && cell_it != cells_.end() && slot->index < cell_it->second.size() &&
            cell_it->second[slot->index].net_id == net_id) {
            GridEntry& entry = cell_it->second[slot->index];
            entry.x = new_position.x;
            entry.y = new_position.y;
            entry.z = new_position.z;
            ++maintenance_.moves_in_cell;
            return false;
        }
        // Missing/stale slot or entry: self-heal by inserting the entry.
        Insert(net_id, new_position, entity);
        return false;
    }

    // Cell change: swap-erase from the old cell (fixing the moved entry's
    // slot) and append to the new cell.
    const auto old_it = cells_.find(old_cell_key);
    if (old_it != cells_.end()) {
        auto& cell = old_it->second;
        const auto* slot = entity.is_valid() ? entity.try_get<GridSlot>() : nullptr;
        std::size_t found = cell.size();
        if (slot != nullptr && slot->index < cell.size() &&
            cell[slot->index].net_id == net_id) {
            found = slot->index;
        } else {
            for (std::size_t i = 0; i < cell.size(); ++i) {
                if (cell[i].net_id == net_id) {
                    found = i;
                    break;
                }
            }
        }
        if (found < cell.size()) {
            cell[found] = cell.back();
            cell.pop_back();
            if (found < cell.size() && cell[found].entity.is_valid()) {
                cell[found].entity.set<GridSlot>(
                    {old_cell_key, static_cast<std::uint32_t>(found)});
            }
        }
        if (cell.empty()) {
            cells_.erase(old_it);
        }
    }
    auto& new_cell = cells_[new_cell_key];
    new_cell.push_back(
        GridEntry{net_id, new_position.x, new_position.y, new_position.z, entity});
    if (entity.is_valid()) {
        entity.set<GridSlot>({new_cell_key, static_cast<std::uint32_t>(new_cell.size() - 1)});
    }
    ++maintenance_.moves_cell;
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

bool SpatialGrid::DebugStoredPosition(std::uint32_t net_id, float& x, float& y, float& z) const
{
    for (const auto& [key, cell] : cells_) {
        (void)key;
        for (const auto& entry : cell) {
            if (entry.net_id == net_id) {
                x = entry.x;
                y = entry.y;
                z = entry.z;
                return true;
            }
        }
    }
    return false;
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
