#pragma once

namespace gs::bench {

// 3D-4B: a cooked clearance/proven-portal world through the strict package
// writer -> strict full loader -> real WorldRuntime -> offline actor queries.
// No world Start, entity admission/movement, network session, database
// access, or modification of a checked-in map is performed.
int RunLayerClearanceScenario();

} // namespace gs::bench
