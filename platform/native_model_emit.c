/*
 * native_model_emit.c -- per-frame model triangle capture (s32 rung 1).
 *
 * docs/NATIVE_MODEL_RENDERER_DESIGN.md. The header states where the capture
 * hooks and why; this file is the buffer and its ownership rules.
 *
 * WHAT THIS PRODUCES
 *
 * A double-buffered snapshot of one frame's model geometry in MODEL SPACE,
 * plus one composed camera record per instance. That is the whole input a PICA
 * vertex shader needs: rung 2 binds a program, uploads an instance's rows as
 * uniforms, and draws its triangle range. Nothing in the snapshot is a live
 * pointer into game state, so the render worker can read it exactly the way it
 * already reads NativeTrackFrame's snapshot.
 *
 * WHAT IT DOES NOT DO
 *
 * It does not draw, does not change game state, and does not remove any work
 * from the packet path. At this rung the packet path still runs in full and the
 * capture rides along beside it. That is the point: the numbers it reports can
 * be checked against a renderer that is known to be correct, on PC, before any
 * shader exists. This project's own history is the argument -- s28's per-leaf
 * ordinal lerp and s29's near-plane reject both shipped before anyone could
 * check them against something that already worked.
 */

#include "platform/native_model_emit.h"

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_MODEL_RENDERER) && CTR_NATIVE_MODEL_RENDERER

/* 0 unless CTR_NATIVE_RENDERER_DEFAULT_ON is configured -- see that option's
 * comment in CMakeLists.txt for why the boot default lives there and not here.
 * The runtime toggle is unaffected either way. */
#ifndef CTR_NATIVE_RENDERER_DEFAULT_ON
#define CTR_NATIVE_RENDERER_DEFAULT_ON 0
#endif

int g_dbg_modelRender = CTR_NATIVE_RENDERER_DEFAULT_ON;

/*
 * CAPS. Measured (s30 [MODELFRAME], hardware): ~1500 triangles/frame across
 * ~9 instances in Arcade on N. Gin Labs, ~440 across ~5 in Time Trial. Note
 * that figure is the TOPOLOGY MAXIMUM of the headers the frame selected, and
 * what lands here is the post-rejection subset, so 1500 is an upper bound on
 * the real capture rate, not a typical one.
 *
 * 4096 triangles is ~2.7x the measured worst case. Instances: N. Gin Labs
 * carries 26 in Arcade and 75 in Time Trial as level INSTANCE LISTS, of which
 * far fewer are queued per frame; 128 covers the list itself with room over.
 *
 * Overflow is reported, never silent. A truncated capture that reads as
 * complete is how this project said "the retained-models campaign is dead" out
 * loud once already -- and at rung 2 an overflow would show up as models
 * flickering out of existence, which is exactly the kind of symptom that gets
 * blamed on the shader.
 */
#define MODELEMIT_MAX_TRIS      4096
#define MODELEMIT_MAX_VERTS     (MODELEMIT_MAX_TRIS * 3)
#define MODELEMIT_MAX_INSTANCES 128

/* Matches [DEMIT]/[MODELFRAME] cadence -- one line per 64 content frames. */
#define MODELEMIT_WINDOW_FRAMES 64

/*
 * 16 bytes. Not yet the GPU-side layout: rung 2 mirrors this into an
 * N3DS_ModelVertex across the plain-scalar boundary the same way
 * N3DS_TrackVertex mirrors NativeTrackVert, because the GPU form has to satisfy
 * PICA200 attribute-alignment rules that have nothing to do with what is
 * convenient here (see n3ds_backend.h's N3DS_Vertex comment for why that
 * matters on hardware and is invisible in Azahar).
 *
 * Position is the GTE's own model-space input, straight from the register pair
 * the projection was about to consume -- see the header on why it is taken from
 * there rather than re-derived.
 */
struct NativeModelEmitVert
{
	s16 x, y, z;
	/* Page-space texel coordinates, 0..255, exactly as the TextureLayout
	 * stores them. NOT atlas coordinates: which atlas cell this face resolves
	 * to is a render-side decision that changes frame to frame as the decode
	 * cache evicts, so resolving here would bake a stale answer into a
	 * snapshot the walk reads later. The track renderer can pre-bake its UVs
	 * because its atlas is packed once per level; this cannot. */
	s16 u, v;
	u8 r, g, b;
	u8 _pad;
};

/* 8 bytes, one per triangle. tpage/clut are the pair that costs a state change
 * -- the same batching key the packet path's resolver takes. */
struct NativeModelEmitFace
{
	u16 tpage;
	u16 clut;
	u8 textured;
	u8 _pad[3];
};

/*
 * One instance's camera record. RAW retail values, no pre-divide -- same
 * convention as struct NativeTrackFrameSnapshot, and for the same reason: the
 * draw side composes the projection rows from these, so there is exactly one
 * copy of that derivation to keep correct.
 *
 * rot/trans are idpp->mvp, the instance's model->VIEW matrix. h/ofx/ofy are the
 * camera terms, identical for every instance sharing a PushBuffer but recorded
 * per instance so a split-screen frame needs no extra bookkeeping.
 */
struct NativeModelEmitInstance
{
	u32 firstTri;
	u32 triCount;
	s16 rot[3][3];
	s32 trans[3];
	s32 h;
	s32 ofx, ofy;
	/*
	 * This instance's primitive WRITER forces semi-transparency regardless of
	 * what its textures say.
	 *
	 * Found by N. Tropy's ghost coming out solid. Classifying by the texture
	 * page's ABR field is correct for the normal writer -- RenderBucket's own
	 * selection (`(texWord1 & 0x00600000) == 0x00600000 ? 0x34 : 0x36`,
	 * QueueExecute:2928/3534) is the same rule and the same polarity as the
	 * track's -- but models have SEVEN primitive writers, and two of them never
	 * consult the texture at all: RB_RETAIL_INST_PRIM_GHOST and
	 * RB_RETAIL_INST_PRIM_DEPTH_FADE write the semi-transparent code 0x36
	 * unconditionally (:3141/:3365 and their split twins :3683/:3770).
	 *
	 * So this is not "which texture", it is "which writer", and the writer is an
	 * instance property (`inst->funcPtr[1]`, the DispatchDrawInstPrim switch at
	 * :3419) -- hence a per-instance flag rather than anything per face. The
	 * game side computes the boolean so the RB_RETAIL_INST_PRIM_* constants stay
	 * in game/ where they belong.
	 */
	u8 forceSemiTrans;

	/*
	 * Depth-axis regime of this instance's mvp, the s33 sink fix.
	 *
	 * Retail's instance pipeline is regime-scaled: RenderBucket_
	 * AdjustViewPositionForMvp shifts the MVP translation <<2 when the
	 * instance origin is nearer than 0x1000 view units ("to match the
	 * full-scale model matrix"; RenderBucket_BuildM3x3's depthShift applies
	 * the matching scale to the rotation), and DRAW_HUGE shifts >>2. So the
	 * SZ axis this capture inherits is view.z TIMES 4 for near instances --
	 * measured, not inferred: [AXISCHK] reads ratio 4.0000 exactly below
	 * 4096 and 1.0000 exactly above, hundreds of samples per window. Retail
	 * never notices because instances are OT-placed by the queue-phase depth
	 * bin (unshifted), never by per-prim SZ -- a painter's algorithm is blind
	 * to a class-wide monotone scale. A z-buffer is not: this is what buried
	 * every near kart under the road (the s32 sink).
	 *
	 * trueViewZ = mvpViewZ >> depthShift. 2 = near regime (mvp is 4x), 0 =
	 * far regime (mvp is true), -2 = far DRAW_HUGE (mvp is true/4). The draw
	 * side divides hParam.x by the factor, which corrects depth WITHOUT
	 * touching screen position -- the model shader uses hParam.x only to
	 * recover view.z; screen x/y come from the qx/qy dp4s and the divide.
	 */
	s8 depthShift;

	/*
	 * Instance fade inputs, the rung-3 colour gap (s33).
	 *
	 * The design doc's ladder calls this rung "fog", and it is not distance
	 * fog: RenderBucket_LoadPrimColors loads IR0 from idpp->alphaScale -- NOT
	 * from the projection's depth-cue -- and gte_dpct interpolates the three
	 * primitive colours toward the FAR COLOUR the instance's setup callback
	 * put in RFC/GFC/BFC. It is the per-INSTANCE fade/tint mechanism (spawn
	 * fade-ins, fade-outs, tinted sections), constant across the instance's
	 * primitives. Captured colours are pre-dpct, so without a replay the
	 * native draw shows a fading instance at full brightness.
	 *
	 * Both inputs are captured at BeginInstance time, which runs after
	 * RenderBucket_RunInstanceSetupCallback in the dispatch order -- so
	 * whatever the callback loaded is what we read, with no re-derivation of
	 * the callback zoo. alphaScale == 0 means the writer skips dpct entirely
	 * (its own gate), so 0 doubles as this struct's "no fade" state.
	 *
	 * Residual, deliberate: the writer's second gate bit (the OT-entry
	 * address test in RenderBucket_OTEntryPassesDpctGate) is per prim and per
	 * depth bin, and the capture runs before the OT entry exists. That test
	 * exists to reject sentinel OT addresses, which real drawn content never
	 * is, so the replay treats it as always-pass. The ghost and depth-fade
	 * WRITERS (gte_dpcs per prim) are a different mechanism and are NOT
	 * replayed here -- their content is forceSemiTrans, bounded, and listed
	 * as a known gap in the design doc.
	 */
	s16 alphaScale;
	s32 fcR, fcG, fcB;
};

