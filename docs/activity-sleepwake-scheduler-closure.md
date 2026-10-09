# TC-0 → TC-3: activity/sleep-wake és scheduler lezárás

**TC-0 → TC-3 végrehajtva, 2026-09-27. Integrált verdikt: PARTIAL, STOP + review.**
Az elfogadott activity-fázisszerződés javítása és az új scheduler-ellenőrzések
bizonyítottak; a teljes C-moving elfogadás nem teljesült. Commit/push nincs.

## 1. Kiindulás és reprodukció

A kiindulás a `With_Auriga` branch `ab2458dee6aa10ef488aa39fd85f1b67b480e2fa`
HEAD-je fölötti, SL-2 végső N2 dirty forrás. A TC indulásakor minden nem-MD N2
forrás hash-e egyezett; négy dokumentum tért el. A teljes jelenlegi forrás,
config, CMake-cache, dirty diff, release/Debug bináris és a védett test_zone
21 fájljának hash-e külön mentést kapott:
`build/tc-20260927-184106/baseline/`. A korábbi SL/MAP bizonyítékok változatlanok.

Az eredeti `worldbench --mode scheduler --seconds 3` a mentett N2-n ismét
**exit 2, 12 PASS / 1 FAIL**: `reference-parallelism`. A referencia 1 workerrel
0,40, 15 workerrel 0,98 „parallelism”; mindkettő 3840 kiadott feladat. Az
eredeti SL-2 11/2 scheduler-futás ettől külön történeti FAIL marad.

A változatlan B1+C kontroll ismét **exit 2, 13/1**:
`activity: sleeping zone 281 holds net 1163428 that is brute-force Reduced`.
A mentett N2 az azonos C-trace-en **exit 0, 14/0**. Az eredeti két B1-C FAIL és
az N2 két PASS nem íródott át. Ezek az új kontrollok önmagukban nem bizonyítják
egy adott történeti audit fázissorrendjét.

Az observation-only N2 kis reprodukciójában a valódi SpawnSystem,
ActivityPublisher, SpatialActivityField, ZoneScheduler és MovementSystem út
szerepel. Várakozás nélkül látható: commit után a régi field Dormant, current
brute force Reduced; forráspublikálás után is régi a field; új field után is
Sleeping a zóna a következő scheduling döntésig. A mezőgeneráció és az aktuális
állapot összehasonlítása ezért külön időbeli szerződést igényel.

| Megállapítás | Bizonyíték / minősítés |
|---|---|
| Az eredeti B1-C sleeping/Reduced tünet újra előjött | Új eredeti parancsos futás, exit 2; konkrét NetId/ZoneId csak diagnosztikai adat. |
| A régi field/publisher/schedule/current audit eltérő fázisai előállíthatók | Determinisztikus N2 observation-only reprodukció, 17/0; a teszt PASS-ja a hibajelenség reprodukálását jelenti. |
| Az N2 nem biztosítja a most jóváhagyott első-fázis wake-et | Régi mezővel az új hatás nem ébreszt; source publish és field publish külön későbbi lépés. Ez az új célhoz képesti hiány, nem utólag állított régi garancia. |
| A current brute-force és régi immutable mező összevetése lehet időben hibás oracle | Ugyanaz a régi generáció saját source-aival helyes, a current pozícióval eltér; az új két oracle ezt külön kezeli. |
| A két eredeti SL-2 B1-C failure pontos belső sorrendje | Nem rekonstruálható teljes bizonyossággal: azokban nem volt commit/cut/decision trace. A bizonyított hibacsaládot nem nevezzük mindkét történeti run egzakt eseménysorának. |
| A legacy occupancy küszöb dispatch-képességet mér | Cáfolt a képlet és azonos feladatdarab mellett különböző callback-foglaltság alapján; a gate-teszt külön bizonyítja a production párhuzamos belépést. |

## 2. Jóváhagyott időbeli szerződés

A felhasználó a TC közben kifejezetten jóváhagyta az első következő supervisor
ütemezési fázis wake/runnable szerződését. Ez **új célviselkedés**, nem állítás
arról, hogy N2 már garantálta. A jóváhagyás összefoglalója az evidence gyökér
`approved-contract.md` fájlja.

