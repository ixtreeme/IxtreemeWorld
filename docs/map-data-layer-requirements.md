# Real World / Map Data Layer — kötelező követelmények (M0)

> Státusz: **követelmény-dokumentum, nem implementáció.** A gameserver
> hardening (H0–H10) után ez a következő infrastruktúra-lépés bemenete. A régi
> loadert szándékosan NEM foltozzuk egyenként: minden itt felsorolt hiba a
> jelenlegi kódban reprodukálható, és az új rétegnek kell megszüntetnie.
>
> Bizonyíték: `worldbench --mode mapaudit` (csak olvas, a jelenlegi loadert
> hívja memóriabeli mini-térképekkel; minden sor `REPRODUCED`, amíg a hiba él —
> ha egy sor `CHANGED`-re vált, ezt a dokumentumot is frissíteni kell).
>
> Hatókör: gameserver-oldali világadat (terrain/collision/logikai régiók,
> spawn/warp adat, betöltés, validáció, konfiguráció). Kliens-renderelés,
> loginserver, DB és gameplay nem része.
>
> **Megvalósítási állapot:** lásd [MAP-0 / MAP-1 státusz](#map-0--map-1-státusz-2026-09-24)
> (R1–R13 traceability-mátrix, audit, bench snapshot, baseline, nyitott
> döntések). A mapaudit azóta két oszlopot ír: legacy (kliens-API, a
> reprodukció változatlan) és szerver (CHANGED / PARTIAL / OPEN(MAP-n)).
> A formátum-szerződés: [`map-data-format.md`](map-data-format.md).

## 0. Fogalmak — négy KÜLÖN dolog

| Fogalom | Mi ez | Ki birtokolja | Élettartam |
|---|---|---|---|
| **Map Chunk** | a világadat tárolási/streamelési egysége (fájl, szekció) | map formátum / tooling | statikus (build-time) |
| **Area / Region (logikai)** | tervezői szemantikus terület: spawn-, warp-, esemény-, zene-, PvP-régió | world design | statikus |
| **Server Zone** | szimulációs tulajdon-egység (partíció-levél, saját flecs world) | gameserver (ASF) | **dinamikus** (split/merge futásidőben) |
| **Client Map** | render-assetek: splat, textúra-paletta, víz-vizuál, dekor | kliens | statikus |

A jelenlegi kód ezeket összemossa (lásd R1, R2). Az új rétegben egyik sem
származtatható a másikból implicit módon; a kapcsolatuk explicit leképezés.

---

## R1 — Chunk grid ≠ zone grid

**Követelmény.** A chunk-rács (tárolás) és a szerver zóna-/partíció-rács
(szimuláció) független. A chunkok felsorolása kizárólag a chunk-rács
dimenzióiból (`ceil(world_extent / chunk_size)` tengelyenként) történik; a
szerver zónái a chunkokra semmilyen feltevést nem tehetnek (egy zóna tetszőleges
számú, akár részleges chunkot fed le; egy chunk több zónához is tartozhat).

**Jelenlegi állapot.** `shared/map/src/MapData.cpp:378–381` a chunk-fájlokat a
**zóna-rács** (`manifest.zone_grid_x/y`) szerint iterálja. Ha a kettő eltér, a
világ egy része csendben be sem töltődik, a heightfield mégis „valid".

```
MAPAUDIT R1-chunk-grid-conflated-with-zone-grid 2x2 chunk files, zone_grid=1x1
  -> chunk files read=1, height in chunk(0,0)=1.00m chunk(1,1)=0.00m
  (never loaded, reported valid=1): REPRODUCED
```

A teszt-térkép (`Client/assets/Maps/test_zone`: 4×4 chunk, 4×4 zóna) csak
véletlenül működik.

**Elfogadás.** (a) Chunk-rács ≠ zóna-rács esetén minden chunk pontosan egyszer
töltődik; (b) hiányzó chunk-fájl betöltési hiba (nem nulla magasság);
(c) a szerver zónák száma/alakja a chunk-rácstól függetlenül változtatható
(ASF split/merge) anélkül, hogy bármi újratöltődne.

## R2 — Map Chunk ≠ Server Zone ≠ Client Map

**Követelmény.**
1. A térképformátum **nem tartalmaz szerver-zóna fogalmat.** A manifest
   `zoneGridDims` / `zoneSizeCells` mezői megszűnnek (vagy csak tooling-
   metaadatként maradnak, a szerver nem olvassa). A szerver kezdeti
   partícionálása a szerver konfigurációjából és a világ-bounds-ból jön, a
   futásidejű az ASF-ből.
2. A worldlogic „zóna" rekordjai **logikai area/region** rekordokká válnak
   (saját `AreaId` névtérrel), nem szimulációs partíciók. A szerver `ZoneId`
   névtere független: jelenleg `ZoneManager::BuildFromWorldLogic`
   (`world/zone/ZoneManager.cpp`) a worldlogic id-t közvetlenül `ZoneId`-nak
   veszi; a `0` id ütközik a partíció-gyökér sentinel `zone_id = 0`-val,
   duplikált id `FindIndexById`-t kétértelművé tenné.
3. A kliens-render adatok (splat, textúra-paletta, víz-vizuál) a Client Map
   része; a szerver csomagja nem függ tőlük (lásd R5).

**Elfogadás.** A szerver egy olyan világcsomaggal indul, amely nem tartalmaz
zóna-rácsot és render-adatot; a partícionálás kizárólag a szerver-konfig +
ASF műve; a map `AreaId`-k és a szerver `ZoneId`-k külön névtérben élnek.

## R3 — WorldBounds / origin a betöltött adatból

**Követelmény.** A világ határa és origója (min_x, min_y, max_x, max_y — nem
feltétlenül (0,0)-tól, nem feltétlenül négyzet) a betöltött világcsomagból
(vagy explicit szerver-konfigból) jön, és **minden** világszintű struktúra ebből
származik: régió-/partíció-gyökerek, load field bounds, activity field, spatial
indexek, terrain mintavétel.

**Jelenlegi állapot.**
- `world/partition/RegionDefinition.h:31–43` (`DefaultRegions`): négy fix
  100 km-es kvadráns (0,0)-tól, a betöltött térképtől függetlenül.
- `LoadFieldConfig::bounds` alapértéke `WorldBounds::FromExtent(100000)`;
  `WorldRuntime::ConfigureLoadField` `FromExtent(terrain_.WorldExtentMeters())`
  — mindkettő origó = (0,0) és négyzetes világ.
- `HeightField::SampleHeightMeters` / `IsWalkable` (`MapData.cpp:277–324`)
  origó = 0 és négyzetes `world_size_cells` feltevéssel dolgozik.

```
MAPAUDIT R3-hardcoded-world-bounds regions=4 fixed 0..100000 m quadrants;
  a 1 km map lands entirely in region 3; a point at (-10,-10) has no region: REPRODUCED
```

**Elfogadás.** Nem-nulla origójú és nem-négyzetes tesztvilágon minden
világszintű struktúra a valós bounds-ot használja; a bounds-on kívüli pont
determinisztikusan kezelt (R10); nincs 100 km-es konstans a szerver-kódban.

## R4 — Worldlogic betöltési validáció

**Követelmény.** A betöltés **teljes** validációja, hibás bemenetre
diagnosztizált, egyértelmű elutasítás (production: indítás megtagadva, R8):
- fájl-integritás: pontos méret-/csonkolás-ellenőrzés, magic/verzió, rekord-
  számok felső korlátja, ismeretlen trailing adat kezelése;
- **ID-k:** reserved (0) tiltott, egyediség típusonként, hivatkozási
  integritás (spawn → létező area, warp → létező cél);
- **numerikus:** minden koordináta véges (nincs NaN/±Inf), `min < max`,
  bounds-on belül (R3);
- **geometria:** az átfedés-szabály explicit (area-k átfedhetnek-e; ha nem,
  átfedés = hiba), a határ-tulajdon félig nyitott intervallum (R9);
- a hibaüzenet megnevezi a rekordot, a mezőt és a szabályt.

**Jelenlegi állapot.** `MapData.cpp:450–517` (`LoadWorldLogic`) csak magic,
verzió és ≤1024 rekordszám ellenőrzést végez. A csonkolás-detektálás **soha
nem sül el**: a `ReadU32/ReadF32` túlolvasáskor `offset = size`-ra áll és 0-t
ad vissza, az ellenőrzés viszont `offset > size`.

```
MAPAUDIT R4-truncated-worldlogic-accepted file=24B claims 1 zone
  -> zone id=7 bounds=(0,0)-(0,0): REPRODUCED
MAPAUDIT R4-invalid-worldlogic-accepted zone id 0, duplicate id 5, NaN bounds,
  min>max, overlapping zones, spawn->unknown zone 42, NaN warp target: all accepted: REPRODUCED
```

A manifest (`LoadManifest`, `MapData.cpp:326–359`) `cellSizeMeters`-e sincs
ellenőrizve (NaN/0/negatív → a heightfield később „invalid" lesz → csendes
flat fallback, R8); a Cap'n Proto reader kivétele nincs elkapva (egy sérült
manifest diagnosztika nélküli indítási kivétel).

