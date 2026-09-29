#include "NavigationService.h"

#include <algorithm>
#include <cmath>

#include "TerrainStreamer.h"

namespace gs::game {
namespace {

constexpr float kSqrt2 = 1.41421356f;

std::uint64_t Key(std::uint32_t x, std::uint32_t y) noexcept
{
    return (static_cast<std::uint64_t>(y) << 32) | x;
}
std::uint32_t KeyX(std::uint64_t key) noexcept
{
    return static_cast<std::uint32_t>(key & 0xffffffffu);
}
std::uint32_t KeyY(std::uint64_t key) noexcept
{
    return static_cast<std::uint32_t>(key >> 32);
}

} // namespace

const char* ToString(NavStatus status) noexcept
{
    switch (status) {
    case NavStatus::Pending:
        return "pending";
    case NavStatus::Found:
        return "found";
    case NavStatus::NoPath:
        return "no-path";
    case NavStatus::NotResident:
        return "not-resident";
    case NavStatus::OutsideWorld:
        return "outside-world";
    case NavStatus::BudgetExceeded:
        return "budget-exceeded";
    case NavStatus::Cancelled:
        return "cancelled";
    case NavStatus::InvalidData:
        return "invalid-data";
    case NavStatus::UnsupportedLayer:
        return "unsupported-layer";
    case NavStatus::Rejected:
        return "rejected";
    }
    return "?";
}

void NavigationService::Request(JobId id, const NavRequest& request, Clock::time_point now)
{
    if (jobs_.size() >= kMaxJobs && jobs_.find(id) == jobs_.end()) {
        NavResult rejected;
        rejected.status = NavStatus::Rejected;
        std::lock_guard lock(results_mutex_);
        results_[id] = rejected;
        while (results_.size() > kMaxResults) {
            results_.erase(results_.begin());
        }
        ++stats_.requested;
        ++stats_.rejected;
        published_stats_=stats_;
        return;
    }
    Job& job = jobs_[id];
    job.request = request;
    job.started = now;
    std::lock_guard lock(results_mutex_);
    results_[id] = NavResult{};
    ++stats_.requested;
    stats_.running = jobs_.size();
    published_stats_=stats_;
}

void NavigationService::Cancel(JobId id)
{
    const auto it = jobs_.find(id);
    if (it != jobs_.end()) {
        it->second.cancel = true;
    }
}

NavResult NavigationService::Result(JobId id) const
{
    std::lock_guard lock(results_mutex_);
    const auto it = results_.find(id);
    return it != results_.end() ? it->second : NavResult{};
}

NavigationService::Stats NavigationService::GetStats() const
{
    std::lock_guard lock(results_mutex_);
    return published_stats_;
}

NavigationService::CellState NavigationService::Traversable(Job& job,
                                                            const TerrainService& terrain,
                                                            TerrainStreamer* streamer,
                                                            std::uint32_t from_x,
                                                            std::uint32_t from_y,
                                                            std::uint32_t x,
                                                            std::uint32_t y,
                                                            std::uint32_t& missing_chunk)
{
    const auto* grid = terrain.Terrain();
    const auto& g = grid->Geometry();
    const auto cell = grid->CellAt(x, y);
    const std::uint32_t chunk = grid->ChunkIndex(x / g.chunk_cells, y / g.chunk_cells);
    switch (cell.status) {
    case mx::map::TerrainStatus::Ok:
        break;
    case mx::map::TerrainStatus::NotResident:
        missing_chunk = chunk;
        return CellState::Missing;
    case mx::map::TerrainStatus::InvalidData:
        return CellState::Invalid;
    case mx::map::TerrainStatus::OutsideWorld:
        return CellState::Closed;
    }
    if (job.pins.find(chunk) == job.pins.end()) {
        // Pin every chunk the search has read: eviction skips it until the
        // job ends (lifetime across supervisor passes).
        job.pins.emplace(chunk, grid->Owned(chunk));
        job.result.pinned_chunks = std::max<std::uint32_t>(job.result.pinned_chunks,
                                                           static_cast<std::uint32_t>(job.pins.size()));
    }
    if(job.terrain_request && job.waiting_chunk==chunk) {
        job.terrain_request->Consume();
        job.terrain_request.reset();
    }
    (void)streamer;
    if ((cell.attributes & mx::map::CellSample::kBlocked) != 0) {
        return CellState::Closed;
    }
    const auto& rules = terrain.Rules();
    const auto center = [&](std::uint32_t cx, std::uint32_t cy) {
        return std::pair<float, float>{static_cast<float>(g.origin_x + (cx + 0.5) * g.cell_size_m),
                                       static_cast<float>(g.origin_y + (cy + 0.5) * g.cell_size_m)};
    };
    const auto [tx, ty] = center(x, y);
    if (rules.max_slope > 0.0f) {
        const auto [fx, fy] = center(from_x, from_y);
        const auto h0 = terrain.Height(fx, fy);
        const auto h1 = terrain.Height(tx, ty);
        const float dist = std::hypot(tx - fx, ty - fy);
        if (h0.Ok() && h1.Ok() && dist > 0.0f && (h1.meters - h0.meters) / dist > rules.max_slope) {
            return CellState::Closed;
        }
    }
    if (rules.max_water_depth_m >= 0.0f) {
        const auto water = terrain.Water(tx, ty);
        if (water.IsWater() && water.depth_m > rules.max_water_depth_m) {
            return CellState::Closed;
        }
    }
    return CellState::Open;
}

void NavigationService::Finish(Job& job, NavStatus status, Clock::time_point now)
{
    job.result.status = status;
    job.result.elapsed_ms = std::chrono::duration<double, std::milli>(now - job.started).count();
    job.pins.clear(); // releases the pins
    if(job.terrain_request) job.terrain_request->Cancel();
    job.terrain_request.reset();
    std::lock_guard lock(results_mutex_);
    stats_.expansions += job.result.expansions;
    switch (status) {
    case NavStatus::Found:
        ++stats_.found;
        break;
    case NavStatus::NoPath:
        ++stats_.no_path;
        break;
    case NavStatus::NotResident:
        ++stats_.not_resident;
        break;
    case NavStatus::BudgetExceeded:
        ++stats_.budget_exceeded;
        break;
    case NavStatus::Cancelled:
        ++stats_.cancelled;
        break;
    case NavStatus::InvalidData:
        ++stats_.invalid;
        break;
    default:
        ++stats_.other;
        break;
    }
}

bool NavigationService::Advance(Job& job,
                                const TerrainService& terrain,
                                TerrainStreamer* streamer,
                                Clock::time_point now,
                                std::uint32_t& budget)
{
    if (job.cancel) {
        Finish(job, NavStatus::Cancelled, now);
        return true;
    }
    // Deadline also applies when another job used this pass's expansion
    // budget; otherwise a partial pin set could be held indefinitely.
    if(std::chrono::duration<double>(now-job.started).count()>job.request.timeout_seconds) {
        Finish(job,job.result.chunk_waits ? NavStatus::NotResident : NavStatus::BudgetExceeded,now);
        return true;
    }
    if(job.terrain_request) {
        const auto status=job.terrain_request->Status();
        if(status!=TerrainRequestStatus::Pending && status!=TerrainRequestStatus::Ready) {
            Finish(job,status==TerrainRequestStatus::InvalidData ? NavStatus::InvalidData :
                status==TerrainRequestStatus::CapacityRejected ? NavStatus::BudgetExceeded : NavStatus::NotResident,now);
            return true;
        }
    }
    const auto* grid = terrain.Terrain();
    if (grid == nullptr) {
        Finish(job, NavStatus::UnsupportedLayer, now);
        return true;
    }
    const auto& g = grid->Geometry();
    const auto& r = job.request;
    if (!job.started_search) {
        if (!grid->CellIndexOf(r.start_x, r.start_y, job.sx, job.sy) ||
            !grid->CellIndexOf(r.goal_x, r.goal_y, job.gx, job.gy)) {
            Finish(job, NavStatus::OutsideWorld, now);
            return true;
        }
        const auto margin = static_cast<std::uint32_t>(std::ceil(r.search_margin_m / g.cell_size_m));
        auto lo = [&](std::uint32_t a, std::uint32_t b) { return std::min(a, b) > margin ? std::min(a, b) - margin : 0u; };
        auto hi = [&](std::uint32_t a, std::uint32_t b, std::uint32_t cells) {
            return std::min<std::uint64_t>(static_cast<std::uint64_t>(std::max(a, b)) + margin, cells - 1);
        };
        job.min_x = lo(job.sx, job.gx);
        job.min_y = lo(job.sy, job.gy);
        job.max_x = static_cast<std::uint32_t>(hi(job.sx, job.gx, g.cells_x));
        job.max_y = static_cast<std::uint32_t>(hi(job.sy, job.gy, g.cells_y));
        job.nodes[Key(job.sx, job.sy)] = Node{0.0f, Key(job.sx, job.sy), false};
        job.open.push_back(Open{0.0f, Key(job.sx, job.sy)});
        job.started_search = true;
    }
    const float cell = static_cast<float>(g.cell_size_m);
    auto heuristic = [&](std::uint32_t x, std::uint32_t y) {
        const float dx = std::abs(static_cast<float>(x) - static_cast<float>(job.gx));
        const float dy = std::abs(static_cast<float>(y) - static_cast<float>(job.gy));
        return cell * (std::max(dx, dy) + (kSqrt2 - 1.0f) * std::min(dx, dy));
    };
    const std::uint64_t goal = Key(job.gx, job.gy);
    while (!job.open.empty() && budget > 0) {
        if (job.result.expansions >= r.max_expansions) {
            Finish(job, NavStatus::BudgetExceeded, now);
            return true;
        }
        std::pop_heap(job.open.begin(), job.open.end());
        const Open top = job.open.back();
        job.open.pop_back();
        Node& current = job.nodes[top.key];
        if (current.closed) {
            continue;
        }
        const std::uint32_t cx = KeyX(top.key);
        const std::uint32_t cy = KeyY(top.key);
        if (top.key == goal) {
            // Reconstruct; keep only the turning points.
            std::vector<std::uint64_t> keys{goal};
            for (std::uint64_t k = goal; k != Key(job.sx, job.sy);) {
                k = job.nodes[k].parent;
                keys.push_back(k);
            }
            std::reverse(keys.begin(), keys.end());
            auto center = [&](std::uint64_t k) {
                return std::pair<float, float>{static_cast<float>(g.origin_x + (KeyX(k) + 0.5) * g.cell_size_m),
                                               static_cast<float>(g.origin_y + (KeyY(k) + 0.5) * g.cell_size_m)};
            };
            for (std::size_t i = 0; i < keys.size(); ++i) {
                if (i > 0 && i + 1 < keys.size()) {
                    const int dx0 = static_cast<int>(KeyX(keys[i])) - static_cast<int>(KeyX(keys[i - 1]));
                    const int dy0 = static_cast<int>(KeyY(keys[i])) - static_cast<int>(KeyY(keys[i - 1]));
                    const int dx1 = static_cast<int>(KeyX(keys[i + 1])) - static_cast<int>(KeyX(keys[i]));
                    const int dy1 = static_cast<int>(KeyY(keys[i + 1])) - static_cast<int>(KeyY(keys[i]));
                    if (dx0 == dx1 && dy0 == dy1) {
                        continue;
                    }
                }
                job.result.waypoints.push_back(center(keys[i]));
            }
            job.result.length_m = current.g;
            Finish(job, NavStatus::Found, now);
            return true;
        }
        current.closed = true;
        ++job.result.expansions;
        --budget;
        bool waiting = false;
        std::uint32_t missing = mx::map::ServerTerrain::kNoChunk;
        auto state_of = [&](std::uint32_t x, std::uint32_t y) {
            std::uint32_t chunk = mx::map::ServerTerrain::kNoChunk;
            const CellState s = Traversable(job, terrain, streamer, cx, cy, x, y, chunk);
            if (s == CellState::Missing) {
                waiting = true;
                missing = chunk;
            }
            job.saw_invalid = job.saw_invalid || s == CellState::Invalid;
            return s;
        };
        for (int dy = -1; dy <= 1 && !waiting; ++dy) {
            for (int dx = -1; dx <= 1 && !waiting; ++dx) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                const std::int64_t nx = static_cast<std::int64_t>(cx) + dx;
                const std::int64_t ny = static_cast<std::int64_t>(cy) + dy;
                if (nx < job.min_x || ny < job.min_y || nx > job.max_x || ny > job.max_y) {
                    continue;
                }
                const auto x = static_cast<std::uint32_t>(nx);
                const auto y = static_cast<std::uint32_t>(ny);
                const std::uint64_t key = Key(x, y);
                const auto existing = job.nodes.find(key);
                if (existing != job.nodes.end() && existing->second.closed) {
                    continue;
                }
                if (state_of(x, y) != CellState::Open) {
                    continue;
                }
                if (dx != 0 && dy != 0 &&
                    (state_of(static_cast<std::uint32_t>(nx), cy) != CellState::Open ||
                     state_of(cx, static_cast<std::uint32_t>(ny)) != CellState::Open)) {
                    continue; // no corner cutting
                }
                const float step = (dx != 0 && dy != 0) ? cell * kSqrt2 : cell;
                const float tentative = current.g + step;
                auto& node = job.nodes[key];
                if (existing == job.nodes.end() || tentative < node.g) {
                    node.g = tentative;
                    node.parent = top.key;
                    job.open.push_back(Open{tentative + heuristic(x, y), key});
                    std::push_heap(job.open.begin(), job.open.end());
                }
            }
        }
        if (waiting) {
            // A needed chunk is not resident: re-open this node, demand the
            // chunk, and continue on a later pass (bounded by the timeout).
            current.closed = false;
            --job.result.expansions;
            job.open.push_back(top);
            std::push_heap(job.open.begin(), job.open.end());
            ++job.result.chunk_waits;
            const bool can_wait = streamer != nullptr && streamer->Enabled();
            if (can_wait) {
                if(!job.terrain_request || job.waiting_chunk!=missing) {
                    if(job.terrain_request) job.terrain_request->Cancel();
                    const double left=std::max(0.0,r.timeout_seconds-std::chrono::duration<double>(now-job.started).count());
                    job.terrain_request=std::make_shared<TerrainRequest>(now,left);
                    job.waiting_chunk=missing;
                    streamer->Request(job.terrain_request,{missing},TerrainPriority::Active);
                }
            }
            const double waited = std::chrono::duration<double>(now - job.started).count();
            if (!can_wait || waited > r.timeout_seconds) {
                Finish(job, NavStatus::NotResident, now);
                return true;
            }
            return false;
        }
    }
    if (job.open.empty()) {
        // Exhausted. With unusable data on the frontier the honest answer is
        // InvalidData: a path may exist through the cells we could not read.
        Finish(job, job.saw_invalid ? NavStatus::InvalidData : NavStatus::NoPath, now);
        return true;
    }
    return false; // pass budget used up; continues next pass
}

void NavigationService::Pump(const TerrainService& terrain,
                             TerrainStreamer* streamer,
                             Clock::time_point now,
                             std::uint32_t expansion_budget)
{
    std::vector<JobId> done;
    for (auto& [id, job] : jobs_) {
        if (Advance(job, terrain, streamer, now, expansion_budget)) {
            done.push_back(id);
        }
    }
    std::size_t pins = 0;
    for (const JobId id : done) {
        auto node = jobs_.extract(id);
        std::lock_guard lock(results_mutex_);
        results_[id] = std::move(node.mapped().result);
        while (results_.size() > kMaxResults) {
            results_.erase(results_.begin()); // oldest id first
        }
    }
    for (const auto& [id, job] : jobs_) {
        (void)id;
        pins += job.pins.size();
    }
    std::lock_guard lock(results_mutex_);
    stats_.running = jobs_.size();
    stats_.pinned_chunks = pins;
    published_stats_=stats_;
}

} // namespace gs::game
