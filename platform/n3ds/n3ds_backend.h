/*
 * Boundary between the game's unity translation unit and libctru.
 *
 * This header deliberately exposes NOTHING from <3ds.h> -- no libctru types,
 * no libctru constants -- and is safe to include from the unity build. That
 * separation is not stylistic. Pulling <3ds.h> into the unity TU collides with
 * the decompilation in three concrete ways:
 *
 *   1. include/psx/r3000.h defines the MIPS assembler aliases `flush` and
 *      `inval`, which mangle libctru's own parameter names (boss.h and nfc.h
 *      both declare parameters called `inval`).
 *   2. libctru typedefs `ThreadFunc` as void(*)(void*); the game already has
 *      its own ThreadFunc taking a struct Thread*.
 *   3. libctru typedefs u8/u32/s32 to the stdint types, which is exactly the
 *      binding include/macros.h has to avoid on ARM (see the note there).
 *
 * n3ds_backend.c is therefore compiled as its own translation unit -- it sees
 * libctru and never sees the game headers. Everything crossing this line uses
 * plain C scalar types.
 */

#ifndef N3DS_BACKEND_H
#define N3DS_BACKEND_H

/* --------------------------------------------------------------- timing --- */
/* Monotonic tick counter and its rate. Backed by the ARM11 system tick, which
 * runs at a fixed frequency regardless of New3DS CPU speed mode. */
unsigned long long N3DS_GetTickCount(void);
unsigned long long N3DS_GetTickFrequency(void);
void N3DS_SleepMs(unsigned int ms);

/* ------------------------------------------------------------- graphics --- */
void N3DS_GfxInit(void);
void N3DS_GfxExit(void);
/* Push the current framebuffer to the screen. Does NOT wait for vblank --
 * frame pacing belongs to VSync() in the platform layer. */
void N3DS_GfxPresent(void);

/* -------------------------------------------------------------- render --- */
/* citro3d lives entirely behind this boundary too, for the same reason as
 * everything else here -- see the file comment. Types crossing it are plain
 * scalars only. This is the mechanical GPU boundary for
 * docs/CITRO3D_RENDERER_DESIGN.md; it grows as native_renderer_3ds.c needs
 * new operations, phase by phase, rather than being designed complete
 * up front. */

/* <stdint.h>, not <3ds.h> or macros.h, for s16/u8 below -- this header is
 * included directly and EARLY from the unity build (main.c, before
 * <platform.h>/macros.h are reached, so their s16/u8 don't exist yet in that
 * TU at this point), as well as from n3ds_backend.c (which sees libctru's own
 * s16/u8 via <3ds.h>, included before this header there). Typedef'ing them
 * here too, rather than assuming either side already has, is what keeps this
 * header genuinely "safe to include from the unity build" per the file
 * comment above -- not just for the old all-float struct, which never needed
 * a sized type at all. Not a collision risk: GCC accepts a typedef name being
 * redeclared with an identical underlying type (confirmed against this exact
 * toolchain/-std=gnu99), and libctru's 3ds/types.h and macros.h both spell
 * s16/u8 as int16_t/uint8_t too -- verified by reading both, not assumed. */
#include <stdint.h>
typedef int16_t s16;
typedef uint8_t u8;

/* Plain-scalar mirror of the vertex layout the 3DS GPU consumes. Narrower
 * than the shared GrVertex (include/platform/native_renderer_types.h):
 * page/clut never reach the GPU here -- texture resolution happens on the
 * CPU side of native_renderer_3ds.c before a draw call (design doc section
 * 1). Packed to 12 bytes -- GPU_SHORT position/texcoord + GPU_UNSIGNED_BYTE
 * color, down from the original 32-byte all-float layout (docs/
 * VERTEX_FORMAT_DESIGN.md, "Lever 2") -- because PICA200 vertex fetch does
 * NOT normalize integer attribute formats: a GPU_SHORT of 6000 arrives in
 * the shader as 6000.0f, not 6000.0f/32767.0f. Verified against Azahar's own
 * vertex-loader source and a disassembly of citro3d's AttrInfo_AddLoader
 * (design doc finding 4), not assumed. The shader (ctr.v.pica) rescales
 * texcoord/color back to their old numeric range itself instead.
 *
 * Field order (position, texcoord, color) must NOT change: it matches the
 * GPU attribute-register permutation (0x210, set in N3DS_C3DInit below) and
 * is what lands every attribute on its own element-size boundary with zero
 * inter-attribute padding (design doc finding 5) -- reordering would force a
 * gap the struct doesn't provide, which PICA200 hardware can react to by
 * freezing rather than just drawing wrong.
 *
 * The trailing pad byte is not slop: without it the stride is 11 (odd), so
 * odd-indexed vertices' SHORT-typed fields land at odd byte offsets -- an
 * unaligned access real hardware may not tolerate. This risk is specifically
 * NOT visible by testing in Azahar (its vertex loader reads via an x86
 * reinterpret_cast that silently tolerates misalignment ARM hardware might
 * not), so don't drop this byte just because it "looks unused" on an
 * emulator screenshot -- see design doc finding 5 for the full reasoning. */
