# Rétegezett 3D világ: terrain-szerződés és teljes szervercsomag-export

Dátum: 2026-09-30. Branch: `codex/engine-server-integration`.

## Eredmény és hatókör

A motor editorának aktuális jelenetéből teljes, Full/Eager/Strict szerveres
világcsomag készíthető. A csomag a valós terrainmagasságot, blocked cellákat,
explicit terrain-spawnt, deklarált vizet és az előző mérföldkő collisionből
generált rétegmetaadatát együtt tartalmazza. Új célkönyvtárba kerül, csak
sikeres strict ellenőrzés után válik végleges csomaggá.

A nem síkbeli terraincellák új magasságszerződése a motor Jolt colliderének
háromszögfelosztását követi. A motor renderelt terrainfelülete és CPU-s
magasságlekérdezése ugyanezt használja. A régi height v1/v2 csomagok bilineáris
jelentése megmarad; az új jelentést kötelező height v3 réteg jelöli.

Ez a terrain- és csomag-mérföldkő. A generált magasabb szinteken authoritative
játékos-/mobmozgás, volume ownership, portalváltás és rétegenkénti AOI még
nincs aktiválva. A modellcolliderek teljes geometriája és renderassetek nem
kerülnek a szervercsomagba. A spawn továbbra is a terrainen van.

A felhasználó későbbi igényét külön következő integrációként rögzítettük:
olyan motorprojekt kell, amely a valódi loginserverhez és gameserverhez
kapcsolódik, és két klienssel ellenőrzi a teljes utat. Ebben a munkában
szolgáltatáscsere, DB-módosítás és élő admissionteszt nem történt.

## Baseline és megőrzés

A mérföldkő kezdetén a working tree tiszta volt:

| Referencia | Commit |
| --- | --- |
| Elsődleges authoring baseline | `c118b2fa7e3c346bd480e2d7eabe4b250e44a9cb` |
| Motor/szerver integráció baseline | `bf11861bb8eabf831c69ec8edf60273b7e3cdc3d` |
| Motor forrásbranch: main | `56df1c16e06a34cc939a296bea284c1ecc0c7986` |
| Szerver forrásbranch: With_Auriga | `19d664f9f2b6e1baecc2d74f547ee2008ee42530` |

Az előző bizonyítékcsomagok és a checked `test_zone` megmaradtak. Az új GUI
fixture és a futási naplók az ignored
`D:/IxtreemeWorld/Client/build/layer-terrain-package-20260930/` könyvtárban vannak.
Az editor teszt közben létrehozott layout-változásait a futás előtti pontos
mentésből visszaállítottuk; nem részei a mérföldkőnek. Nem volt reset, stash
vagy push. A lezárt kód a korábban engedélyezett helyi mérföldkőcommitba kerül.

## Bizonyított felületeltérés és javítás

Az audit három külön felületet talált a korábbi forrásban:

1. A Jolt terrain collider source-rácsban a `v10–v01` átlót használta.
2. A terrain renderindexek a másik, `v00–v11` átlót használták.
3. A motor CPU-lekérdezése és a legacy szerverlekérdezés bilineáris volt.

Ezek csak sík cellán egyeznek. A canonical szervercellában SW=0, SE=4,
NW=0, NE=0 méteres sarokmagasságnál középen a fizikai felület 0 m,
a bilineáris 1 m, a korábbi renderátló 2 m. Ez forrásból és független
matematikai oracle-ből rekonstruált baseline, nem a régi binárison mért
teljes motoros benchmark.

Az új teszt egy másik, nem síkbeli cellán tényleges Jolt raycasttal is
megkülönbözteti a felületeket: a source NE=4 m sarok esetén a fizikai
középmagasság 2 m, a legacy bilineáris 1 m, a másik átló 0 m.
Az új szerver és a motor CPU-lekérdezése a 2 m-es fizikai eredményt adja.

Az engine koordinátája `(x, Y-up, z)` méterben; a szerveré `(x, y=z, z=Y-up)`.
A source terrain középre helyezett és a második rácstengelyen északról délre
tárol: `(i*cell-width/2, Hcm/100, depth/2-j*cell)`. Exportkor:

