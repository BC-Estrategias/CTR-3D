/*
 * native_model_frame.c -- per-frame instance sampler (s30 rung 0).
 *
 * WHY THIS EXISTS SEPARATELY FROM THE CENSUS
 *
 * platform/native_model_census.c weighs the whole model population once per
 * level. That is the wrong number for a frame budget: most of a level's models
 * are never on screen together, and the ~9ms instance cost measured on hardware
 * is a per-frame cost. This counts what a frame actually commits to drawing.
 *
 * The distinction is not pedantic. The s29 sweep read one cycle of a rotating
 * config as proof that karts were free, and was wrong because the thing being
 * counted was not the thing being paid for. A population total would make the
 * same mistake in the other direction.
 *
 * WHAT THE NUMBERS MEAN
 *
 * `tris` is the topology maximum of every header this frame selected -- see
 * native_model_mesh.c's header comment for why a static scan cannot subtract
 * the runtime's backface and off-screen rejections. It is an upper bound on
 * what a PICA-native model path would submit, and a proportional-not-absolute
 * signal for what the packet path is currently spending.
 *
 * `rung+1` answers the owner's question directly: if every instance drew one
 * LOD rung coarser than the game chose -- racers in their multiplayer models --
 * this is the triangle count that would result. Nothing here CHANGES the rung;
 * it prices the trade before anyone takes it.
 *
 * Counts only -- no allocation, no game state touched.
 */

#include "platform/native_model_frame.h"
#include "platform/native_model_mesh.h"

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_MODEL_RENDERER) && CTR_NATIVE_MODEL_RENDERER

/* Matches native_direct_emit.c's [DEMIT] cadence -- one line per 64 content
 * frames is frequent enough to see a corner change the answer and rare enough
 * not to dominate the 3DS SD log. */
#define MODELFRAME_WINDOW_FRAMES 64

#define MODELFRAME_MAX_RUNGS 4

struct NativeModelFrameCounters
{
	u32 instances;
	u32 verts;
	u32 tris;
	/* Sum of per-header distinct (tpage, clut) pairs -- the draw-call floor if
	 * every instance were drawn separately with no cross-instance batching.
	 * Deliberately NOT the texture-layout-entry count: those are per-face UV
	 * records and a nine-instance frame has hundreds of them, which reads as a
	 * catastrophic draw count and is not one. */
	u32 texPages;

	u32 staticInstances;
	u32 staticTris;
	u32 animInstances;
	u32 animTris;

	/* What the same frame would have cost one rung coarser. Instances already
	 * on their model's last rung contribute their current cost, because that
	 * is what forcing a coarser rung would actually do -- there is nothing
	 * below it to fall to. */
	u32 nextRungTris;

	u32 rung[MODELFRAME_MAX_RUNGS];
	u32 rungBeyond;

	/* Instances whose header had no cached scan. Non-zero means the topology
	 * table filled or the level changed mid-frame, and every other number in
	 * the line is short by that much. */
	u32 unscanned;
};

static struct NativeModelFrameCounters s_mfFrame;
static struct NativeModelFrameCounters s_mfWindow;
static u32 s_mfWindowFrames;

static void NativeModelFrame_Fold(struct NativeModelFrameCounters *dst, const struct NativeModelFrameCounters *src)
{
	dst->instances += src->instances;
	dst->verts += src->verts;
	dst->tris += src->tris;
	dst->texPages += src->texPages;
	dst->staticInstances += src->staticInstances;
	dst->staticTris += src->staticTris;
	dst->animInstances += src->animInstances;
	dst->animTris += src->animTris;
	dst->nextRungTris += src->nextRungTris;
	dst->rungBeyond += src->rungBeyond;
	dst->unscanned += src->unscanned;

	for (unsigned r = 0; r < MODELFRAME_MAX_RUNGS; r++)
	{
		dst->rung[r] += src->rung[r];
	}
}

/*
 * Cost of the same instance one rung coarser. The Model owns the rung ladder
 * (headers[] in order, RenderBucket_SelectModelHeader walks it), so this is a
 * second scan lookup against headers[lodIndex+1] -- clamped to the last rung,
 * which is what "force a coarser LOD" would really do for a model that has no
 * coarser version.
 */
static u32 NativeModelFrame_NextRungTris(const struct Instance *inst, int lodIndex, u32 currentTris)
{
	const struct Model *model = inst->model;
	s16 next;
	const struct NativeModelHeaderScan *scan;

	if ((model == NULL) || (model->headers == NULL) || (model->numHeaders <= 0))
	{
		return currentTris;
	}

	next = (s16)(lodIndex + 1);

	if (next >= model->numHeaders)
	{
		return currentTris;
	}

	scan = NativeModelMesh_Scan(&model->headers[next]);
	return (scan != NULL) ? scan->tris : currentTris;
}

