#include <macros.h>
#include "platform/native_gte_capture.h"

#ifndef CTR_NATIVE_GTE_CAPTURE
#define CTR_NATIVE_GTE_CAPTURE 0
#endif

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE

#include <string.h>

// Direct-mapped, verify-by-stored-key side table (docs/GTE_CAPTURE_DESIGN.md).
// A collision or a stale (last frame's) entry is always a miss, never a wrong
// value -- callers fall back to today's exact snapped behavior on a miss.
//
// DOUBLE-BUFFERED (s26, the render-thread prerequisite): the game thread
// writes frame N+1's captures while the core-2 worker may still be walking
// frame N and reading its table. Two buffers; MainFrame_ResetDB's
// GTECapture_Clear() is the flip point (it already sits exactly at the DB
// swap), and DrawOTag publishes the write buffer as the walk's read buffer at
// post time. One walk in flight ever (the game's own DrawSync fence), so by
// the time ResetDB flips back to a buffer, no walk can still hold it. The
// publish->worker-read ordering rides the render queue's own lock/signal
// pair; no extra synchronization exists or is needed. PC's synchronous walk
// reads the buffer it just wrote (publish makes read==write), byte-identical
// to the single-buffered behavior.
//
// Sizing: eviction misses are not free visually -- on a STATIC scene (menus),
// a vertex that hits most frames but is evicted on some frames pops by up to
// 1px on exactly those frames. PC stays at 2^18 entries (4 MiB/buffer). The
// 3DS -- which consumes this table for the retained-geometry full form, not
// desnap -- takes 2^16 (1.5 MiB/buffer): there an eviction only downgrades a
// prim from retained to walked for a frame, invisible by construction.
#ifdef __3DS__
#define GTE_CAPTURE_TABLE_BITS 16
#else
#define GTE_CAPTURE_TABLE_BITS 18
#endif
#define GTE_CAPTURE_TABLE_SIZE (1u << GTE_CAPTURE_TABLE_BITS)

typedef struct
{
	u32 key;        // (u16)sy << 16 | (u16)sx -- same halves as the real C2_SXYn register (gtereg.h)
	u32 generation; // stamped from the owning buffer's generation; 0 = never written, always a miss
	u8 fracX8;      // top 8 of the 16 discarded fractional bits, i.e. 1/256px resolution
	u8 fracY8;
	u16 sz3;      // reserved for stereo (unused by vertex de-snapping)
	s32 hOverSz3; // reserved for perspective-correct UV (unused by vertex de-snapping)
	s16 vx, vy, vz; // model-space input vector (retainability rendezvous)
	u8 mtxId; // frame-scoped matrix-state id this projection ran under (s26 matrix-class probe / full-form matrix classes)
	u8 _pad;
} GTECaptureEntry;

// Matrix-state ids: which distinct GTE R+TR state a Store's projection ran
// under, numbered 0.. in order of first appearance within the frame. The
// full retained form needs to know whether a stable node was projected
// under the frame's dominant (camera) matrix or a per-object one -- that
// decides its v1 coverage (docs/RETAINED_GEOMETRY_FULL_DESIGN.md, open Q1).
// Ids above the cap collapse into the last slot; a frame with >63 real
// matrix states would make "dominant share" mushy, not wrong.
#define GTE_CAPTURE_MTX_IDS 64

typedef struct
{
	GTECaptureEntry entries[GTE_CAPTURE_TABLE_SIZE];
	u32 generation;
	// UI band travels with the buffer: the band pointers describe the frame
	// whose captures live here, and the walk that reads them may run a frame
	// behind the game thread's current SetUIBand call.
	const void *bandStart;
	const void *bandEnd;
	// UI-band EXEMPTION (s39 flag depth): the one prim range inside the band
	// that is world content -- the checkered flag's packets, which retail
	// links into the swapchain OT's entry 0 during transitions. Travels with
	// the buffer for the same frame-behind reason as the band itself; cleared
	// every frame by Clear (unlike the band, which ResetDB re-sets), so it is
	// only ever live for a frame whose RaceFlag_DrawSelf registered it.
	const void *exemptStart;
	const void *exemptEnd;
	// Per-frame matrix census (reset by Clear): Store count per matrix id,
	// and how many ids this frame actually used.
	u32 mtxStores[GTE_CAPTURE_MTX_IDS];
	u32 mtxUsed;
} GTECaptureBuffer;

// Cost ablation (s29). The whole subsystem's per-frame price is two calls per
// projected vertex in GTE_RotTransPers; this switch removes both without a
// rebuild, so the "what does capture cost" question can be answered by a
// mid-race hardware A/B on identical content instead of by two netloads of two
// binaries racing two different attract tracks. It ablates COST, not the
// table's contents: stale entries are left alone and simply fail their
// generation/key check on lookup, which is the same miss path collisions and
// evictions already take.
int g_dbg_gteCapture = 1;

global_variable GTECaptureBuffer s_gteCaptureBuf[2] = {{{{0}}, 1, NULL, NULL, NULL, NULL}, {{{0}}, 1, NULL, NULL, NULL, NULL}};
// Game-thread cursor: which buffer Store/SetUIBand/Clear touch.
global_variable int s_gteCaptureWriteIdx;
// Walk cursor: which buffer Lookup*/MarkOTNode read. Written only by
// GTECapture_PublishForWalk on the game thread at DrawOTag time, before the
// walk job is posted (or before the synchronous walk runs).
global_variable int s_gteCaptureReadIdx;

