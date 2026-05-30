# Térkép-adatformátum — saját, clean-room kontraktus

> **Cél:** egy önálló, self-describing térkép-formátum, ami a heightmap + chunk + attribútum-rács *koncepciót* megtartja (mainstream, nem elavult), de minden konkrét megvalósítása **saját** — saját konténer, magic, séma, betöltő —, így nem köthető a Metin2/Gameforge formátumához.
>
> **Jogi megjegyzés (nem jogi tanács, nem vagyok jogász):** a clean-room cél a saját konténer + saját séma + saját kód + saját/licencelt asszetek. A formátum-koncepció (magasság-rács) általában nem védett; a konkrét kód, asszet, pontos elrendezés átvétele igen. Éles bizonyossághoz konzultálj jogásszal.

---

## 1. Alapelvek

1. **Self-describing.** Egy `map.manifest` leírja a világ dimenzióit, a cellaméretet, a chunkolást, a koordináta-konvenciót. Semmi hardcode-olt feltevés (szemben a Metin2 implicit konvencióival).
2. **Kliens/szerver réteg-szétválasztás.** A vizuális rétegeket (splat, objektum, környezet) csak a kliens tölti; a gameplay-rétegeket (attribútum/collision, zóna, spawn, NPC, warp) a szerver. A magasság közös. Ez ugyanaz az elv, mint a render distance ≠ AOI.
3. **Két tárolási stílus a tartalom jellege szerint:**
   - **Strukturált, ritka gameplay-adat** (manifest, zónák, spawnok, NPC-k, warpok, lokációk, arénák) → **saját Cap'n Proto séma** (már a build részed, te birtoklod, verziózott).
   - **Tömeges, homogén rács-adat** (magasság, splat-súlyok, attribútumok) → **saját magic-byte-os bináris konténer**, szekcionálva, hogy a szerver csak a kellő szekciókat olvassa.
4. **Chunkolt streamelés.** A 10×10 km nem töltődik be egyben; a néző/zóna körüli chunkok streamelődnek.
5. **Verziózott.** A manifest és a chunk-konténer is verziószámot hordoz a jövőbeli evolúcióhoz.

## 2. Csomag-struktúra (directory)

```
/<mapnev>/
  map.manifest            # Cap'n Proto: dimenziók, skála, chunk-séma, ref-ek
  worldlogic.dat          # Cap'n Proto: zónák, spawnok, NPC-k, warpok, lokációk, arénák (SZERVER)
  environment.dat         # Cap'n Proto: világítás/köd/skybox régiónként (KLIENS)
  /chunks/
    c_<cx>_<cy>.mxchunk    # saját konténer: height + splat + attribútum szekciók
    ...
  /textures/              # terrain textúrák (a splat-csatornák ezekre hivatkoznak)
  /objects/               # statikus mesh ref-ek / placement-adat
```

## 3. Manifest (Cap'n Proto — saját séma)

```capnp
struct MapManifest {
  formatVersion   @0 :UInt16;        # = 1
  worldId         @1 :Text;
  worldName       @2 :Text;

  worldSizeCells  @3 :UInt32Pair;    # pl. 5000 x 5000
  cellSizeMeters  @4 :Float32;       # pl. 2.0
  heightUnit      @5 :HeightUnit;    # centimeter (int16 tárolás)

  chunkSizeCells  @6 :UInt16;        # pl. 125 (= 250 m egy chunk)
  zoneGridDims    @7 :UInt16Pair;    # pl. 10 x 10
  zoneSizeCells   @8 :UInt16;        # pl. 500 (= 1000 m egy zóna; zóna = N×N chunk)

  texturePalette  @9 :List(TextureRef);   # globális textúra-paletta, indexelve
  worldLogicFile  @10 :Text;         # "worldlogic.dat"
  environmentFile @11 :Text;         # "environment.dat"

  enum HeightUnit { centimeter @0; millimeter @1; }
  struct UInt32Pair { x @0 :UInt32; y @1 :UInt32; }
  struct UInt16Pair { x @0 :UInt16; y @1 :UInt16; }
  struct TextureRef { id @0 :UInt16; path @1 :Text; tilingMeters @2 :Float32; }
}
```

> A chunk-méret legyen a zóna-méret egész osztója (pl. 125 cella chunk → 4×4 chunk / zóna; 40×40 chunk az egész világra). Így a chunk- és zónahatárok illeszkednek.

## 4. Chunk-konténer (`.mxchunk` — saját bináris, little-endian)

Egy chunk egy fájl, fejléccel és szekció-táblával (TOC), hogy a szerver **csak a Height + Attribute szekciókat** olvashassa, a vizuálisakat (Splat) átugorva.

