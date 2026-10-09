# 3D-4A — bizonyított járófelület és explicit offline ground state

Dátum: 2026-09-30. Branch: `codex/engine-server-integration`.
Elsődleges baseline: `5cadefea7db4c48d8c3d085305166633219b4ad2`
(strict editor terrain package). A munkakezdéskor a working tree tiszta volt.
A korábbi bizonyítékok és a checked `test_zone` változatlanok maradtak.

## Eredmény és hatókör

Az exact modell-collision cooker megőrzi az általa bizonyított téglalap
járófelület valódi síkját. A motor és a szerver ugyanazt a read-only
placement/move API-t használja explicit volume azonosítóval. Az editor
offline próbaponttal megmutatja a felületből számított magasságot.

Ez a következő, koherens előfeltétel a rétegezett mozgáshoz. Production
entity admission, movement, AOI, migration, ghost és hálózati állapot nem
kapott részleges rétegaktiválást. A jelenlegi CharacterController sem
változott. A későbbi login/game kapcsolatú motorprojekt nem része ennek
a mérföldkőnek. Élő szolgáltatás és DB nem lett módosítva.

## Audit: miért nem elég az előző sidecar?

`shared/map/src/LayeredWorldGeometry.cpp` eddig a komponensből XY footprintet
és a floor-min/floor-max köré képzett occupancy Z bandet adott át. A sík
és a normál elveszett. Azonos footprint és azonos band ellenkező irányú
lejtőhöz is tartozhat; a `min_z` nem járófelület-magasság.

Az új shared teszt két ellentétes lejtőt független analitikus oracle-lal
ellenőriz. A régi v2-layout szerint ugyanazt az adatot kapnánk, miközben
ugyanazon XY ponthoz eltérő fizikai magasság tartozik. A motor tesztje
a tényleges Jolt collider raycastját és a normált is összeveti a síkkal.
Egymás fölötti collidereket az oracle izolál: egy közelebb lévő padló
nem fedheti el a vizsgált felület hibáját.

Konkrét baseline-kontroll: footprint `[-4,4) × [-2,2)`, `z=+x/4` és
`z=-x/4`. A régi v2 bytes azonosak; v3 bytes eltérnek. `(0.5,0.5)`-nél
az elvárt eredmény `+0.125 m`, illetve `-0.125 m`. V1/v2-ből egyik síkot
sem lehet bizonyítani; az új query ezekből `NotAvailable`-t ad, nem
találja ki a floor-t. Ez adatvesztési reprodukció, nem production
entitásmozgási benchmark.

`WorldRuntime` production mozgása, warpja és resident transferje továbbra
terrainből állítja a magasságot. Az XY spatial/AOI út és a ghost/packet
nem hordoznak együtt authoritative volume ownershipot. Csak a mozgási
magasság átírása migrációnál visszaejtést és AOI-azonosságütközést okozna.
Ezeket a későbbi bekötésben együtt kell kezelni.

A motor authored világában a canonical átváltás engine `(X,Y-up,Z)` →
server `(X,Y-horizontal,Z-up)` = `(X,Z,Y)`. Az adapter ezt használja.
A régi network viewport eltérő előjel-konvencióját nem terjesztettük át
az új authoring adatokra. Physics collision layer és logikai LayerId
továbbra két külön fogalom.

## Implementáció

### Shared geometria, codec és query

- `shared/map/include/map/LayeredWorld.h`: opcionális `LayerSupportPlane`,
  source/component identity, double anchor/slope és mért Z residual.
- `shared/map/src/LayeredWorldGeometry.cpp`: a tényleges collider vertexekből
  számított sík és maximális abszolút vertex-height residual.
- `shared/map/src/LayeredWorldGenerator.cpp`: az exact út megőrzi a síkot;
  a legacy gap/AABB merge út mindig eldobja a support proofot.
- `shared/map/src/LayeredWorld.cpp`: MX3D v3 encode/decode, record- és
  geometriai validáció; v1/v2 változatlanul beolvasható support nélkül.
- `shared/map/include/map/LayerGroundSupport.h` és a megfelelő `.cpp`:
  explicit `ResolveLayerGroundPlacement` / `ResolveLayerGroundMove`.

