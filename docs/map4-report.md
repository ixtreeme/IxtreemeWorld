# MAP-4 — warp/spawn és végső map-layer acceptance

Dátum: 2026-09-27. Ág: `With_Auriga`; HEAD: `ab2458de`.
A MAP-2, MAP-3 és MAP-4 változásai a munkakönyvtárban vannak; nincs új
commit vagy push. A MAP-4 előtti forrásdiff, az untracked források másolata,
SHA-256 lenyomatai és a hozzájuk tartozó RelWithDebInfo `worldbench`:
`build/map4-baseline/`. Ez a dirty MAP-3 baseline, nem pusztán a HEAD.

## 1. Implementáció

### Warp (R6)

- `WarpState`: csomagon belüli triggerazonosító, arming/cooldown/pending
  állapot és számlálók az authoritative entitáson.
- Siker után 1 s szimulációs cooldown; újraélesítés csak minden triggeren
  kívül, a cooldown lejárta után. A triggerbe spawnolás belépésnek számít.
- A nem ciklikus lánc céljában álló entitás nem teleportál tovább. A loader
  továbbra is elutasítja a ciklust/önhurkot, láncra warningot ad. A korábbi
  kompatibilitási döntés folytatása; az eredeti R6 szigorúbb céltilalmának
  eltérése explicit a formátumdokumentum 13. pontjában.
- Entitásonként egy függő kérés, legfeljebb 5 s authoritative szimulációs
  idő. Kilépés/forrásváltás/despawn megszünteti. Nincs entitást vagy zónát
  tartó I/O callback, végtelen újrapróba vagy WARN/tick.
- A cella, magasság és célzóna ellenőrzése megelőzi a pozícióváltást.
  Missing adat: várakozás. Blocked/invalid/outside: egyszeri elutasítás.
- A meglévő migrációs sor és tranzakció kezeli a nem szomszédos zónát is;
  warp esetén nincs mozgási hysteresis. Átadásig a forrás rögzíti a
  célpozíciót. `EntityTransfer` őrzi a triggerállapotot split/merge alatt is.
- A meglévő terrain-metrikákhoz került success/refused/cancelled/timeout és
  az összesített, szimulációs másodpercben mért warp-várakozás.

### Spawn (R13)

- Manifest v3, `mobSpawns` réteg v2: kötelező, egyedi, nem nulla u32
  `spawn_id` és `area_id` (0 = világ; másként létező, a középpontot
  tartalmazó area). A mob-típust továbbra is a szerver registry ellenőrzi.
- Régi v1 elfogadott: immutábilis csomagon belüli rekord-sorszám az id,
  area=0. Kommentbeszúrás nem változtatja; átrendezés igen. Tartós
  szerkesztési identitáshoz explicit v2 konverzió szükséges.
- Az id átmegy `LoadedWorld` → `MobSpawnPoint` → `MobSpawnRef` →
  `EntityTransfer` útvonalon. A lokális respawn-index megmarad, nem DB-id.
- A kezdeti aktiválás számon tartja a sikeresen létrehozott példányokat;
  ismételt hívása csak a még hiányzókat próbálja. A respawn külön meglévő
  út. Chunk load/reload/eviction és split/merge nem új spawn-esemény.
- Nincs a meglévő `Client/assets/Maps/test_zone`-t átíró konverzió.

### A mérés közben talált hiba

`PostTerrainDemand` korábban sugártól és chunkmérettől függő float lépésekkel
mintázta a tartományt. 1 m-es sugárnál csak az `(x-1,y-1)` pontot kérhette;
ha a középpont chunkhatár másik oldalán volt, a kért hely nem töltődött be.
A javítás a világra metszett doboz teljes egész chunk-index tartományát
járja be. A `demand-includes-centre-across-seams` regressziós teszt két
egymást metsző chunkhatárnál ellenőrzi a középpontot és az ellenoldalt.

