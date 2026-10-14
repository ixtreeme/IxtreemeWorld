# 3D-5B2 – bizonyított terep↔szint átmenet (review, 2026-10-01)

Állapot: **kész, review-ra vár.** Alap: `6739d44` (3D-5B1).

## Cél

A 3D-5A/5B1 után a rétegzett szintek a terepről nem voltak elérhetők, és
róluk sem lehetett lelépni a terepre. A 3D-5B2 bizonyított terep-éleket
vezet be: a terepszintű padló (pl. egy 0,2 m-es plaza) oldalain a játékos
fel- és lesétálhat.

## Szerződés

- **`LayerTerrainEdge`** (shared, `LayeredWorld`): kötet-footprint
  oldalanként egy rekord, a teljes oldalra. A cook slotonként (0,25 m)
  bizonyítja, hogy a terep az élszakaszon a sík lépésmagasságán belül van
  (konzervatív quad-sarok korlátokkal); a bizonyított kötet–kötet portálhoz
  tartozó slotok kimaradnak. A kötet-oldali folyosó (sugár + cella befelé)
  cellánként jelzi, hogy a kapszula szabad-e a sík és a helyi terep-maximum
  felső burkolója fölött; a lépéspróbán elbukott slotok minden cellája
  blokkolt. Ha egy oldalon egyetlen él-menti cella sem szabad (fal áll
  rajta), nincs rekord.
- **MX3D v5:** a terep-élek a portálok után (darabszám + rekordok,
  bitcsomagolt folyosó, nulla tartalékbájtok). Csak terep-élekkel íródik;
  különben v4/v3 bájtra azonos. Validáció: a kötet sétálható és van
  clearance-e, az él pontosan a footprint határán van, a span a footprinten
  belül, a terep-oldal a világon belül, a folyosó mérete egyezik, a lépés
  ≤ a profilé, egy oldal élei nem fednek át, az azonosítók egyediek.
- **Járhatóság:** a terep-él tiszta folyosócellája a kötet számára
  járható (mint a portál-folyosó).
- **`ResolveLayerActorExitToTerrain`** / **`ResolveLayerActorEnterFromTerrain`**
  csak a kötet-oldalt bizonyítja: az első átlépett terep-él a spanon belül,
  az átlépési ponton az él melletti folyosócella szabad, a szakasz kötet
  felőli része járható, belépéskor az actor elfér a célponton.
- **Szerver (`MovementSystem`):** a terep-oldalt a saját, autoritatív terepe
  alapján ellenőrzi: a terep-útvonal tiszta az átlépési pont és a terepes
  végpont között (`CheckStep`), és a terep magassága az átlépési pontban a
  sík lépésmagasságán belül van. Nem rezidens chunk esetén igényli a chunkot,
  és az átlépés vár (nem talál ki semmit). A bizonyított kötet-portál mindig
  előbb jön, mint a terepre kilépés. Játékos beléphet; rétegzett játékos és
  mob kiléphet; terepes mob nem lép be. Új számlálók:
  `layered_terrain_entries_total`, `layered_terrain_exits_total`.
- **Editor:** a generálási státusz kiírja a terep-élek számát; a *Show layer
  volumes* cián vonallal rajzolja az átléphető él-slotokat. A
  `SceneLayerGroundTest --validate-package` v5-öt is elfogad, és minden
  terep-élre ellenőrzi, hogy a sütött actor elhagyhatja-e.

## Tesztek (lefuttatva, Windows, Debug)

