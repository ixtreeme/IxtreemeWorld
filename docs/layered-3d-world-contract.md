# Layered 3D world – 3D-0/3D-1 contract

## Scope

The first layered-world milestone adds a validated contract for stacked,
model-backed spaces. It does not yet change production AOI, movement,
collision, migration or client rendering. Existing heightfield maps remain
valid and keep their implicit legacy layer.

The target is a Metin-style world with buildings, dungeons, bridges, caves
and water spaces that can reuse the same horizontal footprint at different
heights. A voxel grid is deliberately out of scope.

## Coordinate and identity model

An entity keeps the authoritative continuous position:

    x, y, z

Layered maps add a stable volume identity:

    layer_id, volume_id, x, y, z

`layer_id` is a logical grouping such as a floor, water surface or dungeon
level. `volume_id` identifies the actual navigable/collision volume. A layer
is not a permission to teleport by changing `z`; all transitions go through a
declared portal.

Legacy packages have no declared volumes and retain the implicit
`kLegacyLayerId=0` behaviour. The empty layered contract is therefore valid
and does not alter existing maps.

## Volumes

Each `LayerVolume` is a finite half-open 3D box:

    [min_x, max_x) × [min_y, max_y) × [min_z, max_z)

The first contract uses an XY rectangle plus a z interval. This is a broad
phase and identity contract, not final collision geometry. Model-derived
server collision meshes/BVH data will be added in a later milestone.

Two volumes may overlap in XY, which is how stacked floors share a footprint.
Their open 3D interiors may not overlap. This makes point-to-volume lookup
deterministic and prevents an entity from belonging to two volumes at once.

Supported initial kinds are `ground`, `interior`, `water_surface`,
`underwater` and `connector`. The kind is descriptive; collision and
movement rules remain explicit runtime data.

## Portals

`LayerPortal` is the only declared transition between volumes. It has a
source footprint, a target footprint, and separate source and target z
intervals. A stair, lift, door, dungeon transition, lock or river connection
can use the same shape. The intervals are separate because a transition from
the ground floor to an upper floor does not happen at the same absolute z.

Validation requires both endpoint footprints to lie inside their respective
volume bounds and each endpoint z interval to lie inside its own volume.
Runtime movement must still perform collision, clearance, ownership and
gameplay checks.

## Validation rules in this milestone

- world bounds are finite and non-degenerate;
- volume and portal IDs are non-zero and unique;
- every volume is finite, non-degenerate and inside the world XY bounds;
- volume interiors do not overlap in 3D;
- portals reference two different declared volumes;
- portal footprints are finite and inside their endpoint volumes;
- each portal endpoint z range is inside its corresponding endpoint volume;
- an empty volume/portal set is a valid legacy contract;
- point lookup returns at most one volume after validation.

## Package integration (3D-2)

Version 3 of the optional `layered_world.mx3d` sidecar stores bounded
volume and portal records, semantic volume tags and optional cooked ground
support planes in little-endian form. Version 1/2 files remain readable;
they have no ground-support proof and ground queries return unavailable.
A package without this file
keeps the legacy contract. When the file is present, the strict package
loader decodes it, validates its record envelope, validates the 3D contract
against the package XY bounds, and exposes it through `ServerWorldData`.
Malformed headers, unsupported versions, truncation, trailing bytes, unknown
tags, record
limits and geometric violations reject the package before runtime creation.

The strict fixture covers a ground floor, upper floor, underpass and water
surface, including portal traversal and malformed-sidecar rejection.

## Next milestone boundaries

3D-3 has defined the layer-aware spatial identity in
`LayeredSpatialCellKey`. The production spatial index and AOI admission still
need to adopt it together with authoritative entity volume ownership; the key
must not remain only `(cell_x, cell_y)` once layered entities are admitted.

3D-4A preserves a proved rectangular collision component's plane, original
mesh source id and deterministic component id. The plane evaluates as
`z = anchor_z + slope_x*(x-anchor_x) + slope_y*(y-anchor_y)`; its measured
maximum deviation from source vertices is stored separately. Occupancy
`min_z`/`max_z` is never reinterpreted as floor height or capsule clearance.
The legacy gap/AABB merge path discards support proof. Water/underwater and
non-ground movement policy cannot produce walking support.

`ResolveLayerGroundPlacement` requires an explicit volume id.
`ResolveLayerGroundMove` checks the current pose/identity against that
surface and retains it on failure. Ordinary moves cannot change volume;
even a declared portal returns `TransitionRequired` until explicit physical
transition validation is implemented. There is no nearest-floor selection
or fallback from unknown support to terrain.

These queries are read-only over a validated immutable world. WorldRuntime
exposes them for offline ground-state checks; the engine uses the same API
in an immutable scene adapter and an editor support probe. They do not
admit production entities or replace the CharacterController. Generated
volume/component ids belong to one bake; structural rebakes may renumber
them. A new dataset requires explicit placement, not carrying old ids.

Remaining 3D-4 work is model blocking collision/clearance/navigation and
explicit portal transitions. 3D-5 connects authoritative entity ownership,
movement, migration, spatial/AOI/ghost paths, replication and the client.

No layer-aware production path should be enabled before the fixture and
contract tests prove deterministic lookup, non-overlap, portal validation,
legacy compatibility and failure-before-runtime behaviour.
