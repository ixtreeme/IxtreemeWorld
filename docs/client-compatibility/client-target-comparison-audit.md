# Auriga és StandaloneVulkanClear – összehasonlító integrációs audit

Dátum: 2026-09-28. Hatókör: a felhasználó legutóbbi helyesbítése szerinti **szűk forrásaudit**, CA-0 → CA-2. A StandaloneVulkanClear az Auriga alternatív kliense; nem az IxtreemeWorld elveszett kis kliense. Utóbbira nem épül a javaslat. Unity-forrást nem auditáltam és nem módosítottam.

## 1. Verdikt és célazonosság

**Az első kétklienses infrastruktúrateszt alapjának a StandaloneVulkanClear klienst javaslom, feltételekkel.** Ennek oka a közvetlen, kicsi network → world-state → render illesztési felület, az elérhető laza map/modell/animáció állományok és a most megismételt izolált build. Nem a Vulkan renderer alapján választottam.

**Egyik kliens sem kompatibilis változtatás nélkül az új gameserverrel.** Mindkettő Auriga session-/packet-szemantikát használ. Az IxtreemeWorld hosszprefixes, codec-jelölős Cap'n Proto/binary üzeneteihez egyik vizsgált aktív útban sincs kész fogadó–fogyasztó lánc. Egy endpointcsere elégtelen; az első protokollakadály már a handshake kezdeményezője és framingje.

Az eredeti kliens több kész játékvilág-funkciót ad: actor-kezelés, animáció, mapválasztás, attribútum- és objektumütközés, tényleges stop-esemény, több megjelenési típus. Cserébe Python 2, csomagolt tartalom, race/appearance-adatok, UI-fázisok és a régi pozícióalapú mozgásmodell együtt illesztendők. A normál mozgásfogadó út saját actorra nem alkalmazható. A standalone hiányai konkrétan felsorolhatók, és nem követelnek renderer- vagy gameplay-fejlesztést a szűk, terrain-alapú első teszthez.

Ez **forrásból indokolt munkaterület-választás**, nem mért implementációs idő vagy kész kétklienses PASS. Ha a cél már az első próbán teljes Auriga objektum-/híd-/combat-viselkedés lenne, a választást újra kellene értékelni; ez most nincs a hatókörben.

Állapotjelölések: `SOURCE_VERIFIED` = aktív forráslánc; `RUN_VERIFIED` = az itt megnevezett izolált próba; `PROPOSED` = következő munka; `NOT_FOUND_IN_SCOPE` = a leírt keresési körben nem találtam; `NOT_RUN` = nem futtatott. **A szűk forrásaudit elkészült; tényleges klienskapcsolat és kétklienses E2E nem futott.**

## 2. Forrás-, build- és tartalomleltár

| Cél | Azonosítás és védelem | Build / indítás / tartalom |
|---|---|---|
| Eredeti Auriga | `D:/AurigaGlobal/LiveWork/AurigaGlobal`, `ECS_+_BGFX`, HEAD `c06a47197d1092f055a334e489b9b6bbd0d8c369`; kezdetben tiszta git státusz. 2278 követett fájl SHA-256 baseline. A megadott Launcher alatti aktív utakat vizsgáltam. | `UserInterface` CMake target, Windows x64, C++23, VS18/2026, meglévő cache: static triplet, ASan OFF. Debug/Release/RelWithDebInfo exe létezik; ezek aktuális forráshoz tartozása nem bizonyított. Friss teljes build: **NOT_RUN**. |
| Standalone | `D:/AurigaGlobal/LiveWork/StandaloneVulkanClear`; nem git repository, így branch/HEAD/dirty státusz helyett 441 forrás-/tartalomfájl hash-manifest. A meglévő `build` külön output, nem forrásbaseline. | `VulkanClear`, Windows x64, C++17; WinMain és Debug main ugyanazt a `Run` függvényt hívja. VS18 MSVC 19.51, Vulkan SDK 1.4.350, DXC, Noesis 3.2.13, Granny static Debug lib, Crypto++. A sikeres friss build külön könyvtárba készült. |
| Új szerverek | `D:/IxtreemeWorld`, `With_Auriga`, HEAD `ab2458dee6aa10ef488aa39fd85f1b67b480e2fa`; a korábbi MAP/SL/TC dirty munka változatlan referencia. Gameserver + a szükséges loginserver handoff út. | U-final 227 hash-elt fájljából 226 egyezik; az egyetlen eltérés `gameserver/scripts/tc4_acceptance.py` (riportoló script). **A nyilvántartott production szerverforrás egyezik U-final-lal.** A loginserver összevetése az aktuális, külön hash-elt munkafából történt; nem állítom, hogy minden loginserver-fájl szerepelt U-finalban. |
| Régi Auriga szerver | Ugyanazon Auriga repo `SRC/Server` fái. | Csak a handshake-kezdeményezés és pozícióalapú movement szerződés tisztázására; nem teljes szerveraudit. |

Baseline és próbaanyag: [bizonyítékcsomag](D:/IxtreemeWorld/build/client-audit-20260927), benne `client-baseline.json`, `auriga-baseline.json`, `server-baseline.json`, kezdeti git státuszok, `u-comparison.json`. A könyvtár neve a munka kezdeti napját őrzi.

**Standalone buildbizonyíték.** A meglévő cache `x64-windows` Crypto++ függőségét követő izolált Debug build natív exit **1**, 19,16 s: `/MTd` target és dinamikus CRT-s Crypto++ között `__imp__aligned_free`, `__imp__aligned_malloc`, `__imp_clock` linkhibák. Ez nem parser-/protokollhiba. A már telepített `x64-windows-static` triplettel, módosítatlan kliensforrásból a külön második konfiguráció buildje exit **0**, konfigurálással együtt 25,93 s. Nincs dependency-install vagy toolchain-frissítés. A sikeres profil Debug; Release újrafordítás és grafikus indítás **NOT_RUN**. A CMake alapértelmezett Granny libje Debug: Release CRT/SDK egyezését külön kell ellenőrizni.

Forrás: [Standalone CMake](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/CMakeLists.txt:147), `add_executable`, compile definitions, runtime és POST_BUILD (147–286 körüli blokk); [Auriga target](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/CMakeLists.txt:1). A standalone POST_BUILD csak a target könyvtárába másol Noesis DLL-t, `xaml`, `fonts`, `Character`, `Maps`, `textureset`, `ymi work`, shader tartalmat; ezért az izolált build nem írta felül a meglévő kiadást.

Az eredeti repo stock configure-je `vcpkg_require` esetén csomagot telepíthet és `vcpkg integrate install` műveletet is futtat: [BootstrapVcpkg](D:/AurigaGlobal/LiveWork/AurigaGlobal/cmake/BootstrapVcpkg.cmake:54), 54–71 és 225–239. Emiatt ebben az auditban nem indítottam el a teljes konfigurálást/buildet. Ez **NOT_RUN**, nem bizonyított fordítási hiba. A korábbi bináris jelenléte önmagában nem friss build-PASS.

