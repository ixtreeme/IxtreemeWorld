# IxtreemeEngine – English user guide

Available offline in Help > Editor Guide / Súgó. Menu and field names match the editor. Search covers both translations, and Hungarian accents are optional when searching.

## Getting started

<!-- topic: quick-start -->
### Your first project in five minutes

#### Purpose
Create a scene you can save and try in Play. Asset Browser contains resources, Hierarchy contains scene instances, and Inspector edits the selected item.

#### Short tutorial
1. File > New Project...: enter a name and choose a project folder. Creating a project also creates its initial scene.
2. Create > Cube, then select it in Hierarchy.
3. In Inspector > Transform, set Position Y to 1. Press F to frame the selection in Scene View.
4. Ctrl+S saves. F5 starts Play; press F5 again to stop.

#### Result and common mistake
You have a saved project containing a cube. If creation is disabled, open a scene and stop Play first.

<!-- topic: projects-scenes -->
### Projects, scenes and saving

#### Purpose
A project groups assets, scenes and settings. A scene stores its objects, terrain, lights and environment.

- File > Open Project... / Recent Projects open projects.
- File > New Scene / Open Scene / Recent Scenes switch scenes.
- Ctrl+S saves the project and scene; Ctrl+Shift+S opens Save Scene As....
- Hierarchy scene nodes and scene assets also help navigate scenes.

#### Short tutorial
1. Open a project and press Ctrl+N.
2. Save Scene As... with a distinct name.
3. Create a Sphere and save with Ctrl+S.
4. Use Open Scene to switch away, then reopen your scene.

#### Result and common mistake
Your saved object returns. Resolve the save prompt before switching scenes. Changes made during Play are temporary, so stop Play before making persistent edits.

<!-- topic: layout -->
### Windows, docking and help

#### Purpose
View opens Hierarchy, Inspector, Scene Settings, Asset Browser, Scripts, Console, Build Output, Animator, Statistics, Scene View and Game View.

#### Short tutorial
1. Drag a tab header onto a docking target, or release it as a floating window.
2. Open View > Game View. Float it to see Scene and Game simultaneously.
3. View > Reset Layout restores the default arrangement.
4. Help > Editor Guide / Súgó: select Magyar or English and search for a topic such as painting.

#### Result and common mistake
Each pane scrolls independently. Headings, emphasis and numbered tutorials make help readable; Copy article copies the entire topic. An inactive viewport tab stops rendering new frames.

<!-- topic: navigation-transform -->
### Navigation, selection and transforms

#### Purpose
Scene View uses a free editor camera. Selected models have an outline; empty space is no longer selected using the old large bounding box.

- Right mouse + WASD: fly the camera. F: frame selection.
- W / E / R: Move / Rotate / Scale when you are not flying or typing.
- Transform: Position in metres, Rotation in degrees, Scale per axis.
- The toolbar magnet enables snapping; Delete removes the selection.

#### Short tutorial
1. Select a cube in Hierarchy and press F.
2. Press W and drag an axis; try E to rotate and R to scale.
3. Enable the magnet, select a 1.0 step and move again.
4. Enter exact values in Inspector and save.

#### Result and common mistake
The object changes position, rotation or size. If an imported model is too large, check its source units; a model authored in centimetres often needs Scale 0.01.

<!-- topic: hierarchy -->
### Hierarchy, parents, visibility and instances

#### Purpose
Hierarchy organizes scene objects. Search retains required parent branches; objects with identical display names remain separate instances.

#### Short tutorial
1. Use + or Create for Cube, Sphere, Capsule, Point Light, Spot Light, Water Body or Terrain.
2. Right-click for Rename, Duplicate and Delete.
3. Drag an object row onto another to reparent it; check its transform afterward.
4. The eye icon controls editor visibility. Enabled controls operation; these are different states.
5. Search, select a result and press F.

#### Result and common mistake
Instances are easier to organize. Before deleting, check the selection and children. An asset file and an instance of that asset in the scene are different items.

<!-- topic: play-cameras -->
### Play, Pause and the two cameras

#### Purpose
Scene View's editor camera and Game View's game camera are independent. Physics, scripts, audio, character movement and animation run in Play.

#### Short tutorial
1. Assemble and save your scene in Edit mode.
2. F5 or Play starts the simulation; the toolbar turns green.
3. F6 pauses/resumes; Shift+F5 stops.
4. Check that stopping restores the editing state.

#### Result and common mistake
You test a temporary game state. **Changes made during Play are discarded on Stop.** Game uses the project's Main Camera, or an active player Character Controller camera. Moving Scene View's free camera does not reposition the game camera.

During Play, when Game View is also visible, an inactive Scene View refreshes at up to 15 frames per second. Hovering or working in Scene View restores rendering every frame. Water uses each view's own camera image and depth.

## Assets and materials

<!-- topic: asset-browser -->
### Asset Browser and file management

#### Purpose
Import, organize and use project resources. Create provides folders, Lua/AngelScript scripts, Material, Water Material, Physics Material, Animator Controller and Particle Effect assets.

#### Short tutorial
1. Select a folder and use Create > New Folder to organize Models and Materials.
2. Use Import Asset... from the browser/import controls or context menu.
3. Drag a model into Scene View or an appropriate Hierarchy target.
4. Double-click editable assets; right-click for operations supported by their type.

#### Result and common mistake
The resource belongs to the project; its instance belongs to the scene. Expand a model's arrow to browse associated materials and textures. Prefer editor Rename/Delete operations so related metadata is handled too.

<!-- topic: model-import -->
### Importing FBX and other models

#### Purpose
Import geometry, supported material properties, referenced textures and animation content. FBX import can create native material assets and texture dependencies.

#### Short tutorial
1. Keep the FBX's referenced images accessible and import the model.
2. Expand the model's arrow in Asset Browser.
3. Drag the model into the scene; inspect Inspector > Material Slots.
4. Double-click a material to open Material Editor and check Diffuse / Albedo and Normal Map.
5. Correct Scale if necessary and save.

#### Result and common mistake
The model appears with its materials. Specialized source shaders may require manual adjustment in native materials. For missing textures, check the source file's references and import log.

<!-- topic: textures-exr -->
### Textures, normal maps and EXR

#### Purpose
Diffuse / Albedo supplies colour; Normal Map supplies surface direction detail; AO modulates occlusion. Roughness and Metallic represent different surface properties.

#### Short tutorial
1. Import the surface's images; EXR images are supported too.
2. Open a Material and drop each image onto its corresponding texture preview field.
3. Adjust Normal Strength, then Roughness to inspect the surface response.
4. Save and inspect under different lighting angles.

#### Result and common mistake
Material texture previews are visible. EXR supports floating-point HDR data; a small display preview does not measure the stored dynamic range. Do not confuse colour and roughness/normal data. Check the source normal-map convention if details look inverted.

<!-- topic: materials -->
### Material Editor, PBR and overrides

#### Purpose
The shared Material Editor has Material and Water tabs. A PBR material is a shared asset; an object's material override belongs to that instance.

- Textures: Diffuse / Albedo, Normal Map, AO, Roughness, Metallic, Height.
- Parameters: tiling, tint, normal/AO/roughness/metallic strengths and emissive settings.
- Shading Mode: Lit or Unlit. Alpha Mode: Opaque, Mask or Blend.
- Mask suits cutout leaves; Blend suits translucent surfaces.

#### Short tutorial
1. Asset Browser > Create > New Material, then double-click it.
2. Assign albedo and adjust tiling.
3. Save and drop the material onto a model's Material Slots field.
4. To change one instance, select its Material Slot and enable Override Active.

#### Result and common mistake
Editing a shared material changes all its users; an override changes only its instance. Double-clicking an Inspector material slot opens the material directly.

Material Slots shows compact cards in a grid that adapts to the Inspector width, with the material name and its albedo preview (or base-colour swatch if it has no albedo texture). Hover to see the full name and submesh index. GUIDs are omitted here. Drop onto the preview or name to assign a material; the X clears that slot.

<!-- topic: prefabs -->
### Creating and updating prefabs

#### Purpose
A prefab is a reusable object assembly. Several instances can share the same asset with individual overrides.

#### Short tutorial
1. Assemble a model with components or children in Edit mode.
2. Hierarchy context menu > Create Prefab.
3. Drag the prefab asset into the scene several times.
4. In the instance Inspector, Open edits the prefab source and Select reveals it in Asset Browser.
5. Overrides lists local changes with Apply/Revert per change and Apply All/Revert All. More... contains instance refresh, refresh of all linked instances and Unpack.

#### Result and common mistake
Multiple instances come from one template. Before Apply, confirm that your change should affect other users. Double-click the prefab asset to open its asset-editing interface.

- The prefab asset Inspector edits its name, entity hierarchy/transforms and material slots, and can Add Point Light / Add Spot Light.
- Save Prefab Asset writes asset edits; Reload Prefab Asset discards unsaved asset edits. Instantiate Prefab creates a scene instance. Parent cycles and missing mesh references block saving.
- LOD Group settings are preserved by prefab creation and instantiation, including disabled groups. The Overrides menu can apply or revert LOD Group changes separately.

<!-- topic: model-export -->
### Exporting a model to FBX

#### Purpose
Export supported model assets for use in another modelling application. Model export is separate from packaging the full game.

#### Short tutorial
1. Select a model asset in Asset Browser.
2. Right-click > Export to FBX....
3. Review available export options and choose the destination file.
4. Open the export in the target application and check scale, materials and orientation.

#### Result and common mistake
You receive a separate FBX file. Scene instance transforms, scripts and editor features do not automatically become a playable game in another application. File > Build Game... creates the executable package.

## Terrain and water

<!-- topic: terrain-create -->
### Creating terrain and choosing resolution

#### Purpose
Terrain combines an editable height grid and material layers. Cell size controls geometry density; texture tiling is independent.

#### Short tutorial
1. Open a scene and choose Create > Terrain.... With existing terrain this becomes Replace Terrain....
2. Choose dimensions and cell resolution in the creation dialog.
3. Select terrain in Hierarchy; Inspector shows Sculpt, Paint and Settings.
4. Build a small hill and inspect it up close before creating a large area.

#### Result and common mistake
You have terrain you can save. Smaller cells create more geometry and work. Replace Terrain replaces the existing terrain; save a copy first if you need to retain the old area.

- Settings > Save Terrain writes height and paint data; Ctrl+S saves the project/scene too.
- Reload From Disk discards unsaved terrain changes. Switching Sculpt/Paint/Settings puts the current brush away.

<!-- topic: terrain-sculpt -->
### Shaping terrain with Sculpt

#### Purpose
Raise lifts, Lower lowers, Smooth softens and Flatten approaches Target height.

- Brush radius sets metres; Strength sets the amount of change.
- Falloff 0 gives a harder edge, 1 a softer edge.
- Hold left mouse in Scene View to sculpt; click the active tool again to stop.

#### Short tutorial
1. Terrain > Sculpt > Raise; choose a medium radius and low Strength.
2. Build a hill using short strokes.
3. Use Smooth on the slope.
4. Use Flatten and Target height to create a building pad, then save.

#### Result and common mistake
Height and terrain geometry change. A large brush can affect distant areas too; texture painting does not change height.

<!-- topic: terrain-paint -->
### Painting terrain with three brush sliders

#### Purpose
Paint supports eight material layers with previews. One painting tool replaces the old Mix/Replace selector.

- **Diameter:** the complete brush diameter in metres.
- **Edge smoothing:** softness of the transition; minimal grid-related antialiasing remains even at low values.
- **Overall coverage:** 100% replaces the previous layer in the interior; lower values blend.

#### Short tutorial
1. Drop two Materials or textures onto two layer slots and select the second.
2. Diameter 8 m, Edge smoothing 10%, Overall coverage 100%: paint a fully covering strip.
3. Try Edge smoothing 80% for a softer transition.
4. Try Overall coverage 40% for blending. Undo Last Stroke reverses the latest stroke; Stop painting ends the tool.

#### Result and common mistake
Interior coverage and edge softness are independent. Holding the brush does not accumulate unlimited coverage; existing stronger coverage can remain. Use the sliders rather than brush speed to control blending.

<!-- topic: terrain-mapping -->
### Terrain layer parameters and triplanar mapping

#### Purpose
Layer tiling, tint, normal/roughness/metallic/AO strengths, UV offset and rotation control appearance. Triplanar projection reduces texture stretching on steep slopes.

#### Short tutorial
1. Select a Paint layer and adjust Tiling to change pattern scale.
2. Sculpt a steep slope, then enable Settings > Triplanar mapping.
3. Sharpness controls projection blending; Slope threshold and Slope transition control the slope-based transition.
4. Inspect near and far, then save.

#### Result and common mistake
Textures stretch less on slopes. More tiling gives a denser pattern, not a denser terrain grid. Layer settings and the shared material asset have different scopes.

<!-- topic: water-body -->
### Water Body and painting its shape

#### Purpose
Water Body is an independent scene object with level, area and material. Its Shape brush adds or removes water coverage.

#### Short tutorial
1. Create > Water Body and select it.
2. Adjust its centre, dimensions and water level in Inspector.
3. Under Shape, select Add or Remove and Brush radius.
4. Paint with left mouse in Scene View; click the tool again to stop.
5. Assign a Water Material and save.

#### Result and common mistake
You create a lake or irregular water area. Water shape and terrain height are separate; sculpt the basin separately. If water is invisible, check its level, Enabled state and camera position.

<!-- topic: water-material -->
### Water Material: waves, foam and refraction

#### Purpose
Material Editor > Water edits water in the same window. Colors sets depth colours/fade, Wave sets small and large waves, and Foam controls foam.

- Caustic controls underwater light patterns.
- Refraction / Reflection controls distortion, depth response and reflections.
- Edge Fade controls shore fading distance and curve.
- Textures, Normal Tiling and Fresnel control surface detail and angle-dependent reflection.

#### Short tutorial
1. Create > New Water Material in Asset Browser and open it.
2. Set shallow/deep colours and moderate Wave Scale/Speed values.
3. Enable Foam and inspect the shore.
4. Save and assign it to a Water Body.

#### Result and common mistake
Multiple water bodies can share one material. Excessive wave, normal or refraction values distort the image; reflection adds a separate GPU cost.

## Objects and physics

<!-- topic: components -->
### Inspector and components

#### Purpose
Inspector shows the selected object's built-in Transform, renderer, materials and animation sections. Add Component offers supported Collider/Trigger shapes, Rigidbody, Fixed/Hinge Joint, Character Controller, Script, Audio Source/Listener, Particle System, LOD Group and Note.

#### Short tutorial
1. Select a Cube and open Add Component at the bottom of Inspector.
2. Search for Collider and add it.
3. Inspect the component header menu for available removal actions.
4. Add an editor Note and save.

#### Result and common mistake
Only supported, missing components can be added. Note stores editor information. Inspector's debug icon reveals identifiers and runtime details; these are optional for normal content creation.

<!-- topic: lights -->
### Point Light, Spot Light and sunlight

#### Purpose
Point Light shines in all directions; Spot Light shines in a rotatable cone. Sun is the global directional light in Scene Settings > Environment.

#### Short tutorial
1. Create > Point Light, raise it above the ground, and adjust Color, Intensity and Radius.
2. Create > Spot Light and rotate it toward an object.
3. Adjust the spot radius and inner/outer cone angles.
4. Adjust Sun direction/elevation and Casts shadows in Environment.

#### Result and common mistake
Point Light has no preferred down/up direction, so rotation is unnecessary. For an unexpectedly dark side, check normals, occlusion and material. Many local lights and shadows can increase render cost.

<!-- topic: camera -->
### Main Camera and viewing range

#### Purpose
A Camera object's position, rotation and projection control Game View when no active player-controller camera takes over.

#### Short tutorial
1. Select Main Camera in Hierarchy. A new scene creates its initial camera automatically.
2. Set its position and rotation in Transform.
3. Camera > Field of View sets the angle; Near Plane / Far Plane set clipping distances.
4. Set as Main Camera selects the game's camera; inspect Game View.

#### Result and common mistake
Scene View and Game differ because they use different cameras. A large Near Plane clips nearby models; very small Near and excessive Far can reduce depth precision.

<!-- topic: colliders -->
### Collider, triggers and Physics Material

#### Purpose
Collider defines physical contact geometry. Is Trigger reports contact without a solid obstacle. Physics Material shares friction and restitution settings.

- Shape supports Box, Sphere, Capsule, Mesh and Convex Hull. Mesh follows model triangles; use it mainly for static/kinematic bodies. Convex Hull suits dynamic objects.
- Fit controls Box/Sphere/Capsule size to the mesh; Center resets the offset. Edit Collider In Scene moves the collider centre separately from the object's transform.
- Physics Presets Default/Bouncy/Ice/Heavy/Trigger adjust contact and associated Rigidbody settings. Check them after applying a preset.

#### Short tutorial
1. Add Collider to a cube and choose a suitable Shape, size and centre.
2. Use Edit Collider In Scene to inspect the shape.
3. Set Physics Layer; Edit > Project Settings controls which layer pairs collide.
4. Create > New Physics Material, adjust Friction, Bounciness and Combine modes, then assign it.
5. Test in Play; enable Is Trigger for a sensor area.

#### Result and common mistake
The visible mesh and collider can differ. Incorrect layer pairs prevent contact; NoCollision deliberately disables collisions.

<!-- topic: rigidbody -->
### Rigidbody and scene physics

#### Purpose
Static is fixed, Dynamic is simulated and Kinematic is controlled movement. Mass, gravity, damping, sleeping, Continuous Collision and axis locks define behaviour.

#### Short tutorial
1. Create collider-equipped ground and a collider-equipped cube above it.
2. Add Rigidbody to the upper cube, choose Dynamic and Use Gravity.
3. Scene Settings > Physics: start with Gravity 0, -9.81, 0. Fixed timestep defines simulation steps; Max substeps limits catch-up work.
4. Test falling in Play; Live physics (Play mode) provides impulse test actions.

#### Result and common mistake
The cube lands on the ground. Mass applies to dynamic bodies. Continuous Collision helps fast small bodies avoid tunnelling; excessive substeps or detailed mesh colliders cost more.

<!-- topic: joints -->
### Fixed Joint and Hinge Joint

#### Purpose
Fixed Joint locks two Rigidbody objects together; Hinge Joint permits rotation around an axis. Add the joint to only one body for each connection.

#### Short tutorial
1. Create two meshes with colliders and Rigidbody components.
2. Add Hinge Joint to one and enter the other's Connected Entity ID.
3. Set Anchor and Axis; enable Limits and set Min Angle / Max Angle.
4. Adjust Friction Torque and test in Play.
5. Use Fixed Joint instead for a rigid connection.

#### Result and common mistake
You create a hinge or rigid link. Zero, missing or Rigidbody-free targets are unsuitable. Inspector's debug view can display entity identifiers.

<!-- topic: character-controller -->
### Player and Character Controller

#### Purpose
Character Controller marks the mesh as a player with capsule movement and a camera mode.

#### Short tutorial
1. Add Character Controller to a character model and enable it.
2. Set Capsule Radius/Height, Walk Speed, Run Speed and Jump Height.
3. Set Slope Limit and Step Height for traversable slopes and steps.
4. Choose First Person, Third Person or Top Down Camera Mode and adjust associated height/distance fields.
5. In Play use WASD, Shift to run, Space to jump; right-drag in Game View to look around.

#### Result and common mistake
You have a controllable player. Match capsule size to the model's size in metres. If movement fails, check ground colliders, step height and physics layer interactions.

## Animation, effects and audio

<!-- topic: animation-clips -->
### Animated models and clips

#### Purpose
A skinned model's skeleton and compatible animation clip produce motion. Animation (debug) tests clips directly; Animator Controller provides state-driven playback.

#### Short tutorial
1. Import a rigged model and compatible animation.
2. Drag the model into the scene and select it.
3. Choose Inspector > Animation (debug) > Clip and inspect playback.
4. For game logic, assign a Controller asset in Animator.

#### Result and common mistake
The current animated shape renders with a selection outline. Incompatible skeletons or a model imported only as static geometry will not animate correctly. Debug clips and controllers are different testing paths.

<!-- topic: animator -->
### Animator states and transitions

#### Purpose
Animator Controller graphs contain states, clips, parameters and transitions. Entry marks startup; Any State allows shared transitions. Float, Bool and Trigger parameters are supported.

#### Short tutorial
1. Create > New Animator Controller and open it in Animator.
2. Canvas context menu > Add State Here; assign a clip. Set as Default selects the starting state.
3. Right-drag from one state onto another to create a transition. Left-click the edge midpoint to edit its conditions.
4. Has Exit Time, exit time and Duration control timing; add a Speed Float in Parameters.
5. Assign the controller to the model and watch the active state in Play.

#### Result and common mistake
Parameters control clip transitions. Parameter names must match the script that sets them. Rename/Delete and Delete Transition are in the corresponding node/edge context menus.

<!-- topic: blend-trees -->
### Animator blend trees

#### Purpose
Blend trees mix motions from parameters, such as idle/walk/run using Speed. 1D uses thresholds; 2D uses child positions.

#### Short tutorial
1. Select a state and its blend-tree settings.
2. Choose 1D and a Float parameter.
3. Add Motion for Idle, Walk and Run with increasing thresholds.
4. Change the parameter in Play to inspect blending.
5. For two-axis movement, choose a supported 2D mode, two parameters and child positions.

#### Result and common mistake
Clips mix progressively. Use animations compatible with the same skeleton and distinct thresholds. + Add Motion and X edit the child list.

<!-- topic: particles -->
### Particle System and Particle Effect assets

#### Purpose
Emitters produce smoke, sparks and similar effects. Emission, Lifetime, Speed, Size, Blending, Texture, Shape, Color, Collision and Soft Particles control different aspects; GPU Simulation is optional.

