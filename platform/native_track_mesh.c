/*
 * Native track renderer -- load-time mesh builder (s27 scaffold).
 * docs/NATIVE_TRACK_RENDERER_DESIGN.md sections 4.1/4.2.
 *
 * Ladder step 1 (PC count-only): builds the resident-mesh representation --
 * per-static-leaf vertex/index ranges over the game's own quadblock data --
 * and logs a [TRKMESH] block. Nothing is drawn yet on any platform and the
 * gate is OFF by default, so golden output is untouched. The 3DS draw side
 * (linear-heap upload, track atlas, the (otIndex<<k)|bias depth shader)
 * lands on top of exactly these structures in a later step.
 *
 * Leaf classification mirrors the v1 packet-path exclusions (design 4.4):
 * water-flagged leaves and leaves referencing water/scenery-animated
 * vertices are skipped here and keep their retail draw. The s27 census
 * measured one scenery-heavy level where that excludes ~2/3 of quads --
 * the planned v2 answer is re-uploading those verts (they're enumerable),
 * not widening the exclusion.
 */

/* Included unconditionally (review fix, FIX 5): the accessor stubs in the
 * #else branch far below need struct NativeTrackVert's real definition too
 * (it's a complete type in the header regardless of the gate), and pulling
 * the include out from inside the #if is what lets the SAME header serve
 * both the real builder below and the stubs, rather than each half seeing a
 * different implicit tag for the same struct name. */
#include "platform/native_track_mesh.h"

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER

/* struct NativeTrackVert itself now lives in the header above (rung 3a step
 * 1) -- the read-only accessor surface at the end of this file hands a
 * draw side a typed `const struct NativeTrackVert *`, and a 3DS re-packer
 * needs the real field layout to do that, so the definition moved out of
 * this file's private section rather than being duplicated. Everything
 * about it is unchanged: 16 bytes, matches the design's budget. Positions
 * are model-space straight from LevVertex; colors are the near set
 * (color_hi); u/v are page-space texel bytes from the near-variant
 * TextureLayout corner assigned to this grid vertex (atlas translation
 * happens at upload, not here); texRef indexes the per-level layout table
 * below; drawOrder is the signed per-face OT byte folded into depth by the
 * shader (GetProjectedOtIndex's own formula, 226:1956). */

struct NativeTrackLeafRange
{
	u16 bspIndex;    /* index into mesh->bspRoot -- visibility lookup key */
	u16 atlasBin;    /* reserved: draws are binned per (leaf x atlas) */
	u32 firstIndex;  /* into s_trackIndices */
	u32 indexCount;
	/* Rung 3a step 2: copied from mesh->bspRoot[bspIndex].box at BUILD time
	 * (NOT re-read from the BSP tree per frame) so the draw side's per-leaf
	 * depth-range compose (platform/native_track_frame.c, design 4.3/12b)
	 * never has to dereference a raw mesh_info/BSP pointer on the worker --
	 * this flat copy enjoys the same build-or-nothing validity contract every
	 * other field in this struct already does. */
	s16 boxMin[3], boxMax[3];
	/* Review fix (FIX 1a): running min/max of the signed per-face drawOrder
	 * byte over every face THIS leaf actually emits (same loop that already
	 * reads block->draw_order_high per face below -- tracked alongside it,
	 * not re-derived). The box alone bounds where a leaf's geometry SITS;
	 * it says nothing about how far a face's own drawOrder can additionally
	 * push that leaf's vertices along the OT axis in the shader. Widening
	 * the draw-side range by [drawOrderMin, drawOrderMax] before it reaches
	 * the shader is what stops a vertex from extrapolating past a valid
	 * ordinal and being clipped away -- see native_track_mesh.h's accessor
	 * comment for the full failure mode. Sentinel-initialized to
	 * [127, -128] (the widest-possible empty range) before the emit loop; a
	 * leaf that ends up with indexCount == 0 never has these read. */
	s8 drawOrderMin, drawOrderMax;
};

/* Grew this session (FIX 1a added drawOrderMin/drawOrderMax): 24B -> 28B
 * (2 bytes of struct-alignment padding after the two s8 fields, since a u32
 * member forces 4-byte alignment). Tripwire so the next field added here
 * doesn't silently drift the size assumed by every allocation/log line that
 * sizes off this struct without anyone noticing, the same reasoning
 * NativeTrackVert's own assert (native_track_mesh.h) already applies. */
CTR_STATIC_ASSERT(sizeof(struct NativeTrackLeafRange) == 28);

/*
 * Rung 2a: track atlas LAYOUT (docs/NATIVE_TRACK_RENDERER_DESIGN.md 12).
 * Platform-neutral half of the dedicated track atlas: per-(tpage,clut)
 * union UV rectangles gathered from the resident verts, shelf-packed into
 * 512x512 atlas units, with a translation entry per rect so upload (rung
 * 2b, 3DS) can rewrite baked page-space UVs into atlas coordinates. The
 * census bound the working set (<=120 pairs, ~0.5-1.2MB near-variant
 * decoded), so fixed-size tables suffice; overflow logs and truncates
 * rather than growing.
 */
#define TRKMESH_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define TRKMESH_MAX(a, b) (((a) > (b)) ? (a) : (b))
#define TRKMESH_ROUND8(a) (((a) + 7) & ~7)

#define TRACK_ATLAS_DIM 512
#define TRACK_ATLAS_MAX_UNITS 8
#define TRACK_ATLAS_MAX_RECTS 512
#define TRACK_ATLAS_PAD 1 /* texel border per rect against bleed */
#define TRACK_ATLAS_UNIT_INVALID 0xff

struct NativeTrackAtlasRect
{
	u16 tpage, clut;
	u8 minU, minV, maxU, maxV; /* TRUE page-space texel bbox (inclusive) -- UV translation reads these, never allocW/allocH */
	u8 srcU, srcV;              /* page-relative decode origin actually passed to N3DS_DecodeAndTilePage. srcU is always minU
	                              * (a horizontal decode overrun wraps in-bounds -- PS1 VRAM row addressing, harmless). srcV is
	                              * pulled UP from minV whenever minV+allocH would overrun the page half: N3DS_DecodeAndTilePage
	                              * wraps the COLUMN but not the ROW (native_renderer_3ds.c ~:1151), so decoding from minV with
	                              * a tall allocH can read vram[] past the end of the page half (review finding #1). Rung 3's UV
	                              * translation must read THESE fields, never minU/minV: atlasU = atlasX + (u - srcU),
	                              * atlasV = atlasY + (v - srcV). */
	u8 atlasUnit;                /* TRACK_ATLAS_UNIT_INVALID until placed by NativeTrackAtlas_Pack, or if the backend
	                               * couldn't/didn't upload this rect (NativeTrackAtlas_UploadToBackend resets it on every
	                               * skip path) -- rung 3 must never bind a unit for a rect carrying this sentinel. */
	u16 atlasX, atlasY; /* placement of (srcU,srcV) inside the unit -- always a multiple of 8, see NativeTrackAtlas_Pack */
	u16 allocW, allocH; /* (maxU-minU+1)/(maxV-minV+1) rounded UP to multiples of 8 and capped at 256 (one PSX page) -- the
	                      * shelf-packed/uploaded size, distinct from the true bbox above (rung 2b, upload needs 8x8-tile-
	                      * aligned rects) */
};

struct NativeTrackAtlasLayout
{
	struct NativeTrackAtlasRect rects[TRACK_ATLAS_MAX_RECTS];
	int rectCount;
	int unitsUsed;
	int rectsDropped;
	u32 packedTexels;
};

/* One animated texture chain that at least one resident face samples. `anim`
 * is borrowed from the level's own data (cleared with the rest of the state on
 * teardown, same contract as animSrcArray). buildUnit is the atlas unit the
 * ACTIVE frame landed in at build time, which is the unit
 * NativeTrackMesh_BinLeavesByAtlasUnit binned this texture's leaves against --
 * a frame that packed into a different unit therefore cannot be patched in
 * without the leaf sampling the wrong atlas, so it is skipped and counted. */
struct NativeTrackAnimTexEntry
{
	const struct AnimTex *anim;
	u16 texRef;
	u16 numFrames;
	u32 frameSlot; /* first index into animFrameRect */
	u32 vertFirst; /* first index into animTexVerts */
	u32 vertCount;
	s16 curFrame; /* frame currently baked into the GPU buffer; -1 = "as built" */
	u8 buildUnit;
};

#define TRACK_ANIMTEX_MAX 128
#define TRACK_ANIMTEX_MAX_FRAMESLOTS 4096

struct NativeTrackMeshState
{
	struct mesh_info *builtFor; /* rebuild latch, same idiom as the census */

	struct NativeTrackVert *verts;
	u32 vertCount;
	u16 *indices;
	u32 indexCount;
	struct NativeTrackLeafRange *leafRanges;
	u32 leafCount;

	/* Reverse lookup, bspIndex -> leafRanges index (rung 3a step 1,
	 * include/platform/native_track_mesh.h's NativeTrackMesh_LeafIndexForBsp):
	 * numBspNodes entries, -1 = no resident range for that BSP index (an
	 * excluded leaf per design 4.4, or simply not a leaf). Built once per
	 * mesh build alongside leafRanges itself, NOT per frame -- the frame
	 * recorder's visibility walk maps the game's own per-frame BSP-index
	 * lists through this table, which is what keeps that walk O(visible
	 * leaves) instead of O(leaves x ranges). */
	s32 *bspToLeafIndex;

	/* bspToLeafIndex's allocated element count, latched at the SAME time as
	 * the allocation itself (review fix: NativeTrackMesh_LeafIndexForBsp used
	 * to bound bspIndex against builtFor->numBspNodes read LIVE at call time,
	 * but that can silently drift from what this table was actually sized
	 * for -- a level reloading into the same mesh_info arena slot with equal
	 * quad/vertex counts but a different bsp node count would read past the
	 * allocation before the rebuild latch above ever catches it. Bound
	 * against THIS field instead, which only ever changes alongside the
	 * allocation it describes. */
	u32 bspNodeCount;

	/* Per-level table of near-variant TextureLayout pointers; texRef fields
	 * index it. AnimTex faces record the AnimTex instead (low bit set,
	 * matching the game's own encoding) so per-frame UV patching can find
	 * the live frame. */
	uintptr_t *texTable;
	u32 texCount;

	/* texRef -> rect index into the atlas layout (-1 = unplaced). */
	s16 texRectIndex[1024];
	struct NativeTrackAtlasLayout atlas;

	/*
	 * s30 animated-vertex patching. Leaves touching an animated vertex used to
	 * be dropped whole (Polar Pass: 43 of 77 missing leaves); they are kept now,
	 * which means the few genuinely moving vertices inside them have to be
	 * refreshed per frame or they sit frozen.
	 *
	 * The source data really does move under us: AnimateWater1P / AnimateQuad
	 * (game/RenderLevel/AnimateWater.c) write straight into LevVertex.pos and
	 * .color_hi, and MainFrame_RenderFrame.c:1174-1181 calls them every frame
	 * for 1P regardless of which renderer draws -- checked, because a patcher
	 * reading an array nobody animates would look like it worked.
	 *
	 * srcVert is parallel to verts (NOT a field on it -- NativeTrackVert is
	 * pinned at 16 bytes by a static assert). TRACK_SRCVERT_NONE means "never
	 * animate this one", which covers both non-animated vertices and, more
	 * importantly, WELDED ones: the emit loop may replace a vertex's position
	 * with an edge's canonical mid to close a T-junction crack, and re-copying
	 * the raw source every frame would silently undo that repair and reopen the
	 * crack on exactly the water edges this feature exists to animate.
	 */
	u16 *srcVert;
	u32 *animVerts; /* indices into verts[] that need a per-frame refresh */
	u32 animVertCount;
	u32 animVertWelded; /* animated in the source but frozen by the weld above */
	const struct LevVertex *animSrcArray;
	/* animSrcArray's element count, latched with the pointer. The build-time
	 * filter already bounds every srcVert against it, but the PATCH re-reads
	 * those indices a frame later and must not trust that on its own -- same
	 * reasoning bspNodeCount exists for above. */
	u32 animSrcCount;

	/*
	 * s30 AnimTex ("picture flip") patching -- turbo-pad arrows, waterfall
	 * sheets, lava. Distinct from the animated-VERTEX case above and far more
	 * visible: nothing moves, the game just repoints a face at the next frame
	 * of a texture chain every vblank (CTR_CycleTex_LEV, called unconditionally
	 * from MainFrame_RenderFrame.c:102). Checked that this is the ONLY runtime
	 * appearance change a track face can have: nothing in game/ ever writes
	 * QuadBlock.ptr_texture_mid, so every scrolling/blinking track surface in
	 * the game reaches the screen through this one indirection.
	 *
	 * The expensive half was already done at load: EVERY frame of every chain
	 * is decoded and packed into the track atlas (the AddLayout loop below).
	 * Only the ACTIVE frame's rect index was kept, which is exactly why the
	 * resident renderer freezes them on whatever frame happened to be up when
	 * the level loaded.
	 *
	 * Two things are needed per frame and both are recorded here at build time:
	 *   - animFrameRect: every frame's rect index, not just the active one.
	 *     Slot f of an entry is frame f, ALWAYS allocated even when that frame
	 *     was unusable (NULL icon group, bad tpage) -- it holds -1 then, so the
	 *     frame index the game hands us indexes straight in with no remap.
	 *   - animTexVerts: which resident vertices belong to each animated texRef,
	 *     so the patch is O(that texture's vertices), not O(track).
	 *
	 * A frame swap is NOT just a rect swap. Atlas rects are keyed by
	 * (tpage, clut) and MERGED (NativeTrackAtlas_AddLayout), so two frames of
	 * one chain may share a rect and differ only in their page-space corner
	 * UVs -- which is what the baked NativeTrackVert.u/.v came from. So the
	 * patch re-reads the frame's own TextureLayout corners and re-encodes,
	 * rather than translating the old UV into a new rect. The corner each
	 * vertex used is stashed in faceFlags bits 3-4 at emit time (the field's
	 * own comment already reserved them as "rotation scratch"; nothing outside
	 * this file reads it, and the GPU mirror does not carry it at all).
	 */
	struct NativeTrackAnimTexEntry *animTex;
	u32 animTexCount;
	s16 *animFrameRect;
	u32 animFrameSlotCount;
	u32 *animTexVerts;
	u32 animTexVertCount;

	/* Per-frame patch outcome, reported once per level and then only when it
	 * changes -- see NativeTrackAnim_PatchFrame. */
	u32 animPatchSkipUnit;  /* frame lives in a different atlas unit than the leaf bin assumes */
	u32 animPatchSkipRect;  /* frame never got a placed rect */
	u32 animPatchVertsLast; /* vertices rewritten on the most recent patching frame */
};

#define TRACK_SRCVERT_NONE 0xFFFFu

global_variable struct NativeTrackMeshState s_trackMesh;

/*
 * Scatter-write staging for the per-frame AnimTex patch
 * (NativeTrackAnim_PatchFrame, far below -- declared up here only because
 * NativeTrackMesh_Free has to reset it and comes first in this file).
 *
 * Batched rather than one backend call per vertex because the expensive part
 * of the write is not the store, it is the cache flush that has to follow it
 * before the GPU reads the block: one flush over a chunk's touched range beats
 * one flush per vertex by orders of magnitude, and a fixed 4KB of static
 * staging beats a per-frame malloc on a path that runs every vblank.
 */
#define TRACK_ANIM_PATCH_CHUNK 512

static u16 s_animPatchIndex[TRACK_ANIM_PATCH_CHUNK];
static s16 s_animPatchUV[TRACK_ANIM_PATCH_CHUNK * 2];
static u32 s_animPatchCount;
static u32 s_animPatchOutOfRect;
static u32 s_animPatchOutOfRectLogged;
static u32 s_animPatchFrames; /* heartbeat divider -- see the [TRKANIM] window line */
static u32 s_animPatchSteps;  /* chain frame-changes actually applied this window */

/* The animated-VERTEX staging (water surfaces that physically move), kept in
 * its own batch: it runs every frame over every animated vertex, where the UV
 * batch above runs only when a texture chain steps. Sharing one batch would
 * make the cheap case pay the expensive case's flush range. */
static u32 s_vertPatchSrcOOB; /* srcVert indices outside the latched source array -- see the patch loop */
static u16 s_vertPatchIndex[TRACK_ANIM_PATCH_CHUNK];
static s16 s_vertPatchXYZ[TRACK_ANIM_PATCH_CHUNK * 3];
static u8 s_vertPatchRGB[TRACK_ANIM_PATCH_CHUNK * 3];
static u32 s_vertPatchCount;

/* The four mid-LOD quads of the 3x3 grid (design section 2 / ctr-tools),
 * counter-clockwise in grid space; triangulated (0,2,1)(1,2,3) per quad,
 * the same diagonal ctr-tools exports. */
static const u8 sTrackFaceCorners[4][4] = {
    {0, 4, 5, 6},
    {4, 1, 6, 7},
    {5, 6, 2, 8},
    {6, 7, 8, 3},
};

/*
 * Weld-at-load (design INST-1 closure): the census measured 0-86 edges per
 * level where two quadblocks share an edge (same endpoint positions) but
 * carry different edge-mid positions -- genuine T-junction crack candidates
 * in the authored data. Since we build the mesh, they are repaired here: a
 * pre-pass records one canonical mid per matched edge (first sighting wins),
 * and the emit loop snaps any divergent mid onto it. Load-time only.
 */
#define TRACK_WELD_SLOTS 32768

struct NativeTrackWeldTable
{
	u64 keys[TRACK_WELD_SLOTS];
	u64 mid[TRACK_WELD_SLOTS]; /* packed 48-bit position + 1 */
};

/* Outer edges of the 3x3 grid: corner pair + the mid between them (same
 * table as the census edge-matcher). */
static const u8 sTrackOuterEdges[4][3] = {{0, 1, 4}, {0, 2, 5}, {1, 3, 7}, {2, 3, 8}};

/*
 * Per-face UV rotation/flip (design risk #4). faceFlags per face =
 * (draw_order_low >> (8 + 5*face)) & 0x1f: low 3 bits are a RotateFlipType,
 * next 2 bits faceMode. The eight permutations below are derived by
 * replaying ctr-tools' QuadTexRotate (per rotation: swap(0,1), swap(0,2),
 * swap(2,3)) and QuadTexFlip (swap(0,1), swap(2,3)) swap sequences on the
 * TL,TR,BL,BR corner order. Entry [rot][corner] = which TextureLayout UV
 * index that corner takes. Audit status: matches ctr-tools' exporter
 * exactly; confirmation against retail pixels lands with the first textured
 * draw (Azahar side-by-side).
 */
static const u8 sTrackUvPerm[8][4] = {
    {0, 1, 2, 3}, /* None */
    {2, 0, 3, 1}, /* Rotate90 */
    {3, 2, 1, 0}, /* Rotate180 */
    {1, 3, 0, 2}, /* Rotate270 */
    {0, 2, 1, 3}, /* FlipRotate270 */
    {2, 3, 0, 1}, /* FlipRotate180 */
    {3, 1, 2, 0}, /* FlipRotate90 */
    {1, 0, 3, 2}, /* Flip */
};

/* TextureLayout corner byte offsets relative to &layout->u0: (u,v) pairs for
 * UV indices 0-3 sit at +0, +4, +8, +10 (clut/tpage interleave between). */
static const u8 sTrackUvByteOfs[4] = {0, 4, 8, 10};

static u64 NativeTrackMesh_PackPos(const struct LevVertex *v)
{
	return (((u64)(u16)v->pos.x << 32) | ((u64)(u16)v->pos.y << 16) | (u64)(u16)v->pos.z) + 1;
}

static u64 NativeTrackMesh_EdgeKey(u64 ka, u64 kb)
{
	u64 lo = (ka < kb) ? ka : kb;
	u64 hi = (ka < kb) ? kb : ka;
	u64 key = lo * 0x9e3779b97f4a7c15ull ^ (hi + 0x517cc1b727220a95ull);

	return key ? key : 1;
}

/* Probe for an edge key; returns the slot, or -1 when the table is full.
 * Insert semantics live at the call sites. */
static int NativeTrackMesh_WeldSlot(struct NativeTrackWeldTable *table, u64 key)
{
	u32 slot = (u32)((key ^ (key >> 31)) % TRACK_WELD_SLOTS);
	int probes = 0;

	while ((table->keys[slot] != 0) && (table->keys[slot] != key))
	{
		slot = (slot + 1) % TRACK_WELD_SLOTS;
		if (++probes >= TRACK_WELD_SLOTS)
		{
			return -1;
		}
	}

	return (int)slot;
}

static void NativeTrackMesh_Free(void)
{
#if defined(__3DS__)
	/* Release the dedicated atlas pool's linear-heap textures too (review
	 * finding #8) -- this is the only teardown path a level currently has
	 * (called both on rebuild and on allocation-failure unwind below), so
	 * it's also the only place leaving a track can release that memory.
	 * Rung 3a step 2: the resident geometry buffer (N3DS_TrackGeom_Upload)
	 * is the SAME per-level linear-heap allocation shape, freed here for
	 * exactly the same reason. */
	N3DS_TrackAtlas_Free();
	N3DS_TrackGeom_Free();
#endif
	free(s_trackMesh.verts);
	free(s_trackMesh.indices);
	free(s_trackMesh.leafRanges);
	free(s_trackMesh.bspToLeafIndex);
	free(s_trackMesh.texTable);
	free(s_trackMesh.srcVert);
	free(s_trackMesh.animVerts);
	free(s_trackMesh.animTex);
	free(s_trackMesh.animFrameRect);
	free(s_trackMesh.animTexVerts);
	/* The memset below is what clears animSrcArray -- a pointer into the
	 * level's own vertex pool, borrowed not owned, and dangling the moment the
	 * level it came from is torn down. Each animTex entry's `anim` pointer is
	 * borrowed the same way and cleared by the same memset. */
	memset(&s_trackMesh, 0, sizeof(s_trackMesh));

	/* Patch staging is static, so it does NOT ride the memset -- a half-filled
	 * chunk left over from the outgoing level would otherwise be flushed
	 * against the incoming one's vertex buffer, writing UVs into whatever
	 * geometry happens to share those indices. */
	s_animPatchCount = 0;
	s_animPatchOutOfRect = 0;
	s_animPatchOutOfRectLogged = 0;
	s_animPatchFrames = 0;
	s_animPatchSteps = 0;
	s_vertPatchCount = 0;
	s_vertPatchSrcOOB = 0;
}

/* Merge one TextureLayout's corner bbox into the per-(tpage,clut) rect
 * map. Layout corners ARE the face's texel footprint (vert UVs are just
 * permutations of them), so no vertex pass is needed. */
static void NativeTrackAtlas_AddLayout(const struct TextureLayout *layout, s16 *outRectIndex)
{
	struct NativeTrackAtlasLayout *atlas = &s_trackMesh.atlas;
	u16 tpage = CTR_ReadU16LE(&layout->tpage);
	u16 clut = CTR_ReadU16LE(&layout->clut);
	u8 minU = layout->u0, maxU = layout->u0, minV = layout->v0, maxV = layout->v0;
	static const u8 cornerOfs[3] = {4, 8, 10}; /* u1, u2, u3 byte offsets */

	for (int c = 0; c < 3; c++)
	{
		u8 u = (&layout->u0)[cornerOfs[c]];
		u8 v = (&layout->u0)[cornerOfs[c] + 1];

		if (u < minU)
		{
			minU = u;
		}
		if (u > maxU)
		{
			maxU = u;
		}
		if (v < minV)
		{
			minV = v;
		}
		if (v > maxV)
		{
			maxV = v;
		}
	}

	for (int i = 0; i < atlas->rectCount; i++)
	{
		if ((atlas->rects[i].tpage == tpage) && (atlas->rects[i].clut == clut))
		{
			struct NativeTrackAtlasRect *rect = &atlas->rects[i];

			rect->minU = TRKMESH_MIN(rect->minU, minU);
			rect->minV = TRKMESH_MIN(rect->minV, minV);
			rect->maxU = TRKMESH_MAX(rect->maxU, maxU);
			rect->maxV = TRKMESH_MAX(rect->maxV, maxV);

			if (outRectIndex != NULL)
			{
				*outRectIndex = (s16)i;
			}
			return;
		}
	}

	if (atlas->rectCount >= TRACK_ATLAS_MAX_RECTS)
	{
		atlas->rectsDropped++;
		return;
	}

	struct NativeTrackAtlasRect *rect = &atlas->rects[atlas->rectCount];

	rect->tpage = tpage;
	rect->clut = clut;
	rect->minU = minU;
	rect->minV = minV;
	rect->maxU = maxU;
	rect->maxV = maxV;
	rect->atlasUnit = TRACK_ATLAS_UNIT_INVALID;

	if (outRectIndex != NULL)
	{
		*outRectIndex = (s16)atlas->rectCount;
	}

	atlas->rectCount++;
}

/*
 * Shelf-pack the gathered rects into 512x512 units. Census-sized: ~120
 * pairs, ~0.5-1.2MB of near-variant texels -- a plain shelf is plenty.
 *
 * 8-texel alignment (rung 2b): the 3DS upload path writes whole 8x8
 * PICA200 tile blocks (N3DS_TrackAtlas_UploadRect), so both the allocated
 * SIZE of each rect and its PLACEMENT must be multiples of 8 -- an
 * unaligned destination offset would address the wrong tile entirely, not
 * just look slightly off.
 *
 * w/h below are the true bbox rounded UP to a multiple of 8 (with
 * TRACK_ATLAS_PAD folded into the rounding rather than added as a separate
 * texel offset -- see the struct comment on allocW/allocH). Because w and h
 * are always multiples of 8 and both cursors start at 0, cursorX/cursorY
 * stay multiples of 8 by construction as they accumulate whole w/h steps;
 * TRKMESH_ROUND8 is applied anyway at the point they're stored, so this
 * invariant is enforced rather than merely relied upon. The true bbox
 * (minU/minV/maxU/maxV) is untouched -- UV translation always reads that,
 * never allocW/allocH -- so the rounding only affects how much atlas space
 * a rect reserves and how many texels the decoder produces, not addressing.
 *
 * Two review fixes (post rung-2b-first-cut) folded in here:
 *   - allocW/allocH are capped at 256 after rounding. Uncapped, a
 *     near-full-page union (trueW/trueH up to 256, the likeliest-large rect
 *     shape) rounds to 264 and would otherwise trip the TRACK_ATLAS_DIM
 *     check below and get dropped outright, uploading nothing. The cap
 *     sacrifices the bleed pad for these rects, which is fine under
 *     GPU_NEAREST with exact UVs -- there's no filtering to bleed into. With
 *     the cap in place w/h can never exceed 256 < TRACK_ATLAS_DIM (512), so
 *     that check is now unreachable; left as-is (defensive dead code).
 *   - srcU/srcV (the actual decode origin, distinct from minU/minV) are
 *     computed here per rect -- see the struct comment on those fields for
 *     why plain minV isn't safe to decode from directly.
 */
static void NativeTrackAtlas_Pack(void)
{
	struct NativeTrackAtlasLayout *atlas = &s_trackMesh.atlas;
	int unit = 0;
	int cursorX = 0, cursorY = 0, shelfH = 0;

	for (int i = 0; i < atlas->rectCount; i++)
	{
		struct NativeTrackAtlasRect *rect = &atlas->rects[i];
		int trueW = rect->maxU - rect->minU + 1;
		int trueH = rect->maxV - rect->minV + 1;
		int w = TRKMESH_ROUND8(trueW + 2 * TRACK_ATLAS_PAD);
		int h = TRKMESH_ROUND8(trueH + 2 * TRACK_ATLAS_PAD);
		int srcV;

		if (w > 256)
		{
			w = 256;
		}
		if (h > 256)
		{
			h = 256;
		}

		if ((w > TRACK_ATLAS_DIM) || (h > TRACK_ATLAS_DIM))
		{
			atlas->rectsDropped++;
			continue;
		}

		if (cursorX + w > TRACK_ATLAS_DIM)
		{
			cursorX = 0;
			cursorY += shelfH;
			shelfH = 0;
		}

		if (cursorY + h > TRACK_ATLAS_DIM)
		{
			unit++;
			cursorX = 0;
			cursorY = 0;
			shelfH = 0;

			if (unit >= TRACK_ATLAS_MAX_UNITS)
			{
				atlas->rectsDropped += atlas->rectCount - i;
				break;
			}
		}

		/* Decode origin (review finding #1): pull srcV up from minV just far
		 * enough that the whole h-tall decode window stays inside the page
		 * half (pageOriginY + srcV + h <= pageOriginY + 256), never past the
		 * end of vram[]. srcU never needs this treatment -- see the struct
		 * comment. */
		srcV = TRKMESH_MIN((int)rect->minV, 256 - h);
		if (srcV < 0)
		{
			srcV = 0;
		}
		rect->srcU = rect->minU;
		rect->srcV = (u8)srcV;

		rect->atlasUnit = (u8)unit;
		rect->atlasX = (u16)TRKMESH_ROUND8(cursorX);
		rect->atlasY = (u16)TRKMESH_ROUND8(cursorY);
		rect->allocW = (u16)w;
		rect->allocH = (u16)h;
		cursorX += w;
		shelfH = TRKMESH_MAX(shelfH, h);
		atlas->packedTexels += (u32)(w * h);
	}

	/* unit can reach TRACK_ATLAS_MAX_UNITS exactly at the overflow break
	 * above (review finding #7) -- clamp rather than report one more unit
	 * than the pool actually has. */
	atlas->unitsUsed = (atlas->rectCount > 0) ? TRKMESH_MIN(unit + 1, TRACK_ATLAS_MAX_UNITS) : 0;
}