| Átmenet | Kezdő / záró esemény és követelmény |
|---|---|
| Commit → wake publikáció | Player spawn/transfer binding commit, illetve a valódi movement/warp Position commit és a publikált pozíció ugyanazon zóna activity-mutexének kritikus szakaszában történik. Nincs várakozás az 1 Hz-es builderre. |
| Publikáció → fázisbemenet | A scheduler a publikációs mutexeket stabil slot-sorrendben megfogja. Az utolsó lock megszerzése a vágópont. A korábbi commitok ebben, a későbbiek a következő fázisban kötelezőek. |
| Fázisbemenet → wake | Ugyanabban a ScheduleOnce-ban a pozitív hatású, mobot tartalmazó leaf Active/runnable lesz. A nem teljesített döntés önálló hiba. |
| Wake → végrehajtás | NextTick megmarad; nincs előrehozott, 50 ms dt-jű extra tick. A production CAS és ZoneWriteGuard változatlanul védi a munkát. |
| Periodikus field | Az 1 Hz-es immutable field és a LOD evaluation/demotion szabályai megmaradnak. A wake-döntés nem teljes field-rebuild és nem azonnali mob-tier átírás. |
| Audit | Quiescent current authority/publisher ellenőrzés + a már lezárt scheduling bemenetéből kötelező wake. A későbbi commit pending; az audit nem publikál, ébreszt vagy ütemez. |
| Elmaradt wake | Régi döntési generáció vagy a lezárt fázisban pozitív hatás ellenére Sleeping resident: FAIL. Újabb pozíciók nem tüntetik el a már elmulasztott döntést. |

Az első még át nem vett frissítés steady-clock bélyege ismételt update során
megmarad. Az alvó zóna helyi óráját nem használjuk maximális wake-késéshez.
A coalescing a fázis bemeneteként a legutolsó authoritative pozíciót adja;
a fázis előtt megszűnt hatás nem tart mesterségesen minden zónát ébren.
Túlterhelésnél nincs feltétlen 50 ms completion-ígéret. Deadline:
`completion > original due + 50 ms`, változatlanul.

## 3. Scheduler: három külön állítás

Az eredeti számláló képlete `delta(sum worker work_micros) / (requested seconds * 1e6)`.
A számláló teljes callback-eltelt időket tartalmaz, a nevező a kért három
másodpercet, nem egy pontosan lezárt feladatlista eltelt idejét. Benne van a
20 Hz-es üres idő; a határon átnyúló feladat teljes költsége a befejezés oldali
mintához kerülhet. A task elapsed deschedulinget is tartalmazhat. A per-zone
256-os tick-ringek eltérő időablakúak. A kiírt két tizedesre kerekített szám
helyett az eredeti kód a teljes double értékre vizsgálja a `>2.0` és `>1.0`
feltételt. A `reference-work-equivalence` pedig a legterheltebb worker idejét,
nem a teljes hasznos munka azonosságát hasonlítja.

Egy 0,40 worker-eltelt-idő/falióra igényű, 20 Hz-re korlátozott munka nem
követel >1 folyamatos foglaltságot, akkor sem, ha több worker képes dispatchre.
A lassabb callback/descheduling akár növelheti ezt a mutatót. Ezért a régi
occupancy küszöb nem bizonyít dispatch-képességet vagy azonos munkán gyorsulást.
A régi parancs, küszöb és nyers exit megmarad; a történeti FAIL nem lesz PASS.

A végső T-n ugyanez az eredeti parancs **exit 2, 11 PASS / 2 FAIL**:
`uniform-parallelism` és `reference-parallelism`. Reference 1-worker 0,46,
15-worker 0,91 foglaltsági érték, mindkettő 3840 enqueue. Az új hard
correctness/dispatch és fixed-work feltétel külön PASS; a legacy runner
összesített exitje emiatt változatlanul nemnulla. A régi baseline-bukás
önmagában nem felmentés: a fenti képlet/workload elemzés és az új, ugyanazon
production queue-n végzett független teszt az értelmezésváltás bizonyítéka.

- **A: schedulercontract** — valódi ScheduleOnce/FIFO pool/ZoneWriteGuard,
  stabil ZoneId+original due azonosság, egyszeri output, ismételt due alatt
  megmaradó claim, shutdown előtti drain, sleep és retired kizárás,
  split/merge/reclaim/slot-incarnation.
- **B: schedulercontract** — külső vezérlő által oldott bounded gate;
  1/2/4 workerhez külön egyidejű belépési elvárás. A gate-várakozás nem CPU-work.
- **C: schedulerwork v1** — 64 determinisztikus, független feladat, egyenként
  1 millió számítás, 64 millió összes művelet, checksum
  `15990358326065361963`. 1/2/4 worker, konfigurációnként hét váltott N2/új pár.
  A teljes előre befagyasztott szerződés: `scheduler-work-contract.json`.

A mikromérés a ScheduleOnce előtti monotonic ponttól az utolsó callback
befejezéséig tart. Előkészítés, referencia-output, pool-start, join és audit
kívül esik. Az összes 64 item és a késői completion is beleszámít. Per-item
callback-időpontokat és enqueue ELŐTTI hook-bélyeget írunk; az utóbbi nem az
enqueue tényleges időpontja. Az egzakt due→enqueue, enqueue→worker-start,
due→start és completion deadline számlálók a production poolból származnak.
Valódi callback CPU a Windows GetThreadTimes alapján, a rendszer accounting
felbontásával; nem a task elapsed összegének átnevezése. Nullamintás metrika N/A.

