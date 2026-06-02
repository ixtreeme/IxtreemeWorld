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

## Test

Start the server, then run:

```sh
pip install -r tools/requirements.txt
python tools/test_client.py
```

## Future AuthServer extraction

The auth logic (`AuthHandler`) is intentionally separated from game logic (`GameHandler`)
so a standalone AuthServer can be extracted later:

1. Create `apps/authserver/` with its own `main.cpp`.
2. Move `AuthHandler.h/.cpp` and the `AccountRepository` dependency to the new binary.
3. Add a token system (Redis-backed) for cross-server session validation.
4. Replace `AuthHandler` in gameserver with a `TokenValidator` that checks the token.
5. The client connects to AuthServer first, gets a token, then connects to GameServer.
6. TLS can be added to AuthServer only, where the password travels.

The current code structure makes this extraction mechanical, not a refactor.
