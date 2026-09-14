# ThirdParty (Phase 1: declaration only, no moves)

Vendored / fetched dependencies stay where the build expects them in Phase 1 —
moving them would destabilize vcpkg/FetchContent/OZZ_ROOT wiring for zero runtime gain:

- Flecs (vcpkg) — ECS runtime, preserved; EnTT forbidden.
- Vulkan SDK (system) — graphics backend; Apple later via MoltenVK->Metal.
- JoltPhysics v5.5.0 (FetchContent) — physics backend behind libs/physics/PhysicsWorld.
- ozz-animation (OZZ_ROOT=D:/SDK/ozz-animation-master) — animation sampling.
- RmlUi 6.2 (vcpkg) — engine UI capability.
- miniaudio (libs/audio/third_party/miniaudio.h) — audio.
- Assimp + fastgltf (vcpkg) — importer/exporter.
- meshoptimizer v0.23 (FetchContent) — mesh optimization.
- stb, efsw, capnproto, zlib (vcpkg) — image IO, file watching, misc.
- imgui (docking-experimental/vulkan/win32) + imguizmo (vcpkg, editor-only) — editor UI.

Dependency declarations live in Client/CMakeLists.txt + libs/*/CMakeLists.txt and are unchanged in Phase 1.
