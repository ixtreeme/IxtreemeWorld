#include "HardeningBench.h"
#include "ReadinessBench.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "db/CharacterRepository.h"
#include "network/Session.h"

#include "map/MapData.h"
#include "map/ServerTerrain.h"
#include "map/ServerWater.h"
#include "map/WorldPackage.h"
#include "map/WorldPackageWriter.h"

#include "../world/WorldConstants.h"
#include "../world/WorldRuntime.h"
#include "../world/components/SimulationLod.h"
#include "../world/components/Tags.h"
#include "../world/components/TransformComponents.h"
#include "../world/package/WorldPackageLoader.h"
#include "../world/partition/PartitionConfig.h"
#include "../world/partition/RegionDefinition.h"
#include "../world/terrain/NavigationService.h"
#include "../world/terrain/TerrainService.h"
#include "../world/terrain/TerrainStreamer.h"
#include "BenchSnapshot.h"

// MAP-3: chunk streaming, residency/budget, lifetime, and the world queries
// (collision / water / navigation) on file-backed packages written to a
// scratch directory and loaded through the production loader.
//
//   streaming : the streamer against a scripted chunk source (delays, gates,
//               failures, out-of-order completion), budget/admission,
//               pins/eviction/free, stale completions, shutdown; then the
//               runtime: streamed vs fully resident REFERENCE provider on the
//               same package, movement waiting for data, split/merge + slow
//               I/O, shutdown with pending loads.
//   worldquery: collision path (tunneling, chunk borders, corners, slope),
//               water (undeclared / none / sea level / bodies), navigation
//               (oracle path length, no path, cross-chunk, budget, cancel,
//               waiting for chunks, pins under eviction), runtime movement.
//   streamsoak: a large world with a small cache and players roaming it
//               (real misses / loads / evictions), memory vs process RSS.
namespace gs::bench {
namespace {

namespace fs = std::filesystem;
namespace asio = boost::asio;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using mx::map::TerrainStatus;
using ChunkState = gs::game::TerrainStreamer::ChunkState;

std::string Fmt(const char* format, ...)
{
    char buffer[4096];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

struct Checks {
    const char* tag;
    int failures = 0;
    int passes = 0;
    void Report(const std::string& name, bool pass, const std::string& detail)
    {
        std::printf("%s %s %s: %s\n", tag, name.c_str(), detail.c_str(), pass ? "PASS" : "FAIL");
        std::fflush(stdout);
        (pass ? passes : failures) += 1;
    }
};

struct IoRunner {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work{asio::make_work_guard(io)};
    std::thread thread{[this] { io.run(); }};
    ~IoRunner()
    {
        work.reset();
        io.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }
};

std::shared_ptr<gs::network::Session> DetachedSession(asio::io_context& io, gs::common::SessionId id)
{
    asio::ip::tcp::socket socket(io);
    return std::make_shared<gs::network::Session>(std::move(socket), id);
}

gs::db::Character MakeCharacter(std::uint64_t index)
{
    gs::db::Character character;
    character.id = gs::db::CharacterId{97000 + index};
    character.account_id = gs::db::AccountId{98000 + index};
    character.name = "MapStreaming" + std::to_string(index);
    character.level = 1;
    character.class_id = 1;
    character.created_at = std::chrono::system_clock::now();
    character.last_played_at = character.created_at;
    return character;
}

bool WaitFor(std::chrono::milliseconds timeout, const std::function<bool()>& condition)
{
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return condition();
}

std::string AuditNow(gs::game::WorldRuntime& sim)
{
    sim.RequestValidation();
    std::string result;
    for (int i = 0; i < 400; ++i) {
        if (sim.TryTakeValidationResult(result)) {
            return result;
        }
        std::this_thread::sleep_for(25ms);
    }
    return "TIMEOUT";
}

fs::path ScratchRoot()
{
    return fs::temp_directory_path() / "ixw_mapstreaming";
}

fs::path FixtureDir(const std::string& name)
{
    const fs::path dir = ScratchRoot() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir;
}

struct Loaded {
    std::optional<gs::game::LoadedWorld> world;
    mx::map::PackageReport report;
    std::string First() const
    {
        const auto* e = report.FirstError();
        return e != nullptr ? e->Format() : std::string("no error");
    }
};

Loaded LoadPackage(const fs::path& dir, mx::map::ResidencyMode residency)
{
    Loaded out;
    gs::game::WorldLoadRequest request;
    request.package_root = dir;
    request.mob_types_config = IXTREEME_DEFAULT_MOB_TYPES_CONFIG;
    request.residency = residency;
    out.world = gs::game::LoadWorldPackage(request, out.report);
    return out;
}

// ---- fixture: 4096 m world at origin (-2048,-2048), 8 m cells, 64-cell
// chunks = 8 x 8 chunks of 512 m; rolling hills; a blocked wall 2 cells wide
// (x cells 320..321 = x 512..528 m, y cells 64..447) with open ends; water bodies: one lake
// (surface above ground) and one dry basin (surface below ground). The world
// spawn region (areaId 0) is at (0..10, 0..10).
constexpr double kOX = -2048.0;
constexpr double kOY = -2048.0;
constexpr double kCell = 8.0;
constexpr std::uint32_t kCells = 512;
constexpr std::uint32_t kChunk = 64;

std::int32_t RawStream(std::uint32_t vx, std::uint32_t vy)
{
    return static_cast<std::int32_t>(
        std::lround(1500.0 * std::sin(vx * 0.05) + 900.0 * std::cos(vy * 0.037) + 2.0 * vx));
}

bool WallCell(std::uint32_t cx, std::uint32_t cy)
{
    return cx >= 320 && cx <= 321 && cy >= 64 && cy <= 447;
}

mx::map::PackageWriteSpec SpecStream(mx::map::WaterModel water = mx::map::WaterModel::Bodies)
{
    mx::map::PackageWriteSpec spec;
    spec.world_id = "map3-stream";
    spec.world_name = "MAP-3 streaming world";
    spec.size_cells_x = kCells;
    spec.origin_x = kOX;
    spec.origin_y = kOY;
    spec.cell_size_m = static_cast<float>(kCell);
    spec.chunk_size_cells = kChunk;
    spec.height_raw = RawStream;
    spec.attributes = [](std::uint32_t cx, std::uint32_t cy) -> std::uint16_t { return WallCell(cx, cy) ? 1 : 0; };
    spec.logic.spawns = {{1, 0, {0.0f, 0.0f, 10.0f, 10.0f}}}; // world-level spawn region, centre (5, 5)
    spec.mob_spawns = std::string("mob_type_id=1 x=-1500 y=-1500 count=30 radius=200\n"
                                  "mob_type_id=2 x=1500 y=-1500 count=30 radius=200\n"
                                  "mob_type_id=1 x=-1500 y=1500 count=30 radius=200\n"
                                  "mob_type_id=2 x=1500 y=1500 count=30 radius=200\n");
    spec.water_model = water;
    spec.sea_level_m = water == mx::map::WaterModel::SeaLevel ? 5.0 : 0.0;
    if (water == mx::map::WaterModel::Bodies) {
        spec.water_bodies = {{1, {-1800.0f, -600.0f, -1400.0f, -200.0f}, 60.0f},   // lake: ~+40 m over ground
                             {2, {1000.0f, 400.0f, 1400.0f, 800.0f}, -100.0f}};     // basin: below ground = dry
    }
    spec.overwrite = true;
    return spec;
}

// ---- scripted chunk source: delays, gates, failures, call accounting ----
class ScriptedSource final : public mx::map::ChunkSource {
public:
    explicit ScriptedSource(std::shared_ptr<const mx::map::ChunkSource> inner)
        : inner_(std::move(inner))
    {
    }

    mx::map::ChunkLoadResult Load(std::uint32_t index) const override
    {
        std::chrono::milliseconds delay{0};
        bool fail = false;
        bool corrupt = false;
        {
            std::unique_lock lock(mutex_);
            ++calls_[index];
            ++concurrent_;
            max_concurrent_ = std::max(max_concurrent_, concurrent_);
            cv_.wait(lock, [&] { return !gated_ || released_.count(index) != 0 || open_; });
            if (const auto it = delays_.find(index); it != delays_.end()) {
                delay = it->second;
            }
            delay = std::max(delay, global_delay_);
            fail = failing_.count(index) != 0;
            corrupt = corrupt_border_.count(index) != 0;
        }
        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }
        mx::map::ChunkLoadResult result;
        if (fail) {
            result.code = mx::map::PackageErrorCode::FileUnreadable;
            result.error = "scripted failure";
        } else {
            result = inner_->Load(index);
            if (corrupt && result.ok) {
                // A valid, CRC-correct chunk whose south-west border sample
                // disagrees with its neighbours (seam check at publication).
                auto copy = std::make_shared<mx::map::TerrainChunk>(*result.chunk);
                if (!copy->heights16.empty()) {
                    copy->heights16[0] = static_cast<std::int16_t>(copy->heights16[0] + 7);
                }
                result.chunk = std::move(copy);
            }
        }
        {
            std::lock_guard lock(mutex_);
            --concurrent_;
            order_.push_back(index);
        }
        return result;
    }
    std::uint64_t FileBytes(std::uint32_t index) const override
    {
        std::lock_guard lock(mutex_);
        if(const auto it=reservation_file_bytes_.find(index);it!=reservation_file_bytes_.end()) return it->second;
        return inner_->FileBytes(index);
    }
    void SetReservationFileBytes(std::uint32_t index,std::uint64_t bytes) {
        std::lock_guard lock(mutex_); reservation_file_bytes_[index]=bytes;
    }

    void Gate(bool on)
    {
        std::lock_guard lock(mutex_);
        gated_ = on;
        open_ = false;
        released_.clear();
    }
    void Release(std::uint32_t index)
    {
        {
            std::lock_guard lock(mutex_);
            released_.insert(index);
        }
        cv_.notify_all();
    }
    void OpenAll()
    {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }
    void SetDelay(std::uint32_t index, std::chrono::milliseconds delay)
    {
        std::lock_guard lock(mutex_);
        delays_[index] = delay;
    }
    void SetGlobalDelay(std::chrono::milliseconds delay)
    {
        std::lock_guard lock(mutex_);
        global_delay_ = delay;
    }
    void CorruptBorder(std::uint32_t index)
    {
        std::lock_guard lock(mutex_);
        corrupt_border_.insert(index);
    }
    void FailAlways(std::uint32_t index)
    {
        std::lock_guard lock(mutex_);
        failing_.insert(index);
    }
    void ClearFailure(std::uint32_t index)
    {
        std::lock_guard lock(mutex_);
        failing_.erase(index);
    }
    int Calls(std::uint32_t index) const
    {
        std::lock_guard lock(mutex_);
        const auto it = calls_.find(index);
        return it != calls_.end() ? it->second : 0;
    }
    int Concurrent() const
    {
        std::lock_guard lock(mutex_);
        return concurrent_;
    }
    int MaxConcurrent() const
    {
        std::lock_guard lock(mutex_);
        return max_concurrent_;
    }
    std::vector<std::uint32_t> Order() const
    {
        std::lock_guard lock(mutex_);
        return order_;
    }

private:
    std::shared_ptr<const mx::map::ChunkSource> inner_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    mutable std::unordered_map<std::uint32_t, int> calls_;
    mutable int concurrent_ = 0;
    mutable int max_concurrent_ = 0;
    mutable std::vector<std::uint32_t> order_;
    bool gated_ = false;
    bool open_ = false;
    std::unordered_set<std::uint32_t> released_;
    std::unordered_map<std::uint32_t, std::chrono::milliseconds> delays_;
    std::unordered_map<std::uint32_t,std::uint64_t> reservation_file_bytes_;
    std::chrono::milliseconds global_delay_{0};
    std::unordered_set<std::uint32_t> failing_;
    std::unordered_set<std::uint32_t> corrupt_border_;
};

// Streamer pump loop on the owner (bench) thread; `quiescent` = no runtime.
bool PumpUntil(gs::game::TerrainStreamer& streamer, const std::function<bool()>& done,
               std::chrono::milliseconds timeout = 5000ms, bool quiescent = true,
               const std::function<void()>& each_pass = {})
{
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (each_pass) {
            each_pass();
        }
        streamer.Pump(Clock::now(), quiescent);
        if (done()) {
            streamer.PublishStats();
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    streamer.PublishStats();
    return done();
}

// Bitwise comparison of a streamed chunk with the fully resident reference.
bool SameChunk(const mx::map::TerrainChunk* a, const mx::map::TerrainChunk* b)
{
    return a != nullptr && b != nullptr && a->cells_x == b->cells_x && a->cells_y == b->cells_y &&
           a->heights16 == b->heights16 && a->heights32 == b->heights32 && a->attributes == b->attributes;
}

} // namespace

namespace {

// A streamer on a fresh streaming load of `dir` (the loader published the
// startup set, pinned permanently like the runtime does). Destroyed in the
// right order: streamer (threads) before the terrain it publishes into.
struct Rig {
    std::unique_ptr<mx::map::ServerTerrain> terrain;
    std::shared_ptr<ScriptedSource> source;
    std::unique_ptr<gs::game::TerrainStreamer> streamer;
    std::vector<std::uint32_t> startup;
    ~Rig()
    {
        streamer.reset();
    }
};

std::unique_ptr<Rig> MakeRig(const fs::path& dir, const gs::game::TerrainStreamingConfig& config)
{
    auto loaded = LoadPackage(dir, mx::map::ResidencyMode::Streaming);
    if (!loaded.world) {
        return nullptr;
    }
    auto rig = std::make_unique<Rig>();
    rig->source = std::make_shared<ScriptedSource>(loaded.world->chunk_source);
    rig->terrain = std::make_unique<mx::map::ServerTerrain>(std::move(loaded.world->terrain));
    rig->startup = loaded.world->startup_chunks;
    rig->streamer = std::make_unique<gs::game::TerrainStreamer>(*rig->terrain, rig->source, config);
    for (const std::uint32_t index : rig->startup) {
        rig->streamer->PinPermanently(index);
    }
    return rig;
}

std::size_t ChunkReservation(const mx::map::ServerTerrain& t, const mx::map::ChunkSource& s, std::uint32_t i)
{
    return t.ChunkBytes(i) + static_cast<std::size_t>(s.FileBytes(i)) + sizeof(mx::map::TerrainChunk) + 64;
}

} // namespace

int RunStreamingScenario()
{
    Checks c{"STREAMING"};
    using gs::game::TerrainStreamer;
    using gs::game::TerrainStreamingConfig;
    const auto dir = FixtureDir("stream_world");
    if (const auto written = mx::map::WritePackage(dir, SpecStream()); !written.ok) {
        c.Report("fixture-write", false, written.error);
        return c.failures;
    }
    auto streamed = LoadPackage(dir, mx::map::ResidencyMode::Streaming);
    auto reference = LoadPackage(dir, mx::map::ResidencyMode::Eager);
    if (!streamed.world || !reference.world) {
        c.Report("fixture-load", false, "streamed: " + streamed.First() + " | reference: " + reference.First());
        return c.failures;
    }
    const mx::map::ServerTerrain& ref = reference.world->terrain;
    c.Report("fixture-load",
             streamed.report.chunks_checked == 64 &&
                 streamed.report.chunks_decoded == streamed.world->startup_chunks.size() &&
                 streamed.world->terrain.ResidentChunkCount() == streamed.world->startup_chunks.size() &&
                 reference.report.chunks_decoded == 64 && ref.ResidentChunkCount() == 64,
             Fmt("streaming startup: %u chunk files size-checked, %u decoded (startup set = %zu: spawn region "
                 "centre), resident %zu B | eager reference: %u decoded, resident %zu B",
                 streamed.report.chunks_checked, streamed.report.chunks_decoded,
                 streamed.world->startup_chunks.size(), streamed.world->terrain.ResidentBytes(),
                 reference.report.chunks_decoded, ref.ResidentBytes()));

    // ================= A. streamer against a scripted source =================
    TerrainStreamingConfig base;
    base.budget_bytes = 8u << 20;
    base.io_threads = 2;
    base.retain_seconds = 0.2;
    base.retry_backoff_seconds = 0.01;
    {
        auto rig = MakeRig(dir, base);
        auto& s = *rig->streamer;
        auto& t = *rig->terrain;
        // A1 cold request, hit, miss; bitwise equal to the reference.
        s.Demand(0, Clock::now());
        const bool ready = PumpUntil(s, [&] { return s.State(0) == ChunkState::Ready; });
        s.Demand(0, Clock::now());
        s.PublishStats();
        const auto st = s.GetStats();
        const bool same0 = SameChunk(t.Published(0), ref.Published(0));
        c.Report("cold-miss-then-hit-bitwise",
                 ready && same0 && st.misses >= 1 && st.hits >= 1 && st.loads_completed == 1 &&
                     rig->source->Calls(0) == 1,
                 Fmt("ready=%d same_as_reference=%d misses=%llu hits=%llu loads=%llu source_calls=%d "
                     "latency_p50=%.2fms",
                     ready ? 1 : 0, same0 ? 1 : 0, static_cast<unsigned long long>(st.misses),
                     static_cast<unsigned long long>(st.hits), static_cast<unsigned long long>(st.loads_completed),
                     rig->source->Calls(0), st.load_ms_p50));
        // A2 many requesters of one chunk -> one load.
        for (int i = 0; i < 100; ++i) {
            s.Demand(1, Clock::now());
        }
        const bool ready1 = PumpUntil(s, [&] { return s.State(1) == ChunkState::Ready; });
        const auto st2 = s.GetStats();
        c.Report("same-chunk-many-requesters-one-load",
                 ready1 && rig->source->Calls(1) == 1 && st2.deduplicated >= 99,
                 Fmt("100 demands -> source loads=%d deduplicated=%llu", rig->source->Calls(1),
                     static_cast<unsigned long long>(st2.deduplicated)));
        // A3 out-of-order completion.
        rig->source->SetDelay(2, 150ms);
        s.Demand(2, Clock::now());
        s.Demand(3, Clock::now());
        const bool both = PumpUntil(
            s, [&] { return s.State(2) == ChunkState::Ready && s.State(3) == ChunkState::Ready; }, 5000ms, true,
            [&] {
                s.Demand(2, Clock::now());
                s.Demand(3, Clock::now());
            });
        const auto order = rig->source->Order();
        const auto pos2 = std::find(order.begin(), order.end(), 2u);
        const auto pos3 = std::find(order.begin(), order.end(), 3u);
        c.Report("out-of-order-completion",
                 both && pos3 < pos2 && SameChunk(t.Published(2), ref.Published(2)) &&
                     SameChunk(t.Published(3), ref.Published(3)),
                 Fmt("requested 2 then 3; completed %s first; both published and equal to the reference",
                     pos3 < pos2 ? "3" : "2"));
        // A4 bounded retry, then published invalid.
        rig->source->FailAlways(4);
        const bool failed = PumpUntil(
            s, [&] { return s.GetStats().permanent_failures >= 1; }, 5000ms, true,
            [&] { s.Demand(4, Clock::now()); });
        PumpUntil(s, [] { return false; }, 150ms, true, [&] { s.Demand(4, Clock::now()); });
        const auto& g = t.Geometry();
        const double c4x = g.origin_x + (4 + 0.5) * kChunk * kCell;
        const double c4y = g.origin_y + 0.5 * kChunk * kCell;
        const auto h4 = t.Height(c4x, c4y);
        const auto cell4 = t.Cell(c4x, c4y);
        c.Report("failed-chunk-bounded-retry-then-invalid",
                 failed && rig->source->Calls(4) == 3 && h4.status == TerrainStatus::InvalidData &&
                     cell4.status == TerrainStatus::InvalidData && !cell4.Walkable(),
                 Fmt("source calls=%d (max_attempts=3, then none despite demand) query=%s cell=%s walkable=%d",
                     rig->source->Calls(4), mx::map::ToString(h4.status), mx::map::ToString(cell4.status),
                     cell4.Walkable() ? 1 : 0));
        // A4b the north neighbour of the INVALID chunk loads: its seam check
        // skips the sample-less neighbour (it used to index the empty
        // arrays), and a raw vertex query on the invalid chunk is InvalidData.
        const bool north = PumpUntil(s, [&] { return s.State(12) == ChunkState::Ready; }, 5000ms, true,
                                     [&] { s.Demand(12, Clock::now()); });
        const auto v4 = t.Vertex(4 * kChunk + 5, 5);
        c.Report("neighbour-of-invalid-chunk-loads",
                 north && SameChunk(t.Published(12), ref.Published(12)) && v4.status == TerrainStatus::InvalidData,
                 Fmt("chunk 12 (north of invalid 4) ready=%d equal to reference=%d; Vertex inside chunk 4: %s",
                     north ? 1 : 0, SameChunk(t.Published(12), ref.Published(12)) ? 1 : 0,
                     mx::map::ToString(v4.status)));
        // A13 seam mismatch against a published neighbour: 8 is loaded first,
        // then 9 arrives with a corrupted west border sample.
        PumpUntil(s, [&] { return s.State(8) == ChunkState::Ready; }, 5000ms, true,
                  [&] { s.Demand(8, Clock::now()); });
        rig->source->CorruptBorder(9);
        const bool seam = PumpUntil(
            s, [&] { return s.GetStats().seam_rejects >= 1; }, 5000ms, true, [&] {
                s.Demand(8, Clock::now());
                s.Demand(9, Clock::now());
            });
        c.Report("seam-mismatch-rejected-at-publication", seam && s.State(9) != ChunkState::Ready,
                 Fmt("seam_rejects=%llu state(9)=%s (never published as valid)",
                     static_cast<unsigned long long>(s.GetStats().seam_rejects), gs::game::ToString(s.State(9))));
        c.Report("io-concurrency-bounded", rig->source->MaxConcurrent() <= static_cast<int>(base.io_threads),
                 Fmt("max concurrent loads=%d io_threads=%u queue_high_water=%zu max_in_flight=%u",
                     rig->source->MaxConcurrent(), base.io_threads, s.GetStats().queue_high_water,
                     base.max_in_flight));
    }
    // A5 a corrupted chunk file (real CRC path through the package source).
    {
        const auto corrupt_dir = FixtureDir("stream_world_corrupt");
        (void)mx::map::WritePackage(corrupt_dir, SpecStream());
        auto rig = MakeRig(corrupt_dir, base);
        {
            std::fstream f(corrupt_dir / "chunks" / "chunk_5_0.mxchunk", std::ios::in | std::ios::out | std::ios::binary);
            f.seekp(200);
            const char byte = 0x5a;
            f.write(&byte, 1);
        }
        auto& s = *rig->streamer;
        const bool failed = PumpUntil(
            s, [&] { return s.GetStats().permanent_failures >= 1; }, 5000ms, true,
            [&] { s.Demand(5, Clock::now()); });
        const auto st = s.GetStats();
        const auto* invalid = rig->terrain->Published(5);
        c.Report("corrupt-chunk-file-crc-then-invalid",
                 failed && st.load_failures == 3 && invalid != nullptr && invalid->heights16.empty(),
                 Fmt("startup accepted the size-correct file; on load: failures=%llu permanent=%llu -> queries "
                     "InvalidData",
                     static_cast<unsigned long long>(st.load_failures),
                     static_cast<unsigned long long>(st.permanent_failures)));
    }
    // A6 budget: more demand than room -> bounded, never above the budget.
    {
        auto probe = MakeRig(dir, base);
        const std::size_t reservation = ChunkReservation(*probe->terrain, *probe->source, 0);
        probe->streamer->PublishStats();
        const std::size_t fixed = probe->streamer->GetStats().accounted_bytes; // metadata + startup set
        probe.reset();
        TerrainStreamingConfig tight = base;
        tight.budget_bytes = fixed + 6 * reservation; // ~6 loading or ~12 resident chunks
        tight.retain_seconds = 0.3;
        tight.max_in_flight = 4;
        auto rig = MakeRig(dir, tight);
        auto& s = *rig->streamer;
        std::size_t peak = 0;
        std::vector<std::uint32_t> wanted;
        for (std::uint32_t i = 16; i < 48; ++i) {
            wanted.push_back(i); // 32 chunks wanted, room for ~12
        }
        auto demand_all = [&] {
            for (const auto i : wanted) {
                s.Demand(i, Clock::now());
            }
            s.PublishStats();
            peak = std::max(peak, s.GetStats().accounted_bytes);
        };
        PumpUntil(s, [] { return false; }, 1500ms, true, demand_all);
        const auto full = s.GetStats();
        c.Report("demand-above-budget-degrades-bounded",
                 peak <= tight.budget_bytes && full.admission_waits > 0 &&
                     full.resident < wanted.size() + rig->startup.size() && full.pressure_evictions > 0 &&
                     full.loads_completed > wanted.size(),
                 Fmt("wanted=32 budget=%zu B peak=%zu B resident=%zu waiting=%zu admission_waits=%llu evictions=%llu "
                     "(soft demand yields under pressure; bounded cache keeps serving the larger union)",
                     tight.budget_bytes, peak, full.resident, full.waiting,
                     static_cast<unsigned long long>(full.admission_waits),
                     static_cast<unsigned long long>(full.evictions)));
        // The consumers move on to another area (40..47, not loaded above):
        // after the retention window the old chunks are evicted, the new
        // area loads within the same budget.
        wanted.clear();
        for (std::uint32_t i = 40; i < 48; ++i) {
            wanted.push_back(i);
        }
        peak = 0;
        const bool settled = PumpUntil(
            s,
            [&] {
                for (const auto i : wanted) {
                    if (s.State(i) != ChunkState::Ready) {
                        return false;
                    }
                }
                return true;
            },
            5000ms, true, demand_all);
        const auto after = s.GetStats();
        c.Report("eviction-frees-room-for-waiting-demand",
                 settled && after.evictions > 0 && peak <= tight.budget_bytes,
                 Fmt("new area 40..47 -> all resident=%d evictions=%llu frees=%llu peak=%zu B <= budget %zu",
                     settled ? 1 : 0, static_cast<unsigned long long>(after.evictions),
                     static_cast<unsigned long long>(after.frees), peak, tight.budget_bytes));
        // A7 pinned data above the budget: readers pin every resident chunk,
        // nothing is demanded any more, a new area is wanted.
        std::vector<std::shared_ptr<const mx::map::TerrainChunk>> pins;
        for (std::uint32_t i = 0; i < 64; ++i) {
            if (auto pin = rig->terrain->Owned(i)) {
                pins.push_back(std::move(pin));
            }
        }
        wanted.clear();
        for (std::uint32_t i = 48; i < 64; ++i) {
            wanted.push_back(i);
        }
        peak = 0;
        std::this_thread::sleep_for(350ms); // past the retention window of the old area
        PumpUntil(s, [] { return false; }, 800ms, true, demand_all);
        const auto pinned = s.GetStats();
        std::size_t still = 0;
        for (const auto& pin : pins) {
            still += rig->terrain->Published(pin->y * 8 + pin->x) == pin.get() ? 1u : 0u;
        }
        c.Report("pinned-above-budget-no-eviction-no-overrun",
                 peak <= tight.budget_bytes && still == pins.size() && pinned.admission_waits > after.admission_waits,
                 Fmt("pins=%zu still published=%zu new demand waits (admission_waits %llu -> %llu) peak=%zu <= "
                     "budget=%zu",
                     pins.size(), still, static_cast<unsigned long long>(after.admission_waits),
                     static_cast<unsigned long long>(pinned.admission_waits), peak, tight.budget_bytes));
        pins.clear(); // readers done
        const bool moved = PumpUntil(s, [&] { return s.State(48) == ChunkState::Ready; }, 5000ms, true, demand_all);
        c.Report("released-pins-become-evictable", moved,
                 Fmt("after the readers released their pins the new area loads: state(48)=%s",
                     gs::game::ToString(s.State(48))));
    }
    // A8 lifetime: unpublished while a tick may still read -> freed only in a
    // quiescent window; a pinned chunk is never evicted.
    {
        TerrainStreamingConfig lc = base;
        lc.retain_seconds = 0.1; // the minimum (one consumer re-demand period + margin)
        auto rig = MakeRig(dir, lc);
        auto& s = *rig->streamer;
        const bool loaded10 = PumpUntil(s, [&] { return s.State(10) == ChunkState::Ready; }, 5000ms, true,
                                        [&] { s.Demand(10, Clock::now()); });
        std::weak_ptr<const mx::map::TerrainChunk> weak = rig->terrain->Owned(10);
        auto pin = rig->terrain->Owned(10);
        const std::size_t kept = s.EvictUndemanded(Clock::now() + 1s, false);
        const bool survived_pin = rig->terrain->Published(10) == pin.get();
        pin.reset();
        const std::size_t evicted = s.EvictUndemanded(Clock::now() + 1s, false); // a tick "in flight"
        const bool unpublished = rig->terrain->Published(10) == nullptr;
        const bool alive_until_quiescent = !weak.expired();
        s.Pump(Clock::now(), true); // quiescent window
        const bool freed = weak.expired();
        c.Report("evicted-chunk-freed-only-in-quiescent-window",
                 loaded10 && kept == 0 && survived_pin && evicted >= 1 && unpublished && alive_until_quiescent && freed,
                 Fmt("pinned: evicted=%zu still published=%d | unpinned: evicted=%zu unpublished=%d memory alive "
                     "while a tick may read=%d freed at the quiescent pump=%d",
                     kept, survived_pin ? 1 : 0, evicted, unpublished ? 1 : 0, alive_until_quiescent ? 1 : 0,
                     freed ? 1 : 0));
    }
    // A8b no over-eviction outside quiescent windows: an evicted chunk stays
    // accounted (retired) until the next quiescent free, so repeated
    // non-quiescent passes must not keep evicting everything evictable.
    {
        auto probe = MakeRig(dir, base);
        const std::size_t reservation = ChunkReservation(*probe->terrain, *probe->source, 0);
        const std::size_t resident_chunk = probe->terrain->ChunkBytes(0) + sizeof(mx::map::TerrainChunk) + 64;
        probe->streamer->PublishStats();
        const std::size_t fixed = probe->streamer->GetStats().accounted_bytes;
        probe.reset();
        TerrainStreamingConfig oc = base;
        oc.retain_seconds = 0.1;
        oc.budget_bytes = fixed + 10 * resident_chunk + reservation / 2; // 10 resident, no room for a load
        auto rig = MakeRig(dir, oc);
        auto& s = *rig->streamer;
        std::vector<std::uint32_t> ten;
        for (std::uint32_t i = 16; i < 26; ++i) {
            ten.push_back(i);
        }
        PumpUntil(
            s,
            [&] {
                return std::all_of(ten.begin(), ten.end(), [&](std::uint32_t i) { return s.State(i) == ChunkState::Ready; });
            },
            5000ms, true, [&] {
                for (const auto i : ten) {
                    s.Demand(i, Clock::now());
                }
            });
        std::this_thread::sleep_for(200ms); // the ten are no longer demanded
        s.PublishStats();
        const auto before = s.GetStats();
        for (int pass = 0; pass < 6; ++pass) {
            s.Demand(50, Clock::now());
            s.Pump(Clock::now(), false); // ticks "in flight": nothing can be freed
        }
        s.PublishStats();
        const auto during = s.GetStats();
        const bool loaded = PumpUntil(s, [&] { return s.State(50) == ChunkState::Ready; }, 5000ms, true,
                                      [&] { s.Demand(50, Clock::now()); });
        const std::uint64_t evicted = during.evictions - before.evictions;
        c.Report("no-over-eviction-outside-quiescent-window",
                 evicted >= 1 && evicted <= 3 && during.resident >= before.resident - 3 && loaded,
                 Fmt("10 idle resident chunks, a new one wanted, 6 non-quiescent passes: evicted %llu (just enough "
                     "for one reservation, not all 10), retired %zu B awaiting the free; after a quiescent pass the "
                     "new chunk loaded=%d",
                     static_cast<unsigned long long>(evicted), during.retired_bytes, loaded ? 1 : 0));
    }
    // A9 cancellation: queued loads whose last consumer left are cancelled
    // before any read; chunks another consumer still wants load.
    {
        TerrainStreamingConfig cc = base;
        cc.io_threads = 1;
        cc.max_in_flight = 4;
        cc.retain_seconds = 0.15;
        auto rig = MakeRig(dir, cc);
        auto& s = *rig->streamer;
        rig->source->Gate(true);
        for (const std::uint32_t i : {20u, 21u, 22u, 23u}) {
            s.Demand(i, Clock::now());
        }
        s.Pump(Clock::now(), true); // 20..23 admitted; the single worker blocks on 20
        std::this_thread::sleep_for(50ms);
        PumpUntil(s, [] { return false; }, 400ms, true, [&] {
            s.Demand(20, Clock::now());
            s.Demand(23, Clock::now());
        });
        rig->source->OpenAll();
        const bool done = PumpUntil(
            s, [&] { return s.State(20) == ChunkState::Ready && s.State(23) == ChunkState::Ready; }, 5000ms, true,
            [&] {
                s.Demand(20, Clock::now());
                s.Demand(23, Clock::now());
            });
        const auto st = s.GetStats();
        c.Report("cancel-last-consumer-keeps-other-consumers-load",
                 done && st.cancelled >= 2 && rig->source->Calls(21) == 0 && rig->source->Calls(22) == 0,
                 Fmt("cancelled=%llu reads of 21/22=%d/%d; 20 and 23 (still wanted) ready=%d",
                     static_cast<unsigned long long>(st.cancelled), rig->source->Calls(21), rig->source->Calls(22),
                     done ? 1 : 0));
    }
    // A10 a late completion of a superseded generation is rejected.
    {
        TerrainStreamingConfig sc = base;
        sc.io_threads = 1;
        auto rig = MakeRig(dir, sc);
        auto& s = *rig->streamer;
        rig->source->Gate(true);
        s.Demand(30, Clock::now());
        s.Pump(Clock::now(), true);
        WaitFor(2000ms, [&] { return rig->source->Concurrent() == 1; }); // the read is in flight
        s.ResetGenerationForTest(true);
        rig->source->OpenAll();
        const bool stale = PumpUntil(s, [&] { return s.GetStats().stale_completions >= 1; });
        const bool not_published = rig->terrain->Published(30) == nullptr;
        const bool reload = PumpUntil(s, [&] { return s.State(30) == ChunkState::Ready; }, 5000ms, true,
                                      [&] { s.Demand(30, Clock::now()); });
        c.Report("late-completion-after-generation-reset-rejected",
                 stale && not_published && reload && SameChunk(rig->terrain->Published(30), ref.Published(30)),
                 Fmt("stale_completions=%llu published_from_stale=%d reloaded_in_new_generation=%d",
                     static_cast<unsigned long long>(s.GetStats().stale_completions), not_published ? 0 : 1,
                     reload ? 1 : 0));
    }
    // A10b a chunk published INVALID belongs to its package generation: a
    // generation change retires it (not freed while a tick may still read
    // it) and a fresh load of the repaired chunk publishes valid data.
    {
        auto rig = MakeRig(dir, base);
        auto& s = *rig->streamer;
        rig->source->FailAlways(6);
        const bool invalid = PumpUntil(
            s, [&] { return s.GetStats().permanent_failures >= 1; }, 5000ms, true,
            [&] { s.Demand(6, Clock::now()); });
        const auto* published = rig->terrain->Published(6);
        const bool was_invalid = published != nullptr && published->heights16.empty();
        std::weak_ptr<const mx::map::TerrainChunk> weak = rig->terrain->Owned(6);
        s.ResetGenerationForTest(false); // a zone tick "in flight"
        const bool unpublished = rig->terrain->Published(6) == nullptr;
        const bool alive = !weak.expired();
        rig->source->ClearFailure(6);
        const bool reloaded = PumpUntil(s, [&] { return s.State(6) == ChunkState::Ready; }, 5000ms, true,
                                        [&] { s.Demand(6, Clock::now()); });
        const bool freed = weak.expired();
        const bool valid = SameChunk(rig->terrain->Published(6), ref.Published(6));
        c.Report("invalid-chunk-generation-reset-retired-then-valid-reload",
                 invalid && was_invalid && unpublished && alive && reloaded && freed && valid,
                 Fmt("invalid published=%d; reset during a tick: unpublished=%d still alive (retired)=%d; repaired "
                     "chunk reloaded=%d equal to reference=%d; old invalid freed at the quiescent pump=%d",
                     was_invalid ? 1 : 0, unpublished ? 1 : 0, alive ? 1 : 0, reloaded ? 1 : 0, valid ? 1 : 0,
                     freed ? 1 : 0));
    }
    // A10c the waiting list is bounded: with no room for a single load, 10
    // distinct demands leave max_waiting (4) waiting and 6 explicitly
    // rejected -- no allocation, the consumers simply demand again later.
    {
        TerrainStreamingConfig wc = base;
        wc.max_waiting = 4;
        auto probe = MakeRig(dir, wc);
        const std::size_t reservation = ChunkReservation(*probe->terrain, *probe->source, 0);
        probe->streamer->PublishStats();
        const std::size_t fixed = probe->streamer->GetStats().accounted_bytes;
        probe.reset();
        wc.budget_bytes = fixed + reservation / 2;
        auto rig = MakeRig(dir, wc);
        auto& s = *rig->streamer;
        for (std::uint32_t i = 16; i < 26; ++i) {
            s.Demand(i, Clock::now());
        }
        s.Pump(Clock::now(), true);
        s.PublishStats();
        const auto st = s.GetStats();
        c.Report("waiting-list-full-rejects-explicitly",
                 st.waiting == 4 && st.admission_rejects == 6 && st.loading == 0 &&
                     st.accounted_bytes <= wc.budget_bytes && rig->source->Calls(16) == 0,
                 Fmt("10 demands, no room, max_waiting=4: waiting=%zu rejected=%llu loading=%zu accounted=%zu <= "
                     "budget=%zu",
                     st.waiting, static_cast<unsigned long long>(st.admission_rejects), st.loading,
                     st.accounted_bytes, wc.budget_bytes));
    }
    // A11 shutdown with reads in flight: bounded, nothing published after.
    {
        TerrainStreamingConfig sc = base;
        sc.io_threads = 2;
        auto rig = MakeRig(dir, sc);
        auto& s = *rig->streamer;
        rig->source->Gate(true);
        for (const std::uint32_t i : {40u, 41u, 42u, 43u, 44u}) {
            s.Demand(i, Clock::now());
        }
        s.Pump(Clock::now(), true);
        WaitFor(2000ms, [&] { return rig->source->Concurrent() == 2; });
        const auto t0 = Clock::now();
        std::thread releaser([&] {
            std::this_thread::sleep_for(50ms);
            rig->source->OpenAll(); // the in-flight reads finish
        });
        s.Stop();
        releaser.join();
        const double stop_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        std::size_t published = 0;
        for (const std::uint32_t i : {40u, 41u, 42u, 43u, 44u}) {
            published += rig->terrain->Published(i) != nullptr ? 1u : 0u;
        }
        const int queued_reads = rig->source->Calls(42) + rig->source->Calls(43) + rig->source->Calls(44);
        c.Report("shutdown-with-pending-io",
                 stop_ms < 2000.0 && published == 0 && queued_reads == 0,
                 Fmt("stop took %.0f ms (waits only for the 2 reads in flight); queued loads dropped (reads of "
                     "42..44: %d); published after stop: %zu",
                     stop_ms, queued_reads, published));
    }

    // ================= C. the runtime on a streaming world =================
    struct RuntimeRig {
        IoRunner runner; // outlives the runtime
        std::shared_ptr<ScriptedSource> source;
        std::unique_ptr<gs::game::WorldRuntime> sim;
    };
    auto make_runtime = [&](const TerrainStreamingConfig& cfg) {
        auto loaded = LoadPackage(dir, mx::map::ResidencyMode::Streaming);
        auto rig = std::make_unique<RuntimeRig>();
        rig->source = std::make_shared<ScriptedSource>(loaded.world->chunk_source);
        loaded.world->chunk_source = rig->source;
        gs::game::PartitionLayout layout;
        layout.regions_x = layout.regions_y = 1;
        rig->sim = std::make_unique<gs::game::WorldRuntime>(rig->runner.io, gs::game::RuntimeIdentity{},
                                                            std::move(*loaded.world), layout, cfg);
        gs::game::PartitionConfig partition;
        partition.scoring.adaptive_enabled = false;
        rig->sim->ConfigurePartition(partition);
        return rig;
    };
    struct WorldView {
        std::size_t entities = 0;
        std::size_t mobs = 0;
        std::size_t leaves = 0;
        std::vector<std::uint32_t> net_ids;
        bool duplicate = false;
        std::size_t z_mismatch = 0;
        std::size_t points_ok = 0;
        std::size_t points_not_resident = 0;
        std::size_t points_bad = 0; // any other status or a height different from the reference
    };
    // Supervisor-side: entities, every entity's z vs the reference provider,
    // and sampled world queries (Ok must equal the reference bit for bit,
    // anything else must be NotResident).
    auto view_world = [&](gs::game::WorldRuntime& sim, std::uint32_t seed) {
        return ReadWorld(sim, [&](const WorldSnapshot& snap) {
            WorldView v;
            std::unordered_set<std::uint32_t> seen;
            for (std::size_t i = 0; i < snap.zones.ZoneCount(); ++i) {
                for (const auto& [net_id, entity] : snap.zones.GetZone(i).Entities()) {
                    ++v.entities;
                    v.duplicate = v.duplicate || !seen.insert(net_id).second;
                    v.net_ids.push_back(net_id);
                    if (entity.has<gs::game::MobTag>()) {
                        ++v.mobs;
                    }
                    const auto pos = entity.get<gs::game::Position>();
                    const auto h = ref.Height(pos.x, pos.y);
                    v.z_mismatch += h.Ok() && h.meters == pos.z ? 0u : 1u;
                }
            }
            std::sort(v.net_ids.begin(), v.net_ids.end());
            v.leaves = snap.zones.GetActiveLeaves().size();
            std::mt19937 rng(seed);
            std::uniform_real_distribution<float> u(-2048.0f, 2047.9f);
            for (int i = 0; i < 400; ++i) {
                const float x = u(rng);
                const float y = u(rng);
                const auto h = sim.Terrain().Height(x, y);
                if (h.status == TerrainStatus::Ok) {
                    const auto r = ref.Height(x, y);
                    (r.Ok() && r.meters == h.meters ? v.points_ok : v.points_bad) += 1;
                } else if (h.status == TerrainStatus::NotResident) {
                    ++v.points_not_resident;
                } else {
                    ++v.points_bad;
                }
            }
            return v;
        });
    };
    auto read_player = [&](gs::game::WorldRuntime& sim, gs::common::SessionId session) {
        return ReadWorld(sim, [session](const WorldSnapshot& snap) {
            std::optional<gs::game::Position> out;
            const auto it = snap.owners.find(session);
            if (it != snap.owners.end() && it->second.zone_index < snap.zones.ZoneCount()) {
                const auto e = snap.zones.GetZone(it->second.zone_index).FindEntity(it->second.net_id);
                if (e.is_valid()) {
                    out = e.get<gs::game::Position>();
                }
            }
            return out;
        });
    };
    {
        TerrainStreamingConfig cfg = base;
        cfg.budget_bytes = 700u << 10; // ~30 resident chunks of 64
        cfg.retain_seconds = 1.0;
        auto rig = make_runtime(cfg);
        // The runtime's own package load happened above; from here on only
        // chunk loads may happen -- never another package load.
        const std::uint64_t package_loads0 = mx::map::PackageLoadCount();
        auto& sim = *rig->sim;
        const auto boot = sim.GetTerrainStats();
        const auto boot_view = view_world(sim, 1);
        c.Report("runtime-startup-spawn-batches-within-budget",
                 boot.streaming && boot_view.mobs == 120 && boot.streamer.peak_accounted_bytes <= cfg.budget_bytes &&
                     boot.streamer.loads_completed > 0 && boot.streamer.evictions > 0 && boot_view.z_mismatch == 0,
                 Fmt("mobs=%zu/120 spawned from 4 circles in batches: loads=%llu evictions=%llu peak=%zu <= "
                     "budget=%zu B; every z equals the reference height (mismatches=%zu)",
                     boot_view.mobs, static_cast<unsigned long long>(boot.streamer.loads_completed),
                     static_cast<unsigned long long>(boot.streamer.evictions), boot.streamer.peak_accounted_bytes,
                     cfg.budget_bytes, boot_view.z_mismatch));
        // Four players enter at the (pinned) spawn region centre (5, 5) --
        // 5 m from the x = 0 and y = 0 chunk borders -- and run W / S / SW / N.
        const struct {
            gs::common::SessionId session;
            float heading;
        } runners[] = {{95001, -1.5707963f}, {95002, 3.1415926f}, {95003, -2.3561944f}, {95004, 0.0f}};
        std::uint64_t index = 0;
        for (const auto& r : runners) {
            sim.PostSpawn(DetachedSession(rig->runner.io, r.session), MakeCharacter(index++), std::nullopt);
        }
        sim.Start();
        WaitFor(10000ms, [&] {
            return ReadWorld(sim, [](const WorldSnapshot& snap) { return snap.owners.size(); }) == 4;
        });
        std::uint32_t seq = 0;
        const auto run_until = Clock::now() + 4s;
        while (Clock::now() < run_until) {
            for (const auto& r : runners) {
                sim.PostMoveInput(r.session, ++seq, r.heading, gs::game::MoveState::Running);
            }
            std::this_thread::sleep_for(50ms);
        }
        for (const auto& r : runners) {
            sim.PostMoveInput(r.session, ++seq, r.heading, gs::game::MoveState::Idle);
        }
        std::this_thread::sleep_for(200ms);
        const auto w = read_player(sim, 95001);
        const auto south = read_player(sim, 95002);
        const auto sw = read_player(sim, 95003);
        const auto run = sim.GetTerrainStats();
        const auto v1 = view_world(sim, 2);
        c.Report("players-cross-chunk-borders-streamed-heights-exact",
                 w && south && sw && w->x < -10.0f && south->y < -10.0f && sw->x < -5.0f && sw->y < -5.0f &&
                     v1.z_mismatch == 0 && v1.points_bad == 0 && !v1.duplicate,
                 Fmt("W x=%.1f S y=%.1f SW (%.1f,%.1f) crossed the x=0 / y=0 chunk borders; z==reference for all "
                     "%zu entities (mismatch=%zu); 400 sampled queries: ok(bit-equal)=%zu not-resident=%zu bad=%zu",
                     w ? w->x : 0.0f, south ? south->y : 0.0f, sw ? sw->x : 0.0f, sw ? sw->y : 0.0f, v1.entities,
                     v1.z_mismatch, v1.points_ok, v1.points_not_resident, v1.points_bad));
        c.Report("prefetch-demand-from-movement",
                 run.streamer.loads_completed > boot.streamer.loads_completed && run.queries.ok > 0,
                 Fmt("lookahead=%.2fs load p50/p99=%.2f/%.2f ms; steps: ok=%llu waiting-for-data=%llu blocked=%llu "
                     "outside=%llu; hits=%llu misses=%llu dedup=%llu",
                     run.streamer.lookahead_seconds, run.streamer.load_ms_p50, run.streamer.load_ms_p99,
                     static_cast<unsigned long long>(run.queries.ok),
                     static_cast<unsigned long long>(run.queries.steps_waiting),
                     static_cast<unsigned long long>(run.queries.steps_blocked),
                     static_cast<unsigned long long>(run.queries.outside),
                     static_cast<unsigned long long>(run.streamer.hits),
                     static_cast<unsigned long long>(run.streamer.misses),
                     static_cast<unsigned long long>(run.streamer.deduplicated)));

        // Split/merge with slow I/O while players criss-cross chunk AND zone
        // borders (the first cut is x = 0 / y = 0, a chunk border too).
        rig->source->SetGlobalDelay(30ms);
        std::atomic<bool> moving{true};
        std::thread mover([&] {
            std::uint32_t s = 100000;
            int step = 0;
            while (moving.load()) {
                const float sign = (step / 30) % 2 == 0 ? 1.0f : -1.0f;
                for (const auto& r : runners) {
                    sim.PostMoveInput(r.session, ++s, sign > 0 ? r.heading + 3.1415926f : r.heading,
                                      gs::game::MoveState::Running);
                }
                ++step;
                std::this_thread::sleep_for(50ms);
            }
        });
        const auto base_view = view_world(sim, 3);
        bool topology_ok = true;
        std::string topology;
        auto check = [&](const char* step, std::size_t expect_leaves) {
            const auto v = view_world(sim, 4);
            const std::string audit = AuditNow(sim);
            const bool ok = v.leaves == expect_leaves && v.net_ids == base_view.net_ids && !v.duplicate &&
                            v.z_mismatch == 0 && v.points_bad == 0 && audit == "OK";
            topology_ok = topology_ok && ok;
            topology += Fmt(" %s:leaves=%zu same_entities=%d zbad=%zu qbad=%zu audit=%s;", step, v.leaves,
                            v.net_ids == base_view.net_ids ? 1 : 0, v.z_mismatch, v.points_bad, audit.c_str());
        };
        auto leaf_ids = [&] {
            return ReadWorld(sim, [](const WorldSnapshot& snap) {
                std::vector<gs::game::ZoneId> ids;
                for (const auto* leaf : snap.zones.GetActiveLeaves()) {
                    ids.push_back(leaf->zone_id);
                }
                return ids;
            });
        };
        auto force = [&](bool split, gs::game::ZoneId id) {
            const auto m0 = sim.PartitionMetricsSnapshot();
            split ? sim.PostForceSplit(id) : sim.PostForceMerge(id);
            return WaitFor(15000ms, [&] {
                const auto m = sim.PartitionMetricsSnapshot();
                return split ? m.split_commits > m0.split_commits : m.merge_commits > m0.merge_commits;
            });
        };
        check("initial", 1);
        const auto level0 = leaf_ids();
        bool steps = force(true, level0.front());
        check("split-4", 4);
        const auto level1 = leaf_ids();
        for (const auto id : level1) {
            steps = force(true, id) && steps;
        }
        check("split-16", 16);
        for (const auto id : level1) {
            steps = force(false, id) && steps;
        }
        check("merge-4", 4);
        steps = force(false, level0.front()) && steps;
        check("merge-1", 1);
        moving.store(false);
        mover.join();
        c.Report("split-merge-with-slow-io-no-duplication-no-reload",
                 steps && topology_ok && mx::map::PackageLoadCount() == package_loads0,
                 Fmt("1->4->16->4->1 with 30 ms reads:%s package loads %llu -> %llu (chunk loads are not package "
                     "loads)",
                     topology.c_str(), static_cast<unsigned long long>(package_loads0),
                     static_cast<unsigned long long>(mx::map::PackageLoadCount())));

        // Simulated package switch while loads are in flight: stale
        // completions are rejected, consumers re-demand, heights stay exact.
        sim.PostTerrainDemand(0.0f, 0.0f, 2000.0f);
        std::this_thread::sleep_for(40ms);
        const auto pre = sim.GetTerrainStats();
        sim.PostTerrainResetForTest();
        std::this_thread::sleep_for(1500ms);
        const auto post = sim.GetTerrainStats();
        const auto v2 = view_world(sim, 5);
        c.Report("generation-reset-at-runtime-stale-rejected",
                 post.streamer.generation > pre.streamer.generation && v2.points_bad == 0 && v2.z_mismatch == 0 &&
                     post.streamer.resident > 0,
                 Fmt("generation %llu -> %llu, stale completions %llu -> %llu, resident again=%zu, queries bad=%zu",
                     static_cast<unsigned long long>(pre.streamer.generation),
                     static_cast<unsigned long long>(post.streamer.generation),
                     static_cast<unsigned long long>(pre.streamer.stale_completions),
                     static_cast<unsigned long long>(post.streamer.stale_completions), post.streamer.resident,
                     v2.points_bad));

        // Shutdown with loads pending (slow reads).
        rig->source->SetGlobalDelay(250ms);
        sim.PostTerrainDemand(0.0f, 0.0f, 2000.0f);
        std::this_thread::sleep_for(60ms);
        const auto t0 = Clock::now();
        sim.Stop();
        const double stop_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        const auto after = sim.GetTerrainStats();
        c.Report("runtime-shutdown-with-pending-loads", stop_ms < 3000.0,
                 Fmt("stop took %.0f ms with 250 ms reads pending (in-flight reads finish, queued ones are dropped); "
                     "stale=%llu",
                     stop_ms, static_cast<unsigned long long>(after.streamer.stale_completions)));
    }
    // Movement waits for missing data instead of guessing: no prefetch
    // (lookahead 0), 150 ms reads; the stop state survives.
    {
        TerrainStreamingConfig cfg = base;
        cfg.lookahead_min_seconds = 0.0;
        cfg.lookahead_max_seconds = 0.0;
        cfg.retain_seconds = 5.0;
        auto rig = make_runtime(cfg);
        auto& sim = *rig->sim;
        rig->source->SetGlobalDelay(150ms);
        sim.PostSpawn(DetachedSession(rig->runner.io, 96001), MakeCharacter(50), std::nullopt);
        sim.Start();
        WaitFor(5000ms, [&] { return read_player(sim, 96001).has_value(); });
        std::uint32_t seq = 0;
        const auto until = Clock::now() + 2500ms;
        while (Clock::now() < until) {
            sim.PostMoveInput(96001, ++seq, -1.5707963f, gs::game::MoveState::Running);
            std::this_thread::sleep_for(50ms);
        }
        sim.PostMoveInput(96001, ++seq, -1.5707963f, gs::game::MoveState::Idle);
        std::this_thread::sleep_for(200ms);
        const auto stopped = read_player(sim, 96001);
        std::this_thread::sleep_for(500ms);
        const auto still = read_player(sim, 96001);
        const auto st = sim.GetTerrainStats();
        c.Report("movement-waits-for-data-no-guess-stop-kept",
                 stopped && still && st.queries.steps_waiting > 0 && stopped->x < -1.0f && stopped->x == still->x &&
                     std::abs(stopped->z - ref.Height(stopped->x, stopped->y).meters) == 0.0f,
                 Fmt("no prefetch, 150 ms reads: steps refused while the next chunk loaded=%llu, then crossed to "
                     "x=%.2f; idle x stays %.2f -> %.2f; z exact",
                     static_cast<unsigned long long>(st.queries.steps_waiting), stopped ? stopped->x : 0.0f,
                     stopped ? stopped->x : 0.0f, still ? still->x : 0.0f));
        sim.Stop();
    }
    std::error_code ec;
    // Keep the generated corpus for the isolated runner's fixture SHA256 manifest.
    (void)ec;
    std::printf("STREAMING-DONE passes=%d failures=%d\n", c.passes, c.failures);
    return c.failures;
}


namespace {

// Tiny-cell collision fixture: 256 x 256 cells of 0.5 m (128 m), 64-cell
// chunks (32 m). Blocked: a one-cell wall column at cell x = 100 (x 50..50.5)
// for cell y < 200; one cell right after the x = 32 chunk border (cell (64,
// 140), y 70..70.5); two diagonal cells (20,21) and (21,20). Heights: flat
// 0 m for x < 80 m, then a 1.2 m/m ramp (0.6 m per 0.5 m sample).
bool TinyBlocked(std::uint32_t cx, std::uint32_t cy)
{
    return (cx == 100 && cy < 200) || (cx == 64 && cy == 140) || (cx == 20 && cy == 21) || (cx == 21 && cy == 20);
}

mx::map::PackageWriteSpec SpecTiny()
{
    mx::map::PackageWriteSpec spec;
    spec.world_id = "map3-tiny";
    spec.world_name = "MAP-3 collision fixture";
    spec.size_cells_x = 256;
    spec.cell_size_m = 0.5f;
    spec.chunk_size_cells = 64;
    spec.height_raw = [](std::uint32_t vx, std::uint32_t) -> std::int32_t {
        return vx <= 160 ? 0 : static_cast<std::int32_t>((vx - 160) * 60);
    };
    spec.attributes = [](std::uint32_t cx, std::uint32_t cy) -> std::uint16_t { return TinyBlocked(cx, cy) ? 1 : 0; };
    spec.logic.spawns = {{1, 0, {10.0f, 60.0f, 12.0f, 62.0f}}};
    spec.water_model = mx::map::WaterModel::None;
    spec.overwrite = true;
    return spec;
}

// Independent path-length oracle: Dijkstra over the cell grid of the
// streaming fixture (walls from WallCell, not from the terrain), 8-connected,
// a diagonal only between two open orthogonal neighbours, inside a window.
float OraclePathLength(std::uint32_t sx, std::uint32_t sy, std::uint32_t gx, std::uint32_t gy, std::uint32_t min_x,
                       std::uint32_t min_y, std::uint32_t max_x, std::uint32_t max_y, float cell)
{
    const std::uint32_t w = max_x - min_x + 1;
    const std::uint32_t h = max_y - min_y + 1;
    std::vector<float> dist(static_cast<std::size_t>(w) * h, std::numeric_limits<float>::infinity());
    using Item = std::pair<float, std::uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    auto id = [&](std::uint32_t x, std::uint32_t y) { return (y - min_y) * w + (x - min_x); };
    auto free = [&](std::int64_t x, std::int64_t y) {
        return x >= min_x && y >= min_y && x <= max_x && y <= max_y &&
               !WallCell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
    };
    dist[id(sx, sy)] = 0.0f;
    open.push({0.0f, id(sx, sy)});
    while (!open.empty()) {
        const auto [d, k] = open.top();
        open.pop();
        if (d > dist[k]) {
            continue;
        }
        const std::uint32_t x = k % w + min_x;
        const std::uint32_t y = k / w + min_y;
        if (x == gx && y == gy) {
            return d;
        }
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if ((dx == 0 && dy == 0) || !free(static_cast<std::int64_t>(x) + dx, static_cast<std::int64_t>(y) + dy)) {
                    continue;
                }
                if (dx != 0 && dy != 0 &&
                    (!free(static_cast<std::int64_t>(x) + dx, y) || !free(x, static_cast<std::int64_t>(y) + dy))) {
                    continue;
                }
                const float step = (dx != 0 && dy != 0) ? cell * 1.41421356f : cell;
                const std::uint32_t n = id(static_cast<std::uint32_t>(static_cast<std::int64_t>(x) + dx),
                                           static_cast<std::uint32_t>(static_cast<std::int64_t>(y) + dy));
                if (d + step < dist[n]) {
                    dist[n] = d + step;
                    open.push({dist[n], n});
                }
            }
        }
    }
    return std::numeric_limits<float>::infinity();
}

gs::game::NavResult RunNav(gs::game::NavigationService& nav, const gs::game::TerrainService& terrain,
                           gs::game::TerrainStreamer* streamer, const gs::game::NavRequest& request,
                           std::uint64_t id, std::uint32_t per_pass = 100000,
                           const std::function<void()>& each_pass = {})
{
    nav.Request(id, request, Clock::now());
    for (int pass = 0; pass < 20000; ++pass) {
        if (each_pass) {
            each_pass();
        }
        nav.Pump(terrain, streamer, Clock::now(), per_pass);
        if (streamer != nullptr) {
            streamer->Pump(Clock::now(), true);
        }
        const auto r = nav.Result(id);
        if (r.status != gs::game::NavStatus::Pending) {
            return r;
        }
        std::this_thread::sleep_for(1ms);
    }
    return nav.Result(id);
}

} // namespace