void NativeModelFrame_Sample(const struct Instance *inst, const struct ModelHeader *mh, int lodIndex)
{
	const struct NativeModelHeaderScan *scan;

	if ((inst == NULL) || (mh == NULL))
	{
		return;
	}

	scan = NativeModelMesh_Scan(mh);

	if (scan == NULL)
	{
		s_mfFrame.instances++;
		s_mfFrame.unscanned++;
		return;
	}

	s_mfFrame.instances++;
	s_mfFrame.verts += scan->verts;
	s_mfFrame.tris += scan->tris;
	s_mfFrame.texPages += scan->texPages;

	if ((scan->flags & NATIVE_MODEL_SCAN_STATIC) != 0)
	{
		s_mfFrame.staticInstances++;
		s_mfFrame.staticTris += scan->tris;
	}
	else
	{
		s_mfFrame.animInstances++;
		s_mfFrame.animTris += scan->tris;
	}

	if ((lodIndex >= 0) && (lodIndex < MODELFRAME_MAX_RUNGS))
	{
		s_mfFrame.rung[lodIndex]++;
	}
	else
	{
		s_mfFrame.rungBeyond++;
	}

	s_mfFrame.nextRungTris += NativeModelFrame_NextRungTris(inst, lodIndex, scan->tris);
}

void NativeModelFrame_EndFrame(void)
{
	/*
	 * Menus, load screens and the pause overlay queue no instances at all.
	 * Counting them would drag every average toward zero and make a heavy
	 * race look cheap -- the same shape of error as averaging a sweep across
	 * different stretches of track. Empty frames are dropped from the window
	 * rather than folded in.
	 */
	if (s_mfFrame.instances == 0)
	{
		return;
	}

	NativeModelFrame_Fold(&s_mfWindow, &s_mfFrame);
	memset(&s_mfFrame, 0, sizeof(s_mfFrame));
	s_mfWindowFrames++;

	if (s_mfWindowFrames < MODELFRAME_WINDOW_FRAMES)
	{
		return;
	}

	{
		u32 f = s_mfWindowFrames;

		Platform_LogInstrument("[MODELFRAME] lvl=%d f=%lu inst/f=%lu tris/f=%lu verts/f=%lu pages/f=%lu unscanned=%lu\n", NativePerf_CurrentLevelID(),
		             (unsigned long)f, (unsigned long)(s_mfWindow.instances / f), (unsigned long)(s_mfWindow.tris / f), (unsigned long)(s_mfWindow.verts / f),
		             (unsigned long)(s_mfWindow.texPages / f), (unsigned long)s_mfWindow.unscanned);

		Platform_LogInstrument("[MODELFRAME] lvl=%d static inst/f=%lu tris/f=%lu | anim inst/f=%lu tris/f=%lu | rung+1 tris/f=%lu (%lu%% of now)\n",
		             NativePerf_CurrentLevelID(), (unsigned long)(s_mfWindow.staticInstances / f), (unsigned long)(s_mfWindow.staticTris / f),
		             (unsigned long)(s_mfWindow.animInstances / f), (unsigned long)(s_mfWindow.animTris / f), (unsigned long)(s_mfWindow.nextRungTris / f),
		             (s_mfWindow.tris > 0) ? (unsigned long)((s_mfWindow.nextRungTris * 100u) / s_mfWindow.tris) : 0ul);

		Platform_LogInstrument("[MODELFRAME] lvl=%d rungs/f: 0=%lu 1=%lu 2=%lu 3=%lu beyond=%lu\n", NativePerf_CurrentLevelID(), (unsigned long)(s_mfWindow.rung[0] / f),
		             (unsigned long)(s_mfWindow.rung[1] / f), (unsigned long)(s_mfWindow.rung[2] / f), (unsigned long)(s_mfWindow.rung[3] / f),
		             (unsigned long)(s_mfWindow.rungBeyond / f));
	}

	memset(&s_mfWindow, 0, sizeof(s_mfWindow));
	s_mfWindowFrames = 0;
}

#else

void NativeModelFrame_Sample(const struct Instance *inst, const struct ModelHeader *mh, int lodIndex)
{
	(void)inst;
	(void)mh;
	(void)lodIndex;
}

void NativeModelFrame_EndFrame(void)
{
}

#endif
