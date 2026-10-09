#!/usr/bin/env bash
# MAP-1 + MAP-2 acceptance through the REAL gameserver startup path (not the loader
# unit tests). POSIX shell + the built binaries; works on Windows (Git Bash)
# and FreeBSD/Linux alike.
#
#   map1_startup_acceptance.sh <out_dir> <gameserver> <worldbench> <database.json> <mob_types.conf> <test_map_dir>
#
# The fixtures are generated into <out_dir> (never into the repository), the
# server is launched from working directories OTHER than its own and the
# config's, and every case checks the exit code plus the log/console lines:
#   positive: --startup-check reaches "STARTUP-CHECK OK" (world validated and
#             running, DB connected, listener bound) and prints the summary;
#   negative: non-zero exit with the structured error, and none of the
#             post-world steps ("GameServer starting", "DB pool started",
#             "STARTUP-CHECK OK") ever happened -- no DB, no admission.
set -u
OUT="$1"
GS="$2"
WB="$3"
DB="$4"
MOBS="$5"
TESTMAP="$6"
PORT="${7:-21020}" # optional isolated listener port for a run

rm -rf "$OUT"
mkdir -p "$OUT/cfg" "$OUT/cwd_a/deeper" "$OUT/cwd_b" "$OUT/logs"
FIX="$OUT/fixtures"
"$WB" --mode worldpackage --fixtures-out "$FIX" > "$OUT/fixtures.txt" 2>&1 || true
grep -q "fixture many_chunks_v3" "$OUT/fixtures.txt" || { echo "fixture generation failed"; exit 1; }
cp "$MOBS" "$OUT/cfg/mob_types.conf"

# Paths written INTO config files must be native (Git Bash converts only
# command-line arguments); cygpath exists only there.
native() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf %s "$1"; fi; }
write_config() { # $1 = file, $2 = extra key lines
  cat > "$1" <<EOF
listen_port=$PORT
game_server=127.0.0.1:$PORT
log_level=info
log_file=$(native "$OUT/logs/gameserver.log")
mob_types_config=mob_types.conf
$2
EOF
}
# Config-relative package path: resolved against cfg/, never against the cwd.
# The corpus worlds are 256 m: the default 2x2 partition would make 128 m
# initial leaves (below the 240 m AOI floor, refused -- case 6b), so this
# config asks for one region (MAP-2: the partition is server configuration).
write_config "$OUT/cfg/file_mode.conf" "world_package=../fixtures/valid_v3
partition_regions=1x1"
write_config "$OUT/cfg/default_partition.conf" "world_package=../fixtures/geometry_v3"
write_config "$OUT/cfg/geometry_2x1.conf" "world_package=../fixtures/geometry_v3
partition_regions=2x1"
write_config "$OUT/cfg/bad_grid.conf" "world_package=../fixtures/valid_v3
partition_regions=abc"
write_config "$OUT/cfg/too_many_regions.conf" "world_package=../fixtures/valid_v3
partition_regions=17x1"
write_config "$OUT/cfg/too_many_zones.conf" "world_package=../fixtures/valid_v3
partition_regions=1x1
partition_initial_leaves=33x32"
write_config "$OUT/cfg/synthetic_too_big.conf" "world_mode=synthetic
synthetic_extent_m=200000
synthetic_zones_x=2
synthetic_zones_y=2"
write_config "$OUT/cfg/streaming.conf" "world_package=../fixtures/geometry_v3
partition_regions=2x1
terrain_residency=streaming
terrain_cache_budget_mb=4"
write_config "$OUT/cfg/bad_residency.conf" "world_package=../fixtures/valid_v3
partition_regions=1x1
terrain_residency=lazy"
write_config "$OUT/cfg/streaming_tiny_budget.conf" "world_package=../fixtures/many_chunks_v3
terrain_residency=streaming
terrain_cache_budget_mb=1"
write_config "$OUT/cfg/no_package.conf" ""
write_config "$OUT/cfg/synthetic.conf" "world_mode=synthetic
synthetic_extent_m=2000
synthetic_zones_x=2
synthetic_zones_y=1"
write_config "$OUT/cfg/synthetic_2x1.conf" "world_mode=synthetic
synthetic_extent_m=2000
synthetic_zones_x=2
synthetic_zones_y=1
partition_regions=2x1"

