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

Ami maradt: ha a kamera mozog, a kaszkádok mátrixa változik, és akkor a teljes újrarajzolás továbbra is jár. Ebben a
sűrű jelenetben ez frame-enként ~20 ms GPU lehet. Ehhez árnyék-LOD vagy a statikus réteg görgetése kellene
(eltolás és a szélek utánrajzolása).

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