- Duration, Rate (/s), Burst and Max Particles control emission and its cap; Min/Max fields randomize lifetime, speed and size.
- Drag Size Over Life's four curve points; Color 0–3 defines lifetime colour/alpha changes. Gravity, Drag and Spin alter movement.
- Turbulence adds smooth swirling motion on both CPU and GPU. Enable Turbulence turns it on; Strength is acceleration, Scale is the spatial size of the swirls in metres, and Speed controls how quickly the flow changes (0 = static field). Settings are saved in scenes, prefabs and effect presets.
- Atlas Columns/Rows enables a flipbook over lifetime. Shape supports Point/Sphere/Box/Circle/Edge; Direction/Cone Angle guide emission.
- Local Space makes particles follow their emitter, but disables ground collision. Soft Distance controls fading near scene geometry.

#### Short tutorial
1. Add Particle System to a mesh instance.
   For an invisible emitter, choose Create > Empty Object (also available from Hierarchy's + and context menus), then Inspector > Add Component > Particle System. Empty objects start with no model, materials or collider and Scale 0/0/0; moving and rotating them preserves that scale. Particle size and spawn volume remain controlled by the Particle System settings.
2. Choose Texture, moderate emission and a short lifetime; adjust Shape and Color.
3. Compare GPU Simulation on/off. Collide With Ground enables ground contact; Soft Particles softens surface intersections.
4. Save As New Effect stores your configured emitter as a reusable asset; choose it under Effect Preset. Re-apply Preset reloads its values.
5. Set Play On Start and Loop.

#### Result and common mistake
Emitters can preview in Edit mode. Large overlapping translucent particles are expensive on the GPU; longer lifetime also increases the number of live particles.

For floating dust, place a Box emitter above the ground (Extents are half-sizes), use Cone Angle 180°, Gravity 0, Speed Min/Max 0.02/0.08, Drag 0.3 and Lifetime 15/25 seconds. Enable Turbulence with Strength 0.03, Scale 3 and Speed 0.35. Use small Alpha sprites with Color 0–3 alpha values 0/0.15/0.15/0 and constant Size Over Life. New particles spawn throughout the volume; existing ones can drift beyond it. Burst 0 and Rate 15 fill it gradually.

<!-- topic: audio -->
### Audio Source, Listener and volume buses

#### Purpose
Audio Source plays a clip; Audio Listener sets the listening position. Master, Music and SFX buses provide shared volume controls.

#### Short tutorial
1. Import audio and use Asset Browser > Play Preview.
2. Add Audio Source and choose Clip.
3. Set Enabled, Play On Start, Loop, Volume, Pitch and Bus.
4. For spatial audio enable 3D, set Min Distance / Max Distance and ensure an appropriate Audio Listener.
5. Adjust buses in Edit > Project Settings > Audio and test in Play.

#### Result and common mistake
Audio can follow position and distance. If silent, check the clip, listener, source/bus volumes and startup option.

Without an Audio Listener component, the camera supplies the listening position.

## Scripts and game UI

<!-- topic: scripts -->
### Lua and AngelScript: your first script

#### Purpose
Project scripts execute in Play. Editing Lua/AngelScript does not require rebuilding the engine; Scripts opens source files.

#### Short tutorial
1. Asset Browser > Create > New Lua Script or New AngelScript.
2. Open the generated template and edit its OnStart, OnUpdate and OnDestroy lifecycle hooks.
3. Drag the script onto a mesh's Hierarchy row, or select Backend and script in Add Component > Script. Check Class for AngelScript.
4. Edit exposed script fields in Inspector.
5. Save a change during Play; hot reload recreates affected Lua/AngelScript instances. Console reports errors.

#### Result and common mistake
The generated template matches the backend. Reload can reset instance state; invalid source does not produce a valid new running instance. Saving a script file and saving the scene are separate actions.

<!-- topic: script-api -->
### Script API: entities, input, animation and effects

#### Purpose
Use the API to find, transform, spawn and control objects. Backend names and return shapes may differ; follow the generated template and backend interface.

- Entity: Find / EntityExists; Get/Set Position, Rotation and Scale. Rotation uses degrees.
- Input/time: IsKeyDown, MouseDelta, DeltaTime, ElapsedTime.
- World: SpawnMesh, SpawnPrefab, DestroyEntity, SetMaterial; spawning/destruction/material changes are deferred.
- Animator: named Float, Bool and Trigger parameters.
- Character: state queries and run/jump permissions.
- Audio/log: PlayOneShot, Log, LogError.
- ParticlePlay/Stop/Restart/Emit: emitter control; particles already alive finish after Stop.

#### Short tutorial
1. Create a script from a template and assign it to a mesh.
2. In OnStart, find an object by name and verify its identifier exists.
3. In OnUpdate, move proportionally to delta time or set a named Animator parameter.
4. Inspect Console and the target in Play; Stop restores the editing state.

#### Result and common mistake
Movement speed need not depend on FPS. A command targeting a missing object or component does not automatically create that component.

<!-- topic: script-queries -->
### Raycast, OverlapSphere and Enabled state

#### Purpose
Raycast queries a ray, OverlapSphere a spherical area. RaycastFiltered supports layer masks, triggers and an ignored entity; the original Raycast remains available.

#### Short tutorial
1. Create two collider objects on different Physics Layers.
2. Cast a filtered ray from the camera/player with finite direction and positive distance.
3. Select layerMask bits and exclude the player using ignoreEntityId.
4. Use OverlapSphere for proximity, checking results and truncated indication.
5. Temporarily disable and re-enable an object using SetEntityEnabled.

#### Result and common mistake
Entity enabled state affects render, physics, scripts, audio and particles together. Overlap returns at most 1024 results; terrain/non-entity hits can have identifier 0. A layer mask differs from a layer index.

<!-- topic: game-ui -->
### Game UI: RmlUi and text prompts

#### Purpose
Game HUDs and menus use RmlUi .rml and .rcss documents. They appear in Game View, separately from editor ImGui panels.

#### Short tutorial
1. Create ui/hud.rml in the project's asset folder and give elements id attributes.
2. Open it using UiOpen with an asset-relative path; check the nonzero handle.
3. Use UiSetText, UiSetProperty and UiSetClass to update text, RCSS properties and classes.
4. UiConsumeClick handles a button; UiSetVisible/UiClose hides or closes a document.
5. PromptText and associated queries request a line of text; secret masks its field.

#### Result and common mistake
The HUD works in Play and runtime. Stop closes open documents and session prompt state. Incorrect element IDs or asset paths will not update another element.

<!-- topic: network-native -->
### Networking and legacy native C++ scripts

#### Purpose
The TCP API provides a byte stream: NetConnect, NetState, NetSend, NetReceive, NetClose. Scripts implement the game's protocol and message framing. Native C++ scripts use a separate legacy module path.

#### Short tutorial
1. After NetConnect, poll connection state without blocking the game loop.
2. Send framed application messages; assemble received data from partial chunks.
3. Handle closed/failed states and clean up with NetClose.
4. Build existing native module sources through Scripts/Build; inspect Build Output for compiler errors.

#### Result and common mistake
One Receive does not equal one TCP message. Streams close on Stop. Native C++ is not a drag-and-drop Lua script or Lua hot reload; it needs the matching SDK/ABI and module build.

## World building and large scenes

<!-- topic: tree-generator -->
### Tree Generator: saving a tree asset

#### Purpose
Tools > Tree Generator creates procedural trees. General/Seed, Trunk and Branches, Bark, Leaves and Output control geometry and appearance.

#### Short tutorial
1. Open Tree Generator and start with a preset.
2. Set Seed; New Seed generates a variation of the same type.
3. Adjust Branch levels and branches, then leaf density/cards in Leaves.
4. Choose bark/leaf textures and inspect Output geometry counts.
5. Enable Bake distant-tree impostor, choose Impostor distance and Save as Asset..., then place it in the scene.

#### Result and common mistake
You receive a reusable tree and optional distant atlas. Use clustered leaf cards and moderate geometry for games instead of many individual leaves. Measure one tree before duplicating it into a forest.

<!-- topic: lod-occlusion -->
### LOD, leaf LOD and occlusion culling

#### Purpose
LOD simplifies distant geometry; main-view leaf LOD thins foliage cards. GPU occlusion can skip static groups hidden behind terrain or large opaque objects.

#### Short tutorial
1. Add LOD Group to a static mesh. Enabled controls whether the group is used; Override on this instance lets a linked model default be edited locally.
2. The coloured bar shows distances in metres. Drag a divider or open a LOD row to edit Transition (m) and Target geometry. Headers show actual generated triangles and their percentage of LOD 0; Not built means no generated result is available yet.
3. Add LOD / Last LOD changes the number of levels (1–4). Enabled groups generate their geometry in the background even when nearby or selected. Building indicates CPU generation; GPU upload has its own status. Generate / Rebuild LODs also requests generation; Set as model asset default shares the configuration across users of that model.
   Scene View and Game View show their own camera distance and selected level. Distances are measured from the centre of the object's world bounds; standing inside a large bounding box does not pin the distance to zero. Selection and the occluder pass do not force LOD 0. Move away to test transitions. Changing a transition or hysteresis reuses geometry; changing target ratios or the number of levels rebuilds it.
   Lower triangle targets permit stronger simplification. The generator first preserves seams, then relaxes geometric error and uses submesh-local clustering if seams prevent reaching the budget. Very low targets can change the silhouette or texture seams; the actual count may differ from the target, and each submesh keeps geometry. A LOD reduces detail; the last level remains visible rather than disappearing.
4. Advanced contains hysteresis, which prevents switching repeatedly around a distance boundary. The final LOD remains visible at larger distances; this panel does not add a separate distance-culling stage.
5. Place trees at different distances, build a ridge or large solid occluder, then compare FPS/GPU time using the same camera and resolution.

#### Result and common mistake
Nearby geometry retains detail. Automatic geometry LOD currently supports static meshes; alpha-blended material draws keep their original geometry. Default occlusion only hides fully covered groups; characters, transparent geometry, terrain chunks and shadows keep their existing paths. Open forests may gain little. IX_OCCLUSION_CULLING=0 disables this optimization before launch; it is not an editor slider.

<!-- topic: impostors -->
### Impostors for models, terrain and characters

#### Purpose
There are two systems: a pre-baked multi-view model atlas and an automatic GPU view cache. The latter can handle distant terrain chunks, static objects and current animated poses.

#### Short tutorial
1. Static model: Asset Browser context menu > Bake model impostor. Tree Generator's Output checkbox can bake trees too.
2. Move farther away to check the transition. Disable model impostor disables the model atlas.
3. Terrain and characters need no manually created atlas file; the GPU view path automatically selects eligible distant content.
4. Inspect Statistics/Performance > Runtime impostors and Impostor image cache.

#### Result and common mistake
Nearby, selected, oversized or unsuitable objects use original geometry. Model baking does not support every material, including Blend/unlit/emissive surfaces. View caching handles small stationary turns but falls back during walking. IX_TREE_IMPOSTORS=0 and IX_SCENE_IMPOSTORS=0 are launch-time comparison switches.

<!-- topic: layered-world -->
### Layered World and multi-level walkable space

#### Purpose
Tools > Layered World generates layer data from collision surfaces for bridges, floors and stacked walkable areas. These are different from terrain's eight painted texture layers.

#### Short tutorial
1. Create collider-equipped terrain and a walkable bridge above it.
2. Model Inspector > Layer generation: enable Include collision surfaces and appropriate surface flags.
3. Tools > Layered World > Generate layers from collision; enable Show layer volumes to inspect.
4. Under Support probe (offline), choose a volume and X/Z point, then Place support probe.
5. Export layer metadata saves the generated data.

#### Result and common mistake
Distinct walkable volumes are generated. Missing colliders or authoring flags may exclude surfaces. Regenerate and inspect after changing geometry.

<!-- topic: server-export -->
### Strict server world export and spawn

#### Purpose
Server world export validates terrain, collision, layers and spawn data. It is separate from the File > Build Game runtime package.

#### Short tutorial
1. Generate layers from a saved scene and check a support probe.
2. Tools > Layered World > Server world export: enter World ID.
3. Set Player spawn X/Z and Spawn volume; 0 means terrain, another value selects a generated volume.
4. Export strict server world runs validation; read status and Console.

#### Result and common mistake
Spawn must fit at a valid, unblocked location. Check capsule clearance, collision and volume ID rather than merely changing coordinates to evade an error. Export does not automatically start a server.

## Environment and image quality

<!-- topic: environment -->
### Environment, ambient and time of day

#### Purpose
Scene Settings > Environment shares Sun and Ambient settings. Sun direction affects lighting, sky and sun-related effects.

#### Short tutorial
1. Set Sun Height in sky and Direction.
2. Adjust Intensity/colour and enable Casts shadows if desired.
3. Ambient colour/intensity controls baseline illumination of dark surfaces.
4. Try morning/evening with Time of day preset and save.

#### Result and common mistake
The scene gets a different lighting mood. Time of day is a preset selector, not an automatic day/night clock. For black objects, check ambient as well as material and normals.

<!-- topic: sky -->
### Sky: colour, procedural, cube map and panorama

#### Purpose
Scene Settings > Sky offers Color, Procedural, Cube map (6 images) and Panorama (360 image). Procedural uses Zenith, Horizon and Ground colours with Sun size/glow. Image skies support HDR and EXR too.

#### Short tutorial
1. Open Sky and choose Panorama (360 image); import a 2:1 equirectangular image and drop it onto Panorama.
2. For Cube map, assign Right/Left/Up/Down/Front/Back faces. Matching right/left or px/nx-style names can fill empty sibling slots automatically.
3. Adjust Exposure, Rotation and Tint. Ambient from sky uses the sky's average colour for ambient light.
4. Try Procedural and adjust Zenith/Horizon/Ground, Sun size and Sun glow; save.

#### Result and common mistake
Sky appears behind scene geometry. Environment > Sun controls sun position and colour; there is no separate sky-sun gizmo. Sky types expose different fields; missing images or incorrect paths are reported in status.

<!-- topic: fog -->
### Global and local fog

#### Purpose
Global fog fades distant parts of the scene. Fog Volume adds fog only inside a box or sphere. They work independently or together in Scene View, Game View and the packaged game.

#### Short tutorial
1. View > Scene Settings > Fog: enable Global fog. Start with Density 0.01, Start distance 0, Max distance 1000 and Max opacity 0.98, then choose a Color.
2. For ground fog, enable Height fog. Density is constant below Base height and decreases above it according to Height falloff. Affect sky controls whether global fog changes the sky.
3. Create > Fog Volume creates an invisible object with zero scale. Select it in Hierarchy and choose Box or Sphere in Inspector > Fog Volume. Add Component > Fog Volume also attaches fog to an existing object.
4. Transform Position/Rotation sets its location and orientation; Half extents or Radius sets its size in metres. Half extents are measured from the centre: 10/3/10 means a full size of 20/6/20. Object Scale does not resize the fog volume.
5. Edge fade softens the boundary inward; 0 makes it sharp. Save with Ctrl+S; local fog can also be saved in a prefab. The selected volume has an editor outline.

#### Result and common mistake
Density accumulates along distance: at 0.01, about 37% is transmitted over 100 metres. Volumes behind the camera or behind a wall do not obscure the surface in front of them; fog also works from inside the volume. Up to 32 active local volumes are rendered. Fog uses scene depth, so transparent materials and particles that do not write depth receive the fog of the surface behind them. Use God rays for shadow-derived light shafts.

<!-- topic: post-processing -->
### God rays and Tone mapping

#### Purpose
God rays creates sun-related light shafts; Tone mapping maps the HDR scene into displayed colours/brightness. Exposure does not change physical light intensity.

Every quality filters leaf/sky coverage and volumetric shadows to soften foliage transitions. High renders each technique at half resolution: 128 screen-space samples or 64 volumetric steps. Low and Medium use quarter resolution. Screen-space falloff stays consistent when quality changes; the two quality controls are independent.

#### Short tutorial
1. Position a visible sun in Scene Settings > Environment.
2. Enable God rays and gradually tune its strength, density and volumetric fields.
3. Shadow-derived shafts require Sun > Casts shadows; read the panel hint.
4. Adjust exposure/mapping in Tone mapping and compare from the same camera.

#### Result and common mistake
The image gains atmospheric effects. Looking toward the sun can make shafts/volumetric effects expensive. For excessive brightness, reduce exposure or lighting before arbitrarily darkening textures.

## Settings, measurement and publishing

<!-- topic: project-settings -->
### Project Settings and autosave

#### Purpose
Edit > Project Settings controls the project physics layer matrix, audio buses and render resolution. Manual saving remains useful alongside editor autosave.

Autosave runs every five minutes while a project is open in Edit mode; during Play it waits until editing resumes. The status bar shows its countdown.

#### Short tutorial
1. Inspect Physics layers and only change intended collision pairs.
2. Set Master/Music/SFX under Audio.
3. Rendering > Render resolution: View size follows each panel; fixed modes render a fixed pixel count. Custom requires Apply Resolution.
4. Ctrl+S saves the project and current scene.

#### Result and common mistake
Panel size and fixed render resolution can create different loads. Autosave does not replace deliberate saves before major edits/builds. Gravity and Fixed timestep belong to Scene Settings > Physics.

<!-- topic: performance -->
### Measuring FPS, CPU/GPU and memory

#### Purpose
Scene View reports its render FPS; Game View has separate FPS, CPU/GPU utilization, RAM and renderer information. View > Statistics provides detailed frame and scene data.

- Statistics reports renderer, jobs, scene geometry, impostors and image-cache counters.
- Math library > Run Math Benchmark is a synthetic calculation test; its throughput is different from scene FPS.

#### Short tutorial
1. Fix camera and render resolution; keep only necessary views active.
2. Record FPS/frame time: 60 FPS allows about 16.7 ms, 120 FPS about 8.3 ms per frame.
3. Change one factor: foliage density, particles, shadows or resolution.
4. Wait for loading/previews/baking, then compare several seconds of results.

#### Result and common mistake
You can identify expensive systems. Utilization is different from frame-section timing; distinguish RAM percentage and MiB. Submitted triangle counters may differ from actual GPU-rendered triangles with occlusion culling.

<!-- topic: physics-debugger -->
### Physics Debugger: shapes, queries and events

#### Purpose
Tools > Physics Debugger inspects physical bodies and queries separately from visible mesh geometry.

#### Short tutorial
1. Open it during Play and enable desired Scene View debug drawing.
2. Raycast from the camera: choose filter/distance and press Raycast.
3. Overlap in front of the camera: try Sphere, Box or Capsule.
4. Observe Collision and trigger events when bodies meet.

#### Result and common mistake
You see what physics detects. For missing hits, check layer/trigger filters, active colliders and test position. Disable debug drawing/event inspection for final performance measurements.

<!-- topic: console-diagnostics -->
### Console, Build Output and diagnostics

#### Purpose
Console collects operational statuses and script messages; Build Output collects compiler/packager results. They describe different failure sources.

#### Short tutorial
1. Open View > Console and filter Error/Warning messages.
2. Clear before reproducing and repeat one operation.
3. For build failures, inspect Build Output and find the first relevant error.
4. Tools > Engine Diagnostics provides GPU frame capture (F11), frame profile and material dump (F12).
5. Diagnostic switches temporarily disable shadows, reflection, asset discovery/watcher or hierarchy work.

#### Result and common mistake
You obtain precise evidence. GPU capture depends on tool support. Disabled passes are not a permanent scene fix; restore switches after testing. ImGui Demo Window is a developer example interface.

<!-- topic: build-game -->
### Build Game: running without the editor

#### Purpose
File > Build Game... creates an executable runtime, required engine files and a project copy.

#### Short tutorial
1. Stop Play and save the project/scene.
2. Set Game name, Output folder and Startup scene.
3. Existing legacy C++ sources can request a module build; Lua/AngelScript need no such compilation step.
4. Enable Run the game when it is built if desired and press Build.
5. Inspect Build Output and use Run Game / Open Folder to test the package.

#### Result and common mistake
The destination contains Game name.exe and Game/ project copy. The first runtime compile can take several minutes. Startup scene must belong to the project. Shared renderer/physics/script optimizations remain in the built game; the Help editor window does not run there.

<!-- topic: shortcuts -->
### Keyboard shortcuts and focus

#### Purpose
Help > Keyboard Shortcuts lists keys; their input context matters too.

- Ctrl+N: new scene; Ctrl+S: save; Ctrl+Shift+S: Save As.
- F5: Play/Stop; Shift+F5: Stop; F6: Pause/Resume.
- Right mouse + WASD: camera; W/E/R: Move/Rotate/Scale.
- F: frame selection; Delete: delete selected scene object.
- F11: GPU capture; F12: material log; F2 renames in Asset Browser.

#### Short tutorial
1. Select a model, press F and then W.
2. Click a text field and type; the field can consume keys.
3. Finish typing, focus Scene View and retry the gizmo key.

#### Result and common mistake
Focus, active dragging, brushes and Play affect key handling. While flying, W does not simultaneously switch the gizmo.

<!-- topic: troubleshooting -->
### Common problems: a quick checklist

#### Purpose
Identify the system to inspect before changing unrelated settings.

- Cannot create/edit: open project/scene, Stop Play, correct selection.
- Invisible model: Enabled, editor visibility, Scale, Near/Far and import/loading errors.
- Dark/incomplete material: texture slots, lighting/ambient, normal map, alpha mode, overrides.
- Weak paint coverage: Overall coverage; Edge smoothing controls the soft edge.
- Falling character: ground collider, capsule, physics layers, active Play.
- Script not running: Enabled, Backend, asset/class and Console compile/reload errors.
- Silent audio: Source, Listener, clip, bus volumes, Play On Start.
- Low FPS: same camera/resolution, necessary views only, check foliage/shadows/particles/reflection.

#### Short tutorial
1. Save a copy and isolate one problematic object.
2. Open the relevant panel and Console.
3. Change one setting, comparing Edit/Play and runtime.
4. Record exact steps, messages and measurement conditions.

#### Result and common mistake
You get a reproducible problem. There is no general Ctrl+Z guarantee for every editor operation; use only undo actions actually provided by the current tool.
