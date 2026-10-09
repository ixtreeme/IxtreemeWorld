# 3D-5C2 – kliensprotokoll scene-scriptként + általános engine-transzport (review, 2026-10-01)

Állapot: **kész; automatikus tesztek zöldek, az élő GUI-s, adatbázisos végponttól-végpontig teszt
(egy és két kliens) lefutott és sikeres** – lásd „Élő teszt”. Kód: `c508f7b`, alap: `ab4c499` (3D-5C1).

## Cél és döntések

A 3D-5C1 óta a szerver rétegzett (`0x12`) transform-frame-et küld a protokoll ≥ 2 klienseknek. A
3D-5C2 a kliensoldalt adja hozzá, a felhasználói döntések szerint:

- „a protokoll réteget ne a motorba kösd, hanem írd meg scriptként a scene-hez” → a teljes MMO-protokoll
  egy **C++ game-module scriptben** él (`Client/sdk/examples/mmo_client/MmoClient.cpp`);
- a motor csak **általános, protokollfüggetlen** szolgáltatást ad: nem blokkoló TCP bájtfolyam és egy
  egysoros szövegbekérő (jelszóhoz maszkolva);
- az OS-függő kód kizárólag a platformrétegben van (`TcpStream_Win32.cpp` / `TcpStream_Posix.cpp`),
  Win32-specifikus kód máshol nincs.

## Motor (általános)

| Réteg | Változás |
|---|---|
| `libs/platform` | `platform::TcpStream`: DNS/IPv4/IPv6 feloldás, nem blokkoló connect, `Poll` (connect-befejezés, flush, pull), `Send`/`Receive`/`Close`, 16 MiB bemeneti plafon, SIGPIPE-védelem (POSIX), elküldött bájtok nullázása (hitelesítő adat is átmegy rajta). Win32: Winsock (`call_once` WSAStartup), POSIX: `fcntl`/`poll`/`SO_ERROR`. |
| `libs/script` (+ SDK másolat) | `IScriptApi`: `NetConnect/NetState/NetSend/NetReceive/NetClose`, `PromptText/PromptResult` – csak skalár és hívó-tulajdonú puffer a /MT határon. `NativeScript` kényelmi wrapperek. **ABI v4** (`IXTREEME_MODULE_API_VERSION 4`). Lua-paritás: ugyanezek a függvények Lua-stringként kezelt bájtokkal. |
| `apps/client` `ScriptApiImpl` | handle → `TcpStream` tábla (max 16 stream), prompt-tábla (max 8, 256 karakter), a beküldött szöveg átadás után törlődik és felülíródik; `ResetTransportAndPrompts()` Play-leállításkor és kilépéskor minden streamet lezár. Jelszó/szöveg soha nem kerül logba. |
| `libs/render` `EditorImGui` | `SetScriptPrompts` / `TakeScriptPromptAnswers`: a nyitott promptok kis ablakként jelennek meg (secret → `Password` + `NoUndoRedo`, beküldés után a puffer nullázva, `ClearActiveID`). A nem-ImGui buildben is fordul (csak az adatátadás közös). |
| `EngineApplication` | promptok átadása a panelek előtt, válaszok begyűjtése utána; Play-ben ha szövegmező birtokolja a billentyűzetet, a leütések nem mozgatják a játékost. |

A standalone runtime (WITH_EDITOR=OFF) **nem rajzol promptot**: ott `PromptText` 0-t ad, a script ezt
hibaként jelzi („this host has no text prompt UI”). Runtime-UI későbbi feladat.

## Script (`MmoClient.cpp`, ~1450 sor, csak SDK-fejlécek)

- Keretezés (4 bájtos big-endian hossz, 64 KiB limit), kézzel írt Cap'n Proto kódoló (egy szegmens) és
  határellenőrzött dekódoló (több szegmens, far / double-far pointer, bejárási keret mint a szerveren).
- Login: prompt (felhasználónév, majd maszkolt jelszó) → loginserver handshake (v2) → `LoginRequest`
  (a jelszó a sorba állítás után azonnal törlődik) → karakterlista → kiválasztás (`character` paraméter
  vagy az első) → `EnterWorldToken` → gameserver handshake (v2) → `EnterWorld` (token törölve) →
  `EnterWorldAccept` (rétegzett spawn: volume/layer).