int RunWorldQueryScenario()
{
    Checks c{"WORLDQUERY"};
    using mx::map::WaterModel;
    using mx::map::WaterStatus;
    using gs::game::StepResult;
    // ---- fixtures ----
    const auto tiny_dir = FixtureDir("tiny");
    const auto bodies_dir = FixtureDir("water_bodies");
    const auto sea_dir = FixtureDir("water_sea");
    const auto none_dir = FixtureDir("water_none");
    const auto undeclared_dir = FixtureDir("water_undeclared");
    (void)mx::map::WritePackage(tiny_dir, SpecTiny());
    (void)mx::map::WritePackage(bodies_dir, SpecStream(WaterModel::Bodies));
    (void)mx::map::WritePackage(sea_dir, SpecStream(WaterModel::SeaLevel));
    (void)mx::map::WritePackage(none_dir, SpecStream(WaterModel::None));
    (void)mx::map::WritePackage(undeclared_dir, SpecStream(WaterModel::Undeclared));
    auto tiny = LoadPackage(tiny_dir, mx::map::ResidencyMode::Eager);
    auto bodies = LoadPackage(bodies_dir, mx::map::ResidencyMode::Eager);
    auto sea = LoadPackage(sea_dir, mx::map::ResidencyMode::Eager);
    auto none = LoadPackage(none_dir, mx::map::ResidencyMode::Eager);
    auto undeclared = LoadPackage(undeclared_dir, mx::map::ResidencyMode::Eager);
    if (!tiny.world || !bodies.world || !sea.world || !none.world || !undeclared.world) {
        c.Report("fixture-load", false,
                 tiny.First() + " | " + bodies.First() + " | " + sea.First() + " | " + none.First() + " | " +
                     undeclared.First());
        return c.failures;
    }
    auto service = [](const Loaded& l, const gs::game::MovementRules& rules = {}) {
        gs::game::TerrainService svc(l.world->terrain.Clone());
        svc.SetWater(l.world->water);
        svc.SetMovementRules(rules);
        return svc;
    };

    // ================= collision =================
    {
        const auto& t = tiny.world->terrain;
        const gs::game::TerrainService svc = service(tiny);
        // Q1 a 2.2 m step over the 0.5 m wall at x 50..50.5.
        const auto seg = t.Segment(49.0, 60.0, 51.2, 60.0);
        const auto back = t.Segment(51.2, 60.0, 49.0, 60.0);
        const bool dest_free = svc.IsWalkable(51.2f, 60.0f);
        const auto step = svc.CheckStep(49.0f, 60.0f, 51.2f, 60.0f);
        c.Report("path-check-no-tunneling-through-thin-wall",
                 seg.blocked && back.blocked && dest_free && step.result == StepResult::Blocked,
                 Fmt("2.2 m step across a 0.5 m wall: destination walkable=%d (a destination-only check would pass), "
                     "path blocked=%d (back=%d), CheckStep=%s, cells visited=%u",
                     dest_free ? 1 : 0, seg.blocked ? 1 : 0, back.blocked ? 1 : 0, gs::game::ToString(step.result),
                     seg.cells));
        // Q2 across the x = 32 chunk border: clear at y 60, blocked at y 70.2
        // (cell (64,140) is the first cell of the next chunk).
        const auto open_border = t.Segment(31.8, 60.0, 32.3, 60.0);
        const auto blocked_border = t.Segment(31.8, 70.2, 32.3, 70.2);
        c.Report("path-check-across-chunk-border",
                 open_border.Clear() && open_border.cells == 1 && blocked_border.blocked &&
                     blocked_border.chunk == t.ChunkIndex(1, 2),
                 Fmt("x 31.8 -> 32.3: y=60 clear (cells=%u), y=70.2 blocked in chunk %u (expected %u)",
                     open_border.cells, blocked_border.chunk, t.ChunkIndex(1, 2)));
        // Q3 exact diagonal through the shared corner of two blocked cells.
        const auto squeeze = t.Segment(10.25, 10.25, 10.75, 10.75);
        const auto other = t.Segment(10.75, 10.25, 10.25, 10.75);
        const auto free_diag = t.Segment(20.25, 20.25, 20.75, 20.75);
        c.Report("path-check-no-corner-squeeze",
                 squeeze.blocked && free_diag.Clear(),
                 Fmt("diagonal through the corner of blocked (20,21)/(21,20): blocked=%d (other diagonal: blocked=%d "
                     "status=%s); an open diagonal elsewhere: clear=%d",
                     squeeze.blocked ? 1 : 0, other.blocked ? 1 : 0, mx::map::ToString(other.status),
                     free_diag.Clear() ? 1 : 0));
        // Q4 slope rule (1.2 m/m ramp from x = 80 m).
        gs::game::MovementRules steep;
        steep.max_slope = 1.0f;
        const gs::game::TerrainService slope = service(tiny, steep);
        const auto up = slope.CheckStep(90.0f, 30.0f, 90.3f, 30.0f);
        const auto down = slope.CheckStep(90.3f, 30.0f, 90.0f, 30.0f);
        const auto flat = slope.CheckStep(60.0f, 30.0f, 60.3f, 30.0f);
        const auto off = svc.CheckStep(90.0f, 30.0f, 90.3f, 30.0f);
        c.Report("slope-limit-uphill-only",
                 up.result == StepResult::TooSteep && down.Clear() && flat.Clear() && off.Clear(),
                 Fmt("max_slope 1.0 on a 1.2 m/m ramp: up=%s down=%s flat=%s; rule off: up=%s",
                     gs::game::ToString(up.result), gs::game::ToString(down.result), gs::game::ToString(flat.result),
                     gs::game::ToString(off.result)));
        // Q5 missing data / outside on the path.
        auto evicted = t.Clone();
        evicted.EvictChunkForTest(1, 1);
        const auto missing = evicted.Segment(31.0, 40.0, 33.0, 40.0);
        const auto outside = svc.CheckStep(127.9f, 10.0f, 128.2f, 10.0f);
        c.Report("path-check-not-resident-and-outside",
                 missing.status == TerrainStatus::NotResident && missing.chunk == t.ChunkIndex(1, 1) &&
                     outside.result == StepResult::OutsideWorld,
                 Fmt("path into an evicted chunk: %s (chunk %u to demand); step over the max edge: %s",
                     mx::map::ToString(missing.status), missing.chunk, gs::game::ToString(outside.result)));
        // Q6 a step that stays inside its start cell still needs that cell's
        // DATA (its height becomes z): not resident -> wait (demand), invalid
        // -> refused. (Before the fix such a step was Clear with a stale z.)
        gs::game::TerrainService evicted_svc(t.Clone());
        evicted_svc.MutableTerrain()->EvictChunkForTest(1, 1);
        const auto wait = evicted_svc.CheckStep(40.1f, 40.1f, 40.2f, 40.1f);
        auto broken = t.Clone();
        auto empty = std::make_shared<mx::map::TerrainChunk>();
        empty->x = 1;
        empty->y = 1;
        empty->cells_x = t.Geometry().ChunkCellsX(1);
        empty->cells_y = t.Geometry().ChunkCellsY(1);
        (void)broken.Publish(t.ChunkIndex(1, 1), std::move(empty));
        gs::game::TerrainService broken_svc(std::move(broken));
        const auto refused = broken_svc.CheckStep(40.1f, 40.1f, 40.2f, 40.1f);
        const auto resident = svc.CheckStep(40.1f, 40.1f, 40.2f, 40.1f);
        c.Report("path-check-same-cell-needs-data",
                 wait.result == StepResult::NotResident && wait.chunk == t.ChunkIndex(1, 1) &&
                     refused.result == StepResult::InvalidData && resident.Clear(),
                 Fmt("10 cm step inside one cell: chunk evicted -> %s (demand chunk %u); chunk invalid -> %s; "
                     "resident -> %s",
                     gs::game::ToString(wait.result), wait.chunk, gs::game::ToString(refused.result),
                     gs::game::ToString(resident.result)));
    }

    // ================= water =================
    {
        std::mt19937 rng(17);
        std::uniform_real_distribution<float> u(-2048.0f, 2047.9f);
        const gs::game::TerrainService und = service(undeclared);
        const gs::game::TerrainService dry = service(none);
        const gs::game::TerrainService sea_svc = service(sea);
        const gs::game::TerrainService lakes = service(bodies);
        const auto& ref = bodies.world->terrain;
        std::size_t und_ok = 0, none_ok = 0, sea_ok = 0, sea_water = 0, n = 0;
        for (int i = 0; i < 2000; ++i, ++n) {
            const float x = u(rng);
            const float y = u(rng);
            und_ok += und.Water(x, y).status == WaterStatus::UnsupportedLayer ? 1u : 0u;
            none_ok += dry.Water(x, y).status == WaterStatus::NoWater ? 1u : 0u;
            const auto w = sea_svc.Water(x, y);
            const auto ground = ref.Height(x, y);
            const bool expect_water = ground.meters < 5.0f;
            sea_ok += (w.IsWater() == expect_water) &&
                              (!expect_water || w.depth_m == static_cast<float>(5.0 - static_cast<double>(ground.meters)))
                          ? 1u
                          : 0u;
            sea_water += w.IsWater() ? 1u : 0u;
        }
        c.Report("water-undeclared-is-unknown-not-land", und_ok == n && und.Water(3000.0f, 0.0f).status == WaterStatus::OutsideWorld,
                 Fmt("undeclared: %zu/%zu points UnsupportedLayer (never NoWater); outside: %s", und_ok, n,
                     mx::map::ToString(und.Water(3000.0f, 0.0f).status)));
        c.Report("water-none-declared-dry", none_ok == n, Fmt("model none: %zu/%zu NoWater", none_ok, n));
        c.Report("water-sea-level-depth-oracle", sea_ok == n && sea_water > 0 && sea_water < n,
                 Fmt("sea level 5 m: %zu/%zu match (water iff ground < 5 m, depth = 5 - ground exactly); water "
                     "points=%zu",
                     sea_ok, n, sea_water));
        const auto lake = lakes.Water(-1600.0f, -400.0f);
        const auto lake_ground = ref.Height(-1600.0f, -400.0f);
        const auto basin = lakes.Water(1200.0f, 600.0f);
        const auto land = lakes.Water(0.0f, 0.0f);
        c.Report("water-bodies-surface-depth",
                 lake.IsWater() && lake.surface_m == 60.0f &&
                     lake.depth_m == static_cast<float>(60.0 - static_cast<double>(lake_ground.meters)) &&
                     basin.status == WaterStatus::NoWater && basin.surface_m == -100.0f &&
                     land.status == WaterStatus::NoWater,
                 Fmt("lake: %s surface %.1f depth %.3f (60 - ground %.3f); dry basin: %s (surface %.1f below "
                     "ground); outside bodies: %s",
                     mx::map::ToString(lake.status), lake.surface_m, lake.depth_m, lake_ground.meters,
                     mx::map::ToString(basin.status), basin.surface_m, mx::map::ToString(land.status)));
        auto evicted = bodies.world->terrain.Clone();
        const std::uint32_t lake_chunk = evicted.ChunkIndexOf(-1600.0, -400.0);
        evicted.EvictChunkForTest(lake_chunk % 8, lake_chunk / 8);
        gs::game::TerrainService missing(std::move(evicted));
        missing.SetWater(bodies.world->water);
        c.Report("water-ground-not-resident", missing.Water(-1600.0f, -400.0f).status == WaterStatus::NotResident,
                 Fmt("lake point with its chunk evicted: %s (not land, not water)",
                     mx::map::ToString(missing.Water(-1600.0f, -400.0f).status)));
        // Deep-water movement rule.
        gs::game::MovementRules wade;
        wade.max_water_depth_m = 1.0f;
        const gs::game::TerrainService wading = service(bodies, wade);
        const auto into = wading.CheckStep(-1810.0f, -400.0f, -1799.0f, -400.0f);
        const auto off = lakes.CheckStep(-1810.0f, -400.0f, -1799.0f, -400.0f);
        c.Report("deep-water-movement-rule",
                 into.result == StepResult::DeepWater && off.Clear(),
                 Fmt("step into the lake (depth %.1f m) with max_water_depth 1.0: %s; rule off: %s",
                     lakes.Water(-1799.0f, -400.0f).depth_m, gs::game::ToString(into.result),
                     gs::game::ToString(off.result)));
        // Package rules for the declaration.
        auto expect_rejected = [&](const std::string& name, mx::map::PackageWriteSpec spec, mx::map::PackageErrorCode code) {
            const auto d = FixtureDir("reject_" + name);
            const auto written = mx::map::WritePackage(d, spec);
            const auto loaded = LoadPackage(d, mx::map::ResidencyMode::Eager);
            bool has = false;
            for (const auto& issue : loaded.report.issues) {
                has = has || (issue.code == code && issue.severity == mx::map::IssueSeverity::Error);
            }
            c.Report(name, written.ok && !loaded.world && has,
                     Fmt("expect %s -> %s; first: %s", mx::map::ToString(code), loaded.world ? "LOADED" : "refused",
                         loaded.First().c_str()));
        };
        auto overlap = SpecStream(WaterModel::Bodies);
        overlap.water_bodies.push_back({3, {-1500.0f, -500.0f, -1300.0f, -300.0f}, 10.0f});
        expect_rejected("water-bodies-overlap", overlap, mx::map::PackageErrorCode::WaterBodyOverlap);
        auto outside_body = SpecStream(WaterModel::Bodies);
        outside_body.water_bodies[0].bounds.min_x = -2100.0f;
        expect_rejected("water-body-outside-world", outside_body, mx::map::PackageErrorCode::WaterBodyInvalid);
        auto bad_sea = SpecStream(WaterModel::SeaLevel);
        bad_sea.sea_level_m = std::numeric_limits<double>::quiet_NaN();
        expect_rejected("water-sea-level-non-finite", bad_sea, mx::map::PackageErrorCode::ManifestFieldInvalid);
        auto no_layer = SpecStream(WaterModel::Bodies);
        no_layer.patch_manifest = [](mx::map::package_schema::MapManifest::Builder& m) {
            m.getWater().setModel(mx::map::package_schema::WaterModel::SEA_LEVEL); // layer present, model not bodies
        };
        expect_rejected("water-layer-without-bodies-model", no_layer, mx::map::PackageErrorCode::WaterDeclInvalid);
    }

    // ================= navigation =================
    {
        const auto& ref = bodies.world->terrain;
        const gs::game::TerrainService svc = service(bodies);
        const auto& g = ref.Geometry();
        gs::game::NavigationService nav;
        std::uint64_t next_id = 0;
        auto cell_of = [&](float x, float y) {
            std::uint32_t cx = 0, cy = 0;
            ref.CellIndexOf(x, y, cx, cy);
            return std::pair<std::uint32_t, std::uint32_t>{cx, cy};
        };
        // N1 the wall between start and goal: inside a small window there is
        // no way around it (NoPath); a wider window finds the detour, with
        // the oracle's length.
        gs::game::NavRequest across;
        across.start_x = 400.0f;
        across.start_y = 0.0f;
        across.goal_x = 640.0f;
        across.goal_y = 0.0f;
        across.search_margin_m = 256.0f;
        const auto blocked = RunNav(nav, svc, nullptr, across, ++next_id);
        across.search_margin_m = 1800.0f;
        across.max_expansions = 400000;
        const auto around = RunNav(nav, svc, nullptr, across, ++next_id);
        const auto [sx, sy] = cell_of(400.0f, 0.0f);
        const auto [gx, gy] = cell_of(640.0f, 0.0f);
        const auto margin = static_cast<std::uint32_t>(std::ceil(1800.0 / kCell));
        const float oracle = OraclePathLength(sx, sy, gx, gy, std::min(sx, gx) > margin ? std::min(sx, gx) - margin : 0,
                                              std::min(sy, gy) > margin ? std::min(sy, gy) - margin : 0,
                                              std::min<std::uint32_t>(std::max(sx, gx) + margin, kCells - 1),
                                              std::min<std::uint32_t>(std::max(sy, gy) + margin, kCells - 1),
                                              static_cast<float>(kCell));
        c.Report("nav-no-path-inside-window", blocked.status == gs::game::NavStatus::NoPath,
                 Fmt("wall between start and goal, window margin 256 m: %s after %u expansions",
                     gs::game::ToString(blocked.status), blocked.expansions));
        c.Report("nav-detour-matches-oracle",
                 around.status == gs::game::NavStatus::Found && std::abs(around.length_m - oracle) < 0.01f &&
                     around.waypoints.size() >= 3,
                 Fmt("margin 1800 m: %s length %.2f m (Dijkstra oracle %.2f m), %zu waypoints, %u expansions, "
                     "peak pins %u",
                     gs::game::ToString(around.status), around.length_m, oracle, around.waypoints.size(),
                     around.expansions, around.pinned_chunks));
        // N2 a long cross-chunk path with the oracle.
        gs::game::NavRequest long_req;
        long_req.start_x = -1000.0f;
        long_req.start_y = -1000.0f;
        long_req.goal_x = 1000.0f;
        long_req.goal_y = -1800.0f;
        long_req.max_expansions = 400000;
        const auto long_path = RunNav(nav, svc, nullptr, long_req, ++next_id);
        const auto [lsx, lsy] = cell_of(long_req.start_x, long_req.start_y);
        const auto [lgx, lgy] = cell_of(long_req.goal_x, long_req.goal_y);
        const auto lm = static_cast<std::uint32_t>(std::ceil(long_req.search_margin_m / kCell));
        const float long_oracle = OraclePathLength(
            lsx, lsy, lgx, lgy, std::min(lsx, lgx) > lm ? std::min(lsx, lgx) - lm : 0,
            std::min(lsy, lgy) > lm ? std::min(lsy, lgy) - lm : 0,
            std::min<std::uint32_t>(std::max(lsx, lgx) + lm, kCells - 1),
            std::min<std::uint32_t>(std::max(lsy, lgy) + lm, kCells - 1), static_cast<float>(kCell));
        c.Report("nav-cross-chunk-path-oracle",
                 long_path.status == gs::game::NavStatus::Found && std::abs(long_path.length_m - long_oracle) < 0.01f &&
                     long_path.pinned_chunks >= 4,
                 Fmt("(-1000,-1000) -> (1000,-1800): %s length %.2f m (oracle %.2f m), read %u chunks (pinned)",
                     gs::game::ToString(long_path.status), long_path.length_m, long_oracle, long_path.pinned_chunks));
        // N3 work budget, N4 cancel, N7 outside / flat.
        gs::game::NavRequest small = long_req;
        small.max_expansions = 50;
        const auto budget = RunNav(nav, svc, nullptr, small, ++next_id);
        const std::uint64_t cancel_id = ++next_id;
        nav.Request(cancel_id, long_req, Clock::now());
        nav.Pump(svc, nullptr, Clock::now(), 10);
        nav.Cancel(cancel_id);
        nav.Pump(svc, nullptr, Clock::now(), 10);
        const auto cancelled = nav.Result(cancel_id);
        gs::game::NavRequest outside = long_req;
        outside.goal_x = 5000.0f;
        const auto out = RunNav(nav, svc, nullptr, outside, ++next_id);
        const gs::game::TerrainService flat = gs::game::TerrainService::Flat(gs::game::WorldBounds::FromExtent(1000.0f));
        const auto flat_result = RunNav(nav, flat, nullptr, long_req, ++next_id);
        c.Report("nav-budget-cancel-outside-unsupported",
                 budget.status == gs::game::NavStatus::BudgetExceeded && cancelled.status == gs::game::NavStatus::Cancelled &&
                     out.status == gs::game::NavStatus::OutsideWorld &&
                     flat_result.status == gs::game::NavStatus::UnsupportedLayer && nav.GetStats().pinned_chunks == 0,
                 Fmt("max_expansions 50: %s; cancel: %s; goal outside: %s; flat world: %s; pins held after all "
                     "jobs=%zu",
                     gs::game::ToString(budget.status), gs::game::ToString(cancelled.status),
                     gs::game::ToString(out.status), gs::game::ToString(flat_result.status),
                     nav.GetStats().pinned_chunks));
        // N5 streaming: the search waits for chunks, pins what it read (no
        // eviction under it), and finds the same path.
        gs::game::TerrainStreamingConfig cfg;
        cfg.budget_bytes = 8u << 20;
        cfg.retain_seconds = 0.1;
        auto rig = MakeRig(bodies_dir, cfg);
        rig->source->SetGlobalDelay(5ms);
        gs::game::TerrainService streamed(std::move(*rig->terrain));
        // The rig's streamer points at rig->terrain: rebuild it on the service.
        rig->streamer.reset();
        auto streamer = std::make_unique<gs::game::TerrainStreamer>(*streamed.MutableTerrain(), rig->source, cfg);
        streamed.SetWater(bodies.world->water);
        std::size_t evicted_under_job = 0;
        std::size_t max_pins = 0;
        const auto waited = RunNav(nav, streamed, streamer.get(), long_req, ++next_id, 20000, [&] {
            // Eviction of everything undemanded for 100 ms while the job runs:
            // chunks the job already read are pinned and must survive.
            evicted_under_job += streamer->EvictUndemanded(Clock::now() - 100ms, true);
            max_pins = std::max(max_pins, nav.GetStats().pinned_chunks);
        });
        c.Report("nav-streaming-waits-pins-same-result",
                 waited.status == gs::game::NavStatus::Found && std::abs(waited.length_m - long_oracle) < 0.01f &&
                     waited.chunk_waits > 0 && max_pins > 0,
                 Fmt("%s length %.2f m (oracle %.2f), waited %u pass(es) for chunks, pins held during the search "
                     "(max %zu) -- evictions attempted meanwhile freed %zu other chunk(s)",
                     gs::game::ToString(waited.status), waited.length_m, long_oracle, waited.chunk_waits, max_pins,
                     evicted_under_job));
        // N6 a chunk that never arrives: bounded wait -> NotResident.
        rig->source->Gate(true);
        streamer->EvictUndemanded(Clock::now() + 10s, true);
        gs::game::NavRequest stuck = long_req;
        stuck.timeout_seconds = 0.3;
        const auto timeout = RunNav(nav, streamed, streamer.get(), stuck, ++next_id);
        rig->source->OpenAll();
        c.Report("nav-missing-chunk-bounded-wait",
                 timeout.status == gs::game::NavStatus::NotResident && timeout.elapsed_ms < 2000.0,
                 Fmt("reads blocked: %s after %.0f ms (timeout 300 ms)", gs::game::ToString(timeout.status),
                     timeout.elapsed_ms));
        streamer.reset();
        // N8 unusable (INVALID) data is not "no path": a goal behind it answers
        // InvalidData; a detour over valid data is still Found.
        {
            auto broken = ref.Clone();
            const std::uint32_t bad = broken.ChunkIndexOf(-300.0, 100.0);
            auto empty = std::make_shared<mx::map::TerrainChunk>();
            empty->x = bad % g.chunks_x;
            empty->y = bad / g.chunks_x;
            empty->cells_x = g.ChunkCellsX(empty->x);
            empty->cells_y = g.ChunkCellsY(empty->y);
            (void)broken.Publish(bad, std::move(empty));
            gs::game::TerrainService broken_svc(std::move(broken));
            broken_svc.SetWater(bodies.world->water);
            gs::game::NavRequest into;
            into.start_x = 100.0f;
            into.start_y = 100.0f;
            into.goal_x = -300.0f;
            into.goal_y = 100.0f;
            const auto blocked_by_invalid = RunNav(nav, broken_svc, nullptr, into, ++next_id);
            gs::game::NavRequest detour = into;
            detour.start_y = -100.0f;
            detour.goal_y = -100.0f;
            detour.max_expansions = 400000;
            const auto around_invalid = RunNav(nav, broken_svc, nullptr, detour, ++next_id);
            c.Report("nav-invalid-data-not-no-path",
                     blocked_by_invalid.status == gs::game::NavStatus::InvalidData &&
                         around_invalid.status == gs::game::NavStatus::Found,
                     Fmt("goal inside an INVALID chunk: %s after %u expansions (not no-path); goal beside it: %s "
                         "(%.1f m)",
                         gs::game::ToString(blocked_by_invalid.status), blocked_by_invalid.expansions,
                         gs::game::ToString(around_invalid.status), around_invalid.length_m));
        }
        // N9 bounded job count: beyond kMaxJobs a request is Rejected at once
        // (explicit resource shortage); cancelling the rest releases all pins.
        {
            gs::game::NavigationService capped;
            constexpr std::uint64_t kRequests = 300;
            for (std::uint64_t id = 1; id <= kRequests; ++id) {
                capped.Request(id, long_req, Clock::now());
            }
            std::size_t rejected = 0;
            std::size_t pending = 0;
            for (std::uint64_t id = 1; id <= kRequests; ++id) {
                const auto status = capped.Result(id).status;
                rejected += status == gs::game::NavStatus::Rejected ? 1u : 0u;
                pending += status == gs::game::NavStatus::Pending ? 1u : 0u;
            }
            capped.Pump(svc, nullptr, Clock::now(), 2000); // some jobs start reading (pins)
            const auto busy = capped.GetStats();
            for (std::uint64_t id = 1; id <= kRequests; ++id) {
                capped.Cancel(id);
            }
            capped.Pump(svc, nullptr, Clock::now(), 10);
            const auto after = capped.GetStats();
            c.Report("nav-job-cap-rejects-explicitly",
                     rejected == kRequests - gs::game::NavigationService::kMaxJobs &&
                         pending == gs::game::NavigationService::kMaxJobs && busy.pinned_chunks > 0 &&
                         after.running == 0 && after.pinned_chunks == 0 &&
                         after.cancelled == gs::game::NavigationService::kMaxJobs,
                     Fmt("%llu requests: running=%zu rejected=%zu (cap %zu); pins while running=%zu; after cancel: "
                         "running=%zu pins=%zu cancelled=%llu",
                         static_cast<unsigned long long>(kRequests), pending, rejected,
                         gs::game::NavigationService::kMaxJobs, busy.pinned_chunks, after.running,
                         after.pinned_chunks, static_cast<unsigned long long>(after.cancelled)));
        }
    }

    // ================= runtime: movement + navigation through the world =================
    {
        auto world = LoadPackage(bodies_dir, mx::map::ResidencyMode::Eager);
        IoRunner runner;
        gs::game::PartitionLayout layout;
        layout.regions_x = layout.regions_y = 1;
        gs::game::WorldRuntime sim(runner.io, {}, std::move(*world.world), layout);
        gs::game::MovementRules rules;
        rules.max_water_depth_m = 1.0f;
        sim.ConfigureMovementRules(rules);
        gs::game::PartitionConfig partition;
        partition.scoring.adaptive_enabled = false;
        sim.ConfigurePartition(partition);
        // Player 1 walks east into the wall (x 512..528), player 2 west into
        // the lake (x >= -1800 at y -400).
        sim.PostSpawn(DetachedSession(runner.io, 99001), MakeCharacter(1), gs::game::DebugSpawnOverride{505.0f, 0.0f});
        sim.PostSpawn(DetachedSession(runner.io, 99002), MakeCharacter(2),
                      gs::game::DebugSpawnOverride{-1805.0f, -400.0f});
        sim.Start();
        WaitFor(5000ms, [&] { return ReadWorld(sim, [](const WorldSnapshot& s) { return s.owners.size(); }) == 2; });
        std::uint32_t seq = 0;
        const auto until = Clock::now() + 2500ms;
        while (Clock::now() < until) {
            sim.PostMoveInput(99001, ++seq, 1.5707963f, gs::game::MoveState::Running);
            sim.PostMoveInput(99002, ++seq, 1.5707963f, gs::game::MoveState::Running);
            std::this_thread::sleep_for(50ms);
        }
        auto pos = [&](gs::common::SessionId s) {
            return ReadWorld(sim, [s](const WorldSnapshot& snap) {
                gs::game::Position p{};
                const auto it = snap.owners.find(s);
                if (it != snap.owners.end()) {
                    p = snap.zones.GetZone(it->second.zone_index).FindEntity(it->second.net_id).get<gs::game::Position>();
                }
                return p;
            });
        };
        const auto wall = pos(99001);
        const auto shore = pos(99002);
        // World queries from a bench thread go through a snapshot collector
        // (supervisor), never straight at the published slots.
        const float lake_depth =
            ReadWorld(sim, [&sim](const WorldSnapshot&) { return sim.Terrain().Water(-1799.0f, -400.0f).depth_m; });
        c.Report("runtime-movement-stops-at-wall",
                 wall.x < 512.0f && wall.x > 511.0f,
                 Fmt("player running east from x=505 stops at x=%.2f before the wall at 512 (no tunneling)", wall.x));
        const auto st = sim.GetTerrainStats();
        c.Report("runtime-movement-stops-at-deep-water",
                 shore.x < -1800.0f && shore.x > -1801.0f && st.queries.steps_blocked > 0,
                 Fmt("player running east from x=-1805 stops at x=%.2f at the lake shore (-1800, depth ~%.0f m > "
                     "1 m); steps blocked=%llu ok=%llu",
                     shore.x, lake_depth,
                     static_cast<unsigned long long>(st.queries.steps_blocked),
                     static_cast<unsigned long long>(st.queries.ok)));
        gs::game::NavRequest req;
        req.start_x = 400.0f;
        req.start_y = 0.0f;
        req.goal_x = 640.0f;
        req.goal_y = 0.0f;
        req.search_margin_m = 1800.0f;
        req.max_expansions = 400000;
        const std::uint64_t id = sim.PostNavigationRequest(req);
        WaitFor(10000ms, [&] { return sim.NavigationResult(id).status != gs::game::NavStatus::Pending; });
        const auto result = sim.NavigationResult(id);
        c.Report("runtime-navigation-request",
                 result.status == gs::game::NavStatus::Found && result.length_m > 240.0f,
                 Fmt("PostNavigationRequest around the wall: %s, length %.1f m, %.1f ms on the supervisor (bounded "
                     "expansions per pass)",
                     gs::game::ToString(result.status), result.length_m, result.elapsed_ms));
        sim.Stop();
    }
    std::error_code ec;
    // Keep the generated corpus for the isolated runner's fixture SHA256 manifest.
    (void)ec;
    std::printf("WORLDQUERY-DONE passes=%d failures=%d\n", c.passes, c.failures);
    return c.failures;
}


