# MAP-3 — teljes munkariport, review-utómunkával (2026-09-25)

> **Státusz:** kész, a working tree-ben, **commit/push nélkül**. STOP —
> review a MAP-4 előtt. Gameplay- és kliensmunka nem történt.
> Alap: `With_Auriga` @ `ab2458de`; a MAP-2, a MAP-2 review-utómunka, a MAP-3
> és a MAP-3 review-utómunka is commit nélkül a working tree-ben.
> Szerződés: [`map-data-format.md`](map-data-format.md) 9.3, 10, 12;
> állapot: [`map-data-layer-requirements.md`](map-data-layer-requirements.md)
> („MAP-3 státusz", „MAP-3 review utómunka").
> Platform: csak Windows 11 (MSVC, RelWithDebInfo és Debug) futott;
> Linux/FreeBSD **nem tesztelt** (a kód platformfüggetlen: `std::thread`,
> `std::atomic`, `std::filesystem`; nincs Win32-hívás).

## 0. A review négy kérdése — rövid válaszok

| Review-kérdés | Válasz | Bizonyíték |
|---|---|---|
| 1. Mi garantálja, hogy egy ticken belüli, **nem pinnelt** olvasó chunkját az eviction nem szabadítja fel idő előtt? | A felszabadítás egyetlen feltétele a supervisor-csendes ablak (egyetlen tick sincs repülésben), a tick-jelző kiadás előtti beállításával és release/acquire párjával (3.2). Pint csak az író (supervisor) szál vesz, így az eviction-döntés és a pin nem versenyez (3.4). A késői completion nem hivatkozik zónára (3.5). A H9 két tickes grace erre nem bizonyíték, és nem is erre épül (3.6). | `streamlife`: 4 szálas tick-olvasó stressz, mérgezett felszabadítással; 775 540 480 mintaolvasás, **0 eltérés**; 437 chunk került ki a slotból, miközben egy olvasó tartotta. Negatív kontroll (csendes ablak nélküli felszabadítás): 7 312 824 eltérés. Debugban az író-szál assert aktív, nem sült el. |
| 2. Valódi streamingterhelés és budget, streaming módban | A saját könyvelés = rezidens + repülő + retired + metaadat, a budget alatt; RSS külön. Mérve 16 MB-os és 3 MB-os (kimerült) budgettel a 100 km-es világon: betöltés, eviction, resident / in-flight / pinned / retired csúcsok, sor-csúcsok, admission-várakozás. A pinnelt adat > budget esete és az induló halmaz > budget esete (exit 2) tesztelt (4.). | `streamsoak` 16 MB és 3 MB (4.4); `pinned-above-budget-no-eviction-no-overrun`, `waiting-list-full-rejects-explicitly`, `startup-set-above-budget-detected`; indulási teszt 7c (4.3) |
| 3. `NotResident` / `InvalidData` futásidőben | Fogyasztónkénti táblázat: mozgás, spawn, respawn, warp, migráció, víz, navigáció — mit halaszt, mit utasít el, hogyan próbál újra, mit takarít (5.2). A „Failed → korlátos újrapróba → invalid" pontos jelentése (5.1). Az invalid chunk soha nem 0 m és soha nem járható. | 12 teszteset a `streaming`, `worldquery` és `streamlife` módban (5.3) |
| 4. ASF + streaming + reclamation együtt | Egy gated (függő) betöltés alatt 3× split+merge: migráció, retire, reclaim, slot-újrahasználat; utána a késői completion elfogadva és publikálva, a várakozó entitás továbbmegy. Mellette 1→4→16→4→1 lassú I/O-val, alvó zóna ébresztése kiürített chunkokra, generáció-reset és leállítás függő I/O-val (6.). | `late-completion-after-migration-retire-reclaim-slot-reuse`, `dormant-zone-does-not-pin-terrain`, `wake-after-eviction-waits-for-data-no-stale-z`, `split-merge-with-slow-io-no-duplication-no-reload` (6.) |

A review-utómunka **hét valódi hibát** talált és javított (9.2). Kettő
közvetlenül a review kérdéseit érinti: a cellán belüli lépés hiányzó adaton,
és az invalid chunk felülírásakor az azonnali felszabadítás.

## 1. Mi készült (MAP-3)

### 1.1 Loader és formátum (`shared/map`)

- **Rezidencia-mód:** `eager` (alapértelmezés, a MAP-2 szerződése) vagy
  `streaming`. Streaming indításkor:
  - minden chunk-fájl létezése és mérete ellenőrzött, olvasás nélkül;
  - csak az **induló halmaz** töltődik be teljes ellenőrzéssel: a
    spawn-régiók közepe és a warp-célok chunkjai;
  - minden más chunk a betöltéskor kap CRC-, dekódolás- és
    varrat-ellenőrzést.
- **`ChunkSource` / `PackageChunkSource`:** szálbiztos, állapotmentes
  egy-chunk betöltés.
- **`ServerTerrain` publikált slotokkal:**
  - az olvasó zár és referenciaszámlálás nélkül lát egy teljes, immutábilis
    chunkot vagy semmit (`NotResident`);
  - írni egyetlen szál ír;
  - pinek (`Owned`, `PinCount`) a ticken túl élő olvasóknak.
- **Útellenőrzés:** `Segment` (grid traversal; sarok-átlónál mindkét
  oldalcella), `CellIndexOf`.
- **Víz:** manifest `water` @18 (undeclared / none / seaLevel / bodies),
  `waterBodies` réteg (MXWS v1), `ServerWater::Query`, hibakódok 600–604.
- **Korlátok:** a 2^28-as mintakorlát csak eagernél; a chunk-index legfeljebb
  2^20.

### 1.2 Gameserver (`world/terrain/`)

- **`TerrainStreamer`:** I/O-szálak, állapotgép, admission-foglalás a budget
  alatt, LRU + megtartási ablak, pinnelt chunk nem eviktálható, felszabadítás
  csak csendes ablakban, generáció + epoch, lemondás, leállítás, metrikák.
- **`TerrainDemandBuffer`** zónánként: tick alatti igény, dedup, publikálás
  a tick végén; a supervisor minden passban aggregál és pumpál.
