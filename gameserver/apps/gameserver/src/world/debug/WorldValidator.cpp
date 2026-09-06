#include "WorldValidator.h"

#include <sstream>
#include <unordered_set>

#include "../components/Tags.h"
#include "../migration/MigrationQueue.h"
#include "../spatial/SpatialValidator.h"
#include "../zone/Zone.h"
#include "../zone/ZoneManager.h"

namespace gs::game {
namespace {

bool Fail(std::string& out_error, const std::string& message)
{
    out_error = message;
    return false;
}

} // namespace

bool ValidateWorldConsistency(ZoneManager& zones,
                              const OwnerMap& owners,
                              const MigrationQueue& migrations,
                              std::string& out_error)
{
    std::unordered_set<std::uint32_t> authoritative_nets;

    for (std::size_t i = 0; i < zones.ZoneCount(); ++i) {
        auto& zone = zones.GetZone(i);

        // Every indexed NetId resolves to a live entity, exactly once world-wide.
        for (const auto& [net_id, entity] : zone.Entities()) {
            (void)entity;
            if (!zone.FindEntity(net_id).is_valid()) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": indexed net " << net_id << " is stale";
                return Fail(out_error, message.str());
            }
            if (!authoritative_nets.insert(net_id).second) {
                std::ostringstream message;
                message << "net " << net_id << " authoritative in two zones (dual authority!)";
                return Fail(out_error, message.str());
            }
        }

        // Session bindings point at live indexed entities, both maps agree.
        for (const auto& [net_id, binding] : zone.Players()) {
            if (!binding.session) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": player net " << net_id << " has null session";
                return Fail(out_error, message.str());
            }
            if (!zone.FindEntity(net_id).is_valid()) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": player net " << net_id << " has no entity";
                return Fail(out_error, message.str());
            }
            const auto session_net = zone.NetIdForSession(binding.session->Id());
            if (!session_net || *session_net != net_id) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": net_by_session disagrees for session "
                        << binding.session->Id();
                return Fail(out_error, message.str());
            }
        }
        for (const auto& [session_id, net_id] : zone.NetBySession()) {
            const auto* binding = zone.FindPlayer(net_id);
            if (binding == nullptr || !binding->session || binding->session->Id() != session_id) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": orphan net_by_session entry for session "
                        << session_id;
                return Fail(out_error, message.str());
            }
        }

        // RNG drivers must not outlive their mob (leak check).
        for (const auto net_id : zone.MobRngKeys()) {
            const auto entity = zone.FindEntity(net_id);
            if (!entity.is_valid() || !entity.has<MobTag>()) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": orphan mob RNG for net " << net_id;
                return Fail(out_error, message.str());
            }
        }

        // Ghosts are never residents and always tagged.
        for (const auto& ghost : zone.Ghosts()) {
            if (zone.Entities().find(ghost.snapshot.net_id) != zone.Entities().end()) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": ghost net " << ghost.snapshot.net_id
                        << " is also indexed as resident";
                return Fail(out_error, message.str());
            }
            if (!ghost.entity.is_valid() || !ghost.entity.has<GhostTag>()) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": ghost net " << ghost.snapshot.net_id
                        << " missing entity/GhostTag";
                return Fail(out_error, message.str());
            }
        }

        if (!ValidateSpatialIndex(zone, zone.Grid(), out_error)) {
            return false;
        }
    }

    // OwnerMap entries point at a live binding + entity in the recorded zone.
    for (const auto& [session_id, owner] : owners) {
        if (owner.zone_index >= zones.ZoneCount()) {
            std::ostringstream message;
            message << "owner map: session " << session_id << " points at missing zone index "
                    << owner.zone_index;
            return Fail(out_error, message.str());
        }
        auto& zone = zones.GetZone(owner.zone_index);
        const auto* binding = zone.FindPlayer(owner.net_id);
        if (binding == nullptr || !binding->session || binding->session->Id() != session_id) {
            std::ostringstream message;
            message << "owner map: session " << session_id << " has no live binding in zone "
                    << zone.Id();
            return Fail(out_error, message.str());
        }
        if (!zone.FindEntity(owner.net_id).is_valid()) {
            std::ostringstream message;
            message << "owner map: session " << session_id << " has no live entity in zone "
                    << zone.Id();
            return Fail(out_error, message.str());
        }
    }

    // Pending migration requests reference valid zones (liveness itself is
    // revalidated at execution; stale ones are dropped there).
    for (const auto& request : migrations.RequestSnapshot()) {
        if (zones.FindIndexById(request.source_zone_id) >= zones.ZoneCount() ||
            zones.FindIndexById(request.target_zone_id) >= zones.ZoneCount()) {
            std::ostringstream message;
            message << "migration queue: net " << request.net_id << " references invalid zone "
                    << request.source_zone_id << "->" << request.target_zone_id;
            return Fail(out_error, message.str());
        }
    }

    return true;
}

} // namespace gs::game
