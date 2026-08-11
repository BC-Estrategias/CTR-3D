/*
 * dfe=0 offscreen-split capture -- see include/platform/native_offscreen_capture.h
 * for the format and purpose (see docs/OFFSCREEN_RENDER_VRAM_DESIGN.md).
 * PC only: the 3DS is the platform whose missing compositing this data exists
 * to build and verify, so it has nothing to capture.
 */

#include "platform/native_offscreen_capture.h"

#if defined(CTR_INTERNAL) && !defined(__3DS__)

#define OFSCAP_MAGIC          0x474F5443u /* 'CTOG' little-endian */
/* v2: GrVertex grew emitOrdinal (depth substrate), so raw vertex records are
 * 22 bytes now. The header always recorded sizeof(GrVertex), so readers can
 * self-adjust; the bump is so a v1 capture is rejected rather than misread.
 * v3: GrVertex grew parallaxOrdinal (occlusion/parallax depth split, see
 * docs/FLAG_SPLIT_CHANNEL_SPEC.md) -- 24 bytes. Same bump for the same
 * reason: a v2 reader would walk this file's vertex array off by 2 bytes
 * per record, which is silent corruption rather than an error. */
#define OFSCAP_VERSION        3u
#define OFSCAP_TAG_BEGIN      1u
#define OFSCAP_TAG_SPLIT      2u
#define OFSCAP_TAG_END        3u
#define OFSCAP_MAX_RECTS      32
#define OFSCAP_PER_RECT_CAP   6
#define OFSCAP_DETAIL_LOG_CAP OFSCAP_PER_RECT_CAP

typedef char OfscapGrVertexSizeCheck[(sizeof(GrVertex) == 24) ? 1 : -1];

typedef struct
{
	s16 x, y, w, h;
	int sessions;      /* lifetime sessions with this clip rect */
	int captured;      /* of those, how many went into the golden file */
	int splits;        /* lifetime splits */
	int tris;          /* lifetime triangles */
} OfscapRectStat;

global_variable FILE *s_ofscapFile = NULL;
global_variable int s_ofscapMaxSessions = 24;
global_variable int s_ofscapCapturedSessions = 0;
global_variable u32 s_ofscapVblank = 0;

global_variable OfscapRectStat s_ofscapRects[OFSCAP_MAX_RECTS];
global_variable int s_ofscapRectCount = 0;
global_variable int s_ofscapRectOverflowSessions = 0;

/* Current session (one offscreen accumulation, OnBegin..OnFlush). */
global_variable int s_ofscapSessionActive = 0;
global_variable int s_ofscapSessionCaptured = 0;
global_variable int s_ofscapSessionLogged = 0;
global_variable int s_ofscapSessionRectSlot = -1;
global_variable RECT16 s_ofscapSessionRect;

/* Per-vblank cadence counters, reported and reset in Tick. */
global_variable int s_ofscapVblankSessions = 0;
global_variable int s_ofscapVblankSplits = 0;
global_variable int s_ofscapVblankTris = 0;

/* Lifetime totals. */
global_variable int s_ofscapTotalSessions = 0;
global_variable int s_ofscapTotalSplits = 0;
global_variable int s_ofscapTotalTris = 0;

global_variable u16 s_ofscapVramSnapshot[VRAM_WIDTH * VRAM_HEIGHT];
global_variable u16 s_ofscapRectPixels[VRAM_WIDTH * VRAM_HEIGHT];

internal void Ofscap_WriteU32(u32 value)
{
	fwrite(&value, sizeof(value), 1, s_ofscapFile);
}

internal void Ofscap_WriteS32(s32 value)
{
	fwrite(&value, sizeof(value), 1, s_ofscapFile);
}

internal int Ofscap_RectSlot(const RECT16 *rect)
{
	for (int i = 0; i < s_ofscapRectCount; i++)
	{
		if ((s_ofscapRects[i].x == rect->x) && (s_ofscapRects[i].y == rect->y) && (s_ofscapRects[i].w == rect->w) && (s_ofscapRects[i].h == rect->h))
		{
			return i;
		}
	}

	if (s_ofscapRectCount >= OFSCAP_MAX_RECTS)
	{
		return -1;
	}

	OfscapRectStat *slot = &s_ofscapRects[s_ofscapRectCount];
	slot->x = rect->x;
	slot->y = rect->y;
	slot->w = rect->w;
	slot->h = rect->h;
	slot->sessions = 0;
	slot->captured = 0;
	slot->splits = 0;
	slot->tris = 0;
	return s_ofscapRectCount++;
}

