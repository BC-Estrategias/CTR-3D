/*
 * Native track renderer -- per-frame camera + visibility snapshot recorder
 * (rung 3a step 1, docs/NATIVE_TRACK_RENDERER_DESIGN.md 4.2/12b).
 *
 * This is the piece the design doc's first draft got wrong (brief correction
 * (a), doc erratum): there is no pre-existing "recorded leaf list the mesh
 * builder already latches" to read from. The mesh builder (native_track_mesh.c)
 * only knows the level's STATIC per-leaf ranges, built once at load; which of
 * those leaves are actually VISIBLE this frame comes from the game's own
 * RenderLists_Init1P2P traversal, which runs every frame and links visible
 * BSP leaves into gGT->LevRenderLists' six slot lists. This file is what
 * turns that per-frame traversal into something the draw side (step 2) can
 * read as a flat, double-buffered snapshot.
 *
 * Ladder step 1 (plumbing only): records the snapshot and counts what it
 * would draw into a periodic [TRKFRAME] log line. Nothing is drawn on any
 * platform yet, and the gate is OFF by default, so golden output is
 * untouched even when the CTR_NATIVE_TRACK_RENDERER build option is on.
 */

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER

#include "platform/native_track_frame.h"
#include "platform/native_track_mesh.h"
#include "platform/native_gpu.h" /* NativeGpu_GetOtBucketOrdinal -- rung 3a step 2's depth compose */

/* Runtime toggle (0 off / 1 on / 2 both) -- see the header comment for what
 * each state means. Starts OFF: an enabled build must be inert until a
 * dumpreq token, hardware combo, PC argv flag, or PC debug key turns it on. */
/* Boot default; see CTR_NATIVE_RENDERER_DEFAULT_ON in CMakeLists.txt. Normally
 * 0 so an enabled build is inert until a dumpreq token, hardware combo, PC argv
 * flag or PC debug key turns it on. */
#ifndef CTR_NATIVE_RENDERER_DEFAULT_ON
#define CTR_NATIVE_RENDERER_DEFAULT_ON 0
#endif

int g_dbg_trackRender = CTR_NATIVE_RENDERER_DEFAULT_ON;

global_variable struct NativeTrackFrameSnapshot s_trackFrameBuf[2];
/* Game-thread cursor: which buffer Record() writes. */
global_variable int s_trackFrameWriteIdx;
/* Walk cursor: which buffer ForWalk() reads. Written only by
 * NativeTrackFrame_PublishForWalk on the game thread at DrawOTag time,
 * before the walk job is posted (or before the synchronous walk runs) --
 * same ownership rule as native_gte_capture.c's s_gteCaptureReadIdx. */
global_variable int s_trackFrameReadIdx;
global_variable u32 s_trackFrameSeq;

/* Cumulative, session-lifetime totals -- review fix: a per-frame drop count
 * only surfacing on 1-in-64 [TRKFRAME] lines means 63/64 frames that dropped
 * leaves did so invisibly (the periodic line could land on a frame that
 * happened to drop 0 even though the frame right before it dropped 40). The
 * running total is always in the line instead, plus a one-shot warning the
 * first time it ever happens so a drop shows up even between periodic
 * lines. */
global_variable u32 s_trackFrameDroppedTotal;
global_variable int s_trackFrameDroppedWarned;

/* s33 leaf keep-alive (see the block in NativeTrackFrame_Record). Frames a
 * leaf survives past its last appearance in the visible lists; 0 disables.
 * dumpreq 'leafkeep N'. Default 15 (~half a second at 30fps) -- enough to
 * cover a drift swing, small enough that the extra draws stay bounded. */
int g_dbg_trackLeafKeep = 15;

#define TRACKFRAME_KEEP_MAX_SLOTS 4096
global_variable u16 s_leafKeepStamp[TRACKFRAME_KEEP_MAX_SLOTS];
global_variable u16 s_leafKeepNow;
global_variable const struct mesh_info *s_leafKeepMesh;

/*
 * Walk one DrawLevelOvr1PRenderListSlot's linked list of visible BSP leaves,
 * appending every one that has a resident mesh range to dst->leafSlot.
 *
 * bspIndex recovery: RenderLists_LinkBsp (game/RenderLevel/RenderLists.c)
 * links `node = &bspList[bsp - bspRoot]` into the slot head, so a node's
 * offset from bspListBase (gGT->visMem1->bspList[0]) IS its bspIndex with no
 * extra table -- exactly the anchor this brief verified against that
 * function.
 *
 * Iteration cap: review fix -- these lists are built fresh every frame from
 * a fixed-size VisMem arena walking the SAME BSP tree the mesh was built
 * from, so no list can legitimately hold more than the level's own BSP node
 * count (nodeCap, the mesh's numBspNodes at build time); a list still going
 * past that is definitely a cycle, not just unlikely content. Bounding the
 * cap to actual level size means a corrupt list aborts almost immediately
 * instead of chasing a fixed 65536-iteration ceiling copied from the OT
 * walk's own guard (native_gpu.c, otWalkIterations) -- that number made
 * sense there (OTs are a few thousand entries at most, so 65536 is "clearly
 * too many" for ANY level) but is not remotely tight for this walk, whose
 * real per-level ceiling is already known and much smaller. A silent
 * infinite loop here would hang the GAME thread (this runs synchronously
 * inside RenderAllLevelGeometry, never on the render worker), which is worse
 * than the OT walk's case, not better -- hence a cap at all, just a tighter
 * one. Also marks the snapshot `degraded` on abort: a walk that had to bail
 * mid-list produced an incomplete leaf set, and a consumer must not treat
 * that the same as a clean, fully-walked frame.
 */
internal void NativeTrackFrame_WalkList(const struct VisMemBspListNode *bspListBase, struct VisMemBspListNode *head, struct NativeTrackFrameSnapshot *dst,
                                        u32 nodeCap)
{
	struct VisMemBspListNode *node = head;
	u32 iterations = 0;

	while (node != NULL)
	{
		if (++iterations > nodeCap)
		{
			Platform_Log("[TRKFRAME] WARNING: bsp list walk exceeded %u iterations (level has %u BSP nodes), aborting (cyclic list?)\n", iterations, nodeCap);
			dst->degraded = 1;
			break;
		}

		{
			ptrdiff_t bspIndex = node - bspListBase;
			int leafIdx = (bspIndex >= 0) ? NativeTrackMesh_LeafIndexForBsp((unsigned)bspIndex) : -1;

			if (leafIdx >= 0)
			{
				if (dst->leafCount < NATIVE_TRACK_FRAME_MAX_LEAVES)
				{
					/* (u16) truncation: leafIdx is a leaf-RANGE index (into
					 * the mesh's leafRanges array), not a leaf COUNT -- real
					 * tracks measure a few hundred at most (s27 [TRKMESH]
					 * census), so this mirrors the pre-existing u16 bspIndex
					 * field in struct NativeTrackLeafRange rather than
					 * introducing a new risk. No assert: a level large enough
					 * to overflow this would already have tripped
					 * NativeTrackMesh_Frame's own u16-index-buffer guard
					 * (native_track_mesh.c) at BUILD time, long before a
					 * frame gets here. */
					/* Rung 3b: one leaf is now one range PER ATLAS UNIT, and
					 * the reverse map points at the first of them. The
					 * binning pass emits a leaf's sub-ranges contiguously and
					 * in ascending bin order (native_track_mesh.c), so walking
					 * forward while bspIndex matches collects exactly this
					 * leaf's ranges and stops at the next leaf's. A leaf that
					 * fits in one atlas unit still yields one range, i.e. this
					 * degenerates to the rung-3a behaviour. */
					unsigned slot = (unsigned)leafIdx;
					unsigned slotBsp, slotFirst, slotCount;

					while ((dst->leafCount < NATIVE_TRACK_FRAME_MAX_LEAVES) && NativeTrackMesh_LeafAt(slot, &slotBsp, &slotFirst, &slotCount)
					       && (slotBsp == (unsigned)bspIndex))
					{
						dst->leafSlot[dst->leafCount++] = (u16)slot;
						slot++;
					}

					if (dst->leafCount >= NATIVE_TRACK_FRAME_MAX_LEAVES)
					{
						dst->droppedLeaves++;
					}
				}
				else
				{
					dst->droppedLeaves++;
				}
			}
		}

		node = node->next;
	}
}

/*
 * ----------------------------------------------------------------- [TRKNEAR]
 *
 * Road-side half of the sinking-kart investigation. s32 measured the MODEL
 * side to be exact: [MDLDEPTH] compared this renderer's per-vertex view.z
 * against the GTE's own SZ for the same vertex and got maxErr=0 over 263,517
 * samples, so the kart's depth is provably retail's own number. That leaves
 * the road, which is where the owner pointed before the code did.
 *
 * The specific suspect is already written down in ctr_track.v.pica and has
 * been since s29, filed as a leftover rather than as the cause: track
 * visibility is LEAF-granular and retail's per-polygon near-clip was deleted
 * for track content, so a leaf straddling the eye is drawn WHOLE, and
 * depthMinWorld then maps every vertex with view.z < 256 onto exactly 256 --
 * the nearest depth the axis can express. Geometry that should not be on
 * screen at all therefore outranks a kart that genuinely is.
 *
 * Interpolation was eliminated as an alternative before writing this: depth
 * is exact at a triangle's vertices and screen-linear between them, and
 * screen-linear interpolation of view.z always over-estimates (the true value
 * is the harmonic blend, which is smaller). So interpolation can only push the
 * road AWAY mid-triangle, never nearer -- it cannot bury anything.
 *
 * What this counts, per drawn leaf, over the exact index list the 3DS draw
 * would issue:
 *   behind   -- triangles with all three vertices at view.z <= 0
 *   straddle -- triangles with some but not all vertices at view.z <= 0
 *   clamped  -- triangles with at least one vertex in (0, 256), i.e. one the
 *               near-band floor moves
 * Counts, not screen area: a count is what promotes or kills the theory, and
 * an area estimate would need the rasteriser. If `behind` and `straddle` are
 * ~zero in a race, this theory is dead and the road is not the cause either.
 *
 * PC-side deliberately (no __3DS__ guard): the snapshot, the mesh and the
 * camera all exist on PC, so this needs no hardware and no emulator, and PC
 * is where a wrong instrument is cheapest to catch. It is off unless
 * --track-near-census is passed, and it samples 1 frame in 8 -- walking every
 * vertex of every visible leaf every frame is real work, and the failure being
 * measured is a steady-state property of the camera, not a transient.
 */
int g_dbg_trackNearCensus;

global_variable u32 s_trkNearFrames;
global_variable u32 s_trkNearSampleCountdown = 1;
global_variable u32 s_trkNearTris;
global_variable u32 s_trkNearBehind;
global_variable u32 s_trkNearStraddle;
global_variable u32 s_trkNearClamped;
global_variable s32 s_trkNearViewZLo;
global_variable s32 s_trkNearViewZHi;
global_variable s32 s_trkDordLo;
global_variable s32 s_trkDordHi;
global_variable u32 s_trkDordSamples;
global_variable u32 s_trkDordNeg;
global_variable u32 s_trkDordPos;
global_variable double s_trkDordNegSum;

/*
 * -------------------------------------------------------------- [TRKDEPTHCHK]
 *
 * The road-side twin of [MDLDEPTH], and the measurement three sessions of
 * assumption skipped.
 *
 * [MDLDEPTH] proved the MODEL path's view.z is bit-identical to the GTE's own
 * SZ for the same vertex (242 windows, maxErr=0). Nothing equivalent was ever
 * run for the ROAD -- its depth was trusted because "our renderer computes it
 * properly", which is a description of intent, not a measurement. Meanwhile
 * every symptom points at the road: dumpreq `depthtest 2` (road tests but does
 * not write depth) makes racers sit cleanly on the surface, and the racers'
 * own numbers are provably right, so the road must be reading nearer than it
 * should.
 *
 * This compares, for the SAME source vertex in the SAME frame:
 *   ours   = (snapshot.rot[2] . pos)/4096 + snapshot.trans[2]   -- what the
 *            track shader will use, from the matrix we snapshotted
 *   retail = the GTE's SZ, which retail just computed for that vertex
 *            (Ovr226_800a0f78_ProjectVertexTripleFullDepth's gte_stsz3)
 *
 * Called from retail's own projection site, so there is no vertex matching to
 * get wrong and no second copy of the geometry -- the two numbers describe one
 * vertex by construction. A nonzero error IS the sinking bug, and its shape
 * says which kind: a constant difference means we snapshot the wrong matrix
 * (a candidate is already written down -- Ovr226_800aa790_TerminalPreamble
 * loads IDENTITY into GTE R/TR and puts matrix_ViewProj in the LIGHT matrix
 * registers instead), a proportional one means a units/scale mismatch.
 *
 * The snapshot read is the WRITE buffer, not ForWalk()'s published one:
 * NativeTrackFrame_Record runs earlier in the same MainFrame_RenderFrame pass
 * that is about to call DrawLevelOvr1P, so the write buffer is this frame's
 * camera. Using the published buffer would compare against the PREVIOUS
 * frame's camera and manufacture a motion-dependent error that isn't real.
 */
int g_dbg_trackDepthCheck;

global_variable u32 s_trkChkSamples;
global_variable u32 s_trkChkOver1;
global_variable double s_trkChkSumAbsErr;
global_variable s32 s_trkChkMaxAbsErr;
global_variable s32 s_trkChkMinErr;
global_variable s32 s_trkChkMaxErr;
global_variable s32 s_trkChkOursLo;
global_variable s32 s_trkChkOursHi;
global_variable s32 s_trkChkRetailLo;
global_variable s32 s_trkChkRetailHi;

/*
 * s32 SHADER-FORMULA ARM. The check above computes the depth the way this FILE
 * would in integers -- and it reports err=0..0, which is why three sessions
 * concluded "the road's depth is not a wrong number".
 *
 * It is not the arithmetic that ships. The GPU recomputes depth from the
 * COMPOSED rows (see the qw composition further down this file) as
 *
 *     w      = qw . (px, py, pz, 1)      where qw = [r20/h, r21/h, r22/h, trz/h]
 *     viewZ  = w * h
 *
 * which is algebraically the same expression and numerically a different one:
 * it divides by h, sums four terms that can be large and nearly cancelling, and
 * multiplies back -- all in PICA200 float24 (1 sign, 7 exponent, 16 mantissa).
 * A bias sweep put the road several hundred view.z units too near with a
 * plateau from ~400 to 1400+, which no rounding error explains but which a
 * checked-the-wrong-formula bug explains completely.
 *
 * So: same vertex, same frame, both formulas, both compared against retail's
 * GTE. If the integer arm stays at 0 and this one does not, the gap between
 * them IS the sink.
 */
global_variable u32 s_trkChkShSamples;
global_variable s32 s_trkChkShMinErr;
global_variable s32 s_trkChkShMaxErr;
global_variable s32 s_trkChkShMaxAbsErr;
global_variable double s_trkChkShSumAbsErr;
global_variable float s_trkChkWLo;
global_variable float s_trkChkWHi;

/* Round a host float to what a PICA200 float24 register can hold: same sign and
 * exponent fields, mantissa truncated from 23 bits to 16. Truncation rather
 * than round-to-nearest because that is what dropping the low bits does, and
 * this is meant to bound the hardware's error, not to flatter it. */
internal float NativeTrackFrame_F24(float v)
{
	union
	{
		float f;
		u32 u;
	} x;

	x.f = v;
	x.u &= 0xFFFFFF80u;
	return x.f;
}

void NativeTrackFrame_NoteRetailVertexDepth(const s16 *pos, u32 retailDepth)
{
	const struct NativeTrackFrameSnapshot *snap = &s_trackFrameBuf[s_trackFrameWriteIdx];
	s64 mac3;
	s32 ours;
	s32 retail = (s32)retailDepth;
	s32 err;

	if ((g_dbg_trackDepthCheck == 0) || (pos == NULL) || (snap->valid == 0))
	{
		return;
	}

	mac3 = ((s64)snap->trans[2] * 4096) + ((s64)snap->rot[2][0] * pos[0]) + ((s64)snap->rot[2][1] * pos[1]) + ((s64)snap->rot[2][2] * pos[2]);
	ours = (s32)(mac3 >> 12);

	/* Same unsigned-16-bit saturation the GTE applies to SZ3 (Lm_D), so the
	 * comparison is against what retail actually stored, not against an
	 * unclamped ideal that would show a fake error at both extremes. */
	if (ours < 0) { ours = 0; }
	if (ours > 65535) { ours = 65535; }

	err = ours - retail;

	if (s_trkChkSamples == 0)
	{
		s_trkChkOursLo = ours;
		s_trkChkOursHi = ours;
		s_trkChkRetailLo = retail;
		s_trkChkRetailHi = retail;
		s_trkChkMinErr = err;
		s_trkChkMaxErr = err;
	}
	else
	{
		if (ours < s_trkChkOursLo) { s_trkChkOursLo = ours; }
		if (ours > s_trkChkOursHi) { s_trkChkOursHi = ours; }
		if (retail < s_trkChkRetailLo) { s_trkChkRetailLo = retail; }
		if (retail > s_trkChkRetailHi) { s_trkChkRetailHi = retail; }
		if (err < s_trkChkMinErr) { s_trkChkMinErr = err; }
		if (err > s_trkChkMaxErr) { s_trkChkMaxErr = err; }
	}

	/* Shader arm: reproduce the composed-row path term for term, rounding to
	 * float24 at every step the GPU would. h comes from the same snapshot the
	 * draw uses, so this is the number the PICA actually works with. */
	if (snap->h != 0)
	{
		const float h = (float)snap->h;
		const float qw0 = NativeTrackFrame_F24(NativeTrackFrame_F24((float)snap->rot[2][0] * (1.0f / 4096.0f)) / h);
		const float qw1 = NativeTrackFrame_F24(NativeTrackFrame_F24((float)snap->rot[2][1] * (1.0f / 4096.0f)) / h);
		const float qw2 = NativeTrackFrame_F24(NativeTrackFrame_F24((float)snap->rot[2][2] * (1.0f / 4096.0f)) / h);
		const float qw3 = NativeTrackFrame_F24((float)snap->trans[2] / h);
		float w = NativeTrackFrame_F24(qw0 * (float)pos[0]);
		float viewZ;
		s32 shOurs;
		s32 shErr;
		s32 shAbs;

		w = NativeTrackFrame_F24(w + NativeTrackFrame_F24(qw1 * (float)pos[1]));
		w = NativeTrackFrame_F24(w + NativeTrackFrame_F24(qw2 * (float)pos[2]));
		w = NativeTrackFrame_F24(w + qw3);
		viewZ = NativeTrackFrame_F24(w * h);

		if (s_trkChkShSamples == 0)
		{
			s_trkChkWLo = w;
			s_trkChkWHi = w;
		}
		else
		{
			if (w < s_trkChkWLo) { s_trkChkWLo = w; }
			if (w > s_trkChkWHi) { s_trkChkWHi = w; }
		}

		shOurs = (s32)viewZ;
		if (shOurs < 0) { shOurs = 0; }
		if (shOurs > 65535) { shOurs = 65535; }
		shErr = shOurs - retail;
		shAbs = (shErr < 0) ? -shErr : shErr;

		if (s_trkChkShSamples == 0)
		{
			s_trkChkShMinErr = shErr;
			s_trkChkShMaxErr = shErr;
		}
		else
		{
			if (shErr < s_trkChkShMinErr) { s_trkChkShMinErr = shErr; }
			if (shErr > s_trkChkShMaxErr) { s_trkChkShMaxErr = shErr; }
		}
		if (shAbs > s_trkChkShMaxAbsErr) { s_trkChkShMaxAbsErr = shAbs; }
		s_trkChkShSumAbsErr += (double)shAbs;
		s_trkChkShSamples++;
	}

	s_trkChkSamples++;
	{
		s32 absErr = (err < 0) ? -err : err;

		s_trkChkSumAbsErr += (double)absErr;
		if (absErr > s_trkChkMaxAbsErr) { s_trkChkMaxAbsErr = absErr; }
		if (absErr > 1) { s_trkChkOver1++; }
	}

	if (s_trkChkSamples >= 200000u)
	{
		/* err is printed SIGNED and as a range, not just as a magnitude: the
		 * sign is the whole answer. Ours consistently BELOW retail means the
		 * road reads nearer than it should, which is the sink; consistently
		 * above means the opposite. A magnitude alone would hide that. */
		Platform_LogInstrument("[TRKDEPTHCHK] lvl=%d n=%lu maxAbsErr=%ld meanAbsErr=%.3f over1=%lu err=%ld..%ld ours=%ld..%ld retail=%ld..%ld\n",
		                       NativePerf_CurrentLevelID(), (unsigned long)s_trkChkSamples, (long)s_trkChkMaxAbsErr,
		                       s_trkChkSumAbsErr / (double)s_trkChkSamples, (unsigned long)s_trkChkOver1, (long)s_trkChkMinErr, (long)s_trkChkMaxErr,
		                       (long)s_trkChkOursLo, (long)s_trkChkOursHi, (long)s_trkChkRetailLo, (long)s_trkChkRetailHi);

		/* The two arms side by side. If shErr is a few hundred while err is 0,
		 * the composed-row/float24 path is the sink and the integer check was
		 * validating arithmetic the GPU never runs. w= is the intermediate the
		 * dp4 produces before the multiply back by h: a range that straddles
		 * zero, or one whose magnitude dwarfs viewZ/h, is cancellation. */
		Platform_LogInstrument("[TRKDEPTHSH] n=%lu shErr=%ld..%ld shMaxAbs=%ld shMeanAbs=%.3f h=%d trz=%ld w=%.4f..%.4f\n",
		                       (unsigned long)s_trkChkShSamples, (long)s_trkChkShMinErr, (long)s_trkChkShMaxErr,
		                       (long)s_trkChkShMaxAbsErr, s_trkChkShSamples ? (s_trkChkShSumAbsErr / (double)s_trkChkShSamples) : 0.0,
		                       (int)snap->h, (long)snap->trans[2], (double)s_trkChkWLo, (double)s_trkChkWHi);

		s_trkChkShSamples = 0;
		s_trkChkShMaxAbsErr = 0;
		s_trkChkShSumAbsErr = 0.0;

		s_trkChkSamples = 0;
		s_trkChkOver1 = 0;
		s_trkChkSumAbsErr = 0.0;
		s_trkChkMaxAbsErr = 0;
	}
}

internal void NativeTrackFrame_NearCensus(const struct NativeTrackFrameSnapshot *snap)
{
	const struct NativeTrackVert *verts;
	const u16 *indices;
	unsigned vertCount, indexCount;
	u32 i;

	if ((g_dbg_trackNearCensus == 0) || (snap == NULL) || (snap->valid == 0) || (snap->leafCount == 0))
	{
		return;
	}

	if (--s_trkNearSampleCountdown != 0)
	{
		return;
	}
	s_trkNearSampleCountdown = 8;

	verts = NativeTrackMesh_Verts();
	indices = NativeTrackMesh_Indices();
	vertCount = NativeTrackMesh_VertCount();
	indexCount = NativeTrackMesh_IndexCount();

	if ((verts == NULL) || (indices == NULL) || (vertCount == 0) || (indexCount < 3))
	{
		return;
	}

	s_trkNearFrames++;

	for (i = 0; i < snap->leafCount; i++)
	{
		unsigned bspIndex, firstIndex, leafIndexCount, t;

		if (!NativeTrackMesh_LeafAt(snap->leafSlot[i], &bspIndex, &firstIndex, &leafIndexCount))
		{
			continue;
		}

		if ((firstIndex >= indexCount) || ((firstIndex + leafIndexCount) > indexCount))
		{
			continue;
		}

		for (t = 0; (t + 3) <= leafIndexCount; t += 3)
		{
			int nBehind = 0;
			int nClamped = 0;
			int c;

			for (c = 0; c < 3; c++)
			{
				u16 vi = indices[firstIndex + t + c];
				const struct NativeTrackVert *vv;
				s64 mac3;
				s32 viewZ;

				if (vi >= vertCount)
				{
					continue;
				}
				vv = &verts[vi];

				/* Same arithmetic the shader does (viewZ = R2.v/4096 + TRZ),
				 * in integer so the comparison against 0 and 256 is exact. */
				mac3 = ((s64)snap->trans[2] * 4096) + ((s64)snap->rot[2][0] * vv->x) + ((s64)snap->rot[2][1] * vv->y) + ((s64)snap->rot[2][2] * vv->z);
				viewZ = (s32)(mac3 >> 12);

				if ((s_trkNearTris == 0) && (c == 0) && (t == 0) && (i == 0))
				{
					s_trkNearViewZLo = viewZ;
					s_trkNearViewZHi = viewZ;
				}
				else
				{
					if (viewZ < s_trkNearViewZLo) { s_trkNearViewZLo = viewZ; }
					if (viewZ > s_trkNearViewZHi) { s_trkNearViewZHi = viewZ; }
				}

				if (viewZ <= 0)
				{
					nBehind++;
				}
				else if (viewZ < 256)
				{
					nClamped++;
				}
			}

			/*
			 * drawOrder census. This is the LAST asymmetry between the road
			 * and a racer once both depths are proven bit-exact against the
			 * GTE ([TRKDEPTHCHK] err=0..0, [MDLDEPTH] maxErr=0): the track
			 * shader adds `(drawOrder-128) * bucketToDepth` to its depth and
			 * the model shader adds nothing, because packets carry no
			 * drawOrder at all. bucketToDepth is 1.0, so this term IS a
			 * per-face shift in view.z units, applied to the road and to
			 * nothing else -- constant, and independent of camera distance and
			 * angle, which is the shape the owner reported (sunk on aerial
			 * sweeps too).
			 *
			 * It was dismissed twice by argument and never measured. s29 cut
			 * the weight 64 -> 1 after it "buried the player kart to the neck"
			 * and asserted 1 was "far too small to outrank a kart" -- an
			 * assertion about magnitudes made without ever looking at the
			 * VALUES. If real faces carry drawOrder of -20, the road is being
			 * pulled 20 units toward the camera and nothing else is.
			 *
			 * Reported as a signed range plus the mean of the NEGATIVE side
			 * specifically: only negative drawOrder pulls a face nearer, and
			 * an average over both signs would cancel the very thing being
			 * looked for.
			 */
			for (c = 0; c < 3; c++)
			{
				u16 vi = indices[firstIndex + t + c];

				if (vi < vertCount)
				{
					int dord = (int)verts[vi].drawOrder;

					if ((s_trkNearTris == 0) && (c == 0))
					{
						s_trkDordLo = dord;
						s_trkDordHi = dord;
					}
					else
					{
						if (dord < s_trkDordLo) { s_trkDordLo = dord; }
						if (dord > s_trkDordHi) { s_trkDordHi = dord; }
					}

					s_trkDordSamples++;
					if (dord < 0)
					{
						s_trkDordNeg++;
						s_trkDordNegSum += (double)(-dord);
					}
					else if (dord > 0)
					{
						s_trkDordPos++;
					}
				}
			}

			s_trkNearTris++;
			if (nBehind == 3)
			{
				s_trkNearBehind++;
			}
			else if (nBehind > 0)
			{
				s_trkNearStraddle++;
			}
			else if (nClamped > 0)
			{
				s_trkNearClamped++;
			}
		}
	}

	if (s_trkNearFrames >= 8)
	{
		Platform_LogInstrument("[TRKNEAR] lvl=%d f=%lu tris/f=%lu behind/f=%lu straddle/f=%lu clamped/f=%lu viewZ=%ld..%ld\n", NativePerf_CurrentLevelID(),
		                       (unsigned long)s_trkNearFrames, (unsigned long)(s_trkNearTris / s_trkNearFrames), (unsigned long)(s_trkNearBehind / s_trkNearFrames),
		                       (unsigned long)(s_trkNearStraddle / s_trkNearFrames), (unsigned long)(s_trkNearClamped / s_trkNearFrames),
		                       (long)s_trkNearViewZLo, (long)s_trkNearViewZHi);

		Platform_LogInstrument("[TRKDORD] lvl=%d n=%lu range=%ld..%ld neg=%lu (%lu%%) pos=%lu meanNegShift=%.2f viewZunits\n", NativePerf_CurrentLevelID(),
		                       (unsigned long)s_trkDordSamples, (long)s_trkDordLo, (long)s_trkDordHi, (unsigned long)s_trkDordNeg,
		                       (s_trkDordSamples > 0) ? (unsigned long)((s_trkDordNeg * 100u) / s_trkDordSamples) : 0ul, (unsigned long)s_trkDordPos,
		                       (s_trkDordNeg > 0) ? (s_trkDordNegSum / (double)s_trkDordNeg) : 0.0);

		s_trkNearFrames = 0;
		s_trkNearTris = 0;
		s_trkNearBehind = 0;
		s_trkNearStraddle = 0;
		s_trkNearClamped = 0;
		s_trkDordSamples = 0;
		s_trkDordNeg = 0;
		s_trkDordPos = 0;
		s_trkDordNegSum = 0.0;
	}
}

