#pragma once

namespace gs::bench {

// 3D-5A: server-authoritative layered presence on the REAL running runtime.
// A cooked layered package (floor across a zone border, wall, proven stair
// portals, a stacked bridge) is loaded strictly; players are admitted with
// explicit volumes and moved with real input; admission, clearance,
// portal crossings, cross-zone migration, stacked spatial buckets, ghosts,
// AOI visibility and the world audit are checked. Plus unit checks of the
// layered spatial grid, the transfer DTO and the additive protocol fields.
// No network listener, database or checked-in map is touched.
int RunLayeredPresenceScenario();

} // namespace gs::bench
