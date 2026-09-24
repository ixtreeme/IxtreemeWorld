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
  snapshots under split/merge/reclaim). The real startup path is exercised by
  `scripts/map1_startup_acceptance.sh`.

Benches read a running world only through supervisor snapshots
(`bench/BenchSnapshot.h`, `ReadWorld`), never through `sim.Zones()` directly.

## Login server

Authentication (login, character list, handoff token issue) lives in the
separate `loginserver/` application; the gameserver only consumes the
handoff token on `EnterWorld`.