- vertex-height source sor: `cellsZ - serverY`;
- cell-attribute source sor: `cellsZ - 1 - serverY`;
- szerver origin: `(-width/2, -depth/2)`;
- a fizikai átló canonical szervercellában `h00(SW)–h11(NE)`.

Az új képlet `fx >= fy` esetén
`h00 + (h10-h00)*fx + (h11-h10)*fy`, különben
`h00 + (h11-h01)*fx + (h01-h00)*fy`. A shared
`InterpolateTerrainHeight` használja ezt, a legacy ág eredeti bilineáris
aritmetikája változatlan. A motor source-sorokhoz igazított megfelelőjét
`SampleTerrainCollisionHeightCm` adja; a renderindexek és debug-overlay
is a fizikai átlót használják. A sculpt flatten célmagasságának bilineáris
mintavételezését ez a javítás nem változtatja meg.

Fractional cellán az explicit authored pitch visszaosztása a float
szélességből más rácstávolságot adhatott. A `CreateTerrainCollider` most
megtartja az explicit cellSize-t, ha a descriptor mérete pontosan az
authored float `cells*cellSize` szorzat. Az eltérő legacy descriptorok
korábbi width/cells és depth/cells útja megmarad, negatív/regressziós
kontrollal lefedve.

## Verzió, pontosság és kompatibilitás

A manifest marad v3, a chunk konténer marad v2. Az új terrain jelentését a
külön height layer v3 jelöli: int32 nyers minta, `unit_m=0.0001`,
`offset_m=0`, `TriangleMainDiagonal`. Az új schema enum és mező appendelt;
a hiányzó mező legacy defaultja `Bilinear`.

Height v3 csak explicit triangle encodinggal és required rétegként
elfogadott. Height v2 alatt nem engedélyezett triangle mód; ismeretlen
mód, hiányzó encoding és nem required v3 elutasított. Így egy régi,
legfeljebb height v2-t értő szerver elutasítja az új required réteget,
nem olvassa ugyanazt a terrainet csendben bilineáris felületként.
Height v1/v2, régi manifest v2, clone és streaming-plan regresszió PASS.

Az int32 quantization a vertex magasságára legfeljebb 0.00005 m eltérést
jelent. Nem véges vagy int32 tartományon kívüli input hibát ad; nincs clamp.
Ez a bound nem teljes térbeli collisiongarancia: a motor/Jolt float
vertexpozíciói és a szerver double rácsa külön kerekítési eltérést hordoz.
Az exporter ezt külön `maxGridPositionErrorMeters` értékkel jelenti.

| Mért maximum, 13×5 cella, 0.1 m pitch | Méter |
| --- | ---: |
| Vertex-height quantization bound | 0.0000500000 |
| Engine float / server double vertexpozíció | 0.0000000521541 |
| CPU-kliens / független oracle | 0.000000139569 |
| Szerver / független oracle | 0.0000211043 |
| Tényleges Jolt raycast / független oracle | 0.000000686949 |
| Szerver / tényleges Jolt raycast | 0.0000216961 |

Külön nagy koordinátás aritmetikai auditban 10 000 cella és float 12.1 m
pitch esetén az engine east-edge 60 500 m, a szerveré 60 500.003814697266 m,
a maximum X vertexkülönbség 0.00390625 m. Ez csak aritmetikai audit,
nem ekkora világ teljes exportja, nem Jolt- vagy populációbenchmark.
Nagy térképen és meredek felületen a további runtime collisionmunka előtt
külön helykoordináta/pontossági szerződés és mérés szükséges.

## Export és scene-persistence

Az editorban: **Tools → Layered world**, explicit ASCII World ID
(1–64 betű/szám/`_`/`.`/`-`), authored Player spawn X/Z méterben,
majd **Export strict server world**. A jelenet legyen mentett, és legyen
teljes valós terrainrácsa. A handler az ugyanabban a frame-ben feldolgozott
editorváltoztatások utáni friss scene-snapshotból és a valódi CPU-s
physics mesh providerből dolgozik. Play mód alatt a parancs nem fut.

