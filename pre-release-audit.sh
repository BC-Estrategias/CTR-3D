#!/usr/bin/env bash
# Run cheap release gates before cutting a beta tag.

set -euo pipefail

usage() {
    cat <<'EOF'
usage: ./pre-release-audit.sh <previous-tag-or-ref> [release-tag]

Examples:
  ./pre-release-audit.sh beta-6.1
  ./pre-release-audit.sh beta-7 beta-7.1

The second argument is optional. If it exists, it must point at HEAD.
If it does not exist yet, the script treats HEAD as the release candidate.
EOF
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
    usage >&2
    exit 2
fi

BASE_REF="$1"
RELEASE_REF="${2:-}"

if ! git rev-parse --verify "${BASE_REF}^{commit}" >/dev/null 2>&1; then
    echo "ERROR: base ref does not exist: ${BASE_REF}" >&2
    exit 1
fi

BASE_SHA="$(git rev-parse --short=12 "${BASE_REF}^{commit}")"
HEAD_SHA="$(git rev-parse --short=12 HEAD)"

echo "== Release candidate =="
echo "base: ${BASE_REF} (${BASE_SHA})"
echo "head: HEAD (${HEAD_SHA})"

if [[ -n "${RELEASE_REF}" ]]; then
    if git rev-parse --verify "${RELEASE_REF}^{commit}" >/dev/null 2>&1; then
        RELEASE_SHA="$(git rev-parse --short=12 "${RELEASE_REF}^{commit}")"
        echo "release tag: ${RELEASE_REF} (${RELEASE_SHA})"
        if [[ "${RELEASE_SHA}" != "${HEAD_SHA}" ]]; then
            echo "ERROR: ${RELEASE_REF} does not point at HEAD." >&2
            echo "Cut a new point-release tag or move only an unpublished local tag." >&2
            exit 1
        fi
    else
        echo "release tag: ${RELEASE_REF} (not created yet)"
    fi
fi

echo
echo "== Worktree =="
if [[ -n "$(git status --short)" ]]; then
    git status --short
    echo "ERROR: release audit requires a clean worktree." >&2
    exit 1
fi
echo "clean"

echo
echo "== Diff size =="
git diff --shortstat "${BASE_REF}..HEAD" -- game include platform main.c CMakeLists.txt build.sh build.bat package-linux.sh || true

echo
echo "== Whitespace =="
git diff --check

echo
echo "== Build =="
./build.sh
if [[ -x build/ctr_native ]]; then
    ./build/ctr_native --version
fi

if [[ -f .ctr-progress/ctr_progress.py ]]; then
    echo
    echo "== Progress ledger =="
    python3 .ctr-progress/ctr_progress.py validate
fi

if [[ -f .ctr-progress/tools/data_parity.py && -x build/ctr_native && -f ../CTR-ModSDK/build/ctr-u/SCUS_944.26 ]]; then
    echo
    echo "== Global data parity =="
    python3 .ctr-progress/tools/data_parity.py \
        --native build/ctr_native \
        --retail ../CTR-ModSDK/build/ctr-u/SCUS_944.26 \
        --symbol data \
        --max-runs 40
fi

# Risky-pattern scans run under rg when present, falling back to grep -E
# (GNU grep, POSIX ERE) on any of the four fixed patterns below -- none of
# them need PCRE-only features, so grep -P is never required. The scan is
# mandatory: if neither tool exists, the audit fails instead of silently
# skipping coverage.
if command -v rg >/dev/null 2>&1; then
    RISK_SCAN_TOOL="rg"
elif command -v grep >/dev/null 2>&1; then
    RISK_SCAN_TOOL="grep"
else
    echo
    echo "ERROR: neither rg nor grep is available; cannot run risky-pattern scans." >&2
    exit 1
fi

echo
echo "== Risk-scan tool =="
if [[ "${RISK_SCAN_TOOL}" == "rg" ]]; then
    echo "using: rg"
else
    echo "using: grep -E (rg not found on PATH; falling back)"
fi

scan_diff() {
    local title="$1"
    local pattern="$2"
    local tmp

    tmp="$(mktemp)"
    if [[ "${RISK_SCAN_TOOL}" == "rg" ]]; then
        git diff -U0 "${BASE_REF}..HEAD" -- game include platform main.c \
            | rg "${pattern}" >"${tmp}" || true
    else
        git diff -U0 "${BASE_REF}..HEAD" -- game include platform main.c \
            | grep -E "${pattern}" >"${tmp}" || true
    fi

    local count
    count="$(wc -l <"${tmp}")"

    echo
    echo "== Risk scan: ${title} =="
    echo "matches: ${count}"
    if [[ "${count}" -gt 0 ]]; then
        sed -n '1,120p' "${tmp}"
        if [[ "${count}" -gt 120 ]]; then
            echo "... ${count} total; inspect the full '${title}' pattern in pre-release-audit.sh."
        fi
    fi

    rm -f "${tmp}"
}

scan_diff "packed/matrix copies and narrow stores" \
    '^\+[^+].*(memcpy|memmove|sizeof\([^)]*(MATRIX|matrix|struct)|MatrixRotate|ConvertRotToMatrix|MatrixND|CTR_WriteU(16|32)LE|CTR_ReadU(16|32)LE|= \*|\(MATRIX \*\))'

scan_diff "native divergence gates" \
    '^\+[^+].*(CTR_NATIVE|#if defined\(CTR_NATIVE\)|#ifdef CTR_NATIVE)'

scan_diff "retail state width/type edits" \
    '^\+[^+].*(typedef|enum|struct|s8|u8|s16|u16|s32|u32|b32|bool)'

# Comments must state standing constraints, not history (see CLAUDE.md,
# Code style). This flags NEW comment lines carrying session tags, dates,
# or review-fix framing. The s-number class skips s16/s32 (integer types).
# Acceptable matches exist: "rung N" as renderer-design phase vocabulary
# and live-hunt instrumentation docs -- eyeball, don't auto-reject.
scan_diff "session-history comments" \
    '^\+[^+].*(//|/\*| \* ).*\b(s(1[0-57-9]|2[0-9]|3[013-9]|4[0-9])\b|[Ss]ession [0-9]|[Rr]ung [0-9]|[Ss]itting [0-9]|20[0-9]{2}-[01][0-9]-[0-3][0-9]|[Rr]eview fix|REVIEW FIX|[Rr]eview finding)'

# The 3DS release artifact is built with -DCTR_INTERNAL=OFF, which strips the
# dumpreq parser. Its word table is the only source of the literal "dumpreq" in
# the binary, so finding that string means the gate was left ON.
echo
echo "== Debug surface: 3DS artifact =="
CTR_3DSX="${CTR_3DSX:-build-3ds-release/ctr_native.3dsx}"
if [[ -f "${CTR_3DSX}" ]]; then
    if strings "${CTR_3DSX}" 2>/dev/null | grep -q dumpreq; then
        echo "WARN: ${CTR_3DSX} contains \"dumpreq\" -- built with CTR_INTERNAL ON; rebuild with -DCTR_INTERNAL=OFF."
    else
        echo "ok: ${CTR_3DSX} carries no dumpreq parser."
    fi
else
    echo "skipped: ${CTR_3DSX} not present (set CTR_3DSX to the artifact to check it)."
fi

echo
echo "Release audit completed. Risk-scan matches are audit prompts, not automatic proof."
