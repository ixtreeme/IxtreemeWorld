#pragma once

#include <cstdint>

// Per-viewer periodic resync schedule (hardening H7).
//
// A viewer's full-state refresh is staggered across ticks by a phase key (its
// NetId) so no tick carries the whole world's refresh at once. The schedule
// is DUE-based, not an exact tick match: a zone samples the global world tick
// at its own cadence and can skip a value (a late or overloaded tick sees
// N, N+2, ...). An exact `(tick + key) % period == 0` test silently drops the
// resync for that period -- and a zone that consistently ticks every other
// world tick would never refresh an odd-phase viewer at all.
//
// `next_due` lives with the viewer (0 = not yet scheduled). The first call
// schedules the viewer's phase-aligned tick at or after now; afterwards the
// refresh fires on the first sampled tick at or past the due tick and is
// re-armed on the NEXT phase-aligned tick, so the stagger is preserved and
// at most one refresh fires per period.
namespace gs::game {

inline bool ConsumeResyncDue(std::uint32_t& next_due,
                             std::uint32_t world_tick,
                             std::uint32_t phase_key,
                             std::uint32_t period) noexcept
{
    if (period == 0) {
        return false;
    }
    if (next_due == 0) {
        next_due = world_tick + (period - (world_tick + phase_key) % period) % period;
    }
    if (world_tick < next_due) {
        return false;
    }
    next_due = world_tick + (period - (world_tick + phase_key) % period);
    return true;
}

} // namespace gs::game
