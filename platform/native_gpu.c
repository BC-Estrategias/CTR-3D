/*
 * Derived from REDRIVER2/PsyCross MIT source:
 * externals/PsyCross/src/gpu/PsyX_GPU.cpp
 * See THIRD_PARTY_NOTICES.md for copyright and license details.
 */

#include <macros.h>
#include "platform/native_gpu.h"

#include <platform.h>

#if !defined(__3DS__)
#include <SDL3/SDL.h>
#endif

#include "platform/native_gte_capture.h"
#include "platform/native_log.h"
#include "platform/native_perf.h"
#include "platform/native_renderer.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Platform_PollHostEvents(void);
extern int g_cfg_bilinearFiltering;
extern int g_dbg_emulatorPaused;
extern int g_dbg_polygonSelected;

#define NATIVE_GPU_LOG(fmt, ...)   Platform_Log("[CTR GPU] " fmt, ##__VA_ARGS__)
#define NATIVE_GPU_ERROR(fmt, ...) Platform_LogError("[CTR GPU] [%s] - " fmt, __func__, ##__VA_ARGS__)

#ifndef CTR_NATIVE_DIRECT_EMIT
#define CTR_NATIVE_DIRECT_EMIT 0
#endif
#if defined(CTR_NATIVE) && CTR_NATIVE_DIRECT_EMIT
// Defined in platform/native_direct_emit.c, included later in this TU (it
// needs this file's and the 3DS renderer's file-scope state in view).
internal int NativeDirectEmit_TryNode(const u8 *packet, u32 tagLength);
internal void DirectEmit_FoldFrame(void);
#endif

// NOTE(aalhendi): Little-endian tag `CTRG` = CTR native GPU snapshot.
#define NATIVE_GPU_STATE_MAGIC     0x47525443
#define NATIVE_GPU_STATE_VERSION   1

#define GET_TPAGE_BLEND(tpage)     ((BlendMode)(((tpage >> 5) & 3) + 1))

#define GET_TPAGE_DITHER(tpage)    ((tpage >> 9) & 0x1)

#define GET_CLUT_X(clut)           ((clut & 0x3F) << 4)
#define GET_CLUT_Y(clut)           (clut >> 6)

internal TexFormat GetTPageFormat(int tpage)
{
	const int mode = (tpage >> 7) & 0x3;

	// NOTE(aalhendi): ctr-native local divergence from upstream PsyCross. PS1
	// mode 3 is reserved; TF_32_BIT_RGBA is only for explicit native override
	// textures, not raw retail TPAGE mode bits.
	return mode == 3 ? TF_16_BIT : (TexFormat)mode;
}

internal short GetTPageBase(int tpage)
{
	const u16 page = (u16)tpage;

	// NOTE(aalhendi): ctr-native local divergence for CTR retail emitters. The
	// shader wants xPage + yPage * 16, not raw draw-mode bits.
	return (s16)((page & 0xf) | ((page & 0x10) ? 0x10 : 0));
}

internal s16 NativeGpu_SignExtend11(u32 value)
{
	value &= 0x7ff;
	return (s16)((value ^ 0x400) - 0x400);
}

DISPENV activeDispEnv;
DRAWENV activeDrawEnv;
int g_GPUDisabledState = 0;

typedef struct
{
	DRAWENV drawenv;
	DISPENV dispenv;

	BlendMode blendMode;

	TexFormat texFormat;
	TextureID textureId;

	int drawPrimMode;
	bool psxTexturedSemiTrans;
	bool psxTextureOutputSTP;
	bool psxDrawMaskSet;

	u16 startVertex;
	u16 numVerts;

	const char *debugText;
} GPUDrawSplit;

#define MAX_DRAW_SPLITS 4096

typedef struct
{
	const char *currentSplitDebugText;
	TextureID overrideTexture;
	int overrideTextureWidth;
	int overrideTextureHeight;

	int drawPrimMode;
	bool psxDrawMaskSet;
	bool framebufferFeedbackRunActive;

	GrVertex vertexBuffer[MAX_VERTEX_BUFFER_SIZE];
	GPUDrawSplit splits[MAX_DRAW_SPLITS];
	int vertexIndex;
	int splitIndex;

	/*
	 * Next GrVertex.emitOrdinal value. Frame-scoped (reset in ClearSplits,
	 * NOT by mid-frame DrawAllSplits flushes -- depth must stay monotonic
	 * across the whole presented frame). Not part of NativeGpuSnapshot:
	 * it is re-derived every frame, so save states don't carry it.
	 */
	u32 emitOrdinal;
} NativeGpuState;

global_variable NativeGpuState s_gpu;

#ifdef CTR_NATIVE_OTWALK_PROFILE
/*
 * OT-walk content counters ([OTPROF], every 64 rendered frames). Decision
 * instrument for the render campaign: measures WHAT flows through the walk
 * (retainability, blend classes, state-key ceiling), not how fast -- timings
 * stay native_perf's job. Interpretation caveats the numbers depend on:
 * packet hashing runs inside the walk, so draw_otag_parse_ms is inflated in
 * profile builds -- take counts from a profile build, milliseconds from a
 * clean one; "reuse" compares packet payloads against the previous frame as
 * a multiset-blind set, so N identical packets matching one prior packet all
 * count as reused; a "frame" here is the span between ClearSplits calls that
 * saw content, which is one presented frame in the normal one-DrawOTag flow.
 */
#define OTPROF_WINDOW_FRAMES 64
#define OTPROF_MAX_HASHES    16384
#define OTPROF_PREV_SET_SIZE 32768 /* power of two, ~2x OTPROF_MAX_HASHES */
#define OTPROF_KEY_SET_SIZE  512   /* power of two */

typedef struct
{
	/* per-frame, reset by OTProf_FoldFrame */
	u32 frameNodes;
	u32 frameDryNodes;
	u32 framePrims;
	u32 frameBlend[5]; /* indexed by BlendMode */
	u32 frameTextured;
	u32 frameDraws;
	u32 frameKeys;
	u32 frameKeyTransitions;
	u32 lastKey;
	u32 frameHashCount;
	u32 frameHashes[OTPROF_MAX_HASHES];
	u32 frameShapeHashCount;
	u32 frameShapeHashes[OTPROF_MAX_HASHES];
	u32 frameShapeSkip;
	u32 frameMHashCount;
	u32 frameMHashes[OTPROF_MAX_HASHES];
	u32 frameMNoModel;
	u32 keySet[OTPROF_KEY_SET_SIZE];

	/* previous frame's packet hashes, open-addressed (0 = empty) */
	u32 prevSet[OTPROF_PREV_SET_SIZE];
	u32 prevShapeSet[OTPROF_PREV_SET_SIZE];
	u32 prevMSet[OTPROF_PREV_SET_SIZE];

	/* window accumulators */
	u32 winFrames;
	u32 winNodes;
	u32 winPrims;
	u32 winBlend[5];
	u32 winTextured;
	u32 winDraws;
	u32 winKeys;
	u32 winKeyTransitions;
	u32 winHashed;
	u32 winReused;
	u32 winShapeHashed;
	u32 winShapeReused;
	u32 winShapeSkip;
	u32 winMHashed;
	u32 winMReused;
	u32 winMNoModel;
	u32 winTruncated;
	u32 winDryMismatch;
} OTWalkProfileState;

global_variable OTWalkProfileState s_otprof;

internal u32 OTProf_HashWords(const u32 *words, u32 count)
{
	u32 h = 2166136261u;
	for (u32 i = 0; i < count; i++)
	{
		h ^= words[i];
		h *= 16777619u;
	}
	return h ? h : 1u; /* 0 is the empty-slot sentinel */
}

internal void OTProf_RecordNode(const u8 *packet, u32 tagLength)
{
	s_otprof.frameNodes++;

	/* payload words only -- the tag word holds the OT link, which changes
	 * every frame even when the packet content does not. Lengths outside
	 * (0,32] are the walk's own error class; don't hash unvalidated bytes. */
	if (tagLength == 0 || tagLength > 32)
	{
		return;
	}
	if (s_otprof.frameHashCount < OTPROF_MAX_HASHES)
	{
		s_otprof.frameHashes[s_otprof.frameHashCount++] = OTProf_HashWords((const u32 *)packet + P_LEN, tagLength);
	}
}

/*
 * Position-masked node hash ("shape"): the same payload OTProf_RecordNode
 * hashes, with every screen-XY word zeroed per the PS1 GPU command encoding.
 * A frame-over-frame match then means "same colors/UVs/state, position free
 * to move" -- the OPTIMISTIC ceiling for model-space retention, where
 * pkreuse (full bytes, camera-sensitive) is the pessimistic floor. The truth
 * for retained geometry sits between the two: this hash cannot see
 * model-space vertex animation hiding behind a stable appearance.
 *
 * Word layouts must agree with ParsePrimitive/ParseTaglessPrimitive. The
 * agreement is checked at runtime instead of trusted: a node whose computed
 * prim lengths do not tile tagLength exactly is left unhashed and counted in
 * shapeSkip -- nonzero shapeSkip means these tables and the real parser have
 * diverged and shape%% is not to be believed.
 */
