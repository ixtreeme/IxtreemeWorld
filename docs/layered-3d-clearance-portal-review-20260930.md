# 3D-4B – modell-blokkolás, clearance és bizonyított portál-átmenet (review, 2026-09-30)

Állapot: **kész, review-ra vár. Nincs commitolva, nincs pusholva.**
Branch: `codex/engine-server-integration`, baseline HEAD `af97dff`.

## Mit old meg

A 3D-4A csak azt tudta megmondani, milyen magasan van a padló egy explicit
volume-ban. Nem tudott falat, belmagasságot, lépcsőt és volume-váltást.
A 3D-4B ezt a hiányt zárja:

1. **Clearance-rács volume-onként.** Minden járható volume 0.25 m-es
   cellarácsot kap. Egy cella akkor blokkolt, ha bármely statikus akadály
   (minden statikus collider + a terep, mindkét quad-átló) belemetsz abba a
   térrészbe, amelyet a cellán belül *bárhol* álló kapszula elfoglalhat:
   a cella téglalapja a sugárral négyzetesen kitágítva (a körnél
   konzervatívabb), a sík + 2 cm padlókontaktus és a cella legmagasabb
   síkpontja + 1.8 m (+ r·(sec−1) lejtőn) között.
2. **Bizonyított lépcső/rámpa-portál.** Két járható volume között akkor
   jön létre bizonyítás, ha a support-téglalapjaik pontosan (float-egyenlő
   él, tengelyigazított) érintkeznek, és a síkok különbsége a közös él
   mentén legfeljebb a lépésmagasság (0.35 m). A bizonyítás 2D
   folyosó-rácsot tárol (él mentén × él ± (r + cella)); egy folyosócella
   akkor szabad, ha a kapszula a két sík felső burkolója fölött szabad.
3. **Actor-lekérdezések.** `ResolveLayerActorPlacement` / `ResolveLayerActorMove`
   (shared/map). Cella „járható”, ha a saját rácsában szabad, vagy a volume
   valamelyik bizonyított portáljának szabad folyosócellájában van. A mozgás
   a szakasz minden érintett celláját ellenőrzi (supercover, nincs
   átcsúszás vékony falon). Volume-váltás csak **egy** bizonyított portálon
   át, a portál szakaszán belül; az eredmény `portal_id`-t ad. Minden más
   `transition_required`. Clearance nélküli világ: `no_clearance_proof`.
   Nagyobb actor, mint amire sütöttünk: `actor_not_covered`. Hiba esetén az
   állapot változatlan. A 3D-4A support-lekérdezések változatlanok.

Profil (editor): a CharacterController alapértelmezése – r = 0.35 m,
h = 1.8 m, lépés 0.35 m –, 0.25 m cella, 2 cm padlókontaktus.

## Formátum (MX3D v4)

- v4 csak akkor íródik, ha van clearance-profil; support-only világ
  **bájtra azonos v3** marad (teszt + worldbench ellenőrzi).
- Header + 5×f32 profil; volume-onként flag-blokk + `cells_x`, `cells_y`,
  bitcsomagolt rács; portálonként proof-blokk (tengely, él, span, f64 max
  lépés, slots, across, bitcsomagolt folyosó). A kitöltő bitek nullák.
- Validáció: a profil pontosan akkor van jelen, ha van rács vagy proof; a
  rács a footprinthez pontosan illeszkedik és érvényes supportot igényel; a
  proof geometriáját és lépését a síkokból újraszámolja (1e-9 tűrés).
  Cellakorlát volume-onként 2^22, összesen 2^24; kódolt sidecar ≤ 4 MiB.
- Manipulált profil (sugár, lépés, NaN) a szigorú loaderen elbukik
  (`LAYERED_WORLD_INVALID`), a runtime létrejötte előtt.

## Érintett fájlok

Shared (`shared/map`): `LayeredWorld.h/.cpp` (v4 típusok, codec,
validáció), új `LayerClearance.h/.cpp` (sütés), új
`LayerActorMovement.h/.cpp` (lekérdezések), `LayerGroundSupport.h/.cpp`
(új státuszok, `portal_id`), `CMakeLists.txt`, új
`tools/layer_clearance_test.cpp`.

