#include "map/ServerTerrain.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace mx::map {

double InterpolateTerrainHeight(double h00, double h10, double h01, double h11,
                                double fx, double fy, HeightInterpolation interpolation) noexcept
{
    switch (interpolation) {
    case HeightInterpolation::Bilinear: {
        const double south = h00 + (h10 - h00) * fx;
        const double north = h01 + (h11 - h01) * fx;
        return south + (north - south) * fy;
    }
    case HeightInterpolation::TriangleMainDiagonal:
        // h00/h10/h11 for fx >= fy, otherwise h00/h11/h01. Both
        // triangles agree exactly on the shared diagonal and outer edges.
        return fx >= fy ? h00 + (h10 - h00) * fx + (h11 - h10) * fy
                        : h00 + (h11 - h01) * fx + (h01 - h00) * fy;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

std::uint32_t GridGeometry::ChunkCellsX(std::uint32_t cx) const noexcept
{
    const std::uint64_t start = static_cast<std::uint64_t>(cx) * chunk_cells;
    return start >= cells_x ? 0u : static_cast<std::uint32_t>(std::min<std::uint64_t>(chunk_cells, cells_x - start));
}

std::uint32_t GridGeometry::ChunkCellsY(std::uint32_t cy) const noexcept
{
    const std::uint64_t start = static_cast<std::uint64_t>(cy) * chunk_cells;
    return start >= cells_y ? 0u : static_cast<std::uint32_t>(std::min<std::uint64_t>(chunk_cells, cells_y - start));
}

bool GridGeometry::Contains(double x, double y) const noexcept
{
    // NaN fails every comparison below, so it is outside by construction.
    return x >= MinX() && x < MaxX() && y >= MinY() && y < MaxY();
}

double HeightEncoding::MinMeters() const noexcept
{
    const double raw = int32_samples ? static_cast<double>(std::numeric_limits<std::int32_t>::min())
                                     : static_cast<double>(std::numeric_limits<std::int16_t>::min());
    return offset_m + raw * meters_per_unit;
}

double HeightEncoding::MaxMeters() const noexcept
{
    const double raw = int32_samples ? static_cast<double>(std::numeric_limits<std::int32_t>::max())
                                     : static_cast<double>(std::numeric_limits<std::int16_t>::max());
    return offset_m + raw * meters_per_unit;
}

const char* ToString(TerrainStatus status) noexcept
{
    switch (status) {
    case TerrainStatus::Ok:
        return "ok";
    case TerrainStatus::OutsideWorld:
        return "outside-world";
    case TerrainStatus::NotResident:
        return "not-resident";
    case TerrainStatus::InvalidData:
        return "invalid-data";
    }
    return "?";
}

std::size_t TerrainChunk::Bytes() const noexcept
{
    return heights16.size() * sizeof(std::int16_t) + heights32.size() * sizeof(std::int32_t) +
           attributes.size() * sizeof(std::uint16_t);
}

bool TerrainChunk::HasSamples(bool int32_samples) const noexcept
{
    const std::size_t samples = static_cast<std::size_t>(cells_x + 1) * (cells_y + 1);
    return (int32_samples ? heights32.size() : heights16.size()) == samples &&
           attributes.size() == static_cast<std::size_t>(cells_x) * cells_y;
}

ServerTerrain::ServerTerrain(GridGeometry geometry, HeightEncoding encoding)
    : geometry_(geometry)
    , encoding_(encoding)
    , slot_count_(static_cast<std::size_t>(geometry.chunks_x) * geometry.chunks_y)
    , published_(std::make_unique<std::atomic<const TerrainChunk*>[]>(slot_count_))
    , owned_(slot_count_)
{
    for (std::size_t i = 0; i < slot_count_; ++i) {
        published_[i].store(nullptr, std::memory_order_relaxed);
    }
}

ServerTerrain::ServerTerrain(GridGeometry geometry, HeightEncoding encoding, std::vector<TerrainChunk> chunks)
    : ServerTerrain(geometry, encoding)
{
    for (std::size_t i = 0; i < chunks.size() && i < slot_count_; ++i) {
        if (chunks[i].resident) {
            (void)Publish(static_cast<std::uint32_t>(i), std::make_shared<TerrainChunk>(std::move(chunks[i])));
        }
    }
}

ServerTerrain::ServerTerrain(ServerTerrain&& other) noexcept
    : geometry_(other.geometry_)
    , encoding_(other.encoding_)
    , slot_count_(other.slot_count_)
    , published_(std::move(other.published_))
    , owned_(std::move(other.owned_))
    , resident_count_(other.resident_count_.load(std::memory_order_relaxed))
    , resident_bytes_(other.resident_bytes_.load(std::memory_order_relaxed))
    , writer_(other.writer_.load(std::memory_order_relaxed))
{
    other.slot_count_ = 0;
    other.resident_count_.store(0, std::memory_order_relaxed);
    other.resident_bytes_.store(0, std::memory_order_relaxed);
}

ServerTerrain& ServerTerrain::operator=(ServerTerrain&& other) noexcept
{
    if (this != &other) {
        geometry_ = other.geometry_;
        encoding_ = other.encoding_;
        slot_count_ = other.slot_count_;
        published_ = std::move(other.published_);
        owned_ = std::move(other.owned_);
        resident_count_.store(other.resident_count_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        resident_bytes_.store(other.resident_bytes_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        writer_.store(other.writer_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        other.slot_count_ = 0;
        other.resident_count_.store(0, std::memory_order_relaxed);
        other.resident_bytes_.store(0, std::memory_order_relaxed);
    }
    return *this;
}

ServerTerrain ServerTerrain::Clone() const
{
    AssertWriter();
    ServerTerrain copy(geometry_, encoding_);
    for (std::size_t i = 0; i < slot_count_; ++i) {
        if (owned_[i]) {
            (void)copy.Publish(static_cast<std::uint32_t>(i), owned_[i]);
        }
    }
    return copy;
}

void ServerTerrain::BindWriterThread() noexcept
{
    writer_.store(std::this_thread::get_id(), std::memory_order_relaxed);
}

void ServerTerrain::AssertWriter() const noexcept
{
    [[maybe_unused]] const std::thread::id writer = writer_.load(std::memory_order_relaxed);
    assert((writer == std::thread::id{} || writer == std::this_thread::get_id()) &&
           "ServerTerrain writer-only call from a non-writer thread");
}

std::uint32_t ServerTerrain::ChunkIndex(std::uint32_t cx, std::uint32_t cy) const noexcept
{
    if (cx >= geometry_.chunks_x || cy >= geometry_.chunks_y) {
        return kNoChunk;
    }
    return cy * geometry_.chunks_x + cx;
}

std::uint32_t ServerTerrain::ChunkIndexOf(double x, double y) const noexcept
{
    if (slot_count_ == 0 || !geometry_.Contains(x, y)) {
        return kNoChunk;
    }
    std::uint32_t cell_x = 0, cell_y = 0;
    double fx = 0.0, fy = 0.0;
    CellOf(x, y, cell_x, cell_y, fx, fy);
    return ChunkIndex(cell_x / geometry_.chunk_cells, cell_y / geometry_.chunk_cells);
}

std::size_t ServerTerrain::ChunkBytes(std::uint32_t index) const noexcept
{
    if (index >= slot_count_) {
        return 0;
    }
    const std::size_t cx = geometry_.ChunkCellsX(index % geometry_.chunks_x);
    const std::size_t cy = geometry_.ChunkCellsY(index / geometry_.chunks_x);
    return (cx + 1) * (cy + 1) * (encoding_.int32_samples ? 4u : 2u) + cx * cy * 2u;
}

const TerrainChunk* ServerTerrain::Published(std::uint32_t index) const noexcept
{
    return index < slot_count_ ? published_[index].load(std::memory_order_acquire) : nullptr;
}

const TerrainChunk* ServerTerrain::ChunkAt(std::uint32_t cx, std::uint32_t cy) const noexcept
{
    return Published(ChunkIndex(cx, cy));
}

std::shared_ptr<const TerrainChunk> ServerTerrain::Publish(std::uint32_t index,
                                                           std::shared_ptr<const TerrainChunk> chunk)
{
    AssertWriter();
    if (index >= slot_count_ || !chunk) {
        return nullptr;
    }
    std::shared_ptr<const TerrainChunk> replaced = std::move(owned_[index]);
    if (replaced) {
        resident_bytes_.fetch_sub(replaced->Bytes(), std::memory_order_relaxed);
        resident_count_.fetch_sub(1, std::memory_order_relaxed);
    }
    resident_bytes_.fetch_add(chunk->Bytes(), std::memory_order_relaxed);
    resident_count_.fetch_add(1, std::memory_order_relaxed);
    const TerrainChunk* raw = chunk.get();
    owned_[index] = std::move(chunk);
    // Release: a reader that sees the pointer sees the fully built chunk.
    published_[index].store(raw, std::memory_order_release);
    return replaced; // the caller frees it after the read grace period
}

std::shared_ptr<const TerrainChunk> ServerTerrain::Unpublish(std::uint32_t index)
{
    AssertWriter();
    if (index >= slot_count_ || !owned_[index]) {
        return nullptr;
    }
    published_[index].store(nullptr, std::memory_order_release);
    resident_bytes_.fetch_sub(owned_[index]->Bytes(), std::memory_order_relaxed);
    resident_count_.fetch_sub(1, std::memory_order_relaxed);
    return std::move(owned_[index]);
}

std::shared_ptr<const TerrainChunk> ServerTerrain::Owned(std::uint32_t index) const
{
    AssertWriter();
    return index < slot_count_ ? owned_[index] : nullptr;
}

long ServerTerrain::PinCount(std::uint32_t index) const noexcept
{
    AssertWriter();
    // Pins are created and released on the writer thread only, so this count
    // is exact here (use_count's relaxed read is not racing anything).
    return index < slot_count_ && owned_[index] ? owned_[index].use_count() - 1 : 0;
}

std::uint64_t ServerTerrain::SeamMismatches(std::uint32_t index, const TerrainChunk& c) const noexcept
{
    if (index >= slot_count_) {
        return 0;
    }
    auto raw = [this](const TerrainChunk& t, std::uint32_t lx, std::uint32_t ly) -> std::int64_t {
        const std::size_t i = static_cast<std::size_t>(ly) * (t.cells_x + 1) + lx;
        return encoding_.int32_samples ? t.heights32[i] : t.heights16[i];
    };
    // A neighbour published as INVALID has no samples: nothing to compare
    // (and nothing to index).
    const bool i32 = encoding_.int32_samples;
    auto neighbour = [&](bool exists, std::uint32_t at) -> const TerrainChunk* {
        const TerrainChunk* n = exists ? Published(at) : nullptr;
        return n != nullptr && n->HasSamples(i32) ? n : nullptr;
    };
    if (!c.HasSamples(i32)) {
        return 0;
    }
    std::uint64_t mismatches = 0;
    if (const TerrainChunk* w = neighbour(c.x > 0, index - 1)) {
        for (std::uint32_t ly = 0; ly <= c.cells_y; ++ly) {
            mismatches += raw(c, 0, ly) != raw(*w, w->cells_x, ly) ? 1u : 0u;
        }
    }
    if (const TerrainChunk* e = neighbour(c.x + 1 < geometry_.chunks_x, index + 1)) {
        for (std::uint32_t ly = 0; ly <= c.cells_y; ++ly) {
            mismatches += raw(c, c.cells_x, ly) != raw(*e, 0, ly) ? 1u : 0u;
        }
    }
    if (const TerrainChunk* s = neighbour(c.y > 0, index - geometry_.chunks_x)) {
        for (std::uint32_t lx = 0; lx <= c.cells_x; ++lx) {
            mismatches += raw(c, lx, 0) != raw(*s, lx, s->cells_y) ? 1u : 0u;
        }
    }
    if (const TerrainChunk* n = neighbour(c.y + 1 < geometry_.chunks_y, index + geometry_.chunks_x)) {
        for (std::uint32_t lx = 0; lx <= c.cells_x; ++lx) {
            mismatches += raw(c, lx, c.cells_y) != raw(*n, lx, 0) ? 1u : 0u;
        }
    }
    return mismatches;
}

void ServerTerrain::EvictChunkForTest(std::uint32_t cx, std::uint32_t cy)
{
    (void)Unpublish(ChunkIndex(cx, cy)); // freed here: single-threaded tests only
}

void ServerTerrain::CellOf(double x, double y, std::uint32_t& cell_x, std::uint32_t& cell_y, double& fx,
                           double& fy) const noexcept
{
    // The point is inside (half-open, checked in world units by the caller).
    // The division only estimates the cell: a point within an ulp of a cell
    // edge can round onto the wrong side of it, so the cell is then decided
    // in world units by the documented rule [origin + i*cell, origin +
    // (i+1)*cell) -- a point just below a chunk seam must stay in the western
    // / southern chunk. The min() absorbs the outer edge.
    auto axis = [](double v, double origin, double cell, std::uint32_t cells, std::uint32_t& index, double& frac) {
        const double g = (v - origin) / cell;
        double i = std::floor(g);
        if (i > 0.0 && v < origin + i * cell) {
            i -= 1.0;
        } else if (v >= origin + (i + 1.0) * cell) {
            i += 1.0;
        }
        index = std::min(static_cast<std::uint32_t>(std::max(0.0, i)), cells - 1);
        frac = std::clamp(g - static_cast<double>(index), 0.0, 1.0);
    };
    axis(x, geometry_.origin_x, geometry_.cell_size_m, geometry_.cells_x, cell_x, fx);
    axis(y, geometry_.origin_y, geometry_.cell_size_m, geometry_.cells_y, cell_y, fy);
}

double ServerTerrain::RawMeters(const TerrainChunk& chunk, std::uint32_t lx, std::uint32_t ly) const noexcept
{
    const std::size_t index = static_cast<std::size_t>(ly) * (chunk.cells_x + 1) + lx;
    const double raw = encoding_.int32_samples ? static_cast<double>(chunk.heights32[index])
                                               : static_cast<double>(chunk.heights16[index]);
    return encoding_.offset_m + raw * encoding_.meters_per_unit;
}

HeightSample ServerTerrain::Height(double x, double y) const noexcept
{
    if (slot_count_ == 0 || !geometry_.Contains(x, y)) {
        return {TerrainStatus::OutsideWorld, 0.0f};
    }
    std::uint32_t cell_x = 0, cell_y = 0;
    double fx = 0.0, fy = 0.0;
    CellOf(x, y, cell_x, cell_y, fx, fy);
    const std::uint32_t cx = cell_x / geometry_.chunk_cells;
    const std::uint32_t cy = cell_y / geometry_.chunk_cells;
    const TerrainChunk* chunk = ChunkAt(cx, cy);
    if (chunk == nullptr) {
        return {TerrainStatus::NotResident, 0.0f};
    }
    const std::uint32_t lx = cell_x - cx * geometry_.chunk_cells;
    const std::uint32_t ly = cell_y - cy * geometry_.chunk_cells;
    const std::size_t samples = static_cast<std::size_t>(chunk->cells_x + 1) * (chunk->cells_y + 1);
    if (lx >= chunk->cells_x || ly >= chunk->cells_y ||
        (encoding_.int32_samples ? chunk->heights32.size() : chunk->heights16.size()) != samples) {
        return {TerrainStatus::InvalidData, 0.0f};
    }
    const double h00 = RawMeters(*chunk, lx, ly);
    const double h10 = RawMeters(*chunk, lx + 1, ly);
    const double h01 = RawMeters(*chunk, lx, ly + 1);
    const double h11 = RawMeters(*chunk, lx + 1, ly + 1);
    const double height = InterpolateTerrainHeight(h00, h10, h01, h11, fx, fy, encoding_.interpolation);
    if (!std::isfinite(height)) {
        return {TerrainStatus::InvalidData, 0.0f};
    }
    return {TerrainStatus::Ok, static_cast<float>(height)};
}

CellSample ServerTerrain::Cell(double x, double y) const noexcept
{
    if (slot_count_ == 0 || !geometry_.Contains(x, y)) {
        return {TerrainStatus::OutsideWorld, 0};
    }
    std::uint32_t cell_x = 0, cell_y = 0;
    double fx = 0.0, fy = 0.0;
    CellOf(x, y, cell_x, cell_y, fx, fy);
    const std::uint32_t cx = cell_x / geometry_.chunk_cells;
    const std::uint32_t cy = cell_y / geometry_.chunk_cells;
    const TerrainChunk* chunk = ChunkAt(cx, cy);
    if (chunk == nullptr) {
        return {TerrainStatus::NotResident, 0};
    }
    const std::uint32_t lx = cell_x - cx * geometry_.chunk_cells;
    const std::uint32_t ly = cell_y - cy * geometry_.chunk_cells;
    const std::size_t index = static_cast<std::size_t>(ly) * chunk->cells_x + lx;
    if (lx >= chunk->cells_x || ly >= chunk->cells_y || index >= chunk->attributes.size()) {
        return {TerrainStatus::InvalidData, 0};
    }
    return {TerrainStatus::Ok, chunk->attributes[index]};
}

bool ServerTerrain::CellIndexOf(double x, double y, std::uint32_t& cell_x, std::uint32_t& cell_y) const noexcept
{
    if (slot_count_ == 0 || !geometry_.Contains(x, y)) {
        return false;
    }
    double fx = 0.0, fy = 0.0;
    CellOf(x, y, cell_x, cell_y, fx, fy);
    return true;
}

CellSample ServerTerrain::CellAt(std::uint32_t cell_x, std::uint32_t cell_y) const noexcept
{
    if (slot_count_ == 0 || cell_x >= geometry_.cells_x || cell_y >= geometry_.cells_y) {
        return {TerrainStatus::OutsideWorld, 0};
    }
    const std::uint32_t cx = cell_x / geometry_.chunk_cells;
    const std::uint32_t cy = cell_y / geometry_.chunk_cells;
    const TerrainChunk* chunk = ChunkAt(cx, cy);
    if (chunk == nullptr) {
        return {TerrainStatus::NotResident, 0};
    }
    const std::uint32_t lx = cell_x - cx * geometry_.chunk_cells;
    const std::uint32_t ly = cell_y - cy * geometry_.chunk_cells;
    const std::size_t index = static_cast<std::size_t>(ly) * chunk->cells_x + lx;
    if (lx >= chunk->cells_x || ly >= chunk->cells_y || index >= chunk->attributes.size()) {
        return {TerrainStatus::InvalidData, 0};
    }
    return {TerrainStatus::Ok, chunk->attributes[index]};
}

SegmentSample ServerTerrain::Segment(double x0, double y0, double x1, double y1) const noexcept
{
    SegmentSample out;
    if (slot_count_ == 0 || !geometry_.Contains(x0, y0) || !geometry_.Contains(x1, y1)) {
        out.status = TerrainStatus::OutsideWorld;
        return out;
    }
    std::uint32_t cx = 0, cy = 0, ex = 0, ey = 0;
    double fx = 0.0, fy = 0.0;
    CellOf(x0, y0, cx, cy, fx, fy);
    CellOf(x1, y1, ex, ey, fx, fy);
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const int step_x = dx > 0.0 ? 1 : (dx < 0.0 ? -1 : 0);
    const int step_y = dy > 0.0 ? 1 : (dy < 0.0 ? -1 : 0);
    const double cell = geometry_.cell_size_m;
    const double inf = std::numeric_limits<double>::infinity();
    // Ray parameter t in [0, 1] at the next vertical / horizontal cell border.
    auto next_border = [cell](double origin, std::uint32_t c, int step) {
        return origin + (static_cast<double>(c) + (step > 0 ? 1.0 : 0.0)) * cell;
    };
    double t_max_x = step_x != 0 ? (next_border(geometry_.origin_x, cx, step_x) - x0) / dx : inf;
    double t_max_y = step_y != 0 ? (next_border(geometry_.origin_y, cy, step_y) - y0) / dy : inf;
    const double t_delta_x = step_x != 0 ? cell / std::abs(dx) : inf;
    const double t_delta_y = step_y != 0 ? cell / std::abs(dy) : inf;
    const std::uint32_t limit = (cx > ex ? cx - ex : ex - cx) + (cy > ey ? cy - ey : ey - cy) + 2;
    auto visit = [&](std::uint32_t x, std::uint32_t y) {
        ++out.cells;
        const CellSample sample = CellAt(x, y);
        if (!sample.Ok()) {
            out.status = sample.status;
        } else if ((sample.attributes & CellSample::kBlocked) != 0) {
            out.blocked = true;
        } else {
            return true;
        }
        out.chunk = ChunkIndex(x / geometry_.chunk_cells, y / geometry_.chunk_cells);
        return false;
    };
    while ((cx != ex || cy != ey) && out.cells < limit) {
        if (t_max_x < t_max_y) {
            cx = static_cast<std::uint32_t>(static_cast<int>(cx) + step_x);
            t_max_x += t_delta_x;
        } else if (t_max_y < t_max_x) {
            cy = static_cast<std::uint32_t>(static_cast<int>(cy) + step_y);
            t_max_y += t_delta_y;
        } else {
            // Exact corner: both side cells are on the swept path.
            if (!visit(static_cast<std::uint32_t>(static_cast<int>(cx) + step_x), cy)) {
                return out;
            }
            cx = static_cast<std::uint32_t>(static_cast<int>(cx) + step_x);
            cy = static_cast<std::uint32_t>(static_cast<int>(cy) + step_y);
            t_max_x += t_delta_x;
            t_max_y += t_delta_y;
        }
        if (cx >= geometry_.cells_x || cy >= geometry_.cells_y) {
            out.status = TerrainStatus::OutsideWorld;
            return out;
        }
        if (!visit(cx, cy)) {
            return out;
        }
    }
    return out;
}

HeightSample ServerTerrain::Vertex(std::uint32_t vx, std::uint32_t vy) const noexcept
{
    if (slot_count_ == 0 || vx > geometry_.cells_x || vy > geometry_.cells_y) {
        return {TerrainStatus::OutsideWorld, 0.0f};
    }
    // A border sample lives in both neighbours; read the chunk that owns it
    // as a non-final sample (the last chunk for the outer edge).
    const std::uint32_t cx = std::min(vx / geometry_.chunk_cells, geometry_.chunks_x - 1);
    const std::uint32_t cy = std::min(vy / geometry_.chunk_cells, geometry_.chunks_y - 1);
    const TerrainChunk* chunk = ChunkAt(cx, cy);
    if (chunk == nullptr) {
        return {TerrainStatus::NotResident, 0.0f};
    }
    const std::uint32_t lx = vx - cx * geometry_.chunk_cells;
    const std::uint32_t ly = vy - cy * geometry_.chunk_cells;
    if (lx > chunk->cells_x || ly > chunk->cells_y || !chunk->HasSamples(encoding_.int32_samples)) {
        return {TerrainStatus::InvalidData, 0.0f};
    }
    return {TerrainStatus::Ok, static_cast<float>(RawMeters(*chunk, lx, ly))};
}

} // namespace mx::map
