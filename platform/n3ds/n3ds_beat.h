#ifndef N3DS_BEAT_H
#define N3DS_BEAT_H

/*
 * Cross-thread heartbeat counters (Rosalina-freeze hardening,
 * docs/PORT_FEASIBILITY_3DS.md). Each of the three threads that can wedge on
 * an untimed GX/gsp wait -- game/main, audio, render worker -- stamps its own
 * slot every time it passes a known point; a watchdog on the audio thread
 * (whose own wait is time-bounded, so it can never wedge itself) and a
 * lighter periodic check on main compare successive snapshots of the OTHER
 * threads' counters to notice one that stopped advancing.
 *
 * Plain scalars only -- this header is included from both the unity TU
 * (native_render_queue.c) and the libctru-side TUs (n3ds_backend.c,
 * n3ds_audio_thread.c), same rule as n3ds_render_thread.h.
 *
 * Lock-free by design: a torn read of a counter/where pair costs one stale
 * watchdog sample, never a crash, and taking a lock on every job dispatch or
 * frame-begin to protect a diagnostic would add exactly the kind of
 * contention this hardening exists to avoid.
 */

#define N3DS_BEAT_THREAD_MAIN         0
#define N3DS_BEAT_THREAD_AUDIO        1
#define N3DS_BEAT_THREAD_RENDERWORKER 2
#define N3DS_BEAT_THREAD_COUNT        3

/*
 * Where a thread last marked itself. OK covers both "idle" and "running
 * ordinary work" -- the watchdog only cares whether the counter is moving,
 * not which ordinary work it's doing; the other tags name the specific span
 * a FROZEN counter was last seen inside.
 *
 * Mark a span with its tag when entering and with OK when leaving -- never on
 * every retry inside a bounded wait loop, since a counter that keeps moving
 * while genuinely wedged would hide the wedge from the watchdog.
 */
#define N3DS_BEAT_WHERE_OK                0
#define N3DS_BEAT_WHERE_FRAMEBEGIN        1
#define N3DS_BEAT_WHERE_FRAMEEND          2
#define N3DS_BEAT_WHERE_QUEUE_POST_WAIT   3
#define N3DS_BEAT_WHERE_QUEUE_TICKET_WAIT 4
#define N3DS_BEAT_WHERE_EXEC_JOB          5
#define N3DS_BEAT_WHERE_AUDIO_RENDER      6
#define N3DS_BEAT_WHERE_GSP_IPC           7
#define N3DS_BEAT_WHERE_LOGWAIT           8

/* Marks progress for one thread: increments its counter and records where.
 * threadId outside [0, N3DS_BEAT_THREAD_COUNT) is a no-op. */
void N3DS_BeatMark(int threadId, int whereTag);

/* Snapshots one thread's live counter/where with no lock. Returns 0 (outputs
 * left at 0) for an out-of-range threadId; either output pointer may be
 * NULL. */
int N3DS_BeatRead(int threadId, unsigned int *outCounter, int *outWhere);

/* Short uppercase name for a N3DS_BEAT_WHERE_* value, for log lines. Never
 * returns NULL. */
const char *N3DS_BeatWhereName(int whereTag);

#endif