Az exporter ellenőrzi a méreteket, cell/pitch/chunk limiteket, pontos
sampleszámot, finite és int32 magasságot, a blocked biten kívüli reserved
attribute biteket és az explicit spawnt. Legfeljebb 16 millió terrainvertex
exportálható. Hiányzó sample nem lesz mesterséges nulla magasság.
Blocked/outside spawn elutasított; nincs automatikus keresés/áthelyezés.
Fractional cellaseam döntését az authoritative `ServerTerrain::Cell`
adja, nem az exporter külön közelítő floor számítása.

Az exporter a meglévő réteggenerátort csak tényleges bekapcsolt collision-
vagy vízforrásnál használja. Üres forrásból nincs kitalált sidecar.
A víz nélküli világ explicit `WaterModel::None`; a nem teljes téglalapmaszk
nem válik teljes water bboxszá. A korábbi exact footprint korlátok megmaradnak.

Az exporter kizárólag friss final és `.pending` útvonalat fogad el.
Exkluzívan létrehozott stagingbe ír, Full/Eager/Strict szerverbetöltővel
ellenőriz, és a pontos spawn Cell/Height eredményét is vizsgálja.
Windows alatt native `MoveFileExW` replacement flag nélkül publikál.
Sikertelen export csak a saját stagingjét takarítja; előzetes final,
partial, fájl és staging megmarad. Konkurrens exportnak egy győztese van.
Linux noreplace publikálási ág készült, de ezen a platformon nem futott teszt.

A scene mentése eddig int16 centiméterre kerekített/clampelt legacy
chunkokat használt; újraolvasás után az export nem ugyanazt a sculptolt
felületet kapta volna. Most a scene a legacy chunkok mellett a meglévő
float-centiméteres codec segítségével `<scene>_terrain_exact.height`
sidecart és `exact_heightmap_ref` deklarációt ment. Az attributes tényleges
értékei is megmaradnak a chunkokban, nem mind nulla.

A deklarált exact sidecar vagy chunkset hiánya hibát ad. Hibás header count,
dimenzió vagy filehossz a vector-allocation előtt elutasított. A payload
nonfinite float mintája a korlátozott allocation és read után, a jelenlegi
terrain lecserélése előtt ad hibát; nincs visszaesés a lekerekített magasságra.
Régi, exact deklaráció
nélküli scene megőrzi legacy betöltési útját. A codec korábbi 100 milliós
sample felső határa megmaradt; ez nem azonos az exporter szigorúbb 16 milliós
korlátjával. Sikertelen fájlzárás/írás nem jelöli tisztának a scene-t.

Unicode csomagútvonalon a strict loader a filename `.string()` diagnosztikai
konverzióján dobott INTERNAL900 hibát. Ezt az első tesztfutás reprodukálta;
a native path I/O megőrzése mellett UTF-8 diagnosztikai konverzió javította.
Unicode export, teljes betöltés, overwrite-elutasítás és hiányzó root
kontroll azóta PASS. Az eredeti hibás futás naplója megmaradt.

## Build és célzott/regressziós tesztek

Windows x64, Visual Studio 18 2026, Debug, static CRT, Jolt enabled.
A módosított shared library, gameserver, worldbench és engine build sikeres.
A shared loader változatlan constructorhívásánál a layer version
uint32→uint16 konverziójára C4244 warning maradt; a strict loaderben a
támogatott verzió előzetes ellenőrzése megmarad. A build nem warnings-as-errors
konfigurációban futott.

| Teszt | PASS | FAIL | SKIP |
| --- | ---: | ---: | ---: |
| SceneWorldPackageTest | 71 | 0 | 0 |
| SceneLayerAuthoringTest | 71 | 0 | 0 |
| terrain_surface_contract_test | 31 | 0 | 0 |
| layered_world_geometry_test | 46 | 0 | 0 |
| layered_world_contract_test | 9 | 0 | 0 |
| layered_world_package_test | 10 | 0 | 0 |
| layered_world_generator_test | 6 | 0 | 0 |
| layered_spatial_key_test | 7 | 0 | 0 |
| engine_server_map_compatibility_test | 9 | 0 | 0 |
| worldbench worldpackage | 140 | 0 | 1 |
| worldbench terrain | 20 | 0 | 0 |
| worldbench layerlookup | 26 | 0 | 0 |
| Összes CPU-ellenőrzés | **446** | **0** | **1** |

