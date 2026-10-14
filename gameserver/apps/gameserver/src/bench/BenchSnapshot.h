#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <type_traits>

#include "../world/WorldRuntime.h"

// MAP-0: the one sanctioned way for a bench to look at a RUNNING world.
//
// The supervisor splits, merges, retires, reclaims and reuses zone slots
// concurrently with the bench thread, and zone ticks update the per-zone
// gauges. A raw sim.Zones()/sim.Owners() read (or a stored Zone&/Zone*/slot
// index) races all of that. ReadWorld instead hands `collect` to the
// supervisor, which runs it in a quiescent window (no zone tick in flight,
// between two topology transactions); the bench receives the copy the
// collector returns. Everything one collector reads therefore comes from a
// single mutation point, tagged with ctx.epoch / ctx.world_tick.
//
// Rules for collectors: copy values out (never return pointers/references
// into the world, never keep slot indices across calls -- key by ZoneId),
// stay short, and never request another snapshot from inside.
namespace gs::bench {

using WorldSnapshot = gs::game::WorldRuntime::SnapshotContext;

template <class F>
auto ReadWorld(gs::game::WorldRuntime& sim,
               F&& collect,
               std::chrono::milliseconds timeout = std::chrono::seconds(30))
{
    using T = std::decay_t<std::invoke_result_t<F&, const WorldSnapshot&>>;
    static_assert(!std::is_pointer_v<T>, "copy values out of a world snapshot, not pointers");
    auto future =
        sim.CaptureSnapshot<T>(std::function<T(const WorldSnapshot&)>(std::forward<F>(collect)));
    T out{};
    if (!sim.WaitSnapshot(future, timeout, out)) {
        // The queued collector may capture the caller's locals by reference,
        // so unwinding past it would leave a dangling request that the
        // supervisor still runs later. A supervisor that serves no quiescent
        // window for this long is a hard harness failure anyway.
        std::fprintf(stderr, "FATAL ReadWorld: world snapshot not served within %lld ms\n",
                     static_cast<long long>(timeout.count()));
        std::fflush(stderr);
        std::abort();
    }
    return out;
}

} // namespace gs::bench
