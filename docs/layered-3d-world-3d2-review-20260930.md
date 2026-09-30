# Layered 3D 3D-2 review — 2026-09-30

## Result

3D-2 is complete. Layered volume and portal declarations now have an
optional, versioned package sidecar. Existing packages remain valid without
the sidecar and continue through the legacy heightfield path.

## Package contract

- File: `layered_world.mx3d`
- Magic: `MX3D`, little-endian
- Version: `1`
- Bounded records: at most 4096 volumes, 8192 portals, 128-byte volume names
- Records use explicit little-endian integers and IEEE-754 f32 coordinates.
- The loader rejects bad magic/version, truncation, trailing bytes, invalid
  flags, record limits and geometric contract violations.
- A valid sidecar is exposed as `ServerWorldData::layered_world`; absence is
  represented by `std::nullopt` and does not synthesize a legacy volume.

## Fixture

`layered_world_package_test` writes and loads a strict v3 package containing:

- ground floor (`z = 0..4`),
- upper floor (`z = 6..10`),
- underpass (`z = -10..-6`),
- water surface (`z = 4.5..5.5`),
- ground-to-upper and ground-to-underpass portals.

It verifies all three-dimensional lookups and appends a malformed trailing
byte to prove the loader rejects the sidecar before returning world data.

## Verification

Commands:

    cmake -S gameserver -B gameserver/build/windows-debug -DENABLE_WORLDBENCH=ON -DCMAKE_BUILD_TYPE=Debug
    cmake --build gameserver/build/windows-debug --config Debug --target layered_world_contract_test layered_world_package_test --parallel 4
    cmake --build gameserver/build/windows-debug --config Debug --target gameserver --parallel 4

Results:

    LAYERED3D summary: failures=0
    LAYERED3D PACKAGE summary: failures=0
    gameserver Debug: PASS

The full gameserver build retains one existing unrelated unreachable-code
warning in `WorldRuntime.cpp`; no new 3D build error or warning was added.

## Boundary

The sidecar is loaded and validated, but AOI, movement, collision,
zone-migration and network replication still use the existing two-dimensional
runtime contracts. 3D-3 must add a layer-aware spatial identity before any
layered fixture is admitted to production simulation.
