# Project Status Log

Date: 2026-06-08 21:50:53 +02:00
Workspace: `D:\IxtreemeWorld`

## Current State

The editor build is currently green after the latest project/scene workflow fixes.

Latest verified commands:

```text
cmake --build Client\build --config Debug --target VulkanClear -- /m
cmake --build Client\build --config Debug --target IwSelfTest -- /m
Client\build\apps\selftest\Debug\IwSelfTest.exe --asset --render --client-root Client
```

Latest selftest result:

```text
[SUMMARY] passed=146 failed=0
```

Known non-blocking build warning:

```text
'pwsh.exe' is not recognized as an internal or external command,
operable program or batch file.
```

## Implemented Areas

### Project Workflow

- Added `ProjectManager`.
- Added `project.ixproj` create/open/save flow.
- Create Project creates:
  - `project.ixproj`
  - `Assets/`
  - `Scenes/`
- Editor boot starts with a No Project modal.
- Asset Browser is not bound to engine assets on editor boot.
- After a project is active, Asset Browser binds to the project `Assets/` folder.
- Project browser now supports:
  - typed path
  - `Go`
  - Windows drive buttons
  - folder/project filter
  - creating missing typed folders in Create Project mode
  - `Use This Folder`

### Scene Workflow

- Added `SceneManager`.
- Added scene JSON save/load with sidecar files.
- Scene type routes RmlUi runtime UI:
  - `login` -> Login
  - `lobby` -> Lobby
  - `world` -> HUD
  - `loading` -> loading
  - `empty` -> no runtime UI
- Editor build starts with no auto-loaded scene.
- New Scene now counts as an open scene even before it has a saved file path.
- `HasOpenScene()` now uses a dedicated scene-open flag instead of checking whether a file path exists.
- Unsaved new scene appears as `Untitled`.
- Project-relative scene save/recent handling is wired.

### Editor Boot / UI

- Editor build forces ImGui editor open at boot.
- RmlUi player UI remains hidden in empty editor state.
- Diagnostic logging remains in place for boot/frame/UI routing checks.
- Editor chrome, dockspace, menu, hierarchy, scene settings, asset browser, and tools are expected to render in editor mode.

### Asset Browser

- Project Assets root is separated from engine/built-in asset root.
- Asset Browser has category filtering, folder filtering, grid wrapping, independent scroll zones, previews, compact tile labels, and hover metadata.
- Existing asset creation/import/editing flows remain in the ImGui editor.

### Hierarchy / Scene Editing

- Added Hierarchy panel with water bodies, point lights, and spot lights.
- Supports selection, focus, rename, duplicate, delete, and editor visibility toggle.
- Scene dirty state is marked on relevant editor changes.

## Current Git Working Tree

Modified files:

```text
Client/apps/client/src/main.cpp
Client/apps/selftest/src/main.cpp
Client/libs/platform/NativeWindow.h
Client/libs/platform/NativeWindow_Win32.cpp
Client/libs/platform/NativeWindow_Win32.h
Client/libs/render/AssetLibrary.cpp
Client/libs/render/AssetLibrary.h
Client/libs/render/CMakeLists.txt
Client/libs/render/EditorImGui.cpp
Client/libs/render/EditorImGui.h
Client/libs/render/GameClientLayer.cpp
Client/libs/render/GameClientLayer.h
Client/libs/render/IconsFontAwesome6.h
Client/libs/render/MapEditorTypes.h
Client/libs/render/RmlUiLayer.cpp
Client/libs/render/RmlUiLayer.h
```

Untracked files:

```text
Client/libs/render/ProjectManager.cpp
Client/libs/render/ProjectManager.h
Client/libs/render/SceneManager.cpp
Client/libs/render/SceneManager.h
```

Diff stat at time of log:

```text
16 files changed, 1955 insertions(+), 22 deletions(-)
```

Note: untracked `ProjectManager.*` and `SceneManager.*` are not included in that diff stat.

## Last User-Visible Fixes

- Browse button was made visible in Create Project.
- Browse panel gained typed path, Go, drive buttons, and filter.
- Missing typed browse folder creation was added for Create Project mode.
- Create Project button was moved above the browse panel and now shows the exact target path.
- New Scene now actually opens an unsaved scene instead of being treated as no scene.

## Open Follow-Up Candidates

- Persist Recent Projects across editor restarts.
- Replace remaining native scene open/save Win32 dialogs with the in-editor browser if desired.
- Add a visual confirmation after Create Project/Open Project with project root and manifest path.
- Add direct runtime smoke check if a manual editor launch is available.
