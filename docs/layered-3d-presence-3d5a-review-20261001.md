# 3D-5A – szerveroldali autoritatív rétegjelenlét (review, 2026-10-01)

Állapot: **kész, review-ra vár.** Branch: `codex/engine-server-integration`,
alap: `7f39409` (3D-4B).

## Hatókör és bontás

A 3D-5 egyben túl nagy, ezért három szakaszra bontottam:

- **3D-5A (ez):** csak szerver. Autoritatív rétegjelenlét, explicit
  belépés, rétegzett mozgás, (volume, x, y) térbeli index, ghost/border,
  migráció/partíció-transfer, spawn-üzenet mezők, audit.
- **3D-5B:** a volume-váltás jelzése a transform-frame-ekben
  (protokollverzió-emelés) és a kliensek (standalone + engine).
- **3D-5C:** összekötött engine-tesztprojekt valódi login- és
  gameserverrel. (Az engine `Client/` részében ma nincs hálózati kód.)

## Szerződés

- **`LayerPresence {volume_id, layer_id}`** (új flecs komponens,
  `world/components/LayerComponents.h`). Ha hiányzik, az entitás a régi
  terep-entitás. Csak explicit belépés állítja be, és csak bizonyított
  portálátlépés változtatja; soha nem z-ből vagy XY-ból következtetjük ki.
- **z mindig a support-sík magassága.** Rétegzett entitásra nem fut a
  terep-lépésellenőrzés, a terepmagasság és a terep-warp; a migráció és a
  partíció-transfer sem írja felül a z-t terepmagassággal (ez korábban
  minden transfernél megtörtént volna).
- **Mozgás:** `ResolveLayerActorMove` (3D-4B), tengelyenként, legfeljebb
  0,5 m-es darabokban (egy darab legfeljebb egy portált léphet át, így a
  hosszú, alacsony LOD-ú lépés is helyes). Célvolume: az aktuális, ha a
  footprintje tartalmazza a pontot, különben egy bizonyított portál másik
  vége. A volume-rendszer elhagyása tiltott. A célpont float→double pontos,
  így a tárolt pozíció bitre az, amit a clearance ellenőrzött.
- **Belépés:** a debug spawn override új `volumeId` mezője (dev buildek).
  A szerver `ResolveLayerActorPlacement`-tel ellenőrzi a világ sütött
  actor-profiljával; elutasításkor a meglévő szabály szerint a játékos
  spawn régióra esik vissza (soha nem ugyanarra az XY-ra a terepen).
- **Térbeli index:** a bucket-identitás `(volume, cell_x, cell_y)`
  (`LayeredSpatialCellKey`, a 3D-3 kulcsa most már élesben is). Egy XY
  oszlop volume-onként külön bucketet tart; `GridEntry` és `GridSlot`
  viszi a volume-ot; `Insert`/`Move` explicit volume-ot kér (nincs
  alapérték), `Remove`/`Contains` az egész oszlopban keres, `ContainsIn`
  volume-pontos. Az AOI sugárkeresés az oszlop minden bucketjét olvassa, így
  az érdeklődés vízszintes marad, a szintek látják egymást (szándékos
  policy, lásd korlátok).
- **Ghost/border/transfer:** `BorderEntitySnapshot` és `EntityTransfer`
  viszi a jelenlétet; a ghost entitás is megkapja a komponenst.
- **Harc:** ha bármelyik fél rétegzett, a hatótáv 3D (a felső szintről nem
  lehet az alsót ütni); tisztán terepes harc változatlan (vízszintes).