/*
 * PAGE-space texel coord -> the GPU-side s16 atlas UV, in ONE place.
 *
 * Both writers of a resident vertex's UV go through here: the one-time upload
 * repack (NativeTrackGeom_UploadToBackend) and the per-frame AnimTex patch
 * (NativeTrackAnim_PatchFrame). That is a hard requirement, not tidiness --
 * the ten-session texture-quilt bug was a COPY of this arithmetic that dropped
 * the V flip, and a patcher carrying a second copy would reintroduce it on
 * exactly the surfaces (waterfalls, turbo pads) whose whole point is to be
 * looked at.
 *
 * The encoding, and why each part of it: 2x fixed point with +1 for the texel
 * CENTRE (sampling corners shifts every fetch half a texel under GPU_NEAREST),
 * and V flipped because PICA reads the atlas bottom-up, so memory row `a` is
 * addressed by numerator (2*DIM - 1) - 2a. U is not flipped; horizontal order
 * already matches. Denominator is 2*TRACK_ATLAS_DIM = 1024, which is what
 * ctr_track.v.pica's atlasScale must be. Spelled with this file's own
 * TRACK_ATLAS_DIM rather than the backend's N3DS_TRACK_ATLAS_DIM so the
 * arithmetic also compiles (and can be count-verified) on PC, where the
 * backend header is absent; the two are static-asserted equal on 3DS.
 *
 * Reads srcU/srcV, never minU/minV -- see NativeTrackAtlasRect's own field
 * comment for why those differ on tall rects. Returns the RAW pre-encode atlas
 * coords through outAtlasU/outAtlasV so a caller can range-check placement
 * against the rect without re-deriving (or, worse, checking the encoding
 * against itself); pass NULL when not validating.
 */
static void NativeTrackAtlas_EncodeUV(const struct NativeTrackAtlasRect *rect, u8 pageU, u8 pageV, s16 *outU, s16 *outV, int *outAtlasU, int *outAtlasV)
{
	const int atlasU = (int)rect->atlasX + ((int)pageU - (int)rect->srcU);
	const int atlasV = (int)rect->atlasY + ((int)pageV - (int)rect->srcV);

	*outU = (s16)((atlasU << 1) + 1);
	*outV = (s16)(((2 * TRACK_ATLAS_DIM) - 1) - (atlasV << 1));

	if (outAtlasU != NULL)
	{
		*outAtlasU = atlasU;
	}
	if (outAtlasV != NULL)
	{
		*outAtlasV = atlasV;
	}
}

/*
 * Once per level, after the atlas upload and the leaf binning: work out which
 * resident vertices belong to each animated texture chain, and which atlas
 * unit that chain's leaves were binned against.
 *
 * Two passes (count, then fill) so the flat list is sized to what is actually
 * there rather than to a worst case -- an animated chain is typically a
 * handful of faces on a track of twenty thousand quadblocks. The texRef ->
 * entry direction goes through a 1024-entry scratch map rather than a linear
 * scan per vertex, because the naive form is vertCount x animTexCount and
 * both of those are level data, not constants.
 *
 * Allocation failure here is a degrade, not a level drop: animTexVerts stays
 * NULL, the patch does nothing, and the track renders exactly as it did before
 * this feature -- frozen but present.
 */
static void NativeTrackAnim_BuildVertexLists(void)
{
	static s16 refToEntry[1024];
	u32 i, e, total = 0;

	if ((s_trackMesh.animTexCount == 0) || (s_trackMesh.animTex == NULL) || (s_trackMesh.verts == NULL))
	{
		return;
	}

	for (i = 0; i < 1024; i++)
	{
		refToEntry[i] = -1;
	}

	for (e = 0; e < s_trackMesh.animTexCount; e++)
	{
		struct NativeTrackAnimTexEntry *entry = &s_trackMesh.animTex[e];
		s16 rectIdx = (entry->texRef < 1024) ? s_trackMesh.texRectIndex[entry->texRef] : (s16)-1;

		entry->vertCount = 0;
		entry->vertFirst = 0;

		/* The unit the ACTIVE frame landed in, which is the unit
		 * NativeTrackMesh_BinLeavesByAtlasUnit binned this chain's leaves
		 * against. A later frame that packed into a DIFFERENT unit cannot be
		 * swapped in without that leaf sampling the wrong atlas, so the patch
		 * skips it and counts the skip rather than drawing garbage. */
		if ((rectIdx >= 0) && (rectIdx < s_trackMesh.atlas.rectCount))
		{
			entry->buildUnit = s_trackMesh.atlas.rects[rectIdx].atlasUnit;
		}

		if (entry->texRef < 1024)
		{
			refToEntry[entry->texRef] = (s16)e;
		}
	}

	for (i = 0; i < s_trackMesh.vertCount; i++)
	{
		u16 ref = s_trackMesh.verts[i].texRef;
		s16 e16 = (ref < 1024) ? refToEntry[ref] : (s16)-1;

		if (e16 >= 0)
		{
			s_trackMesh.animTex[e16].vertCount++;
			total++;
		}
	}

	if (total == 0)
	{
		return;
	}

	s_trackMesh.animTexVerts = (u32 *)malloc((size_t)total * sizeof(u32));

	if (s_trackMesh.animTexVerts == NULL)
	{
		for (e = 0; e < s_trackMesh.animTexCount; e++)
		{
			s_trackMesh.animTex[e].vertCount = 0;
		}
		return;
	}

	{
		u32 cursor = 0;

		for (e = 0; e < s_trackMesh.animTexCount; e++)
		{
			s_trackMesh.animTex[e].vertFirst = cursor;
			cursor += s_trackMesh.animTex[e].vertCount;
			s_trackMesh.animTex[e].vertCount = 0; /* re-counted as a fill cursor below */
		}
	}

	for (i = 0; i < s_trackMesh.vertCount; i++)
	{
		u16 ref = s_trackMesh.verts[i].texRef;
		s16 e16 = (ref < 1024) ? refToEntry[ref] : (s16)-1;

		if (e16 >= 0)
		{
			struct NativeTrackAnimTexEntry *entry = &s_trackMesh.animTex[e16];

			s_trackMesh.animTexVerts[entry->vertFirst + entry->vertCount] = i;
			entry->vertCount++;
		}
	}

	s_trackMesh.animTexVertCount = total;
}

static void NativeTrackAnim_FlushQueuedUV(void)
{
	if (s_animPatchCount == 0)
	{
		return;
	}

#if defined(__3DS__)
	N3DS_TrackGeom_PatchUV(s_animPatchIndex, s_animPatchUV, (int)s_animPatchCount);
#endif

	s_animPatchCount = 0;
}

static void NativeTrackAnim_QueueVertexUV(u32 vertIndex, const struct NativeTrackAtlasRect *rect, u8 pageU, u8 pageV)
{
	s16 u, v;
	int atlasU, atlasV;

	if (vertIndex > 0xffff)
	{
		return;
	}

	NativeTrackAtlas_EncodeUV(rect, pageU, pageV, &u, &v, &atlasU, &atlasV);

	/* The same in-rect self-check the one-time upload runs ([TRKUV]), applied
	 * to every animation frame rather than only the one that happened to be up
	 * at load. A frame whose corners fall outside its own rect is sampling a
	 * NEIGHBOURING texture -- the quilt failure mode in an animation costume,
	 * and precisely the class of bug that stayed hidden for ten sessions
	 * because it looks like plausible texture rather than like an error.
	 * Counted, never asserted. */
	if ((atlasU < (int)rect->atlasX) || (atlasU >= (int)(rect->atlasX + rect->allocW)) || (atlasV < (int)rect->atlasY)
	    || (atlasV >= (int)(rect->atlasY + rect->allocH)))
	{
		s_animPatchOutOfRect++;
	}

	s_animPatchIndex[s_animPatchCount] = (u16)vertIndex;
	s_animPatchUV[s_animPatchCount * 2 + 0] = u;
	s_animPatchUV[s_animPatchCount * 2 + 1] = v;
	s_animPatchCount++;

	if (s_animPatchCount == TRACK_ANIM_PATCH_CHUNK)
	{
		NativeTrackAnim_FlushQueuedUV();
	}
}

static void NativeTrackAnim_FlushQueuedVerts(void)
{
	if (s_vertPatchCount == 0)
	{
		return;
	}

#if defined(__3DS__)
	N3DS_TrackGeom_PatchPosColor(s_vertPatchIndex, s_vertPatchXYZ, s_vertPatchRGB, (int)s_vertPatchCount);
#endif

	s_vertPatchCount = 0;
}

static void NativeTrackAnim_QueueVertexPos(u32 vertIndex, const struct NativeTrackVert *vert)
{
	if (vertIndex > 0xffff)
	{
		return;
	}

	s_vertPatchIndex[s_vertPatchCount] = (u16)vertIndex;
	s_vertPatchXYZ[s_vertPatchCount * 3 + 0] = vert->x;
	s_vertPatchXYZ[s_vertPatchCount * 3 + 1] = vert->y;
	s_vertPatchXYZ[s_vertPatchCount * 3 + 2] = vert->z;
	s_vertPatchRGB[s_vertPatchCount * 3 + 0] = vert->r;
	s_vertPatchRGB[s_vertPatchCount * 3 + 1] = vert->g;
	s_vertPatchRGB[s_vertPatchCount * 3 + 2] = vert->b;
	s_vertPatchCount++;

	if (s_vertPatchCount == TRACK_ANIM_PATCH_CHUNK)
	{
		NativeTrackAnim_FlushQueuedVerts();
	}
}

/*
 * The animated-VERTEX half: re-read the moving water vertices from the level's
 * own pool and push them at the GPU copy.
 *
 * This is the reason animated-vertex leaves are kept at all now instead of
 * being dropped whole (which is what put the holes in Polar Pass). srcVert
 * carries provenance per resident vertex and is TRACK_SRCVERT_NONE for anything
 * the weld pass moved -- re-copying one of those would reopen the T-junction
 * crack the weld exists to close, on exactly the water edges this animates. So
 * "frozen but welded" is chosen deliberately for those, and counted
 * (animWelded) rather than left to be discovered.
 *
 * Position AND color: AnimateQuadVertex writes both (LevVertex.pos and
 * .color_hi), so patching position alone would animate the shape of a water
 * surface while its shading stayed fixed at load.
 */
static u32 NativeTrackAnim_PatchVertices(void)
{
	u32 i;
	u32 patched = 0;

	if ((s_trackMesh.animVertCount == 0) || (s_trackMesh.animVerts == NULL) || (s_trackMesh.animSrcArray == NULL) || (s_trackMesh.srcVert == NULL))
	{
		return 0;
	}

	for (i = 0; i < s_trackMesh.animVertCount; i++)
	{
		u32 vi = s_trackMesh.animVerts[i];
		struct NativeTrackVert *vert;
		const struct LevVertex *src;
		u16 sv;

		if (vi >= s_trackMesh.vertCount)
		{
			continue;
		}

		sv = s_trackMesh.srcVert[vi];

		if (sv == TRACK_SRCVERT_NONE)
		{
			continue;
		}

		/* Re-bounded here even though the build-time filter already applied the
		 * identical test, because this read happens a frame or more later,
		 * against a pointer this file does not own. Counted rather than
		 * silently skipped: a non-zero srcOOB is direct evidence that the
		 * source array is not the one these indices were built against, which
		 * is a different bug from the patch simply doing nothing. */
		if ((u32)sv >= s_trackMesh.animSrcCount)
		{
			s_vertPatchSrcOOB++;
			continue;
		}

		src = &s_trackMesh.animSrcArray[sv];
		vert = &s_trackMesh.verts[vi];

		vert->x = src->pos.x;
		vert->y = src->pos.y;
		vert->z = src->pos.z;
		vert->r = src->color_hi[0];
		vert->g = src->color_hi[1];
		vert->b = src->color_hi[2];

		NativeTrackAnim_QueueVertexPos(vi, vert);
		patched++;
	}

	NativeTrackAnim_FlushQueuedVerts();
	return patched;
}

/*
 * The per-frame half. Repoints every resident vertex of every animated texture
 * chain at the frame the game has advanced to -- turbo-pad arrows, waterfalls,
 * lava. Without it the resident renderer shows whichever frame happened to be
 * active when the level loaded, forever.
 *
 * WHERE THIS RUNS MATTERS. It writes into the single linearAlloc block
 * N3DS_TrackGeom_Upload handed the GPU, and on render-thread builds core 2
 * draws out of that block. It is therefore called from CtrGpu_ExecDrawOTag --
 * i.e. on the WALK thread, whichever that is, at the head of a walk. The
 * render queue is FIFO with one worker, so the previous frame's walk has
 * already finished by then and the next one has not started: no fence, no
 * double buffer, and no argument about whether some other call site happens to
 * be a barrier today.
 *
 * The game-state it reads (AnimTex.frameCurr, and the frame's own
 * TextureLayout) is written by CTR_CycleTex_LEV on the game thread early in
 * the frame. Reading it a walk later can only ever show one frame of a
 * scrolling texture early; there is no tear that produces an invalid UV,
 * because a frame index is read once and everything else follows from it.
 *
 * Work is proportional to the vertices of chains that ACTUALLY CHANGED frame
 * (curFrame latch), not to the track: a chain with frameSkip set holds for
 * several vblanks and costs nothing on the frames in between.
 *
 * RESIDUAL, stated rather than papered over: the mesh REBUILD runs on the game
 * thread (NativeTrackMesh_Frame, from the 1P gate) and starts by freeing
 * s_trackMesh, so a level transition can in principle free these arrays
 * between this function's builtFor check and its use of them. That is the
 * exact exposure the existing resident DRAW path already has against
 * leafRanges, and closing it properly means a build/teardown handshake for the
 * whole mesh, not a lock bolted onto this one reader. Matched to the existing
 * path deliberately; do not read this comment as a claim that it is safe.
 */
