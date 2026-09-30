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

## Next milestone boundaries

3D-2 will add package serialization/versioning for these declarations and a
strict fixture with a ground floor, upper floor, underpass and water surface.

3D-3 will make the spatial index and AOI key layer-aware. The key must not
remain only `(cell_x, cell_y)` once layered entities are admitted.

3D-4 will add server-cooked model collision/navigation and explicit portal
transitions. 3D-5 will connect movement, migration, replication and the
StandaloneVulkanClear client.

No layer-aware production path should be enabled before the fixture and
contract tests prove deterministic lookup, non-overlap, portal validation,
legacy compatibility and failure-before-runtime behaviour.