Engine (`Client`): `SceneLayerAuthoring.h/.cpp` (akadály-mesh minden
statikus colliderből, terep-akadály, sütés a generálás végén),
`SceneLayerGround.h/.cpp` (opcionális actor-profil), `EngineApplication.cpp`
(státusz, `[LAYER-CLEARANCE]` trace, vizualizáció, actor-alapú szonda),
tesztek: `SceneLayerAuthoringTest.cpp` (+ Jolt-orákulum),
`SceneLayerGroundTest.cpp` (v4 + `--validate-package` clearance-szakasz).

Szerver (`gameserver`): `WorldRuntime.h/.cpp` – `PlaceLayerActor`,
`MoveLayerActor` (csak olvasó); új `bench/LayerClearanceBench.h/.cpp`,
`worldbench --mode layerclearance`; `CMakeLists.txt`, `WorldBench.cpp`.

Dokumentáció: `layered-3d-world-contract.md`, `layered-3d-world-generator.md`,
ez a riport.

## Tesztek és eredmények (mind lefuttatva, Windows, Debug)

| Csomag | Eredmény |
|---|---|
| `layer_clearance_test` (shared) | **62/62 PASS** |
| – független orákulum (körlap-távolság, háromszög-vágás) | 310 000 kapszulaközép, **0** sértés, legközelebbi akadály 0.400 m |
| – negatív kontroll: alulméretezett sugárral sütött rács | orákulum elkapja (1825 érintés) |
| – negatív kontroll: alulméretezett magassággal | orákulum elkapja (16 212 sértés) |
| `layer_ground_support_test` | 77/77 PASS (3D-4A változatlan) |
| `terrain_surface_contract_test` | 31/31 PASS |
| `layered_world_geometry_test` | 46/46 PASS |
| compat / contract / package / spatial key / generator | 9 / 9 / 10 / 7 / 6 PASS |
| `SceneLayerAuthoringTest` (engine) | **95/95 PASS** (71 régi + 24 új) |
| – **valódi Jolt-orákulum** (`PhysicsWorld::OverlapCapsule`) | 36 378 minta (ebből 2052 folyosó), **0** ütközés |
| – Jolt negatív kontroll (r = 0.1 sütés) | **668** ütközést talál |
| `SceneLayerGroundTest` | 64/64 PASS |
| `SceneWorldPackageTest` | 71/71 PASS |
| `worldbench --mode layerclearance` (új) | **24/24 PASS** |
| `worldbench` layersupport / layerlookup | 31/31, 26/26 PASS |
| `worldbench` worldpackage / terrain / mapsplit | 140 PASS + 1 korábbi SKIP (symlink-jog) / 20 / 21 PASS |

A Jolt-orákulum jelenete: padló, nem opt-in fal, 0.5 rad-dal elforgatott
fal, alacsony (2.3–2.7 m) és magas (2.9–3.3 m) gerenda, 1 cm-es szőnyeg,
gömb- és kapszula-collider, trigger, dinamikus láda, 4 fokos lépcső
(0.75 m fok, 0.25 m emelkedés) és pihenő. A kapszula a szerződés sávját
fedi le: [sík + 2 cm, sík + 1.8 m]. Élő-ellenőrzés: ismert ütközéseket
(fal a sugáron belül, alacsony gerenda, forgatott fal, gömb, kapszula)
jelez, ismert szabad helyeket (magas gerenda alatt, szőnyegen) nem; a
trigger csak `hitTriggers=true` mellett ütközik.

Szerver-bench: analitikus padló + fal + lépcső + pihenő → szigorú writer →
Full/Strict loader → valódi `WorldRuntime` (Start nélkül). Ellenőrzi: v4
sidecar bájtra az enkódolt világ; a betöltött metaadat bit-azonos; falon
át `blocked` és az állapot változatlan; fal mellé állás `blocked`, míg a
support-only lekérdezés ugyanott `ok`; lépcső 0.2 m-es lépésekkel 5
portálon át z = 2-ig; lépcső átugrása `transition_required`; nagyobb actor
`actor_not_covered`; manipulált sugár/lépés/NaN profil betöltéskor
elutasítva; support-only csomag v3 marad és actor-lekérdezésre
`no_clearance_proof`; szintetikus világ `not_available`; nincs production
entitás/presence.

