# Layered 3D 3D-3 review — 2026-09-30

## Result

The first 3D-3 spatial-identity slice is complete. The shared key now carries
`(volume_id, cell_x, cell_y)`, so stacked floors with the same horizontal cell
cannot alias in a derived AOI/index map. `volume_id = 0` remains the explicit
legacy identity and is distinct from every declared volume.

## Verification

`layered_spatial_key_test` verifies:

- same-volume keys compare equal;
- upper and ground volumes at the same XY cell compare different;
- legacy and explicit volume identities compare different;
- an unordered map retains all three values independently.

Result:

    LAYERED3D SPATIAL summary: failures=0

## Boundary

The production `SpatialGrid`, movement and migration paths are intentionally
unchanged in this slice. They currently have no authoritative per-entity
volume component, so wiring the new key alone would create a partially
layered runtime. The next implementation must add volume ownership at the
entity/movement boundary, then update GridSlot, insertion, movement, AOI
queries and cross-volume portal transitions as one tested contract.
