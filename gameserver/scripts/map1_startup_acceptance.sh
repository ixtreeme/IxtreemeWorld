#!/usr/bin/env bash
# MAP-1 acceptance through the REAL gameserver startup path (not the loader
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

rm -rf "$OUT"
mkdir -p "$OUT/cfg" "$OUT/cwd_a/deeper" "$OUT/cwd_b" "$OUT/logs"
FIX="$OUT/fixtures"
"$WB" --mode worldpackage --fixtures-out "$FIX" > "$OUT/fixtures.txt" 2>&1 || true
grep -q "fixture invalid_worldlogic" "$OUT/fixtures.txt" || { echo "fixture generation failed"; exit 1; }
cp "$MOBS" "$OUT/cfg/mob_types.conf"

# Paths written INTO config files must be native (Git Bash converts only
# command-line arguments); cygpath exists only there.
native() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf %s "$1"; fi; }
write_config() { # $1 = file, $2 = extra key lines
  cat > "$1" <<EOF
listen_port=21020
game_server=127.0.0.1:21020
log_level=info
log_file=$(native "$OUT/logs/gameserver.log")
mob_types_config=mob_types.conf
$2
EOF
}
# Config-relative package path: resolved against cfg/, never against the cwd.
write_config "$OUT/cfg/file_mode.conf" "world_package=../fixtures/valid_v3"
write_config "$OUT/cfg/no_package.conf" ""
write_config "$OUT/cfg/synthetic.conf" "world_mode=synthetic
synthetic_extent_m=2000
synthetic_zones_x=2
synthetic_zones_y=1"

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
# 1d. the checked-in test map (v2, legacy transition rule).
run test_map "$OUT/cwd_b" --config "$OUT/cfg/file_mode.conf" --database-config "$DB" \
  --world-package "$TESTMAP" --startup-check
check "1d-checked-in-test-map-v2" 0 "$OUT/logs/test_map.txt" \
  "world_id=test_zone" "format=v2" "WORLDLOGIC_WARP_TARGET_IN_TRIGGER" "STARTUP-CHECK OK"

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
  "World mode SYNTHETIC (explicit)" "mode=SYNTHETIC" "STARTUP-CHECK OK" -- "World package: mode=file"

# 4. standalone check reading every layer (Full depth).
run validate_ok "$OUT/cwd_b" --validate-world-package "$FIX/valid_v3" --mob-types "$MOBS"
check "4a-validate-valid-package" 0 "$OUT/logs/validate_ok.txt" \
  "RESULT: VALID" "layer splatA: validated" "layer height: loaded" "chunks=4"
run validate_bad "$OUT/cwd_b" --validate-world-package "$FIX/corrupt_manifest"
check "4b-validate-corrupt-package" 3 "$OUT/logs/validate_bad.txt" "RESULT: INVALID" "MANIFEST_CORRUPT"
run validate_test_map "$OUT/cwd_b" --validate-world-package "$TESTMAP" --mob-types "$MOBS"
check "4c-validate-checked-in-test-map" 0 "$OUT/logs/validate_test_map.txt" \
  "RESULT: VALID" "layer splatA: validated" "chunks=16"

# 5. a server-only package (no splat sections, no texture palette).
run server_only "$OUT/cwd_a" --config "$OUT/cfg/file_mode.conf" --database-config "$DB" \
  --world-package "$FIX/server_only_v3" --startup-check
check "5-server-only-package" 0 "$OUT/logs/server_only.txt" \
  "layers=[height:loaded/shared/required, attributes:loaded/shared/required, worldLogic:loaded" "STARTUP-CHECK OK" -- "splat"

echo "ACCEPT-DONE passes=$PASS failures=$FAIL"
[ $FAIL = 0 ]
