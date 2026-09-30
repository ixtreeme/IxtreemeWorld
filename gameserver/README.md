# GameServer

GameServer is a C++20 networking skeleton for the new MMORPG server project.

## Dependencies

Dependencies are managed through vcpkg manifest mode:

- Boost.Asio
- Boost.System
- Cap'n Proto
- MariaDB Connector/C++
- libsodium
- nlohmann-json
- spdlog
- fmt

Set `VCPKG_ROOT` to your vcpkg checkout before configuring.

For classic vcpkg installs on Windows:

```sh
vcpkg install capnproto:x64-windows
vcpkg install mariadb-connector-cpp:x64-windows-static
vcpkg install libsodium:x64-windows-static
vcpkg install nlohmann-json:x64-windows-static
```

On FreeBSD:

```sh
pkg install capnproto
```

On Debian/Ubuntu:

```sh
apt install capnproto libcapnp-dev
```

## Database Setup

Create the schema:

```sh
mysql -u root -p < libs/db/schema/schema.sql
```

Copy an Argon2id password hash into `libs/db/schema/seed_test_account.sql`
in place of `REPLACE_ME`, then seed:

```sh
mysql -u root -p < libs/db/schema/seed_test_account.sql
```

## Build on Windows

```sh
cmake --preset windows-debug
cmake --build --preset windows-debug
```

