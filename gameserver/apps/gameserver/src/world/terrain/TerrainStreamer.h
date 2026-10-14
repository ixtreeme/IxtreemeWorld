#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <array>
#include "TerrainRequest.h"

#include "map/ServerTerrain.h"
#include "map/WorldPackage.h"

// MAP-3: chunk streaming for the server terrain -- one shared, world-owned,
// immutable chunk cache with asynchronous loading, a memory budget and
// bounded I/O. Contract: docs/map-data-format.md 10 / 12.
//
// Threads:
//   * I/O workers (config io_threads, default 2; never one per chunk/zone)
//     only run ChunkSource::Load (read + CRC + decode + validate) and push the
//     result into the completion queue. They never touch the terrain slots.
//   * Everything else -- demand, admission, publication, eviction, freeing --
//     runs on ONE thread: the runtime supervisor (Pump), or the loader
//     thread before the runtime starts (LoadBlocking).
//
// Chunk state machine (per chunk key = index in the chunk grid):
//   Unloaded -> Waiting (demanded, waiting for budget) -> Loading (bytes
//   reserved, job queued / running) -> Ready (published) -> [evicted:
//   unpublished, bytes retired until the next quiescent window] -> Unloaded
//   Loading -> Failed (bounded retries with backoff; after the last attempt
//   the chunk is published as INVALID: queries answer InvalidData, never
//   "free space"). A Loading job whose demand expired before an I/O worker
//   picked it up is cancelled (reservation released, no read).
//   Failed semantics: every load failure (read error, CRC, decode, seam
//   mismatch against a published neighbour) counts one attempt. Between
//   attempts the slot stays EMPTY (queries: NotResident -- consumers wait),
//   the next load starts only when a consumer demands the chunk again after
//   the backoff (attempt x retry_backoff_seconds). After max_attempts the
//   chunk is published INVALID: an empty chunk (overhead bytes only) that
//   is never evicted, never re-read and never demanded again for the rest of
//   the package generation; only a generation change (package switch; the
//   ResetGenerationForTest seam) retires it and allows a fresh load.
//
// Lifetime: a published chunk is immutable. Readers inside a zone tick (or on
// the supervisor) use the raw published pointer without locks; a chunk that
// left its slot (evicted, or replaced by a later publication) is freed only
// in a supervisor quiescent window (no zone tick in flight) after it was
// unpublished, so no tick can still hold it (ServerTerrain.h has the
// happens-before argument). Readers that outlive a tick hold a pin
// (ServerTerrain::Owned, writer thread only); pinned chunks are never chosen
// for eviction. Completions carry (generation, request epoch) and no zone
// reference at all: a completion for a superseded request, a reset package
// generation or after Stop() is rejected and its memory released (never
// published); a current one publishes into the world-owned cache whatever
// happened to the zones in the meantime (migration, split / merge, retire,
// reclaim, slot reuse).
//
// Budget: own accounting = resident payload + per-chunk overhead + in-flight
// reservations (decoded size + read buffer) + retired-not-yet-freed + fixed
// metadata (slot tables, chunk index). Admission reserves before a load is
// queued; the total never exceeds the budget. Without room (everything
// resident pinned or in its handoff lease) a demand waits in a bounded list;
// when that is full the demand is rejected (counted) and simply re-demanded
// later by its consumer. Process RSS is reported separately (OS view).
namespace gs::game {

struct TerrainStreamingConfig {
    std::size_t budget_bytes = 256ull << 20;
    std::uint32_t io_threads = 2;
    std::uint32_t max_in_flight = 32;   // reserved + queued + reading
    std::uint32_t max_waiting = 4096;   // demands waiting for budget
    double retain_seconds = 5.0;        // soft retention; admission pressure may override (>= 0.1 s)
    double high_water = 0.90;           // proactive eviction starts above this budget fraction
    double low_water = 0.75;            // ... and stops below this one
    std::uint32_t max_attempts = 3;     // failed loads: bounded retries
    double retry_backoff_seconds = 0.5; // x attempt number
    double lookahead_min_seconds = 0.5; // prefetch window bounds (see LookaheadSeconds)
    double lookahead_max_seconds = 5.0;
    // ---- test instrumentation (never set in production) ----
    // A "freed" chunk is poisoned (every sample / attribute overwritten with
    // a sentinel no package can produce) and parked in a bounded quarantine
    // instead of being released: a reader that still used it would read the
    // sentinel -- a use-after-free made observable without undefined
    // behaviour in the correct protocol.
    bool poison_freed_for_test = false;
    // Negative control for the lifetime tests: frees (poisons) retired chunks
    // WITHOUT waiting for a quiescent window. Deliberately broken protocol.
    bool unsafe_free_without_quiescence_for_test = false;
};

// Sentinels written by poison_freed_for_test (outside every encoding range a
// validated package can hold for the fixtures that use it).
constexpr std::int16_t kPoisonHeight16 = -32768;
constexpr std::uint16_t kPoisonAttributes = 0xdeadu;

class TerrainStreamer {
public:
    using Clock = std::chrono::steady_clock;