## Valódi editor GUI-próba (Computer Use)

Frissen buildelt Debug editor, izolált projekt:
`Client/build/layer-clearance-20260930/gui-project` (saját jelenet, sík
48×32 m terep, a fenti elrendezés trigger/gömb/kapszula nélkül). Nincs
felhasználói scene/asset felülírás; a checked-in `test_zone` érintetlen.

**Generate layers from collision** → „Generated 6 volumes … Clearance
(capsule r=0.35 h=1.8): 416 of 4432 cells blocked by 9 static colliders +
terrain. Proven step/ramp portals: 5 (edges above step height: 0).”
Trace: `cells=4432 blocked=416 obstruction_sources=9 mesh_triangles=108
terrain_quads=446 portals=5 corridor_cells=300 corridor_blocked=72`.
**Show layer volumes**: a fal és az alacsony gerenda körül blokkolt cellák,
a lépcsőnél portál-élek, a szonda kapszula-tengelye látszik.

| Szonda-művelet (volume, X/Z) | Eredmény | Engine X/Y/Z |
|---|---|---|
| Place 1, 0/0 | `ok` | 0 / 1 / 0 |
| Move 1, 4/0 (falon át) | `blocked` | 0 / 1 / 0 marad |
| Move 1, 0/−4 → 4/−4 → 6/0 (fal megkerülése) | `ok` ×3 | 6 / 1 / 0 |
| Move 6, 12/0 (lépcső átugrása) | `transition_required` | 6 / 1 / 0 marad |
| Move 2, 8.3/0 | `ok via proven portal 1` | 8.3 / 1.25 / 0 |
| Move 3, 9.1/0 | `ok via proven portal 2` | 9.1 / 1.5 / 0 |
| Move 4, 9.9/0 | `ok via proven portal 3` | 9.9 / 1.75 / 0 |
| Move 5, 10.6/0 | `ok via proven portal 4` | 10.6 / 2 / 0 |
| Move 6, 12/0 | `ok via proven portal 5` | 12 / 2 / 0 |
| Place 1, 1.6/0 (fal mellett) | `blocked` | előző állapot marad |
| Place 1, −3/4 (alacsony gerenda alatt) | `blocked` | előző állapot marad |
| Place 1, −3/6 (magas gerenda alatt) | `ok` | −3 / 1 / 6 |
| Export strict server world | `exported=yes files=5` | új csomag |
| Natív X-bezárás | exit **0** | nincs kényszerített kill |

Exportált csomag: `gui-project/clearance.server-worlds/export-17907951171530850`,
`layered_world.mx3d` **1844 byte, MX3D v4**. Validálás:
`SceneLayerGroundTest --validate-package <dir>` → **20/20 PASS**
(Full/Eager/Strict betöltés, 6/6 support, profil = editor-profil, 6/6
volume-nak van rácsa, blokkolt cella az actort elutasítja, 5/5 bizonyított
portál átjárható). A Codex-féle korábbi v3 GUI-csomag a bővített
validátoron is 10/10 PASS (visszafelé kompatibilis).

Naplók: `Client/build/layer-clearance-20260930/engine-gui*.log`,
`gui-package-validation.log`. Az `editor_layout.ini`-t az indítás előtti
bájtokra állítottam vissza (git blob `a75e473…`, `git status` tiszta).
A stderr **30 Vulkan validation error** sort tartalmaz – ugyanazt a 30-at,
mint a Codex-futás (SceneView.Color/DepthSnapshot layout, presentable image);
nem ettől a munkától származik, és nem renderer-PASS.

## Hatókör-bővítések (review-ra jelölve)

1. **MX3D v4** sidecar-formátum (v3 bájtra változatlan marad support-only
   világnál).
2. Új `GroundSupportStatus` értékek (`blocked`, `no_clearance_proof`,
   `actor_not_covered`) és új `LayerGroundResult::portal_id` mező.
