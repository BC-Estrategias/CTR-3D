/*
 * libctru side of the audio-thread boundary. This file is its own
 * translation unit and must never include game headers -- see
 * n3ds_backend.h for why (same collision, same isolation style). See
 * docs/AUDIO_THREADING_DESIGN.md for the full design this implements,
 * including "What changed from v1" -- the lock-scoping and core-selection
 * fixes below are not incidental, they are the actual point of the design.
 *
 * This file also owns the ndsp output sink: a small ring of linear-memory
 * wave buffers the mixer (worker thread, or the StepVBlank fallback) submits
 * one vblank's rendered stereo PCM into. ndsp resamples 44100 Hz -> the
 * DSP's native output rate per channel, so the mixer keeps producing the
 * exact PSX SPU rate it always has.
 */

#include <3ds.h>
#include <string.h>
#include <stdio.h>

#include "n3ds_audio_thread.h"
#include "n3ds_backend.h" /* N3DS_DebugLog only -- plain-scalar boundary header, no game types */

/*
 * Defined in native_audio.c (the unity build's own translation unit).
 * NativeAudio_RenderFramesNoLock itself is `internal` (static, macros.h:74),
 * so it has no external symbol this file could link against, and this file
 * cannot include native_audio.c's own headers to share a prototype -- doing
 * so would pull the game's PSX headers into this libctru-visible TU, which
 * is exactly the collision n3ds_backend.h's isolation exists to avoid.
 * NativeAudio_RenderVBlankForAudioThread is a small external-linkage wrapper
 * native_audio.c exposes specifically for this cross-TU call: it owns its
 * own scratch buffer and hands the rendered samples to
 * N3DS_NdspSubmitVBlankBlock below, so nothing PSX-typed needs to cross
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

/*
 * One vblank of stereo PCM at the mixer's fixed PSX SPU rate. Hand-matched
 * to NATIVE_AUDIO_VBLANK_FRAMES (native_audio.c: 44100/60 = 735) the same
 * way the extern prototypes above are hand-matched: the game-side header
 * cannot cross this boundary. N3DS_NdspSubmitVBlankBlock rejects (and
 * counts) any submit whose frame count disagrees, so a drift here is loud
 * in the counters rather than silent corruption.
 */
#define N3DS_NDSP_BLOCK_FRAMES 735
/*
 * Pool depth mirrors the PC output queue's cushion
 * (NATIVE_AUDIO_SCHEDULED_QUEUE_FRAMES = 16 vblanks, ~267ms). Production is
 * paced by the vblank emitter (60 Hz real-time, catch-up capped at 8) and
 * ndsp drains at exactly 44100 frames/s, so steady-state queue depth stays
 * wherever the last catch-up burst left it -- the pool only has to absorb
 * that burst plus clock drift. A full pool drops the newest block (counted,
 * reported in the renderer's audio shape line), never blocks the mixer.
 */
#define N3DS_NDSP_BLOCK_COUNT 16
#define N3DS_NDSP_CHANNEL     0

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

/*
 * ndsp sink state. Written only from N3DS_AudioThreadInit/Shutdown (main
 * thread) and N3DS_NdspSubmitVBlankBlock. Submit runs on exactly one thread
 * at any point in the process lifetime -- the worker when one exists, else
 * the main thread's StepVBlank fallback (N3DS_AudioThreadIsRunning never
 * flips true after init) -- so the ring cursor needs no lock of its own.
 */
static int s_ndspInitAttempted;
static int s_ndspUp;
static s16 *s_ndspPcm;                                  /* linearAlloc: BLOCK_COUNT x BLOCK_FRAMES x stereo */
static ndspWaveBuf s_ndspBufs[N3DS_NDSP_BLOCK_COUNT];   /* statuses start 0 = NDSP_WBUF_FREE */
static int s_ndspNextBuf;
static u32 s_ndspDroppedBlocks;

static void N3DS_NdspSinkInit(void)
{
	Result rc;
	char line[96];
	float mix[12];

	if (s_ndspInitAttempted)
	{
		/* Same idempotency contract as the worker thread below: Init is
		 * re-entered on every XA play request via NativeAudio_SpuInit. */
		return;
	}
	s_ndspInitAttempted = 1;

	rc = ndspInit();
	if (R_FAILED(rc))
	{
		/*
		 * The classic homebrew cause is a missing DSP firmware dump
		 * (sdmc:/3ds/dspfirm.cdc, dumped once with the DSP1 homebrew) --
		 * newer Luma3DS setups load it from NAND without the file, so on
		 * this project's target setup success is expected. Failing here is
		 * clean: the mixer keeps rendering (lifecycle stays exact) and
		 * every submit is refused, i.e. exactly the old muted behavior.
		 */
		snprintf(line, sizeof(line), "[CTR-3DS] ndsp init FAILED (0x%08lx) -- dspfirm.cdc? audio stays muted", (unsigned long)rc);
		N3DS_DebugLog(line);
		return;
	}

	s_ndspPcm = (s16 *)linearAlloc((size_t)N3DS_NDSP_BLOCK_COUNT * N3DS_NDSP_BLOCK_FRAMES * 2 * sizeof(s16));
	if (s_ndspPcm == NULL)
	{
		N3DS_DebugLog("[CTR-3DS] ndsp: linearAlloc failed, audio stays muted");
		ndspExit();
		return;
	}

	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnReset(N3DS_NDSP_CHANNEL);
	ndspChnSetInterp(N3DS_NDSP_CHANNEL, NDSP_INTERP_POLYPHASE);
	ndspChnSetRate(N3DS_NDSP_CHANNEL, 44100.0f); /* PSX SPU rate; DSP resamples to its native output rate */
	ndspChnSetFormat(N3DS_NDSP_CHANNEL, NDSP_FORMAT_STEREO_PCM16);
	memset(mix, 0, sizeof(mix));
	mix[0] = 1.0f; /* front left */
	mix[1] = 1.0f; /* front right */
	ndspChnSetMix(N3DS_NDSP_CHANNEL, mix);

	s_ndspUp = 1;
	snprintf(line, sizeof(line), "[CTR-3DS] ndsp: up (%d blocks x %d frames)", N3DS_NDSP_BLOCK_COUNT, N3DS_NDSP_BLOCK_FRAMES);
	N3DS_DebugLog(line);
}