internal u32 GTECapture_PackKey(s16 sx, s16 sy)
{
	return ((u32)(u16)sy << 16) | (u32)(u16)sx;
}

internal u32 GTECapture_Hash(u32 key)
{
	// Fibonacci multiplicative hash over the whole packed key -- both sx and
	// sy must contribute; a plain mask would only look at sx (sy lives in the
	// high 16 bits) and collide constantly on vertical runs.
	return (key * 0x9E3779B1u) >> (32u - GTE_CAPTURE_TABLE_BITS);
}

// Matrix-change detector state: game-thread only, continuity across the
// buffer flip is irrelevant (ids are frame-scoped; the first projection of a
// frame always starts id 0).
global_variable u32 s_gteMtxShadow[8];
global_variable int s_gteMtxShadowValid;
global_variable u32 s_gteMtxCurId;

/* s33 instance depth-axis bracket -- full block sits above GTECapture_Store;
 * these exist because Clear (the frame boundary) runs their window flush. */
void GTECapture_EndInstanceDepth(void);
internal void GTECapture_InstanceAxisFlush(void);
extern int g_dbg_modelDepthCheck;
global_variable u32 s_instAxisFrames;

void GTECapture_Clear(void)
{
	GTECaptureBuffer *buf;

	// Flip first, then invalidate the NEW write buffer by generation bump.
	// The buffer being abandoned may still be read by an in-flight walk; it
	// is left untouched.
	s_gteCaptureWriteIdx ^= 1;
	buf = &s_gteCaptureBuf[s_gteCaptureWriteIdx];

	buf->generation++;
	if (buf->generation == 0) // wrapped after ~2^32 frames (2+ years continuous at 60fps)
	{
		memset(buf->entries, 0, sizeof(buf->entries));
		buf->generation = 1;
	}

	memset(buf->mtxStores, 0, sizeof(buf->mtxStores));
	buf->mtxUsed = 0;
	buf->exemptStart = NULL;
	buf->exemptEnd = NULL;
	s_gteMtxShadowValid = 0;
	s_gteMtxCurId = 0;

	/* [PKTAXIS] window boundary (s33 instance-axis bracket, below). Also the
	 * belt-and-braces close of a bracket a mid-frame early-out left open --
	 * the same leak the model capture closes in its EndFrame. */
	GTECapture_EndInstanceDepth();
	if (g_dbg_modelDepthCheck && (++s_instAxisFrames >= 256))
	{
		s_instAxisFrames = 0;
		GTECapture_InstanceAxisFlush();
	}
}

// Called by GTE_RotTransPers with the 8 contiguous CP2C words holding
// R11..R33 and TRX/Y/Z -- one memcmp per projected vertex detects the
// game switching matrices (camera -> instance -> ...).
void GTECapture_NoteMatrix(const void *words8)
{
	GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureWriteIdx];

	if (s_gteMtxShadowValid && (memcmp(s_gteMtxShadow, words8, sizeof(s_gteMtxShadow)) == 0))
	{
		return;
	}

	memcpy(s_gteMtxShadow, words8, sizeof(s_gteMtxShadow));
	if (!s_gteMtxShadowValid)
	{
		s_gteMtxShadowValid = 1;
		s_gteMtxCurId = 0;
	}
	else if (s_gteMtxCurId < (GTE_CAPTURE_MTX_IDS - 1))
	{
		s_gteMtxCurId++;
	}
	if ((s_gteMtxCurId + 1) > buf->mtxUsed)
	{
		buf->mtxUsed = s_gteMtxCurId + 1;
	}
}

void GTECapture_PublishForWalk(void)
{
	s_gteCaptureReadIdx = s_gteCaptureWriteIdx;
}

/*
 * Depthless-geometry suppression (s32). Game-thread only -- Store's own
 * ownership rule -- and never buffered: it is set and cleared inside one
 * producer's call, so no walk can observe it half-applied.
 *
 * WHY THIS EXISTS. DrawSky_Full (game/DrawSky.c) projects the skybox mesh with
 * the GTE's translation vector explicitly zeroed (`CTC2(0, 5..7)`), because a
 * skybox is meant to sit at infinity and rotate with the camera without ever
 * translating. Its SZ3 is therefore R3-dot-v/4096 with no TRZ: a function of the
 * sky mesh's own radius, in the same numeric range as near-field world content,
 * and completely unrelated to how far away it is.
 *
 * Every projected vertex funnels through GTE_RotTransPers, so the skybox stored
 * those bogus depths like any other geometry, and the packet-depth path
 * (native_gpu.c's PacketDepth_ResolveXY) read them back as real view depth. Two
 * failures fall out of that, and this one flag closes both:
 *
 *   1. The skybox competes on depth with the track and with karts instead of
 *      losing to everything -- the "skybox drawing over distant track" report.
 *   2. WORSE, and not in any report because it does not look like a skybox bug:
 *      the table is keyed by SCREEN COORDINATE, the skybox covers the whole
 *      screen, and DrawSky_Full runs AFTER the track and the instance queue. So
 *      a sky vertex EVICTS whatever world vertex hashed to its slot, and that
 *      world primitive then gets stamped with the sky's depth.
 *
 * Suppressing the store makes those lookups miss, which is a path that already
 * exists and is already correct: PacketDepth_ResolveXY derives depth from the OT
 * bucket the walk is parsing (0xFFFF - (otIndex << 6)). The skybox's bucket is
 * pushBuffer->ptrOT[0x3ff] biased by each face's own signed D offset -- i.e.
 * retail's own painter's-algorithm statement of "farthest, and here is the
 * internal layering". Deriving from it preserves that layering exactly, rather
 * than inventing a depth for content that has none.
 *
 * Cost to the other consumers: sky vertices lose sub-pixel de-snapping
 * (CTR_NATIVE_VERTEX_DESNAP) and cannot be retained. Both are acceptable and
 * neither is shipping -- and de-snapping a mesh whose whole purpose is to be a
 * static backdrop is not a feature anyone would notice missing.
 */