typedef struct
{
	s16 x, y;
	s16 u, v;
	u8 r, g, b;
	u8 _pad;
} N3DS_Vertex;

/* One-time setup: citro3d itself, the top-screen render target, the
 * compiled vertex shader, attribute layout, and the vertex buffer. Must be
 * called after N3DS_GfxInit (needs gfxInitDefault already done). Returns 0
 * on failure. */
int N3DS_C3DInit(void);
void N3DS_C3DExit(void);

/* Must bracket every N3DS_C3DAppendVertices/DrawTriangles call for a given
 * display frame -- native_renderer_3ds.c calls these from
 * NativeRenderer_BeginScene/EndScene (once per frame, per Platform_Begin/
 * EndScene's own re-entrancy guard), NOT from SwapWindow: real drawing
 * happens earlier, via DrawAllSplits (DrawOTag, and potentially again mid-
 * frame via DrawSync), so the frame bracket has to open before that, not at
 * present time. */
/*
 * Orthographic extents in PSX screen coordinates, not 3DS panel pixels. top is
 * always 0 and bottom > top, reproducing the PSX Y-down convention.
 */
void N3DS_C3DSetProjection(float left, float right, float bottom);

void N3DS_C3DBeginFrame(void);
void N3DS_C3DEndFrame(void);

/*
 * One real vertex buffer per FRAME, not per draw. C3D_DrawArrays only
 * appends GPU commands to a command list; the GPU doesn't actually read any
 * vertex data until that whole list is submitted at once, in C3D_FrameEnd
 * (GX_ProcessCommandList) -- confirmed directly against citro3d's upstream
 * renderqueue.c. DrawAllSplits/DrawSplit (native_gpu.c) call
 * NativeRenderer_DrawTriangles once per split -- up to MAX_DRAW_SPLITS
 * (4096) times a frame -- so a frame routinely queues many draws before any
 * of them actually executes. N3DS_C3DAppendVertices therefore writes each
 * caller's vertices into the NEXT free region of one persistent buffer and
 * returns where they landed; the caller must draw from that returned offset,
 * not 0. (A real bug from getting this wrong, found and fixed 2026-07-19:
 * an earlier version always wrote to offset 0 and always drew from offset 0,
 * on the mistaken assumption that C3D_FRAME_SYNCDRAW's frame-to-frame block
 * also serialised draws within one frame -- it doesn't. Every draw ended up
 * reading whichever run's vertices were written last, since GPU execution
 * only happens after ALL of a frame's CPU-side writes are already done.)
 * N3DS_C3DBeginFrame resets the write cursor to 0 every frame.
 *
 * SIZING -- corrected 2026-07-19. This budget covers a whole FRAME, but the
 * PC ceiling it was originally copied from (MAX_VERTEX_BUFFER_SIZE, 65536)
 * is per DrawAllSplits PASS, not per frame. DrawOTag calls DrawAllSplits
 * once (native_gpu.c:856) and DrawSync can call it twice more (:1934,
 * :1954); every pass refills s_gpu.vertexBuffer from scratch and calls
 * UpdateVertexBuffer again, so PC's ceiling resets up to three times a
 * frame where this cursor resets only once. Matching PC's number therefore
 * under-budgeted a frame by up to 3x -- and the overflow was SILENT:
 * N3DS_C3DAppendVertices returns -1 and N3DS_FlushRun drops the run, so
 * geometry vanished with no error, no log line, nothing on screen. Budget
 * the true worst case instead, and report a high-water mark so the real
 * figure stops being a guess.
 *
 * MEASURED 2026-07-19: peak occupancy in menu/attract content is 2937
 * vertices -- about 4% of the figure below, with zero dropped runs. So the
 * under-budgeting described above was real in principle but never actually
 * fired, and it is NOT the cause of any missing geometry seen so far.
 * Keeping PC's own 65536 (22x the observed peak) rather than inflating to
 * the 3x worst case, which would cost 4MB of linear memory to buy headroom
 * nothing has ever needed. The real defect was that overflow was SILENT;
 * droppedRuns below makes it loud, so if race content ever does exceed
 * this, it reports itself instead of quietly deleting models.
 *
 * Do NOT "fix" a future overflow by resetting the cursor per pass. Every
 * draw queued this frame is still unexecuted until C3D_FrameEnd, so reusing
 * offsets within one frame reintroduces exactly the aliasing bug above.
 */