namespace {

// 100 km x 100 km at origin (-50000,-50000): 6250 x 6250 cells of 16 m,
// 64-cell (1024 m) chunks = 98 x 98 = 9604 chunks, the last row/column
// partial (42 cells). ~160 MB of chunk files.
constexpr double kBigOrigin = -50000.0;
constexpr std::uint32_t kBigCells = 6250;

std::int32_t RawBig(std::uint32_t vx, std::uint32_t vy)
{
    return static_cast<std::int32_t>(std::lround(12000.0 * std::sin(vx * 0.004) * std::cos(vy * 0.003) +
                                                 3000.0 * std::sin((vx + 2 * vy) * 0.02)));
}

mx::map::PackageWriteSpec SpecBig()
{
    mx::map::PackageWriteSpec spec;
    spec.world_id = "map3-100km";
    spec.world_name = "MAP-3 100 km streaming world";
    spec.size_cells_x = kBigCells;
    spec.origin_x = kBigOrigin;
    spec.origin_y = kBigOrigin;
    spec.cell_size_m = 16.0f;
    spec.chunk_size_cells = 64;
    spec.height_raw = RawBig;
    spec.attributes = [](std::uint32_t, std::uint32_t) -> std::uint16_t { return 0; };
    spec.logic.spawns = {{1, 0, {0.0f, 0.0f, 10.0f, 10.0f}}};
    std::string mobs;
    for (int j = 0; j < 8; ++j) {
        for (int i = 0; i < 8; ++i) {
            mobs += Fmt("mob_type_id=%d x=%d y=%d count=50 radius=300\n", 1 + (i + j) % 2, -43750 + 12500 * i,
                        -43750 + 12500 * j);
        }
    }
    spec.mob_spawns = mobs;
    spec.water_model = mx::map::WaterModel::SeaLevel;
    spec.sea_level_m = -50.0;
    spec.overwrite = true;
    return spec;
}

} // namespace

