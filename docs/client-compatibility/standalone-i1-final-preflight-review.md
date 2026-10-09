# I1-FINAL — előkészítési review (IF-0–IF-2)

2026-09-28. Az izolált előkészítés elkészült. **Az átállás és az új live tesztek még nem történtek meg: BLOCKED: cutover approval.** A futó loginserver és a régi gameserver változatlan.

[Részletes folytatási napló](D:/IxtreemeWorld/build/client-i1-final-20260928-1910/HANDOFF.md) · [Konkrét átállási terv](D:/IxtreemeWorld/build/client-i1-final-20260928-1910/cutover-record.md) · [Gépi összesítés](D:/IxtreemeWorld/build/client-i1-final-20260928-1910/summary-preflight.json)

## Eredet és megőrzés

Az U-final 227 fájlos leltárában az ismert három DB-fájl és a korábbi tc4_acceptance.py szkript tért el. Az új DB-tesztet és az aktuális 214 forrásfájlt külön leltároztam. A kliens mind a 451 ellenőrzött fájlja változatlan. A 4847 védett fájl ellenőrzése csak a feladathoz tartozó négy meglévő forrásfájl módosítását találta; váratlan eltérés nincs. Az új megfigyelő header külön hozzáadás.

A checked test_zone, a kliens assetjei, az eredeti Auriga és a korábbi bizonyítékok megmaradtak. Nem történt DB-séma-, account- vagy karaktermódosítás, illetve commit, push, reset vagy stash.

Új, elkülönített VS18/MSVC19.51 x64 Debug build készült C++20 és statikus /MTd profillal, meglévő vcpkg-függőségekből. Nem történt telepítés vagy loginserver-újraépítés. A tényleges fordítási egységek és linkelt könyvtárak lenyomatai a build-inputs.json fájlban szerepelnek.

A végső gameserver SHA-256 értéke:
`0A8E978F47039530E71F831D8C31FA7F643538FB7042EF3B698A0DBBCDDFFB4A`.

Az aktuális leltárak: source-final-v2.json, binaries-final-v2.json, changes-v2.patch. A korábbi, v2 nélküli fájlok a fejlesztői teszt első állapotát őrzik; nem írjuk őket felül.

## Megfigyelési változtatás

A cél neve **U-final + DB-fix + I1-local-observation**. Érintett fájlok: main.cpp, WorldRuntime.h/cpp, új world/debug/I1LocalObservation.h és HardeningBench.cpp.

A meglévő quiescent snapshot API ugyanazon epoch, world tick és monoton időpont alatt másolja a két karakter registry → session → binding → NetId → owner → aktív leaf → élő authoritative entity kapcsolatait. A legutóbbi session/NetId megőrzése lehetővé teszi a bontás után maradó owner vagy entity kimutatását. Ghost nem számít második authoritative entitásnak.

A megfigyelő nem ürít parancssort, nem hív állapotot előkészítő validációt, és nem módosít világállapotot. A hiányos spawn/despawn állapot nem lesz konzisztensnek minősítve. Snapshoton kívül nincs nyers world/zone/entity mutató. Alapból kikapcsolt; explicit kétkarakteres konfiggal kapcsolható be. Maximum egy függő kérés, 250 ms-os polling és legfeljebb 6000 poll engedett. Az observer leállítása megelőzi a világ leállítását. Nincs új adminport vagy távoli parancsfogadó.

## Strict fixture

A megőrzött SL-2 N2 startup Debug server_only_v3 csomag bájtazonos, külön másolatát használjuk: hét fájl, 17 844 byte, v3 formátum, 256×256 méter, 4 méteres cellák, négy chunk. A partíció egy region és egy leaf, így teljesíti a 240 méteres kezdeti AOI minimumot. Az eager terrain 16 KB. Megmaradt a két eredeti TestDummy is; ez nem nagy populációs mérés.

A player spawn területe (10,60)–(20,70), area=1. Az egyetlen warp triggere (60,60)–(64,64), célja (200,200), strict módban érvényes. Nem készült új konverter, mapexport vagy kézi bináris javítás.

A végső gameserverrel öt kontroll sikeres:

- Teljes strict validáció: nulla hiba és nulla figyelmeztetés.
- Valódi startup-check a külön 21120-as porton: világ, DB-pool és listener felépült, majd tisztán leállt.
- A checked test_zone továbbra is a várt 413 hibával elutasított.
- Hiányzó és sérült csomag esetén megállás a DB és a listener előtt, fallback nélkül.

Ez még nem a két valódi fiók auth/admission elfogadása.

## Tesztek

