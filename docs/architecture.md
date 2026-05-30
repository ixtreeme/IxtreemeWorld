# Crossplatform MMORPG — Architektúra és Építési Terv

> Konszolidált tervdokumentum. Cél: ez alapján modulonként, mérföldkövenként átadható a feladat egy kódgeneráló ágensnek (Codex). A dokumentum a *mit és miért* szintjén specifikál; a konkrét implementációt az ágens állítja elő, de a benne rögzített invariánsoktól nem szabad eltérni.

---

## 1. Áttekintés

- **Műfaj:** crossplatform MMORPG, Metin2-szerű akció-gameplay.
- **Lépték (kezdeti):** 1500–2000 egyidejű játékos, **egyetlen szervergépen** (scale-up). A teljes architektúra úgy van megtervezve, hogy a scale-out (több gép) később *konfiguráció*, ne újraírás legyen.
- **Platformok:** Windows + Android (mobil), később bővíthető.

### Technológiai stack (rögzített)

| Réteg | Választás |
|---|---|
| Nyelv | C++ (natív, teljes projekt) |
| Renderelés | Vulkan |
| UI | NoesisGUI |
| Modell / animáció | Granny 2.9 |
| Hálózat (kliens + szerver) | boost.asio |
| Packet-szerializáció | Cap'n Proto (+ custom bináris a hot pathon — lásd 9.) |
| ECS | flecs |
| Adatbázis | MariaDB (+ Redis cache később) |
| Jelszó-hash | argon2 |

### Jelenlegi állapot (M0 — kész)

- Account login működik.
- Login után lobby szobába kerül a játékos.
- Jelszavak argon2-vel hash-elve.

---

## 2. Architektúra alapelvek

Három, élesen szétválasztott sík. Egyik sem nyúl a másik felelősségébe.

1. **Adatsík (zónák).** Zónánként egy flecs world, egy worker-poolon ütemezve. Minden zóna autonóm: saját tick, saját entitások, saját mobok/bossok. A zónán belüli munka (mozgás, harc, AI, spawn) lokális és párhuzamos.
2. **Szinkronsík (határsáv).** Kizárólag a szomszédos zónák *határsávja* cserél adatot, tick végi double-bufferes publish/read-del, ghost-entitásokká materializálva. Ez adja a seamless láthatóságot és a migráció varratmentességét.
3. **Vezérlősík (time-authority + supervisor).** Monoton világtick-számláló, eseményütemezés tick-számra, zóna-felügyelet. **Soha nem nyúl a tickenkénti adatforgalomba.** Kezdetben thread a fő processen belül; külön binárissá csak scale-out esetén válik.

### Két kritikus szétválasztás (be kell tartani)

- **Render distance ≠ AOI radius.** A render distance kliensoldali, vizuális, „ingyen" (statikus terep, távoli hegyek LOD-dal). Az AOI radius szerveroldali, ez kerül CPU-ba és sávszélbe (meddig küld a szerver más entitásokról frissítést). A kettő külön rendszer, külön skálázódik.
- **Logikai map ≠ futásidejű particionálás.** A játékos egyetlen összefüggő világot lát (egy koordinátarendszer). Ez nem kényszerít arra, hogy runtime-ban egy szálon fusson — a particionálás (zónák) ettől független.

### Anti-pattern (tilos)

- **Globális barrier / lockstep.** A zónák NEM várnak egymásra tickenként. Nincs „mindenki álljon meg a tick-vonalnál". Ez a világot a leglassabb zóna sebességére húzná, egységes meghibásodási pontot teremtene, és nem skálázódik. A koordináció helyett: minden zóna a saját tempójában tickel, és csak a határsávot + a publikált snapshotokat cseréli.

---

## 3. Világ- és zónamodell

- **Egy logikai map**, összefüggő koordinátarendszer.
- **Méret:** 10 000 × 10 000 m (100 km²).
- **Zónarács:** 10 × 10 = **100 zóna**, zónánként **1000 × 1000 m**.
- **Zóna = egy flecs world**, worker-poolon ütemezve.
- **Üres zóna ~nulla CPU** — a 100 zóna majdnem ingyen van; a költség a tényleges entitás/játékos terhelés, nem a zónaszám.
- **Szigetek / instance-ok:** a határtól elkülönített, warppal elérhető területek on-demand betöltött / instance-olt worldök. Nem terhelik a fő mapot, amíg nincs bennük senki.

### Zónakiosztási szabályok

- A zónahatárokat oda kell tenni, ahol kevés a forgalom (hegygerincek, vízpartok, szűk átjárók) — **ne** városok alá vagy nyílt, tömeggyűjtő mezőre.
- **Boss-arénák és városok teljes egészében egy zóna belsejébe essenek**, legalább a zsúfolt AOI-sugárnyira (~200 m) a határoktól. Így a legnagyobb terhelésű pillanatok (boss-fight, tömeg) zónán belül maradnak, ahol minden lokális és olcsó.

---

## 4. Tick-pipeline (a teljes ciklus)

Globális logikai frame-enként, a következő fázissorrendben. A párhuzamos fázisok a worker-poolon futnak; a sync-fázis egyszálú.

1. **Input intake** — a hálózati IO-szálak betöltötték a beérkező packeteket thread-safe queue-kba; itt szétosztjuk őket zónánkénti command-bufferekbe (a regiszter alapján, ki melyik zónáé).
2. **Párhuzamos zóna-szimuláció** (worker-pool, zónánként flecs belső párhuzamosság):
   - Input-parancsok alkalmazása → mozgás, akciók.
   - Game-rendszerek: AI, harc, fizika, spawn.
   - **Boundary-detektálás:** a határt a hiszterézis-küszöbön túl átlépő entitások megjelölése `MigrateTo{target_zone}` komponensértékkel (nincs struktúraváltás).
   - (flecs a struktúraváltásokat a saját, zónán belüli merge-pontjáig halasztja.)
3. **Sync-pont / migration-fázis** (egyszálú, zónák között):
   - Migrációk feldolgozása: tulajdonjog-átadás A→B, regiszter frissítése, session-átirányítás (lásd 7.).
   - A fázis végén minden entitás a helyes owner-zónájában van.
4. **Border-snapshot publish** (zónánként párhuzamos, read-only): minden zóna a határsávja entitás-snapshotját a saját double-bufferébe írja (`buffer[tick % 2]`).
5. **Ghost-rebuild** (zónánként párhuzamos, read-only): minden zóna a szomszédai *előző* tickben publikált bufferét (`buffer[(tick+1) % 2]`) olvassa, és ghost-entitásként materializálja. **Dedup:** ne hozz létre ghostot olyan entitásra, ami már resident ebben a zónában (regiszter-ellenőrzés).
6. **AOI + kimenő snapshot build** (zónánként párhuzamos): minden resident játékosra kiszámolod a látható halmazt (resident + ghost entitások AOI-n belül, cap-elve), és delta-update-et építesz.
7. **Hálózati egress** (IO-szálak): játékosonkénti update-packetek kiküldése.
8. **`world_tick` léptetése.**

> A 4. és 5. fázis a double-buffer miatt egymással is párhuzamosítható: a publish a *jelenlegi*, a rebuild az *előző* tick bufferét írja/olvassa. Következmény: a ghostok 1 tick késésűek (vizuálisan elhanyagolható „árnyék").

---

## 5. AOI / interest management

A Metin2 naiv megoldása O(n²)-es volt (minden entitás minden közelit követett), és ~2500 mob körül elszállt. Ezt el kell kerülni.

- **Adatszerkezet:** spatial hash grid. Az AOI-lekérdezés a közeli cellák entitásain fut → ~lineáris, nem O(n²).
- **Dinamikus AOI-sugár sűrűség szerint:** ritka területen ~350 m, zsúfolt helyen ~150–200 m. Tartomány: **~100–350 m** entitásszámtól függően.
- **Cap:** játékosonként a látható entitások száma capelve (kiindulás: **~100** legközelebbi/legrelevánsabb). Zsúfolt esemény esetén a cap „harap", és megvédi a tick-büdzsét.
- **Fade-margin (hiszterézis a láthatóságra):** a **szerver-AOI > kliens vizuális fade-vég**. A szerver kicsivel hamarabb küldi az entitás adatát, mint ahogy a kliens kirajzolja → nincs pop-in. Be/ki két különböző küszöbön (pl. be 150 m-nél, le 165 m-nél) → nincs flicker a határon mozgó entitásnál.
- **Fontos költség-szabály:** a szerver-AOI mozogjon együtt a kliens tényleges megjelenítési távolságával. 350 m-nyi adatot küldeni olyan entitásról, amit a kliens 120 m-nél elhalványít, pazarlás. A 350 m csak ott indokolt, ahol a kliens tényleg kirajzol ~330 m-ig.

---

## 6. Ghost / border-replikáció

- **Border band (határsáv) szélessége = lokális AOI-sugár** az adott határszakaszon. NEM globálisan a max AOI-ra fixálva — sűrű határszakaszon keskenyebb (~150 m), ritkán szélesebb (~350 m). Így a ghost-költség önszabályozó: a sáv ott széles, ahol kevés entitás van (olcsó), és ott keskeny, ahol sok (sűrű).
- **Mechanizmus:** minden zóna a tick végén publikálja a határsávja entitás-snapshotját (csak a megjelenítéshez kellő állapot: pozíció, irány, megjelenítési állapot — nincs AI, nincs fizika). A szomszéd ezt ghostként materializálja a saját worldjében.
- **A ghost read-only, nem szimulálódik, és minden tickben újraépül** a szomszéd legutóbb publikált bufferéből. A ghost soha nem migrál — csak a resident autoritatív entitás migrál.
- **Double-buffer, lock-mentes:** írás `buffer[tick%2]`-be, olvasás `buffer[(tick+1)%2]`-ből.
- **Transzport-absztrakció:** a határcsere interfésze „publikálj snapshotot / olvass szomszéd-snapshotot" legyen. Implementáció most: közös memóriába írt double-buffer. Scale-out esetén csak a transzport cserélődik (memória → hálózat/IPC), a zónalogika változatlan.

---

## 7. Handoff / migráció

A „mindenki találkozhat mindenkivel a határokon át" mechanikája. A ghost-replikáció miatt vizuálisan láthatatlan: az entitás mindig resident vagy ghost egy AOI-n belüli zónában, és a resident↔ghost átmenet azonosan renderelődik.

### Alapelvek

1. **A tulajdonjog explicit és ragadós** (eltárolt owner, nem pozícióból újraszámolt minden tickben).
2. **Migráció csak a sync-ponton** (2. fázis után, 3. fázisban), soha tick közben — a flecs threading-modell miatt.
3. **Hiszterézis (két küszöb):** owner A→B vált, ha a játékos a határvonalon túl `H` távolságra ér B-be; vissza A-ba csak `H`-val visszamenve. A 2H széles sávban marad a jelenlegi owner. Kiindulás: **`H` ≈ 5 m** (tunable).

### Három fázis

1. **Detektálás** (A tickjében): boundary-rendszer megjelöli a küszöbön túl átlépő játékost `MigrateTo{B}` komponensértékkel. Olcsó, nincs struktúraváltás.
2. **Átadás** (sync-pont, egyszálú):
   - A migration manager szerializálja a játékos **teljes autoritatív állapotát** (lásd lentebb, mi transzferálódik).
   - Beszúrja a játékost B worldjébe (entitás létrehozás a teljes állapottal).
   - Törli A worldjéből (despawn).
   - Frissíti a **globális entitás-regisztert**: a játékos mostantól B-é.
   - Átirányítja a **hálózati session routingot**: a játékos inputja mostantól B intake-jébe megy, és B kimenő snapshotjai tartalmazzák residentként.
3. **Folytonosság:** a játékos már ghostként ott volt B-ben (határsáv-replikáció), így a megfigyelők nem látnak ugrást; a ghost residentté „lép elő". A játékos pedig mostantól A határsávját látja ghostként → nincs pop-out.

### Specifikálandó részletek (Codexnek pontosan)

- **Globális entitás-regiszter:** egyetlen igazságforrás, `player_id → owning_zone`. Írás **csak** a sync-fázisban (single-writer), olvasás lock-mentes (a hálózati réteg és a migration manager olvassa).
- **Network session continuity:** a TCP/asio kapcsolat NEM változik — csak a logikai hozzárendelés (melyik zóna fogyasztja az inputot és termeli a nézetet). Az input-queue átirányítása a sync-ponton történik.
- **In-flight input:** A a tickjében alkalmazza a játékos összes függő parancsát, hogy tiszta állapotban migráljon. A sync-pont után érkező input már B-hez routol → semmi nem vész el.
- **Mi transzferálódik:** a perzisztens/autoritatív komponensek (pozíció, sebesség, statok, inventory-referenciák, harci állapot, quest-állapot). **Mi nem:** a derivált/tranziens állapot (AOI látható-halmaz, ghost-listák) — ezt B újraépíti.
- **Dedup:** a ghost-rebuild kihagyja azt az entitást, ami már resident a zónában (különben a migrációt követő 1 tickben a stale buffer miatt duplán látszódna).
- **Sarok-eset (4 zóna találkozása):** a „pozíció szerinti természetes owner" szabály egyértelműen eldönti a célt; a hiszterézis a thrashinget akadályozza.
- **Határon átnyúló harc:** ha a játékos harc közben migrál, a targeting elszakadhat. v1: a target feloldódik / újraértékelődik, a mob leash-el visszahúz. (Ezért tartjuk a boss-arénákat a határoktól távol.)
- **Defenzív viselkedés:** ha B érvénytelen/megtelt (ritka a seamless modellben), a migráció megszakad, a játékos A-ban marad.

---

## 8. Time-authority és world-eventek

- **`world_tick`:** monoton növekvő világtick-számláló. A zónák lazán, drift-korrekcióval igazítják a sajátjukat hozzá, de **nem várnak rá** és nem várnak egymásra.
- **Eseményütemezés tick-számra, nem időre:** „az event a `world_tick == N`-nél indul". A time-authority ezt jó előre (több száz tickkel korábban) szétküldi. Minden zóna lokálisan, függetlenül figyeli a saját számlálóját, és N-nél elindítja az event lokális részét. Nincs barrier, mégis az egész map egyszerre kapcsol.
- **Determinizmus:** ha az eventnek egységesnek kell látszania (azonos spawn-pozíciók, fázisok), a broadcast tartalmazza a **seedet** is. Minden zóna ugyanabból a seedből, ugyanazzal a determinisztikus algoritmussal számol — egységes látvány, futás közbeni egyeztetés nélkül.
- **Eventtípusok:**
  - **(a) Zóna boss** (a fő típus): aki a zónában van, az üti. Egyetlen zónán belül, lokális HP/aggro/loot — **nincs cross-zone probléma**, sima egy-worldös szimuláció. A boss aggro/leash-rádiusza ne lógjon át a határon.
  - **(b) Közös, mutálódó állapotú event** (ritkább): egyetlen world boss, amit több zóna együtt ver. Megoldás: **owner-zóna** vezeti az autoritatív HP-t, a többi zóna ghostként jeleníti meg (ugyanaz a ghost-minta), a távoli sebzés üzenetként megy az ownernek. Kerülendő a „minden zónának saját HP-ja" látszat-megosztás.
- **Kezdetben:** a time-authority egy thread a fő processen belül, nem külön bináris.

---

## 9. Hálózati réteg

- **IO ↔ sim leválasztás:** az asio IO-szálak csak betöltik a packeteket thread-safe queue-ba. A world a sim-szálaké; az IO-szálak közvetlenül nem nyúlnak hozzá. A sim a tick elején olvas, a végén termel kimenetet, amit átad az IO-rétegnek.
- **Szerializáció — hibrid:**
  - **Cap'n Proto** a ritka, strukturált üzenetekre (login, inventory, NPC-dialógus, kereskedés).
  - **Tömör custom bináris** a nagy frekvenciás hot pathra (mozgás-szinkron, entitás-replikáció), ahol a wire-méret közvetlenül hajtja az egresst. Mérni kell, és a hot path méretét agresszíven minimalizálni.
- **Delta-kompresszió:** az entitás-update-ek delta-alapúak (csak a változás), nem teljes állapot minden tickben.
- **Egress-büdzsé:** ez a valódi szűk keresztmetszet, nem a CPU. 2000 játékos × ~100 capelt entitás × 20 Hz × ~40 bájt ≈ patologikus csúcson 1,3–2 Gbit/s; normál üzemben (delta + szigorú AOI) ~200–300 Mbit/s. Korán mérni, és a netkódot fegyelmezetten tartani.

---

## 10. Perzisztencia

- **MariaDB** mint elsődleges store. Vázlatos sémacsoportok:
  - `account` (login, argon2 hash, státusz).
  - `character` (statok, pozíció, zóna, megjelenés).
  - `inventory` / `item` (tárgyak, stack, kötések).
  - `world_state` (perzisztens világállapot, ha kell: boss-cooldownok, event-állapot).
- **Mentési stratégia:** periodikus snapshot + aszinkron írás. A sim-szál soha ne blokkoljon DB-IO-n; a perzisztencia háttérben, batch-elve.
- **Redis cache:** később, amikor a forró olvasások (session, gyakran kért karakteradat) indokolják. Nem M1-feladat.
- **Argon2** marad a jelszó-hashre.

---

## 11. Kliens–szerver felelősségmegosztás

- **Szerver-autoritatív:** a szerver dönt minden gameplay-releváns állapotról (pozíció-validáció, harc, loot). A kliens nem megbízható.
- **Kliens:** render distance + LOD (terep, távoli táj „ingyen"), entitás-fade a fade-marginban, input-küldés, megjelenítés.
- **Prediction / reconciliation:** a kliens előrejelzi a saját mozgását a válasz előtt, és a szerver-állapotra korrigál (a hot path latency elfedésére). A komponens-definíciók megoszthatók kliens-szerver közt, de **óvatosan** — ne szivárogjanak szerver-autoritatív feltevések a kliensbe.

---

## 12. Építési roadmap (mérföldkövek)

Ez a sorrend, amiben Codexnek átadható. Minden mérföldkő önállóan tesztelhető legyen.

- **M0 — Kész.** Login, lobby, argon2.
- **M1 — Lobby → világ belépés (egy zóna).** Karakterválasztás/-létrehozás, token-átadásos belépés a game-szerverre, egyetlen flecs world, alap mozgás-szinkron (input → szerver → broadcast). Cél: egy játékos mozogjon egy világban, és lássa magát.
- **M2 — Spatial grid + AOI + entitás-replikáció (egy zónán belül).** Spatial hash grid, dinamikus AOI-sugár, cap, fade-margin. Több játékos lássa egymást egy zónában, pop-in/flicker nélkül. Itt dől el az O(n²) elkerülése.
- **M3 — Több zóna + ghost border-replikáció.** Worker-pool, zónánként egy world, double-bufferes határcsere, ghost-materializáció, dedup. Cél: seamless láthatóság a zónahatárokon át (még migráció nélkül, a játékos egy zónában marad).
- **M4 — Handoff / migráció.** A 7. fejezet teljes megvalósítása: hiszterézis, sync-ponti átadás, regiszter, session-reroute, in-flight input. Cél: a játékos észrevétlenül sétál át a zónahatárokon, egységes világként.
- **M5 — Harc + mobok + spawn.** Harci rendszer, mob-AI (pathfinding amortizálva), spawn-táblák régióhoz/zónához kötve.
- **M6 — Zóna bossok.** Lokális boss-szimuláció, aggro/leash, loot. Arénák a határoktól távol.
- **M7 — World-eventek + time-authority.** `world_tick`, tick-számra ütemezett eventek seeddel, (a) és (b) típusú eventek.
- **M8 — Perzisztencia teljes + Redis cache.** Aszinkron mentés, snapshotok, Redis a forró olvasásokra.
- **M9 — Terhelési teszt + hangolás.** Szintetikus terhelés a célpopulációra, AOI-cap és egress hangolása, egyzónás hotspot-stresszteszt, profilozás (flecs-párhuzamosíthatóság, cache-viselkedés).

---

## 13. Hangolandó paraméterek (kezdő értékek)

| Paraméter | Kiindulás | Megjegyzés |
|---|---|---|
| Tick rate | 20 Hz (50 ms) | akció-MMO-hoz; 10–30 Hz között hangolható |
| Zónaméret | 1000 × 1000 m | 10×10 rács |
| AOI-sugár (ritka) | ~350 m | csak ha a kliens tényleg ennyire rajzol |
| AOI-sugár (zsúfolt) | ~150–200 m | |
| AOI-cap | ~100 entitás / játékos | tömegben „harap" |
| Fade-margin | szerver-AOI − kliens fade-vég | pop-in elleni puffer |
| Hiszterézis `H` | ~5 m | migráció-thrashing ellen |
| Border band szélesség | = lokális AOI | nem globális max |

Minden értéket éles terhelésen kell visszamérni és hangolni (M9).

---

## 14. Kockázatok és csapdák

- **O(n²) AOI** (Metin2-tanulság): spatial grid + cap kötelező, M2-ben.
- **Egyzónás torlódás:** 2000 fő egy zónába tódulva az az egy world tickjét terheli (korlátozott párhuzam), miközben a többi mag pihen. Védelem: dinamikus AOI-cap, boss-arénák szétszórása, szükség esetén instance/channel a tömegeseményekre.
- **Hálózati egress-csúcs:** előbb telik be, mint a CPU. Delta-kompresszió + szigorú AOI + hot-path bináris.
- **Cap'n Proto wire-méret a hot pathon:** mérni; ha bőkezű, custom binárisra váltani a mozgás-szinkronnál.
- **Barrier/lockstep kísértés:** különösen eventnél csábító „szinkronra váltani" — TILOS. A tick-számos, seedelt, owner+ghost megközelítés decentralizált marad a legnagyobb terhelés alatt is.
- **flecs párhuzamosíthatóság:** túl sok írásütközés / sync-pont esetén a magok papíron vannak meg, gyakorlatban nem. Profilozni (M9).
- **Archetype-churn:** a gyakran billegő állapotot komponens-**értékbe** tedd (pl. `alive` bool, state enum), ne tag add/remove-ba (az table-move-ot jelent). A ritka, valódi archetípus-váltásra hagyd a komponensváltást.

---

## 15. Átadás Codexnek — gyakorlati tippek

- **Mérföldkövenként, modulonként** add át (M1, M2, …), ne az egészet egyben. Minden mérföldkő önálló, tesztelhető egység.
- **Interfészeket definiálj először** (a határcsere „publish/read" interfész, a regiszter API, a migration manager API, a tick-fázis hookok), implementációt utána. Ez tartja nyitva a scale-out ajtót.
- **Az invariánsokat tedd explicitté** a promptban: „migráció csak sync-ponton", „regisztert csak a sync-fázis írja", „ghost read-only és tickenként újraépül", „nincs globális barrier". Ezek a leggyakoribb hibaforrások egy generált kódban.
- **Tesztelhetőség:** kérj determinisztikus, fix-seedes szimulációs teszteket a migrációra (határon billegés, sarok-eset) és az AOI-ra (be/ki küszöbök), mert ezek a logika legtörékenyebb pontjai.