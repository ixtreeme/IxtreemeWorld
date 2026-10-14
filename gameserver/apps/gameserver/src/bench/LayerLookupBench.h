#pragma once

namespace gs::bench {

// Strict package -> immutable runtime layered metadata. No world Start,
// network admission, actor movement or database operation is performed.
int RunLayerLookupScenario();

} // namespace gs::bench