### Fejléc
```
[magic : 4 bájt  = 'M','X','C','1']      # SAJÁT magic, nem Metin2
[version : u16   = 1]
[chunkX : u16]
[chunkY : u16]
[cellsPerSide : u16]                      # = manifest.chunkSizeCells (redundáns ellenőrzés)
[sectionCount : u16]
```
### Szekció-tábla (sectionCount × 12 bájt)
```
[sectionType : u16]   # 1=Height, 2=SplatWeights, 3=Attributes (bővíthető)
[elementFormat : u8]  # pl. 0=int16, 1=f32, 2=u8, 3=u16, 4=u32
[reserved : u8]
[byteOffset : u32]    # a fájl elejétől
[byteLength : u32]
```
### Szekciók payloadja

- **Height (type 1):** `(cellsPerSide + 1)²` minta, `int16` cm-ben. A +1 a cellasarkokhoz (fencepost); a jobb/alsó él megegyezik a szomszéd chunk bal/felső élével (a konverter ugyanazt az értéket írja → varratmentes).
- **SplatWeights (type 2):** per-vertex `(cellsPerSide + 1)²` súly, a chunkban aktív textúrákra:
  ```
  [activeTextureCount : u8]
  [textureIds : activeTextureCount × u16]          # a manifest paletta-indexei
  [weights : (cellsPerSide+1)² × activeTextureCount × u8]   # normalizált súlyok, shaderben kevernek
  ```
  (Ez a per-vertex súly-keverés a Metin2 cellánkénti egy-index-splatjánál modernebb, sima átmenetekkel.)
- **Attributes (type 3):** per-cell `cellsPerSide²` érték, `u16` bitfield (lásd 5.). Ezt a **szerver** olvassa (a kliens opcionálisan, predikcióhoz).

## 5. Attribútum-bitfield (saját enum, u16)

```
bit 0  : Walkable        # bejárható
bit 1  : Blocked         # tömör akadály
bit 2  : Water           # sekély víz (lassít)
bit 3  : DeepWater       # úszás/halál
bit 4  : SafeZone        # nincs harc
bit 5  : NoPvP
bit 6  : NoMount
bit 7  : NoBuild
bit 8  : DamageTile      # sebző terep
bit 9  : Trigger         # esemény-trigger cella
bit 10 : ClimbSlope      # meredek, lassít/blokkol mozgástól függően
bit 11-15 : reserved
```
Saját jelentés-hozzárendelés; nincs átvéve sehonnan.

## 6. worldlogic.dat (Cap'n Proto — a SZERVER gameplay-adata)

```capnp
struct MapLogic {
  formatVersion @0 :UInt16;
  zones        @1 :List(Zone);
  spawnTables  @2 :List(SpawnTable);
  spawnRegions @3 :List(SpawnRegion);
  npcs         @4 :List(NpcPlacement);
  warps        @5 :List(WarpPoint);
  locations    @6 :List(NamedLocation);
  arenas       @7 :List(BossArena);

  struct CellPos { x @0 :UInt32; y @1 :UInt32; }
  struct CellRect { minX @0 :UInt32; minY @1 :UInt32; maxX @2 :UInt32; maxY @3 :UInt32; }

  struct Zone {
    id @0 :UInt16; name @1 :Text;
    gridX @2 :UInt16; gridY @3 :UInt16;
    bounds @4 :CellRect;
    musicRef @5 :Text;
  }
  struct SpawnTable {
    id @0 :UInt16;
    entries @1 :List(Entry);
    struct Entry { mobId @0 :UInt32; weight @1 :UInt16; maxCount @2 :UInt16; respawnSeconds @3 :UInt32; }
  }
  struct SpawnRegion {
    id @0 :UInt16; zoneId @1 :UInt16;
    shape @2 :CellRect;            # MVP: téglalap; később polygon
    spawnTableId @3 :UInt16;
  }
  struct NpcPlacement { npcId @0 :UInt32; pos @1 :CellPos; heading @2 :UInt16; zoneId @3 :UInt16; }
  struct WarpPoint {
    id @0 :UInt16; area @1 :CellRect;
    targetPos @2 :CellPos; targetZoneId @3 :UInt16;
    kind @4 :WarpKind;
    enum WarpKind { intraZone @0; crossZone @1; instance @2; }
  }
  struct NamedLocation {
    id @0 :UInt16; name @1 :Text; pos @2 :CellPos;
    kind @3 :LocKind;
    enum LocKind { town @0; recall @1; spawnPoint @2; }
  }
  struct BossArena {
    id @0 :UInt16; zoneId @1 :UInt16;
    center @2 :CellPos; radiusCells @3 :UInt16; leashRadiusCells @4 :UInt16;
  }
}
```

