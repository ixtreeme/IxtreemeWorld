#pragma once

// Publishes a zone's border-band residents as plain snapshots into the
// zone's double-buffered outbound store. Neighbor zones consume the PREVIOUS
// tick's buffer to rebuild ghosts. Snapshot data only; never entity handles.
#include "BorderSnapshot.h"

namespace gs::game {

class Zone;

class BorderPublisher {
public:
    static void Publish(Zone& zone);

    // A zone that goes to sleep never republishes, so its last publication
    // would outlive entities that left it in the tick that emptied it
    // (migrated out, despawned) and neighbours would keep -- or later
    // resurrect -- ghosts of them. Drops every published snapshot whose net
    // is no longer a resident, as one incremental generation (removal
    // delta); residents (e.g. quiet mobs) stay published. Zone owner only.
    static void RetractNonResidents(Zone& zone);

    // 3D-5D migration commit: the new owner publishes its new resident at
    // once (one incremental generation) instead of on its next tick, so
    // neighbours re-attribute their ghost of it without a gap. No-op outside
    // the border band. Zone owner only.
    static void PublishResident(Zone& zone, const BorderEntitySnapshot& snapshot);
};

} // namespace gs::game
