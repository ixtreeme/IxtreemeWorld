# I1-LIVE review — 2026-09-28

**Eredmény: BLOCKED az auth fázisban.** Két külön GUI-klienssel, két külön helyben megadott fiókkal elindult a valódi belépési próba. A login-handshake mindhárom megfigyelt kísérletnél sikerült, majd a loginserver `QueryFailed` hibát jelzett. A kliens helyesen `auth_internal_error` állapotba lépett. Karakterlista, token, game-handoff és EnterWorldAccept nem keletkezett. Ez nem I1 E2E-PASS, és nem U-final minősítés.

Futási bizonyíték: [gépi összesítő](D:/IxtreemeWorld/build/client-i1-live-20260928-1758/summary.json), [külön evidence könyvtár](D:/IxtreemeWorld/build/client-i1-live-20260928-1758). A korábbi riportok változatlanok. A dokumentum a mostani live folytatást minősíti; a korábbi offline eredményeket nem számolja új futtatásnak.

## Tényleges környezet

| Elem | Ellenőrzött adat |
|---|---|
| Loginserver | PID 11504; `D:/IxtreemeWorld/loginserver/build/apps/loginserver/Debug/loginserver.exe`; listener `0.0.0.0:11000`; klienscél `127.0.0.1:11000` |
| Gameserver | PID 41776; `D:/IxtreemeWorld/gameserver/build/apps/gameserver/Debug/gameserver.exe`; listener `0.0.0.0:11020` |
| Kliens A / B | PID 464 / 39524; a megőrzött végső `build/client-i01-20260928/client-build/Debug/VulkanClear.exe`; külön GUI-folyamatok |
| Profil | `IxtreemeWorld-I1`; új [live.conf](D:/IxtreemeWorld/build/client-i1-live-20260928-1758/live.conf); Auriga-login nem indult |
| Handoff allowlist | Kizárólag `127.0.0.1:11020`, egyezik az exe melletti login/game konfiguráció nem titkos értékeivel |
| Időkorlátok | connect 5000, phase 10000, enter 30000, Accepted observe 10000 ms; nincs növelés |

Mindkét szerver a korábban futó folyamat maradt, azonos startidővel. Nem volt explicit config/world CLI-opció; az exe melletti alapértelmezett konfigurációt ellenőriztem. A folyamat munkakönyvtárát nem bizonyítottam. A valódi handoff endpoint **NOT_REACHED**, ezért a konfigurációegyezés nem minősül megfigyelt handoffnak.

A fájlok SHA-256 értékei a futás előtt és után azonosak:

| Bináris | SHA-256 |
|---|---|
| Kliens | `3E1230383379A8C486B20D8EC8566864C7423A97B8FBA21D3FADB62F481F3405` |
| Loginserver | `B957F68280FBD12827B7BE82B252B2479C86B82769C00A7750246469A90351DC` |
| Futó gameserver útvonalán lévő exe | `F96114D2C3E8F8D3CBCB9C9DE6E635433B0733B915483D5A4A0C3D589D1DFF88` |
| Megőrzött U-final Debug referencia | `0CB5C4409667F648EAA91DFF45AC90B2E9F69E18E5E39C4D01E3159D5889B6CE` |

A futó gameserver U-final vagy ellenőrzött utód forrásazonossága **UNVERIFIED**. A live eredmény a fenti tényleges célhoz tartozik.

## Világazonosság és diagnosztika

Az első fájlmetadata-ellenőrzéskor a két napló mérete nulla volt; később mindkettő olvashatóvá vált. Ennek okát nem állapítottam meg, nem módosítottam loggingot és nem indítottam újra szervert. Az `environment.json` ezt a kezdeti megfigyelést tartalmazza; a későbbi bizonyíték a `login-diagnostics.json` és `game-diagnostics.json`.

