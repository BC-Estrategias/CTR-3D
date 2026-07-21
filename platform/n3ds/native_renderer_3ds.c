/*
 * Nintendo 3DS renderer backend -- phase 2a real data path.
 *
 * Replaces platform/native_renderer.c (OpenGL 3.3 + glad), which cannot run on
 * the PICA200. This file satisfies every signature in
 * include/platform/native_renderer.h so the rest of the unity build compiles
 * unmodified. Design: docs/CITRO3D_RENDERER_DESIGN.md.
 *
 * What is REAL here: the CPU-side PSX VRAM mirror and every operation that
 * only touches it (clear/copy/read/save-state); citro3d init, the compiled
 * vertex shader, and the top-screen render target (behind n3ds_backend.h --
 * this file is part of the unity build and can never see libctru/citro3d
 * types directly, same reason as everything else that crosses that
 * boundary); the CPU texture decode cache (design doc section 1, all three
 * VRAM-CLUT formats plus the untextured/flat-color fast path); and the real
 * GrVertex -> N3DS_Vertex conversion with per-(page,clut) draw batching
 * (design doc section 6) that DrawAllSplits/DrawSplit (native_gpu.c) now
 * actually drives every frame via UpdateVertexBuffer/DrawTriangles.
 *
 * What is STILL STUBBED here, per the design doc's own staging: blending
 * beyond BM_NONE and the STP 2-pass (2b), stencil (real no-op, provably dead
 * code -- section 3), offscreen render targets (section 4, deferred),
 * framebuffer readback (section 5, deferred), stereo (2c), clip/scissor
 * (SetupClipMode -- not staged into 2a by the design doc, not touched this
 * pass), the background clear color (NativeRenderer_Clear -- BeginScene
 * always clears to opaque black regardless of the game's DRAWENV clear
 * color), and the 32-bit RGBA override-texture path (TF_32_BIT_RGBA --
 * provably unreachable from any real game/ code path today, zero DR_PSYX_TEX
 * call sites; DrawTriangles skips a split entirely rather than guess at an
 * untested path).
 *
 * The bottom screen hosts a libctru text console (the stats block) so a
 * running main loop stays observable while the top screen is citro3d's.
 */

#include <stdio.h>
#include <string.h>

#include <macros.h>
#include <platform/native_perf.h>
#include <platform/native_renderer.h>

#include "n3ds_backend.h"

/*
 * PSX VRAM mirror: 1024x512 16-bit. native_renderer.c owns this on the host and
 * nothing outside the renderer references it, so the 3DS backend owns its own.
 */
u16 vram[VRAM_WIDTH * VRAM_HEIGHT];

/*
 * Renderer config the PSX GPU layer reads directly (native_gpu.c consults it
 * when building texcoords). Defined by native_renderer.c on the host, so the
 * 3DS backend has to carry it too. Nearest-neighbour matches PSX output.
 */
int g_cfg_bilinearFiltering = 0;

global_variable int s_rendererInitialised = 0;
global_variable u32 s_frameCounter = 0;
global_variable unsigned long s_lastStatFrames = 0;
global_variable unsigned long s_lastStatVblanks = 0;

internal u16 NativeRenderer3DS_PackBGR555(u8 r, u8 g, u8 b)
{
	return (u16)(((r >> 3) & 31) | (((g >> 3) & 31) << 5) | (((b >> 3) & 31) << 10));
}

/*
 * ------------------------------------------------------------------------
 * CPU texture decode cache (design doc section 1) and real vertex
 * submission (section 6).
 * ------------------------------------------------------------------------
 *
 * TextureID is a sentinel, not a GPU handle -- SetTexture only ever receives
 * NativeRenderer_GetVRAMTexture() (real page/CLUT content, decode-cached) or
 * NativeRenderer_GetWhiteTexture() (untextured Gouraud/flat-color splits;
 * see native_gpu.c's AddSplit) in every reachable path. Both sentinels must
 * be distinct and non-zero -- the previous stub returned 0 for both, which
 * would have made them indistinguishable.
 */
#define N3DS_TEXTURE_ID_VRAM  ((TextureID)1)
#define N3DS_TEXTURE_ID_WHITE ((TextureID)2)

global_variable TextureID s_currentTextureId = N3DS_TEXTURE_ID_VRAM;
global_variable TexFormat s_currentTexFormat = TF_16_BIT;

/*
 * Debug-only, mirrors PC's g_dbg_texturelessMode (native_renderer.c:79,
 * F2 key). No keyboard here -- toggled by ZL, see n3ds_backend.c's
 * N3DS_ReadButtons. Forces every draw through the same untextured/white
 * path real Gouraud splits already use (see NativeRenderer_SetTexture),
 * so it also zeroes decode-cache activity -- useful for isolating whether
 * a slow frame is texture-related at all.
 */
int g_dbg_texturelessMode = 0;

/*
 * Diagnostic (2026-07-20): force EVERY textured triangle to sample ONE fixed
 * texture, captured fresh each frame from the first textured triangle (so it is
 * always a currently-loaded, valid texture) and routed through the simple
 * full-page path. If the whole world then shows that single texture cleanly and
 * stably, the base path -- decode, atlas write, GPU sample, cache coherency --
 * is proven sound and the corruption lives entirely in the dynamic
 * per-triangle cache/eviction/sub-page machinery. If it's still garbled, the
 * base path itself is broken. s_forcedTexValid is reset once per frame in
 * NativeRenderer_SwapWindow. Toggle off to restore normal rendering.
 */
int g_dbg_forceSingleTexture = 0;
global_variable s16 s_forcedPage = 0;
global_variable s16 s_forcedClut = 0;
global_variable TexFormat s_forcedFormat = TF_4_BIT;
global_variable int s_forcedTexValid = 0;

/*
 * Diagnostic (2026-07-20): route EVERY textured triangle through the full-page
 * tier, bypassing the sub-page tier entirely. The forced-single-texture test
 * proved the base decode/atlas/sample/coherency path renders one texture
 * cleanly across all geometry -- but it only ever exercised the full-page path,
 * while normal rendering sends ~92% of triangles through sub-page. This isolates
 * whether the sub-page routing/sub-cell-remap machinery is the corruption source.
 * Paired with a raised N3DS_FULLPAGE_RESERVED_CELLS so the now-100%-full-page
 * traffic doesn't evict and confound the result.
 */
int g_dbg_disableSubPage = 0;

/* ABLATION A (2026-07-21, session 14): boot-splash present submits clear +
 * display transfer but no draw calls. Used to prove the real-hardware boot
 * hang lived in the draw path, not clear/transfer -- which led to the
 * partial-output-write fix in shaders/ctr.v.pica (outtc0.zw). Keep at 0. */
int g_dbg_skipSplashDraws = 0;

global_variable const GrVertex *s_pendingVertices = NULL;
global_variable int s_pendingVertexCount = 0;

/*
 * VRAM is 1024x512 = 16x8 64x64 dirty-generation cells (2026-07-20,
 * shrunk from the original 4x2 256x256 page-sized grid -- design doc
 * section 1 still describes the coarse version). With only 8 cells
 * covering all of VRAM, any write anywhere in an eighth of it staled
 * every decoded page sourced from that eighth, unrelated or not; race
 * content measured stale=6418 (+272/w) against that grid. This is a
 * diagnostic experiment to see how much of that is genuine per-texture
 * animation vs. collateral invalidation from cells coarser than the
 * pages decoded from them. A decode cache entry records the cell
 * generation(s) it was decoded against and is stale if any has since moved.
 */
#define N3DS_VRAM_CELL_COLS  16
#define N3DS_VRAM_CELL_ROWS  8
#define N3DS_VRAM_CELL_DIM   64
global_variable u32 s_vramCellGen[N3DS_VRAM_CELL_ROWS * N3DS_VRAM_CELL_COLS];

/*
 * Times N3DS_DirtyVRAMRect actually bumped a generation. Read with the
 * capacity/stale miss split below: if this stays ~0 then stale misses are
 * impossible by construction, and any thrashing is purely a capacity problem.
 */
global_variable u32 s_vramDirtyBumps = 0;

internal int N3DS_VRAMCellIndex(int x, int y)
{
	int col = x / N3DS_VRAM_CELL_DIM;
	int row = y / N3DS_VRAM_CELL_DIM;

	if (col < 0) { col = 0; } else if (col >= N3DS_VRAM_CELL_COLS) { col = N3DS_VRAM_CELL_COLS - 1; }
	if (row < 0) { row = 0; } else if (row >= N3DS_VRAM_CELL_ROWS) { row = N3DS_VRAM_CELL_ROWS - 1; }

	return row * N3DS_VRAM_CELL_COLS + col;
}

/*
 * Bumps the generation of every cell touched by a [x, x+w) x [y, y+h) VRAM
 * write, so any decode cache entry sourced from that region re-decodes on
 * next use. Called from the only two currently-real VRAM mutators
 * (ClearVRAM, CopyVRAM) -- UpdateVRAM/framebuffer readback are still stubs,
 * so they cannot dirty anything yet; wire this in alongside them when they
 * become real (design doc section 5).
 */
internal void N3DS_DirtyVRAMRect(int x, int y, int w, int h)
{
	int x1, y1, cx, cy;

	if ((w <= 0) || (h <= 0))
	{
		return;
	}

	x1 = x + w - 1;
	y1 = y + h - 1;

	for (cy = y / N3DS_VRAM_CELL_DIM; cy <= y1 / N3DS_VRAM_CELL_DIM; cy++)
	{
		for (cx = x / N3DS_VRAM_CELL_DIM; cx <= x1 / N3DS_VRAM_CELL_DIM; cx++)
		{
			s_vramCellGen[N3DS_VRAMCellIndex(cx * N3DS_VRAM_CELL_DIM, cy * N3DS_VRAM_CELL_DIM)]++;
			s_vramDirtyBumps++;
		}
	}
}

typedef struct
{
	int used;         /* 0 = empty cell, never decoded into */
	s16 page;
	s16 clut;
	TexFormat format;
	u32 pageGen;
	u32 clutGenA;
	u32 clutGenB;      /* == clutGenA when the CLUT doesn't straddle two VRAM cells */
	u32 lastUsedFrame;
} N3DS_DecodeCacheEntry;

/*
 * The decode cache is addressed as a flat array of atlas cells (2026-07-19,
 * switched from one dedicated texture per entry to atlas cells -- see the
 * header comment on N3DS_C3DUploadDecodedCell in n3ds_backend.h for why).
 * Flat index 0 is reserved for a permanent flat opaque-white texel
 * (untextured/Gouraud primitives -- N3DS_WHITE_ATLAS/CELL_X/CELL_Y below),
 * never entered into the LRU cache; indices 1..N3DS_TOTAL_CELLS-1 are real,
 * split between two tiers as of the sub-page cache below.
 */
#define N3DS_TOTAL_CELLS (N3DS_ATLAS_COUNT * N3DS_ATLAS_CELLS_PER_ATLAS)
#define N3DS_WHITE_ATLAS  0
#define N3DS_WHITE_CELL_X 0
#define N3DS_WHITE_CELL_Y 0

/*
 * Sub-page decode cache (docs/SUBPAGE_DECODE_CACHE_DESIGN.md): a static split
 * of the same N3DS_TOTAL_CELLS physical atlas cells, not a separate memory
 * budget. Cell 0 stays the reserved white cell; cells 1..N3DS_FULLPAGE_RESERVED_CELLS
 * back the whole-page tier below (unchanged logic, just a smaller array); the
 * remaining physical cells back a new sub-page tier, each physical cell
 * further subdivided into a 4x4 grid of 64x64 sub-cells.
 *
 * 96 is a corrected first-pass number, not yet a fully measured one -- the
 * design doc's original guess of 32 was shipped, boot-tested, and measured
 * WRONG the same day: a menu background scene (hills/road/tunnel/sky) alone
 * peaked at 25/32 full-page cells with evictInUse=11846, i.e. severe
 * mid-frame-eviction-hazard pressure -- the exact mechanism session 9/10 tied
 * directly to visible corruption. Meanwhile the sub-page tier peaked at only
 * ~86/1520 slots across the same content, so it had enormous unused headroom
 * to give back. Growing this further necessarily shrinks the sub-page tier
 * (both draw from the same fixed 127-cell pool, see the design doc's
 * capacity-tension note) -- 96/31 leaves the sub-page tier 31*16=496 slots,
 * still ~6x its observed peak. Race content (the scenario the design doc
 * predicted would stress this tier hardest) has not yet been measured against
 * either number.
 */
#define N3DS_FULLPAGE_RESERVED_CELLS    96
#define N3DS_SUBPAGE_PHYSICAL_CELLS     (N3DS_TOTAL_CELLS - 1 - N3DS_FULLPAGE_RESERVED_CELLS)
#define N3DS_SUBPAGE_CELLS_PER_PHYSICAL 16
#define N3DS_SUBPAGE_TOTAL_CELLS        (N3DS_SUBPAGE_PHYSICAL_CELLS * N3DS_SUBPAGE_CELLS_PER_PHYSICAL)
#define N3DS_SUBPAGE_DIM                64
#define N3DS_SUBPAGE_PHYSICAL_BASE      (1 + N3DS_FULLPAGE_RESERVED_CELLS)

global_variable N3DS_DecodeCacheEntry s_decodeCache[1 + N3DS_FULLPAGE_RESERVED_CELLS];

/* Flat cell index -> which atlas texture, and which cell within its grid. */
internal void N3DS_CellIndexToAtlasCoord(int cellIndex, int *outAtlas, int *outCellX, int *outCellY)
{
	const int within = cellIndex % N3DS_ATLAS_CELLS_PER_ATLAS;

	*outAtlas = cellIndex / N3DS_ATLAS_CELLS_PER_ATLAS;
	*outCellX = within % N3DS_ATLAS_CELLS_PER_ROW;
	*outCellY = within / N3DS_ATLAS_CELLS_PER_ROW;
}

/*
 * Sub-page cache entry -- same shape as N3DS_DecodeCacheEntry plus tileX/tileY,
 * the key's sub-page component (which 64x64 tile of the page, 0-3 each).
 * pageGen is the generation of the ONE VRAM dirty-cell exactly covering this
 * tile (a 64-aligned sub-tile's absolute VRAM origin is always exactly one
 * N3DS_VRAM_CELL_DIM cell -- see the design doc's "useful coincidence" note);
 * clutGenA/B are the identical two-cell check the full-page tier uses (same
 * pre-existing straddle gap, inherited verbatim, not fixed here).
 */
typedef struct
{
	int used;
	s16 page;
	s16 clut;
	TexFormat format;
	u8 tileX, tileY;
	u32 pageGen;
	u32 clutGenA, clutGenB;
	u32 lastUsedFrame;
} N3DS_SubPageCacheEntry;

global_variable N3DS_SubPageCacheEntry s_subPageCache[N3DS_SUBPAGE_TOTAL_CELLS];
/* Mirrors s_lastCellHit. -1 = no memo yet -- unlike the full-page array, index
 * 0 is a real, valid sub-page slot here (no reserved white cell at 0), so 0
 * can't double as the "no memo" sentinel the way it implicitly does not for
 * s_lastCellHit either (that one guards with `> 0`, this one with `>= 0`). */
global_variable int s_lastSubPageCellHit = -1;

/* Sub-page counterpart to N3DS_CellIndexToAtlasCoord -- reuses it unchanged,
 * offset into the physical-cell range the sub-page tier owns, then further
 * resolves which of the 16 sub-cells within that physical cell. */
internal void N3DS_SubCellIndexToAtlasCoord(int subIndex, int *outAtlas, int *outPhysCellX, int *outPhysCellY,
                                             int *outSubX, int *outSubY)
{
	const int physCellIndex = N3DS_SUBPAGE_PHYSICAL_BASE + (subIndex / N3DS_SUBPAGE_CELLS_PER_PHYSICAL);
	const int within = subIndex % N3DS_SUBPAGE_CELLS_PER_PHYSICAL;

	N3DS_CellIndexToAtlasCoord(physCellIndex, outAtlas, outPhysCellX, outPhysCellY);
	*outSubX = within % 4;
	*outSubY = within / 4;
}

/*
 * Cache diagnostics (2026-07-19, added to investigate a real/reported-fps
 * mismatch -- SwapWindow's own fps_now is a frames-per-emulated-vblank
 * ratio, which can look fine on average even when per-frame cost is spiky;
 * a decode is only expensive on a miss, so these numbers say directly
 * whether the cache is thrashing rather than leaving it a guess).
 * s_decodeCacheEvictions counts only misses that discarded a still-valid,
 * different entry -- i.e. genuine thrashing -- as opposed to a miss that
 * just filled a never-yet-used slot (expected once, during cache warm-up).
 */
global_variable u32 s_decodeCacheHits = 0;
global_variable u32 s_decodeCacheMisses = 0;
global_variable u32 s_decodeCacheEvictions = 0;
global_variable u32 s_lastStatCacheMisses = 0;
global_variable u32 s_lastStatCacheEvictions = 0;

/*
 * WHY a miss happened, which the counters above cannot distinguish and which
 * decides the whole architecture (2026-07-20). Menu content measures ~1151
 * misses / 1088 evictions per 32-frame window -- nearly one eviction per miss
 * -- but "the cache is too small" and "the cache keeps being invalidated" look
 * IDENTICAL in that pair, and they have opposite fixes:
 *
 *   - CAPACITY: no entry for this (page,clut,format) at all. The working set
 *     genuinely exceeds this tier's cell budget (N3DS_TOTAL_CELLS at the time
 *     this was measured; now 1+N3DS_FULLPAGE_RESERVED_CELLS for this counter
 *     specifically, after the sub-page split below). Fix = more/smaller
 *     cells, or decode a level's set once at load into persistent atlases.
 *   - STALE: an entry for exactly this (page,clut,format) IS resident, but a
 *     VRAM cell generation moved under it, so it re-decodes. Fix = dirty VRAM
 *     at finer granularity than 256x256, or stop the mutator. Decoding once at
 *     load would be WRONG here -- the source pixels really are changing.
 *
 * Decode-once-at-load is only viable if STALE is near zero. Measure before
 * committing to it: this project has already paid twice for optimising against
 * a premise that was never isolated (the atlas rework, and session 6's
 * "ruled out by measurement" list).
 */
global_variable u32 s_decodeCacheStaleMisses = 0;
global_variable u32 s_decodeCacheCapacityMisses = 0;
global_variable u32 s_lastStatStaleMisses = 0;
global_variable u32 s_lastStatCapacityMisses = 0;

/*
 * Distinct atlas cells touched in one frame -- an estimate of the live working
 * set, to compare against this tier's own cell budget (N3DS_FULLPAGE_RESERVED_CELLS
 * after the sub-page split below; was N3DS_TOTAL_CELLS before). Counted by
 * noticing an entry whose lastUsedFrame was not already this frame, so it
 * SATURATES at the cache size when capacity binds: reading the budget means
 * "at least that many", not "exactly that many". Read it together with the
 * capacity/stale split, not alone.
 */
global_variable u32 s_frameTouchedCells = 0;
global_variable u32 s_frameTouchedPeak = 0;

/*
 * Last cell that satisfied a lookup. Checked before the full scan because
 * consecutive triangles in a split overwhelmingly share one (page,clut) --
 * MakeTexcoordTriangle/Quad emit runs of primitives off the same texture --
 * so this turns the common case from a 63-entry scan into one compare.
 *
 * Measured 2026-07-20, and this is why it exists: renderer_draw_triangles_ms
 * was 16-18ms of a 36-63ms frame (~70% of render_submit_ms, the top bucket)
 * at ~2040 triangles/frame, i.e. ~8us per triangle -- while the cache was
 * hitting 362,396 times against 1,326 misses. The cost was never decoding;
 * it was the linear scan on the way to a hit. Safe against eviction without
 * any invalidation step: the memo re-checks the full key, so a stale index
 * just falls through to the scan.
 */
global_variable int s_lastCellHit = 0;

/*
 * Misses that had to evict a cell already referenced by a draw QUEUED earlier
 * this same frame. citro3d executes nothing until C3D_FrameEnd, but the cell
 * upload is an immediate CPU memcpy, so overwriting such a cell silently
 * changes the texture an already-queued triangle will sample. Nonzero here
 * means on-screen corruption; see the victim search in FindOrDecodeTexture.
 */
global_variable u32 s_evictHazards = 0;

/*
 * Sub-page tier diagnostics -- field-for-field mirror of the full-page
 * counters above; see those comments for what each one means. lastStat*
 * variants exist only to compute the same windowed (+N/w) deltas the
 * stats block already prints for the full-page tier.
 */
global_variable u32 s_subPageCacheHits = 0;
global_variable u32 s_subPageCacheMisses = 0;
global_variable u32 s_subPageCacheEvictions = 0;
global_variable u32 s_subPageLastStatCacheMisses = 0;
global_variable u32 s_subPageLastStatCacheEvictions = 0;
global_variable u32 s_subPageCacheStaleMisses = 0;
global_variable u32 s_subPageCacheCapacityMisses = 0;
global_variable u32 s_subPageLastStatStaleMisses = 0;
global_variable u32 s_subPageLastStatCapacityMisses = 0;
global_variable u32 s_subPageFrameTouchedCells = 0;
global_variable u32 s_subPageFrameTouchedPeak = 0;
global_variable u32 s_subPageEvictHazards = 0;

/*
 * Per-triangle routing outcome (N3DS_ResolveTriangleTexture) -- the single
 * most important new number: whether the straddling/large-surface fallback
 * to the full-page tier dominates real content or is genuinely the minority
 * case this design assumes (design doc "Instrumentation").
 */
global_variable u32 s_trianglesRoutedSubPage = 0;
global_variable u32 s_trianglesRoutedFullPage = 0;

/* One increment per actual C3D_DrawArrays call (N3DS_FlushRun, called from
 * DrawTriangles) -- checks the design doc's own predicted "more, smaller
 * draw calls than PC" risk directly; measured ~150-290/frame before the
 * atlas rework above, which is what motivated it. */
global_variable u32 s_drawCallCount = 0;

/*
 * Draw-call structure counters (session 15 perf survey). Session 14's first
 * real-hardware race put the frame-end GPU wait (platform_end_frame_ms,
 * 231-257ms of a ~350ms frame) on top with ~383 draws/frame, and real-PICA
 * per-draw overhead far exceeds Azahar's -- so draws/frame is the one
 * Azahar-measurable number that transfers. These split it into its
 * mechanisms: s_atlasSwitchFlushes counts run flushes forced by an ATLAS
 * CHANGE between consecutive triangles mid-split (the potentially reducible
 * kind -- cell-to-atlas allocation affinity could shrink them; submission
 * order itself must never change, layering depends on it);
 * s_distinctAtlasTotal accumulates each frame's popcount of distinct atlases
 * actually bound (the switching floor for the frame's content as allocated).
 */
global_variable u32 s_atlasSwitchFlushes = 0;
global_variable u32 s_frameAtlasMask = 0;
global_variable u32 s_distinctAtlasTotal = 0;
global_variable u32 s_frameDistinctAtlasPeak = 0;
global_variable u32 s_lastStatAtlasSwitchFlushes = 0;
global_variable u32 s_lastStatDistinctAtlasTotal = 0;
/* Reached only through DrawAllSplits, so uvb==0 means the OT walk never ran at
 * all, rather than DrawTriangles running and bailing. See the stats block. */
global_variable u32 s_updateVertexBufferCalls = 0;
global_variable u32 s_drawTrianglesCalls = 0;
/* Last DISPENV the projection was built for; -1 forces it on the first frame. */
global_variable int s_projectionDispW = -1;
global_variable int s_projectionDispH = -1;
global_variable u32 s_lastStatDrawCallCount = 0;

/* Scratch for one decoded+tiled page; reused every decode, never persisted.
 * One u16 per texel (GPU_RGBA5551) -- see N3DS_TILE_BYTES in n3ds_backend.c. */
global_variable u16 s_decodeScratch[N3DS_DECODE_TEXTURE_DIM * N3DS_DECODE_TEXTURE_DIM];

/* Scratch for one decoded+tiled 64x64 sub-cell; mirrors s_decodeScratch's
 * role, sized for N3DS_SUBPAGE_DIM instead of N3DS_DECODE_TEXTURE_DIM. */
global_variable u16 s_subpageDecodeScratch[N3DS_SUBPAGE_DIM * N3DS_SUBPAGE_DIM];

/* Scratch for one converted atlas-run. Bounded by N3DS_MAX_RUN_VERTICES,
 * NOT the whole-frame budget: a run is flushed whenever the atlas changes
 * or this fills, so it only ever stages one run at a time, and sizing it
 * per-frame would cost megabytes of BSS for space no single run can use. */
global_variable N3DS_Vertex s_convertScratch[N3DS_MAX_RUN_VERTICES];

/*
 * citro3d frame brackets actually opened/closed, versus s_frameCounter's
 * count of SwapWindow calls. These three should track each other 1:1:1.
 * They are counted because Azahar's own host-side readout said "App: 1 FPS"
 * while the in-guest counter reported ~13 -- a 13x gap that means either the
 * top screen is being presented far less often than the game believes it is
 * drawing, or the in-guest number is measuring the wrong thing. Guessing
 * which is exactly the mistake this port keeps paying for; count it instead.
 */
global_variable u32 s_beginSceneCount = 0;
global_variable u32 s_endSceneCount = 0;

/* Runs dropped because the frame's vertex budget was exhausted. This is
 * silent geometry loss -- whole draws vanish with nothing on screen to say
 * so -- which is exactly how a too-small budget hid for a session. Any
 * nonzero value here means content is missing. */
global_variable u32 s_droppedRuns = 0;

/*
 * Decodes PSX texture page `texPage` (through CLUT `clut` for 4/8-bit) into
 * a 256x256 RGBA8 buffer, applying the PICA200 8x8 Z-order (Morton) tile
 * swizzle inline -- citro3d's C3D_TexLoadImage is a raw memcpy and does not
 * tile for you (verified directly against citro3d's upstream source, see
 * docs/CITRO3D_RENDERER_DESIGN.md section 1). The intra-tile bit-interleave
 * below (x supplies the even bits, y the odd bits) was cross-checked
 * position-by-position against tex3ds's own reference swizzle table
 * (source/swizzle.cpp) rather than assumed.
 *
 * Addressing matches native_gpu.c exactly: page origin in VRAM word/pixel
 * coordinates is ((texPage & 0xF) * 64, (texPage & 0x10) ? 256 : 0) --
 * GetTPageBase/NativeGpu_TPageOverlapsActiveDrawPage agree, and the PC
 * fragment shader's v_page_clut derivation (native_renderer.c:613-616)
 * reduces to the same formula. CLUT origin is ((clut & 0x3F) * 16,
 * clut >> 6), same as the GET_CLUT_X/GET_CLUT_Y macros (native_gpu.c:43-44).
 * u/v (0-255) are native-texel coordinates within the page. A page is 256
 * TEXELS wide in every colour mode -- 8bpp spans two tpage columns of VRAM
 * words, 16bpp four. (An earlier revision claimed 8bpp could only address
 * u=0..127, conflating VRAM-word width with texel width, and under-decoded
 * every wide 8/16bpp texture -- fixed session 13; see the decodeWidth
 * comment at the full-page call site.)
 *
 * Alpha is the same 3-level encoding NativeRenderer_InitRG8LUT's channel
 * expansion feeds the PC shader (native_renderer.c:828-843): 0 = PS1
 * punch-through (raw value 0x0000), 128 = opaque, 255 = STP-on. Phase 2a
 * only consumes the 0-vs-nonzero distinction (single alpha test, see
 * n3ds_backend.c); the 128/255 split is decoded now so 2b's STP 2-pass
 * doesn't need to touch this function at all.
 *
 * Generalized 2026-07-20 for the sub-page decode cache (docs/
 * SUBPAGE_DECODE_CACHE_DESIGN.md): originU/originV/width/height say what
 * page-relative rectangle to decode (the page's own absolute VRAM origin is
 * still added internally, below); destStrideTiles says how wide the
 * DESTINATION buffer is, in tiles -- a property of which scratch buffer the
 * caller is writing into, and NOT derivable from width. They only happen to
 * agree for the sub-page tier (both always 64px/8 tiles); the full-page
 * tier's width is format-clamped (256/128/64) while its destination
 * (s_decodeScratch) is always 256px/32 tiles, so deriving stride from width
 * would silently desync from the unchanged uploader on every 8bpp/16bpp page
 * (caught in review before this shipped -- see the design doc's "What
 * changed from v1" section). Pass destStrideTiles explicitly, always.
 */