global_variable int s_gteCaptureStoreSuppressed;

void GTECapture_SetStoreSuppressed(int suppressed)
{
	s_gteCaptureStoreSuppressed = suppressed;
}

/*
 * Affine depth-remap bracket (s39, the menu/loading checkered flag). Same
 * ownership and same one-producer discipline as the suppression flag above;
 * RaceFlag_DrawSelf is the only client.
 *
 * The flag used to take the suppression bracket: its waving surface genuinely
 * crosses a menu model's depth band (Crash interpenetrated the flag), so its
 * stores were dropped and the packets fell back to flat OT-bucket depth. That
 * fixed the layering and flattened the wave -- in stereo the flag became a
 * cardboard sheet. Owner design, refined from an earlier bias-to-background
 * idea specifically to avoid ANY pose-conditional depth switch (the flag is
 * one continuous animation across its loading, menu and wipe roles): keep the
 * stores, but remap them affinely so the wave's TRUE relief survives at
 * reduced amplitude in a band that cannot collide with anything.
 *
 *   out = OUT_NEAR + ((sz3 - RAW_NEAR) >> SHIFT)
 *
 * Measured raw band under data.matrixTitleFlag: sz3 ~ 61069..65471 (relief
 * ~ +/-2200 view units about ~63270). SHIFT 2 compresses that to ~ +/-550;
 * OUT_NEAR re-anchors the compressed band's near edge just BEHIND the stereo
 * convergence plane (s_stereoConv = 1200 view.z units, n3ds_backend.c -- the
 * physical screen surface). Remapped band for the measured content:
 * ~1267..2367. So the flag ripples at/just behind the screen while menu
 * models (which sit nearer than convergence) pop out in front -- the two
 * bands cannot cross by construction, which is what preserves the painter's
 * guarantee the suppression used to buy, without any role conditionals.
 *
 * Bounds are structural, not runtime-checked: the anchor keeps every remapped
 * value far above PKTDEPTH_UI_BAND (64), so the stamp 0xFFFF - out always
 * lands inside the world band (< PKTDEPTH_UI_BASE, native_gpu.c); the top is
 * OUT_NEAR + ((0xFFFF - RAW_NEAR) >> SHIFT) = 2383, nowhere near u16 range.
 * Raw values that dip under RAW_NEAR (the guard margin below the measured
 * minimum) pin to the anchor -- a flat crest just behind the screen, never a
 * crossing. Baked constants on purpose: the capture side cannot see
 * s_stereoConv (backend TU boundary), and the anchor only needs to be
 * "behind the default screen plane", not track a dumpreq retune.
 *
 * Known accepted caveat (owner, to verify by eye): during screen-covering
 * wipe transitions, content nearer than the flag's band -- popped-out menu
 * models, the near track during race exits -- now depth-tests IN FRONT of
 * the sweeping flag, a departure from retail's flag-over-everything. If it
 * reads wrong on hardware, the documented fallback is flat-front only while
 * the transition is in flight (RaceFlag_IsFullyOnScreen /
 * RaceFlag_DrawOrder), but the pure no-switch form ships first.
 *
 * PC: default builds compile none of this (CTR_NATIVE_GTE_CAPTURE off). On a
 * capture-enabled PC build the extra stores are inert for output --
 * g_dbg_packetDepth stays 0 there -- except under VERTEX_DESNAP, where flag
 * vertices (formerly suppressed) become desnap-eligible like any other
 * geometry. That is the desnap feature working, not a parity break, and no
 * golden gate runs that configuration.
 */
enum
{
	GTE_FLAGREMAP_RAW_NEAR = 60928, /* measured min 61069 minus guard margin */
	GTE_FLAGREMAP_SHIFT = 2,        /* relief compressed to 1/4 */
	GTE_FLAGREMAP_OUT_NEAR = 1232   /* 32 view units behind s_stereoConv 1200 */
};

global_variable int s_gteCaptureDepthRemap;

void GTECapture_SetStoreDepthRemap(int active)
{
	s_gteCaptureDepthRemap = active;
}

/*
 * s32 EVICTION CENSUS -- header comment on GTECapture_ReadEvictCensus has the
 * argument. Counted at the one place a slot can change owner. The extra branch
 * costs nothing measurable: the entry line is already being fetched for the
 * write on the very next instructions.
 */
int g_dbg_gteNoEvict;

global_variable struct
{
	u32 diffMtx;  /* same pixel, different object -- the harmful population */
	u32 sameMtx;  /* same pixel, same object: a real self-overwrite, benign */
	u32 maxDz;    /* biggest sz3 delta a harmful overwrite introduced */
	u32 refused;  /* overwrites declined while g_dbg_gteNoEvict is on */
} s_gteEvict;

u32 GTECapture_ReadEvictCensus(u32 *outSameMtx, u32 *outMaxDz)
{
	u32 diff = s_gteEvict.diffMtx;

	*outSameMtx = s_gteEvict.sameMtx;
	*outMaxDz = s_gteEvict.maxDz;
	s_gteEvict.diffMtx = 0;
	s_gteEvict.sameMtx = 0;
	s_gteEvict.maxDz = 0;
	s_gteEvict.refused = 0;
	return diff;
}

/*
 * ================== s33: INSTANCE DEPTH-AXIS BRACKET ==================
 *
 * The packet-side half of the s33 sink fix. Retail's instance pipeline is
 * regime-scaled (RenderBucket_AdjustViewPositionForMvp: MVP translation <<2
 * under 0x1000 view units, DRAW_HUGE >>2), so every SZ3 an instance's own
 * projection produces is on a 4x / 1x / quarter axis relative to the track's
 * view.z. The native model path corrects this per instance
 * (native_model_emit.c, NativeModelEmitInstance.depthShift -- keep the two
 * classifiers in sync); instance content still drawn as PACKETS (the split /
 * special / reflection draw funcs, models-retail configs) stores its scaled
 * SZ3 into THIS table, and a table hit is what buries it: the resolve-miss
 * fallback derives depth from the OT bucket, which retail builds from the
 * UNSHIFTED view z (native_gpu.c PacketDepth_ResolveXY), so misses were
 * always roughly right and hits were 4x wrong. The owner-visible form is the
 * kart vanishing in the stretch just after an ice pond (Blizzard Bluff /
 * Polar Pass): the zone flags put the kart on DrawFunc_Special there, off
 * the fixed native path.
 *
 * So the correction happens AT THE STORE, inside a bracket the RenderBucket
 * dispatch opens around each instance's draw func. Classification is
 * self-calibrating, same doctrine as the native fix: NORMAL/NORMAL_ALT pass
 * mvp.t[2] and classify exactly; the split/special/reflection funcs carry a
 * VIEW or IDENTITY matrix in mvp (RenderBucket_StoreViewMatrixForSplit, the
 * :1593 identity store), so they classify from the FIRST SZ3 the bracket
 * sees instead -- the candidates sit a factor of 4 apart and a kart's own
 * extent is far smaller, so the nearest-candidate test cannot confuse them
 * for content beyond ~128 view units, and inside 128 units the scale error
 * is smaller than the near-band floor anyway (shift forced to 0 there).
 *
 * Known, accepted residual: the QUEUE-phase bbox projections (8 corners per
 * instance, RenderBucket_PrepareDrawContext) run outside this bracket and
 * still store scaled depths. Eight vertices against hundreds, at screen
 * coordinates the body immediately restores -- measure before caring.
 */

/* dumpreq 'pktaxisfix'. Default ON: OFF is the ice-pond vanish. */
int g_dbg_pktAxisFix = 1;
/* s41: the ground-decal (shadows/skids/tires) fixed-shift unscale. Its own
 * switch rather than a share of g_dbg_pktAxisFix, so an A/B moves one thing. */
int g_dbg_decalAxisFix = 1;

#define GTE_INSTAXIS_UNRESOLVED 99
#define GTE_INSTAXIS_MTX_NONE 0xFFFFFFFFu

global_variable int s_instAxisActive;
/* s41: set by GTECapture_BeginFixedDepthShift -- the shift is known from source
 * rather than measured, so the store skips classify/reclassify/stats and only
 * applies it. See the block in GTECapture_Store. */
global_variable int s_instAxisFixed;
global_variable int s_instAxisShift = GTE_INSTAXIS_UNRESOLVED;
global_variable s64 s_instAxisTrueZ;
global_variable int s_instAxisFunc;
/* Matrix state the current shift was classified under. The split/reflection
 * writers project the BODY and the MIRRORED copy under different GTE
 * matrices within one bracket, and the two passes need not share a regime --
 * owner-observed as the reflection showing through the ground and the body
 * flickering around ice (Bluff, s33). Classification therefore re-runs at
 * every matrix change inside the bracket. Depends on NoteMatrix's change
 * detector (g_dbg_gteCapture == 1); capture state 2 skips NoteMatrix and
 * degrades to one shift per instance. The mirror pass classifies against the
 * BODY origin's trueZ -- an approximation (the mirrored point sits deeper by
 * twice its height above the plane), but the candidates are a factor of 4
 * apart and a kart rides close to the surface it reflects in, so the
 * nearest-candidate test has margin. */
global_variable u32 s_instAxisMtxAtClassify = GTE_INSTAXIS_MTX_NONE;

/* [PKTAXIS] window accumulators, per draw func (0=normal 1=normalAlt 2=split
 * 3=special 4=reflection). Stats gated behind g_dbg_modelDepthCheck -- the
 * axis-investigation flag -- but classification and the fix itself are not. */
extern int g_dbg_modelDepthCheck;
global_variable u32 s_instAxisInstN[5];
global_variable u32 s_instAxisShiftN[5][3]; /* [func][0]=shift2 [1]=shift0 [2]=shift-2 */
global_variable u32 s_instAxisUnresolvedN[5];
global_variable u32 s_instAxisReclassN[5]; /* matrix-change re-classifications (mirror passes) */
global_variable u32 s_instAxisStoreN[5];
global_variable double s_instAxisRatioSum[5];
global_variable double s_instAxisRatioMin[5];
global_variable double s_instAxisRatioMax[5];
/* s_instAxisFrames is declared above GTECapture_Clear, which drives the flush. */

