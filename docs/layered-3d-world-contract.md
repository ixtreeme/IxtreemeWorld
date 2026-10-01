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

3D-4B adds an optional `LayerPortalProof` to a portal. It is only derived
for two walkable volumes whose support rectangles touch exactly (float-equal
shared edge, axis-aligned) and whose planes differ by at most the profile's
step height along the shared span. The proof stores the edge axis/position,
the span, the measured maximum step and a 2D corridor grid (slots along the
span x cells across a band of edge ± (radius + cell), clamped to both
footprints). A corridor cell is clear only when the actor capsule is free
above the upper envelope of the two planes (+ floor contact). Validation
recomputes the geometry and the step from the planes; any mismatch rejects
the world. Authored portals without a proof remain metadata only.

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

Version 4 (3D-4B) is written only when the world carries a clearance
profile; a support-only world still encodes byte-identical version 3. The
v4 header adds the profile (cell size, actor radius, actor height, step
height, floor contact; 5 x f32). Each volume record gains a flag block and,
when present, `cells_x`, `cells_y` and a bit-packed blocked grid; each
portal gains a proof block (axis, edge, span, f64 max step, slots, across,
bit-packed corridor). Padding bits must be zero. Per-volume and total cell
counts are bounded, and the encoded sidecar is refused above 4 MiB. The
profile is present iff at least one grid or proof is present; a grid must
match its volume footprint exactly and requires valid ground support.

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

3D-4B adds the actor contract. `LayerClearanceProfile` is the actor class
the world was baked for (editor: the CharacterController default capsule,
radius 0.35 m, height 1.8 m, step 0.35 m, 0.25 m cells, 2 cm floor
contact). Each walkable volume's clearance grid marks a cell blocked when
any static obstruction triangle (every static collider plus the terrain,
both quad diagonals) intersects the region a capsule centred anywhere in
that cell may occupy: the cell rectangle dilated by the radius (a square,
conservative against the disk) between plane + contact and the highest
plane point + height (+ radius·(sec−1) on slopes). A cell is PASSABLE when
it is clear in its own grid or inside a clear corridor cell of one of the
volume's proven portals.

`ResolveLayerActorPlacement` requires the profile, an actor no larger than
the profile (`ActorNotCovered` otherwise), valid support and passable
touched cells (`Blocked`). `ResolveLayerActorMove` validates the current
pose, then requires every cell the straight segment touches (supercover,
no tunnelling) to be passable. A volume change is accepted only across a
single proven portal joining the two volumes, crossed inside its span, and
reports that `portal_id`; anything else stays `TransitionRequired`. A world
without a clearance bake answers `NoClearanceProof`. Every failure keeps
the current state. The 3D-4A support-only queries are unchanged.

Not covered by 3D-4B: dynamic/kinematic bodies and triggers (not static
world), terrain↔volume transitions (terrain is not a volume), portals
between non-touching or rotated supports, lifts/doors, navigation/path
finding, and more than one actor class per bake.

3D-5 connects authoritative entity ownership, movement, migration,
spatial/AOI/ghost paths, replication and the client. It is staged:

**3D-5A (server, done).** `LayerPresence {volume_id, layer_id}` is the
authoritative layered presence of a gameserver entity; an entity without it
is a legacy terrain entity. Presence is set only by explicit admission (the
debug spawn override's `volumeId`, validated with
`ResolveLayerActorPlacement` against the world's baked actor) and changed
only by a proven portal crossing. Its z is always the support-plane height;
terrain heights, terrain step checks and terrain warps never apply to it
(migration and partition transfers keep its z). Movement goes through
`ResolveLayerActorMove` per axis in pieces of at most 0.5 m; the target
volume is the current one or the far end of a proven portal whose footprint
holds the point; leaving the volume system is refused. The spatial index
buckets are `(volume, cell_x, cell_y)` (`LayeredSpatialCellKey`): an XY
column holds one bucket per volume, `GridSlot`/`GridEntry` carry the volume,
and the AOI radius scan reads every volume of a column, so interest stays
horizontal and floors see each other. Border snapshots, ghosts and entity
transfers carry the presence. Combat range is three-dimensional when either
party is layered. `S2cEntitySpawn` and `S2cEnterWorldAccept` gained
additive `volumeId`/`layerId` fields; transform frames do not carry volume
changes yet. The world audit (`ValidateLayeredPresence`) checks every
layered resident's volume, layer, actor placement and exact support z, and
that ghosts mirror their snapshot.

**3D-5B1 (layered player spawn, done).** A world-logic spawn region may
stand on a layered volume: `SpawnRegion::volume_id` (0 = terrain), stored
by worldlogic (`MXL1`) **version 2**, which appends a u32 volume id to every
spawn record. Version 2 is written only when a spawn is layered; otherwise
the file stays byte-identical version 1. The strict loader cross-checks
every layered spawn against the package's layered world: the volume must
exist with a clearance bake and `ResolveLayerActorPlacement` must admit the
baked actor at the region centre (`WORLDLOGIC_SPAWN_VOLUME_INVALID`, 417);
the terrain walkability rule does not apply to it. The writer refuses a
layered spawn without a v3 package and sidecar. The server's player spawn
rule places players on that volume (no terrain fallback); the editor export
takes a "Player spawn volume".

Not yet: there is no terrain↔volume transition (3D-5B2), mob spawn points
are terrain-only, transform frames do not signal volume changes (protocol
version bump) and no client consumes the new fields (3D-5C).

No layer-aware production path should be enabled before the fixture and
contract tests prove deterministic lookup, non-overlap, portal validation,
legacy compatibility and failure-before-runtime behaviour.