struct NativeModelEmitSnapshot
{
	struct NativeModelEmitVert verts[MODELEMIT_MAX_VERTS];
	struct NativeModelEmitFace faces[MODELEMIT_MAX_TRIS];
	struct NativeModelEmitInstance insts[MODELEMIT_MAX_INSTANCES];

	u32 triCount;
	u32 instCount;

	/* Textured subset of triCount. Per-frame rather than accumulated straight
	 * into the window, so a toggle flipped off mid-frame cannot leave the
	 * window's textured total counting triangles from a frame the window then
	 * refuses to fold in. */
	u32 texTris;

	u32 droppedTris;
	u32 droppedInstances;

	/* Same contract as NativeTrackFrameSnapshot: `degraded` means the capture
	 * was truncated and a consumer must draw nothing rather than a partial
	 * frame; `valid` means a capture actually ran since the last Clear. */
	int degraded;
	int valid;
	u32 frameSeq;
};

global_variable struct NativeModelEmitSnapshot s_modelEmitBuf[2];
global_variable int s_modelEmitWriteIdx;
global_variable int s_modelEmitReadIdx;
global_variable u32 s_modelEmitSeq;

/* Open instance, game thread only. -1 = none open. */
global_variable int s_modelEmitCurInst = -1;

/* Session-lifetime totals, and a one-shot warning the first time anything is
 * dropped -- a periodic line lands on 1 frame in 64 and would miss the other
 * 63, the same gap NativeTrackFrame_Record's drop accounting exists to close. */
global_variable u32 s_modelEmitDroppedTrisTotal;
global_variable u32 s_modelEmitDroppedInstTotal;
global_variable int s_modelEmitDropWarned;

/*
 * ---------------------------------------------------------------- [MDLDEPTH]
 *
 * Depth self-check. Answers one question and nothing else: is the view.z this
 * path hands the shader the SAME number retail's GTE computed for that exact
 * vertex?
 *
 * It can be exact rather than approximate because of where the capture sits.
 * NativeModelEmit_Triangle runs immediately after RenderBucket_ProjectPrim_
 * Normal, so SZ1/SZ2/SZ3 still hold the hardware's projected depth for
 * tempPacked[1]/[2]/[3] -- the very three vertices being captured. RTPT writes
 * all three; RTPS writes SZ3 and RenderBucket_LoadPrimRTPS explicitly re-seeds
 * SZ1/SZ2 for the reuse case (:2678), so the correspondence holds on both
 * paths. That makes this a comparison against ground truth, not against a
 * second derivation of the same guess.
 *
 * Why it exists: the player kart sinks into the road under dmodel, and the
 * model path is now the only thing drawing it (state 1), computing depth from
 * its own instance matrix with no lookup table anywhere near it. Either that
 * computation disagrees with the GTE -- in which case the fault is here and
 * this line says so with a number -- or it agrees, and the disagreement is on
 * the ROAD's side. s29b already ruled a height offset out by reading; this
 * rules the model side in or out by measuring.
 *
 * Comparison is done in integer, in the GTE's own shape: MAC3 = TRZ*4096 +
 * R2.v, SZ3 = clamp(MAC3 >> 12, 0, 65535) (native_gte_core.c's Lm_D). The
 * shader does the same arithmetic in float and skips the floor, so a residual
 * of <1 unit is the expected agreement, not a defect -- err>1 is the signal.
 */
/* Not `global_variable` (= static): --model-depth-check sets this from
 * native_platform.c, which precedes this file in the unity TU. */
int g_dbg_modelDepthCheck;

/* The s33 sink fix (see NativeModelEmitInstance.depthShift). Not static: the
 * 3DS dumpreq handler ('axisfix', n3ds_backend.c -- a separate TU) flips it
 * for the hardware A/B. Default ON because OFF is the sink. */
int g_dbg_modelAxisFix = 1;

/* Rung 3: replay the normal writer's gte_dpct on captured colours (see
 * NativeModelEmitInstance.alphaScale). Same cross-TU story as axisfix;
 * dumpreq 'modelfade'. Default ON: OFF shows fading instances at full
 * brightness on the native path. */
int g_dbg_modelFade = 1;

/*
 * One DPCS component, mirroring native_gte_core.c's case 0x2a EXACTLY
 * (gte_dpct is op 0x0F8002A: sf=1 so A() is >>12, lm=0 so Lm_B clamps to
 * full s16). Kept as integer arithmetic, not float: the writer's packet
 * colours come from the real GTE core, and the state-2 overlay is only an
 * A/B if the two paths round identically.
 *
 *   t   = Lm_B(((FC << 12) - (C << 16)) >> 12)      clamp -32768..32767
 *   mac = ((C << 16) + IR0 * t) >> 12
 *   out = Lm_C(mac >> 4)                            clamp 0..255
 */
internal u8 NativeModelEmit_FadeComponent(int col, s32 farCol, int ir0)
{
	s64 t64 = (((s64)farCol << 12) - ((s64)col << 16)) >> 12;
	s32 t = (t64 > 32767) ? 32767 : (t64 < -32768) ? -32768 : (s32)t64;
	s64 mac = (((s64)col << 16) + ((s64)ir0 * t)) >> 12;
	s32 out = (s32)(mac >> 4);

	if (out < 0)
	{
		out = 0;
	}
	if (out > 255)
	{
		out = 255;
	}
	return (u8)out;
}
global_variable u32 s_modelDepthSamples;

/*
 * ---------------------------------------------------------------- [AXISCHK]
 *
 * Cross-CLASS depth-axis check, and the one comparison no earlier instrument
 * ever made. [MDLDEPTH] proves our instance depth equals the GTE's SZ for the
 * instance's own projection; [TRKDEPTHCHK] proves the road's equals the GTE's
 * for the road's. Both pass at once even if the two GTE runs are in DIFFERENT
 * UNITS -- retail never compares them either (instances are OT-placed by the
 * queue-phase depth bin computed from the UNSHIFTED view position, never by
 * per-prim SZ), so a class-wide unit mismatch is invisible on PS1 by
 * construction and only surfaces under a real z-buffer (dmodel).
 *
 * Retail is explicit that the instance pipeline is regime-scaled:
 * RenderBucket_AdjustViewPositionForMvp shifts the MVP translation <<2 when
 * viewPos->vz < 0x1000 ("to match the full-scale model matrix"), and
 * RenderBucket_BuildM3x3's depthShift applies the matching >>2 to the model
 * scale in the far regime. So mvp.t[2] -- the axis every SZ this capture
 * records AND every packet stamp inherits -- is NOT self-evidently the axis
 * matrix_ViewProj puts the track on.
 *
 * This measures exactly that: the instance ORIGIN's view.z computed the
 * track's way (matrix_ViewProj rotation row 2 against the camera-relative
 * world position, integer, mirroring RenderBucket_GetViewPosition) against
 * mvp.t[2]. Ratio per distance band; a ratio of 1.0 everywhere kills the
 * theory, 4.0 (or a regime-split 4/1) names the sink's mechanism and its
 * un-scale is the fix.
 */
global_variable u32 s_axisChkN[4];        /* bands by trueZ: <1024, <4096, <16384, rest */
global_variable double s_axisChkSum[4];   /* sum of mvpZ/trueZ per band */
global_variable double s_axisChkMin[4];
global_variable double s_axisChkMax[4];
global_variable u32 s_axisChkHuge;        /* DRAW_HUGE instances seen (they carry their own >>2) */
global_variable u32 s_axisChkBehind;      /* trueZ < 128: too near/behind to ratio meaningfully */
global_variable s32 s_axisChkRawTrue[4];  /* first raw pairs of the window, for exact numbers */
global_variable s32 s_axisChkRawMvp[4];
global_variable u32 s_axisChkRawN;
global_variable u32 s_axisChkMaxResid;    /* worst |unscaled mvp - trueZ|, in true view.z units */

/* Classified regime for the instance BeginInstance is currently opening --
 * bridged into the snapshot slot at the field-store site because the slot is
 * only allocated after the capacity check. Game-thread only, like the rest of
 * the capture. */