PASS=0
FAIL=0
check() { # name, expected_exit, log_file, must_contain..., then "--" and must_not_contain...
  local name="$1" expect="$2" log="$3"
  shift 3
  local code
  code=$(cat "$log.exit")
  local ok=1 why=""
  [ "$code" = "$expect" ] || { ok=0; why="exit=$code expected=$expect"; }
  local mode=must
  for pattern in "$@"; do
    if [ "$pattern" = "--" ]; then mode=mustnot; continue; fi
    if [ "$mode" = must ]; then
      grep -qF -- "$pattern" "$log" || { ok=0; why="$why missing:'$pattern'"; }
    else
      grep -qF -- "$pattern" "$log" && { ok=0; why="$why unexpected:'$pattern'"; }
    fi
  done
  if [ $ok = 1 ]; then
    PASS=$((PASS + 1)); echo "ACCEPT $name exit=$code: PASS"
  else
    FAIL=$((FAIL + 1)); echo "ACCEPT $name $why: FAIL"
  fi
}
run() { # name, cwd, args...
  local name="$1" cwd="$2"
  shift 2
  ( cd "$cwd" && "$GS" "$@" ) > "$OUT/logs/$name.txt" 2>&1
  echo $? > "$OUT/logs/$name.txt.exit"
}

# 1. valid package from the config (config-relative path), two unrelated cwds.
run file_cwd_a "$OUT/cwd_a/deeper" --config ../../cfg/file_mode.conf --database-config "$DB" --startup-check
check "1a-valid-package-config-path-cwd-a" 0 "$OUT/logs/file_cwd_a.txt" \
  "World startup summary: mode=file world_id=corpus" "STARTUP-CHECK OK" "fixtures" "valid_v3"
run file_cwd_b "$OUT/cwd_b" --config "$OUT/cfg/file_mode.conf" --database-config "$DB" --startup-check
check "1b-valid-package-config-path-cwd-b" 0 "$OUT/logs/file_cwd_b.txt" \
  "World startup summary: mode=file world_id=corpus" "STARTUP-CHECK OK"
# 1c. CLI override beats the config (relative to the cwd at launch).
run cli_override "$FIX" --config "$OUT/cfg/file_mode.conf" --database-config "$DB" \
  --world-package server_only_v3 --startup-check
check "1c-cli-package-overrides-config" 0 "$OUT/logs/cli_override.txt" \
  "from cli --world-package" "server_only_v3" "STARTUP-CHECK OK" -- "splatA"
# 1d. the checked-in test map (v2, legacy transition rule) with the DEFAULT
#     partition (2x2 regions over its 1 km bounds; its 3 areas are metadata).
run test_map "$OUT/cwd_b" --config "$OUT/cfg/default_partition.conf" --database-config "$DB" \
  --world-package "$TESTMAP" --warp-policy legacy --startup-check
check "1d-checked-in-test-map-v2" 0 "$OUT/logs/test_map.txt" \
  "world_id=test_zone" "format=v2" "WORLDLOGIC_WARP_TARGET_IN_TRIGGER" \
  "Initial partition: 4 region(s) x 4 initial leaf zone(s)" "SouthWest [(0,0),(500,500))" \
  "NorthEast [(500,500),(1000,1000))" "STARTUP-CHECK OK"

# 2. rejections: structured error, non-zero exit, no DB, no listener.
NOT_STARTED=(-- "GameServer starting" "DB pool started" "STARTUP-CHECK OK" "World startup summary")
run missing "$OUT/cwd_a" --config "$OUT/cfg/file_mode.conf" --database-config "$DB" \
  --world-package "$FIX/does_not_exist" --startup-check
check "2a-missing-package" 3 "$OUT/logs/missing.txt" "PACKAGE_ROOT_MISSING" "No fallback world" "${NOT_STARTED[@]}"
run bad_version "$OUT/cwd_a" --config "$OUT/cfg/file_mode.conf" --database-config "$DB" \
  --world-package "$FIX/bad_version" --startup-check
check "2b-bad-version" 3 "$OUT/logs/bad_version.txt" "MANIFEST_VERSION_UNSUPPORTED" "${NOT_STARTED[@]}"
run corrupt "$OUT/cwd_a" --config "$OUT/cfg/file_mode.conf" --database-config "$DB" \
  --world-package "$FIX/corrupt_manifest" --startup-check
check "2c-corrupt-manifest" 3 "$OUT/logs/corrupt.txt" "MANIFEST_CORRUPT" "${NOT_STARTED[@]}"
run bad_logic "$OUT/cwd_a" --config "$OUT/cfg/file_mode.conf" --database-config "$DB" \
  --world-package "$FIX/invalid_worldlogic" --startup-check
