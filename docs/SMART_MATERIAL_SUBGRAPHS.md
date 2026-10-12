# GUID-backed material subgraphs

Implementation milestone: 2026-10-12. Subgraphs use the existing material document,
CPU evaluator, UV baker and HLSL backend. They are expanded before the existing IR
compiler; there is no runtime interpreter or parallel material compiler.

## Data and compatibility

`MaterialSubgraph` stores interface version 1..65535 and a canonical graph. The
`.ixmaterialsubgraph` wrapper has format `ixtreeme.materialsubgraph`, file version
1 and native AssetDatabase GUID metadata. It contains no PBR Output or managed
Layer Stack. At least one connected output is required.

Three appended node kinds declare inputs, declare outputs and call an asset.
Definitions containing them use graph version 4; existing v1/v2/v3 files retain
their formats and load unchanged. Call nodes store the authoritative asset GUID,
interface version and ordered typed port snapshot. Port IDs refer to stable
declaration-node IDs in the asset, not port names or saved array indices.

Scalar, Color, Vector2, Vector3, Texture and Surface ports are supported, with at
most 14 ports per call. Numeric defaults are stored on the caller. Changing the
asset's numeric default affects newly bound/refreshed calls; existing calls retain
their saved defaults. Texture and Surface inputs must be bound. Standalone
authoring previews numeric defaults and reports unsupported unbound resource
inputs instead of inventing them.

`BindMaterialSubgraph` is an explicit transaction. It remaps connected pins and
layer-mask references by stable port ID and type. Removing or changing the type
of a connected port fails without modifying the definition. There is no silent
loss of links. Schema/version/name changes require an explicit interface refresh.

## Expansion and instances

`ExpandMaterialSubgraphs` validates resolver results, detects active GUID cycles
and bounds nesting to 16, calls to 256, expanded nodes to 2048, links to 8192 and
IDs to the existing 24-bit range. All call references are validated, including
disconnected ones, before reachability elimination in the existing compiler.
Resolver results are cached per GUID for a compilation; caller-specific bodies
are then expanded into deterministic generated IDs. Stored node/link array order
does not affect generated HLSL.

Declared inputs are replaced by bound parent outputs or numeric constants;
outputs are aliases. Structural nodes never reach evaluation/HLSL. The saved
definition is retained. A call output can feed an ordinary node, a PBR channel,
another subgraph or a managed layer mask. Geometry requirements propagate from
the expanded ordinary nodes into existing bake/preview diagnostics.

Published parameter keys are scoped by stable call IDs, for example
`Subgraph_6/Scale` and `Subgraph_6/Subgraph_9/Scale`. Repeated asset calls expose
independent parameters. `MaterialInstanceBinding` keeps sharing the compiled
program; override updates do not expand or compile. Published-name changes report
stale override errors. `MaterialCompileResult` records sorted GUID dependencies
and generated-node provenance; HLSL comments include source GUID/node/scope.

## Real asset/editor integration

The native database recognizes `.ixmaterialsubgraph`. `GraphSubgraphCache` resolves
GUIDs through that database, uses bounded atomic-file IO, and checks native path,
mtime and file size. Paths follow native GUID moves. Relative legacy texture
paths inside a subgraph resolve relative to its file; texture GUIDs retain their
existing authoritative semantics.

The existing panel creates, opens and saves subgraphs. It draws typed ports,
binds/refreshes calls, previews output declarations, exposes compiled scoped
parameters and detects changed nested files. Malformed/missing dependencies keep
the last valid program/image. Unused cache entries are removed after compilation.
Definition/subgraph saves write native GUID dependency metadata. The file picker
and graph Inspector are integrated; a dedicated AssetLibrary browser category
for subgraphs is still planned.

UV publishing first captures an expanded definition and evaluates that snapshot.
`source.materialgraph` stores the effective self-contained graph with overrides;
`definition.materialgraph` retains the authored call graph and native dependency
closure. External changes during baking cannot change the evaluated snapshot.
Original project definitions/subgraphs are never overwritten by baking.

## Validation and limits

`MaterialSubgraphTest` covers serialization, all six typed reference values,
defaults, scoped overrides, repeated/nested calls, output previews, UV baking,
managed masks, deterministic source, missing assets/resolver, recursion, interface
refresh errors, ID exhaustion and depth/node/call bounds. It compiles real expanded
HLSL to DXIL/SPIR-V using configured DXC. Editor/baking integration tests use real
files, metadata, moves, invalidation/recovery, instances and native material images.

No GPU numerical parity, runtime shader binding, geometry baker or reusable smart
generator presets are claimed. File polling is mtime/size-based, not content-hash
invalidation; manual Reload subgraphs handles same-stamp edits. Cache polling and
expansion are synchronous editor operations. There is no standalone runtime-player
or asynchronous subgraph compilation test in this milestone.
