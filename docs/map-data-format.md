# Világcsomag-formátum — kötelező kontraktus (MAP-1 + MAP-2)

> **Státusz: kötelező, a kóddal egyező szerződés.** Ez a dokumentum a
> `shared/map` tényleges írójának (`WorldPackageWriter`) és szigorú
> olvasójának (`WorldPackage`, `LoadServerWorld`, `ServerTerrain`) leírása.
> Ahol a kód és ez a leírás eltér, az hiba. A korábbi (clean-room) koncepció el
> nem készült részei a [C. függelékben](#c-függelék--a-korábbi-koncepció-nem-implementált)
> vannak, kifejezetten **nem** érvényes formátumként.
>
> **Jogi megjegyzés (nem jogi tanács):** a clean-room cél a saját konténer +
> saját séma + saját kód + saját/licencelt asszetek. A magic-ek (`MXC1`,
> `MXL1`, `MXWB`) és az elrendezések saját tervezésűek.
>
> Követelmények és nyomkövetés: [`map-data-layer-requirements.md`](map-data-layer-requirements.md)
> (R1–R13, MAP-0/MAP-1/MAP-2 mátrix). Bizonyíték: `worldbench --mode worldpackage`
> (korpusz), `--mode terrain` (független magasság-orákulum + futásidejű
> perem-szabályok), `--mode mapsplit` (split/merge valódi loaderből),
> `--mode mapaudit` (legacy vs szerver út),
> `gameserver/scripts/map1_startup_acceptance.sh` (valódi indulási út).

## 1. Alapelvek

1. **Önleíró csomag.** A `map.manifest` rögzíti az azonosítót, a geometriát
   (origó, méret tengelyenként, cellaméret, chunk-rács), a magasság-kódolást, a
   rétegeket és a hivatkozásokat. Semmi nem jön fordításkori útvonalból vagy
   implicit konvencióból.
2. **Chunk ≠ area ≠ szerver-zóna ≠ kliens-map.** A formátum nem tartalmaz
   szerver-zóna fogalmat (v3-ban a `zoneGridDims`/`zoneSizeCells` tilos). A
   worldlogic „zone" rekordjai **area**-k (saját `AreaId` névtér, tervezési
   metaadat); a szerver partíciója a szerver konfigjából + a betöltött
   bounds-ból jön (`partition_regions`, `partition_initial_leaves`), a
   futásidejű az ASF-ből (MAP-2). A chunk-rács tárolási egység, nem zóna.
3. **Közönség (audience) rétegenként.** A szerver csak a szerver/shared
   rétegeket dekódolja és tartja memóriában (magasság, attribútum, worldlogic,
   spawn-tábla). Kliens-render adat (splat, textúra-paletta, víz-vizuál) soha
   nem kötelező a szervernek, és nem foglal szerver-memóriát.
4. **Hordozható bináris.** Minden bináris mező little-endian, mezőnként
   írva/olvasva; fordítófüggő struktúra-memória soha nem formátum.
5. **Nincs csendes értelmezés.** Ismeretlen/újabb verzió, nem támogatott
   kötelező réteg, sérült vagy hiányzó adat strukturált hiba; nincs lapos
   fallback-világ, és a hiányzó adat soha nem „0 m" vagy „járható".

## 2. Csomag-struktúra

```
<csomag>/
  map.manifest                 # Cap'n Proto MapManifest (unpacked, szegmenstáblával)
  chunks/chunk_<x>_<y>.mxchunk # MXC1 konténer (v3-ban a manifest chunk-indexe nevezi meg)
  worldlogic.dat               # MXL1 v1 (worldLogic réteg)
  mob_spawns.conf              # mob_spawns v1 szöveg (mobSpawns réteg, opcionális)
  water_bodies.mxwater         # MXWB (kliens; a szerver nem validálja)
  world_palette.json           # kliens-szerkesztő adata (a csomag-formátumon kívül)
```

**Hivatkozási szabály (minden csomagon belüli útvonal):** relatív, `/`
elválasztó, legfeljebb 512 bájt; tilos: üres komponens, `.`/`..` komponens,
abszolút út (`/…`), `\`, `:` (meghajtó/stream), vezérlőkarakter. Feloldás után a
kanonikus útnak (szimbolikus link/junction feloldva) **komponensenként** a
kanonikus csomaggyökér alatt kell lennie — nem string-prefix összevetés.
Sértés: `PATH_INVALID` (105) vagy `PATH_OUTSIDE_PACKAGE` (106).

**Fájlméret-korlátok (olvasás előtt):** manifest ≤ 1 MiB, chunk ≤ 256 MiB,
worldlogic ≤ 16 MiB, spawn-tábla ≤ 16 MiB (`FILE_TOO_LARGE`).

## 3. Manifest (`map.manifest`, Cap'n Proto)

Séma: `shared/map/schema/map_manifest.capnp`. A v3 mezők csak hozzáfűzéssel
kerültek be (@12..@18), így egy v2-olvasó hiba nélkül parse-ol egy v3 üzenetet —
ezért **a v2-olvasónak kötelező elutasítania a `formatVersion != 2`-t** (a
legacy `LoadManifest` ezt teszi). Egy `formatVersion = 2` manifest, amely
@12..@18 bármelyikét hordozza, kétértelmű → `MANIFEST_FIELD_FORBIDDEN`.

| Mező | v2 (legacy, csak olvasás) | v3 (aktuális) |
|---|---|---|
| `formatVersion` @0 | 2 | 3 |
| `worldId` @1 | 1..64 karakter `[A-Za-z0-9_.-]` | ugyanaz |
| `worldName` @2 | ≤ 256 bájt | ugyanaz |
| `worldSizeCells` @3 | cellák X-en (és Y-on) | cellák **X-en** (kelet), 1..65536 |
| `cellSizeMeters` @4 | véges, 0.05..1000 | ugyanaz |
| `heightUnit` @5 | `centimeters` | `centimeters` (height v1; v2/v3-ban a `heightEncoding` dönt) |
| `chunkSizeCells` @6 | 1..4096 | ugyanaz |
| `zoneGridDims` @7 | **figyelmen kívül hagyva** (a régi generátor a chunk-számot írta ide; a legacy betöltő chunk-rácsként olvasta — R1) | **0 kötelező** (`MANIFEST_FIELD_FORBIDDEN`) |
| `zoneSizeCells` @8 | figyelmen kívül hagyva | **0 kötelező** |
| `texturePalette` @9 | kliens-adat, a szerver nem tölti | ugyanaz |
| `worldLogicFile` @10 | üres = `worldlogic.dat` | **üres kötelező** (a rétegekből jön) |
| `environmentFile` @11 | nincs fogyasztója (info) | **üres kötelező** |
| `worldSizeCellsY` @12 | tilos (kétértelmű) | cellák **Y-on** (észak), 1..65536; független X-től |
| `origin` @13 | tilos | `Float64` x,y: a (0,0) cella dél-nyugati sarka, méterben; **véges** (negatív is), `MANIFEST_FIELD_INVALID` |
| `chunkGrid` @14 | tilos | = ceil(worldSize / chunkSize) tengelyenként |
| `layers` @15 | tilos (v2-ben implicit, lásd lent) | réteg-deklarációk (≤ 32) |
| `chunks` @16 | tilos | pontosan egy bejegyzés rácscellánként |
| `heightEncoding` @17 | tilos | height v2/v3-nál kötelező, v1-nél tilos (3.4) |
| `water` @18 (MAP-3) | tilos | a víz-képesség deklarációja (3.5); hiánya = ismeretlen |

Az üzenet legyen pontosan egy Cap'n Proto üzenet (mögötte nincs bájt),
8-bájtos szavak egész száma; a dekódolás korlátos (traversal ≤ 4× méret,
nesting ≤ 16). Hiba: `MANIFEST_CORRUPT` (200).

**Geometria.** A világ `[originX, originX + worldSizeCells·cell) ×
[originY, originY + worldSizeCellsY·cell)` — félig nyitott, lásd 9. pont.
Chunk-rács = `ceil(worldSize / chunkSizeCells)` tengelyenként, független a
minta- és a cellaszámtól. A tengelyenként utolsó chunk **részleges** lehet
(v3): csak a világon belüli celláit tárolja (4. pont). v2-ben a világ a
chunkméret egész többszöröse (részleges chunk: `UNSUPPORTED_FEATURE`). Minden
világkoordináta (mindkét sarok) `|c| ≤ 131072 m` (`kMaxWorldCoordinate`; a
futásidejű pozíció `f32`, a lépésköz itt ≤ 1/64 m) — különben
`UNSUPPORTED_FEATURE`. A chunk-index legfeljebb 2^20 chunk
(`MANIFEST_SIZE_OVERFLOW`; a gyakorlati határt a ≤ 1 MiB-os manifest adja:
chunkonként ~64 bájt indexbejegyzéssel kb. 16 ezer chunk). **Eager**
rezidenciánál (10.2) a magasságminta `(W+1)×(H+1) ≤ 2^28`
(`MANIFEST_SIZE_OVERFLOW`); **streaming** rezidenciánál ez nem korlát, a
memóriát a terrain-budget korlátozza (12.).

### 3.1 Rétegek (`layers`, v3)

```
LayerDecl { kind: LayerKind; required: Bool; audience: server|client|shared;
            version: UInt32; file: Text }