int RunStreamingSoak(int seconds, int budget_mb)
{
    Checks c{"STREAMSOAK"};
    seconds = std::max(seconds, 10);
    const auto dir = ScratchRoot() / "world_100km";
    const auto t_write = Clock::now();
    bool reuse = fs::exists(dir / "map.manifest");
    if (!reuse) {
        const auto written = mx::map::WritePackage(dir, SpecBig());
        if (!written.ok) {
            c.Report("fixture-write", false, written.error);
            return c.failures;
        }
    }
    const double write_s = std::chrono::duration<double>(Clock::now() - t_write).count();
    std::uintmax_t package_bytes = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            package_bytes += entry.file_size();
        }
    }
    std::printf("STREAMSOAK package %s: %.1f MB, write %.1f s%s\n", dir.string().c_str(), package_bytes / 1048576.0,
                write_s, reuse ? " (reused)" : "");

    // ---- cold startup: eager vs streaming on the same package ----
    const auto rss0 = static_cast<double>(ProcessWorkingSetBytes());
    auto t0 = Clock::now();
    auto eager = LoadPackage(dir, mx::map::ResidencyMode::Eager);
    const double eager_s = std::chrono::duration<double>(Clock::now() - t0).count();
    const auto rss_eager = static_cast<double>(ProcessWorkingSetBytes());
    t0 = Clock::now();
    auto streamed = LoadPackage(dir, mx::map::ResidencyMode::Streaming);
    const double stream_s = std::chrono::duration<double>(Clock::now() - t0).count();
    if (!eager.world || !streamed.world) {
        c.Report("fixture-load", false, eager.First() + " | " + streamed.First());
        return c.failures;
    }
    c.Report("cold-startup-eager-vs-streaming",
             streamed.world->terrain.ResidentChunkCount() == streamed.world->startup_chunks.size() &&
                 eager.world->terrain.ResidentChunkCount() == 9604,
             Fmt("same package (9604 chunks): eager startup %.2f s, resident %.1f MB (RSS +%.0f MB) | streaming "
                 "startup %.2f s (9604 files size-checked, %zu decoded), resident %.3f MB",
                 eager_s, eager.world->terrain.ResidentBytes() / 1048576.0, (rss_eager - rss0) / 1048576.0, stream_s,
                 streamed.world->startup_chunks.size(), streamed.world->terrain.ResidentBytes() / 1048576.0));
    const mx::map::ServerTerrain& ref = eager.world->terrain; // reference provider (kept for the z oracle)

    // ---- runtime: `budget_mb` cache (default 16), 200 roaming players, 3200 mobs ----
    IoRunner runner;
    gs::game::TerrainStreamingConfig cfg;
    cfg.budget_bytes = static_cast<std::size_t>(std::max(budget_mb, 1)) << 20;
    cfg.io_threads = 2;
    cfg.retain_seconds = 3.0;
    gs::game::PartitionLayout layout;
    layout.regions_x = layout.regions_y = 2;
    layout.leaves_x = layout.leaves_y = 4; // 64 initial zones of 12.5 km
    const auto rss_before_runtime = static_cast<double>(ProcessWorkingSetBytes());
    t0 = Clock::now();
    gs::game::WorldRuntime sim(runner.io, {}, std::move(*streamed.world), layout, cfg);
    const double runtime_s = std::chrono::duration<double>(Clock::now() - t0).count();
    gs::game::PartitionConfig partition;
    partition.scoring.adaptive_enabled = false;
    sim.ConfigurePartition(partition);
    const auto boot = sim.GetTerrainStats();
    std::printf("STREAMSOAK runtime built in %.2f s: initial spawn loaded %llu chunks, evicted %llu, peak %.2f MB of "
                "%.0f MB\n",
                runtime_s, static_cast<unsigned long long>(boot.streamer.loads_completed),
                static_cast<unsigned long long>(boot.streamer.evictions), boot.streamer.peak_accounted_bytes / 1048576.0,
                cfg.budget_bytes / 1048576.0);
    constexpr int kPlayers = 200;
    std::mt19937 rng(20260925);
    std::uniform_real_distribution<float> where(-49000.0f, 49000.0f);
    std::uniform_real_distribution<float> heading(-3.1415926f, 3.1415926f);
    std::vector<float> headings(kPlayers);
    auto place = [&](std::uint64_t generation) {
        const float x = where(rng);
        const float y = where(rng);
        sim.PostTerrainDemand(x, y, 10.0f);
        return std::tuple<float, float, std::uint64_t>{x, y, generation};
    };
    // Initial placement: demand, then spawn with a debug override (falls back
    // to the spawn region if the chunk is not resident yet -- counted).
    std::vector<std::tuple<float, float, std::uint64_t>> targets;
    for (int i = 0; i < kPlayers; ++i) {
        targets.push_back(place(0));
        headings[static_cast<std::size_t>(i)] = heading(rng);
    }
    sim.Start();
    std::this_thread::sleep_for(500ms);
    for (int i = 0; i < kPlayers; ++i) {
        const auto [x, y, g] = targets[static_cast<std::size_t>(i)];
        (void)g;
        sim.PostSpawn(DetachedSession(runner.io, 70000 + i), MakeCharacter(1000 + i), gs::game::DebugSpawnOverride{x, y});
    }
    WaitFor(20000ms, [&] {
        return ReadWorld(sim, [](const WorldSnapshot& snap) { return snap.owners.size(); }) == kPlayers;
    });
    const auto start_stats = sim.GetTerrainStats();
    const auto rss_start = static_cast<double>(ProcessWorkingSetBytes());
    std::size_t peak_accounted = 0;
    std::uint64_t relocations = 0;
    std::uint32_t seq = 0;
    const auto soak_end = Clock::now() + std::chrono::seconds(seconds);
    auto next_relocate = Clock::now() + 2s;
    auto next_sample = Clock::now();
    std::vector<double> rss_samples;
    while (Clock::now() < soak_end) {
        for (int i = 0; i < kPlayers; ++i) {
            sim.PostMoveInput(70000 + i, ++seq, headings[static_cast<std::size_t>(i)], gs::game::MoveState::Running);
        }
        if (Clock::now() >= next_relocate) {
            // 20 % of the players "log out" and come back elsewhere: the
            // active areas move across the 100 km world.
            next_relocate += 2s;
            std::vector<std::pair<int, std::tuple<float, float, std::uint64_t>>> moves;
            for (int k = 0; k < kPlayers / 5; ++k) {
                const int i = static_cast<int>(rng() % kPlayers);
                moves.emplace_back(i, place(relocations));
            }
            std::this_thread::sleep_for(100ms);
            for (const auto& [i, target] : moves) {
                const auto [x, y, g] = target;
                (void)g;
                sim.PostDespawn(70000 + i);
                sim.PostSpawn(DetachedSession(runner.io, 70000 + i), MakeCharacter(1000 + i),
                              gs::game::DebugSpawnOverride{x, y});
                headings[static_cast<std::size_t>(i)] = heading(rng);
                ++relocations;
            }
        }
        if (Clock::now() >= next_sample) {
            next_sample += 1s;
            peak_accounted = std::max(peak_accounted, sim.GetTerrainStats().streamer.accounted_bytes);
            rss_samples.push_back(static_cast<double>(ProcessWorkingSetBytes()));
        }
        std::this_thread::sleep_for(50ms);
    }
    const auto end_stats = sim.GetTerrainStats();
    const double rss_end = static_cast<double>(ProcessWorkingSetBytes());
    // Every entity's height equals the reference provider (supervisor-side).
    struct Z {
        std::size_t entities = 0;
        std::size_t mismatches = 0;
        std::size_t players = 0;
    };
    const Z z = ReadWorld(sim, [&](const WorldSnapshot& snap) {
        Z out;
        for (std::size_t i = 0; i < snap.zones.ZoneCount(); ++i) {
            for (const auto& [net_id, entity] : snap.zones.GetZone(i).Entities()) {
                (void)net_id;
                ++out.entities;
                out.players += entity.has<gs::game::PlayerTag>() ? 1u : 0u;
                const auto pos = entity.get<gs::game::Position>();
                const auto h = ref.Height(pos.x, pos.y);
                out.mismatches += h.Ok() && h.meters == pos.z ? 0u : 1u;
            }
        }
        return out;
    });
    const auto& s0 = start_stats.streamer;
    const auto& s1 = end_stats.streamer;
    const double lookups = static_cast<double>((s1.hits - s0.hits) + (s1.misses - s0.misses));
    const double hit_rate = lookups > 0 ? static_cast<double>(s1.hits - s0.hits) / lookups : 0.0;
    const double steps = static_cast<double>(end_stats.queries.ok + end_stats.queries.steps_waiting +
                                             end_stats.queries.steps_blocked);
    c.Report("soak-streaming-under-budget",
             peak_accounted <= cfg.budget_bytes && s1.peak_accounted_bytes <= cfg.budget_bytes &&
                 s1.loads_completed > s0.loads_completed && s1.evictions > s0.evictions && relocations > 0,
             Fmt("%d s, %d players (%llu relocations), %zu entities: loads %llu evictions %llu (in the soak), "
                 "hit rate %.3f, misses %llu, admission waits %llu rejects %llu, cancelled %llu, stale %llu, "
                 "failures %llu; own accounting peak %.2f MB of %.0f MB (resident %.2f, in-flight %.2f, retired "
                 "%.2f, metadata %.2f MB)",
                 seconds, kPlayers, static_cast<unsigned long long>(relocations), z.entities,
                 static_cast<unsigned long long>(s1.loads_completed - s0.loads_completed),
                 static_cast<unsigned long long>(s1.evictions - s0.evictions), hit_rate,
                 static_cast<unsigned long long>(s1.misses - s0.misses),
                 static_cast<unsigned long long>(s1.admission_waits - s0.admission_waits),
                 static_cast<unsigned long long>(s1.admission_rejects - s0.admission_rejects),
                 static_cast<unsigned long long>(s1.cancelled - s0.cancelled),
                 static_cast<unsigned long long>(s1.stale_completions - s0.stale_completions),
                 static_cast<unsigned long long>(s1.load_failures - s0.load_failures),
                 s1.peak_accounted_bytes / 1048576.0, cfg.budget_bytes / 1048576.0, s1.resident_bytes / 1048576.0,
                 s1.in_flight_bytes / 1048576.0, s1.retired_bytes / 1048576.0, s1.metadata_bytes / 1048576.0));
    c.Report("soak-heights-exact-vs-reference", z.mismatches == 0 && z.players == kPlayers,
             Fmt("%zu entities (%zu players): z == eager reference height for all (mismatches=%zu)", z.entities,
                 z.players, z.mismatches));
    const double rss_min = rss_samples.empty() ? rss_start : *std::min_element(rss_samples.begin(), rss_samples.end());
    const double rss_max = rss_samples.empty() ? rss_end : *std::max_element(rss_samples.begin(), rss_samples.end());
    std::printf("STREAMSOAK memory: process RSS before runtime %.0f MB (includes the %.0f MB eager reference kept for "
                "the oracle), soak start %.0f MB, min/max during the soak %.0f / %.0f MB, end %.0f MB; the streamer's "
                "own accounting is bounded by the budget (%.0f MB); RSS also holds zones, entities, allocator retention\n",
                rss_before_runtime / 1048576.0, ref.ResidentBytes() / 1048576.0, rss_start / 1048576.0,
                rss_min / 1048576.0, rss_max / 1048576.0, rss_end / 1048576.0, cfg.budget_bytes / 1048576.0);
    std::printf("STREAMSOAK latency: load p50 %.2f ms p99 %.2f ms max %.2f ms, lookahead %.2f s; movement steps "
                "%.0f, waiting for data %llu (%.4f %%), blocked %llu; tick: supervisor avg %.3f ms\n",
                s1.load_ms_p50, s1.load_ms_p99, s1.load_ms_max, s1.lookahead_seconds, steps,
                static_cast<unsigned long long>(end_stats.queries.steps_waiting),
                steps > 0 ? 100.0 * static_cast<double>(end_stats.queries.steps_waiting) / steps : 0.0,
                static_cast<unsigned long long>(end_stats.queries.steps_blocked), sim.SupervisorAvgMs());
    // Full memory / I/O accounting of the soak window (population: the
    // world's one streamer; time base: soak start -> end, `seconds` s).
    const double mb = 1048576.0;
    std::printf("STREAMSOAK io: loads started %llu completed %llu (%.1f/s), bytes read %.1f MB (%.2f MB/s), failures %llu "
                "permanent %llu, stale completions %llu, cancelled %llu; demands %llu = hits %llu + misses %llu + "
                "deduplicated %llu; in-flight jobs high-water %zu (max_in_flight %u), waiting high-water %zu "
                "(max_waiting %u), admission waits %llu, admission rejects %llu; load latency p50/p99/max "
                "%.2f/%.2f/%.2f ms; miss latency (first demand -> published) p50/p99/max %.2f/%.2f/%.2f ms\n",
                static_cast<unsigned long long>(s1.loads_started - s0.loads_started),
                static_cast<unsigned long long>(s1.loads_completed - s0.loads_completed),
                static_cast<double>(s1.loads_completed - s0.loads_completed) / seconds,
                static_cast<double>(s1.bytes_read - s0.bytes_read) / mb,
                static_cast<double>(s1.bytes_read - s0.bytes_read) / mb / seconds,
                static_cast<unsigned long long>(s1.load_failures - s0.load_failures),
                static_cast<unsigned long long>(s1.permanent_failures - s0.permanent_failures),
                static_cast<unsigned long long>(s1.stale_completions - s0.stale_completions),
                static_cast<unsigned long long>(s1.cancelled - s0.cancelled),
                static_cast<unsigned long long>(s1.demands - s0.demands),
                static_cast<unsigned long long>(s1.hits - s0.hits),
                static_cast<unsigned long long>(s1.misses - s0.misses),
                static_cast<unsigned long long>(s1.deduplicated - s0.deduplicated), s1.queue_high_water,
                cfg.max_in_flight, s1.waiting_high_water, cfg.max_waiting,
                static_cast<unsigned long long>(s1.admission_waits - s0.admission_waits),
                static_cast<unsigned long long>(s1.admission_rejects - s0.admission_rejects), s1.load_ms_p50,
                s1.load_ms_p99, s1.load_ms_max, s1.miss_ms_p50, s1.miss_ms_p99, s1.miss_ms_max);
    std::printf("STREAMSOAK own accounting (budget %.2f MB): peak accounted %.3f MB = max over time of resident + "
                "in-flight + retired + metadata; peaks resident %.3f, in-flight %.3f, retired %.3f, pinned %.3f "
                "(sampled 10 Hz) MB | end: resident %zu chunks / %.3f MB, pinned %zu (permanent %zu, reader %zu) / "
                "%.3f MB, in-flight %zu jobs / %.3f MB, retired %zu / %.3f MB, metadata %.3f MB, accounted %.3f MB; "
                "evictions %llu frees %llu\n",
                cfg.budget_bytes / mb, s1.peak_accounted_bytes / mb, s1.peak_resident_bytes / mb,
                s1.peak_in_flight_bytes / mb, s1.peak_retired_bytes / mb, s1.peak_pinned_bytes / mb, s1.resident,
                s1.resident_bytes / mb, s1.pinned, s1.permanent_pinned, s1.reader_pinned, s1.pinned_bytes / mb,
                s1.loading, s1.in_flight_bytes / mb, s1.retired, s1.retired_bytes / mb, s1.metadata_bytes / mb,
                s1.accounted_bytes / mb, static_cast<unsigned long long>(s1.evictions - s0.evictions),
                static_cast<unsigned long long>(s1.frees - s0.frees));
    std::printf("STREAMSOAK consumers: movement steps ok %llu, waiting for NOT RESIDENT data %llu, refused on invalid "
                "data %llu, blocked %llu, outside %llu; players %zu of %d, player spawn refusals %llu\n",
                static_cast<unsigned long long>(end_stats.queries.ok),
                static_cast<unsigned long long>(end_stats.queries.steps_waiting),
                static_cast<unsigned long long>(end_stats.queries.invalid),
                static_cast<unsigned long long>(end_stats.queries.steps_blocked),
                static_cast<unsigned long long>(end_stats.queries.outside), z.players, kPlayers,
                static_cast<unsigned long long>(sim.PlayerSpawnRefusals()));
    sim.Stop();
    std::printf("STREAMSOAK-DONE passes=%d failures=%d\n", c.passes, c.failures);
    return c.failures;
}


