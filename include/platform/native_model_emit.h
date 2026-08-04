#ifndef NATIVE_MODEL_EMIT_H
#define NATIVE_MODEL_EMIT_H

#include <macros.h>

// Rung 1 of the PICA-native model renderer (docs/NATIVE_MODEL_RENDERER_DESIGN.md).
//
// The track renderer's equivalent pair is NativeTrackFrame_Record (game thread
// builds a snapshot) and NativeTrackDraw_IssueForWalk (render worker reads it).
// This is the model half of that shape: the game thread captures every model
// triangle it draws, in MODEL SPACE, with its instance's model->view matrix,
// into a double-buffered snapshot of plain scalars. Nothing here draws.
//
// WHERE THE CAPTURE SITS, AND WHY THERE. RenderBucket_DrawFunc_Normal walks a
// model's command list, delta-decodes each vertex, projects each triangle
// through the software GTE, and writes a POLY_GT3. The capture hooks the point
// where that walk has COMMITTED to drawing a triangle -- after projection and
// after the texture lookup succeeded -- so at this rung the captured set is
// exactly the set retail draws, triangle for triangle.
//
// That is deliberate for one rung only. It costs nothing extra (the walk is
// already running and the vertices are already decoded), it makes the capture
// verifiable against the packet path by construction, and the gap between what
// it captures and what native_model_frame.c's [MODELFRAME] reports as the
// topology maximum IS the amount retail's backface/off-screen rejection drops
// -- the number that decides whether the PICA path needs CPU-side culling
// (design section 7). Deleting the projection is rung 4, and moves this call
// about fifteen lines earlier in the same function.

struct InstDrawPerPlayer;
struct Instance;
struct PushBuffer;
struct TextureLayout;

// Runtime toggle, same three-state shape as g_dbg_trackRender:
//   0 = off. No capture, no logging, no cost -- the A/B baseline, which must be
//       indistinguishable from a build with the feature compiled out.
//   1 = native replaces the packet path (rung 4+; at rung 1 it only captures,
//       because there is nothing yet to replace it with).
//   2 = both.
// Defined in platform/native_model_emit.c; only meaningful in
// CTR_NATIVE_MODEL_RENDERER builds.
extern int g_dbg_modelRender;

// Opens capture for one instance and records its model->view matrix and the
// camera terms the projection needs. Returns 0 when capture is inactive (toggle
// off, snapshot full, feature compiled out) -- callers use it to skip the
// per-triangle calls entirely rather than paying a test per triangle.
//
// forceSemiTrans: this instance's primitive WRITER emits semi-transparent
// primitives unconditionally, ignoring its textures (the ghost and depth-fade
// writers do; the normal one classifies by tpage like everything else). The
// caller computes it because the writer identifiers live in game/. Getting it
// wrong in the 0 direction is what made N. Tropy's ghost draw solid.
//
// inst: the instance being drawn, for the [AXISCHK] cross-class depth-axis
// check (its world translation is pushed through matrix_ViewProj -- the TRACK
// renderer's axis -- and compared against mvp.t[2], the axis every per-prim SZ
// this path and the packet stamps inherit). Only read under
// --model-depth-check; may be NULL, which skips the check for this instance.
int NativeModelEmit_BeginInstance(const struct InstDrawPerPlayer *idpp, const struct PushBuffer *pb, int forceSemiTrans, const struct Instance *inst);

// One triangle, in the exact form the software GTE was about to consume:
// packedXY3[i] is that corner's GTE VXY register (VX in the low halfword, VY in
// the high) and packedZ3[i] its VZ register, both already carrying the frame
// origin and retail's <<2 (or <<1 for the interpolated next-frame decoder).
// Taking the GTE's own inputs rather than re-deriving them from the vertex
// stack means the shader is provably fed the same numbers the GTE got.
//
// color3[i] is the rolling per-vertex colour BEFORE gte_dpct's depth cue -- the
// fog is applied by the primitive writer this capture runs ahead of. See the
// implementation note on why that is fine at this rung and what rung 3 does
// about it.
//
// tex may be NULL: retail's explicit untextured path, and a null table entry
// reaches the same writer.
//
// Returns 1 when the native path has CLAIMED this triangle, so the caller may
// skip its packet write; 0 means "draw it the retail way". Callers must AND this
// with their own replace-mode test rather than assuming a capture implies a
// draw -- the two diverge on snapshot overflow and on semi-transparent textures,
// and treating them as identical silently deleted geometry in both cases. See
// the implementation comment and docs/TRANSPARENCY_DESIGN.md section 5.
int NativeModelEmit_Triangle(const u32 *packedXY3, const u32 *packedZ3, const int *color3, const struct TextureLayout *tex);

// Closes the instance opened by BeginInstance. Safe to call after a
// BeginInstance that returned 0.
void NativeModelEmit_EndInstance(void);

// Once per frame, after every instance has been executed: seals the snapshot
// and prints [MDLEMIT] when the window closes.
void NativeModelEmit_EndFrame(void);

// Double-buffer flip, called from MainFrame_ResetDB alongside GTECapture_Clear
// -- the same frame-reset boundary, for the same reason (see
// native_track_frame.c's NativeTrackFrame_Clear for the bug that reasoning by
// analogy alone shipped: only the reset boundary may decide a buffer is stale).
void NativeModelEmit_Clear(void);

// Publishes the buffer the frame just wrote as the walk's read buffer. Called
// from the DrawOTag facade on the game thread, before the walk is posted.
void NativeModelEmit_PublishForWalk(void);

#endif // NATIVE_MODEL_EMIT_H