void NativeTrackAnim_PatchFrame(void)
{
	u32 e;
	u32 patched = 0;
	u32 skipUnit = 0, skipRect = 0;

	/*
	 * TWO conditions, and the second one is the important one.
	 *
	 * builtFor says a mesh exists. It does NOT say the level that mesh was
	 * built from is still loaded -- and animSrcArray is a borrowed pointer
	 * into that level's own vertex pool, which MEMPACK frees and reuses on a
	 * level change. This function runs from DrawOTag, which is called from far
	 * more than the race path: FMV, the error screen, load-time clears. Every
	 * one of those frames would otherwise walk 13,000 indices into a freed
	 * arena. (Roo's Tubes is the first level with a non-empty animated-vertex
	 * list, and a PC attract run reaching it died with SIGSEGV -- this is the
	 * fix for that.)
	 *
	 * The frame snapshot is exactly the "this level is live and rendering"
	 * signal, and it already crosses to the walk thread safely: Record sets
	 * valid only after NativeTrackMesh_IsBuiltFor passes, and Clear
	 * invalidates it every ResetDB, so a frame that did not render the track
	 * publishes valid=0 by construction. Reusing it here costs one load and
	 * needs no new protocol.
	 */
	if (s_trackMesh.builtFor == NULL)
	{
		return;
	}

	{
		const struct NativeTrackFrameSnapshot *snap = NativeTrackFrame_ForWalk();

		if ((snap == NULL) || (snap->valid == 0))
		{
			return;
		}
	}

	NativePerf_BeginScope(NATIVE_PERF_BUCKET_TRACK_ANIM);

	patched = NativeTrackAnim_PatchVertices();

	/* Deliberately NOT an early return: a track can have moving water and no
	 * animated textures, and the heartbeat below is the only runtime evidence
	 * this function ran at all. An early return here would report "steps=0" as
	 * silence rather than as a number. */
	if ((s_trackMesh.animTexCount == 0) || (s_trackMesh.animTexVerts == NULL))
	{
		goto report;
	}

	for (e = 0; e < s_trackMesh.animTexCount; e++)
	{
		struct NativeTrackAnimTexEntry *entry = &s_trackMesh.animTex[e];
		const struct AnimTex *anim = entry->anim;
		struct IconGroup4 **frames;
		const struct TextureLayout *layout;
		const struct NativeTrackAtlasRect *rect;
		s16 rectIdx;
		int f;
		u32 v;

		if ((anim == NULL) || (entry->vertCount == 0))
		{
			continue;
		}

		f = (int)anim->frameCurr;

		if ((f < 0) || (f >= (int)entry->numFrames))
		{
			continue;
		}

		if (f == (int)entry->curFrame)
		{
			continue;
		}

		rectIdx = s_trackMesh.animFrameRect[entry->frameSlot + (u32)f];

		if ((rectIdx < 0) || (rectIdx >= s_trackMesh.atlas.rectCount))
		{
			skipRect++;
			continue;
		}

		rect = &s_trackMesh.atlas.rects[rectIdx];

		if ((rect->atlasUnit == TRACK_ATLAS_UNIT_INVALID) || (rect->atlasUnit != entry->buildUnit))
		{
			skipUnit++;
			continue;
		}

		frames = ANIMTEX_GETARRAY((struct AnimTex *)anim);

		if (frames[f] == NULL)
		{
			skipRect++;
			continue;
		}

		layout = (const struct TextureLayout *)frames[f] + 2; /* near */

		{
			const u8 *uvBytes = &layout->u0;

			for (v = 0; v < entry->vertCount; v++)
			{
				u32 vi = s_trackMesh.animTexVerts[entry->vertFirst + v];
				struct NativeTrackVert *vert;
				int corner;

				if (vi >= s_trackMesh.vertCount)
				{
					continue;
				}

				vert = &s_trackMesh.verts[vi];
				corner = (vert->faceFlags >> 3) & 3;

				/* Same corner, different frame's layout -- the per-face
				 * rotation/flip was already resolved into faceFlags at emit
				 * time, so this is a straight re-read, not a re-derivation. */
				vert->u = uvBytes[sTrackUvByteOfs[corner]];
				vert->v = uvBytes[sTrackUvByteOfs[corner] + 1];

				NativeTrackAnim_QueueVertexUV(vi, rect, vert->u, vert->v);
				patched++;
			}
		}

		entry->curFrame = (s16)f;
		s_animPatchSteps++;
	}

	NativeTrackAnim_FlushQueuedUV();

report:
	s_trackMesh.animPatchSkipUnit = skipUnit;
	s_trackMesh.animPatchSkipRect = skipRect;
	s_trackMesh.animPatchVertsLast = patched;

	/* Counters only. NOTHING here logs: this function runs on the walk thread,
	 * and the 3DS log is an unlocked FILE* (n3ds_backend.c) -- two threads
	 * inside fputs on it is a corrupted log at best. NativeTrackAnim_Report
	 * below does the emitting, from the game thread. */
	s_animPatchFrames++;

	NativePerf_EndScope(NATIVE_PERF_BUCKET_TRACK_ANIM);
}

/*
 * Game-thread reporter for the patcher above, called once per frame from
 * NativeTrackMesh_Frame.
 *
 * The heartbeat exists to make ONE hardware sitting enough. If turbo pads still
 * do not move, this line separates three bugs that look identical through a
 * camera: `steps=0` means the patcher never ran, `steps` climbing means it ran
 * and the frame swap is wrong, and non-zero skipUnit/skipRect means those
 * frames were unswappable in the first place.
 *
 * The counters are written on the walk thread and read here without
 * synchronisation. That is deliberate and safe in the only sense that matters:
 * they are u32 diagnostics, a torn read costs one wrong number in one log line,
 * and the alternative -- a lock on the per-vertex path -- would cost more than
 * the thing being measured.
 */
static void NativeTrackAnim_Report(void)
{
	if ((s_animPatchOutOfRect != 0) && (s_animPatchOutOfRectLogged == 0))
	{
		s_animPatchOutOfRectLogged = 1;
		Platform_Log("[TRKANIM] WARNING outOfRect=%u -- an animation frame is sampling outside its own atlas rect\n", s_animPatchOutOfRect);
	}

	/* Frame-counted rather than time-counted -- ~10s at 60Hz -- so it needs no
	 * clock and stays comparable between a capped and an uncapped build. */
	if (s_animPatchFrames >= 600)
	{
		Platform_Log("[TRKANIM] window steps=%u vertsLast=%u skipUnit=%u skipRect=%u animVerts=%u srcOOB=%u\n", s_animPatchSteps,
		             s_trackMesh.animPatchVertsLast, s_trackMesh.animPatchSkipUnit, s_trackMesh.animPatchSkipRect, s_trackMesh.animVertCount,
		             s_vertPatchSrcOOB);
		s_animPatchFrames = 0;
		s_animPatchSteps = 0;
	}
}

#if defined(__3DS__)
/*
 * Rung 2b: decode + upload driver. Reuses N3DS_DecodeAndTilePage
 * (native_renderer_3ds.c, included earlier in this unity TU -- see main.c's
 * include order) for CLUT resolution and per-texel visibility, so the track
 * atlas's texel results are IDENTICAL to the decode cache's, not a
 * reimplementation of the same rules. N3DS_TEXVIEW_OPAQUE (any non-zero
 * texel visible, punch-through alpha) is the only view baked here: nothing
 * samples these atlases yet (rung 3 draws), and which of the decode cache's
 * three STP views a track triangle would need is a per-draw blend-mode
 * question, not something a one-time load-time upload can predict.
 *
 * One-time per level load, called right after NativeTrackAtlas_Pack from
 * NativeTrackMesh_Frame. The static staging buffer is sized to one PSX page
 * (256x256 texels) -- NativeTrackAtlas_Pack caps allocW/allocH at 256 itself
 * (review finding #2), so the >256 guard in the loop below is defensive,
 * not reachable.
 */

/* Cross-TU constant check (review finding #5): the packer's own
 * TRACK_ATLAS_DIM/MAX_UNITS and the backend pool's N3DS_TRACK_ATLAS_DIM/
 * MAX_UNITS (n3ds_backend.h) must agree -- both size the SAME pool from
 * opposite sides of the plain-scalar boundary, so a drift between them
 * would silently place rects outside what the backend actually allocated.
 * Caught at compile time rather than trusted by inspection. */
CTR_STATIC_ASSERT(TRACK_ATLAS_DIM == N3DS_TRACK_ATLAS_DIM);
CTR_STATIC_ASSERT(TRACK_ATLAS_MAX_UNITS == N3DS_TRACK_ATLAS_MAX_UNITS);

static u16 s_trackAtlasStaging[256 * 256]; /* one PSX page, RGBA5551 tiled -- 128KB, no malloc */

/*
 * Returns 0 only when N3DS_TrackAtlas_Alloc returned fewer units than this
 * level's pack needs -- linear-heap pressure. The caller withholds
 * NativeTrackMesh_Frame's rebuild latch on a 0 so the WHOLE build (mesh AND
 * atlas, not just the upload) retries next frame, the same contract the
 * calloc-failure paths earlier in this file already use. Returns 1
 * otherwise: per-rect skips (oversize, unresolved unit) are permanent for
 * this content -- withholding the latch over those would rebuild forever
 * without ever latching.
 *
 * GIVEUP (Azahar validation, post review round): the alloc shortfall above
 * is NOT reliably transient -- nothing frees the linear heap between
 * frames on its own, so an unconditional "withhold the latch and retry"
 * can retry forever, rerunning the entire census+mesh+pack+decode+upload
 * (~50ms observed) every single frame. mesh/numQuadBlock/numVertex
 * fingerprint THIS level (reset the attempt counter when it changes, same
 * fingerprint the outer latch uses) so repeated genuine failures on the
 * same content are distinguishable from a fresh level's first attempt.
 * After 4 failed attempts on the same level, latch anyway with whatever
 * partial atlas is currently uploaded (better than a permanently pegged
 * frame time) and log a distinct GIVEUP line so the condition shows up in
 * any log harvest, not just as a wall of repeated normal lines.
 */
/*
 * Which atlas unit a triangle samples, keyed off its first vertex (all three
 * share a face and therefore a texRef). Returns N3DS_TRACK_ATLAS_MAX_UNITS as a
 * catch-all "no placed unit" bin -- out-of-range vertex, out-of-range texRef,
 * unplaced rect, or a rect the backend declined to upload all land there and
 * draw untextured rather than sampling whatever happens to sit at atlas origin.
 */
static u32 NativeTrackMesh_UnitBinForIndex(u16 vertexIndex)
{
	const unsigned texRefMax = (unsigned)(sizeof(s_trackMesh.texRectIndex) / sizeof(s_trackMesh.texRectIndex[0]));
	u16 texRef;
	s16 rectIdx;
	u8 unit;

	if ((u32)vertexIndex >= s_trackMesh.vertCount)
	{
		return N3DS_TRACK_ATLAS_MAX_UNITS;
	}

	texRef = s_trackMesh.verts[vertexIndex].texRef;
	if ((unsigned)texRef >= texRefMax)
	{
		return N3DS_TRACK_ATLAS_MAX_UNITS;
	}

	rectIdx = s_trackMesh.texRectIndex[texRef];
	if ((rectIdx < 0) || (rectIdx >= s_trackMesh.atlas.rectCount))
	{
		return N3DS_TRACK_ATLAS_MAX_UNITS;
	}

	unit = s_trackMesh.atlas.rects[rectIdx].atlasUnit;
	return (unit >= N3DS_TRACK_ATLAS_MAX_UNITS) ? (u32)N3DS_TRACK_ATLAS_MAX_UNITS : (u32)unit;
}

/*
 * Rung 3b: split each leaf's index range into one sub-range per ATLAS UNIT.
 *
 * A draw binds exactly one texture, and a leaf's faces routinely land in
 * different atlas units, so "one draw per leaf" and "textured" are mutually
 * exclusive. NativeTrackLeafRange has carried a reserved `atlasBin` field since
 * rung 2a for exactly this; this is what fills it.
 *
 * Why a POST-pass rather than binning inside the emit loop: the rect set is
 * gathered *by* that loop and only packed afterwards, so no face knows its
 * atlas unit while it is being emitted. Running after
 * NativeTrackAtlas_UploadToBackend matters too -- that function resets
 * atlasUnit to the sentinel on every skip path, so binning any earlier could
 * bin against placements the backend then declined to upload.
 *
 * Triangles whose rect never got placed keep the sentinel bin. They are not
 * dropped: they still draw, untextured, exactly as rung 3a drew everything.
 *
 * Exact sizing, two passes: count the distinct units per leaf to get the final
 * range count, allocate that, then fill. The old code sized leafRanges at one
 * per BSP node; binning can only grow that, and guessing a multiplier would
 * either waste heap the atlas needs or overflow on a track nobody tested.
 *
 * Returns 0 on allocation failure, which the caller treats like every other
 * upload failure here: withhold the latch and retry the whole build next frame
 * rather than latch a half-binned mesh.
 */
static int NativeTrackMesh_BinLeavesByAtlasUnit(void)
{
	struct NativeTrackLeafRange *newRanges = NULL;
	u16 *newIndices = NULL;
	u32 newRangeCount = 0;
	u32 newIndexCursor = 0;
	u32 leaf;

	if ((s_trackMesh.leafCount == 0) || (s_trackMesh.indexCount == 0))
	{
		return 1;
	}

	/* Pass 1 -- how many ranges will there be? */
	for (leaf = 0; leaf < s_trackMesh.leafCount; leaf++)
	{
		const struct NativeTrackLeafRange *range = &s_trackMesh.leafRanges[leaf];
		u8 seen[N3DS_TRACK_ATLAS_MAX_UNITS + 1];
		u32 tri;

		memset(seen, 0, sizeof(seen));
		for (tri = 0; tri + 2 < range->indexCount; tri += 3)
		{
			/* A FLAG, deliberately not a count: a u8 counter wraps to 0 at
			 * exactly 256 triangles in one bin, which real leaves reach, and an
			 * undercount here under-allocates the array pass 2 then writes
			 * past. Presence is all this pass needs. */
			seen[NativeTrackMesh_UnitBinForIndex(s_trackMesh.indices[range->firstIndex + tri])] = 1;
		}
		for (u32 b = 0; b < sizeof(seen); b++)
		{
			if (seen[b] != 0)
			{
				newRangeCount++;
			}
		}
	}

	newRanges = (struct NativeTrackLeafRange *)malloc((size_t)newRangeCount * sizeof(struct NativeTrackLeafRange));
	newIndices = (u16 *)malloc((size_t)s_trackMesh.indexCount * sizeof(u16));

	if ((newRanges == NULL) || (newIndices == NULL))
	{
		free(newRanges);
		free(newIndices);
		return 0;
	}

	/* Pass 2 -- emit, grouped. Bins are walked in ascending order so a leaf's
	 * sub-ranges stay contiguous AND ordered, which is what lets the frame
	 * recorder find them all by walking forward while bspIndex matches. */
	newRangeCount = 0;
	for (leaf = 0; leaf < s_trackMesh.leafCount; leaf++)
	{
		const struct NativeTrackLeafRange *src = &s_trackMesh.leafRanges[leaf];
		int firstForThisLeaf = 1;

		for (u32 bin = 0; bin <= N3DS_TRACK_ATLAS_MAX_UNITS; bin++)
		{
			const u32 rangeStart = newIndexCursor;
			u32 tri;

			for (tri = 0; tri + 2 < src->indexCount; tri += 3)
			{
				const u16 i0 = s_trackMesh.indices[src->firstIndex + tri];

				if (NativeTrackMesh_UnitBinForIndex(i0) != bin)
				{
					continue;
				}
				newIndices[newIndexCursor++] = i0;
				newIndices[newIndexCursor++] = s_trackMesh.indices[src->firstIndex + tri + 1];
				newIndices[newIndexCursor++] = s_trackMesh.indices[src->firstIndex + tri + 2];
			}

			if (newIndexCursor == rangeStart)
			{
				continue;
			}

			newRanges[newRangeCount] = *src; /* bspIndex, box, drawOrder range all carry over unchanged */
			newRanges[newRangeCount].atlasBin = (u16)bin;
			newRanges[newRangeCount].firstIndex = rangeStart;
			newRanges[newRangeCount].indexCount = newIndexCursor - rangeStart;

			if (firstForThisLeaf)
			{
				/* The reverse map points at the FIRST sub-range; the recorder
				 * walks forward from there. Rewritten rather than left alone
				 * because range indices have all shifted. */
				s_trackMesh.bspToLeafIndex[src->bspIndex] = (s32)newRangeCount;
				firstForThisLeaf = 0;
			}
			newRangeCount++;
		}
	}

	free(s_trackMesh.leafRanges);
	free(s_trackMesh.indices);
	s_trackMesh.leafRanges = newRanges;
	s_trackMesh.leafCount = newRangeCount;
	s_trackMesh.indices = newIndices;
	s_trackMesh.indexCount = newIndexCursor;
	return 1;
}

