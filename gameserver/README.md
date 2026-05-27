# GameServer

GameServer is a C++20 networking skeleton for the new MMORPG server project.

## Dependencies

Dependencies are managed through vcpkg manifest mode:

- Boost.Asio
- Boost.System
- spdlog
- fmt

Set `VCPKG_ROOT` to your vcpkg checkout before configuring.

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
python tools/test_client.py
```
