# SL-2 streaming acceptance és R6 — végső review

**SL-2 végrehajtás és review kész; az összminősítés PARTIAL.** N C-moving-v1 két futásban PASS, R6 release/Debug PASS, a 23-as mátrix PASS. A fő regresszió 24/25: scheduler performance FAIL, külön B1/N ismétlésben mindkettő PASS. A B1+C-harness activity/sleep validációja kétszer FAIL. **Az eredeti moving churn továbbra is FAIL.** Nincs „minden futás zöld” állítás; a külön verdiktek a 12. részben vannak.

## 1. Státusz, forrás és reprodukálhatóság

Run-id: `sl2-20260927-131802`, 2026-09-27. Ág: `With_Auriga`;
HEAD: `ab2458dee6aa10ef488aa39fd85f1b67b480e2fa`. Az örökölt dirty MAP/SL
állapot része a baseline-nak. Nem történt commit, push, reset vagy stash.
A checked-in `Client/assets/Maps/test_zone` megőrzése fájlonként ellenőrzött.

| Állapot | Megőrzött bizonyíték |
|---|---|
| B0 | `build/sl-20260927-112258-baseline/`: SL-1 előtti dirty MAP-4; korai timeout, nincs steady-state performance |
| B1 | `build/sl2-20260927-131802/B1/`: elfogadott R3, 210 mentett forrás/config/dokumentum, diff, binárisok, CMakeCache, védett fixture-hash |
| N | `build/sl2-20260927-131802/N2/`: a végső befagyasztott runtime és harness, release/Debug binárisok és SHA256 |
| B1+C-harness | Izolált `B1-C/`; a B1 mentésből kizárólag `ReadinessBench.cpp` kapta meg az azonos C-trace-et és oracle-t. Nincs runtime/policy/mérő backport |

A B1 161 korábban rögzített runtime/bench/map fájlja megegyezett a végső R3-mal.
A [bizonyítékcsomag](../build/sl2-20260927-131802/) tartalmazza az elsődleges
specifikáció másolatát, az SL-2-only diffet B1-hez képest, a végső dokumentumok
másolatát, forrás-/fixture-egyezést és a teljes nyers naplókat.
A `results/...`, `B1/...`, `N2/...` bizonyítékutak e run gyökeréhez értendők.
Gépi összesítő: `results/combined-runmanifest.json`; részletes metrikák:
`results/metrics.json`; végső megőrzés: `final-artifacts/preservation-verification.json`.
A final-targeted, Debug, original, matrix és regression öt forrásmanifestjében
0 runtime/harness eltérés van. A későbbi változás csak dokumentáció (köztük
az új README-blokk kódolásjavítása). A shared protocol változatlan HEAD-tartalom.

| Verzió | Bináris | SHA256 |
|---|---|---|
| B1 | gameserver.exe | E10710A65A8ABA98F88BC55685DC8C0D0E72CE71F0ACB198CAB1C26E04E42D26 |
| B1 | worldbench-debug.exe | 1CC1E5CAF5A8DEBFB8024D0AD627B1E356DFF23070DAC47942B5C56A154347EB |
| B1 | worldbench.exe | 789B2E18227BB4388B6801FB8704724C5AC6335358917A60BE27A6067079AD57 |
| N2 | gameserver-debug.exe | CC85AD21B239AFFBFAEF260D1856ECC285D4C3B5571CB8A222430F310BC5FFF4 |
| N2 | gameserver.exe | 55E288CA74F98F75EC704DD77179A0AD20DB56BD178F54DFDB7FF3CC48F3B722 |
| N2 | worldbench-debug.exe | 4477FBD440569D6711A9E46C62CCE9485B34AFC4857777BA93AF56F694652396 |
| N2 | worldbench.exe | 9B1AB06EDC4F87753AC38E59593F531DE2B8119F1B75FEEA8E307AFDD20512DE |
| B1+C-harness | worldbench.exe | F2C044DC1611DF57CB73A017C6D70C687DFE17F17FA47C3FDDFE203C5B7C0778 |

Gép: i7-10700K, **8 fizikai mag / 16 logikai szál**, 63,9 GiB RAM;
Windows/MSVC 19.51, VS 18 2026, RelWithDebInfo `/Zi /O2 /Ob1 /DNDEBUG`,
statikus CRT, `x64-windows-static`. A futások D: Samsung SSD 980 NVMe-n,
azonos gépen, egymás után futottak; fordítás nem futott performance-run mellett.
Egyedi TMP/TEMP és output; startup-port 25183/25184. A fixture friss kiírása
melegítheti az OS cache-t: ez valódi alkalmazásoldali fájlolvasás, nem kontrollált
hideg fizikai lemezmérés. A megmaradt C4702/LNK4075 warningok a buildlogokban látszanak.

Végrehajtható runner: `gameserver/scripts/sl2_regression.ps1`;
`-Suite targeted|debug|original|matrix|regression|calibration`, kötelező friss
`-OutDir`, a párokhoz `-Baseline` és `-BaselineSource`. Az exact CLI, UTC kezdet/vég,
teljes elapsed, source/bináris/fixture hash, exit és assertion-szám minden run
`runmanifest.json` és `summary.csv` fájljában szerepel. A teljes sorrendet a
`run_N2.ps1`, a baseline-adaptert a `run_B1_C.ps1` rögzíti.

## 2. SL-2 változások és okság