static void N3DS_NdspSinkShutdown(void)
{
	/* Caller (N3DS_AudioThreadShutdown) has already joined the worker, and
	 * the fallback submit path runs on the same thread this does -- no
	 * submit can be in flight here. */
	if (s_ndspUp)
	{
		ndspChnWaveBufClear(N3DS_NDSP_CHANNEL);
		ndspExit();
	}
	if (s_ndspPcm != NULL)
	{
		linearFree(s_ndspPcm);
		s_ndspPcm = NULL;
	}
	s_ndspUp = 0;
	s_ndspInitAttempted = 0; /* a later re-Init may bring the sink back up */
	s_ndspNextBuf = 0;
	memset(s_ndspBufs, 0, sizeof(s_ndspBufs));
}

int N3DS_NdspSubmitVBlankBlock(const short *frames, int frameCount)
{
	ndspWaveBuf *buf;
	s16 *slot;

	if (!s_ndspUp)
	{
		return 0;
	}
	if (frameCount != N3DS_NDSP_BLOCK_FRAMES)
	{
		/* Hand-matched constant drifted (see N3DS_NDSP_BLOCK_FRAMES) --
		 * refuse loudly-in-counters rather than under/overrun the slot. */
		s_ndspDroppedBlocks++;
		return 0;
	}

	buf = &s_ndspBufs[s_ndspNextBuf];
	if ((buf->status == NDSP_WBUF_QUEUED) || (buf->status == NDSP_WBUF_PLAYING))
	{
		/* Pool full: drop the newest block (this one), never stall the
		 * mixer -- see the N3DS_NDSP_BLOCK_COUNT comment. */
		s_ndspDroppedBlocks++;
		return 0;
	}

	slot = s_ndspPcm + (size_t)s_ndspNextBuf * N3DS_NDSP_BLOCK_FRAMES * 2;
	memcpy(slot, frames, (size_t)N3DS_NDSP_BLOCK_FRAMES * 2 * sizeof(s16));
	DSP_FlushDataCache(slot, (u32)(N3DS_NDSP_BLOCK_FRAMES * 2 * sizeof(s16)));

	memset(buf, 0, sizeof(*buf));
	buf->data_pcm16 = slot;
	buf->nsamples = N3DS_NDSP_BLOCK_FRAMES; /* per-channel frame count, not halfword total */
	buf->looping = false;
	ndspChnWaveBufAdd(N3DS_NDSP_CHANNEL, buf);

	s_ndspNextBuf = (s_ndspNextBuf + 1) % N3DS_NDSP_BLOCK_COUNT;
	return 1;
}

int N3DS_NdspIsUp(void)
{
	return s_ndspUp;
}

int N3DS_NdspGetQueuedBlocks(void)
{
	int queued = 0;
	int i;

	for (i = 0; i < N3DS_NDSP_BLOCK_COUNT; i++)
	{
		if ((s_ndspBufs[i].status == NDSP_WBUF_QUEUED) || (s_ndspBufs[i].status == NDSP_WBUF_PLAYING))
		{
			queued++;
		}
	}

	return queued;
}

int N3DS_NdspGetDroppedBlocks(void)
{
	return (int)s_ndspDroppedBlocks;
}

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

void N3DS_AudioThreadInit(void)
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
	 * here down, no matter what the checks below decide --
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

	/*
	 * The ndsp sink comes up regardless of whether a worker thread does:
	 * the StepVBlank fallback path submits to it from the main thread when
	 * no worker could be secured. Guarded internally against the repeat
	 * calls described above.
	 */
	N3DS_NdspSinkInit();

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
	if (s_audioThreadRunning && (s_audioThread != NULL))
	{
		s_audioThreadShouldStop = 1;
		LightEvent_Signal(&s_audioWakeEvent);
		threadJoin(s_audioThread, U64_MAX); /* documented infinite-wait sentinel */
		threadFree(s_audioThread);          /* valid only because detached=false at creation */
		s_audioThread = NULL;
		s_audioThreadRunning = 0;
	}

	/* After the join (or if no worker ever existed): nothing can be
	 * submitting anymore, from any thread -- safe to tear the sink down.
	 * No-op if the sink never came up. */
	N3DS_NdspSinkShutdown();
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