check "2d-invalid-worldlogic" 3 "$OUT/logs/bad_logic.txt" "WORLDLOGIC_ZONE_OVERLAP" "${NOT_STARTED[@]}"
run no_package "$OUT/cwd_a" --config "$OUT/cfg/no_package.conf" --database-config "$DB" --startup-check
check "2e-no-package-configured" 2 "$OUT/logs/no_package.txt" "no world package is configured" "${NOT_STARTED[@]}"
run bad_mode "$OUT/cwd_a" --config "$OUT/cfg/file_mode.conf" --database-config "$DB" --world-mode flat --startup-check
check "2f-unknown-world-mode" 2 "$OUT/logs/bad_mode.txt" "is not 'file' or 'synthetic'" "${NOT_STARTED[@]}"

# 3. explicit synthetic mode still works and says so.
run synthetic "$OUT/cwd_b" --config "$OUT/cfg/synthetic.conf" --database-config "$DB" --startup-check
check "3-explicit-synthetic-mode" 0 "$OUT/logs/synthetic.txt" \
  "World mode SYNTHETIC (explicit)" "mode=SYNTHETIC" "regions=1 leaf_zones=2" \
  "Bootstrap resources: initial_zones=2 (cap 1024)" "STARTUP-CHECK OK" \
  -- "World package: mode=file"
# 3b. synthetic with an explicit region grid that divides its zone grid.
run synthetic_2x1 "$OUT/cwd_b" --config "$OUT/cfg/synthetic_2x1.conf" --database-config "$DB" --startup-check
check "3b-synthetic-partition-regions" 0 "$OUT/logs/synthetic_2x1.txt" \
  "mode=SYNTHETIC" "regions=2 leaf_zones=2" "STARTUP-CHECK OK"

# 4. standalone check reading every layer (Full depth).
run validate_ok "$OUT/cwd_b" --validate-world-package "$FIX/valid_v3" --mob-types "$MOBS"
check "4a-validate-valid-package" 0 "$OUT/logs/validate_ok.txt" \
  "RESULT: VALID" "layer splatA: validated" "layer height: loaded" "chunks=4"
run validate_bad "$OUT/cwd_b" --validate-world-package "$FIX/corrupt_manifest"
check "4b-validate-corrupt-package" 3 "$OUT/logs/validate_bad.txt" "RESULT: INVALID" "MANIFEST_CORRUPT"
run validate_test_map "$OUT/cwd_b" --validate-world-package "$TESTMAP" --mob-types "$MOBS" --warp-policy legacy
check "4c-validate-checked-in-test-map" 0 "$OUT/logs/validate_test_map.txt" \
  "RESULT: VALID" "layer splatA: validated" "chunks=16"

# 5. a server-only package (no splat sections, no texture palette).
run server_only "$OUT/cwd_a" --config "$OUT/cfg/file_mode.conf" --database-config "$DB" \
  --world-package "$FIX/server_only_v3" --startup-check
check "5-server-only-package" 0 "$OUT/logs/server_only.txt" \
  "layers=[height:loaded/shared/required, attributes:loaded/shared/required, worldLogic:loaded" "STARTUP-CHECK OK" -- "splat"

# 6. MAP-2 geometry + partition configuration.
# 6a. negative origin, non-square, partial chunks; 2x1 regions from the config.
run geometry "$OUT/cwd_a" --config "$OUT/cfg/geometry_2x1.conf" --database-config "$DB" --startup-check
check "6a-negative-origin-nonsquare-partial" 0 "$OUT/logs/geometry.txt" \
  "bounds=[(-150,-90),(410,270))m half-open" "cells=70x45" "chunks=3x2" \
  "Initial partition: 2 region(s) x 2 initial leaf zone(s)" "West [(-150,-90),(130,270))" \
  "East [(130,-90),(410,270))" "Bootstrap resources: initial_zones=2 (cap 1024) world=560x360m" \
  "STARTUP-CHECK OK"
# 6b. the default 2x2 layout on the same 560 x 360 m world: 280 x 180 m
#     leaves are below the 240 m AOI floor -> refused before DB/network.
run default_layout "$OUT/cwd_a" --config "$OUT/cfg/default_partition.conf" --database-config "$DB" --startup-check
check "6b-layout-below-aoi-floor-refused" 2 "$OUT/logs/default_layout.txt" \
  "Partition layout invalid for this world" "${NOT_STARTED[@]}"