**Elfogadás.** Egy hibás-bemenet korpusz (csonkolt fájl, 0/duplikált id,
NaN/Inf, min>max, lógó hivatkozás, tiltott átfedés, bounds-on kívüli
koordináta, sérült manifest) minden eleme egyértelmű hibával elutasítódik; a
`mapaudit` R4 sorai `CHANGED`-re váltanak.

## R5 — A szerver csak szerver-releváns adatot tölt

**Követelmény.** A szerver világcsomagja: magasság, ütközés/attribútum
(walkable/blocked, felület-típus), navigáció (ha van), logikai area-k,
spawn-/warp-/esemény-adat. Render-adat (splat, textúra-paletta, víz-vizuál,
dekor) **nem kötelező, nem töltődik be, nem foglal memóriát** a szerveren.
Szekció-szelektív betöltés vagy külön szerver-csomag; szerver-oldali
memória-budget dokumentálva (lásd R12).

**Jelenlegi állapot.** `ParseChunk` (`MapData.cpp:211`) sikerességi feltétele
a két splat-szekció megléte; a `HeightField` a teljes világ splat RGBA-ját
(`splat_a_rgba8`, `splat_b_rgba8`) memóriában tartja, plusz a textúra-paletta
útvonalait.

```
MAPAUDIT R5-server-requires-render-splats heights+attributes only -> load=FAILS;
  with splats -> load=ok, server keeps splat_a+b=...: REPRODUCED
```