**Tartalomgyökerek.** Standalone: az exe melletti laza fájlok, `Character/warrior_4-1.gr2`, textúrák és `BaseAnim/{wait,walk,run}.gr2`; térkép `Maps/metin2_map_main_razor93`. XAML/Noesis UI, nincs Python world-indítási függőség. [Run assetbetöltés](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/main.cpp:456), [Warrior mozgások](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/WarriorRenderer.cpp:1195).

Eredeti: `WinMain → Main → PackInitialize("pack") → RunMainScript → system.py → prototype.py → MainStream.SetLoginPhase → app.Loop`. [UserInterface.cpp](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/UserInterface.cpp:150), 150–285, 322–356, 442–489; [prototype.py](D:/AurigaGlobal/LiveWork/AurigaGlobal/root/prototype.py). `__USE_CYTHON__` jelenleg kikommentezett a `Locale_inc.h:3` helyen. A loose `root` könyvtár létezése nem bizonyítja, hogy annak tartalma fut: [RegPack.h](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/RegPack.h:14) pack-keresést és `root.AG`/`maps.AG`/`pc.AG` stb. regisztrálást ír elő; [EterPackManager::Get](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/Pack/EterPackManager.cpp:80) pack-prioritást és whitelisthez kötött file-fallbacket használ. A repo gyökerében és a vizsgált Release output mellett `pack` nem található. A tényleges terjesztett Auriga runtime content-root **UNVERIFIED**, nem állítom, hogy máshol sem létezik.

**Szerverprofil.** Nem indítottam production szervert és nem olvastam titkos DB-konfigurációt. A forrás szerinti alap: protokollverzió 1; v2 replikáció és Network LOD ON; 20 Hz; file world, kötelező validált package, strict warp, eager terrain alapértelmezés, példakonfig port 11020. CLI/config override lehetséges, ezért ez nem állítás egy éppen futó szolgáltatás profiljáról. [main.cpp](D:/IxtreemeWorld/gameserver/apps/gameserver/src/main.cpp:85), [ReplicationConfig](D:/IxtreemeWorld/gameserver/apps/gameserver/src/world/replication/ReplicationConfig.h:12), [példakonfig](D:/IxtreemeWorld/gameserver/apps/gameserver/config/gameserver.conf.example:1). Debug/RelWithDebInfo spawn override csak meglévő validáción keresztül; nem auth-bypass.

## 3. Tényleges hálózat és belépési út

Mindkét kliensben **valódi TCP socketes út van**, nem pusztán lokális mozgásdemó. Ez `SOURCE_VERIFIED`; successful login/world kapcsolatot nem futtattam. A lokális terrain/render/prediction működésből nem következik szerverkompatibilitás.

| Lépés | Eredeti Auriga | Standalone | Új szerver megfelelője / eltérés |
|---|---|---|---|
| Indítás | Python login UI; `MainStream.Connect`, `net.ConnectToAccountServer` / `ConnectTCP` | `NoesisLayer::Impl::OnLoginClicked → CAccountConnector::Connect` | Új loginserver kliens-kezdeményezett handshake-et vár. |
| Endpoint | UI/script által választott game/account cím | Auth-cím és csatorna host/portok forrásba rögzítve; nincs új-szerver profil | Profilozható külön login/game endpoint kell. Az audit nem kapcsolódott a beégetett külső címre. |
| Handshake | Server phase/handshake, key agreement, cipher aktiválás | Ugyanez; `OnConnectSuccess` csak HANDSHAKE állapotba áll | IxtreemeWorld: u32 BE hossz + codec 0 + Cap'n Proto HandshakeRequest, protocolVersion=1. A Metin server-first várakozás nem egyezik. |
| Login | Login3/auth result → login key → game login/select | AccountConnector → `OnLoginSuccess` → `ConnectForCharacterList` → Login2 | Új loginserver: handshake → LoginRequest → authenticated character list. Nincs Login2 uint32 key megfelelő. |
| Kiválasztás | slot, empire/character UI-fázisok | `EnterSelectedCharacter(slot, channel)`; reconnect csatornánként | Új `CharacterSelect.characterId` u64: a szerver account-ownershipt ellenőriz. Slot nem CharacterId. |
| Handoff | Legacy game select/loading | PHASE_LOADING alatt client version + egybájtos ENTERGAME | Új loginserver egyszer használatos token + gameHost/gamePort; új game-socketen handshake, majd EnterWorld(token). |
| Világba engedés | MAIN_CHARACTER beállítja main VID-et, Python LoadData, Warp; ADD/ADDITIONAL_INFO actorokat épít | MAIN_CHARACTER → `PublishEnterWorld` → Noesis `worldActive` → fix map load, helyi actor | Új EnterWorldAccept(yourNetId, spawnPos, serverTick), majd lifecycle/transform üzenetek. Nincs Metin main-character/phase üzenet. |
| Bontás | RemoteDisconnect → Python SetLoginPhase; leave-game felszabadítja actor/item managereket | `OnRemoteDisconnect → SetOffline → ResetWorldState`; UI worldActive értesítés hiányzik ezen az úton | Új Session stop → context törlés → world despawn; kliensben atomikus session/world cleanup szükséges. |

Hivatkozások: [eredeti MainStream.Connect](D:/AurigaGlobal/LiveWork/AurigaGlobal/root/networkModule.py:247), [eredeti handshake](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/PythonNetworkStreamPhaseHandShake.cpp), [eredeti login phase](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/PythonNetworkStreamPhaseLogin.cpp:58), [eredeti main-character](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/PythonNetworkStreamPhaseLoading.cpp:153); [standalone login callback](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/NoesisLayer.cpp:1744), [login success](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/NoesisLayer.cpp:2084), [game connect/dispatch](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/ClientNet/UserInterface/GameNetworkStream.cpp:626), [phase handler](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/ClientNet/UserInterface/GameNetworkStream.cpp:953).

Új szerver: [login ConnectionHandler](D:/IxtreemeWorld/loginserver/apps/loginserver/src/ConnectionHandler.cpp:45), [GameHandler::HandleCharacterSelect](D:/IxtreemeWorld/loginserver/apps/loginserver/src/GameHandler.cpp:90), [gameserver ProcessPacketLocked](D:/IxtreemeWorld/gameserver/apps/gameserver/src/GameConnectionHandler.cpp:193), [HandleEnterWorld](D:/IxtreemeWorld/gameserver/apps/gameserver/src/GameConnectionHandler.cpp:283), [SpawnCoordinator::Spawn](D:/IxtreemeWorld/gameserver/apps/gameserver/src/world/spawn/SpawnCoordinator.cpp:79). A token tárolási TTL itt 30 s; a karakter az auth account alapján ellenőrzött. A world presence elsőként beengedett sessionje nyer; a második `alreadyInWorld` választ kap. Nem javaslok univerzális tokent, kliens által önkényesen megadott identitást vagy auth megkerülést. A korábbi Auriga `ALREADY` login-szöveg nem automatikusan azonos ezzel a reject enum-mal.

**Fontos állapothatár:** az új GameConnectionHandler a karakterbetöltés után `InWorld` kontextust állít és `PostSpawn`-t ad; a tényleges world/admission elkészültét a kliens számára az EnterWorldAccept jelzi. Az új kliensprofil ezen üzenetig nem kezdhet movement-küldésbe. A terrain Ready, a sorba állítás és a world authority commit nem felcserélhető.

## 4. Packet- és transport-kompatibilitás

| Irány / szemantika | Meglévő kliensek | IxtreemeWorld wire és fogyasztó | Minősítés / szükséges határ |
|---|---|---|---|
| Keretezés | u8 header, pack(1) fixed struct vagy u16/u32 dynamic méret; Win32 little endian memóriaképek, feltételes layout | 4 byte BE payload-hossz, maximum 65536; payload codec 0 Cap'n Proto flat array vagy codec 1 binary | **Eltér**, saját profil parser kell mindkettőben. |
| Handshake ↔ | GC/CG 0xF0, 13 byte; phase 0xEE, 2 byte; encryption key agreement | schema HandshakeRequest/Response, `kProtocolVersion=1` | Nincs közvetlen megfelelő packet. A verzió 1 nem azonos a transform v1/0x10 verzióval. |
| Login/select C→S | Login3=44; Login2=109; select=6 slot u8; ENTERGAME=10 | Cap'n Proto login/list/select u64 ID/token/EnterWorld | Új loginserver-kliens út szükséges; régi szerver mód maradhat külön. |
| Own enter S→C | MAIN_CHARACTER=113, BGM változatok 137/138, VID u32, race/name/egész koordináták | EnterWorldAccept: own NetId u32, Vec3 float méter, tick u32 | TWorldEnterInfo vagy main-actor állapot explicit feltöltése; új accept nem tartalmaz teljes legacy megjelenési/UI adatot. |
| Spawn S→C | ADD=1, ADD2=120, ADDITIONAL_INFO külön; race/type/parts/flags | EntitySpawn: NetId, név, classId, pos, heading, mobType, level, hp | Meglévő actor-upsert felület újrahasználható, de class/race és player/mob szemantikát explicit kell kötni. |
| Despawn S→C | DEL=2, VID u32 | EntityDespawn(netId) | Szemantika közel áll; session-scoped actor/baseline/history cleanup szükséges. |
| Move/stop C→S | header=7; func/arg u8; **rotation float**, nem u8; x/y int32; timestamp u32 ms. Standalone 19 byte. Pozíciójelentés + func WAIT/MOVE | payload **9 byte**: codec=1, opcode=1, seq u32 LE, heading u16 LE, state u8 (0 idle/1 walk/2 run) → PostMoveInput | Nem byte-átnevezés: irány/szándék kell, a szerver integrál; klienspozíciót nem szabad authorityként elfogadni. |
| Move S→C | header=3; float rot/5, VID, x/y, time, duration; standalone 27 byte → MoveActor / TWorldMoveSample | codec=1, opcode=0x11, tick u32 LE, count u16 LE; első saját rekord 19 byte, utána NetId u32+mask u8+mezők | Kész 0x11 parser egyikben sincs. Saját rekord **nem maskos**. Delta baseline netId-nként, lifecycle mellett. |
| Régi transform v1 | Egyik aktív kliensben sem új-szerver codec | 0x10, teljes 19 byte rekordok | V1-re visszakapcsolás önmagában sem teszi kompatibilissé a klienst. |
| Korrekció/warp | SYNC_POSITION=5; eredetin SyncActor→TEMP_Push; standalone csak entity x/y frissítés. WARP=65 eredetin reconnect, standalone log-only | Saját authoritative transform folyamatosan jön; nincs itt legacy WARP endpoint üzenet | Normál korrekció, teleport és mapváltás nem azonos. Zónaváltáskor sem reconnect, sem main-actor újrateremtés nem kell. |
| Combat S↔C | Eredetin tényleges hit/attack/skill út. Standalone ATTACK packet ismeret/skip és stat UI található; teljes attack-input→send→eredmény út nem található | AttackTarget u32 netId; health/death üzenetek | A standalone combat teljes működésére nincs bizonyíték. Meglévő combat változatlan; combat-integráció nem az első csomag. |
| Keepalive/failure | Legacy ping/pong, connect timeout, header-alapú hibautak | Session setup/idle/send-queue timeout; nincs schema legacy ping | Idle policyt és kompatibilis liveness-üzenetet külön tisztázni kell; nem küldhető találomra legacy pong. |

Források: [standalone Packet.h](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/ClientNet/UserInterface/Packet.h:1), 206–258, 376–387, 498–559, 729–740, 916–928, 957–1000; [eredeti Packet.h](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/Packet.h:828), pack(1) 453–454; [új schema](D:/IxtreemeWorld/shared/protocol/schema/packet.capnp:17), [Serialization](D:/IxtreemeWorld/shared/protocol/include/protocol/Serialization.h:17), [Framing::EncodeInto](D:/IxtreemeWorld/gameserver/libs/network/src/Framing.cpp:23), [binary move reader](D:/IxtreemeWorld/gameserver/apps/gameserver/src/GameConnectionHandler.cpp:107), [ProtocolEncoder](D:/IxtreemeWorld/gameserver/apps/gameserver/src/world/replication/ProtocolEncoder.cpp:74).

**ABI-próba (`RUN_VERIFIED`).** A valódi standalone Packet.h-val x64 MSVC alatt sizeof/offsetof: pointer 8, long 4, bool 1, float 4; vizsgált packetek align=1. Move: 19 byte, bRot@3, x@7, y@11, time@15. GC Move: 27 byte, VID@7, time@19, duration@23. ADD 38, ADD2 92, ADDITIONAL_INFO 289, MAIN 46, LoginSuccess4 419, Login3 130, Login2 52; a teljes lista [layout.txt](D:/IxtreemeWorld/build/client-audit-20260927/layout.txt). Ezeket nem állítom automatikusan minden eredeti kliens/legacy szerver build méretének. Az eredeti header közös movement mezői forrásból ellenőrzöttek; teljes eredeti layout-probe **NOT_RUN**.

