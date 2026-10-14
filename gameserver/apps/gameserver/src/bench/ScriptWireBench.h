#pragma once

namespace gs::bench {

// 3D-5C2: the MMO client protocol lives in a scene script (Client/sdk/examples/
// mmo_client/MmoClient.cpp) with a hand-written Cap'n Proto codec, because a
// game module only sees the SDK headers. This scenario compiles that script's
// codec (MMO_CLIENT_CODEC_ONLY) and cross-checks it against the real thing:
// script-encoded client packets parsed by real Cap'n Proto (untrusted limits),
// real server packets (incl. multi-segment / far-pointer messages) decoded by
// the script, real v1/v2/v3 transform frames decoded byte-exactly, corrupt and
// truncated input rejected without faults, and a loopback session against the
// real GameConnectionHandler driven only by script-encoded packets.
int RunScriptWireScenario();

} // namespace gs::bench
