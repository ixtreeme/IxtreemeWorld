# Hátralévő munka – runtime teljesítmény (átadási terv, 2026-10-08)

Ez a terv a `docs/runtime-parallelization-plan-20261007.md` végrehajtása után maradt munkát írja le, úgy, hogy
egy másik ügynök előzmények nélkül folytatni tudja. A mért eredmények és a döntések indoklása a
`docs/perf-baseline-20261007.md`-ben vannak; ezt érdemes először végigolvasni.

## 1. Állapot

- **Ág:** `codex/engine-server-integration`.
- **Kész és pusholt (`83f9025`-ig):**
  - mérési alapvonal;
  - job-rendszer (`libs/jobs`);
  - render-rekordok és culling;
  - párhuzamos animáció és részecskék;
  - aszinkron modellbetöltés;
  - spawnonkénti fizika;
  - inkrementális és eltolódó statikus árnyék-cache;
  - halasztott GPU-feltöltés riggelt és statikus modellekhez.
- **Commitolva, még NEM pusholva:**
  - `cc503e6`: anyag-textúrák aszinkron betöltése;
  - `d38ce2c`: árnyék-LOD (opak egyszerűsítés és levélkártya-ritkítás a távoli kaszkádban).
- **Ebben a lépésben hozzáadva, nem commitolva:** ez a dokumentum, valamint a `Client/tools/perf/measure.ps1` és a
  `Client/tools/perf/spawn.ps1`.

## 2. Kötelező szabályok

Ezek a felhasználó állandó kérései; mindegyik feladatra érvényesek.

- **Git:**
  - Ne resetelj, ne stash-elj, és ne törölj idegen munkát.
  - Commit csak kifejezett kérésre, push csak kifejezett kérésre.
  - Egy kérésben kért több commit külön commitokba menjen.
- **Platform:** az engine cross-platform lesz, ezért nem kerülhet bele Win32-specifikus kód. OS-függő rész csak a
  platformrétegen át mehet.
  - Elfogadott minta környezeti változó olvasására: `_dupenv_s` `#if defined(_WIN32)` alatt, máskülönben
    `std::getenv`. Példa: `ShadowLodsEnabled()` a `Client/libs/render/StaticMeshRenderer.cpp`-ben.
  - A sima `getenv` C4996 figyelmeztetést ad.
- **Adatok:**
  - Új map-adatot vagy generált fixture-t külön helyre írj; a meglévő világcsomagot ne írd felül automatikusan.
  - A `D:/AkitaOnline`-t ne módosítsd, csak másolatot használj.
  - A `D:\AurigaGlobal\LiveWork`-höz ne nyúlj.
  - A `Client/assets/Maps/test_zone`-t ne írd felül.
- **Titkok:**
  - A DB-konfigurációra (`gameserver/build/apps/gameserver/Debug/database.json`) csak útvonallal hivatkozz; a
    tartalmát soha ne írd ki.
  - Teszt-belépési adat csak a helyi GUI beviteli mezőibe kerülhet; ne tárold, ne naplózd, ne ismételd.
- **Folyamatok és worktree-k:**
  - Csak olyan folyamatot állíts le, amelyet te indítottál (PID alapján).
  - A `.claude/worktrees/gifted-lovelace-4def65`-höz ne nyúlj.
- **Jelentés:**
  - Magyarul írd.
  - Le nem futtatott tesztet ne minősíts sikeresnek.
  - A feladat kiterjesztését jelezd.
  - Mért számokat adj előtte–utána formában.
- **Ideiglenes mérőkód:** nem kerülhet commitba. Jelöld egyértelműen, és a végén ellenőrizd (`git diff`), hogy
  eltűnt.

## 3. Környezet, build, mérés

### Build (Git Bash, a `Client` mappából)

```bash
cmake --build build --config Release --parallel
cmake --build build-runtime --config Release --parallel
ctest --test-dir build -C Release
```