global_variable s8 s_modelEmitPendingDepthShift;
global_variable u32 s_modelDepthOver1;
global_variable double s_modelDepthSumAbsErr;
global_variable s32 s_modelDepthMaxAbsErr;
global_variable s32 s_modelDepthPredLo;
global_variable s32 s_modelDepthPredHi;
global_variable s32 s_modelDepthActLo;
global_variable s32 s_modelDepthActHi;

/* Window accumulators for [MDLEMIT]. */
global_variable u32 s_modelEmitWinFrames;
global_variable u32 s_modelEmitWinTris;
global_variable u32 s_modelEmitWinInsts;
global_variable u32 s_modelEmitWinTextured;
global_variable u32 s_modelEmitWinPeakTris;
global_variable u32 s_modelEmitWinSemiTris; /* declined to the packet path -- see NativeModelEmit_Triangle */

/*
 * Is this triangle's texture semi-transparent on PS1?
 *
 * Retail's own test, not a heuristic. DrawLevelOvr1P_SelectRawPrimitiveCode
 * (game/226/226_00_DrawLevelOvr1P.c:2133, six call sites) picks the
 * semi-transparent primitive code when (uv1 & 0x00600000) != 0x00600000, and
 * uv1's high halfword IS the layout's tpage (:677) -- so the field being tested
 * is tpage bits 5-6, the PSX ABR (semi-transparency mode). ABR 3 is CTR's "this
 * page is opaque" sentinel; every other value marks the face semi-transparent
 * AND names the blend mode (0 = B/2+F/2, 1 = B+F, 2 = B-F).
 *
 * That rule is written for TRACK faces, and models are a different draw path --
 * but it is a property of the TEXTURE PAGE, not of the geometry that samples it,
 * and the tpage halfword reaching here comes out of the same TextureLayout
 * struct. The model writer's own semi-transparency comes from the primitive code
 * it selects rather than from this field, so treat this as a CONSERVATIVE
 * classifier: it is used only to decline triangles to the packet path, and the
 * cost of a false positive is one triangle drawn by retail instead of by us --
 * invisible in output, negligible in cost. A false NEGATIVE would be a solid
 * block where a plume should be, which is why the test errs toward declining.
 *
 * Untextured faces (tex == NULL) have no tpage and cannot be semi-transparent
 * by this rule; retail's untextured model primitives carry their own ABE bit,
 * which this capture never sees. See docs/TRANSPARENCY_DESIGN.md section 5 --
 * closing that gap is what turns this decline into a real blended draw.
 */
internal int NativeModelEmit_TexIsSemiTrans(const struct TextureLayout *tex)
{
	if (tex == NULL)
	{
		return 0;
	}

	return (((CTR_ReadU16LE(&tex->tpage) >> 5) & 3) != 3);
}

int NativeModelEmit_BeginInstance(const struct InstDrawPerPlayer *idpp, const struct PushBuffer *pb, int forceSemiTrans, const struct Instance *worldInst)
{
	struct NativeModelEmitSnapshot *dst = &s_modelEmitBuf[s_modelEmitWriteIdx];
	struct NativeModelEmitInstance *inst;
	int r, c;

	if ((g_dbg_modelRender == 0) || (idpp == NULL) || (pb == NULL))
	{
		return 0;
	}

	/*
	 * Depth-axis regime classification (see the depthShift field comment) --
	 * runs for every captured instance, because the draw side needs the
	 * shift to put this instance's depth on the track's axis.
	 *
	 * Self-calibrating rather than re-deriving retail's near/huge branches:
	 * compute the instance origin's TRUE view.z on the track's axis, then
	 * pick whichever of {4x, 1x, 1/4x} mvp.t[2] actually is. That measures
	 * what retail's regime logic DID for this instance instead of predicting
	 * what it should have done, so a rounding disagreement at the 0x1000
	 * boundary cannot misclassify -- the candidates are a factor of 4 apart
	 * and the residual is a handful of units.
	 *
	 * trueZ: matrix_ViewProj row 2 against the camera-relative world
	 * position, >>12 -- the same integer arithmetic RenderBucket_
	 * GetViewPosition runs through the GTE light matrix (MVMVA adds no
	 * translation; retail subtracts the camera position explicitly first,
	 * so this does too). SCREENSPACE_INSTANCE bypasses that transform in
	 * retail (its matrix.t IS the view position), so it does here too --
	 * the main-menu character is one of these, and it sinks like the rest.
	 */
	{
		s8 shift = 0;

		if (worldInst != NULL)
		{
			s64 trueZ;
			s64 mvpZ = (s64)idpp->mvp.t[2];

			if ((worldInst->flags & SCREENSPACE_INSTANCE) != 0)
			{
				trueZ = (s64)worldInst->matrix.t[2];
			}
			else
			{
				s64 dx = (s64)worldInst->matrix.t[0] - (s64)pb->pos.x;
				s64 dy = (s64)worldInst->matrix.t[1] - (s64)pb->pos.y;
				s64 dz = (s64)worldInst->matrix.t[2] - (s64)pb->pos.z;

				trueZ = ((s64)pb->matrix_ViewProj.m[2][0] * dx + (s64)pb->matrix_ViewProj.m[2][1] * dy + (s64)pb->matrix_ViewProj.m[2][2] * dz) >> 12;
			}

			if (trueZ > 0)
			{
				s64 eNear = mvpZ - (trueZ << 2); /* near regime: mvp = 4x true */
				s64 eFar = mvpZ - trueZ;         /* far regime: mvp = true */
				s64 eHuge = (mvpZ << 2) - trueZ; /* far DRAW_HUGE: mvp = true/4 */

				if (eNear < 0) { eNear = -eNear; }
				if (eFar < 0) { eFar = -eFar; }
				if (eHuge < 0) { eHuge = -eHuge; }

				shift = (eNear <= eFar) ? ((eNear <= eHuge) ? 2 : -2) : ((eFar <= eHuge) ? 0 : -2);
			}

			/* [AXISCHK] -- the measurement that found the regime scale. Kept
			 * as the fix's own verifier: resid is |unscaled mvp - trueZ| in
			 * true view.z units, so a nonzero window names a content class
			 * the classification above got wrong. */
			if (g_dbg_modelDepthCheck && (trueZ >= 128))
			{
				double ratio = (double)mvpZ / (double)trueZ;
				s64 unscaled = (shift == 2) ? (mvpZ >> 2) : (shift == -2) ? (mvpZ << 2) : mvpZ;
				s64 resid = unscaled - trueZ;
				int band = (trueZ < 1024) ? 0 : (trueZ < 4096) ? 1 : (trueZ < 16384) ? 2 : 3;

				if (resid < 0)
				{
					resid = -resid;
				}
				if ((u32)resid > s_axisChkMaxResid)
				{
					s_axisChkMaxResid = (u32)resid;
				}

				if (s_axisChkN[band] == 0)
				{
					s_axisChkMin[band] = ratio;
					s_axisChkMax[band] = ratio;
				}
				else
				{
					if (ratio < s_axisChkMin[band]) { s_axisChkMin[band] = ratio; }
					if (ratio > s_axisChkMax[band]) { s_axisChkMax[band] = ratio; }
				}
				s_axisChkN[band]++;
				s_axisChkSum[band] += ratio;

				if ((worldInst->flags & DRAW_HUGE) != 0)
				{
					s_axisChkHuge++;
				}

				if (s_axisChkRawN < 4)
				{
					s_axisChkRawTrue[s_axisChkRawN] = (s32)trueZ;
					s_axisChkRawMvp[s_axisChkRawN] = (s32)mvpZ;
					s_axisChkRawN++;
				}
			}
			else if (g_dbg_modelDepthCheck)
			{
				s_axisChkBehind++;
			}
		}

		s_modelEmitPendingDepthShift = shift;
	}

	if (dst->instCount >= MODELEMIT_MAX_INSTANCES)
	{
		dst->droppedInstances++;
		s_modelEmitDroppedInstTotal++;
		dst->degraded = 1;
		return 0;
	}

	inst = &dst->insts[dst->instCount];
	inst->firstTri = dst->triCount;
	inst->triCount = 0;
	inst->forceSemiTrans = (u8)(forceSemiTrans ? 1 : 0);
	inst->depthShift = s_modelEmitPendingDepthShift;

	/* Fade inputs (see the field comment). BeginInstance runs after the
	 * instance setup callback in RenderBucket_DispatchDrawFunc's order, so
	 * the far-colour registers hold exactly what the writer's gte_dpct will
	 * read for this instance. CFC2 is the facade's own control-reg read --
	 * a read, not a GTE op; nothing here perturbs writer-visible state. */
	inst->alphaScale = idpp->alphaScale;
	inst->fcR = (s32)CFC2(21);
	inst->fcG = (s32)CFC2(22);
	inst->fcB = (s32)CFC2(23);

	for (r = 0; r < 3; r++)
	{
		for (c = 0; c < 3; c++)
		{
			inst->rot[r][c] = idpp->mvp.m[r][c];
		}
	}
	inst->trans[0] = idpp->mvp.t[0];
	inst->trans[1] = idpp->mvp.t[1];
	inst->trans[2] = idpp->mvp.t[2];

	/*
	 * Camera terms. h and the half-extents match NativeTrackFrame_Record's, but
	 * the VIEWPORT ORIGIN (rect.x/rect.y) is added here and is NOT optional.
	 *
	 * The GTE is handed OFX/OFY = half the viewport (RenderBucket_Prepare-
	 * DrawContext's gte_SetGeomOffset), so its screen coordinates are relative
	 * to the viewport's own top-left, and the packet path picks up the
	 * viewport's position on screen further downstream. This path projects
	 * straight to final screen coordinates, so if it does not add that origin
	 * itself the model is drawn relative to the SCREEN corner instead.
	 *
	 * Owner-found, character select (2026-08-03): Crash rendered up and to the
	 * left of his preview window, and the diagnosis -- "maybe we're treating
	 * the top-left of the screen as the origin when we should be treating the
	 * top-left of the blue box as the origin" -- was exactly right.
	 * MM_Characters.c:215 sets pb->rect.x/y to that window's position.
	 *
	 * Adding it to OFX/OFY is the whole fix, not an approximation: the shader's
	 * qx row folds OFX as `OFX * qw` (see ctr_model.v.pica's derivation), and
	 * a = w*screenX, so a constant added to OFX adds exactly that constant to
	 * the final screen X after the perspective divide.
	 *
	 * NOTE FOR THE TRACK RENDERER: platform/native_track_frame.c has the same
	 * omission. It has never shown because a 1P race viewport sits at the
	 * screen origin -- but a 2P split-screen lower viewport does not, so this
	 * is a live latent bug there, not a difference between the two paths.
	 */
	inst->h = pb->distanceToScreen_PREV;
	inst->ofx = (pb->rect.w >> 1) + pb->rect.x;
	inst->ofy = (pb->rect.h >> 1) + pb->rect.y;

	s_modelEmitCurInst = (int)dst->instCount;
	return 1;
}