**Elfogadás.** Render-szekciók nélküli csomaggal a szerver indul; a szerver
memóriájában nincs render-adat; a kliens-csomag változása (új textúra) nem
igényli a szerver-csomag újragenerálását.

## R6 — Warp/portál szemantika: region-enter + cooldown, nem tick-alapú

**Követelmény.** A warp/portál **esemény**, nem állapot:
- trigger = belépési él (az entitás az előző tickben kívül volt, most belül);
- entitásonkénti cooldown a sikeres teleport után; a célpont nem lehet trigger
  régión belül (load-time validáció, R4) — így sem önmagába, sem két portál
  között nem pattoghat;
- a cél validált (bounds-on belül, walkable, véges);
- a zónák közötti (partíciók közötti) warp a meglévő tranzakcionális
  migrációs úton megy (jelenleg elutasítva: „cross-zone warp is not enabled in
  M4").

**Jelenlegi állapot.** `world/systems/MovementSystem.cpp:39–74`
(`TryApplyWarp`) minden tickben lefut, amíg a pozíció a forrás-téglalapban van;
nincs él-detektálás, nincs cooldown, nincs cél-validáció a trigger-régiókra.

```
MAPAUDIT R6-warp-retrigger warp 1 target inside its own source -> fires again next tick;
  warps 2<->3 targets inside each other's source -> ping-pong every tick
  (no enter edge, no cooldown): REPRODUCED
```

**Elfogadás.** Egy portálon álló entitás pontosan egyszer teleportál; a
ciklikus portál-konfiguráció betöltéskor elutasítódik; cross-zone warp
migrációval, ownership-invariánsok megtartásával működik.

## R7 — A production map útvonal runtime konfigból jön

**Követelmény.** A világcsomag helye (és minden világ-adat: spawn-táblák, mob
típusok) **runtime** szerver-konfigurációból / parancssorból jön, a telepítés
gyökeréhez relatívan. Build-time abszolút forrásfa-útvonal nem kerülhet a
bináris production-útjába (a tesztek explicit konfiggal adják meg a sajátjukat).

**Jelenlegi állapot.** `apps/gameserver/CMakeLists.txt:78–79`
(`IXTREEME_DEFAULT_MAP_ROOT`, `IXTREEME_DEFAULT_MOB_TYPES_CONFIG`) a
forrásfa abszolút útját fordítja be; a production konstruktor
(`WorldRuntime(io, identity)`, `world/WorldRuntime.cpp:111`) ezt használja,
a `main.cpp` nem olvas map-root kulcsot.

```
MAPAUDIT R7-build-time-map-path production map root = compile-time source-tree path
  ".../gameserver/apps/gameserver/../../../Client/assets/Maps/test_zone": REPRODUCED
```

**Elfogadás.** Egy másik gépre másolt bináris a konfigban megadott világ-
csomaggal indul; a konfig hiányában egyértelmű hibával áll le (R8).

## R8 — Production konfigban nincs csendes szintetikus fallback

**Követelmény.** Production módban a világ betöltésének vagy validációjának
bármely hibája **fail-fast** (indítás megtagadva, pontos diagnosztikával). A
szintetikus/flat világ csak explicit, külön módban érhető el (pl. `world_mode =
synthetic`, benchmarkokhoz), soha nem hiba-következményként.

**Jelenlegi állapot.** `world/terrain/TerrainService.cpp:47–57`: betöltési hiba
→ `LOG_WARN` + flat 1000 m világ; `world/WorldRuntime.cpp:62`: worldlogic hiba →
`LOG_WARN` + egyetlen „fallback" zóna. A szerver egy nem létező világgal is
elindul és kiszolgál.

```
MAPAUDIT R8-silent-flat-fallback unloadable map root -> HasTerrain=0 extent=1000m (flat),
  a warning only: REPRODUCED
```

**Elfogadás.** Hiányzó/sérült/invalid csomag production módban nem-nulla
exit kóddal és a hiba pontos helyével áll le; a szintetikus mód csak explicit
kapcsolóval indul, és ezt a startup log egyértelműen jelzi.

---

## További, az audit során talált követelmények

### R9 — Egységes határ-szemantika (félig nyitott intervallum)
A map `Rect::Contains` zárt `[min, max]` (`MapData.cpp:216–219`), a szerver
partíció félig nyitott `[min, max)` (`world/partition/ZonePartition.h`
`FindLeaf`). Közös élen egy pont két map-area-hoz is tartozhat, az első nyer
(listasorrend-függő). **Követelmény:** a világréteg minden területi fogalma
félig nyitott, így egy pont pontosan egy area-hoz/zónához tartozik.

### R10 — Bounds-on kívüli viselkedés explicit
`SampleHeightMeters` a világon kívül a szélső magasságra clampel, az
`IsWalkable` ugyanott `false`-t ad — ugyanaz a pont egyszerre „van talaj" és
„nem járható". **Követelmény:** egyetlen dokumentált szabály (pl. a bounds-on
kívüli pont nem létező terület: nem járható, nincs magasság, a mozgás nem
léphet ki), és minden lekérdezés ezt követi.

