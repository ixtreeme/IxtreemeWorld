#include "ZoneScheduler.h"

#include <memory>

#include "../WorldConstants.h"
#include "Zone.h"
#include "ZoneManager.h"
#include "ZoneWorkerPool.h"

namespace gs::game {

void ZoneScheduler::ScheduleOnce(ZoneManager& zones,
                                 ZoneWorkerPool& pool,
                                 std::chrono::steady_clock::time_point now)
{
    for (std::size_t i = 0; i < zones.ZoneCount(); ++i) {
        auto& zone = zones.GetZone(i);
        const bool has_commands = !zone.Commands().Empty();

        if (zone.Diagnostics().player_count.load() == 0 && zone.Diagnostics().mob_count.load() == 0 &&
            !has_commands) {
            zone.NextTick() = now + kTickDt;
            zone.Diagnostics().empty_skips_since_diag.fetch_add(1, std::memory_order_relaxed);
            continue;
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
        pool.Enqueue(i);
    }
}

} // namespace gs::game
