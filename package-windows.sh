#!/usr/bin/env bash
# Package the current Windows x86 build for release.

set -euo pipefail

VERSION="${1:-beta-7_1}"
BUILD_DIR="${BUILD_DIR:-build}"
DIST_DIR="${DIST_DIR:-dist}"
PACKAGE_NAME="ctr-native-${VERSION}-windows-x86"
PACKAGE_DIR="${DIST_DIR}/${PACKAGE_NAME}"
EXE_PATH="${CTR_EXE:-${BUILD_DIR}/ctr_native.exe}"

if [[ ! -f "${EXE_PATH}" ]]; then
    echo "ERROR: missing executable: ${EXE_PATH}" >&2
    echo "Run build.bat first, or set CTR_EXE=path." >&2
    exit 1
fi

rm -rf "${PACKAGE_DIR}"
mkdir -p "${PACKAGE_DIR}"

cp "${EXE_PATH}" "${PACKAGE_DIR}/ctr_native.exe"
cp LICENSE "${PACKAGE_DIR}/"
cp THIRD_PARTY_NOTICES.md "${PACKAGE_DIR}/"

cat >"${PACKAGE_DIR}/README.txt" <<EOF
CTR Native Windows x86 ${VERSION} build

Game assets are not included.

Simple setup:
- Put your own NTSC-U retail CTR disc image next to ctr_native.exe, named:

assets/
  ctr-u.bin

- Run:

ctr_native.exe

The disc image must be the common single-track raw PSX BIN layout:
MODE2/2352 sectors, with the data track starting at byte 0.
A cooked 2048-byte ISO does not preserve the XA/STR sector data needed for
audio and video playback.

Extracted asset override:

Extracted files are optional and mostly useful for development, modding, and
debugging. If present, they override files from ctr-u.bin.

assets/
  BIGFILE.BIG
  SOUNDS/KART.HWL
  TEST.STR
  XA/
    ENG.XNF
    ENG/EXTRA/S00.XA ... S05.XA
    ENG/GAME/S00.XA ... S20.XA
    MUSIC/S00.XA ... S01.XA

XA files must preserve CD-XA sector data. Use 2336-byte Mode2/Form2 sector data
or 2352-byte raw sectors. 2048-byte cooked XA extractions are not suitable.

Windows requirements:
- OpenGL 3.3 capable GPU/driver

Reporting a bug:
If the game does not launch or something looks wrong, attach:
- Crash Team Racing.log (written next to ctr_native.exe)
- your GPU and driver version
- what you were doing when it happened
EOF

(
    cd "${PACKAGE_DIR}"
    python -m zipfile -c "../${PACKAGE_NAME}.zip" ctr_native.exe LICENSE THIRD_PARTY_NOTICES.md README.txt
)

echo "Wrote ${DIST_DIR}/${PACKAGE_NAME}.zip"
