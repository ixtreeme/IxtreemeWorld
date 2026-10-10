# Editor Guide / Szerkesztői súgó

Az editorban: **Help > Editor Guide / Súgó**. Magyar és angol nyelv, témalista, teljes szöveges keresés, formázott cikkek, rövid tutorialok és a teljes cikk másolása. A keresés mindkét fordítást vizsgálja, ékezet és kis-/nagybetű különbség nélkül. Nyelvváltáskor a kiválasztott téma megmarad.

In the editor: **Help > Editor Guide / Súgó**. Choose Magyar or English, browse topics or search both translations. The selected topic survives a language switch. Each article explains its purpose, a short hands-on tutorial, the expected result and common mistakes.

The current guide contains **50 topics in nine categories**. Both documents are embedded into editor builds; help works offline, with no external documentation directory. Standalone game builds exclude the help browser and content.

## Documents and coverage

| Category | Covered systems and workflows |
| --- | --- |
| Getting started | Projects/scenes/saving, docking/layout, navigation/transforms/snapping, Hierarchy, selection, Play/Pause and cameras |
| Assets and materials | Asset Browser, import/file management, FBX materials/textures, EXR, PBR/water editor, overrides, prefabs, FBX export |
| Terrain and water | Terrain creation/resolution, Sculpt, three-slider painting, material layers/previews, triplanar mapping, water shape and materials |
| Objects and physics | Components/Notes, lights, Main Camera, colliders/triggers/materials/layers, Rigidbody, joints, Character Controller |
| Animation, effects and audio | Skinned models/clips, Animator states/transitions/parameters, blend trees, particles/effect assets, audio/listeners/buses |
| Scripts and game UI | Lua/AngelScript/hot reload/fields, entity/input/effect API, filtered raycast/overlap/enabled state, RmlUi/prompts, TCP and legacy native builds |
| World building | Tree Generator, LOD/leaf LOD/GPU occlusion, baked and view impostors, collision layers/support probe, server export/spawn |
| Environment and image quality | Sun/ambient/time presets, procedural/image skies, God rays, Tone mapping |
| Settings and publishing | Project Settings/autosave, FPS/CPU/GPU/RAM/statistics, Physics Debugger, Console/Build Output/diagnostics, Build Game, shortcuts and troubleshooting |

- [Magyar útmutató](hu.md)
- [English guide](en.md)

## Maintaining the guide

Update both UTF-8 Markdown documents together when the editor changes. Keep the same stable topic IDs and navigation order in both languages. IDs are defined by `<!-- topic: terrain-paint -->` before an article.

- `##` starts a navigation category; `###` starts an article; `####` renders a section heading.
- Inline `**bold**` and backticks render emphasis and code/field names.
- Numbered steps, bullets, paragraphs and `>` callouts are supported. Avoid Markdown tables, links or embedded images inside the embedded articles; the renderer is deliberately small and self-contained.
- Retain the Purpose / Short tutorial / Result and common mistake sections (and Hungarian equivalents). Use actual menu/field names and working steps; separate asset settings from instance settings.
- Document current behaviour and supported operations. Do not promise future features or universal undo.

CMake watches both documents and regenerates the embedded header after a change. `EditorHelpTest` verifies feature inventory, both translations, sections, parsing, bilingual accent-insensitive search, topic preservation and rendering every article at normal and narrow widths.

Example verification from the repository root:

```powershell
cmake --build Client/build --config Debug --parallel 3
ctest --test-dir Client/build -C Debug -R EditorHelpTest --output-on-failure
```

Visual QA: open Help in the editor, switch language, select a long topic, resize the window, search accented/unaccented terms, clear the search, check an empty result, copy an article, close/reopen and dock the guide. Verify `ő` and `ű` display correctly with bundled Inter fonts.
