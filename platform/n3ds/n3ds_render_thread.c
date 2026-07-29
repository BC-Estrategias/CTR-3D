/*
 * Render worker thread OS primitives -- see n3ds_render_thread.h for the
 * boundary contract and docs/RENDER_THREAD_DESIGN.md for the design. This TU
 * is compiled with -fshort-enums (libctru's ABI), like n3ds_audio_thread.c;
 * it must never include a decomp header.
 */

#include <3ds.h>

#include "n3ds_render_thread.h"
#include "n3ds_backend.h"

static Thread s_renderThread;
static LightLock s_queueLock;
static LightEvent s_workEvent;
static LightEvent s_doneEvent;
static int s_workerUp;

static void (*s_workerMainFn)(void);

static void N3DS_RenderThreadEntry(void *arg)
{
	(void)arg;

	s_workerMainFn();
	threadExit(0);
}

int N3DS_RenderWorkerStart(void (*workerMain)(void))
{
	bool isNew3DS;
	s32 mainPriority;

	if (s_workerUp)
	{
		/* Idempotent for the same reason the audio worker is: re-initialising
		 * a lock the worker may hold, or re-creating the thread, corrupts
		 * state and orphans the live thread. */
		return 1;
	}

	/* Primitives are real from here down regardless of whether a thread ends
	 * up existing -- the unity-TU queue calls Lock/Signal only when the
	 * worker is up, but an initialized lock is cheap insurance against that
	 * invariant ever slipping. */
	LightLock_Init(&s_queueLock);
	LightEvent_Init(&s_workEvent, RESET_STICKY);
	LightEvent_Init(&s_doneEvent, RESET_STICKY);

	isNew3DS = false;
	APT_CheckNew3DS(&isNew3DS);
	if (!isNew3DS)
	{
		/* Target is New3DS-XL only, but fail safe rather than assume. Core 2
		 * does not exist on old hardware, and core_id=-1 means "all CPUs"
		 * (which resolves to core 0), never "any free core" -- same trap the
		 * audio design doc documents for core 1. */
		N3DS_DebugLog("[CTR-3DS] render worker: NO (APT says not New3DS) -- sync fallback");
		return 0;
	}

	/* Same priority as the main thread: the worker has core 2 to itself, so
	 * priority only matters if it ever migrates, which threadCreate's fixed
	 * core assignment prevents. Unlike core 1 there is no AppCpuTimeLimit
	 * gate for core 2 -- either threadCreate succeeds or it refuses, and the
	 * log line below is the whole hardware answer to "is core 2 reachable
	 * under HBL/Luma". */
	mainPriority = 0x30;
	svcGetThreadPriority(&mainPriority, CUR_THREAD_HANDLE);

	s_workerMainFn = workerMain;
	s_renderThread = threadCreate(N3DS_RenderThreadEntry, NULL, 128 * 1024, mainPriority, 2, false);

	s_workerUp = (s_renderThread != NULL);
	N3DS_DebugLog(s_workerUp ? "[CTR-3DS] render worker: UP (core 2)"
	                         : "[CTR-3DS] render worker: NO (threadCreate refused core 2) -- sync fallback");
	return s_workerUp;
}

void N3DS_RenderWorkerStopAndJoin(void)
{
	if (!s_workerUp)
	{
		return;
	}

	threadJoin(s_renderThread, U64_MAX);
	threadFree(s_renderThread);
	s_renderThread = NULL;
	s_workerUp = 0;
}

int N3DS_RenderWorkerIsSelf(void)
{
	/* threadGetCurrent returns NULL for the main thread and a distinct handle
	 * for the audio worker, so equality against our own handle is exact. */
	return s_workerUp && (threadGetCurrent() == s_renderThread);
}

void N3DS_RenderQueueLock(void)
{
	LightLock_Lock(&s_queueLock);
}

void N3DS_RenderQueueUnlock(void)
{
	LightLock_Unlock(&s_queueLock);
}

void N3DS_RenderWorkSignal(void)
{
	LightEvent_Signal(&s_workEvent);
}

void N3DS_RenderWorkWait(void)
{
	LightEvent_Wait(&s_workEvent);
	LightEvent_Clear(&s_workEvent);
}

void N3DS_RenderDoneSignal(void)
{
	LightEvent_Signal(&s_doneEvent);
}

void N3DS_RenderDoneWait(void)
{
	LightEvent_Wait(&s_doneEvent);
	LightEvent_Clear(&s_doneEvent);
}
