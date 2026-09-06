#include "ZoneScheduler.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "common/Logging.h"

#include "../WorldConstants.h"
#include "Zone.h"
#include "ZoneLoadMetrics.h"
#include "ZoneManager.h"
#include "ZoneWorkerPool.h"

namespace gs::game {
namespace {

// Load score for dispatch ordering: players dominate (their ticks carry
// replication), then residents, then recent tick cost. Ordering only --
// every due zone still ticks every interval.
std::uint64_t LoadScore(const Zone& zone)
{
    const auto& diag = zone.Diagnostics();
    const std::uint64_t players = diag.player_count.load(std::memory_order_relaxed);
    const std::uint64_t mobs = diag.mob_count.load(std::memory_order_relaxed);
    const std::uint64_t tick_avg =
        diag.ticks_since_diag.load(std::memory_order_relaxed) > 0
            ? diag.tick_micros_since_diag.load(std::memory_order_relaxed) /
                  diag.ticks_since_diag.load(std::memory_order_relaxed)
            : 0;
    return players * 1'000'000 + (players + mobs) * 1'000 + tick_avg;
}

} // namespace

void ZoneScheduler::ScheduleOnce(ZoneManager& zones,
                                 ZoneWorkerPool& pool,
                                 std::chrono::steady_clock::time_point now)
{
    std::vector<std::pair<std::uint64_t, std::size_t>> due;
    due.reserve(zones.ZoneCount());

    for (std::size_t i = 0; i < zones.ZoneCount(); ++i) {
        auto& zone = zones.GetZone(i);
        const bool has_commands = !zone.Commands().Empty();

        // Sleeping: no players, no mobs, no pending commands. Mob-bearing
        // zones stay Active -- freezing them would lose gameplay time.
        // Wake is implicit: spawn/migration bump the counts synchronously,
        // and any queued command flips has_commands.
        if (zone.Diagnostics().player_count.load() == 0 && zone.Diagnostics().mob_count.load() == 0 &&
            !has_commands) {
            if (zone.Activity() != ZoneActivity::Sleeping) {
                zone.SetActivity(ZoneActivity::Sleeping);
                LOG_DEBUG("Zone {} ('{}') sleeping", zone.Id(), zone.Name());
            }
            zone.NextTick() = now + kTickDt;
            zone.Diagnostics().empty_skips_since_diag.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (zone.Activity() != ZoneActivity::Active) {
            zone.SetActivity(ZoneActivity::Active);
            LOG_DEBUG("Zone {} ('{}') active", zone.Id(), zone.Name());
        }

        if (now < zone.NextTick() && !has_commands) {
            continue;
        }

        bool expected = false;
        if (!zone.TickInProgress().compare_exchange_strong(expected, true)) {
            continue;
        }

        do {
            zone.NextTick() += kTickDt;
        } while (zone.NextTick() <= now);
        due.emplace_back(LoadScore(zone), i);
    }

    // Heaviest zones first so one slow zone cannot starve behind a queue of
    // light ones on the same worker; assignment still stripes naturally via
    // the FIFO task queue.
    std::sort(due.begin(), due.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first > rhs.first;
    });
    for (const auto& [score, index] : due) {
        (void)score;
        pool.Enqueue(index);
    }
}

void CollectZoneLoadMetrics(ZoneManager& zones, std::vector<ZoneLoadMetrics>& out)
{
    out.clear();
    out.reserve(zones.ZoneCount());
    for (std::size_t i = 0; i < zones.ZoneCount(); ++i) {
        auto& zone = zones.GetZone(i);
        const auto& diag = zone.Diagnostics();
        ZoneLoadMetrics metrics;
        metrics.zone_id = zone.Id();
        metrics.players = diag.player_count.load(std::memory_order_relaxed);
        metrics.mobs = diag.mob_count.load(std::memory_order_relaxed);
        metrics.ghosts = diag.ghost_count.load(std::memory_order_relaxed);
        const std::uint64_t ticks = diag.ticks_since_diag.load(std::memory_order_relaxed);
        metrics.avg_tick_us = ticks > 0
                                  ? diag.tick_micros_since_diag.load(std::memory_order_relaxed) / ticks
                                  : 0;
        metrics.aoi_queries = diag.aoi_queries_since_diag.load(std::memory_order_relaxed);
        metrics.migrations = diag.migrations_since_diag.load(std::memory_order_relaxed);
        metrics.repl_records = diag.transform_records_since_diag.load(std::memory_order_relaxed);
        metrics.queue_depth = zone.Commands().Depth();
        metrics.activity = zone.Activity();
        out.push_back(metrics);
    }
}

} // namespace gs::game
