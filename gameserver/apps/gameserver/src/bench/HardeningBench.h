#pragma once

#include <string>

// Gameserver infrastructure hardening harness (work plan H0-H3). Separate
// from WorldBench.cpp so every hardening check lives in one place.
namespace gs::bench {

struct TickRateConfig {
    // Measured window per input rate (after spawn settle + warmup).
    int measure_seconds = 4;
    // Comma-separated input rates in Hz; "flood" = as fast as the poster
    // thread can go.
    std::string rates = "20,30,60,144,flood";
};

// H0/H1: authoritative tick vs input rate. One player on a real loopback
// TCP session; frames are decoded client-side, so ticks/sec, frames/sec
// and movement are measured end-to-end (zone tick from the frame header,
// displacement from the viewer's own record) -- no racy zone reads.
// Invariant: input rate != simulation tick rate; authoritative sim = 20 Hz.
int RunTickRateScenario(const TickRateConfig& config);

// H1 side effects of tick-aligned draining: commands may now wait up to one
// tick in a zone queue. (A) forced split success while a resident player
// streams 60 Hz input (the split/merge gates refuse zones with pending
// commands); (B) client inputs lost across migrations under load (an input
// routed to the source zone right before a migration executes where the
// player no longer lives).
int RunInputPathScenario();

// H2/H3: network concurrency + resource safety against the production
// Server/Session/GameConnectionHandler on 4 IO threads (loopback only). The
// DB pool is deliberately never started, so EnterWorld parks a session in
// EnteringWorld (exactly the state the disconnect tests need) without any
// database. Covers malformed packets in every state, disconnect during
// EnteringWorld, connect/RST storms + accept-loop liveness, rapid
// reconnect, concurrent Send/Stop from foreign threads, repeated Stop.
struct NetStressConfig {
    int io_threads = 4;
    // Foreign-thread Stop / client RST / concurrent repeated Stop during the
    // concurrent-send case.
    bool chaos = true;
    // "all" or "concurrent" (the Send/Stop stress alone, for differential
    // runs such as io_threads=1 vs 4).
    std::string cases = "all";
};
int RunNetStressScenario(const NetStressConfig& config);

} // namespace gs::bench