internal void OTProf_RecordNodeShape(const u8 *packet, u32 tagLength)
{
	if (tagLength == 0 || tagLength > 32)
	{
		return;
	}

	const u32 *words = (const u32 *)packet + P_LEN;
	u32 masked[32];
	u32 w = 0;
	/* Model-identity companion ("mshape"): positions of the XY words this
	 * node's polys carried, so the tail can swap each for its model-space
	 * vector via the GTE-capture rendezvous. Any non-poly positioned prim
	 * (line, sprite, tile, fill, polyline) makes the node model-opaque --
	 * their positions ARE screen content, not projections of a mesh. */
	u8 xyIdx[32];
	u32 xyCount = 0;
	int modelOpaque = 0;

	for (u32 i = 0; i < tagLength; i++)
	{
		masked[i] = words[i];
	}

	while (w < tagLength)
	{
		const u32 code = masked[w] >> 24;
		u32 next;

		if (code >= 0x20 && code <= 0x3F)
		{
			const u32 gouraud = (code & 0x10u) ? 1u : 0u;
			const u32 textured = (code & 0x04u) ? 1u : 0u;
			const u32 numVerts = (code & 0x08u) ? 4u : 3u;
			u32 c = w + 1;

			for (u32 v = 0; v < numVerts; v++)
			{
				if (v > 0 && gouraud)
				{
					c++; /* per-vertex RGB */
				}
				if (c >= tagLength)
				{
					s_otprof.frameShapeSkip++;
					return;
				}
				xyIdx[xyCount++] = (u8)c;
				masked[c++] = 0; /* XY */
				if (textured)
				{
					c++; /* UV + clut/tpage */
				}
			}
			next = c;
		}
		else if (code >= 0x40 && code <= 0x5F)
		{
			modelOpaque = 1;
			if (code & 0x08u)
			{
				/* Polyline: variable length with a terminator scan. Mask the
				 * whole remainder rather than re-implement the scan; merges
				 * any trailing prims into the masked span, which only makes
				 * the ceiling reading slightly more optimistic. */
				for (u32 i = w + 1; i < tagLength; i++)
				{
					masked[i] = 0;
				}
				next = tagLength;
			}
			else if (code & 0x10u)
			{
				next = w + 4; /* G2: cmd+RGB0, XY0, RGB1, XY1 */
				if (next <= tagLength)
				{
					masked[w + 1] = 0;
					masked[w + 3] = 0;
				}
			}
			else
			{
				next = w + 3; /* F2: cmd+RGB, XY0, XY1 */
				if (next <= tagLength)
				{
					masked[w + 1] = 0;
					masked[w + 2] = 0;
				}
			}
		}
		else if (code >= 0x60 && code <= 0x7F)
		{
			/* Tiles/sprites: cmd+RGB, XY, [UV+clut if sprite], [WH if
			 * free-size]. XY is the position; W/H and UV are appearance. */
			const u32 sprite = (code & 0x04u) ? 1u : 0u;
			const u32 freeSize = ((code & 0x18u) == 0) ? 1u : 0u;

			modelOpaque = 1;
			next = w + 2 + sprite + freeSize;
			if (next <= tagLength)
			{
				masked[w + 1] = 0;
			}
		}
		else if (code == 0x02)
		{
			modelOpaque = 1;
			next = w + 3; /* fill: cmd+RGB, XY, WH */
			if (next <= tagLength)
			{
				masked[w + 1] = 0;
			}
		}
		else if (code >= 0xE0 && code <= 0xE7)
		{
			next = w + 1; /* draw-env settings; no position to mask */
		}
		else if (code == 0x00)
		{
			next = w + 1; /* NOP filler */
		}
		else
		{
			/* Unknown to these tables (MoveImage, IRQ, ...): don't guess. */
			s_otprof.frameShapeSkip++;
			return;
		}

		if (next > tagLength)
		{
			s_otprof.frameShapeSkip++;
			return;
		}
		w = next;
	}

	if (s_otprof.frameShapeHashCount < OTPROF_MAX_HASHES)
	{
		s_otprof.frameShapeHashes[s_otprof.frameShapeHashCount++] = OTProf_HashWords(masked, tagLength);
	}

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/* Model-identity hash: appearance words as masked above, plus each XY's
	 * model-space vector recovered through the GTE-capture rendezvous. A
	 * repeat of THIS hash frame-over-frame is true retainability -- same
	 * mesh data, same appearance, position free to be a matrix. UI-band
	 * packets and any vertex that misses the table (LERP'd mid-band
	 * geometry, procedural prims, collisions) make the node model-opaque:
	 * counted, not hashed. */
	if (modelOpaque
#if CTR_NATIVE_VERTEX_DESNAP
	    || GTECapture_LookupSuppressed()
#endif
	)
	{
		s_otprof.frameMNoModel++;
		return;
	}
	{
		u32 mwords[96];
		u32 mcount = 0;

		for (u32 i = 0; i < tagLength; i++)
		{
			mwords[mcount++] = masked[i];
		}
		for (u32 i = 0; i < xyCount; i++)
		{
			const u32 xy = words[xyIdx[i]];
			s16 v[3];

			if (!GTECapture_LookupModel((s16)(xy & 0xFFFFu), (s16)(xy >> 16), v))
			{
				s_otprof.frameMNoModel++;
				return;
			}
			mwords[mcount++] = ((u32)(u16)v[1] << 16) | (u32)(u16)v[0];
			mwords[mcount++] = (u32)(u16)v[2];
		}
		if (s_otprof.frameMHashCount < OTPROF_MAX_HASHES)
		{
			s_otprof.frameMHashes[s_otprof.frameMHashCount++] = OTProf_HashWords(mwords, mcount);
		}
	}
#endif /* CTR_NATIVE_GTE_CAPTURE */
}

internal void OTProf_CountPrim(BlendMode blendMode, TexFormat texFormat, bool textured, int tpage)
{
	s_otprof.framePrims++;
	s_otprof.frameBlend[blendMode]++;
	if (textured)
	{
		s_otprof.frameTextured++;
	}

	/* State key mirrors what a state-sorted submitter would bind: texture
	 * page origin + format + blend class. CLUT variants excluded in v1, so
	 * keys/f is a lower bound on 3DS decode-cache binds. */
	const u32 key = ((u32)tpage & 0x81Fu) | ((u32)texFormat << 12) | ((u32)blendMode << 16) | ((u32)(textured ? 1 : 0) << 20);
	if (key != s_otprof.lastKey)
	{
		s_otprof.frameKeyTransitions++;
		s_otprof.lastKey = key;
	}

	u32 slot = (key * 2654435761u) & (OTPROF_KEY_SET_SIZE - 1);
	for (u32 probes = 0; probes < OTPROF_KEY_SET_SIZE; probes++)
	{
		if (s_otprof.keySet[slot] == key)
		{
			return;
		}
		if (s_otprof.keySet[slot] == 0)
		{
			s_otprof.keySet[slot] = key;
			s_otprof.frameKeys++;
			return;
		}
		slot = (slot + 1) & (OTPROF_KEY_SET_SIZE - 1);
	}
}

internal void OTProf_CountDraw(void)
{
	s_otprof.frameDraws++;
}

