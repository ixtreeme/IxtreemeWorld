#include "TerrainService.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

#include "common/Logging.h"

namespace gs::game {

TerrainService::TerrainService(mx::map::HeightField field)
    : field_(std::move(field))
{
}

TerrainService TerrainService::LoadFromMapRoot(const std::string& map_root)
{
    const std::filesystem::path root(map_root);
    auto field = mx::map::LoadHeightField(
        [&root](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
            std::filesystem::path normalized(path);
            std::ifstream file(root / normalized.relative_path(), std::ios::binary | std::ios::ate);
            if (!file) {
                return std::nullopt;
            }
            const auto end = file.tellg();
            if (end < 0) {
                return std::nullopt;
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
            file.seekg(0);
            if (!bytes.empty()) {
                file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!file) {
                    return std::nullopt;
                }
            }
            return bytes;
        },
        ".");
    if (field) {
        LOG_INFO("Game sim loaded map '{}' cells={} cellSize={}m from {}",
                 field->manifest.world_id,
                 field->manifest.world_size_cells,
                 field->manifest.cell_size_meters,
                 map_root);
        return TerrainService(std::move(*field));
    }

    LOG_WARN("Game sim map load failed from {}; using flat 1000m fallback", map_root);
    return TerrainService{};
}

float TerrainService::SampleGroundHeight(float world_x, float world_y) const noexcept
{
    if (!field_.IsValid()) {
        return 0.0f;
    }
    return field_.SampleHeightMeters(world_x, world_y);
}

bool TerrainService::IsWalkable(float world_x, float world_y) const noexcept
{
    if (!field_.IsValid()) {
        return true;
    }
    return field_.IsWalkable(world_x, world_y);
}

float TerrainService::WorldExtentMeters() const noexcept
{
    if (!field_.IsValid()) {
        return 1000.0f;
    }
    return static_cast<float>(field_.manifest.world_size_cells) * field_.manifest.cell_size_meters;
}

} // namespace gs::game
