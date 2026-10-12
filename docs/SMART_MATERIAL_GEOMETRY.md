# Static mesh geometry baking

Implemented: 2026-10-12. This is the CPU reference and editor atlas workflow;
live GPU graph rendering and GPU baking remain unimplemented.

## Production workflow

The quick path for a full smart material: select the static model in Hierarchy,
open the material, then **Material preview > Apply to model > Bake & Apply to
selected model**. This uses the selected slot/submesh and the same full model
matrix as the renderer, bakes asynchronously, publishes the native material and
assigns the captured slot. A valid geometry snapshot is saved inside the material
folder; no intermediate cache or definition save is required. Resolution is
128/256/512. Cancellation publishes no material. Model/source/transform/slot changes
are checked before assignment; unrelated slots and entity edits are preserved.
The current graph and instance overrides are captured at the start. The following
manual workflow remains available for custom sampling and reusable instances.

1. Open/create the material, for example **Templates > Smart Weathered Stone**.
2. In **Material preview > Geometry Bake**, use **Select mesh...**, then choose
   a static submesh. The existing Assimp importer supplies its local coordinates,
   indexed triangles, normals and UVs. Set atlas resolution, AO samples, padding,
   world ray distance/bias and explicit world translation.
3. **Bake geometry** runs an immutable mesh/settings snapshot on a standard C++
   worker. Progress and **Cancel geometry bake** are available. Importing the
   model and final asset publication are still on the editor thread.
4. **Mesh UV atlas preview** evaluates the material on covered/padded texels with
   the real world position/normal and AO/curvature/cavity. The Channel control also
   displays raw geometry maps. Normal maps display as encoded XYZ; position maps
   normalize each axis to covered bounds; thickness displays divided by ray distance.
   Scalar maps display linearly. This view is the UV atlas, not a 3D mesh view.
5. **Save bake...** writes a native `.ixgeometrybake` and its source model GUID
   dependency. **Open bake...** validates both the cache and actual source content.
   Save the material definition and **Save instance...** to retain the cache GUID.
6. **Bake UV material > Bake to Asset Browser** uses the saved geometry resolution
   in mesh mode. It creates the existing native material/images plus an exact
   geometry snapshot and source dependency metadata under Materials/GraphBakes.
   Assign the material with the existing workflow, to matching UVs/placement.

Analytic sphere/plane previews continue to expose explicit test inputs, clearly
labelled. A mesh atlas with missing/stale/corrupt geometry reports an error and
retains the previous valid image. Settings edits keep using the last successful
bake until rebaking. Ordinary UV-only baking still rejects world/geometry graphs.

## Algorithms and conventions

UVs cover a single 0..1 atlas. Texel centers use barycentric interpolation of
position and normalized shading normals. Degenerate geometry/UVs, non-finite
values, invalid indices, inconsistent winding/normals, non-manifold welded edges
and overlapping sampled interior coverage are rejected. Shared triangle boundaries
use a deterministic first owner. Overlap detection is at the selected texel
resolution; sub-texel overlap is not an exact UV-polygon intersection test.

Topology welds **exact equal positions**, including duplicates at UV/normal seams.
Near-equal unwelded positions are reported as open boundary edges, not guessed to
be the same surface. UV-separated submeshes are baked separately. Coincident
disconnected surfaces may be non-manifold and need separate input submeshes.

Curvature is a dimensionless signed edge-dihedral proxy, linearly interpolated
from welded vertices: `0.5 + 0.5 * length-weighted signed angle / pi`. Flat is 0.5,
convex is above 0.5, concave below. Cavity independently accumulates only concave
edge angles, divided by pi. Both are in object topology, independent of AO and
ray distance. They are not continuous mean/Gaussian curvature or texture-derived
approximations; triangulation and topology affect the vertex averaging.

