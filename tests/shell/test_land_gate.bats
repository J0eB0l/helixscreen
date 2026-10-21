#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make land-gate's wiring (scripts/land-gate.sh). make and test-host-run.sh are
# stubbed: each target exits with a verdict the test chooses and records that it
# ran, so these prove which steps run where and that any red step reds the gate
# and shows in the table, without running a real suite.

load helpers

REPO_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
GATE="$REPO_ROOT/scripts/land-gate.sh"

ALL_STEPS="app-build sweep ci-shards bats ui plugin python theme-tokens xml-schema lint-xml lvgl-event-codes"

# Every make here is a stub, and a stub must never reach the real make or the
# jobpool shim: JOBPOOL=0 keeps the shim out of any make that does run, and the
# stubs refuse to nest (FAKE_MAKE_DEPTH), so a stub that re-enters make fails
# loudly instead of forking without bound.
setup() {
    export JOBPOOL=0
    unset FAKE_MAKE_DEPTH LAND_GATE_ACTIVE
    export TMPDIR="$BATS_TEST_TMPDIR"
    export CALLS="$BATS_TEST_TMPDIR/calls"
    export LAND_GATE_LOGDIR="$BATS_TEST_TMPDIR/logs"
    : >"$CALLS"
    # A target exits with $RC_<target, - as _>, default 0; no target is "app".
    mock_command_script fake-make '
[ "${FAKE_MAKE_DEPTH:-0}" -lt 1 ] || { echo "fake-make re-entered" >&2; exit 97; }
export FAKE_MAKE_DEPTH=$((${FAKE_MAKE_DEPTH:-0} + 1))
t=app; vars=""
for a in "$@"; do case "$a" in -*) ;; *=*) vars="$vars $a" ;; *) t=$a ;; esac; done
echo "make $t$vars" >> "$CALLS"
echo "output of $t"
eval "rc=\${RC_$(echo "$t" | tr - _):-0}"
exit "$rc"'
    mock_command_script fake-test-host-run '
echo "host-run $*" >> "$CALLS"
case "$1" in
    sweep) echo "host sweep $*"; case "$*" in *NPROCS=*) exit "${RC_host_ci:-0}" ;; *) exit "${RC_host_sweep:-0}" ;; esac ;;
    --probe) exit 1 ;;
esac'
    # take exits $CLAIM_TAKE_RC (default 0); every call is recorded.
    mock_command_script fake-claim '
echo "claim $*" >> "$CALLS"
case "$1" in take) exit "${CLAIM_TAKE_RC:-0}" ;; esac'
    export MAKE=fake-make TEST_HOST_RUN=fake-test-host-run HELIX_CLAIM=fake-claim
    export HELIX_TEST_HOST=testhost.invalid HELIX_BUILD_HOSTS_FILE=/nonexistent
    unset CI_SHARDS CI_SHARD_TIMEOUT
    export TEST_HOST=0
}

# No test here runs make in the real tree: a recipe is checked by copying its
# fragment of mk/tests.mk into a scratch Makefile (run_mk_fragment, helpers.bash).
run_fragment() { # <first-line-regex> <target> [make args...]
    run_mk_fragment "$REPO_ROOT/mk/tests.mk" "$@"
}

row() { # <status> <step>: the table row for step has that status
    printf '%s\n' "$output" | grep -Eq "^  $1 +$2 "
}

@test "all green: every step runs, every row passes, and the gate passes" {
    run "$GATE"
    [ "$status" -eq 0 ]
    for s in $ALL_STEPS; do row PASS "$s"; done
    contains "LAND GATE: PASS" "$output"
    grep -qx "make app" "$CALLS"
    grep -qx "make unit-sweep" "$CALLS"
    grep -qx "make test-shell BATS_REQUIRED=1" "$CALLS"
    grep -qx "make test-ui-ci" "$CALLS"
    grep -qx "make test-plugin" "$CALLS"
    grep -qx "make test-python" "$CALLS"
    grep -qx "make check-theme-tokens" "$CALLS"
    grep -qx "make check-xml-schema" "$CALLS"
    grep -qx "make lint-xml" "$CALLS"
    grep -qx "make check-lvgl-event-codes" "$CALLS"
}

@test "the second sweep runs in CI's 8-shard layout, and CI_SHARDS changes it" {
    run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "make unit-sweep NPROCS=8 SHARD_TIMEOUT=600" "$CALLS"
    : >"$CALLS"
    CI_SHARDS=12 run "$GATE"
    grep -qx "make unit-sweep NPROCS=12 SHARD_TIMEOUT=600" "$CALLS"
}

@test "a CI-layout shard gets CI's step budget, not the sweep's 300s, and CI_SHARD_TIMEOUT changes it" {
    [ "$(sed -n '/name: Run unit tests/,/run:/s/.*timeout-minutes: *//p' "$REPO_ROOT/.github/workflows/build.yml")" = 10 ]
    TEST_HOST=1 run "$GATE"
    grep -qx "host-run sweep NPROCS=8 SHARD_TIMEOUT=600" "$CALLS"
    : >"$CALLS"
    CI_SHARD_TIMEOUT=900 run "$GATE"
    grep -qx "make unit-sweep NPROCS=8 SHARD_TIMEOUT=900" "$CALLS"
    refute_grep "make unit-sweep SHARD_TIMEOUT" "$CALLS"
}

@test "each failing step fails the gate and shows FAIL in its own row" {
    local target step
    for pair in app:app-build unit-sweep:sweep test-shell:bats test-ui-ci:ui test-plugin:plugin test-python:python \
                check-theme-tokens:theme-tokens check-xml-schema:xml-schema lint-xml:lint-xml \
                check-lvgl-event-codes:lvgl-event-codes; do
        target=${pair%%:*} step=${pair#*:}
        eval "export RC_$(echo "$target" | tr - _)=3"
        run "$GATE"
        eval "unset RC_$(echo "$target" | tr - _)"
        [ "$status" -eq 1 ] || { echo "red $target did not fail the gate"; return 1; }
        row FAIL "$step" || { echo "no FAIL row for $step"; return 1; }
        contains "LAND GATE: FAIL" "$output"
        contains "output of $target" "$output"
    done
}

@test "a red CI-layout pass alone fails the gate while the local layout passes" {
    TEST_HOST=1 RC_host_ci=1 run "$GATE"
    [ "$status" -eq 1 ]
    row PASS sweep
    row FAIL ci-shards
}

@test "a failed app build skips the UI steps and fails the gate" {
    RC_app=2 run "$GATE"
    [ "$status" -eq 1 ]
    row FAIL app-build
    row SKIP ui
    row SKIP plugin
    refute_grep "make test-ui-ci" "$CALLS"
    row PASS bats
}

@test "TEST_HOST=1 sends both sweeps to the test host and runs neither here" {
    TEST_HOST=1 run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "host-run sweep" "$CALLS"
    grep -qx "host-run sweep NPROCS=8 SHARD_TIMEOUT=600" "$CALLS"
    refute_grep "make unit-sweep" "$CALLS"
    grep -qx "make test-shell BATS_REQUIRED=1" "$CALLS"
    grep -qx "make test-ui-ci" "$CALLS"
}

@test "a bad TEST_HOST refuses before running anything" {
    TEST_HOST=maybe run "$GATE"
    [ "$status" -eq 2 ]
    [ ! -s "$CALLS" ]
}

# The recipe names $(MAKE), so make runs it even under -n: the script it calls
# is a stub in the scratch directory, never the real gate.
@test "make land-gate hands off to scripts/land-gate.sh with CI_SHARDS" {
    mkdir -p "$BATS_TEST_TMPDIR/mkfrag/scripts"
    printf '#!/bin/sh\necho "stub gate CI_SHARDS=$CI_SHARDS TEST_HOST=$TEST_HOST"\n' \
        >"$BATS_TEST_TMPDIR/mkfrag/scripts/land-gate.sh"
    chmod +x "$BATS_TEST_TMPDIR/mkfrag/scripts/land-gate.sh"
    run run_fragment '^CI_SHARDS [?]=' land-gate land-gate
    [ "$status" -eq 0 ]
    contains "stub gate CI_SHARDS=8 TEST_HOST=" "$output"
    run run_fragment '^CI_SHARDS [?]=' land-gate land-gate CI_SHARDS=12 TEST_HOST=1
    contains "stub gate CI_SHARDS=12 TEST_HOST=1" "$output"
}

# The gate is only worth its name while it runs what CI runs.
@test "test-ui-ci runs build.yml's exact tests/ui pytest invocation, headless" {
    local ci args
    ci=$(grep -E '^\s*run: pytest tests/ui/' "$REPO_ROOT/.github/workflows/build.yml" | sed 's/^ *run: pytest //')
    [ -n "$ci" ]
    args=$(sed -n 's/^UI_CI_PYTEST_ARGS := //p' "$REPO_ROOT/mk/tests.mk")
    [ "$args" = "$ci" ] || { printf 'CI:   %s\nmake: %s\n' "$ci" "$args"; return 1; }
    run run_fragment '^UI_CI_PYTEST_ARGS :=' test-ui-ci -n test-ui-ci
    [ "$status" -eq 0 ]
    contains "pytest $ci;" "$output"
    contains "HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy" "$output"
}

@test "CI's unit-test step uses the filter the sweep passes, with explicit shard flags" {
    local ci
    ci=$(grep -F 'helix-tests "~[.] ~[slow]" --shard-count $NPROCS --shard-index $i' \
        "$REPO_ROOT/.github/workflows/build.yml")
    [ -n "$ci" ]
    grep -qxF '        NPROCS=8' "$REPO_ROOT/.github/workflows/build.yml"
}

@test "a step past LAND_GATE_STEP_TIMEOUT is killed and reads TIMEOUT, failing the gate" {
    mock_command_script fake-make '
[ "${FAKE_MAKE_DEPTH:-0}" -lt 1 ] || { echo "fake-make re-entered" >&2; exit 97; }
export FAKE_MAKE_DEPTH=$((${FAKE_MAKE_DEPTH:-0} + 1))
t=app; for a in "$@"; do case "$a" in -*|*=*) ;; *) t=$a ;; esac; done
echo "make $t" >> "$CALLS"
[ "$t" = lint-xml ] && sleep 30
exit 0'
    LAND_GATE_STEP_TIMEOUT=1 run "$GATE"
    [ "$status" -eq 1 ]
    row TIMEOUT lint-xml
    row PASS bats
}

@test "the gate holds build:<tree> for the run and releases it; a held tree refuses" {
    run "$GATE"
    [ "$status" -eq 0 ]
    grep -q "^claim take build:" "$CALLS"
    grep -q "^claim release-if-owned-by .* build:" "$CALLS"
    : >"$CALLS"
    CLAIM_TAKE_RC=1 run "$GATE"
    [ "$status" -eq 2 ]
    contains "is held" "$output"
    refute_grep "^make" "$CALLS"
}

@test "a tree claimed by the session running the gate is taken as that session" {
    # take succeeds only for the pid named in $CLAIM_OWNER, as helix-claim
    # exempts the holder of the sibling worktree: claim.
    mock_command_script fake-claim '
echo "claim $*" >> "$CALLS"
case "$1" in take) [ "$5" = "$CLAIM_OWNER" ] ;; esac'
    CLAIM_OWNER=$$ run "$GATE"
    [ "$status" -eq 0 ]
    grep -q "^claim take build:.* --pid $$\$" "$CALLS"
    grep -q "^claim release-if-owned-by $$ build:" "$CALLS"
}

@test "the verdict names the commit, marked +dirty when the tree has uncommitted changes" {
    local T="$BATS_TEST_TMPDIR/tree"
    git init -q "$T"
    echo a >"$T/f"; git -C "$T" add f
    git -C "$T" -c user.email=t@t -c user.name=t commit -q -m init
    local sha; sha=$(git -C "$T" rev-parse --short HEAD)
    run bash -c "cd '$T' && '$GATE'"
    [ "$status" -eq 0 ]
    contains "Land gate on $sha," "$output"
    lacks "+dirty" "$output"
    echo b >"$T/f"
    run bash -c "cd '$T' && '$GATE'"
    contains "Land gate on $sha+dirty" "$output"
    contains "LAND GATE: PASS ($sha+dirty)" "$output"
}

