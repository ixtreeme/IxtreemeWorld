#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "map/ServerTerrain.h"
#include "map/ServerWater.h"

#include "../activity/ActivityTypes.h"

// Ground queries for gameplay systems (movement, spawn, warp, migration) in
// WORLD coordinates -- never by zone id, so a query answers the same whatever
// the server partition looks like (MAP-2).
//
// Every query returns a status. The world is the half-open rectangle
// Bounds() = [min, max): outside it (or for non-finite input) the answer is
// OutsideWorld, inside it but without data NotResident -- never a silent 0 m
// height or "walkable". Callers own their out-of-world rule (see
// docs/map-data-format.md "Futásidejű szabályok"); there is no generic clamp.
namespace gs::game {

class TerrainStreamer;

// Movement collision rules on top of the blocking grid (MAP-3). Both off by
// default: they change gameplay-visible movement, so a deployment opts in.
struct MovementRules {
    float max_slope = 0.0f;          // max rise per horizontal meter of a step; 0 = off
    float max_water_depth_m = -1.0f; // steps into deeper water are refused; < 0 = off
};

// Outcome of one movement step (the straight path from -> to).
enum class StepResult : std::uint8_t {
    Clear,
    Blocked,      // a blocked cell on the path (attribute bit 0)
    TooSteep,     // uphill rise per meter above MovementRules::max_slope
    DeepWater,    // destination water deeper than MovementRules::max_water_depth_m
    OutsideWorld, // the destination is outside the half-open world
    NotResident,  // a cell on the path (or its water ground) is not loaded yet
    InvalidData,
};
const char* ToString(StepResult result) noexcept;

struct StepCheck {
    StepResult result = StepResult::Clear;
    std::uint32_t chunk = mx::map::ServerTerrain::kNoChunk; // NotResident: the chunk to demand
    bool Clear() const noexcept
    {
        return result == StepResult::Clear;
    }
};

class TerrainService {
public:
    // No world yet: every query is OutsideWorld.
    TerrainService() = default;
    // File-backed terrain from a validated world package. Throws
    // std::invalid_argument for an empty terrain.
    explicit TerrainService(mx::map::ServerTerrain terrain);
    // Explicit synthetic mode: flat 0 m, fully walkable ground on `bounds`.
    static TerrainService Flat(const WorldBounds& bounds);
    // f32 runtime bounds of a terrain, rounded inward (see the constructor).
    static WorldBounds BoundsOf(const mx::map::ServerTerrain& terrain) noexcept;

    const WorldBounds& Bounds() const noexcept
    {
        return bounds_;
    }
    bool Contains(float world_x, float world_y) const noexcept;
    bool IsFlat() const noexcept
    {
        return flat_;
    }

    mx::map::HeightSample Height(float world_x, float world_y) const noexcept;
    mx::map::CellSample Cell(float world_x, float world_y) const noexcept;
    // Walkable ground with data (Cell(...).Walkable()).
    bool IsWalkable(float world_x, float world_y) const noexcept
    {
        return Cell(world_x, world_y).Walkable();
    }
    // One movement step: every cell on the straight path (across chunk
    // borders, not only the destination sample) + the configured slope /
    // water rules. The start cell itself is not checked.
    StepCheck CheckStep(float from_x, float from_y, float to_x, float to_y) const noexcept;
    // Declared water at a point (UnsupportedLayer when the package declares
    // no water capability -- never silently "land").
    mx::map::WaterSample Water(float world_x, float world_y) const noexcept;
    void SetWater(mx::map::ServerWater water)
    {
        water_ = std::move(water);
    }
    const mx::map::ServerWater& WaterData() const noexcept
    {
        return water_;
    }
    void SetMovementRules(const MovementRules& rules) noexcept
    {
        rules_ = rules;
    }
    const MovementRules& Rules() const noexcept
    {
        return rules_;
    }

    // Bytes of resident terrain data (0 for the flat provider).
    std::size_t ResidentBytes() const noexcept;
    // The file-backed terrain (null for flat / empty).
    const mx::map::ServerTerrain* Terrain() const noexcept
    {
        return flat_ || terrain_.Empty() ? nullptr : &terrain_;
    }

    // ---- MAP-3 streaming ----
    // Chunk key of a world point (kNoChunk outside / flat).
    std::uint32_t ChunkIndexOf(float world_x, float world_y) const noexcept
    {
        return flat_ ? mx::map::ServerTerrain::kNoChunk : terrain_.ChunkIndexOf(world_x, world_y);
    }
    // True when chunks load on demand: consumers publish their demand.
    bool Streaming() const noexcept
    {
        return streamer_ != nullptr;
    }
    // Prefetch window for movement demand (0 without streaming).
    float LookaheadSeconds() const noexcept;
    // Runtime setup only (supervisor, before Start): the streamer mutates the
    // published slots of THIS terrain.
    mx::map::ServerTerrain* MutableTerrain() noexcept
    {
        return flat_ || terrain_.Empty() ? nullptr : &terrain_;
    }
    void AttachStreamer(const TerrainStreamer* streamer) noexcept
    {
        streamer_ = streamer;
    }

private:
    mx::map::ServerTerrain terrain_;
    WorldBounds bounds_{}; // empty: nothing is inside
    bool flat_ = false;
    const TerrainStreamer* streamer_ = nullptr;
    mx::map::ServerWater water_;
    MovementRules rules_;
};

} // namespace gs::game