> A `BossArena` azért van a formátumban, hogy a szerkesztő **validálni** tudja: az aréna (center + leashRadius) teljesen a zóna `bounds`-ján belül van, és nem lóg a zónahatárra — pont az architektúra-szabály, amit korábban rögzítettünk.

## 7. Ki mit tölt

| Réteg | Kliens | Szerver |
|---|---|---|
| manifest | ✓ | ✓ |
| Height (chunk) | ✓ | ✓ (Z-mintavétel, spawn-elhelyezés) |
| SplatWeights (chunk) | ✓ | — |
| Attributes (chunk) | opcionális (predikció) | ✓ (mozgás-validáció) |
| worldlogic (zónák/spawn/npc/warp/lokáció/aréna) | részben (warp-trigger, lokáció) | ✓ |
| environment | ✓ | — |
| textures / objects | ✓ | objektum-collision proxy, ha kell |

A szerver a chunk-konténerből a szekció-tábla alapján **csak a Height + Attributes szekciókat seek-eli/olvassa**, a SplatWeights-et átugorja — ez a kliens/szerver szétválasztás konkrét megvalósítása.

## 8. Koordináta-konvenció (rögzíti az M1 nyitott kérdést)

- **Cella:** `cellSizeMeters` (alap 2.0 m), a manifest deklarálja.
- **Futásidejű világpozíció:** `f32` méter (a sim és a wire ezt használja — lásd M1 spec).
- **Tárolás:** cella-index (`UInt32`) + magasság cm-ben (`int16`). A DB `characters.pos_x/pos_y` ehhez igazítva (cella-index vagy mm-fixpont — egy explicit, dokumentált konvenció).
- A manifest `cellSizeMeters` + `heightUnit` mezője teszi a formátumot self-describing-gá: a kód nem feltételez, hanem beolvas.

## 9. Verziókezelés

- `MapManifest.formatVersion`, `MapLogic.formatVersion`, és a chunk-konténer `version` mezője.
- A betöltő ismeretlen/újabb verziónál hibát ad, nem értelmez félre. Új szekció-típusok visszafelé kompatibilisen hozzáadhatók (a régi betöltő az ismeretlen `sectionType`-ot átugorja a TOC offset/length alapján).

## 10. Migráció a jelenlegi (Metin2-ihlette) betöltőből

- A mostani `ReadMapSetting` (text: `mapsize`/`cellscale`/`heightscale`) → a `MapManifest` mezőire képződik le.
- A jelenlegi magasság-rács (`m_heightCmGrid`) → a chunk-konténer Height szekciói (chunkokra vágva, +1 él-átfedéssel).
- A jelenlegi tile-index rács → splat-súlyokra konvertálva (egy-index → egy csatorna 100%-os súly; vagy lágyítva).
- Egy **egyszeri konverter** beolvassa a meglévő teszt-mapot és kiírja az új formátumot; a renderelő `LoadMap`-je az új manifest + chunk-konténer olvasására módosul. A `SampleHeightAt`/`SampleHeight` logika koncepcionálisan változatlan.

## 11. Mit adj Codexnek (a betöltő/mentő feladat váza)

Külön mérföldkő/feladat (nem M1 — a tartalom-pipeline-hoz, M5 környékén vagy párhuzamos sávként):
1. A `map.manifest`, `worldlogic.dat`, `environment.dat` Cap'n Proto sémák a saját sémakönyvtáradban.
2. A `.mxchunk` konténer író/olvasó (magic + TOC + szekciók), seek-alapú szelektív szekció-olvasással.
3. Szerver-oldali `MapLoader`: manifest + worldlogic + chunkonként csak Height + Attributes.
4. Kliens-oldali adaptáció: a `TerrainRenderer::LoadMap` az új formátumra (Height + SplatWeights + textúrák), a magasság-mintavétel megtartva.
5. Egyszeri konverter a jelenlegi teszt-mapról az új formátumra.
6. Tesztek: konténer round-trip (írás→olvasás bitre egyezik), szelektív szekció-olvasás (szerver csak Height+Attr), chunk-él varratmentesség (szomszédos chunkok éle egyezik).

> Megjegyzés: a fenti capnp vázlatok szándékosan tömörek (mezőnevek + típusok). A pontos `@n` indexeket és a `List`/`union` finomságokat a Codex-implementáció rögzíti a meglévő sémastílusodhoz igazítva.