# Adaptive Simulation Fabric — Continuous Load Field

> Konszolidált feladatspecifikáció. Ez a dokumentum a fázis **teljes megrendelése**:
> a §0–§39 szakaszok a megrendelői kérés, a bennük rögzített invariánsok kötelezőek.
> A **§1 audit eredménye**, a **chunk-bontás** és a **státusz** a specifikációból
> levezetett munkaterv — ezek külön jelölve.

| | |
|---|---|
| Repository | `github.com/ixtreeme/IxtreemeWorld` |
| Branch | `With_Auriga` |
| Referencia commit (az audit alapja) | `087e8418` — *Adaptive Simulation Fabric — SpatialActivityField* |
| Érintett terület | `gameserver/apps/gameserver/src/world/` |
| Dokumentum kelte | 2026-09-09 |

---

## 0. Cél és alapmandátum

Ez a fázis az átmenet **reaktív zone-menedzsmentből** egy **Adaptive Simulation Fabric**-be.

A motornak el kell kezdenie felépíteni a **számítási költség térbeli megértését**,
**függetlenül a jelenlegi Zone topológiától**.

Nem a játékosszámra optimalizálunk. A motornak azt kell értenie:

- **HOL** keletkezik a munka,
- **MI** okozza azt a munkát,
- **mennyibe kerülnek** a partition-határok.

Az így kapott mezőnek újrahasznosítható inputtá kell válnia ehhez:

- dynamic split/merge
- jövőbeli elastic boundary movement
- predictive pre-splitting
- process/node placement
- simulation budgeting

### A fejlődési lánc

```
Spatial Activity Field
        ↓
Continuous Load Field
        ↓
spatial compute-cost understanding
        ↓
Adaptive Partition Score
        ↓
split/merge decisions
```

### ⚠ A fázis legfontosabb szabálya

A load field **NE legyen Zone-alapú**.

**Helyes:**

```
WorldPosition  →  LoadCell  →  cost channels
```

**Nem:**

```
ZoneId  →  load
```

**A Zone csak consumer.** A compute-topológia nem azonos az activity-topológiával;
a mező world space-ben indexelt derived adat, amit a Zone *olvas*.

### Amivel a rendszer MÁR rendelkezik (kiindulási állapot)

dynamic Region → Zone partitioning · transactional split/merge · ZoneLoadMetrics ·
Simulation LOD · Spatial Activity Field · PlayerInfluence · SpatialGrid · AOI ·
Visibility · Replication · Migration · Ghost/Border · Worker scheduler ·
WorldDirectory · distributed-ready routing · benchmark + validator

---

## 1. Current HEAD audit (§1) — **elvégezve**

> A §1 előírása: *„Mielőtt módosítasz"* — HEAD-ellenőrzés, build, a 7 bench mód
> futtatása, 17 alrendszer auditja, és egy lista arról, **milyen workot tudunk
> jelenleg mérni, és milyen work nincs még instrumentálva**.
> Az alábbi alfejezetek ennek az eredményei.

### 1.1 Kiindulási állapot (verifikált)

- HEAD pontosan `087e8418`, a fa tiszta volt a módosítás megkezdésekor.
- Build: `gameserver` és `worldbench` egyaránt hibátlanul fordul
  (a `worldbench` az `ENABLE_WORLDBENCH` kapcsoló mögött, alapértelmezés **OFF**).
- Mind a 7 kért futtatás zöld. (A §1 listájában a `selftest` valójában a
  `--routing-selftest` kapcsoló, a többi `--mode` érték; a `dense` mód létezik,
  de a §1 nem kérte.)

| futtatás | eredmény |
|---|---|
| `--routing-selftest` | 5/5 PASS (`unknown-destination`, `draining-destination`, `stale-migration-drop`, `duplicate-migration-id`, `transfer-roundtrip`) |
| `--mode lod` | PASS=18 FAIL=0 |
| `--mode activity` | PASS=22 FAIL=0 |
| `--mode splitmerge` | PASS=20 FAIL=0 |
| `--mode border` | `failures=0` |
| `--mode hotspot` | `failures=0` |
| `--mode spread` | `failures=0` |

### 1.2 Amit MA mérni tudunk