Aktív standalone makrók a [CMake compile definitions](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/CMakeLists.txt:211) és a Packet.h által bevont [ServerBuildDefines.h](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/ClientNet/UserInterface/ServerBuildDefines.h:1) együtteséből jönnek: improved encryption, HWID, 5 slot, acce, costume effect, rune, multi-language/names, ATTR_LOCK, skill colors, target HP, large dynamic/offlineshop stb. A headerbe írt „server layout” megjegyzés az Auriga ágra vonatkozik, nem az új gameserverre. Az eredeti kliens makróforrása [Locale_inc.h](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/Locale_inc.h:1), saját feature-hálóval; nem elegendő a packetnév vagy enum egyezése.

**Transport és szálak.** Mindkét kliens főhurkában fut a network Process, onnan közvetlen actor/UI módosítás: [eredeti Application::Process](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/PythonApplication.cpp:475), 517–522; [standalone main](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/main.cpp:609), 613–615. WinSock nonblocking select + receive/send buffer, részpacketre Peek/HasPacket, összevont packetek dispatch-ciklusa. Standalone world-ismeretlen header disconnect; eredetin `CheckPacket` ismeretlen header ClearRecvBuffer + PostQuitMessage, phase-unknown pedig külön RecvErrorPacket. A nulla byte átugrása legacy viselkedés, nem új framing-dekóder. [Standalone NetStream](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/ClientNet/Render/NetStream.cpp:80), 80–124, 218–235, 300–411; [eredeti CheckPacket](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/PythonNetworkStream.cpp:608), 608–721.

A meglévő encryption send ág mindkét kódban a függő teljes buffert titkosítja a `send` előtt; részleges send/WOULDBLOCK esetén a visszamaradt titkosított byte-ok következő encryptje külön hibakockázat. Ez **forrásból azonosított, nem hálózaton reprodukált** megállapítás; nem javítottam. Framing-fragmentation, invalid length, cipher partial-write regresszió külön próba kell, ha a legacy transportot újrahasználják. Az új szerver Session strand sorosítja a read/write/disconnect műveleteket; async_read pontos header/body olvasással és méretkorláttal dolgozik. [Session.cpp](D:/IxtreemeWorld/gameserver/libs/network/src/Session.cpp:60), 60–78, 138–166, 179–237, 289–330.

**Replikáció illesztési határa.** A zóna workerén `ReplicationSystem::ReplicateZone → VisibilitySystem::ReconcileViewer` ad lifecycle-t, canonical snapshotot és címzettenkénti ismert állapotot. [ReconcileViewer](D:/IxtreemeWorld/gameserver/apps/gameserver/src/world/visibility/VisibilitySystem.cpp:144), 199–219, 248–354, 361–371; [frame assembly](D:/IxtreemeWorld/gameserver/apps/gameserver/src/world/replication/ReplicationSystem.cpp:130). Az ismert állapot kódolás/küldési kérés környékén előrelép, nem kliens-ACK-re. [NetworkSend](D:/IxtreemeWorld/gameserver/apps/gameserver/src/world/replication/NetworkSend.h:15) IO postot végez, majd Session saját strandjára sorol. `queued`/`written`/`received`/`applied` külön állapot. Frame-et eldobó adapter deszinkronizálna; hard queue limitnél a jelenlegi szerver bont. A belső canonical rekord létezik, de kész, leválasztott legacy adapter API-t nem igazoltam. Javaslat: a kliens fogadja a meglévő új protokollt; most ne kerüljön új encoder a szerverbe.

## 5. Map, koordináták, magasság és collision

**Egyetlen első térképjelölt:** a standalone mellett ténylegesen elérhető `metin2_map_main_razor93`. Ezen végeztem read-only adatpróbát; nem generáltam új mapot és nem írtam exportert. A már meglévő út: Metin setting/raw/tile → standalone TerrainRenderer; az új oldalon WorldPackageWriter → WorldPackage loader → ServerTerrain. Kész Metin→MXWP importert nem találtam a két kliens aktív CMake-forrásai, az új `shared/map` és gameserver tooling vizsgált körében. Ez körülhatárolt `NOT_FOUND_IN_SCOPE`, nem állítás minden külső tooling hiányáról.

| Tér / adat | Bizonyított szerződés |
|---|---|
| Auriga wire | x/y egész, cm jellegű egység: CELLSCALE=200 és CELLSCALE_IN_METER=2; globális atlas-base hozzáadás/levonás. Régi szerver távolsága x/y különbség /100. |
| Eredeti actor/world | Lokális pixelpozíció cm; grafikai belső Y előjelváltást használ. `GlobalPositionToLocalPosition`: base kivonása. A render belső cm-rendszerébe nem tehető be közvetlenül új-szerver méter. |
| Standalone world | X/Z vízszintes méter, Y magasság. X=(wireX−spawnX)/100; Z=−(wireY−spawnY)/100. Magasság a mapból abszolút méter; nem az enter Z levonásából. |
| Standalone tile | 5×5 tile, 128×128 darab 2 m cella/tile, 131×131 LE uint16 raw per tile, belső sample offset (+1,+1), HeightScale=0,5 cm/raw. Teljes grid 641×641, méret 1280×1280 m. |
| Új szerver | X/Y vízszintes méter, Z magasság; f32 wire pozíció. Manifest geometry origin/extent explicit. DB pozícióegység külön szerződés (`kDbUnitsPerMeter=1000`), nem legacy cm. Aktuális spawn út worldlogic spawn-régióból választ. |
| Attribútum | Meglévő attr.atr: 6 byte LE header (magic 2634, 256×256), utána 65536 u8; BLOCK bit 0. 1 m attribútumrács a 2 m height-rács mellett. Standalone nem olvassa. Új ServerTerrain u16 attribútumban BLOCK=1 és szakaszbejárás. |

Források: [standalone terrain reader](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/TerrainRenderer.cpp:418), 418–498, 849–875, 957–1031; [main wire konverzió](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/main.cpp:665); [eredeti base/warp](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/PythonBackground.cpp:729); [Terrain constants/raw offset](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/Terrain/Terrain.h:10), 10–62,139–142; [attr loader](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/Terrain/Terrain.cpp:94); [régi szerver movement](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Server/GameServer/entity/movement_input.cpp:79); [új world constants](D:/IxtreemeWorld/gameserver/apps/gameserver/src/world/WorldConstants.h:13).

**Mapválasztás:** eredetin `data/maps/atlasinfo.txt` → `GlobalPositionToMapInfo` → `Warp` → `LoadMap`; hibás mapválasztás logol/return, sikertelen LoadMap kilépést kér. A standalone a worldActive élén mindig a fenti fix könyvtárat tölti; nincs hálózati mapId-selector. A cél új EnterWorldAccept szintén nem ad mapazonosítót vagy hash-t: az első tesztnél közös, explicit, immutable world/content profil kell; több map automatikus támogatását nem állítom.