## Build on Linux/FreeBSD

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
```

## Run

```sh
./build/<preset>/apps/gameserver/gameserver --config gameserver.conf
```

On Windows with the Visual Studio generator, the executable is under the selected
configuration directory, for example:

```sh
./build/windows-debug/apps/gameserver/Debug/gameserver.exe --config gameserver.conf
```

The world comes from runtime configuration only (see
`config/gameserver.conf.example`): `world_mode=file` (default) needs
`world_package` (relative to the config file) or `--world-package <dir>`
(relative to the working directory) plus `mob_types_config` / `--mob-types`.
A missing or invalid package stops the process before the database or the
listener is touched (exit 3); there is no fallback world. `world_mode=synthetic`
starts an explicitly marked flat benchmark world. Package format and rules:
`docs/map-data-format.md`.

The world is the package's half-open rectangle `[min, max)` (any finite
origin, non-square allowed; +X east, +Y north). The server partition is server
configuration over those bounds, not map data: `partition_regions` (default
`2x2`) x `partition_initial_leaves` (default `1x1` per region); every initial
leaf must be >= 240 m (2 x AOI radius) and there may be at most 1024 initial
zones, or startup stops with exit 2. The package's worldlogic areas are
metadata with their own id space. Players enter at the package's first player
spawn region centre (a debug override first in dev builds; the stored DB
position is not used); without a usable one the enter is refused. Outside the
world there is no ground: movement refuses the step (no clamp), terrain queries
answer `OutsideWorld` / `NotResident` instead of 0 m.

Terrain residency (`terrain_residency`): `eager` (default) keeps the whole
server terrain resident; `streaming` loads chunks on demand with dedicated
I/O threads inside `terrain_cache_budget_mb`, validates each chunk on load and
evicts least-recently-used ones (the spawn-region and warp-target chunks stay
resident). Movement checks the whole path of each step against the blocking
grid (plus optional slope / deep-water rules) and waits in place for a chunk
that is still loading -- a step inside the entity's own cell too, since its
height becomes z. A chunk that fails to load 3 times is published INVALID for
the package generation: never height 0, never walkable, never retried. Server water comes from the package's water
declaration (none / sea level / bodies); an undeclared package answers
"unknown", never "land". A grid path query (`WorldRuntime::PostNavigationRequest`)
is available as infrastructure. Details: `docs/map-data-format.md` 12.

```sh
gameserver --validate-world-package <dir> [--mob-types <file>]   # offline, every layer
gameserver --config <conf> --startup-check                       # full startup, then exit
```

## Test

The world simulation is exercised by `worldbench` (built when
`ENABLE_WORLDBENCH=ON`, e.g. the `windows-relwithdebinfo` preset). Each mode
prints `...: PASS|FAIL` lines and ends with `BENCH-DONE <mode> failures=N`.

```sh
./build/windows-relwithdebinfo/apps/gameserver/RelWithDebInfo/worldbench.exe --mode <mode>
```

- Systems: `lod`, `activity`, `loadfield`, `partitionscore`, `stability`,
  `splitmerge`, `ghost`, `aoi`, `replication`, `scheduler`, `spread`,
  `hotspot`, `border`, `readiness` (`--scenario spread|dense|... --players N
  --mobs N --warmup S --seconds S`), plus the `--field-selftest`,
  `--loadfield-selftest`, `--partitionscore-selftest`, `--routing-selftest`
  flags.
- Infrastructure hardening: `tickrate` (authoritative 20 Hz vs input rate),
  `inputpath`, `netstress` (`--net-io-threads N`), `presence` (one character
  = one presence), `asfdeterminism`, `workerpool`, `replv2`, `protocol`
  (malformed/adversarial input), `reclamation` (`--cycles N` split/merge
  cycles), `hygiene`.
- Map data layer: `mapaudit` (legacy loader vs server path per R1-R13),
  `worldpackage` (package corpus through the server loader; `--fixtures-out
  <dir>` also writes startup fixtures), `snapshot` (`--cycles N`: bench
  snapshots under split/merge/reclaim), `terrain` (heights against an
  independent oracle, seams, partial chunks, world edge, NotResident vs 0 m,
  runtime edge rules), `mapsplit` (split 1 -> 4 -> 16 and merge back from the
  real loader: no terrain reload, no entity duplication, min-size refusal
  diagnostic), `bootstrap` (`--grid N`: startup cost of up to N x N initial
  zones, the aggregate bootstrap caps), `streaming` (chunk streaming, budget,
  lifetime, runtime integration vs a resident reference), `worldquery`
  (collision / water / navigation vs oracles), `streamsoak` (`--cycles S`,
  `--budget-mb M`: 100 km world, small cache, roaming players, full memory /
  I/O accounting), `streamlife` (lock-free readers vs eviction with poisoned
  frees and a broken-protocol negative control, late completion across
  migration / retire / reclaim / slot reuse, sleeping zones, permanently
  failed chunks, startup set above the budget). The real startup path is exercised by
  `scripts/map1_startup_acceptance.sh`.

Benches read a running world only through supervisor snapshots
(`bench/BenchSnapshot.h`, `ReadWorld`), never through `sim.Zones()` directly.

## Login server

Authentication (login, character list, handoff token issue) lives in the
separate `loginserver/` application; the gameserver only consumes the
handoff token on `EnterWorld`.

### MAP-4 ellenőrzés

`worldbench --mode map4` ellenőrzi a warp belépés/rearm/pending szabályait,
a zónák közötti átadást, a v2 spawnidentitást és a split/merge megőrzést.
`mapaudit` ezt a pozitív/negatív tesztcsoportot is ténylegesen lefuttatja.

```powershell
./gameserver/scripts/map4_regression.ps1 -Bench ./gameserver/build/windows-relwithdebinfo/apps/gameserver/RelWithDebInfo/worldbench.exe -OutDir ./build/map4-results/regression -Suite regression
./gameserver/scripts/map4_regression.ps1 -Bench ./gameserver/build/windows-relwithdebinfo/apps/gameserver/RelWithDebInfo/worldbench.exe -OutDir ./build/map4-results/readiness -Suite readiness
```

A readiness `--file-world` kapcsolója külön temp könyvtárban generált,
nem lapos, 16 m cellaméretű csomagot használ a production loaderen át.
`--eager-terrain` ugyanannak a csomagnak a teljesen rezidens referenciaútja.
`--budget-mb` a streaming cache mérete. A szintetikus alapértelmezés megmarad.

SL-1 admission/liveness: `worldbench --mode streamadmission` a meglévő
streamerhez tartozó fairness, handoff/cancellation, reservation, deadline,
snapshot nélküli reclaim és mérési ablak teszteket futtatja. A külön runner
minden gyermekfolyamatnak saját TEMP/TMP fixture-gyökeret ad, menti a bináris
és forrás SHA256-ot, a parancsot, exit-kódot és valós futásidőt. Új output
könyvtár szükséges; korábbi eredményt nem ír felül.

```powershell
./gameserver/scripts/sl1_regression.ps1 -Bench ./gameserver/build/windows-relwithdebinfo/apps/gameserver/RelWithDebInfo/worldbench.exe -OutDir ./build/sl1-new-targeted -Suite targeted
./gameserver/scripts/sl1_regression.ps1 -Bench ./gameserver/build/windows-relwithdebinfo/apps/gameserver/RelWithDebInfo/worldbench.exe -OutDir ./build/sl1-new-original -Suite original
```

Az `original` változatlan 500 player / 200000 mob / 16 MiB / 4 worker /
seed 20260922 / 60 s warmup / 30 s measure moving, hotspot, border sorozat.
A `regression` az érintett hardening suite, a `debug` a célzott assert suite
(Debug binárist kell átadni). Ez nem indítja el az SL-2 performance mátrixot.
Review és korlátok: [SL-0 + SL-1](../docs/map-streaming-admission-liveness.md).
A futások valódi indítási, spawn-, I/O-, cache- és tickadatokat írnak;
az OS page-cache állapota nincs szabályozva. A normál mérési ablak
`--warmup 60 --seconds 30`. A játékosok célterepének előkészítése a warmup
előtt történik, kérésenként 15 s-os korláttal. A kezdetben aktív mobok
átmenetileg a későbbi steady-state-nél jóval nagyobb terrainigényt okozhatnak;
a 16 MiB-os, 200k mobos esetek itt elérhetik az explicit hibakorlátot.
Az opcionális `-Suite readiness-capacity` 128 MiB-os kiegészítő mérést futtat,
és nem írja felül a 16 MiB-os acceptance eredményeit. A moving futás csak
tényleges mérés alatti load **és** eviction mellett teljesíti a churn ellenőrzést.

A részletes szerződés: `docs/map-data-format.md` 13. pont;
a mérési eredmények és korlátok: `docs/map4-report.md`.


### SL-2 streaming és R6

`--warp-policy strict|legacy` > `warp_policy` config > **strict** alapérték.
Strictben a triggerbe célzó warp hiba (413), legacyben csak aciklikus lánc
engedett warninggal; ciklus/önhurok mindkettőben hiba (412). Hibás policy
exit 2; érvénytelen csomag exit 3. Az offline validator és startup ugyanazt
értelmezi, nincs verzióból vagy hibából automatikus legacy fallback.
A változatlan `test_zone`-hoz tudatosan `--warp-policy legacy` szükséges.

Az SL-1 4:2:1 / 64 request / 16 chunk / 100 ms policy megmaradt. Az SL-2
mérési kiegészítései: monoton request timestamp, authoritative spawn-effect,
bounded chunk reload trace, cache metadata elszámolás, attempted/waiting
movement lépések, due/enqueue/work-start késés, due+50 ms completion deadline,
Windows supervisor thread CPU (más platformon NOT MEASURED).

Runner: `scripts/sl2_regression.ps1 -Bench <worldbench.exe> -OutDir <új könyvtár>
-Suite targeted|debug|regression|original|matrix|calibration`. B1 párokhoz
`-Baseline <mentett worldbench.exe> -BaselineSource <B1/source>` szükséges.
A `matrix` a drága integrált mátrixot futtatja; csak befagyasztott C-kontraktus
és forrás után indítandó. `calibration` elkülönített, nem formális acceptance.
Futásonként izolált TMP/TEMP, bináris/forrás/fixture hash, CLI és nyers eredmény
készül. Nem töröl meglévő outputot; egy időben egy teszt fusson.
Az eredeti moving FAIL a runner exit-kódjában is megmarad.

Részletes eredmények és korlátok: [SL-2 review](../docs/map-streaming-sl2-review.md).