A gameserver 2026-09-27 23:03:41 helyi indulási naplója a **test_zone** térképet azonosítja: 500 cella, 2 m cellaméret, három zóna, kilenc induló mob. A forrásként megnevezett út normalizálva `D:/IxtreemeWorld/Client/assets/Maps/test_zone`. A jelenlegi csomagfájlok hash-ei a [world-files-observed.json](D:/IxtreemeWorld/build/client-i1-live-20260928-1758/world-files-observed.json) állományban vannak. **A betöltéskori bájtazonosság UNVERIFIED**: a régi szerver nem naplózott betöltéskori fájlhash-eket.

A kliensfutásból mintavett 108 másodperces periódusban a szerver diagnosztikája végig `active_sessions=0`, a world_tick 1506020 → 1508160. Ez jelzi a szimuláció előrehaladását és az aktív game-session hiányát; nem teljes presence/binding/ownership-invariáns ellenőrzés. Kétkarakteres authoritative jelenlét nem jött létre.

## Tényleges belépési eredmények

A hitelesítő adatokat a felhasználó helyben vitte be. Nem kerültek a helperbe, CLI-ba, konfigurációba vagy új evidence-be. A szerver username mezőit a feldolgozás csak két külön fiók megkülönböztetésére használta; a mentett összesítő `account-1` és `account-2` aliasokat tartalmaz. Jelszó, token vagy nyers auth-payload nem került rögzítésre.

| GUI / attempt | Login session | Helyi szerveridő | Kliens eredménye | Szerver eredménye |
|---|---:|---|---|---|
| A / 1 | 1, account-1 | 19:58:53.970–54.002 | `auth_internal_error`, 50 ms | handshake OK → QueryFailed → cleaned up |
| A / 2 | 2, account-1 | 19:59:14.361–14.384 | `auth_internal_error`, 50 ms | handshake OK → QueryFailed → cleaned up |
| B / 1 | 3, account-2 | 19:59:24.102–24.134 | `auth_internal_error`, 51 ms | handshake OK → QueryFailed → cleaned up |

Időzóna: Europe/Bratislava, UTC+02:00. Az evidence klienseseményeinek időbélyege UTC. Az 50/51 ms a kliens által jelentett attempt-idő, nem külön szerveroldali SQL-időmérés.

**VERIFIED:** két folyamat, két külön belépési név, TCP/login handshake és feldolgozható auth-hibaválasz, a kliens hibakezelése; mindhárom login-session takarítását szervernapló igazolja. Egyik kliensben sem jelent meg CharacterId/own NetId, `world_frames=0` maradt. Az auth utáni socket-ellenőrzéskor egyik klienshez sem tartozott TCP-kapcsolat. Nincs megfigyelt legacy világ/mozgás-mellékhatás.

**NOT_REACHED:** valódi karakterlista és pontos CharacterId kiválasztása; token/handoff; game handshake; accept utáni spawn/serverTick; átfedő authoritative presence, ownership és duplikációmentesség. A login handshake sikere nem bizonyítja a régi gameserver világcsomagjainak kompatibilitását.

## Bizonyított hibahatár és javítási javaslat

A szervernapló konkrét hibája `QueryFailed`, nem `InvalidCredentials`. Ebből nem állapítható meg sem a jelszavak helyessége, sem az SQL-kivétel pontos oka.

A jelenlegi forrás magyarázza a megfigyelt hibakód útját:

- [AuthHandler::HandleLoginRequest](D:/IxtreemeWorld/loginserver/apps/loginserver/src/AuthHandler.cpp:29): a sikertelen repository eredményt wire `INTERNAL_ERROR` értékre képezi, és a `DbErrorString` kategóriát naplózza. A `result.message` részletét nem.
- [DbPool::Submit](D:/IxtreemeWorld/gameserver/libs/db/include/db/DbPool.h:101): a munka közben elkapott `std::exception` → `QueryFailed`, a részletes `e.what()` a result üzenetébe kerül. A kategória ezért önmagában nem bizonyít konkrét SQL- vagy sémahibát.
- [AccountRepository::Authenticate](D:/IxtreemeWorld/gameserver/libs/db/src/AccountRepository.cpp:80): account SELECT, mezőolvasás, password-ellenőrzés és last_login UPDATE is része az útnak. A pontosan hibázó lépés a rendelkezésre álló naplóból nem azonosítható.
- [Kliens LoginRequest feldolgozás](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/Admission/Admission.cpp:197): a wire hibaválasz `auth_internal_error` állapotot és cleanupot eredményez. Nem timeout vagy kliens által kitalált auth-eredmény.