- Világban: `0x10`/`0x11`/`0x12` frame-ek pontos (maradékmentes) dekódolása; a saját entitás a viewer
  rekordot követi, a szintváltást logolja (`[MMO] level change: volume a/layer b -> ...`); más entitások
  `SpawnMesh` proxyk (`proxyMesh`), `0x08` bitnél szintváltás. Koordináták: szerver (x,y,z) z-fel ↔
  engine (x,z,y). Mozgás: nyilak (vagy WASD) → `[1][0x01][u32 seq][u16 heading_q][u8 state]`, változáskor
  + 1 s keepalive (a szerver idle-timeoutja 60 s). 5 s-enként státuszsor (tick, frame-ek opkód szerint,
  pozíció, volume/layer, proxyk száma).
- `MMO_CLIENT_CODEC_ONLY` mellett csak a kódoló fordul – ezt használja a szerveroldali keresztteszt.

## Tesztek (lefuttatva, Windows)

| Teszt | Eredmény |
|---|---|
| `IxtreemeEngine` (Release) build | sikeres |
| `MmoClient.cpp` önálló modul-build (SDK-fejlécek, /MT, C++20) | sikeres, figyelmeztetés nélkül |
| `worldbench --mode scriptwire` (új, Debug) | **27/27 PASS** |
| – script-kódolt handshake / login (üres, ékezetes, 255 bájtos, NUL-t tartalmazó jelszó) / karakterlista / kiválasztás / EnterWorld (0–200 bájtos token) valódi Cap'n Proto-val (untrusted limitek) olvasva | PASS; a handshake bájtra azonos a capnp saját kódolásával |
| – valódi szerver-üzenetek (handshake, login, karakterlista minden mezővel, token, accept rétegzett spawnnal, reject, spawn, despawn, health, death) a script dekódolójával | PASS |
| – több szegmensű üzenetek (FIXED_SIZE 1–16 szavas szegmensek, max. 8 szegmens, far pointerek) | PASS (azt külön nem mértem, hogy double-far ténylegesen előfordult-e) |
| – 1157 csonkítás: egyiket sem fogadja el; 20 000 véletlen korrupció: hiba nélkül lefut | PASS |
| – valódi `EncodeTransformFrameV3/V2/FromRecords`: minden mező-maszk (v3: 0x00–0x0F), bájtra pontos fogyasztás, csonkítás/maradék elutasítva, v2-ben `0x08` elutasítva | PASS |
| – heading-kvantálás script vs szerver (8001 szög) | PASS, mind bitre azonos |
| – loopback, valódi `GameConnectionHandler` (DB nélkül): script handshake → v2; script EnterWorld + 9 bájtos move elfogadva (`moves_dropped_entering` +1, kapcsolat nyitva); v3 kérés → elutasítás + zárás | PASS |
| – a motor saját `platform::TcpStream`-je a valódi handlerrel: nem blokkoló connect, handshake → v2; elutasított connect → `Failed` (10061) | PASS |
| worldbench regresszió: netstress, layeredpresence, protocol, replv2, hygiene | mind failures=0 |

## Élő teszt (2026-10-01, Windows, Computer Use-zal vezérelt szerkesztő)

Környezet: `IxtreemeEngine` Release (szerkesztő), `loginserver` + `gameserver` a `windows-debug`
buildből, a meglévő MariaDB-adatbázissal (a DB-konfig csak útvonallal átadva), két meglévő teszt-fiók
(a felhasználótól; csak a szerkesztő promptjába gépelve, sehol nem tárolva/logolva). Minden új adat
külön helyen: `Client/build/layer-live-net-20261001/` (a meglévő csomagok és konfigok érintetlenek).

