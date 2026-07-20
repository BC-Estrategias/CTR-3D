/*
 * libctru side of the audio-thread boundary. This file is its own
 * translation unit and must never include game headers -- see
 * n3ds_backend.h for why (same collision, same isolation style). See
 * docs/AUDIO_THREADING_DESIGN.md for the full design this implements,
 * including "What changed from v1" -- the lock-scoping and core-selection
 * fixes below are not incidental, they are the actual point of the design.
 */

#include <3ds.h>

#include "n3ds_audio_thread.h"

/*
 * Defined in native_audio.c (the unity build's own translation unit).
 * NativeAudio_RenderFramesNoLock itself is `internal` (static, macros.h:74),
 * so it has no external symbol this file could link against, and this file
 * cannot include native_audio.c's own headers to share a prototype -- doing
 * so would pull the game's PSX headers into this libctru-visible TU, which
 * is exactly the collision n3ds_backend.h's isolation exists to avoid.
 * NativeAudio_RenderVBlankForAudioThread is a small external-linkage wrapper
 * native_audio.c exposes specifically for this cross-TU call: it owns its
 * own scratch buffer and discards the rendered samples itself (this
 * design's discard-not-queue decision), so nothing PSX-typed needs to cross
 * this boundary -- the signature is deliberately void->void. There is no
 * shared header enforcing this prototype; if that wrapper's signature ever
 * changes, this declaration must be updated by hand.
 */
extern void NativeAudio_RenderVBlankForAudioThread(void);

/*
 * Defined in native_audio.c, same cross-TU reasoning as
 * NativeAudio_RenderVBlankForAudioThread above: NativeAudio_XAStreamPump is
 * `internal` (static, macros.h:74), scoped to that file's own translation
 * unit, so this file has no external symbol to link against without a
 * wrapper. Unlike the render wrapper, no PSX-typed buffer needs to cross
 * this boundary at all -- NativeAudio_XAStreamPump is already void->void,
 * so the wrapper is a pure pass-through with no scratch buffer of its own.
 *
 * Locking discipline this file's caller (N3DS_AudioThreadMain below) must
 * uphold: NativeAudio_XAStreamPump() internally takes
 * NativeAudio_LockOutput()/UnlockOutput(), which resolve on this platform
 * to s_audioRenderLock via N3DS_AudioLock/N3DS_AudioUnlock -- the SAME lock
 * the render call above takes. LightLock is non-recursive, so this must
 * only be called AFTER s_audioRenderLock has been released, never nested
 * inside that lock's held span, or the worker thread self-deadlocks trying
 * to re-lock a lock it already holds. See docs/AUDIO_THREADING_DESIGN.md,
 * "Verification findings".
 */
extern void NativeAudio_XAPumpForAudioThread(void);

/*
 * Reuses NATIVE_VSYNC_CATCHUP_MAX's existing value and precedent (the
 * render-side vblank catch-up loop, native_platform.c) -- same shape of
 * problem, same answer, not a new arbitrary number. See
 * docs/AUDIO_THREADING_DESIGN.md, "The XA lag question".
 */
#define NATIVE_AUDIO_THREAD_MAX_BACKLOG_VBLANKS 8

/* Protects s_audio (native_audio.c) during MixFrame -- backs
 * N3DS_AudioLock/N3DS_AudioUnlock, which back SDL_Lock/UnlockAudioStream. */
static LightLock s_audioRenderLock;
/* Protects ONLY the three counters below -- held for a handful of
 * instructions, never across rendering. */
static LightLock s_audioCounterLock;
/* RESET_ONESHOT -- main thread signals, worker thread wakes. */
static LightEvent s_audioWakeEvent;
static u32 s_targetVBlanks;   /* written only by main thread, under s_audioCounterLock */
static u32 s_renderedVBlanks; /* written only by worker thread, under s_audioCounterLock */
static u32 s_droppedVBlanks;  /* diagnostic only, written only by main thread, under s_audioCounterLock */
static Thread s_audioThread;  /* NULL if never created (fallback path) or after join */
static volatile int s_audioThreadShouldStop;
static volatile int s_audioThreadRunning; /* set true only after a real thread on core 1 is confirmed running */

static void N3DS_AudioThreadMain(void *arg)
{
	(void)arg;

	for (;;)
	{
		LightEvent_Wait(&s_audioWakeEvent);
		if (s_audioThreadShouldStop)
		{
			break;
		}

		for (;;) /* drain the current backlog, one vblank at a time */
		{
			int hasWork;

			LightLock_Lock(&s_audioCounterLock);
			hasWork = (s_renderedVBlanks < s_targetVBlanks);
			LightLock_Unlock(&s_audioCounterLock);
			if (!hasWork)
			{
				break;
			}

			LightLock_Lock(&s_audioRenderLock); /* backs N3DS_AudioLock / NativeAudio_LockOutput */
			NativeAudio_RenderVBlankForAudioThread();
			LightLock_Unlock(&s_audioRenderLock); /* released BEFORE the next iteration -- this is
			                                          the "what changed from v1" fix: the render
			                                          lock is never held across the whole drain. */

			NativeAudio_XAPumpForAudioThread(); /* separate, sequential step, run AFTER
			                                        s_audioRenderLock is released above, NOT nested
			                                        inside it: NativeAudio_XAStreamPump (called
			                                        inside this wrapper) takes that same lock itself
			                                        via N3DS_AudioLock/N3DS_AudioUnlock, and LightLock
			                                        is non-recursive, so calling it while still
			                                        holding s_audioRenderLock would self-deadlock this
			                                        thread. See docs/AUDIO_THREADING_DESIGN.md,
			                                        "Verification findings". */

			LightLock_Lock(&s_audioCounterLock);
			s_renderedVBlanks++;
			LightLock_Unlock(&s_audioCounterLock);
		}
	}

	threadExit(0);
}

void N3DS_AudioThreadInit(int mixingEnabled)
{
	bool isNew3DS;
	s32 mainPriority;
	s32 audioPriority;

	if (s_audioThreadRunning)
	{
		/*
		 * Idempotent by design, not by accident: NativeAudio_SpuInit (this
		 * function's caller) runs on every NativeAudio_PlayXATrack/
		 * PlayXAFile call, not just once at boot -- i.e. routinely many
		 * times per play session. A worker thread is already alive; leave
		 * it running exactly as-is. Re-running LightLock_Init below on
		 * locks the worker may currently hold, or calling threadCreate
		 * again, would corrupt lock state and orphan/leak the existing
		 * thread (it would still be running, but s_audioThread would no
		 * longer point at it, so Shutdown could never join it).
		 */
		return;
	}

	/*
	 * Locks/event/counters are real and initialized unconditionally from
	 * here down, no matter what mixingEnabled or the checks below decide --
	 * N3DS_AudioLock/N3DS_AudioUnlock (backing SDL_LockAudioStream/
	 * UnlockAudioStream) are reachable from ~30 existing native_audio.c call
	 * sites regardless of whether a worker thread ends up existing, and
	 * locking an uninitialized LightLock is not a defined operation.
	 */
	LightLock_Init(&s_audioRenderLock);
	LightLock_Init(&s_audioCounterLock);
	LightEvent_Init(&s_audioWakeEvent, RESET_ONESHOT);
	s_targetVBlanks = 0;
	s_renderedVBlanks = 0;
	s_droppedVBlanks = 0;
	s_audioThreadShouldStop = 0;
	s_audioThread = NULL;
	s_audioThreadRunning = 0;

	if (!mixingEnabled)
	{
		/*
		 * CTR_3DS_SKIP_SPU_MIX is on (today's default): MixFrame only runs
		 * the cheap voice-lifecycle loop, so a dedicated worker thread would
		 * earn nothing and just burn a core. Leave s_audioThreadRunning
		 * false so the caller's StepVBlank fallback renders synchronously
		 * instead -- at this flag setting that is exactly today's existing,
		 * already-cheap behavior. Net effect: this whole feature is inert
		 * in the current default build configuration.
		 */
		return;
	}

	isNew3DS = false;
	APT_CheckNew3DS(&isNew3DS);
	if (!isNew3DS)
	{
		/* This project targets New3DS-XL only, but fail safe rather than
		 * assume -- never fall back to a same-core thread (see "What
		 * changed from v1" in the design doc: core_id=-1 does NOT mean "any
		 * free core", it means "all CPUs", which resolves to core 0 here). */
		return;
	}

	/*
	 * NOTE: SDL's own n3ds audio backend uses 30 here (SDL_systhread.c) for
	 * a LIGHT callback thread that mostly sleeps waiting on DSP completion.
	 * Our mixer is compute-bound at ~221% of the real-time budget even alone
	 * (session 8 measurement) -- 30% of core 1 would starve it far more than
	 * SDL's use case. Starting at 80 as a first guess; this is a genuine
	 * tunable, not a settled number -- measure actual backlog/drop-rate
	 * during verification and adjust.
	 */
	if (!R_SUCCEEDED(APT_SetAppCpuTimeLimit(80)))
	{
		/* Could not secure core 1 -- do NOT fall back to a same-core
		 * thread; caller must keep rendering synchronously. */
		return;
	}

	mainPriority = 0x30;
	svcGetThreadPriority(&mainPriority, CUR_THREAD_HANDLE);
	audioPriority = mainPriority - 1;
	if (audioPriority < 0x19)
	{
		audioPriority = 0x19;
	}
	if (audioPriority > 0x2F)
	{
		audioPriority = 0x2F;
	}
	/* Clamp mirrors SDL_n3dsaudio.c's real audio-thread convention: 0x18 is
	 * reserved for video, 0x30 is the default for the main thread. Higher
	 * priority than main is fine and intentional here BECAUSE this only
	 * runs once core 1 (a distinct core from main) is confirmed reachable
	 * by the checks above -- never reached on the same core as main. */

	s_audioThread = threadCreate(N3DS_AudioThreadMain, NULL, 32 * 1024, audioPriority, 1 /* system core */, false);

	/* threadCreate returns NULL on failure (thread.h) -- a clean failure
	 * mode, not a crash. If this happens, the caller falls back to
	 * synchronous rendering, same as the two checks above. */
	s_audioThreadRunning = (s_audioThread != NULL);
}

