#ifndef NATIVE_TRACK_FRAME_H
#define NATIVE_TRACK_FRAME_H

#include <macros.h>

// Per-frame camera + visibility snapshot for the native track renderer's draw
// side (docs/NATIVE_TRACK_RENDERER_DESIGN.md 4.2/12b). Recording only counts
// what it would draw -- no pixels move here; NativeTrackDraw_IssueForWalk
// (declared below) consumes NativeTrackFrame_ForWalk() to project and draw
// the resident mesh.
//
// Double-buffered like the GTE capture side table (platform/native_gte_capture.c).
// Protocol:
//   - NativeTrackFrame_Record (1P gate, RenderAllLevelGeometry) writes s_trackFrameBuf[write].
//   - NativeTrackFrame_PublishForWalk (every DrawOTag, same site as GTECapture_PublishForWalk)
//     only sets read = write.
//   - NativeTrackFrame_Clear (MainFrame_ResetDB, same site as GTECapture_Clear) does the flip:
//     write ^= 1, then invalidates the new write buffer (valid = 0). This must live in Clear,
//     not PublishForWalk, because DrawOTag also runs from FMV/error-screen/load-time-clear paths
//     that never call Record; only Clear's invalidate keeps those frames from republishing a
//     stale valid=1 buffer.
//   - The walk (main thread on PC, core-2 worker on 3DS render-thread builds) only ever touches
//     s_trackFrameBuf[read].
// Two slots suffice because only one walk is ever in flight: DrawSync fences the previous walk's
// read before the next ResetDB flips the buffer, the same argument native_gte_capture.c makes for
// its own double buffer.

// Ceiling on visible resident leaves recorded per frame. Sized from the mesh
// builder's own ceiling (NativeTrackLeafRange is allocated mesh->numBspNodes
// entries), but real tracks measure far below that: the [TRKMESH] census
// across the attract rotation never reported more than a few hundred
// resident leaves on any track. 1024 is headroom, not a tight fit; overflow
// past it is counted in droppedLeaves rather than silently truncated, so it
// shows up in the [TRKFRAME] line.
#define NATIVE_TRACK_FRAME_MAX_LEAVES 1024

struct NativeTrackFrameSnapshot
{
	// Camera, copied RAW from struct PushBuffer -- no pre-divide, no
	// pre-scale, no float conversion here. rot/trans are matrix_ViewProj's
	// m/t straight off the game struct (R is 1.12 fixed, TR is world units);
	// h/ofx/ofy are distanceToScreen_PREV and rect.w>>1/rect.h>>1, the exact
	// values RenderLists_Load1P2PGteState (game/RenderLevel/RenderLists.c)
	// feeds to the GTE. Keeping this raw means a reviewer can check it
	// against that function by inspection; the perspective divide (design
	// 4.3: sx = ofx + h*view.x/view.z) is step 2's job, done once per draw
	// from these fields, not baked in at record time.
	//
	// Deliberately absent: depthScale. Design doc 12b lists it alongside
	// H/OFX/OFY, but it has no reader anywhere in game/ -- see the "Deliberate
	// omissions" comment in game/MAIN/MainFrame_RenderFrame.c (~line 1040,
	// RenderAllLevelGeometry_ApplyLodBias) which found the same thing for the
	// LOD-bias instrument. Nothing seeds a meaningful value for step 2 to
	// consume, so it isn't recorded; re-add it here if a future retail reader
	// is ever found, not speculatively.
	s16 rot[3][3];
	s32 trans[3];
	s32 h;
	s32 ofx;
	s32 ofy;

	// Camera WORLD position (PushBuffer.pos, the same value
	// PushBuffer_UpdateFrustum calls cameraPos), for the water
	// distance-to-black fade the animated-vertex patcher applies
	// (NativeTrackAnim_PatchVertices). Copied by value like the rest of the
	// camera block; Y is unused -- retail's water fade is X/Z only.
	s32 camX;
	s32 camZ;

	// Visible leaves, as MESH LEAF-RANGE indices (NativeTrackMesh_LeafAt's
	// index space), NOT BSP indices -- the bspIndex -> leaf-range mapping is
	// resolved once here at record time (via NativeTrackMesh_LeafIndexForBsp)
	// so the draw side does zero searching, just a flat index list.
	u16 leafSlot[NATIVE_TRACK_FRAME_MAX_LEAVES];
	u32 leafCount;
	u32 droppedLeaves; // this frame's overflow past NATIVE_TRACK_FRAME_MAX_LEAVES

	// visFaceList[0] (per-quadblock visibility mask, VisMem, design 4.2/12)
	// straight from gGT->visMem1 at record time -- recorded so step 2 does
	// not have to reopen this struct and the publish protocol to add it
	// later, since design 4.2 and 12 both call for it eventually. RECORD ONLY,
	// DO NOT CONSUME in step 1 or step 2's first cut: v1 visibility here is
	// LEAF granularity only (design 4.2 makes per-BLOCK masking with this
	// pointer conditional on INST-2's results), and dereferencing a VisMem
	// pointer captured this way on the WORKER thread a frame later has not
	// been proven safe -- VisMem is game-thread state like PushBuffer, and
	// nothing here double-buffers what it points to the way the camera
	// fields above are copied by value. Any future consumer owes a check
	// that VisMem isn't mutated out from under the worker before reading
	// through this pointer.
	const int *visFaceList;

