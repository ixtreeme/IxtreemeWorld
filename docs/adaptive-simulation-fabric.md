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
| **B** | §3–6, §13–15, §34–37 | `ContinuousLoadField` váz: `LoadChannels`, dense cella-tár, két-időskálás EMA, decay, normalizáció, L0/L1 seam, Current/Predicted szétválasztás | ✅ **KÉSZ** (lásd §6) |
| **C** | §7–12 | Térbeli work-attribúció a hívási pontokon: AI, movement, combat, AOI (candidate/cap), replication (byte + spawn/despawn + recipient), migration | ✅ **KÉSZ** (lásd §6.2) |
| **D** | §16–17, §28, §30–33 | `PartitionLoadScore`, p95/p99 a control plane-en, strukturált döntési log, why-not diagnostics, `MinExpectedImprovement`, `TopologyComplexityPenalty` | ✅ **KÉSZ** (lásd §7) |
| **E** | §18–24, §27 | BoundaryCost field, Partition Objective, hotspot detection (flood fill), hotspot-aware split, multi-candidate split, merge sustained-low timer | ✅ **KÉSZ** (split §7 + merge/stability §8) |
| **F** | §1, §21, §29 | `worldbench --mode loadfield`, validator-bővítés, regresszió a meglévő 7 módra, control-loop frekvenciák konfigurálhatóvá tétele | ⚠ **RÉSZLEGES**: `--mode partitionscore`/`--mode stability`/`--mode readiness` + selftestek kész, regresszió zöld; a control-loop frekvencia config még nyitott |
| **G (Phase 4)** | §0–§28 | Integrált readiness benchmark 100 km / 500 player / 200k mob, stage-instrumentáció, A/B-k, bottleneck audit | ✅ **KÉSZ** (lásd §9) |
| **H (Phase 5A)** | §9.7/1 | Inkrementális/dirty ghost karbantartás: dirty entity tracking, delta-publish, KEEP/ADD/REMOVE reconcile, egzakt equivalence validator + repair seam, `--mode ghost` + `--ghost-shadow` | ✅ **KÉSZ** (lásd §10) |

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

2. **LoadField cellaméret a teszt-térképen (§6/§34 input). — MEGOLDVA (§6.3).**
   A teszt-világ **1000×1000 m**, 3 seeded zónával; 500 m-en ez csak 4 cella,
   ezért a felbontás **config**: production default 500 m (3.4 MB, ~1.5 ms/1 Hz,
   benchmarkolt), a bench-világ 100 m-t használ, és a 250 m is elérhető, ha a
   későbbi boundary-tervezés finomabb felbontást igényel.

3. **A `transfer-roundtrip` selftest időzítés-érzékeny és intermittensen bukik
   a HEAD-en.** Mérve: chunk A binárissal 5-ből 2 FAIL, **érintetlen baseline
   binárissal 5-ből 1 FAIL** — tehát nem a chunk A okozza. Érdemes stabilizálni,
   mielőtt a későbbi fázisokban valódi regressziót takarna el.

4. **Diagnosztikai provenance (chunk D bemenet).** A `cross_zone_full/reduced/low`
   számlálók ma a **Fast** (best-effort) provenance-ból töltődnek, mert az
   entitásonkénti LOD hurokban keletkeznek. Ez teljesítmény-szempontból helyes;
   ha a plannernek egzakt cross-zone attribúció kell, azt a ~1 Hz-es planner
   úton kell számolni az Exact query-vel — **nem** az entitásonkénti hurokban.

---

## 6. Chunk B/C — Continuous Multi-Channel Load Field

> Implementációs jegyzet. Az audit a `e6c9da75` HEAD tényleges source-án
> készült (nem commit üzenetekből/dokumentumokból): a hívási pontok és a
> meglévő mérőszámok forrásszinten lettek ellenőrizve.

### 6.1 Audit — mi mérhető, hol, milyen áron

**Ténylegesen működő, újrahasznosított mérési infrastruktúra**

| Terület | Forrás | Mérés | Attribúció |
|---|---|---|---|
| Zone tick | `ZoneDiagnostics` | tick µs + 256-os ring (p50/p95/p99 a benchben), stage µs (gameplay/ghost/replication/lod eval) | ZoneId |
| LOD | `ZoneDiagnostics` / `LodSystem` | ai/move update **darabszám**, eval µs, tier gauge-ok | ZoneId |
| AOI | `AoiSystem` / `ZoneDiagnostics` | query szám, elfogadott candidate-ek tier bontásban | ZoneId |
| Replication | `ZoneDiagnostics` | elküldött transform **rekordok** száma | ZoneId |
| Migration | `ZoneDiagnostics` + `MigrationMetrics` | committed/stale/dup/retry/fail | ZoneId / process |
| Worker/supervisor | `ZoneWorkerPool` / `WorldRuntime` | task szám, busy µs, supervisor µs | process |
| Activity field | `SpatialActivityField` | source szám, nem üres cellák, rebuild µs, epoch | **world-space cella** |

**Amit eddig NEM mértünk (és most bekötöttük vagy explicit seam lett)**

1. **Replication byte-ok**: az `EncodeTransformFrame` eredménye eddig egyenesen
   a `send()`-be ment — a payload mérete sehol nem volt számlálóban. Most a
   frame + spawn/despawn payload byte-ok mérve (a `ReconcileViewer` opcionális
   `out_bytes` paramétere; a `send` wrapper-elése elkerülve, mert az viewer-enkénti
   `std::function` allokáció lenne a hot pathon).
2. **AOI candidate count a cap előtt**: eddig csak az elfogadott látható
   entitások voltak meg; a pre-cap candidate szám most mérve (a 100-as sapka
   levágása így látható marad).
3. **Combat**: a `CombatSystem`-nek nulla számlálója volt; most sikeres
   attack/damage event kerül a mezőbe (a támadó pozícióján).
4. **Migration pozíció**: eddig csak darabszám; most a committed transfer
   pozíciója (cél oldal) + a boundary-crossing **átmenet** (nem minden tickben
   ismételve) kerül a mezőbe.
5. **Nincs megbízható input** (nem hamisítjuk, dokumentált seam):
   - Replication delta/dirty protokoll: a dirty transform darabszám mérve van
     (`repl_dirty`), de a küldés továbbra is full-frame — a byte-ok a valós
     fanout munkát mérik, a dirty csak diagnosztika.
   - Visibility set churn (enter/leave eseményszám) külön nem számláló; a
     spawn/despawn payload byte-ok a replication channelben jelennek meg.
     AOI recipient count közvetve a byte-okban/records-ban van.
   - Per-entity µs: szándékosan nincs (clock read entitásonként túl drága);
     a zone-szintű measured stage µs marad a mért horgony, a mező a ténylegesen
     lefutott update-eket számolja (nem entity countot: LOD-szűrt).

**Thread ownership (auditált, nem változott)**

A zone írási jogát a `ZoneWriteGuard` (`OwnerThreadId` CAS) adja: egyszerre
egy thread írhat. A load bin írások mind ilyen guard alatt történnek (tick
rendszerek, combat a command drain-ben, migration/split transfer a supervisor
guardja alatt), ezért a bin hot path **nem igényel lockot**. A publikálás a
tick végén a meglévő `activity_mutex_` alatt történik (mikroszekundumos
szakasz); a supervisor 1 Hz-en drainel.

### 6.2 Architektúra (implementált)

```
zone-local integer counters (guard alatt, lock nélkül)
        ↓  sparse touched lista
tick végi publish (activity_mutex)
        ↓  batched, csak touched cellák
supervisor aggregation (~1 Hz, Rebuild)
        ↓  AsymmetricEwma + normalizáció + L1
immutable LoadGrid generation (shared_ptr)
```