internal void N3DS_DecodeAndTilePage(s16 texPage, s16 clut, TexFormat format, int originU, int originV, int width,
                                      int height, int destStrideTiles, u16 *outTiled)
{
	const int pageOriginX = (texPage & 0xF) * 64;
	const int pageOriginY = (texPage & 0x10) ? 256 : 0;
	const int clutOriginX = (clut & 0x3F) * 16;
	const int clutOriginY = clut >> 6;
	int v;

	for (v = 0; v < height; v++)
	{
		const int wordRow = pageOriginY + originV + v;
		int u;

		for (u = 0; u < width; u++)
		{
			/* Page-relative source column -- VRAM addressing uses this, but
			 * the Morton swizzle and destination tile indexing below stay on
			 * the local, 0-based u/v (unchanged math either way). */
			const int pageU = originU + u;
			u16 color16;
			int morton, tileIndex, dstIndex;

			switch (format)
			{
			case TF_4_BIT:
			{
				const int wordCol = (pageOriginX + (pageU >> 2)) & (VRAM_WIDTH - 1);
				const u16 word = vram[wordRow * VRAM_WIDTH + wordCol];
				const int nibble = (word >> ((pageU & 3) * 4)) & 0xF;
				/* Wrap within the CLUT's own VRAM row. PC's VRAM sampler
				 * inherits GL_REPEAT and wraps S independently of T, so it
				 * can never walk into the next scanline; a flat array index
				 * silently would. A 4-bit CLUT (16 entries, 16-aligned
				 * origin) can't actually overflow -- masked for symmetry
				 * with the 8-bit case below, which can. */
				color16 = vram[clutOriginY * VRAM_WIDTH + ((clutOriginX + nibble) & (VRAM_WIDTH - 1))];
				break;
			}
			case TF_8_BIT:
			{
				const int wordCol = (pageOriginX + (pageU >> 1)) & (VRAM_WIDTH - 1);
				const u16 word = vram[wordRow * VRAM_WIDTH + wordCol];
				const int index = (word >> ((pageU & 1) * 8)) & 0xFF;
				/* 256 entries from a 16-aligned origin reaches column 1263 of
				 * a 1024-wide row, i.e. 239 texels into the NEXT scanline.
				 * Wrap instead, matching PC's GL_REPEAT S-wrap (see the 4-bit
				 * case above). */
				color16 = vram[clutOriginY * VRAM_WIDTH + ((clutOriginX + index) & (VRAM_WIDTH - 1))];
				break;
			}
			case TF_16_BIT:
			case TF_32_BIT_RGBA: /* unreachable -- DrawTriangles never calls this path for it, see file header */
			default:
				color16 = vram[wordRow * VRAM_WIDTH + ((pageOriginX + pageU) & (VRAM_WIDTH - 1))];
				break;
			}

			morton = (u & 1) | ((v & 1) << 1) | ((u & 2) << 1) | ((v & 2) << 2) | ((u & 4) << 2) | ((v & 4) << 3);
			tileIndex = ((v >> 3) * destStrideTiles + (u >> 3)) * 64;
			dstIndex = tileIndex + morton;

			/*
			 * PSX BGR555 -> PICA200 GPU_RGBA5551. Both are 16-bit and both keep
			 * 5 bits per channel, so this is a bit shuffle with no expansion or
			 * loss -- unlike the previous RGBA8 path, which widened every
			 * channel to 8 bits and wrote four separate bytes.
			 *
			 * Source (PSX): bit 15 = STP, 14-10 = B, 9-5 = G, 4-0 = R.
			 * Dest (PICA):  15-11 = R, 10-6 = G, 5-1 = B, 0 = A.
			 *
			 * Destination layout verified EMPIRICALLY, not inferred from the
			 * format name -- inferring is precisely what made session 6 rule
			 * out a byte-order bug that turned out to be the bug. Converting a
			 * solid R=0xF8,A=0xFF PNG with `tex3ds -r -z none -f rgba5551`
			 * emits `01 f8` per texel, i.e. 0xF801 little-endian = R=31, G=0,
			 * B=0, A=1. That pins R to the top 5 bits and alpha to bit 0.
			 *
			 * Alpha is now one bit: 0 for the PS1 "pure black is invisible"
			 * punch-through case, 1 otherwise. That is exactly what the single
			 * C3D_AlphaTest(GPU_GEQUAL, 1) consumes. The old three-level
			 * 0/128/255 encoding existed for phase 2b's STP two-pass blend,
			 * which is not built and which can recover the STP bit straight
			 * from VRAM (bit 15, still right there) when it is.
			 */
			outTiled[dstIndex] = (u16)(((color16 & 0x001Fu) << 11) |       /* R */
			                          (((color16 >> 5) & 0x1Fu) << 6) |   /* G */
			                          (((color16 >> 10) & 0x1Fu) << 1) |  /* B */
			                          ((color16 != 0) ? 1u : 0u));        /* punch-through */
		}
	}
}

/*
 * Finds an atlas cell already decoded for (texPage, clut, format) and still
 * fresh (matching VRAM dirty-cell generations), or decodes into the
 * least-recently-used atlas cell on a miss. Returns a flat cell index (see
 * N3DS_CellIndexToAtlasCoord). Linear scan over 1+N3DS_FULLPAGE_RESERVED_CELLS
 * (this tier's whole-page cell budget, since docs/SUBPAGE_DECODE_CACHE_DESIGN.md's
 * two-tier split -- was N3DS_TOTAL_CELLS before) -- fine for a
 * correctness-first pass at this count; the design doc explicitly leaves
 * perf tuning for after on-device measurement. Note "cell" means two
 * different things in this function and both are real: VRAM dirty-tracking
 * cells (N3DS_VRAM_CELL_*, 256x256 regions of PSX VRAM) versus atlas cells
 * (256x256 regions of a shared GPU texture) -- they are unrelated grids that
 * happen to share a name and a size.
 */
internal int N3DS_FindOrDecodeTexture(s16 texPage, s16 clut, TexFormat format)
{
	const int pageCell = N3DS_VRAMCellIndex((texPage & 0xF) * 64, (texPage & 0x10) ? 256 : 0);
	const int clutOriginX = (clut & 0x3F) * 16;
	const int clutOriginY = clut >> 6;
	const int clutCellA = N3DS_VRAMCellIndex(clutOriginX, clutOriginY);
	/* An 8-bit CLUT is 256 texels wide and can straddle a 256-wide VRAM cell
	 * boundary when its 16-aligned origin isn't itself cell-aligned (a
	 * 4-bit CLUT is only 16 wide and can never cross one). Track the far
	 * edge's cell too so a change there isn't missed. */
	const int clutCellB = N3DS_VRAMCellIndex(clutOriginX + 255, clutOriginY);
	/* Hoisted out of the scan: these three were re-read from s_vramCellGen on
	 * every one of up to 63 iterations, per triangle. */
	const u32 pageGen = s_vramCellGen[pageCell];
	const u32 clutGenA = s_vramCellGen[clutCellA];
	const u32 clutGenB = s_vramCellGen[clutCellB];
	int i, oldestSlot;
	u32 oldestFrame;
	/* Set when a cell holds exactly this (page,clut,format) but its source VRAM
	 * generation has moved -- i.e. the texture IS resident and we are re-decoding
	 * it anyway. Separates the two miss causes; see s_decodeCacheStaleMisses. */
	int sawStaleKeyMatch = 0;

	/* Fast path: the cell that answered the previous lookup. See s_lastCellHit. */
	if (s_lastCellHit > 0)
	{
		N3DS_DecodeCacheEntry *e = &s_decodeCache[s_lastCellHit];

		if (e->used && (e->page == texPage) && (e->clut == clut) && (e->format == format) && (e->pageGen == pageGen) &&
		    (e->clutGenA == clutGenA) && (e->clutGenB == clutGenB))
		{
			if (e->lastUsedFrame != s_frameCounter)
			{
				s_frameTouchedCells++;
			}
			e->lastUsedFrame = s_frameCounter;
			s_decodeCacheHits++;
			return s_lastCellHit;
		}
	}

	for (i = 1; i < (1 + N3DS_FULLPAGE_RESERVED_CELLS); i++)
	{
		N3DS_DecodeCacheEntry *e = &s_decodeCache[i];

		/* Key first, cheapest rejection first. The LRU victim search that used
		 * to share this loop has moved below: it is only needed on a miss, and
		 * misses are rare (measured 0-25 per 32-frame window against ~65,000
		 * lookups), so it was pure overhead on every hit. */
		if (!e->used || (e->page != texPage) || (e->clut != clut) || (e->format != format))
		{
			continue;
		}

		if ((e->pageGen == pageGen) && (e->clutGenA == clutGenA) && (e->clutGenB == clutGenB))
		{
			if (e->lastUsedFrame != s_frameCounter)
			{
				s_frameTouchedCells++;
			}
			e->lastUsedFrame = s_frameCounter;
			s_decodeCacheHits++;
			s_lastCellHit = i;
			return i;
		}

		sawStaleKeyMatch = 1;
	}

	s_decodeCacheMisses++;
	if (sawStaleKeyMatch)
	{
		s_decodeCacheStaleMisses++;
	}
	else
	{
		s_decodeCacheCapacityMisses++;
	}

	/*
	 * Pick the victim. Skip any cell already stamped with this frame: a draw
	 * referencing it is queued but unexecuted (citro3d runs the whole command
	 * list at C3D_FrameEnd) while N3DS_C3DUploadDecodedCell memcpys over it
	 * immediately, so evicting one silently repoints an already-queued triangle
	 * at a different texture. That is the mid-frame eviction hazard, and it is
	 * the same class of bug as the vertex-buffer aliasing fixed in session 6.
	 *
	 * The old code could not avoid it and made it worse: it initialised
	 * oldestSlot=1 and compared with strict <, so once every cell carried the
	 * current frame's stamp, every further miss re-evicted slot 1 specifically.
	 *
	 * Must also check `used`: an entry that has never been decoded into can
	 * never be referenced by a queued draw, so it is always a safe victim
	 * regardless of its lastUsedFrame value. Without this, frame 0 breaks --
	 * s_frameCounter starts at 0 and every entry's zero-initialised
	 * lastUsedFrame also reads 0, so EVERY cell (not just genuinely-touched
	 * ones) matches the skip condition on the very first frame, oldestSlot
	 * never gets set, and the code falls into the "every cell spoken for"
	 * hazard branch below on every miss that frame -- exactly the same-slot
	 * re-eviction bug this rewrite was meant to fix, reintroduced for frame 0.
	 */
	oldestSlot = -1;
	oldestFrame = 0xFFFFFFFFu;
	for (i = 1; i < (1 + N3DS_FULLPAGE_RESERVED_CELLS); i++)
	{
		N3DS_DecodeCacheEntry *e = &s_decodeCache[i];

		if (e->used && (e->lastUsedFrame == s_frameCounter))
		{
			continue;
		}

		if (e->lastUsedFrame < oldestFrame)
		{
			oldestFrame = e->lastUsedFrame;
			oldestSlot = i;
		}
	}

	if (oldestSlot < 0)
	{
		/*
		 * Every cell is spoken for by a queued draw. Corruption is unavoidable
		 * without splitting the frame (C3D_FrameSplit, which would flush the
		 * queued draws first); that is the real fix and is not done here. Count
		 * it so the condition is visible rather than silent -- if this stays 0
		 * in real content, the frame split is not worth building.
		 */
		s_evictHazards++;
		oldestSlot = 1;
	}

	if (s_decodeCache[oldestSlot].used)
	{
		s_decodeCacheEvictions++;
	}
	if (s_decodeCache[oldestSlot].lastUsedFrame != s_frameCounter)
	{
		s_frameTouchedCells++;
	}
	s_lastCellHit = oldestSlot;

	{
		/*
		 * A PSX texture page is 256 TEXELS wide in EVERY colour mode -- the
		 * mode only changes how many VRAM words those 256 texels occupy (64 at
		 * 4bpp, 128 at 8bpp, 256 at 16bpp), i.e. how many tpage columns the
		 * texture spans, NOT how far U can reach. U/V are always 0-255. A prior
		 * version decoded only 128 columns for 8bpp / 64 for 16bpp on the theory
		 * that "the game never samples beyond that" -- that was wrong (it
		 * confused word-width with texel-width), and it left the right half (or
		 * three-quarters) of every wide 8/16bpp texture stale, which is exactly
		 * the large-surface corruption. Decode the full 256 always; the wider
		 * VRAM reads are wrap-guarded in N3DS_DecodeAndTilePage (matching PC's
		 * GL_REPEAT). destStrideTiles stays N3DS_DECODE_TEXTURE_DIM/8, the
		 * destination scratch's own width in tiles.
		 */
		const int decodeWidth = N3DS_DECODE_TEXTURE_DIM;

		N3DS_DecodeAndTilePage(texPage, clut, format, 0, 0, decodeWidth, N3DS_DECODE_TEXTURE_DIM,
		                        N3DS_DECODE_TEXTURE_DIM / 8, s_decodeScratch);
	}
	{
		int atlasIdx, cellX, cellY;

		N3DS_CellIndexToAtlasCoord(oldestSlot, &atlasIdx, &cellX, &cellY);
		N3DS_C3DUploadDecodedCell(atlasIdx, cellX, cellY, s_decodeScratch);
	}

	s_decodeCache[oldestSlot].used = 1;
	s_decodeCache[oldestSlot].page = texPage;
	s_decodeCache[oldestSlot].clut = clut;
	s_decodeCache[oldestSlot].format = format;
	s_decodeCache[oldestSlot].pageGen = s_vramCellGen[pageCell];
	s_decodeCache[oldestSlot].clutGenA = s_vramCellGen[clutCellA];
	s_decodeCache[oldestSlot].clutGenB = s_vramCellGen[clutCellB];
	s_decodeCache[oldestSlot].lastUsedFrame = s_frameCounter;

	return oldestSlot;
}