static int NativeTrackAtlas_UploadToBackend(struct mesh_info *mesh, int numQuadBlock, int numVertex)
{
	static struct mesh_info *s_retryMesh = NULL;
	static int s_retryQb = -1, s_retryVtx = -1;
	static int s_retryCount = 0;
	struct NativeTrackAtlasLayout *atlas = &s_trackMesh.atlas;
	unsigned long long freq = N3DS_GetTickFrequency();
	unsigned long long tStart;
	int allocated;
	int rectsUploaded = 0, skipOversize = 0, skipNoUnit = 0;
	u32 texelsDecoded = 0;
	long long elapsedUs;
	unsigned int freeKiB;

	if ((mesh != s_retryMesh) || (numQuadBlock != s_retryQb) || (numVertex != s_retryVtx))
	{
		s_retryMesh = mesh;
		s_retryQb = numQuadBlock;
		s_retryVtx = numVertex;
		s_retryCount = 0;
	}

	if (atlas->unitsUsed <= 0)
	{
		return 1;
	}

	tStart = N3DS_GetTickCount();
	allocated = N3DS_TrackAtlas_Alloc(atlas->unitsUsed);

	for (int i = 0; i < atlas->rectCount; i++)
	{
		struct NativeTrackAtlasRect *rect = &atlas->rects[i];
		TexFormat format;

		/* Unplaced (dropped by the packer) or in a unit the pool failed to
		 * allocate -- never a partial/garbage upload. Reset the sentinel
		 * explicitly (review finding #3): a rect the packer DID place still
		 * carries a real-looking unit index here if its unit's allocation
		 * failed, and rung 3 must never bind that. */
		if ((rect->atlasUnit == TRACK_ATLAS_UNIT_INVALID) || ((int)rect->atlasUnit >= allocated))
		{
			rect->atlasUnit = TRACK_ATLAS_UNIT_INVALID;
			skipNoUnit++;
			continue;
		}

		/* Defensive only -- see the function comment; NativeTrackAtlas_Pack
		 * caps allocW/allocH at 256, so this can't currently trigger. */
		if (((int)rect->allocW > 256) || ((int)rect->allocH > 256))
		{
			rect->atlasUnit = TRACK_ATLAS_UNIT_INVALID;
			skipOversize++;
			continue;
		}

		format = GetTPageFormat(rect->tpage);

		/* srcU/srcV, NOT minU/minV -- see the struct comment on
		 * NativeTrackAtlasRect (review finding #1: minV can put the decode
		 * window past the end of vram[] for a bottom-half page). */
		N3DS_DecodeAndTilePage((s16)rect->tpage, (s16)rect->clut, format, N3DS_TEXVIEW_OPAQUE, rect->srcU, rect->srcV, rect->allocW, rect->allocH,
		                       rect->allocW / 8, s_trackAtlasStaging);
		N3DS_TrackAtlas_UploadRect(rect->atlasUnit, rect->atlasX, rect->atlasY, rect->allocW, rect->allocH, s_trackAtlasStaging);

		texelsDecoded += (u32)rect->allocW * (u32)rect->allocH;
		rectsUploaded++;
	}

	/* us, not ms (review finding #9): an ms-rounded field silently prints 0
	 * for a light level's upload, and hardware has since shown the heavy
	 * end (57 rects / 330KB decoded) running ~50ms -- us reads correctly at
	 * either extreme instead of rounding one of them away. */
	elapsedUs = (long long)(((N3DS_GetTickCount() - tStart) * 1000000ull) / (freq ? freq : 1ull));
	freeKiB = (unsigned int)(N3DS_LinearFreeBytes() / 1024ul);

	Platform_Log("[TRKATLAS3DS] units=%d/%d rects=%d/%d skipOversize=%d skipNoUnit=%d texels=%u KB=%u us=%d freeKiB=%u\n", allocated, atlas->unitsUsed,
	             rectsUploaded, atlas->rectCount, skipOversize, skipNoUnit, texelsDecoded, (texelsDecoded * 2u) / 1024u, (int)elapsedUs, freeKiB);

	if (allocated < atlas->unitsUsed)
	{
		s_retryCount++;

		if (s_retryCount >= 4)
		{
			Platform_Log("[TRKATLAS3DS] GIVEUP units=%d/%d after %d tries -- latching with partial atlas\n", allocated, atlas->unitsUsed, s_retryCount);
			s_retryCount = 0;
			return 1;
		}

		return 0;
	}

	s_retryCount = 0;
	return 1;
}

/*
 * Rung 3a step 2: repack + upload the resident mesh's GPU-visible geometry.
 * struct NativeTrackVert (this file's own 16B build-time layout, above) still
 * carries texRef/faceFlags the GPU never reads; N3DS_TrackVertex
 * (n3ds_backend.h, 14B) is the plain-scalar mirror N3DS_C3DInit's
 * s_trackAttrInfo actually consumes. One-time-per-level repack into a
 * transient malloc'd staging buffer -- N3DS_TrackGeom_Upload memcpies it
 * into its own linearAlloc block immediately, so nothing here needs to
 * outlive this call.
 *
 * GIVEUP (review fix, FIX 3 -- mirrors NativeTrackAtlas_UploadToBackend's own
 * mechanism above, same reasoning): a persistently full linear heap makes a
 * bare "return 0 on failure" retry FOREVER, since nothing frees the heap
 * between frames on its own -- every one of those retries reruns the WHOLE
 * build this function is only the tail of (weld pass, per-face emit over up
 * to 20000 quadblocks, atlas repack, atlas decode+upload), not just this
 * upload. Same fingerprint (mesh/numQuadBlock/numVertex, reset when the level
 * changes), same 4-attempt ceiling, same distinct GIVEUP log tag. Unlike the
 * atlas's giveup, there is no partial win to latch here -- N3DS_TrackGeom_Upload
 * either fully succeeds or fully fails -- so giving up means latching this
 * level with NO resident geometry rather than a stale one:
 * N3DS_TrackGeom_Free() is called explicitly on that path (on top of every
 * failing call already leaving s_trackGeomReady at 0 via N3DS_TrackGeom_Upload's
 * own Free-first), so N3DS_TrackDraw_Begin's s_trackGeomReady check is
 * guaranteed to see "not ready" and draw nothing, rather than this function's
 * contract depending on a reader tracing into n3ds_backend.c to confirm it.
 */
static int NativeTrackGeom_UploadToBackend(struct mesh_info *mesh, int numQuadBlock, int numVertex)
{
	static struct mesh_info *s_retryMesh = NULL;
	static int s_retryQb = -1, s_retryVtx = -1;
	static int s_retryCount = 0;
	N3DS_TrackVertex *staging;
	int ok;
	u32 i;

	if ((mesh != s_retryMesh) || (numQuadBlock != s_retryQb) || (numVertex != s_retryVtx))
	{
		s_retryMesh = mesh;
		s_retryQb = numQuadBlock;
		s_retryVtx = numVertex;
		s_retryCount = 0;
	}

	if ((s_trackMesh.vertCount == 0) || (s_trackMesh.indexCount == 0))
	{
		/* An empty resident mesh (every leaf excluded, or nothing textured)
		 * is not a failure -- nothing to upload, nothing to draw. Free any
		 * PRIOR level's geometry so a stale buffer never survives into a
		 * level that builds none of its own, and reset the retry counter so
		 * a LATER level that genuinely fails doesn't inherit an empty
		 * level's count. */
		N3DS_TrackGeom_Free();
		s_retryCount = 0;
		return 1;
	}

	staging = (N3DS_TrackVertex *)malloc((size_t)s_trackMesh.vertCount * sizeof(N3DS_TrackVertex));

	if (staging != NULL)
	{
		u32 uvChecked = 0, uvOutOfRect = 0, uvOutOfAtlas = 0, uvUnplaced = 0;

		for (i = 0; i < s_trackMesh.vertCount; i++)
		{
			const struct NativeTrackVert *src = &s_trackMesh.verts[i];
			N3DS_TrackVertex *dst = &staging[i];

			dst->x = src->x;
			dst->y = src->y;
			dst->z = src->z;

			/* Rung 3b: rewrite the baked PAGE-space UV into ABSOLUTE ATLAS
			 * texel coords, which is why the GPU-side field is s16 while the
			 * resident one is u8 -- a page coordinate is 0..255, an atlas one
			 * is 0..511 and carries the rect's placement.
			 *
			 * The formula and, more importantly, WHICH fields it reads are
			 * fixed by NativeTrackAtlasRect's own comment: srcU/srcV, never
			 * minU/minV. They differ whenever the packer had to pull the decode
			 * origin up to keep a tall rect from overrunning the page half, and
			 * using minV there would shift every texel in that rect.
			 *
			 * Unplaced or unuploaded rects (atlasUnit sentinel) leave the UV at
			 * 0: the vertex still draws, just sampling the atlas origin. The
			 * draw side is what must not bind a unit for those -- see the
			 * per-unit binning below. */
			{
				const unsigned texRefMax = (unsigned)(sizeof(s_trackMesh.texRectIndex) / sizeof(s_trackMesh.texRectIndex[0]));
				const s16 rectIdx = (src->texRef < texRefMax) ? s_trackMesh.texRectIndex[src->texRef] : (s16)-1;

				if ((rectIdx >= 0) && (rectIdx < s_trackMesh.atlas.rectCount) && (s_trackMesh.atlas.rects[rectIdx].atlasUnit != TRACK_ATLAS_UNIT_INVALID))
				{
					const struct NativeTrackAtlasRect *rect = &s_trackMesh.atlas.rects[rectIdx];
					int atlasU, atlasV;

					/* The 2x/centre/V-flip encoding lives in ONE function, shared
					 * with the per-frame AnimTex patcher -- see
					 * NativeTrackAtlas_EncodeUV for the whole derivation and for
					 * the quilt bug a second copy of it caused. */
					NativeTrackAtlas_EncodeUV(rect, src->u, src->v, &dst->u, &dst->v, &atlasU, &atlasV);

					/* Self-check, because "does it look right" is not available:
					 * attract never holds a scene long enough for a same-frame
					 * A/B, and a wrong UV shows up as plausible-looking texture
					 * rather than as an obvious error. Every translated coord
					 * must land inside its own rect's allocated box; anything
					 * outside is sampling a NEIGHBOURING rect's texels, which
					 * is precisely the s16 quilt failure mode wearing a
					 * different hat. Counted, not asserted -- one number in
					 * [TRKUV] beats a crash on a track nobody has loaded yet. */
					/* Validate the RAW atlas coords, before the 2x/flip encode
					 * -- checking the encoded values would just be checking the
					 * encoding against itself. */
					uvChecked++;
					if ((atlasU < (int)rect->atlasX) || (atlasU >= (int)(rect->atlasX + rect->allocW)) || (atlasV < (int)rect->atlasY)
					    || (atlasV >= (int)(rect->atlasY + rect->allocH)))
					{
						uvOutOfRect++;
					}
					if ((atlasU < 0) || (atlasV < 0) || (atlasU >= N3DS_TRACK_ATLAS_DIM) || (atlasV >= N3DS_TRACK_ATLAS_DIM))
					{
						uvOutOfAtlas++;
					}
				}
				else
				{
					dst->u = 0;
					dst->v = 0;
					uvUnplaced++;
				}
			}
			dst->r = src->r;
			dst->g = src->g;
			dst->b = src->b;
			/* Bias +128 -- see N3DS_TrackVertex's own comment (n3ds_backend.h)
			 * for why an UNSIGNED byte group (shared with r/g/b) can't carry a
			 * raw signed value without corrupting r/g/b=255 into -1. */
			dst->drawOrder = (u8)((int)src->drawOrder + 128);
		}

		/* outOfRect/outOfAtlas MUST both be 0. Non-zero outOfRect means verts
		 * sample a neighbouring rect (the quilt failure mode); outOfAtlas means
		 * they sample off the texture entirely. unplaced is benign -- those
		 * draw untextured by design. */
		Platform_Log("[TRKUV] checked=%u outOfRect=%u outOfAtlas=%u unplaced=%u\n", uvChecked, uvOutOfRect, uvOutOfAtlas, uvUnplaced);

		ok = N3DS_TrackGeom_Upload(staging, (int)s_trackMesh.vertCount, (int)sizeof(N3DS_TrackVertex), s_trackMesh.indices, (int)s_trackMesh.indexCount);

		free(staging);
	}
	else
	{
		/* Staging malloc failure is the same "couldn't get memory" signal as
		 * a linearAlloc failure inside N3DS_TrackGeom_Upload -- count it
		 * toward the same retry ceiling rather than returning early. */
		ok = 0;
	}

	if (ok)
	{
		s_retryCount = 0;
		Platform_Log("[TRKGEOM3DS] verts=%u idx=%u KB=%u\n", s_trackMesh.vertCount, s_trackMesh.indexCount,
		             (u32)((s_trackMesh.vertCount * sizeof(N3DS_TrackVertex) + s_trackMesh.indexCount * sizeof(u16)) / 1024));
		return 1;
	}

	s_retryCount++;

	if (s_retryCount >= 4)
	{
		Platform_Log("[TRKGEOM3DS] GIVEUP after %d tries -- latching level with NO resident geometry\n", s_retryCount);
		s_retryCount = 0;
		N3DS_TrackGeom_Free();
		return 1;
	}

	return 0;
}
#endif /* __3DS__ */

static u16 NativeTrackMesh_InternTexture(uintptr_t texPtr)
{
	/* Small linear intern -- track texture tables measured at <=119 pairs
	 * (s27 census), so linear search at build time is fine. */
	for (u32 i = 0; i < s_trackMesh.texCount; i++)
	{
		if (s_trackMesh.texTable[i] == texPtr)
		{
			return (u16)i;
		}
	}

	s_trackMesh.texTable[s_trackMesh.texCount] = texPtr;
	return (u16)s_trackMesh.texCount++;
}

static int NativeTrackMesh_LeafTouchesAnim(const struct BSP *bsp, const u8 *animMark, int numVertex, int numQuadBlock)
{
	int numQuads = bsp->data.leaf.numQuads;
	struct QuadBlock *range = bsp->data.leaf.ptrQuadBlockArray;

	/* Upper bound included (s27 review finding): a leaf's numQuads is asset
	 * data; never trust it past the mesh's real array size. */
	if ((range == NULL) || (numQuads <= 0) || (numQuads > numQuadBlock))
	{
		return 0;
	}

	for (int q = 0; q < numQuads; q++)
	{
		for (int slot = 0; slot < 9; slot++)
		{
			u16 vi = range[q].index[slot];

			if ((int)vi < numVertex && animMark[vi])
			{
				return 1;
			}
		}
	}

	return 0;
}

