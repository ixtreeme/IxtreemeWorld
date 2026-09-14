# Editor — engine consumer (Phase 1)

Editor code may depend on Engine. Engine must NOT depend on Editor (one-directional).

```
Editor/
  Build/BuildService.*  — cmake game-script build worker (lib IXEngineEditorBuild).
                          Triggered by the Build button / auto-build-on-save in the
                          EngineApplication frame loop; UI state (ScriptBuildState,
                          log panel, scaffold, module load/unload) stays in EditorImGui.
  (live, still in libs/render/) — EditorImGui.* + editor_panels/*.inl (Hierarchy, Inspector,
                          Viewport, Gizmos, Selection, Terrain tools, Asset Browser,
                          Project Settings, Build UI), tools/tree/*, UIHelpers.*.
```

Rules:
- No file under libs/{asset,physics,animation,audio,platform,math,common,debug} may
  include EditorImGui.h / editor_panels / tools/tree (audited: none do in Phase 1).
- libs/render/ is transitional: it currently hosts BOTH engine renderers and the editor.
  Phase 2 splits it into Engine/Graphics/Renderer + Editor/*. The .inl panel split in
  the render CMakeLists (editor-only sources guarded by IXTREEME_WITH_EDITOR) is the seam.
- Build UI (AssetBrowserPanels.inl "Build Game Scripts", MenuToolbarPanels.inl "Build")
  calls into ixeditor::build::GameScriptBuildService. It must not embed cmake argv.
