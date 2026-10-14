# Runtime párhuzamosítási terv

## Cél

Ennek a tervnek a célja, hogy az editor és a buildelt játék ugyanazt a közös runtime job-rendszert használja. A párhuzamosítás ezért nem editor-specifikus optimalizáció lesz: a kész játékban is aktív marad, miközben minden nagyobb terület külön kikapcsolható és soros módban is futtatható.

Az elsődleges teljesítménycél az editor és a könnyű jelenetek CPU-oldali frame idejének csökkentése. A jelenlegi mérésben a render loop több ezer FPS-en fut, ezért néhány tized milliszekundumos főszálbeli munka is jól látható. A cél nem az, hogy minden kód külön szálon fusson, hanem hogy a független, adat-előkészítő munkák párhuzamosan készüljenek, a főszálon pedig rövid és determinisztikus összefésülés maradjon.

## Várható méret

Nagyobb rendszerként körülbelül 20–22 párhuzamosítható munkaterület azonosítható:

- 8–10 terület közvetlenül hasznosítható és viszonylag kis kockázatú;
- 6–7 terület adat-tulajdonlási vagy staging-átalakítást igényel;
- 4–5 terület csak komolyabb renderer-, ECS- vagy script-architektúra módosítással érdemes.

A fizika, a build/packaging, az asset watcher és az async present már tartalmaz háttérmunkát. A fennmaradó, FPS szempontjából fontos munkát az alábbi sorrendben kell megvalósítani:

1. Scene/Game láthatóság és render batch-előkészítés.
2. Animációs pose sampling, retargeting és blending.
3. Particle emitterek és transparent queue-k előkészítése.
4. Scene snapshot, hierarchy snapshot és asset import háttérbe helyezése.
5. Csak ezután: párhuzamos Vulkan command recording secondary command bufferökkel.

## Alapelvek

### Közös runtime, külön editor réteg

A job-rendszer és a runtime feladatok a közös `Client/libs` és engine rétegekben legyenek. A Scene View, Game View, ImGui, RmlUi és editor mérők editor-specifikusak maradnak. A kész játék nem függhet editor headeröktől vagy editor-only kapcsolóktól.

### Fázisok és adat-tulajdonlás

Egy frame három jól elválasztott fázisra bomlik:

```text
Main-thread input / command collection
        |
        v
Parallel read-only jobs
        |
        v
Deterministic merge / ECS commit
        |
        v
Render command preparation and submission
```

A worker feladatok csak saját, thread-local kimeneti tárolót írhatnak. Közös vektorba, ECS komponensbe, renderer batch-be vagy script állapotba közvetlenül nem írhatnak. A merge fázis sorrendje stabil legyen, hogy a frame reprodukálható maradjon.

### Ne legyen rejtett mutexes gyorsítás

Egy mutex mögé zárt közös vektor nem valódi párhuzamosítás. A feladatok először thread-local tömbökbe gyűjtenek, majd egyetlen összefésülési lépés másolja vagy mozgatja az eredményeket. A lock csak ritka, rövid életű állapotváltásoknál használható.

### A GPU-parancs külön kérdés

A CPU oldali láthatóság-, animáció- és batch-előkészítés párhuzamosítható úgy, hogy a meglévő elsődleges `IXRHICommandList` továbbra is a főszálon rögzül. Maga a Vulkan command recording csak akkor válik párhuzamossá, ha a backend secondary command bufferöket vagy workerenkénti command listákat támogat.

## Runtime job-rendszer

### API

Új, közös runtime szolgáltatásként szükséges egy kis job-réteg, például:

```cpp
namespace ix::jobs {
    using Job = std::function<void(JobContext&)>;

    JobHandle Submit(Job job, JobPriority priority = JobPriority::Normal);
    void Wait(JobHandle handle);
    void WaitAll(std::span<const JobHandle> handles);
    uint32_t WorkerCount() const;
}
```

A konkrét név más lehet, de a követelmények rögzítettek:

- worker pool, nem frame-enként létrehozott `std::thread`;
- per-worker scratch allocator vagy arena;
- parent/child job és `WaitAll` támogatás;
- debug módban task tracing és worker azonosító;
- soros fallback ugyanazzal az API-val;
- a játékbuildben is elérhető, editor nélkül linkelhető könyvtár;
- alapértelmezett worker szám: `max(1, hardware_concurrency - 1)`, konfigurálható felső korláttal.

### Kapcsolók

Minden nagyobb csoport ugyanazt az általános kapcsolót és saját részkapcsolót kapjon:

```text
IX_JOBS=0|1
IX_JOBS_WORKERS=<N>
IX_PARALLEL_CULL=0|1
IX_PARALLEL_ANIMATION=0|1
IX_PARALLEL_PARTICLES=0|1
IX_PARALLEL_ASSET_IMPORT=0|1
IX_PARALLEL_RENDER_RECORDING=0|1
```

A kapcsolók elsődleges célja a regressziók izolálása. A végső játékbuild alapértelmezésben bekapcsolt runtime párhuzamosítással készül, a soros mód diagnosztikai fallback.

## Megvalósítási fázisok

### 0. Mérési alapvonal és determinisztikus fallback

**Feladatok**

- A meglévő Game View mérőben külön mérni a CPU frame időt, culling időt, animációt, particle-t, merge időt és render command előkészítést.
- A frame számláló és a jelenlegi Scene View mérő maradjon változatlan.
- Rögzíteni kell egy soros (`IX_JOBS=0`) és egy párhuzamos (`IX_JOBS=1`) alapvonalat.
- Minden új job-csoport kapjon külön trace zónát.

**Elfogadás**

- Azonos jelenet, kamera és ablakméret mellett a két mód összehasonlítható.
- A soros mód ugyanazt a képet és ugyanazt a frame-sorrendet adja.
- A worker pool létrehozása és leállítása nem okoz leaket vagy processz-zárási hibát.

### 1. Scene/Game culling és static mesh batch-előkészítés

**Források**

- `Client/libs/render/SpatialIndex.cpp`
- `Client/libs/render/StaticMeshRenderer.cpp`
- `Client/apps/client/src/EngineApplication.cpp`

**Munkák**

- A Scene View és Game View frustum query-k külön jobként futhatnak.
- A visible entity list, LOD kiválasztás, material/mesh batch-képzés és instance adat előkészítése thread-local tárolóba kerüljön.
- A két view kimenete külön `ViewRenderPreparation` objektumba kerüljön.
- A végső renderer batch sorrend legyen stabil: view, material key, mesh key, entity id.

**Függőségek**

- A `SpatialIndex` query legyen read-only.
- A static mesh és material assetek immutable snapshotként legyenek elérhetők.
- A renderer ne módosítsa a közös cache-t a culling job közben.

**Várható hatás**

Ez a legfontosabb első célpont, mert a jelenlegi Scene és Game View CPU láthatósági munkája sorosan fut, miközben a két view részben független.

### 2. Animációs sampling, retargeting és blending

**Források**

- `Client/libs/animation/AnimationRuntime.cpp`
- `Client/libs/animation/AnimatorRuntime.cpp`

**Munkák**

- Entitásonkénti `SampleAndRetargetAt` és blending jobok párhuzamos futtatása.
- Minden animator saját sampling/blending contextet és scratch memóriát használjon.
- A kész pose buffer indexelt, double-buffered kimenet legyen.
- A render előkészítés csak a teljes pose batch elkészülte után induljon.

**Korlátok**

- Ugyanazt a mutable Ozz contextet több worker nem használhatja.
- Animator state írása, event dispatch és gameplay callback maradjon commit fázisban.
- Root motion eredmények determinisztikus entity-sorrendben kerüljenek vissza.

### 3. Particle emitterek és transparent queue

**Források**

- `Client/libs/particles/ParticleSimulator.cpp`
- `Client/apps/client/src/EngineApplication.cpp`

**Munkák**

- Emitterenként külön particle update job.
- A részecske-képkocka adat és spawn/death lista emitter-local legyen.
- A Scene és Game transparent queue-k külön thread-local listákba gyűjtsenek.
- Merge után maradjon egy globális, stabil sort a depth/material szabályokhoz.

**Korlátok**

- A ground-height és collision callback ne hívjon nem thread-safe world API-t.
- A random stream emitterenként fix seedet és frame indexet használjon.
- Egy emitteren belüli particle loop csak későbbi, mérés által indokolt lépésként legyen chunkolva.

### 4. Shadow, water és egyéb láthatósági előkészítés

**Munkák**

- Shadow caster culling és shadow batch-ek előkészítése thread-local kimenettel.
- Reflection/water visibility és CPU mesh preparation külön jobokban.
- Sky, god-ray és selection outline objektumlisták read-only lekérése.

**Korlátok**

- A passok közötti erőforrás-életciklust nem szabad workerből módosítani.
- A tényleges command list rögzítése maradjon a meglévő backend szabályai szerint.

### 5. Scene snapshot, hierarchy snapshot és asset pipeline

**Scene snapshot**

- A `BuildSceneSnapshot` teljes másolatát worker oldalon kell felépíteni.
- A frame elején immutable scene revision kerül rögzítésre.
- A főszál a kész snapshotot egyetlen pointer/index cserével publikálja.

**Hierarchy/UI snapshot**

