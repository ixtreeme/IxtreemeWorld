# Claude végrehajtási megbízás – IxtreemeWorld I1-FINAL élő admission folytatás

## Szerep és kiindulási pont

Ezt a dokumentumot a következő Claude-munkamenet elsődleges átadási és végrehajtási specifikációjaként használd. A feladat az IxtreemeWorld I1-FINAL Verified Server Live Acceptance IF-0 → IF-5 munkájának folytatása, nem új audit és nem újratervezés.

Az elsődleges working tree:

    D:\IxtreemeWorld

A working tree szándékosan dirty. A korábbi MAP, SL, TC, I0/I1, DB-pool és IF munkát meg kell őrizni. Tilos resetelni, stash-elni, clean-elni, commitolni, pusholni vagy korábbi bizonyítékot törölni. A meglévő test_zone és a strict fixture nem írható felül.

A cél a már elkészült StandaloneVulkanClear I1 admission útjának valódi helyi elfogadása az igazolt U-final + DB-pooljavítás gameserverrel. I2–I6, mapexport, actor- és movement-integráció, gameplay, renderer, Unity és teljes szerverprotokoll-átépítés kívül esik a scope-on.

Ha az idő- vagy limitkeret elfogy, először írj részletes állapotjelentést Markdownba a jelenlegi futáskönyvtárba, a meglévő fájlok felülírása nélkül. A jelentés tartalmazza a végrehajtott lépéseket, időpontokat, PID-eket, logfájlokat, PASS/FAIL/NOT_RUN állapotokat és a következő biztonságos lépést.

## Biztonsági és megőrzési szabályok

1. A két engedélyezett helyi tesztfiók használható, de az azonosítókat, jelszavakat, tokeneket és handoff tokeneket ne írd bele ebbe a promptba, chatbe, parancssorba, konfigurációba, naplóba, screenshotba vagy evidence-fájlba. A hitelesítő adat csak a helyi GUI interaktív mezőjébe kerülhet.
2. Ne használj auth-bypass-t, univerzális tokent, kitalált credentialt vagy synthetic/legacy admission módot.
3. Ne emelj timeoutot, ne csökkents teljesítményküszöböt, ne növeld a cache-t, ne csökkentsd a populációt, ne lazíts validátort, ne módosítsd a checked strict test_zone-t.
4. Ne indíts új játék- vagy loginserver-példányt a már futó szolgáltatás mellett. Ne futtasd újra a cutovert, és ne állítsd le automatikusan a jelenlegi szolgáltatásokat. Ha tényleges szolgáltatáscsere kellene, állj meg és kérj külön jóváhagyást.
5. Az X-close bizonyításához natív ablakbezárást használj az ablak jobb felső sarkában. Ne Alt-F4-ezz, ne öld meg a folyamatot, és ne tekintsd a Noesis belső shutdownját X-close bizonyítéknak.
6. Minden live eset legyen reprodukálható, időbélyegzett, és ügyfél- valamint gameserver-loggal párosítva.

## Könyvtárak és artefaktumok

Szerver/forrás:

    D:\IxtreemeWorld\gameserver
    D:\IxtreemeWorld\loginserver
    D:\IxtreemeWorld\shared

StandaloneVulkanClear:

    D:\AurigaGlobal\LiveWork\StandaloneVulkanClear

Végső kliens bináris:

    D:\IxtreemeWorld\build\client-i01-20260928\client-build\Debug\VulkanClear.exe
    SHA-256: 3E1230383379A8C486B20D8EC8566864C7423A97B8FBA21D3FADB62F481F3405

A kliens forrása és assetjei az I1 összehasonlítás szerint változatlanok a végső I1 kliensforráshoz képest. Ezen a folytatáson ne fordítsd újra és ne módosítsd, hacsak bizonyított acceptance-blokkoló hibához nem szükséges; ilyenkor előbb írj diagnosztikai bizonyítékot.

Aktuális futáskönyvtár:

    D:\IxtreemeWorld\build\client-i1-final-20260928-1910

Fontos fájlok:

- live\gameserver.conf
- live\client-A.conf, live\client-B.conf, live\client-A-reconnect.conf
- live\client-A.log, live\client-B.log, live\gameserver.log
- live\cutover-executed.json
- fixture\
- source-final-v2.json, source-delta.json, changes-v2.patch, binaries-final-v2.json
- preflight-results-verified.json, preservation-preflight-v2.json, protected-before.json
- db-liveness.json, presence-v2.json, protocol.json, snapshot.json, regression-results.json
- summary-preflight.json – régi, cutover előtti összegzés; jelenlegi live állapothoz nem autoritatív.

A végső riport javasolt helye:

    D:\IxtreemeWorld\docs\client-compatibility\standalone-i1-final-live-review.md

Ha már létezik, ne töröld a történeti riportot; készíts dátumozott kiegészítést vagy append-only változatot.

## Szerveroldali változások

### MAP, streaming és world runtime

A korábbi MAP/SL/TC munkákban elkészült a strict file-world package út, WorldPackage/MapData séma és writer/reader, terrain service/streamer/demand/request, server terrain/water, partition és zone scheduler/worker pool, migration, AOI/replication, movement/collision, spawn és WorldValidator. Elkészültek az activity field, sleep/wake scheduler, wake-capture, LOD/load-field/ASF diagnosztikai és benchmark harness-ek is. Ezeket az IF-feladatban ne nyisd újra, és ne cseréld új streaming-, ASF- vagy scheduler-architektúrára.

A strict warp policy és startup validation él. A checked test_zone csak a dokumentált strict útvonalon érvényes; fallback, legacy vagy synthetic world package nem megengedett.

### DB pool liveness javítás

Fő fájlok:

    gameserver/libs/db/src/DbPool.cpp
    gameserver/libs/db/include/db/DbPool.h
    gameserver/libs/db/CMakeLists.txt
    gameserver/libs/db/tests/DbPoolLivenessTest.cpp

A javítás lényegi viselkedése:

- AcquireConnection() a slot kivétele után feloldja a pool mutexet, mielőtt hálózati I/O indul.
- EnsureConnection() a munka előtt ellenőrzi az isClosed()/isValid() állapotot, és szükség esetén újranyit.
- Sikertelen reopen esetén a slot visszakerül a poolkapacitásba.
- Submit és SubmitVoid nem játssza újra az üzleti callbacket.
- A timeoutok nem lettek megemelve.
- Az opt-in liveness teszt 60 assertionnel PASS lett; saját DB-sessiont használt, session-scoped wait_timeout-tal, üzleti account/character/token/schema változtatás nélkül.

Ez a tartós pooljavítás a live gameserver új binárisában benne van. Az IF-live acceptance nem módosíthatja újra az adatbázist és nem ismételhet automatikusan üzleti DB-műveletet.

### I1 helyi observation delta

Fő fájlok:

    gameserver/apps/gameserver/src/world/debug/I1LocalObservation.h
    gameserver/apps/gameserver/src/world/WorldRuntime.*
    gameserver/apps/gameserver/src/main.cpp
    gameserver/apps/gameserver/tests/HardeningBench.cpp

A WorldRuntime::SnapshotContext megkapja a const PresenceRegistry& presence referenciát, a RunSnapshotBatch() pedig a spawn_.Presence() állapotból készít koherens snapshotot. A main.cpp opcionális i1_local_observe_characters beállítással indítja az observert; default esetben ki van kapcsolva, és shutdown előtt leáll.

Az observer ugyanahhoz az epoch/world_tick/captured_at ponthoz kötött mintában követi ezt a láncot:

    CharacterId
      -> PresenceRegistry
      -> SessionId / PlayerBinding
      -> NetId
      -> OwnerMap
      -> active leaf
      -> living entity

Az observer csak olvas és számol; nem ürít command queue-t, nem kér validációt, nem enged ki nyers pointert, legfeljebb egy pending requestet tart, és safe shutdown után megáll. A két live karakter szűrője jelenleg 3,4; ezt ne változtasd meg indokolatlanul.

