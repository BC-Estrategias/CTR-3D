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

global_variable const GrVertex *s_pendingVertices = NULL;
global_variable int s_pendingVertexCount = 0;

/*
 * VRAM is 1024x512 = exactly 4x2 page-sized (256x256) dirty-generation
 * cells (design doc section 1). A decode cache entry records the cell
 * generation(s) it was decoded against and is stale if any has since moved.
 */
#define N3DS_VRAM_CELL_COLS  4
#define N3DS_VRAM_CELL_ROWS  2
#define N3DS_VRAM_CELL_DIM   256
global_variable u32 s_vramCellGen[N3DS_VRAM_CELL_ROWS * N3DS_VRAM_CELL_COLS];

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
 * never entered into the LRU cache; indices 1..N3DS_TOTAL_CELLS-1 are real.
 */
#define N3DS_TOTAL_CELLS (N3DS_ATLAS_COUNT * N3DS_ATLAS_CELLS_PER_ATLAS)
#define N3DS_WHITE_ATLAS  0
#define N3DS_WHITE_CELL_X 0
#define N3DS_WHITE_CELL_Y 0

global_variable N3DS_DecodeCacheEntry s_decodeCache[N3DS_TOTAL_CELLS];

/* Flat cell index -> which atlas texture, and which cell within its grid. */
internal void N3DS_CellIndexToAtlasCoord(int cellIndex, int *outAtlas, int *outCellX, int *outCellY)
{
	const int within = cellIndex % N3DS_ATLAS_CELLS_PER_ATLAS;

	*outAtlas = cellIndex / N3DS_ATLAS_CELLS_PER_ATLAS;
	*outCellX = within % N3DS_ATLAS_CELLS_PER_ROW;
	*outCellY = within / N3DS_ATLAS_CELLS_PER_ROW;
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

/* One increment per actual C3D_DrawArrays call (N3DS_FlushRun, called from
 * DrawTriangles) -- checks the design doc's own predicted "more, smaller
 * draw calls than PC" risk directly; measured ~150-290/frame before the
 * atlas rework above, which is what motivated it. */
global_variable u32 s_drawCallCount = 0;
global_variable u32 s_lastStatDrawCallCount = 0;

/* Scratch for one decoded+tiled page; reused every decode, never persisted. */
global_variable u8 s_decodeScratch[N3DS_DECODE_TEXTURE_DIM * N3DS_DECODE_TEXTURE_DIM * 4];

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
 * u/v (0-255) are native-texel coordinates within the page, already
 * format-aware as authored by the game (4bpp uses the full 0-255 range,
 * 8bpp only 0-127, 16bpp only 0-63) -- decoding a fixed 256x256 regardless
 * of format just leaves the unaddressed columns unused, never sampled.
 *
 * Alpha is the same 3-level encoding NativeRenderer_InitRG8LUT's channel
 * expansion feeds the PC shader (native_renderer.c:828-843): 0 = PS1
 * punch-through (raw value 0x0000), 128 = opaque, 255 = STP-on. Phase 2a
 * only consumes the 0-vs-nonzero distinction (single alpha test, see
 * n3ds_backend.c); the 128/255 split is decoded now so 2b's STP 2-pass
 * doesn't need to touch this function at all.
 */
internal void N3DS_DecodeAndTilePage(s16 texPage, s16 clut, TexFormat format, u8 *outTiled256)
{
	const int pageOriginX = (texPage & 0xF) * 64;
	const int pageOriginY = (texPage & 0x10) ? 256 : 0;
	const int clutOriginX = (clut & 0x3F) * 16;
	const int clutOriginY = clut >> 6;
	/*
	 * Decode only the columns this format can actually address. u/v reach the
	 * full 0-255 at 4bpp, but a texture page is 64 VRAM words wide, so 8bpp
	 * can only address u=0..127 and 16bpp only u=0..63 -- the game authors
	 * texcoords format-aware and never samples beyond that. Decoding a fixed
	 * 256 columns regardless was doing 2x the necessary work for 8bpp and 4x
	 * for 16bpp, on the hottest path in the renderer (a cache miss decodes
	 * 65,536 texels, and real track content was missing ~77 times a frame).
	 * Columns past this bound keep whatever a previous decode left in them
	 * and are never sampled.
	 */
	const int decodeWidth = (format == TF_4_BIT) ? 256 : ((format == TF_8_BIT) ? 128 : 64);
	int v;

	for (v = 0; v < N3DS_DECODE_TEXTURE_DIM; v++)
	{
		const int wordRow = pageOriginY + v;
		int u;

		for (u = 0; u < decodeWidth; u++)
		{
			u16 color16;
			u8 r, g, b, a;
			int morton, tileIndex, dstIndex;

			switch (format)
			{
			case TF_4_BIT:
			{
				const int wordCol = pageOriginX + (u >> 2);
				const u16 word = vram[wordRow * VRAM_WIDTH + wordCol];
				const int nibble = (word >> ((u & 3) * 4)) & 0xF;
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
				const int wordCol = pageOriginX + (u >> 1);
				const u16 word = vram[wordRow * VRAM_WIDTH + wordCol];
				const int index = (word >> ((u & 1) * 8)) & 0xFF;
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
				color16 = vram[wordRow * VRAM_WIDTH + pageOriginX + u];
				break;
			}

			r = (u8)((color16 & 31) << 3);
			g = (u8)(((color16 >> 5) & 31) << 3);
			b = (u8)(((color16 >> 10) & 31) << 3);

			if (color16 == 0)
			{
				a = 0;
			}
			else if (color16 & 0x8000)
			{
				a = 255;
			}
			else
			{
				a = 128;
			}

			morton = (u & 1) | ((v & 1) << 1) | ((u & 2) << 1) | ((v & 2) << 2) | ((u & 4) << 2) | ((v & 4) << 3);
			tileIndex = ((v >> 3) * (N3DS_DECODE_TEXTURE_DIM / 8) + (u >> 3)) * 64;
			dstIndex = tileIndex + morton;

			/*
			 * PICA200 GPU_RGBA8 texels run A,B,G,R in increasing byte order,
			 * not R,G,B,A -- the format name describes which channels it
			 * carries, not their memory layout. Verified empirically against
			 * devkitPro's own tex3ds: converting a solid (R=AA,G=BB,B=CC,A=DD)
			 * image with `tex3ds -r -z none -f rgba8` emits `dd cc bb aa` per
			 * texel. Writing R,G,B,A here put alpha in the sampled red channel
			 * (a near-constant 128/255, hence a uniform red cast), swapped
			 * green and blue, and fed the sampled alpha -- and therefore the
			 * punch-through alpha test -- from the red channel.
			 */
			outTiled256[dstIndex * 4 + 0] = a;
			outTiled256[dstIndex * 4 + 1] = b;
			outTiled256[dstIndex * 4 + 2] = g;
			outTiled256[dstIndex * 4 + 3] = r;
		}
	}
}

/*
 * Finds an atlas cell already decoded for (texPage, clut, format) and still
 * fresh (matching VRAM dirty-cell generations), or decodes into the
 * least-recently-used atlas cell on a miss. Returns a flat cell index (see
 * N3DS_CellIndexToAtlasCoord). Linear scan over N3DS_TOTAL_CELLS -- fine for
 * a correctness-first pass at this count; the design doc explicitly leaves
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
	int i, oldestSlot = 1;
	u32 oldestFrame = 0xFFFFFFFFu;

	for (i = 1; i < N3DS_TOTAL_CELLS; i++)
	{
		N3DS_DecodeCacheEntry *e = &s_decodeCache[i];

		if (e->used && (e->page == texPage) && (e->clut == clut) && (e->format == format) &&
		    (e->pageGen == s_vramCellGen[pageCell]) && (e->clutGenA == s_vramCellGen[clutCellA]) &&
		    (e->clutGenB == s_vramCellGen[clutCellB]))
		{
			e->lastUsedFrame = s_frameCounter;
			s_decodeCacheHits++;
			return i;
		}

		if (e->lastUsedFrame < oldestFrame)
		{
			oldestFrame = e->lastUsedFrame;
			oldestSlot = i;
		}
	}

	s_decodeCacheMisses++;
	if (s_decodeCache[oldestSlot].used)
	{
		s_decodeCacheEvictions++;
	}

	N3DS_DecodeAndTilePage(texPage, clut, format, s_decodeScratch);
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
	for (i = 0; i < N3DS_TOTAL_CELLS; i++)
	{
		s_decodeCache[i].used = 0;
		s_decodeCache[i].lastUsedFrame = 0;
	}

	/*
	 * White cell: flat opaque texel (r=g=b=255, a=128 -- the "opaque" alpha
	 * sentinel, see N3DS_DecodeAndTilePage) for untextured/Gouraud splits.
	 * Uniform colour makes the 8x8 tile swizzle a no-op (every permutation of
	 * identical values is identical), so this just fills linearly. Uploaded
	 * once here into atlas 0's cell (0,0); that cell is never entered into
	 * the LRU cache and never re-decoded. Byte order is A,B,G,R to match
	 * N3DS_DecodeAndTilePage -- writing it R,G,B,A here would land alpha in
	 * the sampled red channel and tint every untextured primitive.
	 */
	for (i = 0; i < (N3DS_DECODE_TEXTURE_DIM * N3DS_DECODE_TEXTURE_DIM); i++)
	{
		s_decodeScratch[i * 4 + 0] = 128;
		s_decodeScratch[i * 4 + 1] = 255;
		s_decodeScratch[i * 4 + 2] = 255;
		s_decodeScratch[i * 4 + 3] = 255;
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

			s_lastStatCacheMisses = s_decodeCacheMisses;
			s_lastStatCacheEvictions = s_decodeCacheEvictions;
			s_lastStatDrawCallCount = s_drawCallCount;

			printf("cache  hits %-8lu miss %-6lu (+%-4lu/w) evict %-4lu (+%-4lu/w)\n", (unsigned long)s_decodeCacheHits,
			       (unsigned long)s_decodeCacheMisses, missesWindow, (unsigned long)s_decodeCacheEvictions, evictionsWindow);
			printf("draws  total %-8lu +%-6lu/w  ~%lu.%lu draws/frame        \n", (unsigned long)s_drawCallCount, drawsWindow,
			       drawsPerFrameAvgX10 / 10UL, drawsPerFrameAvgX10 % 10UL);

			snprintf(cacheLine, sizeof(cacheLine),
			         "[CTR-3DS] cache hits=%lu misses=%lu missesWindow=%lu evictions=%lu evictionsWindow=%lu draws=%lu drawsWindow=%lu",
			         (unsigned long)s_decodeCacheHits, (unsigned long)s_decodeCacheMisses, missesWindow,
			         (unsigned long)s_decodeCacheEvictions, evictionsWindow, (unsigned long)s_drawCallCount, drawsWindow);
			N3DS_DebugLog(cacheLine);

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

void NativeRenderer_PresentVRAMDisplay(void)
{
}

void NativeRenderer_PresentVRAMRect(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
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
	s_pendingVertices = vertices;
	s_pendingVertexCount = count;
}

void NativeRenderer_DrawTriangles(int startVertex, int triangles)
{
	int i;
	int currentAtlas = -1;
	int vertsInRun = 0;
	const int untextured = (s_currentTextureId == N3DS_TEXTURE_ID_WHITE);

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
		int atlasIdx, cellX, cellY, v;

		if (untextured)
		{
			atlasIdx = N3DS_WHITE_ATLAS;
			cellX = N3DS_WHITE_CELL_X;
			cellY = N3DS_WHITE_CELL_Y;
		}
		else
		{
			N3DS_CellIndexToAtlasCoord(N3DS_FindOrDecodeTexture(triVerts[0].page, triVerts[0].clut, s_currentTexFormat), &atlasIdx,
			                            &cellX, &cellY);
		}

		if ((atlasIdx != currentAtlas) || ((vertsInRun + 3) > N3DS_MAX_RUN_VERTICES))
		{
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

			dst->x = (float)src->x;
			dst->y = (float)src->y;

			/* Offset into the cell's own region of the atlas, not the whole
			 * atlas -- (cellX,cellY) select which 256x256 region, (u,v) or
			 * the cell's centre (untextured) select where within it. */
			if (untextured)
			{
				dst->u = ((float)cellX * N3DS_DECODE_TEXTURE_DIM + (N3DS_DECODE_TEXTURE_DIM * 0.5f)) / (float)N3DS_ATLAS_DIM;
				dst->v = ((float)cellY * N3DS_DECODE_TEXTURE_DIM + (N3DS_DECODE_TEXTURE_DIM * 0.5f)) / (float)N3DS_ATLAS_DIM;
			}
			else
			{
				dst->u = ((float)cellX * N3DS_DECODE_TEXTURE_DIM + (float)src->u) / (float)N3DS_ATLAS_DIM;
				dst->v = ((float)cellY * N3DS_DECODE_TEXTURE_DIM + (float)src->v) / (float)N3DS_ATLAS_DIM;
			}

			dst->r = (float)src->r / 255.0f;
			dst->g = (float)src->g / 255.0f;
			dst->b = (float)src->b / 255.0f;
			dst->a = 1.0f;
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
