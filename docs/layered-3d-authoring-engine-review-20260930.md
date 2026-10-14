# Rétegezett 3D világ: motoroldali authoring és szerveroldali metadatalekérdezés

Dátum: 2026-09-30. Branch: `codex/engine-server-integration`.

## Kiindulási állapot és a mérföldkő eredménye

A kiindulási working tree tiszta volt a
`bf11861bb8eabf831c69ec8edf60273b7e3cdc3d` commiton. Ez a mainből átvett
motort/editorot és a With_Auriga szerverét, korábbi streaming-javításait,
rétegezett világ szerződését és szöveges generátorát tartalmazó integrációs
baseline. A motor és a szerver külön Cap'n Proto manifestsémája megmarad.

A jelenlegi változtatás a szerkesztett jelenet valódi ütközési geometriájából
képes ellenőrzött rétegmetaadatot előállítani. A szerkesztőben az objektum
bekapcsolható és szemantikai tagekkel jelölhető, a magasságtartomány pedig az
ütközési felületből és közös generálási szabályból származik. Egy helyszínhez
nem kell kézzel megadni a min/max Z értéket. A generált `layered_world.mx3d`
sidecart a meglévő strict szerverbetöltő fogadja, a tényleges `WorldRuntime`
pedig megőrzi és olvasásra hozzáférhetővé teszi.

Ez az authoring- és metadatamérföldkő nem aktivál többszintű authoritative
mozgást. A szimuláció terrain-, AOI-, zónatulajdonlási és migrációs útja
változatlan; játékosokat/mobokat még nem rendelünk az új volume-okhoz.

## Mi alapján történik a generálás?

Az audit a motor meglévő fizikai útját követte:

- `Client/libs/render/StaticMeshRenderer.cpp`, `CopyPhysicsMesh`: a renderer
  CPU-n tartott modellháromszögeit adja át; ugyanezt használja az editor
  jelenlegi Mesh collider útja.
- `Client/apps/client/src/EngineApplication.cpp`: a colliderhez a jelenet
  példányának helyét, Euler-forgatását, skáláját és colliderközéppontját
  rendeli; explicit authoring parancsnál a legfrissebb jelenet-snapshotból
  dolgozik.
- `Client/libs/physics/PhysicsWorld.cpp`, `CreateShape`: a Jolt tényleges
  colliderkészítését adja. Az authoring adapter az abszolút skála, a
  quaternion-forgatás, a colliderközéppont és a Box minimum-félméret szabályát
  is követi. A Box egészen kis középpont-eltolását a Jolt kihagyja; a Mesh
  beágyazott középpontját nem. Ezt a különbséget az adapter is megtartja.
- Az `EngineApplication` a bekapcsolt Static rigidbodyt gravitáció mellett
  Dynamic típusra emeli. Ilyen objektum authoring forrásként elutasított;
  a tárolt `Static` érték önmagában nem bizonyít statikus collisiont.

A támogatott modellforrás az azonosítóval rendelkező, statikus, bekapcsolt,
nem trigger `Box` vagy `Mesh` collider. Hiányzó Mesh adatokból nem készül
render-AABB vagy automatikus Box-helyettesítés. Dynamic/Kinematic,
CharacterController, skinned, NoCollision és a még nem támogatott
Sphere/Capsule/ConvexHull forrás hibát ad.

A víznek már eleve van felületi magassága és lábnyoma a `WaterBody` adatokban.
A generátor csak teljes, megfelelő méretű, kitöltött téglalapmaszkból készít
vízfelületet. Üres, nulla méretű, hibás hosszúságú, lyukas vagy másként nem
teljes maszkot elutasít. Kör alakú és összetett folyóparti vízfelülethez
később bizonyított geometriai felosztás kell; annak teljes befoglaló dobozát
most nem változtatjuk vízzé.

## Geometriai szerződés és határok

Az új közös cooker a `shared/map/LayeredWorldGeometry` komponensben van.
Összefüggő, egy síkban fekvő, megfelelő irányba néző ütközési lapokat keres.
Az azonos koordinátájú pontokat determinisztikusan összehegeszti, ezért a
material/submesh-határon duplikált csúcsok önmagukban nem szakítják ketté a
felületet. Közeli, eltérő koordinátájú csúcsokat nem illeszt össze találomra.

A jelenlegi volume-szerződés tengelyekkel párhuzamos téglalapból és függőleges
sávból áll. A cooker ezért csak bizonyítottan teljes, összefüggő, síkbeli,
tengelyekkel párhuzamos téglalap-lábnyomot fogad. A határélek, zárt határ és
irányított vetületi terület ellenőrzése megakadályozza, hogy lyukból,
konkáv alakból vagy elforgatott lábnyomból kitalált járható AABB készüljön.
A tiltott vagy hibás topology, index, degenerált/duplikált háromszög és nem
véges koordináta elutasított. Falak, lefelé néző és túl meredek lapok nem
képeznek járható padlót; ha semmilyen megfelelő felület nincs, a generálás
hibával megáll.

Alapértelmezett közös geometriai paraméterek:

| Paraméter | Érték | Jelentés |
| --- | --- | --- |
| `foot_tolerance` | 0,02 m | Az alsó magassághatár a lap legkisebb Z értéke alatt |
| `clearance_height` | 2,0 m | A felső metadatasáv a lap legnagyobb Z értéke felett |
| `max_slope_degrees` | 45° | A padlójelölt lap maximális meredeksége |

A sáv így `[floor_min - 0,02; floor_max + 2,0)`.
Ez egy globális metadata- és identitásszabály. Nem bizonyítja, hogy a
karakter feje fölött 2 méter szabad hely van, hogy a lépcső végig bejárható,
vagy hogy az adott collision mozgásengedélyt ad. Fejtér-, kapszula- és
bejárhatósági vizsgálat a következő collision/movement mérföldkő része.

Az engine út `require_exact_footprints=true` módban hívja a már meglévő
`GenerateLayeredWorld` generátort. Ebben az útban nincs résáthidaló vagy
AABB-t növelő összevonás. Két egymást átfedő függőleges sáv továbbra is
hibás, akár külön szemantikai családból származnak. A régi szöveges adapter
alapértelmezett működése kompatibilis marad.

Portált nem következtetünk közelségből. Egy híd melletti fal, lépcső,
lift, ajtó vagy teleport eltérő átmenetszerződést igényel. A tagek ezeket
megnevezik, de nem kapcsolják össze automatikusan a szinteket.

## Koordináták: a tényleges motorhoz illesztve

A jelenet és a collision metrikus, a motorban Y mutat felfelé:

```text
engine (X, Y-up, Z) méter → package (X, Y, Z-up) méter
engine (x, y, z)          → package (x, z, y)
```

`EngineToLayerCoordinates` és `LayerToEngineCoordinates` kölcsönösen
visszaállítják a pontot. A Y/Z csere megfordítja az orientációt, ezért
az adapter a háromszögek windingját is korrigálja. A régi Auriga
`RawServerToDisplay` centiméteres, negatív vízszintes előjelű átalakítását
nem használja ez az authoring út, és nem módosítja a legacy hálózati utat.

A motor renderere és Jolt terrainje a világorigó köré középre helyezi a
terraint. Szélesség=`width`, mélység=`depth` esetén az authoring world bounds:

```text
[-width/2, width/2) × [-depth/2, depth/2)
```

A megfelelő package-origin `(-width/2, -depth/2)`; a negatív horizontális
koordináta és a negatív magasság is szabályos. A fixture ezért az actual
engine terrainhez illeszkedő, középre helyezett 32×32 m világot és külön
strict-v3 package-origót használ. Terrain nélkül a forrásfelületek
lábnyomának befoglaló téglalapja adja a generálási world bounds-ot.

Ez önmagában nem teljes terrainpackage-export. A motor mentett heightfield
soriránya, a package-indexek/origin és az összes renderer/szerver terrain
minta közös bizonyítása még külön munka. Egy metadatasidecar sikeres
generálása nem állít teljes szervervilág-exportot.

## Szerkesztői használat

1. A jelenetben adj az objektumnak megfelelő, statikus Box vagy Mesh collidert.
   A külön helyzet, skála és forgatás az objektum szokásos transformja marad.
2. Az Inspector **Layer generation** részében kapcsold be az
   **Include collision surfaces** jelölést és válaszd a szerepének megfelelő
   tag vagy tagek kombinációját.
3. A **Tools → Layered world → Generate layers from collision** parancs
   ellenőrzi és generálja az aktuális jelenet metaadatát. Hibánál a státusz
   konkrét forrás- vagy geometriai okot mutat.
4. A **Show layer volumes** kapcsoló jeleníti meg a generált volume-dobozokat.
   A preview a legutóbbi generálás snapshotja: geometriai/szemantikai
   szerkesztés után újra generálni kell. Scene-betöltéskor a régi preview és státusz törlődik.