/* --vram-trace state: attribution of mirror writers (see header). */
global_variable int s_vtraceEnabled = 0;
global_variable int s_vtraceLines = 0;
#define VTRACE_LINE_CAP 20000

void NativeVramTrace_Op(const char *op, int x, int y, int w, int h, int srcX, int srcY)
{
	if (!s_vtraceEnabled)
	{
		return;
	}

	if (s_vtraceLines >= VTRACE_LINE_CAP)
	{
		if (s_vtraceLines == VTRACE_LINE_CAP)
		{
			s_vtraceLines++;
			Platform_Log("[VTRACE] line cap reached, muting\n");
		}
		return;
	}
	s_vtraceLines++;

	if (srcX >= 0)
	{
		Platform_Log("[VTRACE] vblank=%u op=%s dst=(%d,%d %dx%d) src=(%d,%d)\n", s_ofscapVblank, op, x, y, w, h, srcX, srcY);
	}
	else
	{
		Platform_Log("[VTRACE] vblank=%u op=%s rect=(%d,%d %dx%d)\n", s_ofscapVblank, op, x, y, w, h);
	}
}

/* --tri-dump state (see header): one frame of per-triangle records. */
global_variable FILE *s_triDumpFile = NULL;
global_variable u32 s_triDumpVblank = 0;
global_variable int s_triDumpArmed = 0;
global_variable int s_triDumpCount = 0;

internal void TriDump_PutS16(u8 *dst, int v)
{
	dst[0] = (u8)(v & 0xFF);
	dst[1] = (u8)((v >> 8) & 0xFF);
}

void NativeOffscreenCapture_OnAnySplit(const GrVertex *verts, int numVerts, int texFormat, int textured)
{
	/* A 4-vblank window, not equality: the game renders at 30fps, so any
	 * single vblank can be the non-render half of the pair and see zero
	 * splits. */
	if (!s_triDumpArmed || (s_triDumpFile == NULL) || (s_ofscapVblank < s_triDumpVblank) || (s_ofscapVblank >= s_triDumpVblank + 4))
	{
		return;
	}

	for (int i = 0; (i + 2) < numVerts; i += 3)
	{
		const GrVertex *t = &verts[i];
		u8 rec[36];

		TriDump_PutS16(&rec[0], (u16)t[0].page);
		TriDump_PutS16(&rec[2], (u16)t[0].clut);
		rec[4] = textured ? (u8)texFormat : 255;
		rec[5] = (u8)(textured ? 1 : 0);
		for (int v = 0; v < 3; v++)
		{
			rec[6 + v] = t[v].u;
			rec[9 + v] = t[v].v;
		}
		for (int v = 0; v < 3; v++)
		{
			TriDump_PutS16(&rec[12 + v * 2], t[v].x);
			TriDump_PutS16(&rec[18 + v * 2], t[v].y);
		}
		TriDump_PutS16(&rec[24], -1);
		TriDump_PutS16(&rec[26], -1);
		TriDump_PutS16(&rec[28], -1);
		TriDump_PutS16(&rec[30], -1);
		TriDump_PutS16(&rec[32], -1);
		rec[34] = 0;
		rec[35] = 0;
		fwrite(rec, sizeof(rec), 1, s_triDumpFile);
		s_triDumpCount++;
	}
}