AO is visibility from deterministic rotated cosine-hemisphere samples. A median
BVH accelerates two-sided world-space triangle intersections. The shading normal
sets the hemisphere and ray bias; the source triangle is excluded. Ray distance
limits blockers. Thickness rays travel opposite the shading normal, starting
inside by the bias, and measure distance to an outward exit face. An open surface
or an exit beyond the configured distance returns zero thickness. Boundary-edge
counts remain visible; zero is not a claim that an open mesh is a solid body.

Object/world position and normal are separate maps. The explicit row-major affine
matrix multiplies a column vector; position uses the full transform, normal the
inverse transpose. Singular/projective matrices fail. Nonuniform scale and
reflections are supported by the API. Rays, bias and thickness use the transformed
coordinate units (metres when the input is in metres). The panel exposes translation;
automatic scene-object transform selection is future work. Tangent/bitangent come
from each triangle's world edges and UV derivatives, orthogonalized against the
interpolated world normal with UV handedness retained.

Padding is deterministic bounded 8-neighbour dilation (Chebyshev distance), never
wrapping at atlas boundaries. It copies all maps, tangent frame and original source
UV. Material padding therefore samples the original covered texture coordinates.
Real coverage, padding and empty texels remain distinguishable. Empty atlas texels
do not force an otherwise opaque published mesh material into a blended pipeline.

## Cache, identity and lifetime

`GeometryBaker.*` has no asset database, importer, UI or material evaluator. Public
APIs operate on value arrays. `GeometryAssets.*` is the editor-only native bridge.
`IXMaterialGraph` links the independent baker to provide the shared material images
integration; editor-disabled tests use the same implementation.

Cache v1 is little-endian and begins with `IXGEOM01`. It includes 88 bytes of
settings, mesh content hash, coverage/boundary counts, 97 bytes per texel (coverage,
UV, object/world positions/normals, world tangent/bitangent, AO/curvature/cavity/
thickness) and a trailing 64-bit FNV-1a checksum. Exact size, settings, finite/ranged
values, orthonormal frames and coverage counts are validated before replacement.
FNV is a content/integrity check, not a cryptographic authenticity mechanism.
Source hash covers vertex position/normal/UV and indices in stable input order.
Algorithm/schema changes require a new cache version.

Native `.meta` records exactly one source model dependency. Bind/reload/publish
reimport and check actual content, so GUID moves retain geometry while edits fail
as stale. Polling checks model/cache paths, mtime/size and metadata stamps; **Reload
geometry dependencies** forces content checks after stamp-preserving edits. A
cache returned for an old mesh cannot replace a newer selected source. A failed
bind leaves the prior binding intact; a changed active dependency invalidates
sampling and keeps the old preview image with a diagnostic.

Limits: 16..512 square, 0..32 padding, 1..128 AO samples, 1M vertices/triangles,
finite invertible transform and positive bias below ray distance. Native cache
reads are bounded to 28 MiB. A 512-square serialized cache is 25,428,088 bytes;
in-memory maps, raster workspace, mesh, topology and BVH consume additional memory.
Limits are defensive bounds, not a claim that 1M-triangle input was stress-tested.

Worker ownership is local to `GeometryBakeJob`. It receives copies, touches no
native database/library/graphics resource, reports atomic progress, and checks
cancellation throughout validation/raster/ray/padding stages. Final output replaces
the UI binding only on success and a matching current mesh hash. Destruction
requests cancellation and joins before job state is destroyed. BVH construction
and topology reduction are synchronous stages within the worker; cancellation may
wait for one such stage. Material preview/image export still run synchronously.

## Current acceptance boundaries

Plane, closed convex/inward boxes, a concave corner, seams, mirrored/nonuniform
transforms, real OBJ import, cancellation, corruption, moves, source invalidation,
instance reload and Weathered Stone native export have automated tests. Broader
production meshes, GPU readback, terrain, skinned baking, external occluder scenes,
multi-submesh atlas composition and an actual 3D preview remain future validation
and engineering work. Dedicated cache/browser categories are not implemented.