- A Flecs world írása maradjon a főszálon.
- A hierarchia és inspector számára egy read-only, lapos UI snapshot készüljön workerben.
- ImGui panelépítés továbbra is a UI szálon történjen.

**Asset pipeline**

- Fájlrendszer-szkennelés, Assimp import, textúra decode, shader fordítás és pipeline cache-előkészítés workerben.
- GPU resource create/upload/finalize külön render-thread commit queue-ba kerüljön.
- Asset handle állapotok: `Unloaded -> Loading -> CpuReady -> GpuPending -> Ready`.
- Hiba esetén a worker csak immutable error resultot adjon vissza.

### 6. Character controller, network és audio staging

**Character controller**

- Az input és a korábbi transform snapshot a frame elején rögzül.
- A controller jobok eredménye `CharacterStepResult` tömbbe kerül.
- A Jolt world step és az eredmények commitja explicit barrierrel történjen.

**Network**

- Receive, packet decode és snapshot interpolation workerben.
- Az ECS vagy gameplay állapot módosítását command bufferen keresztül kell commitálni.
- A jelenlegi `RuntimeSession::UpdateNetwork` no-op állapotát nem kell mesterségesen párhuzamosítani; csak valódi hálózati terhelésnél érdemes bevezetni.

**Audio**

- Listener/source transform számítás workerben előkészíthető.
- A miniaudio/backend API hívásai csak dokumentáltan thread-safe útvonalon menjenek.
- A végső source update egy dedikált audio commit queue-ba kerüljön.

### 7. Párhuzamos Vulkan command recording – későbbi fázis

Ez külön renderer-projekt, nem a culling jobok egyszerű folytatása.

**Szükséges módosítások**

- `IXRHICommandList` worker-kompatibilis secondary command buffer API.
- Workerenként command pool és frame-local lifetime.
- Passonként előre kiosztott command recording feladatok.
- Vulkan queue submit előtt secondary command buffer execute.
- D3D12 backend számára ennek megfelelő command list/allocator modell.

**Miért későbbi?**

A jelenlegi renderer egy elsődleges command listre rögzít sorban. Ha ezt részben szálakra bontjuk megfelelő backend-szerződés nélkül, race conditiont, rossz barrier-sorrendet és nehezen reprodukálható GPU hibát kapunk.

## Amit szándékosan nem párhuzamosítunk közvetlenül

- ImGui panelépítés és ImGui renderelés.
- RmlUi context/layout/update.
- A jelenlegi mutable script `OnUpdate` callbackek.
- Közvetlen Flecs/ECS strukturális módosítás workerből.
- Közös `IXRHICommandList` egyidejű használata.
- Globális asset cache írása lockolt, meghatározatlan sorrendben.

### Script rendszer későbbi lehetősége

A script futtatás csak akkor osztható szét, ha a script API két részre válik:

1. read-only world query és lokális számítás;
2. determinisztikus command buffer, amely spawn/destroy/component/audio műveleteket gyűjt.

A jelenlegi deferred operation mechanizmus jó alap a második részhez, de önmagában nem teszi thread-safe-é a callbackeket.

## Adatmodell és szinkronizáció

### Frame snapshot

Minden párhuzamos frame-rész ugyanazt az immutable bemenetet kapja:

```cpp
struct FrameSnapshot {
    uint64_t frameIndex;
    float deltaSeconds;
    CameraState sceneCamera;
    CameraState gameCamera;
    SceneReadView scene;
    AssetReadView assets;
    PhysicsReadView physics;
};
```

### Job-kimenetek

```cpp
struct ViewPreparationResult {
    std::pmr::vector<VisibleEntity> visible;
    std::pmr::vector<RenderBatch> batches;
    std::pmr::vector<TransparentItem> transparent;
};
```

A result csak a `WaitAll` után publikálható. A főszál a régi frame resultját nem írhatja felül, amíg a render pass használja.

### Barrier szabályok

- Input/snapshot barrier a simulation elején.
- Physics barrier a controller és gameplay eredmények előtt.
- Animation barrier a skinned render batch előtt.
- Culling barrier a pass command preparation előtt.
- Render submission barrier a swapchain/present előtt.

## Determinizmus és hibakeresés

- Stabil entity id vagy sort key alapján merge-elünk.
- Random generátorok streamenként külön állapotot kapnak.
- A debug buildben minden job logolhatja a frame indexet, job id-t és worker id-t.
- A soros módnak ugyanazt a kimeneti struktúrát kell előállítania.
- Race detector vagy ThreadSanitizer-kompatibilis kisebb teszt-target szükséges, ahol a platform támogatja.
- A frame replay teszt ugyanazzal az inputtal hasonlítsa össze a soros és párhuzamos állapotot.

## Mérési terv