void NativeTrackFrame_Record(struct GameTracker *gGT, struct mesh_info *mesh, struct DrawLevelOvr1PRenderList *renderList, struct PushBuffer *pushBuffer)
{
	struct NativeTrackFrameSnapshot *dst = &s_trackFrameBuf[s_trackFrameWriteIdx];
	static u32 s_logCountdown = 64;
	static int s_lastLoggedToggle = -1; /* -1 = never logged; forces a line on the first call */
	u32 residentIndexCount = 0;

	/* Pessimistic default: only set valid=1 once every field below has been
	 * filled successfully. A bail-out anywhere below leaves a snapshot a
	 * step-2 consumer must treat as "draw nothing", never a half-written one
	 * from a level that no longer matches. */
	dst->leafCount = 0;
	dst->droppedLeaves = 0;
	dst->degraded = 0;
	dst->visFaceList = NULL;
	dst->valid = 0;

	if ((g_dbg_trackRender != 0) && (gGT != NULL) && (mesh != NULL) && (renderList != NULL) && (pushBuffer != NULL) && (gGT->visMem1 != NULL) &&
	    NativeTrackMesh_IsBuiltFor(mesh))
	{
		const struct VisMemBspListNode *bspListBase = gGT->visMem1->bspList[0];
		/* Actual per-level ceiling for the walk cap below -- see
		 * NativeTrackFrame_WalkList's comment on why this replaces a fixed
		 * constant copied from the OT walk. */
		u32 nodeCap = (mesh->numBspNodes > 0) ? (u32)mesh->numBspNodes : 1u;

		/* Camera: RAW retail values, no pre-divide/pre-scale -- see the
		 * struct comment. Matches exactly what RenderLists_Load1P2PGteState
		 * (game/RenderLevel/RenderLists.c) hands the GTE for this same
		 * traversal. */
		dst->h = pushBuffer->distanceToScreen_PREV;
		dst->ofx = pushBuffer->rect.w >> 1;
		dst->ofy = pushBuffer->rect.h >> 1;

		for (int r = 0; r < 3; r++)
		{
			for (int c = 0; c < 3; c++)
			{
				dst->rot[r][c] = pushBuffer->matrix_ViewProj.m[r][c];
			}
		}
		dst->trans[0] = pushBuffer->matrix_ViewProj.t[0];
		dst->trans[1] = pushBuffer->matrix_ViewProj.t[1];
		dst->trans[2] = pushBuffer->matrix_ViewProj.t[2];

		/* visFaceList (review fix, design 4.2/12): recorded now so step 2
		 * doesn't have to reopen this struct/protocol later. RECORD ONLY --
		 * see the struct comment on why nothing may dereference this yet. */
		dst->visFaceList = gGT->visMem1->visFaceList[0];

		/* Five subdivided/water slots, then FULL_DYNAMIC separately -- same
		 * shape as struct DrawLevelOvr1PRenderList (include/namespace_DrawLevel.h). */
		for (int slot = 0; slot < DRAW_LEVEL_OVR1P_RENDER_LIST_SLOT_COUNT; slot++)
		{
			NativeTrackFrame_WalkList(bspListBase, renderList->list[slot].bspListStart, dst, nodeCap);
		}

		/*
		 * FULL_DYNAMIC (design erratum (b) territory, and brief correction
		 * over design doc 4.4's fade language): included here as an
		 * ORDINARY resident leaf, not excluded and not drawn at reduced
		 * density. Two reasons this is right rather than a shortcut:
		 *   1. The CTR_NATIVE_LOD_BIAS instrument (MainFrame_RenderFrame.c)
		 *      already measured that FULL_DYNAMIC is a different DRAWING
		 *      STYLE for a quadblock (the retail fade-from-color_lo), not a
		 *      lower-density one -- so "excluding" it would not even be
		 *      approximating a real LOD tier, just dropping geometry.
		 *   2. Design 4.4 already rules the fade itself out of v1 (uniform
		 *      mid-LOD everywhere, no fade, no mosaic -- perf-over-parity),
		 *      so there is no retail behavior left for FULL_DYNAMIC leaves
		 *      to approximate differently from any other resident leaf.
		 *
		 * WATER slot leaves are ordinary resident leaves too, as of the owner's
		 * s30 ruling. This comment used to assert the opposite -- that the mesh
		 * builder never creates a resident range for a BSP_LEAF_FLAG_WATER leaf,
		 * so the walk dropped them by construction. It does now, so the water
		 * slot walked in the loop above contributes real leaves, and they draw
		 * opaque until the semi-transparency work lands. Nothing here needed to
		 * change for that; the note is corrected because a reader trusting the
		 * old wording would conclude a whole render-list slot was dead.
		 */
		NativeTrackFrame_WalkList(bspListBase, renderList->bspListStart_FullDynamic, dst, nodeCap);

		/*
		 * s33 leaf keep-alive -- the drift-corruption fix. Owner diagnosed the
		 * artifact as HOLES (geometry missing behind the kart mid-drift), which
		 * convicts the visibility list lagging fast camera swings, not the
		 * behind-eye clamp class. A leaf therefore stays in the draw set for
		 * g_dbg_trackLeafKeep frames after it last appeared in retail's own
		 * visible lists ("visible union recently-visible"), which covers the
		 * swing-behind-you case for a bounded cost instead of drawing the whole
		 * level. Appended AFTER the real walks so visible leaves always win the
		 * cap, and NEVER through the droppedLeaves/degraded path: stale leaves
		 * are garnish, and running out of room for them must not blank the
		 * frame the way a real visible-leaf drop does. Stamp 0 means "never
		 * seen"; the counter skips 0 on wrap (one leaf mis-aged every ~36
		 * minutes at 30fps is the accepted cost of a u16 stamp).
		 */
		if (g_dbg_trackLeafKeep > 0)
		{
			unsigned totalSlots = NativeTrackMesh_LeafCount();
			unsigned slot;
			u32 i;

			if (totalSlots > TRACKFRAME_KEEP_MAX_SLOTS)
			{
				totalSlots = TRACKFRAME_KEEP_MAX_SLOTS;
			}

			/* New level = new slot numbering; stale stamps would keep the
			 * wrong leaves for keepN frames. Mesh identity is the pointer the
			 * build was verified against (NativeTrackMesh_IsBuiltFor above). */
			if (mesh != s_leafKeepMesh)
			{
				memset(s_leafKeepStamp, 0, sizeof(s_leafKeepStamp));
				s_leafKeepMesh = mesh;
				s_leafKeepNow = 0;
			}

			if (++s_leafKeepNow == 0)
			{
				s_leafKeepNow = 1;
			}

			for (i = 0; i < dst->leafCount; i++)
			{
				if (dst->leafSlot[i] < TRACKFRAME_KEEP_MAX_SLOTS)
				{
					s_leafKeepStamp[dst->leafSlot[i]] = s_leafKeepNow;
				}
			}

			for (slot = 0; (slot < totalSlots) && (dst->leafCount < NATIVE_TRACK_FRAME_MAX_LEAVES); slot++)
			{
				u16 stamp = s_leafKeepStamp[slot];

				if ((stamp != 0) && (stamp != s_leafKeepNow) && ((u16)(s_leafKeepNow - stamp) <= (u16)g_dbg_trackLeafKeep))
				{
					dst->leafSlot[dst->leafCount++] = (u16)slot;
				}
			}
		}

		if (dst->droppedLeaves > 0)
		{
			dst->degraded = 1;
			s_trackFrameDroppedTotal += dst->droppedLeaves;

			if (!s_trackFrameDroppedWarned)
			{
				s_trackFrameDroppedWarned = 1;
				Platform_Log("[TRKFRAME] WARNING: first leaf-cap drop this session (frame dropped=%u, cap=%d) -- snapshot marked degraded\n",
				             dst->droppedLeaves, NATIVE_TRACK_FRAME_MAX_LEAVES);
			}
		}

		dst->frameSeq = ++s_trackFrameSeq;
		dst->valid = 1;
	}

	/* Resident index count that WOULD be drawn -- summed from the mesh's own
	 * per-leaf index ranges, not tracked incrementally in the walk above, so
	 * this number is always consistent with whatever NativeTrackMesh_LeafAt
	 * reports right now even if the mesh were to change between frames. */
	for (u32 i = 0; i < dst->leafCount; i++)
	{
		unsigned outBspIndex, outFirstIndex, outIndexCount;

		if (NativeTrackMesh_LeafAt(dst->leafSlot[i], &outBspIndex, &outFirstIndex, &outIndexCount))
		{
			residentIndexCount += outIndexCount;
		}
	}

	/* Review fix: log only while the toggle is actually on. The point of
	 * state 0 being the golden/A-B baseline leg is that it costs nothing
	 * extra versus retail; a periodic log line (an SD write on 3DS) firing
	 * even with the toggle off broke that. A state CHANGE still logs
	 * immediately regardless of direction, so a 1->0 flip (or a 2->0, etc.)
	 * remains visible in the log even though the steady-state OFF leg goes
	 * quiet again right after. */
	if (g_dbg_trackRender != s_lastLoggedToggle)
	{
		Platform_Log("[TRKFRAME] toggle changed: %d -> %d\n", s_lastLoggedToggle, g_dbg_trackRender);
		s_lastLoggedToggle = g_dbg_trackRender;
	}

	NativeTrackFrame_NearCensus(dst);

	if (g_dbg_trackRender != 0)
	{
		/* Step 1's only observable output: proves the recorder runs and
		 * counts sane numbers in Azahar before step 2 ever draws a pixel.
		 * Fires every 64 calls while the toggle is on. */
		if (--s_logCountdown == 0)
		{
			s_logCountdown = 64;
			Platform_Log("[TRKFRAME] toggle=%d leaves=%u dropped=%u droppedTotal=%u degraded=%d idx=%u h=%d ofx=%d ofy=%d\n", g_dbg_trackRender,
			             dst->leafCount, dst->droppedLeaves, s_trackFrameDroppedTotal, dst->degraded, residentIndexCount, (int)dst->h, (int)dst->ofx,
			             (int)dst->ofy);
		}
	}
}

void NativeTrackFrame_PublishForWalk(void)
{
	/* Only sets read = write -- the flip-and-invalidate that used to live
	 * here moved to NativeTrackFrame_Clear (called from MainFrame_ResetDB,
	 * same site as GTECapture_Clear). See the header's file-level comment
	 * for the bug that reasoning-by-analogy-alone shipped: this function
	 * runs from EVERY DrawOTag (FMV, the error screen, load-time clears),
	 * not just frames that called Record, so it must never be the thing
	 * that decides a buffer is stale -- only Clear, on the frame-reset
	 * boundary, is allowed to do that. */
	s_trackFrameReadIdx = s_trackFrameWriteIdx;
}

void NativeTrackFrame_Clear(void)
{
	/* Flip THEN invalidate the new write buffer -- same idiom as
	 * GTECapture_Clear (native_gte_capture.c). Flip first so the buffer
	 * being abandoned (which an in-flight walk may still be reading via
	 * s_trackFrameReadIdx) is left untouched; the invalidation only ever
	 * touches the buffer this frame is about to write, which by the same
	 * one-walk-in-flight argument the header comment makes, no walk can
	 * still hold. */
	s_trackFrameWriteIdx ^= 1;
	s_trackFrameBuf[s_trackFrameWriteIdx].valid = 0;
}

const struct NativeTrackFrameSnapshot *NativeTrackFrame_ForWalk(void)
{
	return &s_trackFrameBuf[s_trackFrameReadIdx];
}

/*
 * Does the resident renderer replace the retail track draw THIS frame?
 *
 * The toggle alone is not enough to answer that, which is the bug this
 * function exists to close. `g_dbg_trackRender == 1` means "resident replaces
 * retail" -- but the resident DRAW path below is __3DS__-only, so on a PC
 * build state 1 used to switch retail off with nothing to take its place. The
 * whole level then simply vanished: the owner hit it as "Oxide's cutscene lost
 * his spaceship, he's floating in space", which is exactly right -- the ship
 * IS the level geometry, and nothing was drawing level geometry at all.
 *
 * PC's job for this feature is the counts loop (mesh build, frame recorder,
 * animation patcher, [TRKMESH]/[TRKFRAME]/[TRKANIM]), all of which run fine
 * with the retail draw left alone. It has never been able to verify pixels and
 * does not become able to by blanking them. So: on any platform without a
 * resident draw, state 1 drives the recorder and leaves retail drawing.
 *
 * Lives here rather than in game/ so the platform test stays out of the
 * decomp -- MainFrame_RenderFrame.c calls this and never learns what a 3DS is.
 */
int NativeTrackFrame_SkipRetailDraw(void)
{
#if defined(__3DS__)
	extern int g_dbg_trackGapFill;

	/* Gap fill keeps the retail call alive for the leaves residency excluded
	 * -- see the call site's own comment for that contract. */
	return (g_dbg_trackRender == 1) && !g_dbg_trackGapFill;
#else
	return 0;
#endif
}

#if defined(__3DS__)
/*
 * Rung 3a step 2: the CPU-side per-frame draw issuer (docs/
 * NATIVE_TRACK_RENDERER_DESIGN.md 4.2/4.3/12b). Turns the double-buffered
 * snapshot above into the resident track's actual draw calls, via the
 * plain-scalar N3DS_TrackDraw_* API (platform/n3ds/n3ds_backend.c) --
 * everything below is ordinary game-domain math (matrix rows, retail's OT
 * formula), never a citro3d/libctru call, matching this project's TU/ABI
 * boundary rule.
 *
 * Called from NativeRenderer_EndScene (platform/n3ds/native_renderer_3ds.c),
 * which on render-thread builds runs on the WORKER thread -- every input
 * here is either a copy-by-value from the double-buffered snapshot (rot/
 * trans/h/ofx/ofy) or the mesh builder's own static per-level tables
 * (NativeTrackMesh_LeafAt/LeafBox), never a live gGT/PushBuffer/mesh_info
 * pointer. This is exactly the boundary native_track_frame.h's file
 * comment and native_track_mesh.h's NativeTrackMesh_LeafIndexForBsp comment
 * already establish for the rest of this file.
 */

