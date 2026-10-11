# imgui-node-editor

Source: https://github.com/thedmd/imgui-node-editor

Pinned commit: `021aa0ea4da13fed864bafb2a92d4c5205076866` (2026-03-29).

License: MIT; the unmodified upstream license is in `LICENSE`.

Only the core sources are vendored. Examples, their graphics backends, and the
example-specific fork of Dear ImGui are not included. The library uses the
engine's existing `imgui::imgui` target and is built only with the editor.

Local additions: CMake target and this provenance file.

Compatibility change for Dear ImGui 1.92.8: the scalar-left `ImVec2` multiplication
operator in `imgui_extra_math.h/.inl` is guarded for older versions, because the
engine's ImGui now provides it. No changes to the engine's ImGui are required.

The UI-independent `crude_json.cpp` component is compiled in `IXMaterialGraph`,
which is shared with runtime builds. `IXImGuiNodeEditor` links that target rather
than compiling another copy of JSON. The same MIT notice is packaged with both
editor and runtime executables. Upstream JSON sources are unchanged.