Második hiba: négy workerrel, 500 player / 200k mob dense setup alatt a
mentett MAP-3 **és** a MAP-4 köztes binárisa is 30 s snapshot-timeouttal
leállt. A folyamatosan újra kiadott tickek mellett nem garantált a spontán
világszintű csendes ablak. A supervisor most függő snapshot-kérésnél nem
ad ki új tickhullámot, amíg a már kiadott munkák le nem futnak. Kiszolgálás
után legalább egy hullámot mindig enged, akkor is, ha közben új olvasó
érkezett; a szimulációt sem éheztetheti egy folyamatos olvasósor.
Az ASF mérési/control állapota nem törlődik és nem íródik át.

Ugyanennek a 500/200k, 4 worker setupnak a célzott ismétlése a javítással
sikeres: 500 játékos a világban, snapshot 82.5 ms, világvalidáció OK.
Ez 2 s warmup / 3 s measure hibareprodukciós próba, nem a reprezentatív
60/30-as performance run helyettesítője (`snapshot-saturation.txt`).

## 2. Tesztelés és bizonyíték

A nyers logok és a reprodukáló script eredményei: `build/map4-results/`.
A végső buildlogok: `build/map4-final-build.log`, `build/map4-final-debug-build.log`.

- MAP-4 célzott tesztek: 36/36, RelWithDebInfo és Debug/assert alatt.
- Package parser, terep, split/merge, streaming, query, lifetime, mapaudit,
  snapshot és bootstrap: külön eredményfájlok, a végső összegezés alább.
- Valódi `gameserver` startup más munkakönyvtárból, helyi DB-vel és
  listenerrel: 29/29. Új esetek: v2 spawn pozitív út és hibás area
  elutasítása DB/listener előtt. Log: `build/map4-results/startup.txt`;
  konfiguráció/fixture/egyedi log: `build/map4-acceptance/`.
- A `mapaudit` megőrzi a legacy reprodukciókat, és ténylegesen lefuttatja
  a MAP-4 pozitív/negatív teszteket a runtime státusz megállapításához.
  Az R1–R13 részletes kód- és teszthivatkozásai a követelménymátrixban.

| Ellenőrzés | RelWithDebInfo | Debug/assert |
|---|---|---|
| MAP-4 | 36/36 | 36/36 |
| worldpackage | 119/119, 1 kihagyott | 119/119, 1 kihagyott |
| terrain | 20/20 | 20/20 |
| mapsplit | 21/21 | nem futott ebben a körben |
| streaming | 27/27 | 27/27 |
| worldquery | 27/27 | 27/27 |
| streamlife | 10/10 | 10/10 |
| mapaudit | 14 CHANGED, 0 open/partial | nem futott ebben a körben |
| snapshot / bootstrap | 0 failure / 4/4 | nem futott ebben a körben |
| valódi startup | 29/29 | nem futott ebben a körben |
| 60 s streamsoak, 16 MiB / 3 MiB | 3/3 és 3/3 | nem futott ebben a körben |

A worldpackage kihagyott esete: directory-symlink escape. A környezet nem
engedett directory symlink létrehozását; ez **SKIPPED**, nem PASS.
A `..` útvonal-escape negatív teszt lefutott, de nem helyettesíti a symlinket.

Egy párhuzamos RelWithDebInfo/Debug MAP-4 futás a közös temp fixture
írásán ütközött (32/33); az elkülönített Debug ismétlés 33/33. A fixture-t
használó azonos tesztmódok sorban futtatandók. A végső mob-identity teszt
a mobokat ténylegesen tartalmazó levélen is végrehajtja a split/merge-t.

A teljes regressziós script 27 futásából 26 exit=0. Az egyetlen eltérés a
`scheduler` `uniform-parallelism` faliórás küszöbe: a kerekített érték 2.00,
az elvárás szigorúan >2.0; 12 másik scheduler-ellenőrzés átment. Ez a
MAP-3 riportban már ismert ingadozó teszt. Nem lett lazítva az elvárása,
az első futás hibának marad jelölve. Az elkülönített baseline/új ismétlés
eredménye a méréseknél szerepel.

A többi futás: field/loadfield/partitionscore selftest, routing, lod,
activity, loadfield, partitionscore, stability, splitmerge, ghost, aoi,
replication, tickrate, inputpath, netstress, presence, asfdeterminism,
workerpool, replv2, protocol, hygiene, reclamation 100/1000 és a két soak.