int NativeOffscreenCapture_ConfigureFromArgs(int argc, char **argv)
{
	const char *path = NULL;
	const char *maxSessions = NULL;
	const char *triPath = NULL;
	const char *triVblank = NULL;

	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--vram-trace") == 0)
		{
			s_vtraceEnabled = 1;
			Platform_Log("[VTRACE] tracing VRAM mirror writes\n");
		}
	}

	for (int i = 1; (i + 1) < argc; i++)
	{
		if (strcmp(argv[i], "--tri-dump") == 0)
		{
			triPath = argv[i + 1];
		}
		else if (strcmp(argv[i], "--tri-dump-vblank") == 0)
		{
			triVblank = argv[i + 1];
		}
	}

	if (triPath != NULL)
	{
		s_triDumpFile = fopen(triPath, "wb");
		if (s_triDumpFile != NULL)
		{
			s_triDumpVblank = (u32)((triVblank != NULL) ? atoi(triVblank) : 4000);
			s_triDumpArmed = 1;
			Platform_Log("[TRIDUMP] will dump triangles of vblank %u to %s\n", s_triDumpVblank, triPath);
		}
		else
		{
			Platform_LogWarn("[TRIDUMP] FAILED to open %s\n", triPath);
		}
	}

	for (int i = 1; (i + 1) < argc; i++)
	{
		if (strcmp(argv[i], "--offscreen-capture") == 0)
		{
			path = argv[i + 1];
		}
		else if (strcmp(argv[i], "--offscreen-capture-max") == 0)
		{
			maxSessions = argv[i + 1];
		}
	}

	if (path == NULL)
	{
		return 0;
	}

	s_ofscapFile = fopen(path, "wb");
	if (s_ofscapFile == NULL)
	{
		Platform_LogWarn("[OFSCAP] FAILED to open %s\n", path);
		return 0;
	}

	if (maxSessions != NULL)
	{
		s_ofscapMaxSessions = atoi(maxSessions);
		if (s_ofscapMaxSessions < 1)
		{
			s_ofscapMaxSessions = 1;
		}
	}

	Ofscap_WriteU32(OFSCAP_MAGIC);
	Ofscap_WriteU32(OFSCAP_VERSION);
	Ofscap_WriteU32((u32)VRAM_WIDTH);
	Ofscap_WriteU32((u32)VRAM_HEIGHT);
	Ofscap_WriteU32((u32)sizeof(GrVertex));
	fflush(s_ofscapFile);

	Platform_Log("[OFSCAP] capturing offscreen splits to %s (max %d sessions with pixels)\n", path, s_ofscapMaxSessions);
	return 0;
}

void NativeOffscreenCapture_Tick(void)
{
	if (s_triDumpArmed && (s_ofscapVblank >= (s_triDumpVblank + 4)) && (s_triDumpFile != NULL))
	{
		fclose(s_triDumpFile);
		s_triDumpFile = NULL;
		s_triDumpArmed = 0;
		Platform_Log("[TRIDUMP] dumped %d triangles\n", s_triDumpCount);
	}

	if (s_ofscapFile == NULL)
	{
		/* The vblank counter doubles as --vram-trace's timebase. */
		s_ofscapVblank++;
		return;
	}

	if (s_ofscapVblankSessions > 0)
	{
		Platform_Log("[OFSCAP] vblank=%u sessions=%d splits=%d tris=%d\n", s_ofscapVblank, s_ofscapVblankSessions, s_ofscapVblankSplits, s_ofscapVblankTris);
		s_ofscapVblankSessions = 0;
		s_ofscapVblankSplits = 0;
		s_ofscapVblankTris = 0;
	}

	s_ofscapVblank++;

	if ((s_ofscapVblank % 1024) == 0 && s_ofscapTotalSessions > 0)
	{
		Platform_Log("[OFSCAP] summary vblank=%u: sessions=%d splits=%d tris=%d captured=%d rects=%d (+%d overflow sessions)\n", s_ofscapVblank,
		             s_ofscapTotalSessions, s_ofscapTotalSplits, s_ofscapTotalTris, s_ofscapCapturedSessions, s_ofscapRectCount, s_ofscapRectOverflowSessions);
		for (int i = 0; i < s_ofscapRectCount; i++)
		{
			const OfscapRectStat *r = &s_ofscapRects[i];
			Platform_Log("[OFSCAP]   rect (%d,%d %dx%d): sessions=%d captured=%d splits=%d tris=%d\n", r->x, r->y, r->w, r->h, r->sessions, r->captured,
			             r->splits, r->tris);
		}
	}
}

