/*
 * Derived from REDRIVER2/PsyCross MIT source:
 * externals/PsyCross/src/gpu/PsyX_GPU.h
 * See THIRD_PARTY_NOTICES.md for copyright and license details.
 */

#ifndef NATIVE_GPU_H
#define NATIVE_GPU_H

#include <macros.h>
#include <psx/libgte.h>
#include <psx/libgpu.h>

extern DISPENV activeDispEnv;
extern DRAWENV activeDrawEnv;
extern int g_GPUDisabledState;

/*
 * Name of the content-ablation configuration currently live, as a pointer to a
 * static string ("none" when nothing is ablating). Written by the 3DS [SWEEP]
 * harness (N3DS_SweepApply); read by the windowed instruments -- [PERF] and
 * [OTPROF] -- which CUT THEIR WINDOW when it changes.
 *
 * That cut is the whole point. The sweep holds a config for two 32-frame stats
 * windows while [PERF] and [OTPROF] report every 64 frames on their own phase,
 * so before this every reported window could straddle two configs and average
 * "full" with "no-hud" into a number belonging to neither. The [SWEEP] line's
 * own frame/work microseconds were the only per-config figures in the log;
 * bucket and content attribution had no way to follow the ablation. Identity is
 * the POINTER, not the text -- every writer passes a literal from one static
 * table, so a compare is one instruction and needs no strcmp.
 */
extern const char *g_dbg_ablateConfig;

int NativeGpu_HasPendingSplits(void);
void NativeGpu_ResetEmitOrdinal(void);
void ClearSplits(void);
void DrawAllSplits(void);
void ParsePrimitivesLinkedList(u32 *p, int singlePrimitive);
int NativeGpu_GetStateSize(void);
int NativeGpu_CaptureState(void *dst, int dstSize);
int NativeGpu_RestoreState(const void *src, int srcSize);

// Track-renderer accessor (rung 3a, docs/NATIVE_TRACK_RENDERER_DESIGN.md 12):
// per-draw CPU-side lookup of the bucket->ordinal table recorded during the
// OT walk. Returns 0 -- nothing written to the outputs -- unless ALL of:
// the recorder was latched active for the walk that most recently ran
// (g_dbg_otzWiden or the track-renderer toggle, read once at the top of that
// walk, not live -- see native_gpu.c's otRecorderActive), slotIdx is inside
// that walk's own OT size, and the slot was actually crossed (not still
// holding NATIVE_GPU_OTZ_SENTINEL, native_gpu.c's "never recorded" marker,
// which replaced a stale memset-to-0 that made "crossed at ordinal 0" and
// "never crossed" indistinguishable). Returns 1 otherwise, with *outOrdinal/
// *outWalkOtCount valid.
int NativeGpu_GetOtBucketOrdinal(u32 slotIdx, u16 *outOrdinal, u32 *outWalkOtCount);

// Track-renderer accessor (rung 3a depth-compose fix, docs/
// NATIVE_TRACK_RENDERER_DESIGN.md 4.3): the lowest OT bucket index that held
// real primitive content during the last walk the recorder ran for. Buckets
// nearer than every real primitive this frame emitted (e.g. bucket 0, which
// retail never puts content in when the camera sits on the track) still get
// an entry in the bucket->ordinal table above -- the walk crosses their
// marker node too -- but that entry is just whatever the emit counter had
// reached by the END of the walk, since markers are crossed in descending
// index order. This accessor is the fix: it reports where real content
// actually stopped, so a caller can refuse to trust otBucketOrdinal for any
// bucket nearer than this floor. Same failure contract as
// NativeGpu_GetOtBucketOrdinal above -- returns 0 (nothing written) unless
// the recorder was latched active for the walk that most recently ran AND
// at least one primitive actually advanced the emit ordinal during it.
// Returns 1 otherwise, with *outMinContentBucket valid.
int NativeGpu_GetMinContentBucket(u32 *outMinContentBucket);

// DIAGNOSTIC ONLY (uncommitted rung 3a occlusion investigation, no behavior
// change): read-only view of this frame's highest STAMPED emit ordinal, for
// comparing the track renderer's own bucket->ordinal lookups against where
// the packet path's stamping actually landed this same frame -- if the
// track's ordinals sit at or near this value, something is feeding it
// end-of-frame ordinals rather than the mid-walk ones its own leaves'
// otIndex should map to. s_gpu.emitOrdinal (NativeGpuState) is the NEXT
// ordinal to be handed out (see NativeGpu_StampEmitOrdinal), so the last one
// actually used is one less; a frame that stamped nothing (emitOrdinal still
// 0) reports 0 rather than underflowing. Frame-scoped exactly like
// emitOrdinal itself (reset at depth-clear, not per-DrawOTag -- see
// NativeGpu_ResetEmitOrdinal's comment), so this is meaningful only when
// read after the frame's OT walk(s) have run and before the next reset --
// exactly when NativeTrackDraw_IssueForWalk (native_track_frame.c) calls it.
u32 NativeGpu_GetFrameEmitOrdinal(void);

#endif