| Alrendszer | Mérés | Attribúció |
|---|---|---|
| Zone tick | avg µs, stage µs bontás, 256 mintás ring buffer (p50/p95/p99 létezik) | **ZoneId** |
| Rezidensek | player_count, mob_count, ghost szám | **ZoneId** |
| LOD | tier-eloszlás, promotion/demotion, wake, eval µs, ai/move update szám | **ZoneId** |
| AOI | query szám, elfogadott látható entitások relevance-tier bontásban | **ZoneId** |
| Replication | elküldött transform **rekordok** száma | **ZoneId** |
| Migration | pending, committed, duplicate, quarantined | **ZoneId** |
| Worker pool | task szám, busy µs, supervisor avg | **process** |
| Activity field | source szám, nem üres cellák, rebuild µs, epoch | **cella (world space)** |
| Routing | local / remote / unavailable / draining / miss | **process** |

### 1.3 Instrumentation gap — amit NEM mérünk

Prioritási sorrendben:

1. **Nincs semmilyen térbeli attribúció.** Minden számláló ZoneId- vagy
   process-hatókörű, *pedig a pozíció minden hívási ponton rendelkezésre áll*.
   Ez a fázis fő hiánya.
2. **Replication byte-ok soha nincsenek mérve.** Az `EncodeTransformFrame`
   eredménye egyenesen a `send()`-be megy
   (`replication/ReplicationSystem.cpp:63`) — a méret sehol nem kerül számlálóba.
   Nincs spawn/despawn rekordszám és nincs recipient count sem.
3. **A `CombatSystem`-nek NULLA számlálója van.** Sem attack, sem damage,
   sem aktív résztvevő nincs mérve — a CombatHeat csatornának ma nincs forrása.