Előkészítés a GUI-ban: projekt megnyitása → toolbar **Build** („Build succeeded — module reloaded”,
`Layer_Live_Net_Smoke.dll`, 1 natív osztály) → Tools › Layered world › *Generate layers from collision*
(6 volume: #1 Floor, #2–#5 lépcsőfok, #6 pihenő; 5 bizonyított portál) → spawn X/Z = 4, 0, volume 1 →
*Export strict server world* → `gameserver --validate-world-package`: **VALID**, errors=0.

| Lépés | Eredmény |
|---|---|
| Play → a script „Login” promptja (felhasználónév, majd maszkolt jelszó) | megjelent, a jelszó `*****` |
| loginserver: handshake (build `ixtreeme-scene-script`) → login OK → 1 karakter → select → handoff-token | PASS (szerverlog) |
| gameserver: handshake **protocol 2** → `Using layered player spawn region 1 at 4, 0 on volume 1 z=1` | PASS |
| script: `entered world: net 1 at server (4.00, 0.00, 1.00) volume 1 layer 1` | PASS |
| jobb nyíl: Floor → 4 lépcsőfok → pihenő; script-log `level change: volume 1→2→3→4→5→6`, z 1.00 → 1.25 → 1.50 → 1.75 → 2.00 | PASS |
| bal nyíl: vissza 6→5→4→3→2→1, majd újra fel 1→…→6 | PASS |
| frame-ek: kizárólag v3 (`0x12`), ~20/s (pl. 1901 db, v1/v2 = 0) | PASS |
| Play leállítása → a stream bezárul; szerver: disconnect → cleanup → despawn | PASS |
| **két kliens** (második szerkesztőpéldány külön munkakönyvtárból, a projekt másolatával; a második fiók): mindkettő bent, kölcsönös `spawn net N` proxy (`SpawnMesh`) | PASS |
| az egyik kliens mászik → a másiknál `net N level change: volume 1 -> 2 … 5 -> 6` (a `0x08` delta-bit élőben) | PASS, mindkét irányban |
| az egyik kliens leáll → a másiknál `despawn net N`, proxy törölve | PASS |
| Play újraindítás bejelentkezés nélkül, majd leállítás → a prompt eltűnik, kapcsolat nem nyílik | PASS |
| szerver- és login-log: warning/error a teszt alatt | 0 |
| Vulkan-validációs hiba a két szerkesztőlogban | 0 / 0 |

A teszthez szükséges, csak a tesztmappát érintő igazítások:
- A 48×32 m-es clearance-terep kisebb a gameserver fordítási idejű 240 m-es partíció-minimumánál
  (2 × AOI-sugár), ezért a teszt-projektben 256×256 m-es sík terepet generáltam
  (`clearance_terrain_exact_256.height`; a régi fájl maradt).
- A gameserver-konfig másolata (`servers/game/gameserver.live.conf`) `partition_regions=1x1`-gyel
  (fájl-világnál az alapértelmezés 2×2 régió, ami 128 m-es levelet adna).

Megfigyelés (review-ra, nem 3D-5C2 hiba): a pihenő 14 m-nél végződik, a játékos középpontja 13.90 m-ig
jutott, azaz a 0.35 m-es kapszula a nyitott perem fölé lóg; a támasz a középpontra számít (3D-4B
szemantika).

## Nem futtatott / függő (nem PASS)

- A Lua-kötések (`Net*`, `Prompt*`) futás közben nincsenek kipróbálva (csak fordulnak).
- `TcpStream_Posix.cpp` Windows alatt nem fordul (`#if !defined(_WIN32)`); FreeBSD/Linux build nem futott.
- A standalone runtime (WITH_EDITOR=OFF) build nem futott újra (nincs meglévő runtime build-könyvtár).

## Hatókör-bővítések (review-ra jelölve)

1. ABI v4: a v3 game-modulokat újra kell fordítani.
2. Play-beli bemenet: szövegmező fókusza alatt a billentyűk nem jutnak a játékoshoz (eddig a
   karakter-irányítás alatt az ImGui-fókusz figyelmen kívül maradt).
3. `TcpStream` az elküldött bájtokat nullázza; `NOMINMAX` a Win32 fordítási egységben.
4. A worldbench a kliens platformrétegének `TcpStream` forrásait és a scriptet (codec-only) is fordítja a
  kereszttesztekhez (csak a bench-célpont, a gameserver nem).