```

| kind | Tárolás | Elemformátum | Szerver |
|---|---|---|---|
| `height` (0) | chunk-szekció 1 | v1: int16 cm; v2/v3: int16 vagy int32 a `heightEncoding` szerint; `(cx+1)×(cy+1)` minta | kötelező, betölti |
| `attributes` (1) | chunk-szekció 3 | uint16 bitmező, `cx×cy` cella | kötelező, betölti |
| `splatA` (2) / `splatB` (3) | chunk-szekció 2 / 4 | u16 w, u16 h, RGBA8 | kihagyja (Startup: csak tartomány; Full: szerkezet) |
| `worldLogic` (4) | fájl | MXL1 v1 | kötelező, betölti |
| `mobSpawns` (5) | fájl | mob_spawns v1 | opcionális; ha van, szigorúan betölti |
| `water` (6) | fájl | MXWB (kliens-render adat) | nem validálja, nem olvassa (`LAYER_NOT_VALIDATED` info) |
| `waterBodies` (7) | fájl | MXWS v1 (5a.) | a manifest `water.model = bodies` mellett kötelező; szigorúan betölti |

(`cx`, `cy` = az adott chunk cellái; teljes chunknál = `chunkSizeCells`.)

Szabályok: egy `kind` legfeljebb egyszer (`LAYER_DUPLICATE`); chunk-szekció
rétegnek nincs `file`-ja, fájl-rétegnek van (`LAYER_DECL_INVALID`); szerver-adat
(height/attributes/worldLogic/mobSpawns) nem lehet `client` közönségű
(`LAYER_DECL_INVALID`); a szerver számára a height, attributes és worldLogic
jelenléte kötelező (`LAYER_REQUIRED_MISSING`). Ismeretlen `kind` vagy nem
támogatott `version` **kötelező** rétegnél hiba (`LAYER_UNSUPPORTED`),
**opcionálisnál** dokumentált kihagyás (`LAYER_SKIPPED`: ismeretlen kind = info,
ismeretlen verzió = warning). Réteg-verziók: `height` 1..2, minden más 1.

### 3.2 Chunk-index (`chunks`, v3)

```
ChunkRef { x, y: UInt32; file: Text; byteSize: UInt64; crc32: UInt32 }
```
Rácscellánként pontosan egy bejegyzés, a rácson belül, egyedi fájllal
(`CHUNK_INDEX_INVALID`). A chunk kulcsa `(x, y)` a chunk-rácsban (stabil
azonosító; `x` kelet felé, `y` észak felé nő). A fájlméret egyezzen
`byteSize`-zal (`CHUNK_SIZE_MISMATCH`), a teljes fájl CRC-32-je (IEEE 802.3,
azonos a zlib `crc32()`-vel) `crc32`-vel (`CHUNK_CHECKSUM_MISMATCH`).

### 3.3 v2 → v3 átmeneti szabály (a v2 **nem** értelmeződik csendben v3-ként)

Az olvasó a v2-t kizárólag az alábbi rögzített szabállyal fogadja:
- geometria: négyzetes (`worldSizeCells` mindkét tengelyen), origó (0,0),
  egész chunkok, height v1;
- chunkok: `ceil(worldSizeCells/chunkSizeCells)²` darab, fájlnév
  `chunks/chunk_<x>_<y>.mxchunk`; a `zoneGridDims` csak info (ha eltér a
  chunk-rácstól: `LEGACY_FIELD_IGNORED` warning — a legacy betöltő mást
  olvasna be);
- implicit rétegek: height + attributes (kötelező, shared), splatA/B
  (opcionális, client), worldLogic (`worldLogicFile` vagy `worldlogic.dat`,
  kötelező), mobSpawns (`mob_spawns.conf`, opcionális);
- nincs index → méret/CRC nem ellenőrizhető (`CHUNK_INTEGRITY_UNAVAILABLE` info);
- MXC1 ismeretlen szekciótípus v2-ben warning (v3-ban hiba); foglalt
  attribútumbit v2-ben warning (v3-ban hiba).

A legacy `MapData.h` API (kliens renderer/szerkesztő) változatlan: csak v2-t
olvas, v3-at elutasít. Az írók (mapgen, fixture-ök) v3-at írnak; a
`mapgen_test_zone --format 2` a v2-csak kliens miatt marad. A kliens-szerkesztő
a v2 chunkokat helyben írja (`SaveChunkHeights`); v3 csomag szerkesztése
újraindexelést igényel (tooling-döntés, lásd a követelmény-dokumentum
döntési pontjait).

### 3.4 Magasság-kódolás és felület (height réteg v1 / v2 / v3)

`méter = offsetMeters + raw · metersPerUnit` (double-ben számolva).

| height réteg | `heightEncoding` @17 | Minta | Tartomány |
|---|---|---|---|
| **v1** (történeti) | **tilos** (`MANIFEST_FIELD_FORBIDDEN`) | int16, 0.01 m/egység, offset 0 | ±327.67 m |
| **v2** (MAP-2 bővítés) | **kötelező** (`MANIFEST_FIELD_INVALID`, ha hiányzik) | `sampleType`: `int16` (0) vagy `int32` (1); `metersPerUnit` véges, 0.0001..10; `offsetMeters` véges, \|·\| ≤ 100000 | pl. int32 × 1 mm − 500 m: ≈ −2.1 … +2.1 ezer km; int16 × 5 cm + 1000 m: −638 … +2638 m |
| **v3** (engine terrain export) | **kötelező**, explicit `interpolation=triangleMainDiagonal` (1) | v2 mintatípus, skála és offset; új felületszerződés | azonos tárolási tartomány; editor export: int32 × 0.0001 m, offset 0 |

A chunk height-szekció `elementFormat`-ja kövesse a mintatípust (1 = int16,
2 = int32; eltérés: `CHUNK_TOC_INVALID`), a szekció mérete
`(cx+1)(cy+1)·2` ill. `·4` bájt (`CHUNK_SECTION_SIZE`). A v1 csomagok
változatlanul érvényesek; height v2/v3-at csak a manifest v3 formátum hordozhat.

`HeightEncoding.interpolation @3` enum: `bilinear` (0, alapértelmezett) vagy
`triangleMainDiagonal` (1). A height v1/v2 lekérdezés továbbra is bilineáris;
height v2-ben az 1 érték tiltott. Height v1-ben az egész `heightEncoding`
tiltott. Height v3-ban az 1 érték explicit kötelező; hiányzó mező/0, ismeretlen
enum vagy optional height v3 deklaráció elutasított. A writer mindig
**required height v3** deklarációt ír a háromszögmódhoz, ezért a csak height
v1/v2-t ismerő régi szerver nem értelmezheti bilineárisan az új csomagot.
A manifest verziója és a chunk-konténer verziója nem változik.

A canonical SW, SE, NW, NE sarkok magassága rendre `h00,h10,h01,h11`.
A v3 felület SW–NE átlóval két háromszög:

```text
fx >= fy: h00 + (h10-h00)*fx + (h11-h10)*fy
fx <  fy: h00 + (h11-h01)*fx + (h01-h00)*fy
```

Az engine északról délre tárolt mintasorait az export megfordítja; a Jolt
source `v10–v01` átlója ekkor a canonical `h00–h11` átló lesz. Ez a
felületszerződés nem teszi exact aritmetikává a float motor-koordinátákat.
Az editor külön jelenti a vertexmagasság tárolási kvantálását és a float/double
rácspozíció legnagyobb eltérését; egyik sem teljes collision-hibagarancia.

### 3.5 Víz-képesség (`water` @18, MAP-3)

```
WaterDecl { model: WaterModel; seaLevelMeters: Float64 }
WaterModel = undeclared (0) | none (1) | seaLevel (2) | bodies (3)
```

| model | Jelentés | Szerver-lekérdezés (12.2) |
|---|---|---|
| hiányzik / `undeclared` | a csomag nem nyilatkozik a vízről | `UnsupportedLayer` — **soha nem „szárazföld"** |
| `none` | a világban nincs víz | `NoWater` |
| `seaLevel` | egy globális felszín `seaLevelMeters`-en (véges, \|·\| ≤ 100000; `MANIFEST_FIELD_INVALID`) | víz, ahol a talaj a felszín alatt van |
| `bodies` | lokális vízterek a `waterBodies` rétegből (5a.) | víz a víztesten belül, ahol a talaj a felszín alatt van |

`model = bodies` ⇔ deklarált `waterBodies` réteg; bármelyik oldal hiánya
`WATER_DECL_INVALID` (600). A kliens MXWB rétege (`water`, 6) sosem
számít szerver-víznek.

## 4. Chunk-konténer (`.mxchunk`, MXC1, little-endian)

### Fejléc (14 bájt)
| Offset | Méret | Mező | Szabály |
|---|---|---|---|
| 0 | u32 | magic | `0x3143584d` (`"MXC1"`) |
| 4 | u16 | version | `2` (konténer-verzió) |
| 6 | u16 | chunkX | = index / fájlnév szerinti x |
| 8 | u16 | chunkY | = y |
| 10 | u16 | cellsPerSide | = `chunkSizeCells` (a **névleges** chunk-lépték; részleges chunkban is) |
| 12 | u16 | sectionCount | 1..16 |

Hiba: `CHUNK_HEADER_INVALID` (300), TOC-hiba `CHUNK_TOC_INVALID` (301).

### Szekció-tábla (`sectionCount × 12` bájt, a fejléc után)
| Offset | Méret | Mező |
|---|---|---|
| 0 | u16 | sectionType (1 height, 2 splatA, 3 attributes, 4 splatB) |
| 2 | u8 | elementFormat (1 int16, 2 int32 [height v2/v3], 3 u16-bitmező, 4 RGBA8-kép) — típushoz kötött |
| 3 | u8 | reserved (v3: 0) |
| 4 | u32 | byteOffset (fájl elejétől) |
| 8 | u32 | byteLength |

Szabályok: egy típus legfeljebb egyszer; minden szekció a tábla után és a
fájlon belül; **a tábla utáni minden bájt pontosan egy szekcióhoz tartozik**
(nincs átfedés, rés, záró szemét) — `CHUNK_SECTION_RANGE` (302). v3-ban csak
deklarált réteg szekciója szerepelhet; deklarált splat-réteg minden chunkban
jelen van (`CHUNK_SECTION_MISSING`).

### Szekciók
A chunk `(x, y)` cellái: `cx = min(chunkSizeCells, worldSizeCells − x·chunkSizeCells)`,
`cy` ugyanígy Y-on — a tengelyenként utolsó chunk **részleges** lehet, és
kizárólag a világon belüli celláit tárolja (nincs világon kívüli minta).
- **Height (1):** `(cx+1)×(cy+1)` minta (3.4), soronként (y külső, x belső). A
  szomszédos chunkok közös élmintái **azonosak** (`CHUNK_EDGE_MISMATCH` 307;
  nincs „utolsó író nyer").
- **Attributes (3):** `cx×cy` uint16. **bit 0 = blokkolt (nem járható)**;
  bit 1..15 foglalt = 0 (`CHUNK_ATTRIBUTE_RESERVED_BITS`). A szerver
  járhatósága kizárólag ebből jön.
- **SplatA/B (2/4, kliens):** `u16 w, u16 h`, majd `w·h·4` bájt RGBA8;
  A és B geometriája azonos, minden chunkban azonos (`CHUNK_SPLAT_GEOMETRY`,
  csak Full mélységben dekódolva).
- Méretek: height `(cx+1)(cy+1)·2` (int32: `·4`), attributes `cx·cy·2` bájt
  (`CHUNK_SECTION_SIZE`).

## 5. Worldlogic (`worldlogic.dat`, MXL1 v1, little-endian)

```
u32 magic = 0x314c584d ("MXL1")    u32 version = 1
u32 areaCount   u32 spawnCount   u32 warpCount        (mindegyik ≤ 1024)
area  × areaCount : u32 id, u8 nameLen, nameLen bájt név, f32 minX, minY, maxX, maxY
spawn × spawnCount: u32 id, u32 areaId, f32 minX, minY, maxX, maxY
warp  × warpCount : u32 id, f32 minX, minY, maxX, maxY, f32 targetX, targetY
```
(A bájt-elrendezés a MAP-1-es „zone" rekordokkal azonos; a rekord jelentése
változott: **area**.) A fájl pontosan a rekordok végéig tart
(`WORLDLOGIC_TRUNCATED` 401, `WORLDLOGIC_TRAILING_DATA` 402, fejléc:
`WORLDLOGIC_HEADER_INVALID` 400).

Validáció (a világ a manifest geometriája, félig nyitott, origó-helyes):
- **area-k** (tervezési metaadat, `AreaId` névtér — **nem** szerver-zóna):
  lehet 0 darab is; id ≥ 1 (0 = „nincs area"), a teljes u32 tartomány, hézag
  megengedett (`WORLDLOGIC_ID_INVALID`), egyedi (`…_ID_DUPLICATE`); véges,
  `min < max` (`…_RECT_INVALID`); a világon belül (`…_OUT_OF_BOUNDS`); félig
  nyitott téglalapként nem fedhetik át egymást (`…_ZONE_OVERLAP` — az MXL1
  v1-ben nincs overlay-típus); **nem kell lefedniük a világot** (a
  `WORLDLOGIC_COVERAGE_GAP` 408 és a `WORLDLOGIC_NO_ZONES` 414 a MAP-2 óta
  visszavonva, nem keletkezik);
- **játékos spawn-régiók:** **legalább egy** (`WORLDLOGIC_NO_PLAYER_SPAWN`
  415); id ≥ 1, egyedi; a régió **középpontja járható cella**
  (`WORLDLOGIC_SPAWN_BLOCKED` 416 — a szerver oda teszi a játékost).
  **Area-kapcsolat** (MAP-2 review): `areaId = 0` = nincs area — világszintű
  spawn-régió, csak a világon belül kell lennie (`…_OUT_OF_BOUNDS`);
  `areaId ≥ 1` esetén létező area-ra hivatkozik (`…_REFERENCE_INVALID`) és
  az area-ján belül van (`…_SPAWN_OUTSIDE_ZONE`). Így a „0 area" és a
  „kötelező spawn-régió" együtt érvényes: area nélküli csomag egy `areaId = 0`
  spawn-régióval betölthető; area nélküli csomag spawn-régió nélkül nem (415);
- **warpok:** id ≥ 1, egyedi; forrás véges, `min < max`, világon belül; cél
  véges, a félig nyitott világban, **járható cellán** (`…_WARP_TARGET_INVALID`);
  trigger-gráf (i → j, ha i célja j forrásában van, a szerver **félig nyitott**
  tartalmazásával): **ciklus hiba** (`…_WARP_CYCLE`, 412, önhurok is) mindkét
  módban. SL-2: alapértelmezett `strict` módban a lánc is **hiba**
  (`…_WARP_TARGET_IN_TRIGGER`, 413); explicit `legacy` módban warning.
  `--warp-policy strict|legacy` > `warp_policy` config > strict alapérték.
  Hibás/üres érték exit 2, tartalmi elutasítás exit 3; nincs automatikus fallback.
  A startup, `--startup-check` és offline `--validate-world-package` azonos
  policyt értelmez. A csomag nem választhat legacy kivételt.

## 5a. Szerver-vízterek (`water_bodies.mxws`, MXWS v1, little-endian, MAP-3)

```
u32 magic = 0x5357584d ("MXWS")   u32 version = 1   u32 count (<= 4096)
body x count: u32 id, f32 minX, minY, maxX, maxY, f32 surfaceMeters
```
Pontosan `12 + 24·count` bájt (`WATER_BODIES_TRUNCATED` 602; fejléc:
`WATER_BODIES_HEADER` 601). Minden test: id ≥ 1 egyedi; véges, `min < max`,
a világon belül, `|surface| ≤ 100000` (`WATER_BODY_INVALID` 603); a testek
félig nyitott téglalapként **diszjunktak** — egy pontnak egy felszíne van
(`WATER_BODY_OVERLAP` 604). Szerver-adat, mindig rezidens (kicsi).

## 6. Spawn-tábla (`mob_spawns.conf`, mob_spawns v1, UTF-8/ASCII szöveg)

Soronként egy spawn-pont; `#`-tól sorvégig megjegyzés; üres sor megengedett.
```
mob_type_id=<u32 ≥ 1> x=<méter> y=<méter> count=<1..100000> radius=<méter ≥ 0>
```
Pontosan ez az öt kulcs, mindegyik egyszer, más kulcs nincs
(`SPAWNS_SYNTAX` 500); érték idézőjel nélkül, egész vagy véges decimális
(`SPAWNS_FIELD_INVALID` 501); a középpont a félig nyitott világban
(`SPAWNS_OUT_OF_BOUNDS` 502, origó-helyes); ≤ 100000 sor (`SPAWNS_TOO_MANY`
503). A `mob_type_id`-nak a szerver mob-típus registry-jében léteznie kell
(`SPAWNS_MOB_TYPE_UNKNOWN` 504 — a szerver ellenőrzi, a registry a szerver
konfigja, nem a csomagé). A kör a világ szélén túlnyúlhat: a futásidő a
világon kívüli / nem járható húzást újrasorsolja (legfeljebb 8×), utána
kihagyja — soha nem tolja a szélre (9.3).

