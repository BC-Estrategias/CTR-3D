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

/* Plain-scalar mirror of the vertex layout the 3DS GPU consumes. Narrower
 * than the shared GrVertex (include/platform/native_renderer_types.h):
 * page/clut never reach the GPU here -- texture resolution happens on the
 * CPU side of native_renderer_3ds.c before a draw call (design doc section
 * 1). All-float for now, matching every local citro3d example's attribute
 * format; see design doc section 6 for the packed-format follow-up this
 * deliberately defers. */
typedef struct
{
	float x, y;
	float u, v;
	float r, g, b, a;
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
 * GPU_RGBA8 texture, nearest-filtered, clamped, holding a grid of
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
/* 8 atlases (32MB) does NOT allocate -- C3D_TexInit fails and C3DInit returns
 * 0. 4 is the working figure; the real linear-memory ceiling is logged at
 * init so this stops being guesswork. */
#define N3DS_ATLAS_COUNT           4
#define N3DS_ATLAS_DIM             1024
#define N3DS_ATLAS_CELLS_PER_ROW   (N3DS_ATLAS_DIM / N3DS_DECODE_TEXTURE_DIM)
#define N3DS_ATLAS_CELLS_PER_ATLAS (N3DS_ATLAS_CELLS_PER_ROW * N3DS_ATLAS_CELLS_PER_ROW)

void N3DS_C3DUploadDecodedCell(int atlasIndex, int cellX, int cellY, const void *tiled256RGBA8);
void N3DS_C3DBindAtlas(int atlasIndex);
/* Enables PC's x2 "bright" multiplier on RGB, applied after the texture
 * modulate the way PC's fragment shader does it. Must not be folded into the
 * vertex colour instead -- see the implementation comment for why that loses
 * headroom and darkens everything textured. */
void N3DS_C3DSetColorDoubling(int enable);

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
