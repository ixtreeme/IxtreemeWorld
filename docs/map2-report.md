# MAP-2 — munkariport (2026-09-25)

> **Státusz:** kész, a working tree-ben, **commit/push nélkül**. STOP — review
> a MAP-3 előtt.
> Alap: `With_Auriga` @ `ab2458de` (MAP-0 + MAP-1 commit).
> Munkaprompt: `IxtreemeWorld_Real_World_Map_Data_Layer_Opus_Prompt.md` (MAP-2),
> munkapontok: a MAP-2 lista 1–8. pontja.
> Részletes állapot és mátrix: [`map-data-layer-requirements.md`](map-data-layer-requirements.md)
> („MAP-2 státusz"); formátum-szerződés: [`map-data-format.md`](map-data-format.md).

## 1. Összefoglaló

| # | Munkapont | Eredmény |
|---|---|---|
| 1 | `AreaId` és `ZoneId` szétválasztása; kezdeti partíció a szerver-configból és a bounds-ból | **Kész** |
| 2 | A betöltött WorldBounds minden világszintű fogyasztóba | **Kész** |
| 3 | Negatív origó, nem-négyzetes világ, részleges szélső chunk | **Kész** |
| 4 | Egységes félig nyitott és bounds-on kívüli szemantika | **Kész** (szerver-oldal; a legacy kliens-API változatlan) |
| 5 | Fájlból jövő, nem sík terrain + független magasság-orákulum | **Kész** |
| 6 | A magassági tartomány verziózott bővítése | **Kész — értelmezéssel** (lásd 3. pont, 6. döntés) |
| 7 | Split/merge teszt a valódi loaderből, terrain-újratöltés és entitás-duplikáció nélkül | **Kész** |
| 8 | Követelmény- és formátumdokumentáció, regresszió, STOP | **Kész** |

Eredmények:
- minden új teszt zöld (RelWithDebInfo és Debug);
- a teljes regresszió mind a 22 base/új párja azonos PASS-számmal fut;
- a valódi indulási teszt 22/22.

Közben két valódi hiba került elő és lett javítva:
- a terrain-provider varrat-kerekítési hibája;
- a szintetikus mód indulási regressziója.

Terjedelem: 38 módosított fájl (+1845 / −697 sor) + 4 új fájl (~2000 sor, ennek
nagy része teszt).

## 2. Mi változott

### 2.1 Area ≠ szerver-zóna, kezdeti partíció (1., 2. pont)

**Area-k:** az MXL1 worldlogic „zone" rekordjai **area**-k.
- tervezési metaadat, saját `AreaId` névtér;
- id ≥ 1, a teljes u32 tartomány, hézag megengedett;
- diszjunktak, de **nem kell lefedniük a világot**; 0 darab is lehet.

**Szerver-zónák:** a `ZoneId`-ket a szerver osztja ki (1..N a kezdeti
leveleknek), a map id-jaitól függetlenül:
`ZoneManager::BuildInitialPartition`, `world/partition/RegionDefinition.cpp`.

A kezdeti partíció szerver-konfig a betöltött bounds-ra:

| Kulcs | Jelentés | Alapértelmezés | Tengelyenkénti tartomány |
|---|---|---|---|
| `partition_regions` | régiórács | 2x2 (szintetikus módban 1x1) | 1..16 |
| `partition_initial_leaves` | kezdeti levelek régiónként | 1x1 | 1..64 |

- A régiónevek a tengelyekhez igazodnak: `World`; `West`/`East`;
  `South`/`North`; `SouthWest`…`NorthEast`; más rácsnál `R<ix>_<iy>`.
- Minden kezdeti levél ≥ 240 m (2 × AOI-sugár). Ha nem, vagy a konfig nem
  értelmezhető: exit 2, még a DB és a hálózat előtt.
- Az indulási log régiónként kiírja a nevet és a félig nyitott bounds-ot.

Törölve:
- `RegionDefinition::DefaultRegions` (négy fix 100 km-es kvadráns);
- `ZoneManager::BuildFromWorldLogic`;
- a `FindIndexForPosition` lineáris fallbackje.

A régi WorldBench-szcenáriók 3-levelű tesztpálya-geometriája explicit
bootstrap-leírásként maradt meg (`LegacyTestMapLayout()`, a bench adja). Nem az
area-kból származik.

Minden világszintű fogyasztó a betöltött `TerrainService::Bounds()`-ot használja:
- régiók és levelek;
- load field (config és rács);
- activity field;
- terrain;
- spawn- és warp-validáció.

A partíció-vezérlés és a validátor is változott:
- `ZoneLoadMonitor`: a gyökerek kimaradnak a merge-csoportokból;
- `WorldValidator`: minden belső csomópont lefedését ellenőrzi (a gyökerekét
  is);
- a szimuláló levél-zónák halmazának egyeznie kell a gyökerekből elérhető
  levelekével (egy levél sem kerülheti meg a load monitort).

### 2.2 Geometria és formátum (3., 6. pont)

**Manifest v3:**
- `worldSizeCellsY` (@12) független X-től;
- `origin` (@13) bármilyen véges érték, negatív is;
- minden világkoordináta `|c| ≤ 131072 m` (f32 lépésköz ≤ 1/64 m);
- új `heightEncoding` mező (@17).

**Részleges szélső chunk (csak v3):**
- a tengelyenként utolsó chunk csak a világon belüli celláit tárolja;
- az MXC1 `cellsPerSide` a névleges lépték marad;
- v2-ben `UNSUPPORTED_FEATURE`.

**Height réteg v2** (a tartomány-bővítés):
- explicit `sampleType`: int16 vagy int32, chunk-elemformátum 2 = int32;
- `metersPerUnit`: 0.0001..10;
- `offsetMeters`: |·| ≤ 100000;
- a v1 (int16 cm, ±327.67 m) változatlanul érvényes;
- a v1 nem hordozhat `heightEncoding`-ot, a v2-nek kötelező.

**Worldlogic:**

| Kód | Név | Változás |
|---|---|---|
| 415 | `WORLDLOGIC_NO_PLAYER_SPAWN` | új: legalább egy spawn-régió kötelező |
| 416 | `WORLDLOGIC_SPAWN_BLOCKED` | új: a spawn-régió közepének járhatónak kell lennie |
| 408 | `COVERAGE_GAP` | visszavonva |
| 414 | `NO_ZONES` | visszavonva |

A warp-trigger gráfja félig nyitott tartalmazással épül.

Érintett fájlok:
- `shared/map/schema/map_manifest.capnp`;
- `shared/map/src/WorldPackage.cpp`;
- `shared/map/src/WorldPackageWriter.cpp` (író: nem-négyzetes, origó,
  részleges chunk, int32).

### 2.3 Szerver-terrain és lekérdezés (4., 5. pont)

Új `mx::map::ServerTerrain` (`shared/map/include/map/ServerTerrain.h`):
- chunkonkénti tárolás, `GridGeometry`, `HeightEncoding`;
- `TerrainStatus`: `Ok` / `OutsideWorld` / `NotResident` / `InvalidData`.
- **A hiányzó adat soha nem 0 m és nem járható.**

Interpoláció és cella-hozzárendelés:
- a magasság a tartalmazó cella négy sarokmintájának bilineáris interpolációja;
- a lekérdezés pontosan egy chunkot olvas (a határminta mindkét szomszédban
  megvan, egyezésük validált);
- a cellát **világ-egységben** dönti el.

A `TerrainService` erre épül:
- `Height` / `Cell` / `IsWalkable` státusszal;
- f32 bounds befelé kerekítve;
- a `SampleGroundHeight` és a `WorldExtentMeters` törölve.

### 2.4 Futásidejű szabályok (4. pont) — nincs általános clamp

| Mi | Szabály |
|---|---|
| Játékos-mozgás | Tengelyenként elmarad a lépés, ha a cél nem járható (világon kívül / nem rezidens / blokkolt); a pozíció nem kerül a szélre. A z csak `Ok` magasságból jön, különben az előző marad. |
| Mob-mozgás | Póráz után csak világon belüli, adattal bíró cellára lép (a blokkolt cellát még nem nézi — MAP-3). |
| Játékos-spawn | Debug felülírás (dev build) → a csomag első spawn-régiójának közepe → elutasítás (`EnterWorldReject SERVER_ERROR`). Egy jelölt csak akkor jó, ha világon belül van, járható, van magassága és van tulajdonos zónája. *Javítva a review után: az eredeti szöveg itt még DB-pozíciót is felsorolt harmadik lépésként; a DB-pozíció nem jelölt — lásd 11. pont.* |
| Mob-spawn | A világon kívüli vagy nem járható húzást újrasorsolja (legfeljebb 8×), utána kihagyja. |
| Warp | Félig nyitott trigger; a cél járható, a forrással azonos zónában. |
| Migráció / transzfer | A z csak `Ok` magasságból frissül. |
| Zóna-tulajdon | Egy világon belüli pont pontosan egy aktív levélé; a világon kívüli pont egyiké sem. |

### 2.5 Diagnosztika (7. pont)

Ha a szabályok elutasítanak egy **kényszerített** splitet (pl. a zóna a
min-méret határán van), az nem csendes no-op. Két nyoma marad:
- `WARN partition: forced split REFUSED zone=… reason=min-size`;
- egy `executed=false` / `SplitMinSize` döntési rekord.

### 2.6 Új tesztek

| Hol | Mi |
|---|---|
| `worldbench --mode terrain` | független orákulum + futásidejű perem-szabályok (`bench/MapRuntimeBench.cpp`) |
| `worldbench --mode mapsplit` | split/merge létra a valódi loaderből |
| `worldbench --mode worldpackage` | 12 új geometria-/magasság-eset + 415/416 + nagy és hézagos area-id |
| `--fixtures-out` | új fixture-ök: `geometry_v3`, `height_v2_int32`, `no_player_spawn` |
| `scripts/map1_startup_acceptance.sh` | +7 eset (3b, 6a–6e, 6c2); az 1d alap 2x2 partícióval |

## 3. Döntések — review-ra

1. **AreaId ≠ ZoneId:** az area metaadat; a szerver-zóna a szerver-konfigból
   és a bounds-ból jön.
2. **Kezdeti partíció:** `partition_regions` × `partition_initial_leaves`, a
   levél legalább 240 m. Explicit téglalap-lista csak teszteknek.
3. **Origó és pontosság:** Float64 origó, f32 futásidő, `|c| ≤ 131072 m`.
   - Egy 100 km-es, origó-központosított világ 1/256 m-es lépésközzel
     belefér.
   - Zóna-lokális koordináta most nem kell.
4. **Részleges chunk:** v3-ban engedett, csak a világon belüli cellákat
   tárolja. A határmintának egyetlen olvasó chunkja van.
5. **R10:** a világon kívül nincs talaj. Nincs clamp; minden rendszernek
   explicit szabálya van (2.4).
6. **⚠️ Magassági tartomány — értelmezés.**
   - A munkapont „a fenti döntés alapján" hivatkozott döntése nem volt
     látható.
   - A megvalósítás: **height réteg v2** explicit kódolással (int16/int32 +
     skála + offset).
   - Ha a döntés mást mondott (pl. fix int32 cm vagy float), ezt a pontot
     módosítani kell.

## 4. Javított hibák a munka során

1. **Varrat-kerekítés a terrain-providerben:**
   - Egy chunk-varrat alatt egy ulp-vel lévő pontot (pl. `x = 106 − 1.4e-14`)
     az osztás (`(x − origin)/cell`) a szomszéd chunkba sorolt.
   - Javítás: a cellát világ-egységben dönti el a dokumentált
     `[origin + i·cell, origin + (i+1)·cell)` szabály.
   - A `terrain` teszt `not-resident-vs-zero-meters` esete találta meg.
2. **Szintetikus mód:** az új `partition_regions=2x2` alapértelmezés miatt a
   2x1-es szintetikus világ nem indult.
   - Javítás: szintetikus módban 1x1 az alap.
   - Egy explicit `partition_regions` értéknek osztania kell a zónarácsot.
   - Explicit `partition_initial_leaves` csak `1x1` lehet (egyébként exit 2).
3. **Konfig-határ:** a régiórácsra `main` 64-et engedett,
   `BuildInitialPartition` 16-ot. Most mindkettő 16, pontos hibaüzenettel
   (indulási teszt 6c2).

## 5. Tesztek és eredmények

### 5.1 Új és érintett módok (végső binárisok)

| Teszt | RelWithDebInfo | Debug |
|---|---|---|
| `worldbench --mode terrain` | 18/18 PASS | 18/18 PASS |
| `worldbench --mode mapsplit` | 21/21 PASS | 21/21 PASS |
| `worldbench --mode worldpackage` | 117 PASS, 0 FAIL, 1 SKIPPED | 117 PASS, 0 FAIL, 1 SKIPPED |
| `worldbench --mode mapaudit` | server_changed=11, open/partial=3 (R6, R12, R13) | — |
| `scripts/map1_startup_acceptance.sh` (valódi `gameserver`, DB-vel) | 22/22 PASS | — |

A SKIPPED eset a `path-symlink-escape`: ezen a Windows-fiókon nincs
symlink-jogosultság. Nem PASS-ként számolva.

### 5.2 `terrain` — részletek

**Orákulum:** a fixture generátor-függvénye + saját bilineáris kiértékelés,
kód és adat nélkül, ami a providerrel közös lenne. Mellette kézzel számolt
minták.

A fixture:
- origó (−150, −90), 70×45 cella × 8 m, 3×2 chunk;
- az utolsó oszlop 6, az utolsó sor 13 cella;
- 0 m-es és 5 m-es plató, blokkolt folt.

Statikus lekérdezések:

| Eset | Mérés |
|---|---|
| Véletlen pontok | 20000 pont, max hiba 9.5e-7 m, 14347 negatív magasság |
| Kézi minták | −15.000 / −14.525 / 0.000 / 5.000 / 0.920 m, `vertex(70,45)` = 1.270 m |
| Szintek | a platók bitre pontosak, a lépcső-rámpa 0.46 / 0.92 / 1.38 m |
| Lejtő | 2000 pontpár, a különbségek hibája ≤ 1.8e-6 m |
| Varratok | x = 106, 362 és y = 166 pontosan és ±1e-3 m-re; a határminták egyeznek |
| Részleges chunk | 6×13 cella, 98 minta, hiba 1.2e-7 m |
| Világ-perem | 12 eset: a max él kívül, a min él belül, NaN/±inf kívül; az f32 szolgáltatás bounds-a pontos |
| `NotResident` vs 0 m | ugyanazon a ponton: betöltve `Ok` 0.00 m, kiürített chunkkal `NotResident` és nem járható |
| Varrat egyetlen gazdája | x = 106 → a keleti chunk, x = 106⁻ → a nyugati; y = 166⁻ / 166 ugyanígy |
| Height v2 int32 | 5000 pont, hiba 1.2e-4 m, `vertex(64,64)` = 2763.667 m |

Futásidő (a fixture-ből indított `WorldRuntime`):

| Eset | Eredmény |
|---|---|
| Játékos kelet felé fut (406 → a 410-es max él felé) | 409.8998-nál megáll; nincs clamp, z = orákulum |
| Délnyugati negatív sarok | mindkét tengely a min élen belül, egy lépésen belül áll meg |
| Északi perem, északkelet felé | y megáll, x tovább megy (tengelyenkénti csúszás) |
| 5 érvénytelen spawn-kérés (max él / kívül / NaN / blokkolt / nincs) | mind a spawn-régió közepén (100, 90) |
| Peremen túlnyúló spawn-körök | 80/80 mob a világon belül, egyik sincs a szélre tolva, z = orákulum |
| R3: fogyasztók bounds-a | terrain, load-field config, activity grid, load grid és régió mind [(−150,−90),(410,270)) |
| WorldValidator | OK |

### 5.3 `mapsplit` — részletek

A fixture a valódi loaderből jön:
- 2560×2048 m, origó (−1280, −1024);
- 4×3 chunk, az utolsó 32×64 cella;
- domborzat −22.9…31.5 m;
- 370 mob; 4 játékos, akik az egész létra alatt a vágási vonalakon oda-vissza
  futnak.

**Létra:** 1 → 4 → 16 kényszerített split. A 640×512 m-es levél további
splitjét a szabály **`min-size` diagnosztikával** elutasítja. Utána merge
16 → 4 → 1.

Minden lépés után (5 ellenőrzőpont: kiinduló, 4, 16, 4, 1 levél):
- 0 invariáns-sértés: egyedi authority, nincs Staging, a levelek hézag és
  átfedés nélkül fedik a világot, minden levél a saját régióján belül van, és
  egyik sem kerüli meg a load monitort;
- ugyanaz a net-id halmaz (374 entitás), nincs duplikáció és nincs vesztés;
- 6138 világon belüli lekérdezési pont mindegyike pontosan egy levélé,
  a vágási vonalak ±1 ulp-jén is;
- 8 külső pont (NaN is) egyik levélé sem;
- a `FindIndexForPosition` egyezik a levél-bejárással;
- a terrain-válaszok bitre azonosak a kiinduló állapottal;
- mob-z = terrain;
- `PackageLoadCount` változatlan (nincs újratöltés), a rezidens bájtok
  változatlanok (331672 B);
- WorldValidator OK.

További eredmények:
- 6 játékos-migráció történt a létra alatt (valódi mozgás közben ellenőrizve).
- Worker pool: 1 kezdeti zónával 15 worker, a hardveres szálszám − 1; nem a
  zónaszámhoz kötött.

**Becsekkolt tesztpálya** (1 km, alap 2x2 partíció):

| Beállítás | Eredmény |
|---|---|
| `min_zone=240` | 4 → 16 → 4; 250 m-nél `min-size` elutasítás |
| `min_zone=500` (alap) | a 500 m-es levél nem splitelhető, `min-size` diagnosztikával |

Az area-id-k ({7, 9000} és a tesztpálya 3 area-ja) és a szerver-zónák külön
névtérben élnek.

### 5.4 Indulási teszt (valódi `gameserver`, két idegen cwd, DB-vel) — 22/22

**Megmaradt MAP-1 esetek:** 1a–1c, 2a–2f, 3, 4a–4c, 5. Mind zöld.

**Új vagy módosult esetek:**

| Eset | Mit ellenőriz | Eredmény |
|---|---|---|
| 1d | tesztpálya alap 2x2 partícióval; a logban `SouthWest [(0,0),(500,500))` … `NorthEast` | PASS |
| 3 / 3b | szintetikus mód: 1x1 alap, illetve explicit 2x1 | PASS |
| 6a | negatív origó, nem-négyzetes, részleges chunk, 2x1: `West [(-150,-90),(130,270))`, `East` | PASS |
| 6b | alap 2x2 egy 560×360 m-es világon → 280×180 m-es levél < 240 m | exit 2 |
| 6c / 6c2 | értelmezhetetlen rács; 17x1 > 16 régió/tengely | exit 2 |
| 6d | height v2 int32 a logban | PASS |
| 6e | nincs spawn-régió | exit 3 |

Minden elutasítási esetben ellenőrzött: a DB, a hálózat és a world admission
nem indul el.

### 5.5 Teljes regresszió

RelWithDebInfo, a `worldbench_hardening.exe` (0efc4033) bináris ellen, a
párok egymás után futtatva. Szkript: `scratchpad/regress_map2.sh` (a MAP-1-es
szkript + `terrain`, `mapsplit`).

**Base vs új párok:** mind a 22 azonos PASS-számmal.
- field, loadfield, partitionscore selftest;
- routing ×3; lod, activity, loadfield, partitionscore, stability, splitmerge;
- ghost, aoi, replication; spread, hotspot, border;
- readiness smoke / dense 500p / spread 7000p (új: +1 `snapshot-quiescent`,
  mint a MAP-1-nél);
- scheduler.

**Csak-új módok, mind zöld:**
- tickrate, inputpath, netstress ×3, presence, asfdeterminism, workerpool;
- replv2, protocol, reclamation 100 / 1000, hygiene;
- mapaudit, snapshot (300 ciklus), worldpackage, terrain, mapsplit.

**`scheduler`:** ezúttal mindkét binárison 13/13. A MAP-1 óta ismert
falióra-alapú ingadozás nem jelent meg, de ettől még **nem tekintem
megoldottnak**.

Readiness tick (ms), base → új:

| Futás | p50 | p95 | p99 |
|---|---|---|---|
| dense 500p / 200k | 4.13 → 4.17 | 5.10 → 4.60 | 66.1 → 66.0 |
| spread 7000p / 200k | 2.20 → 2.01 | 7.87 → 7.99 | 9.55 → 9.10 |
| smoke 100p / 20k (3 ismételt pár) | base 1.01–2.05, új 1.05–1.24 | egyező | egyező |

A regressziós futásban látott smoke p50 eltérés (1.16 → 1.92) az ismétléseken
zajnak bizonyult.

### 5.6 Nem futtatott / nem lefedett

- ~~**Spawn-elutasítási ág**: teszt nem futtatja.~~ *A review után lefedve:*
  a `terrain` mód `spawn-refused-without-usable-spawn-region` esete a
  spawn-közép chunkját kiüríti, és az elutasítást végrehajtja (11. pont).
- `path-symlink-escape` (worldpackage): jogosultság híján kihagyva.
- FreeBSD build/futtatás: nem történt. Platform-specifikus kód nem került be;
  a shell-szkript POSIX.

## 6. Viselkedésváltozások (üzemeltetés)

- **Kis világokhoz új konfig kell.** Egy 256 m-es világhoz
  `partition_regions=1x1` szükséges, mert az alap 2x2 128 m-es leveleket
  adna → exit 2. Az acceptance-configba beírva.
- **Az 1 km-es tesztpálya az alap beállításokkal nem splitel.** Az alap
  `partition_min_zone_size_m=500` mellett a 500 m-es levelei nem
  splitelhetők; split-tesztekhez ≤ 250 kell.
- **Clamp megszűnt:** a mozgás a világ szélén egy lépéssel a perem előtt áll
  meg.
- **Spawn-sorrend** (*javítva a review után* — az eredeti mondat DB-first
  sorrendet sugallt, ami ellentmondott a 2.4 táblának): minden karakter a
  csomag első spawn-régiójának közepén lép be (dev buildben a debug felülírás
  előzi meg). A DB-ben tárolt pozíció nem számít. Ha a spawn-közép nem
  használható, a belépés elutasítva (nincs „szélső zónába ejtés").
- **Hibakódok:** 408 és 414 nem keletkezik többé; 415 és 416 új.
- **Új log-sorok:** a kényszerített split elutasítása WARN sort ír; az
  indulási log régiónként kiírja a partíciót.

## 7. Hatókörön túli apró változtatások — review-ra

- `.gitattributes`: `*.sh text eol=lf`. `core.autocrlf=true` mellett egy
  friss Windows-checkout CRLF-fel törné a bash-szkripteket.
- `WorldRuntime::Terrain()` csak olvasható accessor (a terrain inicializálás
  után immutábilis).
- Az indulási log régió-listája; a régiórács 16/tengely korlátja a `main`-ben.

## 8. Nyitott döntések a MAP-3 előtt

1. Magassági tartomány: a height v2 értelmezés megerősítése (3. pont, 6.).
2. Warp-lánc: warning marad vagy hiba legyen. Hibánál a becsekkolt
   `test_zone` tartalmát javítani kell, automatikus felülírás nélkül.
3. Collision, víz, navigáció terjedelme (a mobok még átmennek a blokkolt
   cellán); útvonal-menti ellenőrzés.
4. Streaming: rezidencia-egység, zónánkénti és processzenkénti budget,
   I/O-modell. A `NotResident` / `InvalidData` futásidejű jelentése már rögzített.
5. Tesztpálya partícionálhatósága: nagyobb teszt-térkép, vagy
   `partition_min_zone_size_m ≤ 250` a split-tesztekhez.
6. Kliens és v3 (konverter vagy kliens-v3); a mob-típus registry helye;
   a worldlogic formátum jövője (overlay area-k).
7. `scheduler` teszt: munkára normalizált kritérium.

## 9. Fájlok

**Új fájlok:**

| Fájl | Tartalom |
|---|---|
| `shared/map/include/map/ServerTerrain.h`, `shared/map/src/ServerTerrain.cpp` | szerver-terrain |
| `gameserver/apps/gameserver/src/world/partition/RegionDefinition.cpp` | kezdeti partíció |
| `gameserver/apps/gameserver/src/bench/MapRuntimeBench.cpp` | `terrain` és `mapsplit` mód |
| `docs/map2-report.md` | ez a riport |

**Főbb módosítások:**

| Terület | Fájlok |
|---|---|
| Formátum | `shared/map`: `map_manifest.capnp`, `WorldPackage.{h,cpp}`, `WorldPackageWriter.{h,cpp}`, `MapData.{h,cpp}` (`Rect::ContainsHalfOpen`, `AreaId`), `mapgen_test_zone.cpp` |
| Gameserver | `main.cpp`, `WorldRuntime.{h,cpp}`, `zone/ZoneManager.{h,cpp}`, `partition/RegionDefinition.h`, `partition/ZoneLoadMonitor.cpp`, `terrain/TerrainService.{h,cpp}`, `systems/MovementSystem.cpp`, `spawn/SpawnCoordinator.{h,cpp}`, `migration/MigrationCoordinator.cpp`, `replication/ProtocolEncoder.{h,cpp}`, `debug/WorldValidator.cpp`, `package/WorldPackageLoader.h` |
| Bench | `BenchWorld.h`, `HardeningBench.{h,cpp}`, `MapPackageBench.cpp`, `WorldBench.cpp`, `CMakeLists.txt` |
| Konfig, szkript | `config/gameserver.conf.example`, `scripts/map1_startup_acceptance.sh`, `.gitattributes` |
| Dokumentáció | `docs/map-data-format.md`, `docs/map-data-layer-requirements.md`, `gameserver/README.md` |

A becsekkolt `Client/assets/Maps/test_zone` érintetlen. Minden generált
fixture a temp / scratchpad könyvtárba került, nem a repóba.

## 10. Reprodukálás

A binárisok helye: `gameserver/build/windows-relwithdebinfo/apps/gameserver/RelWithDebInfo`.

```bash
./worldbench.exe --mode terrain
```

```bash
./worldbench.exe --mode mapsplit
```

```bash
./worldbench.exe --mode worldpackage
```

```bash
./worldbench.exe --mode mapaudit
```

```bash
bash gameserver/scripts/map1_startup_acceptance.sh <out_dir> <gameserver.exe> <worldbench.exe> <database.json> <mob_types.conf> Client/assets/Maps/test_zone
```

## 11. Review-döntés és utómunka (2026-09-25)

**Review-döntés:** a MAP-2 elfogadva a riportban bizonyított, **resident
terrainre vonatkozó terjedelemben**. A MAP-3 előtt kért négy pont kész.

1. **A spawn sorrendjének ellentmondása:**
   - Egyetlen sorrend van: debug (dev build) → a csomag első spawn-régiójának
     közepe → elutasítás.
   - A DB-pozíció nem jelölt: a gameserver nem írja vissza, és nincs
     világ-azonossága.
   - Tesztek: `terrain` → `spawn-invalid-debug-falls-back-to-spawn-region`
     (érvényes DB-pozícióval is a spawn-régió), `spawn-refused-without-usable-spawn-region`
     (a korábban nem futtatott elutasítási ág), `mob-spawn-skips-missing-chunk`.
2. **Nulla area és kötelező spawn:**
   - `areaId = 0` = világszintű spawn-régió (csak a világon belül kell
     lennie); `areaId ≥ 1` → létező area, azon belül.
   - Area nélküli csomag egy `areaId = 0` spawn-régióval érvényes.
   - Tesztek: `worldpackage` → `worldlogic-zero-areas-world-spawn-accepted`,
     `worldlogic-world-spawn-outside-world`.
3. **Összesített bootstrap-korlát:**
   - A tengelyenkénti korlátok együtt ~1M kezdeti zónát engedtek.
   - Mért költség: ~1.3 MB/zóna; 4096 zónánál 77 ms üresjárati
     supervisor-pass.
   - Új korlát: legfeljebb **1024** kezdeti zóna, mindkét módban kikényszerítve.
     1024 zónánál 1.3 GB, 1.75 s építés, 1.33 ms supervisor-pass.
   - A szintetikus világ kiterjedése is a ±131072 m-es tartományba került.
   - Új `Bootstrap resources:` logsor.
   - Tesztek: új `worldbench --mode bootstrap` (4/4), indulási teszt 6c3/6c4.
   - Nyitott: futásidejű zónabudget (a split-úton nincs összesített korlát).
4. **Jóváhagyott height- és residency-szerződés:** a
   `docs/map-data-format.md` 10. pontjába került (magasság, resident
   terjedelem, bootstrap-erőforrások, MAP-3 kötelezettségek).

**Utómunka utáni eredmények:**

| Teszt | Eredmény |
|---|---|
| `terrain` | 20/20 |
| `worldpackage` | 119 PASS / 0 FAIL / 1 SKIPPED |
| `bootstrap` | 4/4 |
| indulási teszt | 24/24 |

Részletek: `docs/map-data-layer-requirements.md`, „MAP-2 review utómunka".
