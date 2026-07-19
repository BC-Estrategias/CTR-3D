#ifndef NATIVE_GTE_CAPTURE_H
#define NATIVE_GTE_CAPTURE_H

#include <macros.h>

// Per-frame side table of GTE sub-pixel fractions discarded by the >>16 in
// GTE_RotTransPers, keyed by the exact truncated screen coordinate written to
// the primitive packet. See docs/GTE_CAPTURE_DESIGN.md. Declarations are
// always visible; each caller gates its own use behind CTR_NATIVE_GTE_CAPTURE,
// so a flag-off build never emits a call and these never need to be defined.

void GTECapture_Store(s16 sx, s16 sy, s64 sxFull, s64 syFull, u16 sz3, s32 hOverSz3);
int GTECapture_Lookup(s16 sx, s16 sy, u8 *fracX8, u8 *fracY8);
void GTECapture_Clear(void);

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
