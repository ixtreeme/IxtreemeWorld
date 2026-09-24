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

} // namespace gs::bench
