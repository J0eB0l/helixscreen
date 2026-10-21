#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make land-gate: the pre-landing gate. What CI judges a branch on, run before
# the branch reaches main, so a red CI run is found here instead of on main.
#
#   app-build         make (the app; tests/ui drives the real binary)
#   sweep             make unit-sweep, the local gate's shard layout
#   ci-shards         the sweep again as CI_SHARDS shards (default 8: build.yml's
#                     layout, split across its two test legs). Which tests share a process
#                     decides what a use-after-free corrupts, so a layout nobody
#                     ran locally can crash where the local one passed.
#                     Each shard gets CI_SHARD_TIMEOUT seconds (default 600,
#                     build.yml's 10-minute test step): an eighth of the suite
#                     runs past the sweep's 300s SHARD_TIMEOUT on a slower host
#                     without hanging.
#   bats              make test-shell BATS_REQUIRED=1 (a missing bats fails the row)
#   ui                make test-ui-ci: CI's exact tests/ui pytest slice, headless
#   plugin            make test-plugin: the moonraker plugin pytest, also in CI's job
#   python            make test-python: quality.yml's tests/python pytest step
#   theme-tokens      make check-theme-tokens: generated token table vs ui_xml/
#   xml-schema        make check-xml-schema: linter schema snapshot vs a regeneration
#   lint-xml          make lint-xml: CI's XML Lint job, against the committed snapshot
#   lvgl-event-codes  make check-lvgl-event-codes: crash worker table vs lv_event_code_t
#
# The two C++ steps run on the test host when full-test-run's would
# (scripts/lib/sweep_location.sh: TEST_HOST=1, 0 or unset for the probe), one
# after the other, since both use one mirror. Everything else runs here, in
# parallel lanes beside them. Each step logs to $LAND_GATE_LOGDIR/<step>.log
# (default build/land-gate) and is killed after LAND_GATE_STEP_TIMEOUT seconds
# (default 3600, far above any healthy step), which its row reports as TIMEOUT.
# The table at the end names every step with its duration and the commit it
# judged, marked +dirty when the tree had uncommitted changes; the exit status
# is non-zero unless every row passed. The run holds build:<tree>, so a second
# gate or a peer's make in the same tree is refused instead of racing it.
#
# Outside this gate: [.] (the commit hook), [slow] and the goldens (nightly and
# local-only), the full quality-checks.sh run (pre-push and CI), and build.yml's
# telemetry-worker vitest (server/telemetry-worker: `npm install && npm test`).
set -uo pipefail

MAKE=${MAKE:-make}
SELF="${BASH_SOURCE[0]}"
TEST_HOST_RUN=${TEST_HOST_RUN:-$(dirname "$SELF")/test-host-run.sh}
HELIX_CLAIM=${HELIX_CLAIM:-$(dirname "$SELF")/helix-claim}
CI_SHARDS=${CI_SHARDS:-8}
CI_SHARD_TIMEOUT=${CI_SHARD_TIMEOUT:-600}
LAND_GATE_LOGDIR=${LAND_GATE_LOGDIR:-build/land-gate}
LAND_GATE_STEP_TIMEOUT=${LAND_GATE_STEP_TIMEOUT:-3600}
ROWS_DIR="$LAND_GATE_LOGDIR/rows"

STEPS="app-build sweep ci-shards bats ui plugin python theme-tokens xml-schema lint-xml lvgl-event-codes"

TIMEOUT_BIN=$(command -v timeout || command -v gtimeout || true)

