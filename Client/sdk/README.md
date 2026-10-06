# IxtreemeWorld Native C++ Game Module SDK

> **Legacy path.** C++ is no longer a project scripting language: projects use **AngelScript**
> (`.as`) and **Lua** (`.lua`) assets — both hot-reload in Play and need no build step. This SDK
> documents the native C++ game-module path, kept for existing projects and engine-internal scripts.

Write your game's gameplay code in native C++ and run it in the **prebuilt** IxtreemeWorld engine — no
engine source required. You compile your `NativeScript` classes into a **game module DLL** against the
headers in this SDK; the engine loads it at project open and your classes appear in the Script
component's **Native** class picker, running in Play exactly like the built-in scripts.

This is the C++ counterpart to AngelScript/Lua scripting. Use it only for legacy projects; new
gameplay should be written in AngelScript or Lua, which also work without the engine source.

## Requirements (read this first)

The plugin boundary is the **C++ ABI**. Your module **must** be built with the **same toolchain** as the
engine binary, or it will crash/corrupt silently:

- Same **MSVC toolset** (e.g. the same Visual Studio version the engine shipped with).
- **Static runtime** `/MT` (`MultiThreaded`), matching the engine.
- **C++20**.

The example `CMakeLists.txt` already sets all three. A version constant (`IXTREEME_MODULE_API_VERSION`)
guards against a stale SDK, but it cannot detect a toolchain mismatch — that is on you.

## Write a script

```cpp
#define IXTREEME_GAME_MODULE 1             // module mode (the example CMakeLists doesn't set this for you)
#include "ixtreeme/NativeScript.h"
#include "ixtreeme/IxModuleRegistry.inl"   // include in EVERY script .cpp (inline-merged — no special file)

class MyScript : public ixscript::NativeScript {
public:
    void OnStart()  override { m_speed = ParamFloat("speed", 5.0f); }
    void OnUpdate(float dt) override {
        float p[3]; GetPosition(p);
        if (IsKeyDown(ixscript::ScriptKey::W)) p[2] += m_speed * dt;
        SetPosition(p);
    }
private:
    float m_speed = 5.0f;
};
IXSCRIPT_REGISTER(MyScript)
```

### Lifecycle hooks (override what you need)
`OnStart()` · `OnUpdate(float dt)` · `OnDestroy()` · `OnCollision(uint32_t otherEntityId)`

### Engine API (convenience methods on `NativeScript`, or the raw `api->` facade)
- Transform: `GetPosition/SetPosition`, `GetRotation/SetRotation` (Euler **degrees**), `GetScale/SetScale`
- Input/time: `IsKeyDown(ScriptKey)`, `MouseDelta(dx,dy)`, `DeltaTime()`, `ElapsedTime()`
- World: `Find(name)`, `PlayOneShot(clipAssetId)`, `Log/LogError`
- Spawn/destroy: `SpawnMesh(meshAssetId,x,y,z)->id`, `SpawnPrefab(...)` *(WIP)*, `DestroyEntity(id)`, `DestroySelf()`
- Physics: `Raycast(ox,oy,oz, dx,dy,dz, maxDist) -> RaycastHit{hit, entityId, point[3], normal[3], distance}`
- Animator: `SetAnimatorFloat(name,v)` / `SetAnimatorBool(name,v)` / `SetAnimatorTrigger(name)` (no-op if no animator)
- Parameters: `Param("key")` / `ParamFloat("key", default)` — untyped string values set per-entity
- Network (v4): `NetConnect(host, port)->handle` (non-blocking; 0 = could not start), `NetState(handle)`
  (0 connecting, 1 connected, 2 closed, 3 failed), `NetSend(handle, bytes, n)`, `NetReceive(handle, buf, cap)->n`,
  `NetClose(handle)`. A plain TCP **byte stream** — framing and the game protocol belong in your script.
  Every stream is closed when Play stops.
- Prompt (v4): `PromptText(title, label, secret)->id` shows a one-line input dialog (`secret` masks it, e.g. a
  password; 0 = no prompt UI on this host — today only the editor draws them), then poll
  `PromptResult(id, buf, cap)`: 0 still open, 1 submitted (text copied, the engine forgets it), -1 cancelled.
