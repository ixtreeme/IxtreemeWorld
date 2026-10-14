# Main motor + With_Auriga szerver integráció

Dátum: 2026-09-30. Új branch: `codex/engine-server-integration`.

## Források és eredet

| Rész | Forrás |
| --- | --- |
| Motor, editor, renderer, motor assetek (`Client`) | `main`, `56df1c16e06a34cc939a296bea284c1ecc0c7986` |
| Gameserver, loginserver, hálózati protokoll | `With_Auriga`, `19d664f9f2b6e1baecc2d74f547ee2008ee42530` |
| Szerver terrain/water/package könyvtár és rétegezett világ | Ugyanez a `With_Auriga` állapot |
| Korábbi szerverfeladatok dokumentációja és bizonyítékai | A `With_Auriga` verziók; a csak mainen létező dokumentumok is megmaradnak |

Az integráció a mainből indul. A `15b1c1454fed32dc657ccdcfdd9c0f461b0a45ab`
két szülős merge rögzíti a fenti két pontos forráscommitot. A kiválasztott
könyvtárak átvétele után külön commit rögzíti az alábbi kompatibilitási illesztést.
A main és With_Auriga referenciája változatlan marad. Nincs history-átírás vagy push.

Az eredeti working tree az új branch létrehozásakor tiszta volt. A korábbi
mérföldkövek és a rétegezett világ munkája a With_Auriga történetéből átkerültek.
A külön `StandaloneVulkanClear` projektet ez a művelet nem módosítja.

## Könyvtárválasztás

- A `Client` motorforrása és eredeti assetjei a main változatával azonosak.
  Egyetlen kiegészítés a With_Auriga becsekkolt `Client/assets/Maps/test_zone`
  fixture-jének 21 fájlja; ez a mainen nem létezett. A fixture tartalma változatlan.
- A `gameserver`, `loginserver` és `shared/protocol` a With_Auriga állapotát kapja.
  A gameserver három benchmark-fájljában csak a map-séma include/namespace változik.
  A loginserver és a protokoll fájljai a forrásbranch megfelelő fájljaival azonosak.
- A `shared/map` motorbetöltője (`MapData`) a main változata, a szerver package,
  terrain, water és layered world implementációi a With_Auriga változatai.
- A szerver `.gitattributes` shell-fájlokra előírt LF szabálya átkerül.
  A main projektállapot-naplója megmarad; a szerverág naplója a forráscommitból elérhető.
- A `docs/map-data-format.md` a szerver formátumszerződése. A main eredeti leírását
  a `docs/engine-map-data-format.md` külön megőrzi.

## Szükséges kompatibilitási módosítás

A két ág ugyanazzal a Cap'n Proto fájlazonosítóval és C++ névtérrel használta
a `map_manifest.capnp` sémát, de eltérő jelentéssel:

- main: a MapManifest `@12`/`@13` mezője Float32 triplanar küszöb/átmenet;
- With_Auriga: `@12` a világ Y mérete, `@13` az origin, `@14`–`@18`
  chunkgrid/layers/chunks/height encoding/water adatok.

Ezek egyszerű összeolvasztása adatértelmezési hibát okozna. Az integráció
külön generált típuskészletet tart fenn, a mezők bináris elrendezésének módosítása nélkül:

| Séma | Azonosító | C++ névtér | Használat |
| --- | --- | --- | --- |
| `schema/map_manifest.capnp` | `0xd8a46e2b8a831f55` | `mx::map::schema` | Változatlan main motorbetöltő és editor |
| `schema/world_package_manifest.capnp` | `0xfbd866c308bffdf9` | `mx::map::package_schema` | Szerver package betöltő, író és fixture-ek |

A szerver új sémafájlja a With_Auriga séma mezőit változatlanul őrzi. A friss
azonosító és névtér lehetővé teszi a két generált header együttes használatát.
A meglévő flat-array map.manifest fájlokat nem írjuk át. A Cap'n Proto
reflection típusa megváltozik a szerver kódban; a közvetlen kódhasználók ezért
az új include-ot és névteret használják.

Módosított illesztési pontok:

- `shared/map/CMakeLists.txt`: mindkét séma generálása; a valódi library target
  `IXEngineMapData`, a szerver által használt `mapdata` ennek aliasa.
- `MapData.h/.cpp`: a main összes anyagparamétere és betöltési útja megmarad;
  hozzáadódik a szerver `AreaId`/`Area` aliassa és `Rect::ContainsHalfOpen` metódusa.
  A meglévő zárt `Contains` szemantika változatlan.
- `WorldPackage.cpp`, `WorldPackageWriter.cpp/.h`: a szerver saját sémanévterére vált.
- `HardeningBench.cpp`, `MapPackageBench.cpp`, `MapStreamingBench.cpp`: ugyanaz a
  sémaillesztés, tesztfeltételek vagy timeoutok megváltoztatása nélkül.