## 7. Ki mit tölt

| Réteg | Kliens | Szerver |
|---|---|---|
| manifest | ✓ (v2) | ✓ (v2/v3) |
| height | ✓ (v1) | ✓ eager: rezidens; streaming: igény szerint chunkonként (v1/v2) |
| attributes | opcionális | ✓ mint a height (bit 0 = blokkolt) |
| splatA/B, texturePalette | ✓ | ✗ (nem kötelező, nem tölti, nincs memória) |
| worldLogic | részben | ✓ (area-k metaadatként, spawn-régiók, warpok) |
| mobSpawns | — | ✓ (ha van) |
| water (MXWB) | ✓ | ✗ (kliens-render adat, nem validált, nem használt) |
| waterBodies (MXWS) | — | ✓ rezidens (a `water` deklarációval) |

## 8. Validációs mélység és hibák

- **Startup, eager rezidencia** (a szerver indulása, alapértelmezés):
  manifest, réteg-deklarációk, chunk-index, minden chunk
  fejléce/táblája/tartományai + a dekódolt height/attribute tartalom (+ CRC
  v3-ban) + a chunk-varratok, worldlogic (+ a terrain elleni keresztszabályok:
  warp-cél és spawn-régió-középpont járható), spawn-tábla, vízdeklaráció +
  vízterek, mob-típus kereszt-ellenőrzés. **Minden chunk induló chunk.** A
  kliens-szekciók csak tartomány-ellenőrzöttek.
