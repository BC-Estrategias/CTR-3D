#ifndef NATIVE_GTE_CAPTURE_H
#define NATIVE_GTE_CAPTURE_H

#include <macros.h>

// Per-frame side table of GTE sub-pixel fractions discarded by the >>16 in
// GTE_RotTransPers, keyed by the exact truncated screen coordinate written to
// the primitive packet. See docs/GTE_CAPTURE_DESIGN.md. Declarations are
// always visible; each caller gates its own use behind CTR_NATIVE_GTE_CAPTURE,
// so a flag-off build never emits a call and these never need to be defined.

// Runtime ablation of the capture writes, for measuring what the subsystem
// costs before a feature commits to depending on it. Three states, because the
// two per-vertex calls answer different questions:
//   1 = full capture (NoteMatrix + Store) -- today's compiled-in behavior
//   2 = Store only, no NoteMatrix -- what a depth consumer actually needs
//   0 = neither; every later lookup misses and each consumer takes the exact
//       snapped fallback it already has for collisions and evictions, so 0 is
//       behaviorally today's flag-off build.
// 1-vs-2 prices the matrix-class probe; 2-vs-0 prices the table itself.
// Defined only in CTR_NATIVE_GTE_CAPTURE builds (every caller gates on it).
extern int g_dbg_gteCapture;

void GTECapture_Store(s16 sx, s16 sy, s64 sxFull, s64 syFull, u16 sz3, s32 hOverSz3, s16 vx, s16 vy, s16 vz);
int GTECapture_Lookup(s16 sx, s16 sy, u8 *fracX8, u8 *fracY8);
// Model-space vector that produced this screen coordinate (same rendezvous
// and same miss-on-collision safety as GTECapture_Lookup). Consumed by the
// OTWALK_PROFILE retainability instrument; the retained-geometry play proper
// will grow its own keyed-by-content variant.
// True view-space depth (SZ3) for a packet's screen coordinate -- the shared
// depth axis the track renderer already uses. docs/PACKET_DEPTH_DESIGN.md.
int GTECapture_LookupDepth(s16 sx, s16 sy, u16 *outSz3);
int GTECapture_LookupModel(s16 sx, s16 sy, s16 outV[3]);
// Model vector plus the frame-scoped matrix-state id the projection ran
// under (s26 matrix-class probe; the retained full form's matrix classes).
int GTECapture_LookupModelEx(s16 sx, s16 sy, s16 outV[3], u8 *outMtxId);
// Per projected vertex, from the GTE: the 8 contiguous CP2C words (R+TR).
void GTECapture_NoteMatrix(const void *words8);
// Read-buffer census: returns dominant-id Store share in percent.
u32 GTECapture_ReadMatrixCensus(u8 *outDomId, u32 *outIdsUsed);
void GTECapture_Clear(void);
// Double-buffer publish (s26): called by the DrawOTag facade on the game
// thread, before the walk is posted (3DS worker) or run (PC sync) -- the walk
// then reads the buffer the frame just finished writing, while ResetDB's
// Clear() flips subsequent Stores to the other one.
void GTECapture_PublishForWalk(void);

// UI suppression. The frame's single DrawOTag walk renders world geometry
// first, then crosses into the swapchain OT's low six entries (the
// pushBuffer_UI band) and draws every UI packet last. UI coordinates come
// from game math, not this frame's GTE writes, so a lookup that HITs for one
// is always a false hit (foreign fraction -> visible shimmer). The walk
// reports each visited node; once one lands inside the band, lookups are
// suppressed until the next walk begins.
void GTECapture_SetUIBand(const void *start, const void *end);
void GTECapture_BeginOTWalk(void);
void GTECapture_MarkOTNode(const void *node);
int GTECapture_LookupSuppressed(void);

// DEPTHLESS-GEOMETRY suppression (s32). Bracket any producer whose GTE
// projection runs with the translation vector zeroed -- i.e. geometry drawn as
// if infinitely far, whose SZ3 is a function of the mesh's own radius and NOT
// of its distance from the camera. DrawSky_Full is the one such producer in
// the game. Stores are dropped between Set(1) and Set(0); lookups for those
// screen coordinates then miss, and the packet-depth path falls back to
// deriving depth from the OT bucket, which IS retail's own statement of how
// far the content is. See GTECapture_SetStoreSuppressed's definition for the
// bug this closes.
//
// Game-thread only, same ownership as Store itself. Not a runtime toggle: the
// suppressed population has no valid depth to capture in any configuration.
void GTECapture_SetStoreSuppressed(int suppressed);

#endif
