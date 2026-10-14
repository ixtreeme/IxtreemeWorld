# I0–I1 folytatás – végső GUI ellenőrzés

2026-09-28, 17:25–17:29 UTC. Az [előző review](D:/IxtreemeWorld/docs/client-compatibility/standalone-i0-i1-admission-review.md)
GUI-startup korlátainak egy része lezárult. **Live admission továbbra is BLOCKED.**
Ebben a folytatásban nem módosult production forrás, nem készült új build, és nem indult I2–I6.
Commit/push/reset/stash nem történt.

## Új, tényleges eredmények

Ugyanazzal a végső `VulkanClear.exe` binárissal futott minden próba:
SHA-256 `3E1230383379A8C486B20D8EC8566864C7423A97B8FBA21D3FADB62F481F3405`.
Indítási cwd: `C:/Windows`; a külön config/content-root feloldása működött.
A korábbi Windows biztonsági overlay már nem takarta a felületet. Biztonsági beállítást nem változtattam.

| Próba | Tényleges eredmény |
|---|---|
| I1 végső GUI startup | **PASS**: Noesis I1 képernyő, ProfileReady, attempt=0, nincs own identitás; nulla TCP socket az ellenőrzéskor. |
| Offline Cancel | **PASS**: Cancelled, generation=1; ismételt Cancel ugyanebben az állapotban marad. |
| I1 normál ablakbezárás | **PASS**: az X gombbal rendezett kilépés, native exit 0; nem process-kill. |
| Auriga GUI-regresszió | **PASS startup/close**: eredeti Auriga login felület, explicit Profile: Auriga; X bezárás, exit 0. Nem kattintottam Loginra, nem történt külső auth-kapcsolat. |
| Auriga diagnosztikai listener | Az örökölt Noesis Debug Inspector `0.0.0.0:17629` listener látható; az I1 profilban nincs ilyen listener. Nem gameserver-kapcsolat. |
| Profil nélküli exe | **PASS negatív kontroll**: látható „Explicit profile required” hiba, nulla TCP socket; OK után a várt native exit 2. |
| Ismeretlen configprofil | **PASS negatív kontroll**: `explicit_profile_required`, nulla TCP socket; a várt native exit 2. Nincs fallback. |

Az eltelt futásidők emberi/UI-ellenőrzési időt is tartalmaznak; nem startup-latencia mérések.
A korábbi 15 csoport / 851 assertion offline és három U-final regresszió eredménye történeti,
ebben a forrásváltozás nélküli folytatásban nem futtattam újra őket.

Az Alt+F4 próba nem zárta be az I1 ablakot. A változatlan
[NativeWindow.cpp](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/NativeWindow.cpp:244)
a `WM_SYSKEYDOWN/UP` üzeneteknél is 0-val tér vissza, így nem adja tovább a szokásos rendszerbillentyű-kezelést.
Az X gomb működése külön igazolt. Ez meglévő, nyitott input-viselkedési tétel;
ebben a GUI-ellenőrzésben nem módosítottam a közös input útját.

## Talált futó szerverek

Kizárólag helyi processz-, listener- és binárismetadata ellenőrzés történt; nem endpoint-scan,
nem DB-lekérdezés, és nem titkos konfiguráció olvasása.

| Processz | Megfigyelt listener | Fájlhash |
|---|---|---|
| loginserver, PID 11504 | `0.0.0.0:11000` | `B957F68280FBD12827B7BE82B252B2479C86B82769C00A7750246469A90351DC` |
| gameserver, PID 41776 | `0.0.0.0:11020` | `F96114D2C3E8F8D3CBCB9C9DE6E635433B0733B915483D5A4A0C3D589D1DFF88` |

Gameserver út: `D:/IxtreemeWorld/gameserver/build/apps/gameserver/Debug/gameserver.exe`.
A fájl írási ideje szeptember 24. 14:34 UTC. A fagyasztott U-final Debug exe szeptember 27-i,
hash-e `0CB5C4409667F648EAA91DFF45AC90B2E9F69E18E5E39C4D01E3159D5889B6CE`.
**A futó cél U-final/származtatott forrásazonossága UNVERIFIED.** A timestamp és eltérő hash
nem bizonyít pontos forrásrevíziót; ezért nem minősítem automatikusan megfelelő utódnak.
A szerverprocesszeket nem állítottam le, nem cseréltem ki és nem indítottam újra.

A portok létezése nem helyettesíti a tesztkörnyezet és identitások kijelölését.
Továbbra is szükséges a jóváhagyott login/handoff cél, két előkészített account/karakter,
helyi interaktív credential-bevitel, az alkalmazandó igazolt szerver/világcsomag és
a meglévő, jogosult authoritative presence-diagnosztika.

## Megőrzés és evidencia

Új, külön evidence: [gui-continuation](D:/IxtreemeWorld/build/client-i01-20260928/gui-continuation).
[Gépi összesítő](D:/IxtreemeWorld/build/client-i01-20260928/gui-continuation/summary.json)
tartalmazza a parancsokat, UTC start/end, PID, binárishash, native exit, elvárt eredmény és assertion adatokat.
A korábbi review és futási bizonyítékok megmaradtak.

- **451/451** végső standalone forrás/config/asset egyezik.
- **522/522** szerverbaseline-fájl és **2278/2278** Auriga-fájl egyezik.
- A korábbi evidence, U-final és checked test_zone egyezik.
- Az eredeti standalone `build` alatt **9 generált CMake-fájl eltér** az előző baseline-tól:
  cache, vcxproj/slnx és három CMakeFiles állomány. Megfigyelt írási idejük **17:07 UTC**, a mostani
  GUI-futtatások előtt. Ebben a folytatásban nem futott CMake, nem írtam ezeket, és nem állítottam
  vissza őket. Az eltérés eredetét nem tulajdonítom bizonyíték nélkül konkrét személynek/eszköznek.
  Emiatt a régi teljes output-manifestre nincs új, feltétel nélküli PASS; a pontos delta a JSON-ban szerepel.

## Nyitott végpont

**NOT_RUN/BLOCKED:** valódi kétklienses auth/handoff/accept, átfedő authoritative presence,
live rejectek, pending admission alatti GUI-close és valódi reconnect. Offline Cancel/ablakbezárás
nem helyettesíti ezeket. A tesztbemenetekre feltett kérdés még nyitott.
Release kliens és teljes világ/actor/mozgásintegráció változatlanul nincs minősítve.

Az I0 GUI-startup/explicit profil/normal close ellenőrzése elkészült. A további érdemi I1 E2E
munka a fent megnevezett környezeti adatokhoz kötött; I2–I6 nem indult.
