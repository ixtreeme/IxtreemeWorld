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

// H4: world presence invariant -- one CharacterId, at most one authoritative
// presence -- across duplicate enter, same-session re-enter, ordered and
// reversed old/new session races, concurrent enters, migration and
// split/merge retirement. Every step is followed by the consistency audit.
int RunPresenceScenario();

// H5: control metrics vs diagnostic metrics. Same overloaded workload under
// three diagnostics cadences (1000/100/37 ms): the partition control decision
// state (sustained-breach timer, merge-side low timer, split candidacy timing)
// must not depend on when the diagnostics logger resets its windows.
int RunAsfDeterminismScenario();

// H6: worker pool revalidation on the current code: pool creation timing,
// production-map (few seed zones) sizing, --workers override, and whether a
// split turns into parallelism.
int RunWorkerPoolScenario();

// H7: replication v2 correctness -- periodic resync reaches unchanged
// entities, recipient freshness bound under a tight budget, one monotonic
// wire tick across migrations, QuantizeHeading on non-finite/huge input.
int RunReplicationV2Scenario();

// H8: protocol input hardening -- a malformed/adversarial client closes at
// most its own session: malformed frame corpus in every pre-world state, a
// mutation fuzzer, Cap'n Proto traversal amplification, handler exception
// containment, an oversized server send, non-finite numeric input at the
// world boundary. The process and every IO worker must survive.
int RunProtocolHardeningScenario();

// H9: retired zone reclamation -- `cycles` forced split->merge cycles on a
// populated zone; zone slots, retired zones, live zone worlds and RSS at
// checkpoints, plus a consistency audit at each.
int RunZoneReclamationScenario(int cycles);

// MAP-0: bench snapshot consistency -- reader threads take supervisor
// snapshots back to back while forced split/merge cycles, slot reclamation +
// reuse and player/mob migrations run; every snapshot is checked against
// exact world invariants and the reader's previous snapshot. Also covers the
// supervisor self-wait refusal and the shutdown drain.
int RunSnapshotConsistencyScenario(int cycles);

// H10: low-level hygiene -- ZoneWorkerPool::Stop lost wakeup (Start/Stop
// stress with a watchdog), TCP_NODELAY on accepted sessions, load field cell
// size safe minimum.
int RunHygieneScenario();

// M0/MAP-1: map data audit -- legacy loader vs the server's package path for
// every finding of docs/map-data-layer-requirements.md (evidence probe).
int RunMapAuditScenario();

// MAP-1: world package corpus through the server loader -- positive packages
// (v3 with/without client data, v2, the checked-in test map vs the legacy
// loader) and one fixture per rejection rule with its stable error code,
// plus the map-load baseline. `fixtures_out` (optional) also writes the
// packages the gameserver startup acceptance script launches against.
int RunWorldPackageScenario(const std::string& fixtures_out);

} // namespace gs::bench
