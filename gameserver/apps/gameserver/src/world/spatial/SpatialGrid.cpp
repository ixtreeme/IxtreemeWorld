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
    for (const auto& [key, column] : cells_) {
        (void)key;
        for (const auto& bucket : column) {
            total += bucket.entries.size();
        }
    }
    return total;
}

void SpatialGrid::Append(std::int64_t cell_key, std::uint32_t volume_id, const GridEntry& entry)
{
    auto& column = cells_[cell_key];
    Bucket* bucket = FindBucket(column, volume_id);
    if (bucket == nullptr) {
        column.push_back(Bucket{volume_id, {}});
        bucket = &column.back();
    }
    bucket->entries.push_back(entry);
    bucket->entries.back().volume_id = volume_id;
    if (flecs::entity owner = entry.entity; owner.is_valid()) {
        owner.set<GridSlot>(
            {cell_key, volume_id, static_cast<std::uint32_t>(bucket->entries.size() - 1)});
    }
}

void SpatialGrid::EraseAt(std::unordered_map<std::int64_t, Column>::iterator column_it,
                          std::size_t bucket_index,
                          std::size_t index)
{
    auto& column = column_it->second;
    auto& entries = column[bucket_index].entries;
    const std::uint32_t volume_id = column[bucket_index].volume_id;
    // Swap-erase keeps the GridSlot indices of the remaining entries valid
    // with one fix-up write.
    entries[index] = entries.back();
    entries.pop_back();
    if (index < entries.size() && entries[index].entity.is_valid()) {
        entries[index].entity.set<GridSlot>(
            {column_it->first, volume_id, static_cast<std::uint32_t>(index)});
    }
    if (entries.empty()) {
        // Bucket order inside a column carries no meaning (slots name the
        // volume, not the bucket position).
        if (bucket_index + 1 != column.size()) {
            column[bucket_index] = std::move(column.back());
        }
        column.pop_back();
        if (column.empty()) {
            cells_.erase(column_it);
        }
    }
}

void SpatialGrid::Insert(std::uint32_t net_id, const Position& position, flecs::entity entity,
                         std::uint32_t volume_id)
{
    const std::int64_t cell_key =
        SpatialCellKey(SpatialCellCoord(position.x), SpatialCellCoord(position.y));
    Append(cell_key, volume_id,
           GridEntry{net_id, position.x, position.y, position.z, volume_id, entity});
    ++maintenance_.inserts;
}

void SpatialGrid::Remove(std::uint32_t net_id, const Position& position)
{
    const std::int64_t cell_key =
        SpatialCellKey(SpatialCellCoord(position.x), SpatialCellCoord(position.y));
    const auto column_it = cells_.find(cell_key);
    if (column_it == cells_.end()) {
        return;
    }
    auto& column = column_it->second;
    for (std::size_t b = 0; b < column.size(); ++b) {
        auto& entries = column[b].entries;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].net_id == net_id) {
                EraseAt(column_it, b, i);
                ++maintenance_.removes;
                return;
            }
        }
    }
}

