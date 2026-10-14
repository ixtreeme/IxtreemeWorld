# Standalone I0–I1 – implementációs review

2026-09-28. **IMPLEMENTED + OFFLINE TESTS PASS. Teljes integráció: PARTIAL / BLOCKED.**

Elkészült a külön IxtreemeWorld admission-profil, a sémaalapú login → lista → exact u64
CharacterId → token/handoff → külön game-handshake → EnterWorldAccept út és a Noesis
bekötés. A meglévő Auriga profil megmaradt. A végső statikus Debug build, 15 offline/loopback
tesztcsoport 851 assertionje és a változatlan U-final protocol/presence/netstress regressziója zöld.

**Valódi kétklienses admission/presence nem futott.** Nem kaptam engedélyezett login/game
endpointot, két tesztidentitást, a futó környezet igazolását és jogosult presence-diagnosztikát.
Nem használtam kitalált credentialt, univerzális tokent, auth-bypasst vagy DB seedet.
A GUI renderindítása részben ellenőrzött; a teljes grafikus admission és a pending ablakbezárás
nem kapott PASS-t. I0–I1 után STOP; I2–I6 nem indult. Commit/push/reset/stash nem történt.

## Baseline és build

Elsődleges munkaspecifikáció: a felhasználó által kijelölt
`IxtreemeWorld_Codex_Standalone_I0_I1_Admission_Implementation_Prompt.md`.
Előzmény: [összehasonlító audit](D:/IxtreemeWorld/docs/client-compatibility/client-target-comparison-audit.md).
Új, külön bizonyítékcsomag:
[client-i01-20260928](D:/IxtreemeWorld/build/client-i01-20260928).

| Referencia | Rögzített állapot |
|---|---|
| Standalone | Nem git repo. 441 eredeti forrás-/assetfájl SHA-256 és teljes elkülönített másolat a `before` könyvtárban; auditbeli referencia-exe és CMake cache is másolva. |
| IxtreemeWorld | `With_Auriga`, HEAD `ab2458dee6aa10ef488aa39fd85f1b67b480e2fa`, elfogadott dirty munkával; 522 baseline-fájl. |
| U-final | 227 rögzített fájl összevetve; az egyetlen eltérés továbbra is `gameserver/scripts/tc4_acceptance.py`, a korábbi riportoló script. A rögzített production források egyeznek. |
| Eredeti Auriga | `ECS_+_BGFX`, HEAD `c06a47197d1092f055a334e489b9b6bbd0d8c369`; 2278 fájl védett. |
| Korábbi output/evidence/test_zone | 1541 fájl hash-elve. A zárolt `.vs` IDE-indexek kimaradtak; forrás, asset és bináris nem maradt ki emiatt. |

A baseline első output-hash kísérletét egy zárolt `.vs/...vsidx` állította meg.
A már elkészült forrásbaseline felülírása nélkül folytattam az IDE-cache kizárásával.
A korábbi audit és U-final evidence változatlan maradt.

Windows x64, VS18/MSVC **19.51.36248.0**, C++17, **/MTd**, meglévő `x64-windows-static`
Crypto++/Cap'n Proto; Vulkan **1.4.350**, Noesis **3.2.13**, Granny static Debug.
Sem telepítés, sem vcpkg integrate nem futott. A Cap'n Proto generált fájlok az új build
`generated/schema` könyvtárába kerülnek; közös séma/server CMake-bootstrap nem módosult.

| Futtatás | Native exit | Monoton elapsed |
|---|---:|---:|
| Eredeti forrás izolált configure + build | 0 / 0 | 3,875 + 21,849 s |
| I1 első configure + teljes kliens/test build | 0 / 0 | 3,246 + 26,178 s |
| Végső incremental kliens + teszt build | 0 | 8,405 s |
| Végső közvetlen offline/loopback futás | 0 | 0,555 s |
| Végső CTest | 0 | 0,531 s |

