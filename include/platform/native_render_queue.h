#ifndef NATIVE_RENDER_QUEUE_H
#define NATIVE_RENDER_QUEUE_H

/*
 * Render-job queue: on 3DS (CTR_NATIVE_RENDER_THREAD builds) the native render
 * pipeline -- C3D frame bracket, OT walk, split submission, frame end/swap --
 * runs on a worker thread on core 2, fed by this FIFO. The game already speaks
 * full PS1 async-GPU protocol (bool_DrawOTag_InProgress / DrawSyncCallback /
 * double-buffered OT+primMem), so posting DrawOTag instead of walking inline
 * buys a one-frame pipeline with zero game/ changes. On PC, and on 3DS when
 * the worker is unavailable (old 3DS, core-2 refusal, GTE-capture build) or
 * runtime-disabled (dumpreq 'renderthread'), every facade entry point calls
 * its Exec body directly -- byte-identical to the pre-queue code by
 * construction. Design + hazard list: docs/RENDER_THREAD_DESIGN.md.
 */

#include <string.h>

#include <psx/libgpu.h>

enum NativeRenderJobKind
{
	NATIVE_RENDER_JOB_STOP = 0,
	NATIVE_RENDER_JOB_PUT_DRAWENV,
	NATIVE_RENDER_JOB_PUT_DISPENV,
	NATIVE_RENDER_JOB_SET_DISP_MASK,
	NATIVE_RENDER_JOB_DRAW_OTAG,
	NATIVE_RENDER_JOB_END_FRAME,
	NATIVE_RENDER_JOB_DRAW_SYNC,
	NATIVE_RENDER_JOB_DRAW_PRIM,
	NATIVE_RENDER_JOB_CLEAR_IMAGE,
	NATIVE_RENDER_JOB_LOAD_IMAGE,
	NATIVE_RENDER_JOB_LOAD_IMAGE2,
	NATIVE_RENDER_JOB_MOVE_IMAGE,
	NATIVE_RENDER_JOB_STORE_IMAGE,
	NATIVE_RENDER_JOB_RESET_GRAPH,
	NATIVE_RENDER_JOB_PRESENT_VRAM_DISPLAY,
};

/* Payloads are copied by value at post time: env installs carry their whole
 * env (activeDrawEnv/activeDispEnv become worker-owned while the queue is
 * active -- the game thread must never write them directly), rect ops carry
 * the rect. Pointer payloads (OT root, pixel buffers) are NOT copied; async
 * jobs may only carry pointers whose lifetime the double-buffer protocol
 * already guarantees (the OT), everything else posts synchronously. */
struct NativeRenderJob
{
	int kind;
	void *ptr;
	int i0;
	int i1;
	unsigned char r, g, b;
	RECT16 rect;
	DRAWENV drawEnv;
	DISPENV dispEnv;
};

static inline void NativeRenderQueue_JobInit(struct NativeRenderJob *job, int kind)
{
	memset(job, 0, sizeof(*job));
	job->kind = kind;
}

#if defined(__3DS__) && defined(CTR_NATIVE_RENDER_THREAD)

/* 1 = the queue is taking jobs: post instead of calling the Exec body. Also
 * owns the runtime A/B toggle's OFF transition (fences the queue once so the
 * next direct call cannot race the worker's tail). Returns 0 on the worker
 * thread itself, so a stray facade call from an Exec body degrades to a
 * direct call instead of a self-deadlock. Game-thread callers only. */
int NativeRenderQueue_Route(void);

void NativeRenderQueue_Init(void);
void NativeRenderQueue_Shutdown(void);

/* Drain the queue. Safe from anywhere: no-op when the queue is inactive or
 * when called on the worker itself. This is the guard for the few paths that
 * touch renderer state outside the facade (game-side NativeRenderer_ClearVRAM,
 * VRAM-display pinning, the [PERF] window reporter's bucket reads, savestate
 * capture/apply). */
void NativeRenderQueue_Fence(void);

#else

/* Non-render-thread builds (all PC builds, 3DS with the option off): Route
 * folds to constant 0 so every wrapper compiles to exactly its pre-queue
 * body, and Fence to nothing -- flag off means byte-identical to the
 * untouched baseline, same guarantee the other gates keep. Post/Wait stay
 * real (stub) symbols because -O0 may keep the dead routed branch's calls. */
#define NativeRenderQueue_Route() 0
#define NativeRenderQueue_Fence() ((void)0)

static inline void NativeRenderQueue_Init(void)
{
}

static inline void NativeRenderQueue_Shutdown(void)
{
}

#endif

/* Append a job; returns its ticket. Blocks only when the ring is full. */
unsigned int NativeRenderQueue_Post(const struct NativeRenderJob *job);

/* Wait until the given ticket has completed. */
void NativeRenderQueue_Wait(unsigned int ticket);

/*
 * Present-pipelining fence (s36). Block only until the LAST OT walk (DRAW_OTAG)
 * has completed, letting a trailing END_FRAME present float on the worker. This
 * is the narrower fence the game-frame stall wants: the walk is what frees the
 * double-buffered OT/primMem and what bool_DrawOTag_InProgress tracks, whereas
 * the present only reads the color target citro3d flips under its own
 * backpressure -- so the game thread need never wait on it. Gated at the call
 * site by g_dbg_framePipeline; DrawSync uses it in place of a full queue drain
 * when the toggle is on. Real only on the render-thread build; a no-op stub
 * otherwise. Safe from anywhere (no-op when the queue is inactive or on the
 * worker itself), like NativeRenderQueue_Fence.
 */
void NativeRenderQueue_FenceWalk(void);

/* Runtime A/B for the present-pipelining fence above (dumpreq 'framepipe',
 * hardware chord SELECT+L). 0 = today's behavior (DrawSync drains the whole
 * queue, present included); 1 = walk-only fence. Default 0: owner opts in and
 * judges the two risks -- input lag and tearing -- by feel. */
extern int g_dbg_framePipeline;

static inline void NativeRenderQueue_RunSync(const struct NativeRenderJob *job)
{
	NativeRenderQueue_Wait(NativeRenderQueue_Post(job));
}

/*
 * Exec bodies: the pre-queue synchronous implementations, unchanged. Each is
 * called either directly (queue inactive) or by the worker's dispatcher --
 * never both concurrently. They must not call facade wrappers (DrawSync,
 * ClearImage, Platform_EndFrame, ...), only other Exec bodies and renderer
 * internals, or a worker job would post into its own queue.
 */
void CtrGpu_ExecDrawOTag(void *p);
void CtrGpu_ExecDrawSync(void);
void CtrGpu_ExecDrawPrim(void *p);
int CtrGpu_ExecClearImage(RECT16 *rect, unsigned char r, unsigned char g, unsigned char b);
void CtrGpu_ExecLoadImage(RECT16 *rect, void *p);
void CtrGpu_ExecLoadImage2(RECT16 *rect, void *p);
void CtrGpu_ExecMoveImage(RECT16 *rect, int x, int y);
void CtrGpu_ExecStoreImage(RECT16 *rect, void *p);
void CtrGpu_ExecResetGraph(int mode);
void CtrGpu_ExecPutDrawEnv(const DRAWENV *env);
void CtrGpu_ExecPutDispEnv(const DISPENV *env);
void CtrGpu_ExecSetDispMask(int mask);
void Platform_ExecEndFrame(void);
void Platform_ExecPresentVRAMDisplay(void);

#endif