/*
 * GAP FILL (s29). The builder drops a WHOLE BSP leaf if it is flagged water or
 * if any one of its vertices is animated, and with the retail track draw
 * switched off nothing draws those leaves at all: Polar Pass loses 77 of 526.
 * The owner reported the missing pieces as ordinary road, which the exclusion
 * rule permits -- it discards the leaf, not the offending face.
 *
 * With this on, the retail path is left running and the render-list builder
 * simply declines to link any leaf the resident mesh already owns
 * (RenderLists_LinkBsp). Retail then draws exactly the leaves residency
 * refused, and the two cover the level between them with no overlap.
 *
 * It is also the falsification test for the diagnosis: if the holes persist
 * with this on, retail is not drawing that geometry either and the cause is
 * somewhere else entirely. That was worth having as a switch rather than a
 * silent fix, because "solved" has been claimed for this twice already
 * without either claim being checked.
 *
 * DEFAULT 0 -- BROKEN AS WRITTEN, s29. Filtering in RenderLists_LinkBsp is the
 * wrong place: NativeTrackFrame_Record derives the RESIDENT renderer's own
 * visible-leaf set by walking that same render list (see its bspListBase walk),
 * so withholding resident leaves from the list to prevent double-drawing also
 * blinds the resident renderer completely. On hardware the result was the exact
 * negative of the track -- only the 77 excluded chunks drew, nothing else.
 *
 * The fix is to prune AFTER the recorder has read the list rather than before
 * it is built. Record runs at MainFrame_RenderFrame:1262 and the retail draw at
 * :1290, so a prune pass between them sees a complete list and still hands
 * DrawLevelOvr1P only the gaps. Not attempted yet.
 */
int g_dbg_trackGapFill = 0;