internal void OTProf_FoldFrame(void)
{
	if (s_otprof.frameNodes == 0 && s_otprof.framePrims == 0)
	{
		return; /* GPU-disabled or empty scene edge; not a rendered frame */
	}

	u32 reused = 0;
	for (u32 i = 0; i < s_otprof.frameHashCount; i++)
	{
		const u32 h = s_otprof.frameHashes[i];
		u32 slot = (h * 2654435761u) & (OTPROF_PREV_SET_SIZE - 1);
		while (s_otprof.prevSet[slot] != 0)
		{
			if (s_otprof.prevSet[slot] == h)
			{
				reused++;
				break;
			}
			slot = (slot + 1) & (OTPROF_PREV_SET_SIZE - 1);
		}
	}

	memset(s_otprof.prevSet, 0, sizeof(s_otprof.prevSet));
	for (u32 i = 0; i < s_otprof.frameHashCount; i++)
	{
		const u32 h = s_otprof.frameHashes[i];
		u32 slot = (h * 2654435761u) & (OTPROF_PREV_SET_SIZE - 1);
		while (s_otprof.prevSet[slot] != 0 && s_otprof.prevSet[slot] != h)
		{
			slot = (slot + 1) & (OTPROF_PREV_SET_SIZE - 1);
		}
		s_otprof.prevSet[slot] = h;
	}

	u32 shapeReused = 0;
	for (u32 i = 0; i < s_otprof.frameShapeHashCount; i++)
	{
		const u32 h = s_otprof.frameShapeHashes[i];
		u32 slot = (h * 2654435761u) & (OTPROF_PREV_SET_SIZE - 1);
		while (s_otprof.prevShapeSet[slot] != 0)
		{
			if (s_otprof.prevShapeSet[slot] == h)
			{
				shapeReused++;
				break;
			}
			slot = (slot + 1) & (OTPROF_PREV_SET_SIZE - 1);
		}
	}

	memset(s_otprof.prevShapeSet, 0, sizeof(s_otprof.prevShapeSet));
	for (u32 i = 0; i < s_otprof.frameShapeHashCount; i++)
	{
		const u32 h = s_otprof.frameShapeHashes[i];
		u32 slot = (h * 2654435761u) & (OTPROF_PREV_SET_SIZE - 1);
		while (s_otprof.prevShapeSet[slot] != 0 && s_otprof.prevShapeSet[slot] != h)
		{
			slot = (slot + 1) & (OTPROF_PREV_SET_SIZE - 1);
		}
		s_otprof.prevShapeSet[slot] = h;
	}

	u32 mReused = 0;
	for (u32 i = 0; i < s_otprof.frameMHashCount; i++)
	{
		const u32 h = s_otprof.frameMHashes[i];
		u32 slot = (h * 2654435761u) & (OTPROF_PREV_SET_SIZE - 1);
		while (s_otprof.prevMSet[slot] != 0)
		{
			if (s_otprof.prevMSet[slot] == h)
			{
				mReused++;
				break;
			}
			slot = (slot + 1) & (OTPROF_PREV_SET_SIZE - 1);
		}
	}

	memset(s_otprof.prevMSet, 0, sizeof(s_otprof.prevMSet));
	for (u32 i = 0; i < s_otprof.frameMHashCount; i++)
	{
		const u32 h = s_otprof.frameMHashes[i];
		u32 slot = (h * 2654435761u) & (OTPROF_PREV_SET_SIZE - 1);
		while (s_otprof.prevMSet[slot] != 0 && s_otprof.prevMSet[slot] != h)
		{
			slot = (slot + 1) & (OTPROF_PREV_SET_SIZE - 1);
		}
		s_otprof.prevMSet[slot] = h;
	}

	s_otprof.winFrames++;
	s_otprof.winNodes += s_otprof.frameNodes;
	s_otprof.winPrims += s_otprof.framePrims;
	for (int i = 0; i < 5; i++)
	{
		s_otprof.winBlend[i] += s_otprof.frameBlend[i];
	}
	s_otprof.winTextured += s_otprof.frameTextured;
	s_otprof.winDraws += s_otprof.frameDraws;
	s_otprof.winKeys += s_otprof.frameKeys;
	s_otprof.winKeyTransitions += s_otprof.frameKeyTransitions;
	s_otprof.winHashed += s_otprof.frameHashCount;
	s_otprof.winReused += reused;
	s_otprof.winShapeHashed += s_otprof.frameShapeHashCount;
	s_otprof.winShapeReused += shapeReused;
	s_otprof.winShapeSkip += s_otprof.frameShapeSkip;
	s_otprof.winMHashed += s_otprof.frameMHashCount;
	s_otprof.winMReused += mReused;
	s_otprof.winMNoModel += s_otprof.frameMNoModel;
	if (s_otprof.frameHashCount >= OTPROF_MAX_HASHES)
	{
		s_otprof.winTruncated++;
	}
	if (s_otprof.frameDryNodes != s_otprof.frameNodes)
	{
		s_otprof.winDryMismatch++;
	}

	s_otprof.frameNodes = 0;
	s_otprof.frameDryNodes = 0;
	s_otprof.framePrims = 0;
	memset(s_otprof.frameBlend, 0, sizeof(s_otprof.frameBlend));
	s_otprof.frameTextured = 0;
	s_otprof.frameDraws = 0;
	s_otprof.frameKeys = 0;
	s_otprof.frameKeyTransitions = 0;
	s_otprof.lastKey = 0;
	s_otprof.frameHashCount = 0;
	s_otprof.frameShapeHashCount = 0;
	s_otprof.frameShapeSkip = 0;
	s_otprof.frameMHashCount = 0;
	s_otprof.frameMNoModel = 0;
	memset(s_otprof.keySet, 0, sizeof(s_otprof.keySet));

	if (s_otprof.winFrames < OTPROF_WINDOW_FRAMES)
	{
		return;
	}

	const u32 f = s_otprof.winFrames;
	const u32 p = s_otprof.winPrims ? s_otprof.winPrims : 1;
	const u32 hp = s_otprof.winHashed ? s_otprof.winHashed : 1;
	Platform_Log("[OTPROF] f=%lu nodes/f=%lu prims/f=%lu draws/f=%lu keys/f=%lu ktrans/f=%lu pkreuse=%lu%% pk/f=%lu trunc=%lu drymis=%lu\n",
	             (unsigned long)f, (unsigned long)(s_otprof.winNodes / f), (unsigned long)(s_otprof.winPrims / f), (unsigned long)(s_otprof.winDraws / f),
	             (unsigned long)(s_otprof.winKeys / f), (unsigned long)(s_otprof.winKeyTransitions / f), (unsigned long)(s_otprof.winReused * 100u / hp),
	             (unsigned long)(s_otprof.winHashed / f), (unsigned long)s_otprof.winTruncated, (unsigned long)s_otprof.winDryMismatch);
	Platform_Log("[OTPROF] blend%%: opq=%lu avg=%lu add=%lu sub=%lu aq=%lu | tex=%lu%%\n", (unsigned long)(s_otprof.winBlend[BM_NONE] * 100u / p),
	             (unsigned long)(s_otprof.winBlend[BM_AVERAGE] * 100u / p), (unsigned long)(s_otprof.winBlend[BM_ADD] * 100u / p),
	             (unsigned long)(s_otprof.winBlend[BM_SUBTRACT] * 100u / p), (unsigned long)(s_otprof.winBlend[BM_ADD_QUATER_SOURCE] * 100u / p),
	             (unsigned long)(s_otprof.winTextured * 100u / p));
	{
		const u32 sh = s_otprof.winShapeHashed ? s_otprof.winShapeHashed : 1;
		const u32 mh = s_otprof.winMHashed ? s_otprof.winMHashed : 1;
		Platform_Log("[OTPROF] shape=%lu%% shp/f=%lu shapeSkip=%lu mshape=%lu%% mshp/f=%lu nomdl/f=%lu\n",
		             (unsigned long)(s_otprof.winShapeReused * 100u / sh), (unsigned long)(s_otprof.winShapeHashed / f), (unsigned long)s_otprof.winShapeSkip,
		             (unsigned long)(s_otprof.winMReused * 100u / mh), (unsigned long)(s_otprof.winMHashed / f), (unsigned long)(s_otprof.winMNoModel / f));
	}

	s_otprof.winFrames = 0;
	s_otprof.winNodes = 0;
	s_otprof.winPrims = 0;
	memset(s_otprof.winBlend, 0, sizeof(s_otprof.winBlend));
	s_otprof.winTextured = 0;
	s_otprof.winDraws = 0;
	s_otprof.winKeys = 0;
	s_otprof.winKeyTransitions = 0;
	s_otprof.winHashed = 0;
	s_otprof.winReused = 0;
	s_otprof.winShapeHashed = 0;
	s_otprof.winShapeReused = 0;
	s_otprof.winShapeSkip = 0;
	s_otprof.winMHashed = 0;
	s_otprof.winMReused = 0;
	s_otprof.winMNoModel = 0;
	s_otprof.winTruncated = 0;
	s_otprof.winDryMismatch = 0;
}
#endif /* CTR_NATIVE_OTWALK_PROFILE */

struct NativeGpuSnapshot
{
	u32 magic;
	u32 version;
	u32 size;
	DRAWENV drawEnv;
	DISPENV dispEnv;
	s32 gpuDisabledState;
	s32 psxDrawMaskSet;
	u16 vram[VRAM_WIDTH * VRAM_HEIGHT];
};

int NativeGpu_HasPendingSplits(void)
{
	return s_gpu.splitIndex > 0;
}

/*
 * Stamp every vertex the last Parse*Primitive call appended with the current
 * emission ordinal, then advance it. One ordinal per primitive, monotonic in
 * OT-walk (= submission) order. Clamps at 0xFFFF instead of wrapping: a
 * wrap would put late primitives BEHIND early ones; a clamp only collapses
 * the >65535-prims tail to equal depth, where LEQUAL/GEQUAL-with-equal-pass
 * still resolves by draw order -- i.e. degrades to painter, never inverts.
 */
internal void NativeGpu_StampEmitOrdinal(int firstVertex)
{
	const u16 ordinal = (u16)(s_gpu.emitOrdinal < 0xFFFFu ? s_gpu.emitOrdinal : 0xFFFFu);

	for (int i = firstVertex; i < s_gpu.vertexIndex; i++)
	{
		s_gpu.vertexBuffer[i].emitOrdinal = ordinal;
	}
	s_gpu.emitOrdinal++;
}

/*
 * Reset the emission ordinal. Called by the platform renderer when the DEPTH
 * BUFFER clears (once per presented frame), NOT from ClearSplits: ClearSplits
 * runs per-OT-draw, and frames that draw in multiple passes (the loading
 * screen's flag pass + letters pass) would restart the ordinal mid-frame --
 * the later pass then starts at far depth and LOSES the depth test against
 * the earlier pass's already-written values. Found empirically: the depth
 * build's loading screens rendered the flag but buried the "LOAD" letters.
 * Ordinals must stay monotonic for the lifetime of one depth clear.
 */
void NativeGpu_ResetEmitOrdinal(void)
{
	s_gpu.emitOrdinal = 0;
}

void ClearSplits(void)
{
#ifdef CTR_NATIVE_OTWALK_PROFILE
	OTProf_FoldFrame();
#endif
#if defined(CTR_NATIVE) && CTR_NATIVE_DIRECT_EMIT
	DirectEmit_FoldFrame();
#endif
	// NOTE: the GTE capture table (native_gte_capture.c) is deliberately NOT
	// cleared here -- this runs per-OT-draw, after this frame's captures and
	// before this frame's vertex builds. Its clear lives in MainFrame_ResetDB.
	s_gpu.currentSplitDebugText = NULL;
	s_gpu.vertexIndex = 0;
	s_gpu.splitIndex = 0;
	s_gpu.splits[0].texFormat = (TexFormat)0xFFFF;
	s_gpu.splits[0].psxTexturedSemiTrans = false;
	s_gpu.splits[0].psxTextureOutputSTP = false;
	s_gpu.splits[0].psxDrawMaskSet = false;
	s_gpu.framebufferFeedbackRunActive = false;
}

int NativeGpu_GetStateSize(void)
{
	return (int)sizeof(struct NativeGpuSnapshot);
}

int NativeGpu_CaptureState(void *dst, int dstSize)
{
	struct NativeGpuSnapshot *snapshot = (struct NativeGpuSnapshot *)dst;

	if ((dst == NULL) || (dstSize < (int)sizeof(*snapshot)))
	{
		return 0;
	}
	if (NativeRenderer_GetVRAMStateSize() != (int)sizeof(snapshot->vram))
	{
		return 0;
	}

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->magic = NATIVE_GPU_STATE_MAGIC;
	snapshot->version = NATIVE_GPU_STATE_VERSION;
	snapshot->size = sizeof(*snapshot);
	snapshot->drawEnv = activeDrawEnv;
	snapshot->dispEnv = activeDispEnv;
	snapshot->gpuDisabledState = g_GPUDisabledState;
	snapshot->psxDrawMaskSet = s_gpu.psxDrawMaskSet;

	return NativeRenderer_CaptureVRAMState(snapshot->vram, sizeof(snapshot->vram));
}

int NativeGpu_RestoreState(const void *src, int srcSize)
{
	const struct NativeGpuSnapshot *snapshot = (const struct NativeGpuSnapshot *)src;

	if ((src == NULL) || (srcSize < (int)sizeof(*snapshot)))
	{
		return 0;
	}
	if ((snapshot->magic != NATIVE_GPU_STATE_MAGIC) || (snapshot->version != NATIVE_GPU_STATE_VERSION) || (snapshot->size != sizeof(*snapshot)))
	{
		return 0;
	}
	if ((snapshot->gpuDisabledState < 0) || (snapshot->gpuDisabledState > 1) || (snapshot->psxDrawMaskSet < 0) || (snapshot->psxDrawMaskSet > 1))
	{
		return 0;
	}
	if (NativeRenderer_GetVRAMStateSize() != (int)sizeof(snapshot->vram))
	{
		return 0;
	}
	if (!NativeRenderer_RestoreVRAMState(snapshot->vram, sizeof(snapshot->vram)))
	{
		return 0;
	}

	activeDrawEnv = snapshot->drawEnv;
	activeDispEnv = snapshot->dispEnv;
	g_GPUDisabledState = snapshot->gpuDisabledState;
	s_gpu.psxDrawMaskSet = snapshot->psxDrawMaskSet;
	ClearSplits();
	return 1;
}

internal void DrawEnvDimensionsInt(int *width, int *height)
{
	if (activeDrawEnv.dfe)
	{
		*width = activeDispEnv.disp.w;
		*height = activeDispEnv.disp.h;
	}
	else
	{
		*width = activeDrawEnv.clip.w;
		*height = activeDrawEnv.clip.h;
	}
}

