/*
 * Render-job queue -- storage, tickets, worker drain loop, dispatcher. See
 * include/platform/native_render_queue.h for the contract and
 * docs/RENDER_THREAD_DESIGN.md for the design. OS primitives (thread, lock,
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

/* Present-pipelining A/B (dumpreq 'framepipe', chord SELECT+TOUCH). See the
 * header and DrawSync (native_libgpu.c). Defined unconditionally so the dead
 * routed branch on non-render-thread builds still links. Default OFF. */
int g_dbg_framePipeline = 0;

#if defined(__3DS__) && CTR_NATIVE_RENDER_THREAD

#include "n3ds/n3ds_render_thread.h"
#include "n3ds/n3ds_backend.h"

#define NATIVE_RENDER_QUEUE_RING 64

static struct NativeRenderJob s_jobs[NATIVE_RENDER_QUEUE_RING];
static unsigned int s_postedCount;        /* queue-lock guarded */
static unsigned int s_completedCount;     /* queue-lock guarded */
static unsigned int s_lastDrawOtagTicket; /* queue-lock guarded: ticket of the most recent DRAW_OTAG post, for FenceWalk */
static int s_workerUp;
static int s_routeWasActive;

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
		N3DS_RenderWorkWait();

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
				/* s36: the worker's side of the fence -- see the bucket's
				 * comment. Everything OUTSIDE this scope in the loop is queue
				 * handshake: the lock pairs above and below, the done-signal,
				 * and the wake wait. */
				NativePerf_BeginScope(NATIVE_PERF_BUCKET_WORKER_EXEC);
				NativeRenderQueue_ExecuteJob(&job);
				NativePerf_EndScope(NATIVE_PERF_BUCKET_WORKER_EXEC);
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

	for (;;)
	{
		N3DS_RenderQueueLock();
		if ((s_postedCount - s_completedCount) < NATIVE_RENDER_QUEUE_RING)
		{
			break;
		}
		N3DS_RenderQueueUnlock();
		N3DS_RenderDoneWait();
	}

	s_jobs[s_postedCount % NATIVE_RENDER_QUEUE_RING] = *job;
	s_postedCount++;
	ticket = s_postedCount;
	if (job->kind == NATIVE_RENDER_JOB_DRAW_OTAG)
	{
		/* Latch the walk's ticket so FenceWalk can wait for THIS frame's walk
		 * without draining the END_FRAME present that follows it. */
		s_lastDrawOtagTicket = ticket;
	}
	N3DS_RenderQueueUnlock();

	N3DS_RenderWorkSignal();
	return ticket;
}

void NativeRenderQueue_Wait(unsigned int ticket)
{
	for (;;)
	{
		unsigned int completed;

		N3DS_RenderQueueLock();
		completed = s_completedCount;
		N3DS_RenderQueueUnlock();

		/* Signed distance so ticket/counter wrap is harmless. */
		if ((int)(completed - ticket) >= 0)
		{
			return;
		}
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

void NativeRenderQueue_FenceWalk(void)
{
	unsigned int ticket;

	if (!s_workerUp || N3DS_RenderWorkerIsSelf())
	{
		return;
	}

	/* Wait for the last posted walk, not the queue tail. A trailing END_FRAME
	 * present (posted after DRAW_OTAG by Platform_EndFrame) is deliberately left
	 * in flight; the worker still runs it in FIFO order before the next walk, so
	 * ordering and visuals are unchanged -- only the game thread stops blocking
	 * on it. If no walk has posted yet, ticket 0 completes immediately. */
	N3DS_RenderQueueLock();
	ticket = s_lastDrawOtagTicket;
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
	/* GTE-capture builds no longer force the sync fallback: the capture
	 * table is double-buffered (s26, native_gte_capture.c) -- ResetDB's
	 * Clear() flips the write buffer while the in-flight walk keeps reading
	 * the one DrawOTag published for it. */
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

void NativeRenderQueue_FenceWalk(void)
{
}

#endif