void NativeTrackMesh_Frame(struct Level *level, struct mesh_info *mesh)
{
	/* Latch = pointer + content fingerprint (arena heaps reuse addresses --
	 * s27 review finding), and it is only recorded on SUCCESSFUL builds; the
	 * cheap-validation and calloc-failure paths below return unlatched so a
	 * transient failure retries next frame instead of pinning an empty mesh
	 * to that level forever. numBspNodes joined the fingerprint after a
	 * review finding: a level reloading into the same arena address with
	 * equal quad/vertex counts but a DIFFERENT bsp node count would otherwise
	 * pass this check while bspToLeafIndex (sized from the OLD numBspNodes)
	 * is still allocated at the old size -- see NativeTrackMesh_LeafIndexForBsp's
	 * bound, which now checks bspNodeCount as latched at build time, not
	 * mesh->numBspNodes read live (which could have already changed under
	 * a stale-but-pointer-equal mesh). */
	static int s_builtQb = -1, s_builtVtx = -1, s_builtBspNodes = -1;

	/* Before every early return below: this is the game-thread half of the
	 * per-frame animation patch, and the common case IS an early return (the
	 * mesh is already built). Reporting after the latch check would emit
	 * nothing on exactly the frames the patcher is doing its work. */
	NativeTrackAnim_Report();

	if ((level == NULL) || (mesh == NULL))
	{
		return;
	}

	/*
	 * Review fix (FIX 7, comment only): this closes the specific
	 * arena-address-reuse bug it was written for (a stale mesh at the SAME
	 * pointer with a DIFFERENT shape), but it is still a heuristic, not a
	 * guarantee -- two DIFFERENT levels sharing all three counts (quad,
	 * vertex, bsp-node) AND landing at the same reused arena address would
	 * pass this check and skip the rebuild, drawing a stale mesh. Low
	 * probability across the ~20-track roster, and a real content hash isn't
	 * worth the extra build-time cost to close it -- but that is a residual
	 * risk being accepted, not something this check actually rules out, so a
	 * later reader shouldn't mistake three matching counts for proof it's the
	 * same level.
	 */
	if ((mesh == s_trackMesh.builtFor) && (mesh->numQuadBlock == s_builtQb) && (mesh->numVertex == s_builtVtx) && (mesh->numBspNodes == s_builtBspNodes))
	{
		return;
	}

	NativeTrackMesh_Free();

	int numQuadBlock = mesh->numQuadBlock;
	int numVertex = mesh->numVertex;
	struct QuadBlock *quadBlocks = mesh->ptrQuadBlockArray;
	struct LevVertex *vertexArray = mesh->ptrVertexArray;

	if ((quadBlocks == NULL) || (vertexArray == NULL) || (numQuadBlock <= 0) || (numVertex <= 0) || (numQuadBlock > 20000) || (numVertex > 200000))
	{
		return;
	}

	/* Animated-vertex marks, shared logic with the census (kept separate on
	 * purpose: the census is a temporary instrument, this is product).
	 * configFlags & 4 discriminates water vs SCVert exactly as every retail
	 * reader does -- the inactive side's fields are unvalidated asset data. */
	u8 *animMark = (u8 *)calloc((size_t)numVertex, 1);

	if (animMark == NULL)
	{
		return;
	}

	if ((level->configFlags & 4) == 0)
	{
		for (int i = 0; i < level->numWaterVertices; i++)
		{
			ptrdiff_t vi = level->ptr_water[i].v - vertexArray;

			if (vi >= 0 && vi < numVertex)
			{
				animMark[vi] = 1;
			}
		}
	}
	else
	{
		for (int i = 0; i < level->numSCVert; i++)
		{
			ptrdiff_t vi = level->ptrSCVert[i].v - vertexArray;

			if (vi >= 0 && vi < numVertex)
			{
				animMark[vi] = 1;
			}
		}
	}

	/* Worst case: every quadblock in a static leaf, 4 faces x 4 verts and
	 * 4 faces x 6 indices. Allocate flat, log the real usage. */
	size_t maxVerts = (size_t)numQuadBlock * 16;
	size_t maxIndices = (size_t)numQuadBlock * 24;

	s_trackMesh.verts = (struct NativeTrackVert *)malloc(maxVerts * sizeof(struct NativeTrackVert));
	s_trackMesh.indices = (u16 *)malloc(maxIndices * sizeof(u16));
	s_trackMesh.leafRanges = (struct NativeTrackLeafRange *)malloc((size_t)mesh->numBspNodes * sizeof(struct NativeTrackLeafRange));
	s_trackMesh.bspToLeafIndex = (s32 *)malloc((size_t)mesh->numBspNodes * sizeof(s32));
	s_trackMesh.texTable = (uintptr_t *)malloc(1024 * sizeof(uintptr_t));
	/* Parallel to verts, same worst-case sizing, same lifetime. */
	s_trackMesh.srcVert = (u16 *)malloc(maxVerts * sizeof(u16));

	if ((s_trackMesh.verts == NULL) || (s_trackMesh.indices == NULL) || (s_trackMesh.leafRanges == NULL) || (s_trackMesh.bspToLeafIndex == NULL) ||
	    (s_trackMesh.texTable == NULL) || (s_trackMesh.srcVert == NULL))
	{
		NativeTrackMesh_Free();
		free(animMark);
		return;
	}

	/* -1 = no resident range. Set once here rather than defaulted per-entry
	 * below, since most BSP node indices are branches (never visited by this
	 * leaf-only loop) and would otherwise read as stale/uninitialized. */
	memset(s_trackMesh.bspToLeafIndex, 0xff, (size_t)mesh->numBspNodes * sizeof(s32));

	/* Weld pre-pass: canonical mid per matched edge, over ALL quadblocks so
	 * the choice is stable regardless of which leaves end up resident. */
	struct NativeTrackWeldTable *weldTable = (struct NativeTrackWeldTable *)calloc(1, sizeof(struct NativeTrackWeldTable));

	if (weldTable != NULL)
	{
		for (int qb = 0; qb < numQuadBlock; qb++)
		{
			for (int e = 0; e < 4; e++)
			{
				u16 ia = quadBlocks[qb].index[sTrackOuterEdges[e][0]];
				u16 ib = quadBlocks[qb].index[sTrackOuterEdges[e][1]];
				u16 im = quadBlocks[qb].index[sTrackOuterEdges[e][2]];

				if ((int)ia >= numVertex || (int)ib >= numVertex || (int)im >= numVertex)
				{
					continue;
				}

				u64 key = NativeTrackMesh_EdgeKey(NativeTrackMesh_PackPos(&vertexArray[ia]), NativeTrackMesh_PackPos(&vertexArray[ib]));
				int slot = NativeTrackMesh_WeldSlot(weldTable, key);

				if (slot < 0)
				{
					continue;
				}

				if (weldTable->keys[slot] == 0)
				{
					weldTable->keys[slot] = key;
					weldTable->mid[slot] = NativeTrackMesh_PackPos(&vertexArray[im]);
				}
			}
		}
	}

	/* Grid slot -> outer-edge index for the emit-time snap. */
	static const s8 sEdgeOfSlot[9] = {-1, -1, -1, -1, 0, 1, -1, 2, 3};
	int weldCount = 0;

	int leavesTotal = 0, leavesExcluded = 0, facesDoubleSided = 0, facesNoTexture = 0, facesAnimTex = 0;
	/* keptWater and keptAnim are both populations that USED to be dropped whole
	 * and now are not -- the two halves of the s30 hole fix, kept as separate
	 * columns because they answer different questions. keptWater is how much
	 * geometry is being drawn opaque that shouldn't be (the transparency work's
	 * scope); keptAnim is what the vertex patcher has to cover. leavesExcluded
	 * is now only ever incremented by guards that are genuinely errors, so a
	 * non-zero value in the log is a real signal rather than routine. */
	int leavesKeptWater = 0, leavesKeptAnim = 0, leavesSkippedEmpty = 0;
	/* Both were silent `continue`s before s30 -- see their guard comments. */
	int facesDroppedIndexCap = 0, facesDroppedCapacity = 0;
	int animVertsWelded = 0, leavesEmittedNothing = 0;
	/* notex classification (s27: the water track reported 368 unclassified
	 * faces): null pointer / AnimTex with null active frame / implausible
	 * tpage word. Two raw samples of the bad-tpage case ride in the log. */
	int notexNull = 0, notexAnimNull = 0, notexBadWord = 0;
	u32 notexSample[2] = {0, 0};

	for (int n = 0; n < mesh->numBspNodes; n++)
	{
		struct BSP *bsp = &mesh->bspRoot[n];

		if ((bsp->flag & BSP_NODE_FLAG_LEAF) == 0)
		{
			continue;
		}

		leavesTotal++;

		/*
		 * s30: animated-vertex leaves are now KEPT. The old rule dropped a whole
		 * leaf if any ONE of its vertices was in the level's animated list, and
		 * the owner reported the resulting Polar Pass gaps as ordinary road --
		 * which the rule permits, because it discards the leaf, not the moving
		 * vertex. On Polar Pass that is 43 of the 77 dropped leaves.
		 *
		 * The alternative considered and rejected was leaving these leaves out
		 * and reviving the retail draw to paint them (g_dbg_trackGapFill's
		 * approach). Owner rejected it on direction -- the whole point of this
		 * renderer is that the PICA draws the track -- and the arithmetic agrees:
		 * the resident renderer is worth ~6% (s29 part 2's sweep: 25.5 vs 24.0
		 * fps Time Trial, 19.8 vs 18.8 Arcade, NOT the 18% an earlier reading
		 * claimed), so running DrawLevelOvr1P again every frame for 15% of the
		 * track spends a real fraction of the entire win to fix a hole.
		 *
		 * BSP_LEAF_FLAG_WATER leaves are kept too, as of the owner's ruling this
		 * session ("in future don't skip the water-flagged chunks"). They used
		 * to be excluded on the argument that this renderer bakes every face
		 * opaque (dst->a = 0xff below; N3DS_TrackDraw_Begin asserts opaque blend
		 * once for the whole draw), so drawing them would hide whatever should
		 * show through -- possibly worse than the gap.
		 *
		 * That argument does not survive the scale. Crash Cove flags 174 of its
		 * 504 leaves as water: most of a beach track, and not just the sea
		 * surface but everything binned into the water render-list slot with it.
		 * A solid sea is a visual downgrade, which the standing perf-over-parity
		 * ruling covers; a third of the level missing is a hole, which it does
		 * not. Semi-transparency remains the real fix and is still deferred --
		 * this makes the interim state the better-looking one, not the correct
		 * one.
		 *
		 * Counted, not silent: `water=` in [TRKMESH] now reports water leaves
		 * KEPT rather than dropped, because the number still matters -- it is
		 * how much geometry is being drawn opaque that should not be, and it is
		 * what the transparency work will have to cover.
		 */
		if ((bsp->flag & BSP_LEAF_FLAG_WATER) != 0)
		{
			leavesKeptWater++;
		}

		if (NativeTrackMesh_LeafTouchesAnim(bsp, animMark, numVertex, numQuadBlock))
		{
			/* Kept, but counted -- this is the population the vertex patcher
			 * has to cover, and the number is how we know whether it did. */
			leavesKeptAnim++;
		}

		int numQuads = bsp->data.leaf.numQuads;
		struct QuadBlock *range = bsp->data.leaf.ptrQuadBlockArray;

		if ((range == NULL) || (numQuads <= 0) || (numQuads > numQuadBlock))
		{
			/* s30: counted. This guard was silent, and the arithmetic exposed
			 * it -- Crash Cove reported leaves=273/504 with excl=174, leaving
			 * 57 leaves belonging to neither bucket and no line saying where
			 * they went. An empty leaf is legitimate asset data, but "silently
			 * absent" and "legitimately empty" have to be distinguishable in
			 * the log, because the whole point of this session's work is
			 * telling missing geometry from geometry that was never there. */
			leavesSkippedEmpty++;
			continue;
		}

		struct NativeTrackLeafRange *leafRange = &s_trackMesh.leafRanges[s_trackMesh.leafCount];

		leafRange->bspIndex = (u16)n;
		leafRange->atlasBin = 0;
		leafRange->firstIndex = s_trackMesh.indexCount;
		leafRange->boxMin[0] = bsp->box.min.x;
		leafRange->boxMin[1] = bsp->box.min.y;
		leafRange->boxMin[2] = bsp->box.min.z;
		leafRange->boxMax[0] = bsp->box.max.x;
		leafRange->boxMax[1] = bsp->box.max.y;
		leafRange->boxMax[2] = bsp->box.max.z;
		/* Widest-possible empty range (FIX 1a) -- narrows as real emitted
		 * faces' drawOrder bytes are seen below. Only ever read back when
		 * indexCount > 0 (i.e. at least one face narrowed it), same
		 * build-or-nothing contract as the rest of this struct. */
		leafRange->drawOrderMin = 127;
		leafRange->drawOrderMax = -128;

		for (int q = 0; q < numQuads; q++)
		{
			struct QuadBlock *block = &range[q];
			u32 drawOrderLow = CTR_ReadU32LE(&block->draw_order_low);
			int doubleSided = (drawOrderLow & QUADBLOCK_DRAW_ORDER_LOW_DOUBLE_SIDED) != 0;

			for (int face = 0; face < 4; face++)
			{
				uintptr_t texPtr = (uintptr_t)block->ptr_texture_mid[face];
				const struct TextureLayout *layout = NULL;

				if (texPtr == 0)
				{
					facesNoTexture++;
					notexNull++;
					continue;
				}

				if ((texPtr & 1) != 0)
				{
					/* AnimTex face: intern the AnimTex itself (low bit
					 * kept); resolve the live frame's near layout for the
					 * baked UVs -- the per-frame patcher rewrites them. */
					const struct AnimTex *anim = (const struct AnimTex *)(texPtr - 1);

					facesAnimTex++;
					if (anim->ptrActiveTex != NULL)
					{
						layout = (const struct TextureLayout *)anim->ptrActiveTex + 2; /* near */
					}
					else
					{
						facesNoTexture++;
						notexAnimNull++;
						continue;
					}
				}
				else
				{
					layout = (const struct TextureLayout *)texPtr + 2; /* near */
				}

				if ((CTR_ReadU16LE(&layout->tpage) & 0xfe00) != 0)
				{
					facesNoTexture++;

					if (notexBadWord < 2)
					{
						notexSample[notexBadWord] = ((u32)CTR_ReadU16LE(&layout->tpage) << 16) | CTR_ReadU16LE(&layout->clut);
					}

					notexBadWord++;
					continue;
				}

				if (s_trackMesh.texCount >= 1024)
				{
					continue;
				}

				/* u16 index buffer (PICA's native format): the biggest
				 * measured track builds ~44k verts; refuse past the format
				 * limit rather than wrap.
				 *
				 * s30: counted explicitly rather than "logged via the vert
				 * count below". Keeping animated-vertex leaves adds roughly a
				 * tenth more geometry, which moves 44k nearer to 65535 without
				 * reaching it -- and a silent `continue` here would present as
				 * faces quietly missing from the track, i.e. the exact defect
				 * this change exists to remove, wearing a different hat. This
				 * session has already been fooled three times by a cap sitting
				 * at its own value with nothing saying so. */
				if (s_trackMesh.vertCount + 4 > 0xffff)
				{
					facesDroppedIndexCap++;
					continue;
				}

				/* Allocation capacity guard (s27 review finding): the flat
				 * buffers are sized for a PARTITION of the quadblock array;
				 * overlapping leaf ranges could exceed it. Never write past. */
				if (((size_t)s_trackMesh.vertCount + 4 > maxVerts) || ((size_t)s_trackMesh.indexCount + 6 > maxIndices))
				{
					facesDroppedCapacity++;
					continue;
				}

				/* Vertex indices are asset data too -- skip the face if any
				 * corner would read outside the pool. */
				if ((block->index[sTrackFaceCorners[face][0]] >= numVertex) || (block->index[sTrackFaceCorners[face][1]] >= numVertex) ||
				    (block->index[sTrackFaceCorners[face][2]] >= numVertex) || (block->index[sTrackFaceCorners[face][3]] >= numVertex))
				{
					continue;
				}

				u16 texRef = NativeTrackMesh_InternTexture(texPtr);
				s8 drawOrder = (s8)((const u8 *)&block->draw_order_high)[face];
				u32 baseVert = s_trackMesh.vertCount;

				/* FIX 1a: widen this leaf's drawOrder range to cover every
				 * face actually emitted into it (not just this one) -- see
				 * the struct comment on drawOrderMin/drawOrderMax. */
				if (drawOrder < leafRange->drawOrderMin)
				{
					leafRange->drawOrderMin = drawOrder;
				}
				if (drawOrder > leafRange->drawOrderMax)
				{
					leafRange->drawOrderMax = drawOrder;
				}

				/* Per-face rotation/flip: corner c takes UV index
				 * sTrackUvPerm[rot][c] (see the table's derivation note). */
				u32 faceBits = (drawOrderLow >> (8 + 5 * face)) & 0x1f;
				const u8 *uvPerm = sTrackUvPerm[faceBits & 7];
				u8 faceMode = (u8)((faceBits >> 3) & 3);
				const u8 *uvBytes = &layout->u0;

				for (int c = 0; c < 4; c++)
				{
					int gridSlot = sTrackFaceCorners[face][c];
					const struct LevVertex *src = &vertexArray[block->index[gridSlot]];
					u32 dstIndex = s_trackMesh.vertCount++;
					struct NativeTrackVert *dst = &s_trackMesh.verts[dstIndex];

					/* s30: remember which LevVertex this came from, so the
					 * per-frame patcher can re-read a moving one. Set BEFORE the
					 * weld snap below, which may revoke it. */
					s_trackMesh.srcVert[dstIndex] = block->index[gridSlot];

					dst->x = src->pos.x;
					dst->y = src->pos.y;
					dst->z = src->pos.z;

					/* Weld snap: an edge-mid whose matched edge carries a
					 * different canonical mid takes the canonical position. */
					if ((weldTable != NULL) && (sEdgeOfSlot[gridSlot] >= 0))
					{
						const u8 *edge = sTrackOuterEdges[(int)sEdgeOfSlot[gridSlot]];
						u16 ea = block->index[edge[0]];
						u16 eb = block->index[edge[1]];

						if ((int)ea < numVertex && (int)eb < numVertex)
						{
							u64 key = NativeTrackMesh_EdgeKey(NativeTrackMesh_PackPos(&vertexArray[ea]), NativeTrackMesh_PackPos(&vertexArray[eb]));
							int slot = NativeTrackMesh_WeldSlot(weldTable, key);

							if ((slot >= 0) && (weldTable->keys[slot] == key))
							{
								u64 canon = weldTable->mid[slot];
								u64 mine = NativeTrackMesh_PackPos(src);

								if (canon != mine)
								{
									dst->x = (s16)(u16)((canon - 1) >> 32);
									dst->y = (s16)(u16)((canon - 1) >> 16);
									dst->z = (s16)(u16)(canon - 1);
									weldCount++;

									/* This vertex's position is no longer its
									 * source's -- it is the canonical mid of a
									 * shared edge, chosen to close a crack. A
									 * per-frame re-copy from the source would
									 * undo that repair, so revoke provenance.
									 * Frozen-but-welded beats animated-and-
									 * cracked; the count is reported so the
									 * trade is visible rather than assumed. */
									if (animMark[block->index[gridSlot]])
									{
										animVertsWelded++;
									}
									s_trackMesh.srcVert[dstIndex] = TRACK_SRCVERT_NONE;
								}
							}
						}
					}
					dst->drawOrder = drawOrder;
					/* Bits 3-4 carry which of the layout's four corners this
					 * vertex's UV came from, so the AnimTex patcher can re-read
					 * the SAME corner out of a different frame's layout. The
					 * per-face rotation/flip is already resolved into it, which
					 * is the point -- the patcher never has to re-derive
					 * faceBits from a QuadBlock it no longer holds. */
					dst->faceFlags = (u8)(doubleSided | (faceMode << 1) | ((uvPerm[c] & 3) << 3));
					dst->r = src->color_hi[0];
					dst->g = src->color_hi[1];
					dst->b = src->color_hi[2];
					dst->a = 0xff;
					dst->u = uvBytes[sTrackUvByteOfs[uvPerm[c]]];
					dst->v = uvBytes[sTrackUvByteOfs[uvPerm[c]] + 1];
					dst->texRef = texRef;
				}

				facesDoubleSided += doubleSided;

				/* ctr-tools' export split: (0,2,1)(1,2,3). */
				u16 *idx = &s_trackMesh.indices[s_trackMesh.indexCount];

				idx[0] = (u16)(baseVert + 0);
				idx[1] = (u16)(baseVert + 2);
				idx[2] = (u16)(baseVert + 1);
				idx[3] = (u16)(baseVert + 1);
				idx[4] = (u16)(baseVert + 2);
				idx[5] = (u16)(baseVert + 3);
				s_trackMesh.indexCount += 6;
			}
		}

		leafRange->indexCount = s_trackMesh.indexCount - leafRange->firstIndex;

		if (leafRange->indexCount > 0)
		{
			/* leafRange sits at s_trackMesh.leafRanges[s_trackMesh.leafCount]
			 * (allocated above, before this count is known) -- record the
			 * reverse mapping at that same index before advancing it. */
			s_trackMesh.bspToLeafIndex[n] = (s32)s_trackMesh.leafCount;
			s_trackMesh.leafCount++;
		}
		else
		{
			/* s30, and the last hole in the accounting: a leaf that was NOT
			 * excluded, NOT empty, and still contributed nothing. Crash Cove
			 * reported 273 resident of 504 with 174 excluded and 0 empty --
			 * 57 leaves belonging to no bucket at all. They are these: every
			 * face in them was skipped for having no texture pointer (the same
			 * run reports notex=372).
			 *
			 * Whether that is a DEFECT is not settled here. Retail's own
			 * resolver returns NULL for those faces too
			 * (DrawLevelOvr1P_ResolveTexturePointer), so it may well draw
			 * nothing either -- in which case this is correct behaviour and the
			 * count is just bookkeeping. Counted rather than concluded. */
			leavesEmittedNothing++;
		}
	}

	/*
	 * s30: collect the resident vertices that need a per-frame refresh, now
	 * that both the emit loop (which set provenance) and the weld snap (which
	 * revokes it) have run. Built once per level, walked per frame -- the
	 * alternative, testing animMark inside the frame loop, would mean keeping
	 * animMark alive for the level and re-deriving the same answer 60 times a
	 * second from data that cannot change.
	 *
	 * Sized to the count, not the worst case: this is a per-level allocation
	 * that outlives the build, unlike animMark/weldTable freed just below, and
	 * on a 2MB retail-pressure heap the difference between "the animated ones"
	 * and "every vertex" is worth the extra pass.
	 */
	s_trackMesh.animVertCount = 0;
	s_trackMesh.animVertWelded = 0;
	s_trackMesh.animSrcArray = vertexArray;
	s_trackMesh.animSrcCount = (u32)numVertex;
	{
		u32 needed = 0;
		u32 i;

		for (i = 0; i < s_trackMesh.vertCount; i++)
		{
			u16 sv = s_trackMesh.srcVert[i];

			if ((sv != TRACK_SRCVERT_NONE) && ((int)sv < numVertex) && animMark[sv])
			{
				needed++;
			}
		}

		/* Counted at the weld site itself, where the source index is still
		 * known -- these are vertices that ARE animated in the source but hold
		 * a canonical edge mid instead. A large number would mean the crack
		 * repair is quietly freezing most of a water surface, which is worth
		 * knowing before anyone concludes the patcher does not work. */
		s_trackMesh.animVertWelded = (u32)animVertsWelded;

		if (needed > 0)
		{
			s_trackMesh.animVerts = (u32 *)malloc((size_t)needed * sizeof(u32));

			if (s_trackMesh.animVerts != NULL)
			{
				for (i = 0; i < s_trackMesh.vertCount; i++)
				{
					u16 sv = s_trackMesh.srcVert[i];

					if ((sv != TRACK_SRCVERT_NONE) && ((int)sv < numVertex) && animMark[sv])
					{
						s_trackMesh.animVerts[s_trackMesh.animVertCount++] = i;
					}
				}
			}
			/* NULL here is not fatal -- animVertCount stays 0, the patcher
			 * does nothing, and the level renders exactly as it does before
			 * this feature: frozen but present. Degrade, never drop. */
		}
	}

	free(animMark);
	free(weldTable);

	/* Rung 2a: atlas layout over the interned texture table. Plain refs
	 * contribute their near layout; AnimTex refs contribute EVERY frame's
	 * near layout (all frames live in the atlas -- design 4.1), with the
	 * texRef translation pointing at the active frame's rect. */
	for (u32 t = 0; t < 1024; t++)
	{
		s_trackMesh.texRectIndex[t] = -1;
	}

	int atlasAnimFrames = 0;

	/* s30: every frame's rect index is kept now, not just the active one --
	 * without them the resident renderer can only ever show the frame that
	 * happened to be up at load, which is why turbo pads and waterfalls sit
	 * frozen. Allocated lazily and capped; running out is a degrade (that
	 * chain stays frozen), never a dropped level. */
	s_trackMesh.animTex = (struct NativeTrackAnimTexEntry *)malloc(TRACK_ANIMTEX_MAX * sizeof(struct NativeTrackAnimTexEntry));
	s_trackMesh.animFrameRect = (s16 *)malloc(TRACK_ANIMTEX_MAX_FRAMESLOTS * sizeof(s16));

	int animTexOverflow = 0, animSlotOverflow = 0;

	for (u32 t = 0; t < s_trackMesh.texCount; t++)
	{
		uintptr_t texPtr = s_trackMesh.texTable[t];

		if ((texPtr & 1) != 0)
		{
			const struct AnimTex *anim = (const struct AnimTex *)(texPtr - 1);

			if ((anim->numFrames > 0) && (anim->numFrames <= 256))
			{
				struct IconGroup4 **frames = ANIMTEX_GETARRAY((struct AnimTex *)anim);
				struct NativeTrackAnimTexEntry *entry = NULL;

				/* Reserve this chain's frame slots up front so slot f is frame
				 * f with no remap -- the game hands us a frame INDEX every
				 * vblank, and a compacted table would need a second lookup to
				 * undo. Unusable frames below simply leave their slot at -1. */
				if ((s_trackMesh.animTex != NULL) && (s_trackMesh.animFrameRect != NULL))
				{
					if (s_trackMesh.animTexCount >= TRACK_ANIMTEX_MAX)
					{
						animTexOverflow++;
					}
					else if (s_trackMesh.animFrameSlotCount + (u32)anim->numFrames > TRACK_ANIMTEX_MAX_FRAMESLOTS)
					{
						animSlotOverflow++;
					}
					else
					{
						entry = &s_trackMesh.animTex[s_trackMesh.animTexCount++];
						entry->anim = anim;
						entry->texRef = (u16)t;
						entry->numFrames = (u16)anim->numFrames;
						entry->frameSlot = s_trackMesh.animFrameSlotCount;
						entry->vertFirst = 0;
						entry->vertCount = 0;
						entry->curFrame = -1;
						entry->buildUnit = TRACK_ATLAS_UNIT_INVALID;
						s_trackMesh.animFrameSlotCount += (u32)anim->numFrames;

						for (int f = 0; f < anim->numFrames; f++)
						{
							s_trackMesh.animFrameRect[entry->frameSlot + (u32)f] = -1;
						}
					}
				}

				for (int f = 0; f < anim->numFrames; f++)
				{
					if (frames[f] == NULL)
					{
						continue;
					}

					const struct TextureLayout *frameLayout = (const struct TextureLayout *)frames[f] + 2; /* near */

					if ((CTR_ReadU16LE(&frameLayout->tpage) & 0xfe00) != 0)
					{
						continue;
					}

					atlasAnimFrames++;

					s16 frameRect = -1;

					NativeTrackAtlas_AddLayout(frameLayout, &frameRect);

					if (entry != NULL)
					{
						s_trackMesh.animFrameRect[entry->frameSlot + (u32)f] = frameRect;
					}

					if ((uintptr_t)frames[f] == (uintptr_t)anim->ptrActiveTex)
					{
						s_trackMesh.texRectIndex[t] = frameRect;
					}
				}
			}
		}
		else
		{
			const struct TextureLayout *layout = (const struct TextureLayout *)texPtr + 2; /* near */

			if ((CTR_ReadU16LE(&layout->tpage) & 0xfe00) == 0)
			{
				NativeTrackAtlas_AddLayout(layout, &s_trackMesh.texRectIndex[t]);
			}
		}
	}

	NativeTrackAtlas_Pack();


#if defined(__3DS__)
	int trackAtlasOk = NativeTrackAtlas_UploadToBackend(mesh, numQuadBlock, numVertex);
	/* Rung 3a step 2: geometry upload shares the atlas's own withhold-latch-
	 * on-failure contract (below) -- a transient linearAlloc shortfall
	 * retries the WHOLE build next frame, same as the atlas's documented
	 * linear-heap pressure case, rather than latching a level with no
	 * resident geometry uploaded. */
	/* Rung 3b: bin BEFORE the geometry upload, because binning rewrites the
	 * index buffer (and leafRanges with it) and the upload is what hands that
	 * buffer to the GPU. It must also come AFTER the atlas upload above, which
	 * clears atlasUnit on any rect it skipped. Shares the same
	 * withhold-latch-and-retry contract as its two neighbours. */
	int trackBinOk = NativeTrackMesh_BinLeavesByAtlasUnit();
	int trackGeomOk = trackBinOk && NativeTrackGeom_UploadToBackend(mesh, numQuadBlock, numVertex);
#endif

	/* AFTER the atlas upload and the leaf binning, both of which this depends
	 * on: the upload is what clears atlasUnit on rects it skipped, and the
	 * binning is what fixes which atlas unit each leaf's draw will bind. On PC
	 * neither runs, so the unit column below is optimistic there -- PC verifies
	 * COUNTS and UV arithmetic, never pixels, same as the rest of this file. */
	NativeTrackAnim_BuildVertexLists();

	/* Latch only now, on success -- see the fingerprint comment up top. On
	 * 3DS, a transient track-atlas allocation shortfall (linear-heap
	 * pressure) also withholds the latch so the WHOLE build -- mesh and
	 * atlas together -- retries next frame, same contract as the
	 * calloc-failure paths above; permanent per-rect skips (oversize,
	 * unresolved unit) do not withhold it (review finding #4). */
#if defined(__3DS__)
	if (trackAtlasOk && trackGeomOk)
#endif
	{
		s_trackMesh.builtFor = mesh;
		s_builtQb = numQuadBlock;
		s_builtVtx = numVertex;
		s_builtBspNodes = mesh->numBspNodes;
		s_trackMesh.bspNodeCount = (u32)mesh->numBspNodes;
	}

#if !defined(__3DS__)
	/* Geometry/UV audit artifact: one OBJ per level into the gitignored
	 * debug/ tree (positions + per-vertex color extension + UVs in page
	 * space /256). Deterministic name -> self-deduping across runs. Skipped
	 * silently if debug/ is absent (fresh checkouts). */
	{
		char objPath[128];

		snprintf(objPath, sizeof(objPath), "debug/trackmesh_qb%d_vtx%d.obj", numQuadBlock, numVertex);

		FILE *obj = fopen(objPath, "w");

		if (obj != NULL)
		{
			fprintf(obj, "# ctr-native track mesh dump: verts=%u idx=%u leaves=%u welds=%d\n", s_trackMesh.vertCount, s_trackMesh.indexCount,
			        s_trackMesh.leafCount, weldCount);

			for (u32 i = 0; i < s_trackMesh.vertCount; i++)
			{
				const struct NativeTrackVert *v = &s_trackMesh.verts[i];

				fprintf(obj, "v %d %d %d %.3f %.3f %.3f\n", v->x, -v->y, v->z, v->r / 255.0f, v->g / 255.0f, v->b / 255.0f);
				fprintf(obj, "vt %.5f %.5f\n", v->u / 256.0f, 1.0f - (v->v / 256.0f));
			}

			for (u32 i = 0; i + 2 < s_trackMesh.indexCount; i += 3)
			{
				fprintf(obj, "f %u/%u %u/%u %u/%u\n", s_trackMesh.indices[i] + 1, s_trackMesh.indices[i] + 1, s_trackMesh.indices[i + 1] + 1,
				        s_trackMesh.indices[i + 1] + 1, s_trackMesh.indices[i + 2] + 1, s_trackMesh.indices[i + 2] + 1);
			}

			fclose(obj);
		}
	}
#endif

	/*
	 * lvl= joins lev= because a heap pointer does not identify a track. The
	 * open Polar Pass defect (untextured holes in ground and wall) has to be
	 * matched to the build line for the level it happened on, and reading these
	 * blocks back meant hand-correlating pointers against a separate log
	 * stream. Same source [PERF] and [SWEEP] stamp -- see
	 * NativePerf_CurrentLevelID's comment for why a second level field is not
	 * an option.
	 */
	Platform_Log("[TRKMESH] lvl=%d lev=%p leaves=%u/%d (excl=%d keptWater=%d keptAnim=%d empty=%d noFaces=%d) verts=%u idx=%u tex=%u dropIdxCap=%d dropCap=%d "
	             "animVerts=%u animWelded=%u | KB verts=%u idx=%u | faces ds=%d animtex=%d notex=%d | welds=%d\n",
	             NativePerf_CurrentLevelID(), (void *)level, s_trackMesh.leafCount, leavesTotal, leavesExcluded, leavesKeptWater, leavesKeptAnim,
	             leavesSkippedEmpty, leavesEmittedNothing,
	             s_trackMesh.vertCount, s_trackMesh.indexCount, s_trackMesh.texCount, facesDroppedIndexCap, facesDroppedCapacity,
	             s_trackMesh.animVertCount, s_trackMesh.animVertWelded,
	             (u32)(s_trackMesh.vertCount * sizeof(struct NativeTrackVert) / 1024), (u32)(s_trackMesh.indexCount * sizeof(u16) / 1024), facesDoubleSided,
	             facesAnimTex, facesNoTexture, weldCount);

	/*
	 * The animated-texture ledger. `chains` is how many texture chains any
	 * resident face samples, `frames` how many of their frames got a rect,
	 * `verts` how much geometry a frame change actually has to rewrite (the
	 * per-vblank cost, and the reason this is proportional to the animation
	 * rather than to the track). `unitSplit` is the one that would silently
	 * lose animation: frames of one chain that packed into DIFFERENT atlas
	 * units cannot be swapped in, because the leaf's draw binds a single unit
	 * chosen at build time -- those chains stay frozen and say so here rather
	 * than quietly not moving.
	 *
	 * chains=0 on a track with visible turbo pads would mean the animated faces
	 * are not resident at all (excluded, or untextured) -- a different bug than
	 * "the patcher does not work", which is why the two are separate columns.
	 */
	{
		u32 framesPlaced = 0, unitSplit = 0;

		for (u32 e = 0; e < s_trackMesh.animTexCount; e++)
		{
			const struct NativeTrackAnimTexEntry *entry = &s_trackMesh.animTex[e];

			for (u32 f = 0; f < entry->numFrames; f++)
			{
				s16 r = s_trackMesh.animFrameRect[entry->frameSlot + f];

				if ((r < 0) || (r >= s_trackMesh.atlas.rectCount))
				{
					continue;
				}

				framesPlaced++;

				if (s_trackMesh.atlas.rects[r].atlasUnit != entry->buildUnit)
				{
					unitSplit++;
				}
			}
		}

		Platform_Log("[TRKANIM] lvl=%d chains=%u frames=%u/%u verts=%u unitSplit=%u overflow=%d/%d\n", NativePerf_CurrentLevelID(), s_trackMesh.animTexCount,
		             framesPlaced, s_trackMesh.animFrameSlotCount, s_trackMesh.animTexVertCount, unitSplit, animTexOverflow, animSlotOverflow);
	}

	if (facesNoTexture != 0)
	{
		Platform_Log("[TRKMESH] notex: null=%d animNull=%d badWord=%d sample0=%08x sample1=%08x\n", notexNull, notexAnimNull, notexBadWord, notexSample[0],
		             notexSample[1]);
	}

	Platform_Log("[TRKATLAS] lvl=%d rects=%d units=%d/%d fill=%d%% texelKB=%u animFrames=%d dropped=%d\n", NativePerf_CurrentLevelID(),
	             s_trackMesh.atlas.rectCount, s_trackMesh.atlas.unitsUsed,
	             TRACK_ATLAS_MAX_UNITS, s_trackMesh.atlas.unitsUsed ? (int)((s_trackMesh.atlas.packedTexels * 100u) / ((u32)s_trackMesh.atlas.unitsUsed * TRACK_ATLAS_DIM * TRACK_ATLAS_DIM)) : 0,
	             (s_trackMesh.atlas.packedTexels * 2u) / 1024u, atlasAnimFrames, s_trackMesh.atlas.rectsDropped);
}