- **Protokoll (additív Cap'n Proto mezők):**
  `C2sEnterWorld.DebugSpawnOverride.volumeId`,
  `S2cEnterWorldAccept.spawnVolumeId/spawnLayerId`,
  `S2cEntitySpawn.volumeId/layerId`. Régi kliens figyelmen kívül hagyja őket.
  A transform-frame-ek nem jeleznek volume-váltást (3D-5B).
- **Audit:** új `ValidateLayeredPresence` a világ-auditban: volume létezik,
  járható, layer egyezik, a sütött actor ott elfér, z bitre a sík
  magassága; a ghost entitás jelenléte = a snapshoté. A `SpatialValidator`
  volume-pontos bucketet és `GridSlot.volume_id`-t ellenőriz.

## Tesztek (mind lefuttatva, Windows, Debug)

Új `worldbench --mode layeredpresence`: **39/39 PASS**.

- Egység: rétegzett grid (egymás feletti bejegyzések külön bucketben, a
  sugárkeresés minden volume-ot olvas, volume-váltás bucketet vált,
  eltávolítás bármely volume-ból), transfer-roundtrip, protokollmezők
  kódolás/dekódolás (régi hívásnál 0).
- Determinisztikus zóna-szint (valódi `MovementSystem::Step`, ütemező
  nélkül): lépcsőn a volume-sorrend pontosan padló → 4 fok → pihenő, egy
  portál egyszerre, z minden tickben a sík; a pihenő szélén nem lép ki; fal
  előtt megáll (x = 229,3, a fal 230-nál, r = 0,35); 1,5 m-es (alacsony
  LOD-ú) lépés 0,5 m-es darabokra bontva pontosan egy portált lép át;
  rétegzett metaadat nélkül a rétegzett entitás nem mozdul (nincs
  terep-visszaesés).
- Negatív kontrollok: az audit elkapja a síkon kívüli z-t, ismeretlen
  volume-ot, rossz layert, blokkolt cellában álló entitást, metaadat nélküli
  rétegzett entitást; a térbeli validátor az elavult volume-bucketet; javítás
  után ismét OK.
- Valódi futó runtime (két 256 m-es zóna, szigorú csomag): explicit
  rétegzett belépés (z = 1,0); ugyanazon XY-on híd (z = 6) és padló (z = 1)
  két külön bucketben; falba és ismeretlen volume-ba belépés elutasítva →
  terep spawn; padlón futó játékos átmegy a zónahatáron, jelenléte és
  z = 1,0 megmarad (a terep 0 lenne); lépcsőn felmegy a pihenőre (z = 2,
  pontosan 5 portálátlépés, 0 érvénytelen állapot); fal előtt megáll;
  terepes játékos változatlanul mozog; az előző zóna ghostja viszi a
  volume-ot; az egymás fölötti játékosok látják egymást; a világ-audit OK.

Regresszió (a grid-átalakítás után): `aoi`, `ghost`, `border`,
`replication`, `replv2`, `hygiene`, `presence`, `protocol`, `snapshot`,
`activitytemporal`, `mapsplit` (21), `terrain` (20), `worldpackage`
(140 + 1 korábbi symlink-SKIP), `layersupport` (31), `layerlookup` (26),
`layerclearance` (24), `splitmerge` – mind failures=0. A loginserver
(ugyanazt a sémát fordítja) hibátlanul fordul.

## Hatókör-bővítések (review-ra jelölve)

1. Új komponens és a `SpatialGrid` belső szerkezetének cseréje
   (XY oszlop → volume-bucketek); `Insert`/`Move` aláírás bővült.
2. `BorderEntitySnapshot`, `EntityTransfer`, `GridEntry`, `GridSlot`,
   `ZoneTickContext` új mezői; `ZoneDiagnostics` kumulatív rétegzett
   számlálói.
3. Három additív Cap'n Proto mezőcsoport (lásd fent).
4. **Viselkedésváltozás:** rétegzett félnél a harci hatótáv 3D.
5. `SpawnSystem::SpawnPlayer` új (alapértelmezett) jelenlét-paramétere,
   `SpawnCoordinator::SetLayeredWorld`, `SpawnPlacement`.
6. Világ-audit bővítése (`ValidateLayeredPresence`).

## Korlátok

- **Élesben még nem érhető el rétegzett terület:** a production spawn
  régiók terepesek, és nincs terep↔volume átmenet. Rétegzett belépés csak a
  debug override-dal van (Debug/RelWithDebInfo build). Ehhez spawn-volume
  adat (worldlogic vagy sidecar) és bizonyított terep↔volume portál kell.
- Mob spawn pontok terepesek; a mozgáskód kezeli a rétegzett mobot, de nincs
  rétegzett mob-belépés.
- Az AOI vízszintes (a szintek látják egymást); függőleges érdeklődési sáv
  nincs.
- A transform-frame-ek nem jeleznek volume-váltást; a kliens csak a z-t
  látja (3D-5B).
- A rétegzett lépés lineárisan keres a volume-ok és portálok között (nincs
  index); nagy világon mérni kell. Teljesítményt nem mértem.
- Csak Windows/MSVC Debug futott; Linux/FreeBSD és sanitizer nem.

## Újrafuttatás

```powershell
cmake --build gameserver/build/windows-debug --config Debug --target worldbench gameserver
& ./gameserver/build/windows-debug/apps/gameserver/Debug/worldbench.exe --mode layeredpresence
```

## Következő lépés

**3D-5B:** volume-váltás a transform-frame-ben (pl. új mezőmaszk-bit,
protokollverzió-emelés), spawn-volume adat a világcsomagban, terep↔volume
bizonyított átmenet, majd a kliensoldal. Utána **3D-5C**: engine
hálózati kliens és összekötött tesztprojekt.