A végső snapshot-éhezés javítása után újrafutott: MAP-4 33/33; snapshot,
tickrate, asfdeterminism, workerpool, streamlife és reclamation mind exit=0.
Debugban a MAP-4 soros ismétlése 33/33, a 100 ciklusos snapshot teszt exit=0.
A scheduler újabb, elkülönített futása is 13/13. Az előző teljes regresszió
és a szűkített végső ellenőrzés külön logban marad, nem egyetlen futásnak
feltüntetve (`snapshot-fix-summary.txt`, `snapshot-debug-*.txt`).

A végső tesztbővítés külön lefutott mindkét builden: **36/36**
(`final-map4-36.txt`, `final-debug-map4-36.txt`). Új explicit esetek:
hiányzó area, u32 spawn-id overflow, despawn pending warp közben és késői
terrain-publication után azonos net-id újrafelhasználása. A pozitív v2
fixture immár tényleges, nem nulla area-referenciát is ellenőriz.
A runtime viselkedése a nagy readiness mérésekhez képest nem változott;
az utolsó módosítások tesztlefedettséget és bench-hibadiagnosztikát adnak.

A MAP-4 tesztek érintik a missing/dupla/nulla spawnazonosítót, rossz areát,
areán kívüli középpontot, ismeretlen mob-típust, world boundsot, v1/v2
átmenetet, loader-ciklust, láncot/rearmot, pending lemondást/időtúllépést,
késői rezidenciát, blocked/outside célpontot, fél-nyitott forráshatárt,
nem szomszédos zónák közti migrációt 1 és 4 workerrel, ASF split/merge-t,
spawn-id megőrzést és idempotens kezdeti aktiválást.

A további streaming race/failure esetek a meglévő `streaming`, `streamlife`
és `worldquery` módokban maradnak: késleltetett és sorrendcserélt I/O,
generáció, cancellation, pin/eviction, shutdown, reclaim/slot-reuse,
független magasság-oracle és eager/streamed queryazonosság. Nem helyettesíti
ezeket a MAP-4 tesztcsoport vagy a `CHANGED` címke.

## 3. Mérések

### Scheduler és streaming soak

Az elkülönített scheduler pár: mentett MAP-3 **12/13** (ezúttal
`reference-parallelism`), MAP-4 **13/13**. A wall-clock küszöbök ingadozása
a baseline-on is látható. A korábbi hibás futás nem lett átminősítve.

| 100 km-es soak, 200 player + 3200 mob, 60 s | 16 MiB budget | 3 MiB budget |
|---|---|---|
| PASS | 3/3 | 3/3 |
| Betöltés / eviction | 1182 / 814 | 754 / 757 |
| Csúcs saját könyvelés | 14.748 MiB | 2.990 MiB |
| Adatra váró lépések | 0 | 509164 (35.18%) |
| Invalid adat miatti elutasítás | 0 | 0 |
| Miss p99 | 6.26 ms | 3005.64 ms |

A 3 MiB eset a munkakészlet alatti keret degradációját mutatja: az adat
helyessége és a memóriahatár tart, a késleltetés jelentősen nő.
A 16 MiB-os futásban 1160 aktívterület-áthelyezés történt. A záró független
eager height-oracle mind a 3400 entitás magasságát egyezőnek találta.
Ez valódi C kategóriájú streaming-bizonyíték ezen a kisebb populáción;
nem helyettesíti a 200k mobos cache-korlát eredményét.

### Nagy readiness futások

A readiness CSV és a futásonkénti részletes log a
`build/map4-results/readiness-final/` könyvtárban található. A korábbi,
snapshot-timeoutos baseline/köztes próbák a `readiness/` könyvtárban,
illetve a `baseline-spread.txt` fájlban maradtak meg. A négy workeres
baseline dense és spread futás sem jutott el a warmupig; ezekből nincs
érvényes tick-overhead A/B szám. A kis eager/streamed pár ettől független.

Az alábbi sikeres futások 4 workerrel, seed=20260922 mellett, 60 s warmup /
30 s kért mérési ablakkal készültek. A file-backed cache kerete 128 MiB.
Mindegyik világvalidációja OK, a fájlos esetek tényleges játékospozíciói
egyeznek a kért eloszlással (nincs default spawnra eső rejtett fallback).

