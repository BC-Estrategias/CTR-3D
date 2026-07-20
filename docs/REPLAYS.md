# Replays

Use this for bug reports in internal builds.

> Changing the SPU mixer? You want the **mixer golden test** at the bottom of
> this file, not a replay. It does not need the game to be deterministic.

## Quick State

- `F5`: save `debug/states/quick.ctrstates`
- `F8`: load `debug/states/quick.ctrstates`

## Record

```sh
build/ctr_native --record
```

Windows: use `build\ctr_native.exe` instead.

Normal saves live in `memcards/slot0`.

When recording starts, the CTR save files from `memcards/slot0` and `slot1` are copied to `memcard.seed`. The game records with a writable copy named `memcard.recording`, so saves and ghosts made while recording stay in the report.

To choose when recording starts:

```sh
build/ctr_native --record --toggle
```

- Press `F9` to start.
- Press `F10` to stop.

For more detailed reports:

```sh
build/ctr_native --record --detailed
```

You can combine both:

```sh
build/ctr_native --record --toggle --detailed
```

## Play Back

Use the command written in that folder's `metadata.txt`.

It looks like:

```sh
build/ctr_native --replay "debug/reports/20260605/ctr-123456/input.ctrreplay"
```

Playback creates a fresh writable `memcard.playback` from `memcard.seed` every run and does not touch your real saves.

If a developer asks you to bypass header identity checks:

```sh
build/ctr_native --replay "debug/reports/20260605/ctr-123456/input.ctrreplay" --replay-bypass-header
```

## Mixer golden test

For changes to `NativeAudio_MixFrame` that are supposed to leave the audio
*identical* — the per-voice-across-a-block restructure, deferring the arena
reclaim, skipping a decayed reverb tail.

The mixer is a pure function of SPU state, so this does not need a replay: it
needs a fixed *input*, not a deterministic game. Capture one SPU state, then
render from it as often as you like.

```sh
# 1. Capture a state. Run until the mixer is actually busy, then it exits.
build/ctr_native --audio-capture debug/audio/mixer.spustate --audio-capture-vblank 3600

# 2. Baseline BEFORE you change anything.
build/ctr_native --audio-golden debug/audio/mixer.spustate

# 3. Make the change, run step 2 again. The checksum must not move.
```

`--audio-golden-raw out.raw` also dumps the PCM if you want to diff or listen
to it (signed 16-bit LE stereo, 44100 Hz).

**Check what the capture reports.** It prints `liveVoices`, `reverbSamples` and
`xaActive`, and warns if the state exercises nothing. A vector that misses the
expensive path proves nothing — this project once recorded "reverb is never
configured" from samples taken before the game reached content that enables it,
and the reverb chain alone later measured ~115 ms/frame. Keep one vector with
reverb off and one with `reverbSamples` non-zero.

Checksums are FNV-1a over the rendered output and are **only meaningful against
their own state file**. `debug/` is gitignored, so a fresh checkout has no
fixtures: capture your own and take a new baseline before changing anything.
The value only has to be stable across your own before/after runs.