## 4. Változtatások

Runtime: az activity forráspublikáció összekötése a player committal; bounded,
aktuális source/leaf méretű scheduling input; változáskor a meglévő partition
tree pozitív hatás szerinti bejárása; döntési generáció és bounded időszámlálók.
Nincs globális mob×player scan a production tickben, új streaming policy,
worker affinity, work stealing vagy új ownership-rendszer.

Validátor: külön immutable generation-input oracle és current authority /
lezárt scheduling-fázis wake oracle. A régi fieldben még szereplő, azóta
despawnolt forrás történeti helyességét nem a mai playerlistából ítéljük meg;
a friss publisherből viszont a despawn után hiányoznia kell.

Harness: két külön scheduler mód, deterministic temporal probe/contract,
negatív kontrollok, külön readiness activity-verdict és fázisazonosítók.
Részletes, kizárólag TC-módosítások: a végső freeze `TC-only.diff` fájlja.

| Fájlok a `gameserver/apps/gameserver/` alatt | Konkrét változás |
|---|---|
| `src/world/zone/Zone.{h,cpp}`, `src/world/spawn/SpawnSystem.cpp`, `src/world/systems/MovementSystem.cpp` | Player binding/pozíció commit és source-publikáció közös activity-lock alatt; spawn indexelés a binding előtt; despawn/transfer eltávolítja a forrást; sleeping-clear nem törölhet post-cut megjelent playert. A mob movement változatlan. |
| `src/world/activity/ActivityTypes.h`, `SpatialActivityField.{h,cpp}` | Commit/első pending időbélyeg, revision; immutable generáció eredeti source-inputjának megtartása; publisher nem törli a commit-azonosságot. |
| `src/world/zone/ZoneScheduler.{h,cpp}` | Koherens input-cut, revision szerinti újrafelhasználás, existing partition tree szerinti pozitív leaf-érintettség, döntési generáció. A due/CAS/queue algoritmus változatlan. |
| `src/world/zone/ZoneWorkerPool.{h,cpp}` és scheduler számlálók | Párhuzamosan olvasott mérési számlálók atomikusak. A live többmezős snapshot továbbra sem tranzakcionális; a lezárt, drainelt mikromérés egzakt. |
| `src/world/debug/WorldValidator.{h,cpp}`, `src/world/WorldRuntime.{h,cpp}` | Két külön oracle, immutable generation/wake keret a quiescent snapshotban; auditból nincs capture/rebuild/wake. |
| `src/bench/ActivityTemporalBench.cpp`, `ClosureBench.{h,cpp}`, `WorldBench.cpp`, `ReadinessBench.cpp`, `CMakeLists.txt` | Új tesztmódok és célzott mérés; meglévő CLI-k, C-trace, cache és legacy küszöbök megmaradnak. |

A forráskeret aktuális playerek/zónák számával korlátos; nincs tartós
eseménylista vagy tárolt entity/Zone pointer. A lockok stabil slot-sorrendűek,
a writer csak saját zónájának activity-lockját fogja; a topology tulajdonosa
a supervisor. A régi mező átmeneti pozitív hatása továbbra is megtarthat egy
zónát ébren; a friss pozitív hatás viszont nem várhat az 1 Hz-es mezőre.

## 5. Tesztek és mérés

A végső forrás és mindkét build a `T-final/` könyvtárban befagyasztva.
A kötelező végső mátrix elkészült. Fejlesztői bizonyíték: N2 temporal reproduction; T1 phase/negative kontrollok;
T1 schedulercontract 1/2/4 worker; T1 C-moving exit 0; T2 activity 22/0,
snapshot 8/0, streamlife 10/0, splitmerge 20/0.

| Végső csoport | Natív siker / futás | Assertion PASS / FAIL / SKIP |
|---|---:|---:|
| Release temporal + schedulercontract | 4 / 4 | 74 / 0 / 0 |
| Páros schedulerwork | 42 / 42 | 168 / 0 / 0 |
| Változatlan páros N2/T C | 1 / 4 | 54 / 3 / 0 |
| T readiness smoke / dense / spread | 3 / 3 | 24 / 0 / 0 |
| Debug kötelező csoport | 8 / 8 | 134 / 0 / 0 |
| Meglévő targeted runner, 11 mód | 11 / 11 | 382 / 0 / 1 |
| Meglévő regression runner, 25 mód | 24 / 25 | 331 / 2 / 0 |

Összesen 97 formális natív futás, ebből 4 hibás; a teljes orchestration
exitje **1**, a C és legacy scheduler hibák miatt. Az assertionok nem
egymástól független egyedi tesztesetek száma: például a mapaudit másik
tesztcsomagot is meghív. A plusz két N2 readiness-kontroll külön mérés.

Targeted: streamadmission, streaming, worldquery, streamlife, map4,
snapshot, terrain, worldpackage (R6 strict/legacy pozitív/negatív is),
mapsplit, mapaudit, bootstrap. Regression: három field/loadfield/partitionscore
selftest, routing, lod, activity, loadfield, partitionscore, stability,
splitmerge, ghost, aoi, replication, scheduler, tickrate, inputpath, netstress,
presence, asfdeterminism, workerpool, replv2, protocol, hygiene, reclamation
100 és 1000. Az egyetlen nemnulla regression-run a fent részletezett legacy
scheduler. Az 1000 reclaim ciklus mind konzisztens; maximum 21 zónaslot,
4069 újrafelhasználás, 916 trim; working set 50,7→57,9 MiB a 10→1000 ciklus
között. Ez nem sanitizer-bizonyíték.

A T2 threaded fixture első próbája `3221226505` native exitet adott. A
fixture IO-környezetének élettartama rövidebb volt a zónák által tárolt
sessionökénél. A deklarációs sorrend javítása és a tesztkimenet flush-a után
ugyanaz a valódi worker-fázisteszt PASS. A hibás futás megmaradt, nem végső PASS.

Végső célzott tesztek: release temporal **38/0**, schedulercontract 1/2/4
worker külön **12/0**. Debug: temporal **38/0**, mindhárom schedulercontract
**12/0**, activity **22/0**, snapshot **8/0**, streamlife **10/0**, splitmerge
**20/0**; mind a nyolc natív exit 0.

A temporal teszt a valódi owner-tick előtt megfogott command-barrierrel
publikációs vágópontot képez, utána valódi MovementSystem lépi át a
radius-határt. A következő fázis felébreszti a másik zónát, NextTick változatlan.
Az exact radius és a közvetlen `nextafter` belső/külső pont is tesztelt.
Negatív kontroll: hiányzó publikáció, visszatartott wake, stale döntésgeneráció,
kihagyott pozitív hatás, hibás field-source és ismételt frissítés mögé bújtatott
régi mulasztás mind felismerve. Ezekben a PASS az elvárt hiba felismerését jelenti.

A kis új temporal teszt szintetikus terrainnal fut. A teljes
despawn→PrepareTerrain→spawn műveletsort a változatlan C-kontroll, a gated
NotResident→wake→load és pin-élettartam eseteket a meglévő streamlife teszt
fedi. Ezek integrációs lefedések; nem állítjuk, hogy a 38 új assertion mindegyike
file-backed vagy determinisztikus terrain-I/O teszt.

**Normalizált schedulerwork:** minden 42 futás natív exit 0, mindegyikben 64
azonos outputú feladat és 64 millió művelet; a befagyasztott performance-gate
PASS. A táblázatban hét run mediánja és teljes min–max tartománya szerepel.

| Változat / worker | Wall medián, ms | Min–max, ms | Saját 1-workerhez gyorsulás | Valódi callback CPU medián, ms | Due+50 ms miss medián / 64 |
|---|---:|---:|---:|---:|---:|
| N2 observation / 1 | 121,651 | 117,689–126,129 | 1,00× | 125 | 38 |
| T / 1 | 121,685 | 117,523–124,084 | 1,00× | 125 | 38 |
| N2 observation / 2 | 62,370 | 62,290–62,659 | 1,95× | 125 | 14 |
| T / 2 | 62,863 | 62,272–65,318 | 1,94× | 125 | 14 |
| N2 observation / 4 | 32,411 | 31,505–33,632 | 3,75× | 125 | 0 |
| T / 4 | 32,871 | 31,523–34,688 | 3,70× | 125 | 0 |

A páros T/N2 wall arány mediánja 1/2/4 workerrel 0,9869 / 1,0090 / 1,0245,
mind az előre rögzített 1,10 alatt. Ez új relatív regressziós feltétel azonos
véges munkára, nem a régi >2/>1 occupancy-küszöb csökkentése. A régi parancs
megmaradt. Az 1/2-worker deadline-miss nem elveszett munka: minden késői
completion bekerült a számlálásba. A valós CPU-mérés Windows accounting
felbontása miatt durva; az egyedi rövid tasknál a nulla nem nulla CPU-munkát jelent.

Az N2 observation és T harness hasznos munkája azonos. T-ben az enqueue előtti
hook címkéje pontosított, és a completion-populáció ellenőrzése az egzakt pool
számlálót használja a callback-bélyegből közelített deadline helyett. A két
harness forrása külön megőrzött; nem bitazonos tesztbinárisoknak nevezzük őket.
A mikromérés izolált scheduler-workload, nincs benne 7000 mozgó player:
a PASS nem jelenti az új activity-input költségének általános elfogadását.

T mérési szakaszok, hét run feladatonkénti átlagának mediánja (nem p99):

| Worker | Eredeti due→enqueue, ms | Enqueue→worker-start, ms | Callback start→end eltelt, ms |
|---|---:|---:|---:|
| 1 | 0,0646 | 60,2151 | 1,8895 |
| 2 | 0,0705 | 30,6196 | 1,9395 |
| 4 | 0,0825 | 15,2470 | 1,9688 |

A micro ablak elején/végén pending=0, minden runban overflow=0. A callback
minimum/maximum, worker-feladatdarabok és tényleges átfedés minden run
SCHEDWORK/SCHEDWORKER/SCHEDTASK soraiban megmaradt. A gate-tesztes várakozási
idő nem szerepel ebben a performance-táblában.

Gép: i7-10700K, **8 fizikai / 16 logikai**, 63,9 GiB RAM, Windows,
MSVC 19.51/v145, Windows SDK 10.0.28000.0, static CRT; D: Samsung SSD 980 NVMe.
RelWithDebInfo és Debug/assert build. A performance-futások sorosak, közben
nem volt build vagy másik általunk indított benchmark. Affinity, priority,
timer- és energiapolicy nem módosult; dedikált, külső folyamatoktól mentes gépet
nem állítunk. A meglévő flecs C4702/LNK4075 figyelmeztetések nem új TC-hibák.

**Kiegészítő readiness N2-kontroll:** mindkét natív exit 0, külön 6/0
assertion. Azonos standard CLI, 60 s warmup / 30 s kért measure, seed
20260922, 4 worker, 200000 mob; dense 500 player, spread 7000 player.
A T megfelelő runjai külön 8/0-t adtak a két új activity-oracle miatt.

| Workload / változat | Audit zónaslot / measure split | Completion / due+50 ms miss | Scheduler eltelt, ms | Ebből új capture eltelt, ms | Working set, MiB |
|---|---:|---:|---:|---:|---:|
| Dense N2 | 84 | 6295 / 641 | 527,737 | N/A | 2117,1 |
| Dense T | 84 | 6582 / 568 | 633,917 | 97,156 | 2117,1 |
| Spread N2 | 140 / 18 | 59484 / 2466 | 354,719 | N/A | 4381,4 |
| Spread T | 84 / 5 | 44194 / 1005 | 7351,863 | 7116,640 | 4445,6 |

A T coalesced commit→wake-döntés átlaga dense/spread 4,631 / 7,957 ms;
lifetime maximuma, setupot is beleértve, 131,494 / 138,506 ms.
Due→enqueue átlag N2→T: dense 11,024→9,914 ms, spread 4,839→6,325 ms.
Enqueue→start átlag N2→T: dense 0,557→0,580 ms, spread 10,806→16,510 ms.
Ezek külön szakaszok, nem percentilis-összegek és nem feltétlen 50 ms-os
completion-garanciák. N2 commit→wake számláló nincs: **NOT MEASURED**, nem 0.

Az új capture dense esetben kb. 0,32%, spread esetben kb. 23,7% a kért
30 s faliórához viszonyítva. A számláló lock-várakozást, source-másolást és
leaf-keresést együtt tartalmazó eltelt idő, nem OS CPU. Ez a spread esetben
érdemi költség és nyitott performance-review tétel.

Ez egy-egy kiegészítő run, nem hét páros readiness-mérés. A dinamikus ASF
eltérő topologyt állított elő (140 vs. 84 slot, 18 vs. 5 measure split),
ezért a completion-darabszámból, process memóriából vagy deadline-missből
nem állítunk izolált gyorsulást, memóriaregressziót vagy kauzális javulást.
A különbséget nem tüntettük el ASF/LOD kikapcsolásával. A raw eredmény és az
új komponens mért költsége alapján további célzott profiling review-zható;
a TC itt nem terjed át ASF-újratervezésbe.

## 6. C-moving-v1

Az útvonal SHA256:
`5DD7D6835D8DC6B5924003C8C4603F60F9B500EDCA517A0B36B7A87359231762`.
500 player / 200000 mob, 100 km, seed 20260922, 4 simulation / 2 IO worker,
16 MiB, 60 s warmup / 30 s kért measure, 15 s preparation/kérés. Setup A,
majd az eredeti loopban B→A→B ≥0/10/20 s-nál. Nincs plusz settle.
Az 1500 effect, minden placement létszám-/pozíció-/height-ellenőrzése megmarad.

A fejlesztői T1 futás 31,07 s mérési ablakban 205541 load / 205531 eviction,
1355086 terrain-wait step, 0 invalid, 16 MiB peak és 0 due+50 ms miss volt.
1500 átvett wake-update, 805 változó inputgeneráció, 671061 µs capture munka;
a lifetime maximum commit→decision 124347 µs setupot is tartalmaz, nem csak
a mérési ablak maximuma. Ez még nem a formális páros összehasonlítás.

