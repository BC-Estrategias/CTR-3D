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
int g_dbg_trackRender = 0;

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
	u32 drawsIssued = 0, leavesSkippedOrdinal = 0, indicesDrawn = 0;
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

	for (u32 i = 0; i < snap->leafCount; i++)
	{
		unsigned bspIndex, firstIndex, indexCount;
		s16 boxMin[3], boxMax[3];
		s8 drawOrderMin, drawOrderMax;
		float otIndexLo, ordinalLo, ordinalSpan, invIndexSpan;
		float otIndexHi, ordinalHi, viewZLo, viewZHi; /* diagnostic-only outputs */
		u32 walkOtCount;                              /* diagnostic-only output */

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
		                    (int)NativeTrackMesh_LeafAtlasBin(snap->leafSlot[i]));
		drawsIssued++;
		indicesDrawn += indexCount;
	}

	N3DS_TrackDraw_End();

	s_totalSkippedOrdinal += leavesSkippedOrdinal;

	if (--s_logCountdown == 0)
	{
		s_logCountdown = 512;
		Platform_Log("[TRKDRAW] toggle=%d draws=%u leaves=%u skippedOrdinal=%u skippedOrdinalTotal=%u idx=%u degraded=%d\n", g_dbg_trackRender, drawsIssued,
		             snap->leafCount, leavesSkippedOrdinal, s_totalSkippedOrdinal, indicesDrawn, snap->degraded);

	}
}
#endif /* __3DS__ */

#endif /* CTR_NATIVE_TRACK_RENDERER */