A fixture-kezdő hibát, amelyben 901/902 szerepelt a MakeCharacter(index) által ténylegesen képzett 9000+ index ID-k helyett, javították. A presence-v2 teszt exit 0, mind a 7 új ellenőrzés PASS.

### Strict fixture

A live célvilág a korábbi SL2 N2 startup debug fixture byte-identical strict másolata:

- 7 fájl, összesen 17 844 byte.
- v3 world package, 256×256 m, 4 m cellák, 4 chunk.
- 1×1 region/leaf, eager terrain 16 KiB.
- Player spawn rectangle: (10,60)–(20,70), area 1.
- Egy strict valid warp (60,60)–(64,64) → (200,200).
- Két TestDummy mob.
- Manifest SHA-256: B2443D7A946B8FF27876BBC9E9FC16E2A305D0F4D882C41A08A19B2FB913FBC0.
- File-set hash: FD03550DB38F5FC9052D4E2E6B1CF761F4606E0EE7F262FBA4727A3A05A1B678.

Strict final validation, startup-check és a checked test_zone 413-as elutasítási tesztje PASS. Hiányzó vagy korrupt package esetén a szerver DB/listener előtt leáll; nincs fallback.

## Kliensoldali változások

A StandaloneVulkanClear I0/I1 admission implementációja az Admission/Admission.cpp és Admission/Admission.h környékén, valamint a kapcsolódó session/transport/framing/codec kódban készült el. A kliens:

- explicit IxtreemeWorld profilt használ;
- numerikus IPv4 címmel csatlakozik 127.0.0.1:11000 loginhoz és csak az engedélyezett 127.0.0.1:11020 game endpointot fogadja el;
- bounded queue-kat és parser-hardeninget használ;
- 4-byte big-endian hosszmezős framinget és a meglévő Cap’n Proto/binary codec útvonalat használja;
- login handshake → auth → character list → exact uint64 character select → opaque handoff token → új game socket/generation → game handshake → EnterWorld → Accepted állapotgépet hajtja végre;
- login után törli a credential memóriabeli példányát, tokent/jelszót nem logol és nem tartósít;
- admission-only sink: az Accepted státusz kódja admission_accepted_world_view_and_movement_not_integrated;
- nem tartalmaz map-exportot, actor spawn/render-integrációt, movementet, combatot vagy Unity-integrációt.

A timeoutok változatlanok: connect 5 s, phase 10 s, character select 30 s, EnterWorld 30 s, observation 10 s. Ezeket nem szabad megemelni.

A live GUI indító:

    D:\IxtreemeWorld\build\client-i1-final-20260928-1910\launch-gui.py

Két külön secret-free konfigurációt és külön logot használ. A kliens GUI-logokban attempt, phase, character ID, saját net ID, spawn és tick/world frame adatok szerepelnek, credential nem.

## Cutover jelenlegi állapota

A cutover már megtörtént. Ne futtasd újra a cutover.ps1-t.

Loginserver:

- PID 39580
- Start 2026-09-28 20:12:59 +02
- EXE D:\IxtreemeWorld\build\i1-auth-fix-20260928\out\loginserver\Debug\loginserver.exe
- SHA-256 9626E5BD97531E46479C359EB327417823F342D7BFDC4B0C79D1709E29625F24
- Port 0.0.0.0:11000
- A loginserver binárisa és processze a cutovernél változatlan maradt.

Régi gameserver:

- PID 25312
- régi U-final jelölt SHA F96114D2C3E8F8D3CBCB9C9DE6E635433B0733B915483D5A4A0C3D589D1DFF88
- preflight után explicit, kényszerített Stop-Process állította le; ezt forced stopként, nem graceful shutdownként kell jelenteni.

Jelenlegi gameserver:

- PID 25628
- Start 2026-09-28 22:30:57 +02
- EXE D:\IxtreemeWorld\build\client-i1-final-20260928-1910\out\gameserver\Debug\gameserver.exe
- SHA-256 0A8E978F47039530E71F831D8C31FA7F643538FB7042EF3B698A0DBBCDDFFB4A
- Port 0.0.0.0:11020
- A futtatási parancs a live gameserver.conf, a meglévő database.json és --warp-policy strict beállítást használja.