| Csomag | Eredmény |
|---|---|
| `layer_clearance_test` (shared) | **85/85** |
| – terep-él független orákulum (sík + helyi terep-maximum burkoló) | 45 550 minta, **0** sértés |
| – 3 nyitott oldal kap élt, a falas oldal nem; részben falazott oldal | PASS |
| – kilépés/belépés nyitott élen, falas slot `blocked`, nem-belépés `not_available` | PASS |
| – v5 kódolás + roundtrip, determinisztikus sütés, manipulációk elutasítva | PASS |
| – 0,5 m-es platform: nincs él, v4 marad; terep-adat nélkül nincs él | PASS |
| `worldbench --mode layeredpresence` | **57/57** |
| – determinisztikus: terepes játékos fel a platformra (z = 0,2), le a túloldalon (z = 0), 1 belépés + 1 kilépés | PASS |
| – falas oldalon nincs belépés; rétegzett játékos nem lép ki a falon át | PASS |
| – terep-élek nélkül nincs belépés | PASS |
| – valódi futó runtime: spawn a terepen → át a platformon → vissza a terepre, audit OK | PASS |
| `SceneLayerAuthoringTest` | **96/96** (új: terepszintű padló 4 terep-éle) |
| `SceneLayerGroundTest`, `SceneWorldPackageTest` | 64/64, 82/82 |
| worldbench regresszió (20 mód: layeredpresence, aoi, ghost, border, replication, replv2, hygiene, presence, protocol, snapshot, activitytemporal, mapsplit, terrain, worldpackage, map4, mapaudit, layersupport, layerlookup, layerclearance, splitmerge) | mind failures=0 |

**Valódi editor GUI-próba** (Computer Use, izolált projekt
`Client/build/layer-terrain-edge-20261001/gui-project`: 0,2 m-es plaza
falas +Z oldallal, 1 m-es emelt fedélzet): *Generate* → „Proven terrain
edges: 3”, a falas oldalon blokkolt cellák, a fedélzetnek nincs éle;
*Player spawn volume* = 1 a plazán → export OK (**MX3D v5**, **worldlogic
v2**); a validátor 13/13 (3 terep-él, mind elhagyható); volume = 2 a plazán
→ export elutasítva `WORLDLOGIC_SPAWN_VOLUME_INVALID(417) … outside_volume`.
Natív X-bezárás exit 0, `editor_layout.ini` visszaállítva. A stderr 26
Vulkan validation error sora ugyanaz a korábbi renderer-hiba.

## Hatókör-bővítések (review-ra jelölve)

1. MX3D v5 formátum, `LayerTerrainEdge`, `LayeredWorld::terrain_edges`.
2. Két új shared lekérdezés és `LayerTerrainCrossing` típus.
3. **Viselkedésváltozás:** azokban a jelenetekben, ahol egy padló a terep
   szintjén van, a generálás mostantól terep-éleket süt, és az export v5.
4. `LayerClearanceReport::terrain_edges_derived`; az `edges_rejected_step`
   számláló mostantól a terep-slot elutasításokat is számolja.
5. Szerver: terepes játékosok belépése, rétegzettek kilépése; két új
   diagnosztikai számláló.

## Korlátok

- A terepes entitásoknak továbbra sincs modell-ütközésük: ha a falas
  oldalon nincs belépés, a terepes játékos a terepen továbbsétál a padló
  alatt (a régi viselkedés).
- A lépéspróba konzervatív (quad-sarkok): durva terep-rácson egy enyhe
  lejtőn lévő érvényes él is kieshet.
- A terep-útvonal ellenőrzés iránya belépéskor az átlépési pontból a
  terepes pont felé fut (`CheckStep` szimmetrikusnak tekintve).
- Terepes mob nem lép be kötetbe; mob spawn pontok terepesek.
- Minden terep-él minden terepes játékos minden lépésénél lineárisan
  vizsgálva (nincs térbeli index); nagy világon mérni kell.
- A független orákulum lassú (~2,5 perc), ezért a fixture terepe ritka.
- Kliens nem fogyasztja a volume-mezőket; a transform-frame nem jelez
  volume-váltást (3D-5C).
- Csak Windows/MSVC Debug futott.

## Következő lépés

3D-5C: a volume-váltás a transform-frame-ben (protokollverzió-emelés) és a
kliensoldal (standalone kliens + engine hálózati kliens), majd összekötött
engine-tesztprojekt valódi login- és gameserverrel.
