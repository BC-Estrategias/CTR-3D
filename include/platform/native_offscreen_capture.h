#ifndef NATIVE_OFFSCREEN_CAPTURE_H
#define NATIVE_OFFSCREEN_CAPTURE_H

/*
 * PC-side dfe=0 (offscreen render-to-VRAM) split capture, Phase 0 of
 * CTR_INTERNAL, PC only.
 *
 * Two outputs from one instrumented run, both armed by
 * `--offscreen-capture <file>`:
 *
 * 1. Inventory (text, via Platform_Log, "[OFSCAP]" prefix): every offscreen
 *    session and split -- clip rect, vertex/triangle counts, blend mode,
 *    texture format, gouraud/semi-trans/mask flags, dtd, distinct
 *    page/clut values. This bounds the 3DS raster's feature matrix with
 *    data instead of assumptions, and records decal session/triangle
 *    counts for the delete-the-imposters decision gate.
 *
 * 2. Golden stream (binary, <file>): for the first captured sessions, the
 *    full VRAM mirror at session begin, every split's raw GrVertex stream +
 *    state, and the mirror rect PC's own flush produced. The host-side
 *    raster test (tools/offscreen_raster_test) replays the same stream
 *    through the 3DS software raster and byte-diffs the rects -- the Phase 1
 *    pass/fail harness.
 *
 * Hook sites: DrawSplit (native_gpu.c) for splits; the PC renderer's
 * SetOffscreenState (native_renderer.c) for session begin / post-flush.
 *
 * Golden file layout (little-endian, x86 host both writes and reads):
 *   header:  u32 magic 'CTOG' (0x474F5443), u32 version=1,
 *            u32 vramW=1024, u32 vramH=512, u32 grVertexSize=20
 *   records: u32 tag, then
 *     tag=1 SESSION_BEGIN: u32 vblank; s32 x,y,w,h;
 *                          u16 vram[1024*512] (full mirror snapshot)
 *     tag=2 SPLIT:         u32 vblank; s32 blendMode, texFormat, textured,
 *                          semiTrans, stpOut, maskSet, drawPrimMode, dtd,
 *                          numVerts; GrVertex[numVerts] raw
 *     tag=3 SESSION_END:   u32 vblank; s32 x,y,w,h; u16 rect[w*h]
 *                          (mirror contents after PC's flush)
 *   Sessions beyond the capture caps produce no records (inventory text
 *   still logs them); a session's records are always the contiguous
 *   BEGIN, SPLIT*, END sequence.
 */

#include <platform/native_renderer_types.h>

int NativeOffscreenCapture_ConfigureFromArgs(int argc, char **argv);
void NativeOffscreenCapture_Tick(void);
void NativeOffscreenCapture_OnBegin(const RECT16 *rect);
void NativeOffscreenCapture_OnFlush(const RECT16 *rect);
void NativeOffscreenCapture_OnSplit(const GrVertex *verts, int numVerts, int blendMode, int texFormat, int textured, int semiTrans, int stpOut, int maskSet,
                                    int drawPrimMode, int dtd, const RECT16 *clip);

/*
 * --vram-trace: log every VRAM-mirror write operation ("[VTRACE] op=...")
 * with its rect, plus src for copies. Same Phase 0 diagnostic family: built
 * to attribute which writer populates a given VRAM region on PC (the
 * menu-text pages question -- boot->menu shows ZERO dfe=0 sessions, so the
 * writer must be LoadImage, MoveImage or the framebuffer-readback path).
 * srcX/srcY are -1 when the op has no VRAM source.
 */
void NativeVramTrace_Op(const char *op, int x, int y, int w, int h, int srcX, int srcY);

/*
 * --tri-dump <file> --tri-dump-vblank N: dump one frame's triangles as the
 * same 36-byte records the 3DS writes to sdmc:/tridump_<frame>.bin (layout
 * documented in native_renderer_3ds.c; atlas-resolution fields are -1 on
 * PC). Content dumps proved the 3DS caches byte-correct while
 * the screen quilts, so the platforms must disagree on the ADDRESSING side
 * -- per-triangle page/clut/UV -- and this is the PC half of that diff.
 */
void NativeOffscreenCapture_OnAnySplit(const GrVertex *verts, int numVerts, int texFormat, int textured);

#endif