Pontos parancsok, UTC start/end, native exit és további futások:
[run-summary.json](D:/IxtreemeWorld/build/client-i01-20260928/run-summary.json).
A hibás/korai futások logjai megmaradtak.

Végső `VulkanClear.exe` SHA-256:
`3E1230383379A8C486B20D8EC8566864C7423A97B8FBA21D3FADB62F481F3405`.
Végső `AdmissionTests.exe`:
`253804205920EA8249B67029AB661810370B8BBD47541847C2D9C68CD06019BD`.
[Binárismanifeszt](D:/IxtreemeWorld/build/client-i01-20260928/binaries-final.json),
[dependency hash-ek](D:/IxtreemeWorld/build/client-i01-20260928/dependencies.json),
[végső source/config/asset manifest](D:/IxtreemeWorld/build/client-i01-20260928/client-final.json).

## Tényleges fájlok és API-határ

Nyolc meglévő forrásfájl módosult, tíz új fájl készült.
Teljes lista: [i01-files.json](D:/IxtreemeWorld/build/client-i01-20260928/i01-files.json);
review-diff: [I01-only.diff](D:/IxtreemeWorld/build/client-i01-20260928/I01-only.diff).

| Felület | Konkrét megvalósítás |
|---|---|
| Config/profil/content | [Config.cpp](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/Admission/Config.cpp:26): `ValidEndpoint`, `LoadConfig`, `ValidateContent`; [Run](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/main.cpp:420): explicit CLI, hibára nincs fallback. |
| Framing/send queue | [Admission.cpp](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/Admission/Admission.cpp:45): `Framer::Append/Pop`, `SendQueue::Push/Consume`. |
| Admission állapotgép | [Core::Start](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/Admission/Admission.cpp:103), `Select`, `Poll`, `Handle`; tulajdonolt lista/own modell az [Admission.h](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/Admission/Admission.h) fájlban. |
| Socket | [Transport.cpp](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/Admission/Transport.cpp): főszálas nonblocking Winsock, connect SO_ERROR, partial I/O, generáció és RAII close. |
| UI | [NoesisLayer.cpp](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/NoesisLayer.cpp:1759): I1 early branch a legacy credential/log út előtt; `OnAdmissionNext/Enter/Cancel`, `UpdateAdmission`; új `Admission.xaml`. |
| Legacy kizárás | [főhurok](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/main.cpp:652): I1-ben a legacy Process ág nem fut; [legacy callback guard](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/NoesisLayer.cpp:2139): nincs legacy login/world mellékhatás. |
| Content-root | `RuntimePaths.h`; a négy rendererben kizárólag az eddigi exe-directory helper konfigurált content-root visszatérése változott. Renderer/gameplay algoritmus nem. |
| Build/teszt | [CMakeLists.txt](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/CMakeLists.txt:1), [AdmissionTests.cpp](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/tests/AdmissionTests.cpp); GUI és headless ugyanazt az AdmissionCore-t linkeli. |

Tényleges szerződésforrások változatlanul:
[packet.capnp](D:/IxtreemeWorld/shared/protocol/schema/packet.capnp),
[Serialization.h](D:/IxtreemeWorld/shared/protocol/include/protocol/Serialization.h),
[Framing.cpp](D:/IxtreemeWorld/gameserver/libs/network/src/Framing.cpp),
[login ConnectionHandler](D:/IxtreemeWorld/loginserver/apps/loginserver/src/ConnectionHandler.cpp),
[GameHandler::HandleCharacterSelect](D:/IxtreemeWorld/loginserver/apps/loginserver/src/GameHandler.cpp:90),
[gameserver handler](D:/IxtreemeWorld/gameserver/apps/gameserver/src/GameConnectionHandler.cpp:193),
[SpawnCoordinator](D:/IxtreemeWorld/gameserver/apps/gameserver/src/world/spawn/SpawnCoordinator.cpp:79),
[player NetId allocator](D:/IxtreemeWorld/gameserver/apps/gameserver/src/world/replication/NetworkEntityId.h:12).