- `build`: az editor, plusz a tesztek. A `ctest` jelenleg 5/5.
- `build-runtime`: a szerkesztő nélküli játéklejátszó, ezzel mérünk.
- A Git Bash alatt `--parallel` kell, nem `/m`.
- **LNK1104:** a futó játék fogja az exe-t. Állítsd le a saját PID-edet, utána linkelj újra.
- **Fordítási figyelmeztetések:** a `TerrainRenderer.cpp` `thickness` és a runtime-build `EngineApplication.cpp`
  C4189-es figyelmeztetései régiek, nem tőlünk vannak.

### A stresszprojekt

- **Helye:** `Client/build/stress-20261007/project` (nincs gitben). A generátor:
  `Client/tools/perf/make_stress_scene.js`.
- **Tartalma:** 10 071 háló (3004 `ujfa` fa, 6472 háromszög/fa), 64 skinnelt karakter, 50 CPU-emitter, ~5000
  részecske.
- **Jelenetváltozatok** a `Scenes/Main/` alatt; a kívántat másold a `Main.scene` helyére:
  - `.full`: alap, gyenge nap;
  - `.shadowtest`: erős oldalnap;
  - `.shadowvis`: erős nap, és 25 s-nál 12 fa törlődik;
  - `.spawntest`, `.staticspawn`: játék közbeni spawn és destroy;
  - `.physdestroy`;
  - `.particles`: 101 emitter, ~100 000 részecske;
  - `.physics`: 1000 eső doboz;
  - `.crowd`, `.crowd256`, `.crowd256anim`.
- **Teszt után** a `Main.scene` mindig a `Main.scene.full` másolata legyen.
- **`SpawnTest.lua`** (`Assets/scripts/`): a paraméterei a jelenetben vannak.
  - `model`: a spawnolt modell assetje;
  - `destroyBodies = "1"`: törlés fizikai testekkel;
  - `visual = "1"`: a 12 fa törlése.
  - A spawnhoz használt modellmásolatok: `Assets/ujfa_spawn`, `Assets/KicsiK_spawn`.

### Mérőszkriptek

- **`Client/tools/perf/measure.ps1 -Seconds 25 [-Gpu] -Env @{ ... }`:** elindítja a buildelt játékot, és kiírja
  a [PERF] sorokat.
  - Az `IX_PERF_LOG=2` 2 másodpercenként átlagol.
  - A `-Gpu` (`IX_GPU_PROFILE=1`) GPU-időket ad passonként, és a kaszkádokra is.
- **`Client/tools/perf/spawn.ps1`:** a spawn-teszt sorai, másodpercenkénti [PERF] sorral és az ablak legnagyobb
  frame-jével.

### Kapcsolók

| Kapcsoló | Hatás |
| --- | --- |
| `IX_JOBS=0` | soros futás |
| `IX_JOBS_WORKERS` | a workerek száma |
| `IX_PARALLEL_CULL`, `IX_PARALLEL_ANIMATION`, `IX_PARALLEL_PARTICLES` | `=0`: az adott csoport sorosan fut |
| `IX_SHADOW_SHIFT=0` | a mozdult kaszkád egészben rajzolódik újra |
| `IX_SHADOW_LOD=0` | nincs árnyék-LOD |
| `IX_SKIN_VERIFY=1` | a régi, szinkron skin-ellenőrzés |

### Validáció (Release)

A `measure.ps1` `-Env` paraméterébe:

```text
VK_INSTANCE_LAYERS = "VK_LAYER_KHRONOS_validation"
VK_KHRONOS_VALIDATION_DEBUG_ACTION = "VK_DBG_LAYER_ACTION_LOG_MSG"
VK_KHRONOS_VALIDATION_LOG_FILENAME = <log útvonala>
VK_KHRONOS_VALIDATION_REPORT_FLAGS = "error"
VK_KHRONOS_VALIDATION_VALIDATE_SYNC = "true"
```

Az elvárás: üres log. Validációval a játék ~96 FPS-sel fut, ez normális.

### Referenciaszámok

Buildelt játék, `Main.scene.full`, ezen a gépen:

- 128–138 FPS;
- CPU-frame ~7,3–7,7 ms (render ~2,4, árnyék ~0,8, animáció ~1,4);
- GPU-frame ~7,3–8,2 ms, ebből a fő scene pass 5,5–6,6 ms, az árnyék 0,7–0,8 ms, a terep ~0,45 ms.

