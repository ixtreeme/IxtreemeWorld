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
