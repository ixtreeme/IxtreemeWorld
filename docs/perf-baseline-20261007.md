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

## Job-rendszer (1. lépés) – 2026-10-08

`libs/jobs` (`IXEngineJobs`): egyetlen közös worker pool az editorban és a buildelt játékban is. Alapból
hardverszál − 1 worker (legfeljebb 31). `IX_JOBS=0`: minden a hívó szálon fut, ugyanazon a hívási úton.
`IX_JOBS_WORKERS=<n>`: a workerek száma. `IX_PARALLEL_CULL=0`: csak a render-előkészítés megy sorosan.

- Fork-join modell: a feladat függvénypointer és adat (nincs allokáció), egy `Counter` számolja a csoportot,
  és a váró szál maga is futtat feladatokat (egymásba ágyazott várakozás is működik). A `ParallelFor`
  chunk-indexet ad, így a chunkonkénti kimenetek soros és párhuzamos módban is ugyanabban a sorrendben fésülődnek.
- A Jolt is ezt a poolt használja (`EngineJoltJobSystem`, `JobSystemWithBarrier` alapon), nem a saját,
  hardverszál − 1 méretű poolját.
- Párhuzamos lett: a render-rekordok ellenőrzése, a Scene nézet jelöltjeinek osztályozása (entitás, rekord,
  frustum), valamint a nagy batch-ek példányblokkjainak kitöltése (a fő nézetben és az árnyékban is).

Terhelt jelenet, 15 worker (16 szálas CPU), a CPU tényleges munkája (frame − GPU-várakozás):

| Mód | CPU-munka | rekordok | render |
| --- | --- | --- | --- |
| `IX_JOBS=0` | 5,45 ms | 1,11 ms | 2,91 ms |
| párhuzamos | **3,78 ms** | 0,29 ms | 2,07 ms |

Az FPS (137) nem változott, mert a jelenet GPU-korlátos. A nyereség CPU-tartalék. 1000 leeső dinamikus
dobozzal a fizika 0,70–0,93 ms helyett 0,58–0,63 ms; az editorban 4 Play/Stop ciklus hibátlanul lefutott.
A Vulkan-validáció tiszta.

## Animáció (2. lépés) – 2026-10-08

A karakterek pózát a skinning-előfázis három lépésben számolja ki:

1. **Soros, entitásonként:** mit játszik a karakter (animátor, klip vagy mozgásállapot), az animátor-kötés és a
   paraméterek, valamint a skin-slotok lefoglalása.
2. **Párhuzamos (`IX_PARALLEL_ANIMATION`):** az animátor kiértékelése, a klip mintavétele, a csontpaletta
   számítása és feltöltése. Szálanként saját ozz-scratch van (`ReserveParallelPalettes`, `PreparePalette`).
3. **Soros:** a dispatchek modellenként egy pipeline-kötéssel, a végén pedig egyetlen közös memória-barrier
   (új RHI-hívás: `BufferMemoryBarrier`) a slotonkénti barrierek helyett. A skinned árnyékvetők is modellenként,
   kötegben rajzolódnak.

256 karakter (128 animátoros Arissa és 128 beépített klipes KicsiK), buildelt játék:

| Mód | FPS | animáció | GPU-frame |
| --- | --- | --- | --- |
| javítások előtt | 180 | 1,09 ms (soros) / 0,85 ms | ~5,5 ms |
| `IX_JOBS=0` | 204 | 0,93 ms | 4,8 ms |
| párhuzamos | **205** | **0,51 ms** | 4,8 ms |

Az FPS-nyereség a közös barrierből jön: a 256 külön barrier sorba rendezte a GPU-n a skinning-dispatcheket.

Javított hiányosságok:

- **Az animátor a buildelt játékban nem futott.** Az AnimatorController és a debug-klip kiértékelése editor-only
  volt, mert a klip- és kontrollerútvonalat csak az editor oldotta fel. A futásidejű `EditorImGui` most ugyanúgy
  feloldja őket a projekt asset-könyvtárából.
- **Az animátor-hozzárendelés nem mentődött a jelenetbe.** Új mező: `animator_controller_id`. A prefabok még
  nem mentik.
- **Szinkronizációs validáció** (`VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`): a mélység- és színattachmentek
  layout-váltása és a render passok külső függősége nem fedte az attachment-olvasást (`LOAD`, mélységteszt), és
  a swapchain-kép acquire-jét sem. Javítva; a terhelt jeleneten és a tömegen is 0 hazárd.

## Aszinkron asset-betöltés (4. lépés) – 2026-10-08

- **Háttérsor a job-rendszerben** (`SubmitBackground`): hosszú munka, amire egyetlen frame sem vár. A workerek
  csak akkor veszik elő, ha nincs frame-feladat, és a `Wait` is csak külön kérésre (`runBackground`) futtatja.
  Workerek nélkül (`IX_JOBS=0`) azonnal lefut a hívó szálon, ez a régi, szinkron viselkedés.
- **A modellek létrehozása két félre vált.** A `LoadCpu` bármely szálon futhat: fájlolvasás, parse és a
  textúrák dekódolása, eszköz nélkül. A `FinishGpu` a render-szálon fut: bufferek, textúrafeltöltés, descriptorok,
  pipeline-ok. A glTF-anyagok regisztrálása (`MaterialAssetManager`, `AssetDatabase`, egyik sem szálbiztos)
  a `FinishGpu`-ba került.
- **A frame-enkénti utak csak kérik a modellt.** Ilyen a render-rekord, a térbeli index és a skinning-előfázis.
  Az első kérés háttérbe küldi a betöltést, és a frame a modell nélkül megy tovább. A frame elején a
  `pumpModelLoads` befejezi a kész betöltéseket, és a modellre váró entitásokat beteszi a térbeli indexbe. Ahol a
  modell azonnal kell (fizikai alak, kijelölés, gizmo, export), a `get…` megvárja, és közben maga is tölt.
- **Jelenetbetöltéskor a térbeli index újraépítése indítja el az összes modellt.** Ezek a terep-palettával egy
  időben töltődnek, és az `ApplySceneData` végén (`finishSceneModelLoads`) mind elkészül, így az első frame-ről
  egy modell sem hiányzik.
- **A script-spawn nem parse-olja a glTF-et a fő szálon.** Egy 15 MB-os modellnél ez eddig 18 ms volt. Ha a
  cache már ismeri a modellt, onnan dönti el, hogy riggelt-e. Ha nem, statikusként indul, a betöltés jelzi a
  rigget, és a skinning-előfázis meglévő önjavítása skinneddé teszi.
- **Egyéb indulási munka:**
  - A terep-paletta képei és mip-láncai rétegenként párhuzamosan készülnek, sRGB↔lineáris táblákkal.
  - A jelenet `mesh_entity` elemei párhuzamosan dolgozódnak fel, a fájlbeli sorrendben.
  - A hang aszinkron dekódol: a zene és a hurkolt klip streamel.
  - A betöltési lépések mérése logba kerül (`[SCENE] load/apply/models`, modellenként betöltés és befejezés).

Indulás, terhelt jelenet, buildelt játék (a legnagyobb frame az, amelyben a jelenet betöltődik):

| Állapot | legnagyobb frame | jelenet alkalmazása |
| --- | --- | --- |
| a 4. lépés előtt | 2548 ms | – |
| `IX_JOBS=0` (soros, a mostani kóddal) | 1606 ms | 1299 ms |
| párhuzamos | **868–878 ms** | 576–585 ms |