bool SpatialGrid::Move(flecs::entity entity,
                       std::uint32_t net_id,
                       std::int64_t old_cell_key,
                       const Position& new_position,
                       std::uint32_t new_volume_id)
{
    const std::int64_t new_cell_key =
        SpatialCellKey(SpatialCellCoord(new_position.x), SpatialCellCoord(new_position.y));
    const auto* slot = entity.is_valid() ? entity.try_get<GridSlot>() : nullptr;
    const auto old_it = cells_.find(old_cell_key);

    // Locate the current entry: the slot first (O(1)), else a column scan.
    std::size_t bucket_index = 0;
    std::size_t entry_index = 0;
    bool found = false;
    if (old_it != cells_.end()) {
        auto& column = old_it->second;
        if (slot != nullptr && slot->cell_key == old_cell_key) {
            for (std::size_t b = 0; b < column.size() && !found; ++b) {
                if (column[b].volume_id == slot->volume_id && slot->index < column[b].entries.size() &&
                    column[b].entries[slot->index].net_id == net_id) {
                    bucket_index = b;
                    entry_index = slot->index;
                    found = true;
                }
            }
        }
        for (std::size_t b = 0; b < column.size() && !found; ++b) {
            for (std::size_t i = 0; i < column[b].entries.size(); ++i) {
                if (column[b].entries[i].net_id == net_id) {
                    bucket_index = b;
                    entry_index = i;
                    found = true;
                    break;
                }
            }
        }
    }

    if (found && new_cell_key == old_cell_key &&
        old_it->second[bucket_index].volume_id == new_volume_id) {
        // In-bucket move: one O(1) write. The stored position is what the AOI
        // radius scan reads.
        GridEntry& entry = old_it->second[bucket_index].entries[entry_index];
        entry.x = new_position.x;
        entry.y = new_position.y;
        entry.z = new_position.z;
        // Self-heal the handle and slot too: the AOI passes it on as a hint.
        entry.entity = entity;
        if (entity.is_valid() && (slot == nullptr || slot->index != entry_index ||
                                  slot->volume_id != new_volume_id || slot->cell_key != old_cell_key)) {
            entity.set<GridSlot>({old_cell_key, new_volume_id, static_cast<std::uint32_t>(entry_index)});
        }
        ++maintenance_.moves_in_cell;
        return false;
    }
    if (!found) {
        // Missing/stale slot or entry: self-heal by inserting the entry.
        Insert(net_id, new_position, entity, new_volume_id);
        return false;
    }

    // Cell or volume change: swap-erase from the old bucket (fixing the moved
    // entry's slot) and append to the new bucket.
    EraseAt(old_it, bucket_index, entry_index);
    Append(new_cell_key, new_volume_id,
           GridEntry{net_id, new_position.x, new_position.y, new_position.z, new_volume_id, entity});
    ++maintenance_.moves_cell;
    return true;
}

bool SpatialGrid::Contains(std::uint32_t net_id, const Position& position) const
{
    const auto cell_it =
        cells_.find(SpatialCellKey(SpatialCellCoord(position.x), SpatialCellCoord(position.y)));
    if (cell_it == cells_.end()) {
        return false;
    }
    for (const auto& bucket : cell_it->second) {
        if (std::any_of(bucket.entries.begin(), bucket.entries.end(),
                        [net_id](const GridEntry& entry) { return entry.net_id == net_id; })) {
            return true;
        }
    }
    return false;
}

bool SpatialGrid::ContainsIn(std::uint32_t net_id, const Position& position,
                             std::uint32_t volume_id) const
{
    const auto cell_it =
        cells_.find(SpatialCellKey(SpatialCellCoord(position.x), SpatialCellCoord(position.y)));
    if (cell_it == cells_.end()) {
        return false;
    }
    for (const auto& bucket : cell_it->second) {
        if (bucket.volume_id != volume_id) {
            continue;
        }
        return std::any_of(bucket.entries.begin(), bucket.entries.end(),
                           [net_id](const GridEntry& entry) { return entry.net_id == net_id; });
    }
    return false;
}

bool SpatialGrid::DebugStoredPosition(std::uint32_t net_id, float& x, float& y, float& z) const
{
    for (const auto& [key, column] : cells_) {
        (void)key;
        for (const auto& bucket : column) {
            for (const auto& entry : bucket.entries) {
                if (entry.net_id == net_id) {
                    x = entry.x;
                    y = entry.y;
                    z = entry.z;
                    return true;
                }
            }
        }
    }
    return false;
}

std::vector<std::pair<std::uint32_t, mx::map::LayeredSpatialCellKey>> SpatialGrid::SnapshotEntries() const
{
    std::vector<std::pair<std::uint32_t, mx::map::LayeredSpatialCellKey>> entries;
    entries.reserve(Size());
    for (const auto& [key, column] : cells_) {
        const auto packed = static_cast<std::uint64_t>(key);
        const auto cell_x = static_cast<std::int32_t>(static_cast<std::uint32_t>(packed >> 32));
        const auto cell_y = static_cast<std::int32_t>(static_cast<std::uint32_t>(packed & 0xffffffffu));
        for (const auto& bucket : column) {
            for (const auto& entry : bucket.entries) {
                entries.emplace_back(entry.net_id,
                                     mx::map::LayeredSpatialCellKey{bucket.volume_id, cell_x, cell_y});
            }
        }
    }
    return entries;
}

} // namespace gs::game