A képlet méterben:

```text
z = anchor_z + slope_x*(x-anchor_x) + slope_y*(y-anchor_y)
```

A v3 volume rekord v2 mezői/name után 4 byte jelenlét/reserved jelző áll.
Support esetén 2 u32 és 6 f64, összesen további 56 byte következik.
Portal-layout, manifest, terrain és chunk-verzió nem változott. A sidecar
4 MiB, 4096 volume, 8192 portal korlátja megmaradt. Unknown version,
reserved bit, NaN/Inf, truncation, trailing bytes és hibás proof reject;
failed decode nem publikál részleges világot.

A sík horgonypontja az XY footprint zárt határain belül marad, így a
cooker boundary vertexe megőrizhető. Anchor és mind a négy corner
magassága ± residual a volume min-inclusive/max-exclusive Z bandjében
kell legyen. A residual maximumát a meglévő coplanarity tolerance adja:
`1e-5*sqrt(1+slope_x²+slope_y²)` méter. A tárolt érték a tényleges mért
eltérés, nem szabadon választott mozgási tolerancia vagy fejtér.

A placement nem keres legközelebbi emeletet. Water/underwater,
movement-policy tiltás, ismeretlen vagy hibás support külön eredmény.
Nincs terrain fallback. A move ellenőrzi a current identity/pozíciót,
és csak ugyanazon volume-ban mehet tovább. Minden hiba a current state-et
változatlanul adja vissza. Még egy deklarált portalon át történő váltás is
`TransitionRequired`, amíg nincs explicit fizikai transition validation.

### Szerver

`gameserver/apps/gameserver/src/world/WorldRuntime.{h,cpp}` read-only
`PlaceLayerGround` / `MoveLayerGround` wrapperrel bővült. Hiányzó sidecar
nem válik synthetic járófelületté. A `LoadedWorld` konstruktor a record
capet és a terrain geometriából képzett világ-bounds szerinti validációt
a metadata megtartása és az `InitializeWorld` előtt elvégzi.

Az új `bench/LayerSupportBench.{h,cpp}` a valódi strict writer → loader →
WorldRuntime úton tesztel. A `worldbench --mode layersupport` offline fut;
nem indít auth/session/DB vagy production entitás admissiont.

### Motor és editor

`Client/libs/render/SceneLayerGround.{h,cpp}` egy immutable worldra kötött
adapter. Siker esetén atomikusan megtartja a canonical ground state-et és
az engine `(x,z,y)` megjelenítési pozíciót; hiba megőrzi a korábbi state-et.
A worldnak az adapter teljes élettartamáig élnie kell; temporary worldhoz
kötés tiltott. Új bake-hez új adapter és explicit Place szükséges.

`Tools → Layered world → Support probe (offline)`:

1. Generált Support volume és engine X/Z pont megadása.
2. `Place support probe`: magasság, volume/layer és engine X/Y/Z státusz.
3. `Move support probe`: ugyanazon volume-ban frissít; határ/volumeváltás
   esetén megtartja a korábbi helyet.
4. Scene-módosítás után a bake-azonosság ellenőrzése elutasítja a move-ot,
   elengedi a korábbi probe/worldot és új Place-t kér.

A bake-azonosság tényleges, nem üres MX3D encodingek összehasonlítása,
nincs mesterséges revision token. A codec által nem reprezentálható bake
Place előtt is reject. Scene-load és shutdown a probe-ot a hozzá tartozó
world előtt engedi el. A művelet gombnyomásonként fut, nem frame-enként.
Ez státuszt adó authoring eszköz; nem játékosmozgatás vagy marker-renderer.

## Review közben reprodukált hibák és javításuk

1. Távoli, hatalmas finite anchor (`1e308`) cancellation miatt egy hamis
   flat surface-t is elfogadtathatott volna. A cooker valódi vertexhez kötött
   anchor-korlátját a reader/query is ellenőrzi. Remote anchor és finite
   cancellation negatív kontroll, zárt maximum-határi anchor pozitív kontroll.
2. A scene-generátor által engedett túl hosszú name codec-hibát okozhat.
   Két üres encoding egyszerű equality-je téves bake-azonosságot jelentene.
   A motor explicit nonempty encodinget követel. Long-name + megváltozott
   plane negatív kontroll a tényleges adapter-azonossági út ellenőrzése.
3. Az észlelt új bake után a régi probe megtartása byte-identical visszaálláskor
   Place nélkül folytatást engedett volna. Az észlelt eltérés elengedi azt.

## Build, tesztek és GUI bizonyíték

Evidence könyvtár: `Client/build/layer-support-20260930/` (ignored).
Az első build és a javítások utáni build külön naplóban szerepel.

Windows Debug build PASS: `gameserver`, `worldbench`, `IxtreemeEngine`,
az alábbi shared és engine tesztcélok. Végső naplók:
`server-build-final.log`, `engine-build-final.log`, `engine-test-build.log`.
A korábbi MSVC/dependency warningok nem buildhibák; warningmentes buildet
nem állítunk.

| Teszt | PASS | FAIL | SKIP |
|---|---:|---:|---:|
| Új `layer_ground_support_test` | 77 | 0 | 0 |
| Új `SceneLayerGroundTest` | 64 | 0 | 0 |
| Új `worldbench --mode layersupport` | 31 | 0 | 0 |
| `SceneLayerAuthoringTest` | 71 | 0 | 0 |
| `SceneWorldPackageTest` | 71 | 0 | 0 |
| `terrain_surface_contract_test` | 31 | 0 | 0 |
| `layered_world_geometry_test` | 46 | 0 | 0 |
| `layered_world_contract_test` | 9 | 0 | 0 |
| `layered_world_package_test` | 10 | 0 | 0 |
| `layered_world_generator_test` | 6 | 0 | 0 |
| `layered_spatial_key_test` | 7 | 0 | 0 |
| `engine_server_map_compatibility_test` | 9 | 0 | 0 |
| `worldbench --mode layerlookup` | 26 | 0 | 0 |
| `worldbench --mode worldpackage` | 140 | 0 | 1 |
| `worldbench --mode terrain` | 20 | 0 | 0 |
| GUI-export `SceneLayerGroundTest --validate-package` | 10 | 0 | 0 |
| **Összesen** | **628** | **0** | **1** |

Az egy skip a meglévő Windows directory-symlink kontroll jogosultsági
korlátja. A CPU-csomag 618 PASS; a külön GUI-package-validáció további
10 PASS. Nincs NOT_RUN Jolt oracle ezen a builden. A geometriai eltérés
ellenőrzése `1e-4 m` height és `1e-4` normál komponens teszttoleranciával
futott; ez teszt-oracle tolerancia, nem runtime current-pose tolerancia.

A három új célzott ellenőrzés reprodukálható a repo gyökeréből:

```powershell
& './gameserver/build/windows-debug/shared_map_build/Debug/layer_ground_support_test.exe'
& './Client/build/libs/render/Debug/SceneLayerGroundTest.exe'
& './gameserver/build/windows-debug/apps/gameserver/Debug/worldbench.exe' --mode layersupport
```

A GUI export megőrzött könyvtárára:

```powershell
& './Client/build/libs/render/Debug/SceneLayerGroundTest.exe' --validate-package './Client/build/layer-support-20260930/gui-project/terrain-stacked.server-worlds/export-17907898189383614'
```

Nem szükséges szolgáltatás, hitelesítő adat vagy settle-várakozás. A CPU
fixture-ek friss, saját temporary könyvtárban készülnek. A tesztcélok
CMake-listái a megfelelő shared-map/render/worldbench buildben szerepelnek.

### Valódi editor GUI-próba

Computer Use plugin, saját frissen buildelt editor, friss izolált
`gui-project/project.ixproj`. Az eredeti fixture másolata használatos;
nincs felhasználói scene/asset felülírás. Három collider azonos
`[0,16) × [0,16)` footprinttel, Box/Mesh/Box, road/bridge/underpass taggel.

| GUI művelet | Eredmény | Megtartott engine X/Y/Z |
|---|---|---|
| Place volume 1, X/Z = 0/0 | `ok`, layer 1 | `(0,-6,0)` |
| Place volume 3, X/Z = 0/0 | `ok`, layer 3 | `(0,6,0)` |
| Move volume 3, X/Z = 1/0 | `ok` | `(1,6,0)` |
| Move volume 2, X/Z = 1/0 | `transition_required` | `(1,6,0)`, volume/layer 3 |
| Export strict server world | 5 fájl, sikeres | új csomag |
| Normál X-bezárás | exit **0**, saját ablak eltűnt | nincs kényszerített kill |

A half-open határ, water és scene-edit invalidation negatív kontrolljai
a CPU-tesztekben futottak; teljes GUI inspector-edit invalidation próbát
nem állítunk. A státuszmezők képernyőn, a műveletek az
`engine-gui-stderr.log` `[LAYER-SUPPORT]` soraiban ellenőrizhetők.

Exportált csomag:
`gui-project/terrain-stacked.server-worlds/export-17907898189383614`.
Az új sidecar **348 byte**, az azonos korábbi három-volume v2 sidecar
**168 byte** volt: volume-onként 4 byte flag + 56 byte plane, összesen
**+180 byte**. A loader Full/Eager/Strict módban mindhárom supportot
visszaolvasta, centre placement és same-volume move is sikerült:
volume/source `1/3 → -6 m`, `2/1 → 0 m`, `3/2 → 6 m`;
mindegyik component 1 és mért vertex residual **0 m**. Napló:
`gui-package-support-validation.log`.

A terrain-export külön mért maximum height quantization errorja továbbra
**0.00005 m**, grid position error **0 m**. Ez nem a support-plane residual.
Production cache/timeout/populáció/acceptance-küszöb nem változott.
A query lineáris volume-scan, allocation és terrain I/O nélkül; a GUI
explicit művelete encode/cook munkát is végez. Új performance- vagy teljes
process-RAM ígéretet nem mértünk. Production scheduler/20 Hz változatlan.

A GUI stderr **30 Vulkan validation error** sort tartalmazott a korábban
nyitott swapchain/presentable-image layout problémáról. A collision/query/
package tesztek PASS eredménye nem renderer PASS. A fixture magenta
fallback anyaga szándékosan nem asset-kompatibilitási bizonyíték.
Az editor saját futása által módosított layoutot az indítás előtt
elmentett byteokra állítottuk vissza; Git blob hash mindkét oldalon
`a75e473e459f6bec57ec1d73ede757f49ebb4034`.

## Korlátok és következő munka

- A reader authored/cooked proofot validál; a teljes eredeti modellmesh nincs
  a sidecarban, ezért betöltéskor nem recookol és nem ellenőrzi újra az assetet.
- A rectangular, coplanar upward component a jelenlegi geometriai kör.
  Fal, teljes blocking collision, capsule/headroom, összetett lépcső,
  navigation és portal physics továbbra hiányzik.
- A query double ground state-et használ. Production Vec3 float és wire
  kvantálás előtt a tárolási pontosságot és a support state authoritative
  forrását külön bizonyítani kell; a residual toleranciát itt nem lazítottuk.
- Source/component/volume ID egy bake-hez kötött. Structural rebake
  átszámozhat; persistent entity ownershiphoz világverzió-szerződés kell.
- A jelenlegi `CanTraverse` metadata API nem production portal traversal;
  a ground move nem aktiválja és nem támaszkodik rá.
- Release, Linux/Android, live kétklienses E2E és nagy populációs benchmark
  nem része ennek az offline mérföldkőnek.
- A korábbi Vulkan layout-validation jelzések nyitottak; ez a munka nem
  renderer elfogadás és nem renderer-refaktor.

Következő koherens lépés: modell blocking/clearance és explicit transition
proof, majd együttes authoritative volume ownership + movement + migration
+ spatial/AOI/ghost/replication bekötés a motor ellenoldalával. A külön
login/gameserverhez kötött motor tesztprojekt akkor tud valódi entitás-
és hálózati rétegviselkedést igazolni. Ez a riport az offline 3D-4A lezárása.