/*
 * Unpack one GTE vertex register pair into model-space shorts.
 *
 * The packed form is what RenderBucket_PackModelVertexXY built and what
 * MTC2(.., 0/1) was about to load: VX in the low halfword of xy, VY in the
 * high, VZ in the low halfword of z. Reading them back as s16 is not a
 * reinterpretation -- those halfwords ARE the GTE's 16-bit V registers
 * (include/psx/gtereg.h: C2_VX0 is CP2D.p[0].sw.l, C2_VY0 its .sw.h, C2_VZ0
 * CP2D.p[1].sw.l), so this recovers exactly the three signed values the
 * hardware would have multiplied by the rotation matrix.
 */
internal void NativeModelEmit_UnpackVertex(u32 packedXY, u32 packedZ, s16 *outX, s16 *outY, s16 *outZ)
{
	*outX = (s16)(u16)(packedXY & 0xffffu);
	*outY = (s16)(u16)(packedXY >> 16);
	*outZ = (s16)(u16)(packedZ & 0xffffu);
}

/*
 * Returns 1 when the native path has CLAIMED this triangle, i.e. the caller may
 * skip its packet write. Returns 0 to mean "not ours -- draw it the retail way",
 * which is the safe answer and the one every early-out below gives.
 *
 * The return value exists because "captured" and "will be drawn" used to be
 * assumed identical and are not. Two cases where they diverge, both of which
 * silently DELETED geometry in native-only mode before this returned anything:
 *
 *   - the triangle cap. An overflowing snapshot dropped the triangle here while
 *     the caller, told nothing, also skipped the packet -- so a frame that
 *     exceeded MODELEMIT_MAX_TRIS lost those triangles from the screen entirely
 *     rather than degrading to the retail draw. Cap overflow is now a
 *     performance event, not a correctness one.
 *   - semi-transparency. The native draw path asserts opaque blend for the whole
 *     pass (N3DS_ModelDraw_Begin) and carries no per-run blend mode, so a
 *     claimed semi-transparent triangle would draw SOLID -- a filled rectangle
 *     where a particle, shadow or item glow belongs. Declining it hands it back
 *     to the packet path, which has done PS1 two-pass STP correctly since s17
 *     (native_gpu.c:2670).
 *
 * The second is a stopgap with a known end state, not the design: rung 3 should
 * carry ABR through to N3DS_ModelDraw_Run and blend properly. Models sample the
 * shared decode cache, which already materialises PASS1/PASS2 views on demand,
 * so that costs no new atlas pool -- see docs/TRANSPARENCY_DESIGN.md section 5.
 * Until then this keeps native-only mode HONEST: what it draws, it draws right,
 * and what it cannot draw right it does not draw at all.
 */
int NativeModelEmit_Triangle(const u32 *packedXY3, const u32 *packedZ3, const int *color3, const struct TextureLayout *tex)
{
	struct NativeModelEmitSnapshot *dst = &s_modelEmitBuf[s_modelEmitWriteIdx];
	struct NativeModelEmitFace *face;
	struct NativeModelEmitVert *v;
	int i;

	if (s_modelEmitCurInst < 0)
	{
		return 0;
	}

	/* Counted, not declined. An earlier cut of this handed semi-transparent
	 * triangles back to the packet path because the model draw could only draw
	 * opaque; the draw side now does real two-pass PS1 blending
	 * (NativeModelDraw_IssueForWalk), so the native path owns this content like
	 * any other. The counter stays because it sizes the blend pass. */
	if (NativeModelEmit_TexIsSemiTrans(tex))
	{
		s_modelEmitWinSemiTris++;
	}

	if (dst->triCount >= MODELEMIT_MAX_TRIS)
	{
		dst->droppedTris++;
		s_modelEmitDroppedTrisTotal++;
		dst->degraded = 1;

		if (!s_modelEmitDropWarned)
		{
			s_modelEmitDropWarned = 1;
			Platform_Log("[MDLEMIT] WARNING: first triangle-cap drop this session (cap=%d) -- snapshot marked degraded\n", MODELEMIT_MAX_TRIS);
		}
		return 0;
	}

	face = &dst->faces[dst->triCount];
	v = &dst->verts[dst->triCount * 3];

	/*
	 * UV corner i goes to vertex i, no strip-parity swap. That is not an
	 * assumption: RenderBucket_DrawInstPrim_NormalAtOTEntry writes
	 * TextureLayout words 0/1/2 into a POLY_GT3's u0/u1/u2 in that fixed order
	 * for every primitive, and the three positions it writes alongside them
	 * come from the same tempPacked[1..3] window this capture is handed. Model
	 * faces carry their UVs per FACE, not per vertex, so the layout entry is
	 * the whole story.
	 *
	 * Read by field name into a local rather than by stride arithmetic off
	 * &tex->u0. The three pairs do sit 4 bytes apart today, but that spacing is
	 * a consequence of the interleaved clut/tpage halfwords, and retail's own
	 * code computes it with offsetof (RENDER_BUCKET_TEX_WORD1_OFFSET) instead
	 * of writing 4 -- so neither does this.
	 */
	{
		u8 uv[3][2];

		if (tex != NULL)
		{
			uv[0][0] = tex->u0;
			uv[0][1] = tex->v0;
			uv[1][0] = tex->u1;
			uv[1][1] = tex->v1;
			uv[2][0] = tex->u2;
			uv[2][1] = tex->v2;

			face->tpage = tex->tpage;
			face->clut = tex->clut;
			face->textured = 1;
		}
		else
		{
			memset(uv, 0, sizeof(uv));

			face->tpage = 0;
			face->clut = 0;
			face->textured = 0;
		}
		face->_pad[0] = 0;
		face->_pad[1] = 0;
		face->_pad[2] = 0;

		const struct NativeModelEmitInstance *mi = &dst->insts[s_modelEmitCurInst];

		for (i = 0; i < 3; i++)
		{
			NativeModelEmit_UnpackVertex(packedXY3[i], packedZ3[i], &v[i].x, &v[i].y, &v[i].z);

			v[i].u = (s16)uv[i][0];
			v[i].v = (s16)uv[i][1];

			/*
			 * Colour is the rolling per-vertex value, which is what the packet
			 * writer feeds gte_dpct -- so this is the colour BEFORE the depth
			 * cue, even though the capture site runs after the writer (dpct
			 * leaves its result in GTE registers and never touches
			 * ctx->tempColor).
			 *
			 * Rung 3 (s33): the dpct is now replayed here in C, exactly as the
			 * rung-1 comment predicted it could be -- IR0 is idpp->alphaScale
			 * and FC is whatever the instance's setup callback loaded, both
			 * captured at BeginInstance. The stored colour is therefore
			 * POST-fade, matching the packet the writer is about to build, and
			 * the draw side needs no new state. alphaScale == 0 mirrors the
			 * writer's own skip gate.
			 */
			v[i].r = (u8)(u32)(color3[i] & 0xff);
			v[i].g = (u8)(((u32)color3[i] >> 8) & 0xff);
			v[i].b = (u8)(((u32)color3[i] >> 16) & 0xff);
			v[i]._pad = 0;

			/* forceSemiTrans excluded: the GHOST and DEPTH_FADE writers never
			 * run LoadPrimColors' dpct -- they do their own per-prim gte_dpcs
			 * -- so replaying the NORMAL writer's fade on their content is
			 * applying a colour transform retail never applies. Shipping
			 * without this test made TT ghosts nearly invisible and put an
			 * unintended tint on Hot Air Skyway's depth-fade barriers
			 * (owner-reported, s33). Their own dpcs replay is the known gap
			 * recorded in the field comment above; until it lands, their
			 * captured colours stay pre-fade, which is yesterday's behaviour. */
			if (g_dbg_modelFade && (mi->alphaScale != 0) && !mi->forceSemiTrans)
			{
				v[i].r = NativeModelEmit_FadeComponent(v[i].r, mi->fcR, mi->alphaScale);
				v[i].g = NativeModelEmit_FadeComponent(v[i].g, mi->fcG, mi->alphaScale);
				v[i].b = NativeModelEmit_FadeComponent(v[i].b, mi->fcB, mi->alphaScale);
			}
		}
	}

	/* [MDLDEPTH] -- see the accumulator block's comment. Reads the GTE regs
	 * the projection just wrote; must stay inside this function, because it is
	 * the only place where "the vertex we captured" and "the SZ the hardware
	 * produced for it" are both in hand. */
	if (g_dbg_modelDepthCheck)
	{
		const struct NativeModelEmitInstance *mi = &dst->insts[s_modelEmitCurInst];

		for (i = 0; i < 3; i++)
		{
			/* MAC3 = TRZ*4096 + R20*vx + R21*vy + R22*vz, then >>12 and the
			 * unsigned 16-bit clamp -- native_gte_core.c's RTPS, in integer. */
			s64 mac3 = ((s64)mi->trans[2] * 4096) + ((s64)mi->rot[2][0] * v[i].x) + ((s64)mi->rot[2][1] * v[i].y) + ((s64)mi->rot[2][2] * v[i].z);
			s32 pred = (s32)(mac3 >> 12);
			s32 act = (s32)(u32)MFC2(17 + i); /* SZ1, SZ2, SZ3 */
			s32 err;

			if (pred < 0) { pred = 0; }
			if (pred > 65535) { pred = 65535; }

			err = pred - act;
			if (err < 0) { err = -err; }

			if (s_modelDepthSamples == 0)
			{
				s_modelDepthPredLo = pred;
				s_modelDepthPredHi = pred;
				s_modelDepthActLo = act;
				s_modelDepthActHi = act;
			}
			else
			{
				if (pred < s_modelDepthPredLo) { s_modelDepthPredLo = pred; }
				if (pred > s_modelDepthPredHi) { s_modelDepthPredHi = pred; }
				if (act < s_modelDepthActLo) { s_modelDepthActLo = act; }
				if (act > s_modelDepthActHi) { s_modelDepthActHi = act; }
			}

			s_modelDepthSamples++;
			s_modelDepthSumAbsErr += (double)err;
			if (err > s_modelDepthMaxAbsErr) { s_modelDepthMaxAbsErr = err; }
			if (err > 1) { s_modelDepthOver1++; }
		}
	}

	dst->triCount++;
	dst->texTris += (u32)face->textured;
	dst->insts[s_modelEmitCurInst].triCount++;
	return 1;
}