/* Nearest of {4x, 1x, 1/4x}: which regime this mvp-axis depth actually is,
 * measured against the track-axis view.z of the same point. MUST stay in
 * lockstep with the classifier in NativeModelEmit_BeginInstance. */
internal int GTECapture_ClassifyShift(s64 mvpAxisZ, s64 trueZ)
{
	s64 eNear = mvpAxisZ - (trueZ << 2);
	s64 eFar = mvpAxisZ - trueZ;
	s64 eHuge = (mvpAxisZ << 2) - trueZ;

	if (eNear < 0) { eNear = -eNear; }
	if (eFar < 0) { eFar = -eFar; }
	if (eHuge < 0) { eHuge = -eHuge; }

	return (eNear <= eFar) ? ((eNear <= eHuge) ? 2 : -2) : ((eFar <= eHuge) ? 0 : -2);
}

void GTECapture_BeginInstanceDepth(const struct Instance *inst, const struct PushBuffer *pb, s32 mvpZIfNormal, int funcKind)
{
	s64 trueZ;

	if ((inst == NULL) || (pb == NULL))
	{
		return;
	}

	/* Track-axis view.z of the instance origin -- same integer arithmetic as
	 * RenderBucket_GetViewPosition's GTE path and the native classifier's. */
	if ((inst->flags & SCREENSPACE_INSTANCE) != 0)
	{
		trueZ = (s64)inst->matrix.t[2];
	}
	else
	{
		s64 dx = (s64)inst->matrix.t[0] - (s64)pb->pos.x;
		s64 dy = (s64)inst->matrix.t[1] - (s64)pb->pos.y;
		s64 dz = (s64)inst->matrix.t[2] - (s64)pb->pos.z;

		trueZ = ((s64)pb->matrix_ViewProj.m[2][0] * dx + (s64)pb->matrix_ViewProj.m[2][1] * dy + (s64)pb->matrix_ViewProj.m[2][2] * dz) >> 12;
	}

	s_instAxisActive = 1;
	s_instAxisTrueZ = trueZ;
	s_instAxisFunc = ((funcKind >= 0) && (funcKind < 5)) ? funcKind : 0;
	s_instAxisMtxAtClassify = GTE_INSTAXIS_MTX_NONE;

	if (trueZ < 128)
	{
		/* Too near (or behind) for the nearest-candidate test to be safe,
		 * and a 3z scale error under 128 units is inside the shared 256-unit
		 * near-band floor. Store untouched. */
		s_instAxisShift = 0;
	}
	else if (mvpZIfNormal > 0)
	{
		s_instAxisShift = GTECapture_ClassifyShift((s64)mvpZIfNormal, trueZ);
	}
	else
	{
		s_instAxisShift = GTE_INSTAXIS_UNRESOLVED; /* classify on first store */
	}
}

/*
 * s41: open a bracket whose regime shift is KNOWN, not measured. `shift` uses
 * the same encoding as s_instAxisShift (2 = stored SZ3 is 4x the track axis,
 * divide it down; -2 = quarter axis; 0 = already correct). Closed by
 * GTECapture_EndInstanceDepth like the instance bracket -- and, like it, by
 * GTECapture_Clear at the frame edge, so a mid-frame early-out cannot leak the
 * shift into unrelated content.
 *
 * Never nests with the instance bracket: its three callers (VehGroundShadow,
 * VehGroundSkids, DrawTires) all run outside the RenderBucket dispatch.
 */
void GTECapture_BeginFixedDepthShift(int shift)
{
	s_instAxisActive = 1;
	s_instAxisFixed = 1;
	/* Runtime A/B (dumpreq `decalaxis`): 0 leaves the stores exactly as they
	 * were before s41, which is the leg this fix has to be judged against.
	 * Separate from g_dbg_pktAxisFix on purpose -- that one also governs the
	 * instance bracket, so sharing it would move two things at once. */
	s_instAxisShift = (g_dbg_decalAxisFix != 0) ? shift : 0;
	s_instAxisTrueZ = 0;
	s_instAxisMtxAtClassify = GTE_INSTAXIS_MTX_NONE;
}

void GTECapture_EndInstanceDepth(void)
{
	if (!s_instAxisActive)
	{
		return;
	}

	if (s_instAxisFixed)
	{
		/* Fixed brackets contribute no [PKTAXIS] statistics by construction --
		 * there is nothing measured to report, and borrowing one of the five
		 * func buckets would corrupt a line the instance work is still read
		 * against. */
		s_instAxisActive = 0;
		s_instAxisFixed = 0;
		s_instAxisShift = GTE_INSTAXIS_UNRESOLVED;
		s_instAxisMtxAtClassify = GTE_INSTAXIS_MTX_NONE;
		return;
	}

	if (g_dbg_modelDepthCheck)
	{
		s_instAxisInstN[s_instAxisFunc]++;
		if (s_instAxisShift == 2)
		{
			s_instAxisShiftN[s_instAxisFunc][0]++;
		}
		else if (s_instAxisShift == 0)
		{
			s_instAxisShiftN[s_instAxisFunc][1]++;
		}
		else if (s_instAxisShift == -2)
		{
			s_instAxisShiftN[s_instAxisFunc][2]++;
		}
		else
		{
			/* Ended without a single store to classify from -- the draw func
			 * rejected every triangle. Nothing was scaled, nothing was wrong. */
			s_instAxisUnresolvedN[s_instAxisFunc]++;
		}
	}

	s_instAxisActive = 0;
	s_instAxisShift = GTE_INSTAXIS_UNRESOLVED;
	s_instAxisMtxAtClassify = GTE_INSTAXIS_MTX_NONE;
}