/*
 * Sub-page counterpart to N3DS_FindOrDecodeTexture above -- same memo/scan/
 * stale-vs-capacity/hazard-aware-LRU logic, additionally keyed on which 64x64
 * tile of the page (tileX,tileY), operating over s_subPageCache/
 * N3DS_SUBPAGE_TOTAL_CELLS instead of s_decodeCache/(1+N3DS_FULLPAGE_RESERVED_CELLS).
 * Every bound below is 0-based, NOT 1-based like the full-page tier: index 0
 * is a real, valid sub-page slot here (no reserved white cell at 0 in this
 * array -- see s_lastSubPageCellHit's own comment). Copy-pasting the
 * full-page tier's 1-based bounds here would be silently wrong: the memo
 * guard `> 0` must be `>= 0`, both scan loops start at `i = 0` not `i = 1`,
 * and the hazard-fallback victim is slot 0, not 1.
 */
internal int N3DS_FindOrDecodeSubPageTexture(s16 texPage, s16 clut, TexFormat format, int tileX, int tileY)
{
	const int pageOriginX = (texPage & 0xF) * 64;
	const int pageOriginY = (texPage & 0x10) ? 256 : 0;
	/* The one VRAM dirty-cell exactly covering this 64x64 tile -- both
	 * pageOriginX/Y and tileX*64/tileY*64 are multiples of N3DS_VRAM_CELL_DIM,
	 * so this is exact, not an approximation (see the struct's own comment). */
	const int pageCell = N3DS_VRAMCellIndex(pageOriginX + (tileX * 64), pageOriginY + (tileY * 64));
	const int clutOriginX = (clut & 0x3F) * 16;
	const int clutOriginY = clut >> 6;
	const int clutCellA = N3DS_VRAMCellIndex(clutOriginX, clutOriginY);
	const int clutCellB = N3DS_VRAMCellIndex(clutOriginX + 255, clutOriginY);
	const u32 pageGen = s_vramCellGen[pageCell];
	const u32 clutGenA = s_vramCellGen[clutCellA];
	const u32 clutGenB = s_vramCellGen[clutCellB];
	int i, oldestSlot;
	u32 oldestFrame;
	int sawStaleKeyMatch = 0;

	/* Fast path: the cell that answered the previous lookup. See s_lastSubPageCellHit. */
	if (s_lastSubPageCellHit >= 0)
	{
		N3DS_SubPageCacheEntry *e = &s_subPageCache[s_lastSubPageCellHit];

		if (e->used && (e->page == texPage) && (e->clut == clut) && (e->format == format) && (e->tileX == tileX) &&
		    (e->tileY == tileY) && (e->pageGen == pageGen) && (e->clutGenA == clutGenA) && (e->clutGenB == clutGenB))
		{
			if (e->lastUsedFrame != s_frameCounter)
			{
				s_subPageFrameTouchedCells++;
			}
			e->lastUsedFrame = s_frameCounter;
			s_subPageCacheHits++;
			return s_lastSubPageCellHit;
		}
	}

	for (i = 0; i < N3DS_SUBPAGE_TOTAL_CELLS; i++)
	{
		N3DS_SubPageCacheEntry *e = &s_subPageCache[i];

		if (!e->used || (e->page != texPage) || (e->clut != clut) || (e->format != format) || (e->tileX != tileX) ||
		    (e->tileY != tileY))
		{
			continue;
		}

		if ((e->pageGen == pageGen) && (e->clutGenA == clutGenA) && (e->clutGenB == clutGenB))
		{
			if (e->lastUsedFrame != s_frameCounter)
			{
				s_subPageFrameTouchedCells++;
			}
			e->lastUsedFrame = s_frameCounter;
			s_subPageCacheHits++;
			s_lastSubPageCellHit = i;
			return i;
		}

		sawStaleKeyMatch = 1;
	}

	s_subPageCacheMisses++;
	if (sawStaleKeyMatch)
	{
		s_subPageCacheStaleMisses++;
	}
	else
	{
		s_subPageCacheCapacityMisses++;
	}

	/* Victim search -- see N3DS_FindOrDecodeTexture's own comment above for
	 * the full reasoning (mid-frame eviction hazard; frame-0 all-zero-
	 * lastUsedFrame guard). Identical here except the hazard-fallback victim
	 * is slot 0, not 1 -- see this function's own header comment. */
	oldestSlot = -1;
	oldestFrame = 0xFFFFFFFFu;
	for (i = 0; i < N3DS_SUBPAGE_TOTAL_CELLS; i++)
	{
		N3DS_SubPageCacheEntry *e = &s_subPageCache[i];

		if (e->used && (e->lastUsedFrame == s_frameCounter))
		{
			continue;
		}

		if (e->lastUsedFrame < oldestFrame)
		{
			oldestFrame = e->lastUsedFrame;
			oldestSlot = i;
		}
	}

	if (oldestSlot < 0)
	{
		s_subPageEvictHazards++;
		oldestSlot = 0;
	}

	if (s_subPageCache[oldestSlot].used)
	{
		s_subPageCacheEvictions++;
	}
	if (s_subPageCache[oldestSlot].lastUsedFrame != s_frameCounter)
	{
		s_subPageFrameTouchedCells++;
	}
	s_lastSubPageCellHit = oldestSlot;

	/*
	 * originU/originV are PAGE-RELATIVE (0-based within the page), matching
	 * the convention N3DS_ResolveTriangleTexture's outOriginU/V use below --
	 * N3DS_DecodeAndTilePage adds the page's own absolute VRAM origin
	 * internally, so do NOT add pageOriginX/Y here too (that double-counts
	 * the page origin and reads off the bottom of VRAM for any bottom-half
	 * page -- caught in review before this shipped, see the design doc's
	 * "What changed from v1" section). width=height=64 always exactly fills
	 * s_subpageDecodeScratch, unlike the full-page tier's format-clamped
	 * decodeWidth<256 cases, which leave stale columns from a prior decode.
	 */
	N3DS_DecodeAndTilePage(texPage, clut, format, tileX * 64, tileY * 64, N3DS_SUBPAGE_DIM, N3DS_SUBPAGE_DIM,
	                        N3DS_SUBPAGE_DIM / 8, s_subpageDecodeScratch);
	{
		int atlasIdx, physCellX, physCellY, subX, subY;

		N3DS_SubCellIndexToAtlasCoord(oldestSlot, &atlasIdx, &physCellX, &physCellY, &subX, &subY);
		N3DS_C3DUploadDecodedSubCell(atlasIdx, physCellX, physCellY, subX, subY, s_subpageDecodeScratch);
	}

	s_subPageCache[oldestSlot].used = 1;
	s_subPageCache[oldestSlot].page = texPage;
	s_subPageCache[oldestSlot].clut = clut;
	s_subPageCache[oldestSlot].format = format;
	s_subPageCache[oldestSlot].tileX = (u8)tileX;
	s_subPageCache[oldestSlot].tileY = (u8)tileY;
	s_subPageCache[oldestSlot].pageGen = s_vramCellGen[pageCell];
	s_subPageCache[oldestSlot].clutGenA = s_vramCellGen[clutCellA];
	s_subPageCache[oldestSlot].clutGenB = s_vramCellGen[clutCellB];
	s_subPageCache[oldestSlot].lastUsedFrame = s_frameCounter;

	return oldestSlot;
}

/*
 * Binds atlas `atlasIndex` and draws the vertCount vertices already sitting
 * in s_convertScratch. Appends (does not overwrite) into the shared
 * per-frame vertex buffer and draws from wherever they actually landed --
 * NOT from a hardcoded offset. That distinction is load-bearing: GPU
 * command execution for a whole frame is deferred to a single
 * GX_ProcessCommandList call at frame end (see n3ds_backend.h's header
 * comment on N3DS_C3DAppendVertices), and DrawTriangles is called once per
 * split -- up to thousands of times a frame -- so many runs get queued
 * before any of them actually executes. Drawing from offset 0 every time
 * (the bug this replaced, 2026-07-19) made every queued draw in the frame
 * read whichever run's vertices were written last, since nothing stops a
 * later CPU-side write from landing before an earlier draw's GPU read.
 */
internal void N3DS_FlushRun(int atlasIndex, int vertCount)
{
	int baseVertex;

	if (vertCount <= 0)
	{
		return;
	}

	baseVertex = N3DS_C3DAppendVertices(s_convertScratch, vertCount);
	if (baseVertex < 0)
	{
		/* Frame vertex budget exhausted -- drop this run rather than draw
		 * from a wrong/stale offset. Counted and reported, never silent:
		 * the budget was previously sized from PC's PER-PASS ceiling while
		 * covering a whole frame of up to three passes, and the resulting
		 * dropped draws looked like missing models rather than an error. */
		s_droppedRuns++;
		return;
	}

	N3DS_C3DBindAtlas(atlasIndex);
	N3DS_C3DDrawTriangles(baseVertex, vertCount / 3);
	s_drawCallCount++;
	s_frameAtlasMask |= (1u << (atlasIndex & 31));
}

/* This codebase has no 3-way min/max anywhere -- native_libgte.c's
 * fst_min/fst_max are 2-argument and, being defined in a different
 * translation unit's file scope, not usable from here regardless. Trivial
 * enough not to warrant sharing; kept local to where N3DS_ResolveTriangleTexture
 * needs them. */
internal inline int MIN3(int a, int b, int c)
{
	return (a < b) ? ((a < c) ? a : c) : ((b < c) ? b : c);
}

internal inline int MAX3(int a, int b, int c)
{
	return (a > b) ? ((a > c) ? a : c) : ((b > c) ? b : c);
}

/*
 * Routes one triangle to the sub-page tier (common case: its UV bounding box
 * fits one 64x64 grid-aligned tile) or the full-page tier (fallback: it
 * straddles a tile boundary or is simply larger than 64x64 -- e.g. a large
 * floor/wall/sky surface). GrVertex.u/.v are raw u8 (0-255) with no alignment
 * guarantee, so this bbox-vs-tile test must run per triangle, not once per
 * (page,clut): two triangles sharing a texture can still have different UV
 * footprints (docs/SUBPAGE_DECODE_CACHE_DESIGN.md). Both branches resolve to
 * the same output shape (pixelBase/origin), so the caller's UV assignment is
 * one formula for both tiers -- see NativeRenderer_DrawTriangles.
 */
internal int N3DS_ResolveTriangleTexture(const GrVertex *v0, const GrVertex *v1, const GrVertex *v2, s16 texPage,
                                          s16 clut, TexFormat format, int *outAtlasIdx, int *outPixelBaseU,
                                          int *outPixelBaseV, int *outOriginU, int *outOriginV)
{
	const int minU = MIN3(v0->u, v1->u, v2->u), maxU = MAX3(v0->u, v1->u, v2->u);
	const int minV = MIN3(v0->v, v1->v, v2->v), maxV = MAX3(v0->v, v1->v, v2->v);
	const int tileX0 = minU / N3DS_SUBPAGE_DIM, tileX1 = maxU / N3DS_SUBPAGE_DIM;
	const int tileY0 = minV / N3DS_SUBPAGE_DIM, tileY1 = maxV / N3DS_SUBPAGE_DIM;

	if (!g_dbg_disableSubPage && (tileX0 == tileX1) && (tileY0 == tileY1))
	{
		int atlasIdx, physCellX, physCellY, subX, subY;

		N3DS_SubCellIndexToAtlasCoord(N3DS_FindOrDecodeSubPageTexture(texPage, clut, format, tileX0, tileY0), &atlasIdx,
		                               &physCellX, &physCellY, &subX, &subY);
		*outAtlasIdx = atlasIdx;
		*outPixelBaseU = (physCellX * N3DS_DECODE_TEXTURE_DIM) + (subX * N3DS_SUBPAGE_DIM);
		*outPixelBaseV = (physCellY * N3DS_DECODE_TEXTURE_DIM) + (subY * N3DS_SUBPAGE_DIM);
		*outOriginU = tileX0 * N3DS_SUBPAGE_DIM;
		*outOriginV = tileY0 * N3DS_SUBPAGE_DIM;
		s_trianglesRoutedSubPage++;
		return 1;
	}

	{
		int atlasIdx, cellX, cellY;

		N3DS_CellIndexToAtlasCoord(N3DS_FindOrDecodeTexture(texPage, clut, format), &atlasIdx, &cellX, &cellY);
		*outAtlasIdx = atlasIdx;
		*outPixelBaseU = cellX * N3DS_DECODE_TEXTURE_DIM;
		*outPixelBaseV = cellY * N3DS_DECODE_TEXTURE_DIM;
		*outOriginU = 0;
		*outOriginV = 0;
		s_trianglesRoutedFullPage++;
		return 0;
	}
}

