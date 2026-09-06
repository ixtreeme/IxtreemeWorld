#pragma once

// Publishes a zone's border-band residents as plain snapshots into the
// zone's double-buffered outbound store. Neighbor zones consume the PREVIOUS
// tick's buffer to rebuild ghosts. Snapshot data only; never entity handles.
namespace gs::game {

class Zone;

class BorderPublisher {
public:
    static void Publish(Zone& zone);
};

} // namespace gs::game
