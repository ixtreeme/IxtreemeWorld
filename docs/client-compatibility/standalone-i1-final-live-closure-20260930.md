# I1-FINAL live acceptance closure – 2026-09-30

## Státusz

**IF-5 review closure: NOT_COMPLETED / BLOCKED by the live observation boundary.**

A jelenlegi I1-FINAL munkamenetet lezárom review-ready állapotban. Új szerverforrás-módosítás nem történt, a futó loginserver/gameserver nem lett újraindítva, a régi bizonyítékok megmaradtak, és az I2–I6 / rétegezett 3D munka nem ebben a mérföldkőben kezdődött el.

A hiányzó gate-eket nem jelölöm mesterséges PASS-ként:

- natív Accepted X-close utáni authoritative presence cleanup: NOT_COMPLETED;
- friss, külön reconnect-bizonyítás: NOT_RUN.

## Kiindulási és futó állapot

Working tree:

    D:\IxtreemeWorld

A repository a korábbi mérföldkő-commitok után tiszta volt. Az I1-folytatáson csak live kliensnaplók és ez a záróriport készült; forráskódot nem módosítottam.

Futó szolgáltatások:

- loginserver PID 39580, port 0.0.0.0:11000, bináris: build/i1-auth-fix-20260928/out/loginserver/Debug/loginserver.exe;
- gameserver PID 25628, port 0.0.0.0:11020, bináris: build/client-i1-final-20260928-1910/out/gameserver/Debug/gameserver.exe;
- a live gameserver SHA-256 továbbra is 0A8E978F47039530E71F831D8C31FA7F643538FB7042EF3B698A0DBBCDDFFB4A;
- mindkét listener a zárás után is LISTEN állapotban maradt;
- a GUI kliensfolyamatokat natív ablak-X használatával bezártam.

A cutovert nem futtattam újra, és nem állítottam le szolgáltatást.

## Már korábban bizonyított eredmények

Az offline és preflight bizonyítékok változatlanul érvényesek:

- DB pool liveness: 60 assertion PASS;
- protocol regression: PASS;
- presence-v2: PASS;
- netstress: PASS;
- world package: 140 PASS, 1 környezeti SKIP;
- snapshot, strict fixture, startup-check és negative fallback: PASS;
- Debug bootstrap 1024 threshold FAIL megőrzött korlátként.

A korábbi live attempt 3-ban mindkét kliens Accepted állapotba került, és a gameserver observer közös I1-OBS mintái character 3/session 3/net 3 és character 4/session 4/net 4 koherens jelenlétét mutatták. Ez a G4 overlap bizonyíték jelenlegi legjobb eredménye. Az attempt 3 azonban observation timeouttal/Noesis shutdownnal végződött, nem natív X-close-szal.

## 2026-09-30 kontrollált live próbálkozás

A meglévő GUI binaryt és a secret-free A/B configokat használtam. A hitelesítő adat csak a GUI mezőibe került; credential, password és token nem került logba vagy evidence-fájlba.

A friss próba eredménye a gameserver logban:

- session 5 handshake: 2026-09-30 06:05:29.658;
- session 5 assigned: 2026-09-30 06:05:29.737, net ID 5, spawn 15,65, ground_z 1.4375;
- session 5 disconnected/cleaned/despawned: 06:05:39.746–06:05:39.783;
- session 6 handshake: 2026-09-30 06:06:23.697;
- session 6 assigned: 2026-09-30 06:06:23.733, net ID 6, spawn 15,65, ground_z 1.4375;
- session 6 disconnected/cleaned/despawned: 06:06:33.746–06:06:33.779.

Ez két külön egyéni admission/cleanup eseményt bizonyít, de nem átfedő kétklienses jelenlétet és nem friss reconnectet.

A log szerint az I1 observer korábbi mintakerete 2026-09-28 22:56:28.757 körül I1-OBS stopped állapotba jutott. A jelenleg két napja futó gameserveren ezért a mostani 2026-09-30-as sessionökhöz nincs új observer-snapshot, amellyel az X-close előtti és utáni registry/reverse/owner/leaf/living-entity állapotot korrektül össze lehetne párosítani. A szerver újraindítása csak ezért nem történt meg.

## Gate-értékelés

| Gate | Eredmény | Bizonyíték / megjegyzés |
|---|---|---|
| G0 | PASS | cutover binary, fixture, port és PID preflight; futó listener újra ellenőrizve |
| G1 | PASS | korábbi attempt 3 valódi login/auth/character select; friss session 5/6 is admissionig jutott |
| G2 | PASS | korábbi attempt 3 game handoff és EnterWorld; friss sessionek assignmentet kaptak |
| G3 | PASS | korábbi attempt 3 mindkét kliens Accepted loggal |
| G4 | PASS | korábbi attempt 3 közös koherens I1-OBS minták |
| G5 | NOT_COMPLETED | friss próbában nem lett Accepted overlap; az observer már leállt; X-close utáni authoritative cleanup nem bizonyítható |
| G6 | NOT_RUN | külön reconnect attempt új generation/session/net azonosítóval nem futott le |
| G7 | PASS / LIMIT | offline regressziók és strict bizonyítékok PASS; Debug bootstrap threshold FAIL megőrzött korlát |
| G8 | CLOSED WITH LIMITS | review-ready zárás; I2–I6 nem kezdődött |

## Döntés a továbblépésről

Az I1-FINAL munkamenetet lezárom a fenti korlátozással. Nem állítom, hogy a G5/G6 bizonyított, ezért ez nem teljes live acceptance PASS. A korlát oka mérési/observer-életciklus és a korábbi 10 másodperces observation ablak, nem megváltoztatott timeout vagy lazított validáció.

A rétegezett 3D világ előkészítése külön következő mérföldkőként kezdhető. Annak első lépése egy kis layered-world contract és tesztfixture legyen; az I1 live acceptance hiányzó G5/G6 eseteit ne keverd bele ebbe az új munkába.

**STOP – IF-5 review closure. I2–I6 nincs elkezdve. A live zárás során forráscommit, push, reset vagy stash nem történt; ezt a review-riportot külön dokumentációs commitban rögzítjük.**