### R11 — Strukturált betöltési hibák, formátum-verziózás
Minden loader strukturált hibát ad (fájl, szekció, rekord, mező, szabály), a
Cap'n Proto/ bináris olvasók kivételeit a loader zárja le. A formátumok
verziózottak, visszafelé-kompatibilitási szabállyal (mi a teendő egy újabb
minor verzióval).

### R12 — Világléptékű memória: chunk-streamelt szerver-terrain
A jelenlegi `HeightField` a teljes világot egy tömbben tartja
(`world_size_cells²` magasság + attribútum + splat). A „Real World" léptéken
(100 km, néhány méteres cella) ez nem tartható. **Követelmény:** a szerver
terrain/ütközés-adata chunk-granulárisan töltődik és a zóna-rezidenciához
igazodik (egy zóna csak a saját + a ghost-sáv chunkjait tartja), dokumentált
per-zóna és per-process memória-budgettel; a betöltés nem blokkolhatja a 20 Hz
tick-et (háttér-betöltés, a zóna az adat megérkezése előtt nem aktiválódik).

### R13 — A spawn-adat a validált világcsomag része
A mob-spawnok jelenleg egy szöveges `mob_spawns.conf`-ból jönnek a map
root-ban (`SpawnCoordinator::Initialize`). **Követelmény:** a spawn-táblák a
világcsomag validált részei (area-hivatkozással, R4 szabályaival), nem külön,
lazán csatolt konfig.

---

---

## MAP-0 / MAP-1 státusz (2026-09-24)

> Munkaprompt: `IxtreemeWorld_Real_World_Map_Data_Layer_Opus_Prompt.md`.
> Alap: `With_Auriga` @ `0efc4033` (hardening chunk 3), a MAP-0/MAP-1
> változások commit nélkül a working tree-ben. Referencia-binárisok:
> `worldbench_hardening.exe` / `gameserver_hardening.exe` (0efc4033).

### MAP-0 audit — a production indulási út