    enum class ChunkState : std::uint8_t { Unloaded, Waiting, Loading, Ready, Failed };

    struct ReloadTrace {
        std::uint32_t chunk=0;
        std::uint64_t generation=0, evicted_us=0, requested_us=0, published_us=0;
    };
    struct Stats {
        // Chunks by state (point in time).
        std::size_t chunks_total = 0;
        std::size_t resident = 0;
        std::size_t waiting = 0;
        std::size_t loading = 0;
        std::size_t failed = 0;
        std::size_t invalid = 0;           // permanently failed, published as invalid (overhead only)
        std::size_t pinned = 0;            // resident chunks with a permanent pin or a reader pin
        std::size_t permanent_pinned = 0;  // startup set (spawn region centres, warp targets)
        std::size_t reader_pinned = 0;     // pinned by a reader (navigation job)
        std::size_t retired = 0;
        std::size_t soft_retained_bytes = 0;
        std::size_t evictable_bytes = 0;
        std::size_t completion_pending = 0;
        std::size_t io_queued = 0;
        double oldest_wait_ms = 0;
        std::uint64_t room_checks = 0;
        std::uint64_t room_blocked = 0;
        std::uint64_t pump_calls = 0;
        std::uint64_t quiescent_pumps = 0;
        std::uint64_t pump_us = 0;
        std::uint64_t pressure_evictions = 0;
        std::uint64_t retry_suppressed = 0;
        std::uint64_t ingress_unique_rejected = 0;
        std::uint64_t requests_accepted = 0, requests_consumed = 0, requests_cancelled = 0;
        std::uint64_t requests_timed_out = 0, requests_rejected = 0, requests_invalid = 0;
        std::size_t requests_pending = 0, requests_ready = 0;
        double oldest_request_ms = 0;
        std::array<std::uint64_t,3> admitted_by_class{}, completed_by_class{}, examined_by_class{};
        std::array<std::uint64_t,3> waiting_by_class{};
        std::uint64_t window_id = 0, window_miss_samples = 0;
        std::uint64_t captured_us=0, window_started_us=0;
        std::uint64_t unique_requested=0, unique_loaded=0, reloads=0, short_reloads=0;
        std::array<ReloadTrace,16> reload_trace{};
        std::size_t reload_trace_count=0;
        std::uint64_t room_us=0, unpublish_us=0, reclaim_us=0, completion_us=0, admission_us=0;
        double window_miss_p99_ms = 0;
        std::size_t window_initial_waiting = 0;
        double window_initial_oldest_ms = 0;
        // Own accounting (bytes, point in time) vs the budget. accounted =
        // resident + in_flight + retired + metadata; pinned_bytes is the part
        // of resident that eviction may not touch.
        std::size_t budget_bytes = 0;
        std::size_t resident_bytes = 0;
        std::size_t pinned_bytes = 0;
        std::size_t in_flight_bytes = 0;
        std::size_t retired_bytes = 0;
        std::size_t metadata_bytes = 0;
        std::size_t accounted_bytes = 0;
        // Peaks: exact at every mutation, except pinned (sampled with the
        // stats snapshot, <= 10 Hz, and at every explicit operation).
        std::size_t peak_accounted_bytes = 0;
        std::size_t peak_resident_bytes = 0;
        std::size_t peak_in_flight_bytes = 0;
        std::size_t peak_retired_bytes = 0;
        std::size_t peak_pinned_bytes = 0;
        std::size_t quarantined = 0;       // poison_freed_for_test only
        // Cumulative.
        std::uint64_t demands = 0;          // Demand() calls
        std::uint64_t hits = 0;             // demanded while Ready
        std::uint64_t misses = 0;           // demanded while not Ready
        std::uint64_t deduplicated = 0;     // demanded while already Waiting/Loading
        std::uint64_t loads_started = 0;
        std::uint64_t loads_completed = 0;
        std::uint64_t load_failures = 0;
        std::uint64_t permanent_failures = 0;
        std::uint64_t seam_rejects = 0;
        std::uint64_t stale_completions = 0;
        std::uint64_t cancelled = 0;
        std::uint64_t admission_waits = 0;
        std::uint64_t admission_rejects = 0;
        std::uint64_t evictions = 0;
        std::uint64_t frees = 0;
        std::uint64_t bytes_read = 0;
        std::size_t queue_high_water = 0;   // in-flight jobs high-water
        std::size_t waiting_high_water = 0;
        std::uint64_t replaced = 0;         // publications over a still-published chunk (retired, not freed)
        // Load latency (admission -> published), recent window.
        double load_ms_p50 = 0.0;
        double load_ms_p99 = 0.0;
        double load_ms_max = 0.0;
        // Miss latency (first demand of a non-resident chunk -> published:
        // admission wait + I/O + decode), recent window.
        double miss_ms_p50 = 0.0;
        double miss_ms_p99 = 0.0;
        double miss_ms_max = 0.0;
        double lookahead_seconds = 0.0;
        std::uint64_t generation = 0;
    };