/* Per-frame window flush, driven by GTECapture_Clear below. 256 content-ish
 * frames per line keeps it to a few lines a minute at 30fps. */
internal void GTECapture_InstanceAxisFlush(void)
{
	static const char *const funcName[5] = {"normal", "normalAlt", "split", "special", "reflection"};
	int f;

	for (f = 0; f < 5; f++)
	{
		if ((s_instAxisInstN[f] == 0) && (s_instAxisStoreN[f] == 0))
		{
			continue;
		}
		Platform_LogInstrument("[PKTAXIS] func=%s inst=%lu shift4x=%lu shift1x=%lu shiftQ=%lu unresolved=%lu reclass=%lu stores=%lu ratio=%.3f (%.3f..%.3f)\n",
		                       funcName[f], (unsigned long)s_instAxisInstN[f], (unsigned long)s_instAxisShiftN[f][0], (unsigned long)s_instAxisShiftN[f][1],
		                       (unsigned long)s_instAxisShiftN[f][2], (unsigned long)s_instAxisUnresolvedN[f], (unsigned long)s_instAxisReclassN[f],
		                       (unsigned long)s_instAxisStoreN[f], s_instAxisStoreN[f] ? (s_instAxisRatioSum[f] / (double)s_instAxisStoreN[f]) : 0.0,
		                       s_instAxisRatioMin[f], s_instAxisRatioMax[f]);
		s_instAxisReclassN[f] = 0;
		s_instAxisInstN[f] = 0;
		s_instAxisShiftN[f][0] = 0;
		s_instAxisShiftN[f][1] = 0;
		s_instAxisShiftN[f][2] = 0;
		s_instAxisUnresolvedN[f] = 0;
		s_instAxisStoreN[f] = 0;
		s_instAxisRatioSum[f] = 0.0;
		s_instAxisRatioMin[f] = 0.0;
		s_instAxisRatioMax[f] = 0.0;
	}
}

