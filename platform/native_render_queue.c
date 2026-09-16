/*
 * Render-job queue -- storage, tickets, worker drain loop, dispatcher. See
 * include/platform/native_render_queue.h for the contract.
 * OS primitives (thread, lock,
 * events) live across the plain-scalar boundary in
 * platform/n3ds/n3ds_render_thread.c; this file is part of the unity TU and
 * may call Exec bodies directly.
 */

#include <platform/native_render_queue.h>

#ifndef CTR_NATIVE_RENDER_THREAD
#define CTR_NATIVE_RENDER_THREAD 0
#endif

#ifndef CTR_NATIVE_GTE_CAPTURE
#define CTR_NATIVE_GTE_CAPTURE 0
#endif

/* Runtime A/B (dumpreq 'renderthread', n3ds_backend.c). Checked at post time
 * on the game thread; the OFF transition fences before the first direct call
 * (NativeRenderQueue_Route). */
int g_dbg_renderThread = 1;

#if defined(__3DS__) && CTR_NATIVE_RENDER_THREAD

#include "n3ds/n3ds_render_thread.h"
#include "n3ds/n3ds_backend.h"
#include "n3ds/n3ds_beat.h"

#define NATIVE_RENDER_QUEUE_RING 64

static struct NativeRenderJob s_jobs[NATIVE_RENDER_QUEUE_RING];
static unsigned int s_postedCount;    /* queue-lock guarded */
static unsigned int s_completedCount; /* queue-lock guarded */
static int s_workerUp;
static int s_routeWasActive;

#ifdef CTR_INTERNAL
#include "n3ds/sdl3_compat_3ds.h" /* SDL_GetPerformanceCounter/Frequency shim */

/* qstall probe accumulators -- see NativeRenderQueueDebugStats. Written only on
 * the game thread inside Post/Wait; the worker never calls these. */
static unsigned int s_qsPostCalls;
static unsigned int s_qsPostBlockedCalls;
static unsigned int s_qsWaitCalls;
static unsigned int s_qsPeakDepth;
static u64 s_qsPostBlockTicks;
static u64 s_qsWaitBlockTicks;

static unsigned long NativeRenderQueue_TicksToUs(u64 ticks)
{
	const u64 freq = SDL_GetPerformanceFrequency();

	return freq ? (unsigned long)((ticks * 1000000ull) / freq) : 0UL;
}

void NativeRenderQueue_DebugTakeStats(struct NativeRenderQueueDebugStats *out)
{
	if (out == NULL)
	{
		return;
	}

	out->postCalls = s_qsPostCalls;
	out->postBlockedCalls = s_qsPostBlockedCalls;
	out->waitCalls = s_qsWaitCalls;
	out->peakDepth = s_qsPeakDepth;
	out->postBlockUs = NativeRenderQueue_TicksToUs(s_qsPostBlockTicks);
	out->waitBlockUs = NativeRenderQueue_TicksToUs(s_qsWaitBlockTicks);
	out->ringSize = NATIVE_RENDER_QUEUE_RING;

	s_qsPostCalls = 0;
	s_qsPostBlockedCalls = 0;
	s_qsWaitCalls = 0;
	s_qsPeakDepth = 0;
	s_qsPostBlockTicks = 0;
	s_qsWaitBlockTicks = 0;
}

/* Worker-owned profile accumulators. Written only on the worker thread (inside
 * WorkerMain); TakeWorkerStats reads+zeroes them from the stats block. Lock-free
 * on the same benign-race terms as worker_exec -- a diagnostic, not a fence. */
enum
{
	WPROF_OTAG = 0,
	WPROF_SYNC,
	WPROF_ENDFRAME,
	WPROF_IMAGE,
	WPROF_OTHER,
	WPROF_COUNT
};

static u64 s_wpTicks[WPROF_COUNT];
static u32 s_wpCalls[WPROF_COUNT];
static u64 s_wpIdleTicks;
static u32 s_wpIdleWaits;