`main.cpp` → config (`--config`, a világkulcsok a config-fájl könyvtárához
relatívak) → **világcsomag betöltése és validálása** (`WorldPackageLoader` →
`mx::map::LoadServerWorld`) → hiba esetén exit 3, még DB és hálózat előtt →
libsodium → DB pool → `WorldRuntime(io, identity, LoadedWorld)` →
`TerrainService(HeightField)` + `ZoneManager::BuildFromWorldLogic` (a
worldlogic zónái a partíció-gyökerek levelei a fix `DefaultRegions` alatt) +
activity/load field (`WorldBounds::FromExtent`) + spawn (`SpawnCoordinator`) →
`Start()` (worker pool) → indulási összegzés → `Server::Start()` (world
admission). MAP-1 előtt a világ fordításkori forrásfa-útból jött, csendes
lapos/egyzónás fallbackkel, és a DB-csatlakozás megelőzte.

Tényleges szerepek: `RegionDefinition::DefaultRegions` — négy fix 100 km-es
kvadráns (0,0)-tól (R3, változatlan); `WorldBounds` — `[0, extent]²`;
`SpawnCoordinator` — játékos alap-spawn a worldlogic első spawn-régiójából,
mob-spawn a csomag spawn-táblájából, járhatóság-ellenőrzéssel;
`MovementSystem` — tengelyenkénti `IsWalkable`, magasság-mintavétel,
`TryApplyWarp` minden tickben zárt téglalap-tartalmazással (R6);
`SpatialGrid` — zóna-lokális AOI-index, nem térképadat; `mapgen_test_zone` —
most a közös íróra épül.