void NativeModelEmit_EndInstance(void)
{
	struct NativeModelEmitSnapshot *dst = &s_modelEmitBuf[s_modelEmitWriteIdx];

	if (s_modelEmitCurInst < 0)
	{
		return;
	}

	/*
	 * An instance that captured nothing is dropped rather than kept as an
	 * empty range. Every one costs a uniform upload and a bind at rung 2, and
	 * retail's own walk routinely rejects every triangle of an instance that
	 * is on screen but facing away -- keeping those would put empty draws in
	 * the frame that no counter would explain.
	 */
	if (dst->insts[s_modelEmitCurInst].triCount == 0)
	{
		s_modelEmitCurInst = -1;
		return;
	}

	dst->instCount++;
	s_modelEmitCurInst = -1;
}

/*
 * [FADESELFTEST] -- proves NativeModelEmit_FadeComponent mirrors the real GTE
 * core, by running the core. One shot, gated behind --model-depth-check, and
 * placed at EndFrame: the frame's writers are done with the GTE, and the next
 * frame reloads everything it reads before reading it. Every register this
 * touches (colour FIFO, IR0-3, far colour) is saved and restored anyway, so
 * even that ordering argument is belt-and-braces.
 *
 * Sweeps IR0 through the fade range including both clamp directions and FC
 * through per-channel-distinct values, so a channel swap or a missed clamp in
 * the C mirror cannot pass. mismatches=0 is the pass; any other number means
 * the replay diverges from what the writer's own dpct produces.
 */
internal void NativeModelEmit_FadeSelfTest(void)
{
	static const int irs[] = {1, 37, 128, 1024, 2048, 4095, 4096, 8191};
	static const int cols[] = {0, 1, 127, 200, 255};
	static const s32 fcs[] = {0, 16, 255, 1200, 4080};
	u32 savedRgb[3], savedIr[4];
	s32 savedFc[3];
	u32 n = 0, bad = 0;
	int i, c, f, k;

	savedRgb[0] = MFC2(20);
	savedRgb[1] = MFC2(21);
	savedRgb[2] = MFC2(22);
	for (k = 0; k < 4; k++)
	{
		savedIr[k] = MFC2(8 + k);
	}
	savedFc[0] = (s32)CFC2(21);
	savedFc[1] = (s32)CFC2(22);
	savedFc[2] = (s32)CFC2(23);

	for (i = 0; i < (int)(sizeof(irs) / sizeof(irs[0])); i++)
	{
		for (c = 0; c < (int)(sizeof(cols) / sizeof(cols[0])); c++)
		{
			for (f = 0; f < (int)(sizeof(fcs) / sizeof(fcs[0])); f++)
			{
				/* Distinct per channel: R gets cols[c]/fcs[f], G the next
				 * entry of each, B the one after -- wraps within the table. */
				const int cr = cols[c], cg = cols[(c + 1) % 5], cb = cols[(c + 2) % 5];
				const s32 fr = fcs[f], fg = fcs[(f + 1) % 5], fb = fcs[(f + 2) % 5];
				const u32 packed = (u32)cr | ((u32)cg << 8) | ((u32)cb << 16);
				u32 out;

				CTC2((u32)fr, 21);
				CTC2((u32)fg, 22);
				CTC2((u32)fb, 23);
				MTC2(packed, 20);
				MTC2(packed, 21);
				MTC2(packed, 22);
				MTC2((u32)irs[i], 8);
				doCOP2(0x0F8002A); /* gte_dpct, the writer's own op word */

				for (k = 0; k < 3; k++)
				{
					out = MFC2(20 + k);
					n++;
					if (((out & 0xff) != NativeModelEmit_FadeComponent(cr, fr, irs[i])) || (((out >> 8) & 0xff) != NativeModelEmit_FadeComponent(cg, fg, irs[i])) ||
					    (((out >> 16) & 0xff) != NativeModelEmit_FadeComponent(cb, fb, irs[i])))
					{
						bad++;
					}
				}
			}
		}
	}

	MTC2(savedRgb[0], 20);
	MTC2(savedRgb[1], 21);
	MTC2(savedRgb[2], 22);
	for (k = 0; k < 4; k++)
	{
		MTC2(savedIr[k], (s32)(8 + k));
	}
	CTC2((u32)savedFc[0], 21);
	CTC2((u32)savedFc[1], 22);
	CTC2((u32)savedFc[2], 23);

	Platform_LogInstrument("[FADESELFTEST] n=%lu mismatches=%lu\n", (unsigned long)n, (unsigned long)bad);
}