# step <name> <command...>: run it under the step timeout, log it, record its row.
step() {
    local name=$1 log t0 status rc=0
    shift
    log="$LAND_GATE_LOGDIR/$name.log"
    t0=$SECONDS
    echo "[$(date +%H:%M:%S)] $name: started"
    if [ -n "$TIMEOUT_BIN" ]; then
        "$TIMEOUT_BIN" -k 30 "$LAND_GATE_STEP_TIMEOUT" "$@" >"$log" 2>&1 || rc=$?
    else
        "$@" >"$log" 2>&1 || rc=$?
    fi
    case "$rc" in
        0) status=PASS ;;
        124 | 137) status=TIMEOUT
                   echo "killed after ${LAND_GATE_STEP_TIMEOUT}s (LAND_GATE_STEP_TIMEOUT)" >>"$log" ;;
        *) status=FAIL ;;
    esac
    printf '%s %s %s\n' "$status" $((SECONDS - t0)) "$log" >"$ROWS_DIR/$name"
    echo "[$(date +%H:%M:%S)] $name: $status in $((SECONDS - t0))s"
    [ "$status" = PASS ]
}

skip() { printf 'SKIP - %s\n' "$2" >"$ROWS_DIR/$1"; }

lane() {
    local mk=("$MAKE" --no-print-directory)
    case "$1" in
        unit)
            if [ "$SWEEP_WHERE" = remote ]; then
                step sweep "$TEST_HOST_RUN" sweep
                step ci-shards "$TEST_HOST_RUN" sweep "NPROCS=$CI_SHARDS" "SHARD_TIMEOUT=$CI_SHARD_TIMEOUT"
            else
                step sweep "${mk[@]}" unit-sweep
                step ci-shards "${mk[@]}" unit-sweep "NPROCS=$CI_SHARDS" "SHARD_TIMEOUT=$CI_SHARD_TIMEOUT"
            fi ;;
        app)
            if step app-build "${mk[@]}"; then
                step ui "${mk[@]}" test-ui-ci
                step plugin "${mk[@]}" test-plugin
            else
                skip ui "app-build failed"
                skip plugin "app-build failed"
            fi ;;
        bats) step bats "${mk[@]}" test-shell BATS_REQUIRED=1 ;;
        fresh)
            step python "${mk[@]}" test-python
            step theme-tokens "${mk[@]}" check-theme-tokens
            step xml-schema "${mk[@]}" check-xml-schema
            step lint-xml "${mk[@]}" lint-xml
            step lvgl-event-codes "${mk[@]}" check-lvgl-event-codes ;;
        *) echo "✗ unknown lane '$1'" >&2; return 2 ;;
    esac
    return 0
}

if [ "${1:-}" = --lane ]; then
    lane "$2"
    exit $?
fi

# A step that ends up running the gate again (a make target, a stub) would fan
# out every lane once more at each level. Lanes inherit this, so a nested gate
# refuses at once.
if [ -n "${LAND_GATE_ACTIVE:-}" ]; then
    echo "✗ land-gate is already running (pid $LAND_GATE_ACTIVE): refusing to nest" >&2
    exit 2
fi
export LAND_GATE_ACTIVE=$$

# shellcheck source-path=SCRIPTDIR source=lib/build_hosts.sh
. "$(dirname "$SELF")/lib/build_hosts.sh"
# shellcheck source-path=SCRIPTDIR source=lib/sweep_location.sh
. "$(dirname "$SELF")/lib/sweep_location.sh"
resolve_sweep_location || exit $?
export SWEEP_WHERE MAKE TEST_HOST_RUN CI_SHARDS CI_SHARD_TIMEOUT LAND_GATE_LOGDIR LAND_GATE_STEP_TIMEOUT ROWS_DIR

# build:<tree> is taken for this run's pid. The two claims exclude each other
# across owners, so a worktree:<tree> holder running the gate must read as the
# same owner. helix-claim matches a session-owned holder (taken with --session)
# recorded under the session id this gate inherited, which survives a detached
# launch reparented to init. A caller with no session id (a plain shell that
# took the claim with --pid $$) is matched by retrying the take as each
# ancestor in turn; a peer's claim matches neither and still refuses.
tree=$(basename "$(git rev-parse --show-toplevel 2>/dev/null || pwd)")
claim_pid=$$
until "$HELIX_CLAIM" take "build:$tree" "make land-gate" --pid "$claim_pid" >/dev/null 2>&1; do
    claim_pid=$(ps -o ppid= -p "$claim_pid" 2>/dev/null | tr -d ' ')
    if [ -z "$claim_pid" ] || [ "$claim_pid" -le 1 ]; then
        echo "✗ build:$tree is held by another session: a gate or build is running in this tree" >&2
        if [ -z "${CLAUDE_CODE_SESSION_ID:-${HELIX_SESSION_ID:-}}" ]; then
            echo "  If that claim is your own: a gate launched detached outside a session" >&2
            echo "  is no descendant of the holder and carries no session id, so it cannot" >&2
            echo "  be matched to it. Run it in the foreground, or set HELIX_SESSION_ID." >&2
        fi
        "$HELIX_CLAIM" check "build:$tree" >&2
        exit 2
    fi