- **Startup, streaming rezidencia** (`terrain_residency=streaming`, MAP-3):
  ugyanez, de a chunkoknál csak a **létezés és a méret** (v3 index) ellenőrzött
  indításkor, olvasás nélkül; a tartalmat csak az **induló halmaz** kapja meg
  (minden spawn-régió közepe és warp-cél chunkja). Minden más chunk a
  betöltésekor kap teljes ellenőrzést: CRC, dekódolás, varrat a már publikált
  szomszédokkal. A később sérültnek talált chunk **nem válhat szabad
  területté**: korlátozott újrapróba után `InvalidData`-ként publikálódik (12.1).
- **Full** (`gameserver --validate-world-package <dir> [--mob-types f]`):
  Startup + a kliens-adat szerkezeti dekódolása (splat-geometria) és a
  kliens-fájlok olvashatósága. Validátor nélküli réteg (víz)
  `LAYER_NOT_VALIDATED` — soha nem „validált".
- **A szerver indulása a csomag után** (még DB és hálózat előtt, szintetikus
  módban is): a kezdeti partíció a világ bounds-ára (`partition_regions` ×
  `partition_initial_leaves`) — ha a kezdeti zónák száma 1024 fölött van, egy
  kezdeti levél keskenyebb 240 m-nél (2 × AOI-sugár), vagy a konfig nem
  értelmezhető, a szerver exit 2-vel leáll. Az indulási log egy
  `Bootstrap resources:` sorban kiírja a kezdeti zónaszámot, a világméretet és
  a rezidens terrain-bájtokat a korlátaik mellett (10.3).

Minden hiba strukturált: `SEVERITY CODE(szám) package= layer= file=
chunk=(x,y) field= offset=|line= reason= expected=`. A számkódok stabilak (csak
hozzáfűzés; visszavont kód nem kap új jelentést): 100–106 csomag/út, 200–206
manifest, 210–215 réteg, 220–223 index/integritás, 300–307 chunk, 400–416
worldlogic (408 és 414 visszavonva a MAP-2-ben; 415 nincs spawn-régió, 416
nem járható spawn-középpont), 500–504 spawn, 600–604 víz (MAP-3), 800 szerver
indulási adat (mob-típus registry), 900 belső. Lista:
`shared/map/include/map/WorldPackage.h` (`PackageErrorCode`).

## 9. Koordináta-konvenció és futásidejű szabályok (MAP-2)

### 9.1 Tengelyek, egységek, transzformációk
- **Tengelyek:** +X = kelet, +Y = észak, +Z = fel; minden hossz méter.
  Balkezes/jobbkezes kérdés nincs: a szerver 2D (X, Y) + magasság. Heading:
  0 = +Y (észak), π/2 = +X (kelet); `dir = (sin a, cos a)`.
- **Világ:** `[MinX, MaxX) × [MinY, MaxY)`, `MinX = originX`,
  `MaxX = originX + worldSizeCells·cell` (Y ugyanígy, `worldSizeCellsY`-nal).
  Az origó bármilyen véges érték (negatív is), a világ nem kell négyzet legyen.
- **Cella** `(i, j)`: `[origin + i·cell, origin + (i+1)·cell)` tengelyenként;
  egy pont celláját a szerver **világ-egységben** dönti el (az osztás csak
  becslés, egy cellahatár ulp-közelében a világkoordinátás összevetés dönt).
- **Magasságminta** `(vx, vy)` a cellasarkokon: `vx ∈ [0, worldSizeCells]`,
  helye `origin + vx·cell`. Mintaszám tengelyenként = cellák + 1 ≠ chunk-szám.
- **Chunk** `(x, y)`: az `[x·chunkSizeCells, …)` cellatartomány; stabil kulcs.
- **Futásidő:** a pozíció `f32`; a szerver bounds-a a manifest double-
  geometriájából befelé kerekítve (`TerrainService::BoundsOf`), így minden
  `f32` pont, amit a szerver világon belülinek lát, a double-geometriában is
  belül van.

### 9.2 Szerver-terrain lekérdezés (`mx::map::ServerTerrain`)
- `Height(x, y)`: a tartalmazó cella **négy sarokmintájának bilineáris
  interpolációja** (`déli = h00 + (h10−h00)·fx`, `északi = h01 + (h11−h01)·fx`,
  `h = déli + (északi−déli)·fy`), double-ben, `f32`-re kerekítve. A lekérdezés
  pontosan egy chunkot olvas (a cella chunkját; a határminta mindkét chunkban
  megvan és azonos) — a varrat nem jelent kettős tulajdonlást.
- `Cell(x, y)`: a tartalmazó cella attribútumai; `Walkable()` = van adat
  **és** bit 0 = 0.
- **Státusz** minden lekérdezésben: `Ok`; `OutsideWorld` (a félig nyitott
  világon kívül, a keleti/északi külső él is, és nem véges input); `NotResident`
  (világon belül, de a chunk nincs betöltve — MAP-3 streaming); `InvalidData`
  (a chunk jelen van, de használhatatlan; validált betöltés nem állít elő ilyet).
  A hívónak minden státuszt kezelnie kell: **csak az `Ok` jelent magasságot**
  — a 0 m érvényes magasság, a hiányzó adat nem 0 m és nem járható.
- `Vertex(vx, vy)`: egy minta közvetlenül (eszközök, orákulumok).

### 9.3 Határon kívüli szabályok (R10) — nincs általános clamp
- **Mozgás (játékos):** tengelyenkénti lépés, a lépés **teljes útja**
  ellenőrzött (MAP-3, `CheckStep`, 12.1). Ha az út nem tiszta, azon a
  tengelyen a lépés elmarad:
  - világon kívül, nem rezidens, érvénytelen adat;
  - blokkolt cella;
  - a bekapcsolt lejtő- vagy mélyvíz-szabály.

  A pozíció **nem** kerül a szélre, a másik tengely mehet tovább (csúszás a
  fal mentén). A z a terrain-magasság, ha `Ok`, különben az előző z marad.
  Egy lépés, amely a kezdőcellán **belül** marad, szintén csak adattal megy
  (MAP-3 review): az útellenőrzés a kezdőcellát kihagyja (abból mindig ki
  lehet lépni), de a cél magassága lesz a z — nem rezidens adatnál a lépés
  vár, invalid adatnál elutasított. (Korábban ilyenkor a lépés elavult z-vel
  megtörtént.)
- **Mozgás (mob):** a wander-póráz (játékszabály) után a lépés útja ugyanígy
  ellenőrzött; a blokkoló rács a mobokra is érvényes (MAP-3). Egy nagy
  (alacsony LOD-ú) lépés sem ugorhat át egycellás falat.
- **Hiányzó adat mozgás közben (streaming):** a nem rezidens chunkra vezető
  lépés ebben a tickben elmarad, és a chunk igényként kerül a streamerhez.
  - Az entitás helyben vár, korlátosan (a betöltési késleltetésig); nincs
    találgatott eredmény.
  - A kliens bemenete a latest-movement modellben marad; a szimulációs idő
    nem ugrik; a megállás állapota megmarad.
  - A mozgás előre is kér: a jelenlegi pozíció chunkját és a
    `sebesség × lookahead` ponton lévőt (12.3).
  - Dormant entitás és alvó zóna nem igényel semmit, így a chunkja
    eviktálható. Ébredéskor az ottani lépések várnak, amíg az adat megjön.
- **Érvénytelen adat mozgás közben** (`InvalidData`, 12.2): a lépés
  elutasítva, igény és várakozás nélkül (nem érkezik más adat a csomag-
  generáción belül); számláló: `invalid`, nem `steps_waiting`. Az entitás a
  határon megáll; a z az utolsó `Ok` magasság.