- **Fájlok**: `world/activity/LoadFieldTypes.h`, `ContinuousLoadField.h/.cpp`,
  `LoadFieldPublisher.h/.cpp`; a közös koordináta-primitív
  (`ClampedAxisCellFor`) az `ActivityTypes.h`-ba került, a `SpatialActivityField`
  ugyanazt használja (nincs duplikált mapping).
- **Tárolás (§34-35)**: dense rectangular L0 grid `WorldBounds` fölött
  (`dim_x × dim_y`, row-major); 100 km / 500 m = 40 000 cella. A zone-oldali
  gyorsító egy **dense rect a zone saját boundsára** (+32 m margin a
  boundary-crossing sávnak), a publikálás **sparse** (csak a touched cellák).
  L1 = `l1_ratio × l1_ratio` L0 blokk összeg (nem külön EMA — az EMA lineáris),
  így a későbbi L0/L1/L2 irány nem kizárt, de nincs hierarchy framework.
- **Csatornák**: `Simulation`, `Replication`, `AOI`, `Combat`, `Migration`
  (`LoadChannelMetadata` mondja meg channelenként a `Measured`/`EventCount`/
  `Unavailable` forrást és a mértékegységet). Nyersen soha nem adódnak össze:
  a normalizáció reference budgettel (raw/second ÷ budget/second, clamp [0,1],
  NaN/Inf → 0), a composite pedig explicit, konfigurálható súlyokkal készül; a
  channel breakdown mindig elérhető marad.
- **SimulationCost**: ténylegesen lefutott AI döntések + movement integrációk
  száma (LOD-szűrve, tehát nem entity count); a zone-szintű measured µs a
  `ZoneDiagnostics`-ban marad, és a mező nem kever bele hamis per-cell µs-t.
- **ReplicationPressure**: **mért byte** (frame + spawn/despawn), viewer
  pozícióra attribútálva; rekord és dirty darabszám külön diagnosztika.
- **AOIWork**: query + pre-cap candidate (sűrű hotspot drágább, mint ugyanannyi
  entitás szétszórva).
- **CombatHeat**: event-derived, az aszimmetrikus EMA-val decay-el (egy régi
  csata nem marad hot).
- **MigrationPressure**: committed transfer + boundary-crossing átmenet +
  split/merge belső transfer.
- **Smoothing (§13-14, §19)**: két időskálás aszimmetrikus EWMA τ-alapú
  alpha-val (`alpha = 1 - exp(-dt/τ)`), így a cadence-től független:
  fast rise τ=1.5 s / fall τ=8 s, slow rise τ=8 s / fall τ=25 s (config).
  A `predicted` mező a slow EMA másolata — §37 storage seam, nincs trajectory
  modell.
- **Immutability (§20)**: a supervisor épít új generációt és `shared_ptr`-t
  cserél; olvasók (planner/validator/bench) másolják a pointert. A generáció a
  saját config snapshotját hordozza, így a query-k a build-kori budgettel
  egyeznek.
- **§25 topológia-függetlenség**: a mező world-space; split/merge nem írja.
  Élő teszt: üres zóna force-splitja után a migration channel végig 0 maradt,
  a mező nem resetelődött; lakott zóna splitja viszont migration workként
  jelent meg (belső transfer).

### 6.3 Mért eredmények (RelWithDebInfo, 1 km teszt-térkép / 100 km sweep)

**Felbontás sweep** (`worldbench --mode loadfield`, 4200 szintetikus sparse
entry, 10 rebuild átlag):

| cella | cellák | aktív | memória/generáció | rebuild átlag | dense/sparse kontraszt |
|---|---|---|---|---|---|
| 250 m | 160 000 | 216 | 13.60 MB | ~7.1–8.2 ms | 280× |
| 500 m | 40 000 | 204 | 3.40 MB | ~1.5–1.6 ms | 1031× |
| 1000 m | 10 000 | 199 | 0.85 MB | ~0.43–0.45 ms | 2000× |

**Döntés: production default 500 m.** Indoklás: a 100 km-es világon 3.4 MB
generációnként, ~1.5 ms/1 Hz aggregáció (egy mag ~0.15%-a), miközben egy
hotspot 500 m-re lokalizálható. A 250 m (13.6 MB, ~7 ms) akkor indokolt, ha a
későbbi boundary-tervezés finomabb felbontást igényel; az 1000 m túl durva
(2 km kontraszt elveszik a szomszédokban). A felbontás **config**, és nincs
automatikusan a zone mérethez / AOI sugárhoz / activity cellához kötve. Az 1 km
teszt-térképen a default 500 m csak 4 cellát adna, ezért a bench 100 m-t
használ (a hotspot-teszt így értelmes).

**Smoothing válasz** (pure selftest): 1 s lépcsőre fast 5 s alatt 96%-on, slow
ugyanekkor 46%; egyetlen 1 s spike fast csúcsa 49%, 5 csendes másodperc után
26% / slow 10%; 30 csendes másodperc után fast 2.3%, slow 14%, és fast < slow
(a gyors idősík enged el előbb). Tehát rövid spike nem tartósít, a hotspot
lassan hűl, nem ragad be.

**Instrumentation overhead**: A/B `--mode spread` 100 player / 1000 mob,
20 s: ON avg 2.766 ms / p99 6.710 ms vs OFF 2.928 ms / 8.376 ms — a különbség
a futásonkénti szórás alatt van. Az instrumentation entitásonként/eventenként
1 index-számítás + integer increment (nincs lock, allokáció, string, clock
read, globális lookup a hot pathon).

### 6.4 Tesztelés

- `worldbench --loadfield-selftest` (pure, world nélkül): mapping (negatív
  origin, non-square, clamp), zone bin sparse publish + reset + margin,
  normalizáció (budget, cadence-függetlenség, clamp, NaN/Inf), aszimmetrikus
  EMA (rise/fall/spike/decay), L1 pontos blokk-összeg + grid audit, config
  validáció (minden invalid mező javítva, warning).
- `worldbench --mode loadfield` (élő, saját sim lifecycle): hotspot
  lokalizáció + második hotspot + hideg cella 0, channel breakdown
  (sim/repl/AOI), normalizált/composite bounds, fast-leads-slow, combat heat
  + decay, §25 topológia-függetlenség (üres split nem termel load-ot),
  split-transfer mint migration work, hotspot decay, generáció-audit
  (`ValidateLoadFieldGrid` a supervisor quiescent ablakában), majd a
  felbontás sweep.
- Regresszió: `--field-selftest` 4/4, `--mode lod` 18/0, `--mode activity`
  22/0, `--mode splitmerge` 20/0 továbbra is zöld; a viselkedésérzékeny
  értékek (LOD tier-pillanatképek, cross-counters) változatlanok.

### 6.5 Ami szándékosan kimaradt (következő chunkok)

- `PartitionLoadScore`, p95/p99 a control plane-en, döntési log, why-not
  diagnostics, `MinExpectedImprovement`, `TopologyComplexityPenalty` (D).
- BoundaryCost field, Partition Objective, hotspot detection (flood fill),
  hotspot-aware split, multi-candidate split, merge sustained-low timer (E).
- `worldbench --mode loadfield` validator-bővítés a meglévő 7 módra (F) —
  a regresszió már fut, a `LoadFieldValidator` integráció a D chunkkal jön.
- A replication delta protokoll és a per-entity cost model továbbra is seam;
  a mező ezek nélkül is korrekt és konzervatív (nullát mér, nem hamisít).

---

## 7. Chunk D/E — Adaptive Partition Scoring + Hotspot-Aware Split