**Formális végső kontroll, N2→T / T→N2 sorrend:**

| Run | Natív exit / PASS–FAIL | Measure, s | Requested=Ready=Consume=effect | Load / eviction | Terrain-wait step | Due+50 ms miss |
|---|---|---:|---:|---:|---:|---:|
| N2, 1. pár | 2 / 12–1 | 31,54 | 1000 | 186343 / 186386 | 1293372 | 100 |
| T, 1. pár | 2 / 14–1 | 31,02 | 1000 | 180613 / 180651 | 1291549 | 57 |
| T, 2. pár | 2 / 14–1 | 30,73 | 1000 | 192948 / 192954 | 1443252 | 10 |
| N2, 2. pár | 0 / 14–0 | 31,21 | 1500 | 207207 / 207187 | 1383906 | 17 |

Mind a négy runban a terrain-budget PASS, a peak accounted legfeljebb
16777216 byte, invalid movement 0, terrain-height és world validation PASS.
A két T run külön activity-generation és activity-current-phase ellenőrzése
is PASS. **A teljes C-elfogadás mégis FAIL a két végső T runban.** Az egyetlen
hibafeltétel `c-moving-three-relocations`: két teljes placement után a harmadik
már nem indult. Emiatt egy harmadik placement-assertion sem futott le; a kisebb
assertionszámot nem tekintjük lefedésnek. Nem csökkentettük a populációt:
minden elkészült placementnél 500 player / 200000 mob maradt ellenőrizve.
A befejezett műveletszám eltérése miatt ezekből nem számolunk azonos munkára
vonatkozó N2→T C-throughput gyorsulást vagy performance-PASS-t.

Bizonyított: a változatlan harness minden 500-as batchben sorosan várja a
PrepareTerrain eredményt; `WaitFor` 20 ms-os `sleep_for` ciklussal pollol;
a következő batch csak a teljes korábbi batch után kezdhető. Az újabb
action kezdetét a 30 s-os külső loop korlátozza. A hibás futásokban a két
batch kitölti ezt az ablakot. Az elindított 1000 művelethez 0 failure és
0 pending tartozik. A T ready→consume run-p99 38,768 / 45,007 ms, a
request→ready run-p99 21,316 / 29,076 ms; ezeket nem adjuk össze. Az N2
első run ugyanezen feltétellel bukik, a második teljesíti mindhárom batch-et.

Nem bizonyított: az eltérő batch-időből mennyi a Windows sleep-felbontás,
descheduling, a consumer-polling, a supervisor vagy a pillanatnyi gépállapot
hatása. Az indító Python/PowerShell különbség önmagában nem magyarázat:
azonos formális indító alatt az N2 egyik runja PASS, a másik FAIL.
A PASS-hoz nem módosítottunk pollingot, timeoutot, ablakot vagy trace-t;
ezt külön harness/timing review-t igénylő nyitott korlátnak hagyjuk.

| Új wake-metrika | T C 1. pár | T C 2. pár |
|---|---:|---:|
| Átvett pending update | 1000 | 1000 |
| Változó inputgeneráció | 684 | 675 |
| Input capture és leaf-feldolgozás összes eltelt ideje | 579069 µs | 592246 µs |
| Első pending commit→döntés átlag, összeg / átvett update | 4,926 ms | 4,277 ms |
| Commit→döntés lifetime maximum, setupot is beleértve | 132,123 ms | 128,947 ms |
| Process working set az auditkor | 1991,7 MiB | 1993,9 MiB |

A capture költsége a measure falióra 1,87% / 1,93%-a; **eltelt idő**, nem
külön OS CPU-mérés. A source coalescing miatt ez update-populáció, nem minden
egyes position-write eloszlása. A lifetime maximum nem measure-window max.
A külön scheduling adatok nyers logjai tartalmazzák original due→enqueue,
enqueue→start és due→start összegeket, completion-populációt és deadline-misst.
A rolling tick eloszlás továbbra is legfeljebb 256 mintás zónagyűrűkből áll,
nem használjuk kizárólagos measure-window globális p99-ként.

A `READINESS memory.activity_grid_mb` örökölt képlete csak a cellatömböt méri:
nem foglalja magába a player-vectorokat, a megtartott generation-source-ot
vagy a wake-frame-et. Ezek mérete aktuális player/zone számmal korlátos; az
auditkori process working set tartalmazza őket, de abból izolált allokációs
többletet nem állítunk. A terrain-ledger 16 MiB-os korlátja külön mérés,
nem a teljes gameserver memóriakerete.

## 7. Korlátok és külön verdiktek

