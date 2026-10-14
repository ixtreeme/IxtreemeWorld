# TC-4 review — wake capture és C-moving

## 1. Verdikt

| Terület | Eredmény |
| --- | --- |
| TC-4A capture | PASS a célterheléseken; C trade-off: §6 |
| Temporal safety | PASS |
| H0 / c-moving-v1 | Eredeti T0: FAIL (native 2); a páros futások nyers eredménye lent |
| H1 / c-moving-v2 | PASS |
| Scheduler | PASS |
| Regresszió / Debug | PASS |
| Integrált aktuális acceptance | PASS |
| Platform | Windows optimized + Debug; Linux/FreeBSD/ASan/TSan NOT RUN; symlink/junction SKIPPED |

Blokkoló feltételek: nincs a lefuttatott aktuális Windows acceptance-ben.

STOP + review. Nem történt commit, push, reset, stash, timer/affinity/prioritás/energiapolicy-módosítás vagy toolchain-telepítés. A kliens, loginserver, DB, gameplay és monitorozóprogram nem változott.

## 2. Baseline és bizonyítékvédelem

A baseline a `With_Auriga` branch TC-végi T-final dirty állapota; HEAD `ab2458dee6aa10ef488aa39fd85f1b67b480e2fa`. A TC-4 indulásakor minden korábbi T-final runtime-forrás hash-egyezett. Egy korábbi review-dokumentum változott az archivált T-finalhoz képest; a runtime nem. Az érintett induló források teljes másolata, tracked patch-e, státusza és binárisai a `baseline/` alatt vannak. A HEAD önmagában nem reprodukálja ezt a munkát.

A végső T-control-final és U-final fordított forrása csak a `ZoneScheduler.cpp` geometriai keresésében tér el. Profilozásuk, wait hookjuk, H0/H1 harnessük és tesztjeik azonosak. Az acceptance-összesítő későbbi szigorításai és a végső riport nem módosították a mért runtime/bench forrást.

| Változat | Bináris | SHA-256 |
| --- | --- | --- |
| baseline | worldbench.exe | B0691033FBDE83A5E1B5BF93DB29B9AB0B173C13030E652C4E821561B0109122 |
| baseline | worldbench-debug.exe | 17FBD307174F97E85B76F609B3855D94EDADA3421F4A75570BF3FC25768F78D5 |
| baseline | gameserver.exe | D827F0D3023C4C99E10D1C18F66659BFCB48D8D14D4BB50547794E92EFEF9629 |
| baseline | gameserver-debug.exe | 57FB7779098F61C4C633BE0D0321A94FF77C937689CB6D4A69ABB4E4DDC03077 |
| TP-profile | worldbench.exe | 6D433BB04E6AFABFD241549095F1B70AF438BC12E6B450671F248FEE59B4BA84 |
| TP-profile | gameserver.exe | D27F02DA7426F262D01D6CE7A0AF7C6FE0DE0121BFADE678BC1AC9DB6E71ED3E |
| T-pre | worldbench.exe | 67F1FD32167F2E1186E4C6E037962A0CE6201E9307E4A88BF1C60CD77B5484C3 |
| T-pre | gameserver.exe | 2ACA56AD3DAC523374CCBBE0812F4539A1BA47C3163707794C366BA7CE0FE541 |
| T-control-final | worldbench.exe | 81A3C8FEBCFA4BB6690EB84D07759611B7D9D062AF8C9632EC48B5B1E0FC34D7 |
| T-control-final | gameserver.exe | 2ACA56AD3DAC523374CCBBE0812F4539A1BA47C3163707794C366BA7CE0FE541 |
| U-final | worldbench.exe | 75A04BECAE26D4A41086C5A508D1448093907AE3ECBDE5777729C96A9CD21DB0 |
| U-final | worldbench-debug.exe | EAF8C7FCCAADB0B3E28100DAEFF24B24C922F2FC9E48E2A37E069F5C50122CEC |
| U-final | gameserver.exe | DD55627E236C31D1844DAB775FEB8A910442AD940980977DAF2BA13B0C07BB7D |
| U-final | gameserver-debug.exe | 0CB5C4409667F648EAA91DFF45AC90B2E9F69E18E5E39C4D01E3159D5889B6CE |