5. Mentsd a jelenetet, majd válaszd az **Export layer metadata** parancsot.
   Az export újra generál az aktuális scene-adatokból, és új útvonalra ír:
   `<scene>.layers/export-<egyedi időazonosító>/layered_world.mx3d`.

A tagek: `ground`, `building`, `bridge`, `water`, `underwater`, `dungeon`,
`interior`, `connector`, `road`, `stairs`, `lift`, `dock`.
A `LayerAuthoringSettings` példányonként tárolja az enabled értéket és a
tagbitmaszkot; mindkettő a scene JSON opcionális `layer_authoring` mezőjében
megmarad. Régi scene mező nélkül opt-out marad. A kikapcsolt, de már tagelt
objektum beállítása is mentődik. A hibás tagadat nem alakul át automatikusan
Ground típussá.

A generált azonosítók stabilak ugyanazon bemenet és puszta array/triangle
sorrendcsere mellett. Strukturális szerkesztés, új vagy törölt felület
azonban átszámozhatja a volume/layer ID-kat. A jelenlegi exportból még nem
szabad tartós gameplay- vagy DB-identitásra következtetni.

## Export és szerveroldali viselkedés

Az export előbb kódolja, visszaolvassa és validálja a teljes `MX3D` adatot,
majd kizárólag új staging fájlt és új végleges fájlt hoz létre. Windows alatt
natív Unicode útvonallal `CreateFileW(CREATE_NEW)` és replacement nélküli
`MoveFileExW` működik; a POSIX út `O_EXCL` és replacement nélküli link
műveletet használ. Már létező végleges vagy `.pending` fájl megmarad.
Két párhuzamos azonos célú export közül egy publikálhat sikeresen.

A strict package-loader eddig is validálta az opcionális sidecart.
Most a production `WorldRuntime` is megtartja a validált adatot
`unique_ptr<const LayeredWorld>` formában:

- `LayeredMetadata()` a runtime életidejére immutable adatot ad;
- `FindLayerVolume(x,y,z)` csak fél-nyitott point lookupot végez;
- nem fedett és nem véges pontra nincs kitalált volume;
- hiányzó/üres sidecar és synthetic világ a korábbi működésen marad;
- split/merge vagy lookup nem tölti újra és nem módosítja a csomagot.

A query még nem egy authoritative entity-layer döntés. A benne szereplő
layer és volume identitást a mozgás, AOI és migráció jelenleg nem alkalmazza.
Az olvasás költsége lineáris a bounded volume-listán; ez most metadataquery,
nem új streaming/scheduler vagy production spatial index.

## Ellenőrzések és bizonyítékállapot

| Ellenőrzés | Rögzített eredmény |
| --- | --- |
| `layered_world_geometry_test` | 46 sikeres ellenőrzés |
| `SceneLayerAuthoringTest` | 71 sikeres ellenőrzés, 0 hiba; tényleges Jolt oracle bekapcsolva |
| `worldbench --mode layerlookup` | 26 sikeres ellenőrzés |
| Korábbi layered contract | 9 sikeres ellenőrzés |
| Korábbi layered package | 10 sikeres ellenőrzés |
| Korábbi layered generator | 6 sikeres ellenőrzés |
| Engine/server map compatibility | 9 sikeres ellenőrzés |
| Korábbi layered spatial key | 7 sikeres ellenőrzés |
| `worldbench --mode worldpackage` | 140 sikeres ellenőrzés; 1 symlink-kontroll környezeti okból SKIP |
| `worldbench --mode terrain` | 20 sikeres ellenőrzés |
| Végső engine/editor GUI-próba | PASS: 12 tag, három szint generálása, volume preview, Box és valódi Mesh export |
| Végső Debug build és evidence útvonal | PASS: IxtreemeEngine, SceneLayerAuthoringTest, gameserver, worldbench és közös teszt-targetek Windows Debug buildje |
| Git | A riport a mérföldkő commitjának része; a végső hash a Git logban és a záró válaszban szerepel |

Összesen 344 sikeres automatizált ellenőrzés, 0 hiba, 1 környezeti SKIP.
Az ellenőrzésszám az egyes executable-ök saját ellenőrzéseinek összege,
nem 344 külön CTest test case. Windows/MSVC Debug konfigurációban futottak;
Release, Linux/POSIX és Android build ebben a mérföldkőben NOT_RUN.

### Végrehajtott GUI-próba

