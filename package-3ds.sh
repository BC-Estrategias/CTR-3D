#!/usr/bin/env bash
# Package the current New Nintendo 3DS build for release.

set -euo pipefail

ALLOW_INTERNAL=0
VERSION=""
for arg in "$@"; do
    case "${arg}" in
        --allow-internal)
            ALLOW_INTERNAL=1
            ;;
        *)
            VERSION="${arg}"
            ;;
    esac
done
VERSION="${VERSION:-1.0.0-beta.1}"

DIST_DIR="${DIST_DIR:-dist}"
PACKAGE_NAME="ctr3d-${VERSION}"
PACKAGE_DIR="${DIST_DIR}/${PACKAGE_NAME}"
CTR_3DSX="${CTR_3DSX:-build-3ds/ctr_native.3dsx}"
MIN_SIZE_BYTES=$((1024 * 1024))

if [[ ! -f "${CTR_3DSX}" ]]; then
    echo "ERROR: missing 3dsx: ${CTR_3DSX}" >&2
    echo "Build for 3DS first (see README.md), or set CTR_3DSX=path." >&2
    exit 1
fi

SIZE_BYTES="$(wc -c <"${CTR_3DSX}" | tr -d '[:space:]')"
if (( SIZE_BYTES <= MIN_SIZE_BYTES )); then
    echo "ERROR: ${CTR_3DSX} is ${SIZE_BYTES} bytes, expected more than 1 MB (looks truncated)." >&2
    exit 1
fi

DUMPREQ_COUNT="$(grep -a -c "dumpreq" "${CTR_3DSX}" 2>/dev/null || true)"
DUMPREQ_COUNT="${DUMPREQ_COUNT:-0}"
if [[ "${DUMPREQ_COUNT}" -gt 0 && "${ALLOW_INTERNAL}" -ne 1 ]]; then
    echo "refusing: ${CTR_3DSX} is an internal build (contains dumpreq); pass --allow-internal to override" >&2
    exit 1
fi

rm -rf "${PACKAGE_DIR}"
mkdir -p "${PACKAGE_DIR}/3ds/ctr_native/assets"

cp "${CTR_3DSX}" "${PACKAGE_DIR}/3ds/ctr_native/ctr_native.3dsx"
cp LICENSE "${PACKAGE_DIR}/"
cp THIRD_PARTY_NOTICES.md "${PACKAGE_DIR}/"

cat >"${PACKAGE_DIR}/3ds/ctr_native/assets/PUT_YOUR_DISC_IMAGE_HERE.txt" <<'EOF'
Copy your own NTSC-U retail Crash Team Racing disc image into this folder,
named ctr-u.bin. It must be the raw MODE2/2352 BIN layout (not a cooked
2048-byte ISO), so the XA/STR sectors that audio and FMV need are present.
Nothing playable is included in this download -- you must provide your own
legally-owned disc dump.
EOF

cat >"${PACKAGE_DIR}/README.txt" <<EOF
CTR 3D ${VERSION} for New Nintendo 3DS

A homebrew port of Crash Team Racing (PS1, 1999). CTR 3D is a fork of
CTR Native, the native PC port built on the CTR-ModSDK decompilation.

Requirements:
- New Nintendo 3DS, New 3DS XL, or New 2DS XL only -- the original
  3DS/2DS is not supported.
- Custom firmware with the Homebrew Launcher (Luma3DS).
- One-time DSP firmware dump (Rosalina -> Miscellaneous options ->
  Dump DSP firmware), or audio will be silent.
- Your own NTSC-U retail CTR disc image. Nothing playable is included.

Install:
1. Extract this zip to your SD card root. It mirrors the SD card
   layout, so the game ends up at /3ds/ctr_native/ctr_native.3dsx.
2. Copy your disc image to /3ds/ctr_native/assets/ctr-u.bin -- raw
   MODE2/2352 BIN, not a cooked ISO.
3. Launch "CTR 3D" from the Homebrew Launcher.

Saves are written to /3ds/ctr_native/memcards/, the same files the PC
port uses.

Controls follow the button's position on the PlayStation pad:
  B = accelerate (Cross)      A = use weapon (Circle)
  Y = brake / reverse (Square)   X = map / speedometer (Triangle)
  L, R = hop and power-slide (L1, R1)   START = pause
In menus, A and B confirm; X and Y go back.

Screens: the bottom screen always shows the in-race map. The top screen
starts on the speedometer; X switches it to the map, as Triangle does on
PS1. The 3D slider works in real time.

The main menu has an OPTIONS entry with four toggles: PAL PENTA,
RESERVES METER, BOTTOM MAP, and TEXTURE FILTER. VS and Battle modes are
not offered.

Reporting a bug: create an empty file at
/3ds/ctr_native/enable-log.txt, reproduce the problem, then attach
/ctr3ds.log from the SD card root. Delete enable-log.txt afterwards --
logging costs a small stutter every couple of seconds.

License and third-party notices: see LICENSE and THIRD_PARTY_NOTICES.md
in this zip.
EOF

(
    cd "${PACKAGE_DIR}"
    python -m zipfile -c "../${PACKAGE_NAME}.zip" README.txt LICENSE THIRD_PARTY_NOTICES.md 3ds
)

echo "Wrote ${DIST_DIR}/${PACKAGE_NAME}.zip"