static int NativeRenderQueue_WorkerBucket(int kind)
{
	switch (kind)
	{
		case NATIVE_RENDER_JOB_DRAW_OTAG:
		{
			return WPROF_OTAG;
		}
		case NATIVE_RENDER_JOB_DRAW_SYNC:
		{
			return WPROF_SYNC;
		}
		case NATIVE_RENDER_JOB_END_FRAME:
		case NATIVE_RENDER_JOB_PRESENT_VRAM_DISPLAY:
		{
			return WPROF_ENDFRAME;
		}
		case NATIVE_RENDER_JOB_LOAD_IMAGE:
		case NATIVE_RENDER_JOB_LOAD_IMAGE2:
		case NATIVE_RENDER_JOB_MOVE_IMAGE:
		case NATIVE_RENDER_JOB_STORE_IMAGE:
		case NATIVE_RENDER_JOB_CLEAR_IMAGE:
		{
			return WPROF_IMAGE;
		}
		default:
		{
			return WPROF_OTHER;
		}
	}
}

void NativeRenderQueue_DebugTakeWorkerStats(struct NativeRenderWorkerDebugStats *out)
{
	if (out == NULL)
	{
		return;
	}

	out->otagUs = NativeRenderQueue_TicksToUs(s_wpTicks[WPROF_OTAG]);
	out->syncUs = NativeRenderQueue_TicksToUs(s_wpTicks[WPROF_SYNC]);
	out->endFrameUs = NativeRenderQueue_TicksToUs(s_wpTicks[WPROF_ENDFRAME]);
	out->imageUs = NativeRenderQueue_TicksToUs(s_wpTicks[WPROF_IMAGE]);
	out->otherUs = NativeRenderQueue_TicksToUs(s_wpTicks[WPROF_OTHER]);
	out->idleUs = NativeRenderQueue_TicksToUs(s_wpIdleTicks);
	out->otagCalls = s_wpCalls[WPROF_OTAG];
	out->syncCalls = s_wpCalls[WPROF_SYNC];
	out->endFrameCalls = s_wpCalls[WPROF_ENDFRAME];
	out->imageCalls = s_wpCalls[WPROF_IMAGE];
	out->otherCalls = s_wpCalls[WPROF_OTHER];
	out->idleWaits = s_wpIdleWaits;

	memset(s_wpTicks, 0, sizeof(s_wpTicks));
	memset(s_wpCalls, 0, sizeof(s_wpCalls));
	s_wpIdleTicks = 0;
	s_wpIdleWaits = 0;
}
#endif

static void NativeRenderQueue_ExecuteJob(struct NativeRenderJob *job)
{
	switch (job->kind)
	{
		case NATIVE_RENDER_JOB_PUT_DRAWENV:
		{
			CtrGpu_ExecPutDrawEnv(&job->drawEnv);
			break;
		}
		case NATIVE_RENDER_JOB_PUT_DISPENV:
		{
			CtrGpu_ExecPutDispEnv(&job->dispEnv);
			break;
		}
		case NATIVE_RENDER_JOB_SET_DISP_MASK:
		{
			CtrGpu_ExecSetDispMask(job->i0);
			break;
		}
		case NATIVE_RENDER_JOB_DRAW_OTAG:
		{
			CtrGpu_ExecDrawOTag(job->ptr);
			break;
		}
		case NATIVE_RENDER_JOB_END_FRAME:
		{
			Platform_ExecEndFrame();
			break;
		}
		case NATIVE_RENDER_JOB_DRAW_SYNC:
		{
			CtrGpu_ExecDrawSync();
			break;
		}
		case NATIVE_RENDER_JOB_DRAW_PRIM:
		{
			CtrGpu_ExecDrawPrim(job->ptr);
			break;
		}
		case NATIVE_RENDER_JOB_CLEAR_IMAGE:
		{
			CtrGpu_ExecClearImage(&job->rect, job->r, job->g, job->b);
			break;
		}
		case NATIVE_RENDER_JOB_LOAD_IMAGE:
		{
			CtrGpu_ExecLoadImage(&job->rect, job->ptr);
			break;
		}
		case NATIVE_RENDER_JOB_LOAD_IMAGE2:
		{
			CtrGpu_ExecLoadImage2(&job->rect, job->ptr);
			break;
		}
		case NATIVE_RENDER_JOB_MOVE_IMAGE:
		{
			CtrGpu_ExecMoveImage(&job->rect, job->i0, job->i1);
			break;
		}
		case NATIVE_RENDER_JOB_STORE_IMAGE:
		{
			CtrGpu_ExecStoreImage(&job->rect, job->ptr);
			break;
		}
		case NATIVE_RENDER_JOB_RESET_GRAPH:
		{
			CtrGpu_ExecResetGraph(job->i0);
			break;
		}
		case NATIVE_RENDER_JOB_PRESENT_VRAM_DISPLAY:
		{
			Platform_ExecPresentVRAMDisplay();
			break;
		}
		default:
		{
			break;
		}
	}
}