void N3DS_AudioThreadNotifyVBlank(void)
{
	LightLock_Lock(&s_audioCounterLock);
	if ((s_targetVBlanks - s_renderedVBlanks) < NATIVE_AUDIO_THREAD_MAX_BACKLOG_VBLANKS)
	{
		s_targetVBlanks++;
	}
	else
	{
		s_droppedVBlanks++; /* non-silent cap -- see design doc's "XA lag question" */
	}
	LightLock_Unlock(&s_audioCounterLock);
	LightEvent_Signal(&s_audioWakeEvent);
}

void N3DS_AudioThreadShutdown(void)
{
	if (!s_audioThreadRunning || (s_audioThread == NULL))
	{
		return; /* never created -- mixingEnabled was false, the fallback
		           checks bailed, or shutdown was reached via the early
		           init-failure path before Init ever ran */
	}

	s_audioThreadShouldStop = 1;
	LightEvent_Signal(&s_audioWakeEvent);
	threadJoin(s_audioThread, U64_MAX); /* documented infinite-wait sentinel */
	threadFree(s_audioThread);          /* valid only because detached=false at creation */
	s_audioThread = NULL;
	s_audioThreadRunning = 0;
}

int N3DS_AudioThreadIsRunning(void)
{
	return s_audioThreadRunning;
}

int N3DS_AudioThreadGetBacklog(void)
{
	int backlog;

	LightLock_Lock(&s_audioCounterLock);
	backlog = (int)(s_targetVBlanks - s_renderedVBlanks);
	LightLock_Unlock(&s_audioCounterLock);

	return backlog;
}

int N3DS_AudioThreadGetDropped(void)
{
	int dropped;

	LightLock_Lock(&s_audioCounterLock);
	dropped = (int)s_droppedVBlanks;
	LightLock_Unlock(&s_audioCounterLock);

	return dropped;
}

void N3DS_AudioLock(void)
{
	LightLock_Lock(&s_audioRenderLock);
}

void N3DS_AudioUnlock(void)
{
	LightLock_Unlock(&s_audioRenderLock);
}