**Activity: BOTH_FIXED az újonnan jóváhagyott fázisszerződésre.** A korábbi
CONTRACT_UNDEFINED állapotot a felhasználó explicit döntése zárta le.
A commit→publication→első scheduling-fázis pozitív wake út javított, a
generáció- és current-phase oracle külön működik; a mentett hibás út és
a negatív kontrollok igazolják a különbséget. Ez nem az eredeti két B1-C
futás pontos eseménysorának utólagos bizonyítása, és nem a teljes C-elfogadás.

**Scheduler:** runtime correctness PASS, production dispatch-konkurencia
PASS, azonos véges munkán a befagyasztott normalizált performance-gate PASS.
**Legacy küszöb FAIL maradt.** A régi parancs és runner exitje nem lett
átírva, az új elfogadás a bizonyítottan eltérő mérési kérdések szétválasztása.

**Integrált lezárás: PARTIAL / review szükséges.** A két végső T C-run nem
teljesítette az előírt három áthelyezést; ezért teljes ACCEPTED/PASS nincs.
A terrain safety és a megfigyelt activity-fázisok sikeresek, de ezek nem
helyettesítik az el nem indult harmadik batch-et. A nagy player-populáció
wake-input költségét külön meg kell ítélni a lent mért adatokból.

Az eredeti SL-2 moving eviction-FAIL külön történeti eredmény marad. Linux/FreeBSD és ASan/TSan nem
kap Windows-futásból PASS-t; a korábbi NOT RUN és symlink/junction SKIPPED
státuszok megmaradnak. Nem telepítünk toolchaint és nem változtatunk géppolicyt.

| Nem futtatott / kihagyott tétel | Ok és következmény |
|---|---|
| Linux / FreeBSD | Nincs elérhető megfelelő build-toolchain; WSL jelenléte önmagában nem futási bizonyíték. NOT RUN. |
| ASan / TSan | A helyi sanitizer-konfigurációhoz hiányzó dependency/toolchain feltételek; nincs telepítés vagy migráció. NOT RUN. Debug/assert nem helyettesíti. |
| Symlink/junction escape | A worldpackage runner a szükséges Windows jogosultság hiánya miatt SKIPPED-et adott. A `..` és strict/legacy negatív tesztek PASS-a nem írja ezt felül. |
| Valódi startup runner újrafuttatása | A startup/main, R6 loader és CLI nem változott N2-höz képest; a worldpackage strict/legacy pozitív/negatív regresszió újrafutott. A korábbi SL-2 startup bizonyíték történeti marad. |
| Teljes SL-2 soak/query/file-world nagy mátrix megismétlése | Nem része a TC kötelező végső mátrixának; a terrain/admission policy és kód változatlan. Korábbi eredményekre hivatkozunk, nem új PASS-ként számoljuk. |

A 7000-player workload input-capture költsége review-t igényel. A mutexek
felvételének várakozása és a pozitív leaf-keresés együtt szerepel a capture
eltelt időben; ebből nem állítunk külön CPU-profilt vagy pontos lock-contention
megoszlást. Új ASF/scheduler vagy nagyobb cache bevezetése nem történt.

A további munka előtt eldöntendő review-tételek:

1. A C-moving fogyasztói polling/batch-idő változékonyságának külön
   instrumentált kivizsgálása szükséges-e. Az eredeti C-moving-v1 trace,
   1500 effect feltétel és minden mostani FAIL változatlan referencia marad;
   esetleges új harness-verzió nem nevezhető visszamenőleg ugyanannak a tesztnek.
2. Elfogadható-e a 7000-player eset mért input-capture költsége, vagy külön
   engedélyezett, célzott profilozás/optimalizálás szükséges a meglévő
   infrastruktúrán belül. Az izolált scheduler micro PASS nem oldja fel ezt.
3. Mely elérhető CI/platform biztosítsa a most NOT RUN sanitizer és
   Linux/FreeBSD bizonyítást, illetve a symlink negatív kontrollt.

Ezeket nem kezdjük el automatikusan a TC-3 után.

## 8. Bizonyítékok és megállás

Gyökér: `build/tc-20260927-184106/`. A mentett specifikáció,
baseline/observation/development source manifestek, binárisok, pontos CLI-k,
natív exitek, kezdet/vég időpontok, fixture-hash-ek és minden FAIL megmarad.
A `run_cases.py` natív hiba esetén összesítve is nemnulla exitet ad.
A végső munka után STOP + review; commit/push/reset/stash nincs.