- **Játékos spawn** (MAP-2 review, egyetlen érvényes sorrend):
  1. debug felülírás — csak dev buildben (`MMO_DEBUG_SPAWN_OVERRIDE=1`);
  2. a csomag **első** (fájl-sorrendű) játékos spawn-régiójának középpontja;
  3. különben a belépés `EnterWorldReject SERVER_ERROR`-ral elutasítva (nincs
     „valamelyik szélső zónába ejtés", nincs találgatott pozíció).

  Minden jelölt csak akkor jó, ha a világon belül van, járható, van `Ok`
  magassága és van tulajdonos szerver-zónája. **A DB-ben tárolt pozíció nem
  jelölt:** a gameserver soha nem írja vissza (nincs perzisztencia-útja), és
  nincs világ-azonossága (a `map_id` nincs a csomag `world_id`-jához kötve),
  így egy másik világ pontját nevezhetné meg. A tárolt pozícióból való
  folytatáshoz előbb perzisztencia és világ-azonosság kell (DB/login-munka,
  a map-fázison kívül). Validált, rezidens csomagnál a 2. lépés mindig
  teljesül (416); elutasítás csak akkor fordul elő, ha a spawn-középpont
  adata nem elérhető (pl. `NotResident`).

  Streamingnél a spawn-régiók közepének chunkjai az induló halmaz részei,
  **állandóan rezidensek** (pinned), így a játékos-spawn soha nem vár adatra.
  A debug felülírás nem rezidens pontja a spawn-régióra esik vissza, és
  igényli a chunkot.
- **Mob spawn:** 6. pont (újrasorsolás, majd kihagyás; soha nem a szélre).
- **Mob spawn / respawn (streaming):** ha a választott pont chunkja nem
  rezidens, az nem „blokkolt":
  - a chunkot igényli, és az újraéledés 0.5 s múlva újrapróbál, spawnpontonként
    legfeljebb 40-szer egymás után, utána eldob (WARN, számláló);
  - az induló mob-spawn kötegekben tölti be a spawn-körök chunkjait, majd
    elengedi őket; egy köteg a **szabad** budgetbe fér (fix metaadat és
    induló halmaz után), a betöltés valódi foglalásával számolva (dekódolt
    méret + olvasási puffer + overhead);
  - **invalid** chunkon a jelölt elutasítva (a körön belül újrasorsol); ha
    az utolsó jelölt is invalid, a mob nem jön létre, számláló
    (`MobSpawnsRefusedInvalidTerrain`), újrapróba nincs.
- **Warp:** a trigger félig nyitott (`Rect::ContainsHalfOpen`); a cél a
  betöltéskor ellenőrzött (világon belül, járható); futásidőben is járható kell
  legyen, ismert magassággal és aktív célzónával. A zónák közötti warp a
  meglévő migrációs tranzakciót használja (MAP-4, részletesen a 13. pont). A
  warp-célok chunkjai az induló halmaz részei: indításkor teljes
  ellenőrzéssel töltődnek (hibánál a szerver el sem indul), és állandóan
  rezidensek, így validált csomagnál futásidőben nem lehetnek sem `NotResident`,
  sem `InvalidData`. A védekező ágak: nem rezidens célnál a warp abban a
  tickben nem sül el, és a cél chunkja igénylődik, legfeljebb 5 s-ig;
  invalid / blokkolt célnál egyszeri elutasítás történik belépésenként,
  ismétlődő WARN nélkül. Az újraélesítés és cooldown a 13. pont szerint működik.
- **Zóna-tulajdon:** egy pontot pontosan egy aktív levél birtokol (félig
  nyitott), a világon kívüli pontot egyik sem; nincs lineáris „legközelebbi
  zóna" fallback.

### 9.4 Szerver-partíció (nem a csomag része)
A szerver konfigja (`gameserver.conf`): `partition_regions=<x>x<y>`
(1..16 tengelyenként; alapértelmezés 2x2, szintetikus módban 1x1) és
`partition_initial_leaves=<x>x<y>` (1..64 tengelyenként, régiónként;
alapértelmezés 1x1) a betöltött bounds-ra. Régiónevek a
tengelyekhez igazodnak: 1x1 `World`; 2x1 `West`/`East`; 1x2 `South`/`North`;
2x2 `SouthWest`, `SouthEast`, `NorthWest`, `NorthEast`; más rács `R<ix>_<iy>`.
A `ZoneId`-k a szerveré (1..N a kezdeti leveleknek, utána futásidejűek), az
area-id-któl független névtér. A futásidejű split/merge nem tölti újra a
terraint és nem másol entitást (`worldbench --mode mapsplit`).

**Összesített bootstrap-korlát** (MAP-2 review): a kezdeti levélzónák száma
(`regions_x·leaves_x·regions_y·leaves_y`, vagy az explicit levelek száma)
legfeljebb **1024** (`kMaxInitialLeafZones`). A tengelyenkénti korlátok
önmagukban ~1M kezdeti zónát engednének. Mért költség (üres 100 km-es
világ, `worldbench --mode bootstrap`): zónánként ~1.3 MB working set;
256 zóna 332 MB / 0.18 ms üresjárati supervisor-pass, 1024 zóna 1.3 GB /
1.8 s építés / 1.3–1.5 ms, 4096 zóna 5.2 GB / 77 ms (a 20 Hz tick fölött).
A korlát fájl- és szintetikus módban is ugyanott (`BuildInitialPartition`)
érvényesül; túllépésnél exit 2, még DB és hálózat előtt. A finomabb
partíciót a futásidejű split adja, nem a bootstrap.

## 10. Magasság- és rezidencia-szerződés — jóváhagyva (MAP-2 review, 2026-09-25)

> A MAP-2 review a munkát a **resident terrainre vonatkozó terjedelemben**
> fogadta el. Ez a szakasz a jóváhagyott height- és residency-szerződés a
> kóddal egyeztetve; a MAP-3 streaming csak bővítheti, a garanciáit nem
> gyengítheti. (A height réteg v2 korábban a munkapont értelmezése volt; a
> review-val jóváhagyott.)

### 10.1 Magasság
1. **Tárolás:** height réteg v1 (int16 × 0.01 m, offset 0, ±327.67 m) vagy
   v2 (explicit `heightEncoding`: int16 | int32, `metersPerUnit` 0.0001..10,
   `|offsetMeters|` ≤ 100000), `méter = offset + raw·unit` double-ben (3.4).
   Height v3 ugyanilyen tárolás mellett kötelező explicit háromszögmódot ad.
   A v1 csomagok változatlanul érvényesek; height v2/v3 csak manifest v3-ban.
2. **Minták** a cellasarkokon, chunkonként; a szomszédos chunkok közös
   határmintái bitre azonosak (betöltéskor validált, `CHUNK_EDGE_MISMATCH` 307).
3. **Lekérdezés:** a pont celláját világ-egységben dönti el (9.1); a cella
   négy sarkának bilineáris interpolációja height v1/v2-ben; height v3-ban a
   3.4 szerinti két háromszög. Mindkettő double-ben számol, az eredmény `f32`. A
   mintapontokon a tárolt kvantálás pontos értéke; a `f32`-re kerekítés hibája
   ≤ fél ulp (2.7 km-en ≈ 0.12 mm — a `terrain` mód int32-esete ezt méri).
4. **Partíció-függetlenség:** az eredmény csak a világkoordinátától és a
   betöltött csomagtól függ — nem a szerver-zóna topológiától, a split/merge
   állapottól vagy a hívó workertől (`mapsplit`: bitre azonos válaszok 1 → 4
   → 16 → 4 → 1 alatt).
5. **Státusz:** `Ok` / `OutsideWorld` / `NotResident` / `InvalidData`; csak
   `Ok` hordoz magasságot. A 0 m érvényes érték; hiányzó adat soha nem 0 m,
   nem járható és nem „nincs akadály".
6. **Hívók:** mozgás (z csak `Ok`-ból, különben az előző z), játékos-spawn
   (`Ok` kötelező, különben elutasítás), mob-spawn (`Ok` kötelező, különben
   újrasorsolás/kihagyás), migráció/transzfer (z csak `Ok`-ból).

### 10.2 Rezidencia — a jóváhagyott (MAP-2) terjedelem
1. **Eager, teljes rezidencia:** a teljes szerver-terrain (magasság +
   attribútum) a runtime létrehozása **előtt** betöltődik és validálódik
   (Startup mélység: minden chunk tartalma, CRC, varratok); hibánál exit 3,
   még DB és hálózat előtt.
2. **Egy példány világonként**, a világ-adat tulajdonában (`TerrainService`
   a `WorldRuntime`-ban); a zónák közösen olvassák, zónánkénti másolat nincs.
   Inicializálás után **immutábilis**: split/merge, migráció, zóna-reclaim nem
   tölt újra és nem másol (`PackageLoadCount` változatlan — `mapsplit`).
3. A szimulációs hot path **nem végez fájl-I/O-t** (minden adat memóriában).
4. **Korlát:** ≤ 2^28 magasságminta (`MANIFEST_SIZE_OVERFLOW`); chunkonként
   `(cx+1)(cy+1)·(2|4) + cx·cy·2` bájt. Legrosszabb eset int32 mintával
   ≈ 1 GiB magasság + ≈ 0.5 GiB attribútum; kliens-render adat 0 B.
5. **`NotResident`** jelentése rögzített, bár MAP-2-ben csak teszt-seam
   állítja elő (`EvictChunkForTest`): világon belüli, jelenleg nem elérhető
   adat — nincs magasság, nem járható; a hívók a 9.3 szerint járnak el
   (mozgás elutasítja a lépést, játékos-spawn elutasít, mob-spawn kihagy;
   `terrain` mód: `not-resident-vs-zero-meters`,
   `spawn-refused-without-usable-spawn-region`, `mob-spawn-skips-missing-chunk`).

### 10.3 Összesített bootstrap-erőforrások (MAP-2 review)

| Erőforrás | Korlát | Hol érvényesül | Mérés / bizonyíték |
|---|---|---|---|
| Világkoordináták | `\|c\|` ≤ 131072 m (csomag **és** szintetikus világ) | loader (`UNSUPPORTED_FEATURE`); `main` exit 2; `WorldRuntime` (`invalid_argument`) | worldpackage `coordinate-range`; bootstrap `synthetic-world-caps-enforced`; indulási teszt 6c4 |
| Kezdeti zónák | ≤ 1024 | `BuildInitialPartition` (mindkét mód, explicit levelek is) | bootstrap: 1024 zóna 1.3 GB / 1.8 s / 1.3 ms üresjárati supervisor-pass; `initial-zone-cap-enforced`; indulási teszt 6c3 |
| Kezdeti levél mérete | ≥ 240 m (2 × AOI) | `BuildInitialPartition` | indulási teszt 6b |
| Rezidens terrain | ≤ 2^28 minta | loader | worldpackage `manifest-size-overflow`; test_zone 984 KB |
| Activity-mező | 500 m cella a bounds-on (a koordináta-korlát miatt ≤ 525 × 525 cella) | `SpatialActivityField` | — |
| Load-mező | ≤ 2^20 cella (a cellaméret szükség esetén nő) | `ValidateLoadFieldConfig` | H10 hygiene |
| Worker pool | `hardware_concurrency − 1` vagy `zone_workers` | `ZoneWorkerPool` | nem a zónaszámhoz kötött (`workerpool`, `mapsplit`) |

A futásidejű zónaszám (split) nem bootstrap-erőforrás, és nincs összesített
korláttal fedve. Felső határa: kezdeti zónák × 4^(max_depth−1), illetve a
min-zone-size. A supervisor-pass költsége a zónaszámmal szuperlineáris (4096
zónánál 77 ms). Ez nyitott döntés: futásidejű zónabudget, ahol a split-kapu
`zone-budget` okkal utasít el.

### 10.4 Rezidencia — streaming (MAP-3)
A streaming a 10.1–10.3 garanciáit változatlanul hagyja. Ugyanaz a
magasság-szerződés: bitre azonos válaszok a teljesen rezidens referenciával
(`streaming` mód). Ugyanaz a státusz-szemantika. A megvalósított modell, a
korlátok és az élettartam: 12.

## 11. Eszközök

- `mapgen_test_zone --out <dir> [--format 2|3] [--server-only] [--no-spawns]
  [--force]` — a közös íróval generál; kimeneti könyvtár kötelező, meglévő
  csomagot `--force` nélkül nem ír felül (a becsekkolt `test_zone` véletlen
  felülírása kizárt; az szerkesztővel módosított tartalom).
- `gameserver --validate-world-package <dir>` — offline Full validáció, exit
  0 = érvényes, 3 = érvénytelen.
- Szerver-futtatás: [`gameserver.conf.example`](../gameserver/apps/gameserver/config/gameserver.conf.example)
  `world_mode` / `world_package` / `mob_types_config` / `partition_regions` /
  `partition_initial_leaves` kulcsai; MAP-3: `terrain_residency`
  (`eager` | `streaming`), `terrain_cache_budget_mb`, `terrain_io_threads`,
  `terrain_max_in_flight`, `terrain_retain_seconds`, `movement_max_slope`,
  `movement_max_water_depth_m`.
- Tesztek: `worldbench --mode streaming | worldquery | streamlife |
  streamsoak --cycles <mp> [--budget-mb <MB>]` (MAP-3), a MAP-1/2 módok
  mellett.

## 12. Szerver-világlekérdezések és streaming (MAP-3)

A 12. pont a MAP-3 **megvalósított** szerződése. Kódhelyek:
- `shared/map`: `ServerTerrain`, `ServerWater`, `ChunkSource`;
- gameserver `world/terrain/`: `TerrainService`, `TerrainStreamer`,
  `NavigationService`.

### 12.1 Lekérdezések és eredmény-státuszok

| Lekérdezés | API | Eredmény |
|---|---|---|
| Magasság | `TerrainService::Height(x, y)` | `Ok` / `OutsideWorld` / `NotResident` / `InvalidData` (9.2) |
| Cella | `Cell(x, y)`, `IsWalkable` | ugyanazok; járható = van adat és bit 0 = 0 |
| Mozgás-lépés (static collision) | `CheckStep(from, to)` | `Clear` / `Blocked` / `TooSteep` / `DeepWater` / `OutsideWorld` / `NotResident` (+ az igényelendő chunk) / `InvalidData` |
| Út menti cellák | `ServerTerrain::Segment` | az útba eső összes cella, sorrendben, chunkhatáron át; a kezdőcella kivételével; sarok-átlónál mindkét oldalcella |
| Víz | `Water(x, y)` | `Water` (felszín, mélység) / `NoWater` / `OutsideWorld` / `NotResident` (a talaj nem rezidens) / `UnsupportedLayer` (nincs deklaráció) / `InvalidData` |
| Útvonal (navigation) | `PostNavigationRequest` / `NavigationResult` | `Found` (fordulópontok, hossz) / `NoPath` / `NotResident` / `OutsideWorld` / `BudgetExceeded` / `Cancelled` / `InvalidData` (a keresés kimerült, és invalid adat volt a határán — nem `NoPath`) / `UnsupportedLayer` / `Rejected` (több mint 256 futó job) |

A hiányzó adat soha nem 0 m, soha nem „járható", soha nem „nincs víz" és soha
nem „nincs út": a hívó szabálya dönt (9.3). A lekérdezés világkoordinátára
szól, a válasz független a zóna-topológiától és a hívó workertől (`mapsplit`,
`streaming` mód).

**Collision (a támogatott geometria):**
- a blokkoló cellarács (attribútum bit 0), a teljes lépés-úton ellenőrizve;
- egy **lejtőkorlát** (emelkedés/m, csak felfelé; `movement_max_slope`,
  alapból kikapcsolva);
- egy **mélyvíz-szabály** (`movement_max_water_depth_m`, alapból kikapcsolva).

**Nem támogatott:** többszintes geometria (híd, barlang), statikus alakzatok,
dinamikus akadályok. Egyetlen heightfield, nem teljes 3D collision.

**Navigation:** rács-A* a járhatósági rácson.
- 8-irányú; átlós lépés csak két nyitott ortogonális szomszéd között (nincs
  sarokvágás).
- A lejtő- és a vízszabály ugyanúgy érvényes.
- Keresési ablak: a start/cél befoglaló téglalapja + `search_margin_m`.
- Korlátok: munkabudget jobonként (`max_expansions`), passonként közös
  budget (4000 csomópont); legfeljebb 256 futó job (fölötte `Rejected`,
  azonnal); lemondható; időkorlát a chunk-várakozásra; az eredmények
  tárolása korlátos (4096, a legrégebbi esik ki).
- A job minden általa olvasott chunkot **pinnel**, amíg fut.
- Takarítás: a job nincs entitáshoz vagy sessionhöz kötve (nincs
  gameplay-hívó); a hívó mondja le (`PostNavigationCancel`), a pinek a
  befejezéskor / lemondáskor azonnal elengedődnek.
- Nincs navmesh-réteg a formátumban: a járhatósági rács maga a navigációs
  adat.
- Gameplay-hívó (AI, aggro, útvonal-policy) nincs; a szolgáltatás
  infrastruktúra, tesztek és a `WorldRuntime` API használják.

**Water:** 3.5 és 5a. Minimális felszín / mélység / víz-föld lekérdezés.
Hajófizika, hullám és áramlás nem része.

### 12.2 Streaming — szálak, állapotgép, élettartam

- **Szálak:**
  - a szimulációs zóna-workerek csak olvasnak;
  - külön I/O-szálak (`terrain_io_threads`, alap 2; soha nem chunkonként vagy
    zónánként) futtatják a `ChunkSource::Load`-ot: olvasás, CRC, dekódolás,
    validálás;
  - minden más a supervisor-szálon történik: igény-aggregálás, admission,
    publikálás, eviction, felszabadítás.
- **Állapotgép chunkonként:**

  ```
  Unloaded -> Waiting -> Loading -> Ready -> [evicted] -> Unloaded
                           +-> Failed -> (retry, backoff) -> ... -> invalid
  ```

  - `Waiting`: igényelt, budgetre vár.
  - `Loading`: bájtok lefoglalva, job a sorban vagy futás közben.
  - `Ready`: publikált.
  - evicted: unpublish; a bájtok retired állapotban maradnak a következő
    csendes ablakig.
  - `Failed` (pontos szemantika, MAP-3 review):
    - Kísérletnek számít minden betöltési hiba: olvasás, CRC, dekódolás,
      varrat-eltérés egy publikált szomszéddal.
    - Két kísérlet között a slot **üres**: a lekérdezés `NotResident`, a
      fogyasztók várnak. A következő kísérlet csak akkor indul, ha egy
      fogyasztó a backoff (`kísérlet × 0.5 s`) után újra igényli.
    - A 3. kísérlet után a chunk **invalidként** publikálódik: egy
      mintátlan, üres chunk, amelynek csak az overheadje könyvelt.
      - Minden lekérdezése `InvalidData`: nincs magasság, nem járható, nincs
        víz-mélység. Soha nem 0 m és soha nem szabad terület.
      - Nem eviktálható és nem olvasódik újra; a generáció végéig nem
        igénylődik.
      - A szomszédai varrat-ellenőrzése kihagyja (nincs mit összevetni;
        korábban az üres tömböt indexelte).
      - Csak generációváltás (csomagcsere; a `ResetGenerationForTest` seam)
        vonja vissza, és az is **retire**-ral, nem azonnali
        felszabadítással.
  - A sorban álló, de még nem olvasott jobot lemondja, ha az igénye lejárt
    (`terrain_retain_seconds`, ≥ 0.1 s). Az igény minden fogyasztó között
    aggregált, így egy fogyasztó távozása nem mondja le a másikét.
- **Publikálás:** egy chunk csak teljesen beolvasva, CRC-, dekódolás- és
  varrat-ellenőrzés után, egyetlen release-store-ral válik láthatóvá. Félkész
  adatot olvasó nem lát.
- **Élettartam:**
  - Olvasni egy zóna-tick alatt (vagy a supervisoron) a publikált nyers
    mutatóval lehet: zár és referenciaszámlálás nélkül.
  - Minden, ami a slotot elhagyja (evicted, vagy egy későbbi publikálás
    lecseréli), a retired listára kerül, és **csak supervisor-csendes
    ablakban szabadul fel**, amikor nincs repülő zóna-tick. Ez a türelmi idő
    a következő happens-before láncon alapul:
    1. Csak a supervisor indít zóna-ticket, és a tick-jelzőt **a kiadás
       előtt** állítja be (CAS).
    2. A worker a jelzőt a tick végén release-store-ral törli.
    3. A supervisor a felszabadítás előtt acquire-rel minden jelzőt töröltnek
       lát. Így minden olyan tick, amely a régi mutatót betölthette, befejeződött.
    4. Új tick nem indulhat, amíg a supervisor fel nem szabadított és újra ki
       nem ad. A kiadás mutex-szinkronizált sorral megy az unpublish
       (release) után, így az új tick már az üres vagy az új slotot látja.
  - A csendes ablak a streamer egyetlen felszabadítási feltétele, és a
    supervisor ugyanabban a passban számolja, mint a felszabadítást
    (`PumpTerrain`). A H9 zóna-reclaim két tickes türelmi ideje ettől
    független: az a zóna-objektumokat védi, nem a chunkokat.
  - A ticken túl élő olvasó **pint** tart (`ServerTerrain::Owned`, például a
    navigációs job); pinnelt chunkot nem választ eviction. Pint **csak az
    író (supervisor) szál** vesz és enged el, ugyanaz, amelyik az eviction-ról
    dönt. A „nincs pin" (`PinCount == 0`) ezért nem változhat meg az
    ellenőrzés és az unpublish között. A Debug build ezt ki is kényszeríti:
    `BindWriterThread` után minden író-oldali hívás (`Publish`, `Unpublish`,
    `Owned`, `PinCount`, `Clone`) assertál az író szálra.
  - Bench, eszköz vagy más szál futó világot csak supervisor-snapshoton át
    kérdezhet (`ReadWorld`).
  - A completion (generáció, kérés-epoch) párral érkezik. Elavult generáció,
    lemondott kérés vagy `Stop()` utáni eredmény nem publikálódik, és a
    memóriája felszabadul.
  - A chunk-élettartam független a zóna-slotoktól: callback nem tart
    `Zone*`-t, entitást vagy slotot, ezért a zóna-reclaim és a slot-reuse
    nem érinti. Egy migráció / split / merge / reclaim / slot-reuse közben
    késve érkező, **aktuális** completion publikálódik a világ-cache-be; a
    várakozó entitás az új zónájából olvassa (`streamlife`).
  - Az I/O-worker a completion után a supervisort a várakozási
    predikátumon át ébreszti. Korábban a puszta `notify` elnyelődött, és a
    completion az 5 ms-os tétlen timeoutig várt, ami Windowson kb. 15.6 ms.
- **Leállítás:** a sorban állók eldobva; csak a már olvasás alatt állók
  fejeződnek be (egy chunk olvasási ideje), azok eredménye elutasítva.
- **Split / merge:** nem ürít cache-t, nem tölt újra, nem spawnol és nem
  despawnol entitást. A NetId, a presence, az AOI és a replikációs alapállapot
  változatlan.

### 12.3 Budget, admission, igény, prefetch

- **Saját könyvelés** (a `terrain_cache_budget_mb` alatt tartva):
  - rezidens payload + chunkonkénti overhead;
  - folyamatban lévő foglalások (dekódolt méret + olvasási puffer);
  - még fel nem szabadított (retired) chunkok;
  - fix metaadat (slot-táblák, chunk-index, bounded request-állapotok,
    admission scratch, mérési gyűrűk/hisztogramok). Ez konzervatív saját
    könyvelés, nem az allocator vagy a teljes folyamat memóriahatára.

  A folyamat RSS-e külön mérendő (OS-nézet), a kettő nem azonos.
- **SL-1 admission:** az eredeti queue/cache marad. Betöltés előtt a teljes
  read+decode foglalás szükséges. Nyomás alatt a legrégebben igényelt,
  nem pinnelt chunk puha retentionje felülbírálható; a 100 ms-os friss
  publikálási/handoff lease és minden élő pin védett. A 4096 egyedi chunkos
  sor utolsó 64 helye blokkoló Admission műveletnek van fenntartva.
  Nincs overflow-sor és elfogadott kérés nem kerül kidobásra emiatt.
  Admission/Active/Prefetch osztályban 4:2:1 weighted round robin dolgozik;
  legfeljebb 64 próbálkozás/pass, változatlan 32 in-flight és 2 I/O worker.
  A túl nagy jelölt után a kisebb is sorra kerül, az eredeti kor megmarad.
  A helyhiányos admission újrapróbája legalább 10 ms, korábban érkező
  completion is ébresztheti; ingress-elutasításnál chunkonként 100 ms backoff.
- **Művelet:** `PrepareTerrain` / `TerrainRequest` monoton id/start/deadline,
  legfeljebb 64 regisztrált kérés, plusz 64 beküldésre váró command,
  legfeljebb 16 chunk/kérés. Nem tart entity/session/zone mutatót.
  `Ready` a teljes halmazt pineli `Consume`, `Cancel`, utolsó fogyasztói
  referencia elengedése vagy deadline állapotig. Az owner megtartja a kis
  állapotot a következő passig, így a Consume-then-drop is visszaigazolt.
  Részhalmazra nem vesz fel előkészítési pint. Ugyanazt a fizikai chunkot
  több művelet közösen tölti; egyik lemondása a többit nem törli.
- **Kapacitás:** a minimális payload + szükséges legnagyobb read buffer +
  fix metaadat + a halmazon kívüli permanent pinek feletti kérés
  `CapacityRejected`; nem foglalja le előre az összes read buffert.
  Ideiglenes élő pin-nyomásnál várakozás, majd monoton `TimedOut` lehetséges.
  Hibás adat `InvalidData`, hibás koordináta `OutsideWorld`.
  Mozgásnál továbbra is explicit `NotResident` és visszautasított lépés van.
  A fairness kiszolgálható munkakészletet, normál I/O-t, felszabaduló pineket
  és supervisor/tick előrehaladást feltételez; nem általános 50 ms ígéret.
  Ha az induló halmaz sem fér be, továbbra is exit 2 (DB/hálózat előtt).
- **Safe-point:** jogosult retired payload esetén a supervisor a meglévő
  scheduling gate-en át megvárja a már kiadott tickek végét; quiescent
  felszabadítás után ismét ütemez. Snapshot/benchmark kérés nem szükséges.
  Generációváltás után az élő I/O-reservation és a pinnelt retired payload
  a tényleges completionig/pin release-ig elszámolva marad.
- **Időalapok:** terrain request és navigation deadline monoton falióra.
  Warp megtartja az 5 s simulation limitet, a terrain-erőforráskérés ezen
  felül legfeljebb 5 s faliórát várhat. Navigation a teljes job határidején
  lezárja a részleges pinhalmazt akkor is, ha nincs expansion quota.
- **Magas vízállás:** 90 % fölött proaktív eviction 75 %-ig.
- **Igény (demand):**
  - minden játékos jelenlegi chunkja minden tickben;
  - a mozgó entitások `sebesség × lookahead` pontjának chunkja;
  - minden `NotResident` lekérdezés chunkja;
  - a navigációs jobok és a spawn-újrapróbák.

  Egy dormant (nem mozgó) mob nem igényel semmit.
- **Lookahead:** `clamp(3 × mért p99 betöltési idő + egy tick, 0.5 s, 5 s)`.
  Nincs a 120 m-es ghost-sugárhoz, az 500 m-es load-field cellához vagy a
  zónamérethez kötve.
- **Metrikák** (`TerrainStreamer::Stats`, `WorldRuntime::GetTerrainStats`):
  - chunkok állapot szerint: resident / waiting / loading / failed /
    invalid / pinned (permanent, reader) / retired;
  - saját könyvelés: resident, pinned, in-flight, retired, metaadat,
    accounted;
  - csúcsok: accounted, resident, in-flight és retired pontosan; pinned
    10 Hz-es mintavétellel;
  - kumulatív: demands, hits, misses, deduplicated (az elutasítás/backoff miatt
    nem teljes felbontásuk), betöltések, hibák,
    stale, cancelled, admission waits / rejects, evictions / frees,
    replaced, olvasott bájtok;
  - high-water: repülő jobok és várólista;
  - késleltetés: betöltés (admission → publikálás) és miss (első igény →
    publikálás) p50 / p99 / max;
  - fogyasztói számlálók: lépések ok / várakozó / invalid miatt elutasított /
    blokkolt / világon kívüli; mob-spawn invalid miatt; eldobott respawn;
    navigációs státuszok.

  SL-1 külön számlálja a logical accepted/consumed/cancelled/timed-out/
  rejected/pending/ready állapotokat, pressure evictiont, suppressed retryt,
  safe-pointot és drain-várakozást. A readiness setup/warmup/measure ablak
  külön hisztogram: új fizikai miss-completion minták darabszáma és a
  logaritmikus p99 felső korlátja (legfeljebb 10% bucket-szélesség), üres
  ablakban **N/A**. A kezdeti és megmaradt várólista kora külön látható;
  függő kérés nem kerül a completion-percentilisbe. A régi recent p99 csak
  rolling diagnosztika/lookahead, nem a mérési ablak percentilise.

Az SL-0/SL-1 bizonyíték és review-korlátok:
[`map-streaming-admission-liveness.md`](map-streaming-admission-liveness.md).
Az alábbi MAP-3 számsorok történeti mérések, nem az SL-1 build eredményei.

### 12.4 Mért eredmények (MAP-3, RelWithDebInfo, Windows 11, NVMe SSD)

**MAP-3 review-utómunka után (végső bináris):**

| Mérés | 16 MB budget | 3 MB budget (a munkakészlet alatt) |
|---|---|---|
| Betöltés / eviction | 1180 / 803 | 731 / 734 |
| Találati arány | 0.997 | 0.982 |
| Saját könyvelés csúcsa | 14.60 MB | 2.99 MB |
| Csúcsok: resident / in-flight / retired | 11.97 / 1.02 / 2.61 MB | 1.36 / 1.02 / 0.03 MB |
| Repülő jobok / várólista csúcsa | 32 / 281 | 32 / 281 |
| Betöltési késleltetés p50 / p99 | 5.91 / 17.25 ms | 1.64 / 13.55 ms |
| Miss-késleltetés p50 / p99 | 6.40 / 18.49 ms | 1140 / 3095 ms |
| Adatra váró lépés | 0 | 35.2 % |
| Supervisor-pass átlag | 0.41 ms | 0.52 ms |
| Magasságok | bitre = referencia | bitre = referencia |

A részletes tábla (sorok, admission, végállapot, RSS) és az élettartam-
stressz: [`map3-report.md`](map3-report.md) 3.7 és 4.4.

**Történeti (a MAP-3 első riportja, a review előtt, változatlanul).** A
betöltési késleltetést ekkor még az elnyelt I/O-ébresztés torzította
(12.2); ugyanaz a bináris egy későbbi futásban 14.8 ms-os p50-et mért.

Futás: `streamsoak`, 100 km × 100 km, 16 m-es cellák, 9604 chunk (152 MB),
16 MB budget, 200 barangoló játékos (2 másodpercenként 20 % áthelyezve),
3400 entitás, 60 s.

| Mérés | Eredmény |
|---|---|
| Hideg indulás, eager | 2.06 s, 151 MB rezidens |
| Hideg indulás, streaming | 0.99 s (9604 fájl méret-ellenőrzés), 16 KB rezidens |
| Betöltés / eviction | 1179 betöltés, 796 eviction |
| Találati arány | 0.997 |
| Saját könyvelés csúcsa | 14.6 MB (≤ 16 MB) |
| Betöltési késleltetés | p50 6.0 ms, p99 12.1 ms |
| Adatra váró lépés | 0 (a prefetch megelőzött) |
| Magasságok | minden entitás z-je bitre egyezik az eager referenciával |

A folyamat RSS-e ugyanebben a futásban 307 → 340 MB. Ebben benne vannak a
zónák, az entitások, az allokátor és a teszt által tartott 151 MB-os eager
referencia is; a streamer saját része ≤ 16 MB.

---

## C. függelék — a korábbi koncepció (NEM implementált)

A formátum első tervéből az alábbiak **nem** valósultak meg, és nem érvényesek
(jövőbeli bővítés csak új, explicit verzióval):
- `worldlogic.dat` Cap'n Proto `MapLogic` sémaként (zóna/spawnTable/NPC/warp
  kind/lokáció/aréna) — a tényleges formátum az 5. pont MXL1 v1 binárisa;
