# Teljesítmény-alapvonal terhelt jelenettel – 2026-10-07

A runtime párhuzamosítási terv 0. lépése: egy valós játékhoz hasonlóan terhelt jelenetet a **buildelt
játékkal** (editor nélküli runtime player) mértem, hogy a további lépések hatása ehhez legyen mérhető.

## Mérőeszközök (mindkét buildben, alapból kikapcsolva)

| Kapcsoló | Hatás |
| --- | --- |
| `IX_PERF_LOG=<másodperc>` | Ennyi másodpercenként egy `[PERF]` sor a naplóban: render-loop és **ténylegesen megjelenített** FPS, a CPU-frame átlagos fázisai (frame-várakozás, szimuláció: fizika / szkript / részecske, animáció, render-rögzítés: ebből árnyék, UI, submit), valamint a jelenet terhelése. |
| `IX_GPU_PROFILE=1` | GPU pass-időbélyegek Release-ben is (frame, árnyék, tükröződés, terep, jelenet, utófeldolgozás, UI). 64 frame-enként egy mintát vesz, mert a mintavétel megvárja a GPU-t. |

A megjelenített FPS a sikeres `vkQueuePresentKHR` hívásokat számolja (`IXRHIDevice::GetPresentedFrameCount`),
a present-szálon is. Az aszinkron present mellett ez különbözhet a render-loop FPS-étől. A mérésekben a kettő
egyezett, mert a present-sor legfeljebb két elemű.

## Terhelő jelenet

A `Client/tools/perf/make_stress_scene.js` az AkitaOnline `Main` jelenet másolatából készít terhelt változatot.
A bemeneti fájlt sosem írja felül.

- 62 további animált karakter: 31 Arissa és 31 KicsiK, a játékos körül, 3 m-es rácsban. Ez 64 karakter
  összesen, mert a mérés idején modellenként 32 volt a motor korlátja (lásd lent; azóta javítva). A generátor
  argumentumai: `node make_stress_scene.js <be> <ki> [karakterek=62] [objektumok=10000] [emitterek=100]`.
- 10 000 statikus objektum 400×400 m-en: 30% fa (`ujfa`), 70% doboz.
- 100 részecske-emitter a játékoshoz legközelebbi dobozokon: 50 CPU-s, 50 GPU-s, emitterenként 200 részecske.

A mérési projekt: `Client/build/stress-20261007/project` (a `Scenes/Main` mappában `Main.scene.full`,
`Main.scene.crowd` és `Main.scene.original` változat). Futtatás a projekt mappájából:

```powershell
$env:IX_PERF_LOG = "2"; $env:IX_GPU_PROFILE = "1"
& D:\IxtreemeWorld\Client\build-runtime\apps\client\Release\IxtreemeEngine.exe
```

## Eredmények (Release runtime, 2560×1369 ablak, NVIDIA)

| Változat | FPS (loop / megjelenített) | CPU-frame | Ebből |
| --- | --- | --- | --- |
| Teljes: 10 077 entitás, 64 karakter, 100 emitter | **36 / 36** | **28,0 ms** | render 25,0 ms (ebből **árnyék 15,3 ms**), animáció 2,0 ms, szimuláció 0,87 ms (részecske 0,6, fizika 0,2, szkript 0,05), GPU-várakozás 0,02 ms |
| Csak karakterek és emitterek (177 entitás) | **365 / 364** | 2,7 ms | GPU-várakozás 1,3 ms (GPU-korlátos), render 0,83 ms, animáció 0,33 ms, részecske 0,13 ms |

A teljes változat GPU-ideje (`IX_GPU_PROFILE`): **frame 18–22 ms, ebből árnyék 12–15 ms**, jelenet 5,5 ms,
terep 0,5 ms. A teljes jelenet tehát CPU-korlátos, de a GPU is közel van a határhoz.

## Mit mutat a CPU-mintavétel (teljes változat)