	// Set when EITHER the leaf cap (droppedLeaves > 0 this frame) or the BSP
	// list-walk cap (NativeTrackFrame_WalkList aborted a cyclic/runaway list)
	// truncated this snapshot. A truncated snapshot must not look complete to
	// a consumer -- step 2 should treat degraded=1 the same as it would treat
	// missing geometry, not silently draw a partial track.
	u8 degraded;

	u32 frameSeq; // monotonic; lets a consumer notice it read a stale buffer
	u8 valid;     // 0 = never recorded (or invalidated by NativeTrackFrame_Clear)
};

// Runtime toggle, three states (docs/NATIVE_TRACK_RENDERER_DESIGN.md 12b):
//   0 = off. Retail path draws the track exactly as today. Record() still
//       runs every frame (cheap, count-only) but leaves
//       `valid` at 0, so the draw consumer must treat state 0 as "draw
//       nothing from here", not "draw whatever happens to be in the buffer".
//   1 = on. Resident path REPLACES the retail track draw.
//   2 = both. Resident path draws IN ADDITION TO the retail draw -- a
//       diagnostic overlay so a wrong projection ("track draws somewhere
//       else") is visually distinguishable from nothing drawing at all
//       ("track doesn't draw"), which state 1 alone cannot tell apart.
// NativeTrackFrame_SkipRetailDraw returns true only for state 1 -- that is
// what makes 1 REPLACE the retail draw and 2 OVERLAY it.
// Declared here (not static) so n3ds_backend.c's dumpreq/button handlers and
// native_platform.c's PC argv/key surface -- all outside this file -- can
// reach it via a plain `extern int`, the same idiom every other runtime
// toggle uses.
extern int g_dbg_trackRender;

// Called once per frame from the RenderAllLevelGeometry 1P gate site
// (game/MAIN/MainFrame_RenderFrame.c), between RenderLists_Init1P2P and
// DrawLevelOvr1P. Concrete decomp types, not void* -- this file already
// lives inside the unity TU and sees game headers, same reasoning
// platform/native_track_mesh.c uses for struct Level/mesh_info.
void NativeTrackFrame_Record(struct GameTracker *gGT, struct mesh_info *mesh, struct DrawLevelOvr1PRenderList *renderList, struct PushBuffer *pushBuffer);

// Double-buffer publish, called from DrawOTag (platform/native_libgpu.c) at
// the exact same call site as GTECapture_PublishForWalk -- game thread,
// before the render job post, so the queue's own lock/signal orders it for
// whichever thread ends up walking (worker on 3DS render-thread builds, main
// thread everywhere else). Only sets read = write; the flip-and-invalidate
// used to live here too until the review fix above moved it to
// NativeTrackFrame_Clear (see the file-header comment for why).
void NativeTrackFrame_PublishForWalk(void);

// 1 when game/MAIN/MainFrame_RenderFrame.c must skip retail's DrawLevelOvr1P
// because the resident renderer is drawing the track instead. Not simply
// "g_dbg_trackRender == 1": the resident draw path is 3DS-only, so on any other
// platform state 1 drives the recorder and leaves retail drawing rather than
// blanking the level. See the .c definition for the bug that wording fixes.
int NativeTrackFrame_SkipRetailDraw(void);

// Flip-and-invalidate, called from MainFrame_ResetDB (game/MAIN/MainFrame.c)
// at the exact same call site as GTECapture_Clear, same idiom: write ^= 1,
// then the NEW write buffer's valid is forced to 0. This is what guarantees a
// frame that never calls Record still publishes a buffer a consumer must
// treat as empty -- see the file-header comment for the bug this fixes.
void NativeTrackFrame_Clear(void);

// What the walk (step 2's draw code) reads. Never NULL -- always points at
// one of the two static slots -- but check ->valid before trusting its
// contents; an unbuilt mesh, a mid-load level transition, the toggle being
// off, or simply a frame that never called Record since the last
// NativeTrackFrame_Clear all leave a published buffer with valid=0.
const struct NativeTrackFrameSnapshot *NativeTrackFrame_ForWalk(void);

// The CPU-side per-frame draw issuer (docs/NATIVE_TRACK_RENDERER_DESIGN.md
// 4.2/4.3/12b). Reads NativeTrackFrame_ForWalk()'s snapshot, composes the
// camera projection rows the shader needs, and issues one N3DS_TrackDraw_Leaf
// per visible resident leaf with its own per-leaf OT-bucket depth-range
// uniform. 3DS-only (CTR_NATIVE_TRACK_RENDERER && __3DS__).
void NativeTrackDraw_IssueForWalk(void);

#endif