- **`TerrainService::CheckStep`:** útellenőrzés, cellán belüli
  adat-követelmény, lejtő- és mélyvíz-szabály; `Water()`,
  `LookaheadSeconds()`.
- **`NavigationService`:** rács-A*, 8 irány, sarokvágás nélkül; keresési
  ablak; job- és passbudget; legfeljebb 256 futó job; lemondás; pinek a job
  alatt; korlátos várakozás chunkra.

### 1.3 Konfiguráció (`gameserver.conf`)

`terrain_residency` (`eager` | `streaming`), `terrain_cache_budget_mb`,
`terrain_io_threads`, `terrain_max_in_flight`, `terrain_retain_seconds`
(≥ 0.1), `movement_max_slope` (alapból ki), `movement_max_water_depth_m`
(alapból ki). Ha az induló halmaz nem fér a budgetbe: exit 2.

## 2. Futásidejű szabályok (a korábbi riport 2.3 pontjának folytatása)

- **Mozgás:** a lépés teljes útja ellenőrzött, a blokkoló rács a mobokra is
  érvényes. A kezdőcellán belül maradó lépés is csak adattal megy (a cél
  magassága lesz a z; ez a review-körben javított hiba, 9.2/1).
- **Hiányzó chunk (`NotResident`):** a lépés ebben a tickben elmarad, a
  chunk igénylődik, az entitás helyben vár. A várakozás a betöltés idejéig
  tart. A megállás állapota és a latest-movement input megmarad, a
  szimulációs idő nem ugrik.
- **Érvénytelen chunk (`InvalidData`):** a lépés elutasítva, igény és
  várakozás nélkül. Az entitás a határon megáll, a z az utolsó `Ok`
  magasság.
- **Prefetch:** `sebesség × lookahead`, ahol
  `lookahead = clamp(3 × p99 betöltés + 1 tick, 0.5 s, 5 s)`.
- **Spawn:**
  - A spawn-régió és a warp-cél chunkjai állandóan rezidensek (pinned).
  - A kezdő mob-spawn a szabad budgetbe férő kötegekben fut.
  - A respawn korlátosan újrapróbál (40 × 0.5 s), utána eldob (számláló).
  - Invalid chunkon a mob-jelölt elutasítva és számolva, újrapróba nincs.
- **Dormant entitás és alvó zóna** nem igényel terraint: a chunkjuk
  eviktálható, ébredéskor az ottani lépések várnak.
- **Split / merge / migráció / reclaim** nem ürít cache-t, nem tölt újra,
  nem spawnol és nem despawnol.

A teljes, fogyasztónkénti viselkedés az 5.2 pontban, a szerződés a
[`map-data-format.md`](map-data-format.md) 9.3 és 12 pontjában.

## 3. Élettartam: zár nélküli olvasás és felszabadítás (review 1)

### 3.1 Ki olvas, melyik szálon, mi védi

| Olvasó | Szál | Olvasási ablak | Mi védi a chunkot |
|---|---|---|---|
| Mozgás (játékos, mob), warp-trigger (`CheckStep`, `Height`, `Cell`, `Water`) | zóna-worker, a zóna tickje alatt | egy lekérdezés. A nyers mutató nem hagyja el a hívást; a szerződés az egész ticket engedi. | csendes ablak (3.2) |
| Játékos-spawn, mob spawn/respawn, migráció és partíció-transzfer (z), `PumpTerrain`, navigáció, snapshot-gyűjtők (`ReadWorld`) | supervisor | a hívás | szál-azonosság: ugyanez a szál szabadít fel, olvasás közben nem tud |
| Navigációs job több passon át | supervisor | a job teljes ideje | pin (a rezidenciát tartja; a memóriát a szál-azonosság) |
| Loader és kezdő kötegelt spawn | a runtime-ot építő szál, `Start` előtt | — | nincs más szál |
| Bench / eszköz futó világon | bármely | — | csak `ReadWorld`-on át, a supervisoron |

Nyers chunk-mutatót (`Published`) és pint (`Owned`) a gameserverben csak a
streamer és a navigáció használ, mindkettő a supervisoron (grep-pel
ellenőrizve). A zóna-kód csak érték-visszaadó lekérdezést hív.

### 3.2 A happens-before lánc (miért nem szabadul fel idő előtt)

1. A zóna-ticket **csak a supervisor** indítja: `ZoneScheduler` a tick-jelzőt
   (`tick_in_progress`) **a kiadás előtt** CAS-szal igazra állítja, utána
   teszi a feladatot a worker-sorba.
2. A worker a tick legvégén, minden olvasás után, **release**-szel hamisra
   állítja.
3. A felszabadítás egyetlen helye `TerrainStreamer::FreeRetired`, amelyet
   csak `quiescent == true` mellett hív (egyetlen kivétel a teszt-
   negatívkontroll kapcsolója; a `Start` előtti blokkoló betöltés és kötegelt
   spawn eleve csendes, mert még nincs zóna-tick). A `quiescent` értéke:
   `!zones_.AnyTickInProgress()`, **acquire** olvasással, ugyanabban a
   supervisor-passban (`PumpTerrain`).
4. Ha a supervisor minden jelzőt hamisnak lát, minden korábban kiadott tick
   olvasásai happen-before a felszabadítás. Egy chunk, amelyet bármelyik
   tick betölthetett, addigra már nincs használatban.
5. A felszabadítás közben új tick nem indulhat, mert a supervisor maga van a
   `Pump`-ban. Egy később kiadott tick a mutex-szinkronizált worker-soron át
   látja az unpublish (release store) hatását: az üres vagy az új slotot, a
   régi mutatót soha.
6. A slotot elhagyó chunk — evicted, vagy egy későbbi publikálás lecseréli —
   a **retired listára** kerül, bájtjai könyvelve maradnak, és csak a 3.
   pont feltétele mellett szabadul fel. A `Publish` a lecserélt chunkot
   visszaadja; korábban azonnal eldobta (9.2/2).

### 3.3 A csendes ablak pontos szerepe

- Ez a streamer **egyetlen** felszabadítási feltétele; nincs időalapú vagy
  tick-számláló alapú türelmi idő.
- Csendes ablakon kívül az eviction csak retire-ol. A `MakeRoom` a vetített
  könyvelést (retired nélkül) használja, így nem eviktál túl (MAP-3 első kör).
- Ha a supervisor sokáig nem lát csendes ablakot (telítettség), a retired
  bájtok könyvelve maradnak. Az admission ilyenkor vár; a budget nem lépődik
  túl.
- A stresszben a passok 11 %-a (2257 / 19 832) volt csendes; a felszabadítás így is
  folyamatos volt (627 felszabadítás 4 s alatt).

### 3.4 Pinek: ki veszi, miért biztonságos

- Pint **csak az író (supervisor) szál** vesz és enged el: a navigációs job
  a supervisoron fut, a streamer ugyanott dönt az evictionről.
- A `PinCount` (`use_count − 1`) ezért pontos, és nem változhat meg az
  ellenőrzés és az unpublish között. Nincs TOCTOU.
- A **Debug build kikényszeríti:** a `BindWriterThread` után minden
  író-oldali hívás (`Publish`, `Unpublish`, `Owned`, `PinCount`, `Clone`)
  assertál az író szálra. A supervisor a `Run` elején köti be magát, a
  leállító szál a `Stop`-ban. A teljes Debug `streaming` / `worldquery` /
  `streamlife` futás alatt az assert nem sült el.
- A pin a **rezidenciát** védi (pinnelt chunkot eviction nem választ). A
  memória-élettartamot a supervisoron a szál-azonosság adja. Ha egy jövőbeli,
  más szálon futó olvasó pint vesz, azt az író szál adja át neki.
- A generáció-reset (teszt-seam, csomagcsere) a pinnelt chunkot is kiveszi a
  slotból. A pin tulajdonos marad, tehát a memória él, amíg el nem engedik.

### 3.5 Késői I/O-completion

- A completion `(generation, request_epoch, chunk index)`-et hordoz, és
  **semmilyen zóna-, slot- vagy entitás-hivatkozást nem**.
- Elavult generáció, lemondott kérés (epoch) vagy `Stop()` utáni eredmény
  nem publikálódik; a memóriája a supervisoron felszabadul.
- Egy aktuális completion a világ-cache-be publikálódik, bármi történt
  közben a zónákkal.
- Tesztek:
  - `late-completion-after-generation-reset-rejected`;
  - `generation-reset-at-runtime-stale-rejected`;
  - `shutdown-with-pending-io`, `runtime-shutdown-with-pending-loads`;
  - `late-completion-after-migration-retire-reclaim-slot-reuse` (6.).

### 3.6 Miért nem a H9 grace a bizonyíték

- A H9 retired-zone reclamation két tickes türelmi ideje a **zóna-
  objektumokat** (flecs világ, slot) védi, és azokra érvényes.
- A chunk-élettartam **nem erre épül**:
  - a felszabadítás feltétele a 3.2 szerinti csendes ablak, amely
    tetszőlegesen későn érkező I/O mellett is érvényes;
  - az I/O-feladat és a completion nem tart zónát, így a zóna-reclaim és a
    slot-újrahasználat nem érinti;
  - a függő olvasásokat a generáció és az epoch szűri.
- A tesztek ezt külön bizonyítják: gated (tetszőlegesen késleltetett) I/O
  közben 3 × split+merge, reclaim és slot-reuse (6.).

### 3.7 Tesztek — pozitív eset és negatív kontroll

**`lifetime-tick-readers-never-see-freed-chunk`** (`streamlife`):
- 4 olvasó szál a runtime pontos tick-protokolljával: jelző CAS-szal a
  kiadás előtt, release a tick végén. A supervisor-pass ugyanabban a
  sorrendben fut, mint a `WorldRuntime::Run`.
- Az olvasók a tick **egész idejére** megtartják a betöltött nyers mutatókat
  (0.8–2.0 ms). Ez szélesebb ablak, mint bármely production olvasóé, amely
  lekérdezésenként tölti újra.
- Minden olvasott mintát bitre összevetnek a teljesen rezidens referenciával.
- Churn: sepert igény-ablak, ~30 rezidens chunkos budget, 0.1 s megtartás,
  véletlenszerű, egymástól független periódusok.
- A felszabadított chunkot a teszt-mód **megmérgezi** (egy csomagban elő nem
  forduló sentinel) és karanténba teszi. Így egy idő előtti felszabadítás
  látható eltérés lesz, nem undefined behaviour.

| Futás | Olvasások | Eltérés | A tartott chunk közben kikerült a slotból | Eviction / felszabadítás |
|---|---|---|---|---|
| RelWithDebInfo | 775 540 480 | **0** | 437 | 627 / 627 |
| Debug | 245 807 360 | **0** | 373 | 612 / 612 |
| **Negatív kontroll** (csendes ablak nélküli felszabadítás), RelWithDebInfo | 821 412 096 | **7 312 824** | 380 | — / 624 |
| Negatív kontroll, Debug | 263 602 688 | **2 765 661** | 429 | — / 638 |

A negatív kontroll mutatja, hogy a teszt észleli az idő előtti
felszabadítást. A pozitív futás nullája ezért bizonyíték, nem a teszt
vaksága.

- **Korlát:** a negatív kontroll szándékosan versenyt ír (a méregírás és az
  olvasás egyidejű). Ez csak ebben a teszt-módban fordul elő; a karantén
  miatt memória-hozzáférési hiba nincs.
- **Nem futott:** AddressSanitizer / ThreadSanitizer. MSVC-n nincs TSan, az
  ASan-build nem készült el.

Egység-szintű tesztek (`streaming`):
- `evicted-chunk-freed-only-in-quiescent-window`;
- `no-over-eviction-outside-quiescent-window`;
- `pinned-above-budget-no-eviction-no-overrun`;
- `released-pins-become-evictable`;
- `invalid-chunk-generation-reset-retired-then-valid-reload`.

## 4. Memória- és I/O-budget, streaming módban (review 2)

### 4.1 Könyvelési modell

**Saját könyvelés** (`accounted`), amely soha nem lépheti túl a
`terrain_cache_budget_mb`-t:

| Tétel | Tartalom |
|---|---|
| resident | publikált chunkok payloadja + chunkonkénti overhead; az invalid chunk csak overhead |
| in-flight | a folyamatban lévő betöltések foglalása: dekódolt méret + olvasási puffer + overhead, már a sorba állítás előtt lefoglalva |
| retired | slotból kikerült, még fel nem szabadított chunkok (régi generáció is) |
| metaadat | slot-táblák, chunk-index (9604 chunknál 1.61 MB) |

- A **pinned** bájtok a resident része (ezt eviction nem érintheti), külön
  mérve.
- A **folyamat RSS-e** külön mérendő (OS-nézet: zónák, entitások,
  allokátor-visszatartás). A kettő nem azonos.
- A teszt-karantén (méreg-mód) a könyvelésen kívül esik; productionben nincs.

### 4.2 Admission, várólista, elutasítás

- **Betöltés előtt foglal.** Ha nincs hely:
  1. LRU eviction a megtartási ablakon kívüli, nem pinnelt chunkokból;
  2. ha így sincs hely, az igény korlátos várólistán vár (4096,
     igénysorrendben);
  3. ha az is tele, elutasítás (`admission_rejects`), és a fogyasztó a
     következő tickben újra igényli.
- **I/O-korlátok:** `terrain_io_threads` szál (alap 2), legfeljebb
  `terrain_max_in_flight` foglalt vagy repülő job (alap 32).
- **Magas vízállás:** 90 % fölött proaktív eviction 75 %-ig.
- **Egy passban az első sikertelen helycsinálás után** a többi várakozó a
  következő passra vár (9.2/6).

### 4.3 Ha a szükséges vagy pinnelt adat nem fér el

| Eset | Viselkedés | Teszt |
|---|---|---|
| Az induló halmaz (pinned) > budget | a szerver exit 2-vel leáll, DB és hálózat előtt; semmi nem eviktálódik, hogy beférjen | indulási teszt 7c (valódi `gameserver`); `startup-set-above-budget-detected` |
| Az olvasók pinjei > budget | nincs eviction, nincs túllépés; az új igény vár | `pinned-above-budget-no-eviction-no-overrun`, `released-pins-become-evictable` |
| Túl sok egyidejű igény, nincs hely | a várólista korlátos, a többi explicit elutasítás | `waiting-list-full-rejects-explicitly` (4 vár, 6 elutasítva, 0 olvasás) |
| A munkakészlet > budget futás közben | degradált: a fogyasztók várnak, a budget tart, a z pontos (4.4, 3 MB) | `streamsoak --budget-mb 3` |
| Egy spawn-kör chunkjai > szabad budget | a pont kimarad (ERROR log) | kód: `SpawnConfiguredMobsStreaming` |

- **Éhezés elleni garancia** nincs azon túl, hogy a várólista
  igénysorrendű. A budgetet a munkakészletre kell méretezni; a
  miss-késleltetés és a `steps_waiting` metrika mutatja.

### 4.4 Mérések (`streamsoak`, 100 km × 100 km)

Futtatás: RelWithDebInfo, Windows 11, NVMe SSD. A csomag 152 MB, 9604
chunk, 16 m-es cellák. 200 barangoló játékos, 2 másodpercenként 20 %-uk
áthelyezve; 3400 entitás; 60 s. Időalap: a soak ablaka. Populáció: a világ
egyetlen streamere.

| Mérés | 16 MB budget | 3 MB budget (a munkakészlet alatt) |
|---|---|---|
| Betöltés / eviction / felszabadítás | 1180 / 803 / 803 | 731 / 734 / 734 |
| Olvasott bájt | 18.8 MB (0.31 MB/s) | 11.6 MB (0.19 MB/s) |
| Találati arány; igények (hit / miss / dedup) | 0.997; 359 011 = 357 830 + 1180 + 1 | 0.982; 116 136 = 68 144 + 1232 + 46 760 |
| Repülő jobok csúcsa / várólista csúcsa | 32 (a korlát) / 281 | 32 (a korlát) / 281 |
| Admission-várakozás / elutasítás / lemondás | 427 / 0 / 0 | 371 179 / 0 / 734 (a lejárt várakozó igények) |
| **Saját könyvelés csúcsa** | **14.60 MB ≤ 16 MB** | **2.99 MB ≤ 3 MB** |
| Csúcsok: resident / in-flight / retired / pinned | 11.97 / 1.02 / 2.61 / 0.016 MB | 1.36 / 1.02 / 0.03 / 0.016 MB |
| Végállapot: resident / pinned / in-flight / retired / metaadat | 695 chunk, 11.14 MB / 1 (permanent), 0.016 MB / 0 / 0 / 1.61 MB | 82 chunk, 1.31 MB / 1 (permanent), 0.016 MB / 0 / 0 / 1.61 MB |
| Betöltési késleltetés p50 / p99 / max (admission → publikálás) | 5.91 / 17.25 / 18.79 ms | 1.64 / 13.55 / 15.42 ms |
| Miss-késleltetés p50 / p99 / max (első igény → publikálás) | 6.40 / 18.49 / 19.23 ms | 1140 / 3095 / 3111 ms |
| Mozgás-lépések: ok / adatra vár / invalid | 1 554 799 / **0** / 0 | 937 218 / **509 919 (35.2 %)** / 0 |
| Supervisor-pass átlag | 0.41 ms | 0.52 ms |
| Folyamat RSS (soak eleje → min / max → vége) | 307 → 307 / 336 → 336 MB | 304 → 305 / 335 → 336 MB |
| Magasság-orákulum | minden entitás z-je bitre = eager referencia | ugyanígy |

Hideg indulás ugyanazon a csomagon:
- **eager:** 2.07 s, 151 MB rezidens;
- **streaming:** 0.99 s, 16 KB rezidens (9604 fájl méret-ellenőrzése, 1
  dekódolt chunk).

**Megjegyzések a mérésekhez:**
- A 16 MB-os futás 16 MB alatti csúccsal 0 adatra váró lépést adott: a
  prefetch megelőzte a hiányt.
- A 3 MB-os futás a kimerült budget: a munkakészlet (játékosonként 1–2
  chunk) nagyobb a helynél. A fogyasztók várnak (a lépések ~35 %-a), a
  miss-késleltetést a megtartási ablak (3 s) uralja. A budget tart, a
  magasság pontos.
- A betöltési késleltetés p99-ét az áthelyezési hullámok sorban állása adja
  (32 repülő job, 2 I/O-szál).
- Az első MAP-3 riport p50 6.0 ms-os értékét a **hibás ébresztés** mellett
  mérték (9.2/4). Ugyanez a bináris ebben a körben 14.8 ms-ot mért: az
  OS-időzítő felbontásától függött. A javítás után a mért érték a fenti.
- **OS page cache:** a csomag frissen írt, így valószínűleg az OS
  cache-ében van. Hideg-lemez mérést nem végeztünk; a „cold" csak a szerver
  saját cache-ére vonatkozik.

## 5. `NotResident` és `InvalidData` futásidőben (review 3)

### 5.1 „Failed → korlátos újrapróba → invalid" pontosan

1. **Kísérletnek számít** minden betöltési hiba: olvasás, CRC, dekódolás,
   varrat-eltérés egy publikált szomszéddal.
2. **Két kísérlet között** a slot üres: a lekérdezés `NotResident`, a
   fogyasztók várnak. A következő kísérlet csak akkor indul, ha egy
   fogyasztó a backoff (`kísérlet × 0.5 s`) után **újra igényli**. Igény
   nélkül nem próbálkozik.
3. **A 3. kísérlet után** a chunk **invalidként** publikálódik: mintátlan,
   üres chunk, csak az overhead könyvelt.
   - Minden lekérdezése `InvalidData`.
   - Nem eviktálható, nem olvasódik újra, és a csomag-generáció végéig nem
     igénylődik (a `Demand` azonnal visszatér).
   - A szomszédai varrat-ellenőrzése kihagyja (9.2/3).
4. **Csak generációváltás** (csomagcsere; teszt: `ResetGenerationForTest`)
   vonja vissza: retire-ral, nem azonnali felszabadítással. Utána friss
   betöltés jöhet (`invalid-chunk-generation-reset-retired-then-valid-reload`).
5. Az invalid chunk **soha nem 0 m, soha nem járható, soha nem „nincs víz"
   és soha nem „nincs út"**.

### 5.2 Fogyasztónként

| Fogyasztó | `NotResident` | `InvalidData` | Újrapróba / korlát | Takarítás (disconnect, lemondás) |
|---|---|---|---|---|
| Mozgás (játékos) | a lépés elmarad, a chunk igénylődik (a jelenlegi és a lookahead-chunkot tickenként amúgy is kéri); z nem változik | a lépés elutasítva, nincs igény; a határon megáll; számláló `invalid` | tickenként újra (latest-movement input); a várakozás a betöltésig tart | nincs tartós regisztráció: disconnect után az igény a megtartási ablak (≥ 0.1 s) után lejár; a még nem olvasott job lemondódik; más fogyasztó betöltését nem mondja le |
| Mozgás (mob) | ugyanígy; a mob a saját chunkját igényli, amikor integrál | ugyanígy | ugyanígy | despawn / migráció után az igény lejár |
| Mozgás, kezdőcellán belüli lépés | vár (javítva, 9.2/1) | elutasítva | — | — |
| Játékos-spawn | a spawn-régió közepe pinned, tehát nem fordulhat elő; a debug felülírás nem rezidens pontja a spawn-régióra esik vissza, és igényli a chunkot | a debug pont elutasítva, visszaesés a spawn-régióra; ha már a spawn-közép sem `Ok`, a belépés elutasítva (`SERVER_ERROR`, számláló) | nincs várakozás a belépésben | az elutasított belépés a presence-t azonnal elengedi |
| Mob kezdő spawn (kötegek) | a köteg chunkjai betöltődnek, mielőtt a pontok spawnolnak | a jelölt elutasítva, a körön belül újrasorsol (legfeljebb 9 jelölt); ha az utolsó is invalid, a mob nem jön létre, számláló `MobSpawnsRefusedInvalidTerrain` | nincs újrapróba | a köteg után a chunkok elengedve (eviktálhatók) |
| Mob respawn | igényli a chunkot, 0.5 s múlva újra; spawnpontonként legfeljebb 40-szer egymás után, utána eldob (`RespawnsDroppedNoTerrain`) | elutasítva és számolva, nincs újrapróba | 40 × 0.5 s | a számláló a sikeres spawnkor törlődik |
| Warp | a warp-célok pinnelt induló halmaz, validált csomagnál nem fordulhat elő; védekező ág: nem sül el, a cél chunkja igénylődik | nem fordulhat elő (az induló halmaz hibája indítási hiba); védekező ág: nem sül el (tickenként WARN — MAP-4) | tickenként, amíg a játékos a forrásban áll | — |
| Migráció / split-merge transzfer | a z a saját marad (nincs találgatott magasság) | ugyanígy | — | — |
| Víz (`Water`) | `NotResident` (a mélységhez talaj kell) | `InvalidData`; víztesten kívül `NoWater`, mert a víztest-lista teljes csomagadat | — | — |
| Navigáció | a job igényli a chunkot, és vár (korlátos időkorlát), utána `NotResident` | a cella zárt; ha a keresés kimerül, és invalid adat volt a határán: `InvalidData` (nem `NoPath`); a körülötte vezető út `Found` | a várakozás passonként | a job nincs entitáshoz kötve (nincs gameplay-hívó); `PostNavigationCancel` a pineket azonnal elengedi; legfeljebb 256 futó job (fölötte `Rejected`); az eredmények tárolása korlátos (4096) |

### 5.3 Tesztek

| Teszt | Mód | Mit bizonyít |
|---|---|---|
| `failed-chunk-bounded-retry-then-invalid` | streaming | 3 olvasás, utána igény mellett sem több; `InvalidData`, nem járható |
| `corrupt-chunk-file-crc-then-invalid` | streaming | valódi CRC-hiba betöltéskor, 3 kísérlet |
| `neighbour-of-invalid-chunk-loads` | streaming | az invalid szomszédja betöltődik (korábban OOB olvasás); `Vertex` → `InvalidData` |
| `invalid-chunk-generation-reset-retired-then-valid-reload` | streaming | a reset retire-ol (a chunk él, amíg tick olvashat), a javított chunk érvényesen újratöltődik |
| `path-check-same-cell-needs-data` | worldquery | cellán belüli lépés: nem rezidens → vár, invalid → elutasítva |
| `nav-invalid-data-not-no-path`, `nav-job-cap-rejects-explicitly` | worldquery | `InvalidData` ≠ `NoPath`; 300 kérésből 256 fut, 44 `Rejected`, lemondás után 0 pin |
| `invalid-chunks-mob-spawn-refused-counted` | streamlife | 4 invalid chunkon álló kör: 90/120 mob, 30 elutasítva és számolva |
| `invalid-chunk-movement-refused-no-guess` | streamlife | a futó a határon megáll, a z = referencia; 0 várakozásnak számolt lépés |
| `invalid-chunk-queries-never-land-or-zero` | streamlife | magasság / cella / víz `InvalidData`, nem járható |
| `runtime-nav-invalid-data-not-no-path` | streamlife | ugyanez a runtime `PostNavigationRequest`-en át |
| `movement-waits-for-data-no-guess-stop-kept` | streaming | prefetch nélkül vár, átmegy, a megállás megmarad |
| `wake-after-eviction-waits-for-data-no-stale-z` | streamlife | ébredés kiürített chunkokra: 0 adat nélküli lépés, 0 z-eltérés |

## 6. ASF + streaming + reclamation együtt (review 4)

| A munkaprompt 17. pontja | Teszt | Eredmény |
|---|---|---|
| Entitás chunk- és zónahatáron át | `players-cross-chunk-borders-streamed-heights-exact`; `split-merge-with-slow-io-no-duplication-no-reload` (az első vágás chunkhatár is) | z bitre = referencia; ugyanaz az entitáshalmaz |
| `1 → 4 → 16 → 4 → 1` | `split-merge-with-slow-io-no-duplication-no-reload` (30 ms-os olvasások) | minden lépésben validátor OK, lekérdezés-eltérés 0, csomag-újratöltés 0 |
| Chunk load completion migráció után | `late-completion-after-migration-retire-reclaim-slot-reuse` | a futó a chunkhatáron vár (x = 0.20), az olvasás gated; közben 3 × split+merge: 7 különböző zónában élt (migrációk), slot-reclaim 0 → 15, slot-reuse 0 → 9; a betöltés végig függőben. Kinyitás után a completion **elfogadva** (stale 0 → 0), a futó átmegy (x = −10.3); z-eltérés 0, duplikáció 0, validátor OK |
| Retire / reclaim közbeni függő I/O; azonos sloton új zóna | ugyanez | ugyanez az eset: a függő I/O alatt a régi zónák retire-olódtak és reclaimelődtek, az új zónák ugyanazokat a slotokat kapták (9 újrahasználat); a completion csak (generáció, epoch, chunk) — nem látja és nem igényli a zónát |
| Olvasás közbeni eviction | `lifetime-tick-readers-never-see-freed-chunk` (+ negatív kontroll) | 3.7 |
| Scheduler sleep / wake | `dormant-zone-does-not-pin-terrain`, `wake-after-eviction-waits-for-data-no-stale-z` | a mob-only zóna elalszik, a 4 spawn-kör chunkja eviktálódik (30 mob marad, z pontos). Egy játékos belép, az olvasás gated: 23 lépés adatra várt, a nem rezidens chunkon álló 21 mob közül **0 mozdult**, z-eltérés 0. Kinyitás után 4/4 chunk rezidens, 30/30 mob mozgott, z-eltérés 0, validátor OK |
| Szerverleállítás függő betöltéssel | `shutdown-with-pending-io`, `runtime-shutdown-with-pending-loads` | ≤ 1 olvasás ideje (60–190 ms); utána semmi nem publikálódik |
| Csomagváltás (stale completion) | `generation-reset-at-runtime-stale-rejected` | stale elutasítva, újra rezidens, lekérdezés-eltérés 0 |

A terrain-rezidencia nem módosítja a NetId-t, a presence-t, az AOI-t és a
replikációs alapállapotot, és nem spawnol vagy despawnol. Bizonyíték:
azonos entitás- és NetId-halmaz minden topológia-lépés után, a validátor
`OK`, duplikáció nincs.

## 7. Megvalósítási kategóriák (munkaprompt 16. pont)

| Szolgáltatás | Production hívóhoz kötött | Csak fixture/API szinten | Részleges | Nem támogatott |
|---|---|---|---|---|
| Streaming, cache, budget, élettartam | ✓ (`terrain_residency=streaming`) | — | — | — |
| Static collision (rács a lépés útján) | ✓ mozgás, alapból aktív | — | — | többszintes geometria, statikus alakzatok, dinamikus akadályok |
| Lejtőkorlát | ✓ konfig, alapból ki | — | csak felfelé, végpont-magasságból | — |
| Víz | ✓ mélyvíz-szabály, konfig, alapból ki | lekérdezés-API | vízterek csak téglalapok | hajófizika, hullám; a kliens MXWB-maszkja |
| Navigáció | — (nincs gameplay-hívó) | ✓ szolgáltatás + `PostNavigationRequest` | a rács a nav-adat, nincs navmesh | navmesh-generálás, útvonal-policy |

## 8. Tesztek és regresszió

### 8.1 A MAP-3 módok (végső bináris)

| Mód | RelWithDebInfo | Debug |
|---|---|---|
| `streaming` | 27/27 | 27/27 |
| `worldquery` | 27/27 | 27/27 |
| `streamlife` (új) | 10/10 | 10/10 |
| `streamsoak --cycles 60` (16 MB / 3 MB) | 3/3 és 3/3 | — |
| `terrain` / `mapsplit` / `bootstrap` | 20 / 21 / 4 | — |
| `worldpackage` / `snapshot --cycles 300` | 119 / 8 (a regressziós binárison; a későbbi változások nem érintik) | — |
| indulási teszt (valódi `gameserver`, DB-vel) | 27/27 | — |

### 8.2 Teljes regresszió

RelWithDebInfo, a `worldbench_hardening.exe` (0efc4033) bináris ellen; a
párok egymás után futottak, így a gép állapotának változása mindkettőt
egyformán érinti.

**Base vs új párok (22):**
- 19 pár azonos PASS-számmal.
- A 3 readiness-futás új binárison +1 (`snapshot-quiescent`), mint a MAP-1
  óta.
- A `scheduler` mindkét binárison 13/13. Ez a teszt falióra-alapú, és egy
  korábbi köztes futásban mindkét binárison elbukott: nem számoljuk
  bizonyítéknak.

**Csak-új módok, mind zöld:**
- tickrate 5, inputpath 2, netstress ×3 (13), presence 8,
  asfdeterminism 5, workerpool 3, replv2 9, protocol 7;
- reclamation 100 / 1000 (3 / 3), hygiene 3;
- mapaudit: server_changed = 12, open/partial = 2;
- snapshot (300) 8, worldpackage 119, terrain 20, mapsplit 21, bootstrap 4;
- streaming 27, worldquery 27, streamlife 10;
- streamsoak 16 MB 3/3 és 3 MB 3/3.

**Readiness tick (ms), base → új:**

| Futás | p50 | p95 | p99 |
|---|---|---|---|
| smoke 100p / 20k | 2.32 → 0.94 | 4.36 → 3.59 | 4.90 → 4.40 |
| dense 500p / 200k | 4.13 → 4.12 | 5.09 → 5.08 | 66.3 → 67.2 |
| spread 7000p / 200k | 6.29 → 6.03 | 8.53 → 8.38 | 9.91 → 9.79 |

- A 7000p abszolút értékei magasabbak, mint a MAP-3 első regressziójában
  (2.00 ms p50): más gépállapot és más zónaszám (76 / 84). Párban egyeznek.
- A smoke p50-es eltérése a korábbi ismétlések szerint zaj.
- A readiness szintetikus, sík világon mér. A file-backed eager világ
  lépésenkénti plusz cella-lekérdezését (cellán belüli lépés) külön nem
  mértük; a `terrain` és a `mapsplit` futás zöld.

**A regresszió után három apró változás** történt: a magasvízállás-scan
kihagyása sikertelen helycsinálás után, a `LoadBlocking` dedup-mentes
igény-frissítése, és a `wake-after-eviction-waits-for-data-no-stale-z` teszt megfigyelési ablaka (legalább 20
visszautasított lépés). Ezért a végső binárison újrafutott:
- `streamlife` 10/10, `streaming` 27/27, `worldquery` 27/27, `terrain` 20,
  `mapsplit` 21, `bootstrap` 4;
- `streamsoak` 16 MB és 3 MB (3/3, 3/3);
- Debug: `streamlife` 10/10, `streaming` 27/27, `worldquery` 27/27;
- indulási teszt 27/27.

A változások csak a streaming-módú kódot és a benchet érintik; a
regressziós párok eager / szintetikus világon futnak. A fenti mérések (4.4,
3.7, 6.) a végső binárisról valók.


## 9. Talált és javított hibák

### 9.1 Az első MAP-3 körben

1. **Activity duplikált forrás** (meglévő verseny, Debugban 5 futásból
   2-szer): migráció, transzfer vagy despawn után a forrás-zóna publikált
   activity-forrása a következő tickjéig megmaradt. Javítás:
   `Zone::ExtractPlayerBinding` a forrást is eltávolítja.
2. **Túl-eviction csendes ablakon kívül:** a retired bájtok miatt egy
   nem-csendes pass minden eviktálhatót kidobott. Javítás: vetített
   könyvelés.
3. **Megtartási minimum:** 0 s-os megtartásnál egy igény a betöltés előtt
   lejárt; a megtartás most legalább 0.1 s.
4. **`fixture-load`:** a streaming induló chunk kétszer számolódott.

### 9.2 A review-utómunkában (mind tesztelve)

1. **Cellán belüli lépés hiányzó adaton** (a MAP-3 saját hibája; a review
   3. pontja):
   - Hiba: az útellenőrzés kihagyja a kezdőcellát, ezért egy nem rezidens
     vagy invalid chunkon a cellán belüli lépés `Clear` volt, és a z
     elavult maradt. Ez eviktált chunkon ébredő dormant mobnál fordult elő.
   - Javítás: a `CheckStep` a célcella adatát is megköveteli.
   - Visszaellenőrzés: a javítás ideiglenes kikapcsolásával a
     `path-check-same-cell-needs-data` és a
     `wake-after-eviction-waits-for-data-no-stale-z` elbukott (14 mob lépett
     adat nélkül, 14 z-eltérés); a javítással 0.
2. **Egy publikált chunk felülírása azonnal felszabadította a régit:**
   - Út: generáció-reset után az invalid chunk a slotban maradt, és az
     újratöltés publikálása eldobta. Ez tick alatt use-after-free lehetett.
   - Javítás: a `Publish` visszaadja a lecserélt chunkot, és a streamer
     retire-olja; a reset az invalid chunkot is retire-olja.
   - Teszt: `invalid-chunk-generation-reset-retired-then-valid-reload`.
3. **Invalid szomszéd a varrat-ellenőrzésben és a `Vertex()`-ben:** mindkettő
   az invalid chunk üres tömbjét indexelte (tömbön kívüli olvasás, ha egy
   invalid chunk szomszédja betöltődik).
   - Javítás: `TerrainChunk::HasSamples`.
   - Teszt: `neighbour-of-invalid-chunk-loads`.
4. **Az I/O-completion ébresztése elnyelődött:**
   - A supervisor várakozási predikátuma nem tartalmazta a completiont, így
     a `notify` hatástalan volt, és a completion az 5 ms-os tétlen timeoutig
     várt (Windowson kb. 15.6 ms).
   - Javítás: mutex alatti jelző a predikátumban.
   - Eredmény (végső bináris): betöltési p50 14.8 → 5.9 ms (16 MB), illetve 1.6 ms (3 MB).
5. **Kötegelt kezdő spawn szűk budgetnél:**
   - A köteg a budget felét a nyers chunk-méretben mérte, a fix metaadatot
     és az olvasási puffert nem számolta. 3 MB-nál egyetlen köteg sem fért
     be, így 0 mob spawnolt (ERROR log mellett).
   - Javítás: a köteg a szabad budgethez és a valódi foglaláshoz igazodik.
     3 MB-nál most 3400/3400 entitás.
6. **Admission kimerült budgetnél:** minden várakozó igény újra végigpásztázta
   a 9604-es slot-táblát, a supervisor-pass 3.4 ms volt.
   - Javítás: passonként az első sikertelen helycsinálás után megáll.
   - Eredmény: kimerült (3 MB-os) budgetnél a supervisor-pass átlaga 3.38 → 0.52 ms (16 MB-nál 0.41 ms).
7. **Navigáció:**
   - Invalid adat mögötti cél `NoPath`-nak tűnt; most `InvalidData`.
   - A futó jobok száma korlátlan volt; most legfeljebb 256, fölötte
     `Rejected`.

Kisebb pontosítások:
- A mob-spawn invalid adaton külön számlálót kapott, nem „blocked terrain".
- Az `InvalidData` lépés már nem számít `steps_waiting`-nek.
- A worldquery bench a futó világ vizét már snapshoton át kérdezi.

## 10. Viselkedésváltozások és scope-bővítések (review-ra)

**Viselkedés:**
- A mobok nem lépnek blokkolt cellára, és semmilyen lépés nem ugorhat át
  falat. Korábban a mob a blokkolt cellát figyelmen kívül hagyta.
- A cellán belüli lépés hiányzó adaton vár, invalid adaton elutasított.
- Az invalid chunk generáció-resetkor retire-olódik (csak teszt-seam; a
  production csomagcserét nem ismer).
- Az I/O-completion azonnal ébreszti a supervisort (kevesebb várakozás, több
  supervisor-pass betöltési hullámokban).
- A navigáció új státuszai: `Rejected`, valamint `InvalidData` a `NoPath`
  helyett.

**Scope-bővítések:**
- **API:** a `ServerTerrain::Publish` `[[nodiscard]]`, és visszaadja a
  lecserélt chunkot. Új hívások: `BindWriterThread`, `PinCount`,
  `TerrainChunk::HasSamples`; streamer-oldalon `ReservationBytes` és
  `FreeBytes`.
- **Teszt-instrumentáció** a `TerrainStreamingConfig`-ban:
  `poison_freed_for_test` és `unsafe_free_without_quiescence_for_test`.
  Productionben soha nincs beállítva; a konfigból nem érhető el.
- **Új runtime teszt-seam:** `PostTerrainEvictIdleForTest`.
- **Metrika-bővítés:**
  - invalid és permanent/reader pinned darabszám;
  - resident, in-flight, retired és pinned csúcsok;
  - miss-késleltetés;
  - `replaced` és `quarantined` számlálók;
  - `MobSpawnsRefusedInvalidTerrain`.
- **Új bench-mód és kapcsoló:** `worldbench --mode streamlife`; a
  `streamsoak` `--budget-mb` kapcsolót kapott.
- **Előző körből:**
  - `.gitattributes`: `*.sh text eol=lf`;
  - új log-sorok: `Bootstrap resources` és a streaming-összegzés;
  - a megtartási idő legalább 0.1 s, a `terrain_cache_budget_mb` legalább
    0.01.

## 11. Döntést igénylő pontok (MAP-4 előtt)

1. **Futásidejű zónabudget:** a split-úton nincs összesített korlát, a
   supervisor költsége szuperlineáris.
2. **Warp-szemantika — MAP-4:** belépési él, cooldown, cross-zone; a
   blokkolt / invalid célra tickenként írt WARN.
3. **A lejtő- és a mélyvíz-szabály alapértéke:** gameplay-látható, most ki
   van kapcsolva.
4. **Navigáció:** kell-e gameplay-hívó vagy navmesh-réteg.
5. **Manifest-méret:** az 1 MiB kb. 16 ezer chunkot enged; finom cellákhoz
   külön indexfájl kellene.
6. **500p / 7000p readiness a file-backed 100 km-es világon — MAP-4.**
7. **A munkakészlet > budget esetére** nincs éhezés elleni garancia (4.3).
   Kell-e prioritás, például a játékos-igény a mob-igény előtt?
8. **Sanitizer-futás:** kell-e ASan/TSan (clang, Linux/FreeBSD) a MAP-4
   előtt?
9. **Korábbról nyitott:** warp-lánc, kliens és v3, a mob-registry helye, a
   worldlogic jövője, a `scheduler` teszt ingadozása.

## 12. Fájlok

Új fájlok:

| Fájl | Tartalom |
|---|---|
| `shared/map/include/map/ServerWater.h`, `shared/map/src/ServerWater.cpp` | szerver-víz |
| `gameserver/apps/gameserver/src/world/terrain/TerrainStreamer.{h,cpp}` | chunk-streaming |
| `gameserver/apps/gameserver/src/world/terrain/TerrainDemand.h` | zónánkénti igény |
| `gameserver/apps/gameserver/src/world/terrain/NavigationService.{h,cpp}` | navigáció |
| `gameserver/apps/gameserver/src/bench/MapStreamingBench.cpp` | `streaming`, `worldquery`, `streamlife`, `streamsoak` módok |

Érintett meglévő fájlok:
- `shared/map`: `ServerTerrain`, `WorldPackage`, `WorldPackageWriter`,
  `MapData.h`, a séma;
- gameserver: `TerrainService`, `WorldRuntime`, `MovementSystem`,
  `SpawnCoordinator`, `Zone`, `RegionDefinition`, `main.cpp`;
- benchek, a konfig-példa, az indulási teszt, a README és a dokumentáció.

A becsekkolt `test_zone` érintetlen; minden fixture a temp könyvtárba kerül.

## 13. Reprodukálás

A binárisok: `gameserver/build/windows-relwithdebinfo/apps/gameserver/RelWithDebInfo`.

```bash
./worldbench.exe --mode streamlife
```

```bash
./worldbench.exe --mode streaming
```

```bash
./worldbench.exe --mode worldquery
```

```bash
./worldbench.exe --mode streamsoak --cycles 60
```

```bash
./worldbench.exe --mode streamsoak --cycles 60 --budget-mb 3
```

```bash
bash gameserver/scripts/map1_startup_acceptance.sh <out_dir> <gameserver.exe> <worldbench.exe> <database.json> <mob_types.conf> Client/assets/Maps/test_zone
```
