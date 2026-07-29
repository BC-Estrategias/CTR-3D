#include <macros.h>
#include "platform/native_gte_capture.h"

#ifndef CTR_NATIVE_GTE_CAPTURE
#define CTR_NATIVE_GTE_CAPTURE 0
#endif

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE

#include <string.h>

// Direct-mapped, verify-by-stored-key side table (docs/GTE_CAPTURE_DESIGN.md).
// A collision or a stale (last frame's) entry is always a miss, never a wrong
// value -- callers fall back to today's exact snapped behavior on a miss.
//
// DOUBLE-BUFFERED (s26, the render-thread prerequisite): the game thread
// writes frame N+1's captures while the core-2 worker may still be walking
// frame N and reading its table. Two buffers; MainFrame_ResetDB's
// GTECapture_Clear() is the flip point (it already sits exactly at the DB
// swap), and DrawOTag publishes the write buffer as the walk's read buffer at
// post time. One walk in flight ever (the game's own DrawSync fence), so by
// the time ResetDB flips back to a buffer, no walk can still hold it. The
// publish->worker-read ordering rides the render queue's own lock/signal
// pair; no extra synchronization exists or is needed. PC's synchronous walk
// reads the buffer it just wrote (publish makes read==write), byte-identical
// to the single-buffered behavior.
//
// Sizing: eviction misses are not free visually -- on a STATIC scene (menus),
// a vertex that hits most frames but is evicted on some frames pops by up to
// 1px on exactly those frames. PC stays at 2^18 entries (4 MiB/buffer). The
// 3DS -- which consumes this table for the retained-geometry full form, not
// desnap -- takes 2^16 (1.5 MiB/buffer): there an eviction only downgrades a
// prim from retained to walked for a frame, invisible by construction.
#ifdef __3DS__
#define GTE_CAPTURE_TABLE_BITS 16
#else
#define GTE_CAPTURE_TABLE_BITS 18
#endif
#define GTE_CAPTURE_TABLE_SIZE (1u << GTE_CAPTURE_TABLE_BITS)

typedef struct
{
	u32 key;        // (u16)sy << 16 | (u16)sx -- same halves as the real C2_SXYn register (gtereg.h)
	u32 generation; // stamped from the owning buffer's generation; 0 = never written, always a miss
	u8 fracX8;      // top 8 of the 16 discarded fractional bits, i.e. 1/256px resolution
	u8 fracY8;
	u16 sz3;      // reserved for stereo (unused by vertex de-snapping)
	s32 hOverSz3; // reserved for perspective-correct UV (unused by vertex de-snapping)
	s16 vx, vy, vz; // model-space input vector (retainability rendezvous)
	s16 _pad;
} GTECaptureEntry;

typedef struct
{
	GTECaptureEntry entries[GTE_CAPTURE_TABLE_SIZE];
	u32 generation;
	// UI band travels with the buffer: the band pointers describe the frame
	// whose captures live here, and the walk that reads them may run a frame
	// behind the game thread's current SetUIBand call.
	const void *bandStart;
	const void *bandEnd;
} GTECaptureBuffer;

global_variable GTECaptureBuffer s_gteCaptureBuf[2] = {{{{0}}, 1, NULL, NULL}, {{{0}}, 1, NULL, NULL}};
// Game-thread cursor: which buffer Store/SetUIBand/Clear touch.
global_variable int s_gteCaptureWriteIdx;
// Walk cursor: which buffer Lookup*/MarkOTNode read. Written only by
// GTECapture_PublishForWalk on the game thread at DrawOTag time, before the
// walk job is posted (or before the synchronous walk runs).
global_variable int s_gteCaptureReadIdx;

internal u32 GTECapture_PackKey(s16 sx, s16 sy)
{
	return ((u32)(u16)sy << 16) | (u32)(u16)sx;
}

internal u32 GTECapture_Hash(u32 key)
{
	// Fibonacci multiplicative hash over the whole packed key -- both sx and
	// sy must contribute; a plain mask would only look at sx (sy lives in the
	// high 16 bits) and collide constantly on vertical runs.
	return (key * 0x9E3779B1u) >> (32u - GTE_CAPTURE_TABLE_BITS);
}

void GTECapture_Clear(void)
{
	GTECaptureBuffer *buf;

	// Flip first, then invalidate the NEW write buffer by generation bump.
	// The buffer being abandoned may still be read by an in-flight walk; it
	// is left untouched.
	s_gteCaptureWriteIdx ^= 1;
	buf = &s_gteCaptureBuf[s_gteCaptureWriteIdx];

	buf->generation++;
	if (buf->generation == 0) // wrapped after ~2^32 frames (2+ years continuous at 60fps)
	{
		memset(buf->entries, 0, sizeof(buf->entries));
		buf->generation = 1;
	}
}

void GTECapture_PublishForWalk(void)
{
	s_gteCaptureReadIdx = s_gteCaptureWriteIdx;
}

void GTECapture_Store(s16 sx, s16 sy, s64 sxFull, s64 syFull, u16 sz3, s32 hOverSz3, s16 vx, s16 vy, s16 vz)
{
	GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureWriteIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	GTECaptureEntry *entry = &buf->entries[GTECapture_Hash(key)];

	entry->key = key;
	entry->generation = buf->generation;
	entry->fracX8 = (u8)((sxFull & 0xFFFF) >> 8);
	entry->fracY8 = (u8)((syFull & 0xFFFF) >> 8);
	entry->sz3 = sz3;
	entry->hOverSz3 = hOverSz3;
	entry->vx = vx;
	entry->vy = vy;
	entry->vz = vz;
}

int GTECapture_Lookup(s16 sx, s16 sy, u8 *fracX8, u8 *fracY8)
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	const GTECaptureEntry *entry = &buf->entries[GTECapture_Hash(key)];

	if ((entry->generation != buf->generation) || (entry->key != key))
	{
		return 0;
	}

	*fracX8 = entry->fracX8;
	*fracY8 = entry->fracY8;
	return 1;
}

int GTECapture_LookupModel(s16 sx, s16 sy, s16 outV[3])
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];
	u32 key = GTECapture_PackKey(sx, sy);
	const GTECaptureEntry *entry = &buf->entries[GTECapture_Hash(key)];

	if ((entry->generation != buf->generation) || (entry->key != key))
	{
		return 0;
	}

	outV[0] = entry->vx;
	outV[1] = entry->vy;
	outV[2] = entry->vz;
	return 1;
}

// Walk-thread state: BeginOTWalk/MarkOTNode/LookupSuppressed all run on
// whichever thread owns the walk (worker on 3DS, main on PC) -- one walk in
// flight, so a single flag needs no buffering.
global_variable int s_gteCaptureSuppress;

void GTECapture_SetUIBand(const void *start, const void *end)
{
	GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureWriteIdx];

	buf->bandStart = start;
	buf->bandEnd = end;
}

void GTECapture_BeginOTWalk(void)
{
	s_gteCaptureSuppress = 0;
}

void GTECapture_MarkOTNode(const void *node)
{
	const GTECaptureBuffer *buf = &s_gteCaptureBuf[s_gteCaptureReadIdx];

	// Sticky: the UI band sits at the tail of the reversed OT walk, so once
	// entered, every later packet this walk is UI. Reset by BeginOTWalk.
	if ((node >= buf->bandStart) && (node < buf->bandEnd))
	{
		s_gteCaptureSuppress = 1;
	}
}

int GTECapture_LookupSuppressed(void)
{
	return s_gteCaptureSuppress;
}

#endif /* defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE */
