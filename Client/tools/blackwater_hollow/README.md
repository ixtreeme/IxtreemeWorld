# Blackwater Hollow scene authoring

An original 1000 × 1000 m forest exploration scene for the existing AkitaOnline project. The generators create native editor content, not a separate game engine. No network download is needed.

The terrain has 500 × 500 two-metre cells, 64 streaming chunks, an exact float height snapshot, four painted surface layers and a shaped lake. Four shared tree assets have four geometric LODs, trimmed masked foliage and 8-azimuth / 3-elevation impostor atlases at 128 pixels per view. The scene includes open interiors, trunk/prop collision, a first-person CharacterController, an attached torch, global height fog, five local fog volumes, eleven GPU particle emitters, spatial audio and a small Lua exploration HUD.

## Rebuild

Requires Blender, Python with NumPy/Pillow, an existing engine project, and the existing grass/cobblestone terrain textures referenced in `prepare.py`.

```powershell
python Client/tools/blackwater_hollow/prepare.py D:/AkitaOnline D:/IxtreemeWorld/Client/build/blackwater-hollow-20261010
& 'C:/Program Files/Blender Foundation/Blender 4.5/blender.exe' --background --python Client/tools/blackwater_hollow/blender_assets.py -- D:/AkitaOnline/Assets/BlackwaterHollow
cmake --build Client/build --config Release --target BlackwaterHollowBuilder --parallel 3
& Client/build/tools/blackwater_hollow/Release/BlackwaterHollowBuilder.exe D:/AkitaOnline D:/IxtreemeWorld/Client/build/blackwater-hollow-20261010
```

The builder creates tree assets only when they are absent, registers models/materials/audio/scripts in the existing asset library, writes the native terrain chunks and water mask through SceneManager, then reloads the saved scene and checks the exact terrain and entity count. Rerunning replaces the generated `BlackwaterHollow.scene`; retain a copy before making manual scene edits. It does not change the project's startup scene or `Main.scene`. The target is excluded from the normal build.

## Play

Open `D:/AkitaOnline/project.ixproj` in the editor, open `Scenes/BlackwaterHollow/BlackwaterHollow.scene`, then press Play and use the Game view. WASD walks, Shift runs, Space jumps, holding the right mouse button looks around, and E rings the chapel bell. Four landmarks can be discovered. Deep water and the outer map boundary return the player to the last safe ground position.

The source model library is `Assets/BlackwaterHollow/BlackwaterPropLibrary.blend`; each prop is also exported as a separate GLB. Model meshes, additional textures, synthesized WAVs, terrain layout, HUD and scripts are original to this scene. Tree bark/leaf textures and terrain grass/cobblestone maps reuse assets already in the project/engine; their pre-existing provenance remains applicable. No new external licensed model or sound is introduced.

The original sound beds are quiet by design. The lake has its own water material, with quarter-resolution reflections, refraction, slow waves and no foam/caustics; volumetric rays use low quality over 85 m; per-emitter particle caps are 100–160. The engine still requires right-drag mouse look, and currently does not provide a swimming controller. This is an exploration atmosphere scene, with no enemy AI or combat.