A frame-et nem a párhuzamosítás hiánya fogja vissza, hanem az, hogy minden frame-ben mind a 10 000
entitáson újraszámolódik minden:

| Tétel | A fő szál idejéből | Mi történik |
| --- | --- | --- |
| `TerrainRenderer::RenderSunShadowMap` + árnyékvető-gyűjtés | ~21% + ~10% | minden frame-ben az összes árnyékvető összegyűjtése (8 sarokponttal), kaszkádonkénti hash az összesre, majd kaszkádonként `unordered_map` batch-építés |
| `StaticMeshWorldAabb` | ~17% | a világ-AABB frame-enként újraszámolva (Euler→kvaternió→mátrix), entitásonként többször |
| `resolveMeshRuntimePath` | ~12% | frame-enként, entitásonként egy `std::string` másolat és string-hash keresés |
| `std::vector<std::string>` másolások, heap | ~5% + ~10% | `StaticMeshRenderer::Instance` példányonként lemásolja az anyag-slot stringeket |
| `getStaticMeshRenderer` | ~3% | string-kulcsos map-keresés entitásonként |

A skinning-előfázis és a részecske-frissítés is mind a 10 000 entitáson végigmegy, hogy megtalálja a 64
karaktert és az 50 emittert. Ezért 2,0 ms az animáció a teljes jelenetben, és csak 0,33 ms objektumok nélkül.
A tényleges animáció karakterenként kb. 5 µs.

## Talált hibák (a terhelés hozta elő, nem a mérőkód)

1. **A `StaticMeshRenderer` frame-enként 64 uniform-slotot kezel** (`kUniformSlots`). Az árnyék-pass a
   maszkolt anyagú példányokat (levelek) egyenként, saját slottal rajzolja. Sok fánál a slotok elfogynak, és
   az utolsót használja újra: egy már kötött descriptor set-et ír felül. A Vulkan-validáció ezt több ezer
   hibával jelzi (`... was destroyed or updated without UPDATE_AFTER_BIND`). Ez nem definiált viselkedés:
   rossz transzformációkhoz és összeomláshoz vezethet.
2. **Egy skinned modellből frame-enként legfeljebb 32 példány rajzolódik** (`kSkinSlots`). A többit a motor
   eldobja. MMO-tömegnél ez kemény korlát.
3. Egy összeomlás a swapchain újraépítésekor (`[MAIN] swapchain changed, recreating pipelines`, exit 139),
   a mintavételező futása után. 18 minimalizálás/visszaállítás ciklusban nem reprodukálódott. Az 1. pont
   érvénytelen parancspuffere valószínű ok.

## Javítások és a 3. lépés (render-adatok kinyerése) után – 2026-10-08

Ugyanaz a terhelt jelenet, ugyanaz a gép és ablakméret, buildelt játék:

| Állapot | FPS | CPU-frame | GPU-frame (ebből árnyék) |
| --- | --- | --- | --- |
| Alapvonal | 36 | 28,0 ms (render 25,0, árnyék 15,3, animáció 2,0) | 18–22 ms (12–15) |
| Render-rekordok | 45 | 22,6 ms, ebből 8 ms GPU-várakozás (render 10,9, árnyék 6,0) | 22 ms (15,6) |
| + statikus árnyék-cache | 115 | 8,6 ms (render 5,7, árnyék 1,1) | 7,2 ms (0,6–0,9) |
| + rekord-slotok, egyszeri frustum | **137** | 7,3 ms, ebből 2 ms GPU-várakozás (render 3,3, árnyék 0,8, animáció 1,4) | 7,3 ms (0,7) |

A jelenet most GPU-korlátos: a 7,3 ms-ből 5,5 ms a fő jelenet-pass (a sok fa és doboz rajzolása).
A Vulkan-validáció tiszta (0 bájtos napló) a teljes jeleneten, a 256 karakteres tömegen és az editorban
(Scene nézet szerkesztés közben, Game nézet Play-ben).