A párhuzamos indulás bontása:

- Jelenet-JSON: ~305 ms, ebből a parse 239 ms, soros.
- Alkalmazás: a paletta 365–382 ms, a víz 78 ms, az anyagslotok 39 ms, a térbeli index 25 ms. A modellekre a
  végén már csak 54–61 ms-ot kell várni: az 5 modell a többi lépéssel párhuzamosan, ~540 ms alatt töltődik be.

Játék közbeni spawn: a jelenetben még nem szereplő modellt (a KicsiK 15 MB-os másolata) tölt be egy Lua-script, és a
2 másodperces ablak legnagyobb frame-jét mérem:

| Mód | legnagyobb frame |
| --- | --- |
| `IX_JOBS=0` (szinkron betöltés) | 228 ms |
| párhuzamos | 71 ms |
| párhuzamos, a spawn nem parse-ol | **54 ms** |

A modell ~150 ms alatt töltődik be: 127 ms a betöltő szálon, 21–31 ms a fő szálon (`FinishGpu`). A maradék spawn-költség
nem asset-betöltés:

- Minden spawn és destroy után a teljes fizikai világ újraépül (`rebuildEditorPhysicsWorld`): ~37 ms a 10 000
  entitásos jeleneten.
- Az első spawn átméretezi a 10 000 elemű entitásvektort: ~8 ms, egyszeri.

Ellenőrzés:

- A szinkronizációs validáció 0 hibát jelzett, beleértve a játék közbeni betöltést.
- Ha betöltés közben zárják be az ablakot, a kilépés megvárja a betöltést és tisztán lezárul (exit code 0).
- Editorban a projektmegnyitás, a Play, a spawn és a Stop hibátlanul lefutott.

Hátravan:

- A `FinishGpu` a fő szálon fut: skinned modellnél 21–38 ms (bufferek, compute-skin ellenőrzés, textúrák,
  pipeline-ok), statikusnál 6–13 ms. Aszinkron GPU-feltöltés (transfer queue) és pipeline-megosztás csökkentené.
- A jelenet-JSON parse és a paletta-dekódolás a legnagyobb indulási tétel.

## Részecskék (5. lépés) – 2026-10-08

- **Emitterenként egy job.** A szimulátor gyűjtése soros: új emitterek indulnak, a megszűntek kiesnek. Utána
  minden emitter külön feladatként lép (`IX_PARALLEL_PARTICLES`). Mindegyiknek saját szimulátora és saját
  véletlen-sorozata van (az entitás-id a seed), a talaj-lekérdezés pedig csak olvassa a terepet, így az eredmény
  a futás sorrendjétől független. 4096 részecske alatt sorosan fut, mert ott a szétosztás többe kerülne.
- **A CPU-részecskék példányadata frame-enként egyszer készül.** Eddig minden nézet és pass újra felépítette és
  feltöltötte. Mivel világtérben vannak, minden nézet ugyanazt használhatja: emitterenként párhuzamosan,
  közvetlenül a frame példány-bufferébe íródnak, és minden nézet a saját emittereinek tartományát rajzolja
  (`ParticleRenderer::BeginCpuInstances`, `Batch::instanceBase/instanceCount`).
  - Új RHI-hívás: `IXRHIBuffer::HostAddress()`. Ez egy CPU-ról írható buffer állandó leképezése; dokumentáltan
    több szál is írhat rajta keresztül egyszerre, egymástól független bájtokra.
- **A példány-buffer nő.** Eddig frame-enként legfeljebb 16 384 CPU-részecske rajzolódott ki, a többi elveszett.
  Most a buffer frame-enként és szükség szerint duplázódik, legfeljebb 1 048 576 részecskéig.
- **Az emitterek listája egyszer készül.** A szimuláció, a GPU-emitterek és a nézetek eddig mind a 10 000
  entitást végignézték; most egyszer gyűjtődik ki az emitterek listája.

Részecske-terhelt változat (`Main.scene.particles`: 101 CPU-emitter, ~100 000 részecske), buildelt játék:

| Mód | szimuláció | példányadat | CPU-munka (frame − GPU-várakozás) | FPS | kirajzolt részecske |
| --- | --- | --- | --- | --- | --- |
| az 5. lépés előtt | 2,44 ms | (a renderben) | ~7,7 ms | 127–131 | 16 384 |
| `IX_JOBS=0` | 2,45 ms | 0,71 ms | ~9,5 ms* | 105* | ~100 000 |
| `IX_PARALLEL_PARTICLES=0` | 2,46 ms | 0,72 ms | ~7,0 ms | 134 | ~100 000 |
| párhuzamos | **0,44–0,48 ms** | **0,48–0,54 ms** | **4,2–4,8 ms** | 121–133 (GPU-korlátos) | ~100 000 |

\* Ez egy későbbi mérésből van, amikor a gép lassabb volt: ugyanakkor a párhuzamos mód 4,8 ms és 121 FPS volt
(korábban 4,2 ms és 133 FPS).

A terhelt alapjelenetben (50 CPU-emitter, ~5000 részecske) a szimuláció 0,20 ms helyett 0,12 ms, a példányadat
~0,1 ms; 145 FPS, visszaesés nincs. A szinkronizációs validáció 0 hibát jelzett, beleértve a növekvő buffert is.
A buildelt játékban és az editorban (Edit-előnézet és Play) is helyesen rajzolódnak a részecskék. Az editor
frame-jében a részecskék ~1,1 ms-ot tesznek ki; a 25 ms-os frame fő tételei a 10 000 entitásos hierarchia
és a UI.

A tervből kimaradt: a Scene és Game transparent queue szálankénti gyűjtése és összefésülése. A gyűjtés most
emitterenként csak néhány mező, a meglévő `stable_sort` megmarad, így ez nem mérhető tétel.

## Spawn és destroy: a fizika és az árnyék – 2026-10-08

- **A fizikai világ testenként változik.** A script `SpawnMesh`/`DestroyEntity` után eddig a teljes Jolt-világ
  újraépült (~37 ms 10 000 entitásnál), és minden test elvesztette a sebességét, a kontaktusok pedig újraindultak
  (`CollisionEnter` minden spawn után). Most a spawnolt entitás teste hozzáadódik (`createPhysicsBodyForEntity`,
  ugyanaz, amiből a teljes építés is dolgozik), a törölté pedig kikerül. A teljes újraépítés csak akkor marad, ha
  az entitás jointban van (a joint a testeket fogja), vagy ha még nincs világ.
- **A törlések egyszerre hagyják el az entitáslistát** a drain végén, egy menetben. Eddig minden destroy külön
  keresett és külön törölt a 10 000 elemű listából, és utána mindig újraépült az id-index.
- **Egy új anyag betöltése nem rajzoltatja újra az összes árnyékot.** A spawnolt fa két, addig nem töltött anyagot
  hozott. A `MaterialAssetManager` revíziója ettől lépett, minden render-rekord újra-előkészült és „változottnak”
  számított, így 30 frame-en át mind a ~10 000 árnyékvető mozgóként rajzolódott (2 másodpercig 63–75 FPS). Két
  javítás:
  - Az első betöltés nem lépteti a revíziót; a mentés, a generált anyag mentése és az eldobás továbbra is igen.
  - A `PrepareInstance` megmondja, változott-e ténylegesen az előkészített adat; csak akkor számít a rekord
    változottnak.