Ellenőrzéskor mindig friss Get-Process/Get-NetTCPConnection állapotot vegyél fel; ne csak a fenti PID-ből indulj ki. Ha PID újraindult vagy port foglalt, dokumentáld és ne cseréld le automatikusan.

## Offline és preflight eredmények

Már bizonyított:

- DB pool liveness: PASS, 60 assertion.
- Protocol regression: PASS.
- Presence-v2: PASS, exit 0, 7 új célzott ellenőrzés.
- Netstress: PASS.
- World package: 140 PASS, 1 környezeti SKIP.
- Snapshot: PASS; 10 churn cycle, 3 reader, 214 capture, self-wait/nested request/shutdown drain.
- Kis readiness smoke: PASS; 2 km, 2 player, 20 mob, 2 s warmup + 4 s mérés, LOD/ASF bekapcsolva.
- Strict fixture validation/startup/negative fallback: PASS.

Megmaradt, nem elrejtett korlát:

- Debug bootstrap 1024 FAIL a változatlan threshold mellett. Új build konstrukció kb. 8678 ms, supervisor kb. 6.00 ms, RSS kb. 1529 MB ≤ 2048 MB, threshold 5000 ms/5 ms. A megőrzött U-final Debug kb. 9028 ms/12.18 ms FAIL volt. Az optimized, történeti 1821 ms más profilból származik. Ezt ne kozmetikázd és ne állítsd PASS-ra.

Az első hibás presence fixture és az első preflight harness failure megőrzött történeti bizonyíték; ne töröld őket.

## Eddigi live eredmények

### Attempt 2

Mindkét kliens külön elérte az Accepted állapotot, de A megfigyelési ablaka lejárt, mielőtt B belépett. Így:

- egyéni admission: PASS;
- átfedő authoritative presence: NOT_COMPLETED / nem bizonyított;
- X-close: nem történt;
- friss reconnect: ezen az új gameserveren nem bizonyított.

### Attempt 3

A klienslogok szerint mindkét GUI sikeresen belépett:

- A: character 3, own net ID 3, spawn 15,65,1.4375, Accepted, elapsed kb. 771 ms.
- B: character 4, own net ID 4, spawn 15,65,1.4375, Accepted, elapsed kb. 2884 ms.

A gameserver observer logban epoch 989 környékén több koherens minta látható, például tick 10067, ahol character 3/session 3/net 3 és character 4/session 4/net 4 ugyanabban a mintában szerepel; mindkettőnél registry=1, reverse=1, owner=1, zone=1, leaf=1, binding=1, alive=1, bindings=1, entities=1, owners=1, queued=0, coherent=1. Ez az authoritative presence átfedésének legerősebb bizonyítéka, és megfelelő log-párosítással L1 overlap PASS-ként rögzíthető.

A kliensek ezután az observation befejezése miatt Noesis shutdownba kerültek. Ez nem X-close. A későbbi gameserver-logban a sessionök és presence rekordok 0-ra tisztultak. Továbbra sincs bizonyítva:

- Accepted állapotban végzett natív X-close A-n;
- B Accepted túlélése A lezárása után;
- A stale presence/session/net/owner rekordjainak eltűnése X-close után;
- friss login → character select → handoff → új game socket/generation → Accepted reconnect ezen a cutover utáni gameserveren.

A GUI processzállapotot minden folytatás elején frissen ellenőrizd.

## Kötelező folytatási sorrend

### 1. IF-0 állapotellenőrzés

1. Ellenőrizd a working tree-t és készíts append-only állapotpillanatképet.
2. Ellenőrizd a két portot és a login/game PID-eket. Ha PID vagy SHA eltér, dokumentáld; ne cseréld le automatikusan.
3. Ellenőrizd a három live log végét, és hogy nincs futó vagy félbemaradt case.
4. Ellenőrizd, hogy nem maradt aktív régi kliensfolyamat.
5. A summary-preflight.json helyett friss live logokat és cutover-executed.json-t használj.

### 2. IF-1/L1 overlap bizonyíték

Az attempt 3-at gépileg párosítsd:

- mindkét kliens Accepted sorával;
- a gameserver megfelelő I1-OBS epoch/tick soraival;
- character/session/net/owner/leaf/living entity lánccal;
- ugyanazon falióra-időablakkal.

Ne csak kliensszövegből következtess presence-re. Az overlap csak akkor PASS, ha mindkét karakter ugyanabban a közös observer mintában coherent=1. Attempt 2-t ne jelöld overlap PASS-nak.

### 3. IF-2/L2 kontrollált Accepted X-close

Egyetlen kontrollált új párost futtass a már futó szervereken:

1. Indítsd a két végső klienst a meglévő launch-gui.py és secret-free configok útján.
2. Credential csak a GUI login mezőjébe kerülhet; ne mentsd és ne screenshotold.
3. Mindkét kliensnél LOGIN, majd Enter selected character.
4. Várd meg, hogy mindkettő Accepted legyen, és legyen legalább egy közös koherens observer minta.
5. Rögzítsd A/B Accepted időt, character ID-t, own net ID-t, közös epoch/tick-et.
6. A valódi A ablak jobb felső sarkában kattints a natív X-re. Jegyezd fel a kattintás idejét; ne Alt-F4-ezz és ne öld meg a folyamatot.
7. Ellenőrizd A session/presence/reverse/owner/living entity cleanupját.
8. Ellenőrizd, hogy B Accepted és authoritative presence-e megmarad A takarítása alatt.
9. Ezután B-t külön, kontrolláltan zárd le, és ellenőrizd a teljes cleanupot.

Ha a próba nem éri el az egyidejű Accepted/overlap állapotot az eredeti timeoutok alatt, ne ismételd vakon és ne hosszabbíts timeoutot. Jelöld NOT_COMPLETED-nek, mentsd a logokat, és írd le az okot.

### 4. IF-3/L3 friss reconnect

A cleanup után, új folyamatból és új attempt számmal futtasd a client-A-reconnect.conf útvonalat:

- login handshake;
- auth;
- character list;
- exact uint64 character select;
- opaque handoff token;
- új game socket és generation;
- game handshake;
- EnterWorld;
- Accepted.

Bizonyítsd, hogy nem régi socketből, régi tokenből vagy stale presence-ből sikerült. Rögzíts új session/net ID-t, előző rekord cleanupját és új Accepted sort. Ha B még él, ellenőrizd, hogy nincs kettős ownership.

### 5. IF-4 regresszió és csomag

A live eset után csak szükséges célzott ellenőrzéseket futtasd:

- port/PID/SHA és config hash;
- login/game process és listener;
- kliens admission logok titokmentessége;
- presence observer koherenciája és shutdown drain;
- DB pool liveness korábbi PASS bizonyítéka;
- strict fixture hash;
- nincs új, nem engedélyezett working-tree módosítás.

Ne futtasd újra az összes nehéz benchmarkot pusztán új log kedvéért. Forrásmódosítás esetén külön diff, build és regresszió szükséges; live acceptance önmagában nem indokol új implementációt.

### 6. IF-5 záró review és STOP

A végén készíts gépi összegzést és emberileg olvasható riportot, majd állj meg. Ne kezdd el I2–I6-ot.

Javasolt új artefaktumok:

    D:\IxtreemeWorld\build\client-i1-final-20260928-1910\live\live-cases.json
    D:\IxtreemeWorld\build\client-i1-final-20260928-1910\live\runs.csv
    D:\IxtreemeWorld\build\client-i1-final-20260928-1910\live\if5-summary.json
    D:\IxtreemeWorld\docs\client-compatibility\standalone-i1-final-live-review.md

A meglévő fájlokat ne írd felül; foglalt névnél dátumozott suffixet használj.

## Acceptance gate-ek

