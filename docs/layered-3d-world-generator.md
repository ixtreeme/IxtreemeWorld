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