int NativeRenderer_InitialiseRender(char *windowName, int width, int height, int fullscreen)
{
	(void)fullscreen;

	N3DS_GfxInit();

	if (!N3DS_C3DInit())
	{
		N3DS_DebugLog("[CTR-3DS] citro3d init FAILED");
		return 0;
	}

	s_rendererInitialised = 1;

	printf("\x1b[2J");
	printf("CTR-Native / Nintendo 3DS\n");
	printf("-------------------------\n");
	printf("%s\n", (windowName != NULL) ? windowName : "(no title)");
	printf("PSX display: %dx%d\n", width, height);
	printf("VRAM mirror: %d KiB\n", (int)(sizeof(vram) / 1024));
	printf("\nPhase 2a: citro3d is live on the top\nscreen (see file header for scope).\n\n");

	N3DS_DebugLog("[CTR-3DS] renderer init ok, citro3d ready");

	/* Hand the backend TU the VRAM buffer so its SELECT+R debug capture can
	 * write it to SD (see N3DS_DebugSetVRAMForDump). vram[] and s_frameCounter
	 * are stable globals, so registering their addresses once is safe. */
	N3DS_DebugSetVRAMForDump(vram, (unsigned int)sizeof(vram), &s_frameCounter);

	return 1;
}

int NativeRenderer_InitialisePSX(void)
{
	int i;

	memset(vram, 0, sizeof(vram));

	for (i = 0; i < (N3DS_VRAM_CELL_ROWS * N3DS_VRAM_CELL_COLS); i++)
	{
		s_vramCellGen[i] = 0;
	}
	for (i = 0; i < (1 + N3DS_FULLPAGE_RESERVED_CELLS); i++)
	{
		s_decodeCache[i].used = 0;
		s_decodeCache[i].lastUsedFrame = 0;
	}
	for (i = 0; i < N3DS_SUBPAGE_TOTAL_CELLS; i++)
	{
		s_subPageCache[i].used = 0;
		s_subPageCache[i].lastUsedFrame = 0;
	}

	/*
	 * White cell: flat opaque texel for untextured/Gouraud splits. Uniform
	 * colour makes the 8x8 tile swizzle a no-op (every permutation of identical
	 * values is identical), so this just fills linearly. Uploaded once here
	 * into atlas 0's cell (0,0); that cell is never entered into the LRU cache
	 * and never re-decoded.
	 *
	 * Must use the same GPU_RGBA5551 layout as N3DS_DecodeAndTilePage -- it
	 * feeds every untextured primitive, so a mismatch here inverts the bug onto
	 * flat-shaded geometry instead of textured. 0xFFFF = R,G,B all 31, alpha 1.
	 */
	for (i = 0; i < (N3DS_DECODE_TEXTURE_DIM * N3DS_DECODE_TEXTURE_DIM); i++)
	{
		s_decodeScratch[i] = 0xFFFFu;
	}
	N3DS_C3DUploadDecodedCell(N3DS_WHITE_ATLAS, N3DS_WHITE_CELL_X, N3DS_WHITE_CELL_Y, s_decodeScratch);

	return 1;
}

void NativeRenderer_Shutdown(void)
{
	if (!s_rendererInitialised)
	{
		return;
	}

	s_rendererInitialised = 0;
	N3DS_C3DExit();
	N3DS_GfxExit();
}

void NativeRenderer_ResetDevice(void)
{
}

void NativeRenderer_BeginScene(void)
{
	/*
	 * Opens the citro3d frame bracket. Must happen here, not in SwapWindow:
	 * real drawing happens via DrawAllSplits, which DrawOTag calls once near
	 * the start of the frame (and DrawSync can call again mid-frame) -- long
	 * before SwapWindow runs at frame end. Platform_BeginScene (the sole
	 * caller) already guards re-entry within a frame, so this only opens the
	 * bracket once regardless of how many times DrawAllSplits runs.
	 */
	if (!s_rendererInitialised)
	{
		return;
	}

	s_beginSceneCount++;

	/* Working-set estimate is per frame, so latch its peak and restart the
	 * count at the frame boundary. BeginScene is guarded against re-entry by
	 * Platform_BeginScene, so this runs exactly once per frame. */
	if (s_frameTouchedCells > s_frameTouchedPeak)
	{
		s_frameTouchedPeak = s_frameTouchedCells;
	}
	s_frameTouchedCells = 0;

	/* Sub-page tier counterpart -- same latch-and-reset, see above. */
	if (s_subPageFrameTouchedCells > s_subPageFrameTouchedPeak)
	{
		s_subPageFrameTouchedPeak = s_subPageFrameTouchedCells;
	}
	s_subPageFrameTouchedCells = 0;

	/* Distinct-atlas-per-frame latch (see the counter block's header). */
	{
		u32 distinct = (u32)__builtin_popcount(s_frameAtlasMask);

		s_distinctAtlasTotal += distinct;
		if (distinct > s_frameDistinctAtlasPeak)
		{
			s_frameDistinctAtlasPeak = distinct;
		}
		s_frameAtlasMask = 0;
	}

	/*
	 * Fit the game's own display area to the panel, cropped to the PSX's
	 * intended 4:3 aspect rather than widened to the top screen's 5:3
	 * (2026-07-20). CTR runs a 512x216 DISPENV; previously this widened the
	 * ortho range to fill the full 400px panel width, which showed ~40 extra
	 * PSX-space columns of world per edge that were never meant to be on
	 * screen -- the source of the "can see past the edge of the track"
	 * jaggies. That's gone now: the range below is unwidened (native dispW),
	 * matched by a viewport crop in N3DS_C3DBeginFrame (n3ds_backend.c) so
	 * this native range isn't just re-stretched across the full 400px
	 * instead (which would trade the extra-content problem for a
	 * ~25%-too-wide distortion problem).
	 *
	 * Read from the live DISPENV rather than hardcoding 512x216: the game
	 * double-buffers two dispEnvs and could change modes, and this costs two
	 * integer compares a frame.
	 */
	{
		const int dispW = (activeDispEnv.disp.w > 0) ? activeDispEnv.disp.w : 512;
		const int dispH = (activeDispEnv.disp.h > 0) ? activeDispEnv.disp.h : 216;

		if ((dispW != s_projectionDispW) || (dispH != s_projectionDispH))
		{
			char line[96];

			s_projectionDispW = dispW;
			s_projectionDispH = dispH;
			N3DS_C3DSetProjection(0.0f, (float)dispW, (float)dispH);

			snprintf(line, sizeof(line), "[CTR-3DS] projection: disp %dx%d -> ortho 0..%d x 0..%d (4:3, viewport-cropped)", dispW, dispH, dispW, dispH);
			N3DS_DebugLog(line);
		}
	}

	NativePerf_BeginScope(NATIVE_PERF_BUCKET_RENDERER_BEGIN_SCENE);
	N3DS_C3DBeginFrame();
	NativePerf_EndScope(NATIVE_PERF_BUCKET_RENDERER_BEGIN_SCENE);
}

void NativeRenderer_EndScene(void)
{
	if (!s_rendererInitialised)
	{
		return;
	}

	s_endSceneCount++;
	// C3D_FrameEnd: the whole frame's queued GPU commands actually execute
	// here, so this is where any GPU-side stall or submit cost shows up.
	NativePerf_BeginScope(NATIVE_PERF_BUCKET_N3DS_FRAME_END);
	N3DS_C3DEndFrame();
	NativePerf_EndScope(NATIVE_PERF_BUCKET_N3DS_FRAME_END);
}

void NativeRenderer_UpdateSwapIntervalState(int swapInterval)
{
	(void)swapInterval;
}