| Gate | Jelentés | Jelenlegi állapot |
|---|---|---|
| G0 | Eredet-, fixture-, binary-, PID- és portazonosság | PASS preflight; cutover után újra ellenőrizendő |
| G1 | Valódi login/auth/character select | PASS attempt 3 |
| G2 | Game handoff, új socket/generation, EnterWorld | PASS attempt 3 |
| G3 | Mindkét kliens egyidejű Accepted | PASS attempt 3 klienslogok alapján |
| G4 | Átfedő authoritative presence közös koherens observer mintában | PASS-jelölt attempt 3; gépileg párosítandó |
| G5 | Natív X-close után A cleanup és B túlélése | NOT_RUN / OPEN |
| G6 | Friss reconnect új session/net/generationnel | NOT_RUN / OPEN |
| G7 | Regression, strict world, secret-free evidence, no workaround | Offline PASS; live utáni ellenőrzés szükséges |
| G8 | IF-5 review-ready összegzés és STOP | Csak G5/G6 után vagy explicit NOT_COMPLETED riporttal |

A teljes IF-5 PASS csak G0–G7 tényleges bizonyítása után adható. Ha G5 vagy G6 nem futtatható, a végső státusz legyen NOT_COMPLETED vagy BLOCKED, pontos okkal; ne adj mesterséges PASS-t.

## Riportkövetelmények

A végső Markdown-riport tartalmazza:

1. Kiindulási állapot és védelem: dirty tree, megőrzött artefaktumok, aktuális PID/SHA/port.
2. Szerverváltozások: DB pool javítás, I1 observer, strict fixture és releváns forrásfájlok.
3. Kliensváltozások: admission state machine, framing, handshake/handoff, endpoint allowlist, admission-only határ.
4. Bizonyíték: pontos klienslog-sorok, gameserver observer epoch/tick, character/session/net/owner lánc.
5. Páros élő mérés: A/B Accepted idő, overlap epoch/tick, X-click idő, A cleanup, B survival, reconnect session/net/generation.
6. Eredmény: G0–G8 táblázat PASS/FAIL/NOT_RUN/BLOCKED állapottal.
7. Korlátok: Debug bootstrap threshold FAIL, környezeti SKIP, timeouton belüli reprodukciós hiány, GUI vagy szolgáltatási blokk.
8. Nyitott döntések: csak az IF-5 után szükséges I2–I6 döntések; implementációt ne kezdj.
9. Titokellenőrzés: credential/token nem került logba vagy evidence-be.

A riport végén explicit írd le: STOP – IF-5 review, és hogy commit/push/reset/stash nem történt.

## Rövid állapotellenőrző parancsok

Ezek csak olvasási ellenőrzések:

~~~powershell
Get-CimInstance Win32_Process -Filter "Name='gameserver.exe' OR Name='loginserver.exe' OR Name='VulkanClear.exe'" |
  Select-Object ProcessId,ParentProcessId,ExecutablePath,CommandLine,CreationDate

Get-NetTCPConnection -State Listen -LocalPort 11000,11020 |
  Select-Object LocalAddress,LocalPort,OwningProcess,State

Get-Content D:\IxtreemeWorld\build\client-i1-final-20260928-1910\live\gameserver.log -Tail 200
Get-Content D:\IxtreemeWorld\build\client-i1-final-20260928-1910\live\client-A.log -Tail 120
Get-Content D:\IxtreemeWorld\build\client-i1-final-20260928-1910\live\client-B.log -Tail 120
~~~

Jelszó és token ezekben a parancsokban soha nem szerepelhet. A GUI indításkor csak a már meglévő launch-gui.py útvonalat használd; credential ne legyen command line argumentum.

## Végrehajtási döntési szabály

A következő Claude-agent akkor jár el helyesen, ha a jelenlegi cutover utáni állapotból folytat, az attempt 3 overlap bizonyítékát precízen összepárosítja, majd legfeljebb egy kontrollált Accepted X-close + fresh reconnect ciklust futtat. Ha ez sikerül, készítse el az IF-5 riportot és álljon meg. Ha nem sikerül, őrizze meg a teljes bizonyítékot, adjon őszinte NOT_COMPLETED/BLOCKED státuszt, írjon részletes folytatási jegyzetet, és szintén álljon meg.

Semmilyen eredmény nem indokolja a timeout, cache, populáció, validátor, strictness vagy admission-contract megváltoztatását.