Spawn-teszt (`Main.scene.spawntest`, buildelt játék), a 2 másodperces ablak legnagyobb frame-je:

| Esemény | előtte | most |
| --- | --- | --- |
| még be nem töltött riggelt modell spawnja | 54 ms | 33–35 ms |
| betöltött statikus modell (fa) spawnja | 45 ms, utána 2 s-ig 63 FPS | 26–28 ms, utána 145 FPS |
| a riggelt modell törlése | 42 ms | 8–18 ms |
| a fa törlése | 49 ms | 27 ms |

Ellenőrzés: a jointos kocka törlése teljes újraépítést kér (helyes). A dinamikus kocka törlése csak a saját
testét veszi ki, a többi tovább mozog. Az 1000 eső dobozos jelenet 1005 testtel épül fel. A szinkronizációs
validáció 0 hibát jelzett.

Ami maradt (nem fizika):

- A még be nem töltött riggelt modell `FinishGpu`-ja a fő szálon 21–31 ms.
- A statikus árnyékvető megjelenése vagy eltűnése mind a 4 kaszkád statikus cache-ét újrarajzolja (~20 ms egy
  frame-ben). Kaszkádonkénti revízió vagy több frame-re elosztott újrarajzolás segítene.
- Az editorban Play közben az Inspector változásai el vannak dobva, így ott csak a Play indítása épít világot.

## Statikus árnyék-cache: csak a változás rajzolódik – 2026-10-08

Ha egy statikus árnyékvető megjelent vagy eltűnt, eddig minden kaszkád statikus rétege teljesen újrarajzolódott. A
terhelt jelenetben ez egy frame-ben ~20,5 ms GPU és ~4,5 ms CPU volt; minden frame-re kényszerítve 37 FPS-t adott.
A fő tétel a 3004 fa egyenként 6472 háromszöggel. Most a réteg csak a változást kapja meg:

- **A belépő árnyékvetők** (letelepedett vagy új) a meglévő rétegre rajzolódnak rá. A mélység csak közelebb
  kerülhet, így az eredmény ugyanaz, mint egy teljes újrarajzolásé.
- **A kilépő árnyékvetők** (elmozdult, eltűnt, rejtett) helyén a réteg egy texelnyi ráhagyással töröl
  (`ClearDepth` téglalapra), majd scissorral újrarajzolja a terepet és az ott lévő statikus árnyékvetőket.
  - A téglalapot a régi befoglaló dobozból számolja (amivel az árnyékvetőt a cache-be rajzolta), a kaszkád
    vetítésével.
  - Az ott lévő árnyékvetőket a kaszkád-vetítésben vett téglalapjuk alapján választja ki. Ezt kaszkádonként
    frame-enként egyszer számolja ki, és a régiók abból választanak.
- **Teljes újrarajzolás csak akkor van,** ha a kaszkád elmozdult, a terep változott, vagy a változás nem
  ismert. Ugyanígy, ha 16-nál több régió kellene, vagy a régiók a réteg negyedénél többet fednek.
- **A változás a render-rekordban követődik:** `inStaticShadow` és a rajzolás kori befoglaló doboz. Egy
  kaszkád, amelyben nincs mozgó árnyékvető, most szintén a statikus réteg másolata.
  - Új render target a megtartott rétegre (`m_staticShadowLoadTargets`).
  - A `ShadowCasters` új mezői: `staticDeltaFrom`, `staticJoined`, `staticLeft`, `drawStaticJoined`,
    `drawStaticRegion`.

Spawn-teszt, buildelt játék, a 2 másodperces ablak legnagyobb frame-je (ezen a gépen ~10 ms akkor is, ha semmi
nem történik):

| Esemény | az árnyékjavítás előtt | most |
| --- | --- | --- |
| betöltött fa spawnja | 26–28 ms | 8–10 ms |
| a fa törlése | 27 ms | 8–10 ms |
| 12 fa törlése egy frame-ben (erős nap): az árnyékpass CPU-ja | – | 12,4 ms régiónkénti szűréssel, 5,0 ms a téglalap-cache-sel |

Ellenőrzés:

- **Képi:** erős napfényes változaton 12 közeli fa törlése előtt és után. Az árnyékuk eltűnt, a többi árnyék
  ép, téglalap alakú lyuk vagy perem nincs.
- **Validáció:** a szinkronizációs validáció 0 hibát jelzett.
- **A/B az előző commit ellen:** ugyanaz az FPS. Az árnyékpass CPU-ja 0,69 helyett 0,78 ms, a rekordonkénti
  könyvelés miatt (az új jelzőt a sűrűn olvasott mezők mellé tettem, így 0,88 ms-ról jött le).

### Mozgó kamera: a statikus réteg eltolódik

A kaszkádok a fény rögzített nézetéből, a saját texelrácsukhoz illesztve követik a kamerát. Ha egy kaszkád csak
egész texelnyit mozdult, és a mélységtartománya ugyanaz, a statikus rétege továbbra is helyes, csak eltolva. Ezért
a réteg nem rajzolódik újra:

- A tartalma egész texelekkel eltolódik: a kaszkád saját árnyéktérkép-rétegén át másolódik vissza új helyre
  (új RHI-hívás: `CopyTextureLayerRegion`).
- A szabaddá vált szélsávok a régiók közé kerülnek, és ugyanúgy újrarajzolódnak, mint a kilépő árnyékvetők helyei
  (`StaticShadowShift`, `StaticShadowRegions`).
- A mélységtartomány közepe 64 méteres lépésekben mozog (eddig 1 méteresekben), a ráhagyás ugyanennyivel nőtt. Így
  mozgás közben ritkán kell teljes újrarajzolás.
- A terep árnyék-biasa méterben a régi maradt: a `Terrain.hlsl` a mátrixból olvasott mélységskálával kivonja a
  ráhagyás részét.
- `IX_SHADOW_SHIFT=0`: a mozdult kaszkád egészben rajzolódik újra, mint eddig; összehasonlításhoz.

Erős napfényes változat, buildelt játék, 3 másodperc gyaloglás (W):

| Mód | FPS gyaloglás közben | árnyékpass CPU | GPU-várakozás |
| --- | --- | --- | --- |
| `IX_SHADOW_SHIFT=0` (teljes újrarajzolás) | 73–90 | 2,1–3,2 ms | 5,4–6,9 ms |
| eltolással | **113–124** | ~1,55 ms | 3,1–3,7 ms |
| állva (összevetésként) | 132–135 | ~0,78 ms | ~3,4 ms |

Az alap terhelt jeleneten a teljes újrarajzolással mért gyaloglás korábban 40–58 FPS volt.

Ellenőrzés:

- **Képi:** ugyanaz a gyaloglás eltolással és nélküle ugyanazt az árnyékképet adja. A kezdőpozícióban az árnyékok
  a módosítás előtti képpel azonosak (a terep biasa nem változott).
- **Validáció:** a szinkronizációs validáció gyaloglás közben 0 hibát jelzett. Az első futás kimutatta, hogy a fő
  árnyéktérképről hiányzott a `TransferSrc`, a statikus cache-ről a `TransferDst` használat; ez javítva.

Ami maradt: a sávok újrarajzolása gyaloglás közben kb. 0,8 ms CPU, mert a sávokba lógó fák egészben rajzolódnak. A
mélységlépés átlépésekor (64 m a fény irányában) továbbra is teljes újrarajzolás van. A volumetrikus fény biasa
mélységegységben maradt, így ott kissé nagyobb lett.