    // Streaming disabled (eager world or flat): every query is resident; the
    // streamer is inert (no threads).
    TerrainStreamer() = default;
    TerrainStreamer(mx::map::ServerTerrain& terrain,
                    std::shared_ptr<const mx::map::ChunkSource> source,
                    TerrainStreamingConfig config,
                    std::function<void()> wake = {});
    ~TerrainStreamer();
    TerrainStreamer(const TerrainStreamer&) = delete;
    TerrainStreamer& operator=(const TerrainStreamer&) = delete;

    bool Enabled() const noexcept
    {
        return terrain_ != nullptr;
    }
    const TerrainStreamingConfig& Config() const noexcept
    {
        return config_;
    }

    // ---- owner thread (supervisor / loader) ----
    // Marks the chunk as needed now (aggregated over every consumer: the most
    // recent demand wins, so one consumer going away never cancels a load
    // another still needs). kNoChunk / out of range is ignored.
    void Demand(std::uint32_t chunk_index, Clock::time_point now,
                TerrainPriority priority = TerrainPriority::Active);
    static constexpr std::size_t kMaxRequests = 64;
    static constexpr std::size_t kMaxRequestChunks = 16;
    // Owner thread: bounded logical request, same admission/cache as ticks.
    // Ready holds pins until Consume/Cancel/last handle drop/deadline.
    void Request(const TerrainRequestHandle& request, const std::vector<std::uint32_t>& chunks,
                 TerrainPriority priority = TerrainPriority::Admission);
    bool NeedsQuiescence() const noexcept {
        return std::any_of(retired_.begin(),retired_.end(),[](const auto& r){return r.chunk.use_count()==1;});
    }
    // Never evicted (startup set: player spawn region centres, warp targets).
    void PinPermanently(std::uint32_t chunk_index);
    // One control step: drain completions (publish / fail / reject stale),
    // admit waiting demands within the budget, evict above the high-water
    // mark, and -- only when `quiescent` (no zone tick in flight) -- free
    // retired chunks.
    void Pump(Clock::time_point now, bool quiescent);
    // Blocking load of `chunks` before the runtime runs (startup / tests):
    // waits until every chunk is Ready or permanently failed (published
    // invalid). Returns false with `error` when any failed or the set does
    // not fit the budget. Not for the running simulation.
    bool LoadBlocking(const std::vector<std::uint32_t>& chunks, std::string& error);
    // Evicts every unpinned Ready chunk not demanded since `older_than`
    // (startup batch spawning; tests). Frees at once when `quiescent`.
    std::size_t EvictUndemanded(Clock::time_point older_than, bool quiescent);
    // Stops the I/O workers (joins them). Queued jobs are dropped; results
    // that arrive later are rejected. Idempotent.
    void Stop();
    // Test seam: simulates a package switch -- the generation advances, every
    // published chunk (valid or invalid) is retired, completions of the old
    // generation become stale. Owner thread.
    void ResetGenerationForTest(bool quiescent);

    ChunkState State(std::uint32_t chunk_index) const;
    // Owner thread: what a load of `chunk_index` reserves against the budget
    // (decoded payload + read buffer + per-chunk overhead), and the part of
    // the budget not accounted right now.
    std::size_t ReservationBytes(std::uint32_t chunk_index) const noexcept
    {
        return Enabled() ? Reservation(chunk_index) : 0;
    }
    std::size_t FreeBytes() const noexcept
    {
        return config_.budget_bytes - std::min(config_.budget_bytes, Accounted());
    }
    // Current prefetch window: clamp(3 x p99 load latency + one tick,
    // lookahead_min, lookahead_max). Read by movement (any thread).
    float LookaheadSeconds() const noexcept
    {
        return lookahead_seconds_.load(std::memory_order_relaxed);
    }
    // Thread-safe snapshot (published by the owner thread, <= 10 Hz from
    // Pump, and after every explicit operation).
    Stats GetStats() const;
    // Owner thread: publish a fresh snapshot now (tests, shutdown summary).
    void PublishStats()
    {
        UpdateStats();
    }
    void BeginMeasurementWindow(Clock::time_point now);

private:
    struct Slot {
        ChunkState state = ChunkState::Unloaded;
        bool permanent_pin = false;
        bool in_waiting = false;
        std::uint32_t attempts = 0;
        std::uint64_t request_epoch = 0;
        std::size_t reserved_bytes = 0;
        Clock::time_point last_demand{};
        Clock::time_point first_miss{};    // first demand while not resident
        Clock::time_point retry_after{};
        Clock::time_point admitted_at{};
        Clock::time_point ready_until{};
        Clock::time_point ingress_retry{};
        std::array<Clock::time_point,3> last_by_class{};
        std::uint64_t observed_request_window=~0ull, observed_load_window=~0ull, evicted_window=~0ull;
        Clock::time_point observed_eviction{}, observed_redemand{};
        bool ingress_rejected = false;
        unsigned operation_priority = 3;
        bool admission_deferred = false;
    };
    struct Job {
        std::uint32_t chunk_index = 0;
        std::uint64_t generation = 0;
        std::uint64_t request_epoch = 0;
        std::atomic<bool> cancelled{false};
        std::size_t reservation = 0;
        unsigned admitted_class = 0;
    };
    struct Completion {
        std::shared_ptr<Job> job;
        bool cancelled = false;
        mx::map::ChunkLoadResult result;
    };
    struct Retired {
        std::shared_ptr<const mx::map::TerrainChunk> chunk;
        std::size_t bytes = 0;
    };
    struct RequestEntry {
        // Keep only the small request state alive until the next owner pass.
        // use_count==1 detects the last consumer drop, while preserving a
        // Consume() acknowledgement even when its handle was dropped at once.
        std::shared_ptr<TerrainRequest> consumer;
        std::array<std::uint32_t,kMaxRequestChunks> chunks{};
        std::array<std::shared_ptr<const mx::map::TerrainChunk>,kMaxRequestChunks> pins{};
        std::size_t count = 0;
        TerrainPriority priority = TerrainPriority::Admission;
        std::uint64_t generation = 0;
    };
    void PumpRequests(Clock::time_point now);
    void DemandImpl(std::uint32_t chunk_index, Clock::time_point now, TerrainPriority priority, bool operation);
    unsigned PriorityOf(const Slot& slot, Clock::time_point now) const;