/*
 * Per-leaf depth-range compose (design 4.3/12b's zOrd bullet): project the
 * leaf's STATIC bounding box through the SAME camera Z row the shader uses
 * (rot[2]/trans[2], NO H term -- this is retail's SZ3/maxDepth axis
 * directly, GTE_RotTransPers's C2_MAC3->Lm_D(.,1) path,
 * platform/native_gte_core.c:319-328, confirmed to be R2.v/4096+TRZ with NO
 * reciprocal, not assumed from general PS1 GTE lore), convert both ends to
 * an OT bucket index with retail's own formula
 * (DrawLevelOvr1P_GetProjectedOtIndex, game/226/226_00_DrawLevelOvr1P.c:1950-1976),
 * clamps both ends UP to the nearest bucket that actually held real content
 * this walk (NativeGpu_GetMinContentBucket -- the depth-compose fix, see the
 * comment at that clamp below for why an unclamped near-camera bucket reads
 * as nearer than the whole frame), then looks up each end's emit ordinal via
 * the rung-1 bucket->ordinal table. Returns 0 (leaf must be skipped, not
 * drawn with a guessed ordinal) when any lookup reports failure -- honoring
 * NativeGpu_GetOtBucketOrdinal's / NativeGpu_GetMinContentBucket's own
 * failure contract (recorder not active, slot/content never recorded this
 * walk).
 *
 * REVIEW FIX (FIX 1a -- correctness, not cosmetic): drawOrderMin/drawOrderMax
 * used to be omitted entirely on the reasoning that drawOrder is a per-FACE
 * term the shader adds back per vertex, so the leaf-level range only needed
 * the box's own geometry. That reasoning was incomplete: the box bounds
 * where the leaf's geometry SITS, but says nothing about how far a face's
 * OWN drawOrder can additionally push that face's vertices along the OT
 * axis in the shader. A leaf whose box-derived range spans only one or two
 * OT buckets has a large invIndexSpan; a face whose drawOrder pushed its
 * own otIndex_v outside [otIndexLo, otIndexLo+span] made the shader's lerp
 * EXTRAPOLATE by a large multiple, and because a valid ordinal (0..65535)
 * already saturates the vertex-z clip range, an extrapolated ordinal lands
 * outside the clip volume and the GPU DISCARDS those triangles -- geometry
 * silently vanishes, not merely mis-layers. The prior comment in
 * ctr_track.v.pica called this "the same harmless residual...within-bucket
 * ties", which was wrong: the failure isn't a tie-break, it's clipped-away
 * geometry, and it's proportional to how narrow a leaf's OWN box-derived
 * range is (worse on small leaves, exactly where a single wrong-signed
 * drawOrder byte is most visible). Widening the leaf's [otIndexLo, otIndexHi]
 * by its own [drawOrderMin, drawOrderMax] (tracked per-leaf at BUILD time,
 * native_track_mesh.c) before the retail clamp below guarantees every vertex
 * this leaf can ever emit falls inside the range its draw uniform describes
 * -- no extrapolation, by construction. The shader's own fraction clamp
 * (ctr_track.v.pica) is belt-and-braces on top of this, not a substitute for
 * it.
 */
/*
 * DIAGNOSTIC-ONLY out-params added below (outOtIndexHi, outOrdinalHi,
 * outViewZLo, outViewZHi, outWalkOtCount) -- uncommitted rung 3a occlusion
 * investigation (2026-07-30): the resident track draws but occludes
 * everything (retail track, karts, HUD, minimap) even though every leaf's
 * bucket->ordinal lookup reports success. These mirror values this function
 * ALREADY computes for the real (non-diagnostic) outputs -- nothing here
 * changes what's computed for drawing, or what N3DS_TrackDraw_Leaf receives;
 * every existing statement and return value is untouched. Caller
 * (NativeTrackDraw_IssueForWalk) uses these to log per-leaf and per-frame
 * depth-compose numbers without duplicating this math a second time, which
 * would risk the log silently drifting from what actually got drawn.
 */
internal int NativeTrackDraw_ComputeLeafDepthRange(const struct NativeTrackFrameSnapshot *snap, const s16 boxMin[3], const s16 boxMax[3], s8 drawOrderMin,
                                                    s8 drawOrderMax, float *outOtIndexLo, float *outOrdinalLo, float *outOrdinalSpan, float *outInvIndexSpan,
                                                    float *outOtIndexHi, float *outOrdinalHi, float *outViewZLo, float *outViewZHi, u32 *outWalkOtCount)
{
	float r20 = (float)snap->rot[2][0] * (1.0f / 4096.0f);
	float r21 = (float)snap->rot[2][1] * (1.0f / 4096.0f);
	float r22 = (float)snap->rot[2][2] * (1.0f / 4096.0f);
	float trz = (float)snap->trans[2];
	float viewZLo = 0.0f, viewZHi = 0.0f;
	int corner;
	int otIndexLoInt, otIndexHiInt;
	u16 ordinalLoU16, ordinalHiU16;
	u32 walkOtCount;

	for (corner = 0; corner < 8; corner++)
	{
		float vx = (float)((corner & 1) ? boxMax[0] : boxMin[0]);
		float vy = (float)((corner & 2) ? boxMax[1] : boxMin[1]);
		float vz = (float)((corner & 4) ? boxMax[2] : boxMin[2]);
		float viewZ = r20 * vx + r21 * vy + r22 * vz + trz;

		if ((corner == 0) || (viewZ < viewZLo))
		{
			viewZLo = viewZ;
		}
		if ((corner == 0) || (viewZ > viewZHi))
		{
			viewZHi = viewZ;
		}
	}

	/* Clamp to retail's SZ3 range (Lm_D, native_gte_core.c:211-213) before
	 * quantizing -- maxDepth is a clamped-unsigned-16-bit GTE register, not a
	 * raw signed float, and DrawLevelOvr1P_GetProjectedOtIndex's own >>6
	 * assumes that clamp already happened. */
	if (viewZLo < 0.0f) { viewZLo = 0.0f; }
	if (viewZLo > 65535.0f) { viewZLo = 65535.0f; }
	if (viewZHi < 0.0f) { viewZHi = 0.0f; }
	if (viewZHi > 65535.0f) { viewZHi = 65535.0f; }

	/* otIndex = clamp((maxDepth>>6) + drawOrder, 0, MAX_OT_INDEX) -- (int)
	 * truncation of a non-negative float is floor, matching an unsigned >>6.
	 * FIX 1a: drawOrderMin/drawOrderMax (this leaf's own min/max per-face
	 * drawOrder, tracked at build time) are added HERE, before the retail
	 * clamp, so the clamp itself matches DrawLevelOvr1P_GetProjectedOtIndex's
	 * exact bounds (game/226/226_00_DrawLevelOvr1P.c:1966-1976: clamp to
	 * [0, MAX_OT_INDEX], not just capped at the top) -- the low end can now
	 * go negative before clamping (viewZLo/64 is always >= 0, but
	 * drawOrderMin can be as negative as -128), which the pre-fix code never
	 * needed to handle because drawOrder was omitted entirely. */
	otIndexLoInt = (int)(viewZLo * (1.0f / 64.0f)) + drawOrderMin;
	otIndexHiInt = (int)(viewZHi * (1.0f / 64.0f)) + drawOrderMax;
	if (otIndexLoInt < 0) { otIndexLoInt = 0; }
	if (otIndexHiInt < 0) { otIndexHiInt = 0; }
	if (otIndexLoInt > DRAW_LEVEL_OVR1P_MAX_OT_INDEX) { otIndexLoInt = DRAW_LEVEL_OVR1P_MAX_OT_INDEX; }
	if (otIndexHiInt > DRAW_LEVEL_OVR1P_MAX_OT_INDEX) { otIndexHiInt = DRAW_LEVEL_OVR1P_MAX_OT_INDEX; }

	/*
	 * Depth-compose fix (docs/NATIVE_TRACK_RENDERER_DESIGN.md 4.3, confirmed
	 * 2026-07-30 in a N. Gin Labs race): a bucket nearer than all real packet
	 * content is not a meaningful depth anchor. The camera sits ON the
	 * track, so this leaf's own box can reach bucket 0 -- a bucket retail
	 * never draws anything into -- and native_gpu.c's otBucketOrdinal table
	 * still has an entry for it (the walk crosses its marker node), but that
	 * entry is just whatever the emit counter had reached by the END of the
	 * walk (markers are crossed in descending index order), i.e. a value
	 * PAST every real primitive this frame. Measured: otIdxMin=0 recorded
	 * ordinal 931 vs finalOrdinal 930 -- the track drew in front of
	 * literally everything, including the HUD.
	 *
	 * Clamping both ends UP to the nearest bucket that actually held content
	 * (NativeGpu_GetMinContentBucket) means a leaf can be at most as near as
	 * the nearest real content this frame, never nearer -- and because a
	 * bucket's recorded ordinal is captured BEFORE that bucket's own
	 * primitives are emitted (the walk crosses the marker, then parses/
	 * direct-emits the bucket's content), the clamped leaf sorts BEHIND
	 * whatever real content shares its now-nearest bucket, which is exactly
	 * right for the HUD and other near-camera UI that must stay on top.
	 *
	 * If no content-advance was recorded at all this walk (recorder not
	 * active, or a genuinely empty walk), there is nothing to clamp against
	 * -- guessing a depth here would be exactly the failure mode this fix
	 * closes, so the leaf is skipped like any other ordinal-lookup failure.
	 */
	{
		u32 minContentBucket;

		if (!NativeGpu_GetMinContentBucket(&minContentBucket))
		{
			return 0;
		}

		if ((u32)otIndexLoInt < minContentBucket) { otIndexLoInt = (int)minContentBucket; }
		if ((u32)otIndexHiInt < minContentBucket) { otIndexHiInt = (int)minContentBucket; }
	}

	if (!NativeGpu_GetOtBucketOrdinal((u32)otIndexLoInt, &ordinalLoU16, &walkOtCount))
	{
		return 0;
	}
	if (!NativeGpu_GetOtBucketOrdinal((u32)otIndexHiInt, &ordinalHiU16, &walkOtCount))
	{
		return 0;
	}

	*outOtIndexLo = (float)otIndexLoInt;
	*outOrdinalLo = (float)ordinalLoU16;
	*outOrdinalSpan = (float)ordinalHiU16 - (float)ordinalLoU16;
	/* DIAGNOSTIC ONLY: mirrors of values already computed above, for the
	 * caller's log line -- see this function's header comment. outViewZLo/Hi
	 * are the retail-SZ3-clamped view-z BEFORE the >>6 quantization to
	 * otIndex (i.e. before the block just above this one), matching what a
	 * reviewer would want to sanity-check the bounding-box projection itself
	 * against, independent of the OT-bucket math downstream of it. */
	*outOtIndexHi = (float)otIndexHiInt;
	*outOrdinalHi = (float)ordinalHiU16;
	*outViewZLo = viewZLo;
	*outViewZHi = viewZHi;
	*outWalkOtCount = walkOtCount;
	/* otIndexHiInt >= otIndexLoInt by construction (Hi is the max sample plus
	 * drawOrderMax, Lo the min sample plus drawOrderMin; floor is monotone
	 * non-decreasing on non-negative reals, drawOrderMax >= drawOrderMin by
	 * definition since both come from the same leaf's face set, and clamping
	 * to [0, MAX_OT_INDEX] is itself monotone, so the ordering survives it)
	 * -- the only degenerate case is equality, a leaf small enough (and
	 * uniform enough in drawOrder) that its whole depth range quantizes into
	 * one OT bucket. invIndexSpan of 0 makes the shader's lerp fraction
	 * always 0, i.e. a per-leaf CONSTANT ordinal -- exactly the one-line
	 * degradation the design doc names as an acceptable fallback, falling
	 * out naturally here rather than needing its own special case. */
	*outInvIndexSpan = (otIndexHiInt > otIndexLoInt) ? (1.0f / (float)(otIndexHiInt - otIndexLoInt)) : 0.0f;
	return 1;
}