namespace {

// ---------------------------------------------------------------------------
// MAP-3 review follow-up: lifetime / failure-path / lifecycle evidence.
// ---------------------------------------------------------------------------

// A reader that emulates a zone tick with the runtime's exact protocol: the
// "supervisor" (the bench thread) claims it by setting its tick flag
// (CAS false -> true, like ZoneScheduler) BEFORE dispatching it, the reader
// clears the flag with release at the end of its tick, and the supervisor
// passes quiescent = "every flag clear" (acquire) to TerrainStreamer::Pump.
// A reader keeps the raw published pointers it loaded for its WHOLE tick --
// the widest read window the contract allows, wider than any production
// reader (those re-load the pointer per query) -- and compares every sample
// it reads with the fully resident reference. Freed chunks are poisoned and
// quarantined (poison_freed_for_test): a premature free shows up as a
// mismatch instead of undefined behaviour.
struct StressReader {
    std::atomic<bool> in_tick{false};
    std::atomic<std::uint64_t> dispatched{0};
    std::atomic<bool> stop{false};
    std::uint64_t ticks = 0; // reader-local until joined
    std::uint64_t reads = 0;
    std::uint64_t bad = 0;
    std::uint64_t unpublished_while_held = 0;
    std::thread thread;
};

struct StressOutcome {
    std::uint64_t ticks = 0;
    std::uint64_t reads = 0;
    std::uint64_t bad = 0;
    std::uint64_t unpublished_while_held = 0;
    std::uint64_t passes = 0;
    std::uint64_t quiescent_passes = 0;
    bool permanent_kept = false;
    std::size_t budget = 0;
    gs::game::TerrainStreamer::Stats stats;
};

StressOutcome RunLifetimeStress(const fs::path& dir, const mx::map::ServerTerrain& ref, bool unsafe,
                                std::chrono::milliseconds duration)
{
    StressOutcome out;
    auto probe = MakeRig(dir, {});
    const std::size_t reservation = ChunkReservation(*probe->terrain, *probe->source, 0);
    probe->streamer->PublishStats();
    const std::size_t fixed = probe->streamer->GetStats().accounted_bytes;
    probe.reset();
    gs::game::TerrainStreamingConfig cfg;
    cfg.budget_bytes = fixed + 16 * reservation; // ~30 resident chunks of 64
    cfg.io_threads = 2;
    cfg.max_in_flight = 8;
    cfg.retain_seconds = 0.1;
    cfg.poison_freed_for_test = true;
    cfg.unsafe_free_without_quiescence_for_test = unsafe;
    out.budget = cfg.budget_bytes;
    auto rig = MakeRig(dir, cfg);
    auto& s = *rig->streamer;
    const mx::map::ServerTerrain& terrain = *rig->terrain;
    const auto chunks = static_cast<std::uint32_t>(terrain.ChunkCount());
    const std::uint32_t permanent = rig->startup.empty() ? 0u : rig->startup.front();
    const auto* permanent_ptr = terrain.Published(permanent);

    constexpr int kReaders = 4;
    std::vector<std::unique_ptr<StressReader>> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.push_back(std::make_unique<StressReader>());
        StressReader& reader = *readers.back();
        reader.thread = std::thread([&terrain, &ref, &reader, chunks, r] {
            std::mt19937 rng(1234u + static_cast<unsigned>(r));
            std::uint64_t done = 0;
            while (!reader.stop.load(std::memory_order_relaxed)) {
                const std::uint64_t d = reader.dispatched.load(std::memory_order_acquire);
                if (d == done) {
                    std::this_thread::yield();
                    continue;
                }
                done = d;
                ++reader.ticks;
                // ---- tick: load up to 4 published chunks, hold them ----
                std::uint32_t held_index[4] = {};
                const mx::map::TerrainChunk* held[4] = {};
                int n = 0;
                for (int pick = 0; pick < 4; ++pick) {
                    const std::uint32_t start = rng() % chunks;
                    for (std::uint32_t k = 0; k < chunks; ++k) {
                        const std::uint32_t i = (start + k) % chunks;
                        if (const auto* p = terrain.Published(i)) {
                            held_index[n] = i;
                            held[n++] = p;
                            break;
                        }
                    }
                }
                // Tick length 0.8 .. 2.0 ms, independent of the supervisor phase.
                const auto tick_end = Clock::now() + std::chrono::microseconds(800 + rng() % 1200);
                do {
                    for (int h = 0; h < n; ++h) {
                        const auto* p = held[h];
                        const auto* truth = ref.Published(held_index[h]);
                        for (int k = 0; k < 32; ++k) {
                            const std::size_t j = rng() % p->heights16.size();
                            const std::size_t a = rng() % p->attributes.size();
                            reader.reads += 2;
                            reader.bad += p->heights16[j] != truth->heights16[j] ? 1u : 0u;
                            reader.bad += p->attributes[a] != truth->attributes[a] ? 1u : 0u;
                        }
                    }
                } while (Clock::now() < tick_end);
                for (int h = 0; h < n; ++h) {
                    reader.unpublished_while_held += terrain.Published(held_index[h]) != held[h] ? 1u : 0u;
                }
                reader.in_tick.store(false, std::memory_order_release); // ---- tick end ----
            }
        });
    }