## Riggelt modell befejezése a fő szálon – 2026-10-08

A riggelt modell betöltésének GPU-s befejezése (`SkinnedMeshRenderer::FinishGpu`) játék közben 21–38 ms volt a fő
szálon. A lépések mérése (a még be nem töltött KicsiK-másolat, játék közben):

| Lépés | Idő |
| --- | --- |
| a skin-slotok bufferei (egy lapon 2 frame × 32 slot × 2 buffer = 128 külön memóriafoglalás) | 9–20 ms |
| index-buffer feltöltése (a szinkron másolás `vkQueueWaitIdle`-lel kivárja a repülő frame-eket) | 11 ms |
| textúrák feltöltése (ugyanígy szinkron) | 2–5,5 ms |
| `VerifyComputeSkin` (CPU-skin, GPU-dispatch, kivárás, visszaolvasás, összevetés) | 3 ms |
| pipeline-ok | 0,3–0,7 ms (5 ms csak a legelső alkalommal) |

Javítások:

- **A slot bufferei első használatkor készülnek** (`EnsureSkinSlot`). Egy példány 4 buffert foglal, nem 128-at.
  Ez memóriát is spórol: a 15 000 csúcsos modell kimeneti bufferei slotonként ~1 MB-osak.
- **A feltöltéseket az első frame rögzíti, amely skinneli a modellt.** Az index- és nyugalmi-csúcs buffer,
  valamint a textúrák üresen készülnek, az adat host-látható staging bufferbe kerül. A modell első
  dispatch-e előtt a frame saját parancslistája másolja be (`RecordPendingUploads`, sorrendben a rajzolások
  előtt); a staging akkor szabadul fel, amikor az a frame lefutott. Így nincs CPU-oldali várakozás és nincs
  külön submit.
- **A `VerifyComputeSkin` opcionális lett** (`IX_SKIN_VERIFY=1`): akkor a régi, szinkron út fut, és minden
  modellnél ellenőriz.
- **Javított hiba (a külön szálas present óta élt):** az `ExecuteAndWait` és az aszinkron buffer-feltöltés úgy
  hívta a `vkQueueSubmit`-et, hogy nem fogta a queue-mutexet, amelyet a present szál a `vkQueuePresentKHR`
  körül tart. Ha a grafikus és a present queue ugyanaz a `VkQueue`, ez tiltott, egyidejű hozzáférés volt.

Eredmény (buildelt játék):

| Modell | befejezés előtte | most |
| --- | --- | --- |
| KicsiK-másolat, játék közben | 30,9 ms | 4,4–4,6 ms |
| Arissa, indulás | 20,6 ms | 2,3–3,4 ms |
| KicsiK, indulás (az első pipeline-ok miatt) | 37,5 ms | 11,8–12,1 ms |

A még be nem töltött riggelt modell spawnjának ablakában a legnagyobb frame 33–37 ms helyett 17–20 ms. A maradékot
a spawn saját költsége adja (az első spawnnál a 10 000 elemű entitásvektor egyszeri növelése, asset-keresés).

Ellenőrzés:

- A karakterek textúrázva, helyesen rajzolódnak.
- A szinkronizációs validáció 0 hibát jelzett.
- `IX_SKIN_VERIFY=1` mellett minden csúcs a tűrésen belül van (15 481/15 481 és 6498/6498).

### Statikus modellek ugyanígy

A statikus modellek `FinishGpu`-ja is szinkron töltötte fel a vertex- és index-buffert és a három textúrát. A
statikus modell csak render passokon belül rajzolódik, ahol másolni nem lehet. Ezért:

- **A feltöltéseket az alkalmazás rögzíti** minden frame-ben egy fix ponton: a GPU-részecskék szimulációja után,
  a passokon kívül, az árnyékpass előtt. A lista a befejezett, még fel nem töltött rendererekből áll
  (`staticMeshUploads`, `StaticMeshRenderer::RecordPendingUploads`).
- **Védelem:** ha egy modell ezen a ponton túl készül el (blokkoló lekérés frame közben), addig nem rajzol
  (`UploadsRecorded`), amíg a következő frame be nem másolja az adatát.
- **Opcionális:** `FinishGpu(rhi, deferUploads)`; a `Create` továbbra is szinkron.

| Modell | befejezés előtte | most |
| --- | --- | --- |
| a fa másolata, játék közben | 5,7 ms (első import: 24,5 ms) | 4,0 ms |
| a fa, induláskor | 5,9–9,3 ms | 4,0 ms |

Ellenőrzés: a fák, kockák és árnyékaik a korábbi képpel egyezően rajzolódnak, a buildelt játékban és az editorban
(Play) is. A szinkronizációs validáció 0 hibát jelzett.

### Anyag-textúrák

Az anyagok textúrái (`EnsureMaterialTexture`) eddig az első rajzoláskor töltődtek be, a render passon belül: a fő
szálon dekódolták a képfájlt, és szinkron töltötték fel.

- **Most a betöltés aszinkron:** az első kérés csak elindítja a dekódolást egy betöltő szálon, és addig a modell
  saját textúrái helyettesítik az anyagét.
- **A kész textúrák a feltöltési ponton kerülnek a GPU-ra.** A modellek feltöltési pontján (passokon kívül) jön
  létre az objektum és rögzítődik a másolás, így a frame rajzolásai már használhatják. A staging akkor szabadul
  fel, amikor az a frame lefutott.
- Az alkalmazás ezt minden frame-ben minden betöltött statikus modellre meghívja, nem egy listára.
- Az `UploadTexture` már nem másolja le a képet a memóriában.

A betöltött modell (új anyagokkal) spawnjának ablakában a legnagyobb frame 8,8 ms, a jelenet szokásos szintje. A fák
textúrázva rajzolódnak, a szinkronizációs validáció 0 hibát jelzett.

## Árnyék-LOD – 2026-10-08

A távoli kaszkádok texele nagy: az 50 m-ig tartóé 4,9 cm, a 200 m-ig tartóé 19,6 cm. Ott a modell minden háromszöge fölösleges.

A modell betöltésekor, a betöltő szálon készülnek az árnyék részletességi szintjei (`BuildShadowLods`):

- **Opak részhálók:** a varratoknál összevarrva (a mélységpass csak pozíciót olvas), meshoptimizerrel egyszerűsítve.
  - 3 szint, 5 / 16 / 64 mm hibán belül (modellegységben), mindegyik az előzőből.
  - Egy szint csak akkor marad meg, ha legalább negyedét leveszi az előzőnek; különben az előző szint ismétlődik.
- **Maszkolt levélkártyák:** egy maszkolt részháló, amely sok kis különálló darabból áll (legalább 64 darab,
  darabonként legfeljebb 16 háromszög), csak a legdurvább szinten egyszerűsödik. Ilyenkor:
  - a darabok negyede rajzolódik;
  - mindegyik a középpontja körül nagyobb lesz, hogy a kimaradtak területének 70%-át fedje
    (`kShadowCardCover`). A nagy kártyák kevésbé fedik egymást, mint a sok kicsi, ezért kell a 70%.
  - Ellenőrzés közelre kényszerítve: 100%-os fedéssel a talaj átlagfényessége 7%-kal sötétebb lett, 70%-kal az
    alsó képfél átlaga egyezik (343,4 mindkettőn).
- **Szintválasztás példányonként:** a kaszkád a legdurvább olyan szintet rajzolja, amelynek hibája a példány
  legnagyobb skálájával szorozva a texel felén belül marad. 1,5-ös skáláig az 50 m-es kaszkád a 2. szintet, a 200 m-es
  kaszkád a 3. szintet és a ritkított kártyákat rajzolja.
- **GPU-adat:** a szintek saját index-bufferbe kerülnek, a kártyák saját vertex-bufferbe. A halasztott feltöltéssel
  együtt másolódnak be.
- **Kikapcsolás:** `IX_SHADOW_LOD=0`, összehasonlításhoz.

A fa (`ujfa.glb`) szintjei:

| Rész | Eredeti | 1. szint | 2. szint | 3. szint |
| --- | --- | --- | --- | --- |
| kéreg | 4456 háromszög | 4456 | 1187 | 181 |
| levelek | 2016 háromszög | 2016 | 2016 | 504 |

A szintek elkészítése 3,4 ms a betöltő szálon.

Mérés: buildelt játék, a statikus réteg minden kaszkádban minden frame-ben egészben újrarajzolva (ideiglenes
mérőkapcsolóval, amely nem került be a kódba):

| Jelenet | LOD nélkül | LOD-dal |
| --- | --- | --- |
| alap terhelt: árnyék GPU | 21,8–23,5 ms (34 FPS) | 17,5–19,0 ms (41 FPS) |
| alap terhelt: 200 m-es kaszkád | 5,7–6,8 ms | 1,2–1,8 ms |
| alap terhelt: 50 m-es kaszkád | 3,7 ms | 3,4 ms |
| erős nap: árnyék GPU | 11,3–13,7 ms (53 FPS) | 7,4–8,0 ms (70 FPS) |
| erős nap: 200 m-es kaszkád | 5,1–5,7 ms | 1,1 ms |

Állóképben nincs különbség (128–131 FPS mindkét módban), mert a statikus réteg ott a cache-ből jön. A nyereség a
teljes újrarajzoláskor (mélységlépés, terepváltozás), a sávok és régiók újrarajzolásakor és a mozgó árnyékvetőknél
jelentkezik.

Ellenőrzés:

- **Képi:** a távoli domboldal árnyékai LOD-dal és nélküle azonosak (átlagos eltérés 3,5 a 765-ös skálán, főleg a
  mozgó karakterek és a részecskék miatt).
- **Validáció:** a szinkronizációs validáció 0 hibát jelzett az alap jeleneten és a játék közbeni fa-spawnnál
  (halasztott LOD-feltöltés).
- **Editor:** a Scene nézet és a Play rendben rajzol. A ctest 5/5.

Ami maradt: az első két kaszkád (5 m és 15 m) teljes újrarajzolása 6,5 ms és 5 ms. Ezt a maszkolt levelek kitöltése
viszi (közelről nagy a fedett terület), nem a háromszögszám, ezért itt a LOD nem segít.

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

## 2026-10-08: fő pass, árnyékstabilitás és ABI 7

A mérések standalone **Release** runtime-on, az elkülönített stresszprojekt másolatán készültek.
A validáció külön **Debug**, `IX_VALIDATION=1`, `IX_JOBS=0`, Vulkan synchronization validation
beállítással futott; annak FPS-e nem teljesítményeredmény. Az async present maradt az alapértelmezés.
Az első három kétmásodperces GPU-mérési ablakot kihagytuk, a további GPU-időket mintaszámmal súlyoztuk.
A megjelenített FPS és a Scene GPU ideje külön mérőszám.

### Fő pass lebontása fix kamerával

Az ideiglenes kapcsolók csak egy-egy rajzolási kategóriát hagytak ki; a jelenetet és az árnyékpass
tagságát nem változtatták. A kihagyások eredménye nem összeadható: az opak geometria az utána
rajzolt felületek kitakarását is befolyásolja.

| Fő pass változat | Átlag FPS | Scene GPU, ms |
| --- | ---: | ---: |
| Teljes jelenet, 1. kontroll | 137,5 | 5,543 |
| Teljes jelenet, 2. kontroll | 132,2 | 5,795 |
| Teljes jelenet, 3. kontroll | 132,2 | 5,760 |
| Opak statikus nélkül | 136,3 | 5,354 |
| Maszkolt statikus nélkül | 197,0 | 3,258 |
| Skinnelt nélkül | 132,0 | 5,691 |
| Részecskék nélkül | 132,3 | 5,638 |
| Átlátszó nélkül | 131,8 | 5,707 |

A maszkolt levelek dominálnak: kihagyásuk körülbelül **2,3–2,5 ms**-ot vesz le a Scene passból.
A többi kategória nyeresége ezen a terhelésen kicsi, illetve a futások szórásába esik.
A kapcsolók és a mérőkód eltávolítva; a logok a git által figyelmen kívül hagyott
`Client/build/followup-audit-20261008/category-*` könyvtárakban maradtak.

### Kipróbált, elvetett változatok

**Maszkolt depth prepass:** a levelek alpha-tested mélységpassa után EQUAL mélységteszttel,
early depth/stencil színpass következett. Két A/B párban:

| Futás | Scene GPU, prepass nélkül / vele | FPS, prepass nélkül / vele |
| --- | --- | --- |
| 1 | 5,599 / 5,523 ms | 133,5 / 134,7 |
| 2 | 5,616 / 5,616 ms | 135,3 / 135,7 |

Nincs ismételhető érdemi nyereség; a kód, shader-változatok és SPV-k eltávolítva.
Csak a `tested-prepass-*` futások érvényesek. A korábbi `prepass-*` könyvtárak sikertelen
fordítás utáni régi bináris futását tartalmazzák, azokból nem következtetünk.

**Fák főnézeti LOD-ja:** 3004 fán próbáltuk a meglévő rendszerrel, kizárólag jelenetmásolaton
(30/65/130 m, 0,65/0,35/0,15 arány). Az első párban 5,697 → 5,155 ms, a másodikban
5,655 → 6,320 ms Scene GPU-idő adódott. Azonos kamera mellett a pixelek 19,47%-án 8/255-nél
nagyobb eltérés jelentkezett. A bizonytalan teljesítmény és a látható részletvesztés miatt elvetve.
Az eredeti jelenet és a modellek nem kaptak LOD-tartalmi módosítást. Vetített méret szerinti
főnézeti tárgykihagyás sem került be, mert képi változás nélkül nincs rá mért elfogadható jelölt.

### Árnyék-cache: stabil vetítési együtthatók

Gyaloglás közben a régi ortografikus mátrix a mozgó bal/jobb határok kivonásából számította a
skálát. Lebegőpontos kerekítés miatt a skála néhány ULP-vel változott. A cache ezt valódi
vetítésváltozásnak látta, így az első két kaszkád gyakran teljesen újrarajzolódott.

A `MakeSunShadowProjection` közvetlenül a rögzített félméretből számítja a skálát és egész
texeles eltolásból a transzlációt. A mélységtartomány és a felbontás megmarad. A teszt a
mélységtartomány két végét, a skála bitpontos állandóságát és az egész texeles eltolást ellenőrzi.

Determinista, vízszintes 6 m/s kameramozgás, öt másodperc után indulva, 36 másodperces futások:

| Változat | FPS | GPU frame, ms | Árnyék GPU, ms | Legnagyobb CPU frame a mérési szakaszban, ms |
| --- | ---: | ---: | ---: | ---: |
| Régi vetítés | 150,7 | 6,493 | 2,099 | 23,534 |
| Régi vetítés, ismétlés | 148,6 | 6,716 | 2,257 | 24,933 |
| Stabil vetítés | 163,8 | 5,889 | 1,504 | 22,210 |
| Stabil vetítés, ismétlés | 160,2 | 5,929 | 1,410 | 22,944 |
| Stabil vetítés + eltolt mélységfázis | 158,0 | 6,068 | 1,464 | 23,775 |

A stabil vetítés **8–9% FPS-nyereséget** adott gyaloglás közben. A teljes statikus újrarajzolások
száma a bemelegítés után az első két kaszkádon 828/1885, illetve 824/1867 helyett 1/1 lett;
a mélységlépés tényleges átlépése továbbra is teljes újrarajzolást igényel.
A futások időalapú kamerautat használnak, ezért a képkockaszámuk különböző.

A kaszkád × 16 m mélységfázis az `IX_SHADOW_DEPTH_STAGGER=1` kapcsolóval próbálható;
**alapból kikapcsolva**. Megszüntette a négy egyidejű mélységfrissítést (1/2/4/8 maszkok),
de a legrosszabb CPU-frame és az átlag FPS nem javult a stabil vetítéshez képest.
Ezért az eredeti elfogadási feltételt nem tekintjük teljesítettnek. Közeli kaszkád felbontása
és árnyékvetőinek száma nem csökkent.

A statikus képi ellenőrzésből a mozgó karaktereket, részecskéket és scripteket eltávolítottuk
egy másolaton. Ugyanabból a kamerából a stabil és a régi vetítés átlagos csatornaeltérése
0,374/255; a pixelek 0,286%-án nagyobb 8/255-nél. A különbségkép apró árnyék- és levéléleket
mutat; nincs kihagyott objektum vagy új részletcsökkentés. Ez numerikus képi eltérés,
nem bitazonos eredmény. Az ellenőrizhető képek: `stable-control.ppm`, `stable-on.ppm`,
`stable-on-preview.png`, `stable-image-diff.png` a mérési könyvtárban.

### Volumetrikus bias és első képkocka

A `VolumetricLight.hlsl` a Terrain shaderrel azonos, VP z-oszlopából számolt méterarányos
korrekcióval kezeli a megnövelt árnyékmélység-ráhagyást. Mindkét raymarch út korrigálva,
a tárolt `volumetric_light_ps.spv` újragenerálva. A volumetrikus és screen-space út is
futott a szinkron validációban.

A betöltött editor első képkockáján a víz inicializálatlan ReflectionColor és ColorSnapshot
textúrát kapott. A snapshot getter csak olvasható layoutban ad vissza textúrát; a reflection
descriptor az első elkészült passig a meglévő fallbacket kapja. Javítás után a betöltött
Scene és a Game/Play útvonalon sem maradt layout- vagy szinkronizációs hiba.

### Script és prefab: a felhasználó által jóváhagyott ABI 7

- `RaycastFiltered`: rögzített layer mapping, trigger-szűrés, ignore entity; a régi `Raycast` megmarad.
- `OverlapSphere`: stabil entity-id sorrend, egyesített duplikátumok, caller-owned tömb,
  legfeljebb 1024 kimenet, `truncated` jelzés. A korlát a kimenetre vonatkozik; sűrű
  világban a teljes fizikai keresés továbbra is költséges lehet.
- `SetEntityEnabled`: deferred commit, örökölt tiltás a lokális flag megtartásával;
  render/árnyék, fizika, script, animáció, audio és CPU/GPU particle együtt szünetel.
  A Jolt RemoveBody nullázza a sebességet, ezért azt külön snapshot őrzi.
  Visszakapcsoláskor a tiltás alatt szerkesztett transform is átkerül a fizikai testre.
- `SpawnPrefab`: mesh-root, mesh/point/spot támogatás, parent/joint id-remap, közös
  publikálás az OnStart előtt, spatial/fizika/árnyék integráció. Hibás hierarchia nem
  publikálható részlegesen. Kamerás vagy nem mesh-root prefab jelenleg nem támogatott.
- A prefab most menti/betölti az `animator_controller_id` és lokális `enabled` mezőt.
- Editorban a script által törölt szülő megmaradó gyermekeit leválasztjuk a Flecs
  tükörből; így a nem rekurzív `DestroyEntity` nem törli őket mellékhatásként.

A kanonikus és szállított SDK-headerek azonosak. **Az ABI 6 DLL-eket újra kell fordítani**;
az eltérő verzió betöltését a motor továbbra is elutasítja. Részletes aláírások és bindingok:
`Client/sdk/README.md`, elfogadott szerződés: `docs/script-query-abi-proposal-20261008.md`.

Az új PrefabRuntimeTest és ScriptQueryTest ellenőrzi a hierarchia/ID-remap/szerializálás
hibáit, a valós Jolt lekérdezéseket és szüneteltetést, a Lua és AngelScript bindingokat,
az ABI 6 elutasítását, valamint a stabil árnyékvetítést. A runtime és editor Play tesztjelenet
a deferred spawn/query/tiltás/visszakapcsolás/törlés teljes útját is bejárja; egy OnStart
és egy OnDestroy történik, és a tiltás alatt módosított pozíció helyesen visszatér a fizikába.

A későbbi job-fázisokhoz ezen a jeleneten nem találtunk új, kellően nagy CPU-tételt;
a no-op hálózat és a scriptek nem kaptak mesterséges párhuzamosítást.

### Végső ellenőrzés és üres editor

- Editor és runtime: Debug és Release build sikeres, konfigurációnként **ctest 7/7**.
- Vulkan synchronization validation: standalone stresszjelenet, standalone ABI 7 tesztjelenet,
  betöltött editor Scene és editor Play; **0 VUID/layout/szinkronizációs hiba** a javítások után.
  A Scene/Play automatizálása ideiglenes, a rendes projektbetöltési és Play command útvonalat
  használó driverrel történt; a driver eltávolítva, mindkét végső editor bináris újrafordítva.
  A régi sikertelen Scene/Play logok diagnosztikai bizonyítékként megmaradtak.
- A GPU emitter a tiltás alatt kimarad a dispatch/draw útból és nem takarítható ki;
  visszakapcsoláskor a grace-idő frissül, így egy másik aktív emitter takarítása sem
  indítja újra. A két-emitteres runtime teszt egyetlen gyermek-emitter létrehozását várja.
- Az üres editor végső Release futása: **4228–4293 megjelenített FPS**, 0,233–0,236 ms
  CPU-frame, körülbelül 0,098 ms GPU-frame és 0,085 ms UI GPU. A korábbi kontroll
  4274–4277 FPS volt: az új runtime API ezen az üres állapoton nem hoz érdemi FPS-nyereséget.
  A **6500–7000 FPS cél még nincs meg**; a terhelt jeleneten mért 8–9% nyereség külön eredmény.
- Saját mérőkód, kihagyó kapcsolók és képkimentés eltávolítva; az eredeti Main.scene és
  a teszt előtti manifest visszaállítva. Commit/push nem történt.

Újramérési bizonyítékok: `Client/build/followup-audit-20261008/` (gitignored). A fejlécazonosság
és `git diff --check` ellenőrizve. A mérési logok nem helyettesítik a játék tartalmának tesztelését;
audio-visszatérés és az animátor állapotmegtartása kódút-ellenőrzést kapott, külön hangos vagy
vizuális állapot-visszatérési tesztet nem állítunk.