Forrás-, konfiguráció- és fixture-hash: változatonként `source.csv`, `source.json`, CMakeCache; futásonként `runmanifest.json` és `*-fixtures.json`. UTC start/end, steady elapsed, exact CLI és native exit minden futásnál mentve. [TC4-only.diff](D:/IxtreemeWorld/build/tc4-20260927-201219/TC4-only.diff) alapja az ellenőrzött T-final dirty baseline. A 21 becsekkolt test_zone fájl és a korábbi T-final forráscsomag utóellenőrzése: [final-verification.json](D:/IxtreemeWorld/build/tc4-20260927-201219/final-verification.json).

| Változat | Source manifest SHA-256 |
| --- | --- |
| baseline | 269458FD22BC8E765F6B79ED2138C17148E7B2B3F6DB75CE5A5D78A9BAC1A0B0 |
| TP-profile | 0795E618284F9DCF0548C1FE8FFAA36A8C9C081D470E660C4D9014F810F71C17 |
| T-pre | 0A56C8C6F3E7607C0F4E69D45FBD7BB28244FD437C9B7837830AAD084BF725F7 |
| T-control-final | 73646BB230015BE1679817EE6FA1406C6AE386E2DC8E36DEF01A15C0A3B4324D |
| U-final | F6E3D70F827AE7F3E2E03EDFA6BD42F581B987C6AFAF7A39F0C677AC27AD5C93 |

## 3. Bizonyított ok

TP profil, 7000 változó forrás / 200 capture: 1,4 millió másolt forrás, 91,2 millió node-látogatás, mindössze 12 800 külön pozitív levél-beillesztés. A 390,36 ms capture-ből 337,09 ms (86,4%) geometriai keresés, 42,34 ms output, 9,66 ms másolás, 0,434 ms soros mutex-acquisition. A közös vágópontot már a korábbi implementáció biztosította; a keresés már korábban is a publikációs zárakon kívül futott.

TP élő 7000p/200k spread: 8,899 s capture, ebből keresés 6,949 s (78,1%), output 0,755 s, másolás 1,143 s, mutex-acquisition 0,0168 s. 3058 újraépítés / 3076 fázis, 21,406 millió másolt forrás. Az egyetlen mozgó forrás is érvénytelenítette a teljes bemenet újrahasználatát. A mért fő költség a redundáns keresés és ismételt pozitív halmazpróba, nem a lock-contention. A lock-window 1,192 s átfedő mérés, nem hozzáadandó részidő.

C: az érintetlen T0/H0 újra elbukott a harmadik relocationön (native 2, 14 PASS / 1 FAIL). A javított megfigyelésű T-pre/H0 1500 kérésén a request→Ready medián 598,5 µs, Ready→észlelés medián 19 760,15 µs, p95 20 889,5 µs; a tényleges poll-sleep medián 20 541,55 µs. A három soros 500-as preparation 10,56 / 10,34 / 10,30 s-ot vett igénybe. Ez bizonyítja a consumer 20 ms-os pollingpadlójának jelentős költségét; nem bizonyít production streaming-hibát. A harmadik kör időnként belefér, máskor a start-cutoff utánra kerül; a sikeres ismétlés nem törli az eredeti hibát.

Az ok és a küszöbök U előtti rögzítése: [root cause](D:/IxtreemeWorld/build/tc4-20260927-201219/root-cause-before-U.md), [előre rögzített acceptance](D:/IxtreemeWorld/build/tc4-20260927-201219/acceptance-before-U.md).

Az eredeti T0 pontos measure-eredménye: 31,74 s alatt 1000 requested/registered/Ready/Consume/effect, 0 failed/pending, két sikeres relocation-placement; a harmadik nem indult el. A megőrzött első request-stampokban a Ready jellemzően 1–3 ms, a Consume ~32 ms körül történt; Ready→Consume p99 38,84 ms. Nem timeout vagy hibás terrain-placement okozta a FAIL-t. T0-ból teljes batch-clock trace nem létezett; a hiányzó adatot nem rekonstruáltam feltételezett timestampből. A TP és a végső párok már minden batch/request határt rögzítenek.

## 4. Implementáció és invariánsok

A production capture levélenként keres az összes koherensen másolt forrásban, az első pozitív találatig. Ez ugyanaz az egzisztenciális geometriai halmaz, legfeljebb egy beillesztéssel levélenként. A teljes forrásvektor, commit/pending stamp, közös lock-vágópont, revision/incarnation invalidálás és a fázisonkénti resident-eligibility megmarad. Nincs új cache, index, ASF, scheduler, ownership vagy reclamation-rendszer. Nem változott a NextTick, 50 ms dt, 1 Hz mezőfrissítés vagy demotion hysteresis.

A H1 kérésenként saját mutex/CV-t használ. A státusz közzététele ugyanazzal a mutexszel védett, mint a wait predicate; unlock után notify következik. Ready/terminal előtt spurious notify nem siker. A várakozás az eredeti deadline-ig tart, nem pumpál, nem snapshotol és nem Consume. A meglévő owner intézi Cancel/Consume/timeout/pin-felszabadítás sorrendjét. Nincs session/entity callback vagy nyers életciklus-pointer. A két új szinkronizációs objektum 152 bájt/kérés; a meglévő `2*64*sizeof(TerrainRequest)` ledger 19 456 bájttal nőtt, a 16 MiB-on belül.

Fájlok: production `ZoneScheduler.cpp` (profil + keresés), `WakeCaptureProfile.h`, `Zone.cpp` (producer megfigyelés), `ZoneWorkerPool.cpp` (opcionális CPU-mérés), `TerrainRequest.h` (wait). Bench: `ReadinessBench.cpp`, `CaptureProbe.cpp`, `TC4Bench.cpp`, `MapStreamingBench.cpp`, `ClosureBench.h`, `WorldBench.cpp`, CMake regisztráció. Külön acceptance-összesítő és [aktuális szerződés](D:/IxtreemeWorld/docs/tc4-capture-and-wait-contract.md).

## 5. Páros performance, CPU, memória és előrehaladás

Gép: Intel i7-10700K, 8 fizikai / 16 logikai CPU, Windows optimized. A tárolóleltár két Samsung SSD-t és Storage Space eszközt tartalmaz; a fixture alkalmazásoldali olvasása nem kontrollált hideg SSD-mérés. Saját build vagy másik benchmark nem futott párhuzamosan. Külső háttérfolyamatok és OS page-cache teljes izolációja nincs garantálva. A timer-felbontást csak olvastam; a megfigyelések külön evidence-ben vannak.

Fix input: 0/500/7000 forrás × static/rare/dynamic/topology × 5 váltott T/U pár, 200 fázis, 64 kezdeti region. Csak a production capture hívások összege időzített; producer és független teljes forrás×levél oracle kívül van. A topology splitet és merge-et is tartalmaz.

| Forrás | Minta | T ms medián [min,max] | U ms medián [min,max] | U/T |
| --- | --- | --- | --- | --- |
| 0 | static | 1.131 [1.098, 1.151] | 1.127 [1.122, 1.187] | 0.9965 |
| 0 | rare | 1.142 [1.136, 1.200] | 1.143 [1.107, 1.454] | 1.0013 |
| 0 | dynamic | 1.141 [1.133, 1.228] | 1.121 [1.118, 1.134] | 0.9820 |
| 0 | topology | 1.209 [1.170, 1.340] | 1.195 [1.179, 1.223] | 0.9881 |
| 500 | static | 1.370 [1.362, 1.521] | 1.320 [1.302, 1.344] | 0.9637 |
| 500 | rare | 2.811 [2.632, 2.906] | 1.863 [1.776, 1.902] | 0.6627 |
| 500 | dynamic | 30.905 [30.520, 31.410] | 11.489 [11.231, 11.590] | 0.3718 |
| 500 | topology | 1.891 [1.755, 2.166] | 1.559 [1.490, 1.944] | 0.8245 |
| 7000 | static | 4.321 [3.749, 5.707] | 2.517 [2.370, 3.055] | 0.5825 |
| 7000 | rare | 21.393 [20.751, 24.018] | 6.645 [6.474, 6.787] | 0.3106 |
| 7000 | dynamic | 388.937 [378.582, 407.361] | 91.922 [90.697, 102.080] | 0.2363 |
| 7000 | topology | 8.555 [7.784, 9.483] | 3.900 [3.607, 5.257] | 0.4558 |

A 7000 dynamic fix gate U/T ≤0,85. Az élő spread gate medián capture/újraépítés U/T ≤0,90, minden pár <1; dense ≤1,10; producer átlagos (wait+hold)/call medián U/T ≤1,10. Ezek a küszöbök az első U mérés előtt készültek, nem változtak.

Élő terhelés: spread 7000p, dense 500p, mindkettő 200k mob, 4 worker, seed 20260922, 60 s warmup / 30 s measure, ASF/LOD ON. Három spread és két dense pár, majd változatonként OFF kontroll.

| Run | Exit | Capture s | Új input | µs/input | Supervisor CPU s | Worker CPU s | Aktív zóna audit | WS MiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| T-spread-1 | 0 | 5.087 | 1651 | 3080.9 | 6.156 | 84.312 | 106 | 4370.4 |
| U-spread-1 | 0 | 2.312 | 1660 | 1392.8 | 3.406 | 83.391 | 112 | 4367.5 |
| U-spread-2 | 0 | 4.554 | 3904 | 1166.5 | 5.594 | 83.797 | 85 | 4351.4 |
| T-spread-2 | 0 | 8.727 | 3043 | 2867.8 | 10.000 | 86.438 | 91 | 4345.3 |
| T-spread-3 | 0 | 8.899 | 3062 | 2906.3 | 9.734 | 87.719 | 91 | 4362.4 |
| U-spread-3 | 0 | 4.998 | 3821 | 1308.0 | 5.516 | 84.984 | 94 | 4431.2 |
| T-spread-off | 0 | 7.585 | 3308 | 2292.9 | 8.609 | nem mért | 85 | 4376.1 |
| U-spread-off | 0 | 4.747 | 3849 | 1233.4 | 5.875 | nem mért | 100 | 4436.2 |
| T-dense-1 | 0 | 0.144 | 425 | 339.5 | 1.141 | 29.828 | 85 | 2123.8 |
| U-dense-1 | 0 | 0.102 | 424 | 239.5 | 0.984 | 29.766 | 82 | 2121.5 |
| U-dense-2 | 0 | 0.111 | 421 | 263.9 | 1.016 | 29.719 | 91 | 2144.2 |
| T-dense-2 | 0 | 0.132 | 424 | 311.6 | 1.078 | 29.125 | 79 | 2112.7 |
| T-dense-off | 0 | 0.105 | 426 | 245.7 | 1.016 | nem mért | 73 | 2101.9 |
| U-dense-off | 0 | 0.084 | 424 | 196.9 | 0.953 | nem mért | 76 | 2114.5 |

| Scenario | Capture/input U/T páronként | T producer ns/call | U producer ns/call | Producer U/T |
| --- | --- | --- | --- | --- |
| spread | 0.4521, 0.4068, 0.4500 | 224.88 | 222.32 | 0.9886 |
| dense | 0.7054, 0.8468 | 173.39 | 170.98 | 0.9861 |

A geometriai/topológiai munka élő futásonként eltérhet, ezért az izolált nyereség nem teljes szervergyorsulás. A profilozás ON/OFF többlete látható: TP eredeti spread ON capture 8,822–8,974 s, OFF 7,366 s; supervisor ON 9,766–9,906 s, OFF 8,203 s. ON eredmény nem perturbációmentes production-mérés.

Részidők, node/source/insertion számlálók, producer lifetime maximumok, source/frame/scratch kapacitás és az összes nyers pár: [report-data.json](D:/IxtreemeWorld/build/tc4-20260927-201219/report-data.json), [acceptance.json](D:/IxtreemeWorld/build/tc4-20260927-201219/acceptance.json). A kapacitás becsült konténertárhely, nem allocator peak; a scratch a revision-átmeneti tárhelyet nem teljesen számolja. A forrás/keret méret, munkaszám és working set együttesen értékelendő; nincs új tartós geometriai tároló.

| Run | Commit→decision átlag ms | Wake max lifetime ms | Due→enqueue átlag ms | Queue átlag ms | Deadline miss | Completed samples |
| --- | --- | --- | --- | --- | --- | --- |
| T-spread-1 | 13.915 | 141.308 | 15.070 | 20.501 | 8126 | 49938 |
| U-spread-1 | 12.434 | 141.867 | 13.863 | 21.884 | 7124 | 50507 |
| U-spread-2 | 6.451 | 149.591 | 5.393 | 14.552 | 1144 | 48195 |
| T-spread-2 | 9.441 | 142.584 | 8.245 | 18.848 | 1765 | 46846 |
| T-spread-3 | 9.235 | 135.749 | 8.515 | 18.298 | 2190 | 47165 |
| U-spread-3 | 6.625 | 137.703 | 6.005 | 14.617 | 1615 | 52161 |
| T-spread-off | 8.475 | 139.360 | 7.422 | 17.514 | 2987 | 46268 |
| U-spread-off | 6.515 | 145.048 | 6.091 | 13.448 | 1944 | 53544 |
| T-dense-1 | 6.720 | 145.146 | 10.093 | 0.672 | 639 | 7773 |
| U-dense-1 | 8.475 | 136.515 | 11.037 | 0.710 | 617 | 6016 |
| U-dense-2 | 7.526 | 134.305 | 10.262 | 0.620 | 643 | 8269 |
| T-dense-2 | 7.759 | 67.411 | 12.464 | 1.060 | 529 | 3574 |
| T-dense-off | 6.673 | 71.839 | 10.885 | 0.936 | 483 | 3674 |
| U-dense-off | 6.964 | 70.418 | 9.347 | 0.775 | 488 | 4874 |

A commit késleltetés a koaleszkált első pending update-é, nem minden Position write-é. A maximum lifetime és setupot is tartalmaz. A due/enqueue/worker-start/completion számlálók megadott ablakhoz/populációhoz tartoznak; a deadline miss külön osztály, nem elmaradt wake. A rolling tick p99 legfeljebb 256 minta/zóna történeti eloszlása, nem measure-window globális p99.

Benchmark-memória külön: az új PrepareObservation history legfeljebb 65 536 ×48 bájt (3 MiB) rezervált kapacitás, három fix batch-rekorddal. A korábban is meglévő operation_samples megőrzi a mérés terminált request-handle-jeit; a hook miatt ezek objektuma is nagyobb. Ez consumer/diagnosztikai memória, nem új terrain-residency és nem cache-keretemelés; T/U és H0/H1 kontrollokban azonos. A streamer ledger az ingress/world-owned aktív kéréskeretet számolja; a külső consumer által tovább tartott terminált objektumokat és az OS belső szinkronizációs tárhelyét nem szabad teljes heap-elszámolásnak nevezni. A process working set ezt külön megfigyeli.

## 6. C-moving runtime × harness

Változatlan route-v1 SHA-256 `5DD7D6835D8DC6B5924003C8C4603F60F9B500EDCA517A0B36B7A87359231762`; 100 km, 500 player / 200000 mob, 4 sim / 2 IO worker, 16 MiB, 15 s request deadline. 60 s warmup után a 30 s a batch START ablak. Korábban indult batch később befejezhető; későbbi új batch nem indítható. A következő eligibility az előző tényleges kezdés +10 s, nem utólagos settle. H0 polling / H1 értesítés külön scenario, ugyanaz a három placement és 1500 effect.

| Run | Exit | Started/completed | Kérés | Effect | Actual measure s | Ready median ms | Ready→obs median ms | Request→effect median ms | Peak/budget byte |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| T-H0-1 | 0 | 3/3 | 1500 | 1500 | 31.41 | 0.432 | 19.986 | 46.757 | 16777214/16777216 |
| U-H0-1 | 0 | 3/3 | 1500 | 1500 | 31.24 | 0.474 | 19.938 | 47.044 | 16777214/16777216 |
| U-H0-2 | 0 | 3/3 | 1500 | 1500 | 31.61 | 0.580 | 19.778 | 47.414 | 16777214/16777216 |
| T-H0-2 | 0 | 3/3 | 1500 | 1500 | 31.37 | 0.510 | 19.862 | 47.139 | 16777214/16777216 |
| T-H1-1 | 0 | 3/3 | 1500 | 1500 | 30.01 | 0.442 | 0.008 | 29.535 | 16777214/16777216 |
| U-H1-1 | 0 | 3/3 | 1500 | 1500 | 30.01 | 0.407 | 0.008 | 30.634 | 16777214/16777216 |
| U-H1-2 | 0 | 3/3 | 1500 | 1500 | 30.01 | 0.270 | 0.008 | 30.565 | 16777214/16777216 |
| T-H1-2 | 0 | 3/3 | 1500 | 1500 | 30.03 | 0.334 | 0.008 | 34.272 | 16777214/16777216 |

| C run | Új input | Másolt forrás | Capture s | Query ms | Copy ms | Supervisor CPU s | Worker CPU s | Producer ns/call | Deadline miss |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| T-H0-1 | 802 | 201518 | 1.619 | 57.002 | 15.470 | 17.781 | 4.234 | 180.22 | 0 |
| U-H0-1 | 807 | 204268 | 1.683 | 113.031 | 15.669 | 17.531 | 4.266 | 215.67 | 0 |
| U-H0-2 | 812 | 203041 | 1.805 | 114.165 | 15.737 | 18.859 | 5.875 | 195.32 | 0 |
| T-H0-2 | 828 | 207020 | 1.766 | 58.629 | 15.726 | 19.484 | 4.031 | 198.06 | 0 |
| T-H1-1 | 176 | 51854 | 1.748 | 14.726 | 3.078 | 19.906 | 5.688 | 190.44 | 27 |
| U-H1-1 | 176 | 54338 | 1.689 | 25.492 | 3.398 | 19.562 | 5.031 | 188.81 | 23 |
| U-H1-2 | 176 | 53587 | 1.654 | 28.591 | 3.233 | 18.656 | 4.984 | 182.90 | 7 |
| T-H1-2 | 185 | 52988 | 1.420 | 17.123 | 3.725 | 17.766 | 8.422 | 198.38 | 84 |

A H0/H1 forrás- és munkaszámok a consumer időzítésével együtt változhatnak: a runtime T/U hatását azonos harnessen belül kell összevetni. A sok érintetlen levél esetén a leaf-first út végigvizsgálja a forrásokat; a C query-részidőt ezért a capture többi részével és az elvégzett munkával együtt közlöm, nem rejtem a teljes batch-idő mögé.

| C runtime U/T medián | H0 | H1 |
| --- | --- | --- |
| capture_ns_per_phase | 1.0214 | 1.0108 |
| query_ns_per_copied_source | 1.9708 | 1.6515 |
| producer_wait_hold_ns_per_call | 1.0865 | 0.9560 |
| supervisor_cpu_seconds | 0.9765 | 1.0145 |

Dokumentált trade-off: C-ben a query/source költsége 1,65–1,97× lett, a teljes capture/fázis mediánja viszont csak 1,01–1,02×; itt a hívások több mint 98%-a reuse, így nem a query dominál. Ez nem univerzális geometriai gyorsulás. A C/H0 producer wait+hold átlaga mediánban +8,65%, C/H1-ben −4,40%; a rögzített 1,10-es producer-határon belül. Az ismert kis abszolút költségű trade-offot nem fedtem el új, nem mért választási heurisztikával vagy új indexszel. Részletes párok: [C runtime comparison](D:/IxtreemeWorld/build/tc4-20260927-201219/C-runtime-comparison.json).

| Run | Batch | Planned s | Start s | Prep start s | Prep end s | Completed s | Next eligible s |
| --- | --- | --- | --- | --- | --- | --- | --- |
| T-H0-1 | 1 | 0.000000 | 0.000003 | 0.029644 | 10.515216 | 10.585041 | 10.000003 |
| T-H0-1 | 2 | 10.000000 | 10.605540 | 10.626550 | 20.943826 | 20.987386 | 20.605540 |
| T-H0-1 | 3 | 20.000000 | 21.008126 | 21.009768 | 31.364413 | 31.385495 | 31.008126 |
| U-H0-1 | 1 | 0.000000 | 0.000003 | 0.011943 | 10.394964 | 10.441650 | 10.000004 |
| U-H0-1 | 2 | 10.000000 | 10.462247 | 10.483333 | 20.805021 | 20.848871 | 20.462247 |
| U-H0-1 | 3 | 20.000000 | 20.869850 | 20.882234 | 31.195497 | 31.218153 | 30.869850 |
| U-H0-2 | 1 | 0.000000 | 0.000003 | 0.027916 | 10.630254 | 10.698246 | 10.000003 |
| U-H0-2 | 2 | 10.000000 | 10.718918 | 10.740345 | 21.136602 | 21.205670 | 20.718919 |
| U-H0-2 | 3 | 20.000000 | 21.226293 | 21.234670 | 31.545282 | 31.588233 | 31.226293 |
| T-H0-2 | 1 | 0.000000 | 0.000007 | 0.022187 | 10.499910 | 10.544888 | 10.000007 |
| T-H0-2 | 2 | 10.000000 | 10.565080 | 10.587046 | 20.906715 | 20.950920 | 20.565080 |
| T-H0-2 | 3 | 20.000000 | 20.971329 | 20.975023 | 31.282407 | 31.346098 | 30.971329 |
| T-H1-1 | 1 | 0.000000 | 0.000003 | 0.025318 | 0.739570 | 0.784336 | 10.000003 |
| T-H1-1 | 2 | 10.000000 | 10.008005 | 10.030038 | 10.757051 | 10.780000 | 20.008005 |
| T-H1-1 | 3 | 20.000000 | 20.014631 | 20.038386 | 21.081375 | 21.124399 | 30.014631 |
| U-H1-1 | 1 | 0.000000 | 0.000003 | 0.008384 | 0.569706 | 0.613728 | 10.000003 |
| U-H1-1 | 2 | 10.000000 | 10.002510 | 10.022708 | 10.933704 | 10.956439 | 20.002510 |
| U-H1-1 | 3 | 20.000000 | 20.003448 | 20.004580 | 21.072580 | 21.118170 | 30.003448 |
| U-H1-2 | 1 | 0.000000 | 0.000003 | 0.008698 | 0.632087 | 0.684148 | 10.000003 |
| U-H1-2 | 2 | 10.000000 | 10.008321 | 10.032091 | 10.666821 | 10.688443 | 20.008321 |
| U-H1-2 | 3 | 20.000000 | 20.011779 | 20.032587 | 21.022402 | 21.043770 | 30.011779 |
| T-H1-2 | 1 | 0.000000 | 0.000014 | 0.015325 | 0.982849 | 1.031519 | 10.000014 |
| T-H1-2 | 2 | 10.000000 | 10.012395 | 10.064637 | 10.914101 | 10.983353 | 20.012395 |
| T-H1-2 | 3 | 20.000000 | 20.015561 | 20.047231 | 21.233456 | 21.315843 | 30.015561 |

A request relatív timestampje +1 µs presence-sentinelt tartalmaz; a nanosecundumos összevetésben ez legfeljebb 1 µs offset, nem engedélyezett wake-tolerancia. A per-request trace azonosítóval párosított, ablakváltáskor együtt ürül; overflow/coherence ellenőrzött. A státuszablak accepted/consumed számlálói lifetime értékek lehetnek (setup 500 + measure 1500); a measure 1500 külön trace-ből igazolt. Részletes eloszlások és Ready→Consume→effect adatok a JSON-ban, teljes 1500 sor/run a nyers logokban.

## 7. Tesztek és exact végrehajtás

| Csoport | Natív run | Exit 0 | PASS assertion | FAIL assertion | SKIP |
| --- | --- | --- | --- | --- | --- |
| formal-C | 8 | 8 | 136 | 0 | 0 |
| formal-core | 6 | 6 | 102 | 0 | 0 |
| formal-debug | 11 | 11 | 216 | 0 | 0 |
| formal-live | 14 | 14 | 112 | 0 | 0 |
| formal-probe | 120 | 120 | 240 | 0 | 0 |
| formal-regression | 26 | 25 | 340 | 1 | 0 |
| formal-targeted | 11 | 11 | 385 | 0 | 1 |
| formal-work | 42 | 42 | 168 | 0 | 0 |

Az assertionösszeg nem független tesztesetszám. [Összes exact CLI/native exit/idő/hash](D:/IxtreemeWorld/build/tc4-20260927-201219/all-formal-runs.csv). Csoportonként a `*-plan.json` és a `results/formal-*/runmanifest.json` a teljes parancslista. A buildlogok a specifikált Windows RelWithDebInfo és Debug CMake targetekhez tartoznak; a két konfiguráció performance-adatait nem kevertem.

Megőrzött temporal 38; új temporal 13: exact boundary, valós warp és szinkron publikáció, változatlan bemenet később megváltozó mob-eligibility, kétzónás koherens cut/post-cut, retained frame, változatlan forrás melletti split/merge/incarnation, hibás kihagyott pozitív levél és régi topológiai cache negatív kontroll. Új wait 15: Ready/6 terminal státusz, dupla Consume, 64 enrollment-race, determinisztikus predicate→sleep Ready, spurious, elnyomott completion és notify, cancel, consumer-drop lifetime. Admission további három lifecycle ellenőrzés: stop-wake, Ready nem Consume/unpin, cancel/consume/deadline owner-precedence. A Debug ugyanezeket, valamint activity/snapshot/streamlife/splitmerge eseteket futtatja.