- Új `engine_server_map_compatibility_test`: két séma együtt fordítása, main
  anyagparaméterek és alapértékek, szerver v3 mezők, v3 visszautasítása a v2-only
  motorbetöltőben, zárt és félig nyílt határok ellenőrzése.

## Ellenőrzések

Windows / VS 18 / Debug / x64-windows-static környezetben:

| Ellenőrzés | Eredmény |
| --- | --- |
| `IxtreemeEngine`, editor engedélyezve | BUILD PASS |
| `gameserver` és `worldbench` | BUILD PASS |
| `loginserver` | BUILD PASS |
| `engine_server_map_compatibility_test` | 9 PASS, 0 hiba |
| `layered_world_contract_test` | 9 PASS, 0 hiba |
| `layered_world_package_test` | 10 PASS, 0 hiba |
| `layered_spatial_key_test` | 7 PASS, 0 hiba |
| `layered_world_generator_test` | 6 PASS, 0 hiba |
| `worldbench --mode worldpackage` | 140 PASS, 0 hiba, 1 SKIPPED |
| `worldbench --mode terrain` | 20 PASS, 0 hiba |
| Git whitespace ellenőrzés | PASS |

A kihagyott worldpackage-eset a `path-symlink-escape`: ebben a helyi
környezetben a directory symlink létrehozása nem engedélyezett. A corpus
egyéb hibás fixture-jei a várt hibával utasultak vissza. A terrain próbában
szándékos negatív admission esetek hibanaplója is megjelent; az összesített
ellenőrzés 20 sikeres esetet és nulla hibát jelzett.

A build figyelmeztetéseket is adott: például WorldPackage.cpp típuskonverzió,
ReplicationValidator lokális névárnyékolás, WorldRuntime/Flecs elérhetetlen kód,
Lua/sol2 konverzió/UTF-8 és TerrainRenderer nem használt lokális változó.
A build ezekkel együtt sikeres; ez a feladat nem javítja a teljes warning-készletet.

Reprodukció a repository gyökeréből:

```powershell
cmake -S gameserver -B gameserver/build/windows-debug -DENABLE_WORLDBENCH=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build gameserver/build/windows-debug --config Debug --target gameserver worldbench layered_world_contract_test layered_world_package_test layered_spatial_key_test layered_world_generator_test engine_server_map_compatibility_test --parallel 4
cmake -S Client -B Client/build -DIXTREEME_WITH_EDITOR=ON -DBUILD_TESTING=ON
cmake --build Client/build --config Debug --target IxtreemeEngine --parallel 4
cmake -S loginserver -B loginserver/build/windows-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build loginserver/build/windows-debug --config Debug --target loginserver --parallel 4

& ./gameserver/build/windows-debug/shared_map_build/Debug/engine_server_map_compatibility_test.exe
& ./gameserver/build/windows-debug/shared_map_build/Debug/layered_world_contract_test.exe
& ./gameserver/build/windows-debug/shared_map_build/Debug/layered_world_package_test.exe
& ./gameserver/build/windows-debug/shared_map_build/Debug/layered_spatial_key_test.exe
& ./gameserver/build/windows-debug/shared_map_build/Debug/layered_world_generator_test.exe
& ./gameserver/build/windows-debug/apps/gameserver/Debug/worldbench.exe --mode worldpackage --fixtures-out D:/IxtreemeWorld/gameserver/build/windows-debug/engine-server-integration-fixtures
& ./gameserver/build/windows-debug/apps/gameserver/Debug/worldbench.exe --mode terrain
```

A generált fixture-ek külön build könyvtárba kerültek. A checked test_zone
nem lett újragenerálva. A mainhez viszonyított `Client` diff kizárólag a hozzáadott
test_zone; a loginserver és shared/protocol diff a With_Aurigához képest üres.

## A bizonyítás határa és következő lépés

Ez ellenőrzött közös forrás- és buildalap. Nem bizonyít új, valódi
motor–gameserver E2E admissiont vagy közös 3D runtime-ot. Élő szolgáltatást
nem állítottunk le, nem indítottunk újra és DB-műveletet sem végeztünk.
Release/Android build, GUI render smoke és a nagy streaming/performance
acceptance újrafuttatása ebben az integrációs feladatban nem történt meg.

A motor jelenlegi MapData útja továbbra is v2-only; a szerver v3 csomagját
nem kezdi csendben motoradatként értelmezni. Az editor anyagparaméterei nem
kerülnek automatikusan a szerver package írójának texture_palette mezőjébe.
A motor és a szerver koordináta-/magasságszerződését nem változtatjuk meg
ennek az átvételnek a mellékhatásaként.

A rétegezett világ meglévő szerveroldali szerződése, sidecarja, semantic tagjei,
spatial key segédtípusa és authoring generátora átkerült. A modellek tényleges
collision-geometriájából történő automatikus rétegépítés és az editor/runtime
bekötése külön folytatandó feladat. A következő 3D munka most már ezen a
közös branchen vizsgálhatja a main valós editor-/scene-/physics illesztési pontjait.