## Fagenerátor: levélkártyák – 2026-10-08

A generátor fái erősen csökkentették az FPS-t. A mért ok a levélkártyák raszterterülete: a levelek alfa-maszkoltak,
így minden kártya teljes területe végigmegy a fő passon és minden árnyékkaszkádon. A fő pass lebontásában a maszkolt
levelek kihagyása 2,3–2,5 ms-ot vett le a ~5,7 ms-ból.

Egy fára jutó kártyák (a presetek natív méretben, egységük az engine-ben méter):

| Fa | kártya | kártyaterület |
| --- | --- | --- |
| a stresszjelenet fája (alapbeállítás) | 1008 | 328 m² |
| Oak Medium preset | 2592 | 18 427 m² |

Javítások:

- **Az átlátszó szél levágása mentéskor** (`trimLeafCards`, a Codex kezdte; kikapcsolható:
  `TreeMaterialBinding::trimTransparentLeafBorders`). A beépített levéltextúrák 2×2-es atlaszok, 80–93%-ban teljesen
  átlátszók.
  - Mentéskor minden kártya az atlaszcellája látható részére zsugorodik, 16 pixeles ráhagyással a szűrt élnek.
  - Az uv és a pozíció együtt mozog, így a textúra nem nyúlik; a háromszögszám és az indexek változatlanok.
  - Üres cella és nem generátor-topológia érintetlen marad.
  - A cella területéből a beépített textúráknál 27–59% marad meg.
- **A levélcsomók a gally mentén ülnek.** Eddig egy gally összes csomója a csúcsán, egy ~0,35 m-es foltban ült
  egymáson: a „levél kezdete” paraméter csak a csúcson túli eltolást méretezte. Most a csomók a gally `start`-jától
  a csúcsáig oszlanak el, ahogy a paraméter szól.
- **A „Double” kártyák keresztezik egymást.** A kártyák eddig teljes körön oszlottak el, így páros darabszámnál a
  fél fordulattal elforgatott kártya ugyanabba a síkba esett: a „Double” két egymásra rajzolt kártya volt. Páros
  darabszámnál most fél körön oszlanak el.
- **A panel kiírja a fa levélkártyáinak számát és területét**, mentéskor pedig a levágott arányt.
- **Az előnézet** a vetített, rendezett háromszögeket csak változáskor számolja újra (pine_medium: 3,53 → 1,35 ms
  frame-enként, a Codex mérőeszközével).

Mérés: a stresszjelenet 3004 fája ugyanazzal a fával, egyszer vágás nélkül, egyszer a mentéskori vágással
(−45,8% levélterület), buildelt játék, álló kamera:

| Fa | FPS | Scene GPU |
| --- | --- | --- |
| eredeti | 131–136 | 5,72–6,05 ms |
| vágott | **145–150** | **4,77–5,02 ms** |

A nyereség az árnyékkaszkádok újrarajzolásakor is jelentkezik, ugyanannyi területtel kevesebb.

Ellenőrzés:

- **Képi:** a lombkorona-régió a kettőben azonos (átlagos eltérés 1,26 a 765-ös skálán, JPEG-zaj; 48-nál nagyobb
  eltérés egy pixelen sincs).
- **Mentés az editorban:** Oak Medium mentve, 2592 kártya vágva, −46%.
- **Teszt:** a `TreeGeneratorTest` (ctest, most 8/8) a vágást, a gally menti elhelyezést és a keresztező kártyákat
  ellenőrzi. A régi generátorral a két utóbbi teszt elbukik.

Ami maradt:

- **A korábban mentett fák nincsenek vágva.** A nyereséghez újra kell menteni őket a generátorból, vagy egy
  utólagos vágóeszköz kell a meglévő `.glb`-khez. Ugyanez a művelet egy meglévő fán is működik: a mérésnél így
  készült a vágott fa.
- **Ugyanaz a preset most másképp néz ki:** teltebb a korona, mert a levelek a gallyakon oszlanak el. Erdőhöz
  kevesebb vagy kisebb levél (count, size) és a „Single” kártya a legolcsóbb.
- **Távoli fák a fő nézetben:** a levélkártyák ritkítása (mint az árnyék-LOD-ban) vagy impostor. A főnézeti
  LOD-próba képi eltérés miatt el lett vetve (lásd fent).

### A fő pass: a kéreg részletességi szintjei

A vágott levelekkel a fő pass még ~5 ms volt. Ideiglenes kihagyó kapcsolókkal lebontva (vágott fa, álló kamera):

| Tétel | GPU-idő |
| --- | --- |
| a statikus hálók nélkül maradó rész (karakterek, ég, részecskék) | ~1,1 ms |
| opak statikus hálók (3004 fa kérge, 7000 doboz) | 2,0–2,5 ms |
| maszkolt levelek | ~1,6 ms |

A kéreg fánként 4456 háromszög, a távoli fákon pixelnél jóval kisebbek. Ezért:

- **A fő nézetnek is vannak részletességi szintjei** (`BuildLods`, az árnyék-LOD-dal együtt épül a betöltő szálon).
  Az opak részhálók ugyanúgy 5 / 16 / 64 mm hibán belül egyszerűsödnek, de a varratok megmaradnak, így a textúra
  és az árnyalás nem változik. Az árnyék szintjei összevarrt felületből készülnek, azok itt rossz uv-t adnának.
- **Szintválasztás példányonként:** a legdurvább szint, amelynek hibája a példány skálájával, a legközelebbi
  pontja mélységén a képre vetítve fél pixel alatt marad (`kViewLodMaxErrorPixels`). A vetítés skáláját és a
  mélységet a view-projection mátrixból olvassa.
- **Csoportosítás szint szerint:** egy rajzolás példányai szintenként csoportosítva rajzolódnak (a sorrendjük a
  szinten belül marad), így egy szint egy futás. A maszkolt részhálók (levelek) a saját indexeikkel rajzolódnak.
- **Kikapcsolás:** `IX_VIEW_LOD=0`, összehasonlításhoz.
- A fa kérge: 4456 → 1336 → 247 háromszög.

Stresszjelenet, buildelt játék, álló kamera, vágott fával:

| Állapot | FPS | Scene GPU |
| --- | --- | --- |
| eredeti fa (a mai nap elején) | 131–136 | 5,72–6,05 ms |
| vágott fa, LOD nélkül | 141–143 | 5,09–6,16 ms |
| vágott fa, LOD-dal | **173–176** | **3,67–4,06 ms** |

Az opak rész így ~1,0 ms. Ami maradt: ~1,1 ms alap, ~1,0 ms opak, ~1,7 ms levél.

- **Képi:** LOD-dal és nélküle a fák régiójában az átlagos eltérés 1,82 a 765-ös skálán. A különbségkép csak
  JPEG-zajt és a mozgó részecskéket mutatja.
- **Validáció:** a szinkronizációs validáció 0 hibát jelzett. Az editor Scene nézete és a kijelölés rendben van.
- **Kipróbálva, elvetve:** a példányok közelről távolra rendezése (hogy a takart levelek korán kiessenek) nem hozott
  mérhető javulást.
- **A levelek további csökkentése** csak képi változással lehetséges, például a távoli fák kártyáinak ritkításával,
  ahogy az árnyékban.