void NativeModelEmit_EndFrame(void)
{
	struct NativeModelEmitSnapshot *dst = &s_modelEmitBuf[s_modelEmitWriteIdx];

	/*
	 * Left open by a draw func that returned early (PrimMem exhaustion has a
	 * real `return` in the middle of DrawFunc_Normal). Close it here rather
	 * than leaking the open index into the next frame, where its triangles
	 * would be attributed to whichever instance opened next.
	 */
	if (s_modelEmitCurInst >= 0)
	{
		NativeModelEmit_EndInstance();
	}

	if (g_dbg_modelRender == 0)
	{
		return;
	}

	if (g_dbg_modelDepthCheck)
	{
		static int s_fadeSelfTestDone;

		if (!s_fadeSelfTestDone)
		{
			s_fadeSelfTestDone = 1;
			NativeModelEmit_FadeSelfTest();
		}
	}

	dst->frameSeq = ++s_modelEmitSeq;
	dst->valid = 1;

	/*
	 * Frames that captured nothing -- menus, load screens, the pause overlay --
	 * are dropped from the window rather than folded in. Averaging them drags
	 * every figure toward zero and makes a heavy race look cheap, the same
	 * shape of error as averaging a sweep across different stretches of track.
	 * Same rule native_model_frame.c already applies to [MODELFRAME].
	 */
	if (dst->triCount == 0)
	{
		return;
	}

	s_modelEmitWinFrames++;
	s_modelEmitWinTris += dst->triCount;
	s_modelEmitWinTextured += dst->texTris;
	s_modelEmitWinInsts += dst->instCount;
	if (dst->triCount > s_modelEmitWinPeakTris)
	{
		s_modelEmitWinPeakTris = dst->triCount;
	}

	if (s_modelEmitWinFrames < MODELEMIT_WINDOW_FRAMES)
	{
		return;
	}

	{
		u32 f = s_modelEmitWinFrames;

		/*
		 * peakTris is the number that decides MODELEMIT_MAX_TRIS, and it is
		 * reported next to the cap on purpose: a high-water mark quoted
		 * without the ceiling it is approaching is how a silent overflow gets
		 * read as headroom.
		 *
		 * texTris/f against tris/f says how much of a frame is the untextured
		 * G3 path, which decides whether rung 3 needs an untextured draw
		 * variant or can route everything through the atlas.
		 */
		/*
		 * semiTris/f is the population handed back to the packet path, and it is
		 * the number that says how much a real blended model draw is worth: it is
		 * both what native-only mode currently cannot draw AND what rung 3 would
		 * take over. Quoted per frame like its neighbours so it can be read
		 * against tris/f directly.
		 */
		Platform_LogInstrument("[MDLEMIT] semiTris/f=%lu\n", (unsigned long)(s_modelEmitWinSemiTris / (f ? f : 1)));
		s_modelEmitWinSemiTris = 0;

		Platform_LogInstrument("[MDLEMIT] lvl=%d f=%lu tris/f=%lu texTris/f=%lu inst/f=%lu peakTris=%lu/%d droppedTris=%lu droppedInst=%lu\n",
		                       NativePerf_CurrentLevelID(), (unsigned long)f, (unsigned long)(s_modelEmitWinTris / f),
		                       (unsigned long)(s_modelEmitWinTextured / f), (unsigned long)(s_modelEmitWinInsts / f), (unsigned long)s_modelEmitWinPeakTris,
		                       MODELEMIT_MAX_TRIS, (unsigned long)s_modelEmitDroppedTrisTotal, (unsigned long)s_modelEmitDroppedInstTotal);

		/* Emitted on the same window boundary so the two lines describe the
		 * same stretch of play. Ranges are printed alongside the error because
		 * "maxErr=0" over a sample that never left one depth is not a result --
		 * predLo/predHi have to show the content actually spanned some depth. */
		if (g_dbg_modelDepthCheck && (s_modelDepthSamples > 0))
		{
			Platform_LogInstrument("[MDLDEPTH] lvl=%d n=%lu maxErr=%ld meanErr=%.3f over1=%lu pred=%ld..%ld gte=%ld..%ld\n", NativePerf_CurrentLevelID(),
			                       (unsigned long)s_modelDepthSamples, (long)s_modelDepthMaxAbsErr, s_modelDepthSumAbsErr / (double)s_modelDepthSamples,
			                       (unsigned long)s_modelDepthOver1, (long)s_modelDepthPredLo, (long)s_modelDepthPredHi, (long)s_modelDepthActLo,
			                       (long)s_modelDepthActHi);

			s_modelDepthSamples = 0;
			s_modelDepthOver1 = 0;
			s_modelDepthSumAbsErr = 0.0;
			s_modelDepthMaxAbsErr = 0;
		}

		/* [AXISCHK] window flush -- same boundary as [MDLDEPTH] so the two
		 * lines describe the same stretch of play. One line per distance band
		 * with samples; ratio = mvp.t[2] / trueZ-on-the-track-axis. */
		if (g_dbg_modelDepthCheck)
		{
			static const char *bandName[4] = {"<1k", "1k-4k", "4k-16k", ">=16k"};
			int b;
			u32 total = s_axisChkN[0] + s_axisChkN[1] + s_axisChkN[2] + s_axisChkN[3];

			if (total > 0)
			{
				for (b = 0; b < 4; b++)
				{
					if (s_axisChkN[b] == 0)
					{
						continue;
					}
					Platform_LogInstrument("[AXISCHK] lvl=%d band=%s n=%lu ratio=%.4f (%.4f..%.4f)\n", NativePerf_CurrentLevelID(), bandName[b],
					                       (unsigned long)s_axisChkN[b], s_axisChkSum[b] / (double)s_axisChkN[b], s_axisChkMin[b], s_axisChkMax[b]);
					s_axisChkN[b] = 0;
					s_axisChkSum[b] = 0.0;
				}
				Platform_LogInstrument("[AXISCHK] lvl=%d huge=%lu behind=%lu maxResid=%lu raw: (%ld,%ld) (%ld,%ld) (%ld,%ld) (%ld,%ld)\n", NativePerf_CurrentLevelID(),
				                       (unsigned long)s_axisChkHuge, (unsigned long)s_axisChkBehind, (unsigned long)s_axisChkMaxResid, (long)s_axisChkRawTrue[0],
				                       (long)s_axisChkRawMvp[0], (long)s_axisChkRawTrue[1], (long)s_axisChkRawMvp[1], (long)s_axisChkRawTrue[2],
				                       (long)s_axisChkRawMvp[2], (long)s_axisChkRawTrue[3], (long)s_axisChkRawMvp[3]);
				s_axisChkHuge = 0;
				s_axisChkBehind = 0;
				s_axisChkMaxResid = 0;
				s_axisChkRawN = 0;
			}
		}
	}

	s_modelEmitWinFrames = 0;
	s_modelEmitWinTris = 0;
	s_modelEmitWinInsts = 0;
	s_modelEmitWinTextured = 0;
	s_modelEmitWinPeakTris = 0;
}

void NativeModelEmit_Clear(void)
{
	struct NativeModelEmitSnapshot *dst;

	/* Flip THEN invalidate the new write buffer -- the idiom GTECapture_Clear
	 * and NativeTrackFrame_Clear both use. Flipping first leaves the buffer
	 * being abandoned untouched for an in-flight walk that may still be
	 * reading it; the reset only ever touches the buffer this frame is about
	 * to write. */
	s_modelEmitWriteIdx ^= 1;
	dst = &s_modelEmitBuf[s_modelEmitWriteIdx];

	dst->triCount = 0;
	dst->instCount = 0;
	dst->texTris = 0;
	dst->droppedTris = 0;
	dst->droppedInstances = 0;
	dst->degraded = 0;
	dst->valid = 0;
	s_modelEmitCurInst = -1;
}

void NativeModelEmit_PublishForWalk(void)
{
	s_modelEmitReadIdx = s_modelEmitWriteIdx;
}

/*
 * Does the native model path REPLACE the packet draw for this instance?
 *
 * Owner-reported design defect, and it was a real one: rung 1 shipped with
 * states 1 and 2 both meaning "capture alongside", so the native models were
 * drawn ON TOP of the packet ones and there was no way to tell which path any
 * pixel came from. An A/B where both legs draw the same thing is not an A/B,
 * and every question this campaign exists to answer -- do karts still sink, is
 * the geometry right, is it faster -- was unanswerable by construction.
 *
 * The three states now mean what their names always claimed:
 *   0 = packets only (the baseline every measurement is against)
 *   1 = NATIVE ONLY -- the packet write is skipped, so what you see is ours
 *   2 = both, kept deliberately as the overlay diagnostic: two renderers
 *       disagreeing about a position show up as a visible double image in one
 *       frame instead of having to be inferred.
 *
 * The platform test lives here, not in game/, and it is the same guard the
 * track renderer needs for the same reason: the native model DRAW is 3DS-only,
 * so on PC state 1 would switch the packet path off with nothing to take its
 * place and models would simply vanish. That is exactly how the track
 * renderer's state 1 once left Oxide floating in space with no ship.
 */
int NativeModelEmit_SkipRetailDraw(void)
{
#if defined(__3DS__)
	return (g_dbg_modelRender == 1);
#else
	return 0;
#endif
}

