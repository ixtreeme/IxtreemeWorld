# Világcsomag-formátum — kötelező kontraktus (MAP-1)

> **Státusz: kötelező, a kóddal egyező szerződés.** Ez a dokumentum a
> `shared/map` tényleges írójának (`WorldPackageWriter`) és szigorú
> olvasójának (`WorldPackage`, `LoadServerWorld`) leírása. Ahol a kód és ez a
> leírás eltér, az hiba. A korábbi (clean-room) koncepció el nem készült részei
> a [C. függelékben](#c-függelék--a-korábbi-koncepció-nem-implementált) vannak,
> kifejezetten **nem** érvényes formátumként.
>
> **Jogi megjegyzés (nem jogi tanács):** a clean-room cél a saját konténer +
> saját séma + saját kód + saját/licencelt asszetek. A magic-ek (`MXC1`,
> `MXL1`, `MXWB`) és az elrendezések saját tervezésűek.
>
> Követelmények és nyomkövetés: [`map-data-layer-requirements.md`](map-data-layer-requirements.md)
> (R1–R13, MAP-0/MAP-1 mátrix). Bizonyíték: `worldbench --mode worldpackage`
> (korpusz), `--mode mapaudit` (legacy vs szerver út),
> `gameserver/scripts/map1_startup_acceptance.sh` (valódi indulási út).

## 1. Alapelvek

1. **Önleíró csomag.** A `map.manifest` rögzíti az azonosítót, a koordinátákat,
   a chunk-kiosztást, a rétegeket és a hivatkozásokat. Semmi nem jön
   fordításkori útvonalból vagy implicit konvencióból.
2. **Chunk ≠ area ≠ szerver-zóna ≠ kliens-map.** A formátum nem tartalmaz
   szerver-zóna fogalmat (v3-ban a `zoneGridDims`/`zoneSizeCells` tilos). A
   worldlogic zónái a MAP-1-ben még a szerver kezdeti partíciós magjai
   (bootstrap) — ezt a MAP-2 választja szét (AreaId ≠ ZoneId).
3. **Közönség (audience) rétegenként.** A szerver csak a szerver/shared
   rétegeket dekódolja és tartja memóriában (magasság, attribútum, worldlogic,
   spawn-tábla). Kliens-render adat (splat, textúra-paletta, víz-vizuál) soha
   nem kötelező a szervernek, és nem foglal szerver-memóriát.
4. **Hordozható bináris.** Minden bináris mező little-endian, mezőnként
   írva/olvasva; fordítófüggő struktúra-memória soha nem formátum.
5. **Nincs csendes értelmezés.** Ismeretlen/újabb verzió, nem támogatott
   kötelező réteg, sérült vagy hiányzó adat strukturált hiba; nincs lapos
   fallback-világ.

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
kerültek be (@12..@16), így egy v2-olvasó hiba nélkül parse-ol egy v3 üzenetet —
ezért **a v2-olvasónak kötelező elutasítania a `formatVersion != 2`-t** (a
legacy `LoadManifest` ezt teszi).

| Mező | v2 (legacy, csak olvasás) | v3 (aktuális) |
|---|---|---|
| `formatVersion` @0 | 2 | 3 |
| `worldId` @1 | 1..64 karakter `[A-Za-z0-9_.-]` | ugyanaz |
| `worldName` @2 | ≤ 256 bájt | ugyanaz |
| `worldSizeCells` @3 | cellák X-en (és Y-on) | cellák X-en, 1..65536 |
| `cellSizeMeters` @4 | véges, 0.05..1000 | ugyanaz |
| `heightUnit` @5 | `centimeters` | `centimeters` |
| `chunkSizeCells` @6 | 1..4096 | ugyanaz |
| `zoneGridDims` @7 | **figyelmen kívül hagyva** (a régi generátor a chunk-számot írta ide; a legacy betöltő chunk-rácsként olvasta — R1) | **0 kötelező** (`MANIFEST_FIELD_FORBIDDEN`) |
| `zoneSizeCells` @8 | figyelmen kívül hagyva | **0 kötelező** |
| `texturePalette` @9 | kliens-adat, a szerver nem tölti | ugyanaz |
| `worldLogicFile` @10 | üres = `worldlogic.dat` | **üres kötelező** (a rétegekből jön) |
| `environmentFile` @11 | nincs fogyasztója (info) | **üres kötelező** |
| `worldSizeCellsY` @12 | tilos (kétértelmű) | cellák Y-on; MAP-1-ben = X (`UNSUPPORTED_FEATURE`) |
| `origin` @13 | tilos | `Float64` x,y; MAP-1-ben (0,0) (`UNSUPPORTED_FEATURE`) |
| `chunkGrid` @14 | tilos | = ceil(worldSize / chunkSize) tengelyenként |
| `layers` @15 | tilos (v2-ben implicit, lásd lent) | réteg-deklarációk (≤ 32) |
| `chunks` @16 | tilos | pontosan egy bejegyzés rácscellánként |

Az üzenet legyen pontosan egy Cap'n Proto üzenet (mögötte nincs bájt),
8-bájtos szavak egész száma; a dekódolás korlátos (traversal ≤ 4× méret,
nesting ≤ 16). Hiba: `MANIFEST_CORRUPT` (200).

**Geometria (mindkét verzió):** chunk-rács = `ceil(worldSizeCells /
chunkSizeCells)`; MAP-1-ben a világméret a chunkméret egész többszöröse
(részleges szélső chunk: `UNSUPPORTED_FEATURE`, MAP-2). A rezidens
magasságminta `(W+1)×(H+1) ≤ 2^28` (`MANIFEST_SIZE_OVERFLOW`).

### 3.1 Rétegek (`layers`, v3)

```
LayerDecl { kind: LayerKind; required: Bool; audience: server|client|shared;
            version: UInt32; file: Text }
```

| kind | Tárolás | Elemformátum | Szerver |
|---|---|---|---|
| `height` (0) | chunk-szekció 1 | int16 cm, `(n+1)²` minta | kötelező, betölti |
| `attributes` (1) | chunk-szekció 3 | uint16 bitmező, `n²` cella | kötelező, betölti |
| `splatA` (2) / `splatB` (3) | chunk-szekció 2 / 4 | u16 w, u16 h, RGBA8 | kihagyja (Startup: csak tartomány; Full: szerkezet) |
| `worldLogic` (4) | fájl | MXL1 v1 | kötelező, betölti |
| `mobSpawns` (5) | fájl | mob_spawns v1 | opcionális; ha van, szigorúan betölti |
| `water` (6) | fájl | MXWB | nem validálja (`LAYER_NOT_VALIDATED` info) |

Szabályok: egy `kind` legfeljebb egyszer (`LAYER_DUPLICATE`); chunk-szekció
rétegnek nincs `file`-ja, fájl-rétegnek van (`LAYER_DECL_INVALID`); szerver-adat
(height/attributes/worldLogic/mobSpawns) nem lehet `client` közönségű
(`LAYER_DECL_INVALID`); a szerver számára a height, attributes és worldLogic
jelenléte kötelező (`LAYER_REQUIRED_MISSING`). Ismeretlen `kind` vagy nem
támogatott `version` **kötelező** rétegnél hiba (`LAYER_UNSUPPORTED`),
**opcionálisnál** dokumentált kihagyás (`LAYER_SKIPPED`: ismeretlen kind = info,
ismeretlen verzió = warning). Minden réteg-verzió jelenleg 1.

### 3.2 Chunk-index (`chunks`, v3)

```
ChunkRef { x, y: UInt32; file: Text; byteSize: UInt64; crc32: UInt32 }
```
Rácscellánként pontosan egy bejegyzés, a rácson belül, egyedi fájllal
(`CHUNK_INDEX_INVALID`). A fájlméret egyezzen `byteSize`-zal
(`CHUNK_SIZE_MISMATCH`), a teljes fájl CRC-32-je (IEEE 802.3, azonos a zlib
`crc32()`-vel) `crc32`-vel (`CHUNK_CHECKSUM_MISMATCH`).

### 3.3 v2 → v3 átmeneti szabály (a v2 **nem** értelmeződik csendben v3-ként)

Az olvasó a v2-t kizárólag az alábbi rögzített szabállyal fogadja:
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

## 4. Chunk-konténer (`.mxchunk`, MXC1, little-endian)

### Fejléc (14 bájt)
| Offset | Méret | Mező | Szabály |
|---|---|---|---|
| 0 | u32 | magic | `0x3143584d` (`"MXC1"`) |
| 4 | u16 | version | `2` (konténer-verzió) |
| 6 | u16 | chunkX | = index / fájlnév szerinti x |
| 8 | u16 | chunkY | = y |
| 10 | u16 | cellsPerSide | = `chunkSizeCells` |
| 12 | u16 | sectionCount | 1..16 |

Hiba: `CHUNK_HEADER_INVALID` (300), TOC-hiba `CHUNK_TOC_INVALID` (301).

### Szekció-tábla (`sectionCount × 12` bájt, a fejléc után)
| Offset | Méret | Mező |
|---|---|---|
| 0 | u16 | sectionType (1 height, 2 splatA, 3 attributes, 4 splatB) |
| 2 | u8 | elementFormat (1 int16, 3 u16-bitmező, 4 RGBA8-kép) — típushoz kötött |
| 3 | u8 | reserved (v3: 0) |
| 4 | u32 | byteOffset (fájl elejétől) |
| 8 | u32 | byteLength |

Szabályok: egy típus legfeljebb egyszer; minden szekció a tábla után és a
fájlon belül; **a tábla utáni minden bájt pontosan egy szekcióhoz tartozik**
(nincs átfedés, rés, záró szemét) — `CHUNK_SECTION_RANGE` (302). v3-ban csak
deklarált réteg szekciója szerepelhet; deklarált splat-réteg minden chunkban
jelen van (`CHUNK_SECTION_MISSING`).

### Szekciók
- **Height (1):** `(n+1)²` int16 minta cm-ben, soronként (y külső, x belső). A
  szomszédos chunkok közös élmintái **azonosak** (`CHUNK_EDGE_MISMATCH` 307;
  nincs „utolsó író nyer").
- **Attributes (3):** `n²` uint16. **bit 0 = blokkolt (nem járható)**;
  bit 1..15 foglalt = 0 (`CHUNK_ATTRIBUTE_RESERVED_BITS`). A szerver
  járhatósága kizárólag ebből jön.
- **SplatA/B (2/4, kliens):** `u16 w, u16 h`, majd `w·h·4` bájt RGBA8;
  A és B geometriája azonos, minden chunkban azonos (`CHUNK_SPLAT_GEOMETRY`,
  csak Full mélységben dekódolva).
- Méretek: height `(n+1)²·2`, attributes `n²·2` bájt (`CHUNK_SECTION_SIZE`).

## 5. Worldlogic (`worldlogic.dat`, MXL1 v1, little-endian)

```
u32 magic = 0x314c584d ("MXL1")    u32 version = 1
u32 zoneCount   u32 spawnCount   u32 warpCount        (mindegyik ≤ 1024)
zone  × zoneCount : u32 id, u8 nameLen, nameLen bájt név, f32 minX, minY, maxX, maxY
spawn × spawnCount: u32 id, u32 zoneId, f32 minX, minY, maxX, maxY
warp  × warpCount : u32 id, f32 minX, minY, maxX, maxY, f32 targetX, targetY
```
A fájl pontosan a rekordok végéig tart (`WORLDLOGIC_TRUNCATED` 401,
`WORLDLOGIC_TRAILING_DATA` 402, fejléc: `WORLDLOGIC_HEADER_INVALID` 400).

Validáció (a világ: `[0, extentX) × [0, extentY)`, félig nyitott):
- **zónák** (MAP-1-ben ownership-bootstrap): ≥ 1 (`WORLDLOGIC_NO_ZONES`); id
  1..`0x00FFFFFF` (0 = partíciógyökér-sentinel, fölötte futásidejű id-k;
  `WORLDLOGIC_ID_INVALID`), egyedi (`…_ID_DUPLICATE`); véges, `min < max`
  (`…_RECT_INVALID`); a világon belül (`…_OUT_OF_BOUNDS`); félig nyitott
  téglalapként nem fedhetik át egymást (`…_ZONE_OVERLAP`) és hézag nélkül
  lefedik a világot (`…_COVERAGE_GAP`);
- **spawn-régiók:** id ≥ 1, egyedi; létező zónára hivatkoznak
  (`…_REFERENCE_INVALID`); a zónájukon belül vannak (`…_SPAWN_OUTSIDE_ZONE`);
- **warpok:** id ≥ 1, egyedi; forrás véges, `min < max`, világon belül; cél
  véges, `[0, extent)`-ben, **járható cellán** (`…_WARP_TARGET_INVALID`);
  trigger-gráf (i → j, ha i célja j forrásában van, a futásidő zárt
  intervallumával): **ciklus hiba** (`…_WARP_CYCLE`, önhurok is), lánc
  **warning** (`…_WARP_TARGET_IN_TRIGGER`; R6 szerint később hibává tehető —
  review-döntés).

Overlay-szemantikájú régiótípus (átfedhető) a MAP-1 formátumban még nincs; a
worldlogic zónái kizárólag ownership-bootstrapként érvényesek.

## 6. Spawn-tábla (`mob_spawns.conf`, mob_spawns v1, UTF-8/ASCII szöveg)

Soronként egy spawn-pont; `#`-tól sorvégig megjegyzés; üres sor megengedett.
```
mob_type_id=<u32 ≥ 1> x=<méter> y=<méter> count=<1..100000> radius=<méter ≥ 0>
```
Pontosan ez az öt kulcs, mindegyik egyszer, más kulcs nincs
(`SPAWNS_SYNTAX` 500); érték idézőjel nélkül, egész vagy véges decimális
(`SPAWNS_FIELD_INVALID` 501); a középpont `[0, extent)`-ben
(`SPAWNS_OUT_OF_BOUNDS` 502); ≤ 100000 sor (`SPAWNS_TOO_MANY` 503). A
`mob_type_id`-nak a szerver mob-típus registry-jében léteznie kell
(`SPAWNS_MOB_TYPE_UNKNOWN` 504 — a szerver ellenőrzi, a registry a szerver
konfigja, nem a csomagé).

## 7. Ki mit tölt

| Réteg | Kliens | Szerver (MAP-1) |
|---|---|---|
| manifest | ✓ (v2) | ✓ (v2/v3) |
| height | ✓ | ✓ rezidens |
| attributes | opcionális | ✓ rezidens (bit 0 = blokkolt) |
| splatA/B, texturePalette | ✓ | ✗ (nem kötelező, nem tölti, nincs memória) |
| worldLogic | részben | ✓ |
| mobSpawns | — | ✓ (ha van) |
| water (MXWB) | ✓ | ✗ (nem validált, nem használt) |

## 8. Validációs mélység és hibák

- **Startup** (a szerver indulása): manifest, réteg-deklarációk, chunk-index,
  minden chunk fejléce/táblája/tartományai + a dekódolt height/attribute
  tartalom (+ CRC v3-ban), worldlogic, spawn-tábla, mob-típus kereszt-
  ellenőrzés. A MAP-1 a teljes terraint rezidensen tartja, így **minden chunk
  induló chunk**. A kliens-szekciók csak tartomány-ellenőrzöttek.
- **Full** (`gameserver --validate-world-package <dir> [--mob-types f]`):
  Startup + a kliens-adat szerkezeti dekódolása (splat-geometria) és a
  kliens-fájlok olvashatósága. Validátor nélküli réteg (víz)
  `LAYER_NOT_VALIDATED` — soha nem „validált".
- **Streaming (MAP-3, előre rögzítve):** Startup = manifest + index +
  worldlogic + spawn + a kezdeti rezidenciához szükséges chunkok; a többi chunk
  a betöltésekor kap tartalmi ellenőrzést, és egy később sérültnek talált chunk
  **nem válhat üres szabad területté** (a zóna nem aktiválódik rá).

Minden hiba strukturált: `SEVERITY CODE(szám) package= layer= file=
chunk=(x,y) field= offset=|line= reason= expected=`. A számkódok stabilak (csak
hozzáfűzés): 100–106 csomag/út, 200–206 manifest, 210–215 réteg, 220–223
index/integritás, 300–307 chunk, 400–414 worldlogic, 500–504 spawn, 800 szerver
indulási adat (mob-típus registry), 900 belső. Lista:
`shared/map/include/map/WorldPackage.h` (`PackageErrorCode`).

## 9. Koordináta-konvenció (MAP-1)

- Cella: `cellSizeMeters`; futásidejű pozíció `f32` méter; magasság int16 cm.
- A világ `[0, extentX) × [0, extentY)`, origó (0,0); a validátor minden
  területi szabálya félig nyitott. (A futásidejű `Rect::Contains` még zárt —
  R9, MAP-2.)
- Nem-nulla origó és nem-négyzetes világ a v3 formátumban leírható, de a MAP-1
  `UNSUPPORTED_FEATURE`-rel elutasítja, amíg a MAP-2 a bounds-ot végig nem
  vezeti (R3).

## 10. Eszközök

- `mapgen_test_zone --out <dir> [--format 2|3] [--server-only] [--no-spawns]
  [--force]` — a közös íróval generál; kimeneti könyvtár kötelező, meglévő
  csomagot `--force` nélkül nem ír felül (a becsekkolt `test_zone` véletlen
  felülírása kizárt; az szerkesztővel módosított tartalom).
- `gameserver --validate-world-package <dir>` — offline Full validáció, exit
  0 = érvényes, 3 = érvénytelen.
- Szerver-futtatás: [`gameserver.conf.example`](../gameserver/apps/gameserver/config/gameserver.conf.example)
  `world_mode` / `world_package` / `mob_types_config` kulcsai.

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
