# I1 auth javítás és live újrapróba — 2026-09-28

**A belépést akadályozó poolhibát javítottam, a javított loginserver fut.** A két megadott tesztfiók hitelesítése már sikeres. Egy meglévő karakterrel a teljes login → karakterválasztás → handoff → game handshake → EnterWorldAccept lánc is sikerült; a megfigyelési ablak végén a szerver takarított.

A kezdetben üres listát adó fiókhoz (`account_id=3`) a helyi előkészítés után `CharacterId=4` vált elérhetővé. A két külön karakter egyidejű belépése és a friss handoffal végzett reconnect is sikerült. Az agent nem hozott létre karaktert és nem végzett DB-seedelést. A teljes, független presence/binding/ownership-invariáns minősítés korlátai alább szerepelnek.

## Ok és reprodukció

A régi pool a lejárt kapcsolatot ellenőrzés nélkül kiadta, majd `QueryFailed` után visszatette. A LIFO-kivétel miatt a következő kérés is ugyanazt a rossz kapcsolatot kapta. A futó régi szervereknél a hiba idején nem volt Established DB-kapcsolat; indulásuk óta több mint nyolc óra telt el.

A külön diagnosztikai program a kijelölt helyi DB-konfigurációt közvetlenül betöltötte, annak titkos értékeit nem írta ki és nem másolta. Friss kapcsolattal `SELECT 1` és az account-lekérdezés oszlopainak ellenőrzése sikerült. A DB session `wait_timeout` értéke 28800 másodperc.

Csak a program saját tesztkapcsolatán `SET SESSION wait_timeout=1`, majd 2,2 s várakozás reprodukálta a hibát:

| Azonos próba | Javítás előtt | Javítás után |
|---|---|---|
| Friss SELECT | OK | OK |
| Lejárat utáni első SELECT | QueryFailed, SQLSTATE `08` | OK, új kapcsolat |
| Következő SELECT | QueryFailed, SQLSTATE `08000`, vendor 1220 | OK |
| Native exit | 1 | 0 |

A lejárt kapcsolat problémája így determinisztikusan bizonyított. A régi loginserver csak a `QueryFailed` kategóriát naplózta, ezért az eredeti három live kivétel részletes SQLSTATE-je utólag nem áll rendelkezésre. A kapcsolatállapot, az izolált reprodukció és a javítás utáni sikeres live auth ugyanazt az okot támasztja alá.

## Változás

- [DbPool.cpp](D:/IxtreemeWorld/gameserver/libs/db/src/DbPool.cpp:36): közös kapcsolatnyitás; használat előtti állapotellenőrzés és ping; szükség esetén csere az üzleti callback előtt.
- [DbPool.h](D:/IxtreemeWorld/gameserver/libs/db/include/db/DbPool.h:50): a két privát helper deklarációja.
- [CMakeLists.txt](D:/IxtreemeWorld/gameserver/libs/db/CMakeLists.txt:35): alapból kikapcsolt `DB_BUILD_LIVENESS_TEST` cél.
- [DbPoolLivenessTest.cpp](D:/IxtreemeWorld/gameserver/libs/db/tests/DbPoolLivenessTest.cpp): explicit helyi DB-konfigurációval futtatandó integrációs teszt.

A hálózati ellenőrzés a pool mutexén kívül fut. Sikertelen újranyitáskor a slot visszakerül, és a callback `ConnectionFailed` eredményt kap; a pool nem veszít végleg kapacitást. **A már megkezdett műveletet nem játsszuk újra**: bizonytalan kimenetelű UPDATE vagy tokenfogyasztás nem ismétlődhet automatikusan. A változás a `Submit` és `SubmitVoid` útra egyaránt hat.

A meglévő socket/query timeout megmarad. A telepített Connector/C++ 1.1.5 időkorlátos `isValid` változata módosíthatja a query timeoutot, ezért a meglévő socket-beállítást használó overload került a javításba. Általános távoli hálózati blackhole/olvasási timeout hardening nem része ennek a javításnak.

Nem változott a password-ellenőrzés, auth policy, tokenpolicy, séma, protokoll, kliens, térkép vagy gameplay.

## Tesztek és build

**60 assertion PASS**, native exit 0, 4,84 s: egészséges kapcsolat, query timeout megőrzése, lejárt kapcsolat helyreállítása mindkét Submit úton, egyszeri session-művelet, kivétel utáni nem-ismétlés, művelet közbeni kapcsolatbontás, következő kérés helyreállása, 16 várakozó kérés előrehaladása egy kapcsolaton/két workeren, leállított pool elutasítása.