- chunkfájl-név `c_<x>_<y>.mxchunk`, konténer-verzió 1, u16 `formatVersion` —
  tényleges: `chunk_<x>_<y>.mxchunk`, konténer-verzió 2, u32;
- per-vertex splat-súlylista (activeTextureCount + textureIds + súlyok) —
  tényleges: két RGBA8 kép chunkonként;
- attribútum-bitkiosztás (bit 0 = Walkable, 1 = Blocked, Water, SafeZone, …) —
  tényleges: **bit 0 = blokkolt**, a többi foglalt. Felületi típus, víz, PvP stb.
  csak új réteg-verzióval jöhet (MAP-3 döntés);
- `environment.dat`, `/textures`, `/objects` csomagrész — nincs fogyasztó.

## 13. MAP-4: warp és spawn életciklus (2026-09-27)

### Warp

A csomagon belüli, nem nulla `WarpRegion::id` stabil; a fél-nyitott
forrástérfogat világkoordinátákban van. A spawnkor a forrásban megjelenő
entitás belépőnek számít. Sikeres teleport után 1 s authoritative szimulációs
cooldown következik, és minden forrástérfogatot el kell hagyni. Csak egy
cooldown után, forráson kívül megfigyelt tick élesít újra. A célként elért
másik trigger nem aktiválódik magától. A loader továbbra is elutasítja az
önhurkot és az A↔B ciklust mindkét módban. SL-2 óta a nem ciklikus lánc
is betöltési hiba az alapértelmezett strict szerver-policy mellett. Csak
explicit legacy módban marad warning és a fenti rearm/cooldown védelem.
A legacy kliensparser API viselkedése változatlan; ez nem kliensfejlesztés.
A becsekkolt láncos `test_zone` változatlan, az azt használó szervertesztek
explicit legacyt választanak. Részletes bizonyíték: [SL-2 review](map-streaming-sl2-review.md).

