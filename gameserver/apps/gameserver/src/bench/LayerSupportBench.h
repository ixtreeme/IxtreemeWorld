#pragma once

namespace gs::bench {

// Strict package -> real WorldRuntime -> offline ground-state queries.
// No world Start, production entity admission/movement, network session,
// database access, or modification of a checked-in map is performed.
int RunLayerSupportScenario();

} // namespace gs::bench