void GTECapture_Store(s16 sx, s16 sy, s64 sxFull, s64 syFull, u16 sz3, s32 hOverSz3, s16 vx, s16 vy, s16 vz)
{
	GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureWriteIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	GTECaptureEntry *entry = &buf->entries[GTECapture_Hash(key)];

	/* Suppressed producers must not even EVICT -- see the comment above; the
	 * eviction is the half of the bug that does not look like a skybox
	 * problem. Tested here rather than at the GTE_RotTransPers call site so
	 * there is exactly one place a store can be dropped. */
	if (s_gteCaptureStoreSuppressed)
	{
		return;
	}

	/* Flag depth remap (s39, block above SetStoreDepthRemap). Applied before
	 * anything else reads sz3 -- census, ratios, the entry itself -- so every
	 * consumer sees one consistent axis, same placement rule as the instance
	 * bracket below. The two brackets never nest: this one wraps
	 * RaceFlag_DrawSelf, which runs outside the RenderBucket dispatch. */
	if (s_gteCaptureDepthRemap)
	{
		u32 t = ((u32)sz3 > (u32)GTE_FLAGREMAP_RAW_NEAR) ? ((u32)GTE_FLAGREMAP_OUT_NEAR + (((u32)sz3 - (u32)GTE_FLAGREMAP_RAW_NEAR) >> GTE_FLAGREMAP_SHIFT))
		                                                 : (u32)GTE_FLAGREMAP_OUT_NEAR;

		sz3 = (u16)t;
	}

	/* Instance depth-axis bracket (s33, block comment above): put this store
	 * on the track's view.z axis before ANYTHING sees it -- including the
	 * eviction census a few lines down, whose dz would otherwise mix units. */
	if (s_instAxisActive)
	{
		/*
		 * s41 FIXED-SHIFT BRACKET -- the ground-decal producers.
		 *
		 * VehGroundShadow, VehGroundSkids and DrawTires each pre-multiply
		 * their CAMERA-RELATIVE position by 4 before projecting (shadows
		 * `* 4`, skids and tires `<< 2`) and project with a ZERO translation
		 * vector, so every SZ3 they produce is 4x the track's view.z. That is
		 * the same regime the instance bracket below corrects -- but these
		 * three run outside RenderBucket entirely (straight from
		 * MainFrame_RenderFrame), so the dispatch-site bracket never saw them
		 * and their depth went into the table 4x too far. Owner-visible form:
		 * skid marks that survive within about a tile of the camera and are
		 * buried beyond it, because a 4x error grows with distance.
		 *
		 * No classifier here, unlike the instance path: RenderBucket's scale
		 * is CONDITIONAL (AdjustViewPositionForMvp -- <<2 under 0x1000 view
		 * units, DRAW_HUGE >>2), which is exactly why it has to measure. The
		 * shift these three apply is unconditional and visible in the source,
		 * so classifying it would be inventing uncertainty. Skipping the
		 * classify/reclassify/stats work also keeps [PKTAXIS]'s five func
		 * buckets meaning precisely what they have always meant.
		 *
		 * Everything from here to the shift application is instance-only; the
		 * application itself is shared, which is the whole point of riding
		 * this bracket rather than adding a second one.
		 */
		if (!s_instAxisFixed)
		{
			/* Per-matrix re-classification -- see s_instAxisMtxAtClassify. The
			 * first matrix state adopts Begin's exact mvp-based shift; each later
			 * state inside the bracket (the mirror pass) re-classifies from its
			 * own first store. */
			if (s_instAxisMtxAtClassify == GTE_INSTAXIS_MTX_NONE)
			{
				s_instAxisMtxAtClassify = s_gteMtxCurId;
			}
			else if (s_gteMtxCurId != s_instAxisMtxAtClassify)
			{
				s_instAxisMtxAtClassify = s_gteMtxCurId;
				if (s_instAxisTrueZ >= 128)
				{
					s_instAxisShift = GTECapture_ClassifyShift((s64)sz3, s_instAxisTrueZ);
				}
				if (g_dbg_modelDepthCheck)
				{
					s_instAxisReclassN[s_instAxisFunc]++;
				}
			}

			if (s_instAxisShift == GTE_INSTAXIS_UNRESOLVED)
			{
				/* First store of a split/special/reflection instance: SZ3 is
				 * TRZ plus a model-extent term, and the candidates are 4x apart,
				 * so nearest-candidate on the raw SZ3 names the regime. */
				s_instAxisShift = GTECapture_ClassifyShift((s64)sz3, s_instAxisTrueZ);
			}

			if (g_dbg_modelDepthCheck && (s_instAxisTrueZ >= 128))
			{
				double ratio = (double)sz3 / (double)s_instAxisTrueZ;
				int f = s_instAxisFunc;

				if (s_instAxisStoreN[f] == 0)
				{
					s_instAxisRatioMin[f] = ratio;
					s_instAxisRatioMax[f] = ratio;
				}
				else
				{
					if (ratio < s_instAxisRatioMin[f]) { s_instAxisRatioMin[f] = ratio; }
					if (ratio > s_instAxisRatioMax[f]) { s_instAxisRatioMax[f] = ratio; }
				}
				s_instAxisStoreN[f]++;
				s_instAxisRatioSum[f] += ratio;
			}
		} /* !s_instAxisFixed -- shared shift application follows */

		if (g_dbg_pktAxisFix)
		{
			if (s_instAxisShift == 2)
			{
				u32 t = (u32)sz3 >> 2;

				sz3 = (u16)((t != 0) ? t : 1); /* keep a hit distinguishable from empty */
			}
			else if (s_instAxisShift == -2)
			{
				u32 t = (u32)sz3 << 2;

				sz3 = (t > 0xFFFFu) ? (u16)0xFFFFu : (u16)t;
			}
		}
	}

	/* Same key AND live generation means this slot already holds a projection
	 * of THIS SAME screen coordinate from earlier in the frame -- not a hash
	 * collision (different key), which the lookup's key check already turns
	 * into a clean miss. Whoever wrote it is about to lose their depth. */
	if ((entry->generation == buf->generation) && (entry->key == key))
	{
		if (entry->mtxId != (u8)s_gteMtxCurId)
		{
			u32 dz = (entry->sz3 > sz3) ? (u32)(entry->sz3 - sz3) : (u32)(sz3 - entry->sz3);

			s_gteEvict.diffMtx++;
			if (dz > s_gteEvict.maxDz)
			{
				s_gteEvict.maxDz = dz;
			}
			if (g_dbg_gteNoEvict)
			{
				s_gteEvict.refused++;
				return;
			}
		}
		else
		{
			s_gteEvict.sameMtx++;
		}
	}

	entry->key = key;
	entry->generation = buf->generation;
	entry->fracX8 = (u8)((sxFull & 0xFFFF) >> 8);
	entry->fracY8 = (u8)((syFull & 0xFFFF) >> 8);
	entry->sz3 = sz3;
	entry->hOverSz3 = hOverSz3;
	entry->vx = vx;
	entry->vy = vy;
	entry->vz = vz;
	entry->mtxId = (u8)s_gteMtxCurId;
	buf->mtxStores[s_gteMtxCurId]++;
}

int GTECapture_Lookup(s16 sx, s16 sy, u8 *fracX8, u8 *fracY8)
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	const GTECaptureEntry *entry = &buf->entries[GTECapture_Hash(key)];

	if ((entry->generation != buf->generation) || (entry->key != key))
	{
		return 0;
	}

	*fracX8 = entry->fracX8;
	*fracY8 = entry->fracY8;
	return 1;
}

int GTECapture_LookupModel(s16 sx, s16 sy, s16 outV[3])
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	const GTECaptureEntry *entry = &buf->entries[GTECapture_Hash(key)];

	if ((entry->generation != buf->generation) || (entry->key != key))
	{
		return 0;
	}

	outV[0] = entry->vx;
	outV[1] = entry->vy;
	outV[2] = entry->vz;
	return 1;
}

// True view-space depth for a packet's screen coordinate -- the field that has
// sat here unread since capture was written ("reserved for stereo").
//
// This is the SAME axis the track renderer's shader calls view.z and the same
// axis retail quantises into an OT index: ctr_track.v.pica's header and
// native_gte_core.c:319-328 (C2_SZ3 = Lm_D(m_mac3, 1), clamped to unsigned
// 16-bit) establish that SZ3 is a linear view depth, NOT a reciprocal. That is
// what makes one shared depth axis possible at all -- see
// docs/PACKET_DEPTH_DESIGN.md.
//
// Same key, same generation/collision test, same miss semantics as every other
// lookup here: a miss is always a miss, never a wrong value.
int GTECapture_LookupDepth(s16 sx, s16 sy, u16 *outSz3)
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	const GTECaptureEntry *entry = &buf->entries[GTECapture_Hash(key)];

	if ((entry->generation != buf->generation) || (entry->key != key))
	{
		return 0;
	}

	*outSz3 = entry->sz3;
	return 1;
}

