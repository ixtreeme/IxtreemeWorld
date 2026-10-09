# 3D-5B1 – rétegzett játékos-spawn a világcsomagban (review, 2026-10-01)

Állapot: **kész, review-ra vár.** Alap: `952f493` (3D-5A).

## Cél

A 3D-5A után rétegzett szintre csak debug override-dal lehetett belépni.
A 3D-5B1 után a csomag **játékos spawn régiója** is állhat egy rétegzett
volume-on, így az éles belépési szabály (első spawn régió közepe) a
játékost közvetlenül a szintre teszi. A terep↔szint átmenet külön lépés
(3D-5B2).

## Szerződés

- `mx::map::SpawnRegion::volume_id` (0 = terep).
- **worldlogic (`MXL1`) v2:** minden spawn rekord végén u32 volume id. Csak
  akkor íródik, ha van rétegzett spawn; különben a fájl bájtra azonos v1.
  Az olvasó (szerver-loader és a régi engine-oldali `LoadWorldLogic`) v1-et
  és v2-t is elfogad; a 3-as az első ismeretlen verzió.
- **Szigorú loader-keresztellenőrzés** (a sidecar a worldlogic előtt
  dekódolódik): a volume létezzen clearance-sütéssel, és a
  `ResolveLayerActorPlacement` a sütött actort engedje a régió közepén.
  Hiba: `WORLDLOGIC_SPAWN_VOLUME_INVALID` (417). Rétegzett spawnra a terep
  járhatósági szabálya nem vonatkozik (egy szint alatt lehet blokkolt
  terep).
- **Writer:** rétegzett spawn csak v3 csomagban, sidecarral.
- **Szerver:** `SpawnCoordinator` az első spawn régiót, ha volume-hoz kötött,
  rétegzetten helyezi el (`LayerPresence`, z = sík); nincs terep-visszaesés.
- **Editor:** *Export strict server world* mellett új **Player spawn
  volume** mező (0 = terep); az export a pontos pontra az actor-elhelyezést
  is ellenőrzi. A mező a `MergeMapEditorCommands`-on is átmegy.

## Tesztek (lefuttatva, Windows, Debug)

| Csomag | Eredmény |
|---|---|
| `worldbench --mode layeredpresence` | **48/48** (39 korábbi + 9 új) |
| – éles spawn (override nélkül) a híd-szintre, z = 6, audit OK | PASS |
| – ismeretlen volume / falban / volume-on kívüli közép → 417 | PASS |
| – blokkolt terep-cellán terep-spawn továbbra is 416 | PASS |
| – rétegzett spawn sidecar nélkül → a writer elutasítja | PASS |
| – terep-spawn → worldlogic v1 | PASS |
| `SceneWorldPackageTest` (engine export) | **82/82** (11 új) |
| `worldbench --mode worldpackage` | 141/141 + 1 korábbi SKIP |
| `map4`, `mapaudit`, `mapsplit`, `terrain`, `layersupport`, `layerclearance` | failures=0 |
| shared map tesztek (clearance, ground support, terrain, geometry, compat, contract, package, spatial key, generator) | mind PASS |
| editor (`IxtreemeEngine`) Debug build | sikeres |

A korábbi `worldlogic-bad-version` korpusz-eset a 2-es verziót
„ismeretlenként” használta; ez most a v2 elrendezés, ezért az eset a 3-as
verziót kapta, és új eset ellenőrzi, hogy v1 bájtok v2 fejléccel csonkolt
fájlként buknak el.

## Hatókör-bővítések (review-ra jelölve)

1. Worldlogic formátum v2 és új hibakód (417).
2. `SpawnRegion` új mezője (alapértelmezett 0, aggregát inicializálás
   változatlan).
3. Editor: új menümező, `MapEditorCommands.serverWorldSpawnVolume`,
   `SceneWorldExportOptions.spawnVolumeId`.

## Korlátok

- Továbbra sincs terep↔szint átmenet: a rétegzett spawnról indulók a
  szinthálózaton maradnak (3D-5B2).
- Csak az *első* spawn régiót használja a szerver (a meglévő szabály).
- Az editorban a volume id-t a generálási státuszlistából kell kiválasztani;
  a generált id-k sütésenként stabilak, de szerkezeti átsütés után
  változhatnak.
- Kliens nem fogyasztja még a volume-mezőket (3D-5C).
- Csak Windows/MSVC Debug futott.