static void NativeRenderQueue_WorkerMain(void)
{
	for (;;)
	{
#ifdef CTR_INTERNAL
		const u64 idleStart = SDL_GetPerformanceCounter();
#endif
		N3DS_RenderWorkWait();
		N3DS_BeatMark(N3DS_BEAT_THREAD_RENDERWORKER, N3DS_BEAT_WHERE_OK);
#ifdef CTR_INTERNAL
		s_wpIdleTicks += SDL_GetPerformanceCounter() - idleStart;
		s_wpIdleWaits++;
#endif

		for (;;)
		{
			struct NativeRenderJob job;
			int stop;

			N3DS_RenderQueueLock();
			if (s_completedCount == s_postedCount)
			{
				N3DS_RenderQueueUnlock();
				break;
			}
			job = s_jobs[s_completedCount % NATIVE_RENDER_QUEUE_RING];
			N3DS_RenderQueueUnlock();

			stop = (job.kind == NATIVE_RENDER_JOB_STOP);
			if (!stop)
			{
#ifdef CTR_INTERNAL
				const int wbucket = NativeRenderQueue_WorkerBucket(job.kind);
				const u64 execStart = SDL_GetPerformanceCounter();
#endif
				/* The worker's side of the fence -- see NATIVE_PERF_BUCKET_WORKER_EXEC's
				 * comment. Everything outside this scope in the loop is queue
				 * handshake: the lock pairs above and below, the done-signal,
				 * and the wake wait. */
				NativePerf_BeginScope(NATIVE_PERF_BUCKET_WORKER_EXEC);
				N3DS_BeatMark(N3DS_BEAT_THREAD_RENDERWORKER, N3DS_BEAT_WHERE_EXEC_JOB);
				NativeRenderQueue_ExecuteJob(&job);
				N3DS_BeatMark(N3DS_BEAT_THREAD_RENDERWORKER, N3DS_BEAT_WHERE_OK);
				NativePerf_EndScope(NATIVE_PERF_BUCKET_WORKER_EXEC);
#ifdef CTR_INTERNAL
				s_wpTicks[wbucket] += SDL_GetPerformanceCounter() - execStart;
				s_wpCalls[wbucket]++;
#endif
			}

			N3DS_RenderQueueLock();
			s_completedCount++;
			N3DS_RenderQueueUnlock();
			N3DS_RenderDoneSignal();

			if (stop)
			{
				return;
			}
		}
	}
}

unsigned int NativeRenderQueue_Post(const struct NativeRenderJob *job)
{
	unsigned int ticket;
#ifdef CTR_INTERNAL
	int blocked = 0;
	u64 blockStart = 0;

	s_qsPostCalls++;
#endif

	for (;;)
	{
		N3DS_RenderQueueLock();
		{
			const unsigned int depth = s_postedCount - s_completedCount;
#ifdef CTR_INTERNAL
			if (depth > s_qsPeakDepth)
			{
				s_qsPeakDepth = depth;
			}
#endif
			if (depth < NATIVE_RENDER_QUEUE_RING)
			{
				break;
			}
		}
		N3DS_RenderQueueUnlock();
#ifdef CTR_INTERNAL
		if (!blocked)
		{
			blocked = 1;
			s_qsPostBlockedCalls++;
			blockStart = SDL_GetPerformanceCounter();
		}
#endif
		N3DS_BeatMark(N3DS_BEAT_THREAD_MAIN, N3DS_BEAT_WHERE_QUEUE_POST_WAIT);
		N3DS_RenderDoneWait();
	}
	N3DS_BeatMark(N3DS_BEAT_THREAD_MAIN, N3DS_BEAT_WHERE_OK);

#ifdef CTR_INTERNAL
	if (blocked)
	{
		s_qsPostBlockTicks += SDL_GetPerformanceCounter() - blockStart;
	}
#endif

	s_jobs[s_postedCount % NATIVE_RENDER_QUEUE_RING] = *job;
	s_postedCount++;
	ticket = s_postedCount;
	N3DS_RenderQueueUnlock();

	N3DS_RenderWorkSignal();
	return ticket;
}

void NativeRenderQueue_Wait(unsigned int ticket)
{
#ifdef CTR_INTERNAL
	const u64 waitStart = SDL_GetPerformanceCounter();

	s_qsWaitCalls++;
#endif

	for (;;)
	{
		unsigned int completed;

		N3DS_RenderQueueLock();
		completed = s_completedCount;
		N3DS_RenderQueueUnlock();

		/* Signed distance so ticket/counter wrap is harmless. */
		if ((int)(completed - ticket) >= 0)
		{
#ifdef CTR_INTERNAL
			s_qsWaitBlockTicks += SDL_GetPerformanceCounter() - waitStart;
#endif
			N3DS_BeatMark(N3DS_BEAT_THREAD_MAIN, N3DS_BEAT_WHERE_OK);
			return;
		}
		N3DS_BeatMark(N3DS_BEAT_THREAD_MAIN, N3DS_BEAT_WHERE_QUEUE_TICKET_WAIT);
		N3DS_RenderDoneWait();
	}
}

void NativeRenderQueue_Fence(void)
{
	unsigned int ticket;

	if (!s_workerUp || N3DS_RenderWorkerIsSelf())
	{
		return;
	}

	N3DS_RenderQueueLock();
	ticket = s_postedCount;
	N3DS_RenderQueueUnlock();

	NativeRenderQueue_Wait(ticket);
}

int NativeRenderQueue_Route(void)
{
	if (!s_workerUp || N3DS_RenderWorkerIsSelf())
	{
		return 0;
	}

	if (!g_dbg_renderThread)
	{
		if (s_routeWasActive)
		{
			NativeRenderQueue_Fence();
			s_routeWasActive = 0;
		}
		return 0;
	}

	s_routeWasActive = 1;
	return 1;
}

void NativeRenderQueue_Init(void)
{
	/* GTE-capture builds do not need a sync fallback here: the capture table
	 * is double-buffered (native_gte_capture.c) -- ResetDB's Clear() flips
	 * the write buffer while the in-flight walk keeps reading the buffer
	 * DrawOTag published for it. */
	s_workerUp = N3DS_RenderWorkerStart(NativeRenderQueue_WorkerMain);
}

void NativeRenderQueue_Shutdown(void)
{
	struct NativeRenderJob job;

	if (!s_workerUp)
	{
		return;
	}

	NativeRenderQueue_JobInit(&job, NATIVE_RENDER_JOB_STOP);
	NativeRenderQueue_RunSync(&job);
	N3DS_RenderWorkerStopAndJoin();
	s_workerUp = 0;
}

#else /* PC, or 3DS without CTR_NATIVE_RENDER_THREAD: facade calls Exec bodies directly.
       * Route/Fence/Init/Shutdown are header constants; only the two symbols a
       * -O0 build's dead branches may still reference need definitions. */

unsigned int NativeRenderQueue_Post(const struct NativeRenderJob *job)
{
	(void)job;
	return 0;
}

void NativeRenderQueue_Wait(unsigned int ticket)
{
	(void)ticket;
}

#endif
