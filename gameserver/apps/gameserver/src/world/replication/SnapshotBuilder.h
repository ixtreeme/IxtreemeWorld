#pragma once

#include <optional>

#include <flecs.h>

#include "../visibility/BorderSnapshot.h"

// Builds replication snapshots from authoritative flecs state. The ONLY place
// that translates entity components (+ player display data from the session
// binding) into BorderEntitySnapshot plain data for border publish, AOI and
// spawn packets.
namespace gs::game {

class Zone;

BorderEntitySnapshot BuildPlayerSnapshot(Zone& zone, flecs::entity entity);
BorderEntitySnapshot BuildMobSnapshot(flecs::entity entity);

// Resolves any visible net_id (resident or ghost) to its snapshot.
std::optional<BorderEntitySnapshot> ResolveVisibleSnapshot(Zone& zone, std::uint32_t net_id);

} // namespace gs::game