A `WarpState` az authoritative player komponense, az `EntityTransfer`
átviszi migrációkor, splitkor és merge-kor. Azonosítói nem ZoneId-k.
Entitásonként legfeljebb egy függő terrain-kérés létezik, legfeljebb 5 s
szimulációs ideig. Kilépés, másik forrásba lépés vagy despawn megszünteti.
Nincs entitást tartó I/O callback; a megosztott chunkot más fogyasztó tovább
igényelheti. Időtúllépés/elutasítás után új belépés kell.

Teleport előtt a cél cellájának járhatónak, a magasságnak `Ok`-nak és a
célnak érvényes aktív zónában kell lennie. `NotResident`: helyben vár és
adatot kér. Hibás/blocked/outside cél: egyetlen elutasítás belépésenként,
pozícióváltás és WARN/tick nélkül. Siker esetén mindhárom koordináta együtt
változik. Másik zónába teleportálás a meglévő migrációs soron és
ownership-tranzakción megy át; itt nincs szomszédsági vagy 5 m-es
hysteresis-feltétel. A forrásban a célpozíció rögzített az átadásig.

A meglévő `TerrainQueryCounters` összegzi a sikeres, elutasított, megszakított
és lejárt warpokat, valamint a hiányzó adatra várás szimulációs másodperceit.
Ezek kumulatív világértékek (`WorldRuntime::GetTerrainStats`), nem faliórás
I/O-percentilisek. Eager világban is gyűlnek.