## Profil és állapotgép

`--profile Auriga` vagy `--config <fájl>`; hiányzó/hibás profil indulási hiba.
Nincs auto-login, profile fallback vagy futás közbeni váltás. Az I1 game cél a loginserver
handoffjából jön, kötelező exact host+port allowlisttel. A configút exe-relative,
a content-root config-relative. Más cwd-ből azonos eredményt unit teszt igazol.

**Jelenlegi címkorlát: numeric IPv4.** Hostname/IPv6 explicit elutasítás, nincs DNS vagy címátírás.
Ez elkerüli a blokkoló resolver útját; a tényleges live környezet kompatibilitását még ellenőrizni kell.
A mintakonfiguráció érvénytelen helyőrzőket tartalmaz, nem működő célt.

Login és game két külön socket-élettartam/generation, külön handshake, aktuális protocolVersion=1.
Csak saját listából kiválasztott exact u64 kerül CharacterSelectbe; nincs u32/float UI-tárolás.
Saját identitás kizárólag valid game EnterWorldAcceptből: NetId 1..999999, véges spawn,
eredeti u32 serverTick. TCP connect, handshake és queued EnterWorld nem Accepted.
Dupla callback/üzenet nem újabb küldés vagy implicit siker; wrong-phase kapcsolatfailure.
Handoff utáni bizonytalan write/close esetén új login/select/token út szükséges, nincs blind replay.

Az accept után known lifecycle és 0x11 world-forgalom bounded admission-only sinkbe kerül.
Nincs actor, recipient-baseline, map load, movement vagy combat. A teljes 0x11 rekord/delta decode
**NOT_IMPLEMENTED_IN_I1**; első own rekord fix 19 byte. Későbbi world-replica új session/baseline-t kér.

Részletes fázistábla, timeout, titokkezelés és pontos build/run útmutató:
[kliens dokumentáció](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/docs/ixtreeme-i0-i1-admission.md).

## Unit, loopback és negatív eredmények

**15 csoport / 851 assertion PASS** a végső tesztbinárissal:

- Explicit profil és endpoint-hiba, Auriga/I1 izoláció; más cwd, hibás/dupla configkulcs, hiányzó content.
- Független BE-byte orákulum; teljes frame minden prefix/body bytehatáron kettévágva,
  byte-onként fogadva; coalescing és residual fragment megtartása.
- Min/max/max+1, nulla/hibás/u32-max hossz, csonka header/body, buffer overflow, nincs desync.
- Exact egyszeri/sorrendtartó partial write és would-block script; explicit queue overflow.
- Shared szerver-deserializerrel ellenőrzött kliens handshake/login/list/select/enter;
  u64 `4294967297` és `9007199254740993`, külön game handshake, nincs debugSpawnOverride.
- Minden fázis wrong-phase mátrixa, duplikált válaszhatárok, dupla Login/Select; empty list és rejectek.
- Minden pending fázis cancel/close; stale connect/read/Poll/close generáció, friss attempt,
  régi login-close az accepted game mellett, ismételt cleanup.
- Withheld response és slow trickle abszolút timeout, késői accept nem válik identitássá.
- Hibás szegmenstábla/pointer/alignment/trailing bytes, tulajdonolt reader-élettartam,
  mély/ciklikus graph és 500-szorosan aliasoló amplification fixture korlátos elutasítása.
- Rossz endpoint/tokenméret, unknown enum/protocol version, nem véges spawn, invalid/reserved NetId.
- Accept + spawn + 0x11 egy bufferben; 50-frame burstből első Poll 16-ot kezel,
  a többi megmarad; 1000 további world frame mellett nincs history/kimenő movement.
- Production Winsock Transporttal tényleges ephemeral loopback TCP login és game,
  szintetikus szerverválaszokkal; a peer a lezárásig nem kap extra movement/attack/keepalive byte-ot.
- Pending transport megsemmisítés és elérhetetlen localhost cél bounded failure.