/*
 * Read-only accessor surface (include/platform/native_track_mesh.h), rung 3a
 * step 1. Everything below is a plain read of state NativeTrackMesh_Frame
 * above already owns and validates -- no new invariants, no new failure
 * modes. This is the only part of this file another translation unit (the
 * frame recorder, platform/native_track_frame.c) is meant to touch; every
 * other symbol here stays static.
 */

int NativeTrackMesh_IsBuilt(void)
{
	return s_trackMesh.builtFor != NULL;
}

int NativeTrackMesh_IsBuiltFor(const struct mesh_info *mesh)
{
	return (mesh != NULL) && (s_trackMesh.builtFor == mesh);
}

unsigned NativeTrackMesh_LeafCount(void)
{
	return (unsigned)s_trackMesh.leafCount;
}

int NativeTrackMesh_LeafAt(unsigned i, unsigned *outBspIndex, unsigned *outFirstIndex, unsigned *outIndexCount)
{
	if (i >= s_trackMesh.leafCount)
	{
		return 0;
	}

	*outBspIndex = s_trackMesh.leafRanges[i].bspIndex;
	*outFirstIndex = s_trackMesh.leafRanges[i].firstIndex;
	*outIndexCount = s_trackMesh.leafRanges[i].indexCount;
	return 1;
}

/* Rung 3b: which atlas unit this range samples. Values at or above the unit
 * count mean "no placed texture" -- the draw side must leave texturing off for
 * those rather than binding a stale unit. */
unsigned NativeTrackMesh_LeafAtlasBin(unsigned i)
{
	return (i < s_trackMesh.leafCount) ? (unsigned)s_trackMesh.leafRanges[i].atlasBin : 0xFFFFu;
}

int NativeTrackMesh_LeafBox(unsigned i, s16 outMin[3], s16 outMax[3])
{
	if (i >= s_trackMesh.leafCount)
	{
		return 0;
	}

	outMin[0] = s_trackMesh.leafRanges[i].boxMin[0];
	outMin[1] = s_trackMesh.leafRanges[i].boxMin[1];
	outMin[2] = s_trackMesh.leafRanges[i].boxMin[2];
	outMax[0] = s_trackMesh.leafRanges[i].boxMax[0];
	outMax[1] = s_trackMesh.leafRanges[i].boxMax[1];
	outMax[2] = s_trackMesh.leafRanges[i].boxMax[2];
	return 1;
}

int NativeTrackMesh_LeafDrawOrderRange(unsigned i, s8 *outMin, s8 *outMax)
{
	if (i >= s_trackMesh.leafCount)
	{
		return 0;
	}

	*outMin = s_trackMesh.leafRanges[i].drawOrderMin;
	*outMax = s_trackMesh.leafRanges[i].drawOrderMax;
	return 1;
}

int NativeTrackMesh_LeafIndexForBsp(unsigned bspIndex)
{
	if ((s_trackMesh.bspToLeafIndex == NULL) || (s_trackMesh.builtFor == NULL))
	{
		return -1;
	}

	/* bspIndex arrives from the game's own per-frame render-list traversal
	 * (a BSP node index), not something this file bounds itself -- check it
	 * against bspNodeCount, the element count THIS ALLOCATION was actually
	 * sized for at build time (review fix: builtFor->numBspNodes read live
	 * here could silently drift from what bspToLeafIndex was sized for if a
	 * level reloads into the same mesh_info arena slot with a different bsp
	 * node count -- bspNodeCount only ever changes alongside the allocation
	 * it describes, so it can't drift out from under it that way). Unsigned
	 * compare against an unsigned field -- no (int) cast on bspIndex: casting
	 * an unsigned value to int before comparing would let any bspIndex >=
	 * 0x80000000 read as negative and slip past this bound entirely. */
	if (bspIndex >= s_trackMesh.bspNodeCount)
	{
		return -1;
	}

	return (int)s_trackMesh.bspToLeafIndex[bspIndex];
}

const struct NativeTrackVert *NativeTrackMesh_Verts(void)
{
	return s_trackMesh.verts;
}

unsigned NativeTrackMesh_VertCount(void)
{
	return s_trackMesh.vertCount;
}

const u16 *NativeTrackMesh_Indices(void)
{
	return s_trackMesh.indices;
}

unsigned NativeTrackMesh_IndexCount(void)
{
	return s_trackMesh.indexCount;
}

#else /* !(CTR_NATIVE && CTR_NATIVE_TRACK_RENDERER) */

/*
 * Review fix (FIX 5): include/platform/native_track_mesh.h promises every
 * accessor is a no-op/zero-returning stub when the gate is off, so a caller
 * never needs its own #ifdef -- that promise was previously just a comment;
 * nothing defined these symbols on the off path, they only existed because
 * the whole file (mesh builder AND accessors together) sat inside one big
 * #if block above. That matters concretely because a gate-OFF 3DS build is
 * the DEFAULT configuration this whole tree builds (CTR_NATIVE_TRACK_RENDERER
 * defaults OFF in CMakeLists.txt), and step 2's 3DS draw path
 * (platform/n3ds/native_renderer_3ds.c) is meant to call these unconditionally
 * and see "nothing built" rather than fail to link. These stubs are what
 * makes that true.
 */

int NativeTrackMesh_IsBuilt(void)
{
	return 0;
}

void NativeTrackAnim_PatchFrame(void)
{
}

int NativeTrackMesh_IsBuiltFor(const struct mesh_info *mesh)
{
	(void)mesh;
	return 0;
}

unsigned NativeTrackMesh_LeafCount(void)
{
	return 0;
}

int NativeTrackMesh_LeafAt(unsigned i, unsigned *outBspIndex, unsigned *outFirstIndex, unsigned *outIndexCount)
{
	(void)i;
	(void)outBspIndex;
	(void)outFirstIndex;
	(void)outIndexCount;
	return 0;
}

int NativeTrackMesh_LeafBox(unsigned i, s16 outMin[3], s16 outMax[3])
{
	(void)i;
	(void)outMin;
	(void)outMax;
	return 0;
}

int NativeTrackMesh_LeafDrawOrderRange(unsigned i, s8 *outMin, s8 *outMax)
{
	(void)i;
	(void)outMin;
	(void)outMax;
	return 0;
}

int NativeTrackMesh_LeafIndexForBsp(unsigned bspIndex)
{
	(void)bspIndex;
	return -1;
}

const struct NativeTrackVert *NativeTrackMesh_Verts(void)
{
	return NULL;
}

unsigned NativeTrackMesh_VertCount(void)
{
	return 0;
}

const u16 *NativeTrackMesh_Indices(void)
{
	return NULL;
}

unsigned NativeTrackMesh_IndexCount(void)
{
	return 0;
}

#endif /* CTR_NATIVE_TRACK_RENDERER */