### mobSpawns réteg v2

Manifest v3 alatt a `mobSpawns` réteg `version=2` esetén minden rekord:

```text
spawn_id=123 area_id=7 mob_type_id=1 x=-100 y=50 count=3 radius=5
```

- `spawn_id`: egyedi u32, 1..4294967295 a csomag világán belül.
- `area_id`: 0 = világ; egyébként létező area, amelynek fél-nyitott
  bounds-a tartalmazza a spawn középpontját. Nem szerverzóna-azonosító.
- A többi mező és a korlátok a v1 szabályai. Ismeretlen/ismételt mező,
  hiányzó mező, hibás id/referencia/típus/koordináta betöltési hiba.
- A generátor `PackageWriteSpec::mob_spawns_version=2` kapcsolóval írja.
  A writer v2 manifesthez nem enged v2 spawnréteget.

Átmenet: a régi v1 rekordok változatlanul elfogadottak. Identitásuk a
nem üres/nem komment rekordok 1-alapú sorszáma az immutábilis csomagban,
`area_id=0`. A kommentek beszúrása nem változtatja meg ezt; a rekordok
átrendezése igen. Tartalomszerkesztésen át stabil azonosítókhoz v2-re kell
konvertálni és explicit id-ket rendelni. V1 alatt a v2 mezők elutasítottak.
Nincs automatikus csomagátírás, live package reload vagy DB-perzisztencia.

A runtime spawnpont és a `MobSpawnRef` megőrzi a csomag-azonosítót;
az `EntityTransfer` ezt is átviszi. A `SpawnCoordinator` egyszer aktiválja
az egyes kezdő spawnhelyek `count` darabját: ismételt kezdő aktiválás nem
hozza létre őket újra. Sikertelen, terrainre váró kezdő példány később még
aktiválható. A meglévő respawn külön út marad. Chunk load/evict és zóna
split/merge nem kezdeményez új aktiválást, és nem despawnol entitást.

### Explicit terrain-demand tartomány

A `PostTerrainDemand(x,y,r)` a világra metszett, zárt igénydoboz összes
érintett chunkját kéri, beleértve a középpontot és a chunkhatár másik
oldalát. Nem véges/bounds-on kívüli vagy negatív sugarú kérés nem indít I/O-t.
Ez igény, nem pin és nem betöltési sikerígéret: szűk budgetnél várakozhat.