void DrawEnvOffset(float *ofsX, float *ofsY)
{
	if (activeDrawEnv.dfe)
	{
		int w, h;
		DrawEnvDimensionsInt(&w, &h);

		if (w <= 0)
		{
			w = 1;
		}

		// NOTE(aalhendi): Convert PS1 VRAM-page draw offsets into display-relative host-screen offsets.
		// CTR alternates draw pages at y=0 and y=0x128; using raw modulo VRAM offsets shifts every other native frame vertically.
		*ofsX = activeDrawEnv.ofs[0] - activeDispEnv.disp.x;
		*ofsY = activeDrawEnv.ofs[1] - activeDispEnv.disp.y;
	}
	else
	{
		*ofsX = activeDrawEnv.ofs[0] - activeDrawEnv.clip.x;
		*ofsY = activeDrawEnv.ofs[1] - activeDrawEnv.clip.y;
	}
}

void LineSwapSourceVerts(VERTTYPE **p0, VERTTYPE **p1, u8 **c0, u8 **c1)
{
	// swap line coordinates for left-to-right and up-to-bottom direction
	if (((*p0)[0] > (*p1)[0]) || ((*p0)[1] > (*p1)[1] && (*p0)[0] == (*p1)[0]))
	{
		VERTTYPE *tmp = *p0;
		*p0 = *p1;
		*p1 = tmp;

		u8 *tmpCol = *c0;
		*c0 = *c1;
		*c1 = tmpCol;
	}
}

void MakeLineArray(GrVertex *vertex, VERTTYPE *p0, VERTTYPE *p1)
{
	const VERTTYPE dx = p1[0] - p0[0];
	const VERTTYPE dy = p1[1] - p0[1];

	float ofsX, ofsY;
	DrawEnvOffset(&ofsX, &ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	if (dx > abs((s16)dy))
	{ // horizontal
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX + 1;
		vertex[1].y = p1[1] + ofsY;

		vertex[2].x = vertex[1].x;
		vertex[2].y = vertex[1].y + 1;

		vertex[3].x = vertex[0].x;
		vertex[3].y = vertex[0].y + 1;
	}
	else
	{ // vertical
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX;
		vertex[1].y = p1[1] + ofsY + 1;

		vertex[2].x = vertex[1].x + 1;
		vertex[2].y = vertex[1].y;

		vertex[3].x = vertex[0].x + 1;
		vertex[3].y = vertex[0].y;
	} // TODO diagonal line alignment
}

// Draws geometry at its true GTE sub-pixel position instead of snapping to
// whole pixels, by looking up the fraction GTE_RotTransPers discarded (see
// docs/GTE_CAPTURE_DESIGN.md) and carrying it in the otherwise-dead
// GrVertex._p0/_p1 padding bytes. A miss (table cleared, or these vertices
// were never captured -- e.g. built from a synthesized/HUD coordinate) just
// leaves _p0/_p1 at the memset default of 0, i.e. today's exact behavior.
#ifndef CTR_NATIVE_VERTEX_DESNAP
#define CTR_NATIVE_VERTEX_DESNAP 0
#endif

void MakeVertexTriangle(GrVertex *vertex, VERTTYPE *p0, VERTTYPE *p1, VERTTYPE *p2)
{
	assert(p0);
	assert(p1);
	assert(p2);

	float ofsX, ofsY;
	DrawEnvOffset(&ofsX, &ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 3);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

#if defined(CTR_NATIVE) && CTR_NATIVE_VERTEX_DESNAP
	if (!GTECapture_LookupSuppressed())
	{
		VERTTYPE *srcs[3] = {p0, p1, p2};
		int i;
		for (i = 0; i < 3; i++)
		{
			u8 fracX8, fracY8;
			if (GTECapture_Lookup(srcs[i][0], srcs[i][1], &fracX8, &fracY8))
			{
				vertex[i]._p0 = (s8)fracX8;
				vertex[i]._p1 = (s8)fracY8;
			}
		}
	}
#endif
}

void MakeVertexQuad(GrVertex *vertex, VERTTYPE *p0, VERTTYPE *p1, VERTTYPE *p2, VERTTYPE *p3)
{
	assert(p0);
	assert(p1);
	assert(p2);
	assert(p3);

	float ofsX, ofsY;
	DrawEnvOffset(&ofsX, &ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

	vertex[3].x = p3[0] + ofsX;
	vertex[3].y = p3[1] + ofsY;

#if defined(CTR_NATIVE) && CTR_NATIVE_VERTEX_DESNAP
	{
		// 2D UI quads (font glyphs, panels) must not consult the table: the key
		// is only a screen position, so a UI corner coincidentally sharing a
		// pixel with some projected world vertex falsely HITs and inherits that
		// vertex's fraction (visible as UI shimmer tracking background motion).
		// Discriminator: every 2D-affine transform of a rectangle (CTR's menu
		// text waves/rocks/scales its glyph quads) stays an exact integer
		// parallelogram (p1-p0 == p3-p2 in the PSX TL,TR,BL,BR corner order),
		// while a perspective-projected world quad only forms one when it is
		// screen-parallel. Misclassifying such a world quad just draws it
		// snapped for that frame -- the safe fallback by construction.
		int parallelogram = ((p1[0] - p0[0]) == (p3[0] - p2[0])) && ((p1[1] - p0[1]) == (p3[1] - p2[1]));

		if (!parallelogram && !GTECapture_LookupSuppressed())
		{
			VERTTYPE *srcs[4] = {p0, p1, p2, p3};
			int i;
			for (i = 0; i < 4; i++)
			{
				u8 fracX8, fracY8;
				if (GTECapture_Lookup(srcs[i][0], srcs[i][1], &fracX8, &fracY8))
				{
					vertex[i]._p0 = (s8)fracX8;
					vertex[i]._p1 = (s8)fracY8;
				}
			}
		}
	}
#endif
}

void MakeVertexRect(GrVertex *vertex, VERTTYPE *p0, s16 w, s16 h)
{
	assert(p0);

	float ofsX, ofsY;
	DrawEnvOffset(&ofsX, &ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = vertex[0].x;
	vertex[1].y = vertex[0].y + h;

	vertex[2].x = vertex[0].x + w;
	vertex[2].y = vertex[0].y + h;

	vertex[3].x = vertex[0].x + w;
	vertex[3].y = vertex[0].y;
}

void MakeTexcoordQuad(GrVertex *vertex, u8 *uv0, u8 *uv1, u8 *uv2, u8 *uv3, s16 page, s16 clut, u8 dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);
	assert(uv3);

	const u8 bright = 2;
	const short texPage = GetTPageBase(page);

	vertex[0].u = uv0[0];
	vertex[0].v = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = texPage;
	vertex[0].clut = clut;

	vertex[1].u = uv1[0];
	vertex[1].v = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = texPage;
	vertex[1].clut = clut;

	vertex[2].u = uv2[0];
	vertex[2].v = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = texPage;
	vertex[2].clut = clut;

	vertex[3].u = uv3[0];
	vertex[3].v = uv3[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = texPage;
	vertex[3].clut = clut;
	/*
	if (g_cfg_bilinearFiltering)
	{
	    vertex[0].tcx = -1;
	    vertex[0].tcy = -1;

	    vertex[1].tcx = -1;
	    vertex[1].tcy = -1;

	    vertex[2].tcx = -1;
	    vertex[2].tcy = -1;

	    vertex[3].tcx = -1;
	    vertex[3].tcy = -1;
	}*/
}

void MakeTexcoordTriangle(GrVertex *vertex, u8 *uv0, u8 *uv1, u8 *uv2, s16 page, s16 clut, u8 dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);

	const u8 bright = 2;
	const short texPage = GetTPageBase(page);

	vertex[0].u = uv0[0];
	vertex[0].v = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = texPage;
	vertex[0].clut = clut;

	vertex[1].u = uv1[0];
	vertex[1].v = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = texPage;
	vertex[1].clut = clut;

	vertex[2].u = uv2[0];
	vertex[2].v = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = texPage;
	vertex[2].clut = clut;
	/*
	if (g_cfg_bilinearFiltering)
	{
	    vertex[0].tcx = -1;
	    vertex[0].tcy = -1;

	    vertex[1].tcx = -1;
	    vertex[1].tcy = -1;

	    vertex[2].tcx = -1;
	    vertex[2].tcy = -1;

	    vertex[3].tcx = -1;
	    vertex[3].tcy = -1;
	}*/
}

void MakeTexcoordRect(GrVertex *vertex, u8 *uv, s16 page, s16 clut, s16 w, s16 h)
{
	assert(uv);

	// sim overflow
	if ((int)uv[0] + w > 255)
	{
		w = 255 - uv[0];
	}
	if ((int)uv[1] + h > 255)
	{
		h = 255 - uv[1];
	}

	const u8 bright = 2;
	const u8 dither = 0;
	const short texPage = GetTPageBase(page);

	vertex[0].u = uv[0];
	vertex[0].v = uv[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = texPage;
	vertex[0].clut = clut;

	vertex[1].u = uv[0];
	vertex[1].v = uv[1] + h;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = texPage;
	vertex[1].clut = clut;

	vertex[2].u = uv[0] + w;
	vertex[2].v = uv[1] + h;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = texPage;
	vertex[2].clut = clut;

	vertex[3].u = uv[0] + w;
	vertex[3].v = uv[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = texPage;
	vertex[3].clut = clut;

	if (g_cfg_bilinearFiltering)
	{
		vertex[0].tcx = -1;
		vertex[0].tcy = -1;

		vertex[1].tcx = -1;
		vertex[1].tcy = -1;

		vertex[2].tcx = -1;
		vertex[2].tcy = -1;

		vertex[3].tcx = -1;
		vertex[3].tcy = -1;
	}
}

void MakeTexcoordLineZero(GrVertex *vertex, u8 dither)
{
	const u8 bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;

	vertex[3].u = 0;
	vertex[3].v = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = 0;
	vertex[3].clut = 0;
}

void MakeTexcoordTriangleZero(GrVertex *vertex, u8 dither)
{
	const u8 bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;
}

void MakeTexcoordQuadZero(GrVertex *vertex, u8 dither)
{
	const u8 bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;

	vertex[3].u = 0;
	vertex[3].v = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = 0;
	vertex[3].clut = 0;
}

void MakeColourNoShade(GrVertex *vertex, int n)
{
	--n;
	while (n >= 0)
	{
		vertex[n].r = 128;
		vertex[n].g = 128;
		vertex[n].b = 128;
		vertex[n].a = 255;
		--n;
	}
}

void MakeColourLine(GrVertex *vertex, bool shadeTexOn, u8 *col0, u8 *col1)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 4);
		return;
	}
	assert(col0);
	assert(col1);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col1[0];
	vertex[2].g = col1[1];
	vertex[2].b = col1[2];
	vertex[2].a = 255;

	vertex[3].r = col0[0];
	vertex[3].g = col0[1];
	vertex[3].b = col0[2];
	vertex[3].a = 255;
}

void MakeColourTriangle(GrVertex *vertex, bool shadeTexOn, u8 *col0, u8 *col1, u8 *col2)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 3);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = 255;
}

