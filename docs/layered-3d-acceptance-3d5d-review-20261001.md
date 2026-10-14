# 3D-5D – a 3D-5 lezáró acceptance-e (review, 2026-10-01)

Állapot: **kész.** Az automatikus tesztek zöldek, és az élő, kétklienses, adatbázisos acceptance két
futásban lement. Az első futás három hibát talált, ezeket javítottam, a második futás tiszta.
Alap: `215dd4c` (3D-5C2).

## Cél

A 2026-09-30-as authoring-riport 6. pontja szerinti lezárás: kétklienses, többszintű mozgás, megállás,
AOI, kilépés és migráció, a legacy úttal együtt. A felhasználó két további kérése:

- **Peremerózió:** a játékos ne lógjon a nyitott perem fölé (felhasználói döntés: erózió).
- **Textúrák:** a jelenetben minden tárgynak legyen textúrája, hogy jól kivehető legyen, mi hol van.

## Változások

### 1. Peremerózió (shared, `LayerClearance`)

- A bake a portálok és a terep-élek után egy alátámasztás-lefedési lépést futtat
  (`ErodeOpenLedges`). Egy még szabad cella (saját rács, portál-sáv, terep-él sáv) blokkolt lesz,
  ha a sugárral bővített cellanégyzet nincs teljesen lefedve. A lefedést adja:
  - a saját lábnyom,
  - a bizonyított portálon át elérhető volume-ok lábnyoma (portál-sávnál mindkét végéé),
  - a terep az érvényes terep-él slotok mögött.
- A volume sarkánál a terep-él sávot sugárnyival meghosszabbítom, ha a sarokterület terepe a
  lépésmagasságon belül van, így két terep-él sarka nem erodálódik.
- A riport új számlálói: `cells_eroded`, `corridor_slots_eroded`. Az editor rétegezési
  üzenetében „N ledge cells” jelenik meg.
- **Mellékhatás (konzervatív):** portál mellett a futásidejű `BandCovers` a cellahatáron a szomszéd
  sávslotot is nézi, ezért ott az erodált sáv egy cellával szélesebb (a 3 m-es lépcsőn kb. 1.3 m
  szabad szélesség marad).
- A formátum nem változott. A 3D-5D előtt cookolt csomagok középpont-alapúak maradnak, amíg újra nem
  exportálják őket.

### 2. MX3D-dekóder hiba (shared, meglévő 3D-5B2 hiba)

A terep-él rekordok alsó méretbecslése 44 bájt volt, a valódi minimum 41 (40 fix bájt + 1 bájtos
bitset). Kis sávú éleknél (≤ 8 cella, pl. 0.75 m mély lépcsőfok) emiatt a csomag szigorú validálása
„terrain edge count exceeds the limit” hibával elutasította a teljesen helyes exportot. Javítva, és
új regressziós teszt fedi (`acceptance-scene-cooks-encodes-and-round-trips`).

### 3. Szerver: ghost-kezelés (az élő acceptance találta)

- **Zombi-spawn:** egy elalvó zóna (amelynek nincs már játékosa) soha többé nem publikál, így az
  utolsó border-publikációja (benne az épp elvándorolt vagy kilépett játékossal) megmaradt. A
  szomszéd ghost-egyeztetés ebből később „feltámasztotta” az entitást elavult pozícióval: kilépés
  után a megfigyelő `despawn`, majd `spawn` üzenetet kapott 2 perccel korábbi pozícióval.
  - Javítás: `BorderPublisher::RetractNonResidents` elalváskor (a ZoneScheduler a zónát tickként
    foglalja). Csak a már nem rezidens snapshotok esnek ki, az alvó mobok publikációja marad.
- **Migrációs villanás:** a forrászóna megfigyelői egy tickig se rezidensként, se ghostként nem
  látták az elvándorolt entitást (`despawn` + `spawn`). A meglévő `migration-no-spawn-despawn-churn`
  teszt csak a célzóna megfigyelőit nézte.
  - Javítás a migráció-commitban: a forrás visszavonja a publikációt, a célzóna azonnal publikálja
    az új rezidenst (`PublishResident`), a forrászóna pedig azonnal ghostot kap
    (`GhostSystem::AdoptMigratedGhost`). Játékosra és mobra egyaránt.

### 4. Engine (editor)

- **Hierarchia-összeomlás:** ha egy script ugyanabban a frame-ben töröl és spawnol azonos nevű
  objektumot (proxy-csere), a flecs-hierarchia a még nem törölt elődre ütköző nevet kapott, és a
  flecs abortált. Ez okozta a 2. kliens leállását. Javítás: a nevek beállítása az elavult
  hierarchia-entitások törlése után történik. 80 egy-frame-es churn-ciklussal ellenőrizve, összeomlás
  nélkül.
- **Static mesh anyag-batch:** az instanced draw a textúra-descriptort és a world uniformot az
  **első** példányból kötötte be, így egy mesh minden példánya ugyanazt a textúrát kapta (pl. minden
  kocka téglafal-textúrát). Javítás: a draw azonos anyagú, egymás utáni példányszakaszokra bomlik.
- **Anyag `uvTiling`/`uvOffset`:** a static mesh renderer csak az inspector-override-ból vette át,
  az anyag-assetből nem. Javítva.
- **Script API v5:** `IScriptApi::SetMaterial(id, slot, material)` (késleltetett, mint a spawn),
  `NativeScript::SetMaterial(slot, material)` és Lua `SetMaterial`. A modul ABI 5-re nőtt.

### 5. Script (`MmoClient.cpp`)

- `proxyMaterial`: minden proxy textúrát kap.
- `protocolVersion` (1 vagy 2): régi kliens szimulálása.
- A státuszsor v1/v2 frame-eknél „volume n/a (protocol 1)”-t ír az elavult volume helyett.

## Tesztek (lefuttatva, Windows)

| Teszt | Eredmény |
|---|---|
| `layer_clearance_test` | **103/103**: peremgyűrű pontosan (496/4096), pihenő-vég/oldal, lépcsőfok-oldalak, terep-él sarok, terep-él nélküli perem, független alátámasztás-oracle 8 fixture-ön (≈116 ezer póz, 0 kilógó), negatív kontroll (r=0.6 → 524 kilógó), acceptance-jelenet round-trip |
| többi shared map teszt (8 db) | mind failures=0 |
| `SceneLayerAuthoringTest` / `SceneWorldPackageTest` / `SceneLayerGroundTest` | 96/96, 82/82, 64/64 |
| `worldbench --mode layeredpresence` | **64/64**, 3× egymás után. Új: `despawned-layered-player-is-not-respawned-to-observers`, `layered-migration-out-and-back-without-churn-for-source-zone-observer` (javítás előtt `S D S D S`, utána `S D`) |
| worldbench regresszió, 22 mód (aoi, ghost, border, replication, replv2, hygiene, presence, protocol, snapshot, activitytemporal, mapsplit, terrain, worldpackage, map4, mapaudit, layersupport, layerlookup, layerclearance, splitmerge, inputpath, netstress, scriptwire) | mind failures=0 (activitytemporal: APPROVED; map4: 37/0) |

## Élő acceptance (két editor, valódi login- és gameserver, meglévő DB, a felhasználó két teszt-fiókja)

Környezet (minden új adat a `Client/build/layer-acceptance-3d5d-20261001/` alatt, a régi
csomagok és konfigok érintetlenek):

- **Világ:** 512×512 m sík terep 8 m-es rácstextúrával. 13 textúrázott tárgy (padló 1 m-es
  csempével, fal, gerendák, E1–E4, pihenő, S1–S3 déli lépcső a terepre), cián „ME” játékos, lila
  „OTHER” proxy.
- **Bake:** 9 volume, 8 bizonyított portál, 3 terep-él, 560 erodált peremcella. Spawn: (4, −5), 4-es
  volume (padló).
- **Szerver:** konfigmásolat `partition_regions=1x1`, `partition_initial_leaves=2x2` beállítással.
  4 zóna, a határok (x=0, y=0) átmennek a rétegzett padlón.
- **Kliensek:** 1. kliens protocol 2 (`ixtreeme`), 2. kliens protocol 1 (`hexakill`, régi klienst
  szimulál). Külön editorpéldányokban futottak, egymás mellett.

### 1. futás (hibakereső)

| Elem | Eredmény |
|---|---|
| belépés, rétegzett spawn (vol 4), v3 frame a v2-nek, v2 frame a v1-nek | PASS |
| rétegzett migráció a padlón (2→3, 3→2) | PASS |
| mászás 4→5→6→7→8→9 (pihenő) | PASS |
| v1 kliens le a déli lépcsőn a terepre; a v2 kliens látja: `net 1 level change 4→3→2→1→0` | PASS |
| AOI-kilépés (~120 m), majd -belépés `volume 0`-val, utána `0→1→2→3→4` vissza a padlóra | PASS |
| kilépés/visszalépés (új net id) | PASS |
| **migrációs villanás** a forrászóna megfigyelőjénél | **FAIL** → javítva (3. pont) |
| **zombi-spawn** kilépés után, régi pozícióval | **FAIL** → javítva (3. pont) |
| **2. kliens editor-összeomlása** (azonos frame-ben despawn + spawn) | **FAIL** → javítva (4. pont) |

### 2. futás (ellenőrzés a javításokkal)

| Elem | Eredmény |
|---|---|
| 1. kliens három rétegzett migrációja (2→3, 3→1, 1→2) | PASS |
| a 2. kliens (forrászóna, v1) eseményei az 1. kliensről: pontosan `spawn`, majd kilépéskor `despawn` | PASS (nincs villanás, nincs zombi) |
| mindkét editor szabályosan lép ki (exit 0) | PASS |
| v1 státusz: „volume n/a (protocol 1)” | PASS |

A belépési adatok csak a bekérőablakba kerültek, sehol nem tároltam és nem logoltam őket.

## Hatókör-bővítések (review-ra jelölve)

1. Szerveroldali ghost- és migrációs javítások (BorderPublisher, GhostSystem, MigrationCoordinator,
   ZoneScheduler). Általános hibák, nem csak rétegzettek; az acceptance találta őket.
2. Engine renderer: anyagonkénti batch-bontás és az anyag-asset UV-transzformja.
3. Editor-hierarchia: a flecs-nevek késleltetett beállítása.
4. Script ABI v5 (`SetMaterial`); a v4-es modulokat újra kell fordítani.
5. MX3D-dekóder: a terep-él minimum rekordmérete 44 helyett 41.

## Ismert, nem javított

- **Editor autosave a terep-paletta textúráját elhagyja:** az editor autosave-je a pontos
  magasságfájlos terepet saját chunk-manifestre állítja át. Újratöltéskor ezzel a paletta-textúra
  elveszik, és a terep szürke lesz. A tesztben a jelenetet scripttel újragenerálva kerültem meg;
  meglévő editor-viselkedés, külön feladat.
- **Uniform-slot keret:** a static mesh renderer frame-enként legfeljebb 64 rajzolásnyi
  uniform/descriptor slotot használ. Sok különböző anyagú példánynál ezt a batch-bontás hamarabb
  elérheti, és az utolsó slot megosztottá válik; ez a keret meglévő korlátja.
- A szerver migrációs hiszterézise miatt a határ közelében nem minden átlépés jár zónaváltással (ez
  elvárt viselkedés).
