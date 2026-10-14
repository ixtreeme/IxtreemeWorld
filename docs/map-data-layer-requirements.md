# Real World / Map Data Layer — kötelező követelmények (M0)

> **Aktuális SL-2 bizonyíték:** az új C-moving acceptance és az R6 strict/legacy szerződés Windows/MSVC alatt igazolt; az eredeti moving churn továbbra is FAIL. A teljes regresszió, platformhatár és részletes verdiktek: [SL-2 review](map-streaming-sl2-review.md). Az alábbi M0 és MAP-1/2/3 findings történeti szövegek; az aktuális állapotot a traceability-mátrix és a review adja.

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
> (audit, bench snapshot, baseline) és [MAP-2 státusz](#map-2-státusz-2026-09-25)
> (döntések, R1–R13 traceability-mátrix) és [MAP-3 státusz](#map-3-státusz-2026-09-25). A mapaudit azóta két oszlopot ír: legacy (kliens-API, a
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

## MAP-2 státusz (2026-09-25)

> Alap: `With_Auriga` @ `ab2458de` (MAP-0 + MAP-1 commit), a MAP-2 változások
> commit nélkül a working tree-ben. Referencia-bináris a regresszióhoz:
> `worldbench_hardening.exe` (0efc4033), mint a MAP-1-nél.

> **Review-döntés (2026-09-25):** a MAP-2 elfogadva a riportban bizonyított,
> **resident terrainre vonatkozó terjedelemben**. A MAP-3 előtt kért négy
> pontosítás: [MAP-2 review utómunka](#map-2-review-utómunka-2026-09-25).

### Döntések (MAP-2) — elfogadva (review 2026-09-25)

1. **AreaId ≠ ZoneId.** Az MXL1 „zone" rekordjai **area**-k: tervezési
   metaadat, saját `AreaId` (u32 ≥ 1, hézag és nagy id megengedett),
   diszjunktak, de **nem kell lefedniük a világot**; 0 darab is lehet (a
   kötelező spawn-régióval való kapcsolatát a review-utómunka rögzíti:
   `areaId = 0` = világszintű spawn-régió). A szerver `ZoneId`-jait a
   `ZoneManager::BuildInitialPartition` osztja ki (1..N a kezdeti leveleknek),
   a map id-jaitól függetlenül.
2. **Kezdeti partíció = szerver-konfig a betöltött bounds-on:**
   `partition_regions` (alap 2x2, szintetikus módban 1x1) ×
   `partition_initial_leaves` (alap 1x1 régiónként). Régiónevek a tengelyekhez
   igazodnak (World / West,East / South,North / SouthWest…NorthEast /
   `R<ix>_<iy>`). Minden kezdeti levél ≥ 240 m (2 × AOI-sugár) — különben exit 2
   a DB/hálózat előtt. Explicit bootstrap-leírás (téglalap-lista, egy „World"
   régió) csak teszteknek: a régi WorldBench-szcenáriók 3-levelű geometriája
   `LegacyTestMapLayout()`-ként, a bench adja — nem az area-kból származik.
3. **Origó / pontosság:** Float64 origó a formátumban, `f32` futásidejű pozíció;
   minden világkoordináta `|c| ≤ 131072 m` (lépésköz ≤ 1/64 m). Egy 100 km-es
   világ origó-központosítva (±50 km) 1/256 m-es lépésközzel fér bele;
   zóna-lokális koordináta most nem kell.
4. **Részleges szélső chunk:** v3-ban engedélyezett, csak a világon belüli
   cellákat tárolja; az MXC1 `cellsPerSide` a névleges lépték. A határminta
   mindkét szomszédban megvan (egyezés validálva); a lekérdezés a cella
   chunkját olvassa — egy pontnak pontosan egy chunk a gazdája.
5. **R10 szabály:** a világon kívül nincs talaj (nincs magasság, nem járható),
   nincs általános clamp; mozgás/spawn/warp saját explicit szabállyal
   (`map-data-format.md` 9.3).
6. **Magassági tartomány — jóváhagyva:** **height réteg v2**, explicit
   `heightEncoding` (int16 vagy int32 minta, `metersPerUnit`, `offsetMeters`);
   a v1 (int16 cm, ±327.67 m) változatlanul érvényes. (Eredetileg a munkapont
   értelmezése volt; a review a height- és residency-szerződést jóváhagyta —
   szövege: `map-data-format.md` 10.)

### Mi változott (követelményenként)

- **R2/R3:** `RegionDefinition::DefaultRegions` (fix 100 km-es kvadránsok) és
  `ZoneManager::BuildFromWorldLogic` törölve; `BuildInitialPartition(bounds,
  layout)`; `FindIndexForPosition` félig nyitott gyökér-ellenőrzés + fa-bejárás,
  lineáris fallback nélkül; a load field / activity field / régiók / terrain
  bounds-a a betöltött `TerrainService::Bounds()`; a merge-csoportokból a
  gyökerek kizárva; a WorldValidator minden belső csomópont (gyökerek is)
  lefedését és a szimuláló levél-zónák = gyökerekből elérhető levelek
  egyezését ellenőrzi (egy levél sem „szökhet el" a load monitor elől).
- **R1/R9/R10 terrain:** új `mx::map::ServerTerrain` (chunkonkénti tárolás,
  `GridGeometry`, `HeightEncoding`, `TerrainStatus`), a `TerrainService` erre
  épül (`Height`/`Cell`/`IsWalkable` státusszal; a `SampleGroundHeight` /
  `WorldExtentMeters` törölve). A cella-hozzárendelés világ-egységben dől el
  (egy varrat alatti pont nem csúszik át a szomszéd chunkba osztási kerekítés
  miatt — a `terrain` mód találta meg).
- **Mozgás / spawn / warp / migráció:** tengelyenkénti lépés-elutasítás clamp
  nélkül; z csak `Ok` magasságból; mobok világon belül; spawn-sorrend
  (debug → az első spawn-régió közepe; a DB-pozíció a review-utómunka óta
  nem jelölt), különben `EnterWorldReject SERVER_ERROR`; mob-spawn
  újrasorsol/kihagy; warp
  félig nyitott triggerrel; transzfer-z csak `Ok` magasságból.
- **Formátum:** v3 nem-négyzetes, negatív origó, részleges chunk, height v2,
  koordináta-tartomány; worldlogic 415/416 új, 408/414 visszavonva.
- **Diagnosztika:** a szabályok által elutasított *kényszerített* split
  `WARN partition: forced split REFUSED zone=… reason=min-size` sort és
  `executed=false` döntés-rekordot ír (`SplitMinSize`), nem csendes no-op.
  Az indulási log régiónként kiírja a nevet és a félig nyitott bounds-ot.

### MAP-2 tesztek

- `worldbench --mode terrain` — 18/18 PASS (RelWithDebInfo és Debug).
  Orákulum: a fixture generátor-függvénye + saját bilineáris kiértékelés
  (a providerrel közös kód/adat nélkül) + kézzel számolt minták; 20000
  véletlen pont (max hiba 9.5e-7 m, 14347 negatív magasság), szintek (0 m és
  5 m plató bitre pontos, lépcső-rámpa), lejtő (különbségek), varratok
  (x = 106, 362; y = 166; ±1e-3 m; határminták egyeznek), részleges chunk
  (6×13 cella, 98 minta), világ-perem (12 eset: max él kívül, min él belül,
  NaN/±inf), `NotResident` vs érvényes 0 m ugyanazon a ponton (+ a varrat
  egyetlen gazdája X és Y irányban), height v2 int32 (5000 pont, 2763.667 m);
  futásidő: keleti peremen futó játékos 409.8998-nál áll meg (nincs clamp),
  DNy-i negatív sarok, északi peremen tengelyenkénti csúszás, 5 érvénytelen
  spawn-kérés (max él / kívül / NaN / blokkolt / nincs) → spawn-régió közepe,
  80/80 mob a világon belül a peremen túlnyúló körökből, z = orákulum,
  minden világszintű fogyasztó a betöltött bounds-szal, WorldValidator OK.
- `worldbench --mode mapsplit` — 21/21 PASS (RelWithDebInfo és Debug).
  Valódi loaderből: 2560×2048 m, origó (−1280,−1024), 4×3 chunk (utolsó
  32×64 cella), −22.9…31.5 m domborzat, 370 mob + 4 mozgó játékos; 1 → 4 → 16
  kényszerített split, a 640×512 m-es levél további splitje `min-size`
  diagnosztikával elutasítva, merge 16 → 4 → 1. Minden lépésben: 0 invariáns-
  sértés, azonos net-id halmaz (nincs duplikáció/vesztés), 6138 világon belüli
  lekérdezési pont mindegyike pontosan egy levélé (vágási vonalak ±1 ulp),
  8 külső pont egyiké sem, terrain-válaszok bitre azonosak, mob-z = terrain,
  `PackageLoadCount` és rezidens bájtok változatlanok, WorldValidator OK;
  6 játékos-migráció a létra alatt. Worker pool 1 kezdeti zónával 15 worker
  (nem a zónaszámhoz kötve). Teszt-térkép (1 km, alap 2x2): `min_zone=240`
  mellett 4 → 16 → 4, 250 m-nél `min-size` elutasítás; alap `min_zone=500`
  mellett a 500 m-es levél nem splitelhető (`min-size`).
- `worldbench --mode worldpackage` — 117 PASS, 0 FAIL, 1 SKIPPED
  (`path-symlink-escape`, jogosultság). Új: geometria (negatív origó,
  nem-négyzetes, részleges chunk), részleges chunk hibás szekciómérettel,
  v2 részleges chunk, koordináta-tartomány, nem véges origó, height v2
  (int32 tartomány, skálázott int16, kódolás nélkül, v1 kódolással, 0 skála,
  mintatípus-eltérés), 415/416, nagy/hézagos area-id.
- `worldbench --mode mapaudit` — server_changed=11, open/partial=3 (R6, R12,
  R13).
- `scripts/map1_startup_acceptance.sh` — 22/22 PASS (valódi `gameserver`,
  DB-vel): új 1d (teszt-térkép alap 2x2: SouthWest…NorthEast bounds a logban),
  3/3b (szintetikus 1x1 / explicit 2x1), 6a (negatív origó, nem-négyzetes,
  részleges chunk, 2x1: West/East), 6b (alap 2x2 egy 560×360 m-es világon:
  280×180 m-es levél < 240 m → exit 2), 6c/6c2 (értelmezhetetlen rács,
  17x1 > 16 régió/tengely → exit 2), 6d (height v2 int32 a logban), 6e (nincs
  spawn-régió → exit 3). A korpusz 256 m-es csomagjai
  `partition_regions=1x1`-gyel indulnak (2x2-vel 128 m-es levelek lennének).
- Teljes regresszió (RelWithDebInfo, a `worldbench_hardening.exe` 0efc4033
  bináris ellen, egymás után futtatva): mind a 22 pár azonos PASS-számmal
  (readiness +1: `snapshot-quiescent`, mint a MAP-1-nél), a `scheduler` ezúttal
  mindkét binárison 13/13 (a korábbi ingadozás nem jelent meg — ettől még
  nem tekintjük megoldottnak); minden csak-új mód zöld: tickrate, inputpath,
  netstress ×3, presence, asfdeterminism, workerpool, replv2, protocol,
  reclamation 100/1000, hygiene, mapaudit, snapshot (300 ciklus), worldpackage,
  terrain, mapsplit. Readiness tick p50/p95/p99 (ms), base → új: dense
  500p/200k 4.13/5.10/66.1 → 4.17/4.60/66.0; spread 7000p/200k
  2.20/7.87/9.55 → 2.01/7.99/9.10; smoke 100p/20k három ismételt páron
  base p50 1.01–2.05, új 1.05–1.24 (zaj, p95/p99 egyező).

### R1–R13 traceability (MAP-4 után; az R-azonosítók nincsenek újraszámozva)

„Legacy" = a változatlan `MapData.h` kliens-API; „szerver" = a gameserver
tényleges útja (`worldbench --mode mapaudit` mindkét oszlopot kiírja).

**Történeti SL-1 review-státusz (2026-09-27, az SL-2 előtti R3):** a történeti MAP-4 „server changed”
eredmény nem jelent minden eredeti követelményre teljes lezárást. **R6
OPEN/DEFERRED:** nincs explicit strict/legacy loader-kapcsoló; az aciklikus
triggerbe célzás warninggal elfogadott. A runtime rearm működése ettől külön
igazolt. **R12:** a reprodukált retention/admission elakadás célzott javítása
és snapshot nélküli reclaim tesztje SL-1; a review-val elfogadott 200k-mobos
C workload és a teljes integrált SL-2 minősítés még nincs lezárva. A részletes
aktuális verdiktet, új API-t, eredeti reprodukciót és tesztstátuszt a
[`streaming-hardening review`](map-streaming-admission-liveness.md) tartalmazza.
Az alábbi korábbi PASS-számok történeti bizonyítékok, nem új SL-1 futások.

**SL-2 aktuális szerződés:** a szerver strict alapértelmezéssel elutasítja
a triggerbe célzást; explicit legacy aciklikus láncot warninggal enged,
ciklust/önhurkot soha. Az R12 integrált minősítését és az R6 tesztek végső
eredményét a külön [SL-2 review](map-streaming-sl2-review.md) adja; az
eredeti moving FAIL nem válik visszamenőleg PASS-szá.

| R | Jelenlegi kód / hívó | Reprodukció / bizonyíték | Csomag | Elfogadási teszt | Státusz |
|---|---|---|---|---|---|
| **R1** Chunk grid ≠ zone grid | `ParseManifest` (rács = ceil(size/chunk) tengelyenként), `LoadServerWorld` → `ServerTerrain` (chunkonkénti tárolás, részleges szélső chunk); legacy: `MapData.cpp` `LoadHeightField` (zone grid) | mapaudit R1: legacy REPRODUCED / szerver CHANGED; korpusz `index-*`, `chunk-file-missing`, `geometry-negative-origin-nonsquare-partial`; mapsplit `package_loads` változatlan 1→4→16→4→1 | MAP-1, MAP-2 (részleges chunk), MAP-3 (streaming) | (a) minden chunk pontosan egyszer ✓ (b) hiányzó chunk = hiba ✓ (indításkor streamingnél is: létezés + méret) (c) split/merge újratöltés nélkül ✓ — rezidens és streamelt terrainnel is (`streaming` mód: 1→4→16→4→1 lassú I/O mellett) | KÉSZ |
| **R2** Map Chunk ≠ Server Zone ≠ Client Map | v3 tiltja a `zoneGridDims`/`zoneSizeCells`-t; area-k metaadat (`AreaId`); `ZoneManager::BuildInitialPartition` a szerver-konfig rácsából (`ZoneId` 1..N) | mapaudit R2 CHANGED (test map: 3 area vs 4 szerver-zóna); mapsplit `ids-areas-vs-zones` (area {7, 9000} vs zóna {1}); indulási teszt 1d, 6a–6c | MAP-1, MAP-2 | a partícionálás kizárólag szerver-konfig + ASF; külön névterek ✓ | KÉSZ |
| **R3** WorldBounds / origin az adatból | bounds a manifestből (negatív origó, nem-négyzetes); régiók, load/activity field, terrain, validáció mind abból; nincs 100 km-es konstans | mapaudit R3 CHANGED; terrain `world-consumers-use-loaded-bounds`; mapsplit (−1280,−1024)–(1280,1024); indulási teszt 6a | MAP-2 | nem-nulla origójú, nem-négyzetes tesztvilág ✓ | KÉSZ |
| **R4** Worldlogic validáció | `ParseWorldLogic` + `ValidateWorldLogic` (area / spawn-régió (`areaId 0` = világszintű) / warp, origó-helyes, félig nyitott) + `ValidateAgainstTerrain` (warp-cél és spawn-közép járható) | mapaudit R4×2: legacy REPRODUCED / szerver CHANGED; korpusz worldlogic-esetek (415/416 új, 408/414 visszavonva) | MAP-1, MAP-2 | hibás-bemenet korpusz elutasítva, stabil kóddal ✓ | KÉSZ (szerver-út) |
| **R5** Csak szerver-releváns adat | `LoadServerWorld`: csak magasság + attribútum; splat Startup-ban csak tartomány, Full-ban szerkezet | mapaudit R5 CHANGED; korpusz `valid-v3-server-only`; indulási teszt 5; streaming budget (`streaming`, `streamsoak`) | MAP-1, MAP-3 (budget) | ✓ | KÉSZ |
| **R6** Warp: region-enter + cooldown | `WarpState`, `MovementSystem`, `EntityTransfer`, `MigrationSystem` / `MigrationCoordinator`: explicit rearm, 1 s cooldown, 5 s bounded/cancellable terrain wait, cross-zone migration | `map4`: chain, leave/reenter, timeout, cancellation, missing/blocked/outside target, half-open edge, 1/4 worker cross-zone + split/merge; `mapaudit` ténylegesen futtatja | MAP-4 | egyszeri teleport; állapotmegőrzés; nincs logáradat; load-time cycle rejection | SL-2 strict/legacy igazolt: package 140 PASS / 1 SKIPPED, MAP4 37 PASS, startup release és Debug 46/46; részletek: SL-2 review (formátum 13.) |
| **R7** Runtime map-útvonal | `ResolveWorldPlan` (CLI > config > default; config-relatív); + `partition_regions` / `partition_initial_leaves` | mapaudit R7 CHANGED; indulási teszt 1a–1c, 2e | MAP-1 | ✓ | KÉSZ |
| **R8** Nincs csendes fallback | fallback-utak törölve; exit 3 a DB/listen előtt; érvénytelen partíció-layout exit 2; a `BuildFromWorldLogic` üres-logika ága a függvénnyel együtt törölve | mapaudit R8 CHANGED; indulási teszt 2a–2f, 6b, 6c | MAP-1, MAP-2 | ✓ | KÉSZ |
| **R9** Félig nyitott határ | szerveren mindenhol `[min, max)`: világ, cella, chunk, partíció-levél (`FindLeaf`, gyökér), area-validáció, warp-trigger (`ContainsHalfOpen`), spawn; a legacy kliens `Rect::Contains` zárt marad (kliens-API) | mapaudit R9 CHANGED ((500,250) → pontosan egy zóna); terrain `world-edge-half-open`, `seams`, `not-resident-vs-zero-meters`; mapsplit tulajdonos-ellenőrzés a vágási vonalakon ±1 ulp | MAP-2 | közös élen egy pont pontosan egy gazdához ✓ | KÉSZ (szerver) |
| **R10** Bounds-on kívüli szabály | `TerrainStatus` (`OutsideWorld`/`NotResident`/`InvalidData`); mozgás clamp nélkül, a cellán belüli lépés is adatot kér; spawn-lánc + elutasítás; mob újrasorsol/kihagy, invalid adaton számolva; warp belül + járható; navigáció `InvalidData` ≠ `NoPath` | mapaudit R10 CHANGED; terrain `move-*`, `spawn-invalid-debug-*`, `mobs-inside-on-terrain`; streaming: `movement-waits-for-data-no-guess-stop-kept`, `failed-chunk-bounded-retry-then-invalid`; worldquery `path-check-same-cell-needs-data`, `nav-invalid-data-not-no-path`; streamlife `invalid-chunk-*`, `wake-after-eviction-waits-for-data-no-stale-z` | MAP-2, MAP-3 (NotResident / InvalidData futásidőben, review-utómunka) | dokumentált szabály minden lekérdezésben és fogyasztónál ✓ (`map-data-format.md` 9.3, `map3-report.md` 5.2) | KÉSZ |
| **R11** Strukturált hibák, verziózás | `PackageIssue` stabil kóddal; v2→v3 átmeneti szabály; height réteg v1/v2 | mapaudit R11 CHANGED; korpusz 119 eset | MAP-1, MAP-2 | ✓ | KÉSZ (szerver-út) |
| **R12** Chunk-streamelt szerver-terrain | `terrain_residency=streaming`: `TerrainStreamer` (I/O-szálak, állapotgép, admission/budget, LRU, pin, csendes-ablakos felszabadítás, generáció), zóna-igény + prefetch, induló halmaz pinnelve | mapaudit R12 CHANGED; `streaming` 27 eset; `streamlife` 10 eset (élettartam-stressz + negatív kontroll, késői completion reclaim / slot-reuse mellett); `streamsoak` 100 km / 60 s 16 MB-tal és kimerült 3 MB-tal (a számok: `map3-report.md` 4.4) | MAP-3 | világléptékű memória-mérés streameléssel ✓ (a 500p/7000p readiness a file-backed 100 km-es világon: MAP-4) | SL-1 célzott javítás + SL-2 C-moving-v1 Windows acceptance PASS; 16/3 MiB soak és teljes integráció mérve; eredeti moving churn FAIL; platform-/SLA-határok: SL-2 review |
| **R13** Spawn a validált csomag része | `mobSpawns` v2: stabil `spawn_id`, validált `area_id`; v1 explicit ordinal átmenet. `SpawnCoordinator` kezdeti aktiválása idempotens, `MobSpawnRef` és `EntityTransfer` megőrzi az id-t | `map4`: hiányzó/dupla/hibás id, area/type/bounds, v1/v2; runtime idempotence és split/merge; valódi startup 8a/8b | MAP-4 | validáció + runtime életciklus, terrain rendelkezésre állása aktiválás előtt | KÉSZ |

### MAP-2 review utómunka (2026-09-25)

A review a MAP-3 előtt négy pontosítást kért.

1. **A spawn sorrendjének ellentmondása — tisztázva.**
   - Az ellentmondás: a MAP-2 riport egyik helye (2.4, formátum 9.3) a
     „debug → spawn-régió → DB-pozíció" sorrendet írta, egy másik helye (6.)
     viszont azt, hogy „érvénytelen DB-pozíciójú karakter a spawn-régióba
     kerül" — ez DB-first sorrend.
   - A kód (és a MAP előtti viselkedés) spawn-régió-first volt; a DB lépés
     validált csomag mellett elérhetetlen volt, a 416-os szabály miatt.
   - **Döntés:** egyetlen sorrend van: debug felülírás (csak dev build) → a
     csomag **első** spawn-régiójának középpontja → elutasítás
     (`SERVER_ERROR`).
   - **A DB-pozíció nem jelölt,** két okból: a gameserver soha nem írja vissza
     (nincs perzisztencia-útja), és nincs világ-azonossága (`map_id` ≠ csomag
     `world_id`), így egy másik világ pontját nevezhetné meg.
   - Kód: `SpawnCoordinator::ResolveSpawnPosition` (a DB-ág és a
     `DbToMeters` törölve), új `PlayerSpawnRefusals()` számláló.
   - Tesztek (`terrain` mód):
     - `spawn-invalid-debug-falls-back-to-spawn-region`: érvényes, világon
       belüli DB-pozíció (300,200) mellett is a spawn-régió közepe;
     - `spawn-refused-without-usable-spawn-region`: a spawn-közép chunkja nem
       rezidens → a belépés elutasítva, bár a DB-pozíció érvényes; a debug
       felülírás közben működik. Ez a korábban nem futtatott elutasítási ág,
       most végrehajtva;
     - `mob-spawn-skips-missing-chunk`: 40/40 a rezidens körből, 0/40 a
       hiányzó chunkból.
2. **Nulla area és kötelező spawn — rögzítve.**
   - Korábban a „0 area megengedett", a „legalább egy spawn-régió kötelező"
     és a „spawn-régió létező area-ra hivatkozik" szabály együtt azt jelentette,
     hogy area nélküli csomag sosem lehet érvényes.
   - **Döntés:** `areaId = 0` = nincs area — világszintű spawn-régió, csak a
     világon belül kell lennie (`WORLDLOGIC_OUT_OF_BOUNDS`).
   - `areaId ≥ 1` → létező area (`…_REFERENCE_INVALID`), azon belül
     (`…_SPAWN_OUTSIDE_ZONE`).
   - Korábban érvénytelen tartalom válik érvényessé; érvényes tartalom
     jelentése nem változik.
   - Kód: `ValidateWorldLogic`.
   - Tesztek (`worldpackage`): `worldlogic-zero-areas-world-spawn-accepted`,
     `worldlogic-world-spawn-outside-world`; a meglévő
     `worldlogic-no-areas-no-spawn` (415) és `worldlogic-spawn-unknown-zone`
     változatlanul zöld.
3. **Összesített bootstrap-erőforráskorlát — igazolva és kikényszerítve.**
   - A tengelyenkénti korlátok (16 régió × 64 levél) együtt ~1M kezdeti
     zónát engedtek; a 240 m-es padló ezt csak a világmérettel korlátozta
     (~1.19M).
   - Mérés (`worldbench --mode bootstrap`, üres 100 km-es világ): ~1.3 MB
     working set zónánként.

     | Kezdeti zónák | Working set | Építés | Üresjárati supervisor-pass |
     |---|---|---|---|
     | 256 | 332 MB | 0.44 s | 0.18 ms |
     | 1024 | 1.3 GB | 1.75 s | 1.33 ms |
     | 4096 | 5.2 GB | 7.6 s | 77 ms (a 20 Hz tick fölött) |

   - **Döntés:** legfeljebb **1024** kezdeti zóna (`kMaxInitialLeafZones`),
     `BuildInitialPartition`-ben, tehát fájl- és szintetikus módban és explicit
     leveleknél is. A meglévő `scheduler` hardening-teszt 320 kezdeti zónát
     használ, ezért nem 256 lett a határ.
   - A szintetikus világ kiterjedése is a ±131072 m-es koordináta-tartományba
     került (korábban 1000 km-ig engedte).
   - A szintetikus mód partíciója is a DB előtt ellenőrzött.
   - Új `Bootstrap resources:` indulási logsor.
   - Teljes erőforrás-táblázat: `map-data-format.md` 10.3.
   - Tesztek:
     - `bootstrap` mód: `bootstrap-at-cap-within-budget` (1024 zóna: 1306 MB
       ≤ 2048, 1750 ms ≤ 5000, 1.33 ms ≤ 5), `initial-zone-cap-enforced`,
       `explicit-leaves-cap-enforced`, `synthetic-world-caps-enforced`;
     - indulási teszt 6c3 (1056 zóna → exit 2) és 6c4 (200 km-es szintetikus
       → exit 2).
   - **Nyitott (review):** a futásidejű zónaszámot a split növelheti, erre
     nincs összesített korlát, és a supervisor költsége szuperlineáris.
     Javaslat: futásidejű zónabudget (`zone-budget` split-elutasítás).
4. **A jóváhagyott height- és residency-szerződés a dokumentációban:**
   `map-data-format.md` 10:
   - 10.1 magasság;
   - 10.2 a jóváhagyott (resident) rezidencia;
   - 10.3 bootstrap-erőforrások;
   - 10.4 a MAP-3 kötelezettségei (még nem implementált).

Tesztek a review-utómunka után (RelWithDebInfo):

| Teszt | Eredmény |
|---|---|
| `terrain` | 20/20 |
| `worldpackage` | 119 PASS / 0 FAIL / 1 SKIPPED |
| `bootstrap` | 4/4 |
| indulási teszt | 24/24 |

### Döntést igénylő pontok a MAP-3 előtt

A MAP-2 előtti 1–4. pont (AreaId/ZoneId, origó/pontosság, részleges chunk,
R10) és a magassági tartomány a review-val lezárva. Nyitott:

1. **Futásidejű zónabudget:** a split-úton nincs összesített zónakorlát; a
   supervisor-pass 4096 zónánál 77 ms (lásd review-utómunka 3.).
2. **Warp-lánc:** R6 szerint a cél nem lehet trigger-régióban; most warning
   (a becsekkolt test_zone warp 1 célja (760,760) a warp 2 forrásának sarkán
   van). Hibává tétel esetén a test_zone tartalmát javítani kell (automatikus
   felülírás nélkül).
3. **Collision/víz/navigáció terjedelme (MAP-3):** attribútum-bitek (felület,
   víz, PvP) új réteg-verzióként vagy külön rétegként; útvonal-menti
   mozgás-ellenőrzés; a mobok a blokkolt cellát még figyelmen kívül hagyják;
   víz deklarációja; navigációs adat formátuma (jelenleg semmi).
4. **Streaming:** rezidencia-egység (zóna + ghost-sáv chunkjai), per-zóna és
   per-process budget, I/O-szál modell, sérült későn betöltött chunk kezelése
   (a `NotResident`/`InvalidData` futásidejű jelentése már rögzített).
5. **Teszt-térkép partícionálhatósága:** az 1 km-es test_zone alap 2x2
   partícióval 500 m-es leveleket kap, amelyek az alap
   `partition_min_zone_size_m=500` mellett nem splitelhetők (a gyerek 250 m
   lenne). Split-tesztekhez `partition_min_zone_size_m ≤ 250` kell (a
   `mapsplit` 240-nel futtatja) — vagy nagyobb teszt-térkép.
6. **Kliens és v3:** a kliens v2-n marad — konverter vagy kliens-v3 később.
7. **Mob-típus registry helye:** szerver-config (nem a csomag része) —
   megerősítés.
8. **Worldlogic formátum jövője:** MXL1 bináris marad (v2 area-típusokkal,
   overlay area-kkal), vagy a korábbi Cap'n Proto `MapLogic` koncepció?
9. **`scheduler` teszt:** a falióra-alapú párhuzamossági küszöb mindkét
   binárison ingadozik (MAP-1 óta ismert) — munkára normalizált kritérium.

## MAP-3 státusz (2026-09-25)

> **Review-döntés (MAP-2 után):** a MAP-3 terjedelme a közös chunk-cache, az
> aszinkron betöltés, a memória- és I/O-korlátok, valamint a minimális
> collision / navigation / water integráció. A végén STOP + review. Gameplay-
> és kliensfejlesztés nem része. A négy MAP-3 előtti pont:
> [MAP-2 review utómunka](#map-2-review-utómunka-2026-09-25).

### Mi készült

- **Formátum és loader** (`shared/map`):
  - `ResidencyMode` (eager | streaming) és `LoadOptions`.
  - Streaming indítás: minden chunk-fájl létezése és mérete ellenőrzött
    olvasás nélkül; az **induló halmaz** (spawn-régió-közepek, warp-célok)
    teljes ellenőrzéssel betöltődik.
  - `ChunkSource` / `PackageChunkSource`: szálbiztos egy-chunk betöltés, CRC,
    dekódolás.
  - `ServerTerrain` publikált slotokkal:
    - atomikus nyers mutató, olvasás zár nélkül;
    - `shared_ptr` tulajdon és pinek az író oldalon;
    - varrat-ellenőrzés a publikált szomszédokkal.
  - `Segment` és `CellIndexOf` az útellenőrzéshez és a navigációhoz.
  - A 2^28-as mintakorlát csak eagernél él; a chunk-index legfeljebb 2^20.
  - **Víz:** manifest `water` @18 (undeclared / none / seaLevel / bodies),
    MXWS v1 `waterBodies` réteg, `ServerWater` lekérdezés, hibakódok 600–604.
- **Gameserver** (`world/terrain/`):
  - `TerrainStreamer`: I/O-szálak, állapotgép, admission/budget, LRU,
    megtartási ablak, pin-tudatos eviction, csendes-ablakos felszabadítás,
    generáció + epoch, korlátos újrapróba és invalid publikálás, lemondás,
    leállítás, metrikák.
  - `TerrainDemandBuffer` zónánként: tick alatti igény, dedup, publikálás a
    tick végén.
  - A supervisor minden passban: igény-aggregálás → `Pump(quiescent)`.
  - `TerrainService::CheckStep`: útellenőrzés, lejtőkorlát, mélyvíz-szabály.
  - `NavigationService`: rács-A*, jobok, budget, lemondás, pinek,
    chunk-várakozás.
- **Futásidő:**
  - A mozgás (játékos és mob) a teljes úton ellenőriz, a mobokra is él a
    blokkoló rács, a lépés adatra vár, és prefetch-igényt ad.
  - Az induló mob-spawn budget-méretű kötegekben fut; a respawn korlátosan
    újrapróbál.
  - A warp-cél igénylődik; a debug spawn a spawn-régióra esik vissza.
  - A spawn-régió és a warp-cél chunkjai állandóan rezidensek (pinned).
- **Konfig:** `terrain_residency`, `terrain_cache_budget_mb`,
  `terrain_io_threads`, `terrain_max_in_flight`, `terrain_retain_seconds`,
  `movement_max_slope`, `movement_max_water_depth_m`. Indulási ellenőrzés:
  az induló halmaz nem fér a budgetbe → exit 2. Az indulási log kiírja a
  rezidenciát és a budgetet.

### Megvalósítási kategóriák (munkaprompt 16. pont)

| Szolgáltatás | Production hívóhoz kötött | Csak fixture/API szinten bizonyított | Részleges | Nem támogatott |
|---|---|---|---|---|
| Chunk-streaming, cache, budget, élettartam | ✓ `terrain_residency=streaming`; mozgás, spawn, warp, navigáció igényel | — | — | — |
| Static collision: blokkoló rács a lépés útján | ✓ játékos- és mob-mozgás (alapból aktív) | — | — | többszintes geometria (híd, barlang), statikus alakzatok, dinamikus akadályok |
| Lejtőkorlát | ✓ mozgás, konfigból (alapból ki) | — | csak felfelé, lépésvégpont-magasságból | — |
| Víz | ✓ a mozgás mélyvíz-szabálya, konfigból (alapból ki) | a lekérdezés-API (`Water`) | lokális vízterek csak tengelyigazított téglalapok | hajófizika, hullám, áramlás; a kliens MXWB maszkja |
| Navigáció | — (nincs gameplay-hívó: AI / aggro nem feladat) | ✓ `NavigationService`, `WorldRuntime::PostNavigationRequest` | a járhatósági rács a nav-adat (nincs navmesh-réteg) | navmesh-generálás, útvonal-policy |

### MAP-3 tesztek (RelWithDebInfo; Debug külön jelölve)

**`worldbench --mode streaming` — 24/24 PASS (Debug is).**

Streamer, szkriptelt forrással:
- hideg miss → hit, bitre azonos a referenciával;
- 100 kérő → 1 betöltés;
- out-of-order completion;
- a bukott chunk 3 próbálkozás után `InvalidData`, utána nincs több olvasás;
- valódi CRC-hiba betöltéskor;
- varrat-hiba elutasítva;
- I/O-párhuzamosság ≤ `io_threads`;
- túligény → korlátos várakozás; a budget csúcsa ≤ budget;
- eviction helyet ad az új területnek;
- pinnelt adat a budget fölött → nincs eviction, nincs túllépés;
- a pinek elengedése után a terület tovább halad;
- evicted chunk csak csendes ablakban szabadul fel;
- az utolsó fogyasztó lemondása mellett a többi fogyasztó betöltése megy;
- a késői completion generáció-reset után elutasítva;
- leállítás olvasás közben: 64 ms, semmi nem publikálódik;
- csendes ablakon kívül nincs túl-eviction (3 eviction a 10 helyett).

Runtime, streaming világ, eager referenciával:
- induló spawn kötegekben: 120/120 mob, csúcs ≤ budget;
- chunkhatárt keresztező játékosok, z bitre egyezik;
- a mintavételezett lekérdezések vagy bitre egyeznek, vagy `NotResident`;
- 1→4→16→4→1 30 ms-os I/O mellett: azonos entitáshalmaz, validátor OK,
  nincs csomag-újratöltés;
- futásidejű generáció-reset: stale elutasítva;
- leállítás függő betöltésekkel: 190 ms;
- prefetch nélkül a mozgás vár (11 lépés), majd továbbmegy, és a megállás
  megmarad.

**`worldbench --mode worldquery` — 24/24 PASS (Debug is).**

Collision:
- 2.2 m-es lépés 0.5 m-es falon át: az út blokkolt, csak a célpontot nézve
  átmenne;
- chunkhatár (tiszta és blokkolt);
- nincs sarokvágás;
- lejtőkorlát (csak felfelé);
- `NotResident` és határon kívüli pont az úton.

Víz:
- undeclared = `UnsupportedLayer` (2000/2000);
- none = `NoWater`;
- tengerszint: mélység-orákulum 2000/2000;
- vízterek: felszín és mélység, a száraz medence `NoWater`;
- nem rezidens talaj = `NotResident`;
- mélyvíz-szabály;
- 4 csomagszabály (átfedés, határon kívül, NaN tengerszint, deklaráció vs
  réteg).

Navigáció:
- ablakon belül nincs út;
- kerülőút = Dijkstra-orákulum (3185.47 m);
- 2.3 km-es, chunkhatárokon átívelő út = orákulum;
- budget, lemondás, határon kívüli cél, sík világ = unsupported;
- streamingnél vár a chunkokra, pineket tart, ugyanaz az eredmény;
- blokkolt forrás → korlátos várakozás → `NotResident`.

Runtime:
- a falnál megáll (511.90);
- a partnál megáll (−1800.20);
- `PostNavigationRequest` → `Found`.

**`worldbench --mode streamsoak --cycles 60` — 3/3 PASS:** a számok a
formátum-szerződés 12.4 pontjában.

**Indulási teszt — 27/27 PASS (valódi `gameserver`, DB-vel):**
- 7a: streaming indítás; a logban `terrain residency=streaming`;
- 7b: érvénytelen rezidencia-érték → exit 2;
- 7c: 8192-chunkos index 1 MB-os budget fölött → exit 2.

**Talált és javított hiba:** Debugban a `mapsplit` 5 futásból 2-szer
elbukott: `activity: duplicate source net 1`.
- Ok: migráció, split/merge-transzfer vagy despawn után a forrás-zóna
  publikált activity-forrása a következő tickig megmaradt; az 1 Hz-es
  újraépítés a célzónáéval együtt kétszer látta a játékost. Meglévő
  (MAP-előtti) verseny, amelyet a lassabb Debug időzítés hozott elő.
- Javítás: `Zone::ExtractPlayerBinding` a kötéssel együtt a játékos
  publikált forrását is eltávolítja. Utána 8/8 Debug futás zöld.

**Második talált hiba (a MAP-3 saját kódja, kód-átnézésben):** túl-eviction
csendes ablakon kívül.
- Ok: egy evicted chunk bájtjai a felszabadításig könyvelve maradnak, ezért
  egy nem-csendes pass minden evictálhatót kidobott.
- Javítás: a `MakeRoom` a vetített könyvelést (a retired bájtok nélkül)
  használja.
- Teszt: `no-over-eviction-outside-quiescent-window`.
- A soak eviction-száma utána 861 helyett 796.

**Teljes regresszió:**

RelWithDebInfo, a `worldbench_hardening.exe` (0efc4033) bináris ellen, a párok egymás után futtatva.

**Base vs új párok:** mind a 22 azonos PASS-számmal.
- A readiness új binárison +1 (`snapshot-quiescent`), mint a MAP-1 óta.
- A `scheduler` ebben a futásban mindkét binárison 13/13.
- A review-utómunka előtti köztes futásban ugyanez a teszt **mindkét**
  binárison elbukott (base 2, új 1 FAIL). Ez az ismert falióra-alapú
  ingadozás; nem számoljuk PASS-nak.

**Csak-új módok, mind zöld:** tickrate, inputpath, netstress ×3, presence,
asfdeterminism, workerpool, replv2, protocol, reclamation 100/1000, hygiene,
mapaudit, snapshot (300), worldpackage 119, terrain 20, mapsplit 21,
bootstrap 4, streaming 23, worldquery 24, streamsoak 3.

**Utána módosult két dolog** (a túl-eviction javítása és az R12 mapaudit-próba),
ezért a végső binárison újrafutott:
- streaming 24/24 (az új `no-over-eviction` esettel);
- worldquery 24/24; streamsoak 3/3; bootstrap 4/4; terrain 20; mapsplit 21;
  worldpackage 119; snapshot (100) 8/8;
- mapaudit: server_changed=12, open/partial=2;
- Debug: streaming 24/24, worldquery 24/24;
- indulási teszt 27/27.

A túl-eviction javítás csak streaming-módú kódot érint; a regressziós párok
eager / szintetikus világon futnak.

**Readiness tick (ms), base → új:**

| Futás | p50 | p95 | p99 |
|---|---|---|---|
| dense 500p / 200k | 4.12 → 4.14 | 4.75 → 4.89 | 66.0 → 65.8 |
| spread 7000p / 200k | 2.00 → 1.95 | 7.87 → 7.96 | 9.49 → 9.30 |

A smoke p50 a regressziós futásban 1.02 → 1.69 volt. A végső binárison
három ismételt pár: base 1.03–1.22, új 0.88–1.18 ms, a p95/p99 egyező —
tehát zaj.

**1024 kezdeti zóna, üresjárati supervisor-pass:** 0.50 ms a végső binárison
(a korábbi 1.33 ms-os mérés egy másik futásból származik; a zajszint
ekkora).

### MAP-3 review utómunka (2026-09-25)

> **Review-státusz:** az architekturális irány megfelelő. A végleges
> elfogadáshoz négy bizonyíték kellett: a zár nélküli olvasás élettartama, a
> valódi streamingterhelés és budget, a `NotResident` / `InvalidData`
> futásidejű kezelése, valamint az ASF, a streaming és a reclamation együtt.
> A teljes riport: [`map3-report.md`](map3-report.md).

| Review-pont | Eredmény | Bizonyíték |
|---|---|---|
| 1. Élettartam | happens-before lánc a tick-jelzőn (a kiadás előtt beállítva, release a tick végén, acquire a felszabadítás előtt); pint csak az író szál vesz (Debugban assert); a completion nem tart zónát | `streamlife`: tick-olvasó stressz mérgezett felszabadítással, 0 eltérés; negatív kontroll: tömeges eltérés (`map3-report.md` 3.7) |
| 2. Budget | könyvelés: resident + in-flight + retired + metaadat; csúcsok, sor-csúcsok, admission; a munkakészletnél kisebb budget mérve | `streamsoak` 16 MB és 3 MB; `waiting-list-full-rejects-explicitly`; `startup-set-above-budget-detected`; indulási teszt 7c |
| 3. `NotResident` / `InvalidData` | fogyasztónkénti táblázat; a „Failed → újrapróba → invalid" pontos szemantikája | `map3-report.md` 5.1–5.3; 12 teszteset |
| 4. ASF + streaming + reclamation | függő (gated) betöltés 3 × split+merge, reclaim és slot-reuse alatt; a késői completion elfogadva | `late-completion-after-migration-retire-reclaim-slot-reuse`; alvó zóna ébresztése |

**Talált és javított hibák** (részletek: `map3-report.md` 9.2):
1. cellán belüli lépés hiányzó adaton, elavult z-vel (visszaellenőrizve: a
   javítás nélkül a tesztek elbuknak);
2. publikált chunk felülírása azonnali felszabadítással (generáció-reset +
   invalid chunk);
3. invalid szomszéd üres tömbjének indexelése (varrat, `Vertex`);
4. elnyelt I/O-ébresztés (a completion a tétlen timeoutig várt);
5. a kötegelt kezdő spawn szűk budgetnél 0 mobot adott;
6. kimerült budgetnél O(várakozó × chunk) supervisor-pass;
7. navigáció: `InvalidData` `NoPath` helyett, korlátlan jobszám.

**Tesztek a végső binárison:**
- RelWithDebInfo:
  - `streamlife` 10/10 (új mód);
  - `streaming` 27/27 (+3: `neighbour-of-invalid-chunk-loads`,
    `invalid-chunk-generation-reset-retired-then-valid-reload`,
    `waiting-list-full-rejects-explicitly`);
  - `worldquery` 27/27 (+3: `path-check-same-cell-needs-data`,
    `nav-invalid-data-not-no-path`, `nav-job-cap-rejects-explicitly`);
  - `streamsoak` 16 MB és 3 MB, 3/3 és 3/3.
- Debug: `streamlife` 10/10, `streaming` 27/27, `worldquery` 27/27; az
  író-szál assert aktív, és nem sült el.
- Indulási teszt 27/27.
- **Élettartam-stressz:** 775 M mintaolvasás, 0 eltérés; 437 chunk került
  ki a slotból, miközben egy olvasó tartotta. A negatív kontroll 7.3 M
  eltérést talált.

**Teljes regresszió** (`worldbench_hardening.exe` 0efc4033 ellen):
- 22 pár: 19 azonos PASS-számú, a readiness +1 (`snapshot-quiescent`), a
  `scheduler` 13/13 mindkét binárison.
- Minden csak-új mód zöld.
- A tick-számok: `map3-report.md` 8.2.

**Nem futott:** ASan/TSan; Linux/FreeBSD (nincs elérhető környezet).

### Döntést igénylő pontok a MAP-4 előtt

1. **Futásidejű zónabudget:** a split-úton nincs összesített zónakorlát
   (review-utómunka 3.).
2. **Warp-szemantika (MAP-4):** belépési él, cooldown, cross-zone warp,
   függő kérés. Jelenleg a warp-cél chunkja pinnelt, és a trigger tickenként
   értékelődik.
3. **Lejtő- és mélyvíz-szabály alapértéke:** gameplay-látható, ezért most
   alapból ki van kapcsolva. Kell-e és milyen értékkel bekapcsolni?
4. **Navigáció fogyasztója:** nincs gameplay-hívó. Kell-e navmesh-réteg,
   vagy elég a rács?
5. **Manifest-méret:** az 1 MiB-os manifest kb. 16 ezer chunkos indexet
   enged (100 km 1 km-es chunkokkal elfér; 2 m-es cellákhoz külön indexfájl
   kellene).
6. **A 500p/7000p readiness a file-backed 100 km-es világon:** MAP-4
   (munkaprompt 21. pont).
7. **Munkakészlet > budget:** nincs éhezés elleni garancia az igénysorrendű
   várólistán túl. Kell-e prioritás, például a játékos-igény a mob-igény
   előtt?
8. **Sanitizer-futás** (ASan/TSan, clang, Linux/FreeBSD): kell-e a MAP-4
   előtt? Windowson nem futott.
9. A MAP-3 előtti listából nyitott maradt: warp-lánc, kliens és v3,
   mob-típus registry helye, worldlogic formátum jövője, `scheduler` teszt.

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

## MAP-4 státusz (2026-09-27)

A MAP-4 a MAP-3 review után, a felhasználó kifejezett kérésére folytatódott.
A warp és a spawn implementációja, a v2 spawnformátum, a runtime tesztek,
a fájlos readiness út és a reprodukálható regressziós script elkészült.
Részletes, a korábbi történeti méréseket nem felülíró átadás:
[`map4-report.md`](map4-report.md). A végső teszt- és mérési státuszt az a
riport tartalmazza.

A MAP-4 előtti döntési listából a warp-szabály lezárt (formátum 13. pont).
A lejtő/mélyvíz alapértékei, a gameplay nélküli grid-A* API, a manifest
méretkorlátja és a futásidejű ASF összesített zónakorlát kérdése továbbra is
a dokumentált MAP-3 határok szerint értendők. A cache nem garantál
éhezésmentességet a munkakészletnél kisebb budgettel; ezt a mérésekben
külön kell megmutatni. Gameplay, kliens, login és DB-fejlesztés nem része
ennek a csomagnak.
