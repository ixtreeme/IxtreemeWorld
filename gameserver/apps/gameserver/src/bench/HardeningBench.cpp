#include "HardeningBench.h"

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
#include <limits>
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
#include "network/Server.h"
#include "network/Session.h"
#include "protocol/Protocol.h"
#include "protocol/Serialization.h"

#include "../GameConnectionHandler.h"
#include "../world/WorldConstants.h"
#include "../world/WorldRuntime.h"
#include "../world/replication/ProtocolEncoder.h"
#include "../world/replication/ResyncSchedule.h"

namespace gs::bench {
namespace {

using Clock = std::chrono::steady_clock;
namespace asio = boost::asio;
using asio::ip::tcp;

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
            const auto& zone = sim.Zones().GetZone(0);
            const auto t0 = Clock::now();
            const WireSnapshot w0 = TakeWire(wire);
            const std::uint64_t in0 = inputs_posted.load();
            const std::uint64_t pushed0 = zone.Commands().PushedTotal();
            const std::uint64_t drained0 = zone.Commands().DrainedTotal();
            const auto sched0 = sim.SchedulerStats();

            std::this_thread::sleep_for(std::chrono::seconds(measure_seconds));

            const auto t1 = Clock::now();
            const WireSnapshot w1 = TakeWire(wire);
            const std::uint64_t in1 = inputs_posted.load();
            const std::uint64_t pushed1 = zone.Commands().PushedTotal();
            const std::uint64_t drained1 = zone.Commands().DrainedTotal();
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
            result.commands_per_s = static_cast<double>(pushed1 - pushed0) / wall;
            result.drained_per_tick =
                ticks > 0.0 ? static_cast<double>(drained1 - drained0) / ticks : 0.0;
            result.max_drained = zone.Commands().MaxDrainedPerTake();
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
    if (WaitFor(std::chrono::seconds(10), [&] { return sim.Owners().size() == 1; })) {
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
                     [&] { return sim.Owners().size() == static_cast<std::size_t>(kPlayers); })) {
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
                      sim.Owners().empty(),
                  Fmt("closed=%d/%d contexts_left=%zu cleanups=%llu world_presence=%zu",
                      server_closed, kRuns, handler.ContextCount(),
                      static_cast<unsigned long long>(cleanups), sim.Owners().size()));
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
    out.zones = sim.Zones().GetActiveLeaves().size();
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
        gs::game::WorldRuntime sim(runner.io);
        sim.Start();
        production_workers = WaitWorkers(sim);
        std::printf("WORKERPOOL production-map zones=%zu workers=%zu (pool sized once at Start)\n",
                    sim.Zones().ZoneCount(), production_workers);
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
            std::uint64_t total = 0;
            const auto& zones = sim.Zones();
            for (std::size_t i = 0; i < zones.ZoneCount(); ++i) {
                total += zones.GetZone(i).Diagnostics().repl_v2_starvation_since_diag.load(
                    std::memory_order_relaxed);
            }
            return total;
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

} // namespace gs::bench