**Magasságeltérés:** standalone `BilinearHeightCm` és új `ServerTerrain::Height` bilineáris. Az eredeti [CTerrain::GetHeight](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/Game/AreaTerrain.cpp:369) két háromszög szerinti lineáris felületet mintáz. [CMapOutdoor::GetHeight](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/Game/MapOutdoor.cpp:742) ráadásul objektummagasságot is választ, ha az engedélyezett. Az azonos height.raw nem bizonyít azonos fizikai világot. A standalone megjelenített triangle mesh és bilineáris lábmagasság között is lehet eltérés; meredek, nem sík cellán ezt külön vizsgálni kell.

**Read-only adatpróba (`RUN_VERIFIED`).** A változatlan TerrainRenderer.cpp-ből közvetlenül behúzott `ReadMapSetting`, `ReadHeightRaw`, `BilinearHeightCm` függvények futottak. A GPU-s LoadMap/CreateMapBuffers nem futott; az összefűző loop a scratch harness része. Független Python LE raw-oracle is ellenőrizte az adatokat:

| Ellenőrzés | Eredmény |
|---|---|
| 25 tile / 25 attribútumheader | Olvasható; invalid attr header 0 |
| Magasságtartomány | 68,62–224,38 m |
| BLOCK attribútumcellák | 79 575 / 1 638 400 |
| Tile-varrat párok | 5160 összevetésből **1 eltérés, 0,04 m**; nem PASS az összes varratra |
| 8 ismert pont (origó, belső, varrat, félcella, közép, maximum, két kívül eső) | C++ reader vs független oracle legnagyobb megfigyelt eltérés 0,000009 m |
| Kívül eső standalone height-query | Peremre clampel; nem OutsideWorld. Új szerver külön hibastátuszt ad. |
| Terrain-only spawnjelölt | Map-local (640,640), h=163,86 m, attr=0. Objektum-/víz-/slope-validált spawn még **nem** kész. |
| Triangle vs bilinear analitikus különbség | Ugyanezen összefűzött rács cellaközepein maximum **5,53125 m**, (525,465) cella. Ez képlet-alapú adatvizsgálat, nem futó eredeti klienssel mért eredmény. |

[map-oracle.json](D:/IxtreemeWorld/build/client-audit-20260927/map-oracle.json), [map-read.txt](D:/IxtreemeWorld/build/client-audit-20260927/map-read.txt). A 0,000009 m megfigyelt f32 különbség, nem önkényes általános elfogadási epsilon. A varrat- és interpolációhibát nem szabad nagyobb toleranciával eltakarni.

**Collision:** standalone csak bounds-clamp + sampled height; nem tölti az `attr.atr`, `server_attr`, `areadata` objektumütközését. LoadMap hiba esetén flat fallbackot épít, és annak sikerét true-ként visszaadhatja (`TerrainRenderer.cpp:709–716`), ezért a bool önmagában nem „a várt map betöltve” bizonyíték. Eredetin attribútumrács + actor/background/objektum collision: [InstanceBase background binding](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/InstanceBase.cpp:424), [CheckAdvancing collision](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/PythonBackground.cpp:572), [MapOutdoor attr](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/Game/MapOutdoor.cpp:1214). Új szerver: [ServerTerrain::Height/Segment](D:/IxtreemeWorld/shared/map/src/ServerTerrain.cpp:315), [MovementSystem::Step](D:/IxtreemeWorld/gameserver/apps/gameserver/src/world/systems/MovementSystem.cpp:118) útvonal-, slope-, water-ellenőrzéssel. A heightfield nem híd vagy több szintű navigáció.

**Javasolt későbbi közös adatprofil, nem implementált konverter:** a meglévő Metin map marad a kliens vizuális forrása; ugyanennek hash-elt adataiból a meglévő [WorldPackageWriter](D:/IxtreemeWorld/shared/map/include/map/WorldPackageWriter.h:22) v3 írót kell etetni, ha nincs előkerülő használható exporter. Manifest: forráshash, loader/sampling verzió, cm→m transzformáció, origin, méret, height encoding, attribútumtérkép, vízmodell, worldlogic spawn/area, támogatott/nem támogatott objektumok. Az 1 m attribútumfelbontás megőrzése miatt a 2 m height-rács és 1 m collision közös exportfelbontását explicit meg kell választani; bilineáris felezés és int32 explicit height encoding rendelkezésre álló lehetőség, nem kész tervjóváhagyás. A raw ushort adatot nem szabad vakon int16-ba másolni.

Első fizikai modell: terrain-only, dokumentált, sík/enyhén változó tesztfolyosó a jelölt mapon, ismert BLOCK és szabad kontrollpontokkal. Tárgyfal/bridge nem maradhat észrevétlenül csak egyik oldalon. A 4 cm varrateltérésre explicit forrás-/összefűzési szabály és oracle kell. Fordított irány, MXWP test_zone → e két kliens renderere: kész importer nem található az aktív utakban, ezért nem bizonyítottan olcsóbb. A becsekkolt test_zone érintetlen.

Javasolt képlet map-local új worldhöz: `Sx=(legacyX−921600)/100`, `Sy=(legacyY−204800)/100`, `Sz=raw*0.005`. Kliens-megjelenítés `X=Sx−ownSpawnSx`, `Z=−(Sy−ownSpawnSy)`, `Y=Sz`. Irány: a meglévő standalone yaw-val `heading_server = π−yaw_client (mod 2π)` a választott tengelyek mellett; vagy az input world-vektorból közvetlen `atan2(vx,vy)`. Ezt 4 égtájjal, oda-vissza koordináta- és megállásteszttel igazolni kell; a kódbeli yaw-t változtatás nélkül átküldeni hibás lenne.

**Map ≠ chunk ≠ AreaId ≠ ZoneId.** Az 1280 m teljes térképből alap 2×2 régióval 640 m széles régiók adódnak; a min-zone-size=500 mellett ezek nem splittelhetők 320 m gyermekekre. Egy későbbi 1×1 régió/1280 m profil két 640 m tengelyféllel teljesítheti az eredeti méretkaput; a többi safety gate külön ellenőrizendő. [ZoneManager::PlanSplit](D:/IxtreemeWorld/gameserver/apps/gameserver/src/world/zone/ZoneManager.cpp:342). Ez tesztgeometriai konfigurációjavaslat, nem most végrehajtott ASF-átépítés vagy automatikus scaling-PASS.

## 6. Saját/távoli actor, mozgás, idő és életciklus