A `computer-use` útján elindított, helyben buildelt `IxtreemeEngine.exe`
egy külön projekten futott:
`Client/build/layer-authoring-smoke-20260930-174253/project.ixproj`.
A fixture három, azonos XY-n álló lapot tartalmazott: Ground road,
Upper bridge és Underpass. A scene nem használt meglévő játékassetet.
Az anyag nélküli builtin kockalapok a renderer szokásos magenta
helyettesítőanyagával jelentek meg; ez nem anyag- vagy renderer-acceptance.

Az editor megnyitotta a startup scene-t. A Tools menü három volume-ot
generált a következő magasságokkal: `[-6,02; -4)`, `[-0,02; 2)` és
`[5,98; 8)` méter. Az Upper bridge Inspectorában mind a 12 tag látható,
a Bridge és Connector bejelölés megmaradt. A Show layer volumes három
külön színezett dobozt rajzolt a valódi felületek fölé.

Ezután az Upper bridge colliderét az Inspectorban Boxról Meshre
állítottuk. A friss export a production `CopyPhysicsMesh` útból kapta
a builtin modell tényleges háromszögeit. A Box és Mesh export egyaránt
168 bájtos, `MX3D` magicű, és SHA-256 szerint bájtra azonos:
`FDD25F8FAB215438BB854668C301E78ABC92D7A1006C659379BB10A0F30B204D`.
Mindkét fájl megmaradt, külön exportkönyvtárban:

- `stacked.layers/export-17907836211592514/layered_world.mx3d`
- `stacked.layers/export-17907838251589817/layered_world.mx3d`

A scene automatikus mentésében a második objektum colliderje `mesh`,
tagmaszkja 132; a többi objektumé 257 és 384. A saját teszteditor X-es
bezárása után nem maradt engine-ablak. Futó szolgáltatást nem indítottunk
újra, DB-t és más kliensprojektet nem módosítottunk.

### Megismételhető parancsok és lokális naplók

A repository gyökeréből:

```powershell
cmake --build Client/build --config Debug --target IxtreemeEngine SceneLayerAuthoringTest --parallel 4
& Client/build/libs/render/Debug/SceneLayerAuthoringTest.exe
cmake --build gameserver/build/windows-debug --config Debug --target gameserver worldbench layered_world_geometry_test layered_world_contract_test layered_world_package_test layered_world_generator_test layered_spatial_key_test engine_server_map_compatibility_test --parallel 4
& gameserver/build/windows-debug/shared_map_build/Debug/layered_world_geometry_test.exe
& gameserver/build/windows-debug/shared_map_build/Debug/layered_world_contract_test.exe
& gameserver/build/windows-debug/shared_map_build/Debug/layered_world_package_test.exe
& gameserver/build/windows-debug/shared_map_build/Debug/layered_world_generator_test.exe
& gameserver/build/windows-debug/shared_map_build/Debug/layered_spatial_key_test.exe
& gameserver/build/windows-debug/shared_map_build/Debug/engine_server_map_compatibility_test.exe
& gameserver/build/windows-debug/apps/gameserver/Debug/worldbench.exe --mode layerlookup
& gameserver/build/windows-debug/apps/gameserver/Debug/worldbench.exe --mode worldpackage
& gameserver/build/windows-debug/apps/gameserver/Debug/worldbench.exe --mode terrain
```

A lokális eredménynaplók megmaradnak a buildkönyvtárakban:
`Client/build/scene-layer-authoring-test.log`, illetve
`gameserver/build/windows-debug/{layered_world_geometry_test,
layered_world_contract_test,layered_world_package_test,
layered_world_generator_test,layered_spatial_key_test,
engine_server_map_compatibility_test}.log` és
`{layerlookup,worldpackage,terrain}-test.log`.
A buildkönyvtárak nem kerülnek Gitbe; ez a riport a tartós eredményösszegzés.

A célzott engine teszt valódi `SceneManager::SaveSceneAs/LoadScene` mentésen
és betöltésen ellenőrzi a tageket, opt-out kompatibilitást és a regenerálást.
Független analitikus transformellenőrzés és a tényleges Jolt
`PhysicsWorld::CreateBody/Raycast` oracle vizsgálja az eltolást,
abszolút/nem egyenletes skálát, quaternion-forgatást, minimum Box-félméretet
és a Jolt középpontküszöbét. Static+gravity, dynamic, kinematic,
trigger, skinned, nem támogatott collider, ismeretlen tagek, world-bounds (beleértve a valódi terrain pozitív fele melletti külső területet),
hibás vízmaszk, duplikált víz-ID és átfedő szintek negatív kontrollt kapnak.

Az exportteszt Unicode fájlnevet, meglévő végleges/staging fájl
megőrzését és párhuzamos publikálást is ellenőriz. A strict-v3 fixture
ugyanazokat a layer ID-kat, tageket és magasságokat olvassa vissza teljes
`LoadServerWorld` validálással. A `layerlookup` teszt a valódi runtime
konstrukcióját használja supervisor-, worker- vagy hálózati session
indítása nélkül.

A tesztek külön, saját temp fixture-ökkel működnek. Nem írják felül a
checked `test_zone`-t, jelenlegi asseteket vagy futó szervereket, és nem
igényelnek DB-t, credentialt, timeoutemelést vagy populációcsökkentést.

Munkakorlátok: legfeljebb 4096 colliderforrás/generált volume,
131072 collisionháromszög és 262144 collisioncsúcs; az editor preview
legfeljebb 256 volume-dobozt rajzol. A generálás explicit parancsra történik,
nem minden render- vagy szimulációs tickben. Ez correctness-mérföldkő;
nagyvilágos terhelési/performance-elfogadás még nem történt.

## Módosított komponensek

| Terület | Fő fájlok és cél |
| --- | --- |
| Collision cooker | `shared/map/include/map/LayeredWorldGeometry.h`, `shared/map/src/LayeredWorldGeometry.cpp` |
| Pontos footprint-generálási mód | `shared/map/include/map/LayeredWorldGenerator.h`, `shared/map/src/LayeredWorldGenerator.cpp` |
| Engine adapter/export | `Client/libs/render/SceneLayerAuthoring.h/.cpp` |
| Opt-in settings és parancsok | `Client/libs/render/MapEditorTypes.h` |
| Scene JSON | `Client/libs/render/SceneManager.cpp` |
| Inspector, Tools menü, státusz | `EditorImGui.h/.cpp`, `editor_panels/EditorImGuiInspectorPanels.inl`, `EditorImGuiMenuToolbarPanels.inl` |
| Valódi command/provider/preview út | `Client/apps/client/src/EngineApplication.cpp` |
| Immutable runtime query | `gameserver/apps/gameserver/src/world/WorldRuntime.h/.cpp` |
| Új engine/shared/server tesztek | `SceneLayerAuthoringTest.cpp`, `layered_world_geometry_test.cpp`, `LayerLookupBench.h/.cpp` |
| Build regisztráció | shared map, engine render és gameserver CMake; `WorldBench.cpp` |

## Következő mérföldkő: a metadata után valódi többszintű működés

1. A terrain csomagolásának közös koordináta/origin/sorirány szerződését
   véglegesíteni kell, majd ugyanazon editorvilág terrainjét és rétegadatait
   strict package-be exportálni. A negatív koordinátákhoz illeszkedő
   renderer/szerver magasságmintát közös fixture bizonyítsa.
2. Szerveroldali static collision/floor query és actor-kapszula/fejtér
   ellenőrzés kell. A metadata bandot nem szabad fizikai járhatóság helyett
   használni. Ez után lehet összetett, elforgatott, lyukas és folyóparti
   felületeket biztonságos részekre bontani.
3. Definiálni és megvalósítani kell az authoritative entity-volume ownershipot:
   spawn, földön maradás, emelkedés/esés és korrigált mozgás minden esetben
   ugyanazt a collision- és layer-identitást használja.
4. A szerver AOI/spatial index és zónamigráció közösen vegye át a már meglévő
   layer-aware identitást. Egy azonos XY-n álló két szereplő nem válhat
   automatikusan ugyanazon collision-/láthatósági réteg részévé.
5. A lépcső, hídlejáró, lift, ajtó és vízkapcsolat explicit portálját valódi
   collision/nav kapcsolat és átmeneti szabály igazolja; a tagek ehhez
   bemenetek, nem önmagukban mozgásengedélyek.
6. Az authoritative layer/volume és Z állapotot a kliens/motor oldali
   entitás-, interpolation- és korrekciós úttal együtt kell bekötni.
   Kétklienses, többszintű mozgás/stop/AOI/disconnect/migráció acceptance
   bizonyítsa a teljes viselkedést, legacy regressziókkal együtt.

Ezekhez a munkákhoz továbbra is a közös branch a kiindulási alap.
Az editor authoringban szerzett működés és a régi streaming-, wake- és
20 Hz/NextTick szerződések megőrzendők. A jelenlegi mérföldkőből teljes
rétegezett MMO-szimulációra vagy új hálózati protokollra nem következtetünk.