A fixture-ek a tényleges shared `SerializeToBytes` encoderből készülnek; a klienskimenetet
shared `ParseUntrustedPacket` ellenőrzi. A world `ProtocolEncoder` közvetlen linkelése
**SKIPPED**, mert headerfüggősége Flecs; a kliensbe nem húztam be ezt a runtime-ot.
A külön BE/phase/graph orákulum nem a production helper újrahívásából állítja az elvárt eredményt.

Az első offline futás utolsó unavailable-endpoint assertionje **FAIL** volt: a harness 2 s után
számon kérte a lezárást, miközben a production konfiguráció connect-határa 5 s volt.
A dedikált negatív teszt 100 ms connect-konfigurációt kapott és a bounded transport-error/timeout
eredményt ellenőrzi. Production timeoutot nem emeltem; korábbi FAIL megmaradt a csomagban.

Synthetic credential/token sentinel a runtime/build logokban nem szerepel.
A tesztforrás/bináris természetesen tartalmazza a fixture literáljait; ezek nem élő hitelesítő adatok.
A raw peer-auth üzenetet és könyvtári exception-szöveget a production út nem logolja.

## Valódi kétpéldányos admission és GUI

| Tétel | Státusz / bizonyíték |
|---|---|
| Vulkan/Noesis I1 login render | **RUN_VERIFIED, részleges GUI-próba**: két izolált indítás C:/Windows cwd-ből; látható I1 profil, ProfileReady, nulla attempt/own/world-frame. Renderer/model/shader/Noesis betöltés sikeres. |
| Első grafikus finding | Noesis Debug Inspector automatikus listener `0.0.0.0:17629`, Windows tűzfalablak. Az I1 source most `DisableInspector()`-t hív Init előtt. |
| Javított startup | I1 képernyő/karakterválasztó szöveg látható, PID 43632; az ellenőrzéskor nem volt TCP socket/listener. A régi Windows biztonsági prompt még takarta a felületet. |
| Teljes Noesis admission | **BLOCKED**: nincs jóváhagyott live endpoint/identitás; nem történt jelszóbevitel. |
| Két distinct own NetId és authoritative presence | **NOT_RUN / BLOCKED**. A mock nem bizonyítja. |
| GUI pending-close, Cancel és két GUI reconnect | **NOT_RUN** a fennmaradó security overlay/live bemenetek miatt. Core/Transport cleanup offline lefedett. |
| GUI process cleanup | A két saját startup-processzt kényszerített saját-PID lezárással takarítottam. Ez nem normál ablakbezárás-PASS. |
| Végső bináris GUI acceptance | **NOT_RUN**: a részleges screenshotok a korábbi buildből származnak; a végső kisebb callback-guard/buffer változat build+headless tesztje igazolt. |

A tűzfalablakhoz nem nyúltam. A használt
[computer-use SKILL.md](C:/Users/ixtre/.codex/plugins/cache/openai-bundled/computer-use/26.917.62051/skills/computer-use/SKILL.md)
által előírt [guidance](C:/Users/ixtre/.codex/plugins/cache/openai-bundled/computer-use/26.917.62051/docs/guidance.md)
pontos szabálya: **“Do not act on security or privacy permission requests.”**
Ez a Windows prompt automatizált kezelését tiltja; a felhasználónak jeleztem a kézi kezelés szükségét.
Rendszer-/tűzfalbeállítást nem módosítottam. A forrásoldali I1 Inspector-kikapcsolás nem OS beállítás.

Valamennyi valódi bad-auth/non-owned-character/version/token-expiry/reuse/duplicate-presence
és pending-disconnect eset **NOT_RUN**, megfelelő környezet nélkül. Ismeretlen vagy más tulajdonú
karakterre a jelenlegi loginserver nem feltétlenül küld külön rejectet: kliens select-timeout
várható, nem kitalált ownership-hibakód. Korábbi login-elutasítás esetén az alreadyInWorld ág
**NOT_REACHED** lenne; ehhez nincs auth-megkerülés.