| Azonosító | SHA256 |
|---|---|
| Mentett N2 release worldbench | `9B1AB06EDC4F87753AC38E59593F531DE2B8119F1B75FEEA8E307AFDD20512DE` |
| N2 observation-only worldbench | `B3BDB15BCFA285CF9614467B84C6849A3F6A517FC95ECF57C0686444125C9632` |
| T-final release worldbench | `B0691033FBDE83A5E1B5BF93DB29B9AB0B173C13030E652C4E821561B0109122` |
| T-final Debug worldbench | `17FBD307174F97E85B76F609B3855D94EDADA3421F4A75570BF3FC25768F78D5` |
| T-final release gameserver | `D827F0D3023C4C99E10D1C18F66659BFCB48D8D14D4BB50547794E92EFEF9629` |
| T-final Debug gameserver | `57FB7779098F61C4C633BE0D0321A94FF77C937689CB6D4A69ABB4E4DDC03077` |
| T-final source.csv | `CD7822173BDCE48E78D5DC44A8C528920713720D91C96047F19BF6C83C6B9A1B` |

A `T-final/source/` a tesztelt runtime és harness teljes mentése. Az utána
elkészült jelen riport kizárólag dokumentációs eltérés; a végső ellenőrzés ezt
külön jelöli. A régi SL-2 runner a futás indulásakor külön source.csv-t ír,
ezért annak teljes manifest-hash-e dokumentumtartalom/CSV-formátum miatt
eltérhet; az egyes nem-MD runtime/harness hash-eket külön összevetjük.

Fő bizonyítékfájlok a gyökérhez képest:

- `baseline/verification.json`, `source.json`, `tracked.patch`, CMakeCache-ek:
  induló dirty N2, config és védett fixture.
- `N2-observation/temporal.txt`, `schedulercontract.txt`, a módosított harness
  fájljai és `source.csv`: javítás nélküli kis reprodukció.
- `results/original-scheduler/`, `results/original-C/`: érintetlen N2/B1-C
  eredeti parancsok és natív hibák.
- `results/formal-core/`, `formal-debug/`: oksági TEMPORALTRACE és valódi
  multiworker vágópont; negatív kontrollok és dispatch bizonyítás.
- `scheduler-work-contract.json`, `scheduler-work-analysis.{json,csv}`,
  `results/formal-work/`: előre rögzített feltétel és mind a 42 mérés;
  worker-részvétel, task-idők, CPU, queue és deadline adatok a nyers logokban.
- `results/formal-C/`, `formal-readiness/`, `readiness-N2-control/`,
  `integration-analysis.json`: változatlan integrációs workloadok, külön
  natív és rész-oracle eredmények, időablakok, memória és késleltetések.
- `results/formal-targeted/`, `formal-regression/`: meglévő teljes célzott
  és regressziós runner; minden natív hiba változatlanul megmarad.
- `T-final/TC-only.diff`: kizárólag a rögzített dirty baseline fölötti tesztelt
  módosítások. `final-artifacts/TC-only.diff` a végső riportot is tartalmazza.
- `final-artifacts/summary.csv`, `combined-runmanifest.json`, `groups.json`,
  `preservation-verification.json`: géppel olvasható összesítés és megőrzés.

Exact végrehajtott parancsok mintái (PowerShell, repository gyökér):

```powershell
cmake --build gameserver/build/windows-relwithdebinfo --config RelWithDebInfo --target worldbench gameserver -j8
cmake --build gameserver/build/windows-debug --config Debug --target worldbench gameserver -j8
& build/tc-20260927-184106/T-final/worldbench.exe --mode activitytemporal
& build/tc-20260927-184106/T-final/worldbench.exe --mode schedulercontract --workers 4
& build/tc-20260927-184106/T-final/worldbench.exe --mode schedulerwork --workers 4 --work-iterations 1000000
& build/tc-20260927-184106/T-final/worldbench.exe --mode readiness --file-world --scenario c-moving-v1 --players 500 --mobs 200000 --warmup 60 --seconds 30 --workers 4 --seed 20260922 --budget-mb 16
```

Az automatizált exact CLI-k, bináris/source/fixture hash-ek és UTC start/end
idők minden csoport `runmanifest.json` fájljában találhatók. Ismétléshez új
output/TMP/TEMP gyökér szükséges; a runnerek szándékosan megtagadják a
meglévő bizonyítékkönyvtár felülírását. A `pipeline.json` a teljes soros
orchestration eredményét is megőrzi.

Fejlesztői korlátok: az első observation-build WinSock include ütközés miatt
nem fordult; a javított include-sorrenddel sikeres build készült. Az első T2
fixture-crash fent külön jelentett. A `T1-development` régi Debug-másolatai
N2 binárisok, nem T1 Debug-validáció; ezt a mellette lévő `BUILD-NOTE.md`
kifejezetten rögzíti. A végső Debug futások csak a fenti T-final hash-ű
binárist használták. A korai standalone fejlesztői trace-ek metaadata nem
olyan teljes, mint a formális runmanifesteké; nem helyettesítik azokat.
