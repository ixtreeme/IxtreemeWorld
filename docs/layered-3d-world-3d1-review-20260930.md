# Layered 3D 3D-1 review — 2026-09-30

## Result

3D-1 is complete as a contract-only milestone. The current production
world path is unchanged: legacy packages still use their existing
heightfield behaviour, and no layer-aware AOI, movement, collision,
migration or client replication path is enabled.

## Implemented

- `shared/map/include/map/LayeredWorld.h` defines stable `layer_id` and
  `volume_id` identities, finite 3D volumes, and explicit source/target
  portal endpoints.
- `shared/map/src/LayeredWorld.cpp` validates the contract before runtime
  use and provides deterministic point lookup and portal endpoint checks.
- `shared/map/tools/layered_world_contract_test.cpp` covers stacked floors,
  uncovered z gaps, explicit traversal, overlapping-volume rejection,
  portal-bound rejection, and legacy empty-contract compatibility.
- `docs/layered-3d-world-contract.md` records the boundary and the next
  milestones.

## Verification

Configured build: `gameserver/build/windows-debug` (Debug, Worldbench
enabled).

Command:

    cmake --build gameserver/build/windows-debug --config Debug --target layered_world_contract_test --parallel 4

Executable result:

    LAYERED3D stacked-valid: PASS
    LAYERED3D ground-lookup: PASS
    LAYERED3D upper-lookup: PASS
    LAYERED3D gap-uncovered: PASS
    LAYERED3D portal-traverse: PASS
    LAYERED3D overlap-rejected: PASS
    LAYERED3D portal-outside-rejected: PASS
    LAYERED3D legacy-empty-contract: PASS
    LAYERED3D legacy-no-implicit-volume: PASS
    LAYERED3D summary: failures=0

The initial run exposed a test-fixture error: its supposed outside portal
rectangle was still inside the target volume. The fixture was corrected to
cross the target boundary; validation was not weakened.

## Decision for 3D-2

The next safe step is package serialization/versioning plus a strict fixture
containing a ground floor, upper floor, underpass and water surface. Only
after that should the spatial index/AOI key gain a volume component. Model
collision, movement, z transitions, replication and StandaloneVulkanClear
integration remain later milestones.