    // Supervisor pass, in WorldRuntime::Run order: churn the demand, pump
    // with the quiescent flag computed exactly like PumpTerrain, THEN
    // dispatch the due readers (claim first).
    // Independent, randomised periods (readers 2 .. 4 ms, demand window
    // 3 .. 9 ms): evictions land at arbitrary points of the reader ticks.
    std::mt19937 phase(99);
    std::vector<Clock::time_point> next_dispatch(kReaders, Clock::now());
    std::uint32_t window = 0;
    auto next_shift = Clock::now();
    const auto end = Clock::now() + duration;
    while (Clock::now() < end) {
        const auto now = Clock::now();
        if (now >= next_shift) {
            next_shift = now + std::chrono::microseconds(3000 + phase() % 6000);
            ++window;
        }
        for (std::uint32_t k = 0; k < 6; ++k) {
            s.Demand((window + k) % chunks, now); // a window sweeping the world
        }
        bool quiescent = true;
        for (const auto& reader : readers) {
            quiescent = quiescent && !reader->in_tick.load(std::memory_order_acquire);
        }
        s.Pump(now, quiescent);
        ++out.passes;
        out.quiescent_passes += quiescent ? 1u : 0u;
        for (int r = 0; r < kReaders; ++r) {
            auto& reader = *readers[static_cast<std::size_t>(r)];
            if (now < next_dispatch[static_cast<std::size_t>(r)]) {
                continue;
            }
            bool expected = false;
            if (reader.in_tick.compare_exchange_strong(expected, true)) {
                reader.dispatched.fetch_add(1, std::memory_order_release);
                next_dispatch[static_cast<std::size_t>(r)] = now + std::chrono::microseconds(2000 + phase() % 2000);
            }
        }
        // ~200 us between passes (sleep_for would round up to the OS timer
        // period on Windows).
        const auto next_pass = Clock::now() + 200us;
        while (Clock::now() < next_pass) {
            std::this_thread::yield();
        }
    }
    for (auto& reader : readers) {
        reader->stop.store(true, std::memory_order_relaxed);
    }
    for (auto& reader : readers) {
        reader->thread.join();
        out.ticks += reader->ticks;
        out.reads += reader->reads;
        out.bad += reader->bad;
        out.unpublished_while_held += reader->unpublished_while_held;
    }
    s.Pump(Clock::now(), true);
    s.PublishStats();
    out.stats = s.GetStats();
    out.permanent_kept = permanent_ptr != nullptr && terrain.Published(permanent) == permanent_ptr;
    return out;
}

// A runtime on a streaming package with a scripted chunk source.
struct LifeRig {
    IoRunner runner; // outlives the runtime
    std::shared_ptr<ScriptedSource> source;
    std::unique_ptr<gs::game::WorldRuntime> sim;
};

std::unique_ptr<LifeRig> MakeLifeRuntime(const fs::path& dir, const gs::game::TerrainStreamingConfig& cfg,
                                         std::uint32_t regions,
                                         const std::function<void(ScriptedSource&)>& prepare = {},
                                         const std::function<void(gs::game::WorldRuntime&)>& configure = {})
{
    auto loaded = LoadPackage(dir, mx::map::ResidencyMode::Streaming);
    if (!loaded.world) {
        return nullptr;
    }
    auto rig = std::make_unique<LifeRig>();
    rig->source = std::make_shared<ScriptedSource>(loaded.world->chunk_source);
    if (prepare) {
        prepare(*rig->source);
    }
    loaded.world->chunk_source = rig->source;
    gs::game::PartitionLayout layout;
    layout.regions_x = layout.regions_y = regions;
    rig->sim = std::make_unique<gs::game::WorldRuntime>(rig->runner.io, gs::game::RuntimeIdentity{},
                                                        std::move(*loaded.world), layout, cfg);
    gs::game::PartitionConfig partition;
    partition.scoring.adaptive_enabled = false;
    rig->sim->ConfigurePartition(partition);
    if (configure) {
        configure(*rig->sim);
    }
    return rig;
}

struct PlayerView {
    bool found = false;
    gs::game::Position pos{};
    gs::game::ZoneId zone = 0;
};

PlayerView ViewPlayer(gs::game::WorldRuntime& sim, gs::common::SessionId session)
{
    return ReadWorld(sim, [session](const WorldSnapshot& snap) {
        PlayerView out;
        const auto it = snap.owners.find(session);
        if (it != snap.owners.end() && it->second.zone_index < snap.zones.ZoneCount()) {
            const auto& zone = snap.zones.GetZone(it->second.zone_index);
            const auto e = zone.FindEntity(it->second.net_id);
            if (e.is_valid()) {
                out.found = true;
                out.pos = e.get<gs::game::Position>();
                out.zone = zone.Id();
            }
        }
        return out;
    });
}

struct WorldCheck {
    std::size_t entities = 0;
    std::size_t mobs = 0;
    std::size_t z_mismatch = 0;
    bool duplicate = false;
};

// Supervisor-side: every entity's z against the fully resident reference,
// and NetId uniqueness.
WorldCheck CheckWorld(gs::game::WorldRuntime& sim, const mx::map::ServerTerrain& ref)
{
    return ReadWorld(sim, [&ref](const WorldSnapshot& snap) {
        WorldCheck out;
        std::unordered_set<std::uint32_t> seen;
        for (std::size_t i = 0; i < snap.zones.ZoneCount(); ++i) {
            for (const auto& [net_id, entity] : snap.zones.GetZone(i).Entities()) {
                ++out.entities;
                out.duplicate = out.duplicate || !seen.insert(net_id).second;
                out.mobs += entity.has<gs::game::MobTag>() ? 1u : 0u;
                const auto pos = entity.get<gs::game::Position>();
                const auto h = ref.Height(pos.x, pos.y);
                out.z_mismatch += h.Ok() && h.meters == pos.z ? 0u : 1u;
            }
        }
        return out;
    });
}

// Mobs within `radius` of (x, y): net_id -> position.
std::map<std::uint32_t, gs::game::Position> MobsNear(gs::game::WorldRuntime& sim, float x, float y, float radius)
{
    return ReadWorld(sim, [x, y, radius](const WorldSnapshot& snap) {
        std::map<std::uint32_t, gs::game::Position> out;
        for (std::size_t i = 0; i < snap.zones.ZoneCount(); ++i) {
            for (const auto& [net_id, entity] : snap.zones.GetZone(i).Entities()) {
                if (!entity.has<gs::game::MobTag>()) {
                    continue;
                }
                const auto pos = entity.get<gs::game::Position>();
                if (std::hypot(pos.x - x, pos.y - y) <= radius) {
                    out[net_id] = pos;
                }
            }
        }
        return out;
    });
}

} // namespace