@test "test-shell with BATS_REQUIRED=1 fails when bats is missing; without it, skips" {
    run run_fragment '^BATS_BIN [?]=' test-shell test-shell BATS_BIN=no-such-bats BATS_REQUIRED=1
    [ "$status" -ne 0 ]
    contains "BATS_REQUIRED=1" "$output"
    run run_fragment '^BATS_BIN [?]=' test-shell test-shell BATS_BIN=no-such-bats
    [ "$status" -eq 0 ]
    contains "skipping shell tests" "$output"
}

# A stubbed step that re-enters the gate (a make target running land-gate, or a
# stub calling make again) must end in a FAIL row, never in unbounded recursion.
@test "stubbed lanes cannot recurse: a nested gate refuses and a re-entering stub stops" {
    mock_command_script fake-make '
[ "${FAKE_MAKE_DEPTH:-0}" -lt 1 ] || { echo "fake-make re-entered" >&2; exit 97; }
export FAKE_MAKE_DEPTH=$((${FAKE_MAKE_DEPTH:-0} + 1))
t=app; for a in "$@"; do case "$a" in -*|*=*) ;; *) t=$a ;; esac; done
echo "make $t" >> "$CALLS"
case "$t" in
    test-shell) "$NESTED_GATE"; exit $? ;;
    lint-xml) fake-make --no-print-directory again; exit $? ;;
esac
exit 0'
    NESTED_GATE="$GATE" run /usr/bin/timeout 120 "$GATE"
    [ "$status" -eq 1 ]
    row FAIL bats
    row FAIL lint-xml
    grep -q "land-gate is already running" "$LAND_GATE_LOGDIR/bats.log"
    grep -q "fake-make re-entered" "$LAND_GATE_LOGDIR/lint-xml.log"
    [ "$(grep -c '^make' "$CALLS")" -lt 20 ]
}

# A gate launched detached (`( make land-gate ) &` from a shell that exits) is
# reparented to init, so the session holding worktree:<tree> is no ancestor of
# it; only the session id it inherited ties it to that claim. The holder here is
# no ancestor of the gate either, so only the id can match it.
detached_gate_fixture() {
    T="$BATS_TEST_TMPDIR/tree"
    git init -q "$T"
    echo a >"$T/f"; git -C "$T" add f
    git -C "$T" -c user.email=t@t -c user.name=t commit -q -m init
    export HELIX_CLAIM="$REPO_ROOT/scripts/helix-claim" HELIX_CLAIM_DIR="$BATS_TEST_TMPDIR/claims"
    # The session's own claim: taken with --session by a stand-in `claude`
    # process that outlives the take.
    mkdir -p "$BATS_TEST_TMPDIR/fakebin"
    ln -sf "$(command -v bash)" "$BATS_TEST_TMPDIR/fakebin/claude"
    (cd "$T" && exec env HELIX_SESSION_ID=sess-a "$BATS_TEST_TMPDIR/fakebin/claude" -c \
        '"$1" take worktree:tree "session edit" --session >/dev/null || exit 1; exec sleep 120' \
        _ "$HELIX_CLAIM") &
    HOLDER=$!
    local i
    for i in $(seq 100); do
        (cd "$T" && "$HELIX_CLAIM" check worktree:tree >/dev/null 2>&1) || return 0
        sleep 0.05
    done
    return 1
}

teardown() {
    [ -n "${HOLDER:-}" ] && kill "$HOLDER" 2>/dev/null
    return 0
}

@test "a detached gate runs under its own session's worktree: claim" {
    detached_gate_fixture
    run env HELIX_SESSION_ID=sess-a bash -c "cd '$T' && '$GATE'"
    [ "$status" -eq 0 ]
    contains "LAND GATE: PASS" "$output"
}

@test "a detached gate outside any session is refused, and says why" {
    detached_gate_fixture
    run env -u HELIX_SESSION_ID -u CLAUDE_CODE_SESSION_ID bash -c "cd '$T' && '$GATE'"
    [ "$status" -eq 2 ]
    contains "is held by another session" "$output"
    contains "outside a session" "$output"
    refute_grep "^make" "$CALLS"
}