Minden fázis előtt és után azonos profilmérést kell futtatni:

| Mérőszám | Cél |
| --- | --- |
| teljes CPU frame idő | főszálbeli nyereség |
| worker busy time | tényleges kihasználtság |
| culling idő Scene/Game nézetenként | láthatósági regresszió |
| animáció idő | pose pipeline költség |
| particle update idő | emitter terhelés |
| merge/commit idő | szinkronizációs ár |
| render command preparation | renderer CPU költség |
| present wait | present scheduler hatása |
| actual Scene View FPS | offscreen render pass mérés |
| Game View FPS/CPU/GPU/RAM | felhasználói runtime ellenőrzés |

Minden mérésnél külön kell futtatni:

- üres editor, projekt nélkül;
- üres projekt, nyitott jelenettel;
- sok static mesh;
- sok skinned mesh és animáció;
- sok particle és transparent objektum;
- árnyék, water reflection és god-ray bekapcsolva;
- játékbuild editor UI nélkül.

## Tesztelési követelmények

Minden fázis után:

1. Release és Debug build.
2. Release és Debug CTest.
3. Üres editor indítás és szabályos leállítás.
4. Projekt nélküli Scene View és nyitott jelenetes Scene View.
5. Game View telemetry ellenőrzése.
6. Vulkan validation layer futtatás.
7. Soros/párhuzamos képkimenet és scene-state összehasonlítás.
8. Legalább 10 perces soak teszt asset reload és ablakméret-váltás mellett.

Külön ellenőrizni kell, hogy a worker pool leállításakor ne maradjon függő asset, render vagy present job. A swapchain resize és az async present scheduler csak az összes kapcsolódó frame job kiürítése után zárható le.

## Visszaállítási stratégia

Minden fázis önállóan eldobható:

- globális `IX_JOBS=0` azonnal soros módra vált;
- részkapcsolóval egyetlen job-csoport tiltható;
- a worker API ugyanazt a függvényutat használja soros és párhuzamos módban;
- a culling, animáció és particle eredményének típusa nem változik a módok között;
- a renderer command recording párhuzamosítása külön backend feature flag marad;
- a korábbi async present visszakapcsolható `--sync-present` vagy `IX_ASYNC_PRESENT=0` útvonalon.

## Javasolt végrehajtási sorrend

### Mérföldkő A – Job alap és mérés

- runtime worker pool;
- soros fallback;
- tracing és Game View időmérők;
- nincs vizuális vagy gameplay változás.

### Mérföldkő B – Culling és batch-ek

- Scene/Game frustum query;
- static mesh batch-előkészítés;
- shadow caster előkészítés;
- stable merge és képkimeneti összehasonlítás.

### Mérföldkő C – Animáció és particle

- animátor context izoláció;
- emitter-local particle update;
- transparent queue merge;
- deterministic random és event commit.

### Mérföldkő D – Snapshot és asset pipeline

- scene/hierarchy snapshot;
- import/decode/compile worker;
- GPU finalize queue;
- asset reload soak teszt.

### Mérföldkő E – Runtime simulation staging

- character controller;
- valódi network decode;
- audio source preparation;
- csak mérés által indokolt esetben további felbontás.

### Mérföldkő F – Secondary command recording

- RHI API és Vulkan backend;
- worker command pools;
- passonkénti secondary buffer;
- D3D12 kompatibilitási terv;
- külön benchmark és validation kampány.

## Elfogadási kritériumok

A párhuzamosítási terv akkor tekinthető elkészültnek, ha:

- a Release játékbuildben a runtime job-rendszer aktív;
- az editor és a játék ugyanazt a runtime implementációt használja;
- minden fázis külön kikapcsolható;
- a soros fallback működik és reprodukálható;
- nincs új Vulkan validation hiba, race vagy processz-leállási hiba;
- a soros és párhuzamos mód gameplay eredménye egyezik a megengedett lebegőpontos tolerancián belül;
- a benchmark külön kimutatja a worker nyereséget és a merge költségét;
- a Game View mérő az FPS, CPU, GPU, RAM és renderer mellett a worker módot is képes jelölni;
- a párhuzamosítás mérhetően csökkenti a CPU frame időt legalább az animációs, culling vagy particle terheléses jelenetek egyikében.

## Végső döntési szabály

Nem a worker thread-ek száma a cél. A következő terület csak akkor kerülhet be a végleges alapértelmezett útvonalba, ha:

1. a soros eredménnyel determinisztikusan egyezik;
2. a worker idő nagyobb, mint a job/merge overhead;
3. nincs új frame spike vagy asset/shutdown race;
4. Release buildben is mérhető nyereséget ad;
5. a teljes feature egy kapcsolóval kikapcsolható.