### A fagenerátor felülete

- **Az ablak csak a nézet:** a fa előnézete a sarkában a kártya- és háromszögszámmal. Húzás forgat, a görgő
  közelít, dupla kattintás visszaállít.
- **A beállítások az Inspectorban vannak, felcímkézve.** Az Inspector attól kezdve mutatja őket, hogy a nézet
  fókuszt kap (vagy megnyílik), addig, amíg a Scene View, a Game nézet, a Hierarchy vagy az Asset Browser fókuszt
  nem kap.
  - Szakaszok: General, Trunk and Branches (Trunk / Level 1–3 lapok), Bark, Leaves, Output.
  - A korábbi felirat nélküli ágszint-mezők nevet és súgót kaptak.
- **Textúra helyett material:** a kéreg és a levél egy-egy Material-t fogad (az Asset Browserből húzva, vagy
  kattintásra listából, a mappájával).
  - Material nélkül a beépített kéreg vagy levél marad.
  - Megadott materialnál a mentett modell alapanyaga az lesz (nem készül új), a `.glb` a textúrájára hivatkozik,
    és a levelek vágása is ezzel a textúrával megy. Csak akkor vág, ha a material nem csempéz és nem tol el uv-t.
  - A nem alfa-maszkolt levél-materialra figyelmeztet. Az atlasz rácsa (oszlop és sor) is állítható, mert egy saját
    levéltextúra nem feltétlenül 2×2-es.
- **Eltávolított mezők:** a „Tree Type”, a tinták, a „Flat Shading” és a „Textured” sem a generált, sem a mentett
  fára nem hatottak.
- **Javítva: négy material kettő helyett.** Mentéskor a generátor elkészíti a `bark` és a `leaves` materialt,
  és beírja őket a modell alapanyagai közé. Az első betöltéskor a renderer a `.glb`-ből újra legyártotta őket. Az
  `emissive` erőssége a kettőben eltért (0 vs 1), ezért a betöltő `_v2` másolatot mentett, és átírta rájuk a modell
  alapanyagait. Egy választott material így elveszett volna.
  - Most a betöltés megtartja a modell meglévő, mind elérhető alapanyagait (`GenerateMaterialAssetsForGltf`).
  - Az exporter is a glTF-nek megfelelő erősséggel menti őket.

### PBR material editor: a beállítások eddig nem jutottak el a rendererhez

A PBR editor egy `.material` fájl mentésekor csak a színt, az erősségeket, a shading módot és az alpha módot írta ki.
A textúra-slotok (Diffuse, Normal, AO, Roughness, Metallic, Height) és a tiling elvesztek. Az editor mutatta őket,
de a renderer, amely a `.material` fájlt olvassa, sosem kapta meg őket.

- **Mentés:** most minden mező a fájlba kerül, a tint alfája is (`baseColor.a`, színválasztó alfa-sávval). Az alfa
  a textúráéval szorzódik, a mask cutoffnál és a blendnél számít.
- **ORM-csomagolás:** a shader egy csomagolt textúrát olvas (R occlusion, G roughness, B metallic). A külön AO-,
  roughness- és metallic-térképet az editor ebbe csomagolja (`packOcclusionRoughnessMetallic`).
  - A csomag a material mellé kerül, a forrásairól elnevezve. A korábbi csomagok törlődnek.
  - A forrástérképek a material fájlban is megmaradnak (`roughness`, `metallic`, `height` kulcsok), így újranyitva
    az editor mutatja őket.
- **Élő előnézet:** a változás mentés nélkül, azonnal látszik a jelenetben (`MaterialAssetManager::markChanged`). A
  mentés írja ki a fájlba.
- **Height:** a térkép megmarad, de shader még nem használja.
- **Ellenőrizve az editorban:** egy fa kéreg-materialján hat textúra, 3×1-es tiling és fehér tint. A jelenetben a
  kéreg a beállított textúrával, csempézve rajzolódik. A mentett fájlban minden mező ott van, a csomagolt ORM
  1024²-es.

## Egyetlen közeli fa – 2026-10-09

A felhasználó fája (`D:/AkitaOnline`, `tree_36330`) a projekt egy másolatán (`Client/build/akita-copy-20261008`)
mérve, a koronára néző közeli nézetből:
- 54 m magas, 4032 levélkártya (átlagosan 1,5 m-es), a vágás után is 10 570 m² levélterület;
- a levél-material opak, `Stylized_Leaves` JPG-textúrával (alfa nélkül), a kéreg-material `mask` módban.

| Változat | FPS | Scene GPU |
| --- | --- | --- |
| fa nélkül | 446 | 0,48 ms |
| fával, a mai nap elején | 155 | 4,40–4,48 ms |
| + maszkolt mélység-előpass | 195 | 3,69–3,70 ms |
| + opak shader `discard` nélkül | **263** | **1,96–1,97 ms** |

- **Opak változat `discard` nélkül** (`static_mesh_opaque_ps.spv`, `static_mesh_unlit_opaque_ps.spv`,
  `STATIC_MESH_OPAQUE`).
  - A static mesh shaderében `discard` van (alfa-maszk, vízalatti vágás). Emiatt a hardver az opak rajzolásoknál
    sem tesztelte a mélységet árnyalás előtt, és minden takart réteget teljesen árnyalt.
  - Opak materialnál a `discard` sosem futott le, ezért a kép nem változik. Ez minden opak statikus hálót érint.
  - Kikapcsolás: `IX_OPAQUE_NO_DISCARD=0`.
- **A maszkolt rajzolások két lépésben.**
  - Előbb a mélységük kerül be: az árnyékpass alfateszt-shadere végzi, színt nem ír.
  - Utána a színük, `LessOrEqual` teszttel, mélységírás nélkül: csak a látható texel árnyalódik.
  - Kikapcsolás: `IX_MASK_PREPASS=0`.
  - A stresszjeleneten egyedül nem hozott sokat (a Codex is ezt mérte). Közeli, sokrétegű koronánál viszont 16%.
- **Levélkártya-kivágás sokszögre** (`cutoutLeafCards`, mentéskor, a `trimLeafCards` helyett).
  - A kártya a látható texelek konvex burkára szűkül (8 px ráhagyással, legfeljebb kb. 8 csúcs, a cellára vágva),
    háromszöglegyezőként rajzolódik.
  - A beépített egyleveles atlaszoknál a kártya ~40%-kal kisebb a téglalapra vágottnál.
  - Csak alfa-maszkolt levél-materialnál fut, mert opak materialnál látható részt vágna le. A felhasználó fáján
    ezért nem alkalmazható: a mérésnél a mentéskori atlaszhoz vágva valódi lombot vágott le.
- **A kártyák a mentéskori levéltextúrához igazodnak:** a levél-material textúrájának vagy alpha módjának cseréje
  után a fát újra kell menteni. A generátor panelje ezt jelzi.

Ellenőrzés:

- **Képi:** a közeli nézetben az új és a régi út képe azonos (átlagos eltérés 0,92 a 765-ös skálán).
- **Validáció:** a szinkronizációs validáció 0 hibát jelzett a stresszjeleneten és a felhasználó jelenetének
  másolatán.
- **Stresszjelenet:** a fő pass 4,79–5,10 helyett 4,58–4,63 ms, visszaesés nincs.
- **Tesztek:** a ctest 8/8, a `TreeGeneratorTest` a kivágást is ellenőrzi.