| Próba | Eredmény |
|---|---|
| Három Debug target buildje | exit 0, 133,36 s |
| Tesztfixture javítása utáni worldbench build | exit 0, 10,42 s |
| DB-liveness | exit 0, 60 ellenőrzés, 4,83 s |
| Protocol | exit 0, 3,97 s |
| Presence v2 | exit 0, 21,68 s; hét új jelentett eset és a meglévő csomag |
| Netstress | exit 0, 14,06 s |
| Worldpackage | exit 0, 140 PASS, 1 környezeti SKIP, 12,53 s |
| Snapshot | exit 0, 2,44 s; 10 churnciklus, 3 reader, 214 collector |
| Kis readiness | exit 0, 6,29 s; 2 km file/eager világ, 2 player, 20 mob |
| Debug bootstrap, 1024 zóna | FAIL, részletek alább |

A snapshot csomag a supervisor saját future-jére várás és a beágyazott kérés tiltását, valamint a shutdown során függő kérés kiszolgálását is ellenőrizte. Az új observation tesztek lefedik a két konzisztens presence-t, a hiányzó owner/registry negatív kontrollját, a peer megmaradását cleanup alatt, a filter elutasításait, a parancssor változatlanságát és a stop körüli lifetime utat.

A readiness 2 s warmup és 4 s mérés mellett, bekapcsolt LOD/ASF-fel futott, validation OK eredménnyel. Nem igazol 200k/7000 kapacitást. Az assertionszám nem független tesztesetszám. Exact CLI, cwd, UTC, elapsed és native exit a run JSON-okban és a runs-preflight.csv-ben található. A változatlan kliens korábbi 851 ellenőrzése történeti bizonyíték, nem új futás.

### Megőrzött fejlesztői hibák

Az új presence fixture első filtere nem vette figyelembe, hogy a MakeCharacter(index) helper 9000+index ID-t állít elő. Három új ellenőrzés ezért FAIL lett. A teszt most a helper tényleges ID-jéből képezi a filtert; a production megfigyelő nem változott. A presence-v2 sikeres, a régi FAIL és binárishash megmaradt.

Az első preflight kiértékelő egy nem létező PACKAGE-REPORT szöveget várt a sikeres validáció után. A javított kiértékelés a tényleges RESULT: VALID és kötelező layerjelzők alapján készült. A szerver eredménye nem változott, a nyers log és az első kiértékelés megmaradt.

### Debug bootstrap korlát

| Azonos 1024 zónás Debug próba | Construction, határ 5000 ms | Supervisor, határ 5 ms | RSS, határ 2048 MB |
|---|---:|---:|---:|
| Új build, első futás | 8685 ms — FAIL | 5,98 ms — FAIL | 1529 MB — PASS |
| Megőrzött U-final Debug | 9028 ms — FAIL | 12,18 ms — FAIL | 1529 MB — PASS |
| Új build, páros kontroll | 8678 ms — FAIL | 6,00 ms — FAIL | 1529 MB — PASS |

Az időkeret túllépése a megőrzött U-final Debugon is reprodukálható. Ez dokumentált Debug performance-korlát marad: nincs küszöbcsökkentés, timeoutemelés vagy mesterséges PASS. A korábbi optimized bootstrap 1821 ms-os eredménye más buildprofilhoz tartozik. A kiválasztott egyzónás admission fixture sikeres indulása külön bizonyíték.

## Engedélykapu és folytatás

A konkrét cutoverkérdés elküldve; válaszra várunk. A specifikáció 14. pontja kifejezetten megköveteli ezt az egyszeri jóváhagyást, mert a korábbi restartengedély a régi gameserverre szólt.

A cutover-record.md rögzíti a régi PID/start/hash-t, az új exe-t, a strict fixture és konfig lenyomatát, a külön naplókat, az explicit kényszerített stopot, a feltételes rollbacket és a kívánt végállapotot. Siker után az új gameserver maradna futva a külön strict világon; a loginserver változatlan.

Jóváhagyás után következik a friss processzellenőrzés és átállás, majd két valódi GUI admission, átfedő konzisztens presence, külön Accepted X-close, a másik kliens megmaradása és friss reconnect. A tíz másodperces observation ablak változatlan. A cleanup előre rögzített megfigyelési határa öt másodperc, nem production timeout módosítása.

Jelszó és token nem kerül chatbe, parancssorba vagy evidence-be. A GUI-bevitelt a legutóbbi felhasználói kérés kifejezetten engedélyezi; eszközoldali tiltás nem kerülhető meg.

Linux/FreeBSD, sanitizerek, távoli DB blackhole/failover, új tokeninjektor, nagy readiness kampány és I2–I6 nem futott vagy hatókörön kívüli. Új core-live PASS jelenleg nincs. Az IF-5 végső riport a tényleges live eredmények után készülhet el.