void NativeOffscreenCapture_OnBegin(const RECT16 *rect)
{
	if (s_ofscapFile == NULL)
	{
		return;
	}

	if (s_ofscapSessionActive)
	{
		/* SetOffscreenState's contract flushes before re-enabling with a new
		 * rect, so OnFlush always lands first. Seeing this means the hook
		 * wiring regressed -- say so rather than corrupt the record stream. */
		Platform_LogWarn("[OFSCAP] OnBegin while session active (hook order bug)\n");
	}

	s_ofscapSessionActive = 1;
	s_ofscapSessionRect = *rect;
	s_ofscapSessionRectSlot = Ofscap_RectSlot(rect);
	s_ofscapTotalSessions++;
	s_ofscapVblankSessions++;

	int rectSessionCount;
	if (s_ofscapSessionRectSlot >= 0)
	{
		OfscapRectStat *slot = &s_ofscapRects[s_ofscapSessionRectSlot];
		slot->sessions++;
		rectSessionCount = slot->sessions;
	}
	else
	{
		s_ofscapRectOverflowSessions++;
		rectSessionCount = OFSCAP_PER_RECT_CAP + 1; /* overflow rects never capture pixels */
	}

	/* Detailed per-split log lines follow the same first-N-per-rect rule as
	 * pixel capture; afterwards only cadence/summary lines, or a 90-second
	 * run drowns the log. */
	s_ofscapSessionLogged = (rectSessionCount <= OFSCAP_DETAIL_LOG_CAP);

	s_ofscapSessionCaptured = (s_ofscapCapturedSessions < s_ofscapMaxSessions) && (rectSessionCount <= OFSCAP_PER_RECT_CAP);
	if (s_ofscapSessionCaptured)
	{
		s_ofscapCapturedSessions++;
		if (s_ofscapSessionRectSlot >= 0)
		{
			s_ofscapRects[s_ofscapSessionRectSlot].captured++;
		}

		NativeRenderer_ReadVRAM(s_ofscapVramSnapshot, 0, 0, VRAM_WIDTH, VRAM_HEIGHT);
		Ofscap_WriteU32(OFSCAP_TAG_BEGIN);
		Ofscap_WriteU32(s_ofscapVblank);
		Ofscap_WriteS32(rect->x);
		Ofscap_WriteS32(rect->y);
		Ofscap_WriteS32(rect->w);
		Ofscap_WriteS32(rect->h);
		fwrite(s_ofscapVramSnapshot, sizeof(u16), (size_t)VRAM_WIDTH * VRAM_HEIGHT, s_ofscapFile);
		fflush(s_ofscapFile);
	}

	if (s_ofscapSessionLogged)
	{
		Platform_Log("[OFSCAP] BEGIN vblank=%u clip=(%d,%d %dx%d)%s\n", s_ofscapVblank, rect->x, rect->y, rect->w, rect->h,
		             s_ofscapSessionCaptured ? " [golden]" : "");
	}
}

void NativeOffscreenCapture_OnSplit(const GrVertex *verts, int numVerts, int blendMode, int texFormat, int textured, int semiTrans, int stpOut, int maskSet,
                                    int drawPrimMode, int dtd, const RECT16 *clip)
{
	if ((s_ofscapFile == NULL) || (numVerts <= 0))
	{
		return;
	}

	if (!s_ofscapSessionActive)
	{
		Platform_LogWarn("[OFSCAP] split outside session, clip=(%d,%d %dx%d)\n", clip->x, clip->y, clip->w, clip->h);
		return;
	}

	const int tris = numVerts / 3;
	s_ofscapTotalSplits++;
	s_ofscapTotalTris += tris;
	s_ofscapVblankSplits++;
	s_ofscapVblankTris += tris;
	if (s_ofscapSessionRectSlot >= 0)
	{
		s_ofscapRects[s_ofscapSessionRectSlot].splits++;
		s_ofscapRects[s_ofscapSessionRectSlot].tris += tris;
	}

	if (s_ofscapSessionLogged)
	{
		/* Distinct (page,clut) pairs and flags actually present in the vertex
		 * stream -- the raster's feature matrix, measured not assumed. */
		s16 pages[6];
		s16 cluts[6];
		int pairCount = 0;
		int gouraud = 0;
		int minX = verts[0].x, maxX = verts[0].x, minY = verts[0].y, maxY = verts[0].y;

		for (int i = 0; i < numVerts; i++)
		{
			const GrVertex *v = &verts[i];
			int seen = 0;

			for (int j = 0; j < pairCount; j++)
			{
				if ((pages[j] == v->page) && (cluts[j] == v->clut))
				{
					seen = 1;
					break;
				}
			}
			if (!seen && (pairCount < 6))
			{
				pages[pairCount] = v->page;
				cluts[pairCount] = v->clut;
				pairCount++;
			}

			if (v->x < minX) { minX = v->x; }
			if (v->x > maxX) { maxX = v->x; }
			if (v->y < minY) { minY = v->y; }
			if (v->y > maxY) { maxY = v->y; }
		}

		for (int i = 0; (i + 2) < numVerts; i += 3)
		{
			const GrVertex *a = &verts[i];
			const GrVertex *b = &verts[i + 1];
			const GrVertex *c = &verts[i + 2];
			if ((a->r != b->r) || (a->g != b->g) || (a->b != b->b) || (a->r != c->r) || (a->g != c->g) || (a->b != c->b))
			{
				gouraud = 1;
				break;
			}
		}

		char pairText[96];
		int pairLen = 0;
		pairText[0] = '\0';
		for (int i = 0; i < pairCount; i++)
		{
			pairLen += snprintf(pairText + pairLen, sizeof(pairText) - (size_t)pairLen, "%s%04x/%04x", (i > 0) ? " " : "", (u16)pages[i], (u16)cluts[i]);
			if (pairLen >= (int)sizeof(pairText) - 1)
			{
				break;
			}
		}

		Platform_Log("[OFSCAP] split vblank=%u tris=%d blend=%d fmt=%d tex=%d semi=%d stpOut=%d mask=%d prim=%d dtd=%d gouraud=%d xy=(%d..%d,%d..%d) page/clut{%s}\n",
		             s_ofscapVblank, tris, blendMode, texFormat, textured, semiTrans, stpOut, maskSet, drawPrimMode, dtd, gouraud, minX, maxX, minY, maxY,
		             pairText);
	}

	if (s_ofscapSessionCaptured)
	{
		Ofscap_WriteU32(OFSCAP_TAG_SPLIT);
		Ofscap_WriteU32(s_ofscapVblank);
		Ofscap_WriteS32(blendMode);
		Ofscap_WriteS32(texFormat);
		Ofscap_WriteS32(textured);
		Ofscap_WriteS32(semiTrans);
		Ofscap_WriteS32(stpOut);
		Ofscap_WriteS32(maskSet);
		Ofscap_WriteS32(drawPrimMode);
		Ofscap_WriteS32(dtd);
		Ofscap_WriteS32(numVerts);
		fwrite(verts, sizeof(GrVertex), (size_t)numVerts, s_ofscapFile);
		fflush(s_ofscapFile);
	}
}

void NativeOffscreenCapture_OnFlush(const RECT16 *rect)
{
	if (s_ofscapFile == NULL)
	{
		return;
	}

	if (!s_ofscapSessionActive)
	{
		return;
	}

	s_ofscapSessionActive = 0;

	if (s_ofscapSessionCaptured)
	{
		const int w = rect->w;
		const int h = rect->h;

		if ((w > 0) && (h > 0) && (w <= VRAM_WIDTH) && (h <= VRAM_HEIGHT))
		{
			NativeRenderer_ReadVRAM(s_ofscapRectPixels, rect->x, rect->y, w, h);
			Ofscap_WriteU32(OFSCAP_TAG_END);
			Ofscap_WriteU32(s_ofscapVblank);
			Ofscap_WriteS32(rect->x);
			Ofscap_WriteS32(rect->y);
			Ofscap_WriteS32(w);
			Ofscap_WriteS32(h);
			fwrite(s_ofscapRectPixels, sizeof(u16), (size_t)w * (size_t)h, s_ofscapFile);
			fflush(s_ofscapFile);
		}
	}

	if (s_ofscapSessionLogged)
	{
		Platform_Log("[OFSCAP] END vblank=%u clip=(%d,%d %dx%d)\n", s_ofscapVblank, rect->x, rect->y, rect->w, rect->h);
	}

	s_ofscapSessionCaptured = 0;
	s_ofscapSessionLogged = 0;
	s_ofscapSessionRectSlot = -1;
}

#else /* !CTR_INTERNAL || __3DS__ */

void NativeVramTrace_Op(const char *op, int x, int y, int w, int h, int srcX, int srcY)
{
	(void)op;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)srcX;
	(void)srcY;
}

void NativeOffscreenCapture_OnAnySplit(const GrVertex *verts, int numVerts, int texFormat, int textured)
{
	(void)verts;
	(void)numVerts;
	(void)texFormat;
	(void)textured;
}

int NativeOffscreenCapture_ConfigureFromArgs(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return 0;
}

void NativeOffscreenCapture_Tick(void)
{
}

void NativeOffscreenCapture_OnBegin(const RECT16 *rect)
{
	(void)rect;
}

void NativeOffscreenCapture_OnFlush(const RECT16 *rect)
{
	(void)rect;
}

void NativeOffscreenCapture_OnSplit(const GrVertex *verts, int numVerts, int blendMode, int texFormat, int textured, int semiTrans, int stpOut, int maskSet,
                                    int drawPrimMode, int dtd, const RECT16 *clip)
{
	(void)verts;
	(void)numVerts;
	(void)blendMode;
	(void)texFormat;
	(void)textured;
	(void)semiTrans;
	(void)stpOut;
	(void)maskSet;
	(void)drawPrimMode;
	(void)dtd;
	(void)clip;
}

#endif