> Phase-2 implementációs jegyzet. A megrendelői szöveg a §26-nál félbeszakadt
> (*„Concept"*); az implementáció a §0–§26 követelményeit ÉS a dokumentum saját
> D/E chunk-tervét követi. Az audit a `e97333df` HEAD tényleges source-án készült.

### 7.1 Audit — mi volt meg, mi hiányzott

- **Megvolt és újrahasznosítva** (nem lett újraimplementálva): a teljes
  `ContinuousLoadField` pipeline (5 channel, EMA, normalizáció, L1, immutable
  generáció), a `ZoneLoadMonitor` (avg tick + resident score, sustained timer),
  a `ZoneScheduler` gate-jei (sustained/cooldown/depth), a `ZoneManager`
  tranzakciós quadtree split/merge (Plan→Stage→Transfer→Validate→Commit), a
  `WorldRuntime` control loop (1 Hz, max 1 mutáció/ciklus), a validator és a
  bench.
- **Hiányzott**: térbeli (world-space) partition score, candidate cut
  pozíciók, boundary cost, p95/p99 a control plane-en, strukturált döntési log,
  why-not diagnostics, min-improvement gate, hotspot fogalom.
- **Eltérés a prompttól, dokumentálva**: a prompt candidate-jei 2-way cutok
  (X/Y); a meglévő executor kizárólag 4-way quadtree splitet tud. Ezért egy
  candidate itt a **quadtree középpontja**, és a `PlanSplit` kapott egy
  opcionális, min-size-ra clampolt center override-ot. Egy geometria, egy
  executor — nincs második split rendszer (a prompt §7 tiltása).

### 7.2 Architektúra — OBSERVE → SCORE → DECIDE → EXECUTE

```
ZoneLoadMonitor.Update          OBSERVE  (p95/p99 + field overload + timerek)
        ↓
PartitionScorer.ScoreSplit      SCORE    (read-only, tiszta, determinisztikus)
        ↓
gate-ek (sustained, cooldown, depth, min-size, MinExpectedImprovement)
        ↓
RunSplitTransaction             EXECUTE  (meglévő tranzakciós rendszer)
```

- Új fájlok: `world/partition/PartitionScoring.h/.cpp`. A load field oldalon
  `LoadAggregate`/`AggregateLoad` + `DetectLoadHotspots` (read-only seam), az
  activity field oldalon `CountPlayerSourcesIn` (külön boundary signal, §17).
- A scorer **soha nem mutál**: nincs zone create/retire, transfer, OwnerMap
  vagy partition tree írás. A `ScoreSplit` stateless (minden scratch lokális),
  így a supervisor mellett a bench/admin is olvashatja párhuzamosan.
- A monitor csak pontszámot + timert ír a partition node-ra; a döntést a
  `WorldRuntime::ExecutePartitionControl` hozza, max 1 topológia-mutáció/ciklus.

### 7.3 Candidate modell (determinisztikus)

Rögzített sorrendben 9 candidate, mind a meglévő quadtree geometriából:
`midpoint` · `centroid` · `centroid-x` · `centroid-y` · `balanced-x` ·
`balanced-y` · `hotspot-x` · `hotspot-y` · `hotspot-corner`.

- A cut pozíciók a load field **cellahatáraira snap-elnek** (így a child
  aggregáció pontosan csempézi a cellákat), majd a min-zone-size sávba
  clampolódnak; ha abban nincs cellahatár, sima clamp.
- `balanced-*`: a zóna terhelését legjobban felező cellahatár (prefix-összeg,
  O(cells), determinisztikus, holtversenynél az alacsonyabb pozíció nyer).
- `hotspot-*`: a legnagyobb hotspot **izoláló élére** tett cut (a hotspot egész
  marad egy childban); a `hotspot-corner` mindkét tengelyen.
- Dedupe az első előfordulást tartja meg; a lista sorrendje a dokumentált
  végső tie-break.
- **Tie-break**: (1) nagyobb `final_score`, (2) kisebb `boundary_penalty`,
  (3) korábbi lista-pozíció. `unordered_map` iteráció soha nem dönt.

### 7.4 Score modell

```
final_score =  w_balance     * balance_benefit
             - w_boundary    * boundary_penalty
             - w_migration   * migration_penalty
             - w_replication * replication_penalty
             - w_instability * instability_penalty
             - topology_penalty
```

- **balance_benefit = peak_reduction** = `(before_total - max_child)/before_total`.
  Ez a „legterheltebb resulting partition" csökkentése: 99/1 felosztásnál ~0,
  kiegyensúlyozottnál nagy — pontosan a §14 elvárása. `balance_ratio`
  (max/mean child) csak diagnosztika.
- **boundary_penalty** = `0.35*activity_band + 0.35*migration_band +
  0.15*combat_band + 0.15*hotspot_crossed_frac`. A band a candidate két belső
  cut vonala körüli sáv (config `boundary_band_m`, default 1.5×AOI).
  - `activity_band`: **becsült** — activity-field player source-ok a sávban
    (külön signal, §17, soha nem keveredik a load channelekbe).
  - `migration_band`: **mért** — a load field migration channelje a sávban.
  - `combat_band`: event-alapú, a sávban.
  - `hotspot_crossed_frac`: a kettévágott hotspot-load aránya (flood-fill
    komponensek a `hotspot_threshold` felett).
- **replication_penalty** = replication channel a sávban — **estimated**
  (a jelenlegi mért byte-okból extrapolált cross-boundary hatás, nem jövőbeli
  mérés).
- **migration_penalty** = `(players*w_player + mobs*w_mob)/budget` — a
  végrehajtási transfer-work **becslése** a jelenlegi populációból; zóna-szintű,
  ezért candidate-független (nincs csalás a rangsorban), a NOOP-hoz képesti
  összköltséget reprezentálja. **Nem** azonos a MigrationPressure channellel
  (§19): az a jelenlegi churn, ez a candidate végrehajtási költsége.
- **instability_penalty**: recens split/merge után lineárisan lecsengő soft
  penalty (`instability_window_s`, default 120 s); a hard gate-ek (cooldown,
  depth, min-size) változatlanok.
- **topology_penalty**: fix költség a +4 zónáért (§33), default 0.05.
- Minden tag [0,1]-re clampolt, a score [-1,1], NaN/Inf ellen védett.
- **Min improvement gate** (§25): split csak akkor, ha
  `best.final_score >= min_expected_improvement` (default 0.10). A NOOP
  baseline = 0; a `current_peak`/`before_total`/`after_peak` a breakdownben
  látható (§24).
- **Overload detection** (§26): `load_score = max(legacy, field)`, ahol a
  legacy = max(avg tick, **p99 tick**, resident) és a field = max(zone mean,
  zone peak cell composite). A sustained szemantika a meglévő
  `sustained_breach_since` timer (nem duplikálva); egy 1 Hz sample soha nem
  indít splitet.
- **Decision timescale audit** (§5): a scorer a **fast** EMA-t olvassa
  (rise ~1.5 s / fall ~8 s): a `current` egy-window spike-ja zajos lenne, a
  `slow` (8/25 s) túl lassan követi a hotspot mozgását, a `predicted` pedig
  `== slow` seam. A sustained timer adja a temporális megerősítést. Config:
  `partition_scoring_timescale`.

### 7.5 p95/p99 a control plane-en

A `ZoneLoadMonitor` a zóna 256 mintás tick-ringjéből számol p95/p99-et a saját
1 Hz cadence-én (csak ha a zóna az ablakban tickelt — alvó zóna régi mintái
nem éleszthetik fel a régi overloadot), és bekerül a legacy score-ba. Ez zárja
a §1.4-ben dokumentált rést: a hotspot esetben az avg 0.78 volt, miközben a
p99 a budget 2×-e — most a p99 emeli a gate score-t.

### 7.6 Döntési log és why-not diagnostics

- Minden döntés strukturált `PartitionDecisionRecord` (bounded, 64): executed
  split vagy NOOP, a teljes candidate-breakdownnal (balance/boundary/migration/
  replication/instability/topology + channel-bontás a childokra).
- Logolás CSAK topológia-döntésnél; a why-not sorok zónánként rate-limitáltak
  (`why_not_log_seconds`, default 10 s) — nem tick-spam (§30).
- Példa (élő futásból):
  `SPLIT zone=3 candidate=midpoint@(750,500) score=0.374 benefit=0.440
  boundary=0.000 migration=0.108 repl=0.000 instability=0.000 topology=0.050
  before=2.27 after=1.27 p99=1.1ms field_epoch=9`
  `NOOP zone=3 reason=below-min-improvement ... score=-0.058`
- Why-not detail a konkrét blokkolót nevezi meg: `not-sustained`, `cooldown`,
  `max-depth`, `min-size`, `commands-pending` (scheduler gate + PlanSplit
  dry-run).

### 7.7 Merge — seam, nem újragondolás

A merge predikátum változatlan (sibling threshold + cooldown). A
`ZonePartition::field_low_since` rögzíti, mióta van a combined score a merge
threshold alatt — ez a **sustained-low seam** a következő fázisra; egyelőre
nem gate-el (a phase-2 mandátum: a merge scoringot nem bonyolítjuk, amíg a
split nincs validálva).

### 7.8 Mért eredmények

**Pure selftest** (`--partitionscore-selftest`, 9 teszt): rect-aggregáció
(pontos összegek, centroid, fél-nyílt csempézés), hotspot flood-fill,
candidate-determinizmus + hotspot-kerülés, balance formula (uniform 0.75,
99/1 → 0.03), boundary penalty (midpoint mig-band 1.00 → best 0.50),
instability, gate szemantika, config-validáció, döntés-formázás.

**Élő szcenárió** (`--mode partitionscore`, 1 km teszt-térkép, 100 m cella):
- Phase A — egyetlen klaszter a zóna szélén (a min-size floor miatt egyetlen
  valid cut sem tudja szétválasztani): overloaded, de a scorer **NOOP**-ot ad
  (score −0.058 < 0.10), a production loop NOOP-ot logol és **nem** splittel.
- Phase B — második klaszter a túloldalon: a gate átenged (score 0.19–0.37),
  a tranzakciós executor commitol (split_commits=1, merge_commits=0), a
  legnagyobb child terhelés **3.00 → 2.00** (peak reduction ~33%).
- Determinisztikusság: azonos field+activity generáción bit-azonos
  candidate-lista/score.
- Cooldown: a split után nincs azonnali második split.
- Phase C — despawn után a mező lehűl, a csendes világ nem splittel újra.
- Validátor minden fázis után zöld.

**Default súlyok viselkedése (dokumentált trade-off):** a compute balance az
elsődleges; egy tiszta mob-hotspot kettévágása valódi peak-csökkenést hoz, ezért
a default nem tiltja meg. A hotspot-kerülés a boundary band **mért** jelein
(player influence, migration churn, combat) és a configurálható
`weight_boundary`-n keresztül érvényesül; a selftest explicit
`weight_boundary=5.0`-dal bizonyítja, hogy a mechanizmus képes a hotspot
körbevágására, amikor a balance elfogadható marad (§24).

### 7.9 Config (gameserver.conf.example)

`partition_min_expected_improvement`, `partition_scoring_timescale`,
`partition_scoring_boundary_band_m`, `partition_scoring_hotspot_threshold`,
`partition_scoring_hotspot_max_count`, `partition_scoring_weight_{balance,
boundary,migration,replication,instability}`, `partition_scoring_topology_penalty`,
`partition_scoring_{activity,migration,replication,combat}_band_budget`,
`partition_scoring_migration_work_budget`, `partition_scoring_instability_window_s`,
`partition_scoring_decision_log`, `partition_scoring_why_not_log_seconds`.
Minden kulcs opcionális; invalid érték warning + fallback
(`ValidatePartitionScoringConfig`), az effective set startupkor logolódik.

### 7.10 Tesztelés

| futtatás | eredmény |
|---|---|
| `--field-selftest` | 4/4 PASS |
| `--loadfield-selftest` | 6/6 PASS |
| `--partitionscore-selftest` | 9/9 PASS |
| `--mode splitmerge` | 20/0, validations=3 |
| `--mode lod` | 18/0, validations=6 |
| `--mode activity` | 22/0, validations=7 |
| `--mode loadfield` | 22 check + 3 audit, 0 failure |
| `--mode partitionscore` | 21 check + 2 audit, 0 failure |
| `--routing-selftest` | 5/5 PASS |

### 7.11 Ami szándékosan kimaradt (következő fázis)

- Merge sustained-low **gate** bekapcsolása (a seam kész).
- Nem-quadtree candidate-ek tényleges végrehajtása (2-way/elastic boundary):
  a candidate API nem quadtree-only, de az executor egyelőre az.
- Control-loop frekvenciák configból (§29) — jelenleg 1 Hz fix.
- Predicted load alapú pre-splitting (§37) — a storage seam kész.
- Replication delta protokoll (a replication penalty továbbra is estimated).

---

## 8. Phase 3 — Adaptive Merge Scoring + Sustained-Low Merge + Stability Controller

> Implementációs jegyzet. A megrendelői szöveg a §25-nél félbeszakadt
> (*„Alapvetően vizsgáld meg:"*); az implementáció a §0–§24 követelményeit
> követi, a §25 prioritást pedig explicit, determinisztikus szabályként
> rögzíti (lásd §8.6). Az audit a `7caf3c8e` HEAD tényleges source-án készült.

### 8.1 Audit — mi volt meg, mi hiányzott

- **Megvolt és újrahasznosítva** (nem lett újraimplementálva): a teljes
  tranzakciós merge executor (`PlanMerge` → `CreateStagedMergeTarget` →
  transfer → validate → `CommitMerge`/`AbortMerge`), a sibling-set
  validáció (`PlanMerge`), a merge cooldown (`ShouldMerge` /
  `parent->last_merge_time`), a `field_low_since` leaf-seam, a
  `PartitionScorer` split rétege, a `ZoneLoadMonitor` observe-lánc, a
  `PartitionMetrics`, a validator és a bench.
- **Hiányzott**: csoport-szintű sustained-low állapot, merge scoring
  (predicted parent load, safety margin, benefit/penalty breakdown),
  irányfüggő cooldownek, merge why-not, stability metrikák, oscillation
  guard, moving-hotspot validáció.
- **Eltérés a prompttól, dokumentálva**: a prompt merge candidate-je a
  teljes quadtree sibling group — ez pontosan az, amit a meglévő executor
  tud. A kontroller **pontosan 4 aktív leaf childot** követel meg
  (`EvaluateMergeGate::NotFourChildren`), míg a `PlanMerge` history okból
  2+ gyereket tolerál; a kontroller soha nem hívja 4 alatt. A prompt a
  §25-nél megszakadt; a split-vs-merge prioritást explicit szabályként
  implementáltam (§8.6).

### 8.2 Architektúra — OBSERVE → SCORE → DECIDE → EXECUTE

```
ZoneLoadMonitor.Update        OBSERVE  (group sustained-low timer + gate-ek)
        ↓
PartitionScorer.ScoreMerge    SCORE    (read-only, stateless, determinisztikus)
        ↓
stability gates (cooldown, safety margin, min improvement)
        ↓
RunMergeTransaction           EXECUTE  (meglévő tranzakciós rendszer)
```

- A scorer **stateless és read-only maradt** (§23): a stabilitási állapot
  (`group_low_since`, `last_split_time`, `last_merge_time`) a
  partition node-okon él, és **inputként** kerül a `MergeScoreInput`-ba.
  A `ScoreMerge` minden scratch-e lokális, így a supervisor mellett a
  bench/admin is párhuzamosan olvashat.
- Új/ módosított: `PartitionScoring` (merge scoring + breakdown +
  döntés-formázás), `ZoneScheduler` (`EvaluateMergeGate`, irányfüggő
  cooldownek, emergency bypass), `ZoneLoadMonitor` (csoport observe),
  `ZonePartition` (`group_low_since` + `CollectInternalNodes`),
  `ZoneManager` (állapot-reset a tree-mutációknál), `WorldRuntime`
  (score-then-decide + metrikák), `PartitionMetrics`.

### 8.3 Merge szemantika — egy geometria, egy executor

- Merge candidate = egy **valódi quadtree parent** teljes sibling groupja
  (NW/NE/SW/SE). Nincs arbitrary-neighbor, 2-way vagy irregular merge.
- A discovery determinisztikus DFS a partition-erdőkön
  (`CollectInternalNodes`, tree/child-vector sorrend) — `unordered_map`
  iteráció soha nem dönt.
- A belső cut vonalak a **tényleges child boundsból** származnak
  (`NW.max_x`, `NW.min_y`), így az off-midpoint (hotspot-aware) splitek
  után is pontosak.

### 8.4 Sustained-low sibling group (§7-8)

- Csoport-pontszám: `group_load_score = max(max(child.load_score),
  parent_fast, parent_slow)`, ahol a parent aggregátum a **teljes parent
  területre** újraszámolt `max(mean, peak)` a load field mindkét
  idősíkján. Egyetlen hot child → az egész csoport ineligible.
- A `group_low_since` timer explicit: start, ha a csoport a merge
  threshold alatt van; **reset**, ha bármely child vagy az aggregátum
  fölé megy. Nincs implicit lecsengés.
- Default `merge_sustained_low_seconds = 90` ≈ 3.6× a load field slow
  fall tau-ja (25 s): mire a csoport sustained-low lesz, a slow EMA
  gyakorlatilag kiürítette a korábbi hotspotot. A bench rövidebb,
  kompresszált ablakkal validálja a mechanizmust.
- Value hysteresis: `merge_load_threshold` (0.25) jóval a
  `split_load_threshold` (0.9) alatt; time hysteresis: split sustained
  ~10 s vs. merge sustained ~90 s.

### 8.5 Merge score modell (§12-18)

```
final_score =  w_mtopo * topology_benefit
             + w_mbnd  * boundary_benefit      (activity + combat + hotspot)
             + w_mmig  * migration_benefit     (mért migration churn)
             + w_mrep  * replication_benefit   (estimated replication churn)
             - w_mrisk * parent_load_risk      (predicted parent load, négyzetes)
             - w_mexec * execution_penalty     (transfer-work becslés)
             - w_minst * instability_penalty
```

- **predicted parent load**: a teljes parent területre újraszámolt
  aggregátum mindkét idősíkon — NEM a child-score-ok átlaga (§12).
- **Post-merge safety margin** (§13): a merge csak akkor mehet, ha
  `predicted_parent_load <= split_load_threshold - margin` (default
  0.15 → ceiling 0.75). Ez akadályozza meg a közvetlen visszasplittet.
- **parent_load_risk**: `(predicted / ceiling)²` — közel nulla nyugodt
  csoportnál, 1.0 a ceilingnél.
- **topology_benefit**: dokumentált konstans (default 0.05, szimmetrikus
  a split `topology_penalty`-jával): 3 kevesebb
  directory/ghost/scheduler bejegyzés. Szándékosan kicsi — egy csendes,
  churn nélküli csoport **nem** megy át a min-improvement gate-en.
- **boundary/migration/replication benefit**: ugyanaz a band-mechanika,
  mint a split penalty-nél (a belső cut vonalak körüli sáv); a
  migration/replication **nincs** duplán számolva a boundary_benefitben.
- **execution_penalty**: a jelenlegi populációból becsült transfer-work
  (read-only, nincs próbamigráció, §15).
- **Minimum merge improvement** (§18): default 0.10; alatta NOOP.
- Minden tag [0,1], a score [-1,1], NaN/Inf-védett, config-driven.

### 8.6 Stabilitás — cooldownek, prioritás, emergency (§19-25)

- **split_to_merge_cooldown** (default 120 s): split után a csoport nem
  merge-elhető; why-not: `MERGE_SUPPRESSED_RECENT_SPLIT`
  (`detail=split-cooldown`).
- **merge_to_split_cooldown** (default 90 s): merge után a leaf nem
  splittelhető; why-not: `merge-cooldown`.
- **merge_cooldown** (meglévő, merge→merge) változatlan.
- **Explicit prioritás** (§25, a szöveg itt megszakadt): egy control
  ciklusban **max 1 topológia-mutáció**, és a **split elsőbbséget élvez**
  (aktív compute-nyomás), a merge csak akkor fut, ha nem történt split.
  Determinisztikus, dokumentált.
- **Emergency split bypass seam** (§21): a merge-to-split cooldown csak
  **mért dual signal** esetén ugorható át — a split load-gate már
  átment, ÉS a p99 tick ≥ `emergency_p99_multiplier` × budget (default
  2.0). Config-kapcsoló (`emergency_split_bypass`), a bypassok
  számolódnak (`split_emergency_bypasses`). Nincs magic threshold.
- **Oscillation guard** (metrika): ha egy node-on split és merge
  történik az `oscillation_window_s`-en belül, a
  `oscillation_guard_trips` nő. A hard guard a cooldown; ez a metrika a
  tuninghoz.

### 8.7 Metrikák és why-not diagnostics

- `PartitionMetrics` bővítés: `merge_candidates_evaluated`,
  `merge_suppressed_{not_eligible,not_sustained,recent_split,recent_merge,
  post_merge_unsafe,min_improvement,transaction}`,
  `split_suppressed_merge_cooldown`, `split_emergency_bypasses`,
  `oscillation_guard_trips`.
- Strukturált döntési log: `MERGE parent=3 children=[4,5,6,7] score=0.056
  predicted=0.09 peak=0.09 risk=0.069 topology=0.150 boundary=0.028
  migration=0.002 repl=0.004 execution=0.008 instability=0.348
  sustained_low=3s field_epoch=63`, illetve
  `MERGE-NOOP parent=3 reason=merge-not-sustained detail=not-sustained
  sustained_low=1s group_load=0.152`.
- A split why-not `reason` mezője most pontos: `split-not-sustained`,
  `split-cooldown`, `split-max-depth`, `split-min-size`,
  `split-not-eligible` (a `detail` a konkrét gate-et nevezi).

### 8.8 Mért eredmények

**Pure selftest** (`--partitionscore-selftest`, 13 teszt): a 4 új merge
teszt igazolja, hogy
- egy hot child → `safety_ok=false` (peak 0.90 > ceiling 0.75), miközben
  a whole-area mean 0.22 (nem child-átlag);
- egy csendes, churn nélküli csoport score-ja 0.032 < 0.10 → NOOP
  (konzervatív);
- a belső cutokon mért migration churn (0.3/cella) esetén a score 0.116
  ≥ 0.10 → merge (valódi, mért benefit);
- determinizmus, config-validáció (threshold invariáns), döntés-formázás.

**Élő szcenárió** (`--mode stability`, kompresszált smoothing/cooldown):
- Phase A: két klaszter → split commit (split_commits=1).
- Phase B: a klaszterek eltűnnek, 4 játékos a belső cutokon →
  csoport sustained-low + valódi boundary benefit → **merge commit**
  (score 0.056–0.12, predicted 0.07–0.09 < ceiling 0.35).
- Phase C: hotspot közvetlenül a merge után → a split
  **elnyomva** (`split_suppressed_merge_cooldown=9`, why-not
  `merge-cooldown`), majd a cooldown lejárta után split commit.
- Phase D: a hotspot átkerül egy másik childba → amíg hot, **nincs
  merge**; lehűlés után újra merge. `oscillation_guard_trips=0`,
  `emergency_bypass=0`.
- Validátor minden fázis után zöld.

### 8.9 Config (gameserver.conf.example)

`partition_merge_sustained_low_seconds`,
`partition_split_to_merge_cooldown_seconds`,
`partition_merge_to_split_cooldown_seconds`,
`partition_scoring_post_merge_safety_margin`,
`partition_scoring_min_merge_improvement`,
`partition_scoring_merge_topology_benefit`,
`partition_scoring_weight_merge_{topology,boundary,migration,replication,
risk,execution,instability}`,
`partition_scoring_oscillation_window_seconds`,
`partition_scoring_emergency_split_bypass`,
`partition_scoring_emergency_p99_multiplier`.
Minden kulcs opcionális; invalid érték warning + fallback, az effective
set startupkor logolódik.

### 8.10 Tesztelés

| futtatás | eredmény |
|---|---|
| `--field-selftest` | 4/4 PASS |
| `--loadfield-selftest` | 6/6 PASS |
| `--partitionscore-selftest` | 13/13 PASS |
| `--mode splitmerge` | 20/0, validations=3 |
| `--mode lod` | 18/0, validations=6 |
| `--mode activity` | 22/0, validations=7 |
| `--mode loadfield` | 22 check + 3 audit, 0 failure |
| `--mode partitionscore` | 21 check + 2 audit, 0 failure |
| `--mode stability` | 25 check + 2 audit, 0 failure |
| `--routing-selftest` | 5/5 PASS |

### 8.11 Ami szándékosan kimaradt (következő fázis)

- Merge predikció a `predicted` timescale-ből (§37) — a seam kész.
- Emergency policy finomítása valódi terhelési mérésekből.
- Control-loop frekvenciák configból (§29).
- Nem-quadtree merge/elastic boundary — az API nem zárja ki, az executor
  egyelőre quadtree-only.

---

## 9. Phase 4 — Integrált readiness benchmark (100 km / 500 player / 200k mob)

> Mérési fázis, nem feature-fejlesztés. A megrendelői szöveg a §29-nél
> félbeszakadt (*„promotion immediate;"*); a dokumentum a §0–§28 követelményeit
> dolgozza fel, a §29 LOD-correctness pontjait a meglévő LOD/activity
> regressziókra és a mért tier-eloszlásra támaszkodva igazolja.

### 9.1 Audit — mi mérhető, mi nem

- **Már mérve volt**: zone tick (ring + p50/p95/p99/max), gameplay/ghost/repl
  stage-ek, LOD eval µs, AOI query, transform record, migration, worker pool,
  supervisor, routing, activity/load field rebuild, partition tranzakciók.
- **Össze volt vonva** → minimálisan instrumentálva (nem új metrics rendszer):
  `ai_micros`, `movement_micros` (a gameplay-n belül), `aoi_micros` (a
  replikáción belül), `activity_publish_micros`, `load_publish_micros`,
  `ghost_entities` (ghost munka-proxy). A supervisor oldalon:
  `control_us` / `observe_us` / `score_us` / `migration_us`.
  Minden `*_since_diag` a meglévő 1 Hz exchange-csatornába került (különben
  a readiness harness first-sample-je a teljes warmupot beleszámolta volna).
- **Benchmark plumbing újrahasznosítva**: `ZoneDiagnostics` ring,
  `CollectProcessLoad`, `PartitionMetrics`, `LoadFieldMetrics`,
  `ActivityMetrics`, `MigrationMetrics`, routing metrikák, `LodWorkTotals`, a
  meglévő `--lod-off` / `--loadfield-off` kapcsolók + új `--asf-off`
  (observe-only baseline: a telemetria fut, döntés nincs;
  `PartitionScoringConfig::adaptive_enabled`).

### 9.2 Mérési módszertan

- **Világ**: szintetikus, flat, 100 km × 100 km, 8×8 = 64 kezdeti zóna
  (12.5 km), 4 régió. A production rendszerek futnak (nincs mock szimuláció);
  a 100 km² heightfield asset helyett `TerrainService(extent)` seam.
- **Fázisok**: SETUP (spawn) → WARMUP (melegítés, nem mért) → MEASURE →
  FINAL VALIDATION. A setup költség soha nem keveredik a steady-state tickbe.
- **Determinizmus**: fix seed, determinisztikus spawn-pont generálás (nincs
  kontrollálatlan RNG a workload-elrendezésben), dokumentált sűrűségek. A
  mobok spawn-sugara = wander-leash (production szemantika), ezért a "spread"
  200k mob 200k külön ponton, 40 m leash-csel jelenik meg — nem egyetlen
  óriás clusterként (ami hamis migration/ghost churn-t gyártana).
- **Scenario-k**: spread, quiet, hotspot, multi, moving, border, combat,
  replication, churn (dokumentáltan gyorsított control-timerekkel), dense
  (legrosszabb praktikus sűrűség: 20k mob + 500 player 2×2 km-en = 5k mob/km²,
  125 player/km²).
- **Build/környezet**: Windows, 16 logikai mag, 63.9 GB RAM, MSVC 19.51,
  **RelWithDebInfo** (NDEBUG=1, LTCG), x64. Nem virtualizált/container.
- **Mérés**: 200 ms-onkénti mintavétel; a stage-összegek a supervisor 1 Hz-es
  exchange-ablakait delta-követéssel rekonstruálják; a tick percentilisek a
  256 mintás ringből (utolsó ~12.8 s/zóna); a globális számlálók cumulatív
  delták. A validation a supervisor quiescent ablakában fut (max 90 s várakozás).

### 9.3 Eredmények — scenario-k (warmup 60 s, measure 30 s)

| scenario | zónák | tick avg | p50 | p95 | p99 | max | domináns stage |
|---|---|---|---|---|---|---|---|
| spread | 320 | 1.97 ms | 1.54 | 5.77 | 7.01 | 17.0 | **ghost 77.3%** |
| quiet | 320 | 1.97 ms | 1.54 | 5.77 | 7.01 | 17.0 | **ghost 77.3%** |
| hotspot | 280 | 2.55 ms | 1.70 | 4.25 | 17.28 | 61.6 | ghost 54.2%, repl 26.6% |
| multi | 180 | 3.14 ms | 0.93 | 14.01 | 20.55 | 46.4 | ghost 59.5%, repl 19.1% |
| moving | 276 | 3.20 ms | 2.94 | 5.35 | 15.51 | 92.9 | repl 50.0%, gameplay 30.5% |
| border | 328 | 1.95 ms | 0.95 | 5.33 | 10.52 | 25.6 | ghost 56.3%, gameplay 30.1% |
| combat | 80 | 3.41 ms | 0.92 | 6.77 | **70.65** | **187.2** | repl 53.2%, gameplay 29.8% |
| replication | 100 | 3.67 ms | 3.16 | 4.64 | **57.58** | **144.5** | **repl 84.7%** |
| churn | 158 | 2.10 ms | 1.19 | 4.83 | 10.96 | 33.5 | repl 40.9%, gameplay 38.7% |
| dense | 68 | 8.73 ms | 2.94 | 4.61 | **294.94** | **1184.1** | **repl 63.9%** |

Megjegyzések:
- A 64 kezdeti zónát az ASF a warmup/measure alatt 68–328 slotra bontja
  (split), a sleeping zónák száma 0–272 között mozog (zone sleep működik).
- **LOD eloszlás** (spread, 90 s warmup): full 728, reduced 7655, low 75751,
  dormant 115868 — a 200k mob **~58%-a Dormant**, ~0.4% Full. A 30 s warmup
  után (gyorsabb futások) még kevesebb Dormant: a demóciós kaszkád
  (5/30/60 s grace) ~95 s alatt teljes, ezért a warmup hossza kritikus.
- **Migration**: spread 53 commit / 94 boundary crossing 30 s alatt (realista,
  40 m leash mellett); border/churn 15–114 commit; a hotspot 42k crossingja
  a sűrű, határhoz közeli mozgásból jön.
- **ASF döntések**: churn = 21 split + 2 merge commit, 3 merge eval, 6
  sustained-low és 10 eligibility suppression; a többi scenario split-heavy
  (a durva 12.5 km-es kezdeti rács finomítása), merge nélkül.
- **Replication volume**: spread 13.5 MB / 30 s (0.45 MB/s); hotspot 466 MB
  (15.5 MB/s, 41M record); multi 519 MB (17.3 MB/s); replication 422 MB;
  dense 87 MB. A byte-ok a fanouttal együtt skálázódnak.
- **Memória** (working set): spread 2.04 GB, asf-off 1.47 GB, hotspot 3.15 GB,
  multi 3.22 GB, replication 2.74 GB, dense 1.90 GB. A load grid fix 3.24 MB,
  az activity grid 0.92 MB — a többi a flecs entity-tár + zóna struktúrák.

### 9.4 A/B mérések

**ASF control ON vs OFF** (spread, 500p/200k):

| | zónák | tick avg | p99 | ghost | control (30 ciklus) |
|---|---|---|---|---|---|
| ASF ON | 320 | **1.97 ms** | **7.01** | 77.3% | 700 ms (observe 180) |
| ASF OFF | 64 | 3.83 ms | 9.82 | 66.8% | 81 ms |

Az adaptive split **~49%-kal csökkentette az átlagos ticket és ~29%-kal a
p99-et**, miközben a control-plane ~23 ms/ciklus (1 Hz, ~2.3% egy magból) és
a memória +0.6 GB. Ez a fázis legfontosabb pozitív eredménye.

**Load Field ON vs OFF** (asf-off, mindkettő 64 zóna — tiszta telemetria A/B):

| | tick avg | p99 | gameplay | AI+movement | load_publish |
|---|---|---|---|---|---|
| field ON | 3.830 ms | 9.817 | 70.9 s | 69.6 s | 213 µs |
| field OFF | 3.694 ms | 9.669 | 65.1 s | 64.0 s | 2 µs |

A Load Field telemetria **~3–4%-os tick overhead** (a zóna-bin `CellFor`
hívások az AI/movement hurkokban), a rebuild ~1.1 s / 30 s (1 Hz, 40k cella).
Nem bottleneck.

**LOD ON vs OFF** (50k mob, 200 player, 64 zóna, 30 s warmup — kontrollált,
nem a teljes 200k, hogy a gép ne telítődjön):

| | tick avg | p99 | gameplay | AI+movement | Full mob |
|---|---|---|---|---|---|
| LOD ON | **0.644 ms** | **1.22** | 5.49 s | 5.24 s | 137 |
| LOD OFF | 1.251 ms | 2.68 | 36.6 s | 36.1 s | 50000 |

A LOD **~49%-kal csökkenti a ticket és ~86%-kal a tényleges AI+movement
munkát** már 30 s warmupnál (a Dormant arány növekedésével ez tovább javul).
Production default változatlan.

### 9.5 Bottleneck rangsor (mért)

1. **Ghost maintenance** — 54–77% minden spread-jellegű scenarióban. A
   `GhostSystem::Rebuild` minden tickben **az összes** ghostot eldobja és újra
   létrehozza flecs entity-ként a szomszédok publish-buffereiből (spread: 97M
   ghost-entity művelet / 30 s). A költség a player-bearing zónák számával és
   a szomszédok border-band entitásszámával skálázódik.
2. **AOI/replikáció nagy sűrűségnél** — replication 84.7% (AOI 29 s/30 s),
   dense p99 **295 ms**, replication p99 58 ms, combat p99 71 ms. A
   viewer-enkénti ~240 pre-cap candidate + a 100-as cap előtti rendezés a fő
   költség; a fanout byte-ok 14–17 MB/s csúcsot érnek el.
3. **LOD eval az activity-field query-n** — mob-hotspotnál a Fast query
   O(a low-radius boxban lévő összes player-source)/mob; 20k mob × 400 player
   = 8M távolságellenőrzés/eval. Ez adja a hotspot/dense max spike-ok egy
   részét.
4. **Combat command + fanout** — dense hotspotban 5000 attack command/s, a
   health/death payload minden látható viewer-nek megy; combat p99 71 ms.
5. **Control-plane observe** — 320 zónánál ~23 ms/ciklus (1 Hz); most
   elfogadható, 1000+ zónánál újra kell nézni.
6. **Supervisor migration** — 9–92 ms / 30 s; nem bottleneck.

A Load Field, Activity Field, LOD eval, zone sleep, migration és a partition
tranzakciók **nem** jelentenek szűk keresztmetszetet a mért workloadokon.

### 9.6 Talált és javított hibák

1. **Split abort a migration hysteresis sávban lévő resident miatt** (súlyos
   robustness bug): a parent boundsán kívülre került (de még a parenthez
   tartozó) mob nem illeszkedett egyetlen childba sem → `unroutable-resident`
   abort, 200k mobnál **30 abort / 30 s** (a split soha nem tudott lefutni).
   Javítva: a pozíció a parent rectbe clampolva routolódik a tartalmazó
   childba (a children pontosan csempézik a parentet).
2. **Stale LOD tier gauge-ok a retired/tombstoned zónákon**: a világ-szintű
   tier riportok minden zone slotot összegeznek, így a szülő + gyerekek
   duplán számolódtak (320 zónánál ~2× túlszámolás), és az aborted split
   staged childjai (részleges transfer után) véglegesen felszívódtak.
   Javítva: gauge-reset minden retirement/tombstone úton (`RetireZone`,
   `AbortSplit`, `AbortMerge`).
3. **Benchmark harness hibák** (nem production): a per-zóna akkumulátor nem
   nőtt splitkor (heap corruption), a szintetikus világ örökölte a map base
   spawn pointjait (+9 mob), a spawn-sugár = wander-leash miatt a korai
   workload degenerált volt, és az új stage-számlálók exchange-e hiányzott.

### 9.7 Következtetés — Phase 5 javaslat

A mérés alapján a Phase 5 a **ghost/replikáció/AOI skálázás** köré
szerveződjön:

1. **Inkrementális ghost kezelés**: ne épüljön újra a teljes ghost set
   tickenként; publish-buffer generáció/diff alapján csak a változások
   frissüljenek, a ghost entity-k újrahasznosítva (flecs entity churn
   megszüntetése). Cél: a ghost stage 77% → <20%.
2. **AOI candidate költség**: cap előtti candidate-szűrés olcsóbbá tétele
   (távolság-tier prefilter, inkrementális visibility), mert a dense p99 már
   most 6× a budget felett van.
3. **Activity-field query**: source-ok cellánkénti indexelése vagy cap, hogy a
   mob-hotspot LOD eval ne O(players-in-box) legyen mobonként.
4. **Combat fanout**: health/death payload coalescing a látható viewerek felé.
5. A stability controller (split/merge/cooldown) **mérten stabil**: a churn
   scenario 2 merge + 21 split mellett 0 oscillation trip és 0 emergency
   bypass; a suppression okok mérhetők.

A Phase 4 kódváltozásai: minimál instrumentáció (stage timings + diag
exchange), a fenti 2 production robustness fix, a szintetikus világ/spawn
seam, és a `--mode readiness` harness (`ReadinessBench.h/.cpp`).

---

## 10. Phase 5A — Inkrementális ghost karbantartás (a §9.7/1 megvalósítása)

> Cél (a Phase 4 bottleneck rangsor #1): a ghost stage **77% → <20%**.
> A ghost modell és a publikus viselkedés változatlan: a ghost read-only,
> nem-autoritatív, NetId-stabil, és a migrációs ownership-commit pont
> érintetlen.

### 10.1 Audit — mi volt a költség

A Phase 4-es `GhostSystem::Rebuild` minden tickben **minden** ghostot eldobott
és flecs entity-ként újra létrehozott a szomszédok publish-buffereiből (spread:
97M ghost-entity művelet / 30 s), a `BorderPublisher` pedig minden tickben a
zóna **összes** residentjét újra-skennelte a border-band vizsgálathoz. Mindkettő
a player-bearing zónák számával és a border-band entitásszámmal skálázódott.

### 10.2 Design — dirty publish + delta + KEEP/ADD/REMOVE reconcile

**Publisher oldal** (`BorderPublisher`, `Zone`):

- A published mezőket (position, heading, move_state, hp) író pathok
  dirty-jelölést adnak: movement (>1 cm elmozdulás), combat HP-vesztés, player
  move-intent, és **AI intent-váltás** (arrival-stop / blokkolt lépés is
  változtat move_state/heading-et, elmozdulás nélkül).
- `Publish()` csak a dirty entitásokat dolgozza fel (`UpdateDirtyEntities`);
  full refill csak spawn/despawn/transfer (`entity_set_generation_`),
  20 tickenkénti öngyógyító refresh, vagy explicit repair esetén fut.
- A publish-generation **csak akkor lép**, ha a tartalom tényleg változott
  (`SameBorderSnapshot`, egzakt, sorrend-független összevetés).
- **Delta-publish**: a legfrissebb generációhoz tartozó változott/hozzáadott és
  törölt net-id lista (`publish_delta_nets_` / `publish_delta_removed_`). A
  full refill invalidálja a deltát.

**Consumer oldal** (`GhostSystem::Reconcile`):

- Neighborenkénti cursor (`publish_generation`); a változatlan generációjú
  neighbor teljesen kimarad (fast path).
- Ha a generáció pontosan +1 és a delta érvényes: **csak a delta** alapján
  upsert/remove (a nem érintett ghostok stamp nélkül maradnak és a cursor
  szerint életben maradnak). Minden más esetben (gap, full-refill generáció,
  topology-váltás, lokális residency-váltás) teljes buffer-scan fallback.
- A generation **és** a delta együtt, a neighbor publish-mutexe alatt
  olvasódik (különben egy régebbi generáció rögzítésével egy újabb delta
  alkalmazható lenne, és a kimaradt generáció véglegesen elveszne — ez volt a
  legfontosabb talált race).
- Removal pass: a ghost életben marad, ha látta ez a reconcile, vagy ha a
  forrás-neighbor kimaradt; ha a forrás törölte, de egy **változatlan** neighbor
  még publikálja (migrációs tranziensek), re-attribúció történik **friss
  snapshot-tal**. Lokális resident soha nem maradhat ghost.
- Az attribúció unió-szemantikájú: egy net egyszerre két neighbor bufferében
  (migráció) egy ghost, bármelyik forrással.

### 10.3 Correctness — egzakt equivalence validator

`GhostValidator` (`--mode ghost`, illetve readiness `--ghost-shadow`):

1. **Publisher fidelity**: a publish-buffer == friss, authority-ból épített
   halmaz (set + mezők). Egy tick türelmet kap, ha a resident-set generation
   már változott de a publish még nem futott (spawn/transfer).
2. **Reconcile equivalence**: a ghost-set == a jelenlegi neighbor bufferekből
   számított egzakt unió (residentek kizárva, unió-szemantika). Tolerált,
   dokumentált lag: ≤2 publish-generáció, illetve egy tick topology-váltás
   után (a cursor-lista még a régi gráfot tükrözi) — ezek „skipped" számlálóba
   mennek, nem passzba.
3. **Repair seam**: hiba esetén (auto-repair ON) `RepairGhosts()` =
   `RebuildExact` minden simulating leafre + force publish. A production tick
   sosem hívja; a `--ghost-shadow` correctness futás szándékosan kikapcsolja,
   hogy a mérés őszinte legyen.

A validator a supervisor quiescent ablakában fut. ~300 zónánál a világ
telített lehet, ezért **audit drain gate** került a supervisor loopba: amíg
audit van függőben, nem indul új tick, amíg a repülő hullám ki nem ürül
(debug/bench only; a production path sosem kér auditot).

A fázis közben talált és javított hibák:

1. **Delta/generáció race** (generation lock nélkül, delta lock alatt) →
   kimaradó delta, tartósan stale ghost. Javítva: atomi generation+delta
   olvasás a publish-mutex alatt.
2. **Removal-pass duplicate loss**: a forrás által törölt ghostot a pass
   eldobta, pedig egy változatlan neighbor még publikálta (migrációs
   tranziensek) → re-attribúció + snapshot-frissítés.
3. **Re-attribúció stale snapshotja**: a másik neighbor snapshotja eddig nem
   másolódott át.
4. **AI intent-váltás dirty-jelölése**: move_state/heading elmozdulás nélkül is
   változhat (arrival-stop, blokkolt lépés).
5. **Audit-window éhezés** telített világban (a fenti drain gate).

### 10.4 Eredmények — 500 player / 200k mob, warmup 60 s, measure 30 s

| scenario | zónák (4→5A) | tick avg (4→5A) | p99 (4→5A) | domináns stage (4→5A) |
|---|---|---|---|---|
| spread | 320 → 236 | 1.97 → **1.67 ms** | 7.01 → 7.31 | ghost **77.3% → 16.2%**, repl 8.2% |
| hotspot | 280 → 280/244 | 2.55 → **1.93/1.71 ms** | 17.28 → **11.38/8.99** | ghost **54.2% → 3.4/5.9%**, repl 51.6–56.4% |
| dense | 68 → 76 | 8.73 → **4.06 ms** | **294.94 → 82.24** | ghost **7.1%**, repl 65.8% |
| replication | 100 → 68 | 3.67 → 3.64 ms | **57.58 → 51.32** | ghost **2.7%**, repl 85.8% |

- A ghost stage mindenhol a **<20% cél alatt** van (spread 16.2% a legrosszabb;
  a ghost stage az activity publish-ot is tartalmazza).
- Ghost munka (spread): `ops=168k` add/remove/move a Phase 4-es 97M helyett,
  `keep=14.6M` (nagy része a delta-publikációból), publish 10.8 s + reconcile
  18.5 s / 30 s.
- A domináns bottleneck átkerült a **replikációra/AOI-ra** (51–86% a
  hotspot/dense/replication scenariókban) — ez a Phase 5B célja a Phase 4
  rangsor szerint.
- **Correctness futás** (`--ghost-shadow`, spread 500p/200k): 15/15 poll,
  0 validator failure, 0 equivalence failure, 0 repair; a világ-validáció OK.
  `--mode ghost` szcenárió: 0 failure, `max_copies=1`.
- **Regresszió**: a 3 selftest + 7 bench mód + routing selftest zöld.

### 10.5 Következő lépcső (Phase 5B javaslat)

A Phase 4 rangsor 2–5. pontjai változatlanok és most **ezek a dominánsak**:
AOI candidate-szűrés (dense p99), activity-field query O(players-in-box),
combat fanout coalescing. A ghost oldalon opcionális továbbfejlesztés a
`keep`-scan teljes elhagyása (per-source slot-lista), de a mért 16.2% mellett
ez már nem kritikus.