#if defined(__3DS__)
/*
 * ===========================================================================
 * Rung 2: the draw side.
 * ===========================================================================
 *
 * Turns the snapshot above into real PICA draws, via the plain-scalar
 * N3DS_ModelDraw_* API. Called from NativeRenderer_EndScene, which on
 * render-thread builds runs on the core-2 WORKER -- so everything read here is
 * either a copy-by-value out of the double-buffered snapshot or renderer-owned
 * state, never a live gGT/PushBuffer/Instance pointer. Same boundary the track
 * draw already observes.
 *
 * This file sits after platform/n3ds/native_renderer_3ds.c and
 * platform/native_gpu.c in main.c's unity include order, so
 * N3DS_ResolveTriangleTexture and GetTPageFormat are ordinary in-TU calls here,
 * exactly as native_track_frame.c reaches s_runUntextured.
 */

/* Compose one instance's projection rows. Identical derivation to
 * NativeTrackDraw_IssueForWalk's, with idpp->mvp (model->view) in place of the
 * camera's world->view matrix -- see platform/n3ds/shaders/ctr_model.v.pica for
 * the algebra. Kept as its own function so the two copies of this maths can be
 * diffed against each other and against the shader comment. */
internal void NativeModelDraw_ComposeRows(const struct NativeModelEmitInstance *inst, float qx[4], float qy[4], float qw[4], float *outH)
{
	float r00 = (float)inst->rot[0][0] * (1.0f / 4096.0f);
	float r01 = (float)inst->rot[0][1] * (1.0f / 4096.0f);
	float r02 = (float)inst->rot[0][2] * (1.0f / 4096.0f);
	float r10 = (float)inst->rot[1][0] * (1.0f / 4096.0f);
	float r11 = (float)inst->rot[1][1] * (1.0f / 4096.0f);
	float r12 = (float)inst->rot[1][2] * (1.0f / 4096.0f);
	float r20 = (float)inst->rot[2][0] * (1.0f / 4096.0f);
	float r21 = (float)inst->rot[2][1] * (1.0f / 4096.0f);
	float r22 = (float)inst->rot[2][2] * (1.0f / 4096.0f);
	float trx = (float)inst->trans[0];
	float trY = (float)inst->trans[1];
	float trz = (float)inst->trans[2];
	float h = (float)inst->h;
	float ofxOverH, ofyOverH;

	*outH = h;

	if (h == 0.0f)
	{
		/* Degenerate camera (distanceToScreen_PREV == 0). Every division below
		 * would be by zero; the caller skips the instance. */
		return;
	}

	ofxOverH = (float)inst->ofx / h;
	ofyOverH = (float)inst->ofy / h;

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

	/*
	 * Depth-axis correction (the s33 sink fix -- see the depthShift field
	 * comment). The shader recovers depth as viewZ = hParam.x * dp4(qw, v),
	 * and hParam.x is used for NOTHING else -- screen x/y come from the
	 * qx/qy dp4s and the GPU's divide, whose w is dp4(qw, v) alone. So
	 * handing the draw side h scaled by the inverse regime factor puts this
	 * instance's depth on the track's view.z axis while leaving its screen
	 * position bit-identical. qx/qy/qw stay composed with the UNSCALED h:
	 * they encode retail's own projection and must keep doing so.
	 *
	 * Toggle for the hardware A/B (dumpreq `axisfix`); default ON because
	 * OFF is the sink.
	 */
	if (g_dbg_modelAxisFix)
	{
		if (inst->depthShift > 0)
		{
			*outH = h / (float)(1 << inst->depthShift);
		}
		else if (inst->depthShift < 0)
		{
			*outH = h * (float)(1 << -inst->depthShift);
		}
	}
}

/* Open run of triangles sharing one atlas + textured state. Runs never span
 * instances: each instance has its own MVP uniform, so a run that crossed one
 * would draw the second instance's triangles with the first one's matrix. */
struct NativeModelDrawRun
{
	int startVertex;
	int triCount;
	int atlas;
	int textured;
	int blend; /* BlendMode: 0 = opaque, else ABR+1 (see NativeModelDraw_FaceBlend) */
};

internal void NativeModelDraw_FlushRun(struct NativeModelDrawRun *run, u32 *drawsOut)
{
	if (run->triCount <= 0)
	{
		return;
	}

	N3DS_ModelDraw_Run(run->startVertex, run->triCount, run->atlas, run->textured, run->blend);
	(*drawsOut)++;
	run->triCount = 0;
}

/*
 * A face's PSX ABR (semi-transparency mode), or 3 for opaque.
 *
 * Same rule and same citation as the track's (docs/TRANSPARENCY_DESIGN.md 1):
 * tpage bits 5-6, with 3 as CTR's "this page is opaque" sentinel. Reads the
 * face's own captured tpage, so nothing new had to be recorded at emit time.
 *
 * Untextured faces have no tpage and cannot be classified by it. They report
 * opaque, which is what retail's untextured model primitives overwhelmingly are;
 * the ABE bit an untextured G3 could carry is not visible at this capture point
 * (the primitive code is chosen by the writer this capture runs ahead of). That
 * is a known, bounded gap -- 35% of model triangles are untextured, but the
 * semi-transparent ones among them are effects that already looked opaque
 * throughout rung 1-2, so this changes nothing about them either way. Closing it
 * needs the capture to carry the writer's primitive code, which is a rung 3
 * change to the hook, not to this function.
 */
internal int NativeModelDraw_FaceBlend(const struct NativeModelEmitFace *face, const struct NativeModelEmitInstance *inst)
{
	/*
	 * Returns a BlendMode directly (0 = opaque, else PSX ABR + 1, matching
	 * native_gpu.c:49's GET_TPAGE_BLEND) rather than an ABR, because "ABR 3"
	 * and "opaque" are NOT the same thing once a writer can force blending:
	 * a forced face whose page carries ABR 3 blends as B+F/4
	 * (BM_ADD_QUATER_SOURCE), it does not become opaque. The earlier version of
	 * this function returned an ABR and used 3 as its opaque sentinel, which
	 * made that state impossible to express.
	 */
	if (inst->forceSemiTrans)
	{
		/* The writer decided ABE; the tpage still decides HOW. An untextured
		 * ghost face has tpage 0, giving ABR 0 = B/2 + F/2 -- the ordinary
		 * half-and-half blend, which is what a ghost should look like. */
		return (int)((face->tpage >> 5) & 3) + 1;
	}

	if (!face->textured)
	{
		return 0;
	}

	{
		const int abr = (int)((face->tpage >> 5) & 3);

		/* ABR 3 is CTR's "this page is opaque" sentinel for the normal writer. */
		return (abr == 3) ? 0 : (abr + 1);
	}
}