void MakeColourQuad(GrVertex *vertex, bool shadeTexOn, u8 *col0, u8 *col1, u8 *col2, u8 *col3)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 4);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);
	assert(col3);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = 255;

	vertex[3].r = col3[0];
	vertex[3].g = col3[1];
	vertex[3].b = col3[2];
	vertex[3].a = 255;
}

void TriangulateQuad()
{
	/*
	Triangulate like this:

	v0--v1
	|  / |
	| /  |
	v2--v3

	NOTE: v2 swapped with v3 during primitive parsing but it not shown here
	*/

	s_gpu.vertexBuffer[s_gpu.vertexIndex + 4] = s_gpu.vertexBuffer[s_gpu.vertexIndex + 3];

	s_gpu.vertexBuffer[s_gpu.vertexIndex + 5] = s_gpu.vertexBuffer[s_gpu.vertexIndex + 2];
	s_gpu.vertexBuffer[s_gpu.vertexIndex + 2] = s_gpu.vertexBuffer[s_gpu.vertexIndex + 3];
	s_gpu.vertexBuffer[s_gpu.vertexIndex + 3] = s_gpu.vertexBuffer[s_gpu.vertexIndex + 1];
}

//------------------------------------------------------------------------------------------------------------------------

internal bool NativeGpu_RectOverlaps(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
	return (aw > 0) && (ah > 0) && (bw > 0) && (bh > 0) && (ax < bx + bw) && (bx < ax + aw) && (ay < by + bh) && (by < ay + ah);
}

internal bool NativeGpu_TPageOverlapsActiveDrawPage(int tpage)
{
	const int pageX = (tpage & 0xf) << 6;
	const int pageY = (tpage & 0x10) ? 0x100 : 0;
	const int pageW = 0x100;
	const int pageH = 0x100;

	if (!activeDrawEnv.dfe)
	{
		return false;
	}

	if (GetTPageFormat(tpage) != TF_16_BIT)
	{
		return false;
	}

	return NativeGpu_RectOverlaps(pageX, pageY, pageW, pageH, activeDrawEnv.clip.x, activeDrawEnv.clip.y, activeDrawEnv.clip.w, activeDrawEnv.clip.h);
}

internal void NativeGpu_PrepareFramebufferFeedback(int tpage)
{
	if (!NativeGpu_TPageOverlapsActiveDrawPage(tpage))
	{
		return;
	}

	if (s_gpu.framebufferFeedbackRunActive)
	{
		return;
	}

	// NOTE(aalhendi): PS1 can draw into VRAM and immediately texture from that
	// same draw page. Native batches primitives, so screen-feedback effects
	// like heat warp need an explicit barrier before their framebuffer-sampling
	// polygons consume the VRAM texture.
	if (NativeGpu_HasPendingSplits())
	{
		DrawAllSplits();
	}

	NativeRenderer_StoreFrameBuffer(activeDrawEnv.clip.x, activeDrawEnv.clip.y, activeDrawEnv.clip.w, activeDrawEnv.clip.h);
	s_gpu.framebufferFeedbackRunActive = true;
}

internal void AddSplit(bool semiTrans, bool textured, bool framebufferFeedback)
{
	int tpage = activeDrawEnv.tpage;

	if (framebufferFeedback)
	{
		NativeGpu_PrepareFramebufferFeedback(tpage);
	}
	else
	{
		s_gpu.framebufferFeedbackRunActive = false;
	}

	GPUDrawSplit *curSplit = &s_gpu.splits[s_gpu.splitIndex];

	BlendMode blendMode = semiTrans ? GET_TPAGE_BLEND(tpage) : BM_NONE;
	TexFormat texFormat = GetTPageFormat(tpage);
	TextureID textureId = textured ? NativeRenderer_GetVRAMTexture() : NativeRenderer_GetWhiteTexture();
	bool psxTexturedSemiTrans = semiTrans && textured && s_gpu.overrideTexture == 0;
	// NOTE(aalhendi): PS1 framebuffer bit 15 follows sampled texture STP for
	// textured draws unless E6 forces it. Recursive screen-copy effects depend
	// on this bit surviving after the blended textured pass.
	bool psxTextureOutputSTP = textured && s_gpu.overrideTexture == 0;

#ifdef CTR_NATIVE_OTWALK_PROFILE
	// Pre-override retail state: the counter wants what the game asked for,
	// not the native debug-text override texture.
	OTProf_CountPrim(blendMode, texFormat, textured, tpage);
#endif

	if (textured && s_gpu.overrideTexture != 0)
	{
		// override texture format, zero tpage
		texFormat = TF_32_BIT_RGBA;
		textureId = s_gpu.overrideTexture;
		psxTexturedSemiTrans = false;
	}

	// FIXME: compare drawing environment too?
	if (!psxTexturedSemiTrans && curSplit->blendMode == blendMode && curSplit->texFormat == texFormat && curSplit->textureId == textureId &&
	    curSplit->drawPrimMode == s_gpu.drawPrimMode && curSplit->psxTexturedSemiTrans == psxTexturedSemiTrans &&
	    curSplit->psxTextureOutputSTP == psxTextureOutputSTP && curSplit->psxDrawMaskSet == s_gpu.psxDrawMaskSet &&
	    curSplit->drawenv.clip.x == activeDrawEnv.clip.x && curSplit->drawenv.clip.y == activeDrawEnv.clip.y &&
	    curSplit->drawenv.clip.w == activeDrawEnv.clip.w && curSplit->drawenv.clip.h == activeDrawEnv.clip.h && curSplit->drawenv.dfe == activeDrawEnv.dfe &&
	    curSplit->debugText == s_gpu.currentSplitDebugText)
	{
		return;
	}

	curSplit->numVerts = s_gpu.vertexIndex - curSplit->startVertex;

	if (s_gpu.splitIndex + 1 >= MAX_DRAW_SPLITS)
	{
		NATIVE_GPU_ERROR("MAX_DRAW_SPLITS reached (too many blend modes, texture formats, drawEnv clip rects, dfe switches), expect rendering errors\n");
		return;
	}

	GPUDrawSplit *split = &s_gpu.splits[++s_gpu.splitIndex];
	split->blendMode = blendMode;
	split->texFormat = texFormat;
	split->textureId = textureId;
	split->drawPrimMode = s_gpu.drawPrimMode;
	split->psxTexturedSemiTrans = psxTexturedSemiTrans;
	split->psxTextureOutputSTP = psxTextureOutputSTP;
	split->psxDrawMaskSet = s_gpu.psxDrawMaskSet;
	split->drawenv = activeDrawEnv;
	split->dispenv = activeDispEnv;
	split->debugText = s_gpu.currentSplitDebugText;

	split->drawenv.tw.w = s_gpu.overrideTextureWidth;
	split->drawenv.tw.h = s_gpu.overrideTextureHeight;

	split->startVertex = s_gpu.vertexIndex;
	split->numVerts = 0;
}

void DrawSplit(const GPUDrawSplit *split)
{
#ifdef CTR_NATIVE_OTWALK_PROFILE
	OTProf_CountDraw();
#endif
	if (split->debugText)
	{
		NativeRenderer_PushDebugLabel(split->debugText);
	}

	const bool drawOnScreen = split->drawenv.dfe;
	if ((split->drawenv.clip.w <= 0) || (split->drawenv.clip.h <= 0))
	{
		// NOTE(aalhendi): VS/Battle end previews can shrink a losing viewport
		// into an empty retail draw area. Empty clips should consume no pixels,
		// and must not leak stale native offscreen/scissor state.
		NativeRenderer_SetupClipMode(&split->drawenv.clip, &split->dispenv, drawOnScreen);
		NativeRenderer_SetOffscreenState(&split->drawenv.clip, &split->dispenv, 0);
		if (split->debugText)
		{
			NativeRenderer_PopDebugLabel();
		}
		return;
	}

	NativeRenderer_SetStencilMode(split->drawPrimMode); // draw with mask 0x16

	NativeRenderer_SetTexture(split->textureId, split->texFormat);

	if (split->texFormat == TF_32_BIT_RGBA)
	{
		NativeRenderer_SetOverrideTextureSize(split->drawenv.tw.w, split->drawenv.tw.h);
	}

	NativeRenderer_SetPSXDrawMaskSet(split->psxDrawMaskSet);
	NativeRenderer_SetPSXTextureOutputSTP(split->psxTextureOutputSTP);

	NativeRenderer_SetupClipMode(&split->drawenv.clip, &split->dispenv, drawOnScreen);
	NativeRenderer_SetOffscreenState(&split->drawenv.clip, &split->dispenv, !drawOnScreen);

#if defined(CTR_INTERNAL) && !defined(__3DS__)
	if (!drawOnScreen)
	{
		// After SetOffscreenState so the capture's session (OnBegin) already
		// exists when its first split records.
		NativeOffscreenCapture_OnSplit(&s_gpu.vertexBuffer[split->startVertex], split->numVerts, (int)split->blendMode, (int)split->texFormat,
		                               split->textureId != NativeRenderer_GetWhiteTexture(), split->psxTexturedSemiTrans ? 1 : 0,
		                               split->psxTextureOutputSTP ? 1 : 0, split->psxDrawMaskSet, split->drawPrimMode, split->drawenv.dtd,
		                               &split->drawenv.clip);
	}
	// Every split, any dfe: the --tri-dump addressing records (no-op unless
	// armed for this vblank).
	NativeOffscreenCapture_OnAnySplit(&s_gpu.vertexBuffer[split->startVertex], split->numVerts, (int)split->texFormat,
	                                  split->textureId != NativeRenderer_GetWhiteTexture());
#endif

	if (split->psxTexturedSemiTrans)
	{
		// NOTE(aalhendi): CTR native renderer divergence from upstream PsyCross.
		// PS1 textured ABE only blends texels whose sampled 16-bit color has STP
		// set; non-STP texels remain opaque. Native split state is per draw,
		// so draw this primitive-sized split twice with shader-side STP masks.
		NativeRenderer_SetBlendMode(BM_NONE);
		NativeRenderer_SetPSXTextureSemiTransPass(1);
		NativeRenderer_DrawTriangles(split->startVertex, split->numVerts / 3);

		NativeRenderer_SetBlendMode(split->blendMode);
		NativeRenderer_SetPSXTextureSemiTransPass(2);
		NativeRenderer_DrawTriangles(split->startVertex, split->numVerts / 3);

		NativeRenderer_SetPSXTextureSemiTransPass(0);
	}
	else
	{
		NativeRenderer_SetBlendMode(split->blendMode);
		NativeRenderer_SetPSXTextureSemiTransPass(0);
		NativeRenderer_DrawTriangles(split->startVertex, split->numVerts / 3);
	}

	if (split->debugText)
	{
		NativeRenderer_PopDebugLabel();
	}
}