void NativeRenderer_SwapWindow(void)
{
	if (!s_rendererInitialised)
	{
		return;
	}

	/* Re-capture the forced diagnostic texture next frame (see
	 * g_dbg_forceSingleTexture) so it always tracks a currently-loaded one. */
	s_forcedTexValid = 0;

	NativePerf_BeginScope(NATIVE_PERF_BUCKET_SWAP_WINDOW);

	/*
	 * Frame pacing lives in VSync() (platform/native_platform.c), which runs off
	 * the tick clock. Only push the console's backing buffer here; do not wait on
	 * a second vblank source or the 30fps gate would be paced twice.
	 */
	s_frameCounter++;

	if ((s_frameCounter & 31) == 0)
	{
		/*
		 * On-screen stats block. Getting files off the console is awkward (the
		 * SD is under the backplate and only one homebrew app runs at a time,
		 * so an FTP server cannot be up while this is), and a photo of the top
		 * screen carries everything worth knowing. Also mirrored to the log.
		 */
		const unsigned long vblanks = (unsigned long)Platform_GetVBlankCount();
		const unsigned long frames = (unsigned long)s_frameCounter;
		const unsigned long heapKiB = N3DS_HeapUsedBytes() / 1024UL;
		unsigned long avgX10 = 0;
		unsigned long nowX10 = 0;
		char line[128];

		if (vblanks > 0)
		{
			// The retail gate is two vblanks per flip, i.e. 30fps.
			avgX10 = (600UL * frames) / vblanks;
		}

		/*
		 * Report the rate over the last window as well as the average since
		 * boot. A falling average hides how bad the current rate is, because
		 * every earlier good frame keeps dragging it up.
		 */
		if (vblanks > s_lastStatVblanks)
		{
			nowX10 = (600UL * (frames - s_lastStatFrames)) / (vblanks - s_lastStatVblanks);
		}
		s_lastStatFrames = frames;
		s_lastStatVblanks = vblanks;

		/*
		 * Row 1, not row 16: the block started halfway down when the console
		 * shared the TOP screen with game output. citro3d owns the top screen
		 * now, so the whole bottom console is ours -- and starting at 16 left
		 * too few rows, so the block overflowed, scrolled, and then redrew
		 * over its own scrolled remains. Unreadable on hardware, which is
		 * where this is the only output there is.
		 */
		// \x1b[2J (clear) then home. Redrawing in place left fragments of the
		// previous, longer values behind wherever a new line was shorter,
		// which reads as garbled overlapping text -- useless in a photo, and a
		// photo of this screen is the whole readout on real hardware.
		printf("\x1b[2J\x1b[1;1H---- 3DS port stats ----------------\n");
		printf("frames %-8lu vblanks %-8lu\n", frames, vblanks);
		printf("fps    %lu.%lu now / %lu.%lu avg (target 30)   \n",
		       nowX10 / 10UL, nowX10 % 10UL, avgX10 / 10UL, avgX10 % 10UL);
		/* Break the heap down: XA now reports ring occupancy (sectors buffered
		 * / ring depth) and underrun count instead of arena KiB -- this is the
		 * same instrumentation that originally proved the old arena-growth bug
		 * (BUGREPORT_audio_arena_growth.md); after the fix it should show a
		 * small number that stays flat across track transitions instead of
		 * one that doubles over a session. */
		printf("heap   %-6lu  xaBuf %d/%d underrun %-4lu voice %-5d KiB\n", heapKiB,
		       NativeAudio_XAStreamSectorsBuffered(), NATIVE_AUDIO_XA_RING_SECTORS, s_audio.xaStream.underrunEvents,
		       s_audio.voicePcmArena.capacity / 1024);
		printf("state  %-4d vram[0] %04x            \n", (int)sdata->mainGameState, (unsigned)vram[0]);

		snprintf(line, sizeof(line),
		         "[CTR-3DS] frames=%lu vbl=%lu fps_now=%lu.%lu heapKiB=%lu xaBuf=%d/%d underrun=%lu voiceKiB=%d state=%d",
		         frames, vblanks, nowX10 / 10UL, nowX10 % 10UL, heapKiB,
		         NativeAudio_XAStreamSectorsBuffered(), NATIVE_AUDIO_XA_RING_SECTORS, s_audio.xaStream.underrunEvents,
		         s_audio.voicePcmArena.capacity / 1024, (int)sdata->mainGameState);
		N3DS_DebugLog(line);

		/*
		 * Texture decode cache diagnostics -- added to check whether the cache
		 * is thrashing (real, ongoing evictions of still-wanted textures, which
		 * is expensive every single time) versus a one-time warm-up cost
		 * (misses into never-yet-used slots, cheap after the first frame or
		 * two). fps_now above is a frames-per-emulated-vblank ratio and can
		 * look better than the real experience if per-frame cost is spiky --
		 * this says directly whether that's what's happening here.
		 */
		{
			const unsigned long missesWindow = (unsigned long)(s_decodeCacheMisses - s_lastStatCacheMisses);
			const unsigned long evictionsWindow = (unsigned long)(s_decodeCacheEvictions - s_lastStatCacheEvictions);
			const unsigned long drawsWindow = (unsigned long)(s_drawCallCount - s_lastStatDrawCallCount);
			/* Report interval is always exactly 32 frames -- the (s_frameCounter & 31) gate above. */
			const unsigned long drawsPerFrameAvgX10 = (drawsWindow * 10UL) / 32UL;
			char cacheLine[128];

			const unsigned long staleWindow = (unsigned long)(s_decodeCacheStaleMisses - s_lastStatStaleMisses);
			const unsigned long capacityWindow = (unsigned long)(s_decodeCacheCapacityMisses - s_lastStatCapacityMisses);

			s_lastStatCacheMisses = s_decodeCacheMisses;
			s_lastStatCacheEvictions = s_decodeCacheEvictions;
			s_lastStatDrawCallCount = s_drawCallCount;
			s_lastStatStaleMisses = s_decodeCacheStaleMisses;
			s_lastStatCapacityMisses = s_decodeCacheCapacityMisses;

			printf("cache  hits %-8lu miss %-6lu (+%-4lu/w) evict %-4lu (+%-4lu/w)\n", (unsigned long)s_decodeCacheHits,
			       (unsigned long)s_decodeCacheMisses, missesWindow, (unsigned long)s_decodeCacheEvictions, evictionsWindow);
			/* WHY the misses happened. cap = working set exceeds the cache;
			 * stale = the texture was resident but its VRAM moved. These have
			 * opposite fixes, so never read the miss count without this split.
			 * set = distinct cells touched in the worst frame (saturates at
			 * N3DS_FULLPAGE_RESERVED_CELLS, the whole-page tier's own budget
			 * since the two-tier split -- was N3DS_TOTAL_CELLS-1 before);
			 * dirty = generation bumps since boot. */
			printf("miss   cap %-6lu (+%-4lu/w) stale %-6lu (+%-4lu/w)\n", (unsigned long)s_decodeCacheCapacityMisses,
			       capacityWindow, (unsigned long)s_decodeCacheStaleMisses, staleWindow);
			printf("set    peak %-4lu /%-4d cells   vramDirty %-8lu\n", (unsigned long)s_frameTouchedPeak,
			       (int)N3DS_FULLPAGE_RESERVED_CELLS, (unsigned long)s_vramDirtyBumps);
			/* Nonzero = a cell was overwritten while a queued draw still
			 * referenced it, i.e. visible texture corruption. */
			printf("hazard evictInUse %-8lu\n", (unsigned long)s_evictHazards);
			printf("draws  total %-8lu +%-6lu/w  ~%lu.%lu draws/frame        \n", (unsigned long)s_drawCallCount, drawsWindow,
			       drawsPerFrameAvgX10 / 10UL, drawsPerFrameAvgX10 % 10UL);

			snprintf(cacheLine, sizeof(cacheLine),
			         "[CTR-3DS] cache hits=%lu misses=%lu missesWindow=%lu evictions=%lu evictionsWindow=%lu draws=%lu drawsWindow=%lu",
			         (unsigned long)s_decodeCacheHits, (unsigned long)s_decodeCacheMisses, missesWindow,
			         (unsigned long)s_decodeCacheEvictions, evictionsWindow, (unsigned long)s_drawCallCount, drawsWindow);
			N3DS_DebugLog(cacheLine);

			snprintf(cacheLine, sizeof(cacheLine),
			         "[CTR-3DS] miss capacity=%lu capacityWindow=%lu stale=%lu staleWindow=%lu setPeak=%lu/%d vramDirty=%lu",
			         (unsigned long)s_decodeCacheCapacityMisses, capacityWindow, (unsigned long)s_decodeCacheStaleMisses,
			         staleWindow, (unsigned long)s_frameTouchedPeak, (int)N3DS_FULLPAGE_RESERVED_CELLS,
			         (unsigned long)s_vramDirtyBumps);
			N3DS_DebugLog(cacheLine);

			snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] hazard evictInUse=%lu", (unsigned long)s_evictHazards);
			N3DS_DebugLog(cacheLine);

			/* Draw-call structure: window deltas (32-frame window, so /32 for
			 * per-frame; x10 for one decimal without floats). See the counter
			 * block's header for what each mechanism means. */
			{
				const unsigned long switchWindow = (unsigned long)(s_atlasSwitchFlushes - s_lastStatAtlasSwitchFlushes);
				const unsigned long distinctWindow = (unsigned long)(s_distinctAtlasTotal - s_lastStatDistinctAtlasTotal);

				s_lastStatAtlasSwitchFlushes = s_atlasSwitchFlushes;
				s_lastStatDistinctAtlasTotal = s_distinctAtlasTotal;

				snprintf(cacheLine, sizeof(cacheLine),
				         "[CTR-3DS] batch atlasSwitchX10=%lu/frame distinctAtlasX10=%lu/frame distinctPeak=%lu dtriCallsTotal=%lu",
				         (switchWindow * 10UL) / 32UL, (distinctWindow * 10UL) / 32UL,
				         (unsigned long)s_frameDistinctAtlasPeak, (unsigned long)s_drawTrianglesCalls);
				N3DS_DebugLog(cacheLine);
			}

			/*
			 * Sub-page tier diagnostics -- mirrors the full-page block just
			 * above, field-for-field (docs/SUBPAGE_DECODE_CACHE_DESIGN.md
			 * "Instrumentation"). Full per-tier breakdown goes to the log
			 * only; the console gets one compact routing-summary line below
			 * instead of a full duplicate block -- this console has already
			 * hit its practical width/line limit once before (see the
			 * audio-stats line's own history).
			 */
			{
				const unsigned long subMissesWindow = (unsigned long)(s_subPageCacheMisses - s_subPageLastStatCacheMisses);
				const unsigned long subEvictionsWindow =
				    (unsigned long)(s_subPageCacheEvictions - s_subPageLastStatCacheEvictions);
				const unsigned long subStaleWindow =
				    (unsigned long)(s_subPageCacheStaleMisses - s_subPageLastStatStaleMisses);
				const unsigned long subCapacityWindow =
				    (unsigned long)(s_subPageCacheCapacityMisses - s_subPageLastStatCapacityMisses);

				s_subPageLastStatCacheMisses = s_subPageCacheMisses;
				s_subPageLastStatCacheEvictions = s_subPageCacheEvictions;
				s_subPageLastStatStaleMisses = s_subPageCacheStaleMisses;
				s_subPageLastStatCapacityMisses = s_subPageCacheCapacityMisses;

				snprintf(cacheLine, sizeof(cacheLine),
				         "[CTR-3DS] subpage cache hits=%lu misses=%lu missesWindow=%lu evictions=%lu evictionsWindow=%lu",
				         (unsigned long)s_subPageCacheHits, (unsigned long)s_subPageCacheMisses, subMissesWindow,
				         (unsigned long)s_subPageCacheEvictions, subEvictionsWindow);
				N3DS_DebugLog(cacheLine);

				snprintf(cacheLine, sizeof(cacheLine),
				         "[CTR-3DS] subpage miss capacity=%lu capacityWindow=%lu stale=%lu staleWindow=%lu setPeak=%lu/%d",
				         (unsigned long)s_subPageCacheCapacityMisses, subCapacityWindow,
				         (unsigned long)s_subPageCacheStaleMisses, subStaleWindow, (unsigned long)s_subPageFrameTouchedPeak,
				         (int)N3DS_SUBPAGE_TOTAL_CELLS);
				N3DS_DebugLog(cacheLine);

				snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] subpage hazard evictInUse=%lu",
				         (unsigned long)s_subPageEvictHazards);
				N3DS_DebugLog(cacheLine);

				/* s_trianglesRoutedSubPage/FullPage: the single most important
				 * new number (design doc) -- whether the straddling/large-
				 * surface fallback to the full-page tier dominates real
				 * content or stays the minority case this design assumes. */
				snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] routing subPage=%lu fullPage=%lu",
				         (unsigned long)s_trianglesRoutedSubPage, (unsigned long)s_trianglesRoutedFullPage);
				N3DS_DebugLog(cacheLine);

				printf("route  sub %-8lu full %-8lu  subMiss %-5lu subEvict %-5lu\n",
				       (unsigned long)s_trianglesRoutedSubPage, (unsigned long)s_trianglesRoutedFullPage,
				       (unsigned long)s_subPageCacheMisses, (unsigned long)s_subPageCacheEvictions);
			}

			/*
			 * Vertex budget occupancy. dropped != 0 means whole draws were
			 * discarded for want of buffer space, i.e. geometry is missing
			 * from the screen -- the failure that previously had no symptom
			 * other than absent models. peak approaching the ceiling is the
			 * early warning for the same thing.
			 */
			printf("verts  peak %-7d /%-7d  dropped %-6lu\n", N3DS_C3DFrameVertexHighWater(),
			       (int)N3DS_MAX_FRAME_VERTICES, (unsigned long)s_droppedRuns);

			snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] vertsPeak=%d/%d droppedRuns=%lu",
			         N3DS_C3DFrameVertexHighWater(), (int)N3DS_MAX_FRAME_VERTICES, (unsigned long)s_droppedRuns);
			N3DS_DebugLog(cacheLine);

			printf("scene  begin %-8lu end %-8lu (vs %lu swaps)\n", (unsigned long)s_beginSceneCount,
			       (unsigned long)s_endSceneCount, frames);

			snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] beginScene=%lu endScene=%lu swaps=%lu",
			         (unsigned long)s_beginSceneCount, (unsigned long)s_endSceneCount, frames);
			N3DS_DebugLog(cacheLine);

			/*
			 * Audio mixer workload shape. The profiler showed audio_vblank_ms
			 * taking 48-81% of the frame, and NativeAudio_MixFrame runs both
			 * the 24-voice loop and the reverb chain once per sample frame
			 * (735 of them per vblank) -- so voicesX10 and reverbSamples say
			 * which of those two is actually carrying the cost.
			 */
			{
				const unsigned long shapeVblanks = s_audioShapeVblanks;
				const unsigned long voicesX10 = (shapeVblanks > 0) ? ((s_audioShapeVoiceAccum * 10UL) / shapeVblanks) : 0UL;

				s_audioShapeVoiceAccum = 0;
				s_audioShapeVblanks = 0;

				printf("audio  voices ~%lu.%lu  reverbSmp %-6d xa %d\n", voicesX10 / 10UL, voicesX10 % 10UL,
				       s_audioShapeReverbSamples, s_audioShapeXaActive);

				snprintf(cacheLine, sizeof(cacheLine), "[CTR-3DS] audio voicesX10=%lu vblanks=%lu reverbSamples=%d xaActive=%d",
				         voicesX10, shapeVblanks, s_audioShapeReverbSamples, s_audioShapeXaActive);
				N3DS_DebugLog(cacheLine);
			}

			/*
			 * Profiler attribution on screen, not only in sdmc:/ctr3ds.log. On
			 * real hardware the SD card is behind the backplate and the top
			 * screen may still be blank, so this console is often the only
			 * output that can be read at all -- a photo of it should answer
			 * "where did the frame go", not just "how fast was it".
			 */
			/*
			 * Why-is-nothing-drawn diagnostics. A hardware run showed draws=0,
			 * cache 0/0, verts=0 while scene begin/end still ran every frame --
			 * so the citro3d bracket opens and closes around nothing, and the
			 * blank top screen is upstream of the renderer entirely.
			 *
			 * MainMain's case 3 can `break` out before Platform_BeginFrame when
			 * Loading.stage != LOAD_IDLE and the VLC table has not arrived,
			 * which would skip the whole render path AND the perf bracket --
			 * matching the observed perf=0.0ms. stage/vlc below say whether
			 * that is what is happening. LOAD_IDLE is -1; LOAD_VLC is -6.
			 *
			 * uvb/dtri separate "the OT walk never ran" from "it ran and
			 * DrawTriangles bailed": uvb counts UpdateVertexBuffer (reached
			 * only via DrawAllSplits), dtri counts DrawTriangles entries.
			 */
			printf("load   stage %-4d vlc %-4d           \n", (int)sdata->Loading.stage, (int)sdata->bool_IsLoaded_VlcTable);
			printf("call   uvb %-8lu dtri %-8lu    \n", (unsigned long)s_updateVertexBufferCalls, (unsigned long)s_drawTrianglesCalls);

			printf("perf   frame %lu.%lums  work %lu.%lums        \n", (unsigned long)s_perfConsoleTotalUs / 1000UL,
			       ((unsigned long)s_perfConsoleTotalUs % 1000UL) / 100UL, (unsigned long)s_perfConsoleWorkUs / 1000UL,
			       ((unsigned long)s_perfConsoleWorkUs % 1000UL) / 100UL);
			for (int i = 0; i < NATIVE_PERF_3DS_CONSOLE_BUCKETS; i++)
			{
				printf(" %-24s %lu.%lums   \n", (s_perfConsoleName[i] != NULL) ? s_perfConsoleName[i] : "-",
				       (unsigned long)s_perfConsoleUs[i] / 1000UL, ((unsigned long)s_perfConsoleUs[i] % 1000UL) / 100UL);
			}
		}
	}

	NativePerf_BeginScope(NATIVE_PERF_BUCKET_N3DS_GFX_PRESENT);
	N3DS_GfxPresent();
	NativePerf_EndScope(NATIVE_PERF_BUCKET_N3DS_GFX_PRESENT);

	NativePerf_EndScope(NATIVE_PERF_BUCKET_SWAP_WINDOW);
}

void NativeRenderer_StoreFrameBuffer(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
}

/* Scratch for one tiled VRAM-display chunk; reused every call, never
 * persisted across frames. Mirrors s_decodeScratch's role for
 * N3DS_DecodeAndTilePage above. */
global_variable u16 s_vramDisplayScratch[N3DS_VRAM_DISPLAY_TEX_DIM * N3DS_VRAM_DISPLAY_TEX_DIM];

/*
 * Tiles a [x,y,w,h) rect of the emulated VRAM into a 256x256 RGBA5551 buffer
 * for NativeRenderer_PresentVRAMRect below. Reuses N3DS_DecodeAndTilePage's
 * Morton/Z-order tile addressing and BGR555->RGBA5551 channel packing
 * verbatim -- same bug class a byte-order fix already landed in for that
 * function, not worth re-deriving here. Alpha is forced always-opaque
 * (bit set) rather than PSX's conditional punch-through test: this is a
 * straight present of a VRAM rect, not a masked game texture, matching PC's
 * NativeRenderer_BuildDisplayChunkRGBA's unconditional 0xff.
 */
internal void N3DS_TileVRAMDisplayChunk(int srcX, int srcY, int w, int h, u16 *outTiled256)
{
	int v;

	for (v = 0; v < N3DS_VRAM_DISPLAY_TEX_DIM; v++)
	{
		int u;

		for (u = 0; u < N3DS_VRAM_DISPLAY_TEX_DIM; u++)
		{
			u16 color16 = 0;
			int morton, tileIndex, dstIndex;

			if ((u < w) && (v < h))
			{
				color16 = vram[(srcY + v) * VRAM_WIDTH + (srcX + u)];
			}

			morton = (u & 1) | ((v & 1) << 1) | ((u & 2) << 1) | ((v & 2) << 2) | ((u & 4) << 2) | ((v & 4) << 3);
			tileIndex = ((v >> 3) * (N3DS_VRAM_DISPLAY_TEX_DIM / 8) + (u >> 3)) * 64;
			dstIndex = tileIndex + morton;

			outTiled256[dstIndex] = (u16)(((color16 & 0x001Fu) << 11) |      /* R */
			                              (((color16 >> 5) & 0x1Fu) << 6) |   /* G */
			                              (((color16 >> 10) & 0x1Fu) << 1) |  /* B */
			                              1u);                                /* always opaque */
		}
	}
}