A skip a Windows symlink-jogosultsági kontroll; nem elrejtett failure.
Az új tesztek valódi Jolt raycastot, független triangle oracle-t, asymmetric
nonsquare részchunkokat, cell/vertex külön sorfordítást, vízmélységet,
legacy verziót, malformed/NaN inputot, spawnseamet, konkurrens publikálást,
létező adat megőrzését és a valódi SceneManager save/reload útját fedik le.

A meleg OS-cache-s worldpackage regresszió 20 futásos kiegészítő baseline-ja:
legacy loader 344.09 ms / 3 099 154 B resident; strict startup 14.74 ms /
1 008 032 B resident; strict full 14.34 ms. Ez a meglévő test_zone loaderének
regressziós kimenete; nem kontrollált teljesítmény-acceptance, és nem a mostani
javítás előtte/utána gyorsulásának bizonyítéka.

Reprodukáló parancsok a repository gyökeréből:

```powershell
cmake --build Client/build --config Debug --target IxtreemeEngine SceneWorldPackageTest SceneLayerAuthoringTest --parallel 4
cmake --build gameserver/build/windows-debug --config Debug --target gameserver worldbench terrain_surface_contract_test layered_world_geometry_test layered_world_contract_test layered_world_package_test layered_world_generator_test layered_spatial_key_test engine_server_map_compatibility_test --parallel 4
& ./Client/build/libs/render/Debug/SceneWorldPackageTest.exe
& ./Client/build/libs/render/Debug/SceneLayerAuthoringTest.exe
& ./gameserver/build/windows-debug/shared_map_build/Debug/terrain_surface_contract_test.exe
& ./gameserver/build/windows-debug/apps/gameserver/Debug/worldbench.exe --mode worldpackage
& ./gameserver/build/windows-debug/apps/gameserver/Debug/worldbench.exe --mode terrain
& ./gameserver/build/windows-debug/apps/gameserver/Debug/worldbench.exe --mode layerlookup
```

A futási naplók a fent megadott evidence könyvtárban vannak: `scene-world-package.log`,
`terrain_surface_contract_test.log`, `scene-layer-authoring.log`, a shared
tesztnevekhez tartozó `.log` fájlok, `worldpackage.log`, `terrain.log`,
`layerlookup.log`. Az első Unicode failure külön
`scene-world-package-initial-failure.log` alatt maradt meg.

## Tényleges GUI-export és motoros korlátok

A GUI-t a saját buildű engine ablakán, a computer-use skill útján teszteltük.
Working directory: `D:/IxtreemeWorld/Client`. Izolált projekt:
`Client/build/layer-terrain-package-20260930/gui-project/project.ixproj`.
A fixture 64×32 cella, 1 m pitch, 32 cellás chunk, source magasság
`-150.125 + 0.625*column + 1.125*row` centiméter. Három builtin Cube
collision plate engine 0/6/-6 m szinten, road/bridge/underpass tagekkel,
Box/Mesh/Box colliderrel. A renderanyag nélküli tesztplate magenta;
ezt nem minősítettük kész renderassetnek.

A menüből három friss export készült, és külön CLI a publikált könyvtárakat
ismét Full/Strict módban betöltötte: `world=editor-world`, origin `-32,-16`,
méret `64×32`, height layer `3`, `2` chunk. Mindhárom PASS; a három export
mind az öt fájlja byte-azonos. Könyvtárak:

- `terrain-stacked.server-worlds/export-17907861414238849`;
- `terrain-stacked.server-worlds/export-17907868951400998`;
- `terrain-stacked.server-worlds/export-17907873353903724` (végső guardos build).

| Fájl | Byte | SHA-256 |
| --- | ---: | --- |
| map.manifest | 496 | `B7FA385A044B101C50416C182F5675958845D72533A170AD600AD9B8C549D38D` |
| layered_world.mx3d | 168 | `FDD25F8FAB215438BB854668C301E78ABC92D7A1006C659379BB10A0F30B204D` |
| worldlogic.dat | 44 | `37A8FF75D90D9205E536FC3A248A63D0D40DA69071848CCC0E3589A7A9813BA0` |
| chunks/chunk_0_0.mxchunk | 6442 | `EAEB8A72B1EFFE7F7EE6F867569A8292D4948A942191B60647AD5D0FF532296E` |
| chunks/chunk_1_0.mxchunk | 6442 | `0236A811E01BC5F54B7AC9D842419A0B908A81600AC0ACEA8FC495A55C5479B8` |

A GUI-kiegészítő ellenőrzések nem növelik a 446 CPU-check számlálóját.
Az export elfogadása külön eredmény a renderer és az ablakbezárás állapotától.

Az első normál X-close a device shutdown után retained water snapshot
referenciák miatt hét Vulkan objektumot hagyott életben, majd dead-device
`vkDestroySampler` abortot jelzett. A `TerrainRenderer::Destroy` most a
shared refraction image/depth/sampler referenciákat is elengedi, amíg a
device él. Az ownership chain forrásból igazolt; baseline binárisos
runtime reprodukció nem történt, ezért nem állítjuk mérés nélkül, hogy
a hiba már a c118b2f GUI-futásában is jelentkezett.

A következő GUI-close más hibát mutatott: `vkCreateWin32SurfaceKHR` null HWND
és utána invalid swapchain paraméterek, exit `-1073741819`. A Win32
`WM_CLOSE → DestroyWindow → WM_DESTROY → m_hwnd=nullptr/PostQuitMessage`
út után a `PumpMessages` false-t adott, de a RunGame loop még végrehajtott
egy teljes RHI/ImGui frame-et. A minimális javítás a közvetlen `if (!running)
break` a PumpMessages után, a meglévő shutdown megtartásával.
A végső guardos builddel ugyanaz a projekt betöltése és menüexportja után
native X-close történt: **exit=0**, nincs leaked-object, abort vagy null-HWND
jelzés. Az export öt fájlja az előző futásokkal byte-azonos. A két célzott
bezárási javítás együttes futási eredménye PASS; a két javítás külön
teljes factorial izolációja nem történt.

Bizonyíték: `engine-gui.stderr.log` (első abort),
`engine-gui-after.stderr.log` (guard előtti null HWND),
`engine-gui-final.stderr.log` és `.exit.txt` (javított út),
`gui-close-final-validation.json`, `gui-package-final-validation.log`,
`gui-package-final-files.json`. A végső napló 20 Vulkan validációs hibaüzenetet
továbbra is tartalmaz; a duplicate-message limit miatt ez nem összes hibás
frame-et számláló metrika. Ezeket a sikeres process shutdown nem rejti el.

Más Vulkan validációs jelzések is előfordultak export előtt és után:
offscreen color layout `oldLayout-01197`, depth snapshot usage/layout
`01210`, illetve swapchain acquisition/submit. Az érintett backend/layout
függvények ebben a mérföldkőben nem változtak, de ez önmagában nem bizonyít
régi binárison futtatott regressziómentességet. A teljes renderer correctness
nem PASS. A későbbi teljes motoros E2E előtt ezekhez célzott audit/javítás
kell; nem történt renderer-architektúra-refaktor.

## Változtatott fájlok és fontos belépési pontok

