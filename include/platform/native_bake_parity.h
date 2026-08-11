#ifndef NATIVE_BAKE_PARITY_H
#define NATIVE_BAKE_PARITY_H

#include <macros.h>

// Bake-parity diff (roadmap "track bake pre-export"): verifies the
// resident track mesh (platform/native_track_mesh.c) against retail's own
// emitted packets, on PC, in one process. The retail track path transforms
// the SAME LevVertex pool the builder read and emits POLY_FT4/GT4 packets
// with provably-correct per-corner UVs and colors -- so every textured
// packet the OT walk parses is a free ground-truth sample. Matching goes
// screen coordinate -> GTECapture_LookupModel -> model-space position ->
// baked vertex (the builder emits exactly 4 consecutive verts per face, so
// vertIndex/4 names the face). A packet only counts when ALL its corners
// resolve to distinct verts of ONE face, which makes cross-primitive capture
// collisions structurally irrelevant instead of statistically rare.
//
// The UV diff needs no atlas math: baked verts still hold source page-space
// UVs (atlas encoding happens later, at upload). A mismatch whose packet UV
// equals a DIFFERENT corner's baked UV is a permutation error -- accumulated
// per rotation class, this convicts or acquits individual rows of the
// builder's sTrackUvPerm table (whose derivation was never checked against
// retail pixels; see its comment). Mismatches matching no corner bucket
// separately (animated-texture frames land there by construction).
//
// PC-only diagnostic behind CTR_NATIVE_BAKE_PARITY (CMake forces
// TRACK_RENDERER + GTE_CAPTURE on with it). Zero behavior change: reads
// capture/packet/mesh state, writes only its own counters and Platform_Log
// lines. Declarations are always visible; every call site is gated, so no
// stubs exist when the gate is off.

struct LevVertex;

// Build-time hooks (game thread, called by native_track_mesh.c's builder):
// Reset clears the side arrays and invalidates the walk-side index at the
// start of a build; NoteVert records baked vert i's PRE-WELD source position
// (welded verts change position and would otherwise never match retail's
// GTE inputs) and its color_lo (the FULL_DYNAMIC far-fade source, so the
// color diff can classify hi/lo/between/other instead of just "different").
void BakeParity_BuildReset(void);
void BakeParity_BuildNoteVert(unsigned vertIndex, const struct LevVertex *src);

// Walk-side hooks (whichever thread walks the OT -- same single-walker
// contract as NativeTrackAnim_PatchFrame). BeginWalk lazily (re)indexes the
// latched mesh and emits the periodic [BAKEPAR] report; the Note* taps sit
// in native_gpu.c's textured-poly decode cases and take the raw packet
// (typed as void* so this header needs no psyq typedefs).
void BakeParity_BeginWalk(void);
void BakeParity_NotePolyFT3(const void *poly);
void BakeParity_NotePolyFT4(const void *poly);
void BakeParity_NotePolyGT3(const void *poly);
void BakeParity_NotePolyGT4(const void *poly);

#endif