#define N3DS_MAX_FRAME_VERTICES 65536
/* Largest single atlas-run, i.e. the size of native_renderer_3ds.c's
 * conversion scratch. A run flushes whenever the atlas changes or this
 * fills, so it bounds one staging buffer rather than a frame. Stays at PC's
 * per-pass ceiling: no single run can exceed one pass's total geometry. */
#define N3DS_MAX_RUN_VERTICES 65536
/* Returns the vertex offset this call's data was written at (pass to
 * N3DS_C3DDrawTriangles), or -1 if the frame buffer is full -- caller must
 * skip drawing in that case, not draw from a stale/wrong offset. */
int N3DS_C3DAppendVertices(const N3DS_Vertex *vertices, int count);
void N3DS_C3DDrawTriangles(int startVertex, int triangleCount);
/* Highest vertex count reached in any single frame so far. Reported in the
 * stats block: if this ever approaches N3DS_MAX_FRAME_VERTICES the budget
 * above is too small for real content and geometry is about to disappear. */
int N3DS_C3DFrameVertexHighWater(void);

/* ---------------------------------------------------- decoded textures --- */
/* CPU-decoded-texture cache backing store (design doc section 1), organised
 * as a small number of shared texture atlases rather than one dedicated
 * texture per decoded (page,clut,format) combo. Each atlas is a fixed
 * GPU_RGBA5551 texture (2026-07-20, was GPU_RGBA8 -- see N3DS_TILE_BYTES in
 * n3ds_backend.c), nearest-filtered, clamped, holding a grid of
 * N3DS_DECODE_TEXTURE_DIM-sized cells; native_renderer_3ds.c owns cache-
 * replacement policy (which (page,clut,format) is decoded into which
 * (atlas,cell)) and calls UploadDecodedCell to overwrite one cell's content.
 *
 * Why atlases, not one texture per entry (2026-07-19, added after the first
 * real-content Azahar run measured ~150-290 separate draw calls/frame): PC's
 * AddSplit doesn't split by page/clut (native_gpu.c:895-901), so one PC
 * scene commonly touches many (page,clut) combos in an interleaved triangle
 * stream; citro3d can only bind one texture per draw, so without atlasing
 * every combo change forces a new bind+draw. Multiple combos sharing one
 * atlas need only ONE bind+draw for the whole run, addressed via UV offset
 * into the right cell -- draw ORDER is unchanged either way (no reordering,
 * no sorting), which matters because this renderer has no depth test enabled
 * (same as PC) and relies entirely on draw order for correct layering.
 *
 * N3DS_ATLAS_DIM is 1024 because citro3d's own checkTexSize caps any single
 * texture at 1024x1024 -- confirmed directly against citro3d's upstream
 * texture.c, not assumed -- so this is the largest, and therefore
 * highest-cells-per-bind, atlas actually obtainable.
 *
 * IMPORTANT: citro3d's C3D_TexLoadImage (which C3D_TexUpload wraps) is a raw
 * memcpy over a WHOLE mip level -- verified directly against citro3d's
 * upstream source -- with no sub-rect upload API at all, so a single cell's
 * decoded content can't go through it. N3DS_C3DUploadDecodedCell instead
 * writes directly into the atlas's own tiled backing memory, computing the
 * cell's tile-block offset by hand (same PICA200 8x8 Z-order tiling as
 * before -- see the comment on N3DS_DecodeAndTilePage in native_renderer_3ds.c
 * for the verified tile-order derivation this extends). tiledRGBA8 (the
 * per-call input) must already be tiled the same way a whole-texture upload
 * would need to be. */
/*
 * CAPACITY, measured 2026-07-19. With 2 atlases (32 cells, 31 usable) real
 * track content evicted 2472 cells in a single 32-frame window -- 77 per
 * frame, i.e. the whole cache replaced ~2.5x every frame. That one number
 * caused BOTH visible problems at once: ~5M texel decodes/frame (the ~1fps),
 * and -- because an evicted cell is overwritten while draws referencing it
 * are still only queued -- nearly every textured triangle sampling some
 * other triangle's texture, which is what the red/green speckle was.
 * The working set is simply far larger than 31 entries, so give it room.
 */
