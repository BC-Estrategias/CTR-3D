#ifndef NATIVE_GTE_CAPTURE_H
#define NATIVE_GTE_CAPTURE_H

#include <macros.h>

// Per-frame side table of GTE sub-pixel fractions discarded by the >>16 in
// GTE_RotTransPers, keyed by the exact truncated screen coordinate written to
// the primitive packet. See docs/GTE_CAPTURE_DESIGN.md. Declarations are
// always visible; each caller gates its own use behind CTR_NATIVE_GTE_CAPTURE,
// so a flag-off build never emits a call and these never need to be defined.

void GTECapture_Store(s16 sx, s16 sy, s64 sxFull, s64 syFull, u16 sz3, s32 hOverSz3, s16 vx, s16 vy, s16 vz);
int GTECapture_Lookup(s16 sx, s16 sy, u8 *fracX8, u8 *fracY8);
// Model-space vector that produced this screen coordinate (same rendezvous
// and same miss-on-collision safety as GTECapture_Lookup). Consumed by the
// OTWALK_PROFILE retainability instrument; the retained-geometry play proper
// will grow its own keyed-by-content variant.
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

#endif