Új streaming/scheduler/ASF/ownership-rendszer nem készült. Az SL-1 retention és
admission javítását megtartottuk; új bizonyított production correctness-hiba
nem indokolt további policy-változást. Az SL-2 runtime-változás megfigyelés és
az izolált R6 loader-policy; a következő táblázat csoportosítja a B1-hez mért diffet.

| Fájl/csoport | Változás |
|---|---|
| `TerrainRequest.h`, `TerrainStreamer.{h,cpp}`, `WorldRuntime.{h,cpp}` | Immutable request-id/start, egyszer beírt atomikus registered/Ready/Consume/terminal időbélyegek; ingress-denominator; bounded ablak/churn és osztályonkénti számlálók; elapsed-work és supervisor CPU |
| `SpawnCoordinator.{h,cpp}` | A tényleges authoritative SpawnPlayer és indexelés után effect-visszaigazolás, a meglévő zónaparancs útján |
| `TerrainDemand.h`, `MovementSystem.cpp` | Releváns megkísérelt CheckStep denominator, külön várakozás/collision/invalid |
| `ZoneScheduler.cpp`, `ZoneWorkerPool.{h,cpp}` | Eredeti due → enqueue → worker-start, completion és due+50 ms deadline; a dispatch/cadence változatlan |
| `ReadinessBench.cpp`, `MapStreamingBench.cpp` | Fázisok, request-latency, valós ablak és final-audit előtti befagyasztás; C-moving-v1; concurrency/ingress/fairness/reload-regressziók |
| `shared/map/WorldPackage`, `WorldPackageLoader`, `main.cpp` | Strict/legacy, CLI/config precedencia, közös startup/offline értelmezés, stabil hibakódok |
| `MapPackageBench.cpp`, `MapRuntimeBench.cpp`, `HardeningBench.cpp`, `BenchWorld.h`, startup-script | R6 korpusz és runtime; a meglévő láncos tesztelőfeltételek explicit legacyre állítása, fixture-átírás nélkül |
| Runner, README/config, requirements/format/review | Reprodukálhatóság és a történeti/aktuális státusz szétválasztása |

A request-id-hoz tartozó kis állapotban nincs entity/session pointer.
A részletes reload-trace legfeljebb 16 sor/ablak, a bench műveleti mintagyűjtés
65536 handle/fázis. Nincs movement-querynként új heap-allokáció vagy részletes log.
A mérések fix memóriaigénye a cache-mérlegen belül van; a bench handle-vektor
külön process memória. A measurement-reset kizárólag megfigyelést vált.

## 3. Policy, concurrency és kiszolgálhatóság

Változatlan: **4:2:1 Admission/Active/Prefetch**, 64 regisztrált request,
külön 64 PrepareTerrain ingress-command, 16 chunk/művelet, 4096 waiting,
64 Admission-tartalék, 32 in-flight, 2 I/O worker, passonként 64 jelölt,
100 ms puha handoff; az eredeti preparation deadline **15 s/kérés**.
Az authoritative 20 Hz, LOD és ASF policy változatlan.

A célzott tesztek 1/8/32/64 egyidejű kérő × 1/4/16 közös chunk, 64 külön
chunk, egy közös fizikai betöltés és fogyasztónkénti pin/lifecycle esetét mérik.
Az osztott 8 MiB tesztben 64×16 közös halmaz peakje 383450 B, metadata 81136 B.
Ez nem 1024 külön chunk feltétlen egyidejű kiszolgálásának ígérete.
A 17-chunk kérés explicit elutasított, a 65. Start előtti ingress-command
elutasított, a korábbi 64 elfogadott kérés lezárul. A 30 pending Admission
mellett az első 7 fizikai admission-lehetőség osztályonként 5/1/1 lett.
Ez feltételes fairness, nem byte-átviteli vagy hard-real-time SLA.

Megmaradt és futott: túlméretes reservation mellett kisebb kérés előrehaladása,
teljes pinnyomás/feloldás, cancel/shared-flight/last-drop/deadline, késői
generation completion könyvelése, Consume-then-drop, slot-reuse és Stop utáni
Cancelled. A deklaráltan el nem férő kötelező halmaz pontos hiba; nincs illegális
eviction vagy cache-emeléssel eltakart kapacitáshiány.

## 4. Eredeti 16 MiB moving/hotspot/border

B0-ban mindhárom eset terrain-preparation timeouttal, warmup előtt állt meg.
A bizonyított történeti ok: ismételt háttérigény által megújított soft-retention,
telített queue, 1 hard-pinned és 895 soft-retained chunk, 4096 waiting, 0 load;
quiescent előrehaladás közben sem kapott helyet a kiszolgálható fontos kérés.
Ezt nem minősítjük át teljes egészében fizikai kapacitáshiánnyá.

| Eset | Verzió | Exit; PASS/FAIL/SKIP | Valós measure s | Load/eviction | Setup Ready/fail | Measure Ready |
|---|---|---|---|---|---|---|
| moving | B1 | exit 2; 11/1/0 | 31.74 | 21/0 | 500/0 | 1500 |
| moving | N | exit 2; 11/1/0 | 31.67 | 19/0 | 500/0 | 1500 |
| hotspot | B1 | exit 0; 8/0/0 | 30.02 | 56167/56141 | 500/0 | 0 |
| hotspot | N | exit 0; 8/0/0 | 30.01 | 52173/52173 | 500/0 | 0 |
| border | B1 | exit 0; 8/0/0 | 30.01 | 214/154 | 500/0 | 0 |
| border | N | exit 0; 8/0/0 | 30.01 | 152/0 | 500/0 | 0 |

Mindkét verzió változatlan CLI-vel futott: 500 player / 200000 mob,
100 km, seed 20260922, 4 simulation + 2 I/O worker, 60 s warmup / 30 s kért
measure, 16 MiB. E csomagok worldlogicja nem tartalmaz warp-ot: R6 miatt
nem kellett kompatibilitási kapcsoló. B0 történeti fixture-manifest, B1 és N
9606 fájlja, 159776076 bájtja azonos a fájlonkénti SHA256 ellenőrzésben.
A történeti R3 moving 19/0 load/eviction FAIL-ja megmarad.
Az elfogadott korábbi R3 hotspot 55398/55373 és border 216/173 load/eviction
eredménye is megmarad az SL-1 riportban; a fenti B1 sorok újramérések,
nem e történeti számok átírásai.

## 5. C-moving-v1: kalibráció, befagyasztás és acceptance

Egy előkészítő kalibráció futott (`results/calibration-v1/`, dev4 bináris);
nem számít formális N-runba. A route kiválasztásának oka két elkülönült aktív
terület és visszatérés; az eredeti movinget nem módosítottuk. Kalibráció:
223473 load, 223424 eviction/free, 222657 ordered reload, 1500/1500 effect,
0 failed/pending, 31,56 s measure, 16777204 B peak. Nem volt elhallgatott sikertelen
C-kalibráció. A dev4 utáni megfigyelt source-manifest és az actual dev4 manifest
külön megmaradt a `SOURCE-NOTE.txt` magyarázatával; nem N2-eredményként szerepel.

A végső kontraktus a formális futás előtt, `2026-09-27T11:41:03.7895216Z`
lett befagyasztva (`C-moving-v1-contract.json`, trace és hash CSV).
Trace SHA256: `5DD7D6835D8DC6B5924003C8C4603F60F9B500EDCA517A0B36B7A87359231762`.
500 egyfős kohorsz: A chunk-középpontok x=5..29/y=5..24;
B x=60..84/y=60..79. Setup A, majd B→A→B az első loopban ≥0/10/20 s.
A meglévő production despawn/preparation/spawn út blokkoló ideje megnyújthatja
a mérést. A logikai kohorszlétszám 500, a relocation közben átmeneti despawn van;
minden kész placement után 500 tényleges player, 200000 mob, kért x/y és
független generátoros height-oracle ellenőrzött. Nem folyamatos gyaloglás.

Kapacitástervezés: 16802 B/chunk payload+overhead, 33482 B read/decode reservation.
Az 500-chunk régió 8401000 B; metadata 2872256 B + 32 reservation + külső
startup-pin együtt **12361482 B**, 16 MiB alatt. Az 1000-chunk útvonal-unió
16802000 B már payloadként sem fér végig a cache-be. A régió tervezett
retention-halmaz, nem 500 egyidejű hard pin. Az aktuális serial preparation
egy chunkot kér, plusz a külső startup-pin; minimális kerete 2922540 B.
A mob/movement háttérigény ezen felül jelentkezik és várakozhat.

| Verzió | Exit; PASS/FAIL/SKIP | Measure s | Load/eviction | Ordered reload | Effect | Peak B |
|---|---|---|---|---|---|---|
| B1+C-harness | exit 2; 13/1/0 | 31.41 | 221272/221271 | NOT MEASURED | NOT MEASURED | 16777210 |
| N | exit 0; 14/0/0 | 31.24 | 208040/208083 | 207347 | 1500 | 16777208 |

| ChunkKey | Generation | Eviction µs | Új igény µs | Publikálás µs | Időrend/ablak |
|---|---|---|---|---|---|
| 5940 | 1 | 855872972987 | 855873034026 | 855873034431 | PASS |
| 5941 | 1 | 855872993973 | 855873054740 | 855873055232 | PASS |
| 5942 | 1 | 855873014881 | 855873054740 | 855873055232 | PASS |

A további részletes trace-ek és automatikus sorrendellenőrzés: `results/reload-order-checks.json`. B1+C-harness ordered reload / effect metrika hiányzik az eredeti runtime-ból: NOT MEASURED; a tényleges pozíció/height és három relocation külön ellenőrzött.

A B1+C-harness első futásának teljes verdictje FAIL: `activity: sleeping zone 282 holds net 1163904 that is brute-force Reduced`. A terrain-budget, mindhárom kért placement és height-oracle PASS. Ez nem streaming-timeout, és nem válik PASS-szá a sikeres N-eredménytől.

Változatlan paraméterű ismétlési pár:

| Verzió | Exit; PASS/FAIL/SKIP | Load/eviction | FAIL |
|---|---|---|---|
| B1-C | exit 2; 13/1/0 | 200970/200983 | READINESS validation: FAIL; READINESS validation: FAIL: activity: sleeping zone 277 holds net 1163806 that is brute-force Reduced |
| N2 | exit 0; 14/0/0 | 207890/207872 | nincs |

Az N kontrollban is 1500/1500 effect, 0 failed/pending és teljes world-validáció PASS. A két B1 auditjelzés külön zónát/net-id-t érint (282/1163904, majd 277/1163806). A gyökérok nincs bizonyítva: a kódaudit szerint az activity mező ~1 Hz-en frissül, a sleep/wake ebből dönt, a validátor pedig az aktuális authoritative playerpozíciókat vizsgálja, audit a scheduling előtt. Ez lehetséges időzítési magyarázat, nem igazolt javítás. Az N két PASS-ából nem következik, hogy az instrumentálás megszüntette az örökölt activity-korlátot. Nem változott a LOD/cadence, nincs hozzáadott settle-várakozás vagy enyhített validátor.

Az előre deklarált eligible fontos halmaz setupban 500, measure-ben 1500
egy-chunkos művelet; mind regisztrált/Ready/Consumed/effect lett. A teljes
ingress 2000 requested / 0 rejected, failed/pending/timeout 0. Ezt nem vetítjük
az anonim háttérigényekre vagy tetszőleges nagyobb munkakészletre.

Nem volt force-evict, policy/cache reset, flush vagy dummy demand a formális
C-futásban. A trace ugyanazon key/generation időrendjét és azonos measure-ablakot
ellenőrzi; warmup reload nem számít bele. A unit-teszt force-evict seamje
kizárólag a megfigyelés korrekt működését teszteli, nem a C-workload része.

## 6. Teljes mátrix és időablakok

| Eset | Exit; PASS/FAIL/SKIP | Teljes elapsed s | Measure s | Zóna-work p99 ms | Process WS MiB |
|---|---|---|---|---|---|
| B1-smoke | exit 0; 6/0/0 | 6.392 | lásd log | 23.165 | 123 |
| N-smoke | exit 0; 6/0/0 | 6.395 | lásd log | 25.077 | 122.4 |
| B1-synthetic-dense | exit 0; 6/0/0 | 94.822 | lásd log | 64.486 | 2187.5 |
| N-synthetic-dense | exit 0; 6/0/0 | 94.925 | lásd log | 60.864 | 2176.8 |
| N-file-dense-128 | exit 0; 8/0/0 | 114.55 | 30.02 | 14.195 | 2487.7 |
| B1-synthetic-spread | exit 0; 6/0/0 | 99.398 | lásd log | 3.441 | 4522.2 |
| N-synthetic-spread | exit 0; 6/0/0 | 99.546 | lásd log | 3.419 | 4536.8 |
| N-file-spread-128 | exit 0; 8/0/0 | 268.398 | 30.02 | 4.773 | 7434.6 |
| N-soak-16 | exit 0; 3/0/0 | 71.645 | lásd log | N/A | N/A |
| N-soak-3 | exit 0; 3/0/0 | 71.622 | lásd log | N/A | N/A |
| N-C-moving-v1 | exit 0; 14/0/0 | 130.879 | 31.24 | 4.863 | 2004.6 |

Indulás és serial batch (a combined fixture+loader időből a loader külön mérve; tiszta fixture-write külön stopwatch nincs):

| Eset | Fixture+loader s | Loader ms | Mob aktiválás s | Player batch s | Tényleges player |
|---|---|---|---|---|---|
| B1+C-harness/C-moving-v1 | 8.49 | 728.85 | 3.97 | 24.81 | 500 |
| N/N-C-moving-v1 | 8.44 | 722.4 | 4.04 | 25.53 | 500 |
| N/N-file-dense-128 | 8.43 | 684.09 | 3.91 | 10.4 | 500 |
| N/N-file-spread-128 | 8.87 | 729.49 | 3.73 | 150.36 | 7000 |

A nagy synthetic esetekből egy B1/N pár, az eredeti és 128 MiB integrált esetekből
egy-egy futás készült; C-ből a jelzés miatt két B1+C és két N futás van.
Ezekből nincs általános gyorsulási/overhead állítás vagy kitalált
zajhatár. ASF on mellett a topológia és aktív zónaszám is változhat. Smoke 3/3 s,
nagy esetek 60/30 s, eager/query 10/15 s, kis soak 60 s; a valós elapsed külön
látszik. A soak örökölt PostTerrainDemand/spawn-fallback teszt, nem az új C
fontos-műveleti admission-elfogadása; a 3 MiB eredmény degradációként értelmezendő.

| Kis soak | Load / eviction / free | Peak accounted MiB | Movement OK / NotResident | Várakozó arány | Fizikai miss p99 ms | Populáció / oracle |
|---|---|---|---|---|---|---|
| 16 MiB, 60 s | 1190 / 972 / 972 | 14,689 | 1555726 / 0 | 0% | 9,24 | 200p/3200mob; 0 height mismatch |
| 3 MiB, 60 s | 7738 / 7734 / 7734 | 2,995 | 519831 / 913363 | 63,7292% | 1773,78 | 200p/3200mob; 0 height mismatch |

A 3 MiB futásban 18 cancel, 0 permanent/invalid hiba, 1687581 admission-wait
volt. A kisebb keret 15 resident chunkkal zárult; ez tényleges degradáció.
A soak natív ablakában korábban indult completion is befejeződhet: ezért a
7738 completed és 7736 started nem azonos kezdésű populáció. A log 60 s kért
soak-ot és 71,62/71,65 s teljes parancsidőt ad; külön pontos soak-stopwatch
érték nincs. A footprint előtt tartott 151 MiB eager oracle a process memóriához
tartozik, nem a terrain-budgethez.

| Verzió | 4 km / ASF off mód | 3 native exit | 3 run-p99 medián ms | Run-p99 tartomány ms |
|---|---|---|---|---|
| B1 | eager | 0, 0, 0 | 25.177 | 24.618–25.669 |
| N | eager | 0, 0, 0 | 24.614 | 22.640–25.858 |
| B1 | streamed | 0, 0, 0 | 25.622 | 25.270–25.692 |
| N | streamed | 0, 0, 0 | 24.014 | 23.048–24.626 |

A medián a három külön futás jelentett p99-ének mediánja, nem összevont globális p99. Fixture-egyezés páronként és eager/streamed között ellenőrzött; a query-helyességet a külön 27-es worldquery korpusz is vizsgálja.

## 7. Késleltetés, safepoint és előrehaladás

| Eset | Fázis | requested/registered/Ready/Consume/effect/fail/pending | Ready p99 ms | Ready→Consume p99 ms | Effect p99 ms |
|---|---|---|---|---|---|
| N-moving | setup | 500/500/500/500/500/0/0 | 50.006 | 27.821 | 132.952 |
| N-moving | warmup | 0/0/0/0/0/0/0 | N/A | N/A | N/A |
| N-moving | measure | 1500/1500/1500/1500/1500/0/0 | 27.863 | 47.822 | 81.722 |
| N-hotspot | setup | 500/500/500/500/500/0/0 | 119.014 | 27.018 | 180.255 |
| N-hotspot | warmup | 0/0/0/0/0/0/0 | N/A | N/A | N/A |
| N-hotspot | measure | 0/0/0/0/0/0/0 | N/A | N/A | N/A |
| N-border | setup | 500/500/500/500/500/0/0 | 151.395 | 26.355 | 233.95 |
| N-border | warmup | 0/0/0/0/0/0/0 | N/A | N/A | N/A |
| N-border | measure | 0/0/0/0/0/0/0 | N/A | N/A | N/A |
| N-C-moving-v1 | setup | 500/500/500/500/500/0/0 | 138.97 | 21.185 | 194.664 |
| N-C-moving-v1 | warmup | 0/0/0/0/0/0/0 | N/A | N/A | N/A |
| N-C-moving-v1 | measure | 1500/1500/1500/1500/1500/0/0 | 13.206 | 21.479 | 75.011 |
| N-file-dense-128 | setup | 500/500/500/500/500/0/0 | 4.385 | 24.847 | 122.522 |
| N-file-dense-128 | warmup | 0/0/0/0/0/0/0 | N/A | N/A | N/A |
| N-file-dense-128 | measure | 0/0/0/0/0/0/0 | N/A | N/A | N/A |
| N-file-spread-128 | setup | 7000/7000/7000/7000/7000/0/0 | 45.98 | 32.83 | 112.507 |
| N-file-spread-128 | warmup | 0/0/0/0/0/0/0 | N/A | N/A | N/A |
| N-file-spread-128 | measure | 0/0/0/0/0/0/0 | N/A | N/A | N/A |

C fizikai epizódok:

| Fázis | Befejezett miss | p99 felső ms | Kezdő queue | Kezdő oldest ms | Végi queue | Végi oldest ms |
|---|---|---|---|---|---|---|
| setup | 25136 | 21531.814 | 0 | 0 | 4032 | 25537.31 |
| warmup | 158947 | 38144.93 | 4031 | 25631.42 | 0 | 0 |
| measure | 208040 | 85.557 | 0 | 0 | 207 | 166.82 |

C measure scheduler: 28186 completed minta; due→enqueue 52335848 µs, queue 8041096 µs, due→start 60389716 µs összeg; 0 deadline miss. Supervisor CPU 16921875 µs, pump elapsed-work 6384000 µs, drain 6638625 µs.

C measure fizikai osztályszámlálók (a phase-határ statisztikái közötti delta):

| Osztály | Vizsgálati lehetőség | Admitted | Completed | Kezdő waiting | Végi waiting |
|---|---|---|---|---|---|
| Admission | 1121 | 787 | 787 | 0 | 0 |
| Active | 4193872 | 207285 | 207253 | 0 | 207 |
| Prefetch | 0 | 0 | 0 | 0 | 0 |

Start→Ready a production státuszváltás, Ready→Consume a kézbesítési késés,
start→effect a tényleges authoritative spawn vége. Az eredeti pollingos
preparation-p99 külön megmaradt. Nulla mintánál N/A, nem nulla késleltetés.
A timestamp offset +1 µs sentinel-eltolást használ; 0 nem megfigyelt esemény.
A teljes navigation-job és cross-zone warp végső effect-percentilise NOT MEASURED;
Ready-t nem nevezzük ezek befejezésének. A tényleges correctness külön tesztelt.

A fizikai miss-hisztogram az adott ablakban befejezett epizódokat méri,
beleértve korábban kezdődöttet; p99 upper bucket edge. Kezdő/végi queue és
oldest age külön szerepel. A class examined ismételt vizsgálat is lehet,
nem egyedi eligible populáció; admitted/completed a fizikai kiszolgálás osztálya.
A `terrain-window` admission számlálói lifetime értékek, a táblákban delta látszik.
Az adat `captured_us` ideje rögzített, a publikálás legfeljebb 100 ms késésű.
A végső audit N-ben nem növeli visszamenőleg a measure számlálóit.

| Build | Snapshot-terhelés | Valós megfigyelés ms | Supervisor CPU µs | Max drain µs | Completed timing | Deadline miss |
|---|---|---|---|---|---|---|
| N2-targeted | 0 | 5004.616 | 15625 | 131508 | 155 | 155 |
| N2-targeted | 1 | 5048.516 | 46875 | 131063 | 156 | 156 |
| N2-debug | 0 | 5000.817 | 15625 | 931523 | 21 | 21 |
| N2-debug | 1 | 5731.208 | 125000 | 955510 | 24 | 24 |

Release snapshot nélküli stressz: 160 Ready / 0 hiba, 134 load / 118 eviction / 115 free, 159 kiadott tick, 38 safepoint, 4,366274 s drain-összeg, 0 snapshot. Olvasókkal: 162 Ready / 0 hiba, 136/120/117, 160 tick, 39 safepoint, 4,376885 s drain, 122 snapshot. Peak mindkettőben 515616/524288 B. A terhelt single-worker minta halad, de a due+50 ms határidőt ezekben a mintákban nem teljesíti.

A külön 3-olvasós snapshot/split-merge stresszben release maximum request→capture 21,4 ms, Debug 133,7 ms; 100 ciklus és invariánsok PASS. Ez külön workload, nem a terrain-stressz snapshot latency-percentilise. A terrain-stressz snapshot-darabot és 0 olvasói timeoutot mér; ott külön capture-percentilis NOT MEASURED.

A drain alatt korábban kiadott zone-work futhat; a drain összege nem egyetlen
folyamatos világmegállás. Pump/MakeRoom/reclaim/completion/admission egymásba
ágyazott elapsed-work, nem összeadható CPU-idő. A GetThreadTimes külön supervisor
kernel+user CPU, rövid ablakon kvantált, egyéb platformon NOT MEASURED.
A due→enqueue/queue/due→start összegek sok zónára vonatkoznak; határidő:
completion > original due +50 ms. Nincs kitalált globális p99.

A meglévő `READINESS tick` végső snapshotban tárolt, zónánként legfeljebb 256
friss mintát egyesít: visszanyúlhat a pontos measure-határon túl. A scheduler
delta és stage totals ettől külön mutató. B1 eredeti R3 terrain-reportja
tartalmazhat audit utáni többletet; N előtte fagyaszt. Ezért a nyers B1/N
darabszámok nem pontosan szinkronizált, tiszta mérési-overhead kísérletek.

## 8. Memória, I/O és churn-költség

| Eset | Resident B | In-flight B | Retired B | Metadata B | Accounted B | Peak B | Budget B |
|---|---|---|---|---|---|---|---|
| N-moving | 9503352 | 0 | 0 | 2872256 | 12375608 | 16777208 | 16777216 |
| N-hotspot | 13844848 | 0 | 0 | 2872256 | 16717104 | 16777214 | 16777216 |
| N-border | 11845410 | 0 | 0 | 2872256 | 14717666 | 16777208 | 16777216 |
| N-C-moving-v1 | 12819926 | 1071424 | 0 | 2872256 | 16763606 | 16777208 | 16777216 |
| N-file-dense-128 | 114935830 | 0 | 0 | 2872256 | 117808086 | 134217728 | 134217728 |
| N-file-spread-128 | 108343842 | 0 | 0 | 2872256 | 111216098 | 134217728 | 134217728 |

C: 1498 unique requested / 1498 unique loaded; 207347 ordered reload / 208040 load (99.67%). 207326 reload eviction→publish <1 s. I/O 3470107200 B; movement 1399710/4399860 NotResident (31.81%), invalid 0, collision 0. In-flight peak 1071424 B, retired peak 67208 B, mintázott pinmaximum 33604 B. A trace külön tartalmazza eviction→új igény időközét is.

`accounted = resident + in-flight + retired + fixed metadata`; pinned részhalmaz,
nem adható hozzá újra. A retired és régi generációjú élő reservation a valódi
lezárásig könyvelt. N 100 km metadata: 2872256 B; B1: 2479056 B; **+393200 B**
mérési költség ugyanazon keretben. A hard pin, soft handoff/retention és
eviktálható rész az admission-trace-ekben külön látszik.

A process working set pillanatminta, nem peak és nem terrain-budget. ECS,
session/replication, navigation search node, allocator, demand buffer és bench
handle-tároló külön process memória; részletes egyedi heap-attribúció nem készült.
A sok rövid reload és a NotResident arány valódi költség; több load nem önmagában
jobb teljesítmény. A világ összes chunkjának uniója nem egy kérés kötelező halmaza.
A nyers adatok utóellenőrzése 25 readiness-futásban igazolja az exact memóriaegyenletet
és a peak≤budget feltételt (`results/ledger-verification.json`). A 20 fixture-pár
egyezik, a 48 részletes reload-trace a saját ablakában rendezett. Ez ellenőrzött
mintabizonyíték; nem sanitizer vagy minden lehetséges interleaving bizonyítása.

## 9. R6 strict/legacy és valódi startup

**CLI `--warp-policy strict|legacy` > config `warp_policy` > strict default.**
Triggerbe célzó warp strictben strukturált `WORLDLOGIC_WARP_TARGET_IN_TRIGGER(413)`
hiba konkrét warp/trigger azonosítóval. Explicit legacyben aciklikus lánc warning.
Önhurok/ciklus mindkettőben `WORLDLOGIC_WARP_CYCLE(412)` hiba. Nincs verzióból vagy
strict hibából automatikus fallback. A legacy kliensparser API változatlan.
Invalid/üres policy exit 2, tartalmi hiba exit 3, fail-fast DB/listener előtt.

Final release és Debug package-korpusz: 140 PASS / 0 FAIL / 1 symlink SKIPPED;
min/belső/max/nextafter half-open szélek, strict default és mindkét policy.
Strict-valid generált fixture valódi egyszeri warpja és legacy lánc rearm/cooldown,
pending/migration/split/merge invariánsai a MAP4 37 PASS között vannak.
A checked-in test_zone és örökölt láncos tesztsegéd explicit legacy, bájtjaik
változatlanok. `mapaudit`: **14 CHANGED / 0 OPEN**, nem az exitből következtetve.

Valódi startup release **46/46**, Debug **46/46**: az eredeti 29 eset és 17 R6
ellenőrzés; CLI/config ütközés mindkét irányban, default, invalid értékek,
offline/startup egyezés, cycle/self, valamint a pozitív actual startup.
A tartalmi negatív esetek elvárt exit 3-at adnak; az acceptance runner exit 0.
A külön portokhoz és meglévő helyi DB-hez nincs DB- vagy gameplay-fejlesztés.

## 10. Regresszió és platformhatár

| Suite | Parancs | Exit; PASS/FAIL/SKIP |
|---|---|---|
| N2-targeted | streamadmission | exit 0; 51/0/0 |
| N2-targeted | streaming | exit 0; 27/0/0 |
| N2-targeted | worldquery | exit 0; 27/0/0 |
| N2-targeted | streamlife | exit 0; 10/0/0 |
| N2-targeted | map4 | exit 0; 37/0/0 |
| N2-targeted | snapshot | exit 0; 8/0/0 |
| N2-targeted | terrain | exit 0; 20/0/0 |
| N2-targeted | worldpackage | exit 0; 140/0/1 |
| N2-targeted | mapsplit | exit 0; 21/0/0 |
| N2-targeted | mapaudit | exit 0; 37/0/0 |
| N2-targeted | bootstrap | exit 0; 4/0/0 |
| N2-debug | streamadmission | exit 0; 51/0/0 |
| N2-debug | streaming | exit 0; 27/0/0 |
| N2-debug | worldquery | exit 0; 27/0/0 |
| N2-debug | streamlife | exit 0; 10/0/0 |
| N2-debug | map4 | exit 0; 37/0/0 |
| N2-debug | snapshot | exit 0; 8/0/0 |
| N2-debug | worldpackage | exit 0; 140/0/1 |
| N2-regression | field-selftest | exit 0; 4/0/0 |
| N2-regression | loadfield-selftest | exit 0; 6/0/0 |
| N2-regression | partitionscore-selftest | exit 0; 13/0/0 |
| N2-regression | routing | exit 0; 5/0/0 |
| N2-regression | lod | exit 0; 18/0/0 |
| N2-regression | activity | exit 0; 22/0/0 |
| N2-regression | loadfield | exit 0; 20/0/0 |
| N2-regression | partitionscore | exit 0; 20/0/0 |
| N2-regression | stability | exit 0; 24/0/0 |
| N2-regression | splitmerge | exit 0; 20/0/0 |
| N2-regression | ghost | exit 0; 23/0/0 |
| N2-regression | aoi | exit 0; 42/0/0 |
| N2-regression | replication | exit 0; 42/0/0 |
| N2-regression | scheduler | exit 2; 11/2/0 |
| N2-regression | tickrate | exit 0; 5/0/0 |
| N2-regression | inputpath | exit 0; 2/0/0 |
| N2-regression | netstress | exit 0; 13/0/0 |
| N2-regression | presence | exit 0; 8/0/0 |
| N2-regression | asfdeterminism | exit 0; 5/0/0 |
| N2-regression | workerpool | exit 0; 3/0/0 |
| N2-regression | replv2 | exit 0; 9/0/0 |
| N2-regression | protocol | exit 0; 7/0/0 |
| N2-regression | hygiene | exit 0; 3/0/0 |
| N2-regression | reclamation100 | exit 0; 3/0/0 |
| N2-regression | reclamation1000 | exit 0; 3/0/0 |

A mapaudit táblázatban 37 explicit PASS a beágyazott MAP4 runtime-ból származik; az audit saját minősítése 14 CHANGED/0 OPEN. A scheduler faliórás uniform/reference-parallelism küszöbe nem változott; egy sikeres futás nem bizonyítja a történeti flakiság megszűnését.

A scheduler célzott reprodukciója az eredeti `--mode scheduler --seconds 3` paranccsal, külön friss outputban:

| Verzió | Exit; PASS/FAIL/SKIP | FAIL sorok |
|---|---|---|
| B1 | exit 0; 13/0/0 |  |
| N2 | exit 0; 13/0/0 |  |

Symlink/junction-escape: SKIPPED a csomagkorpusz jelzett környezeti feltétele miatt;
a lexikális `..` teszt nem helyettesíti. Linux/FreeBSD és ASan/TSan nem kap
Windows Debugból származtatott PASS-t. Linux/FreeBSD/ASan/TSan: **NOT RUN**. A Windows ASan CMake-út dinamikus x64-windows függőségeket igényel; a Boost.Asio header/config hiányzik (`results/asan-preflight.json`). Az elkészített `run_ASan.ps1` nem lett végrehajtva. Ubuntu WSL-disztribúció van, de nincs cmake, clang++, g++, ninja, pkg-config, illetve a vizsgált vcpkg/flecs/capnp készlet: `results/linux-toolchain-preflight-script.txt`. Környezet-/jogosultságmódosítás nem történt.