#define N3DS_DECODE_TEXTURE_DIM    256
/*
 * 12 atlases at 2MB each (GPU_RGBA5551) = 24MB (2026-07-20, up from 8 = 16MB).
 * The boot log's own "linear free before atlases" line measured ~30169 KiB
 * (~29.5MB) free right before this allocation runs -- 8 atlases only ever used
 * 16MB of that, leaving ~14MB sitting unused. That headroom was never revisited
 * after the RGBA8->RGBA5551 switch that halved atlas cost; the "8 atlases
 * (32MB) does NOT allocate" note two paragraphs below is about the OLD, twice
 * -as-large RGBA8 format and stopped being the real ceiling once atlases got
 * cheaper. 12 leaves ~5.5MB of margin below the observed free figure, not the
 * whole thing -- if a future bump ever fails, N3DS_C3DBackend_InitialisePSX
 * logs exactly which atlas index failed and the free KiB at that point (see
 * its call to C3D_TexInit below), so this is safe to retune from real
 * boot-log numbers, not by guessing further.
 *
 * Why more atlases now, specifically (as opposed to more cells per atlas,
 * which N3DS_ATLAS_DIM's own comment says is already at citro3d's ceiling):
 * real race driving (2026-07-20, sub-page decode cache session) measured the
 * sub-page tier's capacity misses (954) dwarfing its stale misses (62) --
 * i.e. the cache is overwhelmingly serving genuinely-new content, not
 * re-decoding content that went stale. A whole race's total distinct texture
 * footprint, accumulated over the full route, exceeds any single moment's
 * working set (peak concurrent usage stayed well under budget) -- so old,
 * still-visible-from-some-angles cells get reclaimed for new track ahead
 * before they're actually done being needed, which reads as a texture
 * visibly changing as you approach new geometry. See
 * docs/SUBPAGE_DECODE_CACHE_DESIGN.md and the ctr-pc-poc-workspace memory for
 * the full diagnosis.
 *
 * The old note here said "8 atlases (32MB) does NOT allocate": true, and still
 * true, but that was 8 RGBA8 atlases. The real constraint is the linear
 * ceiling logged at init, not any specific atlas count -- keep reading that
 * log line rather than trusting a fixed number here.
 *
 * Why cells and not something cleverer: playing the menu measured a SATURATED
 * 63/63 working set with ~45 capacity misses per frame and 2052 evictions of
 * cells that a queued draw still pointed at. Cell count was the binding
 * constraint on both the corruption and the decode cost.
 */
#define N3DS_ATLAS_COUNT           12
#define N3DS_ATLAS_DIM             1024
#define N3DS_ATLAS_CELLS_PER_ROW   (N3DS_ATLAS_DIM / N3DS_DECODE_TEXTURE_DIM)
#define N3DS_ATLAS_CELLS_PER_ATLAS (N3DS_ATLAS_CELLS_PER_ROW * N3DS_ATLAS_CELLS_PER_ROW)

/* tiled256 buffer is RGBA5551 (2 bytes/texel), not RGBA8 -- see N3DS_TILE_BYTES. */
void N3DS_C3DUploadDecodedCell(int atlasIndex, int cellX, int cellY, const void *tiled256);
/* Sub-page counterpart (docs/SUBPAGE_DECODE_CACHE_DESIGN.md) -- writes an
 * 8x8-tile (64x64 texel) sub-cell at physical cell (physCellX,physCellY)'s
 * (subX,subY) quadrant-of-16, instead of a whole 32x32-tile cell. tiled64 is
 * RGBA5551, same tiling convention as tiled256 above, just a quarter the
 * cell's linear dimension. Both wrap the shared N3DS_C3DUploadTileBlock in
 * n3ds_backend.c. */
void N3DS_C3DUploadDecodedSubCell(int atlasIndex, int physCellX, int physCellY, int subX, int subY, const void *tiled64);
void N3DS_C3DBindAtlas(int atlasIndex);
/* Enables PC's x2 "bright" multiplier on RGB, applied after the texture
 * modulate the way PC's fragment shader does it. Must not be folded into the
 * vertex colour instead -- see the implementation comment for why that loses
 * headroom and darkens everything textured. */
void N3DS_C3DSetColorDoubling(int enable);

