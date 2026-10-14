#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

// Server-side terrain built from a validated world package (MAP-2). Format
// contract: docs/map-data-format.md.
//
// Coordinate frame: +X = east, +Y = north, +Z = up, meters. The world is the
// HALF-OPEN rectangle [MinX, MaxX) x [MinY, MaxY): a point on the east/north
// outer edge, a non-finite point and everything beyond the bounds is outside
// the world. Cell (i, j) covers [origin + i*cell, origin + (i+1)*cell) on each
// axis; height samples sit on the cell corners (cells + 1 per axis).
//
// Storage is per chunk. A chunk keeps the corner samples of all its cells, so
// the sample row/column on a chunk border exists in both neighbours (the
// package validator proves the copies are identical). A query point belongs
// to exactly one cell and therefore to exactly one chunk (half-open), which
// holds all four corners it interpolates: no query ever reads two chunks, and
// the duplicated border samples never mean shared logical ownership.
namespace mx::map {

struct GridGeometry {
    double origin_x = 0.0; // south-west corner of cell (0, 0), world meters
    double origin_y = 0.0;
    double cell_size_m = 1.0;
    std::uint32_t cells_x = 0;     // world size in cells (east)
    std::uint32_t cells_y = 0;     // world size in cells (north)
    std::uint32_t chunk_cells = 0; // nominal chunk pitch in cells
    std::uint32_t chunks_x = 0;    // ceil(cells_x / chunk_cells) -- chunk count
    std::uint32_t chunks_y = 0;

    double MinX() const noexcept
    {
        return origin_x;
    }
    double MinY() const noexcept
    {
        return origin_y;
    }
    double MaxX() const noexcept
    {
        return origin_x + static_cast<double>(cells_x) * cell_size_m;
    }
    double MaxY() const noexcept
    {
        return origin_y + static_cast<double>(cells_y) * cell_size_m;
    }
    // Height samples per axis (cells + 1) -- deliberately not the chunk count.
    std::uint32_t SamplesX() const noexcept
    {
        return cells_x + 1;
    }
    std::uint32_t SamplesY() const noexcept
    {
        return cells_y + 1;
    }
    // Cells in chunk column cx / row cy: the last chunk may be partial.
    std::uint32_t ChunkCellsX(std::uint32_t cx) const noexcept;
    std::uint32_t ChunkCellsY(std::uint32_t cy) const noexcept;
    // Half-open membership; false for non-finite input.
    bool Contains(double x, double y) const noexcept;
};

enum class HeightInterpolation : std::uint8_t {
    Bilinear = 0,
    TriangleMainDiagonal = 1, // canonical h00 (SW) -> h11 (NE), fx == fy
};

// Pure height evaluation shared by package consumers. Corners are canonical
// SW/SE/NW/NE; fx/fy are fractional coordinates in the containing cell.
// Unknown interpolation returns NaN, never a guessed legacy surface.
double InterpolateTerrainHeight(double h00, double h10, double h01, double h11,
                                double fx, double fy, HeightInterpolation interpolation) noexcept;

// Stored height -> meters: meters = offset_m + raw * meters_per_unit.
// Height layer version 1: int16, 0.01 m per unit, offset 0 (historic cm).
// Height layer version 2: explicit sample type, scale and offset (manifest
// heightEncoding) -- the range extension. Height layer version 3 adds the
// explicit triangle-main-diagonal surface contract; v1/v2 remain bilinear.
struct HeightEncoding {
    std::uint32_t layer_version = 1;
    bool int32_samples = false;
    double meters_per_unit = 0.01;
    double offset_m = 0.0;
    HeightInterpolation interpolation = HeightInterpolation::Bilinear;

    double MinMeters() const noexcept;
    double MaxMeters() const noexcept;
};

// Result status of a terrain query. A missing chunk is NOT ground at 0 m,
// not walkable and not "no obstacle": callers must handle every status.
enum class TerrainStatus : std::uint8_t {
    Ok,
    OutsideWorld, // beyond the half-open bounds, or non-finite input
    NotResident,  // inside the world but its chunk is not loaded
    InvalidData,  // chunk present but unusable (never produced by a validated load)
};
const char* ToString(TerrainStatus status) noexcept;

struct HeightSample {
    TerrainStatus status = TerrainStatus::OutsideWorld;
    float meters = 0.0f; // meaningful only when status == Ok

    bool Ok() const noexcept
    {
        return status == TerrainStatus::Ok;
    }
};

struct CellSample {
    static constexpr std::uint16_t kBlocked = 0x0001;
    TerrainStatus status = TerrainStatus::OutsideWorld;
    std::uint16_t attributes = 0; // meaningful only when status == Ok