**Collision / navigation / water — ami ténylegesen van:** collision = az
attribútum-rács bit 0-ja (blokkolt cella), tengelyenkénti mozgás-ellenőrzés
egyetlen célmintán (nincs útvonal-menti ellenőrzés, nincs magassági/lejtő
korlát, nincs statikus alakzat); navigáció = nincs adat és nincs formátum;
víz = a kliens `water_bodies.mxwater` (MXWB, a test_zone-ban 0 víztest),
szerveroldali fogyasztó nincs. A MAP-1 ezt nem bővíti; a víz-réteg a
csomagban deklarálható, de `LAYER_NOT_VALIDATED` (nem „kész").

### MAP-0 — biztonságos bench snapshot

`WorldRuntime::CaptureSnapshot<T>(collector)` / `WaitSnapshot`: a kollektor a
supervisor szálon fut, csendes ablakban (nincs repülő zóna-tick, két
topológia-tranzakció között: a pass elején és a partíció-vezérlés/reclaim
után), `SnapshotContext{zones, owners, epoch, world_tick, captured_at,
reclaim}`. A kérő a jövőre vár; a supervisor saját szálán a várakozás és a
kollektoron belüli új kérés `std::logic_error` (nincs önmagára várás). Leállításkor
a függő kérések a végső csendes ablakban válaszolódnak meg. Nincs production
hot-path lock (a mutex csak függő kérés esetén, a supervisoron). Benchen:
`gs::bench::ReadWorld` (időtúllépésnél abort, hogy lógó referencia ne
maradjon). **Minden** bench-oldali `sim.Zones()` / `sim.Owners()` olvasás és a
zónatáblát bejáró futásidejű olvasó API-k bench-hívásai (`CollectProcessLoad`,
`ScorePartition`, `ScoreMerge`) snapshotra álltak át; zóna-mutató /
`ZonePartition*` / slot-index nem marad meg hívások között. A readiness
akkumulátor `ZoneId` kulcsú (a H9 slot-újrahasznosítás miatt), a riport egyetlen
epochból jön (zónaszám, tier-gauge-ek, slot-reuse, working set).

Bizonyíték: `worldbench --mode snapshot --cycles 300` (3 olvasó szál, 400
split + 400 merge, 1995 reclaim / 1669 slot-újrahasznosítás, migrációk):
5916 snapshot, 2400 topológia-változás két egymás utáni capture között, 0
invariáns-sértés (nincs repülő tick, nincs Staging, levelek hézag/átfedés
nélkül fedik a világot, egyedi authority, owner-cache feloldható, konzervált
populáció, monoton epoch / world tick / reclaim számlálók), max várakozás
~30 ms; Debug buildben (assertekkel) is zöld. A readiness riport
`snapshot-quiescent` ellenőrzése: 0 repülő tick, 0 Staging zóna.

### MAP-0 — új mérési baseline

Windows 11, 16 mag, RelWithDebInfo; ugyanaz a gép, egymás után futtatva.
`hardening` = 0efc4033 (nyers, versenyző bench-olvasás), `map0` = snapshot-út.

| Futás | Bináris | Zóna-slot | tick p50 / p95 / p99 / max ms | Tier-eltérés | Working set | Snapshot-várakozás |
|---|---|---|---|---|---|---|
| smoke 100p / 20k | hardening | 64 | 0.58 / 2.25 / 4.25 / 12.2 | — | 283 MB | — (stuck_zones=0) |
| smoke 100p / 20k | map0 | 64 | 0.55 / 1.72 / 3.08 / 6.9 | 8 (≤ 40) | 283 MB | 1.5 ms |
| dense 500p / 200k | hardening | 64 | 4.19 / 5.76 / 72.6 / 129.1 | — | 1681 MB | — (**stuck_zones=1**: tick olvasás közben) |
| dense 500p / 200k | map0 | 64 | 4.15 / 5.59 / 72.5 / 133.6 | 2 (≤ 400) | 1686 MB | 72 ms |
| spread 7000p / 200k | hardening | 160 | 2.01 / 8.29 / 10.66 / 36.6 | — | 2994 MB | — |
| spread 7000p / 200k | map0 | 164 | 1.90 / 7.97 / 9.93 / 27.3 | 8 (≤ 400) | 2987 MB | 8 ms |

A 7000p topológia eltér (160 vs 164 slot) — nem tiszta A/B. A dense p99 a
baseline-on is > 50 ms; ezt nem a map-réteg javítja.

**Map-betöltési baseline** (becsekkolt `test_zone`, 500×500 cella, 16 chunk,
3.1 MB, meleg OS-cache, 20 futás átlaga): legacy `LoadHeightField` 19.5 ms,
rezidens 3.10 MB (splattel); szigorú szerver-betöltő Startup 4.5 ms, rezidens
1.00 MB (magasság + attribútum); Full 4.4 ms.

### MAP-1 tesztek és regresszió

- `worldbench --mode worldpackage`: 107 PASS, 0 FAIL, 1 SKIPPED
  (`path-symlink-escape`: ezen a Windows-fiókon nincs symlink-jogosultság; a
  kanonikus komponensenkénti tartalmazás elutasító ága így nem futott, a
  lexikális `..`/abszolút/`\`/`:` esetek igen). RelWithDebInfo és Debug.
- `scripts/map1_startup_acceptance.sh` (valódi `gameserver`, két idegen cwd,
  DB-vel): 15/15 PASS.
- Teljes regresszió a 0efc4033 bináris ellen: minden pár azonos PASS-számmal
  (readiness +1: `snapshot-quiescent`), minden hardening-mód zöld. Kivétel:
  `scheduler` — `uniform-parallelism` / `reference-parallelism` falióra-alapú
  küszöbe (> 2.0) ezen a gépen **mindkét** binárison ingadozik (7-7 futásból
  4-4 FAIL, uniform-64 párhuzamosság: base 1.84–5.11, új 1.81–4.90):
  meglévő, nem MAP-eredetű flakiség, nem PASS-ként számolva; munkára
  normalizált kritérium kellene (külön döntés).

### R1–R13 traceability (az R-azonosítók nincsenek újraszámozva)

„Legacy" = a változatlan `MapData.h` kliens-API; „szerver" = a gameserver
tényleges útja (`worldbench --mode mapaudit` mindkét oszlopot kiírja).

| R | Jelenlegi kód / hívó | Reprodukció / bizonyíték | Csomag | Elfogadási teszt | Státusz |
|---|---|---|---|---|---|
| **R1** Chunk grid ≠ zone grid | szerver: `WorldPackage.cpp` `ParseManifest` (rács = ceil(size/chunk)), `LoadServerWorld`; legacy: `MapData.cpp` `LoadHeightField` (zone grid) | mapaudit R1: legacy REPRODUCED / szerver CHANGED (4 chunk olvasva); korpusz `valid-v2-legacy-layout`, `index-*`, `chunk-file-missing` | MAP-1 (formátum + betöltő); részleges chunk: MAP-2 | (a) minden chunk pontosan egyszer ✓ (b) hiányzó chunk = hiba ✓ (c) split/merge újratöltés nélkül: ma rezidens terrain; streamelve MAP-3 | szerver-úton KÉSZ (a,b); (c) MAP-3 |
| **R2** Map Chunk ≠ Server Zone ≠ Client Map | v3 tiltja a `zoneGridDims`/`zoneSizeCells`-t; a worldlogic zónái még `ZoneManager::BuildFromWorldLogic` partíció-magjai (ZoneId = worldlogic id), de id 0 / > 0xFFFFFF / duplikált id betöltéskor elutasítva | mapaudit R2 PARTIAL; korpusz `manifest-v3-zone-grid`, `worldlogic-zone-id-*`; server-only csomag (R5) | MAP-1 (formátum, id-szabály), MAP-2 (AreaId/ZoneId szétválasztás, partíció szerver-configból + bounds-ból) | — | RÉSZLEGES |
| **R3** WorldBounds / origin az adatból | `DefaultRegions` (fix 100 km), `WorldBounds::FromExtent`, `HeightField` origó 0 — változatlan | mapaudit R3 REPRODUCED; nem-nulla origó / nem-négyzetes v3 `UNSUPPORTED_FEATURE`-rel elutasítva (nem félreolvasva) | MAP-2 | nem-nulla origójú, nem-négyzetes tesztvilág | NYITOTT (őrzött) |
| **R4** Worldlogic validáció | `ParseWorldLogic` (pontos méret, trailing, fejléc) + `ValidateWorldLogic` + `ValidateWarpTargets`; manifest: capnp-kivétel elkapva, cellSize/méretek validálva | mapaudit R4×2: legacy REPRODUCED / szerver CHANGED; korpusz 21 worldlogic + 19 manifest eset | MAP-1 | hibás-bemenet korpusz minden eleme elutasítva, stabil kóddal ✓ | KÉSZ (szerver-út) |
| **R5** Csak szerver-releváns adat | `LoadServerWorld`: csak magasság + attribútum marad; splat Startup-ban csak tartomány, Full-ban szerkezet; paletta nem töltődik | mapaudit R5 CHANGED; korpusz `valid-v3-server-only` (splat 0 B), `valid-v3-with-client-data`; indulási teszt 5 | MAP-1; memória-budget MAP-3 (R12) | render-szekció nélküli csomaggal indul, nincs render-adat memóriában ✓ | KÉSZ (budget: MAP-3) |
| **R6** Warp: region-enter + cooldown | futásidő változatlan (`TryApplyWarp` tickenként); betöltéskor: ciklus = hiba, lánc = warning, cél véges/bounds-on belül/járható | mapaudit R6 PARTIAL; korpusz `worldlogic-warp-*`, `warp-chain-warning`; test_zone: warp 1 → warp 2 lánc (warning) | MAP-1 (betöltési szabály), MAP-4 (él, cooldown, cross-zone warp migrációval) | egyszeri teleport, cross-zone warp | RÉSZLEGES |
| **R7** Runtime map-útvonal | `main.cpp` `ResolveWorldPlan` (CLI > config > default; config-relatív); a gameserver targetben nincs fordításkori világ-út; bench: `IXTREEME_TEST_MAP_ROOT` | mapaudit R7 CHANGED; indulási teszt 1a/1b (két cwd), 1c (CLI felülírás), 2e (nincs csomag → exit 2) | MAP-1 | másolt bináris configgal indul; config nélkül egyértelmű hiba ✓ | KÉSZ |
| **R8** Nincs csendes fallback | `TerrainService::LoadFromMapRoot`, `LoadWorldLogicFromMapRoot`, `WorldRuntime(io)` alap-konstruktor törölve; validálás a DB/listen előtt, exit 3; synthetic csak `world_mode=synthetic`, WARN-nal jelölve | mapaudit R8 CHANGED; indulási teszt 2a–2f, 3 | MAP-1 | hiányzó/sérült/invalid csomag → nem-nulla exit, nincs world admission ✓ | KÉSZ (a `BuildFromWorldLogic` üres-logika ága elérhetetlen, a MAP-2 bootstrap-átdolgozáskor törlendő) |
| **R9** Félig nyitott határ | validátor: félig nyitott (zóna átfedés/lefedés, célpont `[0,extent)`); futásidő: `Rect::Contains` zárt, partíció félig nyitott | mapaudit R9 OPEN | MAP-2 | közös élen egy pont pontosan egy area-hoz | NYITOTT |
| **R10** Bounds-on kívüli szabály | változatlan (magasság clamp, járhatatlan) | mapaudit R10 OPEN | MAP-2 | egyetlen dokumentált szabály minden lekérdezésben | NYITOTT |
| **R11** Strukturált hibák, verziózás | `PackageIssue` (stabil kód, package/layer/file/field/offset/chunk/reason/expected), capnp-kivétel lezárva; v2→v3 átmeneti szabály; réteg-verziók | mapaudit R11 CHANGED; korpusz (107 eset) | MAP-1 | — | KÉSZ (szerver-út; legacy kliens-API változatlan) |
| **R12** Chunk-streamelt szerver-terrain | rezidens magasság + attribútum (test_zone 1.0 MB); `(W+1)(H+1) ≤ 2^28` fölött `MANIFEST_SIZE_OVERFLOW` (100 km / 2 m nem tölthető be MAP-3 előtt) | mapaudit R12 OPEN | MAP-3 | világléptékű memória-mérés streameléssel | NYITOTT |
| **R13** Spawn a validált csomag része | `mobSpawns` réteg: szigorú szintaxis, véges, bounds, mob-típus registry kereszt-ellenőrzés; area-hivatkozás nincs | mapaudit R13 PARTIAL; korpusz `spawns-*` (11 eset) | MAP-1 (réteg), MAP-4 (area-hivatkozás, generikus spawn) | — | RÉSZLEGES |

### Döntést igénylő pontok a MAP-2/3 előtt

1. **AreaId vs ZoneId:** a worldlogic zónái logikai area-vá válnak; mi adja a
   szerver kezdeti partícióját (config-rács a bounds-on? egy gyökér + ASF?).
2. **Origó / nem-négyzetes világ / pontosság:** Float64 origó a formátumban,
   f32 futásidejű pozíció — elfogadható-e 100 km-en (~8 mm), vagy
   zóna-lokális koordináta kell?
3. **Részleges szélső chunk** engedélyezése és a szélső minta tulajdonjoga.
4. **R10 szabály:** a bounds-on kívüli pont nem létező terület (nem járható,
   nincs magasság, a mozgás nem léphet ki) — megerősítés.
5. **Warp-lánc:** R6 szerint a cél nem lehet trigger-régióban; most warning
   (a becsekkolt test_zone warp 1 célja (760,760) a warp 2 forrásának sarkán
   van). Hibává tétel esetén a test_zone tartalmát javítani kell (automatikus
   felülírás nélkül).
6. **Collision/víz/navigáció terjedelme (MAP-3):** attribútum-bitek (felület,
   víz, PvP) új réteg-verzióként vagy külön rétegként; útvonal-menti
   mozgás-ellenőrzés; víz deklarációja (nincs / tengerszint / lokális víztestek);
   navigációs adat formátuma (jelenleg semmi).
7. **Streaming:** rezidencia-egység (zóna + ghost-sáv chunkjai), per-zóna és
   per-process budget, I/O-szál modell, sérült későn betöltött chunk kezelése.
8. **Kliens és v3:** a kliens v2-n marad (a szerkesztő a chunkokat helyben
   írja, v3 CRC-t nem frissítene) — konverter vagy kliens-v3 később (kliens
   nem része a fázisnak).
9. **Mob-típus registry helye:** szerver-config (nem a csomag része) —
   megerősítés.
10. **Worldlogic formátum jövője:** MXL1 bináris marad (v2 area-típusokkal),
    vagy a korábbi Cap'n Proto `MapLogic` koncepció?

## Nem cél (ebben a lépésben)
- Gameplay (combat, AI-viselkedés, loot, quest).
- Kliens-renderelés és a Client Map formátuma (csak annyiban, hogy a szerver
  ne függjön tőle).
- Navigációs háló generálása (a formátumban a helyét kell biztosítani).

## Invariánsok, amelyeket az új réteg nem sérthet
Az authoritative 20 Hz, a tranzakcionális ownership/migráció, ghost
`max_copies = 1`, AOI-ekvivalencia, Replication v2 konvergencia, ASF
split/merge stabilitás, scheduler no-double/no-missed, a presence-invariáns
(H4) és a zóna-slot reclamation (H9) — a világréteg csak adatot szolgáltat,
a szimuláció tulajdonmodelljét nem módosítja.

## Elfogadási teszt-terv (összefoglaló)
1. `worldbench --mode mapaudit`: minden R-sor `CHANGED` (a régi viselkedés
   megszűnt), és helyükre pozitív tesztek lépnek (R1–R13 elfogadási pontjai).
2. Hibás-bemenet korpusz (R4, R11) — minden elem egyértelmű elutasítás.
3. Nem-nulla origójú, nem-négyzetes, chunk-rács ≠ zóna-rács tesztvilág (R1,
   R3, R9, R10) a teljes regression-csomaggal.
4. Szerver-csomag render-adat nélkül (R5), runtime útvonallal (R7), sérült
   csomaggal fail-fast (R8).
5. Portál-él/cooldown/ciklus-tesztek + cross-zone warp migrációval (R6).
6. Világléptékű memória-mérés chunk-streameléssel (R12) readiness-futásban.
