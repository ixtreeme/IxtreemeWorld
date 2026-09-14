# ExampleGame (Phase 1 scaffold)

Future game-project layout (Auriga follows this):

```
Projects/ExampleGame/
  Project.json  — project metadata (this scaffold)
  Source/Game.cpp — game-module code, compiled by Editor Build into a DLL (not built in Phase 1)
  Assets/       — project art/audio (cooked by AssetCooker in Phase 2+)
  World/        — project scenes/levels (cooked by WorldCooker in Phase 2+)
  Config/       — project settings (DefaultEngine.ini-style)
```

No Auriga gameplay code is introduced in Phase 1. Engine has zero references to this directory.
