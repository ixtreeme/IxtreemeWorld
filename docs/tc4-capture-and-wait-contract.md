# TC-4 capture, waiting and acceptance contract

The authoritative commit/publication lock and first-next-supervisor-phase wake
contract from TC remain unchanged. Acquiring the last active publication lock
defines the coherent input cut. Later commits belong to the following phase.
Wake/runnable changes do not advance NextTick or insert an extra simulation
tick; the authoritative cadence remains 20 Hz. Periodic activity generation,
demotion hysteresis, ownership, reclamation and snapshot behavior are unchanged.

The optimized capture computes the existential source/leaf geometry leaf-first,
stopping at the first positive source for each active leaf. It retains every
source and commit/pending stamp in the immutable frame. The existing revision
and active-incarnation comparison controls reuse. Resident eligibility is still
evaluated every scheduling phase, independently from cached geometry. Capture
does not retain entity or zone pointers. An unaffected leaf checks all sources.

`--wake-profile` enables bounded aggregate observations; it is OFF by default.
`TC4PROFILE total_ns` is elapsed capture time, not CPU. Prep, mutex acquisition,
revision, source copy, geometry query, output insertion and other time are
disjoint; `lock_window_ns` overlaps these stages and must not be added again.
Query counters count actual visits/tests, whose number changes with traversal.
Producer wait/hold measures authoritative publication calls, including calls
whose position did not change. Worker CPU and supervisor CPU use Windows
GetThreadTimes; unsupported platforms must report NOT MEASURED.

Maximum fields are lifetime maxima including setup/warmup. Capacity gauges
describe the last observed capture, estimate container storage, and exclude
allocator overhead; they are not allocation-profiler heap peaks. Growth counts
observe vector capacity changes, frame creation and inserted set nodes.
Revision temporary capacity and implementation-specific hash-node overhead are
not fully covered by the scratch estimate. Working set is reported separately.
The coalesced oldest-pending update age is not a sample for every Position write.
Rolling readiness tick quantiles are not exact global measure-window quantiles.

`c-moving-v1` retains its 20 ms status polling and route-v1 semantics.
`c-moving-v2` uses the same route, serial preparations, population, cache and
request deadlines; only waiting for a terrain operation differs. Each request
owns its status mutex and condition variable. Status publication under that
mutex precedes notification, closing the predicate-to-sleep gap. Ready or any
terminal status satisfies the predicate; spurious notification alone does not.
The wait uses the original request deadline and never consumes, pins, pumps,
snapshots or extends an operation. A wait timeout can return Pending while the
world owner has not yet published TimedOut; callers must not treat it as Ready.
Cancel/Consume acknowledgements retain the existing world-owner precedence.
The existing bounded request ledger includes `sizeof(TerrainRequest)`, hence
also the added synchronization objects. No consumer callback retains a session
or entity, and stop publishes terminal status before the request is released.

The 30-second C measurement interval is an action-start window. A batch begun
before its cutoff may finish afterwards; no batch starts after the cutoff.
Nominal eligibility is 0/10/20 seconds; actual next eligibility is previous
actual batch start +10 seconds. All three requested placement checks and 1500
registered/Ready/Consume/effect operations remain required. Request timestamps
are relative microseconds with a +1 presence sentinel; cross-clock comparisons
allow that 1-us encoding offset, never a wake tolerance. Trace lists are bounded
and joined by request identity, with reset at the measurement boundary.

`captureprobe` times production capture against a deterministic fixed source
trace, with independent full-pair geometry checking outside the timed calls.
`tc4temporal` and `terrainwait` add concurrency, boundary, eligibility, topology
and wait negative controls to the existing `activitytemporal` suite. A private
test-only pending-predicate hook exercises the same wait implementation;
production supplies an empty inlined callable and stores no hook.

`gameserver/scripts/tc4_acceptance.py` reads serial frozen run manifests and
reports CURRENT_ACCEPTANCE, LEGACY_DIAGNOSTICS and PLATFORM_COVERAGE separately.
Only exact legacy uniform-parallelism/reference-parallelism failures can be
diagnostic once the mandatory scheduler correctness/fixed-work gates pass.
Unknown failure, crash, timeout, missing record or changed binary/source hash
blocks acceptance. Native exits and the old SL-2 runner remain unchanged.
The TC-4 evidence protocol freezes performance gates before the first U build.