| Terület | Eredeti kliens: meglévő működés és rés | Standalone: meglévő működés és rés |
|---|---|---|
| Own actor | MAIN beállítja VID-et; ADD/INFO → NetworkActorManager → CInstanceBase::Create, race/model/parts; a létrehozás függ a race- és asset-regisztrációtól | MAIN callback után slot 0-ban a meglévő warrior modellel létrejön a renderállapot. A server race/class nem választ külön modellt. |
| Remote actor | `AppendActor`, `MoveActor`, `RemoveActor`; létező karaktermanager, láthatósági szűrés, DeleteInstance/ByFade; race/appearance valóban fogyasztott | VID-keyed `m_worldEntities` → Noesis vector → `remoteRenderStates` → warrior skin slot. Csak PC/polymorph type 6/7 renderelhető; mindenki ugyanazzal a warrior modellel. |
| Despawn/reenter | Net actor dictionary törlés és render-instance eltávolítás megvan | DEL törli network dictet és UI vectort, a következő frame-ben nem rajzolja. **A remoteRenderStates history és kiosztott skin slot nem törlődik VID-enként**, csak teljes world-resetkor: reentry/stale history és slot-kimerülés kockázat. |
| Saját mozgás | Billentyű/egér → player/actor mozgás → event handler OnMove/OnMoving/OnStop → abszolút legacy pozíció. Animáció/gameplay mozgásvezérléshez kötött | WASD/camera → WorldPlayerController::Update: kliens lokálisan léptet, 2,35/5,5 m/s. Új szerver 3/6 m/s authority; ennek illesztése hiányzik. |
| Stop | `OnStop` azonnal FUNC_WAIT, OnMoving 300 ms cadence; waiting 100 ms vizsgálat feltételes küldéssel | Mozgás közben ≥120 ms send; stop átmenetkor egyszer WAIT. A send eredményét a main figyelmen kívül hagyja, mégis MarkNetworkMoveSent: send-buffer hibánál stop elveszhet. |
| Saját korrekció | `PushTCPState` main instance esetén return. SYNC_POSITION → `NEW_SyncPixelPosition/TEMP_Push` külön mechanika, nem bizonyított authoritative reconciliation | Saját entity x/y jöhet, de a render `worldPlayer.position`-t használja, nem annak szerverpozícióját. SYNC_POSITION sem állítja a controller pozícióját. |
| Remote idő | packet ms timestamp + átlagos network gap → command queue; duration alapú actor state | Lokális sequence és arrival/renderclock; packet dwTime nincs mintába másolva. 70 ms delay, duration 16–80 ms közé clampelve, max lead 120 ms, max extrap 180 ms, idle fallback. Nem új-szerver tick-alapú interpolátor. |
| Fókusz/long frame | WM_ACTIVATEAPP kezelés van; teljes „fókuszvesztés biztos hálózati stop” út nem igazolt | NativeWindow nem küld focus-lost inputot; beragadt WASD lehet. delta cap 100 ms, Process a renderhurkon; hosszú frame alatt network/UI is késik. |
| Disconnect | Remote callback Python login-phase; leave-game takarítja actort és itemeket. Teljes csomagolt UI-val runtime-teszt kell | Remote callback csak network offline/reset. Noesis worldActive/actor vector változatlan maradhat, így helyi mozgás és régi világ látszhat; explicit logout más, ott LoadLoginView resetel. |

Hivatkozások: [eredeti actor receive](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/PythonNetworkStreamPhaseGameActor.cpp:368), 368–384,421–500; [NetworkActorManager](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/NetworkActorManager.cpp:376),376–442,463–514,601–650; [CInstanceBase::Create](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/InstanceBase.cpp:879), [saját move kihagyása](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/InstanceBase.cpp:1481); [event handler stop/send](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/PythonPlayerEventHandler.cpp:38),38–99; [legacy move encoder](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/PythonNetworkStreamPhaseGame.cpp:1352); [bontás callback](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/PythonNetworkStreamEvent.cpp:4), [leave-game cleanup](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Launcher/UserInterface/PythonNetworkStreamPhaseGame.cpp:1082).

Standalone: [controller](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/main.cpp:221),221–411; [main send/self/remote consumer](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/main.cpp:665),665–915; [network ADD/DEL/MOVE/SYNC](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/ClientNet/UserInterface/GameNetworkStream.cpp:1488),1488–1811; [Noesis world callbacks](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/NoesisLayer.cpp:1556),1556–1613; [remote disconnect](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/ClientNet/UserInterface/GameNetworkStream.cpp:640); [NativeWindow key path](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/NativeWindow.cpp:244).

További mérési csapda: [SeparateOverlappingRemote](D:/AurigaGlobal/LiveWork/StandaloneVulkanClear/main.cpp:120) az origóhoz 0,4 m-en belül levő távoli actort vizuálisan ±0,65 m X és +0,35 m Z irányban eltolja. Ez nem szerverállapot. Pozíció-oracle és a két kliens azonossági ellenőrzése nem alapulhat az így eltolt képen. Következő integrációban ezt az authority-t megjelenítő profilnak explicit kezelnie kell; most nem változtattam meg.

**Identitás:** legacy VID és új NetId u32, de a szerződés különböző. Új CharacterId u64 tartós, SessionId külön connection identity; az actor csak az elfogadott session NetId-ja. [NetIdAllocator](D:/IxtreemeWorld/gameserver/apps/gameserver/src/world/replication/NetworkEntityId.h:14): player 1-től, mob 1 000 000-tól növekszik, határok assertelték; nem bizonyított végtelen/wrap-biztos kiosztás. Reconnect után korábbi NetId/history/baseline nem vihető át automatikusan. A standalone lokális `moveSequence <=` és az új input `sequence < last_input_seq` összevetése sem wrap-protokoll; hosszú élettartam/reset-policy nyitott.

**Idő és authority:** az új [InputRouter](D:/IxtreemeWorld/gameserver/apps/gameserver/src/world/input/InputRouter.h:17) latest-state mozgás + seq; STOP=Idle állapot, nem külön elveszíthető esemény. A scheduler csak NextTick due után futtat 50 ms lépést: [ZoneScheduler](D:/IxtreemeWorld/gameserver/apps/gameserver/src/world/zone/ZoneScheduler.cpp:228). A wire tick globális u32, nemcsökkenő (a jelenlegi szerződés tartományában), ismétlődhet/ugorhat; nem kliens-ms és nem zónalocal tick. [Wire tick contract](D:/IxtreemeWorld/gameserver/apps/gameserver/src/world/replication/ProtocolEncoder.h:18). A 20 Hz, 10 Hz, 5 Hz Network LOD, resync és pending-budget állapotok a kliens interpolációs modelljének bemenetei, nem kikapcsolandó akadályok. 30/60/144 FPS, focus-loss, wrap és jitter vizsgálat **NOT_RUN**.

**ASF/AOI:** a globális NetId a zónamigráció során nem változik; a láthatósági diff és per-recipient baseline ezt kell kövesse. Egy ghost nem új actor, owner-váltás nem új map vagy új socket. A jelenlegi szerver lifecycle/replication kapuit kell használni; kliensoldali teszttel még nincs igazolva a seamess működés. A korábbi C-moving despawn/preparation/spawn kohorszteszt ezt nem helyettesíti.

## 7. Legelső akadály és minimális változtatási leltár

**Legelső statikus akadály mindkét kliensnél:** a régi server-first Metin handshake és nyers header-stream nem egyezik az új kliens-first, hosszprefixes protokollal. Standalone jelenlegi endpointja ráadásul Aurigára mutat. Ez nem most megfigyelt socket-hiba, hanem sender/receiver-forrásból bizonyított eltérés. A régi szerver [DESC::Setup/StartHandshake](D:/AurigaGlobal/LiveWork/AurigaGlobal/SRC/Server/GameServer/network/desc.cpp:255) magyarázza a jelenlegi kliensek várakozását.

| Munka | Standalone konkrét érintkezési pont | Miért kell / következő elfogadás | Eredeti kliens alternatíva |
|---|---|---|---|
| I0 build/runtime profil | CMake cache/profile, exe-relative assets | Bizonyított static Debug profil rögzítése, asset/hash ellenőrzés; flat fallback ne lehessen map-PASS | Stock bootstrap/build install nélkül; tényleges pack/root/race tartalom azonosítása még előfeltétel |
| Login/game transport profil | AccountConnector, GameNetworkStream, Noesis login/lobby határ | Meglévő új loginserver handshake/auth/list/select/token; két külön engedélyezett tesztkarakter; megfelelő reject UI | PythonNetworkStream phases + account connector + Python phase/UI illesztés |
| Új codec consumer | GameNetworkStream → TWorldEnterInfo/TWorldEntityInfo vagy mellettük szűk world-state adapter | Cap'n Proto lifecycle + 0x11 baseline, pontos méretellenőrzés, töredék/összevont frame, hibás message negatív kontroll | CNetworkActorManager jó szemantikus seam, de régi CreateData több appearance/adatfüggőséget vár |
| Authority movement | WorldPlayerController és main.cpp self/remote fogyasztó | Irány+walk/run/idle küldés, saját self-record korrekció, stop retry/focus-loss; nem előrehozott szervertick | EventHandler/send és main-actor PushTCPState tiltás/SyncActor külön igazítása |
| Lifecycle cleanup | Noesis callback + remoteRenderStates/skin slot | DEL/AOI reenter/history reset; remote disconnect worldActive reset, stale session tiltás | Több cleanup már megvan; main respawn mellékhatásai és Python phase-átmenetek megőrzendők |
| Közös mapadat | Meglévő TerrainRenderer és WorldPackageWriter | Egy forrás hash, attribútumfelbontás, varrat/interpoláció szerződés, valós spawn és collision-oracle | Eredeti triangle/object-height és objektumcollision többletegyezőségi feladat |
| Zónamigráció | Kliens NetId actor/baseline folytonosság | Ugyanaz a két session és actor marad, normál authority kapukkal; ghost nem duplikálódik | Ugyanez; nem szabad legacy WARP-ot generálni a szerver zónahatárára |

| Összesített kategória | Tartalom |
|---|---|
| **Megvan és újrahasználható / NO CHANGE** | Új szerver framing/schema, auth/token/presence, authoritative movement és terrain-ellenőrzés, AOI/replication/ASF; standalone warrior modell+3 animáció, raw terrain olvasás, Noesis callback; eredeti CNetworkActorManager referencia |
| **Következő implementáció kell** | Kliens új-szerver profil, admission/codec consumer, own correction, megbízható stop+focus, teljes session/actor cleanup, közös mapadat és oracle, runtime content bizonyítás |
| **Tudatosan nem része az első tesztnek** | Combat bővítés/törlés, inventory/quest/item/shop teljes emuláció, renderer-refaktor, teljes editor/export-rendszer, új transport/gateway/ASF, többmapos termék-warp, 100×100 km stressz |
| **Nincs bizonyíték** | Két valódi kliens az új szerveren, teljes eredeti runtime assetcsomag, standalone végigvezetett combat, kész Metin→MXWP exporter a vizsgált körön kívül, távoli production kapcsolat, Unity-port kompatibilitása |

## 8. Illesztési stratégia és választás

| Stratégia | Bizonyíték és költség | Javaslat |
|---|---|---|
| Kliensoldali szűk IxtreemeWorld profil a standalone-ban | Kicsi main/Noesis world callback felület; a renderer az actorlistát már külön fogyasztja; friss build és helyi assets elérhetők. A hiányzó authority/cleanup út jól körülhatárolható. | **Elsődleges.** A meglévő Auriga mód és combat megőrzésével, külön profilban. A mostani audit nem implementálta. |
| Eredeti Auriga kliens illesztése | Kész actor/collision/animation; de új auth, codec, saját authority, időmodell így is kell. Python/pack/race/appearance függőségek és eltérő magasság/objektum szerződés nő. | Referencia és későbbi teljesebb kliens cél; a szűk első tesztre kevésbé bizonyított, kisebb munka nem igazolható. |
| Szerveroldali legacy adapter | Megőrizheti mindkét legacy client parserét, de login/key/select/UI elvárásokat, pozíció→intent szemantikát, lifecycle+baseline/backpressure határt is illeszteni kell. A saját korrekciós rés ettől még nem tűnik el. | Most nem kisebb bizonyított feladat. Nincs automatikus adapterengedély. |
| Külön gateway | Megduplázza a session, auth, queue/baseline és hibakezelési határt; kész gateway nincs bizonyítva. | Nem javasolt első csomagnak. |

A döntés megőrzi a gameserver U-final authority, wake-cut és NextTick szerződését. Egyetlen új kliensprofilt kellene illeszteni a meglévő célrendszerhez; nem új világot vagy alternatív szerver-authorityt létrehozni.

## 9. Kétklienses terv és közös Unity-szerződés

A következő munka külön jóváhagyást igényel. **Egyetlen első implementációs csomag javaslat: „Standalone IxtreemeWorld admission + own identity” (I0–I1).** Rögzített, izolált build/endpoint/content profil; meglévő loginserver tokenút; gameserver handshake/EnterWorldAccept/Reject; két külön test identity, reconnect és duplicate/expired/used token negatív ellenőrzés. A saját NetId/spawn/tick állapot legyen ellenőrizhető, de ez még nem actor/mozgás/map-kompatibilitási PASS. Nincs ehhez legacy serveradapter vagy mapkonverter implementálva. Tesztfiók/token-provisioning és titokmentes tesztkonfig a későbbi futtatás szükséges bemenete; nem tételezem fel rendelkezésre állásukat.

Az egész első infrastruktúrateszthez szükséges további sorrend:

| Kapu | Megvalósítás és elfogadás |
|---|---|
| I0 | Sikeres reprodukálható build, ismert runtime content és forráshash; az eredeti output/asset érintetlen; profile default nem kapcsolódhat véletlenül másik szerverhez |
| I1 | Két identity helyes auth→handoff→world accept; wrong-phase, protocol-version, duplicate, lejárt/használt token és pending disconnect negatív; nincs kliens-választotta authority |
| I2 | A jelölt Metin-map és ugyanazon forrásból származó server package; raw/height/attrib/bounds/spawn oracle, varrateltérés rendezése, explicit terrain-only korlát. Betöltési fallback nem PASS |
| I3 | Saját és távoli actor, egyszeri NetId mapping, add/update/del, AOI leave/reenter, nem player rekordok szabályos kezelése; nincs duplikált ghost vagy fogyó/stale skin slot |
| I4 | Valós continuous input; own authoritative korrekció és remote interpoláció; STOP, irányváltás, Shift, focus-loss, send fail, hosszú frame; 30/60/144 FPS és 20/10/5 Hz LOD. Renderelt és authoritative pozíció mérése külön; nincs overlap-eltolással kozmetikázott egyezés |
| I5 | Két élő, folyamatosan bent levő kliens zónahatár-átlépése; session/NetId/map/actor stabil. Kényszerített split csak normál safety kapukon, külön automatikus overload-scaling vizsgálattól; majd merge. Nem C-moving cohort |
| I6 | Socket-close, reconnect, old-session bytes, pending world/terrain megszakítás, hibás hossz/codec/record/missing baseline; a késői completion nem éleszt régi actort/presence-t |

Bizonyíték: kliens látható eredmény + szerver identity/presence/AOI trace + world validation összeillesztett eseményei; nem teljes snapshot minden renderframe-ben. 16 MiB/200k mob stressz nem e kétklienses próba előfeltétele. A karakter körüli szerverzóna-váltás kliensoldalon ne legyen megfigyelhető map- vagy socketváltásként.

**Unity későbbi közös szerződése (forrásaudit nélkül):**

1. Közös `shared/protocol/schema/packet.capnp`, protocolVersion=1, framing BE hossz/codec, pontos integer és float endianness; nincs struct ABI másolás C#-ba. A handshake verzió, transform opcode és map manifest verzió külön névtér.
2. Loginserver account-owned character selection, egyszer használatos token és game endpoint; a szerver osztja a NetId-t, külön CharacterId/SessionId. EnterWorldAccept a kliens world-ready kapuja; reject és timeout önálló út.
3. Movement seq/heading/state intent, szerver X/Y méter és Z height; Idle állapot biztos továbbítása. Saját authoritative self-record, delta-baseline és correction fogyasztása; renderFPS nem simulation tick.
4. 0x11 frame első fix self rekordja és utána field-maskos remote rekordok; spawn alapállapot, delta/resync/LOD/budget, despawn törlés, reconnect teljes baseline-reset. Globális tick ismétlődhet és ugorhat; wrap/reset értelmezés explicit következő szerződésfeladat.
5. Egy authority entitás = egy actor; AOI lifecycle külön owner/migration állapottól; ghost és ZoneId nem scene identity. Queue/receive/apply nem azonos, frame-et eldobni baseline megtartásával tilos.
6. Közös immutable world/content manifest, forráshash és exportverzió, units/origin/axis/height/attribute/water/bounds. Unity up-axis átalakítását a tényleges integrációban kell megadni; itt nem állítok kész Unity-maploadert.

## 10. Próbák, korlátok, review-döntések

Futtatott próbák és scope:

| Próba | Eredmény | Mit nem bizonyít |
|---|---|---|
| U-final hash összevetés | Production source egyezés; 1 script eltérés | Futó szerver aktuális konfigját |
| Standalone eredeti cache szerinti dependency profil, Debug | exit 1, 19,16 s CRT linkhiba | Nem következik belőle runtime hálózati hiba |
| Standalone static triplet, Debug izolált build | exit 0, 25,93 s configure+build | GUI, login, GPU működés, Release build |
| Valódi Packet.h sizeof/offsetof + szerver Framing.cpp | exit 0, részletes ABI-lista | Nem golden network capture. A framing-próba szintetikus 9 byte payloadot keretez; nem kézzel gyártott handshake-PASS |
| Valódi standalone height reader + független raw-oracle | exit 0; 8 pont egyezik megfigyelt f32 pontossággal; 1 hibás varrat feltárva | Eredeti packed map betöltés, GPU render, szerverexport, objektumcollision |
| Eredeti full build / GUI / bármely login / két kliens / FPS / migration | **NOT_RUN** | Nem nevezhető kész integrációnak |

Pontos parancsok/probe források és logok a bizonyítékcsomagban: `reproduce.ps1`, `probes/CMakeLists.txt`, `probes/layout.cpp`, `probes/map_read.cpp`, `map_oracle.py`, `configure*.log`, `build-*.log`, `probe-build*.log`, `probe-results.json`. A map-probe első linkje hiányzó Vulkan/Debug objektumokkal elbukott; a végső harness a **változatlan** VulkanDevice.cpp/Debug.cpp függőségekkel linkel, GPU/device függvényt nem futtat. Ez scratch-próba javítása, nem kliensimplementáció.

Nyitott review-pontok: (a) a javasolt standalone I0–I1 profil jóváhagyása; (b) későbbi tesztidentity és endpoint rendelkezésre bocsátása; (c) terrain-only közös map modell, varratszabály és attribútum/exportfelbontás elfogadása az I2 előtt; (d) self-correction/interpolation/idle keepalive és wrap/reset explicit tesztszerződése; (e) ha mégis eredeti kliens a választás, tényleges runtime pack/locale/race csomag azonosítása és install nélküli buildprofil. Ezek nem gátolták a forrásauditot, ezért közben nem kértem újabb jóváhagyást.

Nincs futtatott kliens-logból átvett credential, session-token, HWID vagy személyes adat a riportban. A legacy forrás titkosadat-logolási helyei nem használhatók titokmentes acceptance trace-ként változtatás nélkül.

## 11. Munkafavédelem és STOP

Új dokumentum: ez a riport. Új scratch/build/probe/evidence: `D:/IxtreemeWorld/build/client-audit-20260927/`. A korábbi MAP/SL/TC riportok, frozen evidence-ek, a dirty szervermunka, a becsekkolt test_zone és mindkét kliens implementációja megőrzendő és hash-ellenőrzött. Végső ellenőrzés: [protection-final.json](D:/IxtreemeWorld/build/client-audit-20260927/protection-final.json).

**STOP + review.** Nincs klienspatch, adapter, exporter, renderer-/gameplay-/Unity-/szervermódosítás, commit, push, reset vagy stash. A következő implementáció nem indult el.
