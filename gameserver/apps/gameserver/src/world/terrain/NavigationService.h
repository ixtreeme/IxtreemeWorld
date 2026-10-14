#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "map/ServerTerrain.h"

#include "TerrainService.h"
#include "TerrainRequest.h"

// MAP-3 navigation: the minimal, REAL server path query -- infrastructure
// only (no AI / aggro / route policy). Grid A* over the terrain's cell grid:
// a cell is traversable when it has data, is not blocked (attribute bit 0)
// and the move into it passes the movement rules (slope, deep water).
// 8-connected; a diagonal move needs both orthogonal neighbours traversable
// (no corner cutting). There is no navmesh layer in the package format: the
// walkability grid IS the navigation data (documented limit).
//
// Job lifecycle (owner thread = runtime supervisor, like the terrain
// streamer): Request -> Running (a bounded number of node expansions per
// supervisor pass, shared by all jobs) -> exactly one terminal result.
// A job reading a chunk that is not resident demands it and waits (bounded
// by its timeout). Every chunk the job has read is PINNED by the job until it
// ends, so cache eviction can never take data from under a search.
namespace gs::game {

class TerrainStreamer;

enum class NavStatus : std::uint8_t {
    Pending,
    Found,
    NoPath,           // start and goal not connected inside the search window
    NotResident,      // a needed chunk did not become resident within the timeout
    OutsideWorld,     // start or goal outside the world
    BudgetExceeded,   // expansion/deadline/resource limit reached before a result
    Cancelled,
    InvalidData,      // unusable terrain data on the way
    UnsupportedLayer, // no navigation data (flat synthetic world)
    Rejected,         // too many jobs running (kMaxJobs): explicit resource shortage, retry later
};
const char* ToString(NavStatus status) noexcept;

struct NavRequest {
    float start_x = 0.0f;
    float start_y = 0.0f;
    float goal_x = 0.0f;
    float goal_y = 0.0f;
    std::uint32_t max_expansions = 20000; // per-job work limit
    float search_margin_m = 256.0f;       // window = start/goal bbox + margin
    double timeout_seconds = 5.0;         // incl. waiting for chunks
};

struct NavResult {
    NavStatus status = NavStatus::Pending;
    std::vector<std::pair<float, float>> waypoints; // cell centres, start -> goal (turns only)
    float length_m = 0.0f;
    std::uint32_t expansions = 0;
    std::uint32_t chunk_waits = 0;  // passes spent waiting for a chunk
    std::uint32_t pinned_chunks = 0; // peak pins held
    double elapsed_ms = 0.0;
};

class NavigationService {
public:
    using Clock = std::chrono::steady_clock;
    using JobId = std::uint64_t;

    struct Stats {
        std::uint64_t requested = 0;
        std::uint64_t found = 0;
        std::uint64_t no_path = 0;
        std::uint64_t not_resident = 0;
        std::uint64_t budget_exceeded = 0;
        std::uint64_t cancelled = 0;
        std::uint64_t invalid = 0;  // search exhausted with invalid data on the way (not NoPath)
        std::uint64_t rejected = 0; // job cap reached
        std::uint64_t other = 0;
        std::uint64_t expansions = 0;
        std::size_t running = 0;
        std::size_t pinned_chunks = 0;
    };

    // Owner thread only (the supervisor).
    // `id` is allocated by the caller (unique, > 0): the request may be posted
    // from any thread while the job itself starts on the owner thread.
    void Request(JobId id, const NavRequest& request, Clock::time_point now);
    void Cancel(JobId id);
    // Advances running jobs by at most `expansion_budget` expansions in total.
    void Pump(const TerrainService& terrain, TerrainStreamer* streamer, Clock::time_point now,
              std::uint32_t expansion_budget);
    // Any thread: the result once terminal (Pending while running / unknown).
    NavResult Result(JobId id) const;
    Stats GetStats() const;

private:
    struct Node {
        float g = 0.0f;
        std::uint64_t parent = 0;
        bool closed = false;
    };
    struct Open {
        float f = 0.0f;
        std::uint64_t key = 0;
        bool operator<(const Open& o) const noexcept
        {
            return f > o.f; // min-heap
        }
    };
    struct Job {
        NavRequest request;
        Clock::time_point started{};
        std::uint32_t sx = 0, sy = 0, gx = 0, gy = 0;
        std::uint32_t min_x = 0, min_y = 0, max_x = 0, max_y = 0; // window, inclusive
        std::unordered_map<std::uint64_t, Node> nodes;
        std::vector<Open> open;
        bool started_search = false;
        bool cancel = false;
        bool saw_invalid = false; // a cell with unusable (invalid) data bordered the search
        TerrainRequestHandle terrain_request;
        std::uint32_t waiting_chunk = mx::map::ServerTerrain::kNoChunk;
        std::unordered_map<std::uint32_t, std::shared_ptr<const mx::map::TerrainChunk>> pins;
        NavResult result;
    };

    // Step outcome of one cell for the search.
    enum class CellState : std::uint8_t { Open, Closed, Missing, Invalid };
    CellState Traversable(Job& job, const TerrainService& terrain, TerrainStreamer* streamer, std::uint32_t from_x,
                          std::uint32_t from_y, std::uint32_t x, std::uint32_t y, std::uint32_t& missing_chunk);
    void Finish(Job& job, NavStatus status, Clock::time_point now);
    bool Advance(Job& job, const TerrainService& terrain, TerrainStreamer* streamer, Clock::time_point now,
                 std::uint32_t& budget);

    std::unordered_map<JobId, Job> jobs_; // owner thread

    mutable std::mutex results_mutex_;
    Stats published_stats_; // readers never race the owner's in-progress counters
    std::map<JobId, NavResult> results_; // bounded: oldest dropped beyond kMaxResults
    static constexpr std::size_t kMaxResults = 4096;

public:
    // Running jobs at most; a request beyond it finishes at once as Rejected
    // (bounded memory: each job holds its node map and its pins).
    static constexpr std::size_t kMaxJobs = 256;
    Stats stats_;
};

} // namespace gs::game