A játék tehát a GPU-n áll. Az editor a stresszprojekttel Scene nézetben ~45 FPS (22 ms), Play módban ~38 FPS.

### Buktatók

- **Csalóka állókép:** álló kamerával a statikus árnyékréteg a cache-ből jön, ezért az árnyék költsége nem
  látszik.
  - Teljes újrarajzolás méréséhez ideiglenes kapcsoló kell: a `TerrainRenderer::RenderSunShadowMap` kaszkádciklusa
    elején törölni kell a `m_shadowCascadeInputs[c]` és a `m_staticShadowInputs[c]` értékét.
  - Kaszkádonkénti GPU-idő méréséhez a `ShadowCascadeNBegin/End` időbélyegeket a statikus `Whole` ág köré kell
    tenni (alapból csak a compose-t mérik).
  - Ezt a kódot ne commitold.
- **Zajos GPU-profil:** egy sor 1–4 mintából áll. Futtass többször, és mindig A/B-ben mérj ugyanazzal a
  jelenettel.
- **Fehér ablak:** a maximalizált ablak induláskor fehér lehet; a címsorra duplán kattintva rendbe jön.
  - A `PrintWindow`-alapú ablakmentés ennél a játékablaknál memóriahibával elszállt. Képernyőképpel ellenőrizz.
- **Play mód:** az editor Play módban eldobja az Inspector-változásokat.
- **Selftest:** az `apps/selftest/src/main.cpp` (~1715. és ~1740. sor) forrásszövegre ellenőriz:
  `editorImGui.SetHierarchySceneState` és `buildHierarchyEntities`. Ezek átnevezése eltöri.
- **Új `MapEditorCommands` mező:** kézzel kell felvenni a `MergeMapEditorCommands`-be, különben csendben elvész.

## 4. Feladatok, prioritás szerint

### 4.1 Editor-hierarchia 10 000 entitásnál (első)

**Probléma:** az editor frame-jének fő tétele.

- A `buildHierarchyEntities` (`Client/apps/client/src/EngineApplication.cpp` ~4617. sor, hívása ~11759. sor)
  minden frame-ben végigmegy mind a 10 000 entitáson, és újraépíti a vektort és a térképeket. Ez kb. 11 ms.
- A panel (`Client/libs/render/editor_panels/EditorImGuiHierarchyPanels.inl`) mind a 10 000 sort ImGui-elemként
  rajzolja, vágás nélkül.
- Az idő a `frameProfile.hierarchyIterationMs`-ben mérődik. Az editor debug-parancsa (`commands.dumpFrameProfile`)
  kiírja a `[FRAME-PROFILE]` sorokat.

**Lépések:**

1. **Mérés:** stresszprojekt az editorban, `[FRAME-PROFILE]` dump. Jegyezd fel a `hierarchy_iteration` és az
   `editor_ui_render` értékét, valamint az FPS-t.
2. **Újraépítés csak változáskor:**
   - a hierarchia-állapot csak akkor épüljön újra, ha a jelenet változott (entitás hozzáadva vagy törölve,
     átnevezés, átszülőzés, komponenscímke);
   - használd a meglévő revíziókat, vagy vezess be egy hierarchia-revíziót;
   - különben maradjon az előző állapot, és ne másolódjon át az `EditorImGui`-be (`SetHierarchySceneState`).
3. **A panel csak a látható sorokat rajzolja:**
   - a nyitott csomópontokból lapos, látható sorlistát kell építeni (ez is csak változáskor készüljön);
   - erre `ImGuiListClipper` kell;
   - a fa-behúzást és a kijelölést soronként kell kirajzolni.
4. A sorrend, a kijelölés, az átnevezés, a drag-and-drop átszülőzés, a kontextusmenü, a note-ikon és a keresés
   viselkedése maradjon változatlan.

**Elfogadás:**

- a `hierarchy_iteration` álló jelenetnél 1 ms alatt marad;
- az editor FPS-e mérhetően nő; számokat előtte–utána formában adj;
- a felsorolt műveletek kézzel kipróbálva működnek;
- a selftest és a ctest zöld.