void NativeRenderer_PresentVRAMRect(int displayX, int displayY, int displayW, int displayH)
{
	const int maxChunk = N3DS_VRAM_DISPLAY_TEX_DIM;
	int y;
	int chunkSlot = 0;
	/* DIAGNOSTIC (2026-07-21, session 14): bounded entry/exit trace pairing
	 * with the FrameBegin/FrameEnd traces in n3ds_backend.c -- together they
	 * bracket every stage of the boot-splash present that real hardware
	 * freezes inside. */
	static int s_presentTraceBudget = 16;
	const int trace = (s_presentTraceBudget > 0);

	if (trace)
	{
		s_presentTraceBudget--;
		N3DS_DebugLog("[CTR-3DS] PresentVRAMRect: enter");
	}

	if ((displayW <= 0) || (displayH <= 0))
	{
		return;
	}

	N3DS_C3DSetColorDoubling(0);

	for (y = 0; y < displayH; y += maxChunk)
	{
		const int chunkH = ((displayH - y) > maxChunk) ? maxChunk : (displayH - y);
		int x;

		for (x = 0; x < displayW; x += maxChunk)
		{
			const int chunkW = ((displayW - x) > maxChunk) ? maxChunk : (displayW - x);
			/*
			 * Pre-scaled by (1024/maxChunk), not left as a plain [0,1]
			 * fraction like the old float u1/v1 were: ctr.v.pica now applies
			 * a single, unconditional *1/1024 to every vertex's texcoord
			 * (design doc "Lever 2"/docs/VERTEX_FORMAT_DESIGN.md) because one
			 * vertex shader is shared by every draw call in this renderer,
			 * not just DrawTriangles' atlas-cell path. This quad samples a
			 * dedicated, non-atlas s_vramDisplayTex directly in [0,1]
			 * (N3DS_C3DBindVramDisplayTex -- no atlas-cell offset involved),
			 * so what has to reach the shader is the corner's normalized UV
			 * pre-multiplied back up by the shader's texcoord denominator --
			 * 2048 since the half-texel numerator change (the atlas path
			 * carries 2*texel-1; this edge-normalized [0,1] quad just doubles
			 * its numerators to keep the same normalized value, no bias).
			 * Exact, not approximated: maxChunk (256) divides 2048 evenly.
			 */
			const s16 u1 = (s16)(chunkW * (2048 / maxChunk));
			const s16 v1 = (s16)(chunkH * (2048 / maxChunk));
			/* Which pool texture this chunk uploads to and draws from. Must
			 * be distinct per chunk within this call -- see the header
			 * comment on N3DS_C3DUploadVramDisplayChunk for why reusing one
			 * texture across chunks in the same still-open frame silently
			 * makes every chunk's draw sample whichever chunk uploaded last.
			 * Wraps defensively rather than assuming displayW/H never
			 * exceeds the pool's 2x2-chunk (512x512) coverage. */
			const int slot = chunkSlot % N3DS_VRAM_DISPLAY_TEX_COUNT;
			N3DS_Vertex verts[6];
			int baseVertex;
			int i;

			chunkSlot++;

			N3DS_TileVRAMDisplayChunk(displayX + x, displayY + y, chunkW, chunkH, s_vramDisplayScratch);
			N3DS_C3DUploadVramDisplayChunk(slot, s_vramDisplayScratch);

			/* v0.0 experiment (2026-07-20): source top-of-rect landed on
			 * screen bottom on real output despite the quad's own screen-
			 * space Y matching the same proven per-frame projection every
			 * other draw uses, and despite the tile-address math being a
			 * verbatim copy of N3DS_DecodeAndTilePage's proven-correct
			 * formula. Swapping which screen edge samples V=0 vs V=v1 is the
			 * most localized place left to try a fix, since everything else
			 * here mirrors already-proven code; NOT independently confirmed
			 * against a citro3d whole-texture-upload precedent (there isn't
			 * one elsewhere in this codebase to check against). If this
			 * looks wrong on the next capture, revert this V swap and look
			 * upstream of this function instead. */
			verts[0].x = (s16)x;
			verts[0].y = (s16)y;
			verts[0].u = 0;
			verts[0].v = v1;

			verts[1].x = (s16)x;
			verts[1].y = (s16)(y + chunkH);
			verts[1].u = 0;
			verts[1].v = 0;

			verts[2].x = (s16)(x + chunkW);
			verts[2].y = (s16)(y + chunkH);
			verts[2].u = u1;
			verts[2].v = 0;

			verts[3] = verts[0];
			verts[4] = verts[2];

			verts[5].x = (s16)(x + chunkW);
			verts[5].y = (s16)y;
			verts[5].u = u1;
			verts[5].v = v1;

			for (i = 0; i < 6; i++)
			{
				/* Full-scale white -- ctr.v.pica's colorScale turns raw 255
				 * back into 1.0, same as every other vertex's r/g/b now (see
				 * the main conversion loop above). Alpha is no longer a
				 * struct field: the shader supplies a constant 1.0 for it
				 * instead (design doc finding 3). */
				verts[i].r = 255;
				verts[i].g = 255;
				verts[i].b = 255;
			}

			/* ABLATION A (2026-07-21, session 14, hardware GPU-wedge hunt):
			 * submit the splash frames with clear+transfer but ZERO draws.
			 * Real hardware never completes the first submitted command list
			 * (FrameBegin #2 waits forever); this splits {clear/transfer}
			 * from {draw state}. If boot proceeds past the splash with this
			 * on, the wedge is in the draw path -- and the first real menu
			 * frame (DrawTriangles, not skipped here) then tests the shader
			 * itself. REVERT after the test. */
			if (g_dbg_skipSplashDraws)
			{
				continue;
			}

			N3DS_C3DBindVramDisplayTex(slot);
			baseVertex = N3DS_C3DAppendVertices(verts, 6);
			if (baseVertex >= 0)
			{
				N3DS_C3DDrawTriangles(baseVertex, 2);
			}
		}
	}

	if (trace)
	{
		N3DS_DebugLog(g_dbg_skipSplashDraws ? "[CTR-3DS] PresentVRAMRect: done (draws SKIPPED, ablation A)"
		                                    : "[CTR-3DS] PresentVRAMRect: done (draws queued)");
	}
}

void NativeRenderer_PresentVRAMDisplay(void)
{
	NativeRenderer_PresentVRAMRect(activeDispEnv.disp.x, activeDispEnv.disp.y, activeDispEnv.disp.w, activeDispEnv.disp.h);
}

void NativeRenderer_SaveVRAM(const char *outputFileName, int x, int y, int width, int height, int readFromFramebuffer)
{
	(void)outputFileName; (void)x; (void)y; (void)width; (void)height; (void)readFromFramebuffer;
}

void NativeRenderer_Clear(int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
	(void)x; (void)y; (void)w; (void)h; (void)r; (void)g; (void)b;
}

void NativeRenderer_ClearVRAM(int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
	const u16 colour = NativeRenderer3DS_PackBGR555(r, g, b);
	int row;

	if ((w <= 0) || (h <= 0) || (x < 0) || (y < 0))
	{
		return;
	}

	if (((x + w) > VRAM_WIDTH) || ((y + h) > VRAM_HEIGHT))
	{
		return;
	}

	N3DS_DirtyVRAMRect(x, y, w, h);

	for (row = 0; row < h; row++)
	{
		u16 *dst = vram + x + ((y + row) * VRAM_WIDTH);
		int col;

		for (col = 0; col < w; col++)
		{
			dst[col] = colour;
		}
	}
}

void NativeRenderer_CopyVRAM(u16 *src, int x, int y, int w, int h, int dstX, int dstY)
{
	int stride = w;
	u16 *dst;
	int row;

	if ((w <= 0) || (h <= 0))
	{
		return;
	}

	if (src == NULL)
	{
		/* MoveImage: VRAM-to-VRAM. No host framebuffer exists to reconcile yet. */
		src = vram;
		stride = VRAM_WIDTH;
	}

	src += x + (y * stride);
	dst = vram + dstX + (dstY * VRAM_WIDTH);

	N3DS_DirtyVRAMRect(dstX, dstY, w, h);

	for (row = 0; row < h; row++)
	{
		memcpy(dst, src, (size_t)w * sizeof(u16));
		dst += VRAM_WIDTH;
		src += stride;
	}
}

void NativeRenderer_ReadVRAM(u16 *dst, int x, int y, int dstW, int dstH)
{
	u16 *src = vram + x + (VRAM_WIDTH * y);
	int row;

	for (row = 0; row < dstH; row++)
	{
		memcpy(dst, src, (size_t)dstW * sizeof(u16));
		dst += dstW;
		src += VRAM_WIDTH;
	}
}

void NativeRenderer_UpdateVRAM(void)
{
}

void NativeRenderer_ReadFramebufferDataToVRAM(void)
{
}

void NativeRenderer_DiscardFramebufferReadback(void)
{
}

int NativeRenderer_GetVRAMStateSize(void)
{
	return (int)sizeof(vram);
}

int NativeRenderer_CaptureVRAMState(void *dst, int dstSize)
{
	if ((dst == NULL) || (dstSize < (int)sizeof(vram)))
	{
		return 0;
	}

	memcpy(dst, vram, sizeof(vram));
	return 1;
}

int NativeRenderer_RestoreVRAMState(const void *src, int srcSize)
{
	if ((src == NULL) || (srcSize < (int)sizeof(vram)))
	{
		return 0;
	}

	memcpy(vram, src, sizeof(vram));
	return 1;
}

TextureID NativeRenderer_GetVRAMTexture(void)
{
	return N3DS_TEXTURE_ID_VRAM;
}

TextureID NativeRenderer_GetWhiteTexture(void)
{
	return N3DS_TEXTURE_ID_WHITE;
}

void NativeRenderer_SetBlendMode(BlendMode blendMode)
{
	(void)blendMode;
}

void NativeRenderer_SetStencilMode(int drawPrim)
{
	(void)drawPrim;
}

void NativeRenderer_SetOffscreenState(const RECT16 *offscreenRect, const DISPENV *displayEnv, int enable)
{
	(void)offscreenRect; (void)displayEnv; (void)enable;
}

void NativeRenderer_SetupClipMode(const RECT16 *clipRect, const DISPENV *displayEnv, int enable)
{
	(void)clipRect; (void)displayEnv; (void)enable;
}

void NativeRenderer_SetTexture(TextureID texture, TexFormat texFormat)
{
#ifdef CTR_INTERNAL
	if (g_dbg_texturelessMode)
	{
		texture = N3DS_TEXTURE_ID_WHITE;
	}
#endif
	s_currentTextureId = texture;
	s_currentTexFormat = texFormat;
}

void NativeRenderer_SetOverrideTextureSize(int width, int height)
{
	(void)width; (void)height;
}

void NativeRenderer_SetPSXTextureSemiTransPass(int pass)
{
	(void)pass;
}

void NativeRenderer_SetPSXTextureOutputSTP(int enabled)
{
	(void)enabled;
}

void NativeRenderer_SetPSXDrawMaskSet(int maskSet)
{
	(void)maskSet;
}

void NativeRenderer_UpdateVertexBuffer(const GrVertex *vertices, int count)
{
	/*
	 * Mirrors PC's own pattern (DrawAllSplits -> UpdateVertexBuffer once,
	 * then DrawTriangles many times at different offsets into it): just
	 * remember where the frame's vertices live. s_gpu.vertexBuffer
	 * (native_gpu.c) is a persistent static array, not stack memory, so
	 * holding this pointer across the DrawSplit loop that follows is safe.
	 * The real GrVertex -> N3DS_Vertex conversion happens lazily in
	 * DrawTriangles, which is where texFormat (from SetTexture) and each
	 * triangle's own page/clut are actually known.
	 */
	s_updateVertexBufferCalls++;
	s_pendingVertices = vertices;
	s_pendingVertexCount = count;
}

