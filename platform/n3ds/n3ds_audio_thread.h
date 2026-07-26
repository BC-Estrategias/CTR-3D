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
 * ndsp output sink. Submit one vblank's rendered stereo PCM (interleaved
 * L,R s16 -- passed as plain short to keep this header free of game or
 * libctru types) for playback. frameCount must be exactly one vblank
 * (NATIVE_AUDIO_VBLANK_FRAMES = 735; hand-matched, see
 * N3DS_NDSP_BLOCK_FRAMES in the .c). Returns 1 if the block was queued, 0
 * if it was dropped (sink down, wrong size, or pool full -- all counted).
 * Called by exactly one thread at any point in the process lifetime: the
 * worker when one exists, else the main thread's StepVBlank fallback.
 */
int N3DS_NdspSubmitVBlankBlock(const short *frames, int frameCount);

/* Did ndspInit succeed? False also before audio init and after shutdown.
 * When false, submits are refused and the build behaves like the old muted
 * one (mixer still renders; nothing is audible). */
int N3DS_NdspIsUp(void);

/* Diagnostics for the renderer's periodic audio shape line: current
 * queued/playing block count (0..pool size), and cumulative blocks dropped
 * because the pool was full (or a submit was mis-sized). */
int N3DS_NdspGetQueuedBlocks(void);
int N3DS_NdspGetDroppedBlocks(void);

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