| Futás | Player / mob | Tick avg / p50 / p95 / p99 / max (ms) | Végső zónaszám | Process working set (MiB) | Teljes elapsed (s) |
|---|---|---|---|---|---|
| Synthetic dense | 500 / 200k | 2.572 / 2.487 / 3.044 / 60.535 / 82.625 | 88 | 2155.0 | 94.87 |
| File dense | 500 / 200k | 2.905 / 2.813 / 3.751 / 64.791 / 96.086 | 136 | 2582.0 | 143.49 |
| Synthetic spread | 7000 / 200k | 1.367 / 0.798 / 3.039 / 3.715 / 5.731 | 148 | 4399.1 | 99.69 |
| File spread | 7000 / 200k | 1.038 / 0.700 / 3.601 / 4.937 / 11.043 | 312 | 6084.7 | 201.91 |

A különböző magasságmező és kialakult topológia miatt a synthetic/file
sorokból nem számolható tiszta map-overhead. A dense p99 50 ms feletti:
a helyességi PASS nem garantálja, hogy minden zóna minden tickje 50 ms alatt fut.

| File futás | Mérési idő (s) | Betöltés / eviction / miss a mérésben | Várakozó lépés / invalid | Terrain peak / keret (MiB) | Replication frame MB/s |
|---|---|---|---|---|---|
| dense | 30.72 | 0 / 0 / 0 | 0 / 0 | 128.00 / 128 | 6.09 |
| spread | 30.63 | 398 / 0 / 398 | 442 / 0 | 128.00 / 128 | 26.35 |

A dense futás B kategóriájú integrációs/rezidens terhelés. A spread
6,638,640 byte-ot olvasott a mérési ablakban, de eviction nélkül ez sem
teljes C bizonyíték. A `miss_p99_ms` a streamer legutóbbi mintáinak ablaka,
nem a bench mérési ablakára visszaállított hisztogram: a dense 27,268.93 ms
értéke korábbi setup-missből maradt meg, amikor a mérésben már nem volt miss.
A spread végső recent miss p99 értéke 7.72 ms.

A 100 km-es fájlos világ generálása + loader/runtime építése 12.16–12.47 s;
ebből a production loader/runtime építése 1164.70–1187.85 ms. A loader
708,596 byte-ot olvasott startupkor, és ellenőrizte mind a 9604 chunkfájl
indexét/létezését. A többi adatot az aktiválás/streamer tölti.

### Kis, azonos csomagos eager/streamed pár

4 km-es világ, 2×2 régió, 100 player / 3000 mob, ASF off, 4 worker,
azonos alapértelmezett seed, 10 s warmup / 15 s measure. Mindkét futás PASS.

| Mód | Tick avg / p50 / p95 / p99 / max (ms) | Working set (MiB) | Frame MB/s | Elapsed (s) |
|---|---|---|---|---|
| eager | 4.141 / 1.701 / 10.517 / 25.334 / 27.919 | 63.6 | 0.41 | 25.35 |
| streamed, 3 MiB | 3.699 / 1.367 / 9.890 / 24.670 / 28.128 | 63.2 | 0.41 | 25.42 |

A streamed csúcs 0.24 MiB; a mérésben 0 load/evict/miss/wait. Ez a rezidens
queryút összevetése, nem hideg I/O mérés. Egyetlen párból nincs megalapozott
gyorsulási állítás; a maximum sem javult minden metrikában.

### Kis cache melletti nagy terhelés: nem teljesült elfogadási esetek

500 player / 200k mob, 100 km, 4 worker, 16 MiB keret mellett a moving,
hotspot és border futások az első játékosok előkészítésében elérték a
15 s-os célterep-várakozási korlátot. Mindhárom exit=2, `spawn-terrain-ready:
FAIL`; elapsed rendre 51.76 / 43.87 / 32.23 s. Nem jutottak el a kért
60 s warmup / 30 s measure ablakig, ezért ezekhez nincs érvényes tick-tábla.

A kezdetben aktív mobok világméretű igénye a rendelkezésre álló cache-nél
nagyobb, a FIFO admission nem garantálja egy új játékos kérésének időbeni
kiszolgálását. Ez a MAP-3-ban előre dokumentált éhezésmentességi korlát
konkrét reprodukciója. Nem lett megnövelve a 15 s-os timeout, sem
átminősítve a három eredmény PASS-ra. A végső bench hiba esetén kiírja a
célkoordinátát, cache/queue/budget/admission állapotot, és relocation közben
az első sikertelen kérés után megszakítja az akciót, nem vár 500×15 s-ot.

A végső diagnosztikai ismétlés (`final-cache-limit.txt`) ugyanezt a hibát
adja: cél `(29972.04, 29689.62)`, 896 rezidens chunk, 4096 várakozó kérés,
0 loading, 0 failed, 15.97 MiB könyvelt memória a 16 MiB keretből, 2,554,286
admission-reject. A betöltési sor/cache telített; a mérés nem sérült fájlra
vagy túlfutó memóriafoglalásra utal. Az ismétlés is exit=2.

A `readiness-capacity` suite külön 128 MiB-os moving/hotspot/border
kapacitáspróba. Eredménye nem helyettesítheti a 16 MiB-os elfogadást.
A 128 MiB-os moving lefutott, a három áthelyezés pozícióellenőrzése és a
világvalidáció sikeres. A mérési 30.43 s-ban 7 load, 7 miss, 9 várakozó
lépés, 0 invalid, de **0 eviction** történt; ezért a `real-streaming-churn`
ellenőrzés FAIL, a teljes run exit=2 marad. A tágabb cache itt a mozgó
terhelés integrációját mutatja, a streaming-churn követelményét nem.

| 128 MiB-os kapacitáspróba, 500 player / 200k mob | Tick avg / p50 / p95 / p99 / max (ms) | Working set (MiB) | Elapsed (s) | Eredmény |
|---|---|---|---|---|
| moving | 2.213 / 0.885 / 6.918 / 11.942 / 73.344 | 2491.1 | 141.82 | FAIL: nincs eviction |
| hotspot | 2.585 / 1.229 / 7.260 / 12.974 / 64.025 | 2377.5 | 150.75 | PASS, B kategória |
| border | 1.576 / 0.983 / 3.727 / 5.333 / 14.974 | 2136.1 | 140.27 | PASS, B kategória |

A hotspot és border mérési 30.43 s-os ablakában 0 load/evict/miss/wait/invalid,
128 MiB lifetime terrain peak volt. A border két kohorsza zónahatár és
chunkhatár előtt 2 méterrel indul, kelet/nyugat irányú mozgást kap. A
játékosok induló pozíciója és a végső világállapot validált; a teszt nem
állítja, hogy a rezidens határátlépés hideg chunk-I/O-t kényszerített ki.

### Mérési szerződés

- A: változatlan synthetic világ, mentett MAP-3 vs MAP-4, azonos seed,
  konfiguráció és 4 worker; 60 s warmup / 30 s measure.
- B: production loaderrel betöltött nem lapos világ, valódi startup és
  query/movement; a nagy file-backed readiness külön kategória.
- C: 100 km × 100 km, 16 m cella, 64-cellás chunk, 9604 fájl; korlátozott
  cache, mozgó aktív területek, tényleges miss/load/evict. Warmup után
  rezidensen maradó run önmagában nem streaming-bizonyíték.
- Eager/streamed A/B: azonos kis csomag, seed, workload, 4 worker, ASF off;
  a synthetic flat és az új dombos világ különbsége nem tiszta overhead.
- Indulás: fixture-generálás + startup idő, valamint külön production
  loader/runtime építési idő. Az alkalmazás-cache hideg, az OS page-cache
  nincs szabályozva; a generálás frissen írta a fájlokat.
- Az alkalmazás által könyvelt memória és a process working set külön
  szerepel. A miss-percentilis nem tick-percentilis. A számlálók deltája
  ugyanahhoz a futáshoz és a riportolt mérési időalaphoz tartozik.
- Windows, MSVC 19.51, RelWithDebInfo; a benchmark 16 logikai processzort
  és 63.9 GiB RAM-ot jelent. A tényleges elapsed időt a CSV őrzi.
  CPU: Intel Core i7-10700K (8 mag / 16 szál). A temp fixture-ök a C:
  kötet SAMSUNG MZVLW512HMJP-000L7 NVMe meghajtóján vannak.