void NativeTrackDraw_IssueForWalk(void)
{
	const struct NativeTrackFrameSnapshot *snap = NativeTrackFrame_ForWalk();
	static u32 s_logCountdown = 64;
	static u32 s_invalidLogCountdown = 64;
	static u32 s_zeroHWarned = 0;
	static u32 s_totalSkippedOrdinal = 0;
	u32 drawsIssued = 0, leavesSkippedOrdinal = 0, indicesDrawn = 0, blendDrawsIssued = 0;
	float qx[4], qy[4], qw[4];
	float h, ofx, ofy;

	if (g_dbg_trackRender == 0)
	{
		return;
	}

	/* Review fix (FIX 5): this write used to sit in NativeRenderer_EndScene
	 * OUTSIDE the toggle check, so it ran every frame even at toggle 0 --
	 * the A/B baseline every fps number is measured against, which must be
	 * indistinguishable from a build with this feature compiled out. Toggle
	 * 0 already returned above, so this line only runs on the toggle-on
	 * path now. s_runUntextured lives in native_renderer_3ds.c
	 * (`global_variable` = static) but that file is #include'd into main.c's
	 * unity TU BEFORE this one (main.c's include order), so it's an ordinary
	 * in-TU reference, not a cross-TU reach. Effect is near-nil either way --
	 * N3DS_MergeFlush's own lastDoubling=-1 reasserts regardless -- but
	 * near-nil is not identical, and identical is the bar toggle 0 has to
	 * clear. */
	s_runUntextured = -1;

	/* A degraded snapshot is an INCOMPLETE leaf list (the leaf cap or the
	 * BSP-walk cap truncated it); an invalid one means nothing was recorded
	 * since the last reset (mesh not built, level transition mid-frame, or
	 * this window simply hasn't run Record yet). Either way: draw nothing
	 * rather than a partial track -- the exact contract
	 * struct NativeTrackFrameSnapshot's header comment states for step 2. */
	if ((snap->valid == 0) || (snap->degraded != 0))
	{
		if (--s_invalidLogCountdown == 0)
		{
			s_invalidLogCountdown = 64;
			Platform_Log("[TRKDRAW] skip: snapshot valid=%d degraded=%d\n", snap->valid, snap->degraded);
		}
		return;
	}

	h = (float)snap->h;
	ofx = (float)snap->ofx;
	ofy = (float)snap->ofy;

	if (h == 0.0f)
	{
		/* Degenerate camera state (distanceToScreen_PREV == 0) -- every
		 * division below would be by zero. Never observed in real play;
		 * guard and count rather than divide. */
		if (!s_zeroHWarned)
		{
			s_zeroHWarned = 1;
			Platform_Log("[TRKDRAW] WARNING: h==0 this frame, skipping (degenerate camera snapshot)\n");
		}
		return;
	}

	/*
	 * Compose the three projection rows (design's derived math -- see
	 * platform/n3ds/shaders/ctr_track.v.pica's header comment for the full
	 * derivation): qx/qy fold (OFX or OFY)/H*R2/4096 + TR terms into the
	 * affine rows so the shader's dp4 against raw model-space (vx,vy,vz,1)
	 * yields w*screenX and w*screenY directly; qw recovers w = view.z/H,
	 * and the shader multiplies back by H (passed separately) to reach true
	 * view.z for the depth compose.
	 */
	{
		float r00 = (float)snap->rot[0][0] * (1.0f / 4096.0f);
		float r01 = (float)snap->rot[0][1] * (1.0f / 4096.0f);
		float r02 = (float)snap->rot[0][2] * (1.0f / 4096.0f);
		float r10 = (float)snap->rot[1][0] * (1.0f / 4096.0f);
		float r11 = (float)snap->rot[1][1] * (1.0f / 4096.0f);
		float r12 = (float)snap->rot[1][2] * (1.0f / 4096.0f);
		float r20 = (float)snap->rot[2][0] * (1.0f / 4096.0f);
		float r21 = (float)snap->rot[2][1] * (1.0f / 4096.0f);
		float r22 = (float)snap->rot[2][2] * (1.0f / 4096.0f);
		float trx = (float)snap->trans[0];
		float trY = (float)snap->trans[1];
		float trz = (float)snap->trans[2];
		float ofxOverH = ofx / h;
		float ofyOverH = ofy / h;

		qx[0] = r00 + (ofxOverH * r20);
		qx[1] = r01 + (ofxOverH * r21);
		qx[2] = r02 + (ofxOverH * r22);
		qx[3] = trx + (ofxOverH * trz);

		qy[0] = r10 + (ofyOverH * r20);
		qy[1] = r11 + (ofyOverH * r21);
		qy[2] = r12 + (ofyOverH * r22);
		qy[3] = trY + (ofyOverH * trz);

		qw[0] = r20 / h;
		qw[1] = r21 / h;
		qw[2] = r22 / h;
		qw[3] = trz / h;
	}

	N3DS_TrackDraw_Begin(qx, qy, qw, h);

	/*
	 * DIAGNOSTIC ONLY (uncommitted rung 3a occlusion investigation, no
	 * behavior change below this comment through the loop's end): running
	 * min/max of the ordinal and OT-bucket-index values this frame's leaves
	 * actually got back from NativeGpu_GetOtBucketOrdinal, plus a one-leaf
	 * detail snapshot (the first leaf drawn, matching [TRKDRAW]'s existing
	 * "first" convention), all folded into the periodic log below. Ordinal
	 * min/max is taken over {ordinalLo, ordinalLo+ordinalSpan} per leaf, NOT
	 * assumed ordinalLo <= that sum -- ordinalSpan can be negative (the OT
	 * walk's own near/far convention makes a NEAR otIndex map to a HIGHER
	 * ordinal than a FAR one; see docs/NATIVE_TRACK_RENDERER_DESIGN.md 4.3
	 * and this task's own writeup), so treating outOrdinalLo as always the
	 * smaller end would silently mis-aggregate.
	 */
	int dbgAggValid = 0;
	float dbgOrdMin = 0.0f, dbgOrdMax = 0.0f;
	float dbgOtIdxMin = 0.0f, dbgOtIdxMax = 0.0f;
	u32 dbgWalkOtCount = 0;
	int dbgLeaf0Captured = 0;
	float dbgLeaf0OtIndexLo = 0.0f, dbgLeaf0OtIndexHi = 0.0f;
	float dbgLeaf0OrdinalLo = 0.0f, dbgLeaf0OrdinalHi = 0.0f;
	float dbgLeaf0ViewZLo = 0.0f, dbgLeaf0ViewZHi = 0.0f;

	/*
	 * TWO PASSES, opaque then blended (docs/TRANSPARENCY_DESIGN.md 4).
	 *
	 * pass 0 draws every range whose bin is opaque, with depth writes on, exactly
	 * as this loop always has. pass 1 draws the semi-transparent ranges, with
	 * blending on and depth writes off (both asserted inside N3DS_TrackDraw_Leaf,
	 * which is where the GPU state lives).
	 *
	 * The split has to be global, not per leaf: a blended surface must be
	 * composited over everything opaque BEHIND it, and "behind" spans leaves. Two
	 * passes over the visible set is the whole ordering mechanism -- there is no
	 * sort, because with depth writes off and the test on, the depth buffer built
	 * by pass 0 already resolves blended-vs-opaque correctly, and blended-vs-
	 * blended resolves in walk order, which is retail's own back-to-front order
	 * for the render-list slots this content lives in.
	 *
	 * Cost of the second pass over leaves that have nothing blended: one accessor
	 * call and a compare. The per-leaf depth-range math -- the expensive part --
	 * is skipped by the `continue` below before it runs. Measured content share is
	 * ~4% of vertices, so pass 1 issues a handful of draws.
	 */
	for (int pass = 0; pass < 2; pass++)
	{
	for (u32 i = 0; i < snap->leafCount; i++)
	{
		unsigned bspIndex, firstIndex, indexCount;
		s16 boxMin[3], boxMax[3];
		s8 drawOrderMin, drawOrderMax;
		float otIndexLo, ordinalLo, ordinalSpan, invIndexSpan;
		float otIndexHi, ordinalHi, viewZLo, viewZHi; /* diagnostic-only outputs */
		u32 walkOtCount;                              /* diagnostic-only output */
		unsigned leafBlend = NativeTrackMesh_LeafBlend(snap->leafSlot[i]);
		int leafIsBlended = (leafBlend != 3);
		int uvDx = 0, uvDy = 0;
		int drawBlend;

		/*
		 * A semi-transparent range is drawn TWICE, because that is what PS1
		 * textured semi-transparency IS: only the texels whose sampled colour has
		 * the STP bit set are blended, and the rest stay opaque. Pass 0 draws its
		 * PASS1 half (the opaque body, from the PRIMARY placement, so uvOffset
		 * stays 0 and blending stays off); pass 1 draws its PASS2 half (the
		 * translucent texels, from the second placement, blended).
		 *
		 * So pass 0 draws EVERY range and only pass 1 filters. The first cut of
		 * this skipped blended ranges in pass 0 -- which uploaded a PASS1 view at
		 * level load and then never sampled it, leaving Crash Cove's waterfalls
		 * showing their translucent texels with the solid part of the sheet
		 * missing. It reads as "transparent" at a glance, which is exactly why it
		 * needed the log rather than an eyeball to catch. The same mistake was
		 * made and caught independently in the model path
		 * (NativeModelDraw_IssueForWalk); the note there is the same note.
		 */
		if ((pass == 1) && !leafIsBlended)
		{
			continue;
		}

		if (leafIsBlended && (pass == 1))
		{
			NativeTrackMesh_LeafUvOffset(snap->leafSlot[i], &uvDx, &uvDy);
			drawBlend = (int)leafBlend;
		}
		else
		{
			drawBlend = 3; /* opaque: pass 0 for everything, blended or not */
		}

		if (!NativeTrackMesh_LeafAt(snap->leafSlot[i], &bspIndex, &firstIndex, &indexCount))
		{
			continue;
		}
		if (!NativeTrackMesh_LeafBox(snap->leafSlot[i], boxMin, boxMax))
		{
			continue;
		}
		/* FIX 1a: the leaf's own drawOrder range, tracked at build time
		 * (native_track_mesh.c) alongside boxMin/boxMax -- see
		 * NativeTrackDraw_ComputeLeafDepthRange's header comment for why
		 * this must widen the depth range rather than being omitted. */
		if (!NativeTrackMesh_LeafDrawOrderRange(snap->leafSlot[i], &drawOrderMin, &drawOrderMax))
		{
			continue;
		}

		if (!NativeTrackDraw_ComputeLeafDepthRange(snap, boxMin, boxMax, drawOrderMin, drawOrderMax, &otIndexLo, &ordinalLo, &ordinalSpan, &invIndexSpan,
		                                            &otIndexHi, &ordinalHi, &viewZLo, &viewZHi, &walkOtCount))
		{
			leavesSkippedOrdinal++;
			continue;
		}

		/* DIAGNOSTIC ONLY: aggregate + first-leaf capture, described above.
		 * Runs every leaf, every frame the toggle is on (not just logged
		 * windows) so the periodic line below always reflects the frame it
		 * fires on rather than whatever frame last happened to log. */
		{
			float leafOrdMin = (ordinalLo < ordinalHi) ? ordinalLo : ordinalHi;
			float leafOrdMax = (ordinalLo > ordinalHi) ? ordinalLo : ordinalHi;

			if (!dbgAggValid)
			{
				dbgAggValid = 1;
				dbgOrdMin = leafOrdMin;
				dbgOrdMax = leafOrdMax;
				dbgOtIdxMin = otIndexLo; /* otIndexHi >= otIndexLo always, see that function's comment */
				dbgOtIdxMax = otIndexHi;
			}
			else
			{
				if (leafOrdMin < dbgOrdMin) { dbgOrdMin = leafOrdMin; }
				if (leafOrdMax > dbgOrdMax) { dbgOrdMax = leafOrdMax; }
				if (otIndexLo < dbgOtIdxMin) { dbgOtIdxMin = otIndexLo; }
				if (otIndexHi > dbgOtIdxMax) { dbgOtIdxMax = otIndexHi; }
			}
			dbgWalkOtCount = walkOtCount;

			if (!dbgLeaf0Captured)
			{
				dbgLeaf0Captured = 1;
				dbgLeaf0OtIndexLo = otIndexLo;
				dbgLeaf0OtIndexHi = otIndexHi;
				dbgLeaf0OrdinalLo = ordinalLo;
				dbgLeaf0OrdinalHi = ordinalHi;
				dbgLeaf0ViewZLo = viewZLo;
				dbgLeaf0ViewZHi = viewZHi;
			}
		}

		N3DS_TrackDraw_Leaf((int)firstIndex, (int)indexCount, otIndexLo, ordinalLo, ordinalSpan, invIndexSpan,
		                    (int)NativeTrackMesh_LeafAtlasBin(snap->leafSlot[i], pass == 1), drawBlend, uvDx, uvDy);
		drawsIssued++;
		indicesDrawn += indexCount;

		if (leafIsBlended && (pass == 1))
		{
			blendDrawsIssued++;
		}
	}
	}

	N3DS_TrackDraw_End();

	s_totalSkippedOrdinal += leavesSkippedOrdinal;

	if (--s_logCountdown == 0)
	{
		s_logCountdown = 512;
		/* blendDraws= is how many of this frame's draws were the semi-transparent
		 * pass. Zero on a track with no blended content is correct and expected;
		 * zero on Crash Cove or N. Gin Labs means the feature is not reaching the
		 * screen, and separates "the blend pass never ran" from "it ran and looks
		 * wrong" without a hardware capture. */
		Platform_Log("[TRKDRAW] toggle=%d draws=%u blendDraws=%u leaves=%u skippedOrdinal=%u skippedOrdinalTotal=%u idx=%u degraded=%d\n",
		             g_dbg_trackRender, drawsIssued, blendDrawsIssued, snap->leafCount, leavesSkippedOrdinal, s_totalSkippedOrdinal, indicesDrawn,
		             snap->degraded);

	}
}
#endif /* __3DS__ */

#endif /* CTR_NATIVE_TRACK_RENDERER */
