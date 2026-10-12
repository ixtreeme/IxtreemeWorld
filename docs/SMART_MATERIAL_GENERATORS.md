# Smart generator presets

Implemented: 2026-10-12. Six editable mask recipes use the existing canonical
graph, subgraph expansion, IR, CPU evaluator, HLSL and geometry context. There is
no generator evaluator or generator-specific operational node.

## Editor workflow

1. **Templates > Smart Generators** or Inspector **Smart Generators > Create
   generator preset** creates a standalone recipe. Edit its graph/defaults and
   save an `.ixsmartgenerator`. The single Scalar output is **Mask**; its selected
   output preview is restored automatically. Geometry Bake supplies actual mesh
   inputs; analytic previews retain their labelled test values.
   Assets saved under the project's Assets folder appear immediately in Asset
   Browser. Existing `.ixsmartgenerator` files are discovered on project load or
   Refresh. Double-click or **Open in Material Graph** opens the recipe, with the
   usual Save/Discard/Cancel prompt protecting unsaved graph edits. Import and
   filesystem drops accept these files and give copied assets independent GUIDs.
2. Reopen a material and use **Add generator...** on a Layer Stack row. The call
   and mask assignment are one Undo/Redo transaction. Inspector **Add generator
   preset...** creates an ordinary scalar-output call for a connection or later
   mask assignment. Calls appear in Smart Generators and use ordinary pin links.
3. **Instance preview parameters** exposes controls under each call's separate
   `Subgraph_<node ID>/...` scope. Existing instance files retain these overrides
   and optional geometry GUID. The optional Scalar **NoiseMask** input accepts an
   external mask; its unconnected default is 1.
4. Select the model in Hierarchy and use **Material preview > Apply to model >
   Bake & Apply to selected model** on the full material. This bakes the selected
   submesh with the actual model transform, saves the material and assigns the slot
   in one action. It retains authored dependencies, effective source/overrides and
   geometry snapshot; separate cache saving and material drag/drop are unnecessary.

Missing/malformed presets retain the last valid image. GUID moves keep references;
file edits invalidate the shared subgraph cache. **Reload subgraphs** rereads edits
preserving stamps. Interface edits require explicit **Refresh interface**, retaining
compatible stable ID/type links or rejecting the change without silent loss.
Saving a generator as a plain subgraph is rejected to retain its metadata.

## Recipes and units

All recipes use the existing geometry context, never constant geometry call
defaults. **Scale**, **Seed**, **NoiseInfluence**, **Contrast** and a recipe amount
are published parameters. Seeded four-octave world-space value noise is multiplied
by NoiseMask. NoiseInfluence interpolates from 1 to that noise; zero disables
breakup. Contrast is a positive power curve preserving zero/one endpoints. Amount
multiplies bounded coverage opacity; it is not a simulation time or physical rate.

| Recipe | Signal before noise/contrast | Specific controls |
| --- | --- | --- |
| Edge Wear | Smooth curvature transition from 0.5 to 0.5 + EdgeWidth; convex only. | EdgeWidth, WearAmount |
| Dirt Accumulation | `max(cavity, 1 - AO)`. | DirtAmount |
| Rust Spread | Saturated sum of `saturate(2*(curvature-.5))*EdgeInfluence` and `cavity*CavityInfluence`. | RustAmount, EdgeInfluence, CavityInfluence |
| Moss Growth | Normal alignment with growth direction transitions at dot 0.2..0.8; cavity boosts it. Optional projected-height fade reduces growth between HeightMin/Max. | MossAmount, GrowthDirection, CavityInfluence, HeightMin/Max/Influence |
| Dust Accumulation | Up-facing slope fade (20..70 degrees), modulated by sheltered AO. | DustAmount, ShelterInfluence |
| Water Streaks | Perpendicular position divided by StreakWidth, parallel position by StreakLength; seeded anisotropic noise passes threshold/softness transition. | StreakAmount, GravityDirection, StreakLength/Width/Threshold/Softness |

EdgeWidth is dimensionless curvature transition width, with flat curvature 0.5;
it is not a pixel/physical edge width. Height/streak dimensions use context world
units. Growth defaults up; gravity defaults down. Directions normalize and fall
back to the default axis near zero. Dust uses world up. These are static masks,
not fluid, biological or corrosion simulations.

Values retain the core's finite ±1M bounds. Amounts/influences/masks saturate to
0..1. Scale clamps to .001..1000; sizes/softness guard degenerate ranges; seed
floors within 0..1M; power contrast uses .001..32 effective exponent range. Noise
position scales through the shared bounded vector instruction before octave
conversion, keeping integer lattices within range. Large coordinates inherit the
existing finite-output limits.

## Schema and API

`SmartGenerator.*` belongs to UI-independent `IXMaterialGraph`. The preset catalog
describes six defaults; Create constructs ordinary editable graphs. Validate checks
exactly one connected Scalar **Mask** output and existing typed interface rules.
Custom metadata supports arbitrary authored recipes. Kind labels/describes the
recipe; execution always uses its saved graph, including built-in recipe edits.

Wrapper v1: format `ixtreeme.smartgenerator`, version 1, string kind/name/description
and an existing versioned MaterialSubgraph under `subgraph`. Reads are bounded to
4 MiB, transactional, and reject unknown formats/kinds, invalid metadata/graphs.
AssetDatabase appends SmartGenerator without changing existing enum values. The
native `.meta` GUID owns identity/dependencies; graph versions and node kinds did
not change. Nested generator/subgraph calls retain existing depth/expansion bounds.

AddSmartGenerator validates a copied graph, inserts a regular Subgraph call, binds
its stable typed interface and optionally assigns a canonical layer mask. Failure
preserves the graph. LoadSmartGeneratorAsset and the shared native subgraph cache
bridge files/GUIDs into these APIs. Instances still share immutable programs;
changing dynamic overrides does not compile a new program.

## Verification and limits

SmartGeneratorTest checks all six reference values, amount/contrast/direction/
height controls, deterministic seeds, rotated gravity anisotropy, zero directions/
sizes, large allowed inputs, serialization/errors, real mesh bake/preview, layers,
scopes and shared instances. Actual DXC compiles all expanded recipes to DXIL and
SPIR-V when configured. This proves source compatibility, not GPU numeric readback.

Native editor/baking tests use real files/GUIDs, creation/save, layer Undo/Redo,
geometry-bound instances, moves/corruption/recovery and mesh material snapshots.
They found a stale deleted call preview after Undo; history now keeps a valid
selected output or returns to shading. The browser has separate graph, instance,
subgraph, generator and geometry-cache categories, including manifest persistence,
case-insensitive detection and immediate save refresh. File-picker clicks/pixel
layout, generated browser thumbnails, catalog discovery and runtime shader binding/
reflection/cache/GPU rendering remain next work. CPU timings are documented in
SMART_MATERIAL_PERFORMANCE.md.
