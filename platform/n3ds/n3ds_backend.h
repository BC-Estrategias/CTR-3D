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
 * N3DS_MAX_FRAME_VERTICES is a headroom ceiling, not a measured real number
 * -- matches the PC renderer's own MAX_VERTEX_BUFFER_SIZE (65536) rather
 * than guessing a 3DS-specific figure, since real per-frame vertex counts
 * are still unmeasured on this port (see the design doc's own open question
 * on this). Also used to size native_renderer_3ds.c's per-run conversion
 * scratch, since a single run can never need more space than a whole frame.
 */
#define N3DS_MAX_FRAME_VERTICES 65536
/* Returns the vertex offset this call's data was written at (pass to
 * N3DS_C3DDrawTriangles), or -1 if the frame buffer is full -- caller must
 * skip drawing in that case, not draw from a stale/wrong offset. */
int N3DS_C3DAppendVertices(const N3DS_Vertex *vertices, int count);
void N3DS_C3DDrawTriangles(int startVertex, int triangleCount);

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
#define N3DS_DECODE_TEXTURE_DIM    256
#define N3DS_ATLAS_COUNT           2
#define N3DS_ATLAS_DIM             1024
#define N3DS_ATLAS_CELLS_PER_ROW   (N3DS_ATLAS_DIM / N3DS_DECODE_TEXTURE_DIM)
#define N3DS_ATLAS_CELLS_PER_ATLAS (N3DS_ATLAS_CELLS_PER_ROW * N3DS_ATLAS_CELLS_PER_ROW)

void N3DS_C3DUploadDecodedCell(int atlasIndex, int cellX, int cellY, const void *tiled256RGBA8);
void N3DS_C3DBindAtlas(int atlasIndex);

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