### 4.2 A fő scene pass GPU-ideje (~5,5 ms)

**Probléma:** a játék GPU-korlátos, és a fő tétel a fő scene pass. Hogy ezen belül mi a drága, azt még nem
bontottuk le. A jelölt tételek:

- a 3004 fa kérge és alfa-maszkolt levelei (maszkolásnál nincs early-Z);
- a 64 skinnelt karakter;
- a részecskék;
- a kockák.

**Lépések:**

1. **Lebontás:** fix kamerával, ideiglenes, nem commitolt kihagyó kapcsolókkal kategóriánként mérd le a fő pass
   GPU-idejét. Kategóriák:
   - opak statikus;
   - maszkolt statikus;
   - skinnelt;
   - részecske;
   - átlátszó.

   Ez a módszer az árnyékpassnál bevált: ott kiderült, hogy a költség szinte mind a maszkolt leveleké.
2. **Javítás a lebontás alapján.** Jelöltek:
   - **Távolsági LOD a fő nézetben:** van meglévő entitásonkénti LOD-rendszer (`LodConfig`, `lod_component`,
     `StaticMeshRenderer::BuildLodCpuSet`). A stresszjelenetben ezt csak egy entitás használja. Először egy
     jelenetmásolaton próbáld ki a fákon, tartalmi változásként.
   - **Maszkolt levelek:** early-Z, például depth prepass a maszkolt anyagokra.
   - **Kis tárgyak:** kiszűrésük vetített méret szerint.
3. **Ellenőrzés:** minden változásnál A/B mérés, képi összevetés ugyanabból a kamerából, és üres szinkron
   validációs log.

**Elfogadás:** dokumentált lebontás a `perf-baseline` doksiban. A javítás csak mért nyereséggel és képi eltérés
nélkül kerül be, vagy a dokumentált, elfogadott eltéréssel.

### 4.3 Árnyék-maradékok

1. **Mélységlépés-átlépés.** A kaszkádok mélységközepe 64 m-es lépésekben követi a kamerát a fény irányában
   (`TerrainRenderer.cpp`, `UpdateShadowCascades`: `centerZ = floor(eye.z / kShadowDepthStepMeters) * ...`).
   - Átlépéskor mind a 4 kaszkád statikus rétege egyetlen frame-ben egészben újrarajzolódik. Az alap jeleneten ez
     ~17–19 ms GPU, és rántásként látszik.
   - **Javaslat:** kaszkádonként eltolt fázis (pl. kaszkád × 16 m), így egy átlépés egyszerre csak egy kaszkádot
     érint.
   - Választható még a hiszterézis is. A ráhagyás (`depthPadding = 80 + step`) mindkét irányban fedi az
     eltolást; ezt ellenőrizni kell.
   - **Elfogadás:** egyenesen gyalogolva nincs olyan frame, amelyben mind a 4 réteg újrarajzolódik; a legnagyobb
     frame mérhetően csökken; a képi A/B azonos.
2. **A közeli kaszkádok levél-kitöltése.** Az 5 m-es és a 15 m-es kaszkád teljes újrarajzolása 6,5 ms és 5 ms. Ezt
   a maszkolt levelek kitöltése viszi, ezen a LOD nem segít.
   - Először mérd meg, milyen gyakran fordul elő teljes újrarajzolás gyaloglás közben az 1. pont után.
   - Csak ha gyakori: a legközelebbi kaszkád felbontásának csökkentése, vagy a maszkolt vetők csökkentése a közeli
     kaszkádban. Mindkettő képi kompromisszum, előtte kérdezd meg a felhasználót.
3. **Sávok gyaloglás közben:** a sávok újrarajzolása kb. 0,8 ms CPU, mert a sávba lógó fák egészben
   rajzolódnak. Alacsony prioritás.
4. **Volumetrikus fény biasa:** a `shaders/VolumetricLight.hlsl` mélységegységben kapja (`params.y`). A
   mélységtartomány a 64 m-es lépés óta nagyobb, így a bias méterben kissé nőtt.
   - A `Terrain.hlsl` ugyanezt már méterben tartja: a VP z-oszlopából számolt skálával vonja ki a ráhagyást.
   - Ugyanígy kell javítani, és a módosított shaderből újra kell generálni a tárolt `.spv`-t.

