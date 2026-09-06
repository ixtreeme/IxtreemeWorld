#pragma once

#include <chrono>

// Decides WHEN each zone ticks. Gameplay-agnostic: it only looks at resident
// counts, pending commands and tick deadlines, then hands due zones to the
// worker pool. Empty zones are skipped (diagnostic counter, as before) and
// their deadline is pushed forward so they stay idle.
namespace gs::game {

class ZoneManager;
class ZoneWorkerPool;

class ZoneScheduler {
public:
    void ScheduleOnce(ZoneManager& zones,
                      ZoneWorkerPool& pool,
                      std::chrono::steady_clock::time_point now);
};

} // namespace gs::game
