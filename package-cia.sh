#!/usr/bin/env bash
# Build an installable New 3DS CIA from the native 3DS ELF.
# The disc image is intentionally kept on the SD card and is never packaged.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-3ds}"
OUT_DIR="${OUT_DIR:-${ROOT_DIR}/dist}"
ART="${CTR_ART:-${ROOT_DIR}/cia-assets/ctr3d-art-source.png}"
VERSION="${1:-beta-1.0}"
CIA_PATH="${OUT_DIR}/ctr3d-${VERSION}.cia"

need_tool() {
    local name="$1"
    local value="$2"
    [[ -n "${value}" ]] || {
        echo "ERROR: missing required tool: ${name}" >&2
        echo "Install the devkitPro 3DS tools (makerom and bannertool) and retry." >&2
        exit 1
    }
}

MAKEROM_BIN="${MAKEROM:-$(command -v makerom 2>/dev/null || true)}"
BANNERTOOL_BIN="${BANNERTOOL:-$(command -v bannertool 2>/dev/null || true)}"
need_tool makerom "${MAKEROM_BIN}"
need_tool bannertool "${BANNERTOOL_BIN}"

ELF="${CTR_ELF:-${BUILD_DIR}/ctr_native.elf}"
if [[ ! -f "${ELF}" && -f "${BUILD_DIR}/ctr_native" ]]; then
    ELF="${BUILD_DIR}/ctr_native"
fi
if [[ ! -f "${ELF}" ]]; then
    echo "ERROR: missing 3DS ELF: ${ELF}" >&2
    echo "Build the 3DS target first, or set CTR_ELF=/path/to/ctr_native.elf." >&2
    exit 1
fi

if [[ ! -f "${ART}" ]]; then
    echo "ERROR: missing artwork: ${ART}" >&2
    echo "Set CTR_ART=/path/to/art.png or place the artwork at cia-assets/ctr3d-art-source.png." >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ctr3d-cia.XXXXXX")"
trap 'rm -rf "${WORK_DIR}"' EXIT

resize_art() {
    local source="$1"
    local height="$2"
    local width="$3"
    local output="$4"
    if command -v sips >/dev/null 2>&1; then
        sips -z "${height}" "${width}" "${source}" --out "${output}" >/dev/null
    elif command -v magick >/dev/null 2>&1; then
        magick "${source}" -resize "${width}x${height}!" "${output}"
    elif command -v convert >/dev/null 2>&1; then
        convert "${source}" -resize "${width}x${height}!" "${output}"
    else
        echo "ERROR: need sips, ImageMagick magick, or ImageMagick convert to resize artwork." >&2
        exit 1
    fi
}

# bannertool expects a small square icon and a 256x128 banner image. The source
# artwork supplied for this fork is square, so the same art is resized for both
# surfaces; a preformatted banner can be supplied with CTR_BANNER_PNG.
ICON_PNG="${WORK_DIR}/icon.png"
BANNER_PNG="${CTR_BANNER_PNG:-${WORK_DIR}/banner.png}"
resize_art "${ART}" 48 48 "${ICON_PNG}"
if [[ -z "${CTR_BANNER_PNG:-}" ]]; then
    resize_art "${ART}" 128 256 "${BANNER_PNG}"
fi

SMDH="${WORK_DIR}/ctr3d.smdh"
BANNER="${WORK_DIR}/ctr3d.bnr"
"${BANNERTOOL_BIN}" makesmdh \
    -s "CTR 3D" \
    -l "Crash Team Racing for New 3DS" \
    -p "CTR 3D" \
    -f visible allow3d \
    -i "${ICON_PNG}" \
    -o "${SMDH}"

if [[ -n "${CTR_BANNER_AUDIO:-}" ]]; then
    "${BANNERTOOL_BIN}" makebanner -i "${BANNER_PNG}" -a "${CTR_BANNER_AUDIO}" -o "${BANNER}"
else
    # bannertool requires a tune even for a static banner. Generate a short
    # silent PCM WAV unless the caller supplied custom banner audio.
    SILENT_WAV="${WORK_DIR}/silent.wav"
    if command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg -loglevel error -f lavfi -i anullsrc=channel_layout=stereo:sample_rate=44100 \
            -t 1 -c:a pcm_s16le "${SILENT_WAV}"
    elif command -v python3 >/dev/null 2>&1; then
        python3 - "${SILENT_WAV}" <<'PYEOF'
import sys
import wave

with wave.open(sys.argv[1], "wb") as out:
    out.setnchannels(2)
    out.setsampwidth(2)
    out.setframerate(44100)
    out.writeframes(b"\0" * 44100 * 2 * 2)
PYEOF
    else
        echo "ERROR: provide CTR_BANNER_AUDIO or install ffmpeg/python3 to create silent banner audio." >&2
        exit 1
    fi
    "${BANNERTOOL_BIN}" makebanner -i "${BANNER_PNG}" -a "${SILENT_WAV}" -o "${BANNER}"
fi

"${MAKEROM_BIN}" \
    -f cia \
    -target t \
    -exefslogo \
    -o "${CIA_PATH}" \
    -elf "${ELF}" \
    -rsf "${ROOT_DIR}/cia/ctr3d.rsf" \
    -icon "${SMDH}" \
    -banner "${BANNER}"

echo "Wrote ${CIA_PATH}"