Ezek a jelenlegi forrásra vonatkozó hivatkozások; nem helyettesítik a futó régi szerver binárisának forrásazonosságát.

A legkisebb következő lépés az auth backend **pontos, titokmentes kivételkategóriájának / SQLSTATE-jének és hibázó műveletének** megszerzése a kijelölt környezetben. Ezután lehet célzott kapcsolat-, jogosultság-, séma- vagy repository-javítást választani. Most egyik okot sem bizonyítottam; nincs indokolt kliens/protokoll-módosítás. Szerver-diagnosztika módosítása, rebuild/restart, konfigurációcsere vagy DB-beavatkozás külön engedélyezendő lépés. Nem olvastam DB-credential konfigurációt, nem hajtottam végre közvetlen DB-műveletet.

## Cleanup, reconnect és negatív utak

| Próba | Eredmény |
|---|---|
| Auth-hiba utáni transport/session cleanup | **PASS** a megfigyelt klienssocket-állapot és a szerver `cleaned up` eseményei alapján |
| A második Login attemptje | **VERIFIED** új attempt/generation és külön login-session; ismét ugyanaz az auth-hiba. Nem friss-tokenes game reconnect |
| B Cancel a Failed állapot után | **PASS**: `Cancelled`, generation 4, own identitás nélkül; nem pending-admission cancel |
| A és B normál X bezárás | **PASS**: mindkettő native exit 0, nincs process-kill; a második bezárása után az első hibaképernyője megmaradt |
| Egy Accepted kliens bontása a másik jelenlétének megtartásával | **NOT_REACHED** |
| Friss tokenes game reconnect | **NOT_REACHED** |
| Pending admission alatti close/cancel | **NOT_RUN**; a rövid auth-hibautat nem alakítottam mesterséges várakozássá |
| Invalid/expired/used token és duplicate-presence reject | **NOT_REACHED** |

Nem volt flood, automatikus retry, auth-bypass, univerzális token, kitalált karakter, timeout-növelés vagy validátorlazítás.

## Megőrzés, regresszió és lezárás

A friss baseline 4784 védett forrás-, asset-, build- és korábbi evidence-fájljából **4784 változatlan** a záróellenőrzéskor. A kliens 451 fájlja, az 522 szerverbaseline-fájl és a 2278 Auriga-fájl az előző ellenőrzött manifesttel is egyezik. A becsekkolt test_zone változatlan.

A régi teljes output-manifesthez képest induláskor **48 már meglévő generált/build eltérés** volt az eredeti standalone `build` alatt, köztük a korábban jelzett kilenc CMake-fájl és Release outputok. Nem e live munka hozta létre őket; eredetük nincs hozzárendelve. A mostani baseline ezeket is a tényleges induló állapotukban védte, és egyik sem változott a próba során.

Új kimenet csak a külön live evidence/config/helper és ez a riport. Production forrásmódosítás és új build nem készült. A korábbi offline 851 assertiont és U-final regressziókat ebben a forrásváltozás nélküli live körben nem futtattam újra. A két saját kliens normál módon bezárult; a szerverfolyamatok megmaradtak.

**STOP + review.** Az I1 pozitív elfogadás az auth backend hibája miatt nyitott. A szerver eredetének és a betöltött világ bájtazonosságának korlátai külön megmaradnak. I2–I6, gameplay/renderer/Unity, commit/push/reset/stash és rendszerbiztonsági módosítás nem történt.