    bool Ok() const noexcept
    {
        return status == TerrainStatus::Ok;
    }
    // Walkable only with data: a missing or outside cell is never walkable.
    bool Walkable() const noexcept
    {
        return Ok() && (attributes & kBlocked) == 0;
    }
};

struct TerrainChunk {
    std::uint32_t x = 0; // stable chunk key = (x, y) in the chunk grid
    std::uint32_t y = 0;
    std::uint32_t cells_x = 0;
    std::uint32_t cells_y = 0;
    std::vector<std::int16_t> heights16;   // (cells_x+1)*(cells_y+1), row-major, y outer
    std::vector<std::int32_t> heights32;   // used instead of heights16 for int32 encoding
    std::vector<std::uint16_t> attributes; // cells_x*cells_y, row-major
    bool resident = false;                 // loader: decoded and valid

    std::size_t Bytes() const noexcept;
    // Has the height samples and attributes its cell extent requires. A chunk
    // published as INVALID (permanent load failure) has none: every query in
    // it answers InvalidData and nothing may index its (empty) arrays.
    bool HasSamples(bool int32_samples) const noexcept;
};

// Path query (MAP-3 static collision): the cells a straight segment crosses,
// in order, EXCLUDING the start cell (an entity may leave the cell it stands
// in), stopping at the first cell without usable data or blocked.
struct SegmentSample {
    TerrainStatus status = TerrainStatus::Ok; // first non-Ok cell status on the path
    bool blocked = false;                     // a blocked cell (attribute bit 0) on the path
    std::uint32_t chunk = 0xffffffffu;        // chunk of the first failing cell (demand)
    std::uint32_t cells = 0;                  // cells visited after the start cell
    bool Clear() const noexcept
    {
        return status == TerrainStatus::Ok && !blocked;
    }
};

// Published terrain: geometry + encoding + one slot per chunk (MAP-3).
//
// Readers (any thread inside a valid READ WINDOW, see below) see a slot either
// empty (NotResident) or holding a complete, validated, IMMUTABLE chunk --
// never a half-built one: a chunk is published with one release store after
// it is fully decoded. Reads take no lock and no reference count.
//
// Writer: ONE thread (the loader before the runtime exists, afterwards the
// runtime supervisor) publishes / unpublishes / pins. Unpublish (and a
// Publish that replaces a chunk) only clears the slot and hands the ownership
// back; the caller frees the chunk later, when no reader can still hold the
// raw pointer. In the runtime the read window is a zone tick (or the
// supervisor itself) and the grace period is a supervisor quiescent window:
// the supervisor alone dispatches zone ticks (tick flag set before the
// dispatch, cleared with release at the tick's end), so observing every flag
// clear (acquire) proves that every tick which could have loaded the old
// pointer has finished, and no new tick can start before the supervisor
// frees and dispatches again -- a tick dispatched after the unpublish sees
// the cleared slot. The terrain streamer frees retired chunks only there. A
// reader that must keep a chunk beyond its read window (navigation jobs)
// takes a PIN (Owned(), writer thread), which also keeps the chunk from being
// chosen for eviction.
class ServerTerrain {
public:
    static constexpr std::uint32_t kNoChunk = 0xffffffffu;

    ServerTerrain() = default;
    // Eager: `chunks` must be row-major (y outer) and complete for `geometry`;
    // every chunk the loader decoded (`resident`) is published.
    ServerTerrain(GridGeometry geometry, HeightEncoding encoding, std::vector<TerrainChunk> chunks);
    // Streaming skeleton: the geometry and encoding, no chunk published.
    ServerTerrain(GridGeometry geometry, HeightEncoding encoding);
    ServerTerrain(ServerTerrain&& other) noexcept;
    ServerTerrain& operator=(ServerTerrain&& other) noexcept;
    ServerTerrain(const ServerTerrain&) = delete;
    ServerTerrain& operator=(const ServerTerrain&) = delete;
    // Same geometry, the same published chunks (shared, immutable) -- tests
    // and tools. Writer thread only.
    ServerTerrain Clone() const;

    const GridGeometry& Geometry() const noexcept
    {
        return geometry_;
    }
    const HeightEncoding& Encoding() const noexcept
    {
        return encoding_;
    }
    // No geometry (default constructed).
    bool Empty() const noexcept
    {
        return slot_count_ == 0;
    }