3. `SceneLayerGround` konstruktor opcionális actor-paramétert kap.
4. **Viselkedésváltozás:** a *Generate layers from collision* most
   **fail-closed**, ha a terep magasságrácsa hiányzik vagy egy statikus
   collider geometriája nem reprodukálható (pl. nem opt-in Mesh collider
   betöltetlen modellel). Korábban ilyenkor a generálás sikerült.
5. Az editor-szonda actor-alapú lett (a falnál megáll, portálon átmegy).
6. Editor-vizualizáció (blokkolt cellák, portál-élek, szonda).
7. `WorldRuntime::PlaceLayerActor/MoveLayerActor` (csak olvasó),
   `worldbench --mode layerclearance`, a `--validate-package` clearance-
   szakasza.

## Korlátok (ismertek, nem hibák)

- **Csak statikus világ.** Dinamikus/kinematikus test, trigger és
  CharacterController nincs sütve (a teszt ezt rögzíti).
- **Konzervatív.** Négyzetes tágítás (átlósan ≤ r(√2−1) ≈ 0.145 m többlet);
  gömb/kapszula/convex hull AABB + 5 cm; a Foliage réteget akadálynak
  vesszük, bár az alap ütközési mátrixban a Player átmegy rajta.
- **Portál csak pontosan érintkező, tengelyigazított supportok között.**
  Elforgatott rámpa-primitív nem érintkezik float-pontosan → nincs proof.
  Lift/ajtó/teleport továbbra is metadata.
- **Fokmélység:** ~0.6 m (r + cella) alatti fokon nincs álló cella; a
  folyosó fedi a fellépési zónát.
- **Terep ↔ volume átmenet nincs** (a terep nem volume).
- Egy mozgás legfeljebb egy portált léphet át; a szerver-tick mozgást
  ennek megfelelően kell darabolni (3D-5).
- Egy actor-osztály sütésenként; kisebb actor konzervatívan lefedett.
- A lépésfolyosóban jelentett magasság a kért volume síkja; a fizikai
  kontroller lépés közben legfeljebb `max_step_m`-rel térhet el.
- A sidecarnak nincs CRC-je (korábbi modell, csak a chunkoknak van): egy
  átfordított rácsbit érvényes, de más világként töltődik be.
- „proof header … truncated” hibaüzenet `LAYERED_WORLD_HEADER` kódot kap
  (a meglévő support-header konvenciót követi).
- Sütési komplexitás O(volume × háromszög), nincs térbeli index; sütési
  időt és memóriát **nem mértünk**. A GUI-ban a generálás azonnalinak tűnt.
- A meglévő vonal-renderer keveri a színeket (piros/zöld a magenta
  fallback anyagon halvány).
- Csak Windows/MSVC Debug futott; Linux/FreeBSD és sanitizer **nem futott**.

## Újrafuttatás

```powershell
cmake --build Client/build --config Debug --target SceneLayerAuthoringTest SceneLayerGroundTest SceneWorldPackageTest IxtreemeEngine
cmake --build gameserver/build/windows-debug --config Debug --target worldbench gameserver layer_clearance_test
& ./gameserver/build/windows-debug/shared_map_build/Debug/layer_clearance_test.exe
& ./Client/build/libs/render/Debug/SceneLayerAuthoringTest.exe
& ./gameserver/build/windows-debug/apps/gameserver/Debug/worldbench.exe --mode layerclearance
& ./Client/build/libs/render/Debug/SceneLayerGroundTest.exe --validate-package ./Client/build/layer-clearance-20260930/gui-project/clearance.server-worlds/export-17907951171530850
```

Nem kell szolgáltatás, hitelesítő adat vagy adatbázis.

## Következő lépés

**3D-5**: autoritatív entitás-volume tulajdonlás (spawn/admission explicit
volume-mal), a szerver-tick mozgás a `MoveLayerActor`-on keresztül
(portálonként darabolva), migráció, `LayeredSpatialCellKey` a production
spatial indexben/AOI/ghost útvonalon, replikáció és kliensoldal. Utána
összekötött engine-tesztprojekt valódi login- és gameserverrel, két
klienssel. Nyitott korábbi tételek: I1 G5 (X-bezárás cleanup) / G6
(reconnect), a 30 Vulkan validation error, MAP-4 terheléses korlátok.
