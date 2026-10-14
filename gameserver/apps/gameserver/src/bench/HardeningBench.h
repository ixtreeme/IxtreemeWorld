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

// MAP-2: file-backed terrain against an independent height oracle (generator
// function + hand-computed samples): seams, partial edge chunks, levels,
// slope, negative heights, the half-open world edge, NotResident vs a
// legitimate 0 m, height layer v2 int32 range; plus the runtime edge rules
// (movement without clamp, spawn fallback, mobs inside, loaded bounds in
// every world-level consumer).
int RunTerrainScenario();

// MAP-2: forced split 1 -> 4 -> 16 and merge back on a non-flat, negative-
// origin, non-square world from the real loader (no terrain reload, no entity
// duplication, partition-independent world queries at every step), the
// min-size refusal diagnostic, and the checked-in test map under the approved
// R2 partitioning.
int RunMapSplitScenario();

// MAP-2 review: aggregate bootstrap resources -- startup cost of the initial
// partition (1 .. max_grid^2 initial zones on an empty 100 km world:
// construction/start time, working set, idle supervisor cost), the enforced
// aggregate caps (initial zone count, world coordinate range, terrain
// samples) at the limit and one past it.
int RunBootstrapScenario(int max_grid);

// MAP-3: chunk streaming -- the streamer against a scripted chunk source
// (cold/hit/miss, one load for many requesters, out-of-order completion,
// bounded retry then InvalidData, CRC failure on load, seam rejection,
// budget/admission/eviction, pins above the budget, free only in quiescent
// windows, cancellation, stale completions, shutdown with reads in flight)
// and the runtime on a streaming world against a fully resident reference
// provider (batch spawning, movement across chunk and zone borders, split /
// merge with slow I/O, generation reset, waiting for data, shutdown).
int RunStreamingScenario();

// MAP-3: world queries -- static collision on the movement path (thin wall vs
// large step, chunk border, corner squeeze, slope, NotResident / outside),
// water (undeclared = unknown, none, sea level with a depth oracle, bodies,
// ground not resident, deep-water rule, package rules), navigation (NoPath
// in the window, detour and long cross-chunk paths vs a Dijkstra oracle,
// budget, cancel, outside, flat = unsupported, streaming waits + pins,
// bounded wait) and the runtime (wall, shore, PostNavigationRequest).
int RunWorldQueryScenario();

// MAP-3: 100 km x 100 km file-backed world (9604 chunks) with a `budget_mb`
// terrain cache (default 16): cold startup eager vs streaming, then
// `seconds` of 200 roaming players whose active areas keep moving (real
// misses / loads / evictions), the full own memory / I/O accounting vs
// process RSS, heights vs the eager reference.
int RunStreamingSoak(int seconds, int budget_mb);

// MAP-3 review follow-up: lifetime and failure-path evidence -- lock-free
// tick readers vs eviction with poisoned frees (+ a deliberately broken
// negative control), a late completion across migration / retire / reclaim
// / slot reuse, a sleeping zone releasing its terrain and waking onto
// missing data, permanently failed chunks at runtime (spawn, movement,
// queries, navigation), and the startup set above the budget.
int RunStreamLifecycleScenario();
int RunStreamAdmissionScenario();
int RunMap4Scenario();

} // namespace gs::bench