internal void SetPSXMaskState(u32 code)
{
	s_gpu.psxDrawMaskSet = (code & 1) != 0;
}

//
// Draws all polygons after AggregatePTAG
//
void DrawAllSplits()
{
	NativePerf_BeginScope(NATIVE_PERF_BUCKET_DRAW_ALL_SPLITS);
#ifdef CTR_INTERNAL
	if (g_dbg_emulatorPaused)
	{
		for (int i = 0; i < 3; i++)
		{
			GrVertex *vert = &s_gpu.vertexBuffer[g_dbg_polygonSelected + i];
			vert->r = 255;
			vert->g = 0;
			vert->b = 0;

			NATIVE_GPU_LOG("==========================================\n");
			NATIVE_GPU_LOG("POLYGON: %d\n", g_dbg_polygonSelected);
			NATIVE_GPU_LOG("X: %d Y: %d\n", vert->x, vert->y);
			NATIVE_GPU_LOG("U: %d V: %d\n", vert->u, vert->v);
			NATIVE_GPU_LOG("TP: %d CLT: %d\n", vert->page, vert->clut);

			NATIVE_GPU_LOG("==========================================\n");
		}

		Platform_PollHostEvents();
	}
#endif

	// next code ideally should be called before EndScene
	NativeRenderer_UpdateVertexBuffer(s_gpu.vertexBuffer, s_gpu.vertexIndex);

	for (int i = 1; i <= s_gpu.splitIndex; i++)
	{
		DrawSplit(&s_gpu.splits[i]);
	}

	ClearSplits();
	NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_ALL_SPLITS);
}

// forward declarations
int ParsePrimitive(P_TAG *polyTag);
int ParseTaglessPrimitive(u32 *command);

internal bool NativeGpu_IsValidOTLink(uintptr_t link)
{
	if (NativeGpuLinks_IsRegisteredHostPointer((const void *)link))
	{
		return (link & (sizeof(u32) - 1)) == 0;
	}

	return false;
}

internal u32 NativeGpu_ReadPacketWordForLog(uintptr_t packet, int wordIndex)
{
	const struct PlatformMempackArena *arena = Platform_GetMempackArena();
	const uintptr_t word = packet + (uintptr_t)wordIndex * sizeof(u32);
	const uintptr_t end = word + sizeof(u32);

	if ((NativeGpuLinks_IsRegisteredHostRange((const void *)word, sizeof(u32))) && ((word & (sizeof(u32) - 1)) == 0))
	{
		return *(const u32 *)word;
	}

	if ((word < (uintptr_t)arena->base) || (end > (uintptr_t)arena->endOfMemory) || ((word & (sizeof(u32) - 1)) != 0))
	{
		return 0xffffffffu;
	}

	return *(const u32 *)word;
}

internal void NativeGpu_FormatPointerRegion(char *dst, size_t dstSize, uintptr_t ptr)
{
	if ((sdata == NULL) || (sdata->gGT == NULL))
	{
		snprintf(dst, dstSize, "no-gGT");
		return;
	}

	struct GameTracker *gGT = sdata->gGT;

	for (int playerIndex = 0; playerIndex < 4; playerIndex++)
	{
		struct PushBuffer *pb = &gGT->pushBuffer[playerIndex];
		const uintptr_t start = (uintptr_t)pb->ptrOT;
		const uintptr_t end = (uintptr_t)pb->renderBucketOTRangeEnd;
		if ((start != 0) && (end != 0) && (ptr >= start) && (ptr <= end))
		{
			snprintf(dst, dstSize, "pb%d.ot+0x%zx", playerIndex, (size_t)(ptr - start));
			return;
		}
	}

	struct PushBuffer *uiPB = &gGT->pushBuffer_UI;
	const uintptr_t uiStart = (uintptr_t)uiPB->ptrOT;
	const uintptr_t uiEnd = (uintptr_t)uiPB->renderBucketOTRangeEnd;
	if ((uiStart != 0) && (uiEnd != 0) && (ptr >= uiStart) && (ptr <= uiEnd))
	{
		snprintf(dst, dstSize, "ui.ot+0x%zx", (size_t)(ptr - uiStart));
		return;
	}

	for (int dbIndex = 0; dbIndex < 2; dbIndex++)
	{
		struct DB *db = &gGT->db[dbIndex];
		const uintptr_t primStart = (uintptr_t)db->primMem.start;
		const uintptr_t primEnd = (uintptr_t)db->primMem.end;
		if ((primStart != 0) && (ptr >= primStart) && (ptr < primEnd))
		{
			snprintf(dst, dstSize, "db%d.prim+0x%zx", dbIndex, (size_t)(ptr - primStart));
			return;
		}

		const uintptr_t otStart = (uintptr_t)db->otMem.start;
		const uintptr_t otEnd = (uintptr_t)db->otMem.end;
		if ((otStart != 0) && (ptr >= otStart) && (ptr < otEnd))
		{
			snprintf(dst, dstSize, "db%d.ot+0x%zx", dbIndex, (size_t)(ptr - otStart));
			return;
		}
	}

	snprintf(dst, dstSize, "unknown");
}

#ifdef CTR_NATIVE_OTWALK_PROFILE
/*
 * Links-only replica of the walk below: tag read, terminator test, link
 * validity, nothing else. Runs FIRST, so the real walk that follows sees a
 * warmed cache -- draw_otag_drywalk_ms is an upper bound on chase cost, not
 * an exact split. Mirrors the real walk's exits (terminator, invalid link,
 * 65536-iteration cycle cap) so frameDryNodes should equal frameNodes;
 * drymis in the [OTPROF] line counts frames where it didn't.
 */
internal void OTProf_DryWalk(u8 *basePacket)
{
	NativePerf_BeginScope(NATIVE_PERF_BUCKET_DRAW_OTAG_DRYWALK);

	u32 iterations = 0;
	while (true)
	{
		iterations++;
		if (iterations > 65536)
		{
			break;
		}

		(void)getlen(basePacket);

		if (isendprim(basePacket))
		{
			break;
		}

		u8 *nextPacket = nextPrim(basePacket);
		if (!NativeGpu_IsValidOTLink((uintptr_t)nextPacket))
		{
			break;
		}
		basePacket = nextPacket;
	}
	s_otprof.frameDryNodes += iterations;

	NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_OTAG_DRYWALK);
}
#endif /* CTR_NATIVE_OTWALK_PROFILE */

void ParsePrimitivesLinkedList(u32 *p, int singlePrimitive)
{
	if (!p)
	{
		return;
	}

	NativePerf_BeginScope(NATIVE_PERF_BUCKET_DRAW_OTAG_PARSE);

#ifdef CTR_NATIVE
	if (!singlePrimitive && !NativeGpuLinks_IsRegisteredHostPointer(p) && !isendprim(p))
	{
		char packetRegion[64];
		NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)p);
		NATIVE_GPU_ERROR("unregistered linked DrawOTag packet: packet=%p region=%s addr=%06x len=%d code=%02x words=%08x %08x %08x %08x\n", (void *)p,
		                 packetRegion, getaddr(p), getlen(p), getcode(p), NativeGpu_ReadPacketWordForLog((uintptr_t)p, 0),
		                 NativeGpu_ReadPacketWordForLog((uintptr_t)p, 1), NativeGpu_ReadPacketWordForLog((uintptr_t)p, 2),
		                 NativeGpu_ReadPacketWordForLog((uintptr_t)p, 3));
		NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_OTAG_PARSE);
		return;
	}
#endif

	// setup single primitive flag (needed for AddSplits)
	s_gpu.drawPrimMode = singlePrimitive;

	if (singlePrimitive)
	{
		P_TAG *polyTag = (P_TAG *)p;
		const int firstVertex = s_gpu.vertexIndex;
		ParsePrimitive(polyTag);
		NativeGpu_StampEmitOrdinal(firstVertex);

		GPUDrawSplit *lastSplit = &s_gpu.splits[s_gpu.splitIndex];
		lastSplit->numVerts = s_gpu.vertexIndex - lastSplit->startVertex;
	}
	else
	{
		// walk OT_TAG linked list
		u8 *basePacket = (u8 *)p;

#ifdef CTR_NATIVE_OTWALK_PROFILE
		OTProf_DryWalk(basePacket);
#endif

#if defined(CTR_NATIVE) && CTR_NATIVE_VERTEX_DESNAP
		GTECapture_BeginOTWalk();
#endif

		/*
		 * DIAGNOSTIC (2026-07-20, session 10): this loop's only exits are the
		 * terminator tag (isendprim) and the invalid-link check below -- both
		 * assume the OT is a simple forward chain. Neither catches a genuine
		 * CYCLE: a chain of otherwise-legitimate, individually-registered
		 * pointers that loops back on itself instead of ever reaching the
		 * terminator. Every node in such a cycle passes NativeGpu_IsValidOTLink
		 * (it really is a registered pointer), so that check can never trip,
		 * and the walk spins forever with no log output -- a silent hang.
		 * Real OTs here are at most a few thousand entries (RenderSubmit uses
		 * ptrOT[0x3ff]; MainFrame.c sizes the swapchain OT at
		 * numPlyrCurrGame<<10 | 6, i.e. ~1030-4102 for 1-4 players), so a
		 * 65536-iteration cap is far above any legitimate walk and costs
		 * nothing to check (a few cycles per node) -- but it turns an
		 * unrecoverable freeze into a diagnosable one.
		 */
		u32 otWalkIterations = 0;

		while (true)
		{
			otWalkIterations++;
			if (otWalkIterations > 65536)
			{
				char packetRegion[64];
				char startRegion[64];

				NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)basePacket);
				NativeGpu_FormatPointerRegion(startRegion, sizeof(startRegion), (uintptr_t)p);
				NATIVE_GPU_ERROR(
				    "OT WALK DID NOT TERMINATE after %lu iterations -- likely a cycle. start=%p startRegion=%s "
				    "current=%p currentRegion=%s addr=%06x len=%d code=%02x words=%08x %08x %08x %08x\n",
				    (unsigned long)otWalkIterations, (void *)p, startRegion, (void *)basePacket, packetRegion, getaddr(basePacket), getlen(basePacket),
				    getcode(basePacket), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 0), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 1),
				    NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 2), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 3));
				Platform_Log("[CTR GPU] OT WALK CYCLE after %lu iters, current=%p addr=%06x len=%d code=%02x\n", (unsigned long)otWalkIterations,
				             (void *)basePacket, getaddr(basePacket), getlen(basePacket), getcode(basePacket));
				break;
			}

			const int tagLength = getlen(basePacket);

#ifdef CTR_NATIVE_OTWALK_PROFILE
			OTProf_RecordNode(basePacket, (u32)tagLength);
			OTProf_RecordNodeShape(basePacket, (u32)tagLength);
#endif

#if defined(CTR_NATIVE) && CTR_NATIVE_VERTEX_DESNAP
			GTECapture_MarkOTNode(basePacket);
#endif
			if (tagLength > 0)
			{
				if (tagLength > 32)
				{
					char packetRegion[64];
					NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)basePacket);
					NATIVE_GPU_ERROR("got invalid tag length %d, code %d packet=%p region=%s words=%08x %08x %08x %08x\n", tagLength,
					                 ((P_TAG *)basePacket)->code, (void *)basePacket, packetRegion, NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 0),
					                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 1), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 2),
					                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 3));
					break;
				}

#if defined(CTR_NATIVE) && CTR_NATIVE_DIRECT_EMIT
				// Fast path: an all-opaque-poly node goes packet -> merge bin
				// directly (3DS; PC builds only count and always parse). A
				// handled node emitted its triangles and advanced the emit
				// ordinals itself, so the parse loop must not see it. See
				// native_direct_emit.c.
				if (NativeDirectEmit_TryNode(basePacket, (u32)tagLength))
				{
					// handled: nothing to parse
				}
				else
#endif
				{
					u8 *currentPacket = basePacket;
					u8 *endPacket = basePacket + (tagLength + P_LEN) * sizeof(u32);
					int primLength = 0;
					int firstVertex = s_gpu.vertexIndex;
					if (currentPacket < endPacket)
					{
						primLength = ParsePrimitive((P_TAG *)currentPacket);
						currentPacket += (primLength + P_LEN) * sizeof(u32);
						NativeGpu_StampEmitOrdinal(firstVertex);
					}

					while (currentPacket < endPacket)
					{
						firstVertex = s_gpu.vertexIndex;
						primLength = ParseTaglessPrimitive((u32 *)currentPacket);
						currentPacket += primLength * sizeof(u32);
						NativeGpu_StampEmitOrdinal(firstVertex);
					}

					if (currentPacket != endPacket)
					{
						char packetRegion[64];
						NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)basePacket);
						NATIVE_GPU_ERROR("did not output valid primitive or ptag length is not valid (diff=%d packet=%p region=%s words=%08x %08x %08x %08x)\n",
						                 endPacket - currentPacket, (void *)basePacket, packetRegion, NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 0),
						                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 1), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 2),
						                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 3));
					}
				}
			}

			GPUDrawSplit *lastSplit = &s_gpu.splits[s_gpu.splitIndex];
			lastSplit->numVerts = s_gpu.vertexIndex - lastSplit->startVertex;

			if (isendprim(basePacket))
			{
				break;
			}

			u8 *nextPacket = nextPrim(basePacket);
			if (!NativeGpu_IsValidOTLink((uintptr_t)nextPacket))
			{
				char packetRegion[64];
				char nextRegion[64];
				NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)basePacket);
				NativeGpu_FormatPointerRegion(nextRegion, sizeof(nextRegion), (uintptr_t)nextPacket);
				NATIVE_GPU_ERROR("invalid OT link: packet=%p region=%s addr=%06x next=%p nextRegion=%s len=%d code=%02x words=%08x %08x %08x %08x\n",
				                 (void *)basePacket, packetRegion, getaddr(basePacket), (void *)nextPacket, nextRegion, getlen(basePacket), getcode(basePacket),
				                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 0), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 1),
				                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 2), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 3));
				break;
			}

			basePacket = nextPacket;
		}
	}

	NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_OTAG_PARSE);
}

internal inline int IsNull(POLY_FT3 *poly)
{
	return poly->x0 == -1 && poly->y0 == -1 && poly->x1 == -1 && poly->y1 == -1 && poly->x2 == -1 && poly->y2 == -1;
}

internal int ProcessFlatLines(P_TAG *polyTag)
{
	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		LINE_F2 *poly = (LINE_F2 *)polyTag;

		AddSplit(semiTrans, false, false);

		VERTTYPE *p0 = &poly->x0;
		VERTTYPE *p1 = &poly->x1;
		u8 *c0 = &poly->r0;
		u8 *c1 = c0;

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		LineSwapSourceVerts(&p0, &p1, &c0, &c1);
		MakeLineArray(firstVertex, p0, p1);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 3;
	}
	case 0x8: // TODO (unused)
	{
		LINE_F3 *poly = (LINE_F3 *)polyTag;

		AddSplit(semiTrans, false, false);

		{
			VERTTYPE *p0 = &poly->x0;
			VERTTYPE *p1 = &poly->x1;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		{
			VERTTYPE *p0 = &poly->x1;
			VERTTYPE *p1 = &poly->x2;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		return 5;
	}
	case 0xc:
	{
		LINE_F4 *poly = (LINE_F4 *)polyTag;

		AddSplit(semiTrans, false, false);

		{
			VERTTYPE *p0 = &poly->x0;
			VERTTYPE *p1 = &poly->x1;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		{
			VERTTYPE *p0 = &poly->x1;
			VERTTYPE *p1 = &poly->x2;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		{
			VERTTYPE *p0 = &poly->x2;
			VERTTYPE *p1 = &poly->x3;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		return 6;
	}
	}
	return 0;
}

internal int ProcessGouraudLines(P_TAG *polyTag)
{
	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		LINE_G2 *poly = (LINE_G2 *)polyTag;

		AddSplit(semiTrans, false, false);

		VERTTYPE *p0 = &poly->x0;
		VERTTYPE *p1 = &poly->x1;
		u8 *c0 = &poly->r0;
		u8 *c1 = &poly->r1;

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		LineSwapSourceVerts(&p0, &p1, &c0, &c1);
		MakeLineArray(firstVertex, p0, p1);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 4;
	}
	case 0x8:
	{
		// TODO: LINE_G3
		return 7;
	}
	case 0xC:
	{
		// TODO: LINE_G4
		return 9;
	}
	}
	return 0;
}

internal int ProcessFlatPoly(P_TAG *polyTag)
{
	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		POLY_F3 *poly = (POLY_F3 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2);
		MakeTexcoordTriangleZero(firstVertex, 0);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0);

		s_gpu.vertexIndex += 3;

		return 4;
	}
	case 0x4:
	{
		POLY_FT3 *poly = (POLY_FT3 *)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		// It is an official hack from SCE devs to not use DR_TPAGE and instead use null polygon
		if (!IsNull(poly))
		{
			AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(poly->tpage));

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2);
			MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut,
			                     GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
			MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0);

			s_gpu.vertexIndex += 3;
		}
		return 7;
	}
	case 0x8:
	{
		POLY_F4 *poly = (POLY_F4 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;
		return 5;
	}
	case 0xC:
	{
		POLY_FT4 *poly = (POLY_FT4 *)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(poly->tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2);
		MakeTexcoordQuad(firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage, poly->clut,
		                 GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 9;
	}
	}
	return 0;
}

internal int ProcessGouraudPoly(P_TAG *polyTag)
{
	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		POLY_G3 *poly = (POLY_G3 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2);
		MakeTexcoordTriangleZero(firstVertex, 1);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);

		s_gpu.vertexIndex += 3;

		return 6;
	}
	case 0x4:
	{
		POLY_GT3 *poly = (POLY_GT3 *)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(poly->tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2);
		MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);

		s_gpu.vertexIndex += 3;

		return 9;
	}
	case 0x8:
	{
		POLY_G4 *poly = (POLY_G4 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2);
		MakeTexcoordQuadZero(firstVertex, 1);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3, &poly->r2);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 8;
	}
	case 0xC:
	{
		POLY_GT4 *poly = (POLY_GT4 *)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(poly->tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2);
		MakeTexcoordQuad(firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage, poly->clut,
		                 GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3, &poly->r2);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 12;
	}
	}
	return 0;
}

internal int ProcessTileAndSprt(P_TAG *polyTag)
{
	// NOTE: TILE does not support switching shadeTex on real PSX
	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans = (polyTag->code & 2);

	switch (polyTag->code & 0xFD)
	{
	case 0x60:
	{
		TILE *poly = (TILE *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 3;
	}
	case 0x64:
	{
		SPRT *poly = (SPRT *)polyTag;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(activeDrawEnv.tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, poly->w, poly->h);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 4;
	}
	case 0x68:
	{
		TILE_1 *poly = (TILE_1 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 1, 1);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 2;
	}
	case 0x70:
	{
		TILE_8 *poly = (TILE_8 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 8, 8);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 2;
	}
	case 0x74:
	{
		SPRT_8 *poly = (SPRT_8 *)polyTag;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(activeDrawEnv.tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 8, 8);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, 8, 8);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 3;
	}
	case 0x78:
	{
		TILE_16 *poly = (TILE_16 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 16, 16);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 2;
	}
	case 0x7C:
	{
		SPRT_16 *poly = (SPRT_16 *)polyTag;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(activeDrawEnv.tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 16, 16);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, 16, 16);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 3;
	}
	}
	return 0;
}

