# Layered 3D authoring generator

The offline generator converts collision/navmesh adapter surfaces into the
`layered_world.mx3d` sidecar. It is deliberately separate from the server
runtime.

## Input

The current adapter text format is one record per line:

    surface id name min_x min_y max_x max_y min_z max_z tag|tag 0|1

Names contain no whitespace. Blank lines and `#` comments are ignored. The
supported tags are `ground`, `building`, `bridge`, `water`, `underwater`,
`dungeon`, `interior`, `connector`, `road`, `stairs`, `lift` and `dock`.
The final field says whether normal ground movement is supported.

An example is in
`shared/map/tools/layered_surfaces.example.txt`. A real FBX/glTF/navmesh
importer can emit the same adapter records without touching the generator or
the gameserver.

## Generation

Build and run:

    layered_world_generate <surfaces.txt> <min_x> <min_y> <max_x> <max_y> <output.mx3d>

The generator:

- rejects invalid, duplicate or unknown-tagged source records;
- merges adjacent surfaces only when their XY and Z gaps are within the
  configured tolerances and their semantic families agree;
- derives `VolumeKind` from tags;
- ORs tags across merged surfaces;
- assigns deterministic volume and layer ids by vertical order and source
  identity;
- validates the generated world before writing it.

Portals are intentionally not guessed from proximity. A nearby vertical
surface may be a stair, lift, blocked wall or teleport. Portal records remain
an explicit authoring decision until collision/navmesh connectivity can prove
the transition.

The generated sidecar is still only package geometry metadata. Production AOI,
movement and migration will consume it after authoritative entity-volume
ownership is implemented.

## Engine collision adapter

The integrated editor now has a direct adapter in `SceneLayerAuthoring`.
Static Box and Mesh colliders can opt in through **Inspector → Layer generation**;
their semantic tags survive scene save/load. **Tools → Layered world** provides
generation, a bounded volume preview and export to a fresh sidecar path.

`ExtractLayerSourceSurfaces` derives floor bands from actual transformed collision
triangles. It accepts coplanar upward components with proven axis-aligned
rectangular footprints, and rejects holes, concave or rotated footprints instead
of inventing floor within a render AABB. This adapter uses
`require_exact_footprints=true`, which disables the text adapter's legacy merges.

Coordinates are metres: engine `(x, y-up, z)` becomes package `(x, z, y-up)`.
Engine terrain is centered, so package bounds and origin must use
`(-width/2, -depth/2)`. The sidecar export alone does not export the complete
terrain package or activate layered movement. The production `WorldRuntime`
retains validated metadata and offers a read-only point query.

See `layered-3d-authoring-engine-review-20260930.md` for verified editor usage,
tests, geometry limits and the next runtime integration milestone.

## Complete terrain package export

**Tools → Layered world → Export strict server world** now exports the
current scene's terrain, blocked cell attributes, explicit terrain spawn,
declared water and generated optional layer metadata together. Enter a
stable ASCII **World ID** (1–64 letters, digits, `_`, `.` or `-`) and the
authored spawn in engine **X/Z metres**. A saved scene and a complete real
terrain height grid are required. Blocked/outside spawns fail; no position
is searched or relocated. Spawn remains terrain-based in this milestone.

The editor publishes a fresh `<scene>.server-worlds/export-<timestamp>`
directory only after a full strict server load. Existing complete or
partial target/staging directories are preserved. The required height v3
surface contract matches the Jolt triangle diagonal after source row
conversion; height v1/v2 packages keep their previous bilinear semantics.
This is a server data package; render assets and model collision geometry
are not included. Layer metadata does not activate upper-floor movement.

New scene saves additionally declare an exact float-centimetre height
sidecar, preserving sculpt precision and range across reload. A missing or
malformed declared sidecar fails instead of loading rounded chunk heights.
See `layered-3d-terrain-package-review-20260930.md` for measured numerical
limits, validation evidence and the later connected engine test project.

## Cooked support and the offline editor probe

Collision cooking now retains the plane of each proven rectangular upward
component and attaches it to its exact generated volume. New sidecars use
MX3D v3; old v1/v2 metadata still loads, with unknown support. The gap/AABB
text generator cannot turn its bounds into a floor. Water is not walking
support even if a manually authored movement flag says otherwise.

**Tools → Layered world → Support probe (offline)** accepts an explicit
generated **Support volume** and an engine **X/Z** point. **Place support
probe** computes the floor and shows engine X/Y/Z plus volume/layer id.
**Move support probe** stays on that same volume, refuses uncovered points
and preserves the prior pose on failure. Selecting another volume requires
a new explicit placement; it does not automatically use a portal. The
probe binds one immutable bake and refuses a move after the scene's layer
data changes. This is an authoring check, not a connected game character.

The same support API is available through WorldRuntime's read-only ground
queries. Full strict package export already carries these optional planes
inside `layered_world.mx3d`; no additional terrain format or map converter
is introduced. Support height is not proof of walls, headroom, slope policy
or a traversable staircase. See `layered-3d-ground-support-review-20260930.md`.

## Clearance bake and proven step portals (3D-4B)

**Generate layers from collision** now also runs `CookLayerClearance` after
volume generation. Obstructions are every enabled, non-trigger static
collider of the scene — whether or not it is opted in to layer authoring —
plus the terrain height grid. Box and Mesh colliders give their exact
collision triangles; Sphere, Capsule and ConvexHull give a conservative
axis-aligned box plus Jolt's 5 cm convex radius. Triggers, the NoCollision
layer, character controllers, skinned and dynamic/kinematic bodies are not
static world and are skipped. A missing terrain grid or unreproducible
collider fails the generation instead of assuming free space.

Stairs are authored as separate walkable boxes whose footprints touch
exactly (for example 0.75 m treads, 0.25 m risers); each touching edge
within the step height becomes a proven portal. A tread must be at least
about radius + cell (≈ 0.6 m) deep to contain standable cells; the corridor
covers the riser zone. The status line reports blocked/total cells, the
number of static colliders and the proven portals.

**Show layer volumes** additionally draws blocked cells (X marks on the
support plane), proven portal edges and the probe's capsule axis; the
existing line renderer blends these colours with the scene. The support
probe now uses the baked actor: placements into blocked cells answer
`blocked`, moves may not cross walls, and a move to another volume succeeds
only across one proven portal ("ok via proven portal N"). Strict export
writes MX3D v4; `SceneLayerGroundTest --validate-package <dir>` validates
the profile, grids and portal crossings of an exported package. See
`layered-3d-clearance-portal-review-20260930.md`.

## Layered player spawn (3D-5B1)

**Export strict server world** has a **Player spawn volume** field next to
**Player spawn X/Z**. Volume 0 keeps the terrain spawn (the terrain cell
must be walkable). Any other value is the id of a generated layer volume
(as listed in the generation status): the export binds the spawn region to
that volume, writes worldlogic v2, and both the strict loader and the
export itself require the baked capsule to fit at the exact point. A
layered spawn above a blocked terrain cell is valid. The gameserver then
admits players onto that volume. See
`layered-3d-spawn-volume-3d5b1-review-20261001.md`.