Mi változott:

- **Uniform-slot túlcsordulás javítva.** A `StaticMeshRenderer` és a `SkinnedMeshRenderer` descriptor set-jei és
  uniformjai 64/32-es lapokban bővülnek. Egy frame-en belül egyetlen kötött set-et sem ír át semmi; a példány-buffer
  kötése csak akkor frissül, amikor a slotot frissen kiosztja.
- **A 32-es skin-slot korlát megszűnt.** A slotok szintén lapokban jönnek létre, legfeljebb 1024 példány/modell
  frame-enként. 256 karakterrel (modellenként 128) ellenőrizve.
- **Aszinkron present.** A swapchaint az acquire (render szál) és a present szál most zárral éri el. A validáció ezt
  korábban szálkezelési hibaként jelezte.
- **Render-rekordok.** Statikus mesh-entitásonként frame-ek között megmaradó rekord: feloldott renderer,
  világ-AABB, sarokpontok, valamint előre kiszámolt példányblokk (modellmátrix és anyag). Csak akkor épül újra, ha az
  entitás vagy egy anyag-asset megváltozik (`MaterialAssetManager::Revision`). A transzformot minden frame-ben,
  a modellútvonalat és az anyagokat entitásonként 4 frame-enként vizsgálja. A batch-ek példány-mutatókat kapnak,
  nem másolatokat (`StaticMeshRenderer::InstanceList`).
- **Statikus árnyék-cache.** A terep és a nyugvó statikus mesh-ek mélysége külön rétegben marad meg. Ha egy kaszkádban
  mozgó árnyékvető van (karakter, vagy egy mesh, ami az utolsó 30 frame-ben mozgott), a kaszkád ennek a rétegnek a
  másolata, és csak a mozgók rajzolódnak rá. Új RHI-hívás: `CopyTextureLayer`. Plusz 64 MB GPU-memória, csak ha kell.
- **Kisebb tételek.** Egy részecske-emitter id alapján keresi a saját entitását (eddig lineárisan, 50 × 10 000
  frame-enként). A nézet frustuma nézetenként egyszer számolódik. A kiszűrt mesh-ek naplózó halmazai csak a LOD-
  diagnosztikával futnak. Minimalizált ablaknál a fő ciklus 5 ms-ot alszik (eddig 57 000 kört futott másodpercenként).
- **Editor-hierarchia.** A szülő→gyerek és id→elem index egyszer épül fel. A hierarchia-panel eddig O(N²) volt:
  10 000 entitásnál az editor ~5 FPS-ről ~40 FPS-re javult. A maradék fő tétele a frame-enkénti hierarchia-újraépítés.

## Következmény a párhuzamosítási tervre

- A legnagyobb nyereség a **render-adatok kinyerésének** átalakítása (a terv 3. lépése). Kell hozzá
  entitásonként tárolt és csak változáskor frissített render-rekord (feloldott renderer, világ-AABB,
  példányadat, anyag), a statikus árnyékvetők gyorsítótárazása, valamint térbeli indexre épülő culling a fő
  nézetre és a kaszkádokra. Ez a frame-enkénti O(N) munkát a látható és megváltozott elemekre szűkíti, és
  egyben ez az az adatszerkezet, amelyet a job-rendszer később párhuzamosan dolgoz fel.
- A job-rendszer (1. lépés) nyeresége erre épülve jelentkezik. Az animáció önmagában ma 64 karakternél
  ~0,3 ms. 500 karakternél ~2,5 ms lenne, ott a párhuzamosítás már számít.
- A GPU-oldalon az árnyékvetők számát kell csökkenteni: méret szerinti kiszűrés a távoli kaszkádokból,
  árnyék-LOD, statikus árnyékok gyorsítótárazása.
- Az 1. és 2. talált hibát a 3. lépés előtt vagy azzal együtt kell javítani.