int RunStreamLifecycleScenario()
{
    Checks c{"STREAMLIFE"};
    using gs::game::TerrainStreamingConfig;
    const auto dir = FixtureDir("life_world");
    if (const auto written = mx::map::WritePackage(dir, SpecStream()); !written.ok) {
        c.Report("fixture-write", false, written.error);
        return c.failures;
    }
    auto reference = LoadPackage(dir, mx::map::ResidencyMode::Eager);
    if (!reference.world) {
        c.Report("fixture-load", false, reference.First());
        return c.failures;
    }
    const mx::map::ServerTerrain& ref = reference.world->terrain;

    // ================= L1/L2 lock-free readers vs eviction =================
    {
        const auto ok = RunLifetimeStress(dir, ref, false, 4000ms);
        c.Report("lifetime-tick-readers-never-see-freed-chunk",
                 ok.bad == 0 && ok.unpublished_while_held > 0 && ok.stats.frees > 100 && ok.stats.quarantined > 0 &&
                     ok.stats.peak_accounted_bytes <= ok.budget && ok.permanent_kept,
                 Fmt("4 reader threads (zone-tick protocol, raw pointers held for the whole tick) x 4 s: ticks=%llu "
                     "sample reads=%llu mismatches=%llu | chunks unpublished WHILE a reader held them=%llu | "
                     "evictions=%llu frees=%llu (poisoned+quarantined=%zu) loads=%llu | quiescent passes %llu of "
                     "%llu | peak accounted %zu <= budget %zu | permanent pin kept=%d",
                     static_cast<unsigned long long>(ok.ticks), static_cast<unsigned long long>(ok.reads),
                     static_cast<unsigned long long>(ok.bad),
                     static_cast<unsigned long long>(ok.unpublished_while_held),
                     static_cast<unsigned long long>(ok.stats.evictions),
                     static_cast<unsigned long long>(ok.stats.frees), ok.stats.quarantined,
                     static_cast<unsigned long long>(ok.stats.loads_completed),
                     static_cast<unsigned long long>(ok.quiescent_passes), static_cast<unsigned long long>(ok.passes),
                     ok.stats.peak_accounted_bytes, ok.budget, ok.permanent_kept ? 1 : 0));
        const auto broken = RunLifetimeStress(dir, ref, true, 4000ms);
        c.Report("lifetime-negative-control-early-free-detected",
                 broken.bad > 0,
                 Fmt("same stress, freeing WITHOUT waiting for a quiescent window (deliberately broken): "
                     "mismatches=%llu in %llu reads (chunks unpublished while held=%llu, frees=%llu) -- the stress "
                     "detects a premature free, so its zero above is evidence",
                     static_cast<unsigned long long>(broken.bad), static_cast<unsigned long long>(broken.reads),
                     static_cast<unsigned long long>(broken.unpublished_while_held),
                     static_cast<unsigned long long>(broken.stats.frees)));
    }

    // ============ L3 late completion across migration, retire, reclaim, slot reuse ============
    {
        TerrainStreamingConfig cfg;
        cfg.budget_bytes = 8u << 20;
        cfg.retain_seconds = 1.0;
        auto rig = MakeLifeRuntime(dir, cfg, 1);
        auto& sim = *rig->sim;
        constexpr gs::common::SessionId kRunner = 95101;
        sim.PostSpawn(DetachedSession(rig->runner.io, kRunner), MakeCharacter(101), std::nullopt);
        sim.Start();
        WaitFor(5000ms, [&] { return ViewPlayer(sim, kRunner).found; });
        rig->source->Gate(true); // every chunk read from now on blocks until opened
        std::atomic<bool> running{true};
        std::thread mover([&] {
            std::uint32_t seq = 0;
            while (running.load()) {
                sim.PostMoveInput(kRunner, ++seq, -1.5707963f, gs::game::MoveState::Running);
                std::this_thread::sleep_for(50ms);
            }
            sim.PostMoveInput(kRunner, ++seq, -1.5707963f, gs::game::MoveState::Idle);
        });
        // The runner reaches the x = 0 chunk border; the chunk west of it is
        // demanded and its read is in flight (gated).
        const bool waiting = WaitFor(8000ms, [&] {
            const auto st = sim.GetTerrainStats();
            return st.queries.steps_waiting > 0 && st.streamer.loading > 0;
        });
        const auto before = sim.GetTerrainStats();
        const auto reclaim0 = sim.ZoneReclaimStats();
        std::set<gs::game::ZoneId> zones_seen{ViewPlayer(sim, kRunner).zone};
        auto leaf_ids = [&] {
            return ReadWorld(sim, [](const WorldSnapshot& snap) {
                std::vector<gs::game::ZoneId> ids;
                for (const auto* leaf : snap.zones.GetActiveLeaves()) {
                    ids.push_back(leaf->zone_id);
                }
                return ids;
            });
        };
        auto force = [&](bool split, gs::game::ZoneId id) {
            const auto m0 = sim.PartitionMetricsSnapshot();
            split ? sim.PostForceSplit(id) : sim.PostForceMerge(id);
            return WaitFor(15000ms, [&] {
                const auto m = sim.PartitionMetricsSnapshot();
                return split ? m.split_commits > m0.split_commits : m.merge_commits > m0.merge_commits;
            });
        };
        bool steps = true;
        for (int cycle = 0; cycle < 3; ++cycle) {
            const auto root = leaf_ids();
            steps = !root.empty() && force(true, root.front()) && steps;
            std::this_thread::sleep_for(300ms); // > 2 ticks: the retired root is reclaimed
            zones_seen.insert(ViewPlayer(sim, kRunner).zone);
            steps = force(false, root.front()) && steps;
            std::this_thread::sleep_for(300ms);
            zones_seen.insert(ViewPlayer(sim, kRunner).zone);
        }
        const auto mid = sim.GetTerrainStats();
        const auto p_mid = ViewPlayer(sim, kRunner);
        const auto reclaim1 = sim.ZoneReclaimStats();
        const bool still_pending = mid.streamer.loading > 0 && p_mid.found && p_mid.pos.x > -0.5f &&
                                   mid.streamer.loads_completed == before.streamer.loads_completed;
        rig->source->OpenAll(); // the late completion arrives now
        const bool crossed = WaitFor(5000ms, [&] { return ViewPlayer(sim, kRunner).pos.x < -10.0f; });
        running.store(false);
        mover.join();
        std::this_thread::sleep_for(200ms);
        const auto after = sim.GetTerrainStats();
        const auto w = CheckWorld(sim, ref);
        const std::string audit = AuditNow(sim);
        c.Report("late-completion-after-migration-retire-reclaim-slot-reuse",
                 waiting && steps && still_pending && crossed && reclaim1.reused_total > reclaim0.reused_total &&
                     zones_seen.size() >= 3 && after.streamer.stale_completions == before.streamer.stale_completions &&
                     after.streamer.loads_completed > before.streamer.loads_completed && w.z_mismatch == 0 &&
                     !w.duplicate && audit == "OK",
                 Fmt("read in flight (gated) while the runner waits at x=%.2f; 3 x split+merge meanwhile: zones the "
                     "runner lived in=%zu, slots reclaimed %llu -> %llu, reused %llu -> %llu; still pending=%d; "
                     "opened -> crossed=%d (x=%.1f), stale completions %llu -> %llu (the completion carries no zone: "
                     "accepted), loads %llu -> %llu; entities=%zu z mismatch=%zu duplicate=%d audit=%s",
                     p_mid.pos.x, zones_seen.size(), static_cast<unsigned long long>(reclaim0.reclaimed_total),
                     static_cast<unsigned long long>(reclaim1.reclaimed_total),
                     static_cast<unsigned long long>(reclaim0.reused_total),
                     static_cast<unsigned long long>(reclaim1.reused_total), still_pending ? 1 : 0, crossed ? 1 : 0,
                     ViewPlayer(sim, kRunner).pos.x, static_cast<unsigned long long>(before.streamer.stale_completions),
                     static_cast<unsigned long long>(after.streamer.stale_completions),
                     static_cast<unsigned long long>(before.streamer.loads_completed),
                     static_cast<unsigned long long>(after.streamer.loads_completed), w.entities, w.z_mismatch,
                     w.duplicate ? 1 : 0, audit.c_str()));
        rig->source->OpenAll();
        sim.Stop();
    }

    // ============ L4 sleeping zone releases terrain; wake waits for data ============
    {
        TerrainStreamingConfig cfg;
        cfg.budget_bytes = 8u << 20;
        cfg.retain_seconds = 0.3;
        auto rig = MakeLifeRuntime(dir, cfg, 2, {}, [](gs::game::WorldRuntime& sim) {
            gs::game::LodConfig lod;
            lod.demote_full_sec = 0.2f;
            lod.demote_reduced_sec = 0.2f;
            lod.demote_low_sec = 0.2f;
            sim.ConfigureSimulationLod(lod);
        });
        auto& sim = *rig->sim;
        constexpr float kSx = -1500.0f;
        constexpr float kSy = -1500.0f;
        const std::uint32_t circle[4] = {0u, 1u, 8u, 9u}; // chunks of the SW spawn circle
        struct SwState {
            bool sleeping = false;
            std::size_t resident = 0;
        };
        auto sw_state = [&] {
            return ReadWorld(sim, [&](const WorldSnapshot& snap) {
                SwState out;
                const std::size_t zi = snap.zones.FindIndexForPosition(kSx, kSy);
                out.sleeping = zi < snap.zones.ZoneCount() &&
                               snap.zones.GetZone(zi).Activity() == gs::game::ZoneActivity::Sleeping;
                for (const std::uint32_t i : circle) {
                    out.resident += sim.Terrain().Terrain()->Published(i) != nullptr ? 1u : 0u;
                }
                return out;
            });
        };
        sim.Start(); // nobody in the world: zones whose mobs turn Dormant go to sleep
        const bool slept = WaitFor(10000ms, [&] { return sw_state().sleeping; });
        // Budget pressure stand-in: evict what nobody demanded for 0.5 s
        // (re-posted until the sleeping zone's chunks have been idle long
        // enough).
        const bool released = WaitFor(5000ms, [&] {
            sim.PostTerrainEvictIdleForTest(0.5);
            std::this_thread::sleep_for(50ms);
            const auto st = sw_state();
            return st.sleeping && st.resident == 0;
        });
        const auto mobs0 = MobsNear(sim, kSx, kSy, 260.0f);
        const auto w0 = CheckWorld(sim, ref);
        c.Report("dormant-zone-does-not-pin-terrain",
                 slept && released && mobs0.size() == 30 && w0.z_mismatch == 0,
                 Fmt("SW zone sleeping=%d; its 4 spawn-circle chunks evicted=%d while its %zu mobs stay (z exact: "
                     "mismatch=%zu of %zu entities)",
                     slept ? 1 : 0, released ? 1 : 0, mobs0.size(), w0.z_mismatch, w0.entities));
        // Wake: a player enters the SW zone at the circle centre (its chunk
        // made resident first); every other read is gated from now on, so
        // the woken mobs standing on the three evicted circle chunks must
        // wait -- until the gate opens.
        sim.PostTerrainDemand(kSx, kSy, 1.0f);
        WaitFor(3000ms, [&] {
            return ReadWorld(sim, [&](const WorldSnapshot&) { return sim.Terrain().Height(kSx, kSy).Ok(); });
        });
        rig->source->Gate(true);
        const auto q0 = sim.GetTerrainStats().queries;
        sim.PostSpawn(DetachedSession(rig->runner.io, 95202), MakeCharacter(202),
                      gs::game::DebugSpawnOverride{kSx, kSy});
        const bool entered = WaitFor(5000ms, [&] { return ViewPlayer(sim, 95202).found; });
        // The mobs are promoted by proximity (activity field, ~1 Hz); a mob
        // walks only between 3..8 s idle pauses, so wait until the walking
        // ones have had at least 20 steps onto missing data refused.
        const bool tried = WaitFor(12000ms, [&] {
            return sim.GetTerrainStats().queries.steps_waiting >= q0.steps_waiting + 20;
        });
        struct Early {
            std::size_t waiting_mobs = 0;       // mobs on a chunk that is still not resident
            std::size_t moved_without_data = 0; // ... that moved anyway
            std::size_t z_mismatch = 0;
        };
        const auto early = ReadWorld(sim, [&](const WorldSnapshot& snap) {
            Early out;
            const auto* terrain = sim.Terrain().Terrain();
            for (std::size_t i = 0; i < snap.zones.ZoneCount(); ++i) {
                for (const auto& [net_id, entity] : snap.zones.GetZone(i).Entities()) {
                    const auto pos = entity.get<gs::game::Position>();
                    const auto h = ref.Height(pos.x, pos.y);
                    out.z_mismatch += h.Ok() && h.meters == pos.z ? 0u : 1u;
                    const auto it = mobs0.find(net_id);
                    if (it == mobs0.end()) {
                        continue;
                    }
                    if (terrain->Published(terrain->ChunkIndexOf(it->second.x, it->second.y)) == nullptr) {
                        ++out.waiting_mobs;
                        out.moved_without_data += (pos.x != it->second.x || pos.y != it->second.y) ? 1u : 0u;
                    }
                }
            }
            return out;
        });
        const auto q1 = sim.GetTerrainStats().queries;
        rig->source->OpenAll(); // the data arrives
        std::this_thread::sleep_for(2500ms);
        const auto mobs1 = MobsNear(sim, kSx, kSy, 400.0f);
        std::size_t moved = 0;
        for (const auto& [net_id, pos] : mobs0) {
            const auto it = mobs1.find(net_id);
            moved += it != mobs1.end() && (it->second.x != pos.x || it->second.y != pos.y) ? 1u : 0u;
        }
        const auto late = sw_state();
        const auto w1 = CheckWorld(sim, ref);
        const std::string audit = AuditNow(sim);
        c.Report("wake-after-eviction-waits-for-data-no-stale-z",
                 entered && tried && early.waiting_mobs > 0 && early.moved_without_data == 0 &&
                     early.z_mismatch == 0 && q1.steps_waiting > q0.steps_waiting && late.resident == 4 && moved > 0 &&
                     w1.z_mismatch == 0 && audit == "OK",
                 Fmt("player entered the sleeping zone, reads gated: steps refused for missing data %llu -> %llu; "
                     "then: mobs standing on not-resident chunks=%zu, moved anyway=%zu, z mismatch=%zu | "
                     "gate opened, 2.5 s later: circle chunks resident=%zu/4, mobs moved=%zu of %zu, z mismatch=%zu, "
                     "audit=%s",
                     static_cast<unsigned long long>(q0.steps_waiting),
                     static_cast<unsigned long long>(q1.steps_waiting), early.waiting_mobs, early.moved_without_data,
                     early.z_mismatch, late.resident, moved, mobs0.size(), w1.z_mismatch, audit.c_str()));
        rig->source->OpenAll();
        sim.Stop();
    }

    // ============ L5 permanently failed chunks at runtime ============
    {
        TerrainStreamingConfig cfg;
        cfg.budget_bytes = 8u << 20;
        cfg.retain_seconds = 1.0;
        cfg.retry_backoff_seconds = 0.01;
        // SW spawn circle (0, 1, 8, 9), the lake's chunk 24 and chunk 35 just
        // west of the player spawn fail every read.
        const std::uint32_t failing[] = {0u, 1u, 8u, 9u, 24u, 35u};
        auto rig = MakeLifeRuntime(dir, cfg, 1, [&](ScriptedSource& s) {
            for (const std::uint32_t i : failing) {
                s.FailAlways(i);
            }
        });
        auto& sim = *rig->sim;
        const auto boot = sim.GetTerrainStats();
        const auto boot_world = CheckWorld(sim, ref);
        const std::uint64_t refused = sim.MobSpawnsRefusedInvalidTerrain();
        c.Report("invalid-chunks-mob-spawn-refused-counted",
                 boot_world.mobs == 90 && refused == 30 && boot.streamer.invalid == 4 && boot_world.z_mismatch == 0,
                 Fmt("SW circle entirely on 4 invalid chunks: mobs spawned=%zu of 120, refused on invalid data=%llu "
                     "(counted, not retried), chunks published invalid=%zu, z mismatch=%zu",
                     boot_world.mobs, static_cast<unsigned long long>(refused), boot.streamer.invalid,
                     boot_world.z_mismatch));
        constexpr gs::common::SessionId kWalker = 95301;
        sim.PostSpawn(DetachedSession(rig->runner.io, kWalker), MakeCharacter(301), std::nullopt);
        sim.Start();
        WaitFor(5000ms, [&] { return ViewPlayer(sim, kWalker).found; });
        std::uint32_t seq = 0;
        const auto until = Clock::now() + 3000ms;
        while (Clock::now() < until) {
            sim.PostMoveInput(kWalker, ++seq, -1.5707963f, gs::game::MoveState::Running);
            sim.PostTerrainDemand(-1600.0f, -400.0f, 1.0f); // keep the lake chunk demanded (retries)
            std::this_thread::sleep_for(50ms);
        }
        sim.PostMoveInput(kWalker, ++seq, -1.5707963f, gs::game::MoveState::Idle);
        std::this_thread::sleep_for(200ms);
        const auto p = ViewPlayer(sim, kWalker);
        const auto st = sim.GetTerrainStats();
        const auto ground = ref.Height(p.pos.x, p.pos.y);
        c.Report("invalid-chunk-movement-refused-no-guess",
                 p.found && p.pos.x >= 0.0f && p.pos.x < 1.0f && ground.Ok() && p.pos.z == ground.meters &&
                     st.queries.invalid > 0 && st.streamer.invalid == 6,
                 Fmt("runner stops at x=%.2f before the invalid chunk (x < 0), z=%.2f == reference %.2f; steps "
                     "refused on invalid data=%llu (not counted as waiting: %llu); chunks invalid=%zu",
                     p.pos.x, p.pos.z, ground.meters, static_cast<unsigned long long>(st.queries.invalid),
                     static_cast<unsigned long long>(st.queries.steps_waiting), st.streamer.invalid));
        struct Probe {
            TerrainStatus height = TerrainStatus::Ok;
            TerrainStatus cell = TerrainStatus::Ok;
            bool walkable = true;
            mx::map::WaterStatus lake = mx::map::WaterStatus::NoWater;
            mx::map::WaterStatus dry = mx::map::WaterStatus::Water;
        };
        const auto probe = ReadWorld(sim, [&sim](const WorldSnapshot&) {
            Probe out;
            const auto h = sim.Terrain().Height(-100.0f, 100.0f);
            const auto cell = sim.Terrain().Cell(-100.0f, 100.0f);
            out.height = h.status;
            out.cell = cell.status;
            out.walkable = cell.Walkable();
            out.lake = sim.Terrain().Water(-1600.0f, -400.0f).status;  // inside the lake, chunk invalid
            out.dry = sim.Terrain().Water(-100.0f, 100.0f).status;     // no water body there at all
            return out;
        });
        c.Report("invalid-chunk-queries-never-land-or-zero",
                 probe.height == TerrainStatus::InvalidData && probe.cell == TerrainStatus::InvalidData &&
                     !probe.walkable && probe.lake == mx::map::WaterStatus::InvalidData &&
                     probe.dry == mx::map::WaterStatus::NoWater,
                 Fmt("inside an invalid chunk: height=%s cell=%s walkable=%d; lake point on an invalid chunk: "
                     "water=%s (depth needs the ground); outside every water body: %s (the body list is complete "
                     "package data)",
                     mx::map::ToString(probe.height), mx::map::ToString(probe.cell), probe.walkable ? 1 : 0,
                     mx::map::ToString(probe.lake), mx::map::ToString(probe.dry)));
        gs::game::NavRequest into;
        into.start_x = 100.0f;
        into.start_y = 100.0f;
        into.goal_x = -100.0f;
        into.goal_y = 100.0f;
        gs::game::NavRequest beside = into;
        beside.start_y = -100.0f;
        beside.goal_x = -300.0f;
        beside.goal_y = -100.0f;
        beside.max_expansions = 400000;
        const std::uint64_t id_into = sim.PostNavigationRequest(into);
        const std::uint64_t id_beside = sim.PostNavigationRequest(beside);
        WaitFor(15000ms, [&] {
            return sim.NavigationResult(id_into).status != gs::game::NavStatus::Pending &&
                   sim.NavigationResult(id_beside).status != gs::game::NavStatus::Pending;
        });
        const auto r_into = sim.NavigationResult(id_into);
        const auto r_beside = sim.NavigationResult(id_beside);
        c.Report("runtime-nav-invalid-data-not-no-path",
                 r_into.status == gs::game::NavStatus::InvalidData && r_beside.status == gs::game::NavStatus::Found &&
                     sim.NavigationStats().pinned_chunks == 0,
                 Fmt("goal in the invalid chunk: %s (waited %u pass(es) for other chunks); goal beside it: %s %.1f m; "
                     "pins after both jobs=%zu",
                     gs::game::ToString(r_into.status), r_into.chunk_waits, gs::game::ToString(r_beside.status),
                     r_beside.length_m, sim.NavigationStats().pinned_chunks));
        sim.Stop();
    }

    // ============ L6 the startup set above the budget ============
    {
        TerrainStreamingConfig cfg;
        cfg.budget_bytes = 8u << 10; // 8 KB: below a single chunk
        auto rig = MakeLifeRuntime(dir, cfg, 1);
        const auto st = rig->sim->GetTerrainStats();
        const auto w = CheckWorld(*rig->sim, ref);
        c.Report("startup-set-above-budget-detected",
                 st.streamer.accounted_bytes > st.streamer.budget_bytes && st.streamer.resident >= 1 && w.mobs == 0,
                 Fmt("budget %zu B: the pinned startup set alone is accounted at %zu B > budget -> the gameserver "
                     "refuses to start on exactly this condition (exit 2, acceptance 7c); nothing is evicted to "
                     "make it fit (resident=%zu), no mob batch fits (mobs=%zu)",
                     st.streamer.budget_bytes, st.streamer.accounted_bytes, st.streamer.resident, w.mobs));
    }

    std::error_code ec;
    // Keep the generated corpus for the isolated runner's fixture SHA256 manifest.
    (void)ec;
    std::printf("STREAMLIFE-DONE passes=%d failures=%d\n", c.passes, c.failures);
    return c.failures;
}

