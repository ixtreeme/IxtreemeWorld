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