// Provenance variant -- see the header. Deliberately a separate entry point
// rather than an out-param bolted onto the above, so the existing consumers'
// generated code is unchanged and any behaviour difference this session sees is
// attributable to the new path alone.
int GTECapture_LookupDepthEx(s16 sx, s16 sy, u16 *outSz3, u8 *outMtxId)
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	const GTECaptureEntry *entry = &buf->entries[GTECapture_Hash(key)];

	if ((entry->generation != buf->generation) || (entry->key != key))
	{
		return 0;
	}

	*outSz3 = entry->sz3;
	*outMtxId = entry->mtxId;
	return 1;
}

int GTECapture_LookupModelEx(s16 sx, s16 sy, s16 outV[3], u8 *outMtxId)
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	const GTECaptureEntry *entry = &buf->entries[GTECapture_Hash(key)];

	if ((entry->generation != buf->generation) || (entry->key != key))
	{
		return 0;
	}

	outV[0] = entry->vx;
	outV[1] = entry->vy;
	outV[2] = entry->vz;
	*outMtxId = entry->mtxId;
	return 1;
}

// Read-buffer matrix census for the walk: dominant id (most Stores -- in
// race content, the camera), its share in percent of all Stores, and how
// many distinct ids the frame used. Share 0 means the frame stored nothing.
u32 GTECapture_ReadMatrixCensus(u8 *outDomId, u32 *outIdsUsed)
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];
	u32 total = 0, best = 0, bestId = 0, i;

	for (i = 0; i < GTE_CAPTURE_MTX_IDS; i++)
	{
		total += buf->mtxStores[i];
		if (buf->mtxStores[i] > best)
		{
			best = buf->mtxStores[i];
			bestId = i;
		}
	}

	*outDomId = (u8)bestId;
	*outIdsUsed = buf->mtxUsed;
	return total ? (best * 100u) / total : 0;
}

// Walk-thread state: BeginOTWalk/MarkOTNode/LookupSuppressed all run on
// whichever thread owns the walk (worker on 3DS, main on PC) -- one walk in
// flight, so a single flag needs no buffering. s_gteCaptureExemptNode is
// per-NODE, not sticky: it holds only while the walk is inside the flag's
// registered prim range (s39, see the exempt fields on the buffer).
global_variable int s_gteCaptureSuppress;
global_variable int s_gteCaptureExemptNode;

void GTECapture_SetUIBand(const void *start, const void *end)
{
	GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureWriteIdx];

	buf->bandStart = start;
	buf->bandEnd = end;
}

void GTECapture_SetUIBandExemptRange(const void *start, const void *end)
{
	GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureWriteIdx];

	buf->exemptStart = start;
	buf->exemptEnd = end;
}

void GTECapture_BeginOTWalk(void)
{
	s_gteCaptureSuppress = 0;
	s_gteCaptureExemptNode = 0;
}

void GTECapture_MarkOTNode(const void *node)
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];

	// Sticky: the UI band sits at the tail of the reversed OT walk, so once
	// entered, every later packet this walk is UI. Reset by BeginOTWalk.
	if ((node >= buf->bandStart) && (node < buf->bandEnd))
	{
		s_gteCaptureSuppress = 1;
	}
	// Exemption is per NODE and evaluated on EVERY node, not only inside the
	// UI band: since the channel split its consumer is the PARALLAX channel
	// (native_gpu.c's resolver), and the flag needs its ripple in the menu
	// pose too -- where its packets sit in an ordinary world bucket and the
	// band was never entered. Two pointer compares per node against a range
	// that is NULL/NULL on any frame RaceFlag_DrawSelf did not register.
	s_gteCaptureExemptNode = ((node >= buf->exemptStart) && (node < buf->exemptEnd));
}

int GTECapture_LookupSuppressed(void)
{
	// Deliberately the RAW sticky flag. The flag-range exemption used to be
	// subtracted here, which put flag packets on world OCCLUSION stamps; the
	// channel split moved that job to the parallax channel and handed
	// occlusion back to retail's own layering (see GTECapture_NodeExempt).
	return s_gteCaptureSuppress;
}

/*
 * Is the walk currently inside the registered flag prim range? (s40b channel
 * split, docs/FLAG_SPLIT_CHANNEL_SPEC.md.)
 *
 * Read by PacketDepth_ResolveXY for TWO things, and the pairing is the whole
 * design:
 *   - OCCLUSION: an exempt primitive resolves exactly as if its stores had
 *     been suppressed -- a miss. That is the pre-s40 behaviour, i.e. retail's
 *     own painter's statement: the flag paints over everything during a
 *     screen-covering wipe (its packets are in the UI band there) and sits
 *     behind the menu models when parked (bucket-derived far depth). Pose
 *     changes are retail draw order, not a stereo jump.
 *   - PARALLAX: the same primitive's captures ARE read, and they carry the
 *     compressed ripple band the store-side remap wrote, in every pose,
 *     continuously. This is the channel the owner's no-pose-conditional rule
 *     governs.
 */
int GTECapture_NodeExempt(void)
{
	return s_gteCaptureExemptNode;
}

#endif /* defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE */
