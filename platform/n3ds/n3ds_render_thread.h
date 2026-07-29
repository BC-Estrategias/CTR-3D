#ifndef N3DS_RENDER_THREAD_H
#define N3DS_RENDER_THREAD_H

/*
 * Render worker thread primitives (New3DS core 2). Same TU arrangement as the
 * audio worker: this header is a plain-scalar boundary -- the implementation
 * is compiled with -fshort-enums to match libctru's ABI, so nothing enum- or
 * struct-typed from either side may cross it. The job queue itself (storage,
 * tickets, executors) lives in the unity TU (platform/native_render_queue.c);
 * this side owns only the OS objects: the thread, the queue lock, and the
 * work/done events. Design: docs/RENDER_THREAD_DESIGN.md.
 */

/* Starts the worker on core 2 (New3DS-only core; core 1 belongs to the audio
 * worker, and the audio design doc's law applies here too: never fall back to
 * a same-core thread). workerMain is the queue's drain loop; it returns only
 * after consuming a STOP job. Returns 1 with the thread up, 0 on any refusal
 * (old 3DS, threadCreate failure) -- caller stays synchronous. Idempotent. */
int N3DS_RenderWorkerStart(void (*workerMain)(void));

/* Joins and frees the worker. Caller must have posted STOP first (the join
 * would otherwise never return). No-op if the worker never came up. */
void N3DS_RenderWorkerStopAndJoin(void);

/* 1 iff the calling thread is the render worker itself. Lets shared entry
 * points fence when called from the game thread but pass straight through
 * when the worker re-enters them while executing a job. */
int N3DS_RenderWorkerIsSelf(void);

void N3DS_RenderQueueLock(void);
void N3DS_RenderQueueUnlock(void);

/* Work event: producer signals after appending a job; the worker waits, then
 * clears, then drains everything it can see. Sticky, so a signal that lands
 * between the worker's drain and its next wait is never lost. */
void N3DS_RenderWorkSignal(void);
void N3DS_RenderWorkWait(void);

/* Done event: worker signals after completing each job; sync waiters (ticket
 * waits, fences, ring-full backoff) wait then re-check their condition.
 * Sticky for the same missed-wakeup reason; only the game thread ever waits
 * on it, so the wait-side clear cannot starve another waiter. */
void N3DS_RenderDoneSignal(void);
void N3DS_RenderDoneWait(void);

#endif