### 4.4 Ellenőrzött, kisebb nyitott tételek (script és prefab)

A kódban ellenőrizve, 2026-10-08:

- **`SpawnPrefab`:** a script API-ban megvan (Lua és AngelScript). Az alkalmazás azonban kihagyja: a
  `Client/apps/client/src/EngineApplication.cpp` ~6969. sora azt naplózza, hogy „SpawnPrefab not yet supported”.
  Meg kell valósítani a prefab-példányosítást a deferred op-ban, a `SpawnMesh` mintájára (render-rekord, fizika,
  árnyék-cache).
- **`OverlapSphere` és `SetEntityEnabled`:** nincsenek a script API-ban, és a raycastnek nincs layer-maszkja.
  - A natív modulok C-ABI-ja (`sdk/include/ixtreeme/IxModuleApi.h`) stabil szerződés, bővítése ABI-verzióváltás.
  - Ezt előbb terv szintjén egyeztesd a felhasználóval.
- **`animator_controller_id`:** a prefab nem menti; jelenleg csak a jelenet (`SceneManager.cpp`) szerializálja.

### 4.5 A párhuzamosítási terv későbbi fázisai (csak ha mérés indokolja)

A `runtime-parallelization-plan-20261007.md` alábbi pontjai nincsenek kész. Mindegyik csak valódi terhelésnél
érdemes:

- a scene snapshot (`BuildSceneSnapshot`) felépítése workerben (5. fázis);
- a shader-fordítás és a pipeline cache előkészítése workerben, valamint az asset-reload soak teszt (5. fázis);
- a víz és a tükröződés láthatóságának előkészítése jobokban (4. fázis);
- a character controller, a valódi hálózati decode és az audio staging (6. fázis). A hálózat ma no-op, ezért
  mesterségesen nem kell párhuzamosítani.
- a párhuzamos Vulkan command recording, secondary command bufferekkel (7. fázis, F mérföldkő). Ez külön
  renderer-projekt.
- a scriptek párhuzamosítása: csak a read-only lekérdezés és a command buffer API szétválasztása után;
- a transparent queue szálankénti összefésülése: szándékosan kimaradt, mert nem mérhető tétel.

### 4.6 Más szálak (a korábbi feljegyzések szerint, nem ellenőrizve – lehet elavult)

- **Game nézet:** a víz ott hiányzott.
- **Gameserver:** a Map Data Layer munka a MAP-4 előtt megállt. A layered 3D következő mérföldköve (3D-6
  átmenetek vagy összetett felületek) nincs kiválasztva. Egy worktree-ben commitolatlan autosave-palettajavítás
  van. Több szerver-commit pusholatlan.
- **Asset browser:** ismert korlátok: nem-ASCII nevek, meta nélküli áthelyezés, és az `IwSelfTest` nincs buildelve.

Ezekhez ne nyúlj a felhasználó kérése nélkül; előbb ellenőrizd az állapotukat.

## 5. Munkamód feladatonként

1. Olvasd el a kapcsolódó kódot és a `perf-baseline` doksi vonatkozó szakaszát.
2. **Mérés előtte**, a stresszprojekten (`measure.ps1`, és ha kell, `-Gpu`), A/B kapcsolóval, ha lehet.
3. **Végrehajtás.** Az új viselkedéshez legyen kikapcsoló környezeti változó, ha összehasonlítani kell
   (`IX_...=0`).
4. **Ellenőrzés:**
   - mindkét build;
   - ctest;
   - szinkron validáció a buildelt játékban;
   - az editor Scene és Play nézete;
   - képi A/B, ha rajzolás változott.
5. **Dokumentáció:** új szakasz a `docs/perf-baseline-20261007.md`-ben: probléma, megoldás, mért táblázat,
   ellenőrzés, ami maradt.
6. **Takarítás:** a `Main.scene` visszaállítva, a saját folyamatok leállítva, az ideiglenes kód eltávolítva.
7. **Jelentés** magyarul; commit csak kérésre.
