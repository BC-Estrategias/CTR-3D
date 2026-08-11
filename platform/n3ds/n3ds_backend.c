/*
 * libctru side of the backend boundary. This file is its own translation unit
 * and must never include game headers -- see n3ds_backend.h for why.
 */

#include <3ds.h>
#include <citro3d.h>
#include <malloc.h>
#include <netinet/in.h> /* full struct in_addr for __3dslink_host (3dslink.h only forward-declares it) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* fsync -- the SD log's timed durability commit */

#include "n3ds_backend.h"
#include "ctr_shbin.h"
#ifdef CTR_NATIVE_TRACK_RENDERER
/* Rung 3a step 2's sibling shader, built by the exact same picasso -> bin2s
 * pipeline as ctr_shbin.h above (CMakeLists.txt, guarded by the same
 * CTR_NATIVE_TRACK_RENDERER option) -- see platform/n3ds/shaders/ctr_track.v.pica. */
#include "ctr_track_shbin.h"
#endif
#ifdef CTR_NATIVE_MODEL_RENDERER
/* Rung 2's model shader, same picasso -> bin2s pipeline, guarded by the same
 * CTR_NATIVE_MODEL_RENDERER option -- see platform/n3ds/shaders/ctr_model.v.pica. */
#include "ctr_model_shbin.h"
#endif

/*
 * Diagnostics go to two places because neither alone is sufficient:
 *   - svcOutputDebugString, which Luma surfaces on hardware, but which Azahar's
 *     release build compiles out (its LOG_DEBUG is stripped).
 *   - an absolute-path file on the SD card, which survives on both and can be
 *     read back from the host without a screenshot. The path is absolute
 *     because the working directory is not dependable this early.
 */
#define N3DS_LOG_PATH "sdmc:/ctr3ds.log"
#define N3DS_LOG_PREV_PATH "sdmc:/ctr3ds.prev.log"

static FILE *s_logFile = NULL;

/*
 * 3dslink netload stderr streaming: when sent over Wi-Fi via 3dslink, HBL
 * records the sender's IP in __3dslink_host; connect a TCP stream back to it
 * (3dslink must run with -s) and point stderr at it so every log line
 * mirrors to the PC live. stdout stays the bottom-screen console; SD-card /
 * Azahar launches have s_addr == 0 and skip this.
 */
#define N3DS_SOC_ALIGN       0x1000
#define N3DS_SOC_BUFFER_SIZE 0x100000

static u32 *s_socBuffer = NULL;
static int s_netloadStreamActive = 0;

int N3DS_NetloadStreamActive(void)
{
	return s_netloadStreamActive;
}

static void N3DS_NetloadStreamExit(void)
{
	if (s_socBuffer != NULL)
	{
		socExit();
	}
}

static void N3DS_NetloadStreamInit(void)
{
	if (__3dslink_host.s_addr == 0)
	{
		return;
	}

	s_socBuffer = (u32 *)memalign(N3DS_SOC_ALIGN, N3DS_SOC_BUFFER_SIZE);
	if (s_socBuffer == NULL)
	{
		return;
	}

	if (R_FAILED(socInit(s_socBuffer, N3DS_SOC_BUFFER_SIZE)))
	{
		free(s_socBuffer);
		s_socBuffer = NULL;
		return;
	}

	atexit(N3DS_NetloadStreamExit);

	/* stderr only: stdout must stay the bottom-screen console. If the host
	 * isn't listening (3dslink without -s) this fails fast and boot proceeds
	 * with the socket service up but no stream -- harmless. */
	if (link3dsConnectToHost(false, true) < 0)
	{
		return;
	}

	/* Batch the stream: newlib's stderr is unbuffered, so every log line is a
	 * synchronous soc:u send, and each send is a chance for a transient WiFi
	 * failure to kill the whole stream. Buffer + the shared 2s timed flush in
	 * N3DS_DebugLog turns that into one send per window batch; must precede
	 * the first stderr write, hence before the active flag flips (every
	 * stream write is gated on it). svcOutputDebugString stays the per-line
	 * instant trail for crash forensics. */
	setvbuf(stderr, NULL, _IOFBF, 8192);

	s_netloadStreamActive = 1;
}

void N3DS_EarlyInit(void)
{
	consoleDebugInit(debugDevice_SVC);

	/* One-deep rotation BEFORE the truncating open: "w" wipes the previous
	 * boot's log, so a crash -> relaunch sequence would otherwise truncate
	 * the crashed run's log before it can be read. Keeps exactly the last
	 * two boots, never grows unbounded. */
	remove(N3DS_LOG_PREV_PATH);
	rename(N3DS_LOG_PATH, N3DS_LOG_PREV_PATH);

	s_logFile = fopen(N3DS_LOG_PATH, "w");
	if (s_logFile != NULL)
	{
		/* 16KB stdio buffer: at the stats block's ~2-3KB/s the default ~1KB
		 * buffer would auto-flush (an SD stall) mid-window anyway, defeating
		 * the timed flush in N3DS_DebugLog. */
		setvbuf(s_logFile, NULL, _IOFBF, 16384);
	}

	N3DS_NetloadStreamInit();
	if (s_netloadStreamActive)
	{
		N3DS_DebugLog("[CTR-3DS] netload stderr stream connected");
	}
}

/* Last SD-log flush, in system ticks. Per-LINE fflush stalls gameplay
 * visibly on hardware: each flush pays FAT + card latency (tens of ms), and
 * the stats window emits ~15 lines back-to-back, so every window freezes the
 * frame for a beat. Invisible in Azahar, whose sdmc is a host folder with
 * free flushes. */
static u64 s_logLastFlushTick = 0;

/*
 * SD-card logging master switch, default off: the 3DS SD stack is slow at
 * file work, and N3DS_DebugLog's throttled fflush+fsync runs on the game
 * thread every ~2s, costing one stalled frame per window -- enough to erase
 * the margin to a solid 30fps.
 *
 * Off still leaves svcOutputDebugString per line (free with no debugger
 * attached, and Azahar still captures it) and the whole bottom-screen
 * readout, which is CPU-only. Turn back on when a log is actually wanted --
 * SELECT + TOUCH on hardware, dumpreq `sdlog`.
 */
int g_dbg_sdLog = 0;

/* [LOGFLUSH] accumulators -- see the timing block inside N3DS_DebugLog. */
static unsigned s_logFlushWorstUs = 0;
static unsigned s_logFlushTotalUs = 0;
static unsigned s_logFlushCount = 0;

void N3DS_DebugLog(const char *msg)
{
	if (msg == NULL)
	{
		return;
	}

	svcOutputDebugString(msg, (int)strlen(msg));

	if (s_netloadStreamActive)
	{
		fputs(msg, stderr);
		fputc('\n', stderr);
	}

	/* g_dbg_sdLog off: no fputs either, not just no flush. A buffered write
	 * is only free until the 16KB buffer fills, and then stdio flushes it
	 * wherever it happens to be -- mid-corner, unthrottled, exactly the stall
	 * this switch exists to remove. */
	if ((s_logFile != NULL) && g_dbg_sdLog)
	{
		fputs(msg, s_logFile);
		fputc('\n', s_logFile);
	}

	/* Flush BOTH buffered sinks at most every ~2s (SD file and the netload
	 * stderr stream -- see the setvbuf in N3DS_NetloadStreamInit). Both are
	 * post-hoc trails that tolerate a bounded tail loss on a hard crash;
	 * svcOutputDebugString above is the per-line instant trail. */
	if (((s_logFile != NULL) && g_dbg_sdLog) || s_netloadStreamActive)
	{
		const u64 now = svcGetSystemTick();

		if ((now - s_logLastFlushTick) > (SYSCLOCK_ARM11 * 2ull))
		{
			/* Time the flush itself. Every 64-frame window in the Tubes log
			 * carries exactly ONE frame of 76-176ms, and this 2s-throttled SD
			 * write is the only thing in the build whose cadence matches. One
			 * hitch per window costs ~1.8ms of the window AVERAGE, which is
			 * the whole remaining gap to a solid 30 -- so if this is it,
			 * every perf number we have is carrying our own instrument on
			 * its back. Reported as [LOGFLUSH]. */
			const u64 flushStart = svcGetSystemTick();

			if ((s_logFile != NULL) && g_dbg_sdLog)
			{
				fflush(s_logFile);
				/* fflush alone moves bytes into the FS session; the FAT
				 * size/allocation metadata only commits on an fs-level
				 * flush. Without fsync, a hard freeze loses everything
				 * written after the first flush, since the file's
				 * metadata never committed. */
				fsync(fileno(s_logFile));
			}
			if (s_netloadStreamActive)
			{
				fflush(stderr);
			}
			s_logLastFlushTick = now;

			{
				const u64 took = svcGetSystemTick() - flushStart;
				const unsigned tookUs = (unsigned)((took * 1000000ull) / (u64)SYSCLOCK_ARM11);

				s_logFlushTotalUs += tookUs;
				if (tookUs > s_logFlushWorstUs)
				{
					s_logFlushWorstUs = tookUs;
				}
				s_logFlushCount++;
			}
		}
	}
}

void N3DS_LogFlushStats(unsigned *worstUs, unsigned *totalUs, unsigned *count)
{
	*worstUs = s_logFlushWorstUs;
	*totalUs = s_logFlushTotalUs;
	*count = s_logFlushCount;
	s_logFlushWorstUs = 0;
	s_logFlushTotalUs = 0;
	s_logFlushCount = 0;
}

/*
 * SELECT+R VRAM capture (CTR_INTERNAL, texture-corruption diagnostic). The
 * renderer TU owns vram[] but can't do the SELECT+R button
 * check (libctru's hid* live only in this TU); this TU can't see vram[]. So
 * the renderer registers the buffer pointer once and the button check +
 * fwrite happen here, in N3DS_ReadButtons, where hidScanInput() has already
 * run this frame.
 */
static const void *s_dumpVramPtr = NULL;
static unsigned int s_dumpVramBytes = 0;
static const unsigned int *s_dumpFrameCounterPtr = NULL;

void N3DS_DebugSetVRAMForDump(const void *vram, unsigned int bytes, const unsigned int *frameCounter)
{
	s_dumpVramPtr = vram;
	s_dumpVramBytes = bytes;
	s_dumpFrameCounterPtr = frameCounter;
}

#ifdef CTR_INTERNAL
/* Frame number stamped into every capture filename. Fixed names already cost
 * one race capture (overwritten by a later menu capture); the
 * shared stamp also groups the vramdump/atlas/cachemeta files of one capture
 * instant into a self-identifying set. */
static unsigned int N3DS_DebugCaptureFrame(void)
{
	return (s_dumpFrameCounterPtr != NULL) ? *s_dumpFrameCounterPtr : 0u;
}
#endif

#ifdef CTR_INTERNAL
static void N3DS_DebugWriteVRAMDump(void)
{
	FILE *f;
	char path[64];
	char msg[96];

	if ((s_dumpVramPtr == NULL) || (s_dumpVramBytes == 0))
	{
		N3DS_DebugLog("[CTR-3DS] VRAM dump skipped: buffer not registered");
		return;
	}

	snprintf(path, sizeof(path), "sdmc:/vramdump_%u.bin", N3DS_DebugCaptureFrame());
	f = fopen(path, "wb");
	if (f == NULL)
	{
		snprintf(msg, sizeof(msg), "[CTR-3DS] VRAM dump FAILED: fopen %s", path);
		N3DS_DebugLog(msg);
		return;
	}

	fwrite(s_dumpVramPtr, 1, s_dumpVramBytes, f);
	fclose(f);

	snprintf(msg, sizeof(msg), "[CTR-3DS] VRAM dumped to %s (%u bytes)", path, s_dumpVramBytes);
	N3DS_DebugLog(msg);
}
#endif

unsigned long long N3DS_GetTickCount(void)
{
	return (unsigned long long)svcGetSystemTick();
}

unsigned long long N3DS_GetTickFrequency(void)
{
	return (unsigned long long)SYSCLOCK_ARM11;
}

void N3DS_SleepMs(unsigned int ms)
{
	svcSleepThread((s64)ms * 1000000LL);
}

void N3DS_GfxInit(void)
{
	/*
	 * New3DS 804MHz clock + the extra L2 cache. Nothing in this build asked
	 * for them before: the only osSetSpeedupEnable in the tree is inside
	 * SDL's 3DS main shim, and the 3DS build does not link SDL at all. The
	 * owner's CFW (Luma/Rosalina) forces both on regardless, so this is not
	 * expected to move the frame rate on THIS console -- it is here so the
	 * build does not silently depend on someone else's CFW configuration.
	 * Harmless on an Old3DS, where libctru makes it a no-op.
	 */
	osSetSpeedupEnable(true);

	gfxInitDefault();

	/*
	 * Stereoscopic 3D: double-buffer the top screen's RIGHT framebuffer too.
	 * Costs ~576KB of linear heap up front; with the physical slider at zero
	 * the parallax barrier is off, the right buffer is never presented, and
	 * the render path draws one eye exactly as before (N3DS_StereoLatchFrame).
	 * Called here, before any framebuffer is fetched, because gfxSet3D
	 * reallocates the top screen's buffers.
	 */
	gfxSet3D(true);

	/*
	 * The debug/stats console lives on the bottom screen now that citro3d
	 * (N3DS_C3DInit, below) owns the top screen's render target. The two
	 * coexist because they drive different physical screens through
	 * different presentation paths -- see N3DS_GfxPresent.
	 */
	consoleInit(GFX_BOTTOM, NULL);
}

void N3DS_GfxExit(void)
{
	gfxExit();
}

/*
 * 1 = flush only the BOTTOM framebuffer; 0 = the old whole-system
 * gfxFlushBuffers().
 *
 * gfxFlushBuffers cleans the data cache for top-left, top-right AND bottom
 * (~806KB/frame with gfxSet3D on) via svcFlushProcessDataCache. libctru's own
 * header says it "is only used during software rendering" and "has
 * significant overhead" -- and our only software rendering is the bottom
 * console, which redraws once every 64 frames. The two top framebuffers are
 * written by the GPU (C3D_FrameEnd's display transfer), never by the CPU, so
 * cleaning their cache lines is ~576KB of pure waste every frame.
 *
 * The one thing that ever READ them on the CPU is the pause freeze-frame
 * (N3DS_CopyDisplayedTopFrame); it does its own invalidate, which is where
 * that belongs. Default 1, hardware-measured: present 3.70 -> 1.67ms, and
 * ~0.8ms of that reached the frame (Tubes median 36.1 -> 35.3ms). Free CPU
 * with no visual or behavioural change. dumpreq `fastpresent`.
 */
int g_dbg_fastPresent = 1;

void N3DS_GfxPresent(void)
{
	/*
	 * Only the bottom screen's console framebuffer goes through this path.
	 * The generic gfxSwapBuffers() swaps every screen's buffers, which would
	 * fight citro3d for ownership of the top screen -- C3D_FrameEnd (see
	 * N3DS_C3DEndFrame) already presents that one.
	 */
	if (g_dbg_fastPresent)
	{
		u16 fbW = 0, fbH = 0;
		u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbW, &fbH);

		/* The buffer this returns is the one the console just drew into and
		 * the one the swap below is about to present -- flush exactly it.
		 * Unconditional rather than dirty-tracked: console writes come from
		 * ~90 printf sites (boot banners, the 64-frame stats block, the atlas
		 * inspector), and a missed flush would silently blank text that is
		 * the only readout hardware has. Bottom-only is already the bulk of
		 * the win; skipping clean frames too can come later, with a hook that
		 * can't miss a writer. */
		if (fb != NULL)
		{
			GSPGPU_FlushDataCache(fb, (u32)fbW * (u32)fbH * 3U);
		}
	}
	else
	{
		gfxFlushBuffers();
	}

	gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

/*
 * Copies the top screen's last completed gfx framebuffer into dstRGB as
 * row-major RGB triplets, nearest-resampled from the pillarboxed CONTENT
 * region (N3DS_TOP_CONTENT_*, header) to dstW x dstH
 * (docs/PAUSE_FREEZE_FRAME_DESIGN.md). The source costs nothing
 * to produce: citro3d's C3D_FrameEnd already display-transfers every frame
 * into the linear-heap gfx framebuffers (GSP_BGR8_OES via gfxInitDefault;
 * the transfer's OUT_FORMAT is RGB8, which is what fills that byte layout),
 * so this is a plain CPU read of memory the present path maintains anyway --
 * no GX call, no GPU sync.
 *
 * gfxGetFramebuffer returns the BACK buffer (the one drawn into next), whose
 * content is the frame presented one swap before the current front -- so the
 * copy can be one presentation old. Acceptable by design for the freeze-
 * frame use: the consumer is a paused/transitioning scene.
 *
 * Rotation: the 3DS top framebuffer is stored rotated -- fbW(=240) is the
 * panel's SHORT axis and consecutive bytes walk UP a screen column, so the
 * pixel at screen (sx, sy), y-down, lives at ((sx * 240) + (239 - sy)) * 3,
 * bytes B,G,R.
 */
int N3DS_CopyDisplayedTopFrame(unsigned char *dstRGB, int dstW, int dstH)
{
	u16 fbW = 0, fbH = 0;
	u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbW, &fbH);
	int x, y;

	if ((fb == NULL) || (dstRGB == NULL) || (dstW <= 0) || (dstH <= 0))
	{
		return 0;
	}

	/* This buffer is written by the GPU (C3D_FrameEnd's display transfer) and
	 * read here by the CPU, so the CPU's cached view of it may be stale. This
	 * used to be covered by accident -- N3DS_GfxPresent's whole-system
	 * gfxFlushBuffers cleaned and invalidated all three framebuffers every
	 * frame. g_dbg_fastPresent stops paying that 3.3ms, so the one reader that
	 * actually needed it asks for it directly, on pause entry rather than
	 * 30 times a second. */
	GSPGPU_InvalidateDataCache(fb, (u32)fbW * (u32)fbH * 3U);

	for (y = 0; y < dstH; y++)
	{
		const int sy = (y * 240) / dstH;
		unsigned char *dstRow = dstRGB + ((size_t)y * (size_t)dstW * 3);

		for (x = 0; x < dstW; x++)
		{
			/* Sample the pillarboxed content region only (N3DS_TOP_CONTENT_*):
			 * the full-panel form re-ingested the black side bars, so the
			 * pause background showed them squeezed inside the picture
			 * (user-reported on hardware). */
			const int sx = N3DS_TOP_CONTENT_X + ((x * N3DS_TOP_CONTENT_W) / dstW);
			const u8 *p = fb + (((sx * 240) + (239 - sy)) * 3);

			dstRow[(x * 3) + 0] = p[2]; /* R */
			dstRow[(x * 3) + 1] = p[1]; /* G */
			dstRow[(x * 3) + 2] = p[0]; /* B */
		}
	}

	return 1;
}

/* ------------------------------------------------------------- render --- */

#define N3DS_DISPLAY_TRANSFER_FLAGS                                                                         \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                        \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |          \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static DVLB_s *s_shaderDvlb = NULL;
static shaderProgram_s s_shaderProgram;
static int s_uLocProjection = -1;
static int s_uLocStereo = -1;
static int s_uLocStereoUi = -1;
/*
 * UI pop-out amount, as a percent of this eye's at-infinity shift k --
 * explicitly not at the screen plane, and less than the character-select
 * rotating character.
 *
 * A percent of k rather than an absolute pixel count tracks the 3D slider for
 * free and is identically 0 when the slider is down -- mono safety by
 * construction, the same property the flag depth/parallax split relies on.
 *
 * Runtime-tunable (dumpreq 'uipop N') because this is a comfort value judged
 * by eye, on hardware.
 */
int g_dbg_uiPopPct = 25;
static float s_stereoVecUi[2][4];
static C3D_RenderTarget *s_renderTarget = NULL;

static C3D_Mtx s_projection;
static void *s_vertexBufferMem = NULL;
static int s_vertexCursor = 0;
static int s_vertexHighWater = 0;
static C3D_Tex s_atlasTextures[N3DS_ATLAS_COUNT];
static C3D_Tex s_vramDisplayTex[N3DS_VRAM_DISPLAY_TEX_COUNT];

/* Native track renderer's dedicated atlas pool (rung 2b) -- see the header
 * comment on N3DS_TrackAtlas_Alloc. N3DS_TRACK_ATLAS_DIM/MAX_UNITS live in
 * n3ds_backend.h, not here -- native_track_mesh.c's
 * packer checks its own constants against those exact macros at compile
 * time. Sized to the pool's own max unit count, NOT allocated up front;
 * s_trackAtlasUnitsAllocated tracks how many of these are actually live
 * textures right now (0 until the first level load calls
 * N3DS_TrackAtlas_Alloc). */
static C3D_Tex s_trackAtlasTextures[N3DS_TRACK_ATLAS_MAX_UNITS];
static int s_trackAtlasUnitsAllocated = 0;

#ifdef CTR_NATIVE_MODEL_RENDERER
/*
 * Rung 2 model program (docs/NATIVE_MODEL_RENDERER_DESIGN.md). Only the program
 * and its uniform locations live here -- no attribute layout and no buffer
 * info, because model draws deliberately reuse the packet path's. See
 * n3ds_backend.h's N3DS_ModelDraw_* comment for why that is safe and what it
 * costs in field reinterpretation.
 *
 * Kept OUTSIDE the CTR_NATIVE_TRACK_RENDERER block below on purpose: the two
 * options are independent, and a model-renderer build with the track renderer
 * off is a configuration this campaign will actually want (it isolates the
 * model path's fps contribution from the track's).
 */
static DVLB_s *s_modelShaderDvlb = NULL;
static shaderProgram_s s_modelShaderProgram;
static int s_modelULocQx = -1, s_modelULocQy = -1, s_modelULocQw = -1;
static int s_modelULocProjection = -1, s_modelULocH = -1, s_modelULocStereo = -1;
/* Texenv memo, same idea and lifetime rule as s_trackTexturedState: reset by
 * N3DS_ModelDraw_Begin because the packet path owns texenv 0 between model
 * frames. -1 = unknown, so the first Run after a Begin always emits. */
static int s_modelTexturedState = -1;
/* Last atlas a model Run bound. Only trusted between Begin and End -- the
 * packet path binds atlases too -- so Begin resets it. */
static int s_modelBoundAtlas = -1;
/* Depth-write memo for the model blend pass, same contract as the track's
 * s_trackDepthWriteState: -1 = not asserted in this bracket. */
static int s_modelDepthWriteState = -1;
static int s_modelDrawActive = 0;
#endif

#ifdef CTR_NATIVE_TRACK_RENDERER
/*
 * Rung 3a step 2: the resident-mesh draw path's own program/attribute
 * layout/vertex+index buffer -- a second, independent set of the SAME kind
 * of state N3DS_C3DInit builds for the packet path above, since track draws
 * use a completely different vertex format (model-space N3DS_TrackVertex,
 * not screen-space N3DS_Vertex) and shader (ctr_track.v.pica's perspective
 * projection, not ctr.v.pica's orthographic passthrough).
 */
static DVLB_s *s_trackShaderDvlb = NULL;
static shaderProgram_s s_trackShaderProgram;
static int s_trackULocQx = -1, s_trackULocQy = -1, s_trackULocQw = -1;
static int s_trackULocProjection = -1, s_trackULocH = -1, s_trackULocDepthRange = -1;

/*
 * The track's drawOrder tie-break weight, in view.z units per drawOrder step,
 * uploaded as hParam.w (docs/GROUND_DECAL_PASS_DESIGN.md 3/4). Default 1.0:
 * an ablation to 0 (removing the term) was tested and the ground decals
 * stayed buried, so the drawOrder term is not the burial mechanism. The knob
 * remains as the cheapest way to size any future decal-side offset.
 * File-scope rather than a shared g_dbg_* because both readers (this uniform
 * write and the dumpreq token) live in this TU, which is its own TU so the
 * unity-build tentative-definition collision hazard does not apply here.
 */
static float s_trackDrawOrderWeight = 1.0f;
static int s_trackULocUvOffset = -1, s_trackULocStereo = -1;
/* Memo for the blend pass's depth-write state, so a frame with no semi-
 * transparent content pays nothing and a frame with some flips the pair exactly
 * twice. -1 = unknown/not set this bracket, forcing the first draw to assert. */
static int s_trackDepthWriteState = -1;
/* Rung 3b texenv memo -- see N3DS_TrackDraw_SetTextured. Declared up here with
 * the other track statics because N3DS_TrackDraw_Begin resets it and sits above
 * the function that uses it. */
static int s_trackTexturedState = -1;
static C3D_AttrInfo s_trackAttrInfo;
static C3D_BufInfo s_trackBufInfo;

/* One linearAlloc block per level (N3DS_TrackGeom_Upload's header comment):
 * verts at offset 0, indices at an 8-byte-aligned offset after them.
 * s_trackGeomReady is false until a successful upload, and N3DS_TrackDraw_Begin
 * checks it so a level whose upload failed (or hasn't happened yet, e.g. the
 * toggle flipped on before any level finished loading) draws nothing instead
 * of binding a stale/absent buffer. */
static void *s_trackGeomMem = NULL;
static const void *s_trackIndexPtr = NULL;
static int s_trackGeomReady = 0;
/* Recorded by the upload so N3DS_TrackGeom_PatchUV can bound-check a vertex
 * index against the block it is actually writing into, rather than against a
 * count the caller believes is current. The staging array the upload copied
 * from is freed immediately, so this block is the ONLY surviving copy of the
 * resident geometry -- a bad index here corrupts the level's mesh with nothing
 * to restore it from short of a full rebuild. */
static int s_trackGeomVertCount = 0;
static int s_trackGeomVertStride = 0;
/* Set by Begin only when geometry is actually ready; End always clears it
 * and always restores packet state regardless (cheap, and foolproof against
 * any Begin/Leaf/End call-order mistake upstream). */
static int s_trackDrawActive = 0;
#endif /* CTR_NATIVE_TRACK_RENDERER */

/* ------------------------------------------------------------- stereo 3D --- */
/*
 * See n3ds_backend.h's stereo block for the design. State here, in the order
 * it is used: master toggle and tuning (dumpreq `stereo`/`stereosep`/
 * `stereoconv`), the per-frame latch, the per-eye parallax vectors, and the
 * draw journal the right eye replays.
 *
 * The parallax model is p = k * (1 - convergence/depth) in PSX screen pixels,
 * on the shared view.z axis all three geometry sources sit on. Content AT the
 * convergence depth has zero disparity (screen plane), farther
 * content goes into the screen (up to k px per eye at infinity), nearer
 * content pops out (clamped, see N3DS_StereoEyeVec). The physical slider
 * scales k continuously, so the slider IS the runtime control -- the dumpreq
 * tokens only tune the endpoints.
 *
 * Sign derivation (not a guess): an object behind the screen plane, viewed
 * from the right eye at +e/2, projects onto the screen at +x -- far content
 * shifts RIGHT in the right eye. p = k*(1 - C/d) with k > 0 for the right
 * eye gives exactly that. If hardware disagrees, `stereosep -12` is the A/B.
 */
int g_dbg_stereo = 1; /* master enable; the slider still gates frame by frame */
/*
 * WINDOW MODE: with pop-out allowed, the
 * clamp band turns the near ground into a constant-disparity sheet -- a
 * horizontal crease where graded road meets clamped road, which the owner
 * perceived as the ground "folding down" as it passes the kart. Window mode
 * forbids crossed disparity entirely: the clamp band's pop-out side becomes
 * 0, so the world starts AT the screen surface and only recedes. The flat
 * zone still exists but now coincides with the physical screen, which the
 * eye accepts as "the window frame" -- the standard comfortable-3DS
 * convention.
 *
 * Default OFF (owner ruling, same night he asked for it: "it's less cool").
 * Pop-out wins; the fold is the accepted price until a softer clamp knee
 * exists. dumpreq `stereowindow` turns the tame mode on.
 */
int g_dbg_stereoWindow = 0;
static C3D_RenderTarget *s_renderTargetRight = NULL;
static float s_stereoSep = 12.0f;    /* k*2 at full slider: total left/right disparity at infinite depth, PSX px */
static float s_stereoConv = 1200.0f; /* view.z units at screen depth (just beyond the player kart's ~256..2278 band) */
static int s_stereoFrameActive = 0;  /* latched once per frame in N3DS_C3DBeginFrame */
static int s_stereoRecording = 0;    /* journal writes enabled: active frame, and cleared for
                                      * the replay so re-entered functions cannot re-record */
static int s_stereoEye = 0; /* 0 = left (and mono), 1 = right */
static float s_stereoVecWorld[2][4]; /* [eye][k, k*conv, clampLo, clampHi] -- track/model programs */
static float s_stereoVecPkt[2][4];   /* packet program: zeroed unless packet depth stamping is live */

/*
 * The journal: one entry per draw-affecting call of the left-eye frame, in
 * call order. Replay re-invokes the same public functions (track/model), or
 * re-issues the resolved state + draw inline (packet/VRAM quads), against the
 * right target with the right eye's parallax uniform. Sized for the observed
 * shape of a frame (post-merge packet emits ~20-40, track leaf draws ~50-200,
 * model instances+runs ~100) with generous slack; overflow drops the tail of
 * the RIGHT eye only, loudly, never the left.
 */
enum
{
	N3DS_STEREO_OP_PKT = 0,
	N3DS_STEREO_OP_VRAM,
	N3DS_STEREO_OP_TRACK_BEGIN,
	N3DS_STEREO_OP_TRACK_LEAF,
	N3DS_STEREO_OP_TRACK_END,
	N3DS_STEREO_OP_MODEL_BEGIN,
	N3DS_STEREO_OP_MODEL_INSTANCE,
	N3DS_STEREO_OP_MODEL_RUN,
	N3DS_STEREO_OP_MODEL_END
};

typedef struct N3DS_StereoOp
{
	u8 kind;
	union
	{
		struct
		{
			u8 blend, doubling, depthWrite, atlas;
			s32 baseVertex, vertCount;
		} pkt; /* PKT; VRAM reuses atlas=slot, baseVertex */
		struct
		{
			float qx[4], qy[4], qw[4], h;
		} mtx; /* TRACK_BEGIN, MODEL_INSTANCE */
		struct
		{
			s32 firstIndex, indexCount;
			float otIndexLo, ordinalLo, ordinalSpan, invIndexSpan;
			s32 atlasBin, blend, uvDx, uvDy;
		} leaf; /* TRACK_LEAF */
		struct
		{
			s32 startVertex, triCount, atlas, textured, blend;
		} mrun; /* MODEL_RUN */
	} u;
} N3DS_StereoOp;

#define N3DS_STEREO_MAX_OPS 1536
static N3DS_StereoOp s_stereoOps[N3DS_STEREO_MAX_OPS];
static int s_stereoOpCount = 0;
static int s_stereoOverflow = 0;

static N3DS_StereoOp *N3DS_StereoRecord(int kind)
{
	N3DS_StereoOp *op;

	if (!s_stereoRecording)
	{
		return NULL;
	}
	if (s_stereoOpCount >= N3DS_STEREO_MAX_OPS)
	{
		s_stereoOverflow = 1;
		return NULL;
	}
	op = &s_stereoOps[s_stereoOpCount++];
	op->kind = (u8)kind;
	return op;
}

/* Whether packet depth stamps currently carry real view.z (packet-depth
 * model) rather than emit ordinals. Same compile/runtime pair
 * N3DS_TrackDraw_Begin keys its own axis selector off, deliberately: parallax
 * from an ordinal stamp would be depth-shaped noise, so ordinal-stamped
 * builds/frames get a zero packet vector (flat packets at screen depth)
 * while track/model content -- whose depth is computed, not read from a
 * stamp -- still gets real parallax. */
static int N3DS_StereoPacketDepthActive(void)
{
#if defined(CTR_NATIVE_GTE_CAPTURE) && CTR_NATIVE_GTE_CAPTURE
	extern int g_dbg_packetDepth;

	return (g_dbg_packetDepth != 0);
#else
	return 0;
#endif
}

/* Fills one eye's (k, k*conv, clampLo, clampHi). The clamp band is
 * asymmetric on purpose: pop-out (disparity crossed in front of the screen)
 * is allowed up to 2x the at-infinity shift, then stops -- unbounded pop-out
 * from very-near content (behind-eye track clamped to depth 256, exhaust in
 * the camera) is the classic eye-stabbing failure of layered stereo. */
static void N3DS_StereoEyeVec(float k, float *out4)
{
	const float absK = (k < 0.0f) ? -k : k;
	/* Window mode zeroes the crossed-disparity (pop-out) side of the clamp
	 * band; content nearer than the convergence depth pins to the screen
	 * plane instead of popping out and folding (see g_dbg_stereoWindow). */
	const float popOut = g_dbg_stereoWindow ? 0.0f : (2.0f * absK);

	out4[0] = k;
	out4[1] = k * s_stereoConv;
	if (k >= 0.0f)
	{
		out4[2] = -popOut; /* right eye: near content shifts left */
		out4[3] = absK;
	}
	else
	{
		out4[2] = -absK;
		out4[3] = popOut;
	}
}

/* Once per frame, from N3DS_C3DBeginFrame: read the physical slider, decide
 * whether this frame is stereo, reset the journal, compute both eyes'
 * vectors. Logs only on the active/inactive TRANSITION -- the slider is an
 * analog input and per-frame logging would grind the SD card. */
static void N3DS_StereoLatchFrame(void)
{
	static int s_wasActive = -1;
	float slider = 0.0f;

	s_stereoOpCount = 0;
	s_stereoOverflow = 0;
	s_stereoEye = 0;
	s_stereoFrameActive = 0;

	if (g_dbg_stereo && (s_renderTargetRight != NULL))
	{
		slider = osGet3DSliderState();
		if (slider > 0.01f)
		{
			s_stereoFrameActive = 1;
		}
	}
	s_stereoRecording = s_stereoFrameActive;

	{
		const float mag = s_stereoFrameActive ? (slider * s_stereoSep * 0.5f) : 0.0f;
		const int pktLive = s_stereoFrameActive && N3DS_StereoPacketDepthActive();
		int eye;

		for (eye = 0; eye < 2; eye++)
		{
			const float k = (eye == 1) ? mag : -mag;

			N3DS_StereoEyeVec(k, s_stereoVecWorld[eye]);
			N3DS_StereoEyeVec(pktLive ? k : 0.0f, s_stereoVecPkt[eye]);

			/*
			 * UI pop-out. NEGATIVE k: pop-out is CROSSED disparity, i.e. the
			 * opposite sign to the at-infinity shift (p = k - kC/d goes below k as
			 * content comes nearer, and reaches 0 at the convergence depth). One
			 * expression covers both eyes because k already carries the per-eye
			 * sign. Rides the packet vector's liveness so UI never pops out on a
			 * frame where packet depth is off and the band test is meaningless.
			 */
			s_stereoVecUi[eye][0] = (pktLive ? -k : 0.0f) * ((float)g_dbg_uiPopPct * 0.01f);
			s_stereoVecUi[eye][1] = 0.0f;
			s_stereoVecUi[eye][2] = 0.0f;
			s_stereoVecUi[eye][3] = 0.0f;
		}
	}

	if (s_stereoFrameActive != s_wasActive)
	{
		char msg[96];

		snprintf(msg, sizeof(msg), s_stereoFrameActive ? "[CTR-3DS] stereo: ACTIVE (slider=%d%% sep=%d conv=%d pktdepth=%d)" : "[CTR-3DS] stereo: inactive (slider down)",
		         (int)(slider * 100.0f), (int)s_stereoSep, (int)s_stereoConv, N3DS_StereoPacketDepthActive());
		N3DS_DebugLog(msg);
		s_wasActive = s_stereoFrameActive;
	}
}

/* Upload the current eye's parallax vector to one program's `stereo`
 * uniform. Guarded on the location like every other upload: -1 means the
 * lookup failed at init and writing would land on register 0. */
static void N3DS_StereoUploadUniform(int uLoc, const float (*table)[4])
{
	if (uLoc >= 0)
	{
		const float *v = table[s_stereoEye];

		C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc, v[0], v[1], v[2], v[3]);
	}
}

void N3DS_StereoRecordPacket(int blendMode, int doubling, int depthWrite, int atlas, int baseVertex, int vertCount)
{
	N3DS_StereoOp *op = N3DS_StereoRecord(N3DS_STEREO_OP_PKT);

	if (op != NULL)
	{
		op->u.pkt.blend = (u8)blendMode;
		op->u.pkt.doubling = (u8)doubling;
		op->u.pkt.depthWrite = (u8)depthWrite;
		op->u.pkt.atlas = (u8)atlas;
		op->u.pkt.baseVertex = baseVertex;
		op->u.pkt.vertCount = vertCount;
	}
}

void N3DS_StereoRecordVram(int slot, int baseVertex)
{
	N3DS_StereoOp *op = N3DS_StereoRecord(N3DS_STEREO_OP_VRAM);

	if (op != NULL)
	{
		op->u.pkt.atlas = (u8)slot;
		op->u.pkt.baseVertex = baseVertex;
		op->u.pkt.vertCount = 6;
	}
}

/* Top-screen widescreen mode. 0 = off (4:3 pillarbox crop).
 * 1 = STRETCH: lift the viewport crop only -- the ortho still maps the same
 *     window, so the same picture shows at wider pixels (anamorphic; kept as
 *     a dumpreq curiosity).
 * 2 = REAL: lift the crop AND extend the ortho window sideways by the same
 *     320->400 ratio (below), keeping PSX-units-per-panel-pixel IDENTICAL to
 *     4:3 mode. Perspective projection is planar, so content past the old
 *     window edges already projects to x<0 / x>dispW -- widening the window
 *     IS widening the FOV. The resident track/model renderers project on the
 *     GPU with no screen clamp, so their content genuinely fills the extra
 *     40 panel columns per side; anything CPU/retail-projected at 4:3
 *     (packets, UI) just lands centered at correct aspect. The wall is
 *     game-side 4:3 PVS/frustum culling: margins can show missing geometry.
 * Non-static: the renderer TU's stats screen and projection-rebuild check
 * read it across the plain-scalar TU boundary.
 *
 * Default 2 (REAL widescreen): the port boots widescreen by default.
 * SELECT+D-DOWN cycles off<->REAL, and dumpreq `wide`/`wide 1` reach all
 * three states including the anamorphic stretch. The known-open margin
 * classes (game-side 4:3 PVS/frustum culling holes, kart pop-in at the
 * margin edges, full-screen fades not covering the margins) are unchanged by
 * this -- promoting the default does not close them, it just stops hiding
 * them behind a chord nobody presses. */
int g_dbg_widescreen = 2;

void N3DS_C3DSetProjection(float left, float right, float bottom)
{
	if (g_dbg_widescreen == 2)
	{
		/* REAL widescreen: grow the window by exactly the viewport's
		 * 320->400 growth, split evenly, so 4:3 content keeps its position
		 * and scale and the margins become new world. At dispW=512 this is
		 * [-64, 576] -- 640 PSX columns across 400 panel px, the same 1.6
		 * units/px the 4:3 crop shows. */
		const float extra = (right - left) * ((400.0f - (float)N3DS_TOP_CONTENT_W) / (2.0f * (float)N3DS_TOP_CONTENT_W));

		left -= extra;
		right += extra;
	}
	/*
	 * PSX screen space -> clip space. There is no camera: the GTE already
	 * projected to 2D on the CPU, so this is a pure orthographic remap, and
	 * bottom/top are inverted (bottom > top) to reproduce PSX/PC's Y-down
	 * convention where Y=0 is the top of the image.
	 *
	 * This used to be hardcoded to the 3DS panel's own 400x240, which silently
	 * assumed PSX coordinates were already screen pixels. They are not: CTR
	 * runs a 512x216 DISPENV (SetDefDispEnv(..., 0x200, 0xd8) in MainMain.c),
	 * so a 1:1 passthrough cropped 112 columns -- 22% of the picture -- off the
	 * right edge and left a dead strip along the bottom. The extents now come
	 * from the game's live DISPENV instead; see NativeRenderer_BeginScene.
	 */
	/*
	 * Depth range covers the biased emit-ordinal in vertex z (s16, so
	 * [-32768, 32767] -- see N3DS_Vertex in n3ds_backend.h). Must span the
	 * full range even with CTR_NATIVE_OT_DEPTH off: real z values outside
	 * near/far would be CLIPPED, so a narrow range plus stamped z would
	 * erase geometry, not just mis-layer it.
	 */
	Mtx_OrthoTilt(&s_projection, left, right, bottom, 0.0f, -32769.0f, 32769.0f, true);
}

/*
 * Rebinds the packet path's program + attribute layout + vertex buffer +
 * texture-modulate combiner. Split out of N3DS_C3DInit (which calls it once,
 * right after the one-time DVLB parse/shader-program-init/linear-vertex-
 * buffer-allocation steps below) so N3DS_TrackDraw_End (rung 3a step 2) can
 * call the SAME code to restore this state after the track draw bracket
 * switches program/attrInfo/bufInfo/env0 away from it for its own untextured
 * draws -- see that function's comment for why every piece of this has to
 * come back before the packet path's merge-bin flush runs. Redundant to call
 * during N3DS_C3DInit's own one-time setup (nothing has changed yet); cheap
 * either way, and having exactly one place that knows the packet path's full
 * GPU-state shape is worth the one extra call at boot.
 */
static void N3DS_BindPacketDrawState(void)
{
	C3D_AttrInfo *attrInfo;
	C3D_BufInfo *bufInfo;
	C3D_TexEnv *env;

	C3D_BindProgram(&s_shaderProgram);

	attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	/* v0 count is 4, not 3: x, y, z=occlusion depth, w=parallax depth (the
	 * vertex channel split -- n3ds_backend.h's N3DS_Vertex comment). Must track
	 * the struct's field count exactly; a mismatch is a hardware freeze class,
	 * not a visual bug. */
	AttrInfo_AddLoader(attrInfo, 0, GPU_SHORT, 4);         /* v0 = position (x, y, z=occlusion depth, w=parallax depth) */
	AttrInfo_AddLoader(attrInfo, 1, GPU_SHORT, 2);         /* v1 = texcoord (u, v) */
	AttrInfo_AddLoader(attrInfo, 2, GPU_UNSIGNED_BYTE, 3); /* v2 = color (r, g, b) -- alpha is a shader constant now, not fetched */

	bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, s_vertexBufferMem, sizeof(N3DS_Vertex), 3, 0x210);

	/*
	 * Texture0 modulated by vertex color -- the CPU decode cache (design doc
	 * section 1) resolves page/CLUT/palette on the CPU, so by the time a
	 * texture reaches the GPU it is an ordinary RGBA8 image; the vertex color
	 * multiply reproduces the PC shader's v_color * texel behaviour (bright
	 * baked into vertex color CPU-side -- see native_renderer_3ds.c).
	 */
	env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	C3D_DirtyTexEnv(env);

	/*
	 * PICA200 vertex-shader float uniforms are physical registers shared by
	 * every bound program, not per-program handles: ctr.v.pica's
	 * projection[4] and ctr_track.v.pica's qx/qy/qw/projection/hParam/
	 * depthRange land on overlapping register indices. Re-assert packet
	 * projection/stereo uniforms here on every return to packet drawing, or
	 * a track draw's uniform writes silently corrupt subsequent packet draws.
	 *
	 * Re-asserting it HERE rather than in N3DS_TrackDraw_End on purpose: this
	 * function is the single definition of "packet draw state", it is already
	 * called from every path that returns to packet drawing, and the
	 * projection is as much packet state as the program or the buffer layout.
	 * Guarded because C3D_C3DInit calls this before the location is resolved.
	 */
	if (s_uLocProjection >= 0)
	{
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uLocProjection, &s_projection);
	}

	/* The stereo vector rides f4, which BOTH sibling programs also use
	 * (ctr_model's qx, ctr_track's projection row 1) -- so it is packet
	 * state exactly like the projection rows above, re-asserted on every
	 * return to packet drawing for the identical reason. */
	N3DS_StereoUploadUniform(s_uLocStereo, (const float(*)[4])s_stereoVecPkt);
	/* UI pop-out rides the same re-upload -- it sits on f5, which the model
	 * program uses for qy, so it must be re-asserted on every return to packet
	 * state exactly as the stereo vector is. */
	N3DS_StereoUploadUniform(s_uLocStereoUi, (const float(*)[4])s_stereoVecUi);
}

int N3DS_C3DInit(void)
{
	/* 4x citro3d's 256KB default. Both hardware crashes to date
	 * (crash_dump_00000007; crash_dump_00000008) were
	 * the SAME deliberate panic: prefetch abort, IFSR=debug event, svcBreak
	 * from GPUCMD_AddInternal's overflow check (libctru gpu.c:33) under
	 * C3D_DrawArrays <- N3DS_FlushRun -- a real race's draw volume (multiple
	 * karts + effects; attract demos are lighter and never hit it) overflows
	 * the default command list mid-frame. 1MB of linear heap is cheap here
	 * (~6MB spare after the atlases) vs a hardware crash per overflow. */
	if (!C3D_Init(4 * C3D_DEFAULT_CMDBUF_SIZE))
	{
		return 0;
	}

	s_renderTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	if (s_renderTarget == NULL)
	{
		return 0;
	}
	C3D_RenderTargetSetOutput(s_renderTarget, GFX_TOP, GFX_LEFT, N3DS_DISPLAY_TRANSFER_FLAGS);

	/*
	 * Right-eye target, same shape as the left (another ~768KB of VRAM; the
	 * decode-cache atlases live in linear memory, so VRAM holds only these
	 * two targets and has room to spare). DELIBERATELY non-fatal on failure:
	 * a console that cannot allocate it boots mono with 3D permanently off
	 * and says so once, rather than refusing to boot over an optional
	 * feature. Every stereo path is gated on this pointer being non-NULL.
	 */
	s_renderTargetRight = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	if (s_renderTargetRight != NULL)
	{
		C3D_RenderTargetSetOutput(s_renderTargetRight, GFX_TOP, GFX_RIGHT, N3DS_DISPLAY_TRANSFER_FLAGS);
	}
	else
	{
		N3DS_DebugLog("[CTR-3DS] stereo: right-eye render target FAILED to allocate, 3D disabled");
	}

	s_shaderDvlb = DVLB_ParseFile((u32 *)ctr_shbin, ctr_shbin_size);
	if (s_shaderDvlb == NULL)
	{
		return 0;
	}
	shaderProgramInit(&s_shaderProgram);
	shaderProgramSetVsh(&s_shaderProgram, &s_shaderDvlb->DVLE[0]);
	C3D_BindProgram(&s_shaderProgram);
	s_uLocProjection = shaderInstanceGetUniformLocation(s_shaderProgram.vertexShader, "projection");
	s_uLocStereo = shaderInstanceGetUniformLocation(s_shaderProgram.vertexShader, "stereo");
	s_uLocStereoUi = shaderInstanceGetUniformLocation(s_shaderProgram.vertexShader, "stereoUi");

	/*
	 * Placeholder until the first N3DS_C3DSetProjection call, which derives
	 * the real extents from the game's own DISPENV. See that function for why
	 * hardcoding screen pixels here was wrong.
	 */
	N3DS_C3DSetProjection(0.0f, 400.0f, 240.0f);

	s_vertexBufferMem = linearAlloc((size_t)N3DS_MAX_FRAME_VERTICES * sizeof(N3DS_Vertex));
	if (s_vertexBufferMem == NULL)
	{
		return 0;
	}

	/* Program + attribute layout + vertex buffer + texture-modulate combiner
	 * -- see N3DS_BindPacketDrawState's own comment for why this is a
	 * reusable function rather than inlined here (rung 3a step 2 needs to
	 * restore this exact state after the track draw bracket). */
	N3DS_BindPacketDrawState();

	/*
	 * Visibility sentinel. The decode cache encodes alpha as a single bit,
	 * with WHICH texels carry it decided per-cell at decode time (the
	 * N3DS_TexView per-texel STP views: punch-through for
	 * opaque cells, the STP split for semi-trans pass-1/pass-2 cells). One
	 * GEQUAL-1 test therefore serves every pass unchanged -- the pass
	 * distinction lives in the sampled cell, not in GPU state. Set once
	 * here rather than per-draw since no draw ever needs a different test.
	 */
	C3D_AlphaTest(true, GPU_GEQUAL, 1);

	/*
	 * Fixed blend color shared by BM_AVERAGE (reads the alpha, 0.5) and
	 * BM_ADD_QUATER_SOURCE (reads the RGB, 0.25) -- the same dual-use
	 * constant PC sets with glBlendColor(0.25, 0.25, 0.25, 0.5).
	 * GPUREG_BLEND_COLOR is R in bits 0-7 through A in bits 24-31.
	 * Set once: no mode ever needs a different constant, so
	 * N3DS_C3DSetBlendMode only ever touches equation/factors.
	 */
	C3D_BlendingColor(0x80404040u);
	N3DS_C3DSetBlendMode(0);

	C3D_CullFace(GPU_CULL_NONE);

	/*
	 * No depth testing, matching the PC renderer's explicit
	 * glDisable(GL_DEPTH_TEST) (native_renderer.c ~1880). This renderer is
	 * pure painter's algorithm: the PS1 ordering table sorts whole polygons
	 * on the CPU and draw order alone decides layering, which is why
	 * DrawTriangles is careful never to reorder triangles.
	 *
	 * Setting this explicitly is load-bearing, not tidiness. Every vertex
	 * leaves the shader at the SAME depth (ctr.v.pica forces z=0), so if
	 * citro3d's default happened to enable testing with GPU_GREATER, every
	 * fragment after the first at a given pixel would compare equal, fail,
	 * and be discarded -- silently inverting layering so that whatever drew
	 * FIRST wins instead of last. Relying on an unverified library default
	 * for that is not worth it when PC states the intent outright.
	 */
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);

	{
		int i;
		char msg[128];

		/* Atlases are the single largest linear-memory consumer here
		 * (N3DS_ATLAS_DIM^2 * 2 bytes each, GPU_RGBA5551). Log the budget
		 * before and after so cache capacity can be sized from the real
		 * ceiling instead of guessed -- see N3DS_ATLAS_COUNT's own comment
		 * in n3ds_backend.h for the current count and why; a stale version
		 * of this comment used to claim a specific count "failed outright",
		 * which was true only for the twice-as-large RGBA8 format this no
		 * longer uses. If allocation ever fails now, the log line below
		 * states exactly which atlas index and how much free KiB remained --
		 * trust that over any comment. */
		snprintf(msg, sizeof(msg), "[CTR-3DS] linear free before atlases: %lu KiB (want %d x %d KiB)",
		         (unsigned long)(linearSpaceFree() / 1024), N3DS_ATLAS_COUNT,
		         (N3DS_ATLAS_DIM * N3DS_ATLAS_DIM * 2) / 1024);
		N3DS_DebugLog(msg);

		for (i = 0; i < N3DS_ATLAS_COUNT; i++)
		{
			if (!C3D_TexInit(&s_atlasTextures[i], N3DS_ATLAS_DIM, N3DS_ATLAS_DIM, GPU_RGBA5551))
			{
				snprintf(msg, sizeof(msg), "[CTR-3DS] atlas %d of %d FAILED to allocate; linear free %lu KiB", i,
				         N3DS_ATLAS_COUNT, (unsigned long)(linearSpaceFree() / 1024));
				N3DS_DebugLog(msg);
				return 0;
			}
			C3D_TexSetFilter(&s_atlasTextures[i], GPU_NEAREST, GPU_NEAREST);
			C3D_TexSetWrap(&s_atlasTextures[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		}
	}

	/* Boot-splash present texture pool -- see the header comment on
	 * N3DS_C3DUploadVramDisplayChunk for why this is a small pool, not one
	 * shared texture like this loop might suggest. */
	{
		int slot;

		for (slot = 0; slot < N3DS_VRAM_DISPLAY_TEX_COUNT; slot++)
		{
			if (!C3D_TexInit(&s_vramDisplayTex[slot], N3DS_VRAM_DISPLAY_TEX_DIM, N3DS_VRAM_DISPLAY_TEX_DIM, GPU_RGBA5551))
			{
				N3DS_DebugLog("[CTR-3DS] VRAM display texture FAILED to allocate");
				return 0;
			}
			C3D_TexSetFilter(&s_vramDisplayTex[slot], GPU_NEAREST, GPU_NEAREST);
			C3D_TexSetWrap(&s_vramDisplayTex[slot], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		}
	}

#ifdef CTR_NATIVE_TRACK_RENDERER
	/* Rung 3a step 2's sibling shader/program -- a SECOND program object, not
	 * a second vertex shader in the same one, since the packet and track
	 * paths need to switch attribute layout and uniform sets independently
	 * (N3DS_TrackDraw_Begin/End bind whichever one is active). Failure here
	 * is fatal to init, same as the packet shader above: a track-renderer
	 * build with no track shader would silently draw nothing once the
	 * toggle is flipped on, which is worse than failing loudly at boot. */
	s_trackShaderDvlb = DVLB_ParseFile((u32 *)ctr_track_shbin, ctr_track_shbin_size);
	if (s_trackShaderDvlb == NULL)
	{
		return 0;
	}
	shaderProgramInit(&s_trackShaderProgram);
	shaderProgramSetVsh(&s_trackShaderProgram, &s_trackShaderDvlb->DVLE[0]);
	s_trackULocQx = shaderInstanceGetUniformLocation(s_trackShaderProgram.vertexShader, "qx");
	s_trackULocQy = shaderInstanceGetUniformLocation(s_trackShaderProgram.vertexShader, "qy");
	s_trackULocQw = shaderInstanceGetUniformLocation(s_trackShaderProgram.vertexShader, "qw");
	s_trackULocProjection = shaderInstanceGetUniformLocation(s_trackShaderProgram.vertexShader, "projection");
	s_trackULocH = shaderInstanceGetUniformLocation(s_trackShaderProgram.vertexShader, "hParam");
	s_trackULocDepthRange = shaderInstanceGetUniformLocation(s_trackShaderProgram.vertexShader, "depthRange");
	s_trackULocUvOffset = shaderInstanceGetUniformLocation(s_trackShaderProgram.vertexShader, "uvOffset");
	s_trackULocStereo = shaderInstanceGetUniformLocation(s_trackShaderProgram.vertexShader, "stereo");

	/* Fixed attribute layout for N3DS_TrackVertex -- does not depend on any
	 * per-level allocation (unlike s_trackBufInfo, built in
	 * N3DS_TrackGeom_Upload once a level's vertex buffer address is known),
	 * so it is safe to build once here. v2's count is 4, not 3 like the
	 * packet path's color loader -- drawOrder rides as the 4th fetched byte
	 * (see N3DS_TrackVertex's own comment for why it can't be a signed
	 * GPU_BYTE group instead). */
	AttrInfo_Init(&s_trackAttrInfo);
	AttrInfo_AddLoader(&s_trackAttrInfo, 0, GPU_SHORT, 3);         /* v0 = position (x, y, z) */
	AttrInfo_AddLoader(&s_trackAttrInfo, 1, GPU_SHORT, 2);         /* v1 = texcoord (u, v) -- unused in 3a */
	AttrInfo_AddLoader(&s_trackAttrInfo, 2, GPU_UNSIGNED_BYTE, 4); /* v2 = color + biased drawOrder */
#endif /* CTR_NATIVE_TRACK_RENDERER */

#ifdef CTR_NATIVE_MODEL_RENDERER
	/* Rung 2's program. Fatal on failure for the same reason the other two
	 * are: a model-renderer build with no model shader would draw nothing the
	 * moment the toggle is flipped, and "nothing appeared" is a far more
	 * expensive thing to debug than a loud boot failure.
	 *
	 * NO AttrInfo built here. Model draws reuse the packet layout, which
	 * N3DS_BindPacketDrawState installs; building a second identical one would
	 * be a copy that can silently drift from the layout the vertices are
	 * actually written in. */
	s_modelShaderDvlb = DVLB_ParseFile((u32 *)ctr_model_shbin, ctr_model_shbin_size);
	if (s_modelShaderDvlb == NULL)
	{
		return 0;
	}
	shaderProgramInit(&s_modelShaderProgram);
	shaderProgramSetVsh(&s_modelShaderProgram, &s_modelShaderDvlb->DVLE[0]);
	s_modelULocProjection = shaderInstanceGetUniformLocation(s_modelShaderProgram.vertexShader, "projection");
	s_modelULocQx = shaderInstanceGetUniformLocation(s_modelShaderProgram.vertexShader, "qx");
	s_modelULocQy = shaderInstanceGetUniformLocation(s_modelShaderProgram.vertexShader, "qy");
	s_modelULocQw = shaderInstanceGetUniformLocation(s_modelShaderProgram.vertexShader, "qw");
	s_modelULocH = shaderInstanceGetUniformLocation(s_modelShaderProgram.vertexShader, "hParam");
	s_modelULocStereo = shaderInstanceGetUniformLocation(s_modelShaderProgram.vertexShader, "stereo");
#endif /* CTR_NATIVE_MODEL_RENDERER */

	return 1;
}

void N3DS_C3DExit(void)
{
	int i;

#ifdef CTR_NATIVE_TRACK_RENDERER
	N3DS_TrackGeom_Free();
	shaderProgramFree(&s_trackShaderProgram);
	if (s_trackShaderDvlb != NULL)
	{
		DVLB_Free(s_trackShaderDvlb);
		s_trackShaderDvlb = NULL;
	}
#endif

	for (i = 0; i < N3DS_VRAM_DISPLAY_TEX_COUNT; i++)
	{
		C3D_TexDelete(&s_vramDisplayTex[i]);
	}

	for (i = 0; i < N3DS_ATLAS_COUNT; i++)
	{
		C3D_TexDelete(&s_atlasTextures[i]);
	}

	if (s_vertexBufferMem != NULL)
	{
		linearFree(s_vertexBufferMem);
		s_vertexBufferMem = NULL;
	}

	shaderProgramFree(&s_shaderProgram);
	if (s_shaderDvlb != NULL)
	{
		DVLB_Free(s_shaderDvlb);
		s_shaderDvlb = NULL;
	}

	C3D_Fini();
}

/*
 * DIAGNOSTIC: real hardware can freeze inside the first boot-splash present
 * with no further output. The two blocking candidates are C3D_FrameBegin's
 * SYNCDRAW wait (previous command list never completed) and C3D_FrameEnd's
 * queue handoff. Trace the first few frame brackets so the stream names the
 * exact call that never returns; bounded so steady-state runs don't flood
 * sdmc:/ctr3ds.log.
 */
static int s_frameTraceBudget = 48;

static void N3DS_FrameTrace(const char *msg)
{
	if (s_frameTraceBudget > 0)
	{
		s_frameTraceBudget--;
		N3DS_DebugLog(msg);
	}
}

/*
 * g_dbg_depthTest isolates whether the depth test (state 1) or the track's
 * own depth writes (state 2) hide non-track content. Every state is a VISUAL
 * binary a human settles in one glance -- deliberately not a counter, for the
 * reason PORT_FEASIBILITY_3DS.md's "How to measure" section exists:
 *
 *   0 NORMAL  -- LEQUAL + depth writes. Today's behaviour, the baseline.
 *   1 NOTEST  -- GPU_ALWAYS + depth writes: pure submission (painter's) order.
 *                Content reappears  => the depth TEST is what hides it, so
 *                                      this is a depth-VALUE bug.
 *                Content still gone  => depth is irrelevant and the content is
 *                                      never drawn at all -- look at what
 *                                      skipping DrawLevelOvr1P took with it
 *                                      (MainFrame_RenderFrame.c's
 *                                      skipRetailTrackDraw), starting with the
 *                                      skybox, which that call does NOT draw
 *                                      and which is missing anyway.
 *   2 TRACKRO -- normal test, but the TRACK draws read-only against depth
 *                (GPU_WRITE_COLOR, applied in N3DS_TrackDraw_Begin). Isolates
 *                the track's own depth WRITES from every other variable: if
 *                only this state restores the karts, the track is stamping
 *                depth that outranks them, and the packet side is fine.
 *
 * State 1 vs 2 is the whole point -- 1 asks "is it depth at all", 2 asks "is it
 * OUR depth". Runtime, because the answer is worth more than a rebuild cycle:
 * dumpreq `depthtest [N]` in Azahar, SELECT+ZL on hardware (a dumpreq file
 * cannot be placed mid-race).
 */
int g_dbg_depthTest = 0;

/*
 * s32 near-band polarity (ctr_track.v.pica's NEAR-BAND POLARITY block). 0 is
 * the pre-s32 build bit-for-bit; 1 sends track vertices at or behind the eye
 * to the FAR end of the depth axis instead of the near end.
 *
 * Lives in THIS translation unit like g_dbg_depthTest, because both of its
 * readers (the hParam uniform write and the dumpreq handler) are here.
 *
 * dumpreq `nearfar` only -- deliberately NOT a button. Every existing chord is
 * taken, and unlike a perf A/B this one does not need a mid-race flip: the
 * sink is visible in a still frame, so booting once each way settles it, and a
 * dumpreq IS placeable before launch even on hardware.
 *
 * DEFAULT 0, INCLUDING in demo builds. It briefly rode
 * CTR_NATIVE_RENDERER_DEFAULT_ON so a watch-only sitting would see it armed;
 * the owner then watched that build and reported racers still sunk **on
 * sweeping aerial shots**, which falsifies the theory outright -- from a
 * camera high above the track there is no geometry at or behind the eye for
 * this to touch, so it cannot be what buries anything. A sink that is
 * unchanged by camera distance and angle is a CONSTANT OFFSET, a different
 * failure entirely. Kept (it is still strictly more correct than clamping
 * behind-eye vertices to maximally-near) but off, and no longer allowed to
 * ride along in a build where something else is being judged.
 */
/*
 * Preset ladder for the packet depth bias (see g_dbg_pktDepthBias, native_gpu.c).
 * Values are view.z units pulled toward the camera. Deliberately coarse and
 * short: this is dialled BY EYE mid-race on hardware, where a fine-grained
 * control the owner has to press twenty times is useless. 256 is the top rung
 * because it equals the track's own near-band clamp -- past that, packet
 * content starts winning against track surfaces it is genuinely behind, which
 * is the "racers through walls" failure the owner explicitly ruled out.
 * Shared by the hardware chord and the dumpreq token so the two agree.
 */
static int N3DS_NextPktBias(int cur)
{
	static const int ladder[5] = {0, 32, 64, 128, 256};
	int i;

	for (i = 0; i < 5; i++)
	{
		if (ladder[i] == cur)
		{
			return ladder[(i + 1) % 5];
		}
	}
	return 0; /* off-ladder (set via 'pktbias N') rejoins at the bottom */
}

/*
 * C3D_FrameBegin(SYNCDRAW) aligns the start of every frame build to the
 * hardware vblank; that alignment is native-only overhead, not pacing, since
 * the 30fps cap is enforced by the game itself (ReadyToFlip counts the
 * emulated 60Hz clock, not phase-locked to the panel) and presentation is
 * vblank-latched by GSP regardless, so nothing tears without it. 1 = align,
 * 0 = begin the moment the GPU is free. Runtime A/B: SELECT+B on hardware,
 * dumpreq `framesync` in Azahar.
 *
 * Default 0: hardware-measured, Tubes mono 41->34.7ms, stereo 49->36.2ms --
 * most of stereo's extra frame cost was this wait rounding frames up to the
 * next vblank multiple, not render cost.
 */
int g_dbg_frameSync = 0;

void N3DS_C3DBeginFrame(void)
{
	/* Latch the physical 3D slider for the whole frame BEFORE anything is
	 * cleared or uploaded: the latch decides whether the right target gets
	 * cleared, which parallax the left eye's own uniforms carry, and whether
	 * the draw journal records. Pure CPU, so it sits ahead of the SYNCDRAW
	 * wait rather than inside the frame bracket. */
	N3DS_StereoLatchFrame();

	N3DS_FrameTrace("[CTR-3DS] FrameBegin: enter (SYNCDRAW wait)");
	C3D_FrameBegin(g_dbg_frameSync ? C3D_FRAME_SYNCDRAW : 0);
	N3DS_FrameTrace("[CTR-3DS] FrameBegin: returned");
	/* Depth clears to FAR. Mtx_OrthoTilt maps LARGER vertex z to SMALLER
	 * stored depth (verified empirically, see below), so far = max. Harmless
	 * when CTR_NATIVE_OT_DEPTH is off (depth buffer unused). */
	C3D_RenderTargetClear(s_renderTarget, C3D_CLEAR_ALL, 0x000000FF, 0x00FFFFFF);
	if (s_stereoFrameActive)
	{
		/* Only on stereo frames: a cleared-but-never-drawn right target
		 * would still cost the clear, and with the barrier off nothing
		 * presents it anyway. */
		C3D_RenderTargetClear(s_renderTargetRight, C3D_CLEAR_ALL, 0x000000FF, 0x00FFFFFF);
	}
	C3D_FrameDrawOn(s_renderTarget);

#ifdef CTR_NATIVE_OT_DEPTH
	/*
	 * Depth substrate: every prim carries its OT-emission ordinal as z, so
	 * later-submitted always wins the depth test -- painter's order in the
	 * depth buffer, making submission REORDER legal for the cross-bucket
	 * merge. Polarity was settled EMPIRICALLY and the first guess (GEQUAL +
	 * clear 0) was wrong in an instructive way: inverted layering does NOT
	 * look broken in back-to-front world scenes (far-wins camouflages as
	 * plausible occlusion) -- it announces itself by ERASING late-drawn
	 * content: the loading flag's LOADING letters, the DEMO MODE banner,
	 * the karts. Mtx_OrthoTilt maps larger vertex z to smaller stored
	 * depth, so the correct pair is LEQUAL + clear-to-max. If this ever
	 * needs re-deriving, use the loading screen's letters as the canary,
	 * not world layering.
	 */
	/* State 2 (TRACKRO) leaves this at the normal pair -- it only narrows the
	 * write mask for the track's own draws, inside N3DS_TrackDraw_Begin. */
	if (g_dbg_depthTest == 1)
	{
		C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
	}
	else
	{
		C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
	}
#endif

	/*
	 * Pillarbox the top screen to the bottom screen's 4:3-equivalent width.
	 * Top panel is 400 wide, bottom is 320 -- PSX content is
	 * meant for 4:3, and letting it fill the full 400 (what the ortho range
	 * alone can't prevent, see NativeRenderer_BeginScene) shows ~40 extra PSX
	 * columns of world per edge that were never meant to be on screen.
	 *
	 * The render target is stored sideways, 240x400 (see
	 * C3D_RenderTargetCreate above), for the 3DS's rotated panel, so
	 * screen-X is this buffer's Y axis, not X. A *centred* crop is symmetric
	 * either way that rotation runs, so which physical edge is which doesn't
	 * matter here: (400-320)/2 = 40px margin, full 240 untouched on the
	 * buffer's other axis (both screens are 240 tall, no vertical crop).
	 * Fixed hardware constants, not DISPENV-dependent, so set once a frame
	 * here rather than threaded through as a parameter.
	 *
	 * N3DS_TOP_CONTENT_* (n3ds_backend.h) mirror this crop for anything that
	 * reads the presented frame back -- keep them in lockstep with this call
	 * (N3DS_CopyDisplayedTopFrame samples only the content region, else the
	 * pause background re-ingests the black bars and shows them squeezed
	 * inside the picture -- user-reported on hardware).
	 *
	 * Any nonzero g_dbg_widescreen lifts the crop. Mode 1 alone is an
	 * anamorphic STRETCH; mode 2 (REAL) pairs the lift with the extended
	 * ortho window in N3DS_C3DSetProjection -- see the flag's definition.
	 * KNOWN while wide: the pause-background readback still samples the
	 * 320-wide CONTENT region, so the paused backdrop is center-cropped;
	 * and full-screen packet quads (fades, pause dim) cover only the
	 * centered 4:3 band, leaving the margins undimmed.
	 */
	C3D_SetViewport(0, g_dbg_widescreen ? 0 : N3DS_TOP_CONTENT_X, 240, g_dbg_widescreen ? 400 : N3DS_TOP_CONTENT_W);

	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uLocProjection, &s_projection);
	/* Left eye's parallax (zero in mono) -- rides beside the projection for
	 * the same reason it does in N3DS_BindPacketDrawState. */
	N3DS_StereoUploadUniform(s_uLocStereo, (const float(*)[4])s_stereoVecPkt);
	/* UI pop-out rides the same re-upload -- it sits on f5, which the model
	 * program uses for qy, so it must be re-asserted on every return to packet
	 * state exactly as the stereo vector is. */
	N3DS_StereoUploadUniform(s_uLocStereoUi, (const float(*)[4])s_stereoVecUi);

	/* Every draw this frame appends into the SAME persistent buffer at a
	 * fresh offset (see the header comment on N3DS_C3DAppendVertices) --
	 * reset the write cursor once per frame, not per draw. */
	s_vertexCursor = 0;
}

/* GPU-time window accumulator (see n3ds_backend.h). Written on whichever
 * thread ends frames (the render worker when the queue is live), fetched from
 * the game thread once per [PERF] window -- a torn read costs one frame of a
 * 64-frame average, so no lock. */
static float s_gtCpuMs;
static float s_gtGpuMs;
static unsigned s_gtFrames;

void N3DS_C3DEndFrame(void)
{
	N3DS_FrameTrace("[CTR-3DS] FrameEnd: enter (submit)");
	C3D_FrameEnd(0);
	N3DS_FrameTrace("[CTR-3DS] FrameEnd: returned");

	/* Read here rather than in BeginFrame so the pair is sampled once per
	 * real rendered frame. cpuTime is stamped inside C3D_FrameEnd for the
	 * frame just submitted; gpuTime is stamped by the GX queue-finish
	 * callback, so it belongs to the PREVIOUS completed frame -- a one-frame
	 * skew that vanishes in a windowed average. */
	s_gtCpuMs += C3D_GetProcessingTime();
	s_gtGpuMs += C3D_GetDrawingTime();
	s_gtFrames++;
}

void N3DS_C3DGpuTimeWindow(int *cpuUs, int *gpuUs, unsigned *frames)
{
	unsigned n = s_gtFrames;

	*cpuUs = (n != 0) ? (int)((s_gtCpuMs * 1000.0f) / (float)n) : 0;
	*gpuUs = (n != 0) ? (int)((s_gtGpuMs * 1000.0f) / (float)n) : 0;
	*frames = n;
	s_gtCpuMs = 0.0f;
	s_gtGpuMs = 0.0f;
	s_gtFrames = 0;
}

int N3DS_C3DAppendVertices(const N3DS_Vertex *vertices, int count)
{
	int base;
	N3DS_Vertex *dst;

	if ((count <= 0) || ((s_vertexCursor + count) > N3DS_MAX_FRAME_VERTICES))
	{
		return -1;
	}

	base = s_vertexCursor;
	dst = (N3DS_Vertex *)s_vertexBufferMem + base;
	memcpy(dst, vertices, (size_t)count * sizeof(N3DS_Vertex));
	s_vertexCursor += count;

	if (s_vertexHighWater < s_vertexCursor)
	{
		s_vertexHighWater = s_vertexCursor;
	}

	return base;
}

int N3DS_C3DFrameVertexHighWater(void)
{
	return s_vertexHighWater;
}

void N3DS_C3DDrawTriangles(int startVertex, int triangleCount)
{
	C3D_DrawArrays(GPU_TRIANGLES, startVertex, triangleCount * 3);
}

/*
 * 8x8 texels * 2 bytes/texel (RGBA5551) -- one PICA200 tile, tiled
 * contiguously.
 *
 * Previously RGBA8. The decode cache encoded a three-level alpha
 * (0 punch-through / 128 opaque / 255 STP-on) but NOTHING ever read the
 * 128-vs-255 distinction: the only alpha test in this file is a single
 * C3D_AlphaTest(true, GPU_GEQUAL, 1), i.e. zero-vs-nonzero. So half of
 * every cell's memory was carrying a distinction no code consumed.
 *
 * Halving the texel doubles the number of cache cells the same linear memory
 * buys, and cell COUNT was the measured bottleneck: playing the menu showed
 * 1441 capacity misses per 32-frame window (~45/frame) against a saturated
 * 63/63 working set, plus 2052 evictions of cells a queued draw still
 * referenced -- which is the visible texture corruption. The STP two-pass
 * blend recovered the STP bit exactly as predicted here: from
 * VRAM at decode time, baked into per-view cells (N3DS_TexView), keeping
 * this format one bit of alpha.
 */
#define N3DS_TILE_BYTES (64 * 2)

/*
 * Manual sub-rect write -- see the header comment for why C3D_TexUpload
 * can't be used here. PICA200 tiling addresses 8x8-texel tiles in simple
 * row-major order across a texture's FULL width (verified against
 * tex3ds's reference swizzle, see native_renderer_3ds.c). A tile block is a
 * tilesWide x tilesHigh square positioned at tile offset (baseTileCol,
 * baseTileRow) within the destination texture's own tile grid
 * (tilesPerRow x tilesPerRow); each of its tile-rows is tilesWide
 * CONSECUTIVE tiles, which -- because tile-column is the fastest-varying
 * index in row-major order -- are contiguous in the texture's memory too,
 * so one memcpy per tile-row is exact and correct, not an approximation.
 *
 * Shared by the whole-cell (32x32-tile) and sub-cell (8x8-tile) decode-cache
 * upload wrappers below (docs/SUBPAGE_DECODE_CACHE_DESIGN.md), AND by the
 * native track renderer's dedicated atlas pool (N3DS_TrackAtlas_UploadRect,
 * rung 2b) -- generalized from a hardcoded s_atlasTextures[]/
 * N3DS_ATLAS_DIM to an explicit destination texture + tiles-per-row so both
 * pools can share one blit implementation rather than duplicating it.
 * tilesWide/tilesHigh must be used for ALL FOUR of the base-tile calc (the
 * caller's job here, via baseTileCol/baseTileRow), the loop bound, the
 * srcRow stride, and the memcpy width -- getting only the loop bound right
 * and leaving srcRow/memcpy at a stale hardcoded stride was caught in review
 * before this shipped (correct only for a 32-wide caller, silently wrong for
 * an 8-wide one).
 */
static void N3DS_C3DUploadTileBlock(C3D_Tex *tex, int tilesPerRow, int baseTileCol, int baseTileRow, int tilesWide,
                                     int tilesHigh, const void *tiled)
{
	const u8 *src = (const u8 *)tiled;
	u8 *texData;
	int localTileRow;

	if (tex == NULL)
	{
		return;
	}

	texData = (u8 *)tex->data;

	for (localTileRow = 0; localTileRow < tilesHigh; localTileRow++)
	{
		const int dstTileRow = baseTileRow + localTileRow;
		u8 *dstRow = texData + (size_t)((dstTileRow * tilesPerRow) + baseTileCol) * N3DS_TILE_BYTES;
		const u8 *srcRow = src + (size_t)localTileRow * tilesWide * N3DS_TILE_BYTES;

		memcpy(dstRow, srcRow, (size_t)tilesWide * N3DS_TILE_BYTES);
	}

	/*
	 * Flush the just-written region to physical RAM. Without this the GPU (and,
	 * critically, Azahar's/Citra's host-side texture cache) never learns the
	 * texture's content changed: after a cell is evicted and re-decoded for a
	 * new texture, the emulator keeps serving the cell's PREVIOUS occupant, which
	 * is exactly the "recognizable textures showing up in the wrong places on the
	 * track" corruption. This is the missing counterpart to a C3D_TexUpload that
	 * the raw-memcpy upload path above never had. Flush the bounding range of
	 * the rows written (over-flushing the inter-row gaps is harmless -- the data
	 * there is already correct, and a slightly larger invalidation just re-reads
	 * a little more of the texture). GSPGPU_FlushDataCache rounds to cache lines
	 * internally, so an unaligned start/size is fine. */
	{
		u8 *flushStart = texData + (size_t)((baseTileRow * tilesPerRow) + baseTileCol) * N3DS_TILE_BYTES;
		size_t flushBytes = (size_t)(((tilesHigh - 1) * tilesPerRow) + tilesWide) * N3DS_TILE_BYTES;

		GSPGPU_FlushDataCache(flushStart, (u32)flushBytes);
	}
}

void N3DS_C3DUploadDecodedCell(int atlasIndex, int cellX, int cellY, const void *tiled256)
{
	if ((atlasIndex < 0) || (atlasIndex >= N3DS_ATLAS_COUNT))
	{
		return;
	}
	N3DS_C3DUploadTileBlock(&s_atlasTextures[atlasIndex], N3DS_ATLAS_DIM / 8, cellX * 32, cellY * 32, 32, 32, tiled256); /* unchanged behavior */
}

void N3DS_C3DUploadDecodedSubCell(int atlasIndex, int physCellX, int physCellY, int subX, int subY, const void *tiled64)
{
	if ((atlasIndex < 0) || (atlasIndex >= N3DS_ATLAS_COUNT))
	{
		return;
	}
	N3DS_C3DUploadTileBlock(&s_atlasTextures[atlasIndex], N3DS_ATLAS_DIM / 8, (physCellX * 32) + (subX * 8), (physCellY * 32) + (subY * 8), 8, 8,
	                         tiled64);
}

#ifdef CTR_INTERNAL
/* Dumps every atlas texture's raw (8x8-Morton-tiled RGBA5551) linear storage to
 * sdmc:/atlas_<i>.bin for the texture-corruption investigation.
 * Un-swizzled + rendered on the host. Triggered by SELECT+R alongside the VRAM
 * dump so both capture the same frame. s_atlasTextures[].data is CPU-written by
 * the decode (N3DS_C3DUploadTileBlock), so it is CPU-readable here. */
static void N3DS_DebugWriteAtlasDump(void)
{
	const size_t bytes = (size_t)N3DS_ATLAS_DIM * (size_t)N3DS_ATLAS_DIM * 2;
	char path[64];
	char msg[96];
	int i, written = 0;

	for (i = 0; i < N3DS_ATLAS_COUNT; i++)
	{
		FILE *f;

		if (s_atlasTextures[i].data == NULL)
		{
			continue;
		}

		snprintf(path, sizeof(path), "sdmc:/atlas_%u_%d.bin", N3DS_DebugCaptureFrame(), i);
		f = fopen(path, "wb");
		if (f == NULL)
		{
			continue;
		}

		fwrite(s_atlasTextures[i].data, 1, bytes, f);
		fclose(f);
		written++;
	}

	snprintf(msg, sizeof(msg), "[CTR-3DS] dumped %d/%d atlas textures (%u bytes each)", written, N3DS_ATLAS_COUNT, (unsigned)bytes);
	N3DS_DebugLog(msg);
}
#endif

void N3DS_C3DSetColorDoubling(int enable)
{
	/*
	 * PC's "bright" multiplier, done the way PC does it. Its vertex shader
	 * computes v_color.xyz = (color/255) * bright with NO clamp, and the
	 * fragment shader clamps only at the final write, AFTER multiplying by
	 * the texel (native_renderer.c: "v_color.xyz *= a_texcoord.z", then
	 * "fragColor = dither(color * v_color)").
	 *
	 * Baking bright into the vertex colour CPU-side cannot reproduce that:
	 * a vertex colour is clamped to 1.0 before it ever reaches the multiply,
	 * so all the headroom above 1.0 is lost and every lit textured surface
	 * comes out darker than on PC. The TEV output scale is applied AFTER the
	 * modulate, which restores PC's exact ordering.
	 *
	 * RGB only -- deliberately NOT C3D_Both. PC scales only .xyz; doubling
	 * alpha as well would push the punch-through-vs-opaque bit from 1 to 2,
	 * i.e. out of range for the 1-bit alpha channel (see N3DS_TILE_BYTES),
	 * corrupting the punch-through alpha test in n3ds_backend.c's C3D_AlphaTest.
	 */
	C3D_TexEnv *env = C3D_GetTexEnv(0);

	C3D_TexEnvScale(env, C3D_RGB, enable ? GPU_TEVSCALE_2 : GPU_TEVSCALE_1);
	/* Explicit, not redundant-by-inspection: C3D_TexEnvScale is a plain
	 * inline struct write, and citro3d exposes C3D_DirtyTexEnv precisely
	 * because a modified env is not always flagged for re-emission. Without
	 * a dirty flag the change never reaches the GPU and this whole function
	 * is a silent no-op. Cheap to state outright rather than depend on
	 * C3D_GetTexEnv's internals, which aren't visible from the headers. */
	C3D_DirtyTexEnv(env);
}

void N3DS_C3DBindAtlas(int atlasIndex)
{
	if ((atlasIndex < 0) || (atlasIndex >= N3DS_ATLAS_COUNT))
	{
		return;
	}

	C3D_TexBind(0, &s_atlasTextures[atlasIndex]);
}

/* ------------------------------------------------ native track atlas --- */
/* See the header comment (n3ds_backend.h) for the pool's rationale. */

void N3DS_TrackAtlas_Free(void)
{
	int i;

	for (i = 0; i < s_trackAtlasUnitsAllocated; i++)
	{
		C3D_TexDelete(&s_trackAtlasTextures[i]);
	}
	s_trackAtlasUnitsAllocated = 0;
}

int N3DS_TrackAtlas_Alloc(int unitCount)
{
	char msg[128];
	int i;

	if (unitCount > N3DS_TRACK_ATLAS_MAX_UNITS)
	{
		unitCount = N3DS_TRACK_ATLAS_MAX_UNITS;
	}
	if (unitCount < 0)
	{
		unitCount = 0;
	}

	/* Level reload: free any previous pool before allocating the new one --
	 * same idiom as NativeTrackMesh_Free on the game side. */
	N3DS_TrackAtlas_Free();

	snprintf(msg, sizeof(msg), "[CTR-3DS] linear free before track atlas: %lu KiB (want %d x %d KiB)",
	         (unsigned long)(linearSpaceFree() / 1024), unitCount, (N3DS_TRACK_ATLAS_DIM * N3DS_TRACK_ATLAS_DIM * 2) / 1024);
	N3DS_DebugLog(msg);

	for (i = 0; i < unitCount; i++)
	{
		if (!C3D_TexInit(&s_trackAtlasTextures[i], N3DS_TRACK_ATLAS_DIM, N3DS_TRACK_ATLAS_DIM, GPU_RGBA5551))
		{
			snprintf(msg, sizeof(msg), "[CTR-3DS] track atlas unit %d of %d FAILED to allocate; linear free %lu KiB", i, unitCount,
			         (unsigned long)(linearSpaceFree() / 1024));
			N3DS_DebugLog(msg);
			/* Units [0, i) allocated fine -- keep them live and usable rather
			 * than tearing the whole pool down over a partial failure. */
			s_trackAtlasUnitsAllocated = i;
			return i;
		}
		C3D_TexSetFilter(&s_trackAtlasTextures[i], GPU_NEAREST, GPU_NEAREST);
		C3D_TexSetWrap(&s_trackAtlasTextures[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	}

	s_trackAtlasUnitsAllocated = unitCount;
	return unitCount;
}

void N3DS_TrackAtlas_UploadRect(int unit, int dstX, int dstY, int wTexels, int hTexels, const void *tiledRGBA5551)
{
	if ((unit < 0) || (unit >= s_trackAtlasUnitsAllocated))
	{
		return;
	}

	/* Preconditions (see the header comment): dstX/dstY/wTexels/hTexels are
	 * all multiples of 8, and the rect must fit inside the unit's own
	 * N3DS_TRACK_ATLAS_DIM bounds. A violation here is a caller bug, not
	 * routine content variation (the packer's own invariants guarantee this
	 * in practice) -- reject and log it instead of silently writing into the
	 * wrong tile or past the texture's backing memory. */
	if (((dstX & 7) != 0) || ((dstY & 7) != 0) || ((wTexels & 7) != 0) || ((hTexels & 7) != 0) || ((dstX + wTexels) > N3DS_TRACK_ATLAS_DIM) ||
	    ((dstY + hTexels) > N3DS_TRACK_ATLAS_DIM))
	{
		char msg[128];

		snprintf(msg, sizeof(msg), "[CTR-3DS] track atlas upload REJECTED: unit=%d dst=(%d,%d) size=%dx%d (unaligned or out of bounds)", unit, dstX,
		         dstY, wTexels, hTexels);
		N3DS_DebugLog(msg);
		return;
	}

	/* Dividing by 8 here converts to the tile-space units
	 * N3DS_C3DUploadTileBlock actually addresses in. */
	N3DS_C3DUploadTileBlock(&s_trackAtlasTextures[unit], N3DS_TRACK_ATLAS_DIM / 8, dstX / 8, dstY / 8, wTexels / 8, hTexels / 8, tiledRGBA5551);
}

void N3DS_TrackAtlas_Bind(int unit)
{
	if ((unit < 0) || (unit >= s_trackAtlasUnitsAllocated))
	{
		return;
	}

	C3D_TexBind(0, &s_trackAtlasTextures[unit]);
}

/* ------------------------------------------------ native track draw --- */
/* Rung 3a step 2. See the header comment (n3ds_backend.h) for the bracket
 * contract. #else below provides no-op/failing stubs so the unconditional
 * declarations in n3ds_backend.h always link, the same promise
 * native_track_mesh.h makes for ITS accessors -- these functions have a real
 * compile-time dependency the atlas pool above does not (the generated
 * ctr_track_shbin.h only exists when CTR_NATIVE_TRACK_RENDERER is on), so
 * unlike N3DS_TrackAtlas_*, the real bodies have to be gated. */
#ifdef CTR_NATIVE_TRACK_RENDERER

int N3DS_TrackGeom_Upload(const void *verts, int vertCount, int vertStride, const void *indices, int indexCount)
{
	size_t vertBytes;
	size_t indexOffset;
	size_t totalBytes;
	u8 *block;

	N3DS_TrackGeom_Free();

	if ((verts == NULL) || (vertCount <= 0) || (vertStride <= 0) || (indices == NULL) || (indexCount <= 0))
	{
		return 0;
	}

	vertBytes = (size_t)vertCount * (size_t)vertStride;
	indexOffset = (vertBytes + 7u) & ~(size_t)7u; /* 8-byte align, see the header comment */
	totalBytes = indexOffset + ((size_t)indexCount * sizeof(u16));

	block = (u8 *)linearAlloc(totalBytes);
	if (block == NULL)
	{
		char msg[128];

		snprintf(msg, sizeof(msg), "[CTR-3DS] track geom upload FAILED: %u bytes, linear free %lu KiB", (unsigned)totalBytes,
		         (unsigned long)(linearSpaceFree() / 1024));
		N3DS_DebugLog(msg);
		return 0;
	}

	memcpy(block, verts, vertBytes);
	memcpy(block + indexOffset, indices, (size_t)indexCount * sizeof(u16));

	s_trackGeomMem = block;
	s_trackIndexPtr = block + indexOffset;
	s_trackGeomVertCount = vertCount;
	s_trackGeomVertStride = vertStride;

	/* Rebuilt every upload (not just once): base_paddr depends on THIS
	 * level's linearAlloc address, which N3DS_TrackAtlas_Alloc-style level
	 * reloads change every time. s_trackAttrInfo is NOT rebuilt here -- its
	 * layout is fixed and already built once in N3DS_C3DInit. */
	BufInfo_Init(&s_trackBufInfo);
	BufInfo_Add(&s_trackBufInfo, s_trackGeomMem, vertStride, 3, 0x210);

	s_trackGeomReady = 1;
	return 1;
}

void N3DS_TrackGeom_Free(void)
{
	if (s_trackGeomMem != NULL)
	{
		linearFree(s_trackGeomMem);
		s_trackGeomMem = NULL;
	}
	s_trackIndexPtr = NULL;
	s_trackGeomReady = 0;
	s_trackGeomVertCount = 0;
	s_trackGeomVertStride = 0;
}

/*
 * Rewrite the UV pair of `count` already-uploaded vertices in place.
 *
 * This is how animated track textures (turbo-pad arrows, waterfalls, lava)
 * move at all under the resident renderer -- everything else about a resident
 * vertex is fixed at load, but which frame of a texture chain it samples is
 * not, and the chain advances every vblank.
 *
 * Two things this must get right, both learned the hard way elsewhere in this
 * file:
 *   - The GPU reads PHYSICAL memory and the CPU wrote through the data cache,
 *     so the touched range has to be flushed before the next draw or the GPU
 *     samples the previous frame's UVs (or worse, a mix). Flushed as ONE range
 *     spanning min..max of the batch rather than per vertex: a cache-clean
 *     call has fixed overhead that swamps 4 bytes of payload.
 *   - Every index is bounds-checked against the block's OWN recorded vertex
 *     count. The staging buffer the upload copied from is freed, so this block
 *     is the only copy of the level's geometry that exists.
 *
 * THREAD: called on the walk thread at the head of a walk (see
 * NativeTrackAnim_PatchFrame's comment), never concurrently with a draw out of
 * this same block.
 */
void N3DS_TrackGeom_PatchUV(const unsigned short *vertIndices, const short *uvPairs, int count)
{
	u8 *base;
	int i;
	int lo = 0, hi = 0, touched = 0;

	if (!s_trackGeomReady || (s_trackGeomMem == NULL) || (vertIndices == NULL) || (uvPairs == NULL) || (count <= 0))
	{
		return;
	}

	base = (u8 *)s_trackGeomMem;

	for (i = 0; i < count; i++)
	{
		int vi = (int)vertIndices[i];
		s16 *uv;

		if ((vi < 0) || (vi >= s_trackGeomVertCount))
		{
			continue;
		}

		/* Offset 6 in N3DS_TrackVertex -- see the struct's field comments in
		 * n3ds_backend.h, which pin every s16 to an even offset for exactly
		 * this kind of direct write. */
		uv = (s16 *)(void *)(base + ((size_t)vi * (size_t)s_trackGeomVertStride) + 6);
		uv[0] = uvPairs[i * 2 + 0];
		uv[1] = uvPairs[i * 2 + 1];

		if (!touched)
		{
			lo = vi;
			hi = vi;
			touched = 1;
		}
		else
		{
			if (vi < lo)
			{
				lo = vi;
			}
			if (vi > hi)
			{
				hi = vi;
			}
		}
	}

	if (touched)
	{
		size_t first = (size_t)lo * (size_t)s_trackGeomVertStride;
		size_t last = ((size_t)hi + 1u) * (size_t)s_trackGeomVertStride;

		GSPGPU_FlushDataCache(base + first, (u32)(last - first));
	}
}

/*
 * The same in-place batched rewrite, for POSITION and COLOR rather than
 * UV -- the animated-vertex case (water surfaces that physically rise and
 * fall, AnimateWater.c writing LevVertex.pos/.color_hi every frame).
 *
 * Separate from PatchUV rather than one call with optional arguments because
 * the two have completely different populations and cadences: UVs change only
 * when a texture chain steps to its next frame (often several vblanks apart,
 * a handful of vertices), positions change every single frame for every
 * animated vertex on the track. Fusing them would make the cheap case pay the
 * expensive case's flush range.
 *
 * xyz is three shorts per entry (offset 0), rgb three bytes (offset 10) -- see
 * N3DS_TrackVertex. Same bounds check and same single range flush as PatchUV,
 * same thread rule.
 */
void N3DS_TrackGeom_PatchPosColor(const unsigned short *vertIndices, const short *xyz, const unsigned char *rgb, int count)
{
	u8 *base;
	int i;
	int lo = 0, hi = 0, touched = 0;

	if (!s_trackGeomReady || (s_trackGeomMem == NULL) || (vertIndices == NULL) || (xyz == NULL) || (rgb == NULL) || (count <= 0))
	{
		return;
	}

	base = (u8 *)s_trackGeomMem;

	for (i = 0; i < count; i++)
	{
		int vi = (int)vertIndices[i];
		u8 *vert;
		s16 *pos;

		if ((vi < 0) || (vi >= s_trackGeomVertCount))
		{
			continue;
		}

		vert = base + ((size_t)vi * (size_t)s_trackGeomVertStride);
		pos = (s16 *)(void *)vert;
		pos[0] = xyz[i * 3 + 0];
		pos[1] = xyz[i * 3 + 1];
		pos[2] = xyz[i * 3 + 2];
		vert[10] = rgb[i * 3 + 0];
		vert[11] = rgb[i * 3 + 1];
		vert[12] = rgb[i * 3 + 2];

		if (!touched)
		{
			lo = vi;
			hi = vi;
			touched = 1;
		}
		else
		{
			if (vi < lo)
			{
				lo = vi;
			}
			if (vi > hi)
			{
				hi = vi;
			}
		}
	}

	if (touched)
	{
		size_t first = (size_t)lo * (size_t)s_trackGeomVertStride;
		size_t last = ((size_t)hi + 1u) * (size_t)s_trackGeomVertStride;

		GSPGPU_FlushDataCache(base + first, (u32)(last - first));
	}
}

/* Backface culling for the track (dumpreq 'bfcull', chord SELECT+Y): 0 = off
 * (draw every face), 1 = cull GPU back-faces, 2 = cull front-faces. Applied
 * per-frame in N3DS_TrackDraw_Begin, undone in _End. Default 1: culling
 * matches retail, is near-free, and removes erroneous back-facing geometry
 * (the Crash Cove second waterfall and the like). Safe because the bake gives
 * triangles and double-sided faces a reverse-wound copy (native_track_mesh.c)
 * so only single-sided quads are actually culled. */
int g_dbg_bfCull = 1;

void N3DS_TrackDraw_Begin(const float *qx, const float *qy, const float *qw, float h)
{
	C3D_TexEnv *env;

	/* Stereo journal: recorded at the TOP, mirroring the call itself rather
	 * than its effect -- the replay re-invokes this function, whose own
	 * guards and global reads (geomReady, selTrueDepth, depthTest state)
	 * re-evaluate identically within the same frame. Same pattern for every
	 * recorded call below. */
	{
		N3DS_StereoOp *op = N3DS_StereoRecord(N3DS_STEREO_OP_TRACK_BEGIN);

		if (op != NULL)
		{
			memcpy(op->u.mtx.qx, qx, sizeof(op->u.mtx.qx));
			memcpy(op->u.mtx.qy, qy, sizeof(op->u.mtx.qy));
			memcpy(op->u.mtx.qw, qw, sizeof(op->u.mtx.qw));
			op->u.mtx.h = h;
		}
	}

	s_trackDrawActive = 0;

	if (!s_trackGeomReady)
	{
		return;
	}

	C3D_BindProgram(&s_trackShaderProgram);
	C3D_SetAttrInfo(&s_trackAttrInfo);
	C3D_SetBufInfo(&s_trackBufInfo);

	/*
	 * Backface-cull A/B (dumpreq 'bfcull'), scoped to the TRACK only.
	 *
	 * Retail rejects back-facing SINGLE-SIDED faces per-face (GTE NCLIP winding
	 * test gated by the DOUBLE_SIDED bit, game/226 DrawLevelOvr); our bake emits
	 * every face regardless of winding and the GPU runs GPU_CULL_NONE globally
	 * (:1128), so we draw backfaces retail drops -- e.g. Crash Cove's second
	 * waterfall. This tries PICA hardware culling as the stand-in, restored to
	 * NONE in N3DS_TrackDraw_End so the packet/UI/model draws after the track are
	 * never affected. 1 vs 2 exists because the bake's winding convention is
	 * unverified: if 1 (cull back) punches holes, the bake winds the other way
	 * and 2 (cull front) is the correct one; if BOTH hole, winding is mixed and
	 * culling is unsafe without a bake normalization pass. Judge on: holes in the
	 * track (unsafe) vs the second waterfall vanishing cleanly + fps rising.
	 */
	if (g_dbg_bfCull == 1)
	{
		C3D_CullFace(GPU_CULL_BACK_CCW);
	}
	else if (g_dbg_bfCull == 2)
	{
		C3D_CullFace(GPU_CULL_FRONT_CCW);
	}

#ifdef CTR_NATIVE_OT_DEPTH
	/* State 2 (TRACKRO, see g_dbg_depthTest): the track still TESTS against
	 * depth -- so it can still be hidden by nearer packet content, which is
	 * what makes this a fair test rather than a "draw the track first" hack --
	 * but stops WRITING depth, so nothing the track draws can outrank a kart.
	 * Restored in N3DS_TrackDraw_End rather than left set: the merge flush
	 * immediately after assumes the frame-wide pair from N3DS_C3DBeginFrame. */
	if (g_dbg_depthTest == 2)
	{
		C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_COLOR);
	}
#endif

	/*
	 * Assert opaque blending explicitly rather than
	 * inheriting whatever the LAST packet draw left set. N3DS_C3DSetBlendMode
	 * memoizes through a static `lastMode` (below in this file) and this
	 * function never called it before, so track draws silently picked up
	 * additive/subtractive blend state on any frame containing sparks,
	 * shadows, or transparency, and plain opaque on frames without -- an
	 * intermittent, content-dependent bug that is exactly the worst kind for
	 * an A/B fps measurement (same toggle setting, different blend state,
	 * different-looking or differently-costed frame depending on what else
	 * was on screen a moment ago). Mode 0 is BM_NONE/opaque, matching this
	 * pass's actual content (untextured vertex-lit track geometry, rung 3a --
	 * see N3DS_TrackDraw_Begin's own header comment; rung 3b's textured
	 * draws stay opaque too, design 4.4's exclusions keep semi-trans faces on
	 * the packet path). No restore in N3DS_TrackDraw_End is needed for this:
	 * every packet emit re-asserts its OWN mode through this same memoized
	 * setter before it draws anything, so whatever N3DS_TrackDraw_Begin set
	 * here is always overwritten before it could affect packet content --
	 * don't "fix" this by adding a redundant restore.
	 */
	N3DS_C3DSetBlendMode(0);

	/* Depth writes are ON entering the bracket (N3DS_C3DBeginFrame's frame-wide
	 * pair, or state 2's read-only variant just above). The blend pass turns
	 * them off for its own draws and this memo tracks that; -1 rather than 1 so
	 * the first draw of the frame asserts explicitly instead of trusting an
	 * inherited state that a future change to BeginFrame could alter. */
	s_trackDepthWriteState = -1;

	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_trackULocQx, qx[0], qx[1], qx[2], qx[3]);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_trackULocQy, qy[0], qy[1], qy[2], qy[3]);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_trackULocQw, qw[0], qw[1], qw[2], qw[3]);
	{
		/* hParam.y/z select the track's depth axis, and they are driven by the
		 * SAME toggle that makes packets stamp sz3 (g_dbg_packetDepth,
		 * native_gpu.c). One switch on purpose: with only one side flipped the
		 * two geometry sources sit on different axes and the result is worse
		 * than either alone, which is exactly the sort of half-state that
		 * costs an owner hardware sitting to diagnose.
		 *
		 * z is supplied as 1-y so the shader needs no subtraction (PICA200 has
		 * no SUB opcode). Capture-off builds pin this to the ordinal path, so
		 * they behave exactly as they do today. */
		float selTrueDepth = 0.0f;

#if defined(CTR_NATIVE_GTE_CAPTURE) && CTR_NATIVE_GTE_CAPTURE
		{
			extern int g_dbg_packetDepth;

			selTrueDepth = (g_dbg_packetDepth != 0) ? 1.0f : 0.0f;
		}
#endif

		/* hParam.w: drawOrder tie-break weight, in view.z units per drawOrder
		 * step (docs/GROUND_DECAL_PASS_DESIGN.md 3/4). Was the
		 * `bucketToDepth` shader constant at 1.0; moved onto the uniform so
		 * the sweep is dumpreq `dorder N` instead of a rebuild per leg, for
		 * the same reason the depth-axis selector above is a uniform: this
		 * has to be judged by eye on the content that shows it, and a rebuild
		 * between legs is how a confounded comparison happens.
		 *
		 * 1.0 restores the shipped behaviour exactly. 0.0 removes the term,
		 * which is the phase-0 measurement: the weight is added to TRACK
		 * faces only, so ground faces (negative drawOrder) are pulled toward
		 * the camera while packet decals riding that same surface -- kart
		 * shadows, skid marks, tires -- carry no drawOrder and no equivalent
		 * term, and sit that far behind the road. If they reappear at 0, that
		 * mechanism is confirmed and the fix is a decal-side offset, NOT
		 * shipping 0 here (baked road markings may shimmer without it).
		 *
		 * That ablation RAN and came back negative: the
		 * decals stayed buried at 0, so this term is not the cause and the
		 * default is back at the shipped 1.0. See GROUND_DECAL_PASS_DESIGN.md
		 * 3 for what replaced it (the unbracketed ×4 regime axis). */
		C3D_FVUnifSet(GPU_VERTEX_SHADER, s_trackULocH, h, selTrueDepth, 1.0f - selTrueDepth, s_trackDrawOrderWeight);

		/* Prove the selector actually REACHES the shader. Every depth number
		 * logged so far is CPU-side arithmetic showing what the value would be,
		 * which is not evidence the GPU used it -- and "the fix changed almost
		 * nothing visible" is exactly what an unbound uniform looks like. uLoc
		 * is included because -1 means shaderInstanceGetUniformLocation never
		 * found it, in which case every C3D_FVUnifSet above has been writing
		 * into nowhere. */
		{
			static float s_lastSel = -1.0f;

			if (s_lastSel != selTrueDepth)
			{
				char msg[96];

				snprintf(msg, sizeof(msg), "[CTR-3DS] track depth sel=%d uLocH=%d uLocDepth=%d", (int)selTrueDepth, s_trackULocH, s_trackULocDepthRange);
				N3DS_DebugLog(msg);
				s_lastSel = selTrueDepth;
			}
		}
	}
	/* SAME ortho matrix the packet path uses (n3ds_backend.c's s_projection,
	 * computed by N3DS_C3DSetProjection) -- this is what puts track content
	 * and packet content on one shared depth axis (see the shader's own
	 * header comment for the row3/outpos.w reasoning this depends on). */
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_trackULocProjection, &s_projection);

	/* Current eye's parallax (zero in mono). Per bracket, not per frame:
	 * the right-eye replay re-enters this function with s_stereoEye = 1. */
	N3DS_StereoUploadUniform(s_trackULocStereo, (const float(*)[4])s_stereoVecWorld);

	/*
	 * Vertex-color-only output: 3a is untextured (color_hi is already baked
	 * per vertex -- the familiar "textureless mode" look). Deliberately NOT
	 * a rebind of the packet path's own reserved white decode-cache cell
	 * (N3DS_MERGE_UNTEX_BIN/N3DS_WHITE_ATLAS, native_renderer_3ds.c) even
	 * though that is this renderer's existing precedent for "untextured":
	 * that cell is addressed by a plain atlas-index constant defined in a
	 * DIFFERENT translation unit (the unity build), and hardcoding the same
	 * index here would be an unenforced cross-TU coupling this project's own
	 * TU/ABI boundary rule argues against -- if that index ever changed on
	 * the unity side, this file would silently sample the wrong atlas with
	 * no compile-time signal. GPU_REPLACE from GPU_PRIMARY_COLOR reaches the
	 * identical pixel result (texture0 is never sampled at all) without that
	 * coupling. N3DS_TrackDraw_End restores GPU_MODULATE.
	 */
	env = C3D_GetTexEnv(0);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_DirtyTexEnv(env);

	/* No PS1 2x "bright" modulate for untextured Gouraud content -- matches
	 * N3DS_MERGE_UNTEX_BIN's own choice of doubling=0 for the packet path's
	 * untextured bin. Rung 3b's textured draws are where the PS1 2x texel*
	 * color modulate belongs (design 12b), not here. */
	N3DS_C3DSetColorDoubling(0);

	/* The packet path owns texenv 0 between track frames, so whatever
	 * N3DS_TrackDraw_SetTextured last recorded is stale by now. */
	s_trackTexturedState = 0;

	s_trackDrawActive = 1;
}

/*
 * Rung 3b: flip the track program's combiner between textured and untextured.
 * Memoized -- a track frame issues one draw per (leaf x atlas unit) and most
 * consecutive draws share a state, so re-emitting the env every draw would be
 * pure command-buffer traffic on the platform this campaign exists to speed up.
 * The memo is reset by N3DS_TrackDraw_Begin, since the packet path owns texenv
 * 0 between frames and will have changed it.
 *
 * Colour doubling is part of the state, not incidental: PS1 texel*colour
 * modulate is 2x (design 12b), which is exactly why rung 3a deliberately left
 * doubling off for its untextured draws. Textured track content needs it back
 * or every texture reads half-brightness against retail.
 */
static void N3DS_TrackDraw_SetTextured(int textured)
{
	C3D_TexEnv *env;

	if (s_trackTexturedState == textured)
	{
		return;
	}

	env = C3D_GetTexEnv(0);
	if (textured)
	{
		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	}
	else
	{
		C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	}
	C3D_DirtyTexEnv(env);
	N3DS_C3DSetColorDoubling(textured ? 1 : 0);
	s_trackTexturedState = textured;
}

void N3DS_TrackDraw_Leaf(int firstIndex, int indexCount, float otIndexLo, float ordinalLo, float ordinalSpan, float invIndexSpan, int atlasBin, int blend, int uvDx, int uvDy)
{
	const u16 *indexBase;

	{
		N3DS_StereoOp *op = N3DS_StereoRecord(N3DS_STEREO_OP_TRACK_LEAF);

		if (op != NULL)
		{
			op->u.leaf.firstIndex = firstIndex;
			op->u.leaf.indexCount = indexCount;
			op->u.leaf.otIndexLo = otIndexLo;
			op->u.leaf.ordinalLo = ordinalLo;
			op->u.leaf.ordinalSpan = ordinalSpan;
			op->u.leaf.invIndexSpan = invIndexSpan;
			op->u.leaf.atlasBin = atlasBin;
			op->u.leaf.blend = blend;
			op->u.leaf.uvDx = uvDx;
			op->u.leaf.uvDy = uvDy;
		}
	}

	if (!s_trackDrawActive || (indexCount <= 0) || (firstIndex < 0))
	{
		return;
	}

	/*
	 * Rung 3b: one draw binds one texture, which is the entire reason the mesh
	 * builder splits a leaf's indices per atlas unit. A bin at or past the
	 * allocated unit count means "no placed texture for these faces" -- draw
	 * them untextured rather than binding a stale unit and sampling another
	 * rect's texels, which would look like garbage welded onto the track.
	 *
	 * Texture enable is a TEXTURE-COMBINER decision, not a vertex-shader one:
	 * the vertex program cannot branch cheaply, so it always computes the atlas
	 * UV and N3DS_TrackDraw_SetTextured picks the combiner setup that either
	 * samples it or ignores it.
	 *
	 * This used to say the selector "rides hParam.w". It does not: hParam.w is
	 * now the drawOrder tie-break weight (dumpreq `dorder` -- it named the
	 * long-removed `trkbias` lever until then, the same stale-comment failure
	 * twice over). Anyone trusting the old wording would have gone looking for
	 * a texture flag in a depth uniform -- and on a part whose uniform names
	 * are offsets into ONE shared physical register file, a wrong belief about
	 * who owns a component is exactly how a past projection collision
	 * happened. Caught by the parallel transparency work, whose own uvOffset
	 * was added as a NEW uniform rather than parked on a spare component for
	 * the same reason.
	 */
	if ((atlasBin >= 0) && (atlasBin < s_trackAtlasUnitsAllocated))
	{
		N3DS_TrackAtlas_Bind(atlasBin);
		N3DS_TrackDraw_SetTextured(1);
	}
	else
	{
		N3DS_TrackDraw_SetTextured(0);
	}

	/*
	 * Negate otIndexLo here, not on the caller's side: PICA200 has no SUB
	 * opcode (verified against picasso's own mnemonic list -- see the
	 * shader's header comment), so the shader computes "otIndex_v -
	 * otIndexLo" as an ADD against a pre-negated constant. Keeping that
	 * encoding choice inside this GPU-facing function, rather than pushing a
	 * pre-negated otIndexLo out through the plain-scalar API, is what keeps
	 * platform/native_track_frame.c's math a direct, literal reading of the
	 * design doc's formulas.
	 */
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_trackULocDepthRange, -otIndexLo, ordinalLo, ordinalSpan, invIndexSpan);

	/*
	 * SEMI-TRANSPARENCY (docs/TRANSPARENCY_DESIGN.md 3-4).
	 *
	 * blend is the rect's PSX ABR mode for a range the blend pass owns, or 3
	 * (TRACK_ABR_OPAQUE) for ordinary content. uvDx/uvDy are the TEXEL delta to
	 * the rect's PASS2 placement; the shader wants them in its own encoded units,
	 * which are 2x fixed point, so the doubling happens here -- next to the
	 * comment that says why -- rather than in the mesh builder, which thinks in
	 * texels throughout.
	 *
	 * Written on EVERY draw including opaque ones. An unset uniform component
	 * holds whatever the last program to use that physical register left there,
	 * and PICA uniforms are shared physical registers across programs, not
	 * per-program handles -- a past projection collision was exactly that
	 * mistake and it destroyed every packet drawn after a track draw.
	 */
	/* .z/.w unused (was the behind-eye depth-kick lever, removed); written
	 * every draw to avoid the stale-register issue the paragraph above
	 * describes. */
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_trackULocUvOffset, (float)(uvDx * 2), (float)(uvDy * 2), 0.0f, 0.0f);

	/*
	 * Blended ranges draw with their own PS1 blend mode and, critically, with
	 * depth WRITES OFF. Test stays on, so blended geometry is still correctly
	 * hidden by opaque things in front of it; but it must not write, or two
	 * blended surfaces would occlude each other by depth instead of compositing,
	 * and a waterfall would punch a hole in whatever is behind it.
	 *
	 * The caller guarantees every opaque range is issued before any blended one
	 * (platform/native_track_frame.c runs two passes), so this pair of state
	 * changes happens at most twice per frame, not per draw. Memoized anyway
	 * because "at most twice" is a property of the caller, and a future caller
	 * that interleaved would otherwise pay a GPU state change per draw silently.
	 */
	{
		const int wantDepthWrite = (blend == 3) ? 1 : 0;

		if (s_trackDepthWriteState != wantDepthWrite)
		{
			s_trackDepthWriteState = wantDepthWrite;
#ifdef CTR_NATIVE_OT_DEPTH
			/* Leave state 2 (TRACKRO) alone -- it already means "test but never
			 * write" for the whole track, which is a superset of what the blend
			 * pass needs, and stomping it here would silently disable the
			 * isolation diagnostic. */
			if (g_dbg_depthTest != 2)
			{
				C3D_DepthTest(true, GPU_LEQUAL, wantDepthWrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
			}
#endif
		}

		/* ABR -> BlendMode is +1, and that is not a guess: native_gpu.c:49 spells
		 * the packet path's own conversion as
		 *   #define GET_TPAGE_BLEND(tpage) ((BlendMode)(((tpage >> 5) & 3) + 1))
		 * -- the same field, the same arithmetic. The enum
		 * (include/platform/native_renderer_types.h) lines up term for term:
		 * BM_NONE=0, BM_AVERAGE=1 (ABR0, B/2+F/2), BM_ADD=2 (ABR1, B+F),
		 * BM_SUBTRACT=3 (ABR2, B-F), BM_ADD_QUATER_SOURCE=4 (ABR3). Track
		 * content and packet content therefore reach identical GPU blend state
		 * from identical source data, which is the only way the two can agree on
		 * a waterfall drawn half by each. */
		N3DS_C3DSetBlendMode((blend == 3) ? 0 : (blend + 1));
	}

	indexBase = (const u16 *)s_trackIndexPtr + firstIndex;
	C3D_DrawElements(GPU_TRIANGLES, indexCount, C3D_UNSIGNED_SHORT, indexBase);
}

void N3DS_TrackDraw_End(void)
{
	N3DS_StereoRecord(N3DS_STEREO_OP_TRACK_END);

	/* Program + attribute layout + vertex buffer + texture-modulate combiner
	 * -- N3DS_BindPacketDrawState's own body covers all four (it is the same
	 * function N3DS_C3DInit uses to build this state the first time), which
	 * is the full list of GPU state N3DS_TrackDraw_Begin changed away from
	 * the packet path's own shape. Unconditional: even if Begin no-op'd
	 * (geometry not ready, s_trackDrawActive never went true), restoring
	 * packet state here is always safe and cheap, and guarantees the packet
	 * path's merge-bin flush right after this call is never left pointed at
	 * track state by a call-order mistake upstream. */
	N3DS_BindPacketDrawState();

	/* Undo any track-scoped backface cull so the packet/UI/model/sky draws
	 * after the track are never culled (their winding is not guaranteed). Only
	 * when the toggle is live, so the default leg issues no redundant state
	 * change. Matches the global GPU_CULL_NONE set once in N3DS_C3DInit. */
	if (g_dbg_bfCull != 0)
	{
		C3D_CullFace(GPU_CULL_NONE);
	}

#ifdef CTR_NATIVE_OT_DEPTH
	/* Undo state 2's read-only depth mask. Unconditional on the state rather
	 * than mirrored off a flag set in Begin: Begin can return early (geometry
	 * not ready) having changed nothing, and re-asserting the frame-wide pair
	 * is idempotent, so this cannot desync the way a paired flag could. */
	if (g_dbg_depthTest == 2)
	{
		C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
	}
	else if (s_trackDepthWriteState == 0)
	{
		/* The blend pass turned depth writes off for its own draws. Restore the
		 * frame-wide pair before the packet path's merge flush runs, or every
		 * packet drawn after the track this frame would silently stop writing
		 * depth -- karts would then fail to occlude each other, on exactly the
		 * frames that happened to contain a waterfall. Conditioned on the memo
		 * rather than run unconditionally so a frame with no blended content
		 * issues no redundant state change at all. */
		C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
	}
#endif

	s_trackDepthWriteState = -1;
	s_trackDrawActive = 0;
}

#else /* !CTR_NATIVE_TRACK_RENDERER */

int N3DS_TrackGeom_Upload(const void *verts, int vertCount, int vertStride, const void *indices, int indexCount)
{
	(void)verts;
	(void)vertCount;
	(void)vertStride;
	(void)indices;
	(void)indexCount;
	return 0;
}

void N3DS_TrackGeom_Free(void)
{
}

void N3DS_TrackGeom_PatchUV(const unsigned short *vertIndices, const short *uvPairs, int count)
{
	(void)vertIndices;
	(void)uvPairs;
	(void)count;
}

void N3DS_TrackGeom_PatchPosColor(const unsigned short *vertIndices, const short *xyz, const unsigned char *rgb, int count)
{
	(void)vertIndices;
	(void)xyz;
	(void)rgb;
	(void)count;
}

void N3DS_TrackDraw_Begin(const float *qx, const float *qy, const float *qw, float h)
{
	(void)qx;
	(void)qy;
	(void)qw;
	(void)h;
}

void N3DS_TrackDraw_Leaf(int firstIndex, int indexCount, float otIndexLo, float ordinalLo, float ordinalSpan, float invIndexSpan, int atlasBin, int blend, int uvDx, int uvDy)
{
	(void)firstIndex;
	(void)indexCount;
	(void)otIndexLo;
	(void)ordinalLo;
	(void)ordinalSpan;
	(void)invIndexSpan;
	(void)atlasBin;
	(void)blend;
	(void)uvDx;
	(void)uvDy;
}

void N3DS_TrackDraw_End(void)
{
}

#endif /* CTR_NATIVE_TRACK_RENDERER */

#ifdef CTR_NATIVE_MODEL_RENDERER
/*
 * Rung 2 model draw bracket. Mirrors N3DS_TrackDraw_* in shape; the differences
 * are all consequences of models being per-frame dynamic rather than resident.
 */
void N3DS_ModelDraw_Begin(void)
{
	/*
	 * Start from the packet path's own state, then swap only the program.
	 * BindPacketDrawState installs the attribute layout AND the buffer binding
	 * model vertices were appended into (N3DS_C3DAppendVertices writes the
	 * shared per-frame buffer), so this is not a lazy shortcut -- it is the
	 * only way to be sure the layout matches what wrote the data.
	 */
	N3DS_StereoRecord(N3DS_STEREO_OP_MODEL_BEGIN);

	N3DS_BindPacketDrawState();
	C3D_BindProgram(&s_modelShaderProgram);

	/*
	 * The SAME ortho matrix the packet path uses. The model shader declares
	 * projection[4] first precisely so it occupies the registers ctr.v.pica
	 * expects, but "the registers hold the right value" is a separate claim
	 * from "the registers are where we think" -- upload it explicitly rather
	 * than inheriting whatever the last bound program left, which is the class
	 * of assumption that produced a past uniform-collision bug.
	 */
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_modelULocProjection, &s_projection);

	/* Current eye's parallax (zero in mono). The model program's `stereo`
	 * register (f8) is ctr_track's depthRange, clobbered every track leaf --
	 * this bracket always opens after the track bracket closed, so this
	 * per-bracket upload is exactly what makes that safe. */
	N3DS_StereoUploadUniform(s_modelULocStereo, (const float(*)[4])s_stereoVecWorld);

	/*
	 * Assert opaque blending, for the reason spelled out at length in
	 * N3DS_TrackDraw_Begin: N3DS_C3DSetBlendMode memoizes, so without this the
	 * model pass silently inherits additive or subtractive state from whatever
	 * packet content happened to draw last -- an intermittent, content-
	 * dependent difference, which is the worst kind to meet during an fps A/B.
	 */
	N3DS_C3DSetBlendMode(0);

	/* The packet path owns texenv 0 and the atlas binding between model
	 * frames, so both memos are stale by now. */
	s_modelTexturedState = -1;
	s_modelBoundAtlas = -1;
	s_modelDepthWriteState = -1;
	s_modelDrawActive = 1;
}

void N3DS_ModelDraw_Instance(const float *qx, const float *qy, const float *qw, float h)
{
	{
		N3DS_StereoOp *op = N3DS_StereoRecord(N3DS_STEREO_OP_MODEL_INSTANCE);

		if (op != NULL)
		{
			memcpy(op->u.mtx.qx, qx, sizeof(op->u.mtx.qx));
			memcpy(op->u.mtx.qy, qy, sizeof(op->u.mtx.qy));
			memcpy(op->u.mtx.qw, qw, sizeof(op->u.mtx.qw));
			op->u.mtx.h = h;
		}
	}

	if (!s_modelDrawActive)
	{
		return;
	}

	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_modelULocQx, qx[0], qx[1], qx[2], qx[3]);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_modelULocQy, qy[0], qy[1], qy[2], qy[3]);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_modelULocQw, qw[0], qw[1], qw[2], qw[3]);

	/* Only .x is read by the shader; the other three are written because
	 * leaving uniform components unset means "whatever the last program that
	 * used this register put there". */
	/* .y/.z/.w unused (was the instance depth-bias lever, removed); written
	 * every draw to avoid the stale-register issue noted above. */
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_modelULocH, h, 0.0f, 0.0f, 0.0f);
}

/*
 * Textured/untextured combiner, memoized. 35% of a real frame's model triangles
 * are untextured (measured, design section 7), so both states are hot and the
 * memo earns its keep.
 *
 * Colour doubling is part of the state: PS1 texel*colour modulate is 2x, which
 * is why the untextured leg turns it off -- the same pairing N3DS_TrackDraw_
 * SetTextured makes, and for the same reason (without it every model texture
 * reads half-brightness against retail).
 */
static void N3DS_ModelDraw_SetTextured(int textured)
{
	C3D_TexEnv *env;

	if (s_modelTexturedState == textured)
	{
		return;
	}

	env = C3D_GetTexEnv(0);
	if (textured)
	{
		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	}
	else
	{
		C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	}
	C3D_DirtyTexEnv(env);
	N3DS_C3DSetColorDoubling(textured ? 1 : 0);
	s_modelTexturedState = textured;
}

void N3DS_ModelDraw_Run(int startVertex, int triangleCount, int atlasIndex, int textured, int blend)
{
	{
		N3DS_StereoOp *op = N3DS_StereoRecord(N3DS_STEREO_OP_MODEL_RUN);

		if (op != NULL)
		{
			op->u.mrun.startVertex = startVertex;
			op->u.mrun.triCount = triangleCount;
			op->u.mrun.atlas = atlasIndex;
			op->u.mrun.textured = textured;
			op->u.mrun.blend = blend;
		}
	}

	if (!s_modelDrawActive || (triangleCount <= 0) || (startVertex < 0))
	{
		return;
	}

	/*
	 * Semi-transparency (docs/TRANSPARENCY_DESIGN.md 5). blend is a BlendMode:
	 * 0 = opaque, 1..4 the PSX modes. A blended run is the PASS2 half of a
	 * semi-transparent face and must not WRITE depth -- its PASS1 half already
	 * wrote it in pass 0, and letting the blended half write again would make two
	 * overlapping translucent surfaces occlude rather than composite.
	 *
	 * The caller issues every opaque run before any blended one, so this pair of
	 * state changes happens at most twice per frame. Memoized regardless, because
	 * "at most twice" is a property of the caller and a future interleaving one
	 * would otherwise pay a GPU state change per run without anyone noticing.
	 */
	{
		const int wantDepthWrite = (blend == 0) ? 1 : 0;

		if (s_modelDepthWriteState != wantDepthWrite)
		{
			s_modelDepthWriteState = wantDepthWrite;
#ifdef CTR_NATIVE_OT_DEPTH
			C3D_DepthTest(true, GPU_LEQUAL, wantDepthWrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
#endif
		}

		N3DS_C3DSetBlendMode(blend);
	}

	if (textured && (atlasIndex >= 0) && (atlasIndex < N3DS_ATLAS_COUNT))
	{
		/* Bind only on change. The caller sorts a frame's triangles into
		 * texture-homogeneous runs, so consecutive runs frequently share an
		 * atlas across instances and this skips most of the binds. */
		if (s_modelBoundAtlas != atlasIndex)
		{
			N3DS_C3DBindAtlas(atlasIndex);
			s_modelBoundAtlas = atlasIndex;
		}
		N3DS_ModelDraw_SetTextured(1);
	}
	else
	{
		/* Out-of-range atlas draws untextured rather than binding a stale unit
		 * and sampling another texture's texels, which would read as garbage
		 * welded onto a kart. Same rule N3DS_TrackDraw_Leaf applies. */
		N3DS_ModelDraw_SetTextured(0);
	}

	C3D_DrawArrays(GPU_TRIANGLES, startVertex, triangleCount * 3);
}

void N3DS_ModelDraw_End(void)
{
	N3DS_StereoRecord(N3DS_STEREO_OP_MODEL_END);

	/* Restores program, attribute layout, buffer binding and the modulate
	 * combiner -- the full set this bracket touched. Unconditional for the
	 * same reason N3DS_TrackDraw_End is: cheap, idempotent, and it cannot
	 * desync if Begin returned early. */
	N3DS_BindPacketDrawState();

#ifdef CTR_NATIVE_OT_DEPTH
	/* The blend pass turned depth writes off. Restore the frame-wide pair before
	 * anything else draws, or every packet issued after the models this frame
	 * would silently stop writing depth -- and only on frames that happened to
	 * contain a particle or a shadow, which is the intermittent, content-
	 * dependent kind of bug that costs a hardware sitting to find. */
	if (s_modelDepthWriteState == 0)
	{
		C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
	}
#endif

	s_modelDrawActive = 0;
	s_modelTexturedState = -1;
	s_modelBoundAtlas = -1;
	s_modelDepthWriteState = -1;
}
#else
void N3DS_ModelDraw_Begin(void)
{
}

void N3DS_ModelDraw_Instance(const float *qx, const float *qy, const float *qw, float h)
{
	(void)qx;
	(void)qy;
	(void)qw;
	(void)h;
}

void N3DS_ModelDraw_Run(int startVertex, int triangleCount, int atlasIndex, int textured, int blend)
{
	(void)startVertex;
	(void)triangleCount;
	(void)atlasIndex;
	(void)textured;
	(void)blend;
}

void N3DS_ModelDraw_End(void)
{
}
#endif /* CTR_NATIVE_MODEL_RENDERER */

/*
 * PS1 semi-transparency (the four ABR modes) via PICA's fixed-function blend
 * unit -- the same equation/factor mapping PC uses in GL
 * (NativeRenderer_SetBlendMode, native_renderer.c), including its constant-
 * color trick: one blend color of RGB=0.25 / A=0.5 (set once in N3DS_C3DInit)
 * serves both AVERAGE (constant ALPHA carries the 1/2) and ADD_QUARTER
 * (constant COLOR carries the 1/4).
 *
 * BM_NONE is blending "enabled" with ONE/ZERO rather than a switch to
 * GPU logic-op mode: numerically identical to opaque, and it keeps the
 * fragment pipeline in one mode instead of toggling GPUREG_COLOR_OPERATION
 * per split.
 *
 * Deduped against the last-applied mode: C3D state persists across frames,
 * so a redundant call would only dirty state for nothing. citro3d records a
 * state change into the command list at the NEXT draw, and the whole frame
 * is one deferred command list, so calling this between draws mid-frame
 * takes effect exactly at the following draw -- same ordering contract as
 * every other state setter here.
 *
 * Alpha channel stays ONE/ZERO in every mode (PC does the same via
 * glBlendFuncSeparate): the framebuffer alpha carries nothing this renderer
 * reads, and blending it with the RGB factors could only corrupt it.
 */
/*
 * The right eye: replay the frame's journal against the right target with the
 * right eye's parallax vectors. Called by NativeRenderer_EndScene after every
 * left-eye draw (including the debug atlas inspector, which deliberately
 * stays left-eye-only) and immediately before N3DS_C3DEndFrame submits the
 * whole two-eye command list. No-op in mono.
 *
 * Track/model ops re-enter the same public functions, whose own guards,
 * memos and global reads re-evaluate identically within the frame -- the
 * only thing that differs is s_stereoEye, which their Begin uploads read.
 * Packet/VRAM ops re-issue resolved state inline: doubling and depth-write
 * were resolved at record time (policy ran once, on the left eye), so the
 * replay is policy-free. Both are draws from (baseVertex, count) into the
 * SAME persistent frame VBO the left eye drew from -- nothing is
 * re-appended, so the vertex budget is untouched.
 *
 * On journal overflow the tail is dropped from the RIGHT eye only, loudly;
 * the defensive re-assert at the bottom covers a prefix that ends inside a
 * track/model bracket.
 */
void N3DS_StereoRenderRightEye(void)
{
	int i;
	int lastDoubling = -1;
	int lastDepthWrite = 1;

	if (!s_stereoFrameActive || (s_renderTargetRight == NULL))
	{
		return;
	}

	s_stereoRecording = 0;
	s_stereoEye = 1;

	C3D_FrameDrawOn(s_renderTargetRight);
	/* Same pillarbox crop (or widescreen lift) as N3DS_C3DBeginFrame --
	 * viewport state does not automatically follow the framebuffer binding. */
	C3D_SetViewport(0, g_dbg_widescreen ? 0 : N3DS_TOP_CONTENT_X, 240, g_dbg_widescreen ? 400 : N3DS_TOP_CONTENT_W);
#ifdef CTR_NATIVE_OT_DEPTH
	/* Frame-wide depth pair, mirroring BeginFrame's policy exactly --
	 * whatever state the left eye's last draw left behind is not it. */
	if (g_dbg_depthTest == 1)
	{
		C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
	}
	else
	{
		C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
	}
#endif
	/* Packet program + layout + buffer + combiner + projection + the RIGHT
	 * eye's packet parallax (s_stereoEye is 1 now, so the upload inside
	 * picks the right vector). */
	N3DS_BindPacketDrawState();

	if (s_stereoOverflow)
	{
		static int s_overflowLogged = 0;

		if (!s_overflowLogged)
		{
			s_overflowLogged = 1;
			N3DS_DebugLog("[CTR-3DS] stereo: journal OVERFLOW, right eye truncated (raise N3DS_STEREO_MAX_OPS)");
		}
	}

	for (i = 0; i < s_stereoOpCount; i++)
	{
		const N3DS_StereoOp *op = &s_stereoOps[i];

		switch (op->kind)
		{
		case N3DS_STEREO_OP_PKT:
		{
			if ((int)op->u.pkt.doubling != lastDoubling)
			{
				N3DS_C3DSetColorDoubling((int)op->u.pkt.doubling);
				lastDoubling = (int)op->u.pkt.doubling;
			}
			if ((int)op->u.pkt.depthWrite != lastDepthWrite)
			{
				N3DS_C3DSetDepthWrite((int)op->u.pkt.depthWrite);
				lastDepthWrite = (int)op->u.pkt.depthWrite;
			}
			N3DS_C3DSetBlendMode((int)op->u.pkt.blend);
			N3DS_C3DBindAtlas((int)op->u.pkt.atlas);
			C3D_DrawArrays(GPU_TRIANGLES, op->u.pkt.baseVertex, op->u.pkt.vertCount);
			break;
		}
		case N3DS_STEREO_OP_VRAM:
		{
			/* Mirrors PresentVRAMRect's own draw: doubling off, dedicated
			 * texture, quad at N3DS_DEPTH_TOPMOST (zero parallax via the
			 * shader's UI-band mask, so both eyes see it at screen depth).
			 * The slot's texels persist to frame end -- see the header. */
			if (lastDoubling != 0)
			{
				N3DS_C3DSetColorDoubling(0);
				lastDoubling = 0;
			}
			N3DS_C3DBindVramDisplayTex((int)op->u.pkt.atlas);
			C3D_DrawArrays(GPU_TRIANGLES, op->u.pkt.baseVertex, op->u.pkt.vertCount);
			break;
		}
		case N3DS_STEREO_OP_TRACK_BEGIN:
		{
			N3DS_TrackDraw_Begin(op->u.mtx.qx, op->u.mtx.qy, op->u.mtx.qw, op->u.mtx.h);
			lastDoubling = -1; /* the bracket owns doubling/blend/depth-write */
			break;
		}
		case N3DS_STEREO_OP_TRACK_LEAF:
		{
			N3DS_TrackDraw_Leaf(op->u.leaf.firstIndex, op->u.leaf.indexCount, op->u.leaf.otIndexLo, op->u.leaf.ordinalLo, op->u.leaf.ordinalSpan,
			                    op->u.leaf.invIndexSpan, op->u.leaf.atlasBin, op->u.leaf.blend, op->u.leaf.uvDx, op->u.leaf.uvDy);
			break;
		}
		case N3DS_STEREO_OP_TRACK_END:
		{
			N3DS_TrackDraw_End();
			lastDoubling = -1;
			lastDepthWrite = 1; /* End restores the frame-wide WRITE_ALL pair */
			break;
		}
		case N3DS_STEREO_OP_MODEL_BEGIN:
		{
			N3DS_ModelDraw_Begin();
			lastDoubling = -1;
			break;
		}
		case N3DS_STEREO_OP_MODEL_INSTANCE:
		{
			N3DS_ModelDraw_Instance(op->u.mtx.qx, op->u.mtx.qy, op->u.mtx.qw, op->u.mtx.h);
			break;
		}
		case N3DS_STEREO_OP_MODEL_RUN:
		{
			N3DS_ModelDraw_Run(op->u.mrun.startVertex, op->u.mrun.triCount, op->u.mrun.atlas, op->u.mrun.textured, op->u.mrun.blend);
			break;
		}
		case N3DS_STEREO_OP_MODEL_END:
		{
			N3DS_ModelDraw_End();
			lastDoubling = -1;
			lastDepthWrite = 1;
			break;
		}
		default:
		{
			break;
		}
		}
	}

	/* Defensive: a truncated journal can end inside a bracket, leaving the
	 * track/model program bound. Cheap, idempotent, and the next BeginFrame
	 * re-asserts the rest of the frame-wide state regardless. */
	if (s_stereoOverflow)
	{
		N3DS_BindPacketDrawState();
#ifdef CTR_NATIVE_OT_DEPTH
		if (g_dbg_depthTest != 1)
		{
			C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
		}
#endif
	}

	/* s_stereoEye stays 1 until the next frame's latch resets it -- nothing
	 * else draws between here and N3DS_C3DEndFrame. */
}

/* Depth-WRITE only; the test itself stays LEQUAL so translucent content is
 * still correctly hidden by anything in front of it. Callers memoize (see
 * N3DS_MergeEmit), so this is not gated again here. Under a build without
 * OT_DEPTH there is no depth buffer to mask and this is a no-op. */
void N3DS_C3DSetDepthWrite(int enable)
{
#ifdef CTR_NATIVE_OT_DEPTH
	C3D_DepthTest(true, GPU_LEQUAL, enable ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
#else
	(void)enable;
#endif
}

void N3DS_C3DSetBlendMode(int blendMode)
{
	static int lastMode = -1;

	if (blendMode == lastMode)
	{
		return;
	}
	lastMode = blendMode;

	switch (blendMode)
	{
	default:
	case 0:
	{ /* BM_NONE: opaque */
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
		break;
	}
	case 1:
	{ /* BM_AVERAGE: 0.5*dst + 0.5*src, via constant alpha = 0.5 */
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA, GPU_ONE, GPU_ZERO);
		break;
	}
	case 2:
	{ /* BM_ADD: dst + src */
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ZERO);
		break;
	}
	case 3:
	{ /* BM_SUBTRACT: dst - src (reverse-subtract, same as PC's GL_FUNC_REVERSE_SUBTRACT) */
		C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ZERO);
		break;
	}
	case 4:
	{ /* BM_ADD_QUATER_SOURCE: dst + 0.25*src, via constant color = 0.25 */
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_COLOR, GPU_ONE, GPU_ONE, GPU_ZERO);
		break;
	}
	}
}

void N3DS_C3DUploadVramDisplayChunk(int slotIndex, const void *tiled256)
{
	if ((slotIndex < 0) || (slotIndex >= N3DS_VRAM_DISPLAY_TEX_COUNT))
	{
		return;
	}

	C3D_TexUpload(&s_vramDisplayTex[slotIndex], tiled256);
}

void N3DS_C3DBindVramDisplayTex(int slotIndex)
{
	if ((slotIndex < 0) || (slotIndex >= N3DS_VRAM_DISPLAY_TEX_COUNT))
	{
		return;
	}

	C3D_TexBind(0, &s_vramDisplayTex[slotIndex]);
}

#ifdef CTR_INTERNAL
extern int g_dbg_texturelessMode;
/* Defined in the unity TU (native_renderer_3ds.c) -- the cache arrays live
 * there and this TU can't see them (deliberate TU split, see the file
 * header). void(void), so no ABI hazard across the enum-width boundary. */
void NativeRenderer3DS_DebugDumpCacheMeta(void);

void NativeRenderer3DS_DebugArmTriangleDump(void);

/* Runs the full SELECT+R capture set: emulated VRAM, every atlas's raw
 * storage, and both decode-cache tiers' metadata, all describing the same
 * instant, plus arms a per-triangle dump of the NEXT frame (page/clut/UV/
 * position/atlas-resolution per triangle -- the addressing-side complement
 * to the content-side dumps; content was proven byte-correct while
 * the screen quilts, so the divergence must be visible here). One entry
 * point so the button gesture and the host-side file trigger below can
 * never drift apart in what they capture. */
static void N3DS_DebugCaptureAll(void)
{
	N3DS_DebugWriteVRAMDump();
	N3DS_DebugWriteAtlasDump();
	NativeRenderer3DS_DebugDumpCacheMeta();
	NativeRenderer3DS_DebugArmTriangleDump();
}
#endif

unsigned int N3DS_ReadButtons(void)
{
	unsigned int out = 0;
	u32 held;

	hidScanInput();
	held = hidKeysHeld();

#ifdef CTR_INTERNAL
	/* SELECT+R freed (the VRAM/debug capture set). Still reachable as dumpreq `capture`. */

	/* Host-scriptable trigger: Azahar's SD card is a plain host
	 * folder, so an autonomous session can drive diagnostics by creating
	 * sdmc:/dumpreq.txt -- no window focus or key injection involved. The
	 * file's first word selects the action (the A/B toggles need
	 * to be reachable without a gamepad):
	 *   capture (or anything else) -- the full SELECT+R capture set
	 *   subpage     -- toggle g_dbg_disableSubPage (route all to full-page tier)
	 *   single      -- toggle g_dbg_forceSingleTexture
	 *   textureless -- toggle g_dbg_texturelessMode
	 *   triscopes   -- toggle g_dbg_perfTriScopes (per-triangle perf split; taxed)
	 *   drawmerge   -- toggle g_dbg_drawMerge (CTR_NATIVE_DRAW_MERGE builds only)
	 *   flagstyle [N] -- menu/loading flag: 0 = retail grey checkers (default),
	 *                  1 = pride stripes, 2 = purple/white (Spyro). Bare token
	 *                  cycles. Also reachable in-game by cheat: WONDER, SOAR.
	 *   renderthread -- toggle g_dbg_renderThread (core-2 render worker routing;
	 *                  OFF falls back to synchronous main-thread rendering)
	 *   lodgeo [N]  -- track LOD distance, percent of retail (CTR_NATIVE_LOD_BIAS)
	 *   lodtex [N]  -- texture LOD distance, percent of retail (same gate)
	 *   fbtest      -- toggle g_dbg_skipFbTriTest (skip resolve's
	 *                  per-triangle fb-mirror test; freeze frames stale while ON)
	 *   otz         -- toggle g_dbg_otzWiden (widened depth stamps, track-
	 *                  renderer rung 1; [OTZ] occupancy line while on)
	 *   trackrender [N] -- native track renderer state (CTR_NATIVE_TRACK_RENDERER
	 *                  builds only): 0=off(retail) 1=on(resident replaces retail)
	 *                  2=both(diagnostic overlay). Bare token cycles 0->1->2->0;
	 *                  'trackrender N' sets a specific state. Also reachable on
	 *                  hardware via SELECT+ZR (a dumpreq file can't be placed
	 *                  mid-race there -- see the button handler below).
	 *   packetdepth -- toggle g_dbg_packetDepth (depth model): packets stamp
	 *                  GTE sz3 and the track shader switches to the same
	 *                  true-depth axis, both at once. Hardware button: SELECT+L.
	 *   gtecapture [N] -- g_dbg_gteCapture (CTR_NATIVE_GTE_CAPTURE builds):
	 *                  1=full 2=store-only(no matrix note) 0=ablated, to price
	 *                  the capture subsystem and its matrix probe separately.
	 *                  Bare token cycles 1->2->0. Hardware button: SELECT+D-UP.
	 *   strictmtx   -- toggle g_dbg_pktDepthStrictMtx: a primitive whose
	 *                  vertices resolve to two different matrix states is
	 *                  treated as a miss instead of trusting a foreign depth.
	 *   missordinal -- toggle g_dbg_pktDepthMissOrdinal: on a capture
	 *                  miss keep the emit ordinal instead of deriving depth
	 *                  from the OT bucket. Expect animated water over the HUD.
	 *   noevict     -- toggle g_dbg_gteNoEvict: a capture store may not
	 *                  overwrite a live entry owned by a different matrix.
	 *   stereo      -- toggle g_dbg_stereo (master 3D enable; the physical slider
 *                  still gates frame by frame, this just forces mono)
 *   stereowindow -- toggle window mode (default OFF per owner: pop-out is
 *                  cooler; ON = no pop-out, world starts at the screen)
 *   stereosep [N] -- full-slider disparity at infinite depth, PSX px
 *                  (default 12; negative swaps the eyes; bare resets)
 *   stereoconv [N] -- screen-plane depth, view.z units (default 1200;
 *                  bare resets)
 *   depthtest [N] -- g_dbg_depthTest, the occlusion split: 0=normal
	 *                  1=no depth test (painter's order) 2=track tests but
	 *                  does not write depth. Bare token cycles 0->1->2->0.
	 *                  Hardware button: SELECT+ZL.
	 *   wide [N]    -- g_dbg_widescreen: bare toggles off<->REAL (mode 2,
	 *                  wide FOV window at 4:3 pixel scale); `wide 1` is the
	 *                  rejected anamorphic stretch. Button: SELECT+D-DOWN.
	 *   padmux [N]  -- virtual multitap (default OFF -- renderer can't do
	 *                  split-screen, and off keeps VS/battle behind retail's
	 *                  own controller gate): `padmux N` drives pad N with the
	 *                  3DS buttons, 0 = off (retail single-pad bus), bare =
	 *                  cycle the driven pad. Button: SELECT+ZR.
	 * Polled every 64 frames so the SD probe cost stays negligible on real
	 * hardware; the request file is deleted first so one request fires once. */
	{
		static unsigned int s_dumpPollCountdown = 64;

		if (--s_dumpPollCountdown == 0)
		{
			FILE *req = fopen("sdmc:/dumpreq.txt", "rb");

			s_dumpPollCountdown = 64;
			if (req != NULL)
			{
				extern int g_dbg_disableSubPage;
				extern int g_dbg_forceSingleTexture;
				/* 24, was 16: "stereoconv 12000" (16 chars) no longer fits a
				 * 15-char payload. Everything else is unchanged. */
				char word[24];
				size_t got = fread(word, 1, sizeof(word) - 1, req);

				word[got] = '\0';
				fclose(req);
				remove("sdmc:/dumpreq.txt");

				if (strncmp(word, "widemask", 8) == 0)
				{
					/* Strips-hunt kill switches: bit 1 = instance bbox
					 * margin, 2 = per-tri margin, 4 = sky face margin,
					 * 8 = sky extra sectors. `widemask N` (decimal 0-15)
					 * sets; bare resets to all-on. MUST be matched before
					 * the `wide` token -- shared prefix. */
					extern int g_dbg_wideMarginMask;
					char msg[48];

					g_dbg_wideMarginMask = (word[8] == ' ') ? (atoi(&word[9]) & 0xF) : 0xF;
					snprintf(msg, sizeof(msg), "[CTR-3DS] wide margin mask: 0x%X", (unsigned)g_dbg_wideMarginMask);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "wide", 4) == 0)
				{
					/* Bare token toggles off<->REAL. `wide N` (0/1/2) sets a
					 * mode directly -- 1 is the rejected anamorphic stretch,
					 * kept reachable only this explicit way. */
					if (word[4] == ' ' && word[5] >= '0' && word[5] <= '2')
					{
						g_dbg_widescreen = word[5] - '0';
					}
					else
					{
						g_dbg_widescreen = (g_dbg_widescreen != 0) ? 0 : 2;
					}
					N3DS_DebugLog((g_dbg_widescreen == 2)   ? "[CTR-3DS] widescreen: REAL (wide FOV window, 4:3 pixel scale)"
					              : (g_dbg_widescreen == 1) ? "[CTR-3DS] widescreen: STRETCH (anamorphic)"
					                                        : "[CTR-3DS] widescreen: off (4:3 crop)");
				}
				else if (strncmp(word, "framesync", 9) == 0)
				{
					g_dbg_frameSync ^= 1;
					N3DS_DebugLog(g_dbg_frameSync ? "[CTR-3DS] framesync: ON (FrameBegin waits for hw vblank)"
					                              : "[CTR-3DS] framesync: OFF (begin immediately)");
				}
				else if (strncmp(word, "sdlog", 5) == 0)
				{
					g_dbg_sdLog ^= 1;
					N3DS_DebugLog(g_dbg_sdLog ? "[CTR-3DS] sdlog: ON (writing ctr3ds.log -- costs a ~2s hitch)"
					                          : "[CTR-3DS] sdlog: off (no SD writes; bottom screen still live)");
				}
				else if (strncmp(word, "fastpresent", 11) == 0)
				{
					g_dbg_fastPresent ^= 1;
					N3DS_DebugLog(g_dbg_fastPresent ? "[CTR-3DS] fastpresent: ON (flush bottom framebuffer only)"
					                                : "[CTR-3DS] fastpresent: off (whole-system gfxFlushBuffers)");
				}
				else if (strncmp(word, "subpage", 7) == 0)
				{
					void NativeRenderer3DS_DebugInvalidateCaches(void);

					g_dbg_disableSubPage ^= 1;
					/* Both tiers share physical cells while the diagnostic
					 * bound is widened -- drop all cached decodes so neither
					 * regime serves the other's overwrites. */
					NativeRenderer3DS_DebugInvalidateCaches();
					N3DS_DebugLog(g_dbg_disableSubPage ? "[CTR-3DS] sub-page tier: DISABLED (full-page only)" : "[CTR-3DS] sub-page tier: enabled");
				}
				else if (strncmp(word, "single", 6) == 0)
				{
					g_dbg_forceSingleTexture ^= 1;
					N3DS_DebugLog(g_dbg_forceSingleTexture ? "[CTR-3DS] force single texture: ON" : "[CTR-3DS] force single texture: off");
				}
				else if (strncmp(word, "textureless", 11) == 0)
				{
					g_dbg_texturelessMode ^= 1;
					N3DS_DebugLog(g_dbg_texturelessMode ? "[CTR-3DS] textureless mode: 1" : "[CTR-3DS] textureless mode: 0");
				}
				else if (strncmp(word, "triscopes", 9) == 0)
				{
					extern int g_dbg_perfTriScopes;

					g_dbg_perfTriScopes ^= 1;
					N3DS_DebugLog(g_dbg_perfTriScopes ? "[CTR-3DS] per-tri perf scopes: ON (syscall tax ~10k/frame)"
					                                  : "[CTR-3DS] per-tri perf scopes: off");
				}
#ifdef CTR_NATIVE_DRAW_MERGE
				else if (strncmp(word, "drawmerge", 9) == 0)
				{
					extern int g_dbg_drawMerge;

					/* Latched at BeginScene, so the flip lands on the next
					 * frame boundary -- never mid-frame. */
					g_dbg_drawMerge ^= 1;
					N3DS_DebugLog(g_dbg_drawMerge ? "[CTR-3DS] draw merge: ON (deferred bins + segment replay)"
					                              : "[CTR-3DS] draw merge: off (immediate runs)");
				}
#endif
#if defined(CTR_NATIVE_LOD_BIAS) && CTR_NATIVE_LOD_BIAS
				else if (strncmp(word, "lodgeo", 6) == 0 || strncmp(word, "lodtex", 6) == 0)
				{
					/* Track detail ('lodgeo') and texture detail ('lodtex') as a
					 * percentage of retail switch distance -- lower means detail
					 * drops nearer the camera and the OT walk is handed fewer
					 * prims. 'lodgeo 50' sets a value; bare 'lodgeo' steps the
					 * A/B ladder. Read once per frame when RenderAllLevelGeometry
					 * seeds its thresholds, so a change lands on a frame boundary
					 * and never mid-walk. */
					extern int g_ctrLodGeoScale;
					extern int g_ctrLodTexScale;
					extern int CTR_LodBias_Clamp(int percent);
					extern int CTR_LodBias_Step(int percent);
					int isGeo = (word[3] == 'g');
					int *scale = isGeo ? &g_ctrLodGeoScale : &g_ctrLodTexScale;
					char msg[48];

					*scale = (word[6] == ' ') ? CTR_LodBias_Clamp(atoi(&word[7])) : CTR_LodBias_Step(*scale);
					snprintf(msg, sizeof(msg), "[CTR-3DS] %s LOD scale: %d%%", isGeo ? "level" : "texture", *scale);
					N3DS_DebugLog(msg);
				}
#endif
				else if (strncmp(word, "renderthread", 12) == 0)
				{
					extern int g_dbg_renderThread;

					/* Checked at post time on the game thread; the OFF
					 * transition fences the worker before the first direct
					 * call (NativeRenderQueue_Route), so the flip is safe
					 * mid-race. This is the feature's A/B instrument. */
					g_dbg_renderThread ^= 1;
					N3DS_DebugLog(g_dbg_renderThread ? "[CTR-3DS] render worker routing: ON"
					                                 : "[CTR-3DS] render worker routing: OFF (sync on main thread)");
				}
#ifdef CTR_NATIVE_DIRECT_EMIT
				else if (strncmp(word, "directemit", 10) == 0)
				{
					extern int g_dbg_directEmit;

					/* Both legs produce byte-identical bins, so unlike
					 * 'drawmerge' this needs no BeginScene latch -- see
					 * native_direct_emit.c. */
					g_dbg_directEmit ^= 1;
					N3DS_DebugLog(g_dbg_directEmit ? "[CTR-3DS] direct emit: ON" : "[CTR-3DS] direct emit: OFF");
				}
#endif
				else if (strncmp(word, "resolvememo", 11) == 0)
				{
					extern int g_dbg_resolveMemo;

					g_dbg_resolveMemo ^= 1;
					N3DS_DebugLog(g_dbg_resolveMemo ? "[CTR-3DS] resolve memo: ON" : "[CTR-3DS] resolve memo: OFF");
				}
				else if (strncmp(word, "smokedefer", 10) == 0)
				{
					extern int g_dbg_smokeDefer;

					/* Driver-local exhaust smoke redrawn after the model
					 * pass (over its own kart, retail's painter's order) vs the old
					 * in-merge draw where the body paints over it. */
					g_dbg_smokeDefer ^= 1;
					N3DS_DebugLog(g_dbg_smokeDefer ? "[CTR-3DS] deferred smoke: ON (over own kart)"
					                               : "[CTR-3DS] deferred smoke: off (in-merge, old look)");
				}
				else if (strncmp(word, "stpass1", 7) == 0)
				{
					extern int g_dbg_stPass1;

					/* Minimap-box probe, native_gpu.c DrawSplit: skip the
					 * blending-off opaque-body pass of two-pass semi-trans draws. */
					g_dbg_stPass1 ^= 1;
					N3DS_DebugLog(g_dbg_stPass1 ? "[CTR-3DS] semi-trans pass 1: ON" : "[CTR-3DS] semi-trans pass 1: SKIPPED");
				}
				else if (strncmp(word, "stpass2", 7) == 0)
				{
					extern int g_dbg_stPass2;

					/* Minimap-box probe: skip the blended STP-texel pass. */
					g_dbg_stPass2 ^= 1;
					N3DS_DebugLog(g_dbg_stPass2 ? "[CTR-3DS] semi-trans pass 2: ON" : "[CTR-3DS] semi-trans pass 2: SKIPPED");
				}
				else if (strncmp(word, "skyfar", 6) == 0)
				{
					extern int g_dbg_skyFar;

					/* Pin the skybox to the far depth plane so translucent
					 * track geometry (the waterfalls) wins the depth test against it,
					 * vs the old OT-bucket fallback the sky punched through with. */
					g_dbg_skyFar ^= 1;
					N3DS_DebugLog(g_dbg_skyFar ? "[CTR-3DS] sky far-pin: ON (waterfalls over sky)"
					                           : "[CTR-3DS] sky far-pin: off (old bucket depth)");
				}
				else if (strncmp(word, "skyfirst", 8) == 0)
				{
					extern int g_dbg_skyFirst;

					/* Draw the skybox BEFORE the track pass (its own vertex
					 * accumulator) so the depth-less translucent waterfall paints
					 * over it. OFF routes the sky back into the opaque merge bins,
					 * which flush after the track = the punch-through. */
					g_dbg_skyFirst ^= 1;
					N3DS_DebugLog(g_dbg_skyFirst ? "[CTR-3DS] sky-first: ON (sky under waterfalls)"
					                             : "[CTR-3DS] sky-first: off (sky in merge, punches through)");
				}
				else if (strncmp(word, "backdropdbg", 11) == 0)
				{
					extern int g_dbg_backdropDump;

					g_dbg_backdropDump = 1;
					N3DS_DebugLog("[CTR-3DS] backdrop: one-shot drain dump armed");
				}
				else if (strncmp(word, "backdrop", 8) == 0)
				{
					extern int g_dbg_backdrop;

					/* s46 clearColor backdrop fill A/B (native_renderer_3ds.c). */
					g_dbg_backdrop ^= 1;
					N3DS_DebugLog(g_dbg_backdrop ? "[CTR-3DS] backdrop: ON (clearColor fill under the sky pass)"
					                             : "[CTR-3DS] backdrop: off (pre-s46 frame, fills ignored)");
				}
				else if (strncmp(word, "padmux", 6) == 0)
				{
					/* Virtual multitap (native_input.c): all four pads report
					 * connected so VS/battle player counts open up; the 3DS
					 * buttons drive one selectable pad. `padmux 0` = off
					 * (retail single-pad bus), `padmux N` = drive pad N, bare
					 * = cycle the driven pad. Chord: SELECT+ZR. */
					extern int Platform_InputDebugPadMux(int op);
					const char *arg = word + 6;
					int op = -1;
					int state;
					char msg[80];

					while ((*arg == ' ') || (*arg == '\t'))
					{
						arg++;
					}
					if ((*arg >= '0') && (*arg <= '9'))
					{
						op = atoi(arg);
					}
					state = Platform_InputDebugPadMux(op);
					if (state == 0)
					{
						N3DS_DebugLog("[CTR-3DS] padmux: OFF (retail single-pad bus)");
					}
					else
					{
						snprintf(msg, sizeof(msg), "[CTR-3DS] padmux: 4 pads connected, driving pad %d", state);
						N3DS_DebugLog(msg);
					}
				}
				else if (strncmp(word, "uiorder", 7) == 0)
				{
					extern int g_dbg_uiOrder;

					/* UI off the depth test. ON routes every UI-band prim out
					 * of the state bins into one ordered list drawn last, in
					 * submission order, with all UI pinned to a single depth stamp
					 * -- painter's order, which is what retail does. OFF restores
					 * the per-ordinal band stamps and leaves UI in the bins, i.e.
					 * the old behaviour (256-prim budget, glyph/panel ties resolved
					 * by whichever state bin each landed in). Both halves move
					 * together; they are only correct as a pair. */
					g_dbg_uiOrder ^= 1;
					N3DS_DebugLog(g_dbg_uiOrder ? "[CTR-3DS] ui-order: ON (UI in submission order, off the depth test)"
					                            : "[CTR-3DS] ui-order: off (UI in merge bins, depth-ordered)");
				}
				else if (strncmp(word, "uipin", 5) == 0)
				{
					extern int g_dbg_uiPin;

					/* BISECT (temporary): splits the pin half off the routing
					 * half so a defect can be attributed to one of them. OFF keeps
					 * the ordered pass but restores per-ordinal UI stamps. Not a
					 * supported configuration -- diagnostic only. */
					g_dbg_uiPin ^= 1;
					N3DS_DebugLog(g_dbg_uiPin ? "[CTR-3DS] ui-pin: ON (all UI at one stamp)"
					                          : "[CTR-3DS] ui-pin: off (per-ordinal UI stamps, ordered pass still on)");
				}
				else if (strncmp(word, "uilast", 6) == 0)
				{
					extern int g_dbg_uiLast;

					/* BISECT (temporary): WHERE the ordered UI pass runs. OFF
					 * moves it back to the old post-merge slot, which separates
					 * "content never drawn" from "content painted over by UI drawn
					 * last". Diagnostic only. */
					g_dbg_uiLast ^= 1;
					N3DS_DebugLog(g_dbg_uiLast ? "[CTR-3DS] ui-last: ON (UI pass draws last of all)"
					                           : "[CTR-3DS] ui-last: off (UI pass draws right after the merge barrier)");
				}
				else if (strncmp(word, "uipop", 5) == 0)
				{
					/* How far UI pops out of the screen, as a percent of this
					 * eye's at-infinity shift. 0 = the old flat-at-screen-plane
					 * behaviour. 'uipop N' sets it directly -- a comfort value only
					 * its owner's eyes can judge, so it is dialled at runtime on
					 * hardware rather than rebuilt. Only visible with the 3D slider
					 * up; identically 0 in mono by construction. */
					const char *arg = word + 5;

					while ((*arg == ' ') || (*arg == '\t'))
					{
						arg++;
					}
					if ((*arg >= '0') && (*arg <= '9'))
					{
						g_dbg_uiPopPct = atoi(arg);
						if (g_dbg_uiPopPct > 200)
						{
							g_dbg_uiPopPct = 200;
						}
					}
					{
						char msg[96];

						snprintf(msg, sizeof(msg), "[CTR-3DS] ui pop-out: %d%% of k", g_dbg_uiPopPct);
						N3DS_DebugLog(msg);
					}
				}
				else if (strncmp(word, "uidump", 6) == 0)
				{
					extern int g_dbg_uiDump;

					/* Bisect instrument: one-shot dump of the ordered UI list's
					 * contents on the next frame. Needs the SD log on. */
					g_dbg_uiDump = 1;
					N3DS_DebugLog("[CTR-3DS] ui-dump: armed (next frame)");
				}
				else if (strncmp(word, "bfcull", 6) == 0)
				{
					extern int g_dbg_bfCull;

					/* Perf/parity experiment: track-scoped backface culling.
					 * 'bfcull N' sets 0=off / 1=cull-back / 2=cull-front directly
					 * -- deterministic for a PRE-LAUNCH dumpreq on hardware, where
					 * the SD can't be written mid-race; bare 'bfcull' cycles for a
					 * quick Azahar A/B. Watch for holes in the track (winding
					 * inconsistent = unsafe) vs the second waterfall vanishing
					 * cleanly + fps rising. Rationale in N3DS_TrackDraw_Begin. */
					if ((word[6] == ' ') && (word[7] >= '0') && (word[7] <= '2'))
					{
						g_dbg_bfCull = word[7] - '0';
					}
					else
					{
						g_dbg_bfCull = (g_dbg_bfCull + 1) % 3;
					}
					N3DS_DebugLog(g_dbg_bfCull == 0   ? "[CTR-3DS] backface cull (track): OFF (draw all faces)"
					              : g_dbg_bfCull == 1 ? "[CTR-3DS] backface cull (track): BACK faces culled"
					                                  : "[CTR-3DS] backface cull (track): FRONT faces culled");
				}
				else if (strncmp(word, "fbtest", 6) == 0)
				{
					extern int g_dbg_skipFbTriTest;

					/* Ablate resolve's per-triangle fb-mirror
					 * overlap test. ON = pause/wipe freeze frames may be
					 * stale; instrument only, see the declaration comment. */
					g_dbg_skipFbTriTest ^= 1;
					N3DS_DebugLog(g_dbg_skipFbTriTest ? "[CTR-3DS] fb tri test: SKIPPED (freeze frames may be stale)" : "[CTR-3DS] fb tri test: on");
				}
				else if (strncmp(word, "otz", 3) == 0)
				{
					extern int g_dbg_otzWiden;

					/* Track-renderer rung 1 v2: RECORD-ONLY bucket->ordinal
					 * table (stamps untouched; v1's restamp was falsified by
					 * its own [OTZ] line). [OTZ2] validation line while on. */
					g_dbg_otzWiden ^= 1;
					N3DS_DebugLog(g_dbg_otzWiden ? "[CTR-3DS] otz table: RECORDING (stamps unchanged)" : "[CTR-3DS] otz table: off");
				}
				else if (strncmp(word, "atlasview", 9) == 0)
				{
					extern int g_dbg_atlasView;
					char msg[48];

					/* 'atlasview N' selects atlas N directly; bare
					 * 'atlasview' cycles off -> 0 -> 1 -> ... -> off. */
					if (word[9] == ' ')
					{
						g_dbg_atlasView = atoi(&word[10]) + 1;
					}
					else
					{
						g_dbg_atlasView = (g_dbg_atlasView + 1) % (N3DS_ATLAS_COUNT + 1);
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] atlas view: %d", g_dbg_atlasView - 1);
					N3DS_DebugLog(g_dbg_atlasView ? msg : "[CTR-3DS] atlas view: off");
				}
				else if (strncmp(word, "verbose", 7) == 0)
				{
					extern int g_dbg_statsVerbose;

					g_dbg_statsVerbose ^= 1;
					N3DS_DebugLog(g_dbg_statsVerbose ? "[CTR-3DS] stats: VERBOSE" : "[CTR-3DS] stats: quiet");
				}
				else if (strncmp(word, "pktbias", 7) == 0)
				{
					extern int g_dbg_pktDepthBias;
					char msg[64];

					/* 'pktbias N' sets an exact value (view.z units nearer);
					 * bare 'pktbias' steps the same preset ladder the hardware
					 * chord uses, so the two controls can't disagree. */
					if (word[7] == ' ')
					{
						g_dbg_pktDepthBias = atoi(&word[8]);
					}
					else
					{
						g_dbg_pktDepthBias = N3DS_NextPktBias(g_dbg_pktDepthBias);
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] packet depth bias: %d (units nearer)", g_dbg_pktDepthBias);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "trkchk", 6) == 0)
				{
					/* s32: runs BOTH depth formulas -- the integer one this
					 * project has been validating, and the composed-row float24
					 * one the GPU actually runs -- against retail's GTE for the
					 * same vertex. Taxed (three calls per retail track triangle),
					 * so it is a toggle, not a default. Read [TRKDEPTHCHK] and
					 * [TRKDEPTHSH] together; the gap between them is the point. */
					extern int g_dbg_trackDepthCheck;

					g_dbg_trackDepthCheck ^= 1;
					N3DS_DebugLog(g_dbg_trackDepthCheck ? "[CTR-3DS] track depth check: ON (integer + shader arms)"
					                                    : "[CTR-3DS] track depth check: off");
				}
				else if (strncmp(word, "nofx", 4) == 0)
				{
					extern int g_dbg_noFx;

					g_dbg_noFx ^= 1;
					N3DS_DebugLog(g_dbg_noFx ? "[CTR-3DS] blended packet effects: DROPPED (judging aid)"
					                         : "[CTR-3DS] blended packet effects: drawn");
				}
				else if (strncmp(word, "fxdepth", 7) == 0)
				{
					extern int g_dbg_fxNoDepthWrite;

					g_dbg_fxNoDepthWrite ^= 1;
					N3DS_DebugLog(g_dbg_fxNoDepthWrite ? "[CTR-3DS] blended packets: depth TEST only (default, matches model/track)"
					                                   : "[CTR-3DS] blended packets: also WRITE depth (old behaviour)");
				}
				else if (strncmp(word, "trknear", 7) == 0)
				{
					/* s32: [TRKNEAR] walks OUR OWN resident mesh and reports its
					 * viewZ range. [TRKDEPTHCHK] cannot answer this -- it only
					 * ever sees vertices RETAIL projects, which is a different
					 * (and possibly much farther) set than the mesh the track
					 * shader actually draws. Read [TRKNEAR] viewZ against
					 * [PKTDEPTHSZ] mdlSz: that pair is the kart's distance and
					 * the road's distance, from the two things that really draw. */
					extern int g_dbg_trackNearCensus;

					g_dbg_trackNearCensus ^= 1;
					N3DS_DebugLog(g_dbg_trackNearCensus ? "[CTR-3DS] track near census: ON" : "[CTR-3DS] track near census: off");
				}
				else if (strncmp(word, "trackafter", 10) == 0)
				{
					extern int g_dbg_trackAfterMerge;

					g_dbg_trackAfterMerge ^= 1;
					N3DS_DebugLog(g_dbg_trackAfterMerge ? "[CTR-3DS] track draw: AFTER merge flush" : "[CTR-3DS] track draw: before merge flush (default)");
				}
				/* Stereo tokens. ORDER MATTERS within this family: "stereo" is
				 * a prefix of all the others, so the longer tokens must be
				 * tested first or `stereosep 8` would just flip the master
				 * toggle. */
				else if (strncmp(word, "stereowindow", 12) == 0)
				{
					g_dbg_stereoWindow ^= 1;
					N3DS_DebugLog(g_dbg_stereoWindow ? "[CTR-3DS] stereo window mode: ON (no pop-out, world starts at the screen)"
					                                 : "[CTR-3DS] stereo window mode: off (pop-out allowed, clamped)");
				}
				else if (strncmp(word, "stereosep", 9) == 0)
				{
					char msg[64];

					/* 'stereosep N' sets the full-slider disparity at infinity
					 * (PSX px). Negative flips the eyes -- the cheap A/B if the
					 * depth sense ever reads inverted on hardware. Bare token
					 * resets the default. */
					if (word[9] == ' ')
					{
						int n = atoi(&word[10]);

						s_stereoSep = (float)((n < -64) ? -64 : (n > 64) ? 64 : n);
					}
					else
					{
						s_stereoSep = 12.0f;
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] stereo separation: %d px @ full slider", (int)s_stereoSep);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "stereoconv", 10) == 0)
				{
					char msg[64];

					/* 'stereoconv N' sets the screen-plane depth in view.z
					 * units; nearer content pops out, farther recedes. Bare
					 * token resets the default. */
					if (word[10] == ' ')
					{
						int n = atoi(&word[11]);

						s_stereoConv = (float)((n < 64) ? 64 : (n > 60000) ? 60000 : n);
					}
					else
					{
						s_stereoConv = 1200.0f;
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] stereo convergence: %d view.z units", (int)s_stereoConv);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "stereo", 6) == 0)
				{
					g_dbg_stereo ^= 1;
					N3DS_DebugLog(g_dbg_stereo ? "[CTR-3DS] stereo: enabled (slider gates per frame)" : "[CTR-3DS] stereo: FORCED OFF");
				}
				else if (strncmp(word, "depthtest", 9) == 0)
				{
					/* No extern: g_dbg_depthTest lives in THIS translation
					 * unit (see its comment above N3DS_C3DBeginFrame), unlike
					 * the g_dbg_* below that live in the main.c unity TU. */
					static const char *const names[3] = {"NORMAL (LEQUAL + writes)", "NOTEST (painter's order)",
					                                     "TRACKRO (track tests but does not write depth)"};
					char msg[80];

					if (word[9] == ' ')
					{
						int n = atoi(&word[10]);

						g_dbg_depthTest = (n < 0) ? 0 : (n > 2) ? 2 : n;
					}
					else
					{
						g_dbg_depthTest = (g_dbg_depthTest + 1) % 3;
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] depth test: %s", names[g_dbg_depthTest]);
					N3DS_DebugLog(msg);
				}
#ifdef CTR_NATIVE_TRACK_RENDERER
				else if (strncmp(word, "trackrender", 11) == 0)
				{
					extern int g_dbg_trackRender;
					static const char *const names[3] = {"OFF (retail)", "ON (resident replaces retail)", "BOTH (resident + retail overlay)"};
					char msg[64];

					/* 'trackrender N' sets a specific state (clamped 0-2);
					 * bare 'trackrender' cycles 0->1->2->0 -- same idiom as
					 * 'atlasview' above. */
					if (word[11] == ' ')
					{
						int n = atoi(&word[12]);

						g_dbg_trackRender = (n < 0) ? 0 : (n > 2) ? 2 : n;
					}
					else
					{
						g_dbg_trackRender = (g_dbg_trackRender + 1) % 3;
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] track render: %s", names[g_dbg_trackRender]);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "decalaxis", 9) == 0)
				{
					extern int g_dbg_decalAxisFix;

					/* Ground-decal 4x depth-axis unscale (shadows, skid
					 * marks, tires). Bare token toggles; this is the A/B the
					 * fix gets judged on, so 0 must reproduce the pre-fix behaviour
					 * exactly. docs/GROUND_DECAL_PASS_DESIGN.md 3. */
					g_dbg_decalAxisFix ^= 1;
					N3DS_DebugLog(g_dbg_decalAxisFix ? "[CTR-3DS] decal depth axis: FIXED (4x unscaled)"
					                                : "[CTR-3DS] decal depth axis: off (pre-s41 behaviour)");
				}
				else if (strncmp(word, "dorder", 6) == 0)
				{
					char msg[80];

					/* The drawOrder tie-break sweep. 'dorder N'
					 * sets the weight in view.z units per drawOrder step (N is
					 * an integer: 0 removes the term, 1 is the shipped value
					 * this replaced); bare 'dorder' toggles 0 <-> 1 so the A/B
					 * leg is one token. Integer-only on purpose: atoi is what
					 * every sibling token here uses, and sub-unit weights are
					 * not what this measurement is asking about.
					 * docs/GROUND_DECAL_PASS_DESIGN.md 4. */
					if (word[6] == ' ')
					{
						int n = atoi(&word[7]);

						s_trackDrawOrderWeight = (float)((n < 0) ? 0 : (n > 64) ? 64 : n);
					}
					else
					{
						s_trackDrawOrderWeight = (s_trackDrawOrderWeight != 0.0f) ? 0.0f : 1.0f;
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] track drawOrder weight: %d view.z units/step%s", (int)s_trackDrawOrderWeight,
					         (s_trackDrawOrderWeight == 0.0f) ? " (term REMOVED -- phase 0 diagnostic)" : "");
					N3DS_DebugLog(msg);
				}
#endif
#ifdef CTR_NATIVE_MODEL_RENDERER
				else if (strncmp(word, "modelrender", 11) == 0)
				{
					extern int g_dbg_modelRender;
					static const char *const names[3] = {"OFF (packets only)", "ON (PICA-native models)", "BOTH (native + packet overlay)"};
					char msg[64];

					/* Same idiom as 'trackrender' directly above: 'modelrender N'
					 * sets a state, bare 'modelrender' cycles. State 1 REPLACES
					 * the packet models (RenderBucket_DrawFunc_Normal's
					 * mdlReplaces skips the packet write); state 2 draws both,
					 * as an overlay A/B. A previous version of this comment said
					 * suppression wasn't in yet -- it is, and that stale text
					 * misdiagnosed a bug. */
					if (word[11] == ' ')
					{
						int n = atoi(&word[12]);

						g_dbg_modelRender = (n < 0) ? 0 : (n > 2) ? 2 : n;
					}
					else
					{
						g_dbg_modelRender = (g_dbg_modelRender + 1) % 3;
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] model render: %s", names[g_dbg_modelRender]);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "hudui", 5) == 0)
				{
					extern int g_dbg_hudModelsAsUi;

					/* HUD models (place numeral, Wumpa counter, tokens...) take
					 * the retail packet path so the UI-band rules apply to them.
					 * OFF is the pop-out / "ST 1" / track-intersection state. */
					if (word[5] == ' ')
					{
						g_dbg_hudModelsAsUi = (atoi(&word[6]) != 0);
					}
					else
					{
						g_dbg_hudModelsAsUi ^= 1;
					}
					N3DS_DebugLog(g_dbg_hudModelsAsUi ? "[CTR-3DS] HUD models: drawn as UI (retail packet path)" : "[CTR-3DS] HUD models: native capture (pops out, ST 1)");
				}
				else if (strncmp(word, "flagstyle", 9) == 0)
				{
					extern int g_dbg_flagStyle;
					static const char *const flagStyleNames[3] = {"retail grey checkers", "PRIDE stripes", "PURPLE/WHITE checkers (Spyro)"};
					char styleMsg[80];

					/* Menu/loading flag style. 'flagstyle N' sets, bare token
					 * cycles 0->1->2->0. Both non-retail styles are also
					 * reachable in-game by their cheat codes (WONDER, SOAR) --
					 * docs/EASTER_EGGS.md. */
					if (word[9] == ' ')
					{
						const int n = atoi(&word[10]);

						g_dbg_flagStyle = ((n >= 0) && (n <= 2)) ? n : 0;
					}
					else
					{
						g_dbg_flagStyle = (g_dbg_flagStyle + 1) % 3;
					}
					snprintf(styleMsg, sizeof(styleMsg), "[CTR-3DS] race flag: %s", flagStyleNames[g_dbg_flagStyle]);
					N3DS_DebugLog(styleMsg);
				}
				else if (strncmp(word, "axisfix", 7) == 0)
				{
					extern int g_dbg_modelAxisFix;

					/* Near-regime instances' mvp depth is 4x the
					 * track's view.z axis (measured, [AXISCHK]); ON divides it back
					 * out per instance. 'axisfix N' sets, bare token flips. */
					if (word[7] == ' ')
					{
						g_dbg_modelAxisFix = (atoi(&word[8]) != 0);
					}
					else
					{
						g_dbg_modelAxisFix ^= 1;
					}
					N3DS_DebugLog(g_dbg_modelAxisFix ? "[CTR-3DS] instance depth axis fix: ON (regime unscale)" : "[CTR-3DS] instance depth axis fix: off (raw mvp axis = the sink)");
				}
				else if (strncmp(word, "modelfade", 9) == 0)
				{
					extern int g_dbg_modelFade;

					/* Rung 3: replay of the normal writer's gte_dpct (instance
					 * fade/tint) on captured colours. 'modelfade N' sets, bare
					 * token flips. OFF = fading instances draw full-bright. */
					if (word[9] == ' ')
					{
						g_dbg_modelFade = (atoi(&word[10]) != 0);
					}
					else
					{
						g_dbg_modelFade ^= 1;
					}
					N3DS_DebugLog(g_dbg_modelFade ? "[CTR-3DS] model fade replay: ON" : "[CTR-3DS] model fade replay: off (captured colours stay pre-dpct)");
				}
#endif
#ifdef CTR_NATIVE_TRACK_RENDERER
				else if (strncmp(word, "leafkeep", 8) == 0)
				{
					extern int g_dbg_trackLeafKeep;
					char msg[64];

					/* Drift-holes fix: frames a leaf survives past its last
					 * visible-list appearance. 'leafkeep N' sets (0 = off,
					 * capped 255), bare token flips 0 <-> 15. */
					if (word[8] == ' ')
					{
						int n = atoi(&word[9]);

						g_dbg_trackLeafKeep = (n < 0) ? 0 : (n > 255) ? 255 : n;
					}
					else
					{
						g_dbg_trackLeafKeep = (g_dbg_trackLeafKeep != 0) ? 0 : 15;
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] leaf keep-alive: %d frames%s", g_dbg_trackLeafKeep, g_dbg_trackLeafKeep ? "" : " (off)");
					N3DS_DebugLog(msg);
				}
#endif
#if defined(CTR_NATIVE_GTE_CAPTURE) && CTR_NATIVE_GTE_CAPTURE
				else if (strncmp(word, "packetdepth", 11) == 0)
				{
					extern int g_dbg_packetDepth;

					g_dbg_packetDepth ^= 1;
					N3DS_DebugLog(g_dbg_packetDepth ? "[CTR-3DS] packet depth: ON (sz3 true-depth axis)" : "[CTR-3DS] packet depth: off (emit ordinals)");
				}
				else if (strncmp(word, "gtecapture", 10) == 0)
				{
					extern int g_dbg_gteCapture;
					static const char *const names[3] = {"OFF (ablated)", "FULL (store + matrix note)", "STORE ONLY (no matrix note)"};
					char msg[64];

					/* 'gtecapture N' sets a state; bare token cycles 1->2->0. */
					if (word[10] == ' ')
					{
						int n = atoi(&word[11]);

						g_dbg_gteCapture = (n < 0) ? 0 : (n > 2) ? 2 : n;
					}
					else
					{
						g_dbg_gteCapture = (g_dbg_gteCapture + 1) % 3;
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] gte capture: %s", names[g_dbg_gteCapture]);
					N3DS_DebugLog(msg);
				}
				/* s32 sinking-racers A/B trio. All three default off, so the
				 * first thing on screen is today's behaviour, and each removes
				 * exactly one suspect -- see their definitions in native_gpu.c
				 * and native_gte_capture.c. Read [PKTDEPTH2] alongside. */
				else if (strncmp(word, "strictmtx", 9) == 0)
				{
					extern int g_dbg_pktDepthStrictMtx;

					g_dbg_pktDepthStrictMtx ^= 1;
					N3DS_DebugLog(g_dbg_pktDepthStrictMtx ? "[CTR-3DS] strict matrix: ON (mixed-provenance prim = miss)"
					                                     : "[CTR-3DS] strict matrix: off");
				}
				else if (strncmp(word, "missordinal", 11) == 0)
				{
					extern int g_dbg_pktDepthMissOrdinal;

					g_dbg_pktDepthMissOrdinal ^= 1;
					/* The waterfall/HUD artefact this reintroduces is expected
					 * and is not the thing being judged -- said here so the log
					 * carries the caveat next to the toggle that caused it. */
					N3DS_DebugLog(g_dbg_pktDepthMissOrdinal ? "[CTR-3DS] miss->ordinal: ON (expect animated water over everything)"
					                                        : "[CTR-3DS] miss->ordinal: off (bucket-derived depth)");
				}
				else if (strncmp(word, "noevict", 7) == 0)
				{
					extern int g_dbg_gteNoEvict;

					g_dbg_gteNoEvict ^= 1;
					N3DS_DebugLog(g_dbg_gteNoEvict ? "[CTR-3DS] capture evict: REFUSED across matrix states"
					                               : "[CTR-3DS] capture evict: normal (last writer wins)");
				}
				else if (strncmp(word, "pktaxisfix", 10) == 0)
				{
					extern int g_dbg_pktAxisFix;

					/* Was advertised but never wired -- the
					 * packet-side unscale had NO runtime switch until now.
					 * OFF is the ice-pond kart vanish (Blizzard Bluff), which
					 * doubles as this toggle's positive control. 'pktaxisfix N'
					 * sets, bare token flips, same idiom as 'axisfix'. */
					if (word[10] == ' ')
					{
						g_dbg_pktAxisFix = (atoi(&word[11]) != 0);
					}
					else
					{
						g_dbg_pktAxisFix ^= 1;
					}
					N3DS_DebugLog(g_dbg_pktAxisFix ? "[CTR-3DS] packet depth axis fix: ON (regime unscale at the store)"
					                               : "[CTR-3DS] packet depth axis fix: off (raw regime axis = ice-pond vanish)");
				}
				else if (strncmp(word, "bboxdepth", 9) == 0)
				{
					extern int g_dbg_bboxDepthStores;

					/* Warp-pad prize hunt: 0 removes the queue-phase bbox
					 * corner stores -- the one unbracketed regime-axis producer
					 * left in the capture table (native_gte_capture.c has the
					 * story). If the prize burial vanishes at 0, that leak is
					 * the bug. 'bboxdepth N' sets, bare token flips. */
					if (word[9] == ' ')
					{
						g_dbg_bboxDepthStores = (atoi(&word[10]) != 0);
					}
					else
					{
						g_dbg_bboxDepthStores ^= 1;
					}
					N3DS_DebugLog(g_dbg_bboxDepthStores ? "[CTR-3DS] bbox corner stores: ON (today's behaviour, unbracketed)"
					                                    : "[CTR-3DS] bbox corner stores: SUPPRESSED (s45 A/B leg)");
				}
				else if (strncmp(word, "dcheck", 6) == 0)
				{
					extern int g_dbg_modelDepthCheck;

					/* The depth-axis self-verification both classifiers
					 * already carry -- [AXISCHK]/[MDLDEPTH] (native_model_emit)
					 * and the [PKTAXIS] per-func ratio/reclass stats
					 * (native_gte_capture), flushed every 256 frames while ON.
					 * Was PC-argv-only (--model-depth-check) until now; the 3DS
					 * build had no way to light it. Taxed per store, so a
					 * toggle, not a default. */
					g_dbg_modelDepthCheck ^= 1;
					N3DS_DebugLog(g_dbg_modelDepthCheck ? "[CTR-3DS] depth axis check: ON ([AXISCHK]/[PKTAXIS] every 256f)"
					                                    : "[CTR-3DS] depth axis check: off");
				}
				else if (strncmp(word, "watch", 5) == 0)
				{
					extern char g_dbg_watchName[16];
					char msg[64];

					/* 'watch <name>' taps one named object (instance name
					 * OR model name -- e.g. 'watch prize1'); [WATCH] lines
					 * report its store-side depth story while it draws. Bare
					 * 'watch' turns it off. */
					if (word[5] == ' ')
					{
						int wi;

						for (wi = 0; (wi < 15) && (word[6 + wi] != '\0') && (word[6 + wi] != '\n') && (word[6 + wi] != '\r') && (word[6 + wi] != ' '); wi++)
						{
							g_dbg_watchName[wi] = word[6 + wi];
						}
						g_dbg_watchName[wi] = '\0';
						snprintf(msg, sizeof(msg), "[CTR-3DS] watch: '%s' ([WATCH] while it draws)", g_dbg_watchName);
						N3DS_DebugLog(msg);
					}
					else
					{
						g_dbg_watchName[0] = '\0';
						N3DS_DebugLog("[CTR-3DS] watch: off");
					}
				}
#endif
				else
				{
					N3DS_DebugCaptureAll();
				}
			}
		}
	}

	/* Repurposed: was the automated config sweep, settled since
	 * (dumpreq `sweep` still covers Azahar). Now the frame-pacing A/B --
	 * g_dbg_frameSync, defined above N3DS_C3DBeginFrame. Mid-race by design:
	 * a mid-run flip brackets [PERF] windows by state on identical content,
	 * and dumpreq is unplaceable while the game holds the SD card. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_B))
	{
		g_dbg_frameSync ^= 1;
		N3DS_DebugLog(g_dbg_frameSync ? "[CTR-3DS] framesync: ON (FrameBegin waits for hw vblank)"
		                              : "[CTR-3DS] framesync: OFF (begin immediately)");
	}

	/* SELECT held + R: SD logging on/off (g_dbg_sdLog, defined by the log
	 * statics near the top of this file). R came free when the VRAM-capture
	 * diagnostic was retired; the owner reassigned it here because this is the
	 * toggle he reaches for most -- ON costs a ~100-170ms hitch every 2s, so
	 * it is the one switch whose state can invalidate a whole sitting.
	 *
	 * Briefly lived on SELECT+TOUCH (now free again), which itself replaced an
	 * attempt on SELECT+A -- that is the model-render toggle ~200 lines below,
	 * so one press flipped BOTH and silently confounded the measurement (owner
	 * caught it: "select A turns models off"). Grep the whole platform tree for
	 * a key token before binding it; most chords belong to other sessions. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_R))
	{
		g_dbg_sdLog ^= 1;
		/* Logged AFTER the flip so turning it back ON announces itself in the
		 * file; turning it off leaves its last word in the svc trail only. */
		N3DS_DebugLog(g_dbg_sdLog ? "[CTR-3DS] sdlog: ON (writing ctr3ds.log -- costs a ~2s hitch)"
		                          : "[CTR-3DS] sdlog: off (no SD writes; bottom screen still live)");
	}

	/* SELECT held + X: bottom-screen verbosity. Default off -- the stats
	 * block had grown into a wall the owner couldn't read the live toggles in. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_X))
	{
		extern int g_dbg_statsVerbose;

		g_dbg_statsVerbose ^= 1;
		N3DS_DebugLog(g_dbg_statsVerbose ? "[CTR-3DS] stats: VERBOSE" : "[CTR-3DS] stats: quiet");
	}

	/* SELECT+Y = backface-cull A/B (track-scoped), toggles OFF <-> cull
	 * BACK. Mid-race by design -- sit at one spot and flip it while watching the
	 * `work` ms and the holes/waterfall change live; a dumpreq can't be placed
	 * while the game holds the SD card. BACK is the established-correct winding
	 * (quads cull clean; only stray triangle faces hole). FRONT (state 2) stays
	 * reachable as dumpreq `bfcull 2` for winding checks -- from any culling
	 * state this chord returns to OFF first. SELECT+Y was freed from the
	 * packet-depth-bias ladder (still dumpreq `pktbias`); reused for this perf
	 * experiment because it needs to be flippable mid-race. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_Y))
	{
		extern int g_dbg_bfCull;

		g_dbg_bfCull = g_dbg_bfCull ? 0 : 1;
		N3DS_DebugLog(g_dbg_bfCull ? "[CTR-3DS] backface cull (track): BACK faces culled (watch `work` ms)"
		                           : "[CTR-3DS] backface cull (track): OFF (draw all faces)");
	}

	/* SELECT held + ZL: depth-test state cycle 0->1->2->0 (see g_dbg_depthTest
	 * above N3DS_C3DBeginFrame for what each state proves). ZL has been free
	 * since resolve-memo moved off it. Mid-race by design -- the question
	 * is "where did the karts go", and the karts only exist mid-race, so a
	 * cross-run A/B would confound content the way the LOD null did. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_ZL))
	{
		static const char *const names[3] = {"NORMAL (LEQUAL + writes)", "NOTEST (painter's order)",
		                                     "TRACKRO (track tests but does not write depth)"};
		char msg[80];

		g_dbg_depthTest = (g_dbg_depthTest + 1) % 3;
		snprintf(msg, sizeof(msg), "[CTR-3DS] depth test: %s", names[g_dbg_depthTest]);
		N3DS_DebugLog(msg);
	}

	/* SELECT held + ZR: virtual multitap (native_input.c) -- first press
	 * connects all four pads (opening VS/battle player counts), further
	 * presses cycle which pad the 3DS buttons drive; the others sit idle.
	 * Default OFF: the renderer has no split-screen support, so these modes
	 * stay behind retail's own controller gate until deliberately opened for
	 * renderer work. Mid-race by necessity: hopping karts is the point, and
	 * dumpreq is unplaceable while the game holds the SD card. ZR has been
	 * free since trackrender folded into the SELECT+START master. dumpreq
	 * `padmux [N]` sets a specific pad or turns the mux off again. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_ZR))
	{
		extern int Platform_InputDebugPadMux(int op);
		const int state = Platform_InputDebugPadMux(-1);
		char msg[64];

		snprintf(msg, sizeof(msg), "[CTR-3DS] padmux: driving pad %d of 4", state);
		N3DS_DebugLog(msg);
	}

	/* SELECT held + D-pad DOWN: toggle render-worker routing mid-run (mid-run
	 * flips bracket [PERF] windows by state, killing the cross-run content
	 * confound the same way textureless's combo does below). dumpreq
	 * 'renderthread' covers Azahar (host-writable SD folder), but on
	 * hardware nothing can place that file while the game runs -- FTP and
	 * the game are one-app-at-a-time -- so without a combo the feature's own
	 * hardware A/B would be cross-run. The OFF transition is safe mid-race
	 * by construction: Route() fences the worker before the first direct
	 * call (native_render_queue.c). */
	/* SELECT+DDOWN freed (widescreen -- now part of the SELECT+START master). Still reachable as dumpreq `wide`. */

	/* SELECT+DRIGHT freed (textureless debug view). Still reachable as dumpreq `textureless`. */

#ifdef CTR_NATIVE_DIRECT_EMIT
	/* SELECT+DLEFT freed (direct emit -- baseline, A/B settled). Still reachable as dumpreq `directemit`. */
#endif

	/* SELECT+DLEFT freed again (the particle-lift sweep did its job and
	 * was removed -- the lift nudged every particle, not just a kart's own
	 * smoke). Still reachable as nothing; the real driver-local fix carries no
	 * runtime knob. */

#if defined(CTR_NATIVE_GTE_CAPTURE) && CTR_NATIVE_GTE_CAPTURE
	/* SELECT held + D-pad UP: GTE capture cost A/B. The depth model the
	 * track renderer needs reads per-vertex sz3 out of the capture table, which
	 * means capture stops being an optional instrument and becomes permanently
	 * compiled in on the platform with the least frame time to spare -- so its
	 * price gets measured BEFORE the design commits to it. Completes the
	 * held-SELECT d-pad family (DOWN=render worker, RIGHT=textureless,
	 * LEFT=direct emit); same reason as all of them, a hardware A/B needs a
	 * button because dumpreq is unplaceable while the game holds the SD card.
	 * Safe to flip mid-race: OFF only stops writing a side table whose readers
	 * all treat a miss as "use the exact snapped value". */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_DUP))
	{
		extern int g_dbg_gteCapture;
		static const char *const names[3] = {"OFF (ablated; lookups all miss)", "FULL (store + matrix note)", "STORE ONLY (no matrix note)"};
		char msg[80];

		/* Cycles 1 -> 2 -> 0 -> 1 so a sitting walks full -> what the depth
		 * model would actually pay -> nothing, and each [PERF] window between
		 * two of these log lines belongs to exactly one state. */
		g_dbg_gteCapture = (g_dbg_gteCapture + 1) % 3;
		snprintf(msg, sizeof(msg), "[CTR-3DS] gte capture: %s", names[g_dbg_gteCapture]);
		N3DS_DebugLog(msg);
	}

	/* SELECT held + D-pad DOWN is the render worker; SELECT + L is packet depth
	 * -- the depth model's mid-race A/B. Flips BOTH sides at once (packets
	 * stamp sz3, track shader switches to the true-depth axis), because either
	 * alone puts the two geometry sources on different axes. This is the
	 * toggle the "karts through walls" objection gets tested against, so it
	 * has to be a button: a dumpreq file cannot be placed mid-race. */
	/* SELECT+L freed -- packet depth folded into the SELECT+START master.
	 * Still reachable as dumpreq `packetdepth`. */
#endif

#ifdef CTR_NATIVE_TRACK_RENDERER
	/* SELECT+ZR freed (native track renderer (incl. state 2, BOTH)). Still
	 * reachable as dumpreq `trackrender`. ZR reassigned to the padmux
	 * active-pad cycle (handler above, beside SELECT+ZL). */
#endif

#if defined(CTR_NATIVE_TRACK_RENDERER) && defined(CTR_NATIVE_MODEL_RENDERER) && defined(CTR_NATIVE_GTE_CAPTURE) && CTR_NATIVE_GTE_CAPTURE
	/* SELECT held + START: the owner's "our config" combo --
	 * track renderer + native models + packet depth all flip together, because
	 * reaching the target config from a clean boot cost three chords every
	 * boot. Two states: everything native (1,1,1) or everything retail
	 * (0,0,0); a mixed state goes native first, so one press from clean boot
	 * lands on the config being targeted. START still reaches the game, so
	 * in-race this also opens the pause menu -- acceptable and documented.
	 *
	 * This is now the ONLY renderer chord. The individual track / models /
	 * packet-depth buttons (ZR / A / L) and the widescreen button (D-DOWN) are
	 * gone -- owner's call, "not much use in individual toggles, all in one, I
	 * don't wanna fiddle with toggles". Every one of them survives as a dumpreq
	 * token (`trackrender`, `modelrender`, `packetdepth`, `wide`) for the day a
	 * single variable genuinely needs isolating, and isolating one variable is
	 * a deliberate debugging act that can afford a relaunch. Widescreen joins
	 * the master because "ours" includes it: REAL (2) on, 4:3 crop (0) off; the
	 * rejected stretch (1) stays dumpreq-only. */
	/*
	 * UI pop-out dial: SELECT + D-LEFT / D-RIGHT, in 5% steps of the eye's
	 * at-infinity shift. Earns a chord (owner-approved) on the one criterion
	 * that justifies one: it is a COMFORT value, judged by eye, in stereo, on
	 * hardware -- and it cannot be dialled any other way there, because dumpreq
	 * needs the SD card the game is holding. Deliberately not the touch screen:
	 * touch is a bad A/B edge -- it disturbs the thing being judged.
	 *
	 * Clamped 0..100 here: 0 restores the old flat-at-screen-plane UI, and 100
	 * is already well past the owner's "not as much as the character" bound, so
	 * the dial cannot walk into eye strain by held-button accident. dumpreq
	 * 'uipop N' still reaches 200 for deliberate experiments.
	 *
	 * Current value is on the bottom-screen stats block, not the log: the log is
	 * unreadable while the game runs, and this number is only meaningful at the
	 * moment it is being looked at.
	 */
	if ((held & KEY_SELECT) && (hidKeysDown() & (KEY_DLEFT | KEY_DRIGHT)))
	{
		const int step = (hidKeysDown() & KEY_DRIGHT) ? 5 : -5;
		char msg[80];

		g_dbg_uiPopPct += step;
		if (g_dbg_uiPopPct < 0)
		{
			g_dbg_uiPopPct = 0;
		}
		if (g_dbg_uiPopPct > 100)
		{
			g_dbg_uiPopPct = 100;
		}
		snprintf(msg, sizeof(msg), "[CTR-3DS] ui pop-out: %d%% of k", g_dbg_uiPopPct);
		N3DS_DebugLog(msg);
	}

	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_START))
	{
		extern int g_dbg_trackRender;
		extern int g_dbg_modelRender;
		extern int g_dbg_packetDepth;
		const int allOn =
		    (g_dbg_trackRender != 0) && (g_dbg_modelRender != 0) && (g_dbg_packetDepth != 0) && (g_dbg_widescreen != 0);
		const int target = allOn ? 0 : 1;

		g_dbg_trackRender = target;
		g_dbg_modelRender = target;
		g_dbg_packetDepth = target;
		g_dbg_widescreen = target ? 2 : 0; /* REAL, not stretch */
		N3DS_DebugLog(target ? "[CTR-3DS] OUR CONFIG: track+models+dmodel+widescreen all ON"
		                     : "[CTR-3DS] our config: all off (full retail, 4:3)");
	}
#endif

#ifdef CTR_NATIVE_MODEL_RENDERER
	/* SELECT held + A: PICA-native models on/off (rung 2). A BUTTON for the
	 * same reason trackrender is one -- this is the A/B the sunk-kart question
	 * gets settled against, and it has to be flippable mid-race with a kart
	 * visibly sitting on the road.
	 *
	 * WAS D-RIGHT, which was a COLLISION, not a free chord: D-RIGHT has been
	 * textureless mode since the owner moved it there (see that
	 * handler above), so one press flipped both, and every A/B either control
	 * was supposed to settle would have been confounded by the other. The
	 * comment that used to sit here asserted D-RIGHT was free after checking
	 * only the OTHER three d-pad directions.
	 *
	 * A is safe despite being accelerate: this tests hidKeysDown(), the
	 * press EDGE, so an already-held A (the whole race) never fires it -- only
	 * pressing A after SELECT is already down does. Every other chord is
	 * taken (B/X/Y/L/R/ZL/ZR and all four d-pad directions).
	 *
	 * Two states, not three, matching the trackrender button's reasoning: BOTH
	 * stays reachable as 'modelrender 2' via dumpreq. */
	/* SELECT+A freed -- model render folded into the SELECT+START master.
	 * Still reachable as dumpreq `modelrender` (including state 2, BOTH).
	 * Frees the accelerate button from carrying a chord at all. */
#endif
#endif

	if (held & KEY_A)
	{
		out |= N3DS_BTN_A;
	}
	if (held & KEY_B)
	{
		out |= N3DS_BTN_B;
	}
	if (held & KEY_X)
	{
		out |= N3DS_BTN_X;
	}
	if (held & KEY_Y)
	{
		out |= N3DS_BTN_Y;
	}
	if (held & KEY_L)
	{
		out |= N3DS_BTN_L;
	}
	if (held & KEY_R)
	{
		out |= N3DS_BTN_R;
	}
	if (held & KEY_ZL)
	{
		out |= N3DS_BTN_ZL;
	}
	if (held & KEY_ZR)
	{
		out |= N3DS_BTN_ZR;
	}
	if (held & KEY_START)
	{
		out |= N3DS_BTN_START;
	}
	if (held & KEY_SELECT)
	{
		out |= N3DS_BTN_SELECT;
	}
	if (held & (KEY_DUP | KEY_CPAD_UP))
	{
		out |= N3DS_BTN_UP;
	}
	if (held & (KEY_DDOWN | KEY_CPAD_DOWN))
	{
		out |= N3DS_BTN_DOWN;
	}
	if (held & (KEY_DLEFT | KEY_CPAD_LEFT))
	{
		out |= N3DS_BTN_LEFT;
	}
	if (held & (KEY_DRIGHT | KEY_CPAD_RIGHT))
	{
		out |= N3DS_BTN_RIGHT;
	}

	return out;
}

unsigned long N3DS_HeapUsedBytes(void)
{
	struct mallinfo mi = mallinfo();

	return (unsigned long)mi.uordblks;
}

unsigned long N3DS_LinearFreeBytes(void)
{
	return (unsigned long)linearSpaceFree();
}

int N3DS_MainLoopActive(void)
{
	return aptMainLoop() ? 1 : 0;
}
