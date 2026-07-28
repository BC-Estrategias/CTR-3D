/*
 * Boundary between the game's unity translation unit and libctru, for the
 * 3DS audio mixer thread. Same rule as n3ds_backend.h (see that file's
 * top-of-file comment for the full three-way header collision this avoids):
 * this header exposes NOTHING from <3ds.h> -- no libctru types, no libctru
 * constants -- and is safe to include from the unity build.
 *
 * n3ds_audio_thread.c is therefore compiled as its own translation unit,
 * same as n3ds_backend.c -- it sees libctru and never sees the game headers.
 * Everything crossing this line uses plain C scalar types.
 *
 * See docs/AUDIO_THREADING_DESIGN.md for the full design this implements.
 */

#ifndef N3DS_AUDIO_THREAD_H
#define N3DS_AUDIO_THREAD_H

/*
 * Try to create the New3DS core-1 mixer worker thread, and bring up the
 * ndsp output sink (see the block of N3DS_Ndsp* functions below). Call
 * exactly once per "3DS audio subsystem is finishing initialization" event
 * -- safe to call more than once across the process lifetime (e.g.
 * NativeAudio_SpuInit runs on every XA play request, not just at boot): if
 * a worker thread is already running, this is a cheap no-op rather than
 * re-initializing locks that may be in use or creating a second thread that
 * would orphan the first.
 *
 * Locks/event/counters are initialized unconditionally on the first real
 * call -- N3DS_AudioLock/N3DS_AudioUnlock must be safe to call from the ~30
 * existing native_audio.c call sites no matter what this function decides
 * about the worker thread.
 *
 * Falls back cleanly (leaves N3DS_AudioThreadIsRunning() false, never spawns
 * a same-core thread) if this is not a New3DS, or if
 * APT_SetAppCpuTimeLimit/threadCreate fails to secure real core-1 time. The
 * ndsp sink comes up (or fails) independently of the worker: the StepVBlank
 * fallback submits from the main thread when no worker exists.
 */
void N3DS_AudioThreadInit(void);

/*
 * O(1): bump the target-vblank counter under the small counter lock and
 * signal the worker. Call once per vblank from the main thread when
 * N3DS_AudioThreadIsRunning() is true. Never blocks on the render lock.
 */
void N3DS_AudioThreadNotifyVBlank(void);

/*
 * Stop and join the worker thread, if one was ever created. No-op
 * (null-safe) if N3DS_AudioThreadInit was never called, or never reached a
 * running thread -- safe to call unconditionally from NativeAudio_Shutdown.
 */
void N3DS_AudioThreadShutdown(void);

/* Did Init actually secure a real core-1 worker thread? If false, the
 * caller must render (and discard) synchronously on the main thread instead
 * -- see NativeAudio_StepVBlank's 3DS fallback branch. */
int N3DS_AudioThreadIsRunning(void);

/* Diagnostic: target - rendered, i.e. how many vblanks of mixing the worker
 * is currently behind. Reads under the small counter lock. */
int N3DS_AudioThreadGetBacklog(void);

/* Diagnostic: cumulative count of vblanks whose render request was dropped
 * because the backlog was already at the cap -- see
 * NATIVE_AUDIO_THREAD_MAX_BACKLOG_VBLANKS in n3ds_audio_thread.c. */
int N3DS_AudioThreadGetDropped(void);

/*
 * Where one reporting window of the worker's time went, in microseconds --
 * the decomposition hardware run 6 asked for (audioDropped 7,661 on an
 * XA-music track proved the worker is the frame governor there, but not
 * which of its jobs is the cost). Reading this DRAINS it: every field is
 * per-window, not since-boot, so the renderer can print it directly.
 *
 * Read the fields against each other, not in isolation: `wakes` should be
 * ~60 per second of window -- materially fewer means the loop itself
 * overruns its 16.7ms budget -- and lockWaitUs is time the game thread held
 * the render lock against the worker, i.e. contention, not work.
 * `renderUs` is the render call alone (lock wait excluded); `lifecycleUs` +
 * `mirrorUs` are its two halves, timed on the game side of the boundary.
 *
 * `parcelUs` and `pumpUs` are wall time INCLUDING any wait for the render
 * lock, because both take it deep inside native_audio.c. lockWaitUs is the
 * worker's total wait across all three, so subtract it before calling either
 * of them expensive -- a worker stuck behind a main-thread bulk load
 * otherwise reads as costly XA work when it is really contention.
 */
struct N3DS_AudioWorkerProfile
{
	int wakes;
	int vblanks;
	int parcels;
	int lockWaitUs;
	int renderUs;
	int lifecycleUs;
	int mirrorUs;
	int parcelUs;
	int pumpUs;
};

void N3DS_AudioThreadGetProfile(struct N3DS_AudioWorkerProfile *out);

/* svcGetSystemTick, exposed so the unity TU can time its own side of the
 * boundary on the same clock the worker uses (it cannot include <3ds.h>).
 * Returns 0 off-3DS callers never see -- this header is 3DS-only. */
unsigned long long N3DS_AudioTicksNow(void);

/*
 * Real lock backing SDL_LockAudioStream/SDL_UnlockAudioStream on 3DS (see
 * sdl3_compat_3ds.c). Protects s_audio (native_audio.c) during rendering --
 * the same role the real SDL_AudioStream lock already plays for PC via
 * NativeAudio_LockOutput/UnlockOutput. Held by the worker thread only for
 * one vblank's render call at a time, never across its whole backlog-drain
 * loop.
 */
void N3DS_AudioLock(void);
void N3DS_AudioUnlock(void);

/*
 * ndsp output sink. Submit one vblank's worth of stereo PCM (interleaved
 * L,R s16 -- passed as plain short to keep this header free of game or
 * libctru types) for playback.
 *
 * sampleRate is the rate the samples were PRODUCED at, not 44100 by
 * assumption: session 22 feeds the music channel XA source PCM at its own
 * 37800/18900 rate and lets the DSP resample in hardware, which deleted the
 * CPU resampler that cost 52-56ms per 16.7ms parcel on the ARM11. The
 * worker's XA path therefore submits 630 (or 315) frames at 37800 (or
 * 18900); the no-worker StepVBlank fallback still submits 735 at 44100
 * because it runs the full software mixer. Both are valid -- the sink pokes
 * ndspChnSetRate on change and counts one vblank as one "parcel" either
 * way, so the worker's cushion accounting is unit-stable across rates.
 * Exactly one of those two producers exists for the whole process lifetime.
 *
 * gainLeft/gainRight scale the channel (ndspChnSetMix, poked on change):
 * the XA path hands its CD+master volume here instead of multiplying every
 * sample. The fallback passes 1.0f -- its mixer already applied volume.
 *
 * frameCount must be 1..N3DS_NDSP_PARCEL_FRAMES_MAX. Returns 1 if queued, 0
 * if dropped (sink down, bad size, or pool full -- all counted).
 */
int N3DS_NdspSubmitMusicParcel(const short *frames, int frameCount, int sampleRate, float gainLeft, float gainRight);

/* Did ndspInit succeed? False also before audio init and after shutdown.
 * When false, submits are refused and the build behaves like the old muted
 * one (mixer still renders; nothing is audible). */
int N3DS_NdspIsUp(void);

/* Diagnostics for the renderer's periodic audio shape line: current
 * queued/playing parcel count (one vblank of audio each, whatever the source
 * rate), and cumulative parcels dropped because the pool was full (or a
 * submit was mis-sized). */
int N3DS_NdspGetQueuedBlocks(void);
int N3DS_NdspGetDroppedBlocks(void);

/* Sink aggregation mode receipts (session 21): cumulative counts of full
 * 8-parcel aggregate submissions vs early anti-underrun small flushes. On
 * hardware, full >> early means the seam-rate fix actually engaged; early
 * dominating means the DSP kept running dry and the sink degenerated to
 * crumbs (Azahar always looks like the latter -- its HLE gulps the queue --
 * so only a hardware log can score this). */
void N3DS_NdspGetFlushCounts(int *fullFlushes, int *earlyFlushes);

/*
 * Hardware voices (plan B, session 20): SPU voices 0..22 play on ndsp's
 * own channels; the parcel sink above becomes the music channel (23). SPU
 * voice 23 is sacrificed (lifecycle only, denials counted). All functions
 * are called from the single mixing thread (worker, or the StepVBlank
 * fallback); pointers passed here must come from N3DS_HwVoiceStoreAlloc,
 * whose linear memory the DSP reads directly.
 *
 * Store contract: Alloc a slot, decode into it, Commit (cache flush) --
 * then it may be handed to N3DS_HwVoiceStart. StoreReset silences every
 * hardware channel and recycles the whole store (SPU bank re-upload);
 * callers must re-Alloc afterwards.
 */
short *N3DS_HwVoiceStoreAlloc(int sampleCount);
void N3DS_HwVoiceStoreCommit(const short *pcm, int sampleCount);
void N3DS_HwVoiceStoreReset(void);
void N3DS_HwVoiceStart(int voice, const short *pcm, int sampleCount, int loopStart, int loopEnd, int loopEnabled, int startSample, float rateHz);
void N3DS_HwVoiceSetRate(int voice, float rateHz);
void N3DS_HwVoiceSetMix(int voice, float left, float right);
void N3DS_HwVoiceStop(int voice);
int N3DS_HwVoiceGetDenied(void);
int N3DS_HwVoiceStoreGetUsedKiB(void);

#endif
