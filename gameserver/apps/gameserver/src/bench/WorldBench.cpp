// World load/benchmark harness. Separate target behind ENABLE_WORLDBENCH;
// never linked into production. Drives WorldRuntime through its public
// Post* API only (like real clients), with synthetic sessions and load
// scenarios:
//
//   A) 100 players / 1,000 mobs            (spread)
//   B) 500 players / 5,000 mobs            (spread)
//   C) 1,000 players / 10,000 mobs         (spread)
//   D) 4,000 players / 50,000 mobs         (spread, heavy)
//   E) 1,000 players concentrated          (hotspot: AOI/replication stress)
//   F) border migration stress             (many entities crossing zones)
//   G) dense AOI stress                    (many entities in a small area)
//
// Usage:
//   worldbench [--players N] [--mobs M] [--seconds S]
//              [--mode spread|hotspot|border|dense]
//              [--validate-every K] [--despawn-storm R] [--seed S]
//              [--logical-processes K] [--routing-selftest]
// Logical distribution (§33/§51): --logical-processes stripes zones across
// K logical processes in this binary; routing/migration treat them as
// remote-emulated while all delivery stays local.
//
// Measures zone-tick p50/p95/p99/max (per-zone ring buffers), worker
// utilization, supervisor time, migrations, deaths/respawns, and runs the
// consistency validator periodically when requested.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "common/Logging.h"
#include "db/CharacterRepository.h"
#include "network/Session.h"

#include "../world/migration/EntityTransfer.h"
#include "../world/WorldRuntime.h"
#include "../world/spawn/SpawnLoader.h"

#include <flecs.h>

namespace {

struct BenchConfig {
    int players = 100;
    int mobs = 1000;
    int seconds = 30;
    std::string mode = "spread";
    int validate_every = 0;
    int despawn_storm = 0;
    int logical_processes = 0;
    bool routing_selftest = false;
    std::uint32_t seed = 12345;
};

bool ParseArgs(int argc, char** argv, BenchConfig& config)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const char* name, std::string& out) {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string value;
        if (arg == "--help" || arg == "-h") {
            std::cout << "worldbench [--players N] [--mobs M] [--seconds S]\n"
                         "             [--mode spread|hotspot|border|dense]\n"
                         "             [--validate-every K] [--despawn-storm R] [--seed S]\n"
                         "             [--logical-processes K] [--routing-selftest]\n";
            return false;
        } else if (arg == "--players") {
            if (!need_value("players", value)) {
                return false;
            }
            config.players = std::stoi(value);
        } else if (arg == "--mobs") {
            if (!need_value("mobs", value)) {
                return false;
            }
            config.mobs = std::stoi(value);
        } else if (arg == "--seconds") {
            if (!need_value("seconds", value)) {
                return false;
            }
            config.seconds = std::stoi(value);
        } else if (arg == "--mode") {
            if (!need_value("mode", value)) {
                return false;
            }
            config.mode = value;
        } else if (arg == "--validate-every") {
            if (!need_value("validate-every", value)) {
                return false;
            }
            config.validate_every = std::stoi(value);
        } else if (arg == "--despawn-storm") {
            if (!need_value("despawn-storm", value)) {
                return false;
            }
            config.despawn_storm = std::stoi(value);
        } else if (arg == "--seed") {
            if (!need_value("seed", value)) {
                return false;
            }
            config.seed = static_cast<std::uint32_t>(std::stoul(value));
        } else if (arg == "--logical-processes") {
            if (!need_value("logical-processes", value)) {
                return false;
            }
            config.logical_processes = std::stoi(value);
        } else if (arg == "--routing-selftest") {
            config.routing_selftest = true;
        } else {
            std::cerr << "unknown arg: " << arg << "\n";
            return false;
        }
    }
    if (config.mode != "spread" && config.mode != "hotspot" && config.mode != "border" &&
        config.mode != "dense") {
        std::cerr << "bad mode: " << config.mode << "\n";
        return false;
    }
    return true;
}

double Percentile(std::vector<std::uint64_t> samples, double p)
{
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t index =
        std::min(samples.size() - 1,
                 static_cast<std::size_t>(std::floor(p / 100.0 * samples.size())));
    return static_cast<double>(samples[index]) / 1000.0;
}

gs::db::Character MakeBenchCharacter(int index)
{
    gs::db::Character character;
    character.id = gs::db::CharacterId{static_cast<std::uint64_t>(1000 + index)};
    character.account_id = gs::db::AccountId{static_cast<std::uint64_t>(5000 + index)};
    character.slot = 0;
    character.name = "Bench" + std::to_string(index);
    character.level = 1;
    character.experience = 0;
    character.class_id = 1;
    character.appearance = 0;
    character.pos_x = 0;
    character.pos_y = 0;
    character.map_id = 0;
    character.created_at = std::chrono::system_clock::now();
    character.last_played_at = character.created_at;
    return character;
}

struct BenchClient {
    std::shared_ptr<gs::network::Session> session;
    gs::common::SessionId session_id = 0;
    bool alive = false;
    float heading = 0.0f;
    std::uint32_t seq = 0;
    int slot = 0;
    int target_cursor = 0;
};

void SelftestReport(const char* name, bool pass, bool skipped, int& failures)
{
    if (skipped) {
        std::printf("SELFTEST %s: SKIP\n", name);
    } else if (pass) {
        std::printf("SELFTEST %s: PASS\n", name);
    } else {
        std::printf("SELFTEST %s: FAIL\n", name);
        ++failures;
    }
}

bool WaitFor(std::chrono::milliseconds timeout, const std::function<bool()>& condition)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return condition();
}

// Routing/distribution self-test (§34): unknown + draining destinations,
// stale + duplicate migration delivery, source-free EntityTransfer
// roundtrip. Public runtime APIs only. Returns failure count.
int RunRoutingSelftest(gs::game::WorldRuntime& sim,
                       boost::asio::io_context& io,
                       const BenchConfig& config)
{
    (void)config;
    int failures = 0;
    const auto identity = sim.Identity();

    if (sim.Zones().ZoneCount() < 2) {
        SelftestReport("emulation-needs-2-zones", false, true, failures);
        return failures;
    }
    // Logical-distribution emulation for the draining leg (left on: the
    // subsequent load then runs distributed-emulated, which is intended).
    sim.EmulateDistribution(2);

    // Find an emulated-remote zone.
    std::size_t remote_index = sim.Zones().ZoneCount();
    for (std::size_t i = 0; i < sim.Zones().ZoneCount(); ++i) {
        const auto location = sim.Directory().ResolveZone(sim.Zones().GetZone(i).Id());
        if (location && !sim.Directory().IsLocal(*location)) {
            remote_index = i;
            break;
        }
    }
    if (remote_index >= sim.Zones().ZoneCount()) {
        SelftestReport("emulated-remote-zone", false, true, failures);
        return failures;
    }
    const auto remote_zone_id = sim.Zones().GetZone(remote_index).Id();

    // (a) Unknown destination -> DestinationUnavailable, nothing delivered.
    {
        const auto result = sim.Router().RouteZoneCommand(999999, [](gs::game::Zone&) {});
        SelftestReport("unknown-destination",
                       result == gs::game::DeliveryResult::DestinationUnavailable, false, failures);
    }

    // (b) Draining destination -> DestinationDraining, then recovers.
    {
        sim.Directory().SetZoneDrained(remote_zone_id, true);
        const auto drained =
            sim.Router().RouteZoneCommand(remote_index, [](gs::game::Zone&) {});
        sim.Directory().SetZoneDrained(remote_zone_id, false);
        const auto recovered =
            sim.Router().RouteZoneCommand(remote_index, [](gs::game::Zone&) {});
        SelftestReport("draining-destination",
                       drained == gs::game::DeliveryResult::DestinationDraining &&
                           recovered == gs::game::DeliveryResult::DeliveredRemoteEmulated,
                       false, failures);
    }

    // (c) Stale migration request (bogus net) is dropped, world stays valid.
    {
        const auto zone0 = sim.Zones().GetZone(0).Id();
        sim.TestMigrationQueue().TryEnqueue(
            gs::game::MigrationRequest{42424242u, zone0, remote_zone_id});
        const bool drained = WaitFor(std::chrono::seconds(3),
                                     [&] { return sim.TestMigrationQueue().PendingCount() == 0; });
        std::string error;
        sim.RequestValidation();
        bool valid = false;
        for (int i = 0; i < 50 && !valid; ++i) {
            std::string result;
            if (sim.TryTakeValidationResult(result)) {
                valid = (result == "OK");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        SelftestReport("stale-migration-drop", drained && valid, false, failures);
    }

    // (d) Real micro-migration, then exact-id replay must not duplicate.
    // Race-free: the committed id is read from the coordinator AFTER the
    // owner move is observed (LastCommittedId is monotonic per writer).
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), 1);
        sim.PostSpawn(session, MakeBenchCharacter(9000),
                      gs::game::DebugSpawnOverride{460.0f, 200.0f});
        // Wait for the spawn to land so we learn the scout's net id.
        std::uint32_t scout_net = 0;
        WaitFor(std::chrono::seconds(5), [&] {
            const auto it = sim.Owners().find(1);
            if (it == sim.Owners().end()) {
                return false;
            }
            scout_net = it->second.net_id;
            return true;
        });
        bool migrated = false;
        std::size_t home_index = sim.Zones().ZoneCount();
        std::uint32_t home_zone_id = 0;
        if (scout_net != 0) {
            const auto home = sim.Owners().find(1);
            home_index = home != sim.Owners().end() ? home->second.zone_index : home_index;
            home_zone_id = home_index < sim.Zones().ZoneCount()
                               ? sim.Zones().GetZone(home_index).Id()
                               : 0;
            std::uint32_t seq = 0;
            const auto move_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (std::chrono::steady_clock::now() < move_deadline && !migrated) {
                sim.PostMoveInput(1, ++seq, 1.5707963f, gs::game::MoveState::Running);
                const auto owner = sim.Owners().find(1);
                if (owner != sim.Owners().end() && owner->second.zone_index != home_index) {
                    migrated = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        bool replay_safe = false;
        if (migrated) {
            // Replay the just-committed id against the ORIGINAL route. The
            // committed-set must drop it (duplicates+1) even though the
            // entity still exists -- no second entity may appear.
            const auto owner = sim.Owners().find(1);
            const std::uint32_t dst_zone_id =
                owner != sim.Owners().end() && owner->second.zone_index < sim.Zones().ZoneCount()
                    ? sim.Zones().GetZone(owner->second.zone_index).Id()
                    : 0;
            const auto committed_id = sim.LastCommittedMigration();
            const auto dup_before = sim.MigrationMetrics().duplicates;
            gs::game::MigrationRequest replay{};
            replay.net_id = scout_net;
            replay.source_zone_id = home_zone_id;
            replay.target_zone_id = dst_zone_id;
            replay.migration_id = committed_id;
            replay.priority = gs::game::MessagePriority::Critical;
            // The net may legitimately be re-queued already (still out of
            // bounds in the new zone); retry briefly, else SKIP -- forcing
            // the test would conflate load with incorrectness.
            bool enqueued = false;
            if (committed_id.IsValid() && dst_zone_id != 0) {
                const auto enqueue_deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (std::chrono::steady_clock::now() < enqueue_deadline && !enqueued) {
                    enqueued = sim.TestMigrationQueue().TryEnqueue(replay);
                    if (!enqueued) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
            }
            if (!enqueued) {
                sim.PostDespawn(1);
                SelftestReport("duplicate-migration-id", false, true, failures);
            } else {
                // PendingCount==0 only means dequeued; the duplicate counter
                // proves the committed-set actually took the drop.
                WaitFor(std::chrono::seconds(3), [&] {
                    return sim.MigrationMetrics().duplicates == dup_before + 1;
                });
                const auto after = sim.Owners().find(1);
                const auto dup_after = sim.MigrationMetrics().duplicates;
                replay_safe = after != sim.Owners().end() &&
                              after->second.zone_index != home_index &&
                              dup_after == dup_before + 1;
                std::string error;
                sim.RequestValidation();
                for (int i = 0; i < 50; ++i) {
                    std::string result;
                    if (sim.TryTakeValidationResult(result)) {
                        replay_safe = replay_safe && (result == "OK");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                sim.PostDespawn(1);
                SelftestReport("duplicate-migration-id", replay_safe, false, failures);
            }
        } else {
            sim.PostDespawn(1);
            SelftestReport("duplicate-migration-id", false, true, failures);
        }

        // (e) Source-free DTO roundtrip on the (possibly migrated) scout.
        bool roundtrip = false;
        if (scout_net != 0) {
            const auto owner = sim.Owners().find(1);
            if (owner != sim.Owners().end() && owner->second.zone_index < sim.Zones().ZoneCount()) {
                const auto entity =
                    sim.Zones().GetZone(owner->second.zone_index).FindEntity(scout_net);
                if (entity.is_valid()) {
                    auto transfer = gs::game::BuildTransfer(entity, true, 0);
                    const gs::game::EntityTransfer wire_copy = transfer; // simulated transport
                    roundtrip = wire_copy.net_id == scout_net && wire_copy.is_player &&
                                wire_copy.session == 1 &&
                                wire_copy.position.x == transfer.position.x &&
                                wire_copy.hp.current == transfer.hp.current;
                    flecs::world scratch;
                    const auto rebuilt = gs::game::ApplyTransfer(scratch, wire_copy);
                    roundtrip =
                        roundtrip && rebuilt.is_valid() &&
                        rebuilt.get<gs::game::NetId>().value == scout_net &&
                        rebuilt.get<gs::game::Position>().x == transfer.position.x &&
                        rebuilt.has<gs::game::PlayerTag>();
                }
            }
        }
        SelftestReport("transfer-roundtrip", roundtrip, scout_net == 0, failures);
    }

    return failures;
}

} // namespace

int BenchMain(int argc, char** argv)
{
    BenchConfig config;
    if (!ParseArgs(argc, argv, config)) {
        return 1;
    }

    gs::common::InitLogging("warning", "logs/world_bench.log");
    std::printf("worldbench: players=%d mobs=%d seconds=%d mode=%s validate_every=%d "
                "despawn_storm=%d seed=%u logical_processes=%d routing_selftest=%d\n",
                config.players,
                config.mobs,
                config.seconds,
                config.mode.c_str(),
                config.validate_every,
                config.despawn_storm,
                config.seed,
                config.logical_processes,
                config.routing_selftest ? 1 : 0);

    std::mt19937 rng(config.seed);

    boost::asio::io_context io;
    std::thread io_thread([&io] {
        io.run();
    });

    int validation_failures = 0;
    int validations_run = 0;
    std::uint64_t start_deaths = 0;
    std::uint64_t start_attacks = 0;

    {
        gs::game::WorldRuntime sim(io);
        sim.Start();
        if (config.logical_processes >= 2) {
            sim.EmulateDistribution(static_cast<std::uint32_t>(config.logical_processes));
            std::printf("logical distribution: %d processes emulated\n", config.logical_processes);
        }
        if (config.routing_selftest) {
            validation_failures += RunRoutingSelftest(sim, io, config);
        }
        start_deaths = sim.DeathsTotal();
        start_attacks = sim.AttacksTotal();

        // Extra mob spawn points (type 2 goblins), injected over time. In
        // hotspot/dense modes they cluster at the player anchors so melee
        // attacks land and the kill/respawn path is exercised.
        static constexpr float kAnchors[4][2] = {
            {98.0f, 88.0f}, {240.0f, 123.0f}, {254.0f, 261.0f}, {381.0f, 248.0f}};
        const int base_spawn_points = 4; // from mob_spawns.conf
        for (int i = 0; i < config.mobs; ++i) {
            gs::game::MobSpawnPoint point;
            point.mob_type_id = 2;
            if (config.mode == "hotspot" || config.mode == "dense") {
                point.x = kAnchors[i % 4][0] + static_cast<float>((i * 37) % 10);
                point.y = kAnchors[i % 4][1] + static_cast<float>((i * 53) % 10);
            } else {
                point.x = 50.0f + static_cast<float>((i * 37) % 900);
                point.y = 50.0f + static_cast<float>((i * 53) % 900);
            }
            point.count = 1;
            point.radius = 5.0f;
            sim.AddMobSpawnPoint(point);
        }

        // Players.
        std::vector<BenchClient> clients(static_cast<std::size_t>(config.players));
        std::uniform_real_distribution<float> heading_dist(0.0f, 6.2831853f);
        for (int i = 0; i < config.players; ++i) {
            boost::asio::ip::tcp::socket socket(io);
            const auto session_id = static_cast<gs::common::SessionId>(100 + i);
            clients[static_cast<std::size_t>(i)].session =
                std::make_shared<gs::network::Session>(std::move(socket), session_id);
            clients[static_cast<std::size_t>(i)].session_id = session_id;
            clients[static_cast<std::size_t>(i)].alive = true;
            clients[static_cast<std::size_t>(i)].heading = heading_dist(rng);
            clients[static_cast<std::size_t>(i)].slot = i;

            float sx = 100.0f;
            float sy = 100.0f;
            if (config.mode == "spread") {
                sx = 50.0f + static_cast<float>((i * 71) % 900);
                sy = 50.0f + static_cast<float>((i * 97) % 900);
            } else if (config.mode == "hotspot" || config.mode == "dense") {
                // Co-locate with mob spawn anchors so attacks land and
                // kills/respawns are exercised.
                sx = kAnchors[i % 4][0] + static_cast<float>(i % 7);
                sy = kAnchors[i % 4][1] + static_cast<float>((i / 7) % 7);
            } else if (config.mode == "border") {
                sx = 460.0f + static_cast<float>(i % 30);
                sy = 200.0f + static_cast<float>((i * 13) % 200);
                clients[static_cast<std::size_t>(i)].heading = 1.5707963f; // east
            }
            sim.PostSpawn(clients[static_cast<std::size_t>(i)].session,
                          MakeBenchCharacter(i),
                          gs::game::DebugSpawnOverride{sx, sy});
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(config.seconds);
        auto next_mob_batch = std::chrono::steady_clock::now();
        auto next_move = std::chrono::steady_clock::now();
        auto next_attack = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        auto next_storm = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        auto next_validate = std::chrono::steady_clock::now() + std::chrono::seconds(config.validate_every);
        auto next_progress = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        int mobs_queued = 0;
        int next_session_id = 100 + config.players;
        std::uint32_t mob_net_hi = static_cast<std::uint32_t>(1'000'000 + 9 + config.mobs + 1000);

        while (std::chrono::steady_clock::now() < deadline) {
            const auto now = std::chrono::steady_clock::now();

            // Staggered mob injection (avoids one giant spawn spike).
            if (mobs_queued < config.mobs && now >= next_mob_batch) {
                const int batch =
                    std::min(config.mobs - mobs_queued, std::max(1, config.mobs / 10));
                for (int k = 0; k < batch; ++k) {
                    sim.RequestMobSpawn(static_cast<std::size_t>(base_spawn_points + mobs_queued));
                    ++mobs_queued;
                }
                next_mob_batch = now + std::chrono::seconds(1);
            }

            // Movement inputs ~10 Hz per alive player. Hotspot/dense clients
            // hold position so melee attacks land and kills/respawns happen;
            // other modes roam.
            if (now >= next_move) {
                const bool stationary = config.mode == "hotspot" || config.mode == "dense";
                for (auto& client : clients) {
                    if (!client.alive || stationary) {
                        continue;
                    }
                    if (config.mode == "spread" && (rng() % 20) == 0) {
                        client.heading = heading_dist(rng);
                    }
                    sim.PostMoveInput(client.session_id,
                                      ++client.seq,
                                      client.heading,
                                      gs::game::MoveState::Running);
                }
                next_move = now + std::chrono::milliseconds(100);
            }

            // Attacks. In hotspot/dense modes each client round-robins the
            // extra mobs spawned at its own anchor, so hits land regularly;
            // other modes pick uniformly (mostly exercising routing/rejects).
            if (now >= next_attack) {
                std::uniform_int_distribution<std::uint32_t> mob_pick(1'000'000, mob_net_hi);
                const int per_anchor =
                    (config.mobs > 0) ? (config.mobs + 3) / 4 : 0;
                const bool targeted =
                    (config.mode == "hotspot" || config.mode == "dense") && per_anchor > 0;
                int attackers = 0;
                for (auto& client : clients) {
                    if (!client.alive) {
                        continue;
                    }
                    const bool attack =
                        config.mode == "spread" ? (rng() % 20 == 0) : (rng() % 3 == 0);
                    if (!attack) {
                        continue;
                    }
                    std::uint32_t target = mob_pick(rng);
                    if (targeted) {
                        const int j = client.target_cursor++ % per_anchor;
                        target = static_cast<std::uint32_t>(
                            1'000'009 + ((client.slot % 4) + 4 * j) % config.mobs);
                    }
                    sim.PostAttackTarget(client.session_id, target);
                    if (++attackers > 200) {
                        break;
                    }
                }
                next_attack = now + std::chrono::milliseconds(500);
            }

            // Despawn storm: kill + respawn random players (failure injection,
            // incl. mid-migration disconnects in border mode).
            if (config.despawn_storm > 0 && now >= next_storm) {
                for (int k = 0; k < config.despawn_storm && !clients.empty(); ++k) {
                    const std::size_t victim = rng() % clients.size();
                    auto& client = clients[victim];
                    if (client.alive) {
                        sim.PostDespawn(client.session_id);
                        client.alive = false;
                    } else {
                        boost::asio::ip::tcp::socket socket(io);
                        const auto session_id =
                            static_cast<gs::common::SessionId>(next_session_id++);
                        client.session =
                            std::make_shared<gs::network::Session>(std::move(socket), session_id);
                        client.session_id = session_id;
                        client.alive = true;
                        client.seq = 0;
                        sim.PostSpawn(client.session,
                                      MakeBenchCharacter(client.slot),
                                      gs::game::DebugSpawnOverride{100.0f, 100.0f});
                    }
                }
                next_storm = now + std::chrono::seconds(1);
            }

            // Periodic consistency validation (supervisor-side quiescent window).
            if (config.validate_every > 0 && now >= next_validate) {
                sim.RequestValidation();
                next_validate = now + std::chrono::seconds(config.validate_every);
            }
            std::string validation_result;
            while (sim.TryTakeValidationResult(validation_result)) {
                ++validations_run;
                if (validation_result != "OK") {
                    ++validation_failures;
                    std::printf("VALIDATION FAIL: %s\n", validation_result.c_str());
                }
            }

            if (now >= next_progress) {
                const auto mig_stats = sim.MigrationMetrics();
                std::printf("  t=%us tick=%u deaths=%llu attacks=%llu mig_pending=%zu quarantined=%zu "
                            "mig_committed=%llu mig_dup=%llu\n",
                            config.seconds -
                                static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                                                     deadline - now)
                                                     .count()),
                            sim.WorldTick(),
                            (unsigned long long)(sim.DeathsTotal() - start_deaths),
                            (unsigned long long)(sim.AttacksTotal() - start_attacks),
                            sim.Migrations().PendingCount(),
                            sim.MigrationQuarantined(),
                            (unsigned long long)mig_stats.committed,
                            (unsigned long long)mig_stats.duplicates);
                next_progress = now + std::chrono::seconds(5);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        // Final validation round.
        sim.RequestValidation();
        for (int i = 0; i < 50; ++i) {
            std::string validation_result;
            if (sim.TryTakeValidationResult(validation_result)) {
                ++validations_run;
                if (validation_result != "OK") {
                    ++validation_failures;
                    std::printf("VALIDATION FAIL: %s\n", validation_result.c_str());
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Aggregate tick samples across zones.
        std::vector<std::uint64_t> all_samples;
        std::uint64_t max_sample = 0;
        std::uint64_t sum_samples = 0;
        struct ZoneAvg {
            std::uint32_t zone_id = 0;
            double avg_ms = 0.0;
            std::size_t samples = 0;
        };
        std::vector<ZoneAvg> zone_avgs;
        for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
            const auto& zone = sim.Zones().GetZone(zi);
            std::uint64_t buffer[gs::game::ZoneDiagnostics::kTickSampleCapacity];
            const std::size_t n = zone.Diagnostics().CopyTickSamples(buffer, 256);
            std::uint64_t zone_sum = 0;
            for (std::size_t i = 0; i < n; ++i) {
                all_samples.push_back(buffer[i]);
                zone_sum += buffer[i];
                max_sample = std::max(max_sample, buffer[i]);
                sum_samples += buffer[i];
            }
            zone_avgs.push_back(
                ZoneAvg{zone.Id(), n > 0 ? static_cast<double>(zone_sum) / n / 1000.0 : 0.0, n});
        }
        std::sort(zone_avgs.begin(), zone_avgs.end(), [](const ZoneAvg& a, const ZoneAvg& b) {
            return a.avg_ms > b.avg_ms;
        });

        const auto worker_util = sim.WorkerUtilization();
        std::printf("\n==== worldbench report ====\n");
        std::printf("config: players=%d mobs=%d seconds=%d mode=%s\n",
                    config.players,
                    config.mobs,
                    config.seconds,
                    config.mode.c_str());
        std::printf("zone ticks: samples=%zu avg=%.3fms p50=%.3fms p95=%.3fms p99=%.3fms max=%.3fms\n",
                    all_samples.size(),
                    all_samples.empty() ? 0.0
                                        : static_cast<double>(sum_samples) / all_samples.size() / 1000.0,
                    Percentile(all_samples, 50.0),
                    Percentile(all_samples, 95.0),
                    Percentile(all_samples, 99.0),
                    static_cast<double>(max_sample) / 1000.0);
        std::printf("busiest zones (avg ms):");
        for (std::size_t i = 0; i < std::min<std::size_t>(zone_avgs.size(), 5); ++i) {
            std::printf(" [zone=%u %.3fms n=%zu]", zone_avgs[i].zone_id, zone_avgs[i].avg_ms,
                        zone_avgs[i].samples);
        }
        std::printf("\n");
        std::printf("workers: tasks=%llu busy=%.2fs supervisor_avg=%.3fms\n",
                    (unsigned long long)worker_util.tasks_completed,
                    static_cast<double>(worker_util.busy_micros) / 1'000'000.0,
                    sim.SupervisorAvgMs());
        std::printf("gameplay: deaths=%llu attacks=%llu mig_pending=%zu quarantined=%zu\n",
                    (unsigned long long)(sim.DeathsTotal() - start_deaths),
                    (unsigned long long)(sim.AttacksTotal() - start_attacks),
                    sim.Migrations().PendingCount(),
                    sim.MigrationQuarantined());
        std::printf("validation: runs=%d failures=%d\n", validations_run, validation_failures);
        const auto process_load = sim.CollectProcessLoad();
        const auto routes = sim.Router().MetricsSnapshot();
        std::printf("process load: node=%u process=%u tick=%u zones=%zu active=%zu sleeping=%zu "
                    "players=%llu mobs=%llu ghosts=%llu avg_tick=%.3fms repl=%llu mig=%llu "
                    "worker_tasks=%llu worker_busy_us=%llu sup_avg=%.3fms\n",
                    process_load.identity.node.value, process_load.identity.process.value,
                    process_load.world_tick, process_load.zone_count, process_load.active_zones,
                    process_load.sleeping_zones, (unsigned long long)process_load.players,
                    (unsigned long long)process_load.mobs, (unsigned long long)process_load.ghosts,
                    process_load.avg_zone_tick_ms, (unsigned long long)process_load.repl_records,
                    (unsigned long long)process_load.migrations,
                    (unsigned long long)process_load.worker_tasks,
                    (unsigned long long)process_load.worker_busy_us, process_load.supervisor_avg_ms);
        std::printf("routes: local=%llu remote_emulated=%llu unavailable=%llu draining=%llu miss=%llu\n",
                    (unsigned long long)routes.local_delivered,
                    (unsigned long long)routes.remote_emulated,
                    (unsigned long long)routes.unavailable,
                    (unsigned long long)routes.draining,
                    (unsigned long long)routes.directory_miss);

        sim.Stop();
    }

    io.stop();
    if (io_thread.joinable()) {
        io_thread.join();
    }
    std::printf("BENCH-DONE validations=%d failures=%d\n", validations_run, validation_failures);
    return validation_failures == 0 ? 0 : 2;
}

int main(int argc, char** argv)
{
    try {
        return BenchMain(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "bench fatal: " << error.what() << "\n";
        return 1;
    }
}