A teszt csak saját DB-session változókat/timeoutot használ. Nem módosít accountot, karaktert, tokent, táblát vagy globális DB-beállítást. A normál live belépés meglévő auth/handoff írásai külön, a felhasználó által engedélyezett út részei.

A loginserver és gameserver izolált Debug buildje sikeres. A build az installált VS18/MSVC, C++20, `/MTd`, vcpkg `x64-windows-static` függőségeket használja; nincs csomagtelepítés. A parancsok, exitkódok és futásidők a [külön evidence könyvtárban](D:/IxtreemeWorld/build/i1-auth-fix-20260928) vannak. Az eredeti hibaprobe megőrzött binárisa és a javított futás eredménye is rendelkezésre áll.

**Nem futott:** teljes worldbench/regressziós mátrix, távoli blackhole és teljes DB-kiesés alatti visszacsatlakozási hibatűrés. A sikertelen újranyitás slotmegőrző ága kódszinten ellenőrzött, ebben a lokális integrációs próbában külön outage-injektálást nem kapott. A korábbi kliens 851 assertion eredménye történeti; a változatlan kliensen most valódi belépési próba futott.

## Jóváhagyott futó környezet

A felhasználó külön jóváhagyta a két helyi szerver leállítását és a következő kombináció indítását. A leállítás előtt nem volt aktív kliens TCP-kapcsolat, a gameserver `active_sessions=0` állapotot jelzett. A régi folyamatok PID alapján, `Stop-Process` művelettel álltak le; ez nem graceful shutdown minősítés. A régi exe/config/log fájlok megmaradtak. Az új példányok külön konfiguráció- és naplóútvonalat kaptak.

| Szerep | Futó PID / SHA-256 |
|---|---|
| Javított loginserver, 11000 | 39580 / `9626E5BD97531E46479C359EB327417823F342D7BFDC4B0C79D1709E29625F24` |
| Változatlan régi gameserver, 11020 | 25312 / `F96114D2C3E8F8D3CBCB9C9DE6E635433B0733B915483D5A4A0C3D589D1DFF88` |
| Változatlan I1 kliens | `3E1230383379A8C486B20D8EC8566864C7423A97B8FBA21D3FADB62F481F3405` |

**A futó gameserverben a tartós pooljavítás még nincs benne.** Újraindítása friss DB-kapcsolatot adott a rövid teszthez; nyolcórás idle után ismét jelentkezhet a régi hibája. Ez nem tartós gameserver-javításként jelentett PASS.

Az újonnan fordított gameserver hash-e `9ED67A50DD84127012657125E743D510456B688B4FB352AA70A02B36282F9732`, de nem indítottam vele runtime-ot. A `test_zone` az U-final strict package-validátorán `WORLDLOGIC_WARP_TARGET_IN_TRIGGER(413)` hibával megáll: az egyik warp célja a másik triggerében van. A validátort és a csomagot nem lazítottam/módosítottam, `legacy` warp policyra nem kapcsoltam át.

Az U-final 227 fájlos forrásmanifestjétől a három most módosított DB-fájl és a már korábban eltérő `tc4_acceptance.py` tér el. A futó régi gameserver U-final forrásazonossága továbbra is **UNVERIFIED**.

## Live eredmény

2026-09-28 20:13–20:14 helyi idő (UTC+02:00):

- Mindkét fiók authja sikerült, `account_id=2` és `account_id=3`. Az előző `auth_internal_error` megszűnt.
- A B kliens a szerver listájából `CharacterId=3` karaktert választott. A handoff naplója `127.0.0.1:11020`, egyezik a változatlan exact allowlisttel.
- Game session 1: **Accepted**, own NetId 1, spawn `(100,100,-8.83)`, serverTick 1432. A UI későbbi állapotában 56 világüzenet feldolgozása látszott; ez admission-sink, nem actor/mozgásintegráció.
- A szerver zóna 1 assignmentet naplózott, majd tíz egymást követő másodperces diagnosztikában `active_sessions=1` szerepelt.
- 20:14:21.020 körül lejárt a változatlan tíz másodperces Accepted ablak; 20:14:21.057-kor a szerver `despawned net_id 1` eseményt írt. A session-szám ismét 0 lett.
- Az A kliens sikeres auth után `empty_character_list` eredményt kapott; választás hiányában a meglévő harminc másodperces határnál takarított. Nem kapott kitalált karakterazonosítót.