Reprodukció hordozható build-környezetben: ugyanaz a mentett N2 forrás és shared
protocol, C++20 compiler és a `gameserver/CMakeLists.txt` függőségei; CMake
`-DENABLE_WORLDBENCH=ON`, külön buildgyökér; ASan `-DENABLE_ASAN=ON`. TSanhoz
külön clang/gcc build szükséges `-fsanitize=thread` fordítás/link flaggel.
Utána `worldbench --mode streamadmission`, `streamlife`, `streaming`, `worldquery`,
`map4`, `snapshot`, `worldpackage`, majd az exact manifest CLI-k. A startup-script
külön friss abszolút outputot, ismert DB-konfigurációt és szabad portot igényel.
Ezek reprodukciós lépések, nem lefuttatott platformeredmények.

## 11. Negatív eredmények és nyitott korlátok

- B0 eredeti timeout és R3/B1/N moving churn-FAIL megőrzött; nem mesterséges PASS.
- `build-dev1.log`: új mérési lambdák compile-hibái javítva; nem runtime-teszt.
- `dev3-targeted` mapsplit abort: strict alatt a régi implicit láncos test_zone
  előfeltétel hibás lett. B1 mapsplit PASS; explicit legacy tesztsegéddel dev4,
  majd végső N2 PASS. Nem ownership-hiba és nem „known flaky” címkével eltüntetett FAIL.
- N1 és dev4 zöld körei köztes bizonyítékok; a végső runtime N2, külön hash-egyezéssel.
- A B1+C activity/sleep validáció két futásban FAIL; az N két runja PASS.
  Nincs bizonyított activity-fix, és nem lazítottuk az ellenőrzést. A scheduler
  első N futása FAIL, B1/N kontrollja PASS; az eredeti bukás változatlanul megmarad.
- A kalibráció sikeres, de a formal acceptance csak a befagyasztott N-run alapján áll.
- Az eredeti moving kis releváns halmaza ezen trace-en nem kényszerít evictiont.
  A C nagy region-váltása és visszatérése külön workload, nem a régi feltétel lazítása.
- Véges cache mellett a háttérmozgás várakozhat, és a telített single-worker stressz
  deadline-miss-ei valósak. Nincs 20 Hz teljesítménygarancia minden entitásnak,
  tetszőleges concurrencyre vagy 50k player / 1–2M mob production-readiness állítás.
- A kis soak fallback-útja, process-heap-attribúció, nav/warp effect-percentilis,
  nem futtatott platformok és az ismert scheduler faliórás flakisága nyitott határ.

Végső native nemnulla eredmények:

| Suite | Eset | Exit | Hiba |
|---|---|---|---|
| B1-C | C-moving-v1 | 2 | READINESS validation: FAIL; READINESS validation: FAIL: activity: sleeping zone 282 holds net 1163904 that is brute-force Reduced |
| C-repeat | B1-C-C-repeat | 2 | READINESS validation: FAIL; READINESS validation: FAIL: activity: sleeping zone 277 holds net 1163806 that is brute-force Reduced |
| N2-original | B1-moving | 2 | READINESS real-streaming-churn: FAIL |
| N2-original | N-moving | 2 | READINESS real-streaming-churn: FAIL |
| N2-regression | scheduler | 2 | SCHED uniform-parallelism: FAIL; SCHED reference-parallelism: FAIL |

## 12. Külön verdiktek és megállási pont

| Terület | Verdikt és határ |
|---|---|
| N terrain safety/correctness | PASS a lefuttatott Windows/Debug corpusban: budget, pin/lifetime, stale completion, ownership/warp/query. A tágabb activity/sleep lezárás PARTIAL a B1 új trace-en kétszer reprodukált jelzése miatt; nincs bizonyított általános javítás. |
| Liveness / concurrency | PASS a deklarált tesztpontokon, snapshot nélkül és olvasókkal. A saturated single-worker deadline miss valós; nem hard-real-time garancia. |
| N C-moving acceptance | PASS: az előre befagyasztott formális run és változatlan N kontroll is teljesül; file-read/miss/load/eviction/ordered reload, 1500 effect, budget és world/oracle. B1+C teljes verdict 2/2 FAIL, külön őrizve. |
| R6 / dokumentáció | PASS Windows RelWithDebInfo és Debug: strict default, explicit legacy, startup/offline és runtime. Symlink escape SKIPPED, nem helyettesített teszt. |
| Regresszió | PARTIAL: az első 25-parancsos sorozatban 24 exit0, scheduler exit2 (11 PASS/2 FAIL). Célzott B1/N pár 13/13 PASS, a korábbi FAIL és a nyitott faliórás probléma megmarad. |
| Teljes SL-2 | Végrehajtás és bizonyítékátadás KÉSZ; az SL-2 összminősítése PARTIAL a fennmaradt konkrét tételek miatt. Eredeti moving churn FAIL; Linux/FreeBSD/ASan/TSan NOT RUN; nincs 50k/1–2M readiness vagy mért univerzális SLA. |

Következő review-döntés: az activity mező frissítése, sleep/wake és az aktuális pozíciókat ellenőrző audit közötti szerződés célzott tisztázása/reprodukciója; valamint a scheduler faliórás acceptance kontrollált vizsgálata. Ezeket a jelen feladat nem zárta le és nem módosította önkényesen. A következő fázis megkezdése külön döntés.

SL-2 után **STOP**. Nincs automatikus következő map-fázis, kliens/login/DB/gameplay
munka vagy commit/push. A következő döntés a fenti nyitott mérési/platform- és
terhelési határok közül választandó; nem következik belőlük új scheduler vagy
streamer szükségessége.