internal int ProcessDrawEnv(P_TAG *polyTag)
{
	const u32 *codePtr = (u32 *)&polyTag->pad0;
	int processedLongs = 0;
	bool fullDrawEnvPacket = false;
	for (int i = 0; i < polyTag->len; ++i)
	{
		const u32 code = codePtr[i];
		const int primType = code >> 24 & 0xF0;
		const int primSubType = code >> 24 & 0x0F;

		// NOTE(aalhendi): CTR can pack draw-env commands, tagless geometry,
		// and more draw-env commands into one OT entry. Stop at the first
		// non-E command so ParseTaglessPrimitive owns the geometry payload.
		if (primType != 0xE0)
		{
			return processedLongs;
		}

		switch (primSubType)
		{
		case 0x1:
		{
			// DR_TPAGE
			activeDrawEnv.tpage = (code & 0x1FF);
			activeDrawEnv.dtd = (code >> 9) & 1;
			// NOTE(aalhendi): Standalone DR_TPAGE packets use the same E1 word
			// for blend changes; only full DRAWENV packets retarget native
			// on-screen/offscreen rendering.
			if (fullDrawEnvPacket)
			{
				activeDrawEnv.dfe = (code >> 10) & 1;
			}
			break;
		}
		case 0x2:
		{
			// DR_TWIN
			activeDrawEnv.tw.w = (code & 0x1F);
			activeDrawEnv.tw.h = ((code >> 5) & 0x1F);
			activeDrawEnv.tw.x = ((code >> 10) & 0x1F);
			activeDrawEnv.tw.y = ((code >> 15) & 0x1F);
			break;
		}
		case 0x3:
		{
			// DR_AREA
			activeDrawEnv.clip.x = code & 1023;
			activeDrawEnv.clip.y = (code >> 10) & 1023;
			fullDrawEnvPacket = true;
			break;
		}
		case 0x4:
		{
			// DR_AREA (second part)
			activeDrawEnv.clip.w = code & 1023;
			activeDrawEnv.clip.h = (code >> 10) & 1023;

			activeDrawEnv.clip.w = activeDrawEnv.clip.w - activeDrawEnv.clip.x + 1;
			activeDrawEnv.clip.h = activeDrawEnv.clip.h - activeDrawEnv.clip.y + 1;
			fullDrawEnvPacket = true;
			break;
		}
		case 0x5:
		{
			// DR_OFFSET
			activeDrawEnv.ofs[0] = NativeGpu_SignExtend11(code);
			activeDrawEnv.ofs[1] = NativeGpu_SignExtend11(code >> 11);
			fullDrawEnvPacket = true;
			break;
		}
		case 0x6:
		{
			SetPSXMaskState(code);
			break;
		}
		case 0:
			// NOTE(aalhendi): ctr-native local divergence for CTR OTs. A zero
			// word can be terminal draw-env padding, or the tag word for the
			// next primitive packed into the same OT entry.
			// return processedLongs;
			if (i + 1 != polyTag->len)
			{
				return processedLongs;
			}
			break;
		}
		++processedLongs;
	}

	return processedLongs;
}

internal void ProcessDrawEnvCommand(u32 code)
{
	const int primSubType = code >> 24 & 0x0F;

	switch (primSubType)
	{
	case 0x1:
		activeDrawEnv.tpage = (code & 0x1FF);
		activeDrawEnv.dtd = (code >> 9) & 1;
		break;
	case 0x2:
		activeDrawEnv.tw.w = (code & 0x1F);
		activeDrawEnv.tw.h = ((code >> 5) & 0x1F);
		activeDrawEnv.tw.x = ((code >> 10) & 0x1F);
		activeDrawEnv.tw.y = ((code >> 15) & 0x1F);
		break;
	case 0x3:
		activeDrawEnv.clip.x = code & 1023;
		activeDrawEnv.clip.y = (code >> 10) & 1023;
		break;
	case 0x4:
		activeDrawEnv.clip.w = code & 1023;
		activeDrawEnv.clip.h = (code >> 10) & 1023;
		activeDrawEnv.clip.w = activeDrawEnv.clip.w - activeDrawEnv.clip.x + 1;
		activeDrawEnv.clip.h = activeDrawEnv.clip.h - activeDrawEnv.clip.y + 1;
		break;
	case 0x5:
		activeDrawEnv.ofs[0] = NativeGpu_SignExtend11(code);
		activeDrawEnv.ofs[1] = NativeGpu_SignExtend11(code >> 11);
		break;
	case 0x6:
		SetPSXMaskState(code);
		break;
	}
}

internal int ProcessPsyXPrims(P_TAG *polyTag)
{
	const int primSubType = polyTag->code & 0x0F;

	switch (primSubType)
	{
	case 0x01:
	{
		DR_PSYX_TEX *psytex = (DR_PSYX_TEX *)polyTag;
		s_gpu.overrideTexture = psytex->code[0] & 0xFFFFFF;
		s_gpu.overrideTextureWidth = psytex->code[1] & 0xFFF;
		s_gpu.overrideTextureHeight = psytex->code[1] >> 16 & 0xFFF;
		return 2;
	}
	case 0x02:
	{
		// [A] Psy-X custom debug marker packet
		DR_PSYX_DBGMARKER *psydbg = (DR_PSYX_DBGMARKER *)polyTag;
		s_gpu.currentSplitDebugText = psydbg->text;
		return 2;
	}
	}

	return 0;
}

// Processes primitive
// returns processed primitive primLength in longs
int ParsePrimitive(P_TAG *polyTag)
{
	const int primType = polyTag->code & 0xF0;

	int primLength = 0;
	bool handledZeroLength = false;

	switch (primType)
	{
	case 0x00:
	{
		const int primSubType = polyTag->code & 0x0F;
		const u32 *codePtr = (u32 *)&polyTag->pad0;
		// NOTE(aalhendi): ctr-native local divergence. CTR RenderWeather can
		// emit a retail length-2 zero packet when weather is enabled but the
		// level has no fill-mode payload. The PSX consumes it by tag length;
		// the native parser must advance past it too.
		if (polyTag->len == 2 && codePtr[0] == 0 && codePtr[1] == 0)
		{
			primLength = 2;
		}
		else if (polyTag->len == 0 && *(u32 *)polyTag == 0)
		{
			// CTR ghost transparency packets include raw GPU NOP words between
			// draw-mode changes and triangle commands. They consume exactly one
			// command word; ParsePrimitivesLinkedList adds P_LEN to the return.
			handledZeroLength = true;
		}
		else if (primSubType == 0x0)
		{
			primLength = 3;
		}
		else if (primSubType == 0x1)
		{
			DR_MOVE *drmove = (DR_MOVE *)polyTag;
			const u32 rectPos = drmove->code[2];
			const u32 rectSize = drmove->code[4];

			const int y = drmove->code[3] >> 0x10 & 0xFFFF;
			const int x = drmove->code[3] & 0xFFFF;

			RECT16 rect;
			rect.x = (s16)(rectPos & 0xffff);
			rect.y = (s16)(rectPos >> 16);
			rect.w = (s16)(rectSize & 0xffff);
			rect.h = (s16)(rectSize >> 16);

			if (NativeGpu_HasPendingSplits())
			{
				DrawAllSplits();
			}
			MoveImage(&rect, x, y);
			primLength = 5;
		}
		else if (primSubType == 0x2)
		{
			// NOTE(aalhendi): ctr-native local divergence. CTR emits retail
			// FILL packets in OTs; the old PsyCross parser did not consume them, which caused
			// zero-length primitive spam.
			TILE *fill = (TILE *)polyTag;
			RECT16 rect;

			rect.x = fill->x0;
			rect.y = fill->y0;
			rect.w = fill->w;
			rect.h = fill->h;

			if (NativeGpu_HasPendingSplits())
			{
				DrawAllSplits();
			}
			ClearImage(&rect, fill->r0, fill->g0, fill->b0);
			primLength = 3;
		}
		break;
	}
	case 0x20:
		// Flat polygons
		primLength = ProcessFlatPoly(polyTag);
		break;
	case 0x30:
		// Gouraud shaded polygons
		primLength = ProcessGouraudPoly(polyTag);
		break;
	case 0x40:
		// Flat (single colour) Lines
		primLength = ProcessFlatLines(polyTag);
		break;
	case 0x50:
		// Gouraud lines
		primLength = ProcessGouraudLines(polyTag);
		break;
	case 0x60:
	case 0x70:
		// TILE and SPRT
		primLength = ProcessTileAndSprt(polyTag);
		break;
	case 0xA0:
		// DR_LOAD
		{
			DR_LOAD *drload = (DR_LOAD *)polyTag;
			const u32 rectPos = drload->code[1];
			const u32 rectSize = drload->code[2];

			RECT16 rect;
			rect.x = (s16)(rectPos & 0xffff);
			rect.y = (s16)(rectPos >> 16);
			rect.w = (s16)(rectSize & 0xffff);
			rect.h = (s16)(rectSize >> 16);

			LoadImage(&rect, (uint32_t *)drload->p);

			// TODO(aalhendi): Audit whether CTR ever appends additional GPU
			// commands after a DR_LOAD payload in the same packet.
		}
		primLength = getlen(polyTag);
		break;
	case 0xB0:
		// [A] Psy-X custom primitives
		primLength = ProcessPsyXPrims(polyTag);
		break;
	case 0xE0:
		// Draw Env setup
		primLength = ProcessDrawEnv(polyTag);
		break;
		// default:
		//	NATIVE_GPU_ERROR("got %0x primitive\n", primType);
	}

	if (primLength == 0 && !handledZeroLength)
	{
		NATIVE_GPU_ERROR("Unhandled zero length %0x primitive\n", primType);
	}

	return primLength;
}

int ParseTaglessPrimitive(u32 *command)
{
	const u32 code = *command;
	const int primType = (code >> 24) & 0xF0;

	if (code == 0)
	{
		return 1;
	}

	if (primType == 0xE0)
	{
		ProcessDrawEnvCommand(code);
		return 1;
	}

	P_TAG *polyTag = (P_TAG *)(command - P_LEN);
	int primLength = ParsePrimitive(polyTag);

	if (primLength == 0)
	{
		NATIVE_GPU_ERROR("Unhandled tagless primitive %08x\n", code);
		return 1;
	}

	return primLength;
}