### Negatív eredmény

A fejlesztés közbeni, az állapotkép-éhezés javítása előtti rövid próba (100 player / 3000 mob, 2 s warmup,
12 s kért measure, 16 MiB cache) egy relocation célbetöltésnél elérte a
15 s-es bench-határt. A memória végig a kereten belül maradt, de a mobok
kezdeti, nagy területet érintő igénye miatt hosszú várólista alakult ki.
Ez a run nem PASS és nem a 60 s warmupos reprezentatív mérés helyettesítője.
Log: `readiness-file-smoke.txt`. A mért időablak a szinkron bench-várakozás
miatt hosszabb lett a kért 12 s-nál; a riport ezt külön kiírja.

## 4. Reprodukálás

```powershell
cmake --build gameserver/build/windows-relwithdebinfo --config RelWithDebInfo --target worldbench gameserver -j 6
cmake -S gameserver -B gameserver/build/windows-debug -DENABLE_WORLDBENCH=ON
cmake --build gameserver/build/windows-debug --config Debug --target worldbench -j 4

gameserver/build/windows-relwithdebinfo/apps/gameserver/RelWithDebInfo/worldbench.exe --mode map4
gameserver/scripts/map4_regression.ps1 -Bench gameserver/build/windows-relwithdebinfo/apps/gameserver/RelWithDebInfo/worldbench.exe -OutDir build/map4-results/regression -Suite regression
gameserver/scripts/map4_regression.ps1 -Bench gameserver/build/windows-relwithdebinfo/apps/gameserver/RelWithDebInfo/worldbench.exe -OutDir build/map4-results/readiness-final -Suite readiness -Baseline ''
gameserver/scripts/map4_regression.ps1 -Bench gameserver/build/windows-relwithdebinfo/apps/gameserver/RelWithDebInfo/worldbench.exe -OutDir build/map4-results/readiness-capacity -Suite readiness-capacity -Baseline ''
gameserver/scripts/map4_regression.ps1 -Bench gameserver/build/windows-debug/apps/gameserver/Debug/worldbench.exe -OutDir build/map4-results/debug -Suite debug
```

A startup script platformfüggetlen Bash: a fejlécben felsorolt abszolút
útvonalakkal, külön output könyvtárral futtatandó. Az output könyvtárat
törli és újragenerálja; nem lehet projekt- vagy meglévő világkönyvtár.

## 5. Korlátok és következő kapu

A warp/spawn implementáció és a futtatható acceptance-csomag elkészült.
A teljes nagyterhelésű elfogadási mátrix **nem teljesen zöld**: a 16 MiB-os,
200k mobos indulási éhezés és a 128 MiB-os moving eviction-hiánya nyitott
mérési korlát. Ezek lezárásához külön workload/cache méretezési vagy
admission-prioritási döntés és azonos parancsos ismétlés szükséges.

- Linux/FreeBSD és ASan/TSan nem futott. A Windows Debug/assert eredmény
  nem helyettesíti ezeket. A forrás nem vezet be platformfüggő runtime I/O-t.
- Cache < munkakészlet esetén nincs éhezésmentességi garancia vagy
  player-first prioritás. A budget túllépése helyett várakozás/elutasítás
  történik; az explicit bench-timeout hibának látszik.
- A globális futásidejű ASF zónabudget, finom gridhez szükséges nagyobb
  manifest/index, navmesh és gameplay-navigáció a korábban dokumentált
  külön döntések maradnak. A slope/deep-water szabály alapból továbbra is ki.
- A v2 spawnformátumot a szerver és a shared writer/loader kezeli. A legacy
  kliens nincs migrálva; v1 tartalmát nem írtuk át. Live csomagcsere és
  respawn/DB/gameplay újratervezés nem történt.
- Következő lehetséges külön feladat: a tényleges Metin kliens packet,
  session és map betöltési útjának compatibility auditja, közös koordináta-
  és map-adat szerződés, admission tesztút, entity/movement/spawn/despawn
  adapter. A szerver `0x11` protokollja önmagában nem bizonyítja a kliens
  kompatibilitását. Ez a MAP-4 nem kezdett kliens- vagy gameplay-munkát.