- Material (v5): `SetMaterial(slot, material)` on this entity, or `api->SetMaterial(id, slot, material)` on any
  entity (e.g. one just spawned) — `material` is a material asset GUID (as in a scene's `materials`) or its asset
  id. Deferred like spawn/destroy, so it applies to an entity spawned earlier in the same frame.

### Example: a whole MMO client as a scene script

`examples/mmo_client/MmoClient.cpp` is the IxtreemeWorld MMO client protocol written as a script: login
prompt, login server handshake/login/character select, game server handshake and EnterWorld, transform
frames (incl. layered volume/layer changes) and movement packets, with a hand-written Cap'n Proto codec —
the engine itself knows nothing about the protocol. Attach `MmoClient` to the player's entity (parameters:
`loginHost`, `loginPort`, `character`, `proxyMesh`, `visualLift`) and press Play.

`ScriptKey`: `W A S D Space Shift Ctrl Up Down Left Right MouseLeft MouseRight` — **all wired** (v3).
> **Spawn is deferred:** `SpawnMesh` returns a usable id immediately, but the entity actually appears
> after the current frame's scripts finish (a "ghost" until then — `GetPosition` on it no-ops that frame).
> `OnCollision` fires **enter-only** (once per contact, like Unity's `OnCollisionEnter`).
> Input: with a player `CharacterController` in the scene, WASD reaches your script; without one, Play
> uses the free-fly camera and WASD drives that instead (arrow keys always reach the script).

### Serialized fields — `IX_REFLECT` (Unity `[SerializeField]` style)

Declare plain member fields and list them in `IX_REFLECT(Class, ...)`. They appear in the engine
inspector as **typed widgets**, are serialized per-entity, and the saved value is written into the
member **before `OnStart`** — so you just use the member directly:

```cpp
class HelloSpinner : public ixscript::NativeScript {
public:
    float speed = 120.0f;            // the in-class value is the inspector DEFAULT
    bool  clockwise = true;
    float tint[3] = {1, 1, 1};       // float[3] -> Vec3 widget; float[4] -> Color (rgba)
    int   bursts = 3;

    void OnUpdate(float dt) override { /* speed/clockwise/... already hold the inspector values */ }

    IX_REFLECT(HelloSpinner, speed, clockwise, tint, bursts)
};
```

- Supported types: `float`, `int`, `bool`, `float[3]` (Vec3), `float[4]` (Color, rgba 0..1). Up to 16 fields.
- The default shown in the inspector is the field's in-class initializer; an unedited field keeps that
  default (nothing is serialized until you change it).
- The engine reflects the schema by briefly default-constructing your class, so **constructors must be
  cheap and side-effect-free** (don't call the engine API from a constructor — use `OnStart`).
- `IX_REFLECT` and the manual `Param`/`ParamFloat` table are mutually exclusive per class: if a class
  declares `IX_REFLECT` fields, the inspector shows typed widgets; otherwise it shows the key/value table.

## Build & install

```sh
cd examples/hello_module
cmake -S . -B build
cmake --build build --config Release
```

Copy the resulting `HelloGame.dll` into your project's modules folder:

```
<YourProject>/Binaries/HelloGame.dll
```

Open (or re-open) the project in the engine. The log shows `loaded game module HelloGame.dll: N native
class(es) registered`. Add a **Script** component to an entity, set **Backend = Native**, pick your class
from the **Class** dropdown, set any parameters, and press **Play**.

## What's in the SDK (`include/ixtreeme/`)

| Header | Purpose |
|---|---|
| `NativeScript.h` | the base class you derive from + `IXSCRIPT_REGISTER` |
| `ScriptApi.h` | the engine facade (`IScriptApi`) your script calls |
| `IxModuleApi.h` | the C-ABI module contract + version constant |
| `IxModuleRegistry.inl` | registration plumbing + the exported entry point (include in each script .cpp — inline-merged) |

## v1 limitations

- **No hot reload.** The DLL is loaded at project open; rebuild + reopen the project to pick up changes.
- **One ABI version.** A module built against a different `IXTREEME_MODULE_API_VERSION` is rejected
  (v4 added the network/prompt slots, v5 `SetMaterial`: rebuild older modules).
- **Native only.** This is the C++ path; Lua scripts ship as `.lua` assets (no DLL).
- **Desktop.** Module DLL loading is the desktop workflow; on Android native code is built into the app.