[Titokmentes live kivonat](D:/IxtreemeWorld/build/i1-auth-fix-20260928/live-evidence-181554.json). A szerver assignment/despawn és aggregált session-szám bizonyítéka nem teljes független presence/binding/ownership-invariáns snapshot.

20:16-kor újabb két valódi belépés történt:

| Kliens | Account / CharacterId | Game session / own NetId | Accept / bontás, helyi idő |
|---|---|---|---|
| A, attempt 2 | 3 / 4 | 2 / 2 | 20:16:14.350 / 20:16:24.365 |
| B, attempt 2 | 2 / 3 | 3 / 3 | 20:16:16.482 / 20:16:26.482 |

Mindkét karakter külön hitelesített sessionből, valódi listából, új handoffal jutott az exact allowlist szerinti gameserverre. A spawn mindkettőnél `(100,100,-8.83)`, a tick 3898 / 3941. A kliensoldali Accepted átfedés közel 7,88 s. A szerver hét egymást követő diagnosztikában `active_sessions=2` értéket jelzett; külön session/NetId assignment és külön despawn esemény is szerepel. Az első automatikus bontása után a másik még jelen maradt, majd annak bontása után a session-szám 0 lett.

A B reconnectje az előző NetId 1 helyett NetId 3-at kapott új attempt/generation mellett. A friss handoff kiadását a loginserver, a külön game-sessiont a gameserver igazolja; nyers tokeneket az ellenőrzés nem rögzített. A régi own identitás nem maradt meg.

[Kibővített live kivonat](D:/IxtreemeWorld/build/i1-auth-fix-20260928/live-evidence-181740.json). Ez bizonyítja a két sessionnel végzett admissiont, az átfedő aktív játékosszámot és a külön takarítást. Az összes entity/presence/binding/ownership index teljes, független ellenőrzését a régi szerver meglévő naplója nem biztosítja; erre nincs mesterséges PASS.

Két további páros kör is lefutott (attempt 3 és 4). A végső összesítés: **7 Accepted, 7 külön szerveroldali assignment, 7 despawn**, legfeljebb két egyidejű aktív game-session, a végén nulla aktív session. A negyedik attemptben a két Accepted időpontja 20:20:47.966 / 20:20:48.848; a saját NetId 6 / 7. A reconnectek során minden belépés új saját NetId-t kapott.

**NOT_COMPLETED:** az egyik Accepted GUI normál X-bezárása közben a másik állapotának megmaradása. A harmadik körben mire az agent a bezárandó ablakot ellenőrizte, annak megfigyelési ablaka már lejárt. A negyedik körben is mindkét kliens `observation_complete` eseményt naplózott. Ezeket nem minősítem X-close tesztnek. A timeoutot nem emeltem.

A végén az agent az X gombbal mindkét már Offline ablakot bezárta: **native exit 0 / 0**, nincs megmaradt tesztkliens. Az egyik ablak bezárógombja a képernyő szélén kívül volt; előbb a címsor duplakattintásával maximalizáltam. Nem használtam process-killt a kliensablakokhoz. A pending-close/cancel és az invalid/expired/used token/duplicate-presence reject ágak ebben a live folytatásban **NOT_RUN/NOT_REACHED** maradnak.

[Végső gépi összesítő](D:/IxtreemeWorld/build/i1-auth-fix-20260928/summary.json), [teljes titokmentes live kivonat](D:/IxtreemeWorld/build/i1-auth-fix-20260928/live-evidence-182251.json).

## Megőrzés és következő határ

A korábbi 4784 védett fájl összehasonlításában csak a három szándékosan módosított DB-forrás/buildleíró tér el; váratlan eltérés nincs. Az új integrációs teszt külön új fájl. A kliens, az eredeti Auriga, a checked test_zone és a korábbi bizonyítékcsomagok megmaradtak. Nincs commit/push/reset/stash.

Az auth-javítás kész és a loginserverben aktív. A tesztkörnyezet két meglévő karakterrel valódi admissionre alkalmas. A jóváhagyott szerverkombináció futva maradt, a két kliens bezárult. A tartós gameserver-csere külön, a világcsomag szigorú szerződését megtartó döntést igényel. I2–I6 nem indult. **STOP + review; nincs teljes I1-invariáns vagy U-final E2E-PASS állítás.**