    std::size_t Accounted() const noexcept
    {
        return resident_bytes_ + in_flight_bytes_ + retired_bytes_ + metadata_bytes_;
    }
    std::size_t Reservation(std::uint32_t chunk_index) const noexcept;
    bool Admit(std::uint32_t chunk_index, Clock::time_point now, bool quiescent);
    bool MakeRoom(std::size_t bytes, Clock::time_point now, bool quiescent, bool pressure = false);
    void EvictSlot(std::uint32_t chunk_index, bool quiescent);
    // Hands a chunk that left its slot (evicted or replaced) to the retired
    // list: freed at the next quiescent window, never earlier.
    void Retire(std::shared_ptr<const mx::map::TerrainChunk> chunk);
    void FreeRetired();
    void NotePeaks() noexcept;
    void DrainCompletions(Clock::time_point now);
    void Complete(Completion completion, Clock::time_point now);
    void PublishInvalid(std::uint32_t chunk_index);
    void UpdateStats();
    void IoWorker();

    mx::map::ServerTerrain* terrain_ = nullptr;
    std::shared_ptr<const mx::map::ChunkSource> source_;
    TerrainStreamingConfig config_;
    std::function<void()> wake_;
    std::uint64_t generation_ = 0;
    std::uint64_t next_epoch_ = 0;

    // Owner-thread state.
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> waiting_;   // demand order
    std::array<RequestEntry,kMaxRequests> requests_{};
    std::vector<std::uint32_t> pressure_candidates_;
    std::size_t pressure_cursor_ = 0;
    bool pressure_candidates_valid_ = false;
    std::size_t fairness_cursor_ = 0;
    Clock::time_point retry_admission_at_{};
    std::vector<Retired> retired_;
    std::size_t resident_bytes_ = 0;       // published payload + overhead (own accounting)
    std::size_t in_flight_bytes_ = 0;
    std::size_t retired_bytes_ = 0;
    std::size_t metadata_bytes_ = 0;
    std::size_t in_flight_jobs_ = 0;
    std::vector<double> latency_ms_;       // ring of recent load latencies
    std::size_t latency_next_ = 0;
    std::vector<double> miss_ms_;          // ring of recent miss latencies
    std::size_t miss_next_ = 0;
    std::deque<std::shared_ptr<const mx::map::TerrainChunk>> quarantine_; // poison_freed_for_test
    Stats counters_;                       // cumulative part, owner thread
    std::array<std::uint64_t,256> window_miss_histogram_{};

    // I/O queue (owner -> workers) and completions (workers -> owner).
    std::mutex io_mutex_;
    std::condition_variable io_cv_;
    std::deque<std::shared_ptr<Job>> io_queue_;
    bool io_stop_ = false;
    std::vector<std::thread> io_threads_;
    std::mutex completion_mutex_;
    std::vector<Completion> completions_;

    std::atomic<float> lookahead_seconds_{0.5f};
    Clock::time_point last_stats_{};
    mutable std::mutex stats_mutex_;
    Stats published_stats_;
};

const char* ToString(TerrainStreamer::ChunkState state) noexcept;

} // namespace gs::game