void NativeModelDraw_IssueForWalk(void)
{
	const struct NativeModelEmitSnapshot *snap = &s_modelEmitBuf[s_modelEmitReadIdx];
	static u32 s_logCountdown = 512;
	static u32 s_totalAppendFail = 0;
	u32 drawsIssued = 0, trisDrawn = 0, instDrawn = 0, appendFailed = 0, blendTrisDrawn = 0;
	u32 i;

	if (g_dbg_modelRender == 0)
	{
		return;
	}

	/* Same contract as the track's: a degraded snapshot was TRUNCATED, so
	 * drawing it would show a frame missing arbitrary objects. Draw nothing
	 * rather than something incomplete. */
	if ((snap->valid == 0) || (snap->degraded != 0) || (snap->instCount == 0))
	{
		return;
	}

	N3DS_ModelDraw_Begin();

	/*
	 * TWO PASSES, opaque then blended -- the same shape as the resident track's
	 * (docs/TRANSPARENCY_DESIGN.md 4-5), and for the same reason: a blended
	 * surface must composite over everything opaque behind it, and "behind"
	 * spans instances, so the split cannot be per instance.
	 *
	 * What each pass draws, per PS1 textured semi-transparency: a semi-transparent
	 * primitive blends only the texels whose sampled colour has the STP bit set;
	 * the rest stay opaque. So a semi-transparent face is drawn TWICE --
	 *
	 *   pass 0: PASS1 view (its non-STP texels), blending OFF, depth written
	 *   pass 1: PASS2 view (its STP texels), blending ON, depth NOT written
	 *
	 * -- which is exactly what the packet path has done per-primitive since s17
	 * (native_gpu.c:2670). Opaque faces draw once, in pass 0, with the OPAQUE
	 * view, unchanged from before this feature.
	 *
	 * Unlike the track, this needs no atlas planning at all: the decode cache
	 * keys cells on (page, clut, format, VIEW) and materialises them lazily, so
	 * asking it for PASS1/PASS2 simply produces two cells on first use. That is
	 * the whole reason models were the cheap half of this work.
	 *
	 * Cost when a frame has no semi-transparent model content: one extra walk
	 * over the instance list whose inner loop `continue`s on the first test, and
	 * no GPU state changes at all (the run never opens, so nothing flushes).
	 */
	for (int pass = 0; pass < 2; pass++)
	{
	for (i = 0; i < snap->instCount; i++)
	{
		const struct NativeModelEmitInstance *inst = &snap->insts[i];
		struct NativeModelDrawRun run = {0, 0, -1, 0, 0};
		float qx[4], qy[4], qw[4], h;
		int instanceBound = 0;
		u32 t;

		NativeModelDraw_ComposeRows(inst, qx, qy, qw, &h);

		if (h == 0.0f)
		{
			continue;
		}

		for (t = 0; t < inst->triCount; t++)
		{
			const u32 triIndex = inst->firstTri + t;
			const struct NativeModelEmitFace *face = &snap->faces[triIndex];
			const struct NativeModelEmitVert *src = &snap->verts[triIndex * 3];
			const int faceBlendMode = NativeModelDraw_FaceBlend(face, inst);
			const int faceIsBlended = (faceBlendMode != 0);
			N3DS_Vertex out[3];
			int atlasIdx = -1, pixelBaseU = 0, pixelBaseV = 0, originU = 0, originV = 0;
			int textured = face->textured;
			int offset;
			int v;
			N3DS_TexView view;
			int runBlend;

			/*
			 * Pass 0 draws EVERY face -- opaque ones whole, semi-transparent ones
			 * as their PASS1 (non-STP) half. Pass 1 revisits only the
			 * semi-transparent faces to add their blended half.
			 *
			 * NOT `faceIsBlended != pass`, which is the obvious-looking version
			 * and is wrong: it would skip semi-transparent faces in pass 0 and so
			 * never draw their opaque body at all -- a waterfall or a plume would
			 * lose everything except its translucent texels.
			 */
			if ((pass == 1) && !faceIsBlended)
			{
				continue;
			}

			if (!faceIsBlended)
			{
				view = N3DS_TEXVIEW_OPAQUE;
				runBlend = 0;
			}
			else if (pass == 0)
			{
				view = N3DS_TEXVIEW_PASS1;
				runBlend = 0;
			}
			else
			{
				view = N3DS_TEXVIEW_PASS2;
				runBlend = faceBlendMode; /* already a BlendMode, see FaceBlend */
			}

			/* Deferred until a triangle actually survives the pass filter, so a
			 * pass that draws nothing for this instance costs no uniform upload
			 * and does not inflate instDrawn. */
			if (!instanceBound)
			{
				instanceBound = 1;
				N3DS_ModelDraw_Instance(qx, qy, qw, h);
				instDrawn++;
			}

			if (textured)
			{
				/*
				 * Resolve through the packet path's own decode cache -- same
				 * function, same memo, same eviction policy. Models arrive at
				 * the cache today as packets, so this adds no working set;
				 * design section 5.
				 *
				 * N3DS_ResolveTriangleTexture reads only .u/.v from the three
				 * GrVertex arguments, so a stack triple carrying just those is
				 * a faithful call and not a shortcut. Format comes from the
				 * face's own tpage (GetTPageFormat, native_gpu.c) rather than
				 * the renderer's s_currentTexFormat, which tracks the PACKET
				 * path's GPU state and means nothing here.
				 */
				GrVertex uvOnly[3];

				for (v = 0; v < 3; v++)
				{
					memset(&uvOnly[v], 0, sizeof(uvOnly[v]));
					uvOnly[v].u = (u8)src[v].u;
					uvOnly[v].v = (u8)src[v].v;
				}

				N3DS_ResolveTriangleTexture(&uvOnly[0], &uvOnly[1], &uvOnly[2], (s16)face->tpage, (s16)face->clut, GetTPageFormat((int)(s16)face->tpage),
				                            view, &atlasIdx, &pixelBaseU, &pixelBaseV, &originU, &originV);
			}

			for (v = 0; v < 3; v++)
			{
				out[v].x = src[v].x;
				out[v].y = src[v].y;
				out[v].z = src[v].z;

				if (textured)
				{
					/* Identical encoding to N3DS_ConvertTriangleVerts: texel-
					 * CENTRE numerators (2a+1) against a 1/2048 shader scale,
					 * with V flipped as 2047-2a because the PICA samples the
					 * atlas bottom-up. Both facts were paid for in blood --
					 * see that function's comments for the two separate
					 * multi-session bugs behind them. */
					out[v].u = (s16)(((pixelBaseU + ((int)src[v].u - originU)) << 1) + 1);
					out[v].v = (s16)(2047 - ((pixelBaseV + ((int)src[v].v - originV)) << 1));
				}
				else
				{
					/* Untextured draws replace the fragment with the vertex
					 * colour outright (the combiner never samples), so the UV
					 * is genuinely unread. Zeroed rather than left
					 * uninitialised. */
					out[v].u = 0;
					out[v].v = 0;
				}

				out[v].r = src[v].r;
				out[v].g = src[v].g;
				out[v].b = src[v].b;
				out[v]._pad = 0;
			}

			offset = N3DS_C3DAppendVertices(out, 3);

			if (offset < 0)
			{
				/* Frame vertex budget exhausted. Counted and reported, never
				 * silent -- a silent version of this is what once deleted
				 * geometry with no log line at all (n3ds_backend.h's SIZING
				 * comment). Stop this instance; later instances would fail the
				 * same way. */
				appendFailed++;
				break;
			}

			/* Extend the open run when this triangle is contiguous in the
			 * vertex buffer AND shares the run's state. Appends are sequential
			 * so contiguity normally holds; testing it rather than assuming it
			 * means a future change to the allocator cannot silently make one
			 * draw cover another run's vertices. */
			if ((run.triCount > 0) && (run.atlas == atlasIdx) && (run.textured == textured) && (run.blend == runBlend)
			    && (offset == (run.startVertex + (run.triCount * 3))))
			{
				run.triCount++;
			}
			else
			{
				NativeModelDraw_FlushRun(&run, &drawsIssued);
				run.startVertex = offset;
				run.triCount = 1;
				run.atlas = atlasIdx;
				run.textured = textured;
				run.blend = runBlend;
			}

			trisDrawn++;

			if (faceIsBlended && (pass == 1))
			{
				blendTrisDrawn++;
			}
		}

		NativeModelDraw_FlushRun(&run, &drawsIssued);
	}
	}

	N3DS_ModelDraw_End();

	s_totalAppendFail += appendFailed;

	if (--s_logCountdown == 0)
	{
		s_logCountdown = 512;
		/* blendTris= is the pass-1 population: the blended half of semi-transparent
		 * model faces, drawn by US rather than handed back. Zero here while
		 * [MDLEMIT] semiTris/f is non-zero means the blend pass is not running --
		 * which is a different failure from "it runs and looks wrong", and the two
		 * are indistinguishable by eye. inst= counts instance BINDS, so it now
		 * exceeds the instance count on frames with blended content: an instance
		 * that appears in both passes binds twice, by design. */
		Platform_LogInstrument("[MDLDRAW] toggle=%d inst=%lu draws=%lu tris=%lu blendTris=%lu appendFail=%lu appendFailTotal=%lu\n", g_dbg_modelRender,
		                       (unsigned long)instDrawn, (unsigned long)drawsIssued, (unsigned long)trisDrawn, (unsigned long)blendTrisDrawn,
		                       (unsigned long)appendFailed, (unsigned long)s_totalAppendFail);
	}
}
#endif /* __3DS__ */

#else

int g_dbg_modelRender = 0;
/* Referenced by native_gte_capture.c's s33 instance-depth bracket, which is
 * gated on GTE_CAPTURE, not MODEL_RENDERER -- so the definition must exist in
 * a capture-on/model-off build (the PC golden config). Found in s34: that
 * combination had never been compiled between s33 and the piranha-plant hunt,
 * and it failed at link. Stays 0 here; the --model-depth-check arg that sets
 * it lives with the real implementation above. */
int g_dbg_modelDepthCheck = 0;

int NativeModelEmit_BeginInstance(const struct InstDrawPerPlayer *idpp, const struct PushBuffer *pb, int forceSemiTrans, const struct Instance *worldInst)
{
	(void)idpp;
	(void)pb;
	(void)forceSemiTrans;
	(void)worldInst;
	return 0;
}

int NativeModelEmit_Triangle(const u32 *packedXY3, const u32 *packedZ3, const int *color3, const struct TextureLayout *tex)
{
	(void)packedXY3;
	(void)packedZ3;
	(void)color3;
	(void)tex;
	/* 0 = "not claimed", so a compiled-out build always writes the packet. */
	return 0;
}

void NativeModelEmit_EndInstance(void)
{
}

void NativeModelEmit_EndFrame(void)
{
}

void NativeModelEmit_Clear(void)
{
}

void NativeModelEmit_PublishForWalk(void)
{
}

int NativeModelEmit_SkipRetailDraw(void)
{
	return 0;
}

#endif
