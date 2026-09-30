# Layered 3D generator review — 2026-09-30

## Result

The first offline authoring generator is complete. It accepts adapter-neutral
collision/navmesh surface records, merges compatible adjacent surfaces,
derives volume kind and semantic tags, validates the result, and writes the
existing `MX3D` sidecar format.

## Verification

The generator fixture contains five source surfaces: two adjacent road tiles,
an upper building floor, an underpass and a water surface. It produces four
volumes, merging only the two road tiles. The generator test also rejects an
unknown tag.

The CLI was run with:

    layered_world_generate shared/map/tools/layered_surfaces.example.txt 0 0 16 16 <output.mx3d>

Result:

    generated volumes=4 surfaces=5 merged=1
    LAYERED3D GENERATOR summary: failures=0

## Boundary

This is the reusable generation core and text adapter. It does not parse FBX
or glTF directly; a model/navmesh importer should emit the adapter records.
Portals stay explicit because proximity alone cannot distinguish a stair, lift,
blocked wall or teleport. The server runtime still does not admit layered
entities until authoritative volume ownership is implemented.
