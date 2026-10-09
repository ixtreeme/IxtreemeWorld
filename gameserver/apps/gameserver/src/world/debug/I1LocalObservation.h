#pragma once

#include <array>
#include <charconv>
#include <condition_variable>
#include <sstream>
#include <thread>
#include "common/Config.h"
#include "common/Logging.h"
#include "network/Session.h"
#include "../WorldRuntime.h"

namespace gs::game {

// Admission-only, opt-in local evidence. No input endpoint, mutation, queue
// drain or validator invocation. All references die inside the collector.
struct I1ObservedIdentity {
    gs::db::CharacterId character{};
    gs::common::SessionId session = 0;
    std::uint32_t net = 0;
};
using I1ObservationFilter = std::array<I1ObservedIdentity, 2>;

inline I1ObservationFilter ParseI1ObservationFilter(const std::string& value)
{
    I1ObservationFilter out{};
    const auto comma = value.find(',');
    if (comma == std::string::npos) throw std::invalid_argument("i1_local_observe_characters requires two IDs");
    const std::array<std::string, 2> fields{value.substr(0, comma), value.substr(comma + 1)};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        std::uint64_t id = 0;
        const auto& s = fields[i];
        const auto r = std::from_chars(s.data(), s.data() + s.size(), id);
        if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || id == 0)
            throw std::invalid_argument("invalid i1_local_observe_characters ID");
        out[i].character = static_cast<gs::db::CharacterId>(id);
    }
    if (out[0].character == out[1].character) throw std::invalid_argument("observation IDs must differ");
    return out;
}

struct I1PresenceRow {
    I1ObservedIdentity tracked;
    bool registered = false, reverse = false, owner = false, active_leaf = false;
    bool matching_binding = false, living = false, coherent = false;
    std::uint64_t zone = 0;
    std::size_t bindings = 0, entities = 0, owners = 0, queued = 0;
};
struct I1PresenceSnapshot {
    std::uint64_t epoch = 0;
    std::uint32_t tick = 0;
    std::int64_t captured_us = 0;
    std::array<I1PresenceRow, 2> rows{};
};

inline I1PresenceSnapshot CollectI1Presence(const WorldRuntime::SnapshotContext& ctx,
                                          I1ObservationFilter filter)
{
    I1PresenceSnapshot out;
    out.epoch = ctx.epoch;
    out.tick = ctx.world_tick;
    out.captured_us = std::chrono::duration_cast<std::chrono::microseconds>(
        ctx.captured_at.time_since_epoch()).count();
    for (std::size_t k = 0; k < filter.size(); ++k) {
        auto& row = out.rows[k];
        row.tracked = filter[k];
        if (const auto* record = ctx.presence.Find(filter[k].character)) {
            row.registered = true;
            row.tracked.session = record->session_id;
            row.tracked.net = record->net_id;
            row.reverse = ctx.presence.CharacterOf(record->session_id) == filter[k].character;
        }
        for (const auto& [sid, info] : ctx.owners) {
            if (sid == row.tracked.session || (row.tracked.net && info.net_id == row.tracked.net)) ++row.owners;
        }
        const auto owner = ctx.owners.find(row.tracked.session);
        if (owner != ctx.owners.end() && owner->second.net_id == row.tracked.net &&
            owner->second.zone_index < ctx.zones.ZoneCount()) {
            const auto& zone = ctx.zones.GetZone(owner->second.zone_index);
            row.zone = zone.Id();
            row.owner = owner->second.location.zone == zone.Id();
            row.active_leaf = zone.SimulationEnabled() && zone.Partition() == PartitionState::Leaf;
            const auto* binding = zone.FindPlayer(row.tracked.net);
            row.matching_binding = binding && binding->session &&
                binding->session->Id() == row.tracked.session && binding->character.id == row.tracked.character &&
                zone.NetIdForSession(row.tracked.session) == row.tracked.net;
            const auto entity = zone.FindEntity(row.tracked.net);
            row.living = entity.is_valid() && entity.is_alive() && zone.IsResident(row.tracked.net);
        }
        for (std::size_t i = 0; i < ctx.zones.ZoneCount(); ++i) {
            const auto& zone = ctx.zones.GetZone(i);
            row.queued += zone.Commands().Depth();
            for (const auto& [net, binding] : zone.Players()) {
                if (binding.character.id == row.tracked.character) ++row.bindings;
            }
            if (row.tracked.net && zone.HasEntity(row.tracked.net) && zone.IsResident(row.tracked.net)) ++row.entities;
        }
        // Incomplete queued spawn/despawn is reported as incomplete, never
        // repaired or silently promoted to a consistency PASS.
        row.coherent = row.registered && row.reverse && row.owner && row.active_leaf &&
            row.matching_binding && row.living && row.bindings == 1 && row.entities == 1 && row.owners == 1;
    }
    return out;
}

class I1LocalObservation {
public:
    I1LocalObservation() = default;
    ~I1LocalObservation() { Stop(); }
    void Start(WorldRuntime& world, I1ObservationFilter filter)
    {
        if (thread_.joinable()) throw std::logic_error("observation already started");
        thread_ = std::jthread([&world, filter](std::stop_token stop) mutable {
            using namespace std::chrono_literals;
            // 4 Hz, <= 6000 polls / 25 minutes, at most ONE queued capture.
            // The capture owns only copied identities, so stopping while it
            // is queued cannot leave dangling stack or observer references.
            std::future<I1PresenceSnapshot> pending;
            std::mutex mutex;
            std::condition_variable_any cv;
            std::unique_lock lock(mutex);
            LOG_INFO("I1-OBS start interval_ms=250 max_polls=6000 phase=quiescent_no_drain");
            for (std::size_t n = 0; n < 6000 && !stop.stop_requested(); ++n) {
                if (!pending.valid()) pending = world.CaptureSnapshot<I1PresenceSnapshot>(
                    [filter](const auto& ctx) { return CollectI1Presence(ctx, filter); });
                if (pending.wait_for(0ms) == std::future_status::ready) {
                    try {
                        const auto value = pending.get();
                        std::ostringstream rows;
                        for (std::size_t i = 0; i < value.rows.size(); ++i) {
                            const auto& r = value.rows[i]; filter[i] = r.tracked;
                            rows << " char=" << gs::db::ToUint64(r.tracked.character)
                                 << ",session=" << r.tracked.session << ",net=" << r.tracked.net
                                 << ",registry=" << r.registered << ",reverse=" << r.reverse
                                 << ",owner=" << r.owner << ",zone=" << r.zone << ",leaf=" << r.active_leaf
                                 << ",binding=" << r.matching_binding << ",alive=" << r.living
                                 << ",bindings=" << r.bindings << ",entities=" << r.entities
                                 << ",owners=" << r.owners << ",queued=" << r.queued << ",coherent=" << r.coherent;
                        }
                        LOG_INFO("I1-OBS epoch={} tick={} steady_us={}{}", value.epoch, value.tick, value.captured_us, rows.str());
                    } catch (...) { LOG_ERROR("I1-OBS capture failed"); break; }
                }
                cv.wait_for(lock, stop, 250ms, [] { return false; });
            }
            LOG_INFO("I1-OBS stopped pending={}", pending.valid());
        });
    }
    void Stop() { if (thread_.joinable()) { thread_.request_stop(); thread_.join(); } }
private:
    std::jthread thread_;
};
} // namespace gs::game