## Erőforrás, várakozás és lifecycle

Receive tároló 131080 byte; send összesen 131080 byte és legfeljebb 128 frame.
Max payload 65536, max 64 karakter, max 1024-byte opaque token, bounded peer-text.
Ez a protokollréteg korlátja, nem a Vulkan/Granny processz összmemóriája.
A GUI egyik read-only mintája 174919680 byte working set volt; ez **nem peak/memória-benchmark**.

Főszálas nonblocking select/connect/send/recv, nincs háttérhálózati thread vagy detached callback.
Pollonként 16 dispatch; UI Update-enként legfeljebb 80 dispatch, 32768 byte read/write.
A teljes későbbi frame megmarad; a stream nem resyncel találgatással. A token/queue tárolók
cleanupkor törlődnek; teljes allocator/UI/processzmemória zeroization nincs állítva.

Default connect 5 s, handshake/auth/list/select 10 s, felhasználói választás 30 s,
EnterWorld 30 s, Accepted megfigyelés 10 s. Az enter limit az aszinkron előkészítésnek ad
helyet; live teljesítménymérés hiányában nem garantált completion-idő.
Nincs setup/idle/TTL szerverváltozás, settle-várakozás, populáció/cache-változtatás vagy validátorlazítás.

## Regresszió és megőrzés

U-final worldbench SHA-256:
`75A04BECAE26D4A41086C5A508D1448093907AE3ECBDE5777729C96A9CD21DB0`.
Az új evidence cwd-ből, változatlan paraméterezett tesztmódok:

| Regresszió | Eredmény | Elapsed |
|---|---|---:|
| `--mode protocol` | exit 0, failures=0 | 3,572 s |
| `--mode presence` | exit 0, failures=0 | 21,159 s |
| `--mode netstress` | exit 0, failures=0, default 4 IO thread/all/chaos | 10,196 s |

A tesztek saját szintetikus/loopback környezete nem valódi loginserver/DB admission.
A netstress DB poolja nem indul. A nagy teljesítménymátrix nem futott újra, ahogy a specifikáció engedi.

[Végső megőrzési ellenőrzés](D:/IxtreemeWorld/build/client-i01-20260928/protection-final.json): **PASS**.
522 meglévő IxtreemeWorld fájl, 2278 Auriga fájl, 1541 korábbi output/evidence/test_zone fájl
között nincs módosított/hiányzó. A standalone nyolc szándékos forrásváltozásán kívül
az eredeti 441 fájl többi része változatlan. Meglévő asset, checked test_zone és korábbi evidence
nem íródott felül. A szerverrepóba csak ez az új riport került, implementáció nem.
Unity nem volt munkaterület.

## Nyitott gate és átadás

- **BLOCKED:** engedélyezett login/handoff célok, két előkészített identitás, interaktív credential,
  futó szerver+világcsomag azonosítása, jogosult presence/audit forrás.
- **GUI PARTIAL:** a Windows biztonsági prompt kézi rendezése után a végső binárissal ismételendő
  a normál Noesis belépés/cancel/pending-close és kétpéldányos próba. Nincs rejtett GUI-PASS.
- **NOT_RUN:** legacy live kapcsolat, Auriga GUI-regresszió a modal fennállása alatt,
  Release/RelWithDebInfo kliens a nem igazolt CRT/Granny páros miatt.
- **Korlát:** I1 numeric IPv4; teljes 0x11 replica, actor, map és movement nincs.
- **Következő munka csak külön engedéllyel:** I2 map/adatprofil, I3 actor/baseline, I4 movement,
  I5 migration/split/merge, I6 teljes world reconnect. Az I1 hiányzó live gate-jét előbb tisztázni kell.

A titokmentes configminta, build/run parancsok és eseményhez kötött kétklienses acceptance-eljárás
a [standalone dokumentációban](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/docs/ixtreeme-i0-i1-admission.md) van.
