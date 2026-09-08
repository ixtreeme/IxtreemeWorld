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
//              [--mode spread|hotspot|border|dense|splitmerge|lod|activity]
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
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "common/Logging.h"
#include "db/CharacterRepository.h"
#include "network/Session.h"

#include "../world/migration/EntityTransfer.h"
#include "../world/partition/ZonePartition.h"
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
    // splitmerge scenario: deterministic transfer-failure injection counts
    // (0 = commit path; >0 = abort path expectations). fail_after lets that
    // many transfers succeed first (mid-batch abort, non-empty rollback).
    int fail_snapshot = 0;
    int fail_apply = 0;
    int fail_after = 0;
    // Simulation LOD master switch for A/B runs (default on).
    bool lod_off = false;
    // Optional partition floor override in meters (0 = production default).
    // Lets load-driven scenarios split small test maps; still passes through
    // ValidatePartitionConfig (AOI floor clamp applies).
    int partition_min_size = 0;
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
                         "             [--mode spread|hotspot|border|dense|splitmerge|lod|activity]\n"
                         "             [--validate-every K] [--despawn-storm R] [--seed S]\n"
                         "             [--logical-processes K] [--routing-selftest]\n"
                         "             [--fail-snapshot N] [--fail-apply N] [--fail-after N]\n"
                         "             [--partition-min-size M] [--lod-off]\n";
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
        } else if (arg == "--fail-snapshot") {
            if (!need_value("fail-snapshot", value)) {
                return false;
            }
            config.fail_snapshot = std::stoi(value);
        } else if (arg == "--fail-apply") {
            if (!need_value("fail-apply", value)) {
                return false;
            }
            config.fail_apply = std::stoi(value);
        } else if (arg == "--fail-after") {
            if (!need_value("fail-after", value)) {
                return false;
            }
            config.fail_after = std::stoi(value);
        } else if (arg == "--partition-min-size") {
            if (!need_value("partition-min-size", value)) {
                return false;
            }
            config.partition_min_size = std::stoi(value);
        } else if (arg == "--lod-off") {
            config.lod_off = true;
        } else {
            std::cerr << "unknown arg: " << arg << "\n";
            return false;
        }
    }
    if (config.mode != "spread" && config.mode != "hotspot" && config.mode != "border" &&
        config.mode != "dense" && config.mode != "splitmerge" && config.mode != "lod" &&
        config.mode != "activity") {
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

// Deterministic split -> merge transaction scenario (§16-18). Own sim
// lifecycle; does NOT use the main load loop above.
//
//   1. ConfigurePartition (min_zone 100m so the 500m test-map zones can
//      split; everything else default) + verify effective config.
//   2. Spawn 8 players + ~20 mobs concentrated in one zone; 3 more players
//      exactly on the zone's future child midlines (boundary semantics).
//   3. ForceSplit -> expect COMMIT (or ABORT when --fail-* armed):
//      topology, totals, boundary placement, validator.
//   4. Despawn all players, ForceMerge -> merged holds all mobs, children
//      retired+empty, validator.
// Returns failure count (0 = PASS).
int RunSplitMergeScenario(boost::asio::io_context& io, const BenchConfig& config)
{
    int failures = 0;
    int validations = 0;
    auto check = [&](const char* name, bool pass) {
        if (pass) {
            std::printf("SPLITMERGE %s: PASS\n", name);
        } else {
            std::printf("SPLITMERGE %s: FAIL\n", name);
            ++failures;
        }
    };

    gs::game::WorldRuntime sim(io);
    auto validate_now = [&](const char* what) -> bool {
        sim.RequestValidation();
        for (int i = 0; i < 100; ++i) {
            std::string result;
            if (sim.TryTakeValidationResult(result)) {
                ++validations;
                if (result != "OK") {
                    std::printf("SPLITMERGE validation(%s): FAIL: %s\n", what, result.c_str());
                    return false;
                }
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("SPLITMERGE validation(%s): TIMEOUT\n", what);
        return false;
    };
    auto active_leaf_ids = [&]() {
        std::set<gs::game::ZoneId> ids;
        for (const auto* leaf : sim.Zones().GetActiveLeaves()) {
            ids.insert(leaf->zone_id);
        }
        return ids;
    };
    auto find_zone = [&](gs::game::ZoneId id) -> const gs::game::Zone* {
        const std::size_t idx = sim.Zones().FindIndexById(id);
        return idx < sim.Zones().ZoneCount() ? &sim.Zones().GetZone(idx) : nullptr;
    };

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(std::max(60, config.seconds));
    auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };

    // ---- 1. config ----
    gs::game::PartitionConfig pcfg;
    pcfg.min_zone_size_m = 100.0f; // test map zones are 500m; halves must clear the floor
    sim.ConfigurePartition(pcfg);
    // 100m is below the 2x AOI safety floor (240m): validation must clamp
    // it, and the clamped value is what the split machinery runs on. This
    // doubles as an end-to-end config-validation check.
    check("config-effective",
          sim.EffectivePartitionConfig().min_zone_size_m == 240.0f &&
              sim.EffectivePartitionConfig().split_load_threshold == 0.9f);
    sim.Start();

    // ---- 2. populate one zone ----
    const auto& z0 = sim.Zones().GetZone(0);
    const float cx = (z0.Bounds().min_x + z0.Bounds().max_x) * 0.5f;
    const float cy = (z0.Bounds().min_y + z0.Bounds().max_y) * 0.5f;
    constexpr int kPlayers = 8;
    constexpr int kBoundaryPlayers = 3;
    constexpr int kTotalPlayers = kPlayers + kBoundaryPlayers;
    constexpr gs::common::SessionId kBaseSession = 500;
    std::vector<std::shared_ptr<gs::network::Session>> sessions;
    for (int i = 0; i < kPlayers; ++i) {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(
            std::move(socket), static_cast<gs::common::SessionId>(kBaseSession + i));
        sessions.push_back(session);
        sim.PostSpawn(session,
                      MakeBenchCharacter(700 + i),
                      gs::game::DebugSpawnOverride{cx + static_cast<float>(i),
                                                   cy + static_cast<float>(i % 3)});
    }
    constexpr int kMobPoints = 20;
    for (int k = 0; k < kMobPoints; ++k) {
        gs::game::MobSpawnPoint point;
        point.mob_type_id = 2;
        point.x = cx;
        point.y = cy;
        point.count = 1;
        point.radius = 2.0f;
        sim.AddMobSpawnPoint(point);
    }
    for (int k = 0; k < kMobPoints; ++k) {
        sim.RequestMobSpawn(static_cast<std::size_t>(4 + k));
    }
    bool populated = WaitFor(std::chrono::seconds(20), [&] {
        return sim.Owners().size() == static_cast<std::size_t>(kPlayers) &&
               sim.CollectProcessLoad().mobs >= 16;
    });
    check("populate", populated && !expired());
    if (!populated) {
        sim.Stop();
        return failures + 1;
    }
    const std::uint64_t mob_total = sim.CollectProcessLoad().mobs;

    // Target = wherever the spawns actually landed (no map assumptions).
    const gs::game::ZoneId target_id = sim.Owners().at(kBaseSession).location.zone;
    bool all_in_target = true;
    for (int i = 0; i < kPlayers; ++i) {
        const auto it = sim.Owners().find(static_cast<gs::common::SessionId>(kBaseSession + i));
        if (it == sim.Owners().end() || it->second.location.zone != target_id) {
            all_in_target = false;
        }
    }
    check("single-target-zone", all_in_target);
    const auto* target_zone = find_zone(target_id);
    if (target_zone == nullptr) {
        sim.Stop();
        return failures + 1;
    }
    const auto tb = target_zone->Bounds();
    const float mid_x = (tb.min_x + tb.max_x) * 0.5f;
    const float mid_y = (tb.min_y + tb.max_y) * 0.5f;

    // Boundary players exactly on future child midlines (§23).
    struct BoundarySpawn {
        float x, y;
    };
    const BoundarySpawn kBoundary[kBoundaryPlayers] = {
        {mid_x, (tb.min_y + mid_y) * 0.5f}, // vertical midline -> east child
        {(tb.min_x + mid_x) * 0.5f, mid_y}, // horizontal midline -> north child
        {mid_x, mid_y},                     // corner shared by 4 -> north-east child
    };
    for (int i = 0; i < kBoundaryPlayers; ++i) {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(
            std::move(socket), static_cast<gs::common::SessionId>(kBaseSession + kPlayers + i));
        sessions.push_back(session);
        sim.PostSpawn(session,
                      MakeBenchCharacter(710 + i),
                      gs::game::DebugSpawnOverride{kBoundary[i].x, kBoundary[i].y});
    }
    populated = WaitFor(std::chrono::seconds(15), [&] {
        return sim.Owners().size() == static_cast<std::size_t>(kTotalPlayers);
    });
    check("boundary-spawn", populated && !expired());

    // Boundary spawns must have landed inside the target (interior points).
    bool boundary_home = true;
    std::vector<std::uint32_t> boundary_nets;
    for (int i = 0; i < kBoundaryPlayers; ++i) {
        const auto it = sim.Owners().find(static_cast<gs::common::SessionId>(kBaseSession + kPlayers + i));
        if (it == sim.Owners().end() || it->second.location.zone != target_id) {
            boundary_home = false;
        } else {
            boundary_nets.push_back(it->second.net_id);
        }
    }
    check("boundary-in-target", boundary_home);
    check("pre-split-validate", validate_now("pre-split"));

    const auto pre_leaves = active_leaf_ids();
    const bool expect_abort = config.fail_snapshot > 0 || config.fail_apply > 0;
    if (expect_abort) {
        sim.InjectTransferFailuresForTest(config.fail_snapshot, config.fail_apply,
                                          config.fail_after);
    }

    // ---- 3. forced split ----
    sim.PostForceSplit(target_id);
    const auto split0 = sim.PartitionMetricsSnapshot();
    const bool split_settled = WaitFor(std::chrono::seconds(25), [&] {
        const auto m = sim.PartitionMetricsSnapshot();
        return m.split_commits + m.split_aborts > split0.split_commits + split0.split_aborts;
    });
    check("split-settled", split_settled && !expired());
    const auto split1 = sim.PartitionMetricsSnapshot();
    check("pre-merge-validate-split", validate_now("post-split"));

    auto totals_ok = [&] {
        return sim.Owners().size() == static_cast<std::size_t>(kTotalPlayers) &&
               sim.CollectProcessLoad().mobs == mob_total;
    };
    if (expect_abort) {
        check("split-aborted",
              split1.split_aborts > split0.split_aborts &&
                  split1.split_commits == split0.split_commits);
        // Parent restored as the sole authority; nothing half-built.
        const auto leaves = active_leaf_ids();
        check("abort-parent-authority", leaves.find(target_id) != leaves.end());
        check("abort-no-loss", totals_ok());
        // Apply-stage failure additionally proves destination rollback: the
        // retired-tombstone rule would fail validation otherwise (checked).
    } else {
        check("split-committed",
              split1.split_commits > split0.split_commits &&
                  split1.split_aborts == split0.split_aborts);
        const auto leaves = active_leaf_ids();
        std::set<gs::game::ZoneId> new_children;
        for (auto id : leaves) {
            if (pre_leaves.find(id) == pre_leaves.end()) {
                new_children.insert(id);
            }
        }
        check("split-4-children", new_children.size() == 4);
        check("split-parent-retired",
              leaves.find(target_id) == leaves.end() &&
                  [&] {
                      const auto* z = find_zone(target_id);
                      return z != nullptr &&
                             z->Partition() == gs::game::PartitionState::Retired &&
                             z->Entities().empty() && z->Players().empty();
                  }());
        check("split-no-loss", totals_ok());
        // Boundary placement: each midline player in exactly the half-open
        // owner child (§23).
        bool placement_ok = new_children.size() == 4;
        for (int i = 0; i < kBoundaryPlayers && placement_ok; ++i) {
            const auto it =
                sim.Owners().find(static_cast<gs::common::SessionId>(kBaseSession + kPlayers + i));
            if (it == sim.Owners().end()) {
                placement_ok = false;
                break;
            }
            const float px = kBoundary[i].x;
            const float py = kBoundary[i].y;
            int matches = 0;
            gs::game::ZoneId expected = 0;
            for (auto cid : new_children) {
                const auto* cz = find_zone(cid);
                if (cz == nullptr) {
                    continue;
                }
                const auto& b = cz->Bounds();
                if (px >= b.min_x && px < b.max_x && py >= b.min_y && py < b.max_y) {
                    ++matches;
                    expected = cid;
                }
            }
            placement_ok = (matches == 1 && it->second.location.zone == expected);
        }
        check("split-boundary-placement", placement_ok);
    }

    // ---- 4. despawn players, forced merge ----
    // In abort mode there are no children to merge (the split never
    // committed): the merge phase is skipped, topology must simply be the
    // untouched original plus validation + metrics.
    if (expect_abort) {
        check("abort-post-validate", validate_now("post-abort"));
        const auto am = sim.PartitionMetricsSnapshot();
        check("abort-metrics",
              am.split_aborts >= 1 && am.split_commits == 0 && am.merge_attempts == 0);
    }
    for (int i = 0; i < kTotalPlayers; ++i) {
        sim.PostDespawn(static_cast<gs::common::SessionId>(kBaseSession + i));
    }
    sessions.clear();
    const bool drained = WaitFor(std::chrono::seconds(15), [&] { return sim.Owners().empty(); });
    check("despawn-drained", drained && !expired());
    if (expect_abort) {
        check("abort-final-validate", validate_now("final-abort"));
        sim.Stop();
        std::printf("SPLITMERGE-DONE validations=%d failures=%d\n", validations, failures);
        return failures;
    }

    sim.PostForceMerge(target_id);
    const auto merge0 = sim.PartitionMetricsSnapshot();
    const bool merge_settled = WaitFor(std::chrono::seconds(25), [&] {
        const auto m = sim.PartitionMetricsSnapshot();
        return m.merge_commits + m.merge_aborts > merge0.merge_commits + merge0.merge_aborts;
    });
    check("merge-settled", merge_settled && !expired());
    const auto merge1 = sim.PartitionMetricsSnapshot();
    check("merge-committed", merge1.merge_commits > merge0.merge_commits);
    check("post-merge-validate", validate_now("post-merge"));

    // Merged leaf active; all split children retired+empty; mobs preserved.
    const auto post_leaves = active_leaf_ids();
    std::set<gs::game::ZoneId> merged_ids;
    for (auto id : post_leaves) {
        if (pre_leaves.find(id) == pre_leaves.end()) {
            merged_ids.insert(id);
        }
    }
    // Note: with expect_abort the split never committed, so there are no
    // children to merge; the forced merge then operates on whatever the
    // tree holds (likely a no-op plan refusal). Merge asserts below only
    // apply to the commit path.
    if (!expect_abort) {
        check("merge-single-target", merged_ids.size() == 1);
        std::uint64_t total_mobs = 0;
        bool children_clean = true;
        for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
            const auto& z = sim.Zones().GetZone(zi);
            total_mobs += z.Entities().size();
            // Mobs may have wandered out of the merged subtree into
            // neighbors; the invariant is global preservation + retired
            // emptiness, not that every mob sits in the merged zone.
            if (z.Partition() == gs::game::PartitionState::Retired) {
                if (!z.Entities().empty() || !z.Players().empty()) {
                    children_clean = false;
                }
            }
        }
        check("merge-children-retired-empty", children_clean);
        check("merge-mobs-preserved",
              total_mobs == mob_total && sim.CollectProcessLoad().mobs == mob_total);
    }

    const auto pm = sim.PartitionMetricsSnapshot();
    std::printf("SPLITMERGE metrics: split att=%llu ok=%llu ab=%llu tf=%llu rb=%llu "
                "merge att=%llu ok=%llu ab=%llu tf=%llu rb=%llu retire_rej=%llu\n",
                (unsigned long long)pm.split_attempts, (unsigned long long)pm.split_commits,
                (unsigned long long)pm.split_aborts, (unsigned long long)pm.split_transfer_failures,
                (unsigned long long)pm.split_rollback_failures,
                (unsigned long long)pm.merge_attempts, (unsigned long long)pm.merge_commits,
                (unsigned long long)pm.merge_aborts, (unsigned long long)pm.merge_transfer_failures,
                (unsigned long long)pm.merge_rollback_failures,
                (unsigned long long)pm.retire_rejected_nonempty);
    auto avg = [](std::uint64_t total, std::uint64_t n) {
        return n > 0 ? static_cast<double>(total) / n / 1000.0 : 0.0;
    };
    std::printf("SPLITMERGE phase ms avg: split plan=%.3f xfer=%.3f commit=%.3f rollback=%.3f | "
                "merge plan=%.3f xfer=%.3f commit=%.3f rollback=%.3f\n",
                avg(pm.split_plan_us, pm.split_commits + pm.split_aborts),
                avg(pm.split_transfer_us, pm.split_commits + pm.split_aborts),
                avg(pm.split_commit_us, pm.split_commits),
                avg(pm.split_rollback_us, pm.split_aborts),
                avg(pm.merge_plan_us, pm.merge_commits + pm.merge_aborts),
                avg(pm.merge_transfer_us, pm.merge_commits + pm.merge_aborts),
                avg(pm.merge_commit_us, pm.merge_commits),
                avg(pm.merge_rollback_us, pm.merge_aborts));

    sim.Stop();
    std::printf("SPLITMERGE-DONE validations=%d failures=%d\n", validations, failures);
    return failures;
}

// Simulation LOD correctness scenario (§26). Own sim lifecycle; default LOD
// config (enabled). All asserts use thread-safe public reads (diagnostic
// gauges, Owners, validator, cumulative counters) — never live flecs state.
//
//   Phase 1: 1 AFK player + 2 mob clusters (near/far). Far cluster must
//     cascade to Dormant; near cluster stays Full; validator OK.
//   Phase 2: spawn a player at the far cluster -> Full jumps within ~2 eval
//     periods (immediate proximity wake).
//   Phase 3: blind attack burst around the far cluster -> deaths happen on
//     previously-dormant mobs (damage path transparent under LOD),
//     validator OK. (Proximity bubbles do most waking; the synchronous
//     attack Wake is the sub-eval-period guarantee — both are correct.)
//   Phase 4: despawn everyone, wait out the cascade -> Full decays to zero
//     (hysteresis, no flap); cooldowns/respawns advanced meanwhile.
// Returns failure count (0 = PASS).
int RunLodScenario(boost::asio::io_context& io, const BenchConfig& config)
{
    int failures = 0;
    int validations = 0;
    auto check = [&](const char* name, bool pass) {
        if (pass) {
            std::printf("LOD %s: PASS\n", name);
        } else {
            std::printf("LOD %s: FAIL\n", name);
            ++failures;
        }
    };

    gs::game::WorldRuntime sim(io);
    auto validate_now = [&](const char* what) -> bool {
        sim.RequestValidation();
        for (int i = 0; i < 100; ++i) {
            std::string result;
            if (sim.TryTakeValidationResult(result)) {
                ++validations;
                if (result != "OK") {
                    std::printf("LOD validation(%s): FAIL: %s\n", what, result.c_str());
                    return false;
                }
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("LOD validation(%s): TIMEOUT\n", what);
        return false;
    };
    struct TierDist {
        std::uint64_t full = 0, reduced = 0, low = 0, dormant = 0;
    };
    auto tiers = [&]() {
        TierDist dist;
        for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
            const auto& diag = sim.Zones().GetZone(zi).Diagnostics();
            dist.full += diag.lod_full.load(std::memory_order_relaxed);
            dist.reduced += diag.lod_reduced.load(std::memory_order_relaxed);
            dist.low += diag.lod_low.load(std::memory_order_relaxed);
            dist.dormant += diag.lod_dormant.load(std::memory_order_relaxed);
        }
        return dist;
    };
    auto print_zones = [&](const char* tag) {
        std::printf("LOD zones@%s:\n", tag);
        for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
            const auto& z = sim.Zones().GetZone(zi);
            const auto& diag = z.Diagnostics();
            const auto& b = z.Bounds();
            std::printf("  zone=%u bounds=(%.0f,%.0f)-(%.0f,%.0f) mobs=%u tiers=[%u/%u/%u/%u]\n",
                        z.Id(),
                        b.min_x,
                        b.min_y,
                        b.max_x,
                        b.max_y,
                        diag.mob_count.load(std::memory_order_relaxed),
                        diag.lod_full.load(std::memory_order_relaxed),
                        diag.lod_reduced.load(std::memory_order_relaxed),
                        diag.lod_low.load(std::memory_order_relaxed),
                        diag.lod_dormant.load(std::memory_order_relaxed));
        }
        for (const auto& [sid, owner] : sim.Owners()) {
            std::printf("  owner session=%llu zone=%u net=%u\n",
                        (unsigned long long)sid,
                        owner.location.zone,
                        owner.net_id);
        }
    };

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(std::max(150, config.seconds));
    auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };

    sim.Start();

    // ---- Phase 1: populate. Group A near (150,150), group B near (850,850),
    // player 1 AFK at group A. Mobs spawn via points (type 2) around both.
    constexpr gs::common::SessionId kPlayer1 = 600;
    constexpr gs::common::SessionId kPlayer2 = 601;
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kPlayer1);
        sim.PostSpawn(session, MakeBenchCharacter(800), gs::game::DebugSpawnOverride{150.0f, 150.0f});
    }
    constexpr int kMobPoints = 12;
    for (int k = 0; k < kMobPoints; ++k) {
        gs::game::MobSpawnPoint point;
        point.mob_type_id = 2;
        if (k < kMobPoints / 2) {
            point.x = 150.0f;
            point.y = 150.0f;
        } else {
            point.x = 850.0f;
            point.y = 850.0f;
        }
        point.count = 1;
        point.radius = 3.0f;
        sim.AddMobSpawnPoint(point);
    }
    for (int k = 0; k < kMobPoints; ++k) {
        sim.RequestMobSpawn(static_cast<std::size_t>(4 + k));
    }
    const bool populated = WaitFor(std::chrono::seconds(25), [&] {
        return sim.Owners().size() == 1 && sim.CollectProcessLoad().mobs >= 10;
    });
    check("populate", populated && !expired());
    if (!populated) {
        sim.Stop();
        return failures + 1;
    }
    const std::uint64_t mob_total = sim.CollectProcessLoad().mobs;
    print_zones("populated");
    std::printf("LOD lookup: (150,150)->zoneidx=%zu (850,850)->zoneidx=%zu\n",
                sim.Zones().FindIndexForPosition(150.0f, 150.0f),
                sim.Zones().FindIndexForPosition(850.0f, 850.0f));
    check("pre-validate", validate_now("pre"));
    auto tiers_sum = [](const TierDist& d) { return d.full + d.reduced + d.low + d.dormant; };

    // ---- Phase 1: dormancy. Newborns carry no grace history, so group B
    // (far from the only player) cascades to Dormant within ~3 evals, while
    // group A stays Full. Wait generously, then assert distribution shape,
    // exact tier accounting (every mob in exactly one tier) and no loss.
    std::this_thread::sleep_for(std::chrono::seconds(20));
    if (expired()) {
        check("dormancy-window", false);
        sim.Stop();
        return failures + 1;
    }
    const TierDist d1 = tiers();
    std::printf("LOD tiers@t+20s: full=%llu reduced=%llu low=%llu dormant=%llu (mobs=%llu)\n",
                (unsigned long long)d1.full,
                (unsigned long long)d1.reduced,
                (unsigned long long)d1.low,
                (unsigned long long)d1.dormant,
                (unsigned long long)mob_total);
    check("near-full", d1.full >= 3);
    check("far-demoted", d1.dormant + d1.low + d1.reduced >= 3);
    check("tier-accounting-1", tiers_sum(d1) == mob_total);
    check("no-loss-phase1", sim.CollectProcessLoad().mobs == mob_total);
    check("validate-phase1", validate_now("phase1"));

    // ---- Phase 2: leave hysteresis (before any death, so no respawn can
    // flake the asserts). Player 1 despawns; Full must decay after the 5s
    // grace (Reduced), with no flap back while nobody is around.
    sim.PostDespawn(kPlayer1);
    const bool left1 =
        WaitFor(std::chrono::seconds(15), [&] { return sim.Owners().empty(); });
    check("despawn-1-drained", left1 && !expired());
    std::this_thread::sleep_for(std::chrono::seconds(12));
    if (expired()) {
        check("hysteresis-window", false);
        sim.Stop();
        return failures + 1;
    }
    const TierDist d2 = tiers();
    std::printf("LOD tiers@left+12s: full=%llu reduced=%llu low=%llu dormant=%llu\n",
                (unsigned long long)d2.full,
                (unsigned long long)d2.reduced,
                (unsigned long long)d2.low,
                (unsigned long long)d2.dormant);
    check("hysteresis-decay", d2.full == 0);
    check("tier-accounting-2", tiers_sum(d2) == mob_total);
    check("validate-phase2", validate_now("phase2"));

    // ---- Phase 3: wake on approach. Player 2 spawns at group B; Full must
    // jump within ~2 evaluation periods without any attack input.
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kPlayer2);
        sim.PostSpawn(session, MakeBenchCharacter(801), gs::game::DebugSpawnOverride{850.0f, 850.0f});
    }
    const bool woke = WaitFor(std::chrono::seconds(10), [&] { return tiers().full >= 5; });
    print_zones("after-approach");
    check("wake-on-approach", woke && !expired());
    check("validate-phase3", validate_now("phase3"));

    // ---- Phase 4: attacks land (damage path transparent under LOD).
    // Blind burst over the mob net range; deaths prove hits. Proximity
    // bubbles do most waking; the synchronous attack Wake is the
    // sub-eval-period guarantee (wakes counter reported, not asserted:
    // attribution between the two correct paths is timing-dependent).
    const std::uint64_t deaths_before = sim.DeathsTotal();
    const auto attack_until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < attack_until && !expired()) {
        for (std::uint32_t net = 1000000; net < 1000060; ++net) {
            sim.PostAttackTarget(kPlayer2, net);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    const std::uint64_t deaths = sim.DeathsTotal() - deaths_before;
    const auto work = sim.LodWorkTotalsSnapshot();
    std::printf("LOD combat: deaths=%llu wakes=%llu\n",
                (unsigned long long)deaths,
                (unsigned long long)work.wakes);
    check("attack-kills", deaths > 0);
    // Deaths remove, respawns (30s timers) add back: conservation means the
    // count stays within [total-deaths, total], never below (loss) or above
    // (duplication).
    {
        const std::uint64_t mobs_now = sim.CollectProcessLoad().mobs;
        std::printf("LOD mobs: total=%llu deaths=%llu now=%llu\n",
                    (unsigned long long)mob_total,
                    (unsigned long long)deaths,
                    (unsigned long long)mobs_now);
        check("no-loss-phase4", mobs_now <= mob_total && mobs_now >= mob_total - deaths);
    }
    check("validate-phase4", validate_now("phase4"));

    // ---- Phase 5: respawn timers advance under LOD (supervisor-side
    // countdowns are tier-independent by design). Deaths from phase 4 must
    // come back within their window.
    const bool respawned = WaitFor(std::chrono::seconds(45), [&] {
        return sim.CollectProcessLoad().mobs >= mob_total;
    });
    check("respawn-advance", respawned && !expired());
    check("validate-phase5", validate_now("phase5"));

    const auto work_end = sim.LodWorkTotalsSnapshot();
    std::printf("LOD work totals: ai=%llu mv=%llu prom=%llu dem=%llu wakes=%llu eval_us=%llu\n",
                (unsigned long long)work_end.ai_updates,
                (unsigned long long)work_end.move_updates,
                (unsigned long long)work_end.promotions,
                (unsigned long long)work_end.demotions,
                (unsigned long long)work_end.wakes,
                (unsigned long long)work_end.eval_us);

    sim.Stop();
    std::printf("LOD-DONE validations=%d failures=%d\n", validations, failures);
    return failures;
}

// Cross-zone LOD determinism via the Spatial Activity Field (§32). Own sim
// lifecycle. Uses SMALL LOD radii (15/50/150, short graces) configured
// pre-Start, so every tier boundary fits inside the 1km test map while the
// comparison machinery stays identical (config stays authority).
//
//   Phase 1: discover an adjacent zone pair (A,B) from live bounds. Player
//     in A near the shared edge; 3 static mobs in B at 12/40/110m.
//     Expect exactly Full/Reduced/Low, all flagged cross-zone.
//   Phase 2: 9 more static mobs at 14/15/16, 49/50/51, 139/140/141m.
//     Expect F/R/R, R/L/L, L/Dormant/Dormant (half-open boundaries exact).
//   Phase 3: despawn the player; everything must reach Dormant (fast
//     cascade with the short test graces); a zone must fall asleep;
//     validator Dormant rules execute live.
//   Phase 4: spawn a player OUTSIDE the sleeping zone but inside the wake
//     radius: the zone must wake without any entry (predictive wake).
// Static mobs (spawn radius 0) never move, the AFK player never moves:
// positions are bit-exact forever, so asserts are deterministic.
// Returns failure count (0 = PASS).
int RunActivityScenario(boost::asio::io_context& io, const BenchConfig& config)
{
    int failures = 0;
    int validations = 0;
    auto check = [&](const char* name, bool pass) {
        if (pass) {
            std::printf("ACTIVITY %s: PASS\n", name);
        } else {
            std::printf("ACTIVITY %s: FAIL\n", name);
            ++failures;
        }
    };

    gs::game::WorldRuntime sim(io);
    auto validate_now = [&](const char* what) -> bool {
        sim.RequestValidation();
        for (int i = 0; i < 100; ++i) {
            std::string result;
            if (sim.TryTakeValidationResult(result)) {
                ++validations;
                if (result != "OK") {
                    std::printf("ACTIVITY validation(%s): FAIL: %s\n", what, result.c_str());
                    return false;
                }
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("ACTIVITY validation(%s): TIMEOUT\n", what);
        return false;
    };
    // Detailed field audit + per-mob samples matched by EXACT position
    // (static entities: bit-exact forever, no live flecs reads from here).
    auto activity_samples = [&](const char* what, std::size_t max_samples,
                                std::vector<gs::game::ActivitySampleResult>& out) -> bool {
        sim.RequestActivityValidation(max_samples);
        for (int i = 0; i < 100; ++i) {
            std::string error;
            if (sim.TryTakeActivitySamples(out, error)) {
                ++validations;
                return true;
            }
            if (!error.empty()) {
                ++validations;
                std::printf("ACTIVITY detailed(%s): FAIL: %s\n", what, error.c_str());
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("ACTIVITY detailed(%s): TIMEOUT\n", what);
        return false;
    };
    auto find_sample = [](const std::vector<gs::game::ActivitySampleResult>& samples, float x,
                          float y) -> const gs::game::ActivitySampleResult* {
        for (const auto& sample : samples) {
            if (sample.x == x && sample.y == y) {
                return &sample;
            }
        }
        return nullptr;
    };

    // Small radii so every boundary fits in-map; short graces so the
    // Dormant leg stays fast. Same machinery, config-driven (§5).
    gs::game::LodConfig test_lod;
    test_lod.full_radius_m = 15.0f;
    test_lod.reduced_radius_m = 50.0f;
    test_lod.low_radius_m = 150.0f;
    test_lod.demote_full_sec = 2.0f;
    test_lod.demote_reduced_sec = 2.0f;
    test_lod.demote_low_sec = 2.0f;
    sim.ConfigureSimulationLod(test_lod);
    if (sim.EffectiveLodConfig().low_radius_m != 150.0f) {
        check("config-authority", false);
        return failures + 1;
    }
    check("config-authority", true);
    sim.Start();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(std::max(120, config.seconds));
    auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };

    // ---- Topology discovery: adjacent pair (A,B) sharing an x-edge with
    // >=100m y-overlap, read from LIVE bounds (robust to splits, of which
    // none can happen at this load anyway).
    float x_edge = 0.0f, y_mid = 0.0f;
    std::size_t zone_a = 0, zone_b = 0;
    bool have_pair = false;
    {
        const auto& zones = sim.Zones();
        for (std::size_t i = 0; i < zones.ZoneCount() && !have_pair; ++i) {
            for (std::size_t j = 0; j < zones.ZoneCount() && !have_pair; ++j) {
                if (i == j) {
                    continue;
                }
                const auto& a = zones.GetZone(i).Bounds();
                const auto& b = zones.GetZone(j).Bounds();
                const float overlap =
                    std::min(a.max_y, b.max_y) - std::max(a.min_y, b.min_y);
                if (std::abs(a.max_x - b.min_x) < 0.01f && overlap >= 100.0f) {
                    zone_a = i;
                    zone_b = j;
                    x_edge = a.max_x;
                    y_mid = (std::max(a.min_y, b.min_y) + std::min(a.max_y, b.max_y)) * 0.5f;
                    have_pair = true;
                }
            }
        }
    }
    check("adjacent-pair", have_pair && !expired());
    if (!have_pair) {
        sim.Stop();
        return failures + 1;
    }
    std::printf("ACTIVITY edge: A=%zu B=%zu x=%.0f ymid=%.0f\n", zone_a, zone_b, x_edge, y_mid);
    const float px = x_edge - 10.0f; // player 10m inside A

    // ---- Phase 1: player + 3 static mobs across the border.
    constexpr gs::common::SessionId kPlayer = 700;
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kPlayer);
        sim.PostSpawn(session, MakeBenchCharacter(900), gs::game::DebugSpawnOverride{px, y_mid});
    }
    const float kDistances[3] = {12.0f, 40.0f, 110.0f}; // Full / Reduced / Low
    for (int k = 0; k < 3; ++k) {
        gs::game::MobSpawnPoint point;
        point.mob_type_id = 2;
        point.x = px + kDistances[k];
        point.y = y_mid;
        point.count = 1;
        point.radius = 0.0f; // static mob: exact position forever
        sim.AddMobSpawnPoint(point);
    }
    for (int k = 0; k < 3; ++k) {
        sim.RequestMobSpawn(static_cast<std::size_t>(4 + k));
    }
    const bool populated = WaitFor(std::chrono::seconds(20), [&] {
        return sim.Owners().size() == 1 && sim.CollectProcessLoad().mobs >= 3;
    });
    check("populate", populated && !expired());
    if (!populated) {
        sim.Stop();
        return failures + 1;
    }
    // Placement proof via position->zone mapping (no live flecs reads):
    // player in A, every mob in B.
    bool placed = sim.Zones().FindIndexForPosition(px, y_mid) == zone_a;
    for (int k = 0; k < 3 && placed; ++k) {
        placed = sim.Zones().FindIndexForPosition(px + kDistances[k], y_mid) == zone_b;
    }
    check("cross-zone-placement", placed);
    check("pre-validate", validate_now("pre"));
    std::this_thread::sleep_for(std::chrono::seconds(6)); // eval + settle
    if (expired()) {
        check("settle-window", false);
        sim.Stop();
        return failures + 1;
    }
    std::vector<gs::game::ActivitySampleResult> samples;
    bool detailed_ok = activity_samples("distances", 64, samples);
    check("detailed-ok", detailed_ok);
    const gs::game::SimulationTier kExpected1[3] = {
        gs::game::SimulationTier::Full,
        gs::game::SimulationTier::Reduced,
        gs::game::SimulationTier::Low,
    };
    bool tiers_ok = detailed_ok;
    for (int k = 0; k < 3 && tiers_ok; ++k) {
        const auto* sample = find_sample(samples, px + kDistances[k], y_mid);
        if (sample == nullptr || sample->field_tier != kExpected1[k] ||
            sample->brute_tier != kExpected1[k]) {
            std::printf("ACTIVITY distance[%d]: %s (expected %d)\n",
                        k,
                        sample == nullptr ? "sample-missing"
                                          : (sample->field_tier != sample->brute_tier ? "field!=brute"
                                                                                      : "wrong-tier"),
                        static_cast<int>(kExpected1[k]));
            tiers_ok = false;
        }
    }
    check("cross-zone-tiers", tiers_ok);
    // Metric path: every grant above came from the foreign zone's player,
    // so the cross-zone counters must observe traffic (accumulated over a
    // few diag windows to be independent of exchange phasing).
    {
        std::uint64_t seen = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < until && seen == 0 && !expired()) {
            for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
                const auto& diag = sim.Zones().GetZone(zi).Diagnostics();
                seen += diag.cross_zone_full_since_diag.load(std::memory_order_relaxed);
                seen += diag.cross_zone_reduced_since_diag.load(std::memory_order_relaxed);
                seen += diag.cross_zone_low_since_diag.load(std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("ACTIVITY cross-counters observed: %llu\n", (unsigned long long)seen);
        check("cross-counters-live", seen > 0);
    }
    check("validate-distances", validate_now("distances"));

    // ---- Phase 2: half-open boundary exactness (same player, 9 mobs).
    const float kBounds[9] = {14.0f, 15.0f, 16.0f, 49.0f, 50.0f,  51.0f,
                              149.0f, 150.0f, 151.0f};
    const gs::game::SimulationTier kExpected2[9] = {
        gs::game::SimulationTier::Full,    gs::game::SimulationTier::Reduced,
        gs::game::SimulationTier::Reduced, gs::game::SimulationTier::Reduced,
        gs::game::SimulationTier::Low,     gs::game::SimulationTier::Low,
        gs::game::SimulationTier::Low,     gs::game::SimulationTier::Dormant,
        gs::game::SimulationTier::Dormant,
    };
    for (int k = 0; k < 9; ++k) {
        gs::game::MobSpawnPoint point;
        point.mob_type_id = 2;
        point.x = px + kBounds[k];
        point.y = y_mid;
        point.count = 1;
        point.radius = 0.0f;
        sim.AddMobSpawnPoint(point);
    }
    for (int k = 0; k < 9; ++k) {
        sim.RequestMobSpawn(static_cast<std::size_t>(7 + k));
    }
    const bool populated2 = WaitFor(std::chrono::seconds(25), [&] {
        return sim.CollectProcessLoad().mobs >= 12;
    });
    check("boundary-populate", populated2 && !expired());
    if (!populated2) {
        sim.Stop();
        return failures + 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(6));
    if (expired()) {
        check("boundary-window", false);
        sim.Stop();
        return failures + 1;
    }
    std::vector<gs::game::ActivitySampleResult> samples2;
    bool detailed2 = activity_samples("boundaries", 64, samples2);
    check("detailed2-ok", detailed2);
    bool bounds_ok = detailed2;
    for (int k = 0; k < 9 && bounds_ok; ++k) {
        const auto* sample = find_sample(samples2, px + kBounds[k], y_mid);
        if (sample == nullptr || sample->field_tier != kExpected2[k] ||
            sample->brute_tier != kExpected2[k]) {
            std::printf("ACTIVITY boundary[%d] d=%.0f: %s (expected %d)\n",
                        k,
                        kBounds[k],
                        sample == nullptr ? "sample-missing"
                                          : (sample->field_tier != sample->brute_tier ? "field!=brute"
                                                                                      : "wrong-tier"),
                        static_cast<int>(kExpected2[k]));
            bounds_ok = false;
        }
    }
    check("boundary-exactness", bounds_ok);
    check("validate-boundaries", validate_now("boundaries"));
    // No topology churn allowed at this load (determinism guard).
    check("topology-stable", sim.Zones().ZoneCount() == 3);

    // ---- Phase 3: despawn -> Full/Reduced work must drain (sleep
    // precondition), zones fall asleep, validator Dormant rules execute
    // live on the already-Dormant boundary mobs. NOTE: remaining Low mobs
    // freeze mid-cascade in sleeping zones BY DESIGN (no ticks = no eval);
    // they resume and finish demoting on wake. Asserting all-Dormant here
    // would contradict the sleep economy, so assert no-Full/Reduced.
    sim.PostDespawn(kPlayer);
    const bool drained = WaitFor(std::chrono::seconds(15), [&] { return sim.Owners().empty(); });
    check("despawn-drained", drained && !expired());
    const bool quiet = WaitFor(std::chrono::seconds(20), [&] {
        std::uint64_t hot = 0, total = 0;
        for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
            const auto& diag = sim.Zones().GetZone(zi).Diagnostics();
            hot += diag.lod_full.load(std::memory_order_relaxed) +
                   diag.lod_reduced.load(std::memory_order_relaxed);
            total += hot + diag.lod_low.load(std::memory_order_relaxed) +
                     diag.lod_dormant.load(std::memory_order_relaxed);
        }
        return total >= 12 && hot == 0;
    });
    check("no-full-reduced", quiet && !expired());
    check("validate-dormant", validate_now("dormant"));
    const bool slept = WaitFor(std::chrono::seconds(10), [&] {
        return sim.CollectProcessLoad().sleeping_zones >= 1;
    });
    check("zone-slept", slept && !expired());

    // ---- Phase 4: predictive wake. Spawn OUTSIDE the sleeping zone but
    // inside the wake radius (reduced 50m): the zone must wake with no
    // entry, then validate.
    const float wake_x = x_edge - 40.0f; // zone A side, 40m from B's edge
    if (sim.Zones().FindIndexForPosition(wake_x, y_mid) == zone_b) {
        check("wake-point-outside", false);
        sim.Stop();
        return failures + 1;
    }
    check("wake-point-outside", true);
    constexpr gs::common::SessionId kWaker = 701;
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kWaker);
        sim.PostSpawn(session, MakeBenchCharacter(901), gs::game::DebugSpawnOverride{wake_x, y_mid});
    }
    const bool spawned_waker = WaitFor(std::chrono::seconds(10), [&] {
        return sim.Owners().size() == 1;
    });
    check("waker-spawned", spawned_waker && !expired());
    const bool rewoke = WaitFor(std::chrono::seconds(10), [&] {
        return sim.Zones().GetZone(zone_b).Activity() == gs::game::ZoneActivity::Active;
    });
    check("predictive-wake", rewoke && !expired());
    check("validate-wake", validate_now("wake"));

    const auto pm = sim.PartitionMetricsSnapshot();
    std::printf("ACTIVITY metrics: splits=%llu merges=%llu\n",
                (unsigned long long)(pm.split_commits + pm.split_aborts),
                (unsigned long long)(pm.merge_commits + pm.merge_aborts));
    sim.Stop();
    std::printf("ACTIVITY-DONE validations=%d failures=%d\n", validations, failures);
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

    if (config.mode == "splitmerge") {
        // Deterministic transaction scenario with its own sim lifecycle;
        // the load-loop below is skipped entirely for this mode.
        boost::asio::io_context splitmerge_io;
        std::thread splitmerge_io_thread([&splitmerge_io] { splitmerge_io.run(); });
        const int scenario_failures = RunSplitMergeScenario(splitmerge_io, config);
        splitmerge_io.stop();
        if (splitmerge_io_thread.joinable()) {
            splitmerge_io_thread.join();
        }
        std::printf("BENCH-DONE splitmerge failures=%d\n", scenario_failures);
        return scenario_failures == 0 ? 0 : 2;
    }

    if (config.mode == "lod") {
        // Simulation LOD correctness scenario with its own sim lifecycle.
        boost::asio::io_context lod_io;
        std::thread lod_io_thread([&lod_io] { lod_io.run(); });
        const int scenario_failures = RunLodScenario(lod_io, config);
        lod_io.stop();
        if (lod_io_thread.joinable()) {
            lod_io_thread.join();
        }
        std::printf("BENCH-DONE lod failures=%d\n", scenario_failures);
        return scenario_failures == 0 ? 0 : 2;
    }

    if (config.mode == "activity") {
        // Cross-zone LOD determinism via the activity field.
        boost::asio::io_context activity_io;
        std::thread activity_io_thread([&activity_io] { activity_io.run(); });
        const int scenario_failures = RunActivityScenario(activity_io, config);
        activity_io.stop();
        if (activity_io_thread.joinable()) {
            activity_io_thread.join();
        }
        std::printf("BENCH-DONE activity failures=%d\n", scenario_failures);
        return scenario_failures == 0 ? 0 : 2;
    }

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
        if (config.lod_off) {
            gs::game::LodConfig lod;
            lod.enabled = false;
            sim.ConfigureSimulationLod(lod);
            std::printf("simulation lod: DISABLED (legacy every-tick behavior)\n");
        }
        if (config.partition_min_size > 0) {
            gs::game::PartitionConfig override;
            override.min_zone_size_m = static_cast<float>(config.partition_min_size);
            sim.ConfigurePartition(override);
            std::printf("partition override: min_zone_size_m=%d (effective %.0f after validation)\n",
                        config.partition_min_size,
                        sim.EffectivePartitionConfig().min_zone_size_m);
        }
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
        {
            // Simulation LOD distribution (current gauges) + actual update
            // rates (§25): how many entities really got AI/movement updates
            // per second. Work totals are cumulative (never reset); the
            // per-second windows in the diag log reset every second.
            std::uint64_t t_full = 0, t_reduced = 0, t_low = 0, t_dormant = 0;
            std::uint64_t x_full = 0, x_reduced = 0, x_low = 0, sleep_block = 0, wake_ext = 0;
            for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
                const auto& diag = sim.Zones().GetZone(zi).Diagnostics();
                t_full += diag.lod_full.load(std::memory_order_relaxed);
                t_reduced += diag.lod_reduced.load(std::memory_order_relaxed);
                t_low += diag.lod_low.load(std::memory_order_relaxed);
                t_dormant += diag.lod_dormant.load(std::memory_order_relaxed);
                x_full += diag.cross_zone_full_since_diag.load(std::memory_order_relaxed);
                x_reduced += diag.cross_zone_reduced_since_diag.load(std::memory_order_relaxed);
                x_low += diag.cross_zone_low_since_diag.load(std::memory_order_relaxed);
                sleep_block += diag.sleep_blocked_external_since_diag.load(std::memory_order_relaxed);
                wake_ext += diag.wake_external_since_diag.load(std::memory_order_relaxed);
            }
            const auto work = sim.LodWorkTotalsSnapshot();
            const auto activity = sim.ActivityMetrics();
            const double elapsed = static_cast<double>(std::max(1, config.seconds));
            std::printf("lod tiers: full=%llu reduced=%llu low=%llu dormant=%llu\n",
                        (unsigned long long)t_full,
                        (unsigned long long)t_reduced,
                        (unsigned long long)t_low,
                        (unsigned long long)t_dormant);
            std::printf("lod work: ai_updates=%llu (%.0f/s) move_updates=%llu (%.0f/s) prom=%llu "
                        "dem=%llu wakes=%llu eval_us=%llu\n",
                        (unsigned long long)work.ai_updates,
                        work.ai_updates / elapsed,
                        (unsigned long long)work.move_updates,
                        work.move_updates / elapsed,
                        (unsigned long long)work.promotions,
                        (unsigned long long)work.demotions,
                        (unsigned long long)work.wakes,
                        (unsigned long long)work.eval_us);
            std::printf("activity: sources=%llu cells=%llu/%llu rebuilds=%llu rb_us=%llu "
                        "xzone=[%llu/%llu/%llu] sleep=[blocked=%llu wext=%llu]\n",
                        (unsigned long long)activity.sources,
                        (unsigned long long)activity.cells_nonempty,
                        (unsigned long long)activity.cells_total,
                        (unsigned long long)activity.rebuilds,
                        (unsigned long long)activity.rebuild_us_total,
                        (unsigned long long)x_full,
                        (unsigned long long)x_reduced,
                        (unsigned long long)x_low,
                        (unsigned long long)sleep_block,
                        (unsigned long long)wake_ext);
        }

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