Targeted: streamadmission, streaming, worldquery, streamlife, map4, snapshot, terrain, worldpackage strict/legacy, mapsplit, mapaudit, bootstrap. Regresszió: field/loadfield/partitionscore selftest, routing, lod, activity, loadfield, partitionscore, stability, splitmerge, ghost, aoi, replication, tickrate, inputpath, netstress, presence, asfdeterminism, workerpool, replv2, protocol, hygiene, reclamation 100/1000 és smoke.

## 8. Scheduler és legacy

Új fix munkaszerződés: 64 task × 1 000 000 iteráció, checksum `15990358326065361963`; 7 váltott pár worker-számonként. Gate: U/T medián wall ≤1,10; mindkét változat 2/4 workeres mediánja gyorsabb a saját 1 workerénél.

| Worker | T ms median [min,max] | U ms median [min,max] | U/T |
| --- | --- | --- | --- |
| 1 | 123.987 [122.020, 125.842] | 123.924 [123.290, 125.054] | 0.9995 |
| 2 | 62.507 [61.521, 65.282] | 61.736 [61.275, 64.758] | 0.9877 |
| 4 | 32.229 [31.625, 34.330] | 31.939 [31.455, 33.211] | 0.9910 |

A régi `--mode scheduler --seconds 3` parancs és az SL-2 runner változatlan. Az új összesítő csak a pontos uniform-parallelism/reference-parallelism neveket kezelheti diagnosztikaként, teljes új scheduler PASS mellett. Hiányzó kimenet, más hiba, crash, timeout vagy eltérő natív exit blokkol. Az összesítő negatív kontrollja ezt külön teszteli. A korábbi TC és T0/v1 sikertelen futásait nem minősíti át.

| Legacy/diagnosztika | Native exit | Nyers hibák |
| --- | --- | --- |
| legacy-scheduler | 2 | SCHED uniform-parallelism: FAIL |
| T-H0-1 | 0 | nincs |
| T-H0-2 | 0 | nincs |
| U-H0-1 | 0 | nincs |
| U-H0-2 | 0 | nincs |

## 9. Negatív eredmények és korrekciók

Az első globális „már minden levél pozitív” korai kilépési ötletet implementáció előtt elvetettem: a source-vektor zónánként rendezett, ezért az utolsó érintett levél későn is megjelenhet. Nem állítok hozzá mért nyereséget.

Az első TP C poll-trace hibásan párosította a measure requesteket a setup megfigyeléseivel. A raw log megmaradt, a párok INVALID; abból nem származik consumer-késleltetési következtetés. A listák közös resetje és request-id/time coherence assertion javította; a javított T-pre ismétlés a bizonyíték.

Fejlesztői buildhibák: Windows min/max makróütközés (NOMINMAX), hiányzó teljes MobSpawnPoint típusinclude (SpawnLoader). Az új negatív temporal fixture először sugáron kívüli mobot, majd frissen Full tierben spawnolt mobot használt; ezért a negatív előfeltétel nem állt fenn. A fixture valós warp és valódi LOD-evaluatorral előállított régi üres generáció segítségével javult. Production hysteresis és validator nem változott. A régi sikertelen snapshotok és logok a T-pre/T-control és buildlogok alatt megmaradtak.

A formal natív nemnulla futások nem tűnnek el: legacy-scheduler=2.

A megőrzött korábbi T-control binárison a hibás (nem lehűtött mobot használó) negatív fixture-t külön újrafuttattam: native exit 2, 12 PASS / 1 FAIL. Ez történeti fejlesztői reprodukció, nem a javított U mátrix eredménye; a raw log és manifest a `results/development-fixture-negative/` alatt van.

## 10. Maradék és stop

Az aktuális lefuttatott Windows szerződésben nincs nyitott blokkoló gate.

Linux, FreeBSD, ASan és TSan nem futott megfelelő helyi környezet hiányában; Debug nem sanitizer. A symlink/junction teszt környezeti SKIPPED, nem PASS. Nem történt toolchain-telepítés. A process working set és konténerkapacitás nem teljes allocator-profil; a CPU-mérés Windows thread-accounting felbontású. Az élő topológia és háttérterhelés eltérései minden általánosítás korlátai.

Nem született 50 000 player / 1–2 millió mob readiness-állítás, feltétlen faliórás 50 ms completion-garancia vagy nagyobb cache-re épülő PASS. A korábbi startupbizonyíték történeti; a startup/shared protocol/map-loader út TC-4-ben változatlan, strict/legacy package és bootstrap újrafutott.

A munka itt megáll review-ra; további fázis nem indult.