| Fájl | Viselkedés / függvény |
| --- | --- |
| Client/libs/render/SceneWorldPackage.h/.cpp | `ExportSceneServerWorld`, előellenőrzés, encoding, strict staging/publication |
| Client/libs/render/tests/SceneWorldPackageTest.cpp | 71 correctness/negatív kontroll; `--validate-package` |
| Client/apps/client/src/EngineApplication.cpp | `MergeMapEditorCommands`, friss snapshot export; PumpMessages close guard |
| Client/libs/render/editor_panels/EditorImGuiMenuToolbarPanels.inl | World ID, terrain spawn X/Z, strict export menü |
| Client/libs/render/EditorImGui.h | Export mezők editorállapota |
| Client/libs/render/MapEditorTypes.h | Terrain attributes, exact ref, export parancs |
| Client/libs/render/TerrainRenderer.h/.cpp | Fizikai triangle sampler/index, attribútumok, invalid grid reject, explicit cleanup |
| Client/libs/physics/PhysicsWorld.cpp | `CreateTerrainCollider`, authored cell pitch |
| Client/libs/render/SceneManager.cpp | `Read/WriteTerrainHeightmap`, `Read/WriteTerrainChunkSet`, valódi save/load |
| Client/libs/render/CMakeLists.txt | Export helper és célzott teszt target |
| shared/map/include/map/ServerTerrain.h; src/ServerTerrain.cpp | Encoding enum és `InterpolateTerrainHeight`, terrain Height |
| shared/map/schema/world_package_manifest.capnp | Appendelt interpolation schema |
| shared/map/include/map/WorldPackage.h; src/WorldPackage.cpp | Height max v3, strict verzió/módőrök, Unicode diagnosztika |
| shared/map/include/map/WorldPackageWriter.h; src/WorldPackageWriter.cpp | Explicit encodingból required height v3 írás |
| shared/map/tools/terrain_surface_contract_test.cpp; CMakeLists.txt | 31 shared surface/version/Unicode kontroll |
| gameserver/apps/gameserver/src/bench/MapPackageBench.cpp | Unsupported-version kontroll `max+1`, szigor megőrizve |
| docs/map-data-format.md; layered-3d-world-generator.md | Új szerződés, használat és korlátok |

## Nem futtatott vizsgálatok és következő munka

Nem futott Release/Linux/Android build, 500 player/200 000 mob streaming
benchmark, nagy térképes teljes collider-export vagy élő login/game admission.
A legacy test_zone és éles szolgáltatások nem cserélődtek. Nincs új gameplay,
Unity-fejlesztés, scheduler/ASF-átépítés vagy auth-bypass.

A következő rétegezett runtime-mérföldkőnél entityhez authoritative volume/layer
ownership kell, bizonyított support surface és folytonos átmenet, valamint
irányított portal/stair kapcsolat. A zónahatár, AOI és migráció csak az ehhez
rögzített szerződés alapján kapcsolható át. A mostani terraincsomag nem
állítja, hogy a rétegmetaadat teljes modelcollisiont helyettesít.

A későbbi kapcsolt motorprojekt minimális integrációs terve:

1. Auditálni a mainből átvett engine tényleges hálózati illesztési pontjait;
   a StandaloneVulkanClear korábban bizonyított I1 admission/session útját
   a megfelelő közös komponensekkel átvenni. A jelenlegi engine kész
   kapcsolata nem feltételezhető.
2. Külön projekt/profil, explicit local login/game endpointok és valódi
   tesztidentitások; nincs beépített jelszó/token, univerzális credential
   vagy auth-megkerülés.
3. Ugyanaz az authored scene és SHA-val azonos strict szervercsomag legyen
   az izolált fixture. A jelenlegi engine scene-t renderel; a szerver
   manifest v3-at az engine legacy package reader nem olvassa automatikusan.
   Ez a külön kompatibilitási kontroll ma helyesen elutasítja az ismeretlen
   sémát. Az asset/projekt illesztést explicit meg kell tervezni.
4. Két valódi kliens: login/admission, átfedő presence, saját/távoli
   spawn/move/stop, correction, AOI, disconnect/reconnect és szerveroldali
   zónamigráció külön bizonyítékokkal. Magasság/layer replikáció csak a
   szerver authoritative ownership szerződésével egyezően kapcsolható be.
5. A renderer fennmaradt Vulkan correctness problémáit célzottan rendezni,
   és a fizikai support/koordináta pontosságot a tényleges futási útban mérni.
   Ezt követheti a teljes 3D rétegváltás E2E-elfogadása.

Most az export/terrain-mérföldkő lezárása és review következik. Az élő
kapcsolt projekt a felhasználó „majd” kérésének megfelelően későbbi munka.