int RunStreamAdmissionScenario()
{
    using namespace gs::game;
    Checks c{"ADMISSION"};
    const auto dir=FixtureDir("admission");
    const auto write=mx::map::WritePackage(dir,SpecStream());
    if(!write.ok) {c.Report("fixture",false,write.error); return c.failures;}
    auto reference=LoadPackage(dir,mx::map::ResidencyMode::Eager);
    if(!reference.world) {c.Report("reference",false,reference.First()); return c.failures;}
    TerrainStreamingConfig config;
    config.budget_bytes=8u<<20; config.max_waiting=8; config.max_in_flight=1;
    config.io_threads=1; config.retain_seconds=60;
    auto request=[](Clock::time_point now=Clock::now(),double seconds=5.0) {
        return std::make_shared<TerrainRequest>(now,seconds);
    };
    {
        auto rig=MakeRig(dir,config); auto& s=*rig->streamer;
        rig->source->Gate(true);
        const auto began=Clock::now();
        for(unsigned i=0;i<6;++i) s.Demand(i,began,TerrainPriority::Prefetch);
        auto urgent=request(began); s.Request(urgent,{7});
        s.Pump(began,true);
        WaitFor(2000ms,[&]{return rig->source->Concurrent()==1;});
        c.Report("full-background-ingress-admits-operation",rig->source->Calls(7)==1 && rig->source->Calls(0)==0,
            "one bounded queue, reserved ingress, physical load starts with blocking operation");
        auto other=request(began); s.Request(other,{7},TerrainPriority::Active);
        for(unsigned i=0;i<100;++i) s.Request(other,{7},TerrainPriority::Active);
        urgent->Cancel(); s.Pump(began+10ms,true);
        rig->source->OpenAll();
        const bool ready=PumpUntil(s,[&]{return other->Status()==TerrainRequestStatus::Ready;});
        c.Report("shared-flight-cancel-one-consumer",ready && rig->source->Calls(7)==1 &&
            urgent->Status()==TerrainRequestStatus::Cancelled,"one physical read, surviving logical request completes");
        s.PublishStats();
        c.Report("logical-refresh-is-idempotent",ready && s.GetStats().requests_accepted==2 &&
            s.GetStats().requests_rejected==0 && other->started==began,
            "100 refreshes keep one registration and its original deadline/age; no accepted handle is rejected");
        c.Report("height-oracle",ready && SameChunk(rig->terrain->Published(7),reference.world->terrain.Published(7)),
            "independent fully resident provider");
        s.EvictUndemanded(Clock::now()+100s,true);
        c.Report("ready-handoff-is-pinned",rig->terrain->Published(7)!=nullptr,"consumer has not consumed yet");
        other->Consume(); s.Pump(Clock::now(),true);
        s.EvictUndemanded(Clock::now()+100s,true);
        c.Report("consumed-releases-pin",rig->terrain->Published(7)==nullptr && other->Status()==TerrainRequestStatus::Consumed,
            "no pin leak after handoff");
        auto short_lived=request(); s.Request(short_lived,{rig->startup.front()}); s.Pump(Clock::now(),true);
        short_lived->Consume(); short_lived.reset(); s.Pump(Clock::now(),true); s.PublishStats();
        c.Report("consume-then-drop-keeps-acknowledgement",s.GetStats().requests_consumed==2,
            "normal asynchronous handoff closes as consumed, never as a cancelled operation");
    }
    {
        auto rig=MakeRig(dir,config); auto& s=*rig->streamer;
        rig->source->Gate(true);
        const auto now=Clock::now();
        s.Demand(0,now,TerrainPriority::Prefetch);
        auto urgent=request(now); s.Request(urgent,{8,9,10,11,12,13,14,15});
        s.Pump(now,true);
        WaitFor(2000ms,[&]{return rig->source->Concurrent()==1;});
        rig->source->OpenAll();
        const bool low=PumpUntil(s,[&]{const auto order=rig->source->Order();
            return std::find(order.begin(),order.end(),0)!=order.end();},5000ms,true,
            [&]{s.Demand(0,Clock::now(),TerrainPriority::Prefetch);});
        const auto order=rig->source->Order();
        const auto pos=std::find(order.begin(),order.end(),0);
        c.Report("weighted-fairness-low-class-progress",low && pos!=order.end() && std::distance(order.begin(),pos)<=4,
            "prefetch receives its share despite pending higher-class requests");
    }
    {
        auto rig=MakeRig(dir,config); auto& s=*rig->streamer;
        rig->source->Gate(true);
        const auto now=Clock::now();
        s.Demand(1,now); s.Pump(now,true);
        WaitFor(2000ms,[&]{return rig->source->Calls(1)==1;});
        auto cancelled=request(now); s.Request(cancelled,{2});
        s.Demand(2,now,TerrainPriority::Prefetch); s.Pump(now,true);
        cancelled->Cancel(); s.Pump(now+1ms,true);
        rig->source->OpenAll();
        const bool ready=PumpUntil(s,[&]{return s.State(2)==TerrainStreamer::ChunkState::Ready;});
        s.PublishStats();
        c.Report("cancel-downgrades-shared-chunk-priority",ready &&
            cancelled->Status()==TerrainRequestStatus::Cancelled && s.GetStats().admitted_by_class[0]==0 &&
            s.GetStats().admitted_by_class[2]==1,"remaining prefetch survives; removed urgent consumer leaves no priority boost");
    }
    {
        auto rig=MakeRig(dir,config); auto& s=*rig->streamer;
        rig->source->SetReservationFileBytes(0,config.budget_bytes);
        s.Demand(0,Clock::now()); s.Demand(1,Clock::now());
        const bool small=PumpUntil(s,[&]{return s.State(1)==TerrainStreamer::ChunkState::Ready;});
        s.PublishStats();
        c.Report("variable-reservation-no-head-of-line-stall",small && rig->source->Calls(0)==0 &&
            s.State(0)==TerrainStreamer::ChunkState::Waiting && s.GetStats().peak_accounted_bytes<=config.budget_bytes,
            "scripted oversized read buffer stays waiting; later fitting chunk loads within the same budget");
    }
    {
        auto expiring=config; expiring.retain_seconds=0.1;
        auto rig=MakeRig(dir,expiring); auto& s=*rig->streamer;
        rig->source->Gate(true);
        const auto old=Clock::now()-2s;
        for(unsigned i=0;i<7;++i) s.Demand(i,old,TerrainPriority::Prefetch);
        s.Pump(old,true);
        WaitFor(2000ms,[&]{return rig->source->Calls(0)==1;});
        s.Pump(Clock::now(),true); // other anonymous leases end
        rig->source->OpenAll();
        const bool initial=PumpUntil(s,[&]{return s.State(0)==TerrainStreamer::ChunkState::Ready;});
        s.BeginMeasurementWindow(Clock::now());
        s.Demand(6,Clock::now());
        const bool fresh=PumpUntil(s,[&]{return s.State(6)==TerrainStreamer::ChunkState::Ready;});
        c.Report("new-demand-after-expired-rejection-has-new-age",initial && fresh &&
            s.GetStats().window_miss_samples==1 && s.GetStats().window_miss_p99_ms<1000,
            "ended anonymous episode is not charged to a later new operation; continuous refresh still preserves age");
    }
    {
        auto rig=MakeRig(dir,config);
        std::string error;
        const bool loaded=rig->streamer->LoadBlocking({0,7},error);
        rig->streamer.reset();
        TerrainService terrain(std::move(*rig->terrain));
        TerrainStreamer streamer(*terrain.MutableTerrain(),rig->source,config);
        NavigationService nav;
        NavRequest left; left.start_x=-1800; left.start_y=-1800; left.goal_x=1800; left.goal_y=-1800;
        left.timeout_seconds=1; left.max_expansions=200000;
        auto right=left; std::swap(right.start_x,right.goal_x);
        const auto now=Clock::now();
        rig->source->Gate(true);
        nav.Request(1,left,now); nav.Request(2,right,now);
        nav.Pump(terrain,&streamer,now,200000);
        const auto partial=nav.GetStats();
        c.Report("multi-navigation-partial-pins",loaded && partial.running==2 && partial.pinned_chunks>=2,
            "two jobs read disjoint resident chunks and then wait for missing terrain");
        nav.Pump(terrain,&streamer,now+2s,0);
        streamer.Pump(now+2s,true);
        c.Report("navigation-deadline-releases-pins-with-zero-work-budget",nav.GetStats().pinned_chunks==0 &&
            nav.GetStats().running==0 && nav.Result(1).status==NavStatus::NotResident &&
            nav.Result(2).status==NavStatus::NotResident,
            "both partial sets close at the original deadline, even with no expansion quota");
        rig->source->OpenAll();
    }
    {
        auto rig=MakeRig(dir,config); auto& s=*rig->streamer;
        rig->source->Gate(true);
        const auto now=Clock::now()-1s;
        s.Demand(1,now); s.Pump(now,true);
        WaitFor(2000ms,[&]{return rig->source->Concurrent()==1;});
        s.Demand(2,now);
        for(unsigned i=1;i<=50;++i) {s.Demand(2,now+std::chrono::milliseconds(i)); s.Pump(now+std::chrono::milliseconds(i),true);}
        s.PublishStats();
        c.Report("refresh-does-not-reset-age-or-duplicate",s.GetStats().waiting==1 && s.GetStats().deduplicated>=49 &&
            s.GetStats().oldest_wait_ms>=900,
            "one waiting chunk after 50 refreshes; original logical handle start is immutable");
        const auto before=s.GetStats();
        const auto reserved=ChunkReservation(*rig->terrain,*rig->source,1);
        c.Report("independent-reservation-ledger",before.in_flight_bytes==reserved &&
            before.accounted_bytes==before.metadata_bytes+rig->terrain->ResidentBytes()+
                rig->terrain->ResidentChunkCount()*(sizeof(mx::map::TerrainChunk)+64)+reserved,
            "test recomputes payload/file reservation independently of Accounted()");
        s.ResetGenerationForTest(false); s.PublishStats();
        c.Report("stale-generation-keeps-live-io-reservation",s.GetStats().in_flight_bytes==reserved,
            "gated worker still owns its read/decode reservation after reset");
        rig->source->OpenAll();
        const bool late=PumpUntil(s,[&]{return s.GetStats().stale_completions>0;});
        c.Report("late-result-releases-without-publish",late && rig->terrain->Published(1)==nullptr &&
            s.GetStats().in_flight_bytes==0,"old generation completion is discarded and accounted until destruction");
    }
    {
        auto probe=MakeRig(dir,config);
        probe->streamer->PublishStats();
        const auto memory=probe->streamer->GetStats();
        const auto reservation=ChunkReservation(*probe->terrain,*probe->source,0);
        auto small=config; small.budget_bytes=memory.metadata_bytes+memory.pinned_bytes+3*reservation;
        auto rig=MakeRig(dir,small); auto& s=*rig->streamer;
        std::string error;
        const bool loaded=s.LoadBlocking({0,1},error);
        const auto now=Clock::now()+1s;
        s.Demand(0,now); s.Demand(1,now); // soft retention remains live
        auto need=request(now,10); s.Request(need,{2,3,4});
        bool ready=false;
        for(unsigned i=0;i<3000 && !ready;++i) {
            s.Pump(now+std::chrono::milliseconds(i),true);
            ready=need->Status()==TerrainRequestStatus::Ready;
            if(!ready) std::this_thread::sleep_for(1ms);
        }
        s.PublishStats();
        c.Report("soft-retention-yields-under-pressure",loaded && ready && s.GetStats().pressure_evictions>0 &&
            s.GetStats().peak_accounted_bytes<=small.budget_bytes,error);
        auto too_big=request(); s.Request(too_big,{8,9,10,11,12,13,14,15});
        auto tiny=request(); s.Request(tiny,{2}); s.Pump(now+4s,true);
        c.Report("oversized-request-does-not-block-small",too_big->Status()==TerrainRequestStatus::CapacityRejected &&
            tiny->Status()==TerrainRequestStatus::Ready,"minimal reservation tested before joining admission");
        auto pinned=rig->terrain->Owned(2);
        need->Cancel(); tiny->Cancel(); s.Pump(now+4s,true);
        s.ResetGenerationForTest(true); s.PublishStats();
        const auto held=s.GetStats().retired_bytes;
        c.Report("retired-reader-pin-still-accounted",held>=pinned->Bytes()+sizeof(mx::map::TerrainChunk)+64,
            "reset cannot reclaim a live pin, even in a quiescent pass");
        pinned.reset(); s.Pump(now+5s,true); s.PublishStats();
        c.Report("pin-release-enables-reclaim",s.GetStats().retired_bytes==0,"all retired ownership released");
    }
    {
        auto rig=MakeRig(dir,config); auto& s=*rig->streamer;
        std::vector<TerrainRequestHandle> handles;
        for(std::size_t i=0;i<TerrainStreamer::kMaxRequests+1;++i) {
            auto h=request(); s.Request(h,{rig->startup.front()}); handles.push_back(h);
        }
        c.Report("logical-ingress-bounded",handles.back()->Status()==TerrainRequestStatus::CapacityRejected,
            "64 operations, no secondary unbounded queue");
        for(auto& h:handles) h->Cancel(); s.Pump(Clock::now(),true);
        auto deadline=request(Clock::now()-2s,1); s.Request(deadline,{3}); s.Pump(Clock::now(),true);
        c.Report("monotonic-deadline-terminal",deadline->Status()==TerrainRequestStatus::TimedOut,"no simulation clock dependency");
        auto dropped=request(); s.Request(dropped,{4}); dropped.reset(); s.Pump(Clock::now(),true); s.PublishStats();
        c.Report("last-handle-drop-cancels",s.GetStats().requests_pending==0 && s.GetStats().requests_cancelled>=65,
            "bounded request state does not retain a dead session/entity");
        auto shutdown=request(); s.Request(shutdown,{5});
        auto shutdown_wait=std::async(std::launch::async,[shutdown]{return shutdown->WaitUntilReadyOrTerminal();});
        s.Stop();
        c.Report("shutdown-closes-request",shutdown->Status()==TerrainRequestStatus::Cancelled,"terminal result, no callback into consumer");
        c.Report("tc4-streamer-stop-wakes-waiter",shutdown_wait.get()==TerrainRequestStatus::Cancelled,"request-owned wait survives owner stop without callback");
    }
    {
        auto rig=MakeRig(dir,config);auto& s=*rig->streamer;
        auto h=request();s.Request(h,{rig->startup.front()});
        const bool ready=PumpUntil(s,[&]{return h->Status()==TerrainRequestStatus::Ready;});
        const bool observed=h->WaitUntilReadyOrTerminal()==TerrainRequestStatus::Ready;
        s.PublishStats();const auto retained=s.GetStats().pinned_bytes;
        h->Cancel();h->Consume();h->Consume();s.Pump(h->deadline+1ms,true);s.PublishStats();
        c.Report("tc4-ready-wait-does-not-consume-or-unpin",ready && observed && retained>0,"Ready observation retains operation pins until owner acknowledgement pass");
        c.Report("tc4-cancel-consume-deadline-owner-precedence",h->Status()==TerrainRequestStatus::Cancelled && s.GetStats().requests_consumed==0 && s.GetStats().requests_cancelled==1,"existing owner cancel-before-consume-before-timeout order unchanged");
    }
    {
        auto rig=MakeRig(dir,config); auto& s=*rig->streamer;
        auto h=request(); s.Request(h,{1});
        const bool done=PumpUntil(s,[&]{return h->Status()==TerrainRequestStatus::Ready;});
        s.BeginMeasurementWindow(Clock::now());
        const auto empty=s.GetStats();
        c.Report("empty-measure-window-is-na",done && empty.window_miss_samples==0 && std::isnan(empty.window_miss_p99_ms),
            "setup sample is not inherited; no fake zero latency");
        auto next=request(); s.Request(next,{2});
        const bool again=PumpUntil(s,[&]{return next->Status()==TerrainRequestStatus::Ready;});
        c.Report("window-counts-new-completions",again && s.GetStats().window_miss_samples==1 &&
            std::isfinite(s.GetStats().window_miss_p99_ms),"one new physical miss completion in this interval");
    }
    // SL-2 supported concurrency points: no secondary queues and one read per shared chunk.
    for(unsigned count : {1u,8u,32u,64u}) for(unsigned width : {1u,4u,16u}) {
        auto rig=MakeRig(dir,config); auto& s=*rig->streamer;
        std::vector<TerrainRequestHandle> handles;
        std::vector<std::uint32_t> chunks;
        for(unsigned j=0;j<width;++j) chunks.push_back(j+1);
        for(unsigned i=0;i<count;++i) {auto h=request();s.Request(h,chunks);handles.push_back(h);}
        const bool ready=PumpUntil(s,[&]{return std::all_of(handles.begin(),handles.end(),[](auto& h){return h->Status()==TerrainRequestStatus::Ready;});});
        bool stamps=true, single=true;
        for(auto& h:handles){h->Consume();stamps &= h->registered_us.load()>0 && h->ready_us.load()>=h->registered_us.load() && h->consumed_us.load()>=h->ready_us.load();}
        s.Pump(Clock::now(),true);s.PublishStats();
        for(auto index:chunks) single &= rig->source->Calls(index)<=1;
        c.Report("concurrent-shared-"+std::to_string(count)+"x"+std::to_string(width),ready && stamps && single && s.GetStats().requests_consumed==count && s.GetStats().peak_accounted_bytes<=config.budget_bytes,
            Fmt("registered=%llu consumed=%llu width=%u metadata=%zu peak=%zu budget=%zu",(unsigned long long)s.GetStats().requests_accepted,(unsigned long long)s.GetStats().requests_consumed,width,s.GetStats().metadata_bytes,s.GetStats().peak_accounted_bytes,config.budget_bytes));
    }
    {
        auto rig=MakeRig(dir,config);auto& s=*rig->streamer;
        std::vector<TerrainRequestHandle> handles;
        for(unsigned i=0;i<64;++i){auto h=request();s.Request(h,{i});handles.push_back(h);}
        const bool ready=PumpUntil(s,[&]{return std::all_of(handles.begin(),handles.end(),[](auto& h){return h->Status()==TerrainRequestStatus::Ready;});});
        for(auto& h:handles)h->Consume();s.Pump(Clock::now(),true);s.PublishStats();
        c.Report("concurrent-distinct-64",ready && s.GetStats().requests_consumed==64,"64 distinct chunks fit; no sequential-only acceptance claim");
        std::vector<std::uint32_t> oversized(17);auto h=request();s.Request(h,oversized);
        c.Report("operation-over-16-refused",h->Status()==TerrainRequestStatus::CapacityRejected,"no silent truncation");
    }
    {
        auto loaded=LoadPackage(dir,mx::map::ResidencyMode::Streaming);IoRunner runner;
        WorldRuntime sim(runner.io,{},std::move(*loaded.world),PartitionLayout{},config);
        std::vector<TerrainRequestHandle> handles;
        for(unsigned i=0;i<65;++i) handles.push_back(sim.PrepareTerrain(-1500,-1500,0));
        c.Report("command-ingress-before-owner-is-bounded",handles.back()->Status()==TerrainRequestStatus::CapacityRejected && sim.GetTerrainStats().ingress_rejected==1,"64 pending commands distinct from 64 registered streamer operations");
        sim.Start();
        const bool ready=WaitFor(5000ms,[&]{return std::all_of(handles.begin(),handles.end()-1,[](auto& h){return h->Status()==TerrainRequestStatus::Ready;});});
        for(auto& h:handles)h->Consume();
        c.Report("accepted-ingress-drains-to-owner",ready,"accepted commands are not overwritten by the 65th rejection");
        sim.Stop();
    }
    {
        auto cc=config;cc.max_waiting=64;
        auto rig=MakeRig(dir,cc);auto& s=*rig->streamer;
        std::vector<TerrainRequestHandle> urgent;
        for(unsigned i=1;i<=30;++i){auto h=request();s.Request(h,{i});urgent.push_back(h);}
        auto active=request();s.Request(active,{31},TerrainPriority::Active);
        const bool low=PumpUntil(s,[&]{return active->Status()==TerrainRequestStatus::Ready && s.State(32)==TerrainStreamer::ChunkState::Ready;},5000ms,true,
            [&]{s.Demand(32,Clock::now(),TerrainPriority::Prefetch);});
        s.PublishStats();const auto order=rig->source->Order();
        const auto a=std::find(order.begin(),order.end(),31),p=std::find(order.begin(),order.end(),32);
        c.Report("sustained-admission-active-prefetch-opportunities",low && a!=order.end() && p!=order.end() &&
            std::distance(order.begin(),a)<=6 && std::distance(order.begin(),p)<=6,
            Fmt("30 accepted Admission operations continuously pending; admissions by class=%llu/%llu/%llu (opportunities, not throughput SLA)",
                (unsigned long long)s.GetStats().admitted_by_class[0],(unsigned long long)s.GetStats().admitted_by_class[1],(unsigned long long)s.GetStats().admitted_by_class[2]));
    }
    {
        auto rig=MakeRig(dir,config);auto& s=*rig->streamer;std::string error;
        const bool loaded=s.LoadBlocking({1},error);
        s.BeginMeasurementWindow(Clock::now());
        s.EvictUndemanded(Clock::now()+100s,true); // unit-test seam only; never used by C-moving
        s.Demand(1,Clock::now());
        const bool again=PumpUntil(s,[&]{return s.State(1)==TerrainStreamer::ChunkState::Ready;});
        s.PublishStats();const auto measured=s.GetStats();
        const auto trace=measured.reload_trace[0];
        c.Report("reload-trace-ordered-same-window",loaded && again && measured.reloads==1 && measured.reload_trace_count==1 && trace.chunk==1 &&
            trace.evicted_us<=trace.requested_us && trace.requested_us<=trace.published_us && trace.evicted_us>=measured.window_started_us,
            "same ChunkKey/generation: eviction then demand then publication; timestamps are monotonic");
        s.BeginMeasurementWindow(Clock::now());s.Demand(1,Clock::now());s.PublishStats();
        c.Report("window-reset-does-not-inherit-reload",s.GetStats().reloads==0 && s.GetStats().unique_requested==1 && s.State(1)==TerrainStreamer::ChunkState::Ready,
            "observation-only reset; cache preserved, warmup events excluded");
    }
    // Real supervisor + saturated single-worker queue. The first interval
    // makes NO snapshot/audit requests. The second deliberately floods the
    // existing snapshot API. Assertions use counters, final audit only later.
    for(const bool snapshots : {false,true}) {
        auto spec=SpecStream();
        spec.mob_spawns="mob_type_id=1 x=-1500 y=-1500 count=25000 radius=200\n"
                        "mob_type_id=1 x=1500 y=-1500 count=25000 radius=200\n"
                        "mob_type_id=1 x=-1500 y=1500 count=25000 radius=200\n"
                        "mob_type_id=1 x=1500 y=1500 count=25000 radius=200\n";
        const auto runtime_dir=FixtureDir(snapshots ? "admission-snapshot" : "admission-no-snapshot");
        const auto written=mx::map::WritePackage(runtime_dir,spec);
        auto loaded=LoadPackage(runtime_dir,mx::map::ResidencyMode::Streaming);
        if(!written.ok || !loaded.world) {c.Report("runtime-fixture",false,loaded.First()); continue;}
        IoRunner runner;
        TerrainStreamingConfig cc; cc.budget_bytes=512u<<10; cc.retain_seconds=0.5;
        PartitionLayout layout; layout.regions_x=layout.regions_y=2;
        WorldRuntime sim(runner.io,{},std::move(*loaded.world),layout,cc);
        sim.ConfigureWorkers(1);
        PartitionConfig partition; partition.scoring.adaptive_enabled=false; sim.ConfigurePartition(partition);
        const auto before=sim.GetTerrainStats();
        const auto captures_before=sim.GetSnapshotStats().requests;
        sim.Start();
        std::atomic<bool> reading{snapshots};
        std::atomic<unsigned> snapshot_errors{0};
        std::thread reader;
        if(snapshots) reader=std::thread([&]{
            while(reading.load()) {
                auto future=sim.CaptureSnapshot<std::uint32_t>([](const WorldSnapshot& s){return s.world_tick;});
                std::uint32_t tick=0;
                if(!sim.WaitSnapshot(future,2000ms,tick)) ++snapshot_errors;
            }
        });
        std::size_t completed=0, failed=0;
        TerrainRequestHandle pending;
        const auto stress_start=Clock::now();
        const auto delay_start=sim.GetSchedulingDelays();
        const auto until=stress_start+5s;
        std::uint32_t next=0;
        while(Clock::now()<until) {
            if(!pending) {
                const auto chunk=(next++*13)%64;
                pending=sim.PrepareTerrain(float(kOX+(chunk%8+0.5)*512),float(kOY+(chunk/8+0.5)*512),0,2);
            }
            const auto status=pending->Status();
            if(status==TerrainRequestStatus::Ready) {++completed; pending->Consume(); pending.reset();}
            else if(status!=TerrainRequestStatus::Pending) {++failed; pending.reset();}
            std::this_thread::sleep_for(5ms);
        }
        reading=false; if(reader.joinable()) reader.join();
        if(pending) pending->Cancel();
        const auto after=sim.GetTerrainStats();
        const auto delay_end=sim.GetSchedulingDelays();
        std::printf("SL2 stress: snapshots=%d observation_ms=%.3f supervisor_cpu_us=%llu drain_max_us=%llu samples=%llu due_enqueue_us=%llu queue_us=%llu deadline_misses=%llu\n",snapshots?1:0,
            std::chrono::duration<double,std::milli>(Clock::now()-stress_start).count(),(unsigned long long)(after.supervisor_cpu_us-before.supervisor_cpu_us),(unsigned long long)after.drain_max_us,
            (unsigned long long)(delay_end.samples-delay_start.samples),(unsigned long long)(delay_end.due_to_enqueue_us-delay_start.due_to_enqueue_us),
            (unsigned long long)(delay_end.queue_us-delay_start.queue_us),(unsigned long long)(delay_end.finish_deadline_misses-delay_start.finish_deadline_misses));
        const auto scheduled=sim.SchedulerStats();
        const auto capture_delta=sim.GetSnapshotStats().requests-captures_before;
        const bool progress=completed>5 && after.streamer.loads_completed>before.streamer.loads_completed &&
            after.streamer.evictions>before.streamer.evictions && after.streamer.frees>before.streamer.frees &&
            scheduled.enqueued>10 && after.safepoints>0 && snapshot_errors==0 &&
            (snapshots ? capture_delta>5 : capture_delta==0) &&
            after.streamer.peak_accounted_bytes<=cc.budget_bytes;
        c.Report(snapshots ? "runtime-progress-with-snapshots" : "runtime-progress-without-snapshots",progress,
            Fmt("completed=%zu failed=%zu loads=%llu evictions=%llu frees=%llu safepoints=%llu drain_us=%llu ticks=%llu snapshots=%llu peak=%zu/%zu",
                completed,failed,(unsigned long long)(after.streamer.loads_completed-before.streamer.loads_completed),
                (unsigned long long)(after.streamer.evictions-before.streamer.evictions),
                (unsigned long long)(after.streamer.frees-before.streamer.frees),(unsigned long long)after.safepoints,
                (unsigned long long)after.drain_wait_us,(unsigned long long)scheduled.enqueued,
                (unsigned long long)capture_delta,after.streamer.peak_accounted_bytes,cc.budget_bytes));
        c.Report(snapshots ? "runtime-final-audit-snapshots" : "runtime-final-audit-no-snapshots",
            AuditNow(sim)=="OK","audit is outside the measured no-snapshot interval");
        sim.Stop();
        if(!snapshots) {
            auto after_stop=sim.PrepareTerrain(0,0,0);
            c.Report("runtime-request-after-stop-is-terminal",after_stop->Status()==TerrainRequestStatus::Cancelled,
                "no request can be queued after the supervisor final drain");
        }
    }
    std::printf("ADMISSION-DONE passes=%d failures=%d\n",c.passes,c.failures);
    return c.failures;
}

} // namespace gs::bench