void NativeRenderer_DrawTriangles(int startVertex, int triangles)
{
	int i;
	int currentAtlas = -1;
	int vertsInRun = 0;
	const int untextured = (s_currentTextureId == N3DS_TEXTURE_ID_WHITE);

	s_drawTrianglesCalls++;

	if (!s_rendererInitialised || (s_pendingVertices == NULL) || (triangles <= 0))
	{
		return;
	}

	if ((s_currentTextureId != N3DS_TEXTURE_ID_VRAM) && (s_currentTextureId != N3DS_TEXTURE_ID_WHITE))
	{
		/* TF_32_BIT_RGBA override texture -- provably unreachable from any
		 * real game/ code path (zero DR_PSYX_TEX call sites; the boot
		 * splash uses NativeRenderer_PresentVRAMDisplay/PresentVRAMRect
		 * instead, a separate presentation path entirely). Skip rather
		 * than guess at an untested path -- see the file header. */
		return;
	}

	if ((startVertex < 0) || ((startVertex + (triangles * 3)) > s_pendingVertexCount))
	{
		return;
	}

	// Opened only after every early return above, so the scope is always
	// balanced without an end on each bail-out path.
	NativePerf_BeginScope(NATIVE_PERF_BUCKET_RENDERER_DRAW_TRIANGLES);

	/*
	 * PC's AddSplit coalesces draw calls by blend/format/textureId/mask/
	 * STP/clip/dfe but never by page or CLUT (native_gpu.c:895-901, a
	 * literal FIXME) -- one PC draw call can legitimately span several
	 * texture pages/palettes, relying on true per-vertex resolution in its
	 * shader. citro3d can only bind one texture per draw call, so this walks
	 * the split's triangles (each one's 3 vertices always share a single
	 * page/clut -- see MakeTexcoordTriangle/Quad/Rect), resolves each to its
	 * atlas cell, and flushes a new citro3d draw only when the ATLAS changes
	 * -- not the cell -- since multiple cells of the same atlas share one
	 * bind (see n3ds_backend.h's header comment on N3DS_C3DUploadDecodedCell
	 * for why this exists: measured ~150-290 draws/frame before it did).
	 * Vertex conversion happens inline per triangle (not deferred to a
	 * separate per-run pass like the single-texture design this replaced)
	 * because a run can now span multiple distinct cells, each needing its
	 * own UV offset -- there is no single (page,clut) left to convert against
	 * once triangles from different cells share a run. Triangles are never
	 * reordered, only grouped by contiguous atlas -- draw order is preserved
	 * exactly, which matters because this renderer has no depth test enabled
	 * (matching PC) and relies on draw order alone for correct layering.
	 */
	/* bright is 2 for every textured primitive and 1 for every untextured
	 * one, without exception (native_gpu.c), so the split's texture sentinel
	 * determines it for the whole call -- no need to inspect each vertex. */
	N3DS_C3DSetColorDoubling(untextured ? 0 : 1);

	for (i = 0; i < triangles; i++)
	{
		const GrVertex *triVerts = &s_pendingVertices[startVertex + (i * 3)];
		int atlasIdx, cellX = 0, cellY = 0, pixelBaseU = 0, pixelBaseV = 0, originU = 0, originV = 0, v;

		if (untextured)
		{
			atlasIdx = N3DS_WHITE_ATLAS;
			cellX = N3DS_WHITE_CELL_X;
			cellY = N3DS_WHITE_CELL_Y;
		}
		else
		{
			/* Routes this triangle independently to the sub-page tier
			 * (common case) or the full-page tier (fallback for a UV
			 * footprint that doesn't fit one 64x64 tile) -- must run per
			 * triangle, not once per (page,clut): two triangles sharing a
			 * texture can still have different UV footprints (e.g. a small
			 * decal vs. a large floor triangle both sampling the same page).
			 * See N3DS_ResolveTriangleTexture / docs/SUBPAGE_DECODE_CACHE_DESIGN.md. */
			if (g_dbg_forceSingleTexture)
			{
				/* Diagnostic: pin every textured triangle to one fixed texture
				 * (first textured triangle of the frame), full-page path, real
				 * UVs. See g_dbg_forceSingleTexture's declaration. */
				int fCellX = 0, fCellY = 0;

				if (!s_forcedTexValid)
				{
					s_forcedPage = triVerts[0].page;
					s_forcedClut = triVerts[0].clut;
					s_forcedFormat = s_currentTexFormat;
					s_forcedTexValid = 1;
				}

				N3DS_CellIndexToAtlasCoord(N3DS_FindOrDecodeTexture(s_forcedPage, s_forcedClut, s_forcedFormat), &atlasIdx,
				                            &fCellX, &fCellY);
				pixelBaseU = fCellX * N3DS_DECODE_TEXTURE_DIM;
				pixelBaseV = fCellY * N3DS_DECODE_TEXTURE_DIM;
				originU = 0;
				originV = 0;
			}
			else
			{
				N3DS_ResolveTriangleTexture(&triVerts[0], &triVerts[1], &triVerts[2], triVerts[0].page, triVerts[0].clut,
				                             s_currentTexFormat, &atlasIdx, &pixelBaseU, &pixelBaseV, &originU, &originV);
			}
		}

		if ((atlasIdx != currentAtlas) || ((vertsInRun + 3) > N3DS_MAX_RUN_VERTICES))
		{
			/* Attribute the flush: only a MID-RUN atlas change is the
			 * potentially-reducible kind (vertsInRun > 0 excludes the first
			 * triangle's currentAtlas==-1 bootstrap, which flushes nothing). */
			if ((atlasIdx != currentAtlas) && (vertsInRun > 0))
			{
				s_atlasSwitchFlushes++;
			}
			N3DS_FlushRun(currentAtlas, vertsInRun);
			currentAtlas = atlasIdx;
			vertsInRun = 0;
		}

		for (v = 0; v < 3; v++)
		{
			const GrVertex *src = &triVerts[v];
			N3DS_Vertex *dst = &s_convertScratch[vertsInRun + v];
			/* Vertex colour is the raw PSX colour only. The "bright" x2
			 * multiplier (MakeTexcoordQuad/Triangle/Rect set bright=2 for
			 * every textured primitive, MakeTexcoordZero* set 1 for every
			 * untextured one -- native_gpu.c) is applied by the TEV output
			 * scale instead, AFTER the texture modulate, matching PC. Baking
			 * it here clamped the colour to 1.0 before the multiply and lost
			 * all the headroom above it, which is what made textured surfaces
			 * render darker than PC. See N3DS_C3DSetColorDoubling. */

			dst->x = (s16)src->x;
			dst->y = (s16)src->y;

			/* Offset into the resolved region of the atlas, not the whole
			 * atlas. Untextured uses the fixed white cell's centre
			 * (cellX,cellY); textured uses pixelBaseU/V -- a whole 256x256
			 * full-page cell or a 64x64 sub-page sub-cell, whichever
			 * N3DS_ResolveTriangleTexture resolved -- plus the triangle's own
			 * (u,v) re-based against originU/V (0 on the full-page path, so
			 * this reduces to exactly the old cellX*256+src->u there). Stored
			 * as the exact integer atlas-texel numerator, range [0,1023] --
			 * comfortably inside s16 -- with the old /N3DS_ATLAS_DIM divide
			 * moved into the shader (ctr.v.pica's texcoordScale, design doc
			 * finding 2, Option A) instead of done here. Do NOT divide here
			 * too -- that would double-scale every UV. */
			/*
			 * HALF-texel numerators: 2*texel - 1, scaled by 1/2048 in the
			 * shader, so the GPU samples at (texel - 0.5) -- reproducing the
			 * PC parity target's half-texel-center correction exactly
			 * (native_renderer.c: every textured primitive carries
			 * tcx=tcy=-1, applied as v_texcoord += a_extra.xy*0.5, then the
			 * PSX samplers floor the interpolated result). Session 15: the
			 * capture-verified atlas was byte-perfect while the screen showed
			 * pervasive per-pixel noise -- because without this bias every
			 * fragment whose interpolated UV fraction fell below .5 floored
			 * to the neighbouring texel (~half of all textured pixels; on
			 * 4bpp palette art a neighbour texel is an arbitrary colour), and
			 * far-edge UVs (u=64/256 at a tile boundary) floored into the
			 * NEIGHBOURING atlas cell -- a different texture entirely.
			 */
			if (untextured)
			{
				dst->u = (s16)(((cellX * N3DS_DECODE_TEXTURE_DIM + (N3DS_DECODE_TEXTURE_DIM / 2)) << 1) - 1);
				dst->v = (s16)(((cellY * N3DS_DECODE_TEXTURE_DIM + (N3DS_DECODE_TEXTURE_DIM / 2)) << 1) - 1);
			}
			else
			{
				dst->u = (s16)(((pixelBaseU + ((int)src->u - originU)) << 1) - 1);
				dst->v = (s16)(((pixelBaseV + ((int)src->v - originV)) << 1) - 1);
			}

			/* Raw u8 copy, no scale -- the *1/255 reciprocal-multiply that
			 * used to live here (and the hot-path reasoning for why it was a
			 * multiply, not a divide) moved into the shader's colorScale
			 * constant instead (design doc finding 3). Alpha is no longer a
			 * struct field: color is fetched at count=3 (RGB only) now, and
			 * the shader writes a constant 1.0 for alpha -- dst->a was
			 * already a hardcoded 1.0f here, never read from src->a, so
			 * dropping the field loses no real value. */
			dst->r = src->r;
			dst->g = src->g;
			dst->b = src->b;
		}

		vertsInRun += 3;
	}

	N3DS_FlushRun(currentAtlas, vertsInRun);

	NativePerf_EndScope(NATIVE_PERF_BUCKET_RENDERER_DRAW_TRIANGLES);
}

void NativeRenderer_PushDebugLabel(const char *label)
{
	(void)label;
}

void NativeRenderer_PopDebugLabel(void)
{
}

#ifdef CTR_INTERNAL
/*
 * Little-endian u32 field writer for the cachemeta dump below. ARM11 and the
 * x86 host are both little-endian, but serializing field-by-field through an
 * explicit byte view keeps the on-disk format independent of struct padding
 * and enum width -- TexFormat is 4 bytes in this TU and 1 byte in the backend
 * TU (-fshort-enums), so a raw struct fwrite is exactly the ABI trap the
 * unity-build notes warn about. Never let it near a file format.
 */
internal void N3DS_MetaWriteU32(FILE *f, u32 v)
{
	u8 b[4];

	b[0] = (u8)(v & 0xFFu);
	b[1] = (u8)((v >> 8) & 0xFFu);
	b[2] = (u8)((v >> 16) & 0xFFu);
	b[3] = (u8)((v >> 24) & 0xFFu);
	fwrite(b, 1, 4, f);
}

/*
 * Dumps both decode-cache tiers' metadata + the VRAM dirty-cell generation
 * grid to sdmc:/cachemeta.bin, for the offline cell-by-cell verification loop
 * (tools/texdump/cache_verify.py). Captured alongside the VRAM + atlas dumps
 * so all three describe the same instant: every `used` entry's atlas bytes
 * were uploaded synchronously before the entry was stamped, so entry+atlas
 * are consistent per-cell at any capture point.
 *
 * Format (all fields little-endian u32; page/clut widened via u16 so tpage
 * attribute bits above the page index survive unsigned):
 *   magic 'CTCM', version, frameCounter,
 *   FULLPAGE_RESERVED_CELLS, SUBPAGE_TOTAL_CELLS, SUBPAGE_PHYSICAL_BASE,
 *   ATLAS_COUNT, ATLAS_DIM, DECODE_TEXTURE_DIM, SUBPAGE_DIM,
 *   VRAM_CELL_DIM, VRAM_CELL_COLS, VRAM_CELL_ROWS, vramDirtyBumps,
 *   vramCellGen[COLS*ROWS],
 *   fullpage entries [0 .. FULLPAGE_RESERVED_CELLS]:
 *       used, page, clut, format, pageGen, clutGenA, clutGenB, lastUsedFrame
 *   subpage entries [0 .. SUBPAGE_TOTAL_CELLS):
 *       used, page, clut, format, tileX, tileY, pageGen, clutGenA, clutGenB,
 *       lastUsedFrame
 */
void NativeRenderer3DS_DebugDumpCacheMeta(void)
{
	FILE *f;
	char path[64];
	char msg[96];
	int i;

	/* Same frame stamp as the vramdump/atlas files written alongside this
	 * (n3ds_backend.c N3DS_DebugCaptureAll), so one capture instant forms a
	 * self-identifying file set and captures never overwrite each other. */
	snprintf(path, sizeof(path), "sdmc:/cachemeta_%u.bin", (unsigned)s_frameCounter);
	f = fopen(path, "wb");
	if (f == NULL)
	{
		snprintf(msg, sizeof(msg), "[CTR-3DS] cachemeta dump FAILED: fopen %s", path);
		N3DS_DebugLog(msg);
		return;
	}

	N3DS_MetaWriteU32(f, 0x4D435443u); /* 'CTCM' little-endian */
	N3DS_MetaWriteU32(f, 1u);
	N3DS_MetaWriteU32(f, s_frameCounter);
	N3DS_MetaWriteU32(f, (u32)N3DS_FULLPAGE_RESERVED_CELLS);
	N3DS_MetaWriteU32(f, (u32)N3DS_SUBPAGE_TOTAL_CELLS);
	N3DS_MetaWriteU32(f, (u32)N3DS_SUBPAGE_PHYSICAL_BASE);
	N3DS_MetaWriteU32(f, (u32)N3DS_ATLAS_COUNT);
	N3DS_MetaWriteU32(f, (u32)N3DS_ATLAS_DIM);
	N3DS_MetaWriteU32(f, (u32)N3DS_DECODE_TEXTURE_DIM);
	N3DS_MetaWriteU32(f, (u32)N3DS_SUBPAGE_DIM);
	N3DS_MetaWriteU32(f, (u32)N3DS_VRAM_CELL_DIM);
	N3DS_MetaWriteU32(f, (u32)N3DS_VRAM_CELL_COLS);
	N3DS_MetaWriteU32(f, (u32)N3DS_VRAM_CELL_ROWS);
	N3DS_MetaWriteU32(f, s_vramDirtyBumps);

	for (i = 0; i < (N3DS_VRAM_CELL_ROWS * N3DS_VRAM_CELL_COLS); i++)
	{
		N3DS_MetaWriteU32(f, s_vramCellGen[i]);
	}

	for (i = 0; i < (1 + N3DS_FULLPAGE_RESERVED_CELLS); i++)
	{
		const N3DS_DecodeCacheEntry *e = &s_decodeCache[i];

		N3DS_MetaWriteU32(f, (u32)e->used);
		N3DS_MetaWriteU32(f, (u32)(u16)e->page);
		N3DS_MetaWriteU32(f, (u32)(u16)e->clut);
		N3DS_MetaWriteU32(f, (u32)e->format);
		N3DS_MetaWriteU32(f, e->pageGen);
		N3DS_MetaWriteU32(f, e->clutGenA);
		N3DS_MetaWriteU32(f, e->clutGenB);
		N3DS_MetaWriteU32(f, e->lastUsedFrame);
	}

	for (i = 0; i < N3DS_SUBPAGE_TOTAL_CELLS; i++)
	{
		const N3DS_SubPageCacheEntry *e = &s_subPageCache[i];

		N3DS_MetaWriteU32(f, (u32)e->used);
		N3DS_MetaWriteU32(f, (u32)(u16)e->page);
		N3DS_MetaWriteU32(f, (u32)(u16)e->clut);
		N3DS_MetaWriteU32(f, (u32)e->format);
		N3DS_MetaWriteU32(f, (u32)e->tileX);
		N3DS_MetaWriteU32(f, (u32)e->tileY);
		N3DS_MetaWriteU32(f, e->pageGen);
		N3DS_MetaWriteU32(f, e->clutGenA);
		N3DS_MetaWriteU32(f, e->clutGenB);
		N3DS_MetaWriteU32(f, e->lastUsedFrame);
	}

	fclose(f);
	snprintf(msg, sizeof(msg), "[CTR-3DS] cachemeta dumped to %s", path);
	N3DS_DebugLog(msg);
}
#endif
