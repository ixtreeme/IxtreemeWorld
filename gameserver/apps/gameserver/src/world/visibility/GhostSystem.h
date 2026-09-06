#pragma once

#include <cstdint>

// Maintains a zone's read-only ghost copies of neighbor-zone border
// residents. Ghosts are NEVER authoritative and NEVER written back; they
// exist so AOI/visibility/replication can see across zone borders.
// Cross-zone reads use snapshot buffers only (no access into the neighbor's
// flecs storage).
namespace gs::game {

class Zone;
class ZoneManager;

class GhostSystem {
public:
    static void Rebuild(Zone& zone, ZoneManager& zones);
    static void Clear(Zone& zone);
    static void RemoveByNetId(Zone& zone, std::uint32_t net_id);
};

} // namespace gs::game