4. **Az AOI csak az elfogadott jelölteket számolja.** Nincs nyers candidate
   count és nincs cap-levágás („mennyi entitást dobtunk el a 100-as sapka
   miatt") — így nem lehet megkülönböztetni a *sok entitást* a *sok tényleges
   interakciótól* (§10).
5. **AI / Movement:** van darabszám, de nincs költség-időzítés.
6. **Migration:** nincs határátlépési **pozíció**-attribúció, csak darabszám.
7. **p95/p99 nem jut el a control plane-ig.** A ring buffer létezik
   (`ZoneDiagnostics`), de kizárólag a bench olvassa; a `ZoneLoadMonitor`
   avg-ból számol.
8. **Hiányzik teljesen:** csatorna-szétválasztás, normalizáció, smoothing,
   decay, boundary cost, hotspot fogalom, predicted load.

### 1.4 Mérési bizonyíték, ami a fázist indokolja

Két mérés (`worldbench`, 100 player / 1000 mob, 30 s):

**(a) Az avg-alapú score elrejti a budget-sértést → ez indokolja a §28-at.**

`--mode hotspot` (mindenki egy zónába tömörülve):

```
zone ticks: avg=12.531ms p50=11.093ms p95=22.368ms p99=33.344ms max=35.558ms
process:    zones=3 active=1 sleeping=2 avg_tick=15.325ms
```

A jelenlegi pontozás (`partition/ZoneLoadMonitor.cpp`):

```cpp
tick_frac     = avg_tick_us / 1000 / tick_budget_ms;         // 12.531 / 16   = 0.783
resident_frac = (players + mobs) / resident_budget;          // 1109  / 2000  = 0.554
load_score    = clamp(max(tick_frac, resident_frac), 0, 2);  //               = 0.78
```

`split_load_threshold = 0.9` → **nincs split**, miközben a
**p99 = 33.3 ms = a 16 ms-os budget 2,08-szorosa**. A zóna ténylegesen
túlterhelt, a metrika szerint egészséges.

**(b) Egy skalár nem tudja megmondani az OKOT → ez indokolja a §4–§5-öt.**

Azonos populáció (100 player / 1000 mob), azonos tickszám (602), csak a térbeli
eloszlás más:

| mód | repl (transform rekord) | avg tick | p99 | activity cellák |
|---|---|---|---|---|
| `hotspot` | **30 300** | 15.325 ms | 33.344 ms | 1/4 |
| `spread` | **15 925** | 3.939 ms | 13.358 ms | 4/4 |

Ugyanannyi entitás **1,9-szeres replication-munkát** okoz pusztán a térbeli
koncentráció miatt. Egyetlen `ZoneId → load` skalárban ez az információ elvész —
a planner nem tudja megkülönböztetni az AI-nehéz és a replication-nehéz zónát (§5).

---

## 2. Kötelező invariánsok

Ezek a specifikáció explicit tiltásai és megkötései. Egyik sem sérthető:

| # | Invariáns |
|---|---|
| §0 | A load field **world-position indexelt**, nem ZoneId-indexelt. A Zone csak consumer. |
| §2.2 | *„Ne változtasd meg fölöslegesen a jelenlegi production koordinátákat."* |
| §3 | *„Ne építs enterprise hierarchy-t."* A field **derived data**, nem gameplay authority, bármikor újraépíthető. |
| §4 | *„Ne mosd őket azonnal egyetlen score-ba."* A nyers/normalizált csatornák külön tárolandók. |
| §15 | Ne adj össze nyersen µs-ot, byte-ot, eventet és entityt. Normalizálj. |
| §17 | *„Ne töröld a jelenlegi safety gate-eket."* sustained threshold · cooldown · minimum size · maximum depth · transactional split — mind marad. |
| §21 | A quadtree split **működő fallback** marad; a splitmerge regressziót nem szabad eltörni. |
| §25 | Split után (1 Zone → 4 Zone) a Load Field értékei **NE változzanak** pusztán azért, mert a topológia változott. |
| §26 | A `PartitionCoordinator` a Load Field **snapshotját olvassa. Nem írja.** |
| §30 | Ne logolj minden tickben — csak topology decisionnél. |
| §34 | Ne használj `unordered_map`-et automatikusan minden cellára; dense rectangular boundsnál contiguous storage. |
| §35 | *„Ne készíts interface hierarchy-t emiatt."* A storage cserélhető legyen a consumer API változtatása nélkül — de nem absztrakciós fával. |
| §37 | *„NE implementáld még player trajectory predictiont."* Csak a Current/Predicted adatmodell-szétválasztás készül el. |
| §38 | *„Most NE implementáld a compute credit schedulert."* Csak a seam készül. |
| — | (állandó megkötés korábbi fázisból) Nincs platform-specifikus kód: a motor crossplatform. |

---

## 3. A megrendelés szakaszonként (§2–§39)

### §2 — Az előző commit két apró generalizálása

Mielőtt a Load Fieldre építünk, két dolgot ki kell javítani.

#### §2.1 Fast tier query vs exact influence

A jelenlegi `PlayerInfluence` query Full-találatnál **early-outolhat**. Ez tier
classificationre helyes, de a `nearest_sq` / provenance **nem feltétlenül egzakt**.

Szét kell választani:

- `QueryPlayerTierFast(...)`
- `QueryPlayerInfluenceExact(...)`

vagy ezekkel ekvivalens tiszta API-ra.
**LOD hot path: Fast. Adaptive planner / diagnostics: Exact, ha szükséges.**
A semanticsot dokumentálni kell.

#### §2.2 Activity grid world origin

A `SpatialActivityField` ne feltételezze örökre, hogy `world origin = (0,0)`.

Támogasson `origin_x` / `origin_y` / `extent_x` / `extent_y` értékeket, vagy
explicit `WorldBounds` típust — hogy `0 … 100 km` és `-50 km … +50 km` világgal
is működjön.

### §3 — Continuous Load Field

Új általános modul, például:

```
world/activity/
    LoadFieldTypes.h
    ContinuousLoadField.h
    ContinuousLoadField.cpp
    LoadFieldPublisher.h/.cpp
```

vagy a jelenlegi activity struktúrához jobban illő elrendezés.
**Ne épüljön enterprise hierarchy.** A `ContinuousLoadField` **derived data**,
nem gameplay authority, bármikor újraépíthető.

### §4 — Load channels

Első verzióban legalább ezek a **független** csatornák:

`SimulationCost` · `ReplicationPressure` · `AOIWork` · `CombatHeat` · `MigrationPressure`

Opcionálisan diagnosztikai/támogató csatornaként: `EntityDensity`, `PlayerDensity`.

**FONTOS: ne mossuk őket azonnal egyetlen score-ba.** A nyers/normalizált
csatornák külön tárolandók:

```cpp
struct LoadSample
{
    float simulation;
    float replication;
    float aoi;
    float combat;
    float migration;
};
```

### §5 — Miért külön channel?

Két terület lehet azonos CPU-load mellett teljesen más:

- **Zone A:** sok AI
- **Zone B:** kevés AI, de brutális replication

Ha csak `load = 0.8` értéket tárolunk, **elveszítjük az okot**. A plannernek
tudnia kell, MI okozza a terhelést, mert a döntés más lesz:

| ok | válasz |
|---|---|
| AI-heavy | Simulation LOD / worker budget |
| Replication-heavy | partition / boundary / frequency strategy |
| Migration-heavy | boundary instability |

### §6 — Load cell

A field world-space cellákból álljon:

```cpp
struct LoadCell
{
    LoadChannels current;
    LoadChannels smoothed;
};
```

A cellaméret **konfigurálható** legyen, és **ne feltétlenül egyezzen** a
SpatialGrid- és az ActivityField-cellamérettel. Első default 250–500 m körüli
lehet, de benchmark alapján kell választani.

### §7 — Event/work attribution

A workot **térbeli pozícióhoz** kell kötni:

| work | pozíció |
|---|---|
| AI update | mob position |
| Movement | entity position |
| Combat | combat location |
| AOI query | viewer position |
| Replication | viewer position **és/vagy** source position |
| Migration | boundary crossing position |

**Ne legyen minden work csak ZoneId-hoz kötve.**

### §8 — Simulation cost

Instrumentálni kell legalább: AI work · movement work · combat work · LOD evaluation.

Ha a pontos µs-attribúció entitásonként túl drága, **ne mérjünk clockot minden
entity körül**. Helyette: **operation counters + sampled timing + system-level
measured cost**, és ezekből becsült spatial cost. A mérési overhead maradjon alacsony.

### §9 — Replication pressure

**Kritikus** — a benchmarkok szerint a replication a jelenlegi fő bottleneck.

Mérendő, **spatial attribúcióval**: visible relations · snapshots built ·
transform records · spawn/despawn records · recipient count · becsült/tényleges
byte-ok, ha elérhető.

Cél: megmondani, **hol** drága a replication.

### §10 — AOI work

Mérendő cellánként vagy spatial attribúcióval: AOI query count · candidate count ·
accepted visible count.

Ez különbözteti meg a **sok entitást** a **sok tényleges interakciótól**.

### §11 — Combat heat

A CombatHeat **ne legyen Metin-specifikus** — általános activity: attack events,
damage events, active combat participants. Idővel **decay-eljen**:

```
CombatHeat(t+1) = CombatHeat(t) * decay + newCombatWork
```

Ne maradjon örökké hot egy régi csata helye.

### §12 — Migration pressure

Térben mérendő: zone crossing count · migration requests · committed migrations ·
stale/retry migration.

Ez később kulcsfontosságú a boundary drifthez: ha egy határon **500 migration/sec**
van, az rossz boundary lehet.

### §13 — Temporal smoothing

Ne csak instantaneous load legyen. Kell: `current` + **short-term EMA** +
**longer-term EMA**, vagy egyszerűbb két-időskálás smoothing:

- fast: ~2–5 sec
- slow: ~15–30 sec

A pontos érték config. Ez különbözteti meg a **spike**-ot a **sustained hotspottól**.

### §14 — Load decay

Ha egy területen megszűnik az activity, a load **fokozatosan essen**, ne ragadjon
hot állapotban. Combat/migration különösen decay-based lehet.

### §15 — Normalization

A csatornák más mértékegységűek — **ne adjuk össze nyersen** a µs / bytes /
events / entities értékeket. Kell 0..1 (vagy más stabil tartomány) normalizáció,
konfigurálható reference budgetekkel:

`simulation_budget` · `replication_budget` · `aoi_budget` · `combat_budget` · `migration_budget`

### §16 — Adaptive partition score

A split döntés ne csak avg tick + resident count alapú legyen. Kell egy
`PartitionLoadScore`, amely a **Zone bounds alatti Load Field cellákat aggregálja**:

```
score = Ws*Simulation + Wr*Replication + Wa*AOI + Wc*Combat + Wm*Migration
```

A súlyok configból jönnek. **DE: a raw channel metrics továbbra is elérhetők maradnak.**

### §17 — Score ≠ automatikus split mindig

A score **csak input**. A meglévő invariánsok maradnak: sustained threshold ·
cooldown · minimum size · maximum depth · transactional split.
**Ne töröld a jelenlegi safety gate-eket.**

### §18 — Replication-aware split

Ez az első fontos új viselkedés: ha egy Zone replication-heavy, a planner ezt
vegye figyelembe a split döntésnél.

**DE ne feltételezzük, hogy a split mindig javít a replicationön** — ha a
játékostömeg közepén húzunk boundaryt, a cross-zone cost **nőhet**.
Ezért kell boundary cost.

### §19 — Boundary cost field

Derived számítás, amely megbecsüli egy candidate partition boundary költségét.
Minimum: cross-boundary player influence · cross-boundary AOI relations ·
migration pressure a határ közelében · a határt keresztező combat heat.

```
BoundaryCost = visibility_crossings + migration_crossings + combat_crossings
```

Nem kell tökéletesnek lennie, de legyen **spatially grounded**.

### §20 — Partition objective

A split planner ne a *„felezd a loadot"* célt optimalizálja, hanem:

```
Objective = LoadImbalance + BoundaryCost + MigrationCost + TopologyComplexityPenalty
```

Cél: **kiegyensúlyozott compute MINIMÁLIS cross-zone interakcióval.**

### §21 — Quadtree compatibility

A jelenlegi quadtree split maradjon működő fallback. Az első Adaptive verzióban a
négy child geometriailag lehet ugyanaz, de a planner **már mérje, melyik candidate
split mennyire jó**. A splitmerge regressziót nem szabad eltörni.

### §22 — Adaptive split candidate evaluation

Ha egyszerűen megoldható: több candidate splitet kell értékelni (vertical /
horizontal / quadtree), és a legjobb objective score-t választani.

Ha ez túl nagy scope: első körben csak a quadtree childok load-distributionját
mérjük — **de az API ne legyen quadtree-only örökre**.

### §23 — Hotspot detection

A Load Fieldből legyen `Hotspot` fogalom:

```cpp
struct Hotspot
{
    WorldPosition center;
    float         radius;
    LoadChannels  load;
};
```

Nem kell komplex clustering — connected hot cells / flood fill elég.
Cél, hogy a planner tudja: *„nem az egész Zone hot, csak ez a 700×900 m terület"*.

### §24 — Hotspot-aware split

A planner **kerülje a hotspot kettévágását**, ha az cross-boundary AOI/combat
költséget okozna. Preferálja, hogy a hotspot egy child/partition **belsejében**
maradjon, ha közben a compute balance elfogadható.

### §25 — Load Field és Zone topology függetlenség

**Nagyon fontos:** split (1 Zone → 4 Zone) után a Load Field értékei **NE
változzanak** csak azért, mert a topológia megváltozott. A field world-space
derived workload, **nem Zone-owned truth**.

### §26 — Split/merge integráció

A `PartitionCoordinator` Load Field **snapshotot olvas — nem ír**. A split/merge
tranzakció alatt a field tovább él; a topology commit után a következő control
cycle már az új leaf topológiára aggregál.

### §27 — Merge score

Merge-nél is Load Fieldet kell használni. Sibling leaf zone-ok akkor merge
candidate-ek, ha a **combined load alacsony**, a **boundary removal előnyös**, és
a **sustained low condition** teljesül.

Implementálandó a korábbi TODO: **merge sustained-low timer** (pl. 60 sec default,
konfigurálható).

### §28 — P95/P99 load signal

Ne csak avg zone tick legyen. Kell rolling **p95 / p99**, vagy megfelelő
histogram/quantile közelítés. A split trigger szempontjából a **p99 a fontosabb**.
**Ne épüljön nehéz metrics framework.**

### §29 — Control loop

Az adaptive planner továbbra se fusson 20 Hz-en:

- 1 Hz Load Field aggregation
- 0,2–1 Hz partition decision

Konfigurálható.

### §30 — Automatikus döntési log

Minden split/merge decisionhöz strukturált reason:

```
SPLIT Zone 42
reason:
  p99_tick=28.1ms
  simulation=0.62
  replication=0.91
  aoi=0.78
  combat=0.21
  migration=0.08
  objective_before=0.88
  objective_after=0.51
```

Ez diagnosztikához nagyon fontos. **Ne logolj minden tickben — csak topology
decisionnél.**

### §31 — Why-not diagnostics

Opcionális debugban hasznos:

```
Zone overloaded BUT split rejected because:
  - min size
  - max depth
  - cooldown
  - candidate boundary cost too high
  - insufficient predicted improvement
```

Így később nem vakon tuningolunk.

### §32 — Minimum improvement gate

Ne splitteljünk csak azért, mert threshold fölött vagyunk. A candidate splitnek
legalább `MinExpectedImprovement` értéket kell hoznia (pl. **10–15%**,
konfigurálható). Ha a split várhatóan rosszabb, **ne splitteljünk**.

### §33 — Topology complexity penalty

Minden új Zone-nak van költsége: scheduler · ghost · directory · migration ·
memory · control-plane. Ezért legyen kis `TopologyComplexityPenalty` a score-ban —
ez akadályozza meg a túlzott fragmentációt.

### §34 — Field storage performance

100×100 km world esetén:

| cellaméret | rács | cellák |
|---|---|---|
| 500 m | 200×200 | 40 000 |
| 250 m | 400×400 | 160 000 |

Mindkettő kezelhető. Dense rectangular world boundsnál **contiguous storage**
preferált. **Ne használjunk `unordered_map`-et automatikusan minden cellára.**
Benchmarkolni kell.

### §35 — Sparse vs dense

Vizsgálandó a **dense vector grid** vs **sparse active cells** tradeoff. 100×100 km
fix boundsnál a dense grid valószínűleg jó. A storage implementáció legyen később
cserélhető anélkül, hogy a consumer API változna — **de ne készüljön interface
hierarchy emiatt**.

### §36 — Multi-resolution L1 előkészítés

Most már készüljön egy egyszerű coarse aggregation seam:

- L0 = 500 m
- L1 = 2 km (egy L1 cella = 16 L0 cella aggregátuma)

Nem kell teljes pyramid, de a region/process placement később az L1-et tudja
használni. Ha egyszerű, az L1 aggregation implementálandó.

### §37 — Current vs predicted load

Az adatmodellben **már most külön** legyen `CurrentLoad` és `PredictedLoad`.
A Predicted most lehet üres / azonos a currenttel.
**NE implementáljunk még player trajectory predictiont** — de később ne kelljen
storage redesign.

### §38 — Compute budget future seam

Később Budgeted Simulation Scheduler jön, ezért a `LoadChannels` legyen
használható **system budget allocator inputként**.
**Most NE implementáljuk a compute credit schedulert.**

### §39 — Exact vs fast activity query (teszt)

Az előző commitból javított Fast/Exact semanticsra **tesztet kell írni**:

- LOD: Fast tier query
- Planner/debug: *(a megrendelés szövege itt megszakadt — lásd 5. pont)*

---

## 4. Chunk-bontás és státusz

> *Levezetett munkaterv, nem a megrendelés része.*
> A bontás a szakaszok függőségeit követi: mérni csak azután érdemes, hogy van
> hova írni (B a C előtt), és dönteni csak azután, hogy van mit mérni (C a D előtt).

| Chunk | Szakaszok | Tartalom | Státusz |
|---|---|---|---|
| **A** | §2.1, §2.2, §39 | Fast/Exact query szétválasztás + explicit world origin + a hozzájuk tartozó tesztek | ✅ **KÉSZ** |
| **B** | §3–6, §13–15, §34–37 | `ContinuousLoadField` váz: `LoadChannels`, dense cella-tár, két-időskálás EMA, decay, normalizáció, L0/L1 seam, Current/Predicted szétválasztás | ⬜ következő |
| **C** | §7–12 | Térbeli work-attribúció a hívási pontokon: AI, movement, combat, AOI (candidate/cap), replication (byte + spawn/despawn + recipient), migration | ⬜ |
| **D** | §16–17, §28, §30–33 | `PartitionLoadScore`, p95/p99 a control plane-en, strukturált döntési log, why-not diagnostics, `MinExpectedImprovement`, `TopologyComplexityPenalty` | ⬜ |
| **E** | §18–24, §27 | BoundaryCost field, Partition Objective, hotspot detection (flood fill), hotspot-aware split, multi-candidate split, merge sustained-low timer | ⬜ |
| **F** | §1, §21, §29 | `worldbench --mode loadfield`, validator-bővítés, regresszió a meglévő 7 módra, control-loop frekvenciák konfigurálhatóvá tétele | ⬜ |

### Chunk A — elvégzett munka

**§2.1 Fast/Exact szétválasztás**

- A kétértelmű `QueryPlayerInfluence` megszűnt; helyette `QueryPlayerTierFast()`
  és `QueryPlayerInfluenceExact()` — így minden hívónak **választania kell**.
- Egy közös bejárás, `template <bool EarlyOutOnFull>` az anonim namespace-ben →
  nincs futásidejű elágazás a hot pathon és nincs kódduplikáció.
- A Fast változat **bit-azonos** a korábbi viselkedéssel (ugyanaz a részleges
  early-out), mert a `LodSystem` ebből tölti a `cross_zone_*` diagnosztikai
  számlálókat, amelyekre teszt asszertál.
- Dokumentált szerződés: a `tier` és a `has_influence` **mindkét változatban
  egzakt** (az early-out csak szigorúan a Full-buborékon belül él, ami a Low-n
  belül van); a `nearest_sq` és a `cross_zone` a Fastban best-effort.
- Hívók: LOD hot path → Fast (`systems/LodSystem.cpp`), validator/audit → Exact
  (`debug/WorldValidator.cpp`).

**§2.2 World origin**

- Új `WorldBounds{min_x, min_y, max_x, max_y}` típus + `FromExtent()` helper.
- A grid `dim` → `dim_x` / `dim_y` (nem négyzetes világ is), az indexelés
  **kivonja az origót**.
- A `WorldRuntime` `FromExtent(terrain_.WorldExtentMeters())`-t ad át → a
  **jelenlegi production koordináták bitre azonosak** (§2.2 megkötése teljesül).

**§39 tesztek** — `worldbench --field-selftest` (világ és szálak indítása nélkül):

| teszt | mit bizonyít |
|---|---|
| `field-negative-origin` | −50…+50 km world: `-40000 → cella 20` (nem 0), rács 200×200 |
| `field-non-square-bounds` | 4000×1000 m world → 8×2 cella |
| `field-fast-vs-exact` | azonos `tier`, de `exact.nearest_sq=100` vs `fast=1600`, és a provenance eltér (fast: cross-zone, exact: nem) |
| `field-fast-exact-sweep` | az invariáns 1600 mintaponton tart: tier és has_influence mindig egyezik, `exact.nearest_sq <= fast.nearest_sq` |

**Regresszió:** `lod 18/0` · `activity 22/0` · `splitmerge 20/0`, és a
viselkedésérzékeny értékek bit-azonosak (`cross-counters observed: 3`, LOD
tier-pillanatképek, prom/dem döntések változatlanok).

---

## 5. Nyitott pontok

1. **A megrendelés §39-nél megszakadt** („Planner/debug:" után). A dokumentum a
   rendelkezésre álló szöveget teljes egészében tartalmazza; ha van folytatás
   (§39 vége, §40+), az ide kerül, és befolyásolhatja a B/C tervezést.

2. **LoadField cellaméret a teszt-térképen (§6/§34 input).** A jelenlegi
   teszt-világ **1000×1000 m** (`docs/test-zone-map.md`), 3 seeded zónával
   (500×500 m és 500×1000 m). Az 500 m-es activity cella emiatt összesen **4
   cellát** ad (`cells=1/4` a hotspot módban), egy zóna pedig pontosan **egy
   cella** — ezen a felbontáson a §23 hotspot-detektálás (*„nem az egész Zone
   hot, csak ez a 700×900 m terület"*) elvileg sem működhet.
   Ezért a LoadField cellaméret **configból jön**, és a benchmark-világ saját,
   kisebb értéket kap (≈50–125 m), miközben a 100 km-es production world a
   §34-ben javasolt 250–500 m-t használja.

3. **A `transfer-roundtrip` selftest időzítés-érzékeny és intermittensen bukik
   a HEAD-en.** Mérve: chunk A binárissal 5-ből 2 FAIL, **érintetlen baseline
   binárissal 5-ből 1 FAIL** — tehát nem a chunk A okozza. Érdemes stabilizálni,
   mielőtt a későbbi fázisokban valódi regressziót takarna el.

4. **Diagnosztikai provenance (chunk D bemenet).** A `cross_zone_full/reduced/low`
   számlálók ma a **Fast** (best-effort) provenance-ból töltődnek, mert az
   entitásonkénti LOD hurokban keletkeznek. Ez teljesítmény-szempontból helyes;
   ha a plannernek egzakt cross-zone attribúció kell, azt a ~1 Hz-es planner
   úton kell számolni az Exact query-vel — **nem** az entitásonkénti hurokban.