# 6c. unparsable partition grid.
run bad_grid "$OUT/cwd_a" --config "$OUT/cfg/bad_grid.conf" --database-config "$DB" --startup-check
check "6c-bad-partition-grid" 2 "$OUT/logs/bad_grid.txt" "partition_regions 'abc' is not <x>x<y>" "${NOT_STARTED[@]}"
run too_many_regions "$OUT/cwd_a" --config "$OUT/cfg/too_many_regions.conf" --database-config "$DB" --startup-check
check "6c2-region-grid-limit" 2 "$OUT/logs/too_many_regions.txt" "with 1..16 per axis" "${NOT_STARTED[@]}"
# 6c3. aggregate bootstrap cap: 33x32 = 1056 initial zones > 1024.
run too_many_zones "$OUT/cwd_a" --config "$OUT/cfg/too_many_zones.conf" --database-config "$DB" --startup-check
check "6c3-initial-zone-cap" 2 "$OUT/logs/too_many_zones.txt" "1056 initial zones, above the bootstrap cap of 1024" \
  "${NOT_STARTED[@]}"
# 6c4. synthetic extent beyond the f32 coordinate range.
run synthetic_too_big "$OUT/cwd_a" --config "$OUT/cfg/synthetic_too_big.conf" --database-config "$DB" --startup-check
check "6c4-synthetic-extent-cap" 2 "$OUT/logs/synthetic_too_big.txt" "synthetic world needs 500 <= synthetic_extent_m" \
  "${NOT_STARTED[@]}"
# 6d. height layer v2 (int32, scaled, offset) is reported in the summary.
run int32_heights "$OUT/cwd_b" --config "$OUT/cfg/file_mode.conf" --database-config "$DB" \
  --world-package "$FIX/height_v2_int32" --startup-check
check "6d-height-layer-v2-int32" 0 "$OUT/logs/int32_heights.txt" "height=v2 int32" "STARTUP-CHECK OK"
# 6e. no player spawn region: refused (the server has no safe place for a character).
run no_spawn "$OUT/cwd_a" --config "$OUT/cfg/file_mode.conf" --database-config "$DB" \
  --world-package "$FIX/no_player_spawn" --startup-check
check "6e-no-player-spawn-refused" 3 "$OUT/logs/no_spawn.txt" "WORLDLOGIC_NO_PLAYER_SPAWN" "${NOT_STARTED[@]}"

# 7. MAP-3 terrain residency through the real startup path.
# 7a. streaming residency: only the startup set is decoded, chunks load on demand.
run streaming "$OUT/cwd_b" --config "$OUT/cfg/streaming.conf" --database-config "$DB" --startup-check
check "7a-streaming-residency" 0 "$OUT/logs/streaming.txt" \
  "terrain residency=streaming budget=4096KB" "load=streaming" "STARTUP-CHECK OK"
# 7b. an unknown residency value.
run bad_residency "$OUT/cwd_a" --config "$OUT/cfg/bad_residency.conf" --database-config "$DB" --startup-check
check "7b-bad-residency-value" 2 "$OUT/logs/bad_residency.txt" "terrain_residency 'lazy' is not 'eager' or 'streaming'" \
  "${NOT_STARTED[@]}"
# 7c. an 8192-chunk index above a 1 MB terrain budget: refused before the
#     listener (the runtime -- and so the check -- is built after the DB pool).
run tiny_budget "$OUT/cwd_a" --config "$OUT/cfg/streaming_tiny_budget.conf" --database-config "$DB" --startup-check
check "7c-streaming-budget-below-startup-residency" 2 "$OUT/logs/tiny_budget.txt" \
  "Terrain budget too small" -- "STARTUP-CHECK OK"

# 8. MAP-4 explicit spawn identity and area references through production startup.
run spawns_v2 "$OUT/cwd_b" --config "$OUT/cfg/file_mode.conf" --database-config "$DB" \
  --world-package "$FIX/spawns_v2" --startup-check
check "8a-spawns-v2" 0 "$OUT/logs/spawns_v2.txt" "STARTUP-CHECK OK"
run spawns_v2_bad_area "$OUT/cwd_a" --config "$OUT/cfg/file_mode.conf" --database-config "$DB" \
  --world-package "$FIX/spawns_v2_bad_area" --startup-check
check "8b-spawns-v2-bad-area" 3 "$OUT/logs/spawns_v2_bad_area.txt" "SPAWNS_FIELD_INVALID" "${NOT_STARTED[@]}"

# SL-2 R6: same fixture bytes; startup and offline validation use one policy parser.
write_config "$OUT/cfg/r6_legacy.conf" "world_package=../fixtures/r6_chain
partition_regions=1x1
warp_policy=legacy"
write_config "$OUT/cfg/r6_strict.conf" "world_package=../fixtures/r6_chain
partition_regions=1x1
warp_policy=strict"
write_config "$OUT/cfg/r6_invalid.conf" "world_package=../fixtures/r6_chain
partition_regions=1x1
warp_policy=invalid"
for mode in strict legacy; do
  if [ "$mode" = strict ]; then code=3; else code=0; fi
  run "r6_chain_$mode" "$OUT/cwd_b" --config "$OUT/cfg/file_mode.conf" --database-config "$DB" --world-package "$FIX/r6_chain" --warp-policy "$mode" --startup-check
  if [ "$code" = 3 ]; then
    check "r6-chain-$mode-startup" "$code" "$OUT/logs/r6_chain_$mode.txt" "WORLDLOGIC_WARP_TARGET_IN_TRIGGER" "warp 1 target lands in warp 2" "${NOT_STARTED[@]}"
  else
    check "r6-chain-$mode-startup" 0 "$OUT/logs/r6_chain_$mode.txt" "World warp policy=legacy" "WORLDLOGIC_WARP_TARGET_IN_TRIGGER" "STARTUP-CHECK OK"
  fi
  run "r6_offline_$mode" "$OUT/cwd_b" --validate-world-package "$FIX/r6_chain" --warp-policy "$mode"
  check "r6-chain-$mode-offline" "$code" "$OUT/logs/r6_offline_$mode.txt" "WORLDLOGIC_WARP_TARGET_IN_TRIGGER"
  for kind in cycle self; do
    run "r6_${kind}_$mode" "$OUT/cwd_b" --config "$OUT/cfg/file_mode.conf" --database-config "$DB" --world-package "$FIX/r6_$kind" --warp-policy "$mode" --startup-check
    check "r6-$kind-$mode" 3 "$OUT/logs/r6_${kind}_$mode.txt" "WORLDLOGIC_WARP_CYCLE" "${NOT_STARTED[@]}"
  done
done
run r6_default "$OUT/cwd_b" --config "$OUT/cfg/file_mode.conf" --world-package "$FIX/r6_chain" --startup-check
check "r6-default-strict" 3 "$OUT/logs/r6_default.txt" "World warp policy=strict" "WORLDLOGIC_WARP_TARGET_IN_TRIGGER" "${NOT_STARTED[@]}"
run r6_config "$OUT/cwd_b" --config "$OUT/cfg/r6_legacy.conf" --database-config "$DB" --startup-check
check "r6-config-legacy" 0 "$OUT/logs/r6_config.txt" "World warp policy=legacy" "STARTUP-CHECK OK"
run r6_override_strict "$OUT/cwd_b" --config "$OUT/cfg/r6_legacy.conf" --warp-policy strict --startup-check
check "r6-cli-strict-overrides-legacy" 3 "$OUT/logs/r6_override_strict.txt" "World warp policy=strict" "${NOT_STARTED[@]}"
run r6_override_legacy "$OUT/cwd_b" --config "$OUT/cfg/r6_strict.conf" --warp-policy legacy --database-config "$DB" --startup-check
check "r6-cli-legacy-overrides-strict" 0 "$OUT/logs/r6_override_legacy.txt" "World warp policy=legacy" "STARTUP-CHECK OK"
run r6_invalid_config "$OUT/cwd_b" --config "$OUT/cfg/r6_invalid.conf" --startup-check
check "r6-invalid-config" 2 "$OUT/logs/r6_invalid_config.txt" "warp_policy 'invalid'" "${NOT_STARTED[@]}"
run r6_invalid_cli "$OUT/cwd_b" --config "$OUT/cfg/r6_legacy.conf" --warp-policy invalid --startup-check
check "r6-invalid-cli" 2 "$OUT/logs/r6_invalid_cli.txt" "warp_policy 'invalid'" "${NOT_STARTED[@]}"
run r6_offline_config "$OUT/cwd_b" --config "$OUT/cfg/r6_legacy.conf" --validate-world-package "$FIX/r6_chain"
check "r6-offline-config-legacy" 0 "$OUT/logs/r6_offline_config.txt" "World warp policy=legacy" "RESULT: VALID"
run r6_offline_override "$OUT/cwd_b" --config "$OUT/cfg/r6_legacy.conf" --validate-world-package "$FIX/r6_chain" --warp-policy strict
check "r6-offline-cli-strict" 3 "$OUT/logs/r6_offline_override.txt" "World warp policy=strict" "RESULT: INVALID"
run r6_offline_invalid "$OUT/cwd_b" --validate-world-package "$FIX/r6_chain" --warp-policy invalid
check "r6-offline-invalid" 2 "$OUT/logs/r6_offline_invalid.txt" "warp_policy 'invalid'"

echo "ACCEPT-DONE passes=$PASS failures=$FAIL"
[ $FAIL = 0 ]