/* ------------------------------------------------------ VRAM display --- */
/* Dedicated (not atlas-shared) textures backing NativeRenderer_PresentVRAMRect/
 * PresentVRAMDisplay -- the boot-time SCEA/copyright VRAM-mirror blit. This
 * is boot-only content that never competes with the decode cache for atlas
 * capacity, so it gets its own small C3D_Tex pool rather than a shared cell.
 * tiled256 is a whole 256x256 RGBA5551 buffer, uploaded via the real
 * C3D_TexUpload -- unlike N3DS_C3DUploadDecodedCell's manual sub-rect write,
 * these textures are never sub-addressed, so the plain whole-texture wrapper
 * applies.
 *
 * A POOL, not one shared texture: citro3d defers GPU command execution to
 * C3D_FrameEnd, but C3D_TexUpload is an immediate CPU memcpy (same hazard
 * N3DS_C3DUploadDecodedCell's eviction policy avoids for the atlas, see its
 * header comment above). A single present call chunks a wide rect into
 * multiple pieces uploaded and drawn in the same still-open frame -- with one
 * shared texture, chunk 2's upload would overwrite chunk 1's texture memory
 * before chunk 1's already-queued draw actually executes, so both draws
 * would sample chunk 2's content. slotIndex selects which pool texture a
 * given chunk uses; the caller is responsible for giving each chunk drawn
 * within the same present call a distinct slot. 4 slots covers a 2x2 grid,
 * i.e. up to 512x512 -- comfortable headroom over the ~512x216 DISPENV this
 * is actually used for. */
#define N3DS_VRAM_DISPLAY_TEX_DIM   256
#define N3DS_VRAM_DISPLAY_TEX_COUNT 4
void N3DS_C3DUploadVramDisplayChunk(int slotIndex, const void *tiled256);
void N3DS_C3DBindVramDisplayTex(int slotIndex);

/* ---------------------------------------------------------------- input --- */

#define N3DS_BTN_A      (1u << 0)
#define N3DS_BTN_B      (1u << 1)
#define N3DS_BTN_X      (1u << 2)
#define N3DS_BTN_Y      (1u << 3)
#define N3DS_BTN_L      (1u << 4)
#define N3DS_BTN_R      (1u << 5)
#define N3DS_BTN_START  (1u << 6)
#define N3DS_BTN_SELECT (1u << 7)
#define N3DS_BTN_UP     (1u << 8)
#define N3DS_BTN_DOWN   (1u << 9)
#define N3DS_BTN_LEFT   (1u << 10)
#define N3DS_BTN_RIGHT  (1u << 11)
#define N3DS_BTN_ZL     (1u << 12) /* PSX L2, see native_input.c kc_l2 */
#define N3DS_BTN_ZR     (1u << 13) /* PSX R2, see native_input.c kc_r2 */

/* Samples the pad and returns held buttons as N3DS_BTN_* bits. D-pad and
 * circle pad are folded together. */
unsigned int N3DS_ReadButtons(void);

/* -------------------------------------------------------------- logging --- */
/* Emits to the console's debug string service. Azahar records these in its log
 * and Luma shows them on hardware, so this is the one output channel that works
 * without a screen or a writable SD card. */
void N3DS_DebugLog(const char *msg);

/* Points stderr at that same debug channel. Call before anything that can fail,
 * so early startup errors are visible even though the screen console does not
 * exist until the renderer initialises. */
void N3DS_EarlyInit(void);

/* Nonzero when the app was netloaded via 3dslink and the stderr stream back to
 * the sending PC is connected (run 3dslink with -s to receive it). Platform_Log
 * mirrors into that stream so hardware hang diagnostics arrive live over Wi-Fi
 * instead of needing an SD pull. */
int N3DS_NetloadStreamActive(void);

/* Registers the emulated PSX VRAM buffer so the SELECT+R debug capture in
 * N3DS_ReadButtons can write it to sdmc:/vramdump_<frame>.bin. Called once
 * from the renderer TU (which owns vram[]) since the backend TU can't see
 * that global. bytes is sizeof(vram). frameCounter points at the renderer's
 * frame counter (plain unsigned int, so no ABI hazard) and stamps every
 * capture filename -- fixed names cost a race capture once (2026-07-21).
 * CTR_INTERNAL-only diagnostic (texture-corruption investigation,
 * 2026-07-20); no-op otherwise. */
void N3DS_DebugSetVRAMForDump(const void *vram, unsigned int bytes, const unsigned int *frameCounter);

/* ---------------------------------------------------------------- memory --- */
/* Bytes currently handed out by malloc, and free space in the linear heap.
 * Reported on screen so runaway allocation is visible as it happens. */
unsigned long N3DS_HeapUsedBytes(void);
unsigned long N3DS_LinearFreeBytes(void);

/* ------------------------------------------------------------------ app --- */
/* Zero once the user has closed the applet (HOME/power), meaning the process
 * should shut down. */
int N3DS_MainLoopActive(void);

#endif
