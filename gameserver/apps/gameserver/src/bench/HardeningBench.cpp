#include "HardeningBench.h"
#include "ReadinessBench.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <capnp/message.h>

#include "db/CharacterRepository.h"
#include "db/DbPool.h"
#include "db/HandoffTokenRepository.h"
#include "network/Framing.h"
#include "network/Server.h"
#include "network/Session.h"
#include "protocol/Protocol.h"
#include "protocol/Serialization.h"

#include "../GameConnectionHandler.h"
#include "../world/WorldConstants.h"
#include "../world/WorldRuntime.h"
#include "../world/debug/I1LocalObservation.h"
#include "../world/replication/ProtocolEncoder.h"
#include "../world/replication/ResyncSchedule.h"
#include "../world/activity/LoadFieldTypes.h"
#include "../world/zone/ZoneWorkerPool.h"
#include "../world/partition/RegionDefinition.h"
#include "../world/terrain/TerrainService.h"
#include "map/MapData.h"
#include "map/WorldPackage.h"
#include "map/WorldPackageWriter.h"
#include "schema/world_package_manifest.capnp.h"
#include "BenchSnapshot.h"
#include "BenchWorld.h"

namespace gs::bench {
namespace {

using Clock = std::chrono::steady_clock;
namespace asio = boost::asio;
using asio::ip::tcp;

// Players present in the world (owner map), via a supervisor snapshot.
std::size_t OwnerCount(gs::game::WorldRuntime& sim)
{
    return ReadWorld(sim, [](const WorldSnapshot& snap) { return snap.owners.size(); });
}

constexpr float kHalfPi = 1.5707963f;

gs::db::Character MakeCharacter(std::uint64_t index)
{
    gs::db::Character character;
    character.id = gs::db::CharacterId{9000 + index};
    character.account_id = gs::db::AccountId{19000 + index};
    character.name = "Hardening" + std::to_string(index);
    character.level = 1;
    character.class_id = 1;
    character.created_at = std::chrono::system_clock::now();
    character.last_played_at = character.created_at;
    return character;
}

std::uint32_t ReadU32Le(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

float ReadF32Le(const std::uint8_t* p)
{
    const std::uint32_t bits = ReadU32Le(p);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Client-side view of the wire, filled by the reader thread.
struct WireStats {
    std::mutex mutex;
    std::uint64_t transform_frames = 0;
    std::uint64_t capnp_packets = 0;
    std::uint64_t v1_frames = 0;
    std::uint64_t v2_frames = 0;
    std::uint32_t last_zone_tick = 0;
    bool have_self = false;
    float self_x = 0.0f;
    float self_y = 0.0f;
};

struct WireSnapshot {
    std::uint64_t transform_frames = 0;
    std::uint32_t last_zone_tick = 0;
    float self_x = 0.0f;
    float self_y = 0.0f;
    bool have_self = false;
};

WireSnapshot TakeWire(WireStats& stats)
{
    std::lock_guard lock(stats.mutex);
    return WireSnapshot{stats.transform_frames, stats.last_zone_tick, stats.self_x, stats.self_y,
                        stats.have_self};
}

// Blocking reader: 4-byte big-endian length prefix (Framing), then the
// payload. Transform frames (v1 0x10 / v2 0x11) share the header layout
// codec|opcode|u32 tick|u16 count followed by the viewer's own full 19-byte
// record, so both are decoded identically for self position + zone tick.
void ReadFrames(tcp::socket& socket, WireStats& stats)
{
    try {
        std::vector<std::uint8_t> payload;
        for (;;) {
            std::array<std::uint8_t, 4> header{};
            asio::read(socket, asio::buffer(header));
            const std::uint32_t length = (static_cast<std::uint32_t>(header[0]) << 24) |
                                         (static_cast<std::uint32_t>(header[1]) << 16) |
                                         (static_cast<std::uint32_t>(header[2]) << 8) |
                                         static_cast<std::uint32_t>(header[3]);
            payload.resize(length);
            if (length > 0) {
                asio::read(socket, asio::buffer(payload));
            }
            if (payload.empty()) {
                continue;
            }
            std::lock_guard lock(stats.mutex);
            if (payload[0] == 0) {
                ++stats.capnp_packets;
                continue;
            }
            if (payload.size() < 8 + 19 || (payload[1] != 0x10 && payload[1] != 0x11)) {
                continue;
            }
            ++stats.transform_frames;
            if (payload[1] == 0x10) {
                ++stats.v1_frames;
            } else {
                ++stats.v2_frames;
            }
            stats.last_zone_tick = ReadU32Le(payload.data() + 2);
            stats.self_x = ReadF32Le(payload.data() + 8 + 4);
            stats.self_y = ReadF32Le(payload.data() + 8 + 8);
            stats.have_self = true;
        }
    } catch (const std::exception&) {
        // EOF / reset on teardown ends the reader.
    }
}

// Windows' default sleep granularity is too coarse for 144 Hz: sleep until
// ~2 ms before the deadline, then yield-spin. Keeps inputs UNBATCHED (the
// real client sends once per rendered frame), which is what exposes the
// scheduler behavior; a batching poster would under-report it.
void PreciseSleepUntil(Clock::time_point target)
{
    for (;;) {
        const auto now = Clock::now();
        if (now >= target) {
            return;
        }
        if (target - now > std::chrono::milliseconds(2)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            std::this_thread::yield();
        }
    }
}

bool WaitFor(std::chrono::milliseconds timeout, const std::function<bool()>& condition)
{
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

std::vector<std::string> SplitRates(const std::string& text)
{
    std::vector<std::string> out;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) {
            out.push_back(item);
        }
    }
    return out;
}

struct TickRateResult {
    bool ran = false;
    double input_rate = 0.0;
    double zone_ticks_per_s = 0.0;
    double sim_over_wall = 0.0;
    double move_over_wall = 0.0;
    double frames_per_s = 0.0;
    double commands_per_s = 0.0;
    double drained_per_tick = 0.0;
    std::size_t max_drained = 0;
    double scheduler_enqueued_per_s = 0.0;
    double wire_ticks_per_s = 0.0;
    bool v2 = false;
};

TickRateResult RunOneRate(const std::string& rate_text, int measure_seconds, std::uint64_t index)
{
    TickRateResult result;
    const bool flood = rate_text == "flood";
    const double rate_hz = flood ? 0.0 : std::stod(rate_text);

    asio::io_context io;
    auto work = asio::make_work_guard(io);
    std::thread io_thread([&io] { io.run(); });

    // Real loopback session: server side wrapped in the production Session,
    // client side read by a blocking reader thread.
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    asio::io_context client_io;
    tcp::socket client(client_io);
    client.connect(tcp::endpoint(asio::ip::address_v4::loopback(),
                                 acceptor.local_endpoint().port()));
    client.set_option(tcp::no_delay(true));
    tcp::socket server_socket(io);
    acceptor.accept(server_socket);
    const gs::common::SessionId session_id = 700 + index;
    auto session = std::make_shared<gs::network::Session>(std::move(server_socket), session_id);

    WireStats wire;
    std::thread reader([&client, &wire] { ReadFrames(client, wire); });

    {
        // 10 km flat synthetic world, one zone: the player can run in a
        // straight line for the whole window without crossing a border.
        gs::game::WorldRuntime sim(io, {},
                                   gs::game::WorldRuntime::SyntheticWorldConfig{
                                       10000.0f, 1, 1, {}});
        sim.PostSpawn(session, MakeCharacter(index), gs::game::DebugSpawnOverride{2000.0f, 5000.0f});
        sim.Start();

        const bool spawned = WaitFor(std::chrono::seconds(10), [&] {
            return TakeWire(wire).have_self;
        });
        if (spawned) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // settle

            std::atomic<bool> inputs_run{true};
            std::atomic<std::uint64_t> inputs_posted{0};
            std::thread poster([&] {
                std::uint32_t seq = 0;
                if (flood) {
                    while (inputs_run.load(std::memory_order_relaxed)) {
                        for (int burst = 0; burst < 64; ++burst) {
                            sim.PostMoveInput(session_id, ++seq, kHalfPi,
                                              gs::game::MoveState::Running);
                        }
                        inputs_posted.fetch_add(64, std::memory_order_relaxed);
                        std::this_thread::yield();
                    }
                    return;
                }
                const auto period = std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(1.0 / rate_hz));
                auto next = Clock::now();
                while (inputs_run.load(std::memory_order_relaxed)) {
                    sim.PostMoveInput(session_id, ++seq, kHalfPi, gs::game::MoveState::Running);
                    inputs_posted.fetch_add(1, std::memory_order_relaxed);
                    next += period;
                    PreciseSleepUntil(next);
                }
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(500)); // warmup
            // Single-zone world (no partition control): the command counters of
            // its one zone, read through a supervisor snapshot.
            struct CommandTotals {
                std::uint64_t pushed = 0;
                std::uint64_t drained = 0;
                std::size_t max_drained = 0;
            };
            const auto command_totals = [&sim] {
                return ReadWorld(sim, [](const WorldSnapshot& snap) {
                    const auto& commands = snap.zones.GetZone(0).Commands();
                    return CommandTotals{commands.PushedTotal(), commands.DrainedTotal(),
                                         commands.MaxDrainedPerTake()};
                });
            };
            const auto t0 = Clock::now();
            const WireSnapshot w0 = TakeWire(wire);
            const std::uint64_t in0 = inputs_posted.load();
            const CommandTotals c0 = command_totals();
            const auto sched0 = sim.SchedulerStats();

            std::this_thread::sleep_for(std::chrono::seconds(measure_seconds));

            const auto t1 = Clock::now();
            const WireSnapshot w1 = TakeWire(wire);
            const std::uint64_t in1 = inputs_posted.load();
            const CommandTotals c1 = command_totals();
            const auto sched1 = sim.SchedulerStats();
            inputs_run = false;
            poster.join();

            const double wall = std::chrono::duration<double>(t1 - t0).count();
            // Zone ticks from the scheduler (one zone: every dispatch is one of
            // its ticks). The frame header carries the GLOBAL world tick (H7
            // wire contract), reported separately as the wire tick rate.
            const double ticks = static_cast<double>(sched1.enqueued - sched0.enqueued);
            result.wire_ticks_per_s =
                static_cast<double>(w1.last_zone_tick - w0.last_zone_tick) / wall;
            const double dx = static_cast<double>(w1.self_x - w0.self_x);
            const double dy = static_cast<double>(w1.self_y - w0.self_y);
            const double moved = std::sqrt(dx * dx + dy * dy);
            result.ran = true;
            result.input_rate = static_cast<double>(in1 - in0) / wall;
            result.zone_ticks_per_s = ticks / wall;
            result.sim_over_wall = ticks * gs::game::kTickDtSeconds / wall;
            result.move_over_wall = moved / (gs::game::kPlayerRunSpeed * wall);
            result.frames_per_s =
                static_cast<double>(w1.transform_frames - w0.transform_frames) / wall;
            result.commands_per_s = static_cast<double>(c1.pushed - c0.pushed) / wall;
            result.drained_per_tick =
                ticks > 0.0 ? static_cast<double>(c1.drained - c0.drained) / ticks : 0.0;
            result.max_drained = c1.max_drained;
            result.scheduler_enqueued_per_s =
                static_cast<double>(sched1.enqueued - sched0.enqueued) / wall;
            {
                std::lock_guard lock(wire.mutex);
                result.v2 = wire.v2_frames > 0;
            }
        }
        sim.Stop();
    }

    // Close the server side on its own executor (Session is not safe to
    // stop from a foreign thread); the reader then sees EOF.
    asio::post(io, [session] { session->Stop(); });
    reader.join();
    boost::system::error_code ignored;
    client.close(ignored);
    work.reset();
    io.stop();
    io_thread.join();
    return result;
}

// Background io for sessions whose sockets are never connected (sends fail
// fast and the Session stops itself; the scenario only needs routing).
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

std::shared_ptr<gs::network::Session> MakeDetachedSession(asio::io_context& io,
                                                          gs::common::SessionId id)
{
    tcp::socket socket(io);
    return std::make_shared<gs::network::Session>(std::move(socket), id);
}

// (A) One trial: fresh 2 km single-zone world, one resident player streaming
// 60 Hz Idle input, one forced split. Returns true when the split committed.
bool ForcedSplitUnderInputTrial(std::uint64_t index)
{
    IoRunner runner;
    gs::game::WorldRuntime sim(runner.io, {},
                               gs::game::WorldRuntime::SyntheticWorldConfig{2000.0f, 1, 1, {}});
    const gs::common::SessionId session_id = 5000 + index;
    sim.PostSpawn(MakeDetachedSession(runner.io, session_id), MakeCharacter(index),
                  gs::game::DebugSpawnOverride{500.0f, 500.0f});
    sim.Start();
    bool committed = false;
    if (WaitFor(std::chrono::seconds(10), [&] { return OwnerCount(sim) == 1; })) {
        std::atomic<bool> run{true};
        std::thread poster([&] {
            std::uint32_t seq = 0;
            const auto period = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1.0 / 60.0));
            auto next = Clock::now();
            while (run.load(std::memory_order_relaxed)) {
                sim.PostMoveInput(session_id, ++seq, 0.0f, gs::game::MoveState::Idle);
                next += period;
                PreciseSleepUntil(next);
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto before = sim.PartitionMetricsSnapshot().split_commits;
        sim.PostForceSplit(1);
        committed = WaitFor(std::chrono::milliseconds(1500), [&] {
            return sim.PartitionMetricsSnapshot().split_commits > before;
        });
        run = false;
        poster.join();
    }
    sim.Stop();
    return committed;
}

} // namespace

int RunInputPathScenario()
{
    int failures = 0;

    // ---- (A) forced split under continuous input ---------------------------
    constexpr int kSplitTrials = 10;
    int split_committed = 0;
    for (int trial = 0; trial < kSplitTrials; ++trial) {
        if (ForcedSplitUnderInputTrial(static_cast<std::uint64_t>(trial))) {
            ++split_committed;
        }
    }
    const bool split_pass = split_committed == kSplitTrials;
    std::printf("INPUTPATH forced-split-under-60hz-input: committed=%d/%d: %s\n",
                split_committed,
                kSplitTrials,
                split_pass ? "PASS" : "FAIL");
    if (!split_pass) {
        ++failures;
    }

    // ---- (B) input loss across migrations under load -----------------------
    {
        IoRunner runner;
        // Two 1000 m x 2000 m zones side by side; the border is x = 1000.
        gs::game::WorldRuntime sim(runner.io, {},
                                   gs::game::WorldRuntime::SyntheticWorldConfig{2000.0f, 2, 1, {}});
        // Resident mob load keeps zone ticks non-trivial, so a migration
        // often waits for a quiescent window while inputs keep flowing.
        for (const float cx : {500.0f, 1500.0f}) {
            gs::game::MobSpawnPoint point;
            point.mob_type_id = 2;
            point.x = cx;
            point.y = 1000.0f;
            point.count = 3000;
            point.radius = 400.0f;
            sim.AddMobSpawnPoint(point);
        }
        sim.SpawnConfiguredMobsNow();
        constexpr int kPlayers = 40;
        std::vector<gs::common::SessionId> sessions;
        for (int i = 0; i < kPlayers; ++i) {
            const gs::common::SessionId id = 6000 + static_cast<gs::common::SessionId>(i);
            sessions.push_back(id);
            // x in [980, 990): 5 s of running east reaches ~1010-1020 (past
            // the 5 m hysteresis), 5 s west returns below 995.
            sim.PostSpawn(MakeDetachedSession(runner.io, id),
                          MakeCharacter(100 + static_cast<std::uint64_t>(i)),
                          gs::game::DebugSpawnOverride{980.0f + static_cast<float>(i % 10),
                                                       100.0f + static_cast<float>(i) * 45.0f});
        }
        sim.Start();
        if (!WaitFor(std::chrono::seconds(20),
                     [&] { return OwnerCount(sim) == static_cast<std::size_t>(kPlayers); })) {
            std::printf("INPUTPATH migration-input-loss: FAIL (players never spawned)\n");
            ++failures;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            const auto in0 = sim.InputStats();
            const auto mig0 = sim.MigrationMetrics();
            std::atomic<bool> run{true};
            std::thread poster([&] {
                std::vector<std::uint32_t> seq(sessions.size(), 0);
                const auto start = Clock::now();
                const auto period = std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(1.0 / 60.0));
                auto next = start;
                while (run.load(std::memory_order_relaxed)) {
                    const double t = std::chrono::duration<double>(Clock::now() - start).count();
                    const bool east = (static_cast<int>(t / 5.0) % 2) == 0;
                    const float angle = east ? kHalfPi : -kHalfPi;
                    for (std::size_t i = 0; i < sessions.size(); ++i) {
                        sim.PostMoveInput(sessions[i], ++seq[i], angle,
                                          gs::game::MoveState::Running);
                    }
                    next += period;
                    PreciseSleepUntil(next);
                }
            });
            std::this_thread::sleep_for(std::chrono::seconds(30));
            run = false;
            poster.join();
            std::this_thread::sleep_for(std::chrono::milliseconds(300)); // drain
            const auto in1 = sim.InputStats();
            const auto mig1 = sim.MigrationMetrics();
            const auto lost = in1.moves_dropped_not_resident - in0.moves_dropped_not_resident;
            const auto migrations = mig1.committed - mig0.committed;
            const bool loss_pass = migrations > 0 && lost == 0;
            std::printf("INPUTPATH migration-input-loss: migrations=%llu posted=%llu routed=%llu "
                        "applied=%llu lost_not_resident=%llu no_owner=%llu stale_seq=%llu: %s\n",
                        static_cast<unsigned long long>(migrations),
                        static_cast<unsigned long long>(in1.moves_posted - in0.moves_posted),
                        static_cast<unsigned long long>(in1.moves_routed - in0.moves_routed),
                        static_cast<unsigned long long>(in1.moves_applied - in0.moves_applied),
                        static_cast<unsigned long long>(lost),
                        static_cast<unsigned long long>(in1.moves_dropped_no_owner -
                                                        in0.moves_dropped_no_owner),
                        static_cast<unsigned long long>(in1.moves_dropped_stale_sequence -
                                                        in0.moves_dropped_stale_sequence),
                        loss_pass ? "PASS" : "FAIL");
            if (!loss_pass) {
                ++failures;
            }
        }
        sim.Stop();
    }

    std::printf("INPUTPATH-DONE failures=%d\n", failures);
    return failures;
}

int RunTickRateScenario(const TickRateConfig& config)
{
    int failures = 0;
    std::uint64_t index = 0;
    std::printf("TICKRATE authoritative target: %.0f Hz (dt=%.0f ms), run speed %.1f m/s\n",
                1.0 / gs::game::kTickDtSeconds,
                gs::game::kTickDtSeconds * 1000.0f,
                gs::game::kPlayerRunSpeed);
    for (const auto& rate : SplitRates(config.rates)) {
        const TickRateResult r = RunOneRate(rate, config.measure_seconds, ++index);
        if (!r.ran) {
            std::printf("TICKRATE input=%s: FAIL (player never spawned / no frames)\n",
                        rate.c_str());
            ++failures;
            continue;
        }
        // Invariant: the authoritative simulation clock is 20 Hz no matter
        // how fast inputs arrive. Tolerances cover scheduler jitter only.
        const bool ticks_ok = r.zone_ticks_per_s >= 19.0 && r.zone_ticks_per_s <= 21.0;
        const bool sim_ok = std::abs(r.sim_over_wall - 1.0) <= 0.05;
        const bool move_ok = std::abs(r.move_over_wall - 1.0) <= 0.10;
        const bool frames_ok = r.frames_per_s >= 19.0 && r.frames_per_s <= 21.0;
        const bool wire_ok = r.wire_ticks_per_s >= 19.0 && r.wire_ticks_per_s <= 21.0;
        const bool pass = ticks_ok && sim_ok && move_ok && frames_ok && wire_ok;
        std::printf("TICKRATE input=%-5s inputs/s=%9.1f zone_ticks/s=%6.1f sim/wall=%5.2f "
                    "move/wall=%5.2f frames/s=%6.1f wire_ticks/s=%5.1f commands/s=%9.1f "
                    "drained/tick=%8.1f max_drained=%zu wire=%s: %s\n",
                    rate.c_str(),
                    r.input_rate,
                    r.zone_ticks_per_s,
                    r.sim_over_wall,
                    r.move_over_wall,
                    r.frames_per_s,
                    r.wire_ticks_per_s,
                    r.commands_per_s,
                    r.drained_per_tick,
                    r.max_drained,
                    r.v2 ? "v2" : "v1",
                    pass ? "PASS" : "FAIL");
        if (!pass) {
            ++failures;
        }
    }
    std::printf("TICKRATE-DONE failures=%d\n", failures);
    return failures;
}

// ============================================================================
// Network harness (H2/H3)
// ============================================================================
namespace {

using namespace std::chrono_literals;

// Multi-threaded io for the server side (production default is 2; 4 makes
// cross-thread handler interleavings far more likely).
class IoPool {
public:
    explicit IoPool(int threads)
    {
        for (int i = 0; i < threads; ++i) {
            threads_.emplace_back([this] { io.run(); });
        }
    }
    ~IoPool() { StopAndJoin(); }
    void StopAndJoin()
    {
        work_.reset();
        io.stop();
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        threads_.clear();
    }

    asio::io_context io;

private:
    asio::executor_work_guard<asio::io_context::executor_type> work_{asio::make_work_guard(io)};
    std::vector<std::thread> threads_;
};

// Runs fn on an io thread and waits for it (server objects are driven from
// their own executor, never from the test thread).
void RunOnIo(asio::io_context& io, const std::function<void()>& fn)
{
    std::promise<void> done;
    auto future = done.get_future();
    asio::post(io, [&] {
        fn();
        done.set_value();
    });
    future.wait();
}

// Blocking loopback client with portable timeouts (async op + run_for on a
// private io_context).
class TestClient {
public:
    enum class Read { Frame, Timeout, Closed };

    bool Connect(std::uint16_t port)
    {
        boost::system::error_code ec;
        socket_.connect(tcp::endpoint(asio::ip::address_v4::loopback(), port), ec);
        if (!ec) {
            socket_.set_option(tcp::no_delay(true), ec);
        }
        return !ec;
    }

    bool SendFrame(const std::vector<std::uint8_t>& payload)
    {
        std::vector<std::uint8_t> frame;
        frame.reserve(4 + payload.size());
        const auto length = static_cast<std::uint32_t>(payload.size());
        frame.push_back(static_cast<std::uint8_t>((length >> 24) & 0xff));
        frame.push_back(static_cast<std::uint8_t>((length >> 16) & 0xff));
        frame.push_back(static_cast<std::uint8_t>((length >> 8) & 0xff));
        frame.push_back(static_cast<std::uint8_t>(length & 0xff));
        frame.insert(frame.end(), payload.begin(), payload.end());
        boost::system::error_code ec;
        asio::write(socket_, asio::buffer(frame), ec);
        return !ec;
    }

    // Raw bytes, no framing (e.g. a lying length header).
    bool SendRaw(const std::vector<std::uint8_t>& bytes)
    {
        boost::system::error_code ec;
        asio::write(socket_, asio::buffer(bytes), ec);
        return !ec;
    }

    Read ReadFrame(std::vector<std::uint8_t>& out, std::chrono::milliseconds timeout)
    {
        std::array<std::uint8_t, 4> header{};
        const Read head = ReadExact(header.data(), header.size(), timeout);
        if (head != Read::Frame) {
            return head;
        }
        const std::uint32_t length = (static_cast<std::uint32_t>(header[0]) << 24) |
                                     (static_cast<std::uint32_t>(header[1]) << 16) |
                                     (static_cast<std::uint32_t>(header[2]) << 8) |
                                     static_cast<std::uint32_t>(header[3]);
        out.resize(length);
        if (length == 0) {
            return Read::Frame;
        }
        return ReadExact(out.data(), out.size(), timeout);
    }

    // True when the server closed the connection within the timeout
    // (intermediate frames are skipped).
    bool WaitClosed(std::chrono::milliseconds timeout)
    {
        const auto deadline = Clock::now() + timeout;
        std::vector<std::uint8_t> frame;
        while (Clock::now() < deadline) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            const Read result = ReadFrame(frame, std::max(left, 1ms));
            if (result == Read::Closed) {
                return true;
            }
            if (result == Read::Timeout) {
                return false;
            }
        }
        return false;
    }

    void CloseGraceful()
    {
        boost::system::error_code ec;
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

    // SO_LINGER 0 -> the close sends a RST instead of a FIN.
    void CloseRst()
    {
        boost::system::error_code ec;
        socket_.set_option(asio::socket_base::linger(true, 0), ec);
        socket_.close(ec);
    }

private:
    Read ReadExact(void* data, std::size_t size, std::chrono::milliseconds timeout)
    {
        boost::system::error_code result = asio::error::would_block;
        asio::async_read(socket_, asio::buffer(data, size),
                         [&result](const boost::system::error_code& ec, std::size_t) {
                             result = ec;
                         });
        ctx_.restart();
        ctx_.run_for(timeout);
        if (result == asio::error::would_block) {
            boost::system::error_code ignored;
            socket_.cancel(ignored);
            ctx_.restart();
            ctx_.run();
            return Read::Timeout;
        }
        return result ? Read::Closed : Read::Frame;
    }

    asio::io_context ctx_;
    tcp::socket socket_{ctx_};
};

std::vector<std::uint8_t> HandshakePayload()
{
    capnp::MallocMessageBuilder msg;
    auto request = msg.initRoot<gs::protocol::Packet>().initHandshakeRequest();
    request.setProtocolVersion(gs::protocol::kProtocolVersion);
    request.setClientBuild("hardening");
    return gs::protocol::SerializeToBytes(msg);
}

std::vector<std::uint8_t> HandshakePayloadVersion(std::uint32_t version)
{
    capnp::MallocMessageBuilder msg;
    auto request = msg.initRoot<gs::protocol::Packet>().initHandshakeRequest();
    request.setProtocolVersion(version);
    request.setClientBuild("negotiation");
    return gs::protocol::SerializeToBytes(msg);
}

std::vector<std::uint8_t> EnterWorldPayload()
{
    capnp::MallocMessageBuilder msg;
    auto enter = msg.initRoot<gs::protocol::Packet>().initEnterWorld();
    std::array<kj::byte, 32> token{};
    for (std::size_t i = 0; i < token.size(); ++i) {
        token[i] = static_cast<kj::byte>(i * 7 + 3);
    }
    enter.setToken(kj::ArrayPtr<const kj::byte>(token.data(), token.size()));
    return gs::protocol::SerializeToBytes(msg);
}

// Cap'n Proto codec byte + a body that is not a whole number of words
// (ParsePacket rejects it) ...
std::vector<std::uint8_t> MalformedSizePayload()
{
    return {0x00, 1, 2, 3, 4, 5, 6, 7};
}

// ... and a word-aligned body whose segment table is garbage (the reader
// throws kj::Exception).
std::vector<std::uint8_t> MalformedCapnpPayload()
{
    std::vector<std::uint8_t> payload(1 + 16, 0xff);
    payload[0] = 0x00;
    return payload;
}

std::vector<std::uint8_t> BinaryMovePayload(std::uint32_t sequence)
{
    return {0x01,
            0x01,
            static_cast<std::uint8_t>(sequence & 0xff),
            static_cast<std::uint8_t>((sequence >> 8) & 0xff),
            static_cast<std::uint8_t>((sequence >> 16) & 0xff),
            static_cast<std::uint8_t>((sequence >> 24) & 0xff),
            0x00,
            0x40,
            0x02};
}

bool IsHandshakeResponse(const std::vector<std::uint8_t>& payload)
{
    try {
        auto parsed = gs::protocol::ParsePacket(payload);
        return parsed && parsed->packet.isHandshakeResponse();
    } catch (const kj::Exception&) {
        return false;
    }
}

struct NetCheck {
    int failures = 0;
    void operator()(const char* name, bool pass, const std::string& detail = {})
    {
        std::printf("NETSTRESS %s%s%s: %s\n",
                    name,
                    detail.empty() ? "" : " ",
                    detail.c_str(),
                    pass ? "PASS" : "FAIL");
        std::fflush(stdout); // a crash in a later case must not eat this line
        if (!pass) {
            ++failures;
        }
    }
};

void Progress(const char* stage)
{
    std::printf("NETSTRESS stage: %s\n", stage);
    std::fflush(stdout);
}

std::string Fmt(const char* format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

} // namespace

int RunNetStressScenario(const NetStressConfig& config)
{
    NetCheck check;
    IoPool pool(std::max(1, config.io_threads));
    std::printf("NETSTRESS config: io_threads=%d chaos=%d cases=%s\n",
                std::max(1, config.io_threads),
                config.chaos ? 1 : 0,
                config.cases.c_str());
    if (config.cases == "all") {
        gs::game::WorldRuntime sim(pool.io, {},
                                   gs::game::WorldRuntime::SyntheticWorldConfig{2000.0f, 1, 1, {}});
        sim.Start();
        // Never started: EnterWorld stays parked in EnteringWorld, no DB.
        gs::db::DbPool db(pool.io, gs::db::DbConfig{});
        gs::db::CharacterRepository characters(db);
        gs::db::HandoffTokenRepository tokens(db);
        gs::game::GameConnectionHandler handler(tokens, characters, sim, "127.0.0.1:0");
        gs::network::Server game_server(
            pool.io,
            tcp::endpoint(asio::ip::address_v4::loopback(), 0),
            [&handler](auto session, auto payload) { handler.OnPayload(session, std::move(payload)); },
            [&handler](auto session) { handler.OnDisconnect(session); });
        // Short H3 timeouts so the timeout cases run in seconds; every other
        // case completes its exchanges well within them.
        gs::network::SessionLimits game_limits;
        game_limits.setup_timeout = 500ms;
        game_limits.idle_timeout = 2000ms;
        game_server.SetSessionLimits(game_limits);
        game_server.Start();
        const std::uint16_t port = game_server.LocalPort();

        auto contexts_drained = [&](std::size_t baseline, std::chrono::milliseconds timeout) {
            return WaitFor(timeout, [&] { return handler.ContextCount() <= baseline; });
        };

        Progress("negotiation");
        // ---- 3D-5C1: protocol version negotiation ------------------------
        {
            bool all_ok = true;
            std::string detail;
            for (const std::uint32_t version : {0u, 1u, 2u, 3u}) {
                const bool supported = gs::protocol::IsSupportedProtocolVersion(version);
                TestClient client;
                std::vector<std::uint8_t> frame;
                bool ok = client.Connect(port) && client.SendFrame(HandshakePayloadVersion(version)) &&
                          client.ReadFrame(frame, 2000ms) == TestClient::Read::Frame;
                std::uint32_t answered = 0;
                bool accepted = false;
                if (ok) {
                    auto parsed = gs::protocol::ParsePacket(frame);
                    ok = parsed && parsed->packet.isHandshakeResponse();
                    if (ok) {
                        const auto response = parsed->packet.getHandshakeResponse();
                        accepted = response.getResult() == gs::protocol::HandshakeResult::OK;
                        answered = response.getServerProtocolVersion();
                    }
                }
                const bool closed = !supported && client.WaitClosed(2000ms);
                const bool expected = ok && accepted == supported &&
                                      (supported ? answered == version : answered == gs::protocol::kProtocolVersion && closed);
                all_ok = all_ok && expected;
                detail += Fmt(" v%u:%s->%u", version, accepted ? "ok" : "refused", answered);
                client.CloseGraceful();
            }
            check("protocol-negotiation-accepts-1-and-2-refuses-others", all_ok, detail);
        }
        Progress("malformed");
        // ---- H2.1: malformed packets in every pre-world state -------------
        {
            const std::size_t contexts0 = handler.ContextCount();
            const auto cleanups0 = handler.DisconnectCleanups();
            int closed = 0;
            constexpr int kRuns = 40;
            for (int i = 0; i < kRuns; ++i) {
                TestClient client;
                if (!client.Connect(port)) {
                    continue;
                }
                client.SendFrame(i % 2 == 0 ? MalformedSizePayload() : MalformedCapnpPayload());
                closed += client.WaitClosed(2000ms) ? 1 : 0;
            }
            const bool drained = contexts_drained(contexts0, 1000ms);
            const auto cleanups = handler.DisconnectCleanups() - cleanups0;
            check("malformed-pre-handshake", closed == kRuns && drained && cleanups == kRuns,
                  Fmt("closed=%d/%d contexts_left=%zu cleanups=%llu", closed, kRuns,
                      handler.ContextCount(), static_cast<unsigned long long>(cleanups)));
        }
        {
            const std::size_t contexts0 = handler.ContextCount();
            const auto cleanups0 = handler.DisconnectCleanups();
            int closed = 0;
            int handshakes = 0;
            constexpr int kRuns = 40;
            for (int i = 0; i < kRuns; ++i) {
                TestClient client;
                if (!client.Connect(port) || !client.SendFrame(HandshakePayload())) {
                    continue;
                }
                std::vector<std::uint8_t> frame;
                if (client.ReadFrame(frame, 2000ms) == TestClient::Read::Frame &&
                    IsHandshakeResponse(frame)) {
                    ++handshakes;
                }
                // Alternate: malformed payload vs wrong-state second handshake.
                client.SendFrame(i % 2 == 0 ? MalformedCapnpPayload() : HandshakePayload());
                closed += client.WaitClosed(2000ms) ? 1 : 0;
            }
            const bool drained = contexts_drained(contexts0, 1000ms);
            const auto cleanups = handler.DisconnectCleanups() - cleanups0;
            check("malformed-or-wrong-state-after-handshake",
                  handshakes == kRuns && closed == kRuns && drained && cleanups == kRuns,
                  Fmt("handshakes=%d closed=%d/%d contexts_left=%zu cleanups=%llu", handshakes,
                      closed, kRuns, handler.ContextCount(),
                      static_cast<unsigned long long>(cleanups)));
        }
        Progress("entering-world");
        // Disconnect while EnteringWorld: server-initiated (packet during
        // enter world) and client-initiated (socket closed mid-lookup).
        {
            const std::size_t contexts0 = handler.ContextCount();
            const auto cleanups0 = handler.DisconnectCleanups();
            int server_closed = 0;
            constexpr int kRuns = 20;
            for (int i = 0; i < kRuns; ++i) {
                TestClient client;
                if (!client.Connect(port) || !client.SendFrame(HandshakePayload())) {
                    continue;
                }
                std::vector<std::uint8_t> frame;
                client.ReadFrame(frame, 2000ms);
                client.SendFrame(EnterWorldPayload());
                if (i % 2 == 0) {
                    client.SendFrame(HandshakePayload()); // packet during enter world
                    server_closed += client.WaitClosed(2000ms) ? 1 : 0;
                } else {
                    std::this_thread::sleep_for(20ms);
                    client.CloseRst();
                    ++server_closed;
                }
            }
            const bool drained = contexts_drained(contexts0, 1500ms);
            const auto cleanups = handler.DisconnectCleanups() - cleanups0;
            std::this_thread::sleep_for(200ms);
            check("disconnect-during-entering-world",
                  server_closed == kRuns && drained && cleanups == kRuns &&
                      OwnerCount(sim) == 0,
                  Fmt("closed=%d/%d contexts_left=%zu cleanups=%llu world_presence=%zu",
                      server_closed, kRuns, handler.ContextCount(),
                      static_cast<unsigned long long>(cleanups), OwnerCount(sim)));
        }
        Progress("input-gate");
        // ---- H3 input state gate (observability now; enforced in H3) ------
        {
            const auto posted0 = sim.InputStats().moves_posted;
            TestClient client;
            bool closed = false;
            if (client.Connect(port)) {
                client.SendFrame(BinaryMovePayload(1));
                client.SendFrame(BinaryMovePayload(2));
                closed = client.WaitClosed(1000ms);
            }
            const auto posted = sim.InputStats().moves_posted - posted0;
            check("input-before-world-rejected", closed && posted == 0,
                  Fmt("closed=%d moves_reaching_world=%llu", closed ? 1 : 0,
                      static_cast<unsigned long long>(posted)));
        }
        {
            // EnteringWorld: counted and dropped, never reaches the world,
            // does not punish the connection.
            const auto posted0 = sim.InputStats().moves_posted;
            const auto dropped0 = handler.Stats().moves_dropped_entering;
            TestClient client;
            std::vector<std::uint8_t> frame;
            bool open_after = false;
            if (client.Connect(port) && client.SendFrame(HandshakePayload()) &&
                client.ReadFrame(frame, 2000ms) == TestClient::Read::Frame &&
                client.SendFrame(EnterWorldPayload())) {
                client.SendFrame(BinaryMovePayload(1));
                open_after = client.ReadFrame(frame, 300ms) == TestClient::Read::Timeout;
            }
            client.CloseGraceful();
            const auto posted = sim.InputStats().moves_posted - posted0;
            const auto dropped = handler.Stats().moves_dropped_entering - dropped0;
            check("input-while-entering-dropped", open_after && posted == 0 && dropped == 1,
                  Fmt("still_open=%d moves_reaching_world=%llu dropped=%llu", open_after ? 1 : 0,
                      static_cast<unsigned long long>(posted),
                      static_cast<unsigned long long>(dropped)));
        }

        Progress("timeouts");
        // ---- H3: handshake (setup) + idle timeouts ------------------------
        {
            const auto& counters = gs::network::GlobalSessionCounters();
            const auto setup0 = counters.setup_timeouts.load();
            TestClient silent;
            const auto t0 = Clock::now();
            const bool closed = silent.Connect(port) && silent.WaitClosed(3000ms);
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
            const auto setups = counters.setup_timeouts.load() - setup0;
            check("handshake-timeout", closed && setups == 1 && ms >= 400 && ms <= 1500,
                  Fmt("closed=%d after_ms=%lld setup_timeouts=%llu", closed ? 1 : 0,
                      static_cast<long long>(ms), static_cast<unsigned long long>(setups)));
        }
        {
            const std::size_t contexts0 = handler.ContextCount();
            const auto& counters = gs::network::GlobalSessionCounters();
            const auto idle0 = counters.idle_timeouts.load();
            TestClient client;
            std::vector<std::uint8_t> frame;
            bool handshake = false;
            bool open_early = false;
            bool closed = false;
            if (client.Connect(port) && client.SendFrame(HandshakePayload()) &&
                client.ReadFrame(frame, 2000ms) == TestClient::Read::Frame &&
                IsHandshakeResponse(frame)) {
                handshake = true; // established: the setup deadline no longer applies
                open_early = client.ReadFrame(frame, 1000ms) == TestClient::Read::Timeout;
                closed = client.WaitClosed(3000ms);
            }
            const bool drained = contexts_drained(contexts0, 1000ms);
            const auto idles = counters.idle_timeouts.load() - idle0;
            check("idle-timeout",
                  handshake && open_early && closed && idles == 1 && drained,
                  Fmt("handshake=%d open_at_1s=%d closed=%d idle_timeouts=%llu contexts_left=%zu",
                      handshake ? 1 : 0, open_early ? 1 : 0, closed ? 1 : 0,
                      static_cast<unsigned long long>(idles), handler.ContextCount()));
        }

        Progress("storm");
        // ---- H2.3: connect / RST storm, accept loop must survive ----------
        {
            const std::size_t contexts0 = handler.ContextCount();
            constexpr int kThreads = 4;
            constexpr int kPerThread = 250;
            std::atomic<int> connected{0};
            std::vector<std::thread> stormers;
            for (int t = 0; t < kThreads; ++t) {
                stormers.emplace_back([&, t] {
                    for (int i = 0; i < kPerThread; ++i) {
                        TestClient client;
                        if (!client.Connect(port)) {
                            continue;
                        }
                        connected.fetch_add(1);
                        switch ((i + t) % 3) {
                        case 0:
                            client.CloseRst();
                            break;
                        case 1:
                            client.CloseGraceful();
                            break;
                        default:
                            client.SendFrame(HandshakePayload());
                            client.CloseRst();
                            break;
                        }
                    }
                });
            }
            for (auto& thread : stormers) {
                thread.join();
            }
            // Liveness probe: a fresh client must still complete a handshake.
            int probes_ok = 0;
            constexpr int kProbes = 5;
            for (int i = 0; i < kProbes; ++i) {
                TestClient probe;
                std::vector<std::uint8_t> frame;
                if (probe.Connect(port) && probe.SendFrame(HandshakePayload()) &&
                    probe.ReadFrame(frame, 3000ms) == TestClient::Read::Frame &&
                    IsHandshakeResponse(frame)) {
                    ++probes_ok;
                }
                probe.CloseGraceful();
            }
            const bool drained = contexts_drained(contexts0, 3000ms);
            check("connect-rst-storm-accept-alive", probes_ok == kProbes && drained,
                  Fmt("storm_connects=%d probes_ok=%d/%d contexts_left=%zu", connected.load(),
                      probes_ok, kProbes, handler.ContextCount()));
        }
        Progress("reconnect");
        // ---- rapid reconnect ----------------------------------------------
        {
            const std::size_t contexts0 = handler.ContextCount();
            int ok = 0;
            constexpr int kRuns = 200;
            for (int i = 0; i < kRuns; ++i) {
                TestClient client;
                std::vector<std::uint8_t> frame;
                if (client.Connect(port) && client.SendFrame(HandshakePayload()) &&
                    client.ReadFrame(frame, 2000ms) == TestClient::Read::Frame &&
                    IsHandshakeResponse(frame)) {
                    ++ok;
                }
                client.CloseGraceful();
            }
            const bool drained = contexts_drained(contexts0, 2000ms);
            check("rapid-reconnect", ok == kRuns && drained,
                  Fmt("handshakes=%d/%d contexts_left=%zu", ok, kRuns, handler.ContextCount()));
        }

        RunOnIo(pool.io, [&] { game_server.Stop(); });
        sim.Stop();
    }

    if (config.cases == "all") {
        Progress("rate-gate");
        // ---- H3: edge rate primitives (pure) --------------------------------
        {
            const auto t0 = Clock::now();
            gs::game::RateWindow window;
            std::uint32_t last = 0;
            for (int i = 0; i < 1000; ++i) {
                last = window.Note(t0 + std::chrono::microseconds(i * 900));
            }
            const bool within = last == 1000;
            const bool over = window.Note(t0 + 950ms) == 1001;          // same window
            const bool reset = window.Note(t0 + 1000ms + 1ms) == 1;     // next window
            gs::game::TokenBucket bucket;
            int burst_ok = 0;
            for (int i = 0; i < 25; ++i) {
                burst_ok += bucket.Take(t0, 20.0f, 20.0f) ? 1 : 0; // burst 20, same instant
            }
            const bool refill = bucket.Take(t0 + 100ms, 20.0f, 20.0f);  // +2 tokens
            const bool unlimited = gs::game::TokenBucket{}.Take(t0, 0.0f, 0.0f);
            check("rate-gate-unit", within && over && reset && burst_ok == 20 && refill && unlimited,
                  Fmt("window=%d/%d/%d bucket_burst=%d refill=%d unlimited=%d", within ? 1 : 0,
                      over ? 1 : 0, reset ? 1 : 0, burst_ok, refill ? 1 : 0, unlimited ? 1 : 0));
        }

        Progress("backpressure");
        // ---- H3: send-queue backpressure ------------------------------------
        // Same stream to a fast reader and to a client that never reads: the
        // slow one must be disconnected (bounded queue), the fast one must be
        // untouched and receive everything. Two policies, two paces:
        //   burst   -> the byte hard limit trips first;
        //   trickle -> ~100 KB/s never reaches the byte limit within the age
        //              window, so the oldest-frame age limit trips.
        auto run_backpressure = [&](const char* name, bool burst) {
            std::mutex mutex;
            std::array<std::shared_ptr<gs::network::Session>, 2> pair{};
            gs::network::Server bp(
                pool.io,
                tcp::endpoint(asio::ip::address_v4::loopback(), 0),
                [&](auto session, auto payload) {
                    if (payload.size() == 1 && payload[0] < 2) {
                        std::lock_guard lock(mutex);
                        pair[payload[0]] = session;
                    }
                },
                [](auto) {});
            gs::network::SessionLimits limits;
            limits.send_soft_bytes = 64 * 1024;
            limits.send_hard_bytes = 512 * 1024;
            limits.send_hard_age = 3000ms;
            bp.SetSessionLimits(limits);
            bp.Start();
            const auto& counters = gs::network::GlobalSessionCounters();
            const auto bytes0 = counters.slow_client_disconnects.load();
            const auto age0 = counters.stalled_queue_disconnects.load();
            const auto soft0 = counters.send_soft_pressure_events.load();

            TestClient fast;
            TestClient slow;
            fast.Connect(bp.LocalPort());
            fast.SendFrame({0});
            slow.Connect(bp.LocalPort());
            slow.SendFrame({1});
            WaitFor(3000ms, [&] {
                std::lock_guard lock(mutex);
                return pair[0] && pair[1];
            });
            std::atomic<std::uint64_t> fast_frames{0};
            std::thread fast_reader([&] {
                std::vector<std::uint8_t> frame;
                while (fast.ReadFrame(frame, 5000ms) == TestClient::Read::Frame) {
                    fast_frames.fetch_add(1);
                }
            });
            std::uint64_t sent = 0;
            const auto start = Clock::now();
            const std::vector<std::uint8_t> payload(2048, 0x5a);
            bool slow_stopped = false;
            if (pair[0] && pair[1]) {
                auto next = start;
                while (Clock::now() - start < 20s && !slow_stopped) {
                    const int count = burst ? 32 : 1;
                    for (int i = 0; i < count; ++i) {
                        pair[0]->SendPayload(payload);
                        pair[1]->SendPayload(payload);
                        ++sent;
                    }
                    slow_stopped = pair[1]->IsStopped();
                    next += burst ? 2ms : 20ms;
                    PreciseSleepUntil(next);
                }
            }
            const auto stop_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            const bool fast_complete = WaitFor(5000ms, [&] { return fast_frames.load() >= sent; });
            const bool fast_alive = pair[0] && !pair[0]->IsStopped();
            const std::size_t slow_peak = pair[1] ? pair[1]->MaxQueuedBytes() : 0;
            const std::size_t fast_peak = pair[0] ? pair[0]->MaxQueuedBytes() : 0;
            if (pair[0]) {
                asio::post(pool.io, [session = pair[0]] { session->Stop(); });
            }
            fast_reader.join();
            slow.CloseRst();
            const auto bytes_events = counters.slow_client_disconnects.load() - bytes0;
            const auto age_events = counters.stalled_queue_disconnects.load() - age0;
            const auto soft_events = counters.send_soft_pressure_events.load() - soft0;
            const bool right_policy =
                burst ? (bytes_events == 1 && age_events == 0) : (age_events == 1 && bytes_events == 0);
            check(name,
                  slow_stopped && right_policy && soft_events >= 1 &&
                      slow_peak <= limits.send_hard_bytes + 4096 && fast_alive && fast_complete,
                  Fmt("slow_closed_after_ms=%lld sent=%llu slow_peak_queue=%zuB fast_peak_queue=%zuB "
                      "fast_received=%llu fast_alive=%d by_bytes=%llu by_age=%llu soft_events=%llu",
                      static_cast<long long>(stop_ms), static_cast<unsigned long long>(sent),
                      slow_peak, fast_peak, static_cast<unsigned long long>(fast_frames.load()),
                      fast_alive ? 1 : 0, static_cast<unsigned long long>(bytes_events),
                      static_cast<unsigned long long>(age_events),
                      static_cast<unsigned long long>(soft_events)));
            RunOnIo(pool.io, [&] { bp.Stop(); });
        };
        run_backpressure("backpressure-byte-limit", true);
        run_backpressure("backpressure-age-limit", false);
    }

    Progress("concurrent-send");
    // ---- H2.2: concurrent Send / Stop from foreign threads -----------------
    // A raw Server (no game logic) whose sessions are fed by 8 producer
    // threads -- the zone-worker pattern -- while chaos threads Stop sessions
    // from outside and clients reset. Every untouched session must receive
    // every frame, in order per producer; every session must be notified
    // disconnected exactly once.
    {
        constexpr int kClients = 32;
        constexpr int kProducers = 8;
        constexpr std::uint32_t kPerProducer = 1000;
        const std::uint64_t expected = static_cast<std::uint64_t>(kProducers) * kPerProducer;

        // Session <-> client pairing comes from a hello payload (client
        // index), never from accept order.
        std::mutex sessions_mutex;
        std::vector<std::shared_ptr<gs::network::Session>> targets(kClients);
        std::atomic<int> hellos{0};
        std::atomic<int> disconnect_notifications{0};
        std::mutex notified_mutex;
        std::unordered_map<gs::common::SessionId, int> notified;
        gs::network::Server raw(
            pool.io,
            tcp::endpoint(asio::ip::address_v4::loopback(), 0),
            [&](auto session, auto payload) {
                if (payload.size() == 1 && payload[0] < kClients) {
                    std::lock_guard lock(sessions_mutex);
                    if (!targets[payload[0]]) {
                        targets[payload[0]] = session;
                        hellos.fetch_add(1);
                    }
                }
            },
            [&](auto session) {
                disconnect_notifications.fetch_add(1);
                std::lock_guard lock(notified_mutex);
                ++notified[session->Id()];
            });
        raw.Start();
        const std::uint16_t port = raw.LocalPort();

        // Chaos roles (fixed up front, read-only afterwards):
        //   0-3  server Stop() from a foreign thread at ~30%
        //   4-7  client resets its own socket (RST) at ~50% of its stream
        //   8-11 four threads call Stop() concurrently on the same sessions
        const bool chaos_on = config.chaos;
        auto chaotic = [chaos_on](int i) { return chaos_on && i < 12; };
        struct ClientResult {
            std::atomic<std::uint64_t> frames{0};
            std::atomic<std::uint64_t> order_violations{0};
        };
        std::vector<ClientResult> results(kClients);
        std::vector<std::unique_ptr<TestClient>> clients;
        for (int i = 0; i < kClients; ++i) {
            clients.push_back(std::make_unique<TestClient>());
            if (clients.back()->Connect(port)) {
                clients.back()->SendFrame({static_cast<std::uint8_t>(i)});
            }
        }
        WaitFor(3000ms, [&] { return hellos.load() == kClients; });
        {
            std::lock_guard lock(sessions_mutex);
            for (auto& target : targets) {
                if (!target) {
                    // Keep the pointer vector dense; a missing hello is
                    // reported by the stream check below.
                    tcp::socket dummy(pool.io);
                    target = std::make_shared<gs::network::Session>(std::move(dummy), 0);
                }
            }
        }

        std::vector<std::thread> readers;
        for (int i = 0; i < kClients; ++i) {
            readers.emplace_back([&, i] {
                std::array<std::uint32_t, kProducers> last{};
                std::vector<std::uint8_t> frame;
                for (;;) {
                    if (chaos_on && i >= 4 && i < 8 && results[i].frames.load() >= expected / 2) {
                        clients[i]->CloseRst(); // own socket, own thread
                        return;
                    }
                    const auto result = clients[i]->ReadFrame(frame, 15000ms);
                    if (result != TestClient::Read::Frame) {
                        return;
                    }
                    if (frame.size() < 5) {
                        results[i].order_violations.fetch_add(1);
                        continue;
                    }
                    const std::uint8_t producer = frame[0];
                    const std::uint32_t seq = ReadU32Le(frame.data() + 1);
                    if (producer >= kProducers || seq != last[producer] + 1) {
                        results[i].order_violations.fetch_add(1);
                    }
                    if (producer < kProducers) {
                        last[producer] = seq;
                    }
                    results[i].frames.fetch_add(1);
                }
            });
        }

        std::atomic<std::uint32_t> progress{0};
        std::vector<std::thread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&, p] {
                for (std::uint32_t seq = 1; seq <= kPerProducer; ++seq) {
                    const std::size_t size = 16 + ((seq * 131u + static_cast<std::uint32_t>(p) * 17u) % 1000u);
                    std::vector<std::uint8_t> payload(size, static_cast<std::uint8_t>(seq));
                    payload[0] = static_cast<std::uint8_t>(p);
                    payload[1] = static_cast<std::uint8_t>(seq & 0xff);
                    payload[2] = static_cast<std::uint8_t>((seq >> 8) & 0xff);
                    payload[3] = static_cast<std::uint8_t>((seq >> 16) & 0xff);
                    payload[4] = static_cast<std::uint8_t>((seq >> 24) & 0xff);
                    for (const auto& session : targets) {
                        session->SendPayload(payload);
                    }
                    if (p == 0) {
                        progress.store(seq);
                    }
                }
            });
        }
        std::thread chaos([&] {
            if (!chaos_on) {
                return;
            }
            WaitFor(20000ms, [&] { return progress.load() >= kPerProducer * 3 / 10; });
            for (int i = 0; i < 4; ++i) {
                targets[i]->Stop(); // foreign thread, streams in flight
            }
            WaitFor(20000ms, [&] { return progress.load() >= kPerProducer / 2; });
            std::vector<std::thread> stoppers;
            for (int s = 0; s < 4; ++s) {
                stoppers.emplace_back([&] {
                    for (int i = 8; i < 12; ++i) {
                        targets[i]->Stop();
                    }
                });
            }
            for (auto& stopper : stoppers) {
                stopper.join();
            }
        });
        for (auto& producer : producers) {
            producer.join();
        }
        chaos.join();
        // Let the untouched streams flush, then close everything from the
        // server side (on the io, as production would).
        const bool flushed = WaitFor(20000ms, [&] {
            for (int i = 0; i < kClients; ++i) {
                if (!chaotic(i) && results[i].frames.load() < expected) {
                    return false;
                }
            }
            return true;
        });
        for (const auto& session : targets) {
            asio::post(pool.io, [session] { session->Stop(); });
        }
        for (auto& reader : readers) {
            reader.join();
        }
        std::uint64_t violations = 0;
        int complete = 0;
        int untouched = 0;
        for (int i = 0; i < kClients; ++i) {
            violations += results[i].order_violations.load();
            if (!chaotic(i)) {
                ++untouched;
                complete += results[i].frames.load() == expected ? 1 : 0;
            }
        }
        WaitFor(2000ms, [&] { return disconnect_notifications.load() >= kClients; });
        int double_notified = 0;
        {
            std::lock_guard lock(notified_mutex);
            for (const auto& [id, count] : notified) {
                (void)id;
                double_notified += count > 1 ? 1 : 0;
            }
        }
        // Measured saturation queue depth (8 producers blasting one session):
        // the reference point for the send-queue hard limit.
        std::size_t peak_queue = 0;
        for (const auto& session : targets) {
            peak_queue = std::max(peak_queue, session->MaxQueuedBytes());
        }
        check("concurrent-send-stop",
              flushed && complete == untouched && violations == 0 &&
                  disconnect_notifications.load() == kClients && double_notified == 0,
              Fmt("complete_streams=%d/%d order_violations=%llu disconnects=%d/%d "
                  "double_notified=%d peak_session_queue=%zuB",
                  complete, untouched, static_cast<unsigned long long>(violations),
                  disconnect_notifications.load(), kClients, double_notified, peak_queue));
        RunOnIo(pool.io, [&] { raw.Stop(); });
    }

    pool.StopAndJoin();
    std::printf("NETSTRESS-DONE failures=%d\n", check.failures);
    return check.failures;
}

// ============================================================================
// World presence (H4)
// ============================================================================
namespace {

bool IsEnterWorldRejectAlreadyInWorld(const std::vector<std::uint8_t>& payload)
{
    try {
        auto parsed = gs::protocol::ParsePacket(payload);
        return parsed && parsed->packet.isEnterWorldReject() &&
               parsed->packet.getEnterWorldReject().getReason() ==
                   gs::protocol::S2cEnterWorldReject::RejectReason::ALREADY_IN_WORLD;
    } catch (const kj::Exception&) {
        return false;
    }
}

// Consistency audit in the supervisor's quiescent window (includes the H4
// presence checks). Returns "OK" or the failure text.
std::string AuditNow(gs::game::WorldRuntime& sim)
{
    sim.RequestValidation();
    std::string result;
    for (int i = 0; i < 200; ++i) {
        if (sim.TryTakeValidationResult(result)) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return "TIMEOUT";
}

// A server-side Session whose client end the test reads (to observe the
// EnterWorldReject + close a refused duplicate receives).
struct ObservedSession {
    std::shared_ptr<gs::network::Session> session;
    std::unique_ptr<TestClient> client;
};

ObservedSession MakeObservedSession(asio::io_context& io, gs::common::SessionId id)
{
    ObservedSession out;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    out.client = std::make_unique<TestClient>();
    out.client->Connect(acceptor.local_endpoint().port());
    tcp::socket server_socket(io);
    acceptor.accept(server_socket);
    out.session = std::make_shared<gs::network::Session>(std::move(server_socket), id);
    return out;
}

// True when the client receives EnterWorldReject::alreadyInWorld and then
// the server closes the connection.
bool ExpectRefused(ObservedSession& observed)
{
    std::vector<std::uint8_t> frame;
    for (int i = 0; i < 8; ++i) {
        const auto read = observed.client->ReadFrame(frame, 3000ms);
        if (read != TestClient::Read::Frame) {
            return false;
        }
        if (IsEnterWorldRejectAlreadyInWorld(frame)) {
            return observed.client->WaitClosed(3000ms);
        }
    }
    return false;
}

} // namespace

int RunPresenceScenario()
{
    NetCheck check;
    auto report = [&](const char* name, bool pass, const std::string& detail) {
        std::printf("PRESENCE %s %s: %s\n", name, detail.c_str(), pass ? "PASS" : "FAIL");
        std::fflush(stdout);
        if (!pass) {
            ++check.failures;
        }
    };
    const gs::common::SessionId kBase = 30000;

    // I1 local observation uses the production collector, without AuditNow
    // (which drains queued commands). Incomplete captures must stay incomplete.
    {
        using namespace gs::game;
        const auto filter = ParseI1ObservationFilter(
            std::to_string(gs::db::ToUint64(MakeCharacter(901).id)) + "," +
            std::to_string(gs::db::ToUint64(MakeCharacter(902).id)));
        int rejected = 0;
        for (const auto* text : {"", "901", "901,901", "0,902", "901,902,903", "901,902x"}) {
            try { (void)ParseI1ObservationFilter(text); } catch (const std::invalid_argument&) { ++rejected; }
        }
        report("i1-filter-rejects", rejected == 6, Fmt("rejected=%d", rejected));
        IoRunner runner;
        WorldRuntime world(runner.io, {}, WorldRuntime::SyntheticWorldConfig{2000.0f, 1, 1, {}});
        auto capture = [&world](I1ObservationFilter f) {
            return ReadWorld(world, [f](const auto& ctx) { return CollectI1Presence(ctx, f); });
        };
        const auto empty = capture(filter);
        report("i1-before-start", !empty.rows[0].registered && !empty.rows[1].registered, "empty copy");
        world.PostSpawn(MakeDetachedSession(runner.io, 39001), MakeCharacter(901), DebugSpawnOverride{500,500});
        world.PostSpawn(MakeDetachedSession(runner.io, 39002), MakeCharacter(902), DebugSpawnOverride{520,500});
        world.Start();
        I1PresenceSnapshot both;
        const bool ready = WaitFor(3000ms, [&] {
            both = capture(filter);
            return both.rows[0].coherent && both.rows[1].coherent;
        });
        report("i1-two-coherent", ready && both.rows[0].tracked.net != both.rows[1].tracked.net,
               Fmt("epoch=%llu tick=%u", (unsigned long long)both.epoch, both.tick));
        const bool negatives = ReadWorld(world, [filter](const auto& ctx) {
            OwnerMap missing_owners;
            PresenceRegistry missing_registry;
            WorldRuntime::SnapshotContext no_owner{ctx.zones, missing_owners, ctx.presence};
            WorldRuntime::SnapshotContext no_registry{ctx.zones, ctx.owners, missing_registry};
            const auto a = CollectI1Presence(no_owner, filter);
            const auto b = CollectI1Presence(no_registry, filter);
            return !a.rows[0].coherent && !a.rows[1].coherent &&
                   !b.rows[0].coherent && b.rows[0].bindings == 1;
        });
        report("i1-negative-owner-registry", negatives, "inconsistent inputs cannot PASS");
        auto tracked = filter;
        for (std::size_t i = 0; i < tracked.size(); ++i) tracked[i] = both.rows[i].tracked;
        world.PostDespawn(39001);
        I1PresenceSnapshot closed;
        const bool cleaned = WaitFor(3000ms, [&] {
            closed = capture(tracked);
            const auto& a = closed.rows[0];
            return !a.registered && a.bindings == 0 && a.entities == 0 && a.owners == 0 && closed.rows[1].coherent;
        });
        report("i1-cleanup-keeps-peer", cleaned, "tracked old entity/owner absent; peer coherent");
        // Stop does not wait on its queued future; the queued collector owns
        // its identities and is safe after the observer object is destroyed.
        for (int i = 0; i < 8; ++i) {
            I1LocalObservation observer;
            observer.Start(world, tracked);
            std::this_thread::yield();
            observer.Stop();
        }
        world.Stop();
        const auto stopped = capture(tracked);
        report("i1-stop-lifetime", !stopped.rows[0].registered && !stopped.rows[1].registered,
               "observer destruction plus inline stopped capture");

        // A queued command in a detached test zone is not drained by capture.
        ZoneManager zones;
        InitialPartition initial;
        std::string error;
        const bool built = BuildInitialPartition(WorldBounds::FromExtent(2000.0f), {}, 240.0f, initial, error);
        if (built) zones.BuildInitialPartition(initial);
        bool ran = false;
        if (built) zones.GetZone(0).Commands().Push([&ran](auto&) { ran = true; });
        OwnerMap owners;
        PresenceRegistry registry;
        WorldRuntime::SnapshotContext queued{zones, owners, registry};
        const auto unchanged = CollectI1Presence(queued, filter);
        report("i1-observation-no-drain", built && !ran && unchanged.rows[0].queued == 1 &&
               zones.GetZone(0).Commands().Depth() == 1, "queued command preserved");
    }

    // ---- single-zone world: enter / re-enter / races / concurrency ----------
    {
        IoRunner runner;
        gs::game::WorldRuntime sim(runner.io, {},
                                   gs::game::WorldRuntime::SyntheticWorldConfig{2000.0f, 1, 1, {}});
        sim.Start();
        const auto character_x = MakeCharacter(1);
        auto stats = [&] { return sim.PresenceStats(); };
        auto wait_claims = [&](std::uint64_t claims, std::uint64_t rejects) {
            return WaitFor(3000ms, [&] {
                const auto s = stats();
                return s.claims >= claims && s.rejected_duplicates >= rejects;
            });
        };

        // Duplicate enter: A holds X, B (observed) is refused over the wire.
        sim.PostSpawn(MakeDetachedSession(runner.io, kBase + 1), character_x,
                      gs::game::DebugSpawnOverride{500.0f, 500.0f});
        wait_claims(1, 0);
        ObservedSession b = MakeObservedSession(runner.io, kBase + 2);
        sim.PostSpawn(b.session, character_x, gs::game::DebugSpawnOverride{600.0f, 600.0f});
        const bool refused = ExpectRefused(b);
        wait_claims(1, 1);
        auto s = stats();
        std::string audit = AuditNow(sim);
        report("duplicate-enter-refused",
               refused && s.claims == 1 && s.rejected_duplicates == 1 && s.present == 1 &&
                   audit == "OK",
               Fmt("wire_reject_and_close=%d claims=%llu rejected=%llu present=%llu audit=%s",
                   refused ? 1 : 0, static_cast<unsigned long long>(s.claims),
                   static_cast<unsigned long long>(s.rejected_duplicates),
                   static_cast<unsigned long long>(s.present), audit.c_str()));

        // The refused session's disconnect must not release the holder.
        sim.PostDespawn(kBase + 2);
        std::this_thread::sleep_for(200ms);
        s = stats();
        audit = AuditNow(sim);
        report("refused-disconnect-keeps-holder", s.present == 1 && audit == "OK",
               Fmt("present=%llu audit=%s", static_cast<unsigned long long>(s.present),
                   audit.c_str()));

        // Same session re-entering: still exactly one presence.
        sim.PostSpawn(MakeDetachedSession(runner.io, kBase + 1), character_x,
                      gs::game::DebugSpawnOverride{510.0f, 510.0f});
        wait_claims(2, 1);
        s = stats();
        audit = AuditNow(sim);
        report("same-session-reenter", s.claims == 2 && s.present == 1 && audit == "OK",
               Fmt("claims=%llu present=%llu audit=%s", static_cast<unsigned long long>(s.claims),
                   static_cast<unsigned long long>(s.present), audit.c_str()));

        // Ordered old/new race: A leaves, then C enters -> C holds X.
        sim.PostDespawn(kBase + 1);
        sim.PostSpawn(MakeDetachedSession(runner.io, kBase + 3), character_x,
                      gs::game::DebugSpawnOverride{520.0f, 520.0f});
        wait_claims(3, 1);
        s = stats();
        audit = AuditNow(sim);
        report("old-leaves-then-new-enters", s.claims == 3 && s.rejected_duplicates == 1 &&
                                                 s.present == 1 && audit == "OK",
               Fmt("claims=%llu rejected=%llu present=%llu audit=%s",
                   static_cast<unsigned long long>(s.claims),
                   static_cast<unsigned long long>(s.rejected_duplicates),
                   static_cast<unsigned long long>(s.present), audit.c_str()));

        // Reversed race: D enters BEFORE C's leave is processed -> D refused
        // (first presence wins), C then leaves -> nobody holds X; a retry by
        // D succeeds. Never two presences at any point (audited).
        sim.PostSpawn(MakeDetachedSession(runner.io, kBase + 4), character_x,
                      gs::game::DebugSpawnOverride{530.0f, 530.0f});
        sim.PostDespawn(kBase + 3);
        wait_claims(3, 2);
        std::this_thread::sleep_for(200ms);
        const auto after_race = stats();
        const std::string audit_race = AuditNow(sim);
        sim.PostSpawn(MakeDetachedSession(runner.io, kBase + 4), character_x,
                      gs::game::DebugSpawnOverride{530.0f, 530.0f});
        wait_claims(4, 2);
        s = stats();
        audit = AuditNow(sim);
        report("new-enters-before-old-leaves",
               after_race.rejected_duplicates == 2 && after_race.present == 0 &&
                   audit_race == "OK" && s.claims == 4 && s.present == 1 && audit == "OK",
               Fmt("race_rejected=%llu race_present=%llu retry_claims=%llu present=%llu "
                   "audits=%s/%s",
                   static_cast<unsigned long long>(after_race.rejected_duplicates),
                   static_cast<unsigned long long>(after_race.present),
                   static_cast<unsigned long long>(s.claims),
                   static_cast<unsigned long long>(s.present), audit_race.c_str(),
                   audit.c_str()));

        // Concurrent enters: 20 sessions, 4 threads, one character.
        const auto character_y = MakeCharacter(2);
        const auto before = stats();
        std::vector<std::thread> posters;
        for (int t = 0; t < 4; ++t) {
            posters.emplace_back([&, t] {
                for (int i = 0; i < 5; ++i) {
                    const auto id = kBase + 100 + static_cast<gs::common::SessionId>(t * 5 + i);
                    sim.PostSpawn(MakeDetachedSession(runner.io, id), character_y,
                                  gs::game::DebugSpawnOverride{700.0f + static_cast<float>(i),
                                                               700.0f + static_cast<float>(t)});
                }
            });
        }
        for (auto& poster : posters) {
            poster.join();
        }
        WaitFor(3000ms, [&] {
            const auto now = stats();
            return (now.claims - before.claims) + (now.rejected_duplicates - before.rejected_duplicates) >= 20;
        });
        s = stats();
        audit = AuditNow(sim);
        const auto claimed = s.claims - before.claims;
        const auto refused_n = s.rejected_duplicates - before.rejected_duplicates;
        report("concurrent-enters-one-winner",
               claimed == 1 && refused_n == 19 && s.present == 2 && audit == "OK",
               Fmt("claimed=%llu refused=%llu present=%llu audit=%s",
                   static_cast<unsigned long long>(claimed),
                   static_cast<unsigned long long>(refused_n),
                   static_cast<unsigned long long>(s.present), audit.c_str()));
        sim.Stop();
    }

    // ---- migration: the presence follows the player across zones ------------
    {
        IoRunner runner;
        gs::game::WorldRuntime sim(runner.io, {},
                                   gs::game::WorldRuntime::SyntheticWorldConfig{2000.0f, 2, 1, {}});
        sim.Start();
        const auto character_z = MakeCharacter(3);
        const gs::common::SessionId walker = kBase + 200;
        sim.PostSpawn(MakeDetachedSession(runner.io, walker), character_z,
                      gs::game::DebugSpawnOverride{985.0f, 1000.0f});
        WaitFor(3000ms, [&] { return sim.PresenceStats().claims >= 1; });
        const auto mig0 = sim.MigrationMetrics();
        std::atomic<bool> run{true};
        std::thread poster([&] {
            std::uint32_t seq = 0;
            const auto start = Clock::now();
            while (run.load()) {
                const double t = std::chrono::duration<double>(Clock::now() - start).count();
                const bool east = (static_cast<int>(t / 5.0) % 2) == 0;
                sim.PostMoveInput(walker, ++seq, east ? kHalfPi : -kHalfPi,
                                  gs::game::MoveState::Running);
                std::this_thread::sleep_for(20ms);
            }
        });
        int audits_ok = 0;
        int dup_refused = 0;
        for (int round = 0; round < 8; ++round) {
            std::this_thread::sleep_for(2500ms);
            const auto rejects0 = sim.PresenceStats().rejected_duplicates;
            sim.PostSpawn(MakeDetachedSession(runner.io, kBase + 300 + round), character_z,
                          gs::game::DebugSpawnOverride{1500.0f, 1000.0f});
            if (WaitFor(2000ms,
                        [&] { return sim.PresenceStats().rejected_duplicates > rejects0; })) {
                ++dup_refused;
            }
            audits_ok += AuditNow(sim) == "OK" ? 1 : 0;
        }
        run = false;
        poster.join();
        const auto migrations = sim.MigrationMetrics().committed - mig0.committed;
        const auto s = sim.PresenceStats();
        report("presence-across-migration",
               migrations >= 2 && audits_ok == 8 && dup_refused == 8 && s.present == 1,
               Fmt("migrations=%llu audits_ok=%d/8 duplicates_refused=%d/8 present=%llu",
                   static_cast<unsigned long long>(migrations), audits_ok, dup_refused,
                   static_cast<unsigned long long>(s.present)));
        sim.Stop();
    }

    // ---- split / merge retirement: presence survives topology changes -------
    {
        IoRunner runner;
        gs::game::WorldRuntime sim(runner.io, {},
                                   gs::game::WorldRuntime::SyntheticWorldConfig{2000.0f, 1, 1, {}});
        sim.Start();
        const auto character_w = MakeCharacter(4);
        sim.PostSpawn(MakeDetachedSession(runner.io, kBase + 400), character_w,
                      gs::game::DebugSpawnOverride{400.0f, 400.0f});
        WaitFor(3000ms, [&] { return sim.PresenceStats().claims >= 1; });
        const auto p0 = sim.PartitionMetricsSnapshot();
        sim.PostForceSplit(1);
        const bool split = WaitFor(5000ms, [&] {
            return sim.PartitionMetricsSnapshot().split_commits > p0.split_commits;
        });
        const std::string audit_split = AuditNow(sim);
        const auto rejects0 = sim.PresenceStats().rejected_duplicates;
        sim.PostSpawn(MakeDetachedSession(runner.io, kBase + 401), character_w,
                      gs::game::DebugSpawnOverride{1500.0f, 1500.0f});
        const bool dup_after_split = WaitFor(2000ms, [&] {
            return sim.PresenceStats().rejected_duplicates > rejects0;
        });
        sim.PostForceMerge(1);
        const bool merged = WaitFor(5000ms, [&] {
            return sim.PartitionMetricsSnapshot().merge_commits > p0.merge_commits;
        });
        const std::string audit_merge = AuditNow(sim);
        const auto s = sim.PresenceStats();
        report("presence-across-split-merge",
               split && merged && audit_split == "OK" && audit_merge == "OK" && dup_after_split &&
                   s.present == 1,
               Fmt("split=%d merge=%d audits=%s/%s duplicate_refused=%d present=%llu",
                   split ? 1 : 0, merged ? 1 : 0, audit_split.c_str(), audit_merge.c_str(),
                   dup_after_split ? 1 : 0, static_cast<unsigned long long>(s.present)));
        sim.Stop();
    }

    std::printf("PRESENCE-DONE failures=%d\n", check.failures);
    return check.failures;
}

// ============================================================================
// ASF control-metric determinism (H5)
// ============================================================================
namespace {

struct ControlRun {
    int diag_interval_ms = 0;
    std::uint64_t updates = 0;
    std::uint64_t breach_starts = 0;
    std::uint64_t breach_resets = 0;
    std::uint64_t low_starts = 0;
    double first_candidate_s = -1.0; // since the load was established
};

// One zone permanently over its tick budget (the legacy tick score is the only
// signal: load field off, resident budget out of reach), adaptive execution off
// so topology never changes -- only the control DECISION STATE is observed.
ControlRun RunControlUnderDiagCadence(int diag_interval_ms)
{
    ControlRun run;
    run.diag_interval_ms = diag_interval_ms;
    IoRunner runner;
    gs::game::WorldRuntime sim(runner.io, {},
                               gs::game::WorldRuntime::SyntheticWorldConfig{4000.0f, 1, 1, {}});
    sim.ConfigureDiagnosticsInterval(std::chrono::milliseconds(diag_interval_ms));
    gs::game::PartitionConfig partition;
    partition.tick_budget_ms = 0.25f;     // any real tick of this zone breaches
    partition.resident_budget = 1.0e7f;   // residents never drive the score
    partition.sustained_window_seconds = 3;
    partition.split_cooldown_seconds = 0;
    partition.scoring.adaptive_enabled = false; // observe only: no topology change
    sim.ConfigurePartition(partition);
    gs::game::LoadFieldConfig field;
    field.enabled = false;
    sim.ConfigureLoadField(field);
    gs::game::MobSpawnPoint point;
    point.mob_type_id = 2;
    point.x = 2000.0f;
    point.y = 2000.0f;
    point.count = 3000;
    point.radius = 100.0f; // inside the Full LOD bubble of the player below
    sim.AddMobSpawnPoint(point);
    sim.SpawnConfiguredMobsNow();
    sim.PostSpawn(MakeDetachedSession(runner.io, 40000), MakeCharacter(40),
                  gs::game::DebugSpawnOverride{2000.0f, 2000.0f});
    sim.Start();
    WaitFor(5000ms, [&] { return sim.PresenceStats().claims >= 1; });
    std::this_thread::sleep_for(1500ms); // load established, first windows filled
    const auto t0 = Clock::now();
    const auto c0 = sim.PartitionControlCounters();
    std::this_thread::sleep_for(12s);
    const auto c1 = sim.PartitionControlCounters();
    sim.Stop();
    run.updates = c1.updates - c0.updates;
    run.breach_starts = c1.breach_starts - c0.breach_starts;
    run.breach_resets = c1.breach_resets - c0.breach_resets;
    run.low_starts = c1.low_starts - c0.low_starts;
    if (c1.first_split_candidate_ns != 0) {
        const auto t0_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               t0.time_since_epoch())
                               .count();
        run.first_candidate_s = static_cast<double>(c1.first_split_candidate_ns - t0_ns) / 1e9;
    }
    return run;
}

// A merge re-points the PARENT node at a brand-new zone whose cumulative
// control counters start from zero, while the node still carries the
// baseline of the zone it held before the split. Every control window --
// including the first one after the merge -- must stay bounded by the
// control period, never wrap.
struct MergeWindowRun {
    bool split = false;
    bool merged = false;
    std::uint64_t updates = 0;
    std::uint64_t max_window_ticks = 0;
};

MergeWindowRun RunControlAcrossSplitMerge()
{
    MergeWindowRun run;
    IoRunner runner;
    gs::game::WorldRuntime sim(runner.io, {},
                               gs::game::WorldRuntime::SyntheticWorldConfig{4000.0f, 1, 1, {}});
    gs::game::PartitionConfig partition;
    partition.scoring.adaptive_enabled = false; // topology changes are forced only
    sim.ConfigurePartition(partition);
    gs::game::LoadFieldConfig field;
    field.enabled = false;
    sim.ConfigureLoadField(field);
    gs::game::MobSpawnPoint point;
    point.mob_type_id = 2;
    point.x = 2000.0f;
    point.y = 2000.0f;
    point.count = 500;
    point.radius = 100.0f;
    sim.AddMobSpawnPoint(point);
    sim.SpawnConfiguredMobsNow();
    sim.PostSpawn(MakeDetachedSession(runner.io, 41000), MakeCharacter(41),
                  gs::game::DebugSpawnOverride{2000.0f, 2000.0f});
    sim.Start();
    WaitFor(5000ms, [&] { return sim.PresenceStats().claims >= 1; });
    // Let the original zone accumulate a large baseline on the root node.
    std::this_thread::sleep_for(4s);
    const auto p0 = sim.PartitionMetricsSnapshot();
    const auto c0 = sim.PartitionControlCounters();
    sim.PostForceSplit(1);
    run.split = WaitFor(5000ms, [&] {
        return sim.PartitionMetricsSnapshot().split_commits > p0.split_commits;
    });
    std::this_thread::sleep_for(3s);
    sim.PostForceMerge(1);
    run.merged = WaitFor(5000ms, [&] {
        return sim.PartitionMetricsSnapshot().merge_commits > p0.merge_commits;
    });
    std::this_thread::sleep_for(3s); // several windows of the merged zone
    const auto c1 = sim.PartitionControlCounters();
    sim.Stop();
    run.updates = c1.updates - c0.updates;
    run.max_window_ticks = c1.max_window_ticks;
    return run;
}

} // namespace

int RunAsfDeterminismScenario()
{
    int failures = 0;
    std::vector<ControlRun> runs;
    for (const int interval : {1000, 100, 37}) {
        runs.push_back(RunControlUnderDiagCadence(interval));
        const auto& r = runs.back();
        // Continuously overloaded zone: the sustained-breach timer must start
        // once and never be reset, no leaf may look "low" (merge side), and
        // the split candidacy must mature one sustained window after the
        // breach started (the breach itself precedes t0, hence <= window).
        const bool pass = r.breach_resets == 0 && r.low_starts == 0 && r.first_candidate_s >= -1.5 &&
                          r.first_candidate_s <= 4.5 && r.updates >= 10;
        std::printf("ASF diag_interval=%dms control_updates=%llu breach_starts=%llu "
                    "breach_resets=%llu false_low_starts=%llu first_split_candidate=%.2fs: %s\n",
                    r.diag_interval_ms,
                    static_cast<unsigned long long>(r.updates),
                    static_cast<unsigned long long>(r.breach_starts),
                    static_cast<unsigned long long>(r.breach_resets),
                    static_cast<unsigned long long>(r.low_starts),
                    r.first_candidate_s,
                    pass ? "PASS" : "FAIL");
        std::fflush(stdout);
        if (!pass) {
            ++failures;
        }
    }
    // Cross-cadence agreement: same workload, different diagnostic phase ->
    // the same decision timing (within one control period).
    double lo = 1e9;
    double hi = -1e9;
    for (const auto& r : runs) {
        lo = std::min(lo, r.first_candidate_s);
        hi = std::max(hi, r.first_candidate_s);
    }
    const bool agree = hi - lo <= 1.5;
    std::printf("ASF cross-cadence candidate spread=%.2fs: %s\n", hi - lo, agree ? "PASS" : "FAIL");
    if (!agree) {
        ++failures;
    }

    // Control window across split -> merge (merged zone reuses the parent
    // node). 1 Hz control cadence at 20 Hz: a window is ~20 ticks; a zone's
    // first window covers its lifetime so far. 100 = 5 s of ticks, generous.
    const MergeWindowRun m = RunControlAcrossSplitMerge();
    const bool window_ok = m.split && m.merged && m.updates >= 6 && m.max_window_ticks <= 100;
    std::printf("ASF control-window-across-split-merge split=%d merge=%d control_updates=%llu "
                "max_window_ticks=%llu: %s\n",
                m.split ? 1 : 0,
                m.merged ? 1 : 0,
                static_cast<unsigned long long>(m.updates),
                static_cast<unsigned long long>(m.max_window_ticks),
                window_ok ? "PASS" : "FAIL");
    if (!window_ok) {
        ++failures;
    }
    std::printf("ASF-DONE failures=%d\n", failures);
    return failures;
}

// ============================================================================
// Worker pool revalidation (H6)
// ============================================================================
namespace {

std::size_t WaitWorkers(gs::game::WorldRuntime& sim)
{
    std::size_t workers = 0;
    WaitFor(5000ms, [&] {
        workers = sim.SchedulerStats().workers;
        return workers > 0;
    });
    return workers;
}

struct ParallelWindow {
    double parallelism = 0.0;
    std::size_t workers = 0;
    std::size_t active_workers = 0; // workers that ran at least one zone tick
    std::size_t zones = 0;
};

ParallelWindow MeasureParallelism(gs::game::WorldRuntime& sim, std::chrono::seconds window)
{
    const auto s0 = sim.SchedulerStats();
    const auto t0 = Clock::now();
    std::this_thread::sleep_for(window);
    const auto s1 = sim.SchedulerStats();
    const double wall_us =
        std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
    ParallelWindow out;
    out.parallelism = wall_us > 0.0 ? static_cast<double>(s1.worker_work_micros -
                                                          s0.worker_work_micros) /
                                          wall_us
                                    : 0.0;
    out.workers = s1.workers;
    for (std::size_t i = 0; i < s1.worker_tasks.size(); ++i) {
        const std::uint64_t before = i < s0.worker_tasks.size() ? s0.worker_tasks[i] : 0;
        out.active_workers += s1.worker_tasks[i] > before ? 1 : 0;
    }
    out.zones = ReadWorld(
        sim, [](const WorldSnapshot& snap) { return snap.zones.GetActiveLeaves().size(); });
    return out;
}

} // namespace

int RunWorkerPoolScenario()
{
    int failures = 0;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    std::printf("WORKERPOOL hardware_concurrency=%u\n", hw);

    // (a) Production-shaped: the real map (3 seeded zones), automatic sizing.
    std::size_t production_workers = 0;
    {
        IoRunner runner;
        gs::game::WorldRuntime sim(runner.io, {}, LoadBenchTestWorld(), LegacyTestMapLayout());
        sim.Start();
        production_workers = WaitWorkers(sim);
        std::printf("WORKERPOOL production-map zones=%zu workers=%zu (pool sized once at Start)\n",
                    ReadWorld(sim, [](const WorldSnapshot& snap) { return snap.zones.ZoneCount(); }),
                    production_workers);
        sim.Stop();
    }

    // (b) Explicit override on a 1-zone world.
    std::size_t override_workers = 0;
    {
        IoRunner runner;
        gs::game::WorldRuntime sim(runner.io, {},
                                   gs::game::WorldRuntime::SyntheticWorldConfig{2000.0f, 1, 1, {}});
        sim.ConfigureWorkers(8);
        sim.Start();
        override_workers = WaitWorkers(sim);
        std::printf("WORKERPOOL override requested=8 zones=1 workers=%zu\n", override_workers);
        sim.Stop();
    }

    // (c) Split scaling: one heavily loaded zone split into four. Four player
    // anchored mob clusters (one per future quadrant) keep all four children
    // busy; adaptive control is off so topology changes only when forced.
    ParallelWindow before_split;
    ParallelWindow after_split;
    {
        IoRunner runner;
        gs::game::WorldRuntime sim(runner.io, {},
                                   gs::game::WorldRuntime::SyntheticWorldConfig{8000.0f, 1, 1, {}});
        gs::game::PartitionConfig partition;
        partition.scoring.adaptive_enabled = false;
        sim.ConfigurePartition(partition);
        const float quad[4][2] = {{2000.0f, 2000.0f}, {6000.0f, 2000.0f}, {2000.0f, 6000.0f},
                                  {6000.0f, 6000.0f}};
        for (const auto& q : quad) {
            gs::game::MobSpawnPoint point;
            point.mob_type_id = 2;
            point.x = q[0];
            point.y = q[1];
            point.count = 2500;
            point.radius = 100.0f;
            sim.AddMobSpawnPoint(point);
        }
        sim.SpawnConfiguredMobsNow();
        for (int i = 0; i < 4; ++i) {
            sim.PostSpawn(MakeDetachedSession(runner.io, 41000 + static_cast<gs::common::SessionId>(i)),
                          MakeCharacter(50 + static_cast<std::uint64_t>(i)),
                          gs::game::DebugSpawnOverride{quad[i][0], quad[i][1]});
        }
        sim.Start();
        WaitFor(5000ms, [&] { return sim.PresenceStats().claims >= 4; });
        std::this_thread::sleep_for(2s);
        before_split = MeasureParallelism(sim, 5s);
        const auto p0 = sim.PartitionMetricsSnapshot();
        sim.PostForceSplit(1);
        WaitFor(10000ms, [&] {
            return sim.PartitionMetricsSnapshot().split_commits > p0.split_commits;
        });
        std::this_thread::sleep_for(2s);
        after_split = MeasureParallelism(sim, 5s);
        sim.Stop();
    }
    std::printf("WORKERPOOL split-scaling before: zones=%zu workers=%zu active=%zu parallelism=%.2f | "
                "after split: zones=%zu workers=%zu active=%zu parallelism=%.2f\n",
                before_split.zones, before_split.workers, before_split.active_workers,
                before_split.parallelism, after_split.zones, after_split.workers,
                after_split.active_workers, after_split.parallelism);

    // Expectations for a pool that can use what the hardware and topology
    // offer: production map not capped at its seed zone count, an explicit
    // override honored, and a split turning into real parallelism.
    const std::size_t hw_workers = hw > 1 ? hw - 1 : 1;
    const bool production_ok = production_workers == hw_workers;
    const bool override_ok = override_workers == 8;
    // Capacity, not load: after the split the four zones' ticks must be able
    // to run on different workers (parallelism itself depends on how heavy
    // the zones are and is reported for information).
    const bool scaling_ok =
        after_split.zones == 4 && after_split.workers >= 4 && after_split.active_workers >= 2;
    std::printf("WORKERPOOL production-not-capped-by-seed-zones: %s\n", production_ok ? "PASS" : "FAIL");
    std::printf("WORKERPOOL override-honored: %s\n", override_ok ? "PASS" : "FAIL");
    std::printf("WORKERPOOL split-adds-parallelism: %s\n", scaling_ok ? "PASS" : "FAIL");
    failures += production_ok ? 0 : 1;
    failures += override_ok ? 0 : 1;
    failures += scaling_ok ? 0 : 1;
    std::printf("WORKERPOOL-DONE failures=%d\n", failures);
    return failures;
}

// ============================================================================
// Replication v2 correctness (H7)
// ============================================================================
namespace {

// Client-side view decoded from the real wire: per-net record counts (v1 full
// records and v2 masked deltas), spawn names -> net ids, and the header tick
// sequence (monotonicity / step size).
struct ReplWire {
    std::mutex mutex;
    std::unordered_map<std::uint32_t, std::uint64_t> records_by_net;
    std::unordered_map<std::string, std::uint32_t> net_by_name;
    bool have_tick = false;
    std::uint32_t last_tick = 0;
    std::uint64_t backward_steps = 0;
    std::uint32_t max_forward_step = 0;
    std::uint64_t frames = 0;
    float self_x = 0.0f;
    float self_y = 0.0f;
    std::uint64_t self_nonfinite_frames = 0; // the viewer's own record carried NaN/Inf
};

void DecodeReplFrames(TestClient& client, ReplWire& wire, const std::atomic<bool>& stop)
{
    std::vector<std::uint8_t> payload;
    while (!stop.load()) {
        const auto read = client.ReadFrame(payload, 200ms);
        if (read == TestClient::Read::Closed) {
            return;
        }
        if (read != TestClient::Read::Frame || payload.empty()) {
            continue;
        }
        std::lock_guard lock(wire.mutex);
        if (payload[0] == 0) {
            try {
                auto parsed = gs::protocol::ParsePacket(payload);
                if (parsed && parsed->packet.isEntitySpawn()) {
                    const auto spawn = parsed->packet.getEntitySpawn();
                    wire.net_by_name[spawn.getName().cStr()] = spawn.getNetId();
                }
            } catch (const kj::Exception&) {
            }
            continue;
        }
        if (payload.size() < 8 + 19 || (payload[1] != 0x10 && payload[1] != 0x11)) {
            continue;
        }
        const std::uint32_t tick = ReadU32Le(payload.data() + 2);
        if (wire.have_tick) {
            if (tick < wire.last_tick) {
                ++wire.backward_steps;
            } else {
                wire.max_forward_step = std::max(wire.max_forward_step, tick - wire.last_tick);
            }
        }
        wire.have_tick = true;
        wire.last_tick = tick;
        ++wire.frames;
        wire.self_x = ReadF32Le(payload.data() + 8 + 4);
        wire.self_y = ReadF32Le(payload.data() + 8 + 8);
        if (!std::isfinite(wire.self_x) || !std::isfinite(wire.self_y) ||
            !std::isfinite(ReadF32Le(payload.data() + 8 + 12))) {
            ++wire.self_nonfinite_frames;
        }
        const std::uint32_t count = static_cast<std::uint32_t>(payload[6]) |
                                    (static_cast<std::uint32_t>(payload[7]) << 8);
        std::size_t offset = 8 + 19; // header + the viewer's own full record
        for (std::uint32_t i = 1; i < count; ++i) {
            if (payload[1] == 0x10) {
                if (offset + 19 > payload.size()) {
                    break;
                }
                ++wire.records_by_net[ReadU32Le(payload.data() + offset)];
                offset += 19;
                continue;
            }
            if (offset + 5 > payload.size()) {
                break;
            }
            const std::uint32_t net = ReadU32Le(payload.data() + offset);
            const std::uint8_t mask = payload[offset + 4];
            offset += 5 + ((mask & 0x01) ? 12 : 0) + ((mask & 0x02) ? 2 : 0) +
                      ((mask & 0x04) ? 1 : 0);
            ++wire.records_by_net[net];
        }
    }
}

// Pre-hardening QuantizeHeading loop, kept only as the equivalence reference.
// `max_iterations` bounds the model: the original had no bound at all.
bool LegacyQuantizeHeading(float angle, std::uint16_t& out, long long max_iterations)
{
    long long iterations = 0;
    while (angle < 0.0f) {
        angle += gs::game::kTwoPi;
        if (++iterations > max_iterations) {
            return false;
        }
    }
    while (angle >= gs::game::kTwoPi) {
        angle -= gs::game::kTwoPi;
        if (++iterations > max_iterations) {
            return false;
        }
    }
    out = static_cast<std::uint16_t>(std::lround((angle / gs::game::kTwoPi) * 65535.0f));
    return true;
}

} // namespace

int RunReplicationV2Scenario()
{
    int failures = 0;
    auto report = [&](const char* name, bool pass, const std::string& detail) {
        std::printf("REPLV2 %s %s: %s\n", name, detail.c_str(), pass ? "PASS" : "FAIL");
        std::fflush(stdout);
        failures += pass ? 0 : 1;
    };

    // ---- (a) periodic resync reaches UNCHANGED entities ----------------------
    {
        IoRunner runner;
        gs::game::WorldRuntime sim(runner.io, {},
                                   gs::game::WorldRuntime::SyntheticWorldConfig{2000.0f, 1, 1, {}});
        sim.Start();
        ObservedSession viewer = MakeObservedSession(runner.io, 50000);
        ReplWire wire;
        std::atomic<bool> stop{false};
        std::thread reader([&] { DecodeReplFrames(*viewer.client, wire, stop); });
        sim.PostSpawn(viewer.session, MakeCharacter(60), gs::game::DebugSpawnOverride{1000.0f, 1000.0f});
        // A static neighbor: spawned, then never changes.
        sim.PostSpawn(MakeDetachedSession(runner.io, 50001), MakeCharacter(61),
                      gs::game::DebugSpawnOverride{1010.0f, 1000.0f});
        const std::string neighbor = "Hardening61";
        WaitFor(5000ms, [&] {
            std::lock_guard lock(wire.mutex);
            return wire.net_by_name.contains(neighbor) && wire.frames > 20;
        });
        std::uint32_t neighbor_net = 0;
        std::uint64_t before = 0;
        {
            std::lock_guard lock(wire.mutex);
            neighbor_net = wire.net_by_name[neighbor];
            before = wire.records_by_net[neighbor_net];
        }
        std::this_thread::sleep_for(3500ms);
        std::uint64_t after = 0;
        {
            std::lock_guard lock(wire.mutex);
            after = wire.records_by_net[neighbor_net];
        }
        stop = true;
        reader.join();
        sim.Stop();
        const auto records = after - before;
        // refresh_ticks 20 (1 s): at least 3 full-state resync records in 3.5 s.
        report("resync-reaches-unchanged-entity", neighbor_net != 0 && records >= 3,
               Fmt("static_neighbor_net=%u records_in_3.5s=%llu", neighbor_net,
                   static_cast<unsigned long long>(records)));
    }

    // ---- (b) starvation bound under a tight budget ---------------------------
    // 30 players keep moving around one viewer (new versions every tick);
    // 2 records per frame; the periodic refresh pushed out to 20 s so it cannot
    // mask starvation. The shadow audit checks the recipient freshness bound.
    {
        IoRunner runner;
        gs::game::WorldRuntime sim(runner.io, {},
                                   gs::game::WorldRuntime::SyntheticWorldConfig{2000.0f, 1, 1, {}});
        gs::game::ReplicationConfig replication;
        replication.budget_max_records = 2;
        replication.refresh_ticks = 400;
        replication.max_defer_ticks = 40;
        sim.ConfigureReplication(replication);
        sim.Start();
        sim.PostSpawn(MakeDetachedSession(runner.io, 51000), MakeCharacter(70),
                      gs::game::DebugSpawnOverride{1000.0f, 1000.0f});
        constexpr int kMovers = 30;
        for (int i = 0; i < kMovers; ++i) {
            const float angle = static_cast<float>(i) * 0.2094f;
            sim.PostSpawn(MakeDetachedSession(runner.io, 51001 + static_cast<gs::common::SessionId>(i)),
                          MakeCharacter(71 + static_cast<std::uint64_t>(i)),
                          gs::game::DebugSpawnOverride{1000.0f + 60.0f * std::cos(angle),
                                                       1000.0f + 60.0f * std::sin(angle)});
        }
        WaitFor(5000ms, [&] { return sim.PresenceStats().claims >= kMovers + 1; });
        std::atomic<bool> run{true};
        std::thread poster([&] {
            std::uint32_t seq = 0;
            const auto start = Clock::now();
            while (run.load()) {
                const double t = std::chrono::duration<double>(Clock::now() - start).count();
                const bool east = (static_cast<int>(t * 2.0) % 2) == 0;
                ++seq;
                for (int i = 0; i < kMovers; ++i) {
                    sim.PostMoveInput(51001 + static_cast<gs::common::SessionId>(i), seq,
                                      east ? kHalfPi : -kHalfPi, gs::game::MoveState::Walking);
                }
                std::this_thread::sleep_for(20ms);
            }
        });
        std::this_thread::sleep_for(4s); // let deferral build up
        int audits = 0;
        int audit_failures = 0;
        std::string first_failure;
        for (int round = 0; round < 6; ++round) {
            sim.RequestReplicationValidation();
            std::string result;
            if (WaitFor(3000ms, [&] { return sim.TryTakeReplicationValidationResult(result); })) {
                ++audits;
                if (result != "OK") {
                    ++audit_failures;
                    if (first_failure.empty()) {
                        first_failure = result.substr(0, 160);
                    }
                }
            }
            std::this_thread::sleep_for(700ms);
        }
        run = false;
        poster.join();
        sim.Stop();
        report("starvation-bound-under-budget", audits == 6 && audit_failures == 0,
               Fmt("audits=%d failures=%d%s%s", audits, audit_failures,
                   first_failure.empty() ? "" : " first=", first_failure.c_str()));
    }

    // ---- (b2) idle-then-move is NOT starvation -------------------------------
    // No budget: nothing is ever deferred beyond its Network LOD period. Two
    // neighbors stand still far longer than max_defer_ticks (their last SEND
    // is old, but the recipient's knowledge is re-verified every due check),
    // then start moving. Their first move is a fresh change, not a starved
    // one: zero starvation bypasses, and the audits stay clean.
    {
        IoRunner runner;
        gs::game::WorldRuntime sim(runner.io, {},
                                   gs::game::WorldRuntime::SyntheticWorldConfig{2000.0f, 1, 1, {}});
        sim.ConfigureDiagnosticsInterval(std::chrono::hours(1)); // *_since_diag never reset
        gs::game::ReplicationConfig replication;
        replication.refresh_ticks = 400; // no periodic resync inside the window
        replication.max_defer_ticks = 40;
        sim.ConfigureReplication(replication);
        sim.Start();
        sim.PostSpawn(MakeDetachedSession(runner.io, 53000), MakeCharacter(120),
                      gs::game::DebugSpawnOverride{1000.0f, 1000.0f});
        // near tier (30 m, period 1) and normal tier (60 m, period 2)
        sim.PostSpawn(MakeDetachedSession(runner.io, 53001), MakeCharacter(121),
                      gs::game::DebugSpawnOverride{1030.0f, 1000.0f});
        sim.PostSpawn(MakeDetachedSession(runner.io, 53002), MakeCharacter(122),
                      gs::game::DebugSpawnOverride{1000.0f, 1060.0f});
        WaitFor(5000ms, [&] { return sim.PresenceStats().claims >= 3; });
        const auto starvation = [&] {
            return ReadWorld(sim, [](const WorldSnapshot& snap) {
                std::uint64_t total = 0;
                for (std::size_t i = 0; i < snap.zones.ZoneCount(); ++i) {
                    total += snap.zones.GetZone(i)
                                 .Diagnostics()
                                 .repl_v2_starvation_since_diag.load(std::memory_order_relaxed);
                }
                return total;
            });
        };
        std::this_thread::sleep_for(3500ms); // idle: 70 ticks > max_defer_ticks
        const auto s0 = starvation();
        std::atomic<bool> run{true};
        std::thread poster([&] {
            std::uint32_t seq = 0;
            while (run.load()) {
                ++seq;
                sim.PostMoveInput(53001, seq, kHalfPi, gs::game::MoveState::Walking);
                sim.PostMoveInput(53002, seq, 0.0f, gs::game::MoveState::Walking);
                std::this_thread::sleep_for(20ms);
            }
        });
        int audits = 0;
        int audit_failures = 0;
        for (int round = 0; round < 4; ++round) {
            std::this_thread::sleep_for(round == 0 ? 60ms : 400ms); // first audit right after the start
            sim.RequestReplicationValidation();
            std::string result;
            if (WaitFor(3000ms, [&] { return sim.TryTakeReplicationValidationResult(result); })) {
                ++audits;
                audit_failures += result == "OK" ? 0 : 1;
            }
        }
        run = false;
        poster.join();
        const auto s1 = starvation();
        sim.Stop();
        report("idle-then-move-not-starvation", s1 - s0 == 0 && audits == 4 && audit_failures == 0,
               Fmt("starvation_bypasses=%llu audits=%d failures=%d",
                   static_cast<unsigned long long>(s1 - s0), audits, audit_failures));
    }

    // ---- (b3) resync schedule under skipped world-tick samples ---------------
    // Deterministic: 200 viewers (phase key = NetId), period 20, 4000 world
    // ticks, three sampling patterns of the zone's view of the global tick.
    // The pre-H7 exact-match predicate vs the due-based schedule.
    {
        constexpr std::uint32_t kPeriod = 20;
        constexpr std::uint32_t kViewers = 200;
        constexpr std::uint32_t kTicks = 4000;
        const auto legacy = [](std::uint32_t tick, std::uint32_t key, std::uint32_t period) {
            return period > 0 && ((tick + key) % period) == 0;
        };
        struct Pattern {
            const char* name;
            std::vector<std::uint32_t> samples;
        };
        std::vector<Pattern> patterns;
        {
            Pattern every{"every-tick", {}};
            for (std::uint32_t t = 1; t <= kTicks; ++t) {
                every.samples.push_back(t);
            }
            patterns.push_back(std::move(every));
            Pattern half{"every-other-tick", {}};
            for (std::uint32_t t = 2; t <= kTicks; t += 2) {
                half.samples.push_back(t);
            }
            patterns.push_back(std::move(half));
            // Jitter: steps of 0 (duplicate sample), 1 or 2 (skip).
            Pattern jitter{"jitter-dup-skip", {}};
            std::mt19937 rng(1234);
            std::uniform_int_distribution<int> step(0, 9);
            std::uint32_t t = 1;
            while (t <= kTicks) {
                jitter.samples.push_back(t);
                const int r = step(rng);
                t += r == 0 ? 0u : (r == 1 ? 2u : 1u);
            }
            patterns.push_back(std::move(jitter));
        }
        for (const auto& pattern : patterns) {
            std::uint32_t legacy_min = UINT32_MAX;
            std::uint32_t legacy_starved_viewers = 0;
            std::uint32_t due_min = UINT32_MAX;
            std::uint32_t due_max = 0;
            std::uint32_t due_max_gap = 0;
            std::uint32_t same_as_legacy = 0;
            for (std::uint32_t key = 1; key <= kViewers; ++key) {
                std::uint32_t next_due = 0;
                std::uint32_t legacy_count = 0;
                std::uint32_t due_count = 0;
                std::uint32_t last_refresh = 0;
                bool identical = true;
                for (const std::uint32_t tick : pattern.samples) {
                    const bool a = legacy(tick, key, kPeriod);
                    const bool b = gs::game::ConsumeResyncDue(next_due, tick, key, kPeriod);
                    legacy_count += a ? 1u : 0u;
                    if (b) {
                        if (due_count > 0) {
                            due_max_gap = std::max(due_max_gap, tick - last_refresh);
                        }
                        last_refresh = tick;
                        ++due_count;
                    }
                    identical = identical && a == b;
                }
                legacy_min = std::min(legacy_min, legacy_count);
                legacy_starved_viewers += legacy_count == 0 ? 1u : 0u;
                due_min = std::min(due_min, due_count);
                due_max = std::max(due_max, due_count);
                same_as_legacy += identical ? 1u : 0u;
            }
            const std::uint32_t expected = kTicks / kPeriod; // 200
            // Every viewer refreshed once per period (+-1 at the edges), and
            // no gap longer than a period plus the largest sampling step.
            bool pass = due_min + 1 >= expected && due_max <= expected + 1 && due_max_gap <= kPeriod + 2;
            if (std::strcmp(pattern.name, "every-tick") == 0) {
                // Regular sampling: bit-identical to the legacy stagger.
                pass = pass && same_as_legacy == kViewers;
            }
            report("resync-schedule-under-tick-skips",
                   pass,
                   Fmt("pattern=%s expected=%u due=[min=%u max=%u max_gap=%u] identical_to_legacy=%u/%u "
                       "legacy=[min=%u never_refreshed_viewers=%u]",
                       pattern.name, expected, due_min, due_max, due_max_gap, same_as_legacy,
                       kViewers, legacy_min, legacy_starved_viewers));
        }
    }

    // ---- (c) wire tick semantics across a migration --------------------------
    // Zone B is empty (asleep, its local tick counter frozen) while the player
    // lives in zone A; then the player runs across the border.
    {
        IoRunner runner;
        gs::game::WorldRuntime sim(runner.io, {},
                                   gs::game::WorldRuntime::SyntheticWorldConfig{2000.0f, 2, 1, {}});
        sim.Start();
        ObservedSession viewer = MakeObservedSession(runner.io, 52000);
        ReplWire wire;
        std::atomic<bool> stop{false};
        std::thread reader([&] { DecodeReplFrames(*viewer.client, wire, stop); });
        sim.PostSpawn(viewer.session, MakeCharacter(110), gs::game::DebugSpawnOverride{960.0f, 1000.0f});
        WaitFor(5000ms, [&] {
            std::lock_guard lock(wire.mutex);
            return wire.frames > 0;
        });
        std::this_thread::sleep_for(3000ms); // zone A's local counter runs ahead
        const auto mig0 = sim.MigrationMetrics();
        std::uint32_t seq = 0;
        const auto deadline = Clock::now() + 8s;
        while (Clock::now() < deadline && sim.MigrationMetrics().committed == mig0.committed) {
            sim.PostMoveInput(52000, ++seq, kHalfPi, gs::game::MoveState::Running);
            std::this_thread::sleep_for(20ms);
        }
        sim.PostMoveInput(52000, ++seq, kHalfPi, gs::game::MoveState::Idle);
        std::this_thread::sleep_for(1000ms);
        stop = true;
        reader.join();
        const bool migrated = sim.MigrationMetrics().committed > mig0.committed;
        sim.Stop();
        std::uint64_t backward = 0;
        std::uint32_t max_step = 0;
        {
            std::lock_guard lock(wire.mutex);
            backward = wire.backward_steps;
            max_step = wire.max_forward_step;
        }
        report("wire-tick-monotonic-across-migration", migrated && backward == 0 && max_step <= 3,
               Fmt("migrated=%d backward_steps=%llu max_forward_step=%u", migrated ? 1 : 0,
                   static_cast<unsigned long long>(backward), max_step));
    }

    // ---- (d) QuantizeHeading robustness + equivalence ------------------------
    {
        const float extremes[] = {std::numeric_limits<float>::quiet_NaN(),
                                  std::numeric_limits<float>::infinity(),
                                  -std::numeric_limits<float>::infinity(),
                                  1.0e30f, -1.0e30f, 3.0e38f, 1.0e8f, -1.0e8f, 2.0e9f};
        // The production function runs on a watchdog thread: before the fix a
        // non-finite/huge heading never returned (the test must not hang too).
        auto done = std::make_shared<std::promise<std::pair<bool, double>>>();
        auto finished = done->get_future();
        std::thread probe([done, extremes] {
            const auto t0 = Clock::now();
            bool in_range = true;
            for (const float value : extremes) {
                const std::uint16_t q = gs::game::QuantizeHeading(value);
                in_range = in_range && q <= 65535;
            }
            done->set_value({in_range, std::chrono::duration<double, std::milli>(
                                           Clock::now() - t0)
                                           .count()});
        });
        bool returned = finished.wait_for(2s) == std::future_status::ready;
        bool in_range = false;
        double elapsed_ms = 2000.0;
        if (returned) {
            probe.join();
            const auto [ok, ms] = finished.get();
            in_range = ok;
            elapsed_ms = ms;
        } else {
            probe.detach(); // spinning forever; the process exits at the end
        }
        int legacy_hangs = 0;
        for (const float value : extremes) {
            std::uint16_t legacy = 0;
            // 10M iterations ~ far beyond any finite normalization need.
            legacy_hangs += LegacyQuantizeHeading(value, legacy, 10'000'000) ? 0 : 1;
        }
        int mismatches = 0;
        for (int i = 0; i <= 200000; ++i) {
            const float angle = -4.0f * gs::game::kTwoPi +
                                8.0f * gs::game::kTwoPi * static_cast<float>(i) / 200000.0f;
            std::uint16_t legacy = 0;
            LegacyQuantizeHeading(angle, legacy, 1000);
            mismatches += gs::game::QuantizeHeading(angle) == legacy ? 0 : 1;
        }
        report("quantize-heading-nonfinite-and-huge", returned && in_range && elapsed_ms < 50.0,
               Fmt("returned=%d inputs=9 elapsed_ms=%.3f legacy_loop_nonterminating=%d/9",
                   returned ? 1 : 0, elapsed_ms, legacy_hangs));
        report("quantize-heading-equivalent-in-range", mismatches == 0,
               Fmt("samples=200001 range=[-4pi,4pi] mismatches=%d", mismatches));
    }

    std::printf("REPLV2-DONE failures=%d\n", failures);
    return failures;
}

// ============================================================================
// Protocol input hardening (H8)
// ============================================================================
namespace {

// io threads that survive an escaping handler exception and COUNT it, so the
// pre-fix behavior is observable without tearing the test process down.
// (Production io threads have no such net: an escape ends the thread, and on
// a std::thread that is std::terminate.)
class GuardedIo {
public:
    explicit GuardedIo(int threads)
    {
        for (int i = 0; i < threads; ++i) {
            threads_.emplace_back([this] { Run(); });
        }
    }
    ~GuardedIo() { StopAndJoin(); }
    void StopAndJoin()
    {
        work_.reset();
        io.stop();
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        threads_.clear();
    }
    int Escaped() const { return escaped_.load(); }
    std::string FirstEscape()
    {
        std::lock_guard lock(mutex_);
        return first_;
    }

    asio::io_context io;

private:
    void Note(const char* what)
    {
        escaped_.fetch_add(1);
        std::lock_guard lock(mutex_);
        if (first_.empty()) {
            first_ = what;
        }
    }
    void Run()
    {
        for (;;) {
            try {
                io.run();
                return;
            } catch (const std::exception& error) {
                Note(error.what());
            } catch (...) {
                Note("non-std exception");
            }
        }
    }

    asio::executor_work_guard<asio::io_context::executor_type> work_{asio::make_work_guard(io)};
    std::vector<std::thread> threads_;
    std::atomic<int> escaped_{0};
    std::mutex mutex_;
    std::string first_;
};

std::vector<std::uint8_t> CapnpWords(std::initializer_list<std::uint64_t> words)
{
    std::vector<std::uint8_t> out{0x00};
    for (const std::uint64_t word : words) {
        for (int b = 0; b < 8; ++b) {
            out.push_back(static_cast<std::uint8_t>((word >> (8 * b)) & 0xff));
        }
    }
    return out;
}

std::vector<std::uint8_t> AttackPayload(std::uint32_t target)
{
    capnp::MallocMessageBuilder msg;
    msg.initRoot<gs::protocol::Packet>().initAttackTarget().setTargetNetId(target);
    return gs::protocol::SerializeToBytes(msg);
}

std::vector<std::uint8_t> HandshakeResponsePayload()
{
    capnp::MallocMessageBuilder msg;
    auto response = msg.initRoot<gs::protocol::Packet>().initHandshakeResponse();
    response.setResult(gs::protocol::HandshakeResult::OK);
    return gs::protocol::SerializeToBytes(msg);
}

struct MalformedCase {
    const char* name;
    std::vector<std::uint8_t> bytes;
    bool raw = false; // bytes go on the wire as-is (no length header added)
};

std::vector<MalformedCase> MalformedCorpus()
{
    std::vector<MalformedCase> corpus;
    corpus.push_back({"empty-payload", {}});
    corpus.push_back({"codec-byte-only", {0x00}});
    corpus.push_back({"unaligned-body", MalformedSizePayload()});
    corpus.push_back({"garbage-segment-table", MalformedCapnpPayload()});
    // segment count - 1 = 0x7fffffff
    corpus.push_back({"huge-segment-count", CapnpWords({0x000000007fffffffULL})});
    // one segment claiming 65535 words, one present
    corpus.push_back({"segment-size-overrun", CapnpWords({0x0000ffff00000000ULL, 0})});
    // root = far pointer into segment 7 (does not exist)
    corpus.push_back({"far-pointer-missing-segment",
                      CapnpWords({0x0000000100000000ULL, 0x0000000700000002ULL})});
    // root struct pointer with offset +1,000,000 words
    corpus.push_back({"root-offset-out-of-bounds",
                      CapnpWords({0x0000000100000000ULL,
                                  0x0001000100000000ULL | (1000000ULL << 2)})});
    // root is a (byte) list pointer, not a struct
    corpus.push_back({"root-not-a-struct", CapnpWords({0x0000000100000000ULL, 0x0000000200000001ULL})});
    // root struct pointer (0 data, 1 pointer) pointing at itself
    corpus.push_back({"self-referential-root",
                      CapnpWords({0x0000000100000000ULL, 0x00010000fffffffcULL})});
    corpus.push_back({"unknown-codec", {0x07, 0, 0, 0, 0, 0, 0, 0, 0}});
    corpus.push_back({"binary-truncated", {0x01, 0x01, 0x00}});
    corpus.push_back({"binary-bad-opcode", {0x01, 0x02, 1, 0, 0, 0, 0, 0, 0}});
    corpus.push_back({"binary-bad-move-state", {0x01, 0x01, 1, 0, 0, 0, 0, 0, 0x09}});
    corpus.push_back({"server-only-packet", HandshakeResponsePayload()});
    {
        // max-size frame, zero-filled words: an empty segment table entry
        std::vector<std::uint8_t> zeros(1 + 8 * 8191, 0x00);
        corpus.push_back({"max-size-zero-words", std::move(zeros)});
    }
    // length header 65537 (> protocol limit), then a few bytes
    corpus.push_back({"oversize-length-header", {0x00, 0x01, 0x00, 0x01, 0xaa, 0xbb}, true});
    corpus.push_back({"max-u32-length-header", {0xff, 0xff, 0xff, 0xff, 0x00}, true});
    return corpus;
}

void Mutate(std::vector<std::uint8_t>& payload, std::mt19937& rng)
{
    switch (rng() % 5) {
    case 0: // bit flips (may hit the codec byte)
        for (unsigned k = 0, n = 1 + rng() % 8; k < n && !payload.empty(); ++k) {
            payload[rng() % payload.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
        }
        break;
    case 1: // random byte overwrites
        for (unsigned k = 0, n = 1 + rng() % 4; k < n && !payload.empty(); ++k) {
            payload[rng() % payload.size()] = static_cast<std::uint8_t>(rng());
        }
        break;
    case 2: // truncation
        if (!payload.empty()) {
            payload.resize(rng() % payload.size());
        }
        break;
    case 3: { // garbage extension, whole words (stays on the capnp path)
        const std::size_t extra = 8 * (1 + rng() % 16);
        for (std::size_t k = 0; k < extra; ++k) {
            payload.push_back(static_cast<std::uint8_t>(rng()));
        }
        break;
    }
    default: // smash one whole 8-byte word (pointer / segment table)
        if (payload.size() > 9) {
            const std::size_t word = 1 + 8 * (rng() % ((payload.size() - 1) / 8));
            for (std::size_t b = 0; b < 8 && word + b < payload.size(); ++b) {
                payload[word + b] = static_cast<std::uint8_t>(rng());
            }
        }
        break;
    }
}

// A <= 64 KiB Packet whose CharacterListResponse holds `elements`
// CharacterInfo structs, every `name` Text aliasing ONE shared blob: a
// traversal reads the blob once per element (Cap'n Proto amplification).
struct AmplifiedMessage {
    std::vector<std::uint8_t> payload;
    std::size_t body_words = 0;
    bool ok = false;
};

AmplifiedMessage BuildAmplifiedPacket(std::size_t elements, std::size_t blob_words)
{
    AmplifiedMessage out;
    capnp::MallocMessageBuilder msg(16384); // one segment
    msg.initRoot<gs::protocol::Packet>().initCharacterListResponse().initCharacters(
        static_cast<unsigned>(elements));
    const auto flat = capnp::messageToFlatArray(msg);
    std::vector<std::uint64_t> words(flat.size());
    std::memcpy(words.data(), flat.begin(), flat.size() * sizeof(std::uint64_t));
    if (words.empty() || (words[0] & 0xffffffffULL) != 0) {
        return out; // not a single-segment message
    }
    const std::size_t seg_size = static_cast<std::size_t>(words[0] >> 32);
    const auto seg = [&](std::size_t index) -> std::uint64_t& { return words[1 + index]; };
    const auto target_of = [&](std::size_t at) {
        const auto lower = static_cast<std::int32_t>(static_cast<std::uint32_t>(seg(at) & 0xffffffffULL));
        return static_cast<std::size_t>(static_cast<std::int64_t>(at) + 1 + (lower >> 2));
    };
    const std::size_t packet = target_of(0);
    const std::size_t union_ptr = packet + ((seg(0) >> 32) & 0xffff);
    const std::size_t response = target_of(union_ptr);
    const std::size_t response_dw = (seg(union_ptr) >> 32) & 0xffff;
    const std::size_t response_pc = (seg(union_ptr) >> 48) & 0xffff;
    std::size_t list_ptr = SIZE_MAX;
    for (std::size_t p = 0; p < response_pc; ++p) {
        const std::uint64_t word = seg(response + response_dw + p);
        if ((word & 3) == 1 && ((word >> 32) & 7) == 7) {
            list_ptr = response + response_dw + p; // the composite struct list
        }
    }
    if (list_ptr == SIZE_MAX) {
        return out;
    }
    const std::size_t tag = target_of(list_ptr);
    const std::size_t count = static_cast<std::size_t>((seg(tag) & 0xffffffffULL) >> 2);
    const std::size_t element_dw = (seg(tag) >> 32) & 0xffff;
    const std::size_t element_pc = (seg(tag) >> 48) & 0xffff;
    if (count != elements || element_pc != 1) {
        return out; // CharacterInfo has exactly one pointer: name
    }
    const std::size_t blob = seg_size;
    words.resize(1 + seg_size + blob_words, 0x4141414141414141ULL);
    words.back() = 0x0041414141414141ULL; // Text NUL terminator (last byte)
    const std::uint64_t text_bytes = blob_words * 8;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t name_ptr = tag + 1 + i * (element_dw + element_pc) + element_dw;
        const std::int64_t offset =
            static_cast<std::int64_t>(blob) - static_cast<std::int64_t>(name_ptr + 1);
        const std::uint64_t lower =
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(offset * 4)) | 1u; // list
        const std::uint64_t upper = 2u | (text_bytes << 3); // byte elements, count
        seg(name_ptr) = lower | (upper << 32);
    }
    words[0] = static_cast<std::uint64_t>(seg_size + blob_words) << 32;
    out.payload.push_back(gs::protocol::kCodecCapnp);
    const auto* raw = reinterpret_cast<const std::uint8_t*>(words.data());
    out.payload.insert(out.payload.end(), raw, raw + words.size() * sizeof(std::uint64_t));
    out.body_words = words.size();
    out.ok = true;
    return out;
}

// Full traversal of a parsed packet through the given parse entry point.
// Returns the traversed word count, or -1 when the reader refused.
long long TraverseWords(const std::vector<std::uint8_t>& payload, bool gameserver_path)
{
    try {
        auto parsed = gameserver_path ? gs::game::GameConnectionHandler::ParseClientPacket(payload)
                                      : gs::protocol::ParsePacket(payload);
        if (!parsed) {
            return -1;
        }
        return static_cast<long long>(parsed->packet.totalSize().wordCount);
    } catch (const kj::Exception&) {
        return -1;
    }
}

} // namespace

int RunProtocolHardeningScenario()
{
    int failures = 0;
    auto report = [&](const char* name, bool pass, const std::string& detail) {
        std::printf("PROTOCOL %s %s: %s\n", name, detail.c_str(), pass ? "PASS" : "FAIL");
        std::fflush(stdout);
        failures += pass ? 0 : 1;
    };

    // ---- (a) server-side oversized send ------------------------------------
    // Production sends are posted onto the io_context; the frame encoder
    // refuses > kMaxPayloadSize. That refusal must close THIS session, not
    // escape the io loop (which ends the io thread / the process).
    {
        GuardedIo gio(2);
        std::mutex mutex;
        std::shared_ptr<gs::network::Session> victim_session;
        gs::network::Server server(
            gio.io,
            tcp::endpoint(asio::ip::address_v4::loopback(), 0),
            [&](auto session, auto payload) {
                if (payload.size() == 1 && payload[0] == 0) {
                    std::lock_guard lock(mutex);
                    victim_session = session;
                } else if (payload.size() == 1 && payload[0] == 1) {
                    session->SendPayload({0x42});
                }
            },
            [](auto) {});
        server.Start();
        TestClient victim;
        victim.Connect(server.LocalPort());
        victim.SendFrame({0});
        WaitFor(3000ms, [&] {
            std::lock_guard lock(mutex);
            return victim_session != nullptr;
        });
        bool victim_closed = false;
        if (victim_session) {
            asio::post(gio.io, [session = victim_session] {
                session->SendPayload(
                    std::vector<std::uint8_t>(gs::network::Framing::kMaxPayloadSize + 1, 0x11));
            });
            victim_closed = victim.WaitClosed(2000ms);
        }
        TestClient other;
        std::vector<std::uint8_t> frame;
        const bool serving = other.Connect(server.LocalPort()) && other.SendFrame({1}) &&
                             other.ReadFrame(frame, 2000ms) == TestClient::Read::Frame &&
                             frame.size() == 1 && frame[0] == 0x42;
        const auto rejects = gs::network::GlobalSessionCounters().oversized_send_rejects.load();
        report("oversized-send-closes-only-that-session",
               gio.Escaped() == 0 && victim_closed && serving && rejects == 1,
               Fmt("io_escapes=%d first=\"%s\" victim_closed=%d io_still_serving=%d "
                   "oversized_send_rejects=%llu",
                   gio.Escaped(), gio.FirstEscape().c_str(), victim_closed ? 1 : 0,
                   serving ? 1 : 0, static_cast<unsigned long long>(rejects)));
        RunOnIo(gio.io, [&] { server.Stop(); });
    }

    // ---- (a2) the production io loop survives a throwing handler -----------
    {
        asio::io_context io;
        auto work = asio::make_work_guard(io);
        const auto escapes0 = gs::network::GlobalSessionCounters().io_loop_exceptions.load();
        std::thread runner([&] { gs::network::RunIoContext(io); });
        std::atomic<int> after{0};
        asio::post(io, [] { throw std::runtime_error("posted handler failure"); });
        asio::post(io, [] { throw 7; });
        asio::post(io, [&] { after.fetch_add(1); });
        const bool continued = WaitFor(2000ms, [&] { return after.load() == 1; });
        work.reset();
        io.stop();
        runner.join();
        const auto escapes =
            gs::network::GlobalSessionCounters().io_loop_exceptions.load() - escapes0;
        report("io-loop-survives-throwing-handler", continued && escapes == 2,
               Fmt("later_handler_ran=%d io_loop_exceptions=%llu", continued ? 1 : 0,
                   static_cast<unsigned long long>(escapes)));
    }

    // ---- (b) payload-handler exception containment -------------------------
    {
        GuardedIo gio(2);
        std::atomic<int> disconnects{0};
        gs::network::Server server(
            gio.io,
            tcp::endpoint(asio::ip::address_v4::loopback(), 0),
            [&](auto session, auto payload) {
                if (payload.size() == 1 && payload[0] == 1) {
                    throw std::runtime_error("handler failure");
                }
                if (payload.size() == 1 && payload[0] == 2) {
                    throw 42; // not a std::exception
                }
                if (payload.size() == 1 && payload[0] == 3) {
                    session->SendPayload({0x42});
                }
            },
            [&](auto) { disconnects.fetch_add(1); });
        server.Start();
        TestClient std_thrower;
        std_thrower.Connect(server.LocalPort());
        std_thrower.SendFrame({1});
        const bool std_closed = std_thrower.WaitClosed(2000ms);
        TestClient raw_thrower;
        raw_thrower.Connect(server.LocalPort());
        raw_thrower.SendFrame({2});
        const bool raw_closed = raw_thrower.WaitClosed(2000ms);
        TestClient honest;
        std::vector<std::uint8_t> frame;
        const bool serving = honest.Connect(server.LocalPort()) && honest.SendFrame({3}) &&
                             honest.ReadFrame(frame, 2000ms) == TestClient::Read::Frame;
        WaitFor(1000ms, [&] { return disconnects.load() >= 2; });
        const auto handler_exceptions =
            gs::network::GlobalSessionCounters().payload_handler_exceptions.load();
        report("handler-exceptions-close-only-that-session",
               gio.Escaped() == 0 && std_closed && raw_closed && serving &&
                   disconnects.load() == 2 && handler_exceptions == 2,
               Fmt("io_escapes=%d first=\"%s\" std_exception_closed=%d non_std_closed=%d "
                   "disconnect_callbacks=%d io_still_serving=%d payload_handler_exceptions=%llu",
                   gio.Escaped(), gio.FirstEscape().c_str(), std_closed ? 1 : 0,
                   raw_closed ? 1 : 0, disconnects.load(), serving ? 1 : 0,
                   static_cast<unsigned long long>(handler_exceptions)));
        RunOnIo(gio.io, [&] { server.Stop(); });
    }

    // ---- (c)+(d) the real gameserver handler: malformed corpus + fuzzer ----
    {
        GuardedIo gio(4);
        gs::game::WorldRuntime sim(gio.io, {},
                                   gs::game::WorldRuntime::SyntheticWorldConfig{2000.0f, 1, 1, {}});
        sim.Start();
        gs::db::DbPool db(gio.io, gs::db::DbConfig{}); // never started: EnterWorld parks
        gs::db::CharacterRepository characters(db);
        gs::db::HandoffTokenRepository tokens(db);
        gs::game::GameConnectionHandler handler(tokens, characters, sim, "127.0.0.1:0");
        gs::network::Server game_server(
            gio.io,
            tcp::endpoint(asio::ip::address_v4::loopback(), 0),
            [&handler](auto session, auto payload) { handler.OnPayload(session, std::move(payload)); },
            [&handler](auto session) { handler.OnDisconnect(session); });
        game_server.Start(); // no timeouts: every close below is the handler's decision
        const std::uint16_t port = game_server.LocalPort();
        const auto probe = [&] {
            TestClient client;
            std::vector<std::uint8_t> frame;
            const bool ok = client.Connect(port) && client.SendFrame(HandshakePayload()) &&
                            client.ReadFrame(frame, 2000ms) == TestClient::Read::Frame &&
                            IsHandshakeResponse(frame);
            client.CloseGraceful();
            return ok;
        };
        const std::size_t contexts0 = handler.ContextCount();

        const auto corpus = MalformedCorpus();
        int closed = 0;
        int attempts = 0;
        std::string not_closed;
        for (const auto& malformed : corpus) {
            for (const bool after_handshake : {false, true}) {
                ++attempts;
                TestClient client;
                if (!client.Connect(port)) {
                    continue;
                }
                if (after_handshake) {
                    std::vector<std::uint8_t> frame;
                    client.SendFrame(HandshakePayload());
                    client.ReadFrame(frame, 2000ms);
                }
                if (malformed.raw) {
                    client.SendRaw(malformed.bytes);
                } else {
                    client.SendFrame(malformed.bytes);
                }
                if (client.WaitClosed(1500ms)) {
                    ++closed;
                } else if (not_closed.size() < 200) {
                    not_closed += std::string(malformed.name) + (after_handshake ? "@est " : "@new ");
                }
                client.CloseRst();
            }
        }
        const bool corpus_drained =
            WaitFor(3000ms, [&] { return handler.ContextCount() <= contexts0; });
        const bool corpus_probe = probe();
        report("malformed-corpus-closes-session",
               closed == attempts && gio.Escaped() == 0 && corpus_drained && corpus_probe,
               Fmt("cases=%zu states=2 closed=%d/%d io_escapes=%d contexts_drained=%d "
                   "server_alive=%d%s%s",
                   corpus.size(), closed, attempts, gio.Escaped(), corpus_drained ? 1 : 0,
                   corpus_probe ? 1 : 0, not_closed.empty() ? "" : " not_closed=",
                   not_closed.c_str()));

        // Mutation fuzzer: 4 parallel clients x 300 mutated packets.
        constexpr int kThreads = 4;
        constexpr int kIterations = 300;
        std::atomic<int> sent{0};
        std::atomic<int> server_closed{0};
        const auto fuzz_start = Clock::now();
        std::vector<std::thread> fuzzers;
        for (int t = 0; t < kThreads; ++t) {
            fuzzers.emplace_back([&, t] {
                std::mt19937 rng(0xC0FFEEu + static_cast<unsigned>(t));
                const std::vector<std::vector<std::uint8_t>> bases = {
                    HandshakePayload(), EnterWorldPayload(), AttackPayload(1234), BinaryMovePayload(7)};
                for (int i = 0; i < kIterations; ++i) {
                    auto payload = bases[rng() % bases.size()];
                    Mutate(payload, rng);
                    TestClient client;
                    if (!client.Connect(port)) {
                        continue;
                    }
                    if (rng() % 2 == 0) {
                        std::vector<std::uint8_t> frame;
                        client.SendFrame(HandshakePayload());
                        client.ReadFrame(frame, 1000ms);
                    }
                    client.SendFrame(payload);
                    sent.fetch_add(1);
                    if (client.WaitClosed(40ms)) {
                        server_closed.fetch_add(1);
                    }
                    client.CloseRst();
                }
            });
        }
        for (auto& thread : fuzzers) {
            thread.join();
        }
        const double fuzz_s = std::chrono::duration<double>(Clock::now() - fuzz_start).count();
        const bool fuzz_drained =
            WaitFor(5000ms, [&] { return handler.ContextCount() <= contexts0; });
        const bool fuzz_probe = probe();
        report("mutation-fuzz-server-survives",
               gio.Escaped() == 0 && fuzz_drained && fuzz_probe && OwnerCount(sim) == 0 &&
                   sent.load() == kThreads * kIterations,
               Fmt("packets=%d closed_by_server=%d elapsed_s=%.1f io_escapes=%d first=\"%s\" "
                   "contexts_drained=%d server_alive=%d world_presence=%zu",
                   sent.load(), server_closed.load(), fuzz_s, gio.Escaped(),
                   gio.FirstEscape().c_str(), fuzz_drained ? 1 : 0, fuzz_probe ? 1 : 0,
                   OwnerCount(sim)));
        RunOnIo(gio.io, [&] { game_server.Stop(); });
        sim.Stop();
    }

    // ---- (e) Cap'n Proto traversal amplification ----------------------------
    // 1000 CharacterInfo names aliasing one 3000-word blob in a ~64 KiB frame.
    // No current C2S handler iterates a list, so this is the reader contract:
    // the gameserver's parse path must bound traversal by the message size,
    // while ordinary packets still traverse fine.
    {
        const auto amplified = BuildAmplifiedPacket(1000, 3000);
        const bool fits = amplified.ok &&
                          amplified.payload.size() <= gs::network::Framing::kMaxPayloadSize;
        const long long library_default = TraverseWords(amplified.payload, false);
        const long long gameserver = TraverseWords(amplified.payload, true);
        const double ratio = amplified.body_words > 0 && library_default > 0
                                 ? static_cast<double>(library_default) /
                                       static_cast<double>(amplified.body_words)
                                 : 0.0;
        bool legit_ok = true;
        for (const auto& legit : {HandshakePayload(), EnterWorldPayload(), AttackPayload(9)}) {
            legit_ok = legit_ok && TraverseWords(legit, true) > 0;
        }
        {
            // a legitimately large packet: a ~60 KiB clientBuild string
            capnp::MallocMessageBuilder msg;
            auto request = msg.initRoot<gs::protocol::Packet>().initHandshakeRequest();
            request.setProtocolVersion(gs::protocol::kProtocolVersion);
            request.setClientBuild(std::string(60000, 'b'));
            legit_ok = legit_ok && TraverseWords(gs::protocol::SerializeToBytes(msg), true) > 0;
        }
        report("traversal-amplification-bounded",
               fits && gameserver < 0 && legit_ok,
               Fmt("frame_bytes=%zu body_words=%zu library_default_traversed_words=%lld "
                   "amplification=%.0fx gameserver_path=%s legit_packets_ok=%d",
                   amplified.payload.size(), amplified.body_words, library_default, ratio,
                   gameserver < 0 ? "refused" : "ACCEPTED", legit_ok ? 1 : 0));
    }

    // ---- (f) non-finite numeric input at the world boundary ----------------
    {
        IoRunner runner;
        gs::game::WorldRuntime sim(runner.io, {},
                                   gs::game::WorldRuntime::SyntheticWorldConfig{2000.0f, 1, 1, {}});
        sim.Start();
        ObservedSession viewer = MakeObservedSession(runner.io, 60000);
        ReplWire wire;
        std::atomic<bool> stop{false};
        std::thread reader([&] { DecodeReplFrames(*viewer.client, wire, stop); });
        sim.PostSpawn(viewer.session, MakeCharacter(200), gs::game::DebugSpawnOverride{1000.0f, 1000.0f});
        WaitFor(5000ms, [&] {
            std::lock_guard lock(wire.mutex);
            return wire.frames > 5;
        });
        const float kInf = std::numeric_limits<float>::infinity();
        const float kNaN = std::numeric_limits<float>::quiet_NaN();
        std::uint32_t seq = 0;
        for (const float heading : {kNaN, kInf, -kInf, 3.0e38f}) {
            for (int i = 0; i < 10; ++i) {
                sim.PostMoveInput(60000, ++seq, heading, gs::game::MoveState::Running);
                std::this_thread::sleep_for(20ms);
            }
        }
        // then a normal move: the player must still respond
        float x0 = 0.0f;
        float y0 = 0.0f;
        {
            std::lock_guard lock(wire.mutex);
            x0 = wire.self_x;
            y0 = wire.self_y;
        }
        for (int i = 0; i < 25; ++i) {
            sim.PostMoveInput(60000, ++seq, 0.0f, gs::game::MoveState::Running);
            std::this_thread::sleep_for(20ms);
        }
        std::this_thread::sleep_for(200ms);
        // Spawn overrides that are not finite / out of the world: fall back.
        std::uint64_t index = 201;
        for (const auto& spawn :
             {gs::game::DebugSpawnOverride{kNaN, 1000.0f}, gs::game::DebugSpawnOverride{kInf, 5.0f},
              gs::game::DebugSpawnOverride{1.0e30f, -1.0e30f},
              gs::game::DebugSpawnOverride{-kInf, kNaN}}) {
            sim.PostSpawn(MakeDetachedSession(runner.io, 60000 + index), MakeCharacter(index), spawn);
            ++index;
        }
        const bool spawned = WaitFor(5000ms, [&] { return sim.PresenceStats().claims >= 5; });
        const std::string audit = AuditNow(sim);
        std::uint64_t nonfinite = 0;
        float x1 = 0.0f;
        float y1 = 0.0f;
        {
            std::lock_guard lock(wire.mutex);
            nonfinite = wire.self_nonfinite_frames;
            x1 = wire.self_x;
            y1 = wire.self_y;
        }
        stop = true;
        reader.join();
        sim.Stop();
        const bool moved = std::isfinite(x1) && std::isfinite(y1) && (x1 != x0 || y1 != y0);
        const auto dropped = sim.InputStats().moves_dropped_invalid;
        report("nonfinite-numeric-input-contained",
               nonfinite == 0 && moved && spawned && audit == "OK" && dropped == 30,
               Fmt("nonfinite_self_frames=%llu still_moves_after=%d pos=(%.1f,%.1f)->(%.1f,%.1f) "
                   "nonfinite_moves_dropped=%llu/30 bad_spawn_overrides_claimed=%d audit=%s",
                   static_cast<unsigned long long>(nonfinite), moved ? 1 : 0, x0, y0, x1, y1,
                   static_cast<unsigned long long>(dropped), spawned ? 1 : 0,
                   audit.substr(0, 120).c_str()));
    }

    std::printf("PROTOCOL-DONE failures=%d\n", failures);
    return failures;
}

// ============================================================================
// Retired zone reclamation (H9)
// ============================================================================
int RunZoneReclamationScenario(int cycles)
{
    int failures = 0;
    auto report = [&](const char* name, bool pass, const std::string& detail) {
        std::printf("RECLAIM %s %s: %s\n", name, detail.c_str(), pass ? "PASS" : "FAIL");
        std::fflush(stdout);
        failures += pass ? 0 : 1;
    };
    cycles = std::max(cycles, 10);

    // Two root zones side by side: zone A (west) is split and merged over and
    // over; zone B (east) is its live neighbor, so B's ghost cursors point at
    // A's slots while they retire and get reused (the slot-reuse ABA case).
    IoRunner runner;
    gs::game::WorldRuntime sim(runner.io, {},
                               gs::game::WorldRuntime::SyntheticWorldConfig{4000.0f, 2, 1, {}});
    gs::game::PartitionConfig partition;
    partition.scoring.adaptive_enabled = false; // topology changes are forced only
    sim.ConfigurePartition(partition);
    gs::game::MobSpawnPoint point;
    point.mob_type_id = 2;
    point.x = 2000.0f; // straddles the A|B border: ghosts in both directions
    point.y = 2000.0f;
    point.count = 400;
    point.radius = 1200.0f;
    sim.AddMobSpawnPoint(point);
    sim.SpawnConfiguredMobsNow();
    sim.PostSpawn(MakeDetachedSession(runner.io, 70000), MakeCharacter(300),
                  gs::game::DebugSpawnOverride{1000.0f, 2000.0f});
    sim.Start();
    WaitFor(5000ms, [&] { return sim.PresenceStats().claims >= 1; });
    std::this_thread::sleep_for(500ms);

    // The leaf covering zone A's area (the west half) at this moment.
    const auto west_leaf = [&]() -> gs::game::ZoneId {
        return ReadWorld(sim, [](const WorldSnapshot& snap) -> gs::game::ZoneId {
            for (const auto* leaf : snap.zones.GetActiveLeaves()) {
                if (leaf->bounds.min_x < 1000.0f && leaf->bounds.max_x > 1000.0f &&
                    leaf->bounds.min_y < 2000.0f && leaf->bounds.max_y > 2000.0f) {
                    return leaf->zone_id;
                }
            }
            return 0;
        });
    };
    const auto ghost_audit = [&] {
        sim.RequestGhostValidation();
        std::string result;
        return WaitFor(5000ms, [&] { return sim.TryTakeGhostValidationResult(result); }) ? result
                                                                                        : "TIMEOUT";
    };

    struct Sample {
        int cycle = 0;
        std::size_t slots = 0;
        std::size_t retired = 0;
        std::size_t active = 0;
        double ws_mb = 0.0;
        double cycle_ms = 0.0;
        std::string audit;
        std::string ghost;
        gs::game::ZoneManager::ReclaimStats reclaim;
    };
    const auto sample = [&](int cycle, double cycle_ms) {
        Sample s;
        s.cycle = cycle;
        s.cycle_ms = cycle_ms;
        s.audit = AuditNow(sim); // includes the H9 reclamation invariants
        s.ghost = ghost_audit();
        // Slot count, retired slots, active leaves, reclaim counters and the
        // working set: one snapshot, one mutation point (MAP-0).
        struct TableView {
            std::size_t slots = 0;
            std::size_t retired = 0;
            std::size_t active = 0;
            std::size_t ws_bytes = 0;
            gs::game::ZoneManager::ReclaimStats reclaim;
        };
        const TableView view = ReadWorld(sim, [](const WorldSnapshot& snap) {
            TableView v;
            v.slots = snap.zones.ZoneCount();
            for (std::size_t i = 0; i < v.slots; ++i) {
                if (snap.zones.GetZone(i).Partition() == gs::game::PartitionState::Retired) {
                    ++v.retired;
                }
            }
            v.active = snap.zones.GetActiveLeaves().size();
            v.ws_bytes = ProcessWorkingSetBytes();
            v.reclaim = snap.reclaim;
            return v;
        });
        s.slots = view.slots;
        s.retired = view.retired;
        s.active = view.active;
        s.ws_mb = static_cast<double>(view.ws_bytes) / (1024.0 * 1024.0);
        s.reclaim = view.reclaim;
        std::printf("RECLAIM cycle=%d zone_slots=%zu retired=%zu active_leaves=%zu "
                    "working_set_mb=%.1f avg_cycle_ms=%.1f reclaimed=%llu reused=%llu trimmed=%llu "
                    "pending=%zu reusable=%zu audit=%s ghost_audit=%s\n",
                    s.cycle, s.slots, s.retired, s.active, s.ws_mb, s.cycle_ms,
                    static_cast<unsigned long long>(s.reclaim.reclaimed_total),
                    static_cast<unsigned long long>(s.reclaim.reused_total),
                    static_cast<unsigned long long>(s.reclaim.trimmed_total),
                    s.reclaim.retired_pending, s.reclaim.reusable, s.audit.substr(0, 100).c_str(),
                    s.ghost.substr(0, 100).c_str());
        std::fflush(stdout);
        return s;
    };

    std::vector<Sample> samples;
    samples.push_back(sample(0, 0.0));
    const std::vector<int> checkpoints = {10, 50, 100, 250, 500, 1000, 2000, 5000};
    int completed = 0;
    bool stuck = false;
    auto window_start = Clock::now();
    int window_cycles = 0;
    for (int cycle = 1; cycle <= cycles; ++cycle) {
        const gs::game::ZoneId leaf = west_leaf();
        const auto p0 = sim.PartitionMetricsSnapshot();
        sim.PostForceSplit(leaf);
        const bool split = WaitFor(5000ms, [&] {
            return sim.PartitionMetricsSnapshot().split_commits > p0.split_commits;
        });
        if (split) {
            sim.PostForceMerge(leaf); // the node keeps the parent's id until the merge
        }
        if (!split || !WaitFor(5000ms, [&] {
                return sim.PartitionMetricsSnapshot().merge_commits > p0.merge_commits;
            })) {
            const auto p1 = sim.PartitionMetricsSnapshot();
            std::printf("RECLAIM stuck: cycle=%d leaf=%u split=%d split_attempts=%llu split_aborts=%llu "
                        "merge_attempts=%llu merge_aborts=%llu\n",
                        cycle, static_cast<unsigned>(leaf), split ? 1 : 0,
                        static_cast<unsigned long long>(p1.split_attempts - p0.split_attempts),
                        static_cast<unsigned long long>(p1.split_aborts - p0.split_aborts),
                        static_cast<unsigned long long>(p1.merge_attempts - p0.merge_attempts),
                        static_cast<unsigned long long>(p1.merge_aborts - p0.merge_aborts));
            stuck = true;
            break;
        }
        completed = cycle;
        ++window_cycles;
        if (std::find(checkpoints.begin(), checkpoints.end(), cycle) != checkpoints.end() ||
            cycle == cycles) {
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - window_start)
                                  .count() /
                              std::max(window_cycles, 1);
            samples.push_back(sample(cycle, ms));
            window_start = Clock::now();
            window_cycles = 0;
        }
    }
    const auto presence = sim.PresenceStats();
    sim.Stop();

    const Sample& first = samples[1 < samples.size() ? 1 : 0]; // cycle 10: warm allocator
    const Sample& last = samples.back();
    bool audits_ok = true;
    std::string first_bad;
    for (const auto& s : samples) {
        const bool ok = s.audit == "OK" && s.ghost == "OK";
        if (!ok && first_bad.empty()) {
            first_bad = "cycle " + std::to_string(s.cycle) + ": " + s.audit.substr(0, 120) + " / " +
                        s.ghost.substr(0, 120);
        }
        audits_ok = audits_ok && ok;
    }
    report("split-merge-cycles-consistent",
           !stuck && completed == cycles && audits_ok && presence.present == 1,
           Fmt("cycles=%d/%d world+reclaim+ghost_audits_ok=%d present=%llu%s%s", completed, cycles,
               audits_ok ? 1 : 0, static_cast<unsigned long long>(presence.present),
               first_bad.empty() ? "" : " first_failure=", first_bad.c_str()));
    // Bounded: the zone table must not grow with the number of topology
    // changes (live leaves + a short reclaim backlog), slots really get
    // reused, and memory from cycle 10 on stays flat within allocator noise.
    const double growth_mb = last.ws_mb - first.ws_mb;
    // Bounded at every checkpoint, independent of the cycle count: live
    // leaves + the reclaim backlog (forced cycles every ~20-40 ms against the
    // 2-tick grace keep a few cycles in flight -- far faster than any real
    // ASF cadence), with the vacant tail trimmed back.
    std::size_t max_slots = 0;
    for (const auto& s : samples) {
        max_slots = std::max(max_slots, s.slots);
    }
    report("zone-slots-bounded",
           max_slots <= 32 && last.reclaim.reused_total > 0 && last.reclaim.trimmed_total > 0,
           Fmt("slots_at_cycle_%d=%zu max_slots_any_checkpoint=%zu retired=%zu active=%zu "
               "reused=%llu trimmed=%llu (append-only would be 2+5*cycles=%d)",
               last.cycle, last.slots, max_slots, last.retired, last.active,
               static_cast<unsigned long long>(last.reclaim.reused_total),
               static_cast<unsigned long long>(last.reclaim.trimmed_total), 2 + 5 * last.cycle));
    report("memory-flat-over-cycles", growth_mb < 32.0,
           Fmt("working_set_mb cycle_%d=%.1f cycle_%d=%.1f growth=%.1f per_cycle_kb=%.1f "
               "cycle_ms %.1f->%.1f",
               first.cycle, first.ws_mb, last.cycle, last.ws_mb, growth_mb,
               last.cycle > first.cycle ? growth_mb * 1024.0 / (last.cycle - first.cycle) : 0.0,
               first.cycle_ms, last.cycle_ms));
    std::printf("RECLAIM-DONE failures=%d\n", failures);
    return failures;
}

// ============================================================================
// MAP-0: bench snapshot consistency under concurrent topology mutation
// ============================================================================
namespace {

// What one snapshot collector checks and copies out. Every invariant below is
// exact at a quiescent supervisor point; a raw concurrent read could not
// promise any of them (and could touch a slot being reclaimed/reused).
struct SnapshotProbe {
    std::uint64_t epoch = 0;
    std::uint32_t world_tick = 0;
    std::size_t slots = 0;
    std::size_t leaves = 0;
    std::size_t retired = 0;
    std::size_t owners = 0;
    std::size_t entities = 0;       // authoritative residents, all slots
    std::size_t bound_players = 0;  // player bindings, all slots
    std::uint64_t leaf_hash = 0;    // topology fingerprint (sorted leaf ids)
    std::uint64_t reclaimed_total = 0;
    std::uint64_t reused_total = 0;
    std::string violation;          // first violated invariant, empty = OK
};

SnapshotProbe ProbeWorld(const WorldSnapshot& snap, double world_area)
{
    SnapshotProbe p;
    p.epoch = snap.epoch;
    p.world_tick = snap.world_tick;
    p.slots = snap.zones.ZoneCount();
    p.owners = snap.owners.size();
    p.reclaimed_total = snap.reclaim.reclaimed_total;
    p.reused_total = snap.reclaim.reused_total;
    auto fail = [&p](std::string what) {
        if (p.violation.empty()) {
            p.violation = std::move(what);
        }
    };
    std::unordered_map<std::uint32_t, gs::game::ZoneId> authority;
    for (std::size_t i = 0; i < p.slots; ++i) {
        const auto& zone = snap.zones.GetZone(i);
        if (zone.TickInProgress().load(std::memory_order_acquire)) {
            fail(Fmt("tick in flight zone=%u slot=%zu", zone.Id(), i));
        }
        if (zone.Partition() == gs::game::PartitionState::Staging) {
            fail(Fmt("staged destination visible zone=%u", zone.Id()));
        }
        if (zone.Partition() == gs::game::PartitionState::Retired) {
            ++p.retired;
            if (!zone.Entities().empty() || !zone.Players().empty()) {
                fail(Fmt("retired zone=%u holds %zu entities / %zu players", zone.Id(),
                         zone.Entities().size(), zone.Players().size()));
            }
        }
        p.entities += zone.Entities().size();
        p.bound_players += zone.Players().size();
        for (const auto& [net_id, entity] : zone.Entities()) {
            (void)entity;
            const auto [it, inserted] = authority.emplace(net_id, zone.Id());
            if (!inserted) {
                fail(Fmt("net=%u authoritative in zones %u and %u", net_id, it->second, zone.Id()));
            }
        }
    }
    // Active leaves: live, simulating Leaf zones tiling the world exactly.
    std::vector<gs::game::ZoneId> leaf_ids;
    double area = 0.0;
    for (const auto* leaf : snap.zones.GetActiveLeaves()) {
        leaf_ids.push_back(leaf->zone_id);
        area += static_cast<double>(leaf->bounds.max_x - leaf->bounds.min_x) *
                static_cast<double>(leaf->bounds.max_y - leaf->bounds.min_y);
        const std::size_t index = snap.zones.FindIndexById(leaf->zone_id);
        if (index >= p.slots) {
            fail(Fmt("leaf zone=%u has no slot", leaf->zone_id));
            continue;
        }
        const auto& zone = snap.zones.GetZone(index);
        if (zone.Partition() != gs::game::PartitionState::Leaf || !zone.SimulationEnabled()) {
            fail(Fmt("leaf zone=%u partition=%u sim=%d", leaf->zone_id,
                     static_cast<unsigned>(zone.Partition()), zone.SimulationEnabled() ? 1 : 0));
        }
    }
    p.leaves = leaf_ids.size();
    if (std::abs(area - world_area) > world_area * 1e-6) {
        fail(Fmt("leaves cover %.0f m2 of %.0f m2", area, world_area));
    }
    std::sort(leaf_ids.begin(), leaf_ids.end());
    if (std::adjacent_find(leaf_ids.begin(), leaf_ids.end()) != leaf_ids.end()) {
        fail("duplicate active leaf id");
    }
    std::uint64_t h = 1469598103934665603ull;
    for (const auto id : leaf_ids) {
        h = (h ^ id) * 1099511628211ull;
    }
    p.leaf_hash = h;
    // Owner fast-path caches resolve to the live authoritative entity.
    for (const auto& [session, owner] : snap.owners) {
        if (owner.zone_index >= p.slots) {
            fail(Fmt("owner session=%llu slot %zu out of range", (unsigned long long)session,
                     owner.zone_index));
            continue;
        }
        const auto& zone = snap.zones.GetZone(owner.zone_index);
        if (zone.Id() != owner.location.zone || !zone.FindEntity(owner.net_id).is_valid()) {
            fail(Fmt("owner session=%llu slot=%zu zone=%u location=%u net=%u unresolved",
                     (unsigned long long)session, owner.zone_index, zone.Id(),
                     owner.location.zone, owner.net_id));
        }
    }
    if (p.bound_players != p.owners) {
        fail(Fmt("player bindings %zu != owners %zu", p.bound_players, p.owners));
    }
    return p;
}

} // namespace

int RunSnapshotConsistencyScenario(int cycles)
{
    int failures = 0;
    auto report = [&](const char* name, bool pass, const std::string& detail) {
        std::printf("SNAPSHOT %s %s: %s\n", name, detail.c_str(), pass ? "PASS" : "FAIL");
        std::fflush(stdout);
        failures += pass ? 0 : 1;
    };
    cycles = std::max(cycles, 10);
    constexpr float kExtent = 4000.0f;
    constexpr int kReaders = 3;
    constexpr int kPlayers = 4;
    constexpr int kMobs = 400;
    const double world_area = static_cast<double>(kExtent) * kExtent;

    IoRunner runner;
    gs::game::WorldRuntime sim(runner.io, {},
                               gs::game::WorldRuntime::SyntheticWorldConfig{kExtent, 2, 1, {}});
    gs::game::PartitionConfig partition;
    partition.scoring.adaptive_enabled = false; // topology changes are forced only
    sim.ConfigurePartition(partition);

    // Before Start nothing mutates the world: the capture runs inline.
    const SnapshotProbe before_start =
        ReadWorld(sim, [&](const WorldSnapshot& snap) { return ProbeWorld(snap, world_area); });
    report("inline-before-start", before_start.violation.empty() && before_start.leaves == 2,
           Fmt("epoch=%llu slots=%zu leaves=%zu violation=\"%s\"",
               (unsigned long long)before_start.epoch, before_start.slots, before_start.leaves,
               before_start.violation.c_str()));

    gs::game::MobSpawnPoint point;
    point.mob_type_id = 2;
    point.x = 2000.0f; // straddles the A|B border: migrations + ghosts both ways
    point.y = 2000.0f;
    point.count = kMobs;
    point.radius = 1200.0f;
    sim.AddMobSpawnPoint(point);
    sim.SpawnConfiguredMobsNow();
    // Two players sit 6 m from the A|B border and run 12 m back and forth over
    // it in opposite directions (> the 5 m migration hysteresis each way).
    const float player_x[kPlayers] = {900.0f, 1994.0f, 2006.0f, 3100.0f};
    for (int i = 0; i < kPlayers; ++i) {
        sim.PostSpawn(MakeDetachedSession(runner.io, 80000 + i), MakeCharacter(400 + i),
                      gs::game::DebugSpawnOverride{player_x[i], 2000.0f});
    }
    sim.Start();
    WaitFor(10000ms, [&] { return OwnerCount(sim) == kPlayers; });
    std::this_thread::sleep_for(500ms);
    const SnapshotProbe baseline =
        ReadWorld(sim, [&](const WorldSnapshot& snap) { return ProbeWorld(snap, world_area); });
    report("baseline", baseline.violation.empty() && baseline.owners == kPlayers &&
                           baseline.entities == static_cast<std::size_t>(kPlayers + kMobs),
           Fmt("epoch=%llu tick=%u slots=%zu leaves=%zu owners=%zu entities=%zu violation=\"%s\"",
               (unsigned long long)baseline.epoch, baseline.world_tick, baseline.slots,
               baseline.leaves, baseline.owners, baseline.entities, baseline.violation.c_str()));

    // Readers: back-to-back snapshots for the whole churn, each checked
    // against the invariants and against the reader's previous snapshot.
    struct ReaderStats {
        std::uint64_t snapshots = 0;
        std::uint64_t topology_changes = 0; // leaf set differs from previous
        std::uint64_t max_wait_us = 0;
        std::size_t max_slots = 0;
        std::string first_violation;
    };
    std::atomic<bool> churning{true};
    std::vector<ReaderStats> reader_stats(kReaders);
    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&, r] {
            ReaderStats& st = reader_stats[static_cast<std::size_t>(r)];
            SnapshotProbe prev = baseline;
            while (churning.load(std::memory_order_acquire)) {
                const auto t0 = Clock::now();
                const SnapshotProbe p = ReadWorld(
                    sim, [&](const WorldSnapshot& snap) { return ProbeWorld(snap, world_area); });
                const auto waited = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0)
                        .count());
                st.max_wait_us = std::max(st.max_wait_us, waited);
                st.max_slots = std::max(st.max_slots, p.slots);
                ++st.snapshots;
                std::string v = p.violation;
                if (v.empty() && p.epoch <= prev.epoch) {
                    v = Fmt("epoch %llu after %llu", (unsigned long long)p.epoch,
                            (unsigned long long)prev.epoch);
                }
                if (v.empty() && p.world_tick < prev.world_tick) {
                    v = Fmt("world tick %u after %u", p.world_tick, prev.world_tick);
                }
                if (v.empty() && (p.owners != baseline.owners || p.entities != baseline.entities)) {
                    v = Fmt("population owners=%zu entities=%zu (baseline %zu/%zu)", p.owners,
                            p.entities, baseline.owners, baseline.entities);
                }
                if (v.empty() && (p.reclaimed_total < prev.reclaimed_total ||
                                  p.reused_total < prev.reused_total)) {
                    v = "reclaim counters went backwards";
                }
                if (!v.empty() && st.first_violation.empty()) {
                    st.first_violation = Fmt("epoch=%llu: %s", (unsigned long long)p.epoch,
                                             v.c_str());
                }
                st.topology_changes += p.leaf_hash != prev.leaf_hash ? 1u : 0u;
                prev = p;
            }
        });
    }
    // Movers: players walk back and forth across the A|B border.
    std::thread mover([&] {
        std::uint32_t seq = 0;
        int step = 0;
        while (churning.load(std::memory_order_acquire)) {
            const float heading = (step / 40) % 2 == 0 ? 1.5707963f : -1.5707963f;
            for (int i = 0; i < kPlayers; ++i) {
                sim.PostMoveInput(static_cast<gs::common::SessionId>(80000 + i), ++seq,
                                  i % 2 == 0 ? -heading : heading,
                                  gs::game::MoveState::Running);
            }
            ++step;
            std::this_thread::sleep_for(50ms);
        }
    });

    // Churn: forced split -> merge of the west leaf, every 3rd cycle also the
    // east leaf (reclaim + slot reuse on both sides of the border).
    const auto leaf_covering = [&](float x, float y) -> gs::game::ZoneId {
        return ReadWorld(sim, [x, y](const WorldSnapshot& snap) -> gs::game::ZoneId {
            for (const auto* leaf : snap.zones.GetActiveLeaves()) {
                if (leaf->bounds.min_x <= x && x < leaf->bounds.max_x && leaf->bounds.min_y <= y &&
                    y < leaf->bounds.max_y) {
                    return leaf->zone_id;
                }
            }
            return 0;
        });
    };
    const auto p_start = sim.PartitionMetricsSnapshot();
    const auto t_churn = Clock::now();
    int completed = 0;
    bool stuck = false;
    for (int cycle = 1; cycle <= cycles && !stuck; ++cycle) {
        std::vector<gs::game::ZoneId> roots = {leaf_covering(1000.0f, 2000.0f)};
        if (cycle % 3 == 0) {
            roots.push_back(leaf_covering(3000.0f, 2000.0f));
        }
        for (const auto root : roots) {
            if (root == 0) {
                stuck = true;
                break;
            }
            const auto p0 = sim.PartitionMetricsSnapshot();
            sim.PostForceSplit(root);
            const bool split = WaitFor(5000ms, [&] {
                return sim.PartitionMetricsSnapshot().split_commits > p0.split_commits;
            });
            if (split) {
                sim.PostForceMerge(root);
            }
            if (!split || !WaitFor(5000ms, [&] {
                    return sim.PartitionMetricsSnapshot().merge_commits > p0.merge_commits;
                })) {
                std::printf("SNAPSHOT stuck: cycle=%d root=%u split=%d\n", cycle,
                            static_cast<unsigned>(root), split ? 1 : 0);
                stuck = true;
                break;
            }
        }
        if (!stuck) {
            completed = cycle;
        }
    }
    const double churn_s = std::chrono::duration<double>(Clock::now() - t_churn).count();
    churning.store(false, std::memory_order_release);
    mover.join();
    for (auto& reader : readers) {
        reader.join();
    }
    const auto p_end = sim.PartitionMetricsSnapshot();
    const auto mig = sim.MigrationMetrics();

    ReaderStats total;
    for (const auto& st : reader_stats) {
        total.snapshots += st.snapshots;
        total.topology_changes += st.topology_changes;
        total.max_wait_us = std::max(total.max_wait_us, st.max_wait_us);
        total.max_slots = std::max(total.max_slots, st.max_slots);
        if (total.first_violation.empty()) {
            total.first_violation = st.first_violation;
        }
    }
    const auto stats = sim.GetSnapshotStats();
    const SnapshotProbe after =
        ReadWorld(sim, [&](const WorldSnapshot& snap) { return ProbeWorld(snap, world_area); });
    report("churn-completed", !stuck && completed == cycles,
           Fmt("cycles=%d/%d splits=%llu merges=%llu migrations=%llu mig_stale=%llu "
               "mig_retry=%llu churn_s=%.1f",
               completed,
               cycles, (unsigned long long)(p_end.split_commits - p_start.split_commits),
               (unsigned long long)(p_end.merge_commits - p_start.merge_commits),
               (unsigned long long)mig.committed, (unsigned long long)mig.dropped_stale,
               (unsigned long long)mig.retries, churn_s));
    // Coverage: the snapshots must actually have interleaved with the churn
    // (topology observed changing between consecutive captures, slots
    // reclaimed and reused while readers were running).
    report("snapshots-interleaved-with-mutation",
           total.snapshots >= 100 && total.topology_changes >= static_cast<std::uint64_t>(cycles) &&
               after.reused_total > baseline.reused_total,
           Fmt("snapshots=%llu topology_changes_seen=%llu reclaimed=%llu->%llu reused=%llu->%llu "
               "max_slots=%zu",
               (unsigned long long)total.snapshots, (unsigned long long)total.topology_changes,
               (unsigned long long)baseline.reclaimed_total,
               (unsigned long long)after.reclaimed_total,
               (unsigned long long)baseline.reused_total, (unsigned long long)after.reused_total,
               total.max_slots));
    report("snapshot-invariants", total.first_violation.empty() && after.violation.empty(),
           Fmt("readers=%d first_violation=\"%s\" final=\"%s\"", kReaders,
               total.first_violation.c_str(), after.violation.c_str()));
    report("snapshot-latency", total.max_wait_us < 1000000,
           Fmt("reader_max_wait_ms=%.1f runtime: captures=%llu requests=%llu "
               "max_request_to_capture_ms=%.1f supervisor_avg_ms=%.3f",
               total.max_wait_us / 1000.0, (unsigned long long)stats.captures,
               (unsigned long long)stats.requests, stats.max_wait_us / 1000.0,
               sim.SupervisorAvgMs()));

    // A collector runs on the supervisor: it can neither wait for a snapshot
    // there (it would wait for itself) nor request a nested one.
    const auto guards = ReadWorld(sim, [&sim](const WorldSnapshot&) {
        std::pair<bool, bool> refused{false, false};
        std::promise<int> ready;
        ready.set_value(1);
        auto future = ready.get_future();
        int out = 0;
        try {
            sim.WaitSnapshot(future, std::chrono::milliseconds(0), out);
        } catch (const std::logic_error&) {
            refused.first = true;
        }
        try {
            auto nested = sim.CaptureSnapshot<int>([](const WorldSnapshot&) { return 0; });
            (void)nested;
        } catch (const std::logic_error&) {
            refused.second = true;
        }
        return refused;
    });
    report("supervisor-self-wait-refused", guards.first && guards.second,
           Fmt("wait_on_supervisor_refused=%d nested_request_refused=%d", guards.first ? 1 : 0,
               guards.second ? 1 : 0));

    // A request still queued when Stop() arrives is answered by the final
    // quiescent drain (never left hanging); afterwards captures run inline.
    auto pending = sim.CaptureSnapshot<std::size_t>(
        [](const WorldSnapshot& snap) { return snap.zones.ZoneCount(); });
    sim.Stop();
    const bool answered = pending.wait_for(0ms) == std::future_status::ready;
    const std::size_t after_stop_slots =
        ReadWorld(sim, [](const WorldSnapshot& snap) { return snap.zones.ZoneCount(); });
    report("shutdown-drain", answered && after_stop_slots == 0,
           Fmt("queued_request_answered=%d slots_after_stop=%zu", answered ? 1 : 0,
               after_stop_slots));
    std::printf("SNAPSHOT-DONE failures=%d\n", failures);
    return failures;
}

// ============================================================================
// Low-level hygiene (H10)
// ============================================================================
int RunHygieneScenario()
{
    int failures = 0;
    auto report = [&](const char* name, bool pass, const std::string& detail) {
        std::printf("HYGIENE %s %s: %s\n", name, detail.c_str(), pass ? "PASS" : "FAIL");
        std::fflush(stdout);
        failures += pass ? 0 : 1;
    };

    // ---- (a) ZoneWorkerPool::Stop lost wakeup ------------------------------
    // Start then immediately Stop: workers are entering their wait exactly
    // while Stop publishes `stopping_`. A Stop whose store+notify can slip
    // between a worker's predicate check and its sleep leaves that worker
    // asleep forever and the join hangs. Runs on its own thread with a
    // watchdog (a hung join cannot be recovered; the mode _Exits after).
    {
        constexpr int kCycles = 20000;
        std::atomic<int> done{0};
        std::atomic<bool> finished{false};
        std::thread stress([&] {
            for (int i = 0; i < kCycles; ++i) {
                gs::game::ZoneWorkerPool pool([](std::size_t) {});
                pool.Start(8);
                pool.Stop();
                done.fetch_add(1, std::memory_order_relaxed);
            }
            finished.store(true);
        });
        int last = -1;
        auto last_progress = Clock::now();
        bool hung = false;
        while (!finished.load()) {
            std::this_thread::sleep_for(100ms);
            const int now_done = done.load();
            if (now_done != last) {
                last = now_done;
                last_progress = Clock::now();
            } else if (Clock::now() - last_progress > 5s) {
                hung = true;
                break;
            }
        }
        if (hung) {
            stress.detach(); // stuck in join(): unrecoverable by design of the bug
        } else {
            stress.join();
        }
        report("worker-pool-stop-no-lost-wakeup", !hung,
               Fmt("start_stop_cycles=%d/%d workers=8 hung=%d", done.load(), kCycles, hung ? 1 : 0));
    }

    // ---- (b) TCP_NODELAY on accepted sessions ------------------------------
    // Two small frames 2 ms apart, the client never sends (no piggybacked
    // ACK): with Nagle on, the second frame waits for the first one's ACK.
    {
        IoPool pool(2);
        std::mutex mutex;
        std::shared_ptr<gs::network::Session> session;
        gs::network::Server server(
            pool.io,
            tcp::endpoint(asio::ip::address_v4::loopback(), 0),
            [&](auto s, auto) {
                std::lock_guard lock(mutex);
                session = s;
            },
            [](auto) {});
        server.Start();
        TestClient client;
        client.Connect(server.LocalPort());
        client.SendFrame({0});
        WaitFor(3000ms, [&] {
            std::lock_guard lock(mutex);
            return session != nullptr;
        });
        std::vector<double> gaps_ms;
        if (session) {
            std::vector<std::uint8_t> frame;
            for (int round = 0; round < 30; ++round) {
                session->SendPayload({0x01, 0x02, 0x03});
                PreciseSleepUntil(Clock::now() + 2ms);
                session->SendPayload({0x04, 0x05, 0x06});
                if (client.ReadFrame(frame, 2000ms) != TestClient::Read::Frame) {
                    break;
                }
                const auto first = Clock::now();
                if (client.ReadFrame(frame, 2000ms) != TestClient::Read::Frame) {
                    break;
                }
                gaps_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - first)
                                      .count());
                std::this_thread::sleep_for(50ms);
            }
        }
        std::sort(gaps_ms.begin(), gaps_ms.end());
        const double p50 = gaps_ms.empty() ? -1.0 : gaps_ms[gaps_ms.size() / 2];
        const double worst = gaps_ms.empty() ? -1.0 : gaps_ms.back();
        const bool nodelay = session && session->NoDelay();
        report("accepted-sessions-tcp-nodelay",
               nodelay && gaps_ms.size() == 30 && worst < 20.0,
               Fmt("socket_no_delay=%d rounds=%zu second_frame_gap_ms p50=%.2f max=%.2f "
                   "(sent 2 ms apart)",
                   nodelay ? 1 : 0, gaps_ms.size(), p50, worst));
        if (session) {
            RunOnIo(pool.io, [&] { session->Stop(); });
        }
        RunOnIo(pool.io, [&] { server.Stop(); });
    }

    // ---- (c) load field cell size: safe minimum -----------------------------
    // The grid is dim_x * dim_y cells over the world bounds (runtime-owned:
    // the 100 km production extent here). "finite and > 0" alone admits a
    // cell size whose grid overflows the u32 dimensions or cannot be
    // allocated by the supervisor's field rebuild.
    {
        bool all_ok = true;
        std::string detail;
        for (const float requested : {0.0001f, 0.5f, 5.0f, 50.0f, 100.0f, 500.0f}) {
            gs::game::LoadFieldConfig config;
            config.cell_size_m = requested;
            config.bounds = gs::game::WorldBounds::FromExtent(100000.0f);
            const auto validated = gs::game::ValidateLoadFieldConfig(config);
            const float cell = validated.effective.cell_size_m;
            // Cell counts computed in double: no allocation, no u32 overflow.
            const double cells = std::ceil(100000.0 / cell) * std::ceil(100000.0 / cell);
            const double requested_cells =
                std::ceil(100000.0 / requested) * std::ceil(100000.0 / requested);
            // A request that is already safe must be kept exactly.
            const bool safe_request = requested >= 10.0f && requested_cells <= 1048576.0;
            const bool ok = cell >= 10.0f && cells <= 1048576.0 && (!safe_request || cell == requested);
            all_ok = all_ok && ok;
            char line[160];
            std::snprintf(line, sizeof(line), " %.4g->%.4g(cells=%.3g)", requested, cell, cells);
            detail += line;
        }
        report("load-field-cell-size-safe-minimum", all_ok,
               Fmt("world=100km requested->effective:%s", detail.c_str()));
    }

    std::printf("HYGIENE-DONE failures=%d\n", failures);
    return failures;
}

// ============================================================================
// Map data audit (M0)
// ============================================================================
// Documents map-layer behavior as evidence for the Real World / Map Data
// Layer requirements (docs/map-data-layer-requirements.md). Two columns per
// finding since MAP-1:
//   legacy: the unchanged shared/map MapData.h loaders (client API) --
//           REPRODUCED while the historic defect is still there;
//   server: the path the gameserver actually uses now (strict package
//           loader + runtime) -- CHANGED when the defect no longer reaches
//           the server, OPEN(MAP-n) when it is scheduled for a later package.
namespace {

namespace mfs = std::filesystem;

mfs::path AuditDir(const std::string& name)
{
    const mfs::path dir = mfs::temp_directory_path() / "ixw_mapaudit" / name;
    std::error_code ec;
    mfs::remove_all(dir, ec);
    mfs::create_directories(dir, ec);
    return dir;
}

void WriteBytesTo(const mfs::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::error_code ec;
    mfs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Small valid package: `cells` x `cells`, flat 1 m, one bootstrap zone.
mx::map::PackageWriteSpec AuditSpec(std::uint32_t cells, float cell_m, std::uint32_t chunk)
{
    mx::map::PackageWriteSpec spec;
    spec.world_id = "audit";
    spec.world_name = "audit";
    spec.size_cells_x = cells;
    spec.cell_size_m = cell_m;
    spec.chunk_size_cells = chunk;
    spec.height_raw = [](std::uint32_t, std::uint32_t) -> std::int32_t { return 100; };
    const float extent = static_cast<float>(cells) * cell_m;
    spec.logic.zones.push_back({1, "all", {0.0f, 0.0f, extent, extent}});
    // MAP-2: every package needs a player spawn region (walkable centre).
    spec.logic.spawns.push_back({1, 1, {extent * 0.5f - 5.0f, extent * 0.5f - 5.0f, extent * 0.5f + 5.0f, extent * 0.5f + 5.0f}});
    spec.overwrite = true;
    return spec;
}

struct ServerLoadResult {
    std::optional<gs::game::LoadedWorld> world;
    mx::map::PackageReport report;
    bool Has(mx::map::PackageErrorCode code) const
    {
        for (const auto& issue : report.issues) {
            if (issue.code == code && issue.severity == mx::map::IssueSeverity::Error) {
                return true;
            }
        }
        return false;
    }
    std::string ErrorCodes() const
    {
        std::string out;
        std::set<std::string> seen;
        for (const auto& issue : report.issues) {
            if (issue.severity == mx::map::IssueSeverity::Error && seen.insert(ToString(issue.code)).second) {
                out += (out.empty() ? "" : ",") + std::string(ToString(issue.code));
            }
        }
        return out.empty() ? std::string("none") : out;
    }
};

ServerLoadResult ServerLoad(const mfs::path& dir, mx::map::WarpPolicy policy = mx::map::WarpPolicy::Strict)
{
    ServerLoadResult out;
    gs::game::WorldLoadRequest request;
    request.package_root = dir;
    request.warp_policy=policy;
    request.mob_types_config = IXTREEME_DEFAULT_MOB_TYPES_CONFIG;
    out.world = gs::game::LoadWorldPackage(request, out.report);
    return out;
}

struct MemoryAssets {
    std::unordered_map<std::string, std::vector<std::uint8_t>> files;
    std::vector<std::string> reads;
    mx::map::AssetReadFn Reader()
    {
        return [this](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
            std::string key(path);
            reads.push_back(key);
            const auto it = files.find(key);
            if (it == files.end()) {
                return std::nullopt;
            }
            return it->second;
        };
    }
};

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int b = 0; b < 4; ++b) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * b)) & 0xff));
    }
}

void PutU16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void PutF32(std::vector<std::uint8_t>& out, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    PutU32(out, bits);
}

struct LogicZoneSpec {
    std::uint32_t id;
    mx::map::Rect bounds;
};

std::vector<std::uint8_t> WorldLogicBytes(const std::vector<LogicZoneSpec>& zones,
                                          const std::vector<std::pair<std::uint32_t, mx::map::Rect>>& spawns,
                                          const std::vector<mx::map::WarpRegion>& warps)
{
    std::vector<std::uint8_t> out;
    PutU32(out, 0x314c584d);
    PutU32(out, 1);
    PutU32(out, static_cast<std::uint32_t>(zones.size()));
    PutU32(out, static_cast<std::uint32_t>(spawns.size()));
    PutU32(out, static_cast<std::uint32_t>(warps.size()));
    for (const auto& zone : zones) {
        PutU32(out, zone.id);
        out.push_back(1);
        out.push_back('z');
        PutF32(out, zone.bounds.min_x);
        PutF32(out, zone.bounds.min_y);
        PutF32(out, zone.bounds.max_x);
        PutF32(out, zone.bounds.max_y);
    }
    std::uint32_t spawn_id = 1;
    for (const auto& [zone_id, rect] : spawns) {
        PutU32(out, spawn_id++);
        PutU32(out, zone_id);
        PutF32(out, rect.min_x);
        PutF32(out, rect.min_y);
        PutF32(out, rect.max_x);
        PutF32(out, rect.max_y);
    }
    for (const auto& warp : warps) {
        PutU32(out, warp.id);
        PutF32(out, warp.source.min_x);
        PutF32(out, warp.source.min_y);
        PutF32(out, warp.source.max_x);
        PutF32(out, warp.source.max_y);
        PutF32(out, warp.target_x);
        PutF32(out, warp.target_y);
    }
    return out;
}

std::vector<std::uint8_t> ManifestBytes(std::uint32_t world_cells, float cell_m,
                                        std::uint32_t chunk_cells, std::uint32_t zone_grid)
{
    capnp::MallocMessageBuilder msg;
    auto root = msg.initRoot<mx::map::package_schema::MapManifest>();
    root.setFormatVersion(2);
    root.setWorldId("audit");
    root.setWorldName("audit");
    root.setWorldSizeCells(world_cells);
    root.setCellSizeMeters(cell_m);
    root.setHeightUnit(mx::map::package_schema::HeightUnit::CENTIMETERS);
    root.setChunkSizeCells(chunk_cells);
    root.initZoneGridDims().setX(zone_grid);
    root.getZoneGridDims().setY(zone_grid);
    root.setZoneSizeCells(world_cells / std::max(zone_grid, 1u));
    const auto flat = capnp::messageToFlatArray(msg);
    const auto bytes = flat.asBytes();
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

// One MXC1 chunk: heights (all `height_cm`), attributes, optional splats.
std::vector<std::uint8_t> ChunkBytes(std::uint16_t cx, std::uint16_t cy, std::uint16_t cells,
                                     std::int16_t height_cm, bool with_splats)
{
    const std::uint16_t sections = with_splats ? 4 : 2;
    const std::size_t header = 14 + 12 * static_cast<std::size_t>(sections);
    std::vector<std::uint8_t> heights;
    for (std::size_t i = 0; i < static_cast<std::size_t>(cells + 1) * (cells + 1); ++i) {
        PutU16(heights, static_cast<std::uint16_t>(height_cm));
    }
    std::vector<std::uint8_t> attributes(static_cast<std::size_t>(cells) * cells * 2, 0);
    std::vector<std::uint8_t> splat;
    PutU16(splat, 1);
    PutU16(splat, 1);
    splat.insert(splat.end(), 4, 0x80);
    std::vector<std::pair<std::uint16_t, const std::vector<std::uint8_t>*>> payloads = {
        {1, &heights}, {3, &attributes}};
    if (with_splats) {
        payloads.push_back({2, &splat});
        payloads.push_back({4, &splat});
    }
    std::vector<std::uint8_t> out;
    PutU32(out, 0x3143584d);
    PutU16(out, 2);
    PutU16(out, cx);
    PutU16(out, cy);
    PutU16(out, cells);
    PutU16(out, sections);
    std::uint32_t offset = static_cast<std::uint32_t>(header);
    for (const auto& [type, data] : payloads) {
        PutU16(out, type);
        PutU16(out, 0);
        PutU32(out, offset);
        PutU32(out, static_cast<std::uint32_t>(data->size()));
        offset += static_cast<std::uint32_t>(data->size());
    }
    for (const auto& [type, data] : payloads) {
        (void)type;
        out.insert(out.end(), data->begin(), data->end());
    }
    return out;
}

} // namespace

int RunMapAuditScenario()
{
    const bool map4_accepted = RunMap4Scenario() == 0;
    int changed = 0;
    int open = 0;
    // legacy_reproduced: the historic defect still shows in the legacy
    // (client) loader API. server_status: CHANGED | PARTIAL | OPEN(MAP-n).
    auto finding = [&](const char* id, bool legacy_reproduced, const std::string& legacy,
                       const std::string& server_status, const std::string& server) {
        std::printf("MAPAUDIT %s legacy: %s [%s] | server: %s [%s]\n", id, legacy.c_str(),
                    legacy_reproduced ? "REPRODUCED" : "CHANGED", server.c_str(), server_status.c_str());
        std::fflush(stdout);
        changed += server_status == "CHANGED" ? 1 : 0;
        open += server_status == "CHANGED" ? 0 : 1;
    };
    const float kNaN = std::numeric_limits<float>::quiet_NaN();

    // R4: truncated worldlogic -- the legacy field readers clamp the offset
    // to the end of the buffer and check `offset > size`.
    {
        auto bytes = WorldLogicBytes({{7, mx::map::Rect{0, 0, 100, 100}}}, {}, {});
        bytes.resize(20 + 4); // header + zone id only
        MemoryAssets assets;
        assets.files["./worldlogic.dat"] = bytes;
        const auto logic = mx::map::LoadWorldLogic(assets.Reader(), ".");
        const bool accepted = logic && logic->zones.size() == 1;
        const auto dir = AuditDir("r4_truncated");
        (void)mx::map::WritePackage(dir, AuditSpec(64, 1.0f, 32));
        WriteBytesTo(dir / "worldlogic.dat", bytes);
        const auto server = ServerLoad(dir);
        finding("R4-truncated-worldlogic", accepted,
                accepted ? Fmt("file=24B claims 1 zone -> zone id=%u bounds=(%.0f,%.0f)-(%.0f,%.0f)",
                               logic->zones[0].id, logic->zones[0].bounds.min_x,
                               logic->zones[0].bounds.min_y, logic->zones[0].bounds.max_x,
                               logic->zones[0].bounds.max_y)
                         : std::string("rejected"),
                !server.world && server.Has(mx::map::PackageErrorCode::WorldLogicTruncated) ? "CHANGED" : "OPEN",
                Fmt("load=%s errors=%s", server.world ? "ACCEPTED" : "refused", server.ErrorCodes().c_str()));
    }
    // R4: reserved id 0, duplicate ids, NaN / inverted bounds, overlap, a
    // spawn pointing at an unknown zone, a non-finite warp target.
    {
        const auto bytes = WorldLogicBytes(
            {{0, mx::map::Rect{0, 0, 500, 500}},
             {5, mx::map::Rect{400, 400, 900, 900}}, // overlaps zone 0
             {5, mx::map::Rect{kNaN, 0, 100, 100}},  // duplicate id + NaN
             {6, mx::map::Rect{900, 900, 100, 100}}}, // min > max
            {{42, mx::map::Rect{10, 10, 20, 20}}},  // zone 42 does not exist
            {mx::map::WarpRegion{1, mx::map::Rect{0, 0, 10, 10}, kNaN, 50.0f}});
        MemoryAssets assets;
        assets.files["./worldlogic.dat"] = bytes;
        const auto logic = mx::map::LoadWorldLogic(assets.Reader(), ".");
        const bool accepted = logic && logic->zones.size() == 4 && logic->spawns.size() == 1 &&
                              logic->warps.size() == 1;
        const auto dir = AuditDir("r4_invalid");
        (void)mx::map::WritePackage(dir, AuditSpec(100, 10.0f, 50)); // 1000 m world
        WriteBytesTo(dir / "worldlogic.dat", bytes);
        const auto server = ServerLoad(dir);
        using C = mx::map::PackageErrorCode;
        const bool all_named = server.Has(C::WorldLogicIdInvalid) && server.Has(C::WorldLogicIdDuplicate) &&
                               server.Has(C::WorldLogicRectInvalid) && server.Has(C::WorldLogicZoneOverlap) &&
                               server.Has(C::WorldLogicReferenceInvalid) &&
                               server.Has(C::WorldLogicWarpTargetInvalid);
        finding("R4-invalid-worldlogic", accepted,
                accepted ? std::string("zone id 0, duplicate id 5, NaN bounds, min>max, overlapping zones, "
                                       "spawn->unknown zone 42, NaN warp target: all accepted")
                         : std::string("rejected"),
                !server.world && all_named ? "CHANGED" : "OPEN",
                Fmt("load=%s errors=%s", server.world ? "ACCEPTED" : "refused", server.ErrorCodes().c_str()));
    }
    // R1: legacy enumerates chunks by the ZONE grid. World 64 cells, 32-cell
    // chunks (2x2 chunk files), zoneGridDims 1x1.
    {
        MemoryAssets assets;
        assets.files["./map.manifest"] = ManifestBytes(64, 1.0f, 32, 1);
        for (std::uint16_t cy = 0; cy < 2; ++cy) {
            for (std::uint16_t cx = 0; cx < 2; ++cx) {
                assets.files["./chunks/chunk_" + std::to_string(cx) + "_" + std::to_string(cy) +
                             ".mxchunk"] = ChunkBytes(cx, cy, 32, 100, true);
            }
        }
        const auto field = mx::map::LoadHeightField(assets.Reader(), ".");
        std::size_t chunk_reads = 0;
        for (const auto& read : assets.reads) {
            chunk_reads += read.find("chunks/") != std::string::npos ? 1 : 0;
        }
        const float h_loaded = field ? field->SampleHeightMeters(10.0f, 10.0f) : -1.0f;
        const float h_missing = field ? field->SampleHeightMeters(50.0f, 50.0f) : -1.0f;
        const bool reproduced = field && chunk_reads == 1 && h_missing == 0.0f && h_loaded > 0.9f;
        // Server: the same v2 geometry (writer's legacy layout, zoneGridDims
        // patched to 1x1) through the strict loader.
        auto spec = AuditSpec(64, 1.0f, 32);
        spec.format_version = 2;
        spec.splat_size = 1;
        spec.patch_manifest = [](mx::map::package_schema::MapManifest::Builder& m) {
            m.getZoneGridDims().setX(1);
            m.getZoneGridDims().setY(1);
        };
        const auto dir = AuditDir("r1_grid");
        (void)mx::map::WritePackage(dir, spec);
        const auto server = ServerLoad(dir);
        const auto far_sample = server.world ? server.world->terrain.Height(50.0, 50.0) : mx::map::HeightSample{};
        const float s_far = far_sample.Ok() ? far_sample.meters : -1.0f;
        const bool fixed = server.world && server.report.chunks_checked == 4 && s_far > 0.9f;
        finding("R1-chunk-grid-conflated-with-zone-grid", reproduced,
                Fmt("2x2 chunk files, zone_grid=1x1 -> chunk files read=%zu, height chunk(0,0)=%.2fm "
                    "chunk(1,1)=%.2fm (never loaded, reported valid=%d)",
                    chunk_reads, h_loaded, h_missing, field ? 1 : 0),
                fixed ? "CHANGED" : "OPEN",
                Fmt("chunks read=%u (chunk grid from world/chunk size), height chunk(1,1)=%.2fm, "
                    "zoneGridDims reported as ignored legacy field",
                    server.report.chunks_checked, s_far));
    }
    // R5: render splats required by the loader and kept in server memory.
    {
        MemoryAssets assets;
        assets.files["./map.manifest"] = ManifestBytes(32, 1.0f, 32, 1);
        assets.files["./chunks/chunk_0_0.mxchunk"] = ChunkBytes(0, 0, 32, 100, false);
        const auto without = mx::map::LoadHeightField(assets.Reader(), ".");
        MemoryAssets with_assets;
        with_assets.files["./map.manifest"] = ManifestBytes(32, 1.0f, 32, 1);
        with_assets.files["./chunks/chunk_0_0.mxchunk"] = ChunkBytes(0, 0, 32, 100, true);
        const auto with = mx::map::LoadHeightField(with_assets.Reader(), ".");
        const bool reproduced = !without && with && !with->splat_a_rgba8.empty();
        const auto server_only_dir = AuditDir("r5_server_only");
        (void)mx::map::WritePackage(server_only_dir, AuditSpec(64, 1.0f, 32)); // no splat sections
        auto with_splats = AuditSpec(64, 1.0f, 32);
        with_splats.splat_size = 16;
        const auto client_dir = AuditDir("r5_with_splats");
        (void)mx::map::WritePackage(client_dir, with_splats);
        const auto s_only = ServerLoad(server_only_dir);
        const auto s_with = ServerLoad(client_dir);
        // ServerTerrain has no splat storage at all: the resident bytes with
        // and without client data must be identical.
        const bool fixed = s_only.world && s_with.world &&
                           s_only.report.resident_terrain_bytes == s_with.report.resident_terrain_bytes;
        finding("R5-server-requires-render-splats", reproduced,
                Fmt("heights+attributes only -> load=%s; with splats -> load=%s, keeps splat_a+b=%zu B",
                    without ? "ok" : "FAILS", with ? "ok" : "fails",
                    with ? with->splat_a_rgba8.size() + with->splat_b_rgba8.size() : 0),
                fixed ? "CHANGED" : "OPEN",
                Fmt("server-only package -> load=%s; with splats -> load=%s; resident terrain %llu B vs %llu B "
                    "(no splat storage on the server)",
                    s_only.world ? "ok" : "FAILS", s_with.world ? "ok" : "fails",
                    static_cast<unsigned long long>(s_only.report.resident_terrain_bytes),
                    static_cast<unsigned long long>(s_with.report.resident_terrain_bytes)));
    }
    // R6: warp re-trigger. Runtime semantics (enter edge, cooldown) are MAP-4;
    // MAP-1 adds the load-time rule: a trigger cycle is rejected.
    {
        mx::map::WorldLogic logic;
        logic.warps.push_back(mx::map::WarpRegion{1, mx::map::Rect{0, 0, 20, 20}, 10.0f, 10.0f});
        logic.warps.push_back(mx::map::WarpRegion{2, mx::map::Rect{100, 0, 120, 20}, 200.0f, 10.0f});
        logic.warps.push_back(mx::map::WarpRegion{3, mx::map::Rect{190, 0, 210, 20}, 110.0f, 10.0f});
        const auto* self = logic.FindWarp(10.0f, 10.0f);
        const auto* back = logic.FindWarp(200.0f, 10.0f);
        const bool reproduced = self != nullptr && self->id == 1 && back != nullptr && back->id == 3;
        auto spec = AuditSpec(100, 10.0f, 50);
        spec.logic.warps = logic.warps;
        const auto dir = AuditDir("r6_warp");
        (void)mx::map::WritePackage(dir, spec);
        const auto server = ServerLoad(dir);
        const bool load_rule = !server.world && server.Has(mx::map::PackageErrorCode::WorldLogicWarpCycle);
        auto chain=AuditSpec(64,1.0f,32);
        chain.logic.warps={{1,{10,10,12,12},21,21},{2,{20,20,22,22},40,40}};
        const auto chain_dir=AuditDir("r6_chain"); (void)mx::map::WritePackage(chain_dir,chain);
        const auto strict_chain=ServerLoad(chain_dir);
        const auto legacy_chain=ServerLoad(chain_dir,mx::map::WarpPolicy::Legacy);
        const auto legacy_cycle=ServerLoad(dir,mx::map::WarpPolicy::Legacy);
        const bool policy_ok=!strict_chain.world && strict_chain.Has(mx::map::PackageErrorCode::WorldLogicWarpTargetInTrigger) &&
            legacy_chain.world && !legacy_cycle.world && legacy_cycle.Has(mx::map::PackageErrorCode::WorldLogicWarpCycle);
        finding("R6-warp-retrigger", reproduced,
                "warp 1 target inside its own source -> fires again next tick; warps 2<->3 ping-pong "
                "(no enter edge, no cooldown)",
                load_rule && map4_accepted && policy_ok ? "CHANGED" : "OPEN",
                Fmt("load=%s errors=%s (load-time cycle rule); map4 runtime acceptance executed above; "
                    "R6 strict(default) rejects chain; explicit legacy permits chain; cycles rejected in both modes",
                    server.world ? "ACCEPTED" : "refused", server.ErrorCodes().c_str()));
    }
    // R3: world bounds / origin from the loaded data. The historic
    // DefaultRegions() (four fixed 0..100 km quadrants) is removed; the
    // server now builds regions + initial leaves from the loaded bounds.
    {
        auto spec = AuditSpec(64, 1.0f, 32);
        spec.size_cells_x = 640; // 2560 x 2048 m, cell 4 m, origin (-1280, -1024), partial chunks
        spec.size_cells_y = 512;
        spec.cell_size_m = 4.0f;
        spec.chunk_size_cells = 96;
        spec.origin_x = -1280.0;
        spec.origin_y = -1024.0;
        spec.logic.zones = {{1, "all", {-1280.0f, -1024.0f, 1280.0f, 1024.0f}}};
        spec.logic.spawns = {{1, 1, {-10.0f, -10.0f, 10.0f, 10.0f}}};
        const auto dir = AuditDir("r3_bounds");
        (void)mx::map::WritePackage(dir, spec);
        const auto server = ServerLoad(dir);
        bool fixed = false;
        std::string detail = "load refused: " + server.ErrorCodes();
        if (server.world) {
            const auto bounds = gs::game::TerrainService::BoundsOf(server.world->terrain);
            gs::game::PartitionLayout layout; // 2x2 regions, 1 leaf each
            gs::game::InitialPartition partition;
            std::string error;
            const bool built = gs::game::BuildInitialPartition(bounds, layout, 240.0f, partition, error);
            gs::game::ZoneManager zones;
            if (built) {
                zones.BuildInitialPartition(partition);
            }
            const std::size_t inside = zones.FindIndexForPosition(-10.0f, -10.0f);   // south-west quadrant
            const std::size_t east_edge = zones.FindIndexForPosition(1280.0f, 0.0f); // max x: outside
            const std::size_t beyond = zones.FindIndexForPosition(-1300.0f, 0.0f);
            fixed = built && partition.regions.size() == 4 && partition.regions[0].name == "SouthWest" &&
                    partition.regions[0].bounds.min_x == -1280.0f && partition.regions[0].bounds.min_y == -1024.0f &&
                    partition.regions[3].name == "NorthEast" && partition.regions[3].bounds.max_x == 1280.0f &&
                    inside < zones.ZoneCount() && zones.GetZone(inside).Region() == partition.regions[0].id &&
                    east_edge == zones.ZoneCount() && beyond == zones.ZoneCount();
            detail = Fmt("loaded bounds (%.0f,%.0f)-(%.0f,%.0f) -> regions %s..%s; (-10,-10) -> zone in %s; "
                         "(1280,0) and (-1300,0) -> no zone",
                         bounds.min_x, bounds.min_y, bounds.max_x, bounds.max_y,
                         built ? partition.regions.front().name.c_str() : "?",
                         built ? partition.regions.back().name.c_str() : "?",
                         inside < zones.ZoneCount() ? zones.GetZone(inside).Name().c_str() : "NONE");
        }
        finding("R3-hardcoded-world-bounds", false,
                "DefaultRegions() (fixed 0..100000 m quadrants, names not matching geometry) removed",
                fixed ? "CHANGED" : "OPEN", detail);
    }
    // R8: silent flat fallback. The TerrainService::LoadFromMapRoot fallback
    // is gone; the server path refuses a missing package.
    {
        const auto server = ServerLoad(mfs::temp_directory_path() / "ixw_mapaudit" / "no-such-package");
        finding("R8-silent-flat-fallback", false,
                "TerrainService::LoadFromMapRoot (flat 1000 m on failure) removed from the code base",
                !server.world && server.Has(mx::map::PackageErrorCode::PackageRootMissing) ? "CHANGED" : "OPEN",
                Fmt("missing package -> load=%s errors=%s; gameserver exits non-zero before DB/listen "
                    "(startup acceptance)",
                    server.world ? "ACCEPTED" : "refused", server.ErrorCodes().c_str()));
    }
    // R7: compile-time source-tree map path.
    {
#ifdef IXTREEME_DEFAULT_MAP_ROOT
        const bool defined = true;
#else
        const bool defined = false;
#endif
        finding("R7-build-time-map-path", defined,
                defined ? "IXTREEME_DEFAULT_MAP_ROOT still compiled in"
                        : "IXTREEME_DEFAULT_MAP_ROOT no longer defined (bench uses IXTREEME_TEST_MAP_ROOT)",
                defined ? "OPEN" : "CHANGED",
                "gameserver target: world_package from config (relative to the config file) or "
                "--world-package; no compile-time world path (startup acceptance runs from another cwd)");
    }
    // Rows added with MAP-1 (no legacy reproduction beyond the M0 text).
    {
        auto spec = AuditSpec(64, 1.0f, 32);
        spec.patch_manifest = [](mx::map::package_schema::MapManifest::Builder& m) {
            m.initZoneGridDims().setX(2);
            m.getZoneGridDims().setY(2);
        };
        const auto dir = AuditDir("r2_zone_fields");
        (void)mx::map::WritePackage(dir, spec);
        const auto server = ServerLoad(dir);
        // The checked-in test map: 3 areas (AreaId 1..3), partitioned by the
        // server layout (2x2 regions) into 4 zones (ZoneId 1..4).
        const auto test_map = ServerLoad(IXTREEME_TEST_MAP_ROOT,mx::map::WarpPolicy::Legacy);
        gs::game::ZoneManager zones;
        std::size_t areas = 0;
        if (test_map.world) {
            areas = test_map.world->logic.zones.size();
            gs::game::InitialPartition partition;
            std::string error;
            if (gs::game::BuildInitialPartition(gs::game::TerrainService::BoundsOf(test_map.world->terrain),
                                                gs::game::PartitionLayout{}, 240.0f, partition, error)) {
                zones.BuildInitialPartition(partition);
            }
        }
        const bool r2 = server.Has(mx::map::PackageErrorCode::ManifestFieldForbidden) && areas == 3 &&
                        zones.ZoneCount() == 4;
        finding("R2-map-chunk-vs-server-zone", true, "v2 manifest carries zoneGridDims/zoneSizeCells",
                r2 ? "CHANGED" : "OPEN",
                Fmt("v3 with zoneGridDims -> errors=%s; test map: %zu areas (AreaId, metadata) vs %zu server "
                    "zones from partition_regions=2x2 (ZoneId 1..%zu)",
                    server.ErrorCodes().c_str(), areas, zones.ZoneCount(), zones.ZoneCount()));
        // R9: a point on the shared x = 500 edge of the test map's SW/SE zones.
        const mx::map::Rect sw{0.0f, 0.0f, 500.0f, 500.0f};
        const mx::map::Rect se{500.0f, 0.0f, 1000.0f, 500.0f};
        const bool legacy_double = sw.Contains(500.0f, 250.0f) && se.Contains(500.0f, 250.0f);
        const std::size_t owner = zones.FindIndexForPosition(500.0f, 250.0f);
        const bool r9 = !sw.ContainsHalfOpen(500.0f, 250.0f) && se.ContainsHalfOpen(500.0f, 250.0f) &&
                        owner < zones.ZoneCount() && zones.GetZone(owner).Bounds().min_x == 500.0f;
        finding("R9-half-open-boundaries", legacy_double,
                "client Rect::Contains is closed: (500,250) lies in BOTH neighbouring rectangles",
                r9 ? "CHANGED" : "OPEN",
                Fmt("server: half-open everywhere (partition, areas, warps, world); (500,250) -> exactly zone %u "
                    "(%s)",
                    owner < zones.ZoneCount() ? zones.GetZone(owner).Id() : 0u,
                    owner < zones.ZoneCount() ? zones.GetZone(owner).Name().c_str() : "NONE"));
        // R10: outside the world is not ground.
        const auto edge = test_map.world ? test_map.world->terrain.Height(1000.0, 10.0) : mx::map::HeightSample{};
        const auto inside = test_map.world ? test_map.world->terrain.Height(999.5, 10.0) : mx::map::HeightSample{};
        const bool r10 = edge.status == mx::map::TerrainStatus::OutsideWorld && inside.Ok() &&
                         zones.FindIndexForPosition(1000.0f, 10.0f) == zones.ZoneCount();
        const mfs::path test_root = IXTREEME_TEST_MAP_ROOT;
        const auto legacy_field = mx::map::LoadHeightField(
            [&](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
                std::ifstream file(test_root / mfs::path(path).relative_path(), std::ios::binary);
                if (!file) {
                    return std::nullopt;
                }
                return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)),
                                                 std::istreambuf_iterator<char>());
            },
            ".");
        const float legacy_out = legacy_field ? legacy_field->SampleHeightMeters(1500.0f, 10.0f) : -1.0f;
        const float legacy_edge = legacy_field ? legacy_field->SampleHeightMeters(1000.0f, 10.0f) : -2.0f;
        finding("R10-out-of-bounds-rule", legacy_field && legacy_out == legacy_edge,
                Fmt("legacy HeightField: height(1500,10)=%.2fm == edge height(1000,10)=%.2fm (clamped)", legacy_out,
                    legacy_edge),
                r10 ? "CHANGED" : "OPEN",
                Fmt("server: Height(1000,10)=%s, Height(999.5,10)=%s; no zone owns (1000,10); movement refuses the "
                    "step, spawn/warp require inside+walkable (worldbench --mode terrain)",
                    mx::map::ToString(edge.status), mx::map::ToString(inside.status)));
        mx::map::PackageReport sample;
        (void)mx::map::LoadServerWorld(mfs::temp_directory_path() / "ixw_mapaudit" / "r4_truncated",
                                       mx::map::ValidationDepth::Startup, sample);
        const auto* first = sample.FirstError();
        finding("R11-structured-errors-versioning", true, "std::optional (no reason) / capnp exception",
                first != nullptr ? "CHANGED" : "OPEN",
                first != nullptr ? "e.g. " + first->Format() : std::string("no structured error"));
        {
            // MAP-3: a streaming load decodes only the startup set; the rest
            // is loaded on demand within a budget by the terrain streamer.
            const auto stream_dir = AuditDir("r12_streaming");
            (void)mx::map::WritePackage(stream_dir, AuditSpec(256, 1.0f, 32)); // 8 x 8 chunks
            mx::map::PackageReport stream_report;
            gs::game::WorldLoadRequest request;
            request.package_root = stream_dir;
            request.mob_types_config = IXTREEME_DEFAULT_MOB_TYPES_CONFIG;
            request.residency = mx::map::ResidencyMode::Streaming;
            const auto streamed = gs::game::LoadWorldPackage(request, stream_report);
            const bool streaming_ok = streamed && stream_report.chunks_decoded < stream_report.chunks_checked;
            finding("R12-chunk-streamed-terrain", true, "whole world resident (+splats)",
                    streaming_ok ? "CHANGED" : "OPEN",
                    Fmt("terrain_residency=streaming: startup size-checked %u chunk files, decoded %u (spawn/warp "
                        "startup set); the rest load on demand within terrain_cache_budget_mb, validated on "
                        "load, LRU-evicted (worldbench --mode streaming / streamsoak: 100 km, 16 MB)",
                        stream_report.chunks_checked, stream_report.chunks_decoded));
        }
        const auto spawn_dir = AuditDir("r13_spawns");
        auto spawn_spec = AuditSpec(64, 1.0f, 32);
        spawn_spec.mob_spawns = std::string("mob_type_id=1 x=nan y=5 count=1 radius=1\n");
        (void)mx::map::WritePackage(spawn_dir, spawn_spec);
        const auto spawns = ServerLoad(spawn_dir);
        finding("R13-spawns-in-validated-package", true, "mob_spawns.conf lenient (warn + skip line)",
                !spawns.world && spawns.Has(mx::map::PackageErrorCode::SpawnsFieldInvalid) && map4_accepted ? "CHANGED" : "OPEN",
                Fmt("spawn table is a package layer, strictly parsed + bounds + mob type checked "
                    "(bad line -> errors=%s); v2 identity/area and lifecycle acceptance executed above",
                    spawns.ErrorCodes().c_str()));
    }
    std::error_code ec;
    mfs::remove_all(mfs::temp_directory_path() / "ixw_mapaudit", ec);
    std::printf("MAPAUDIT-DONE server_changed=%d server_open_or_partial=%d\n", changed, open);
    return 0; // evidence probe: statuses are reported, not failed
}

} // namespace gs::bench