done
trap '"$HELIX_CLAIM" release-if-owned-by "$claim_pid" "build:$tree" >/dev/null 2>&1' EXIT

mkdir -p "$ROWS_DIR" || exit 2
rm -f "$ROWS_DIR"/*
gate_t0=$SECONDS
judged="$(git rev-parse --short HEAD 2>/dev/null || echo '?')"
[ -z "$(git status --porcelain --untracked-files=no 2>/dev/null)" ] || judged+="+dirty"

# Each lane is its own process group, so one signal reaches everything under
# it: a test-host ssh, a build, a pytest and its app, a bats run. A lane left
# behind keeps a mirror lock and a run nobody is waiting for.
PIDS=()
start_lane() {
    if command -v setsid >/dev/null 2>&1; then
        setsid bash "$SELF" --lane "$1" &
    else
        bash "$SELF" --lane "$1" &
    fi
    PIDS+=("$!")
}
stop_lanes() {
    local p
    for p in "${PIDS[@]}"; do kill -TERM -- "-$p" 2>/dev/null || kill -TERM "$p" 2>/dev/null; done
    exit 130
}
trap stop_lanes INT TERM HUP

if [ "$SWEEP_WHERE" = remote ]; then
    echo "→ C++ sweeps on $HELIX_TEST_HOST, everything else here (logs: $LAND_GATE_LOGDIR)"
    start_lane unit
else
    echo "→ every step runs here (logs: $LAND_GATE_LOGDIR)"
fi
start_lane bats
start_lane fresh
start_lane app
app_pid=${PIDS[-1]}

# A local sweep waits for the app build: two makes compiling one tree's objects
# at once race on the same files.
if [ "$SWEEP_WHERE" = local ]; then
    while [ ! -f "$ROWS_DIR/app-build" ] && kill -0 "$app_pid" 2>/dev/null; do sleep 1; done
    start_lane unit
fi

for p in "${PIDS[@]}"; do wait "$p"; done
trap - INT TERM HUP

failed=0
echo
echo "Land gate on $judged, C++ sweeps: $SWEEP_WHERE"
[[ "$judged" != *+dirty ]] || echo "  +dirty: uncommitted changes were judged, so this verdict names no commit"
for s in $STEPS; do
    if [ -f "$ROWS_DIR/$s" ]; then
        read -r status secs log <"$ROWS_DIR/$s"
    else
        status=FAIL secs=- log="no result: its lane died"
    fi
    [ "$status" = PASS ] || failed=1
    if [ "$secs" = - ]; then dur=-; else dur="${secs}s"; fi
    printf '  %-7s  %-17s %7s  %s\n' "$status" "$s" "$dur" "$log"
done
for s in $STEPS; do
    [ -f "$ROWS_DIR/$s" ] || continue
    read -r status _ log <"$ROWS_DIR/$s"
    case "$status" in FAIL | TIMEOUT) ;; *) continue ;; esac
    echo
    echo "--- $s: last 25 lines of $log ---"
    tail -n 25 "$log"
done
echo
echo "  total $((SECONDS - gate_t0))s"
if [ "$failed" -ne 0 ]; then
    echo "LAND GATE: FAIL ($judged)"
    exit 1
fi
echo "LAND GATE: PASS ($judged)"