    // Evaluates the containing cell with the declared height surface. v1/v2
    // preserve historic bilinear queries; v3 uses the h00-h11 triangles.
    HeightSample Height(double x, double y) const noexcept;
    // The containing cell's attribute bits (bit 0 = blocked).
    CellSample Cell(double x, double y) const noexcept;
    // Corner sample by global sample index (0..cells) -- tools and oracles.
    HeightSample Vertex(std::uint32_t vx, std::uint32_t vy) const noexcept;
    // Every cell a straight segment from (x0,y0) to (x1,y1) crosses (grid
    // traversal; an exact corner crossing visits both side cells, so no
    // diagonal squeeze between two blocked cells), across chunk borders.
    // Endpoint outside the world -> OutsideWorld.
    SegmentSample Segment(double x0, double y0, double x1, double y1) const noexcept;
    // One cell by its global cell index (0..cells-1).
    CellSample CellAt(std::uint32_t cell_x, std::uint32_t cell_y) const noexcept;
    // Global cell index of a world point (world-unit rule, half-open); false
    // outside the world / for non-finite input.
    bool CellIndexOf(double x, double y, std::uint32_t& cell_x, std::uint32_t& cell_y) const noexcept;

    // Chunk key: index = cy * chunks_x + cx (stable for the package).
    std::size_t ChunkCount() const noexcept
    {
        return slot_count_;
    }
    std::uint32_t ChunkIndex(std::uint32_t cx, std::uint32_t cy) const noexcept;
    // Chunk of a world point (the cell's chunk, half-open); kNoChunk outside.
    std::uint32_t ChunkIndexOf(double x, double y) const noexcept;
    // Decoded size of chunk `index` (heights + attributes), from the geometry.
    std::size_t ChunkBytes(std::uint32_t index) const noexcept;

    // Published chunk or null (read window rules above).
    const TerrainChunk* ChunkAt(std::uint32_t cx, std::uint32_t cy) const noexcept;
    const TerrainChunk* Published(std::uint32_t index) const noexcept;
    std::size_t ResidentChunkCount() const noexcept
    {
        return resident_count_.load(std::memory_order_relaxed);
    }
    std::size_t ResidentBytes() const noexcept
    {
        return resident_bytes_.load(std::memory_order_relaxed);
    }

    // ---- writer thread only ----
    // Binds the writer role to the calling thread (the runtime supervisor
    // while it runs; after it stopped, the thread that stopped it). Debug
    // builds assert every writer-only call below against it; unbound = the
    // loader phase, single-threaded by construction.
    void BindWriterThread() noexcept;
    // Publishes `chunk` in slot `index` and returns the chunk it REPLACED
    // (null for an empty slot): readers may still hold the replaced one, so
    // the caller frees it only after the read grace period, like Unpublish.
    [[nodiscard]] std::shared_ptr<const TerrainChunk> Publish(std::uint32_t index,
                                                              std::shared_ptr<const TerrainChunk> chunk);
    // Clears the slot; returns the ownership (null if it was empty). The
    // caller frees it only after the read grace period.
    std::shared_ptr<const TerrainChunk> Unpublish(std::uint32_t index);
    // Pin: shared ownership of a published chunk (null if none). Pins are
    // taken ONLY on the writer thread -- the thread that also decides
    // eviction -- so "not pinned" cannot change between the eviction check
    // and the unpublish.
    std::shared_ptr<const TerrainChunk> Owned(std::uint32_t index) const;
    // Pins currently held on the published chunk of `index` (owners besides
    // the terrain itself); 0 for an empty slot.
    long PinCount(std::uint32_t index) const noexcept;
    // Shared border samples of `chunk` (index `index`) that differ from its
    // PUBLISHED neighbours (all four sides) -- the seam check for a chunk
    // loaded after startup.
    std::uint64_t SeamMismatches(std::uint32_t index, const TerrainChunk& chunk) const noexcept;
    // Test seam: unpublish and free at once (single-threaded tests only).
    void EvictChunkForTest(std::uint32_t cx, std::uint32_t cy);

private:
    void AssertWriter() const noexcept;
    // Cell of an inside point (the caller checked Contains).
    void CellOf(double x, double y, std::uint32_t& cell_x, std::uint32_t& cell_y, double& fx,
                double& fy) const noexcept;
    double RawMeters(const TerrainChunk& chunk, std::uint32_t lx, std::uint32_t ly) const noexcept;

    GridGeometry geometry_;
    HeightEncoding encoding_;
    std::size_t slot_count_ = 0;
    std::unique_ptr<std::atomic<const TerrainChunk*>[]> published_;
    std::vector<std::shared_ptr<const TerrainChunk>> owned_; // writer side
    std::atomic<std::size_t> resident_count_{0};
    std::atomic<std::size_t> resident_bytes_{0};
    std::atomic<std::thread::id> writer_{};
};

} // namespace mx::map
