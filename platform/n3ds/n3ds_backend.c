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
 * 3dslink netload stderr streaming (2026-07-21). When the app was sent over
 * Wi-Fi with 3dslink, HBL records the sender's IP in __3dslink_host; connect
 * a TCP stream back to it (3dslink must be run with -s to listen) and point
 * stderr at it. Every N3DS_DebugLog / Platform_Log line is then mirrored to
 * the PC live, which is the difference between "the exact last line before a
 * hardware freeze, on the host terminal" and pulling the SD card out from
 * behind the backplate. stdout is deliberately NOT redirected -- it is the
 * bottom-screen console. SD-card / Azahar launches have s_addr == 0 and skip
 * all of this.
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

	/* Batch the stream: newlib's stderr is unbuffered, so every log line was
	 * a synchronous soc:u send -- the stats window's ~17 back-to-back lines
	 * cost one IPC each, every 32 frames, and each send is a chance for a
	 * transient WiFi failure to kill the whole stream (which is how session
	 * 19's first netload lost everything after boot). Buffer + the shared 2s
	 * timed flush in N3DS_DebugLog turns that into one send per window batch.
	 * Must precede the first stderr write, hence before the active flag flips
	 * (every stream write is gated on it). svcOutputDebugString stays the
	 * per-line instant trail for crash forensics. */
	setvbuf(stderr, NULL, _IOFBF, 8192);

	s_netloadStreamActive = 1;
}

void N3DS_EarlyInit(void)
{
	consoleDebugInit(debugDevice_SVC);

	/* One-deep rotation BEFORE the truncating open: "w" wipes the previous
	 * boot's log, and that is exactly how session 19's hardware race numbers
	 * were lost (crash -> relaunch -> the relaunch truncated the crashed
	 * run's log). Keeps exactly the last two boots, never grows unbounded. */
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

/* Last SD-log flush, in system ticks. Per-LINE fflush stalled gameplay
 * visibly on hardware: each flush pays FAT + card latency (tens of ms), and
 * the stats window emits ~15 lines back-to-back, so every window froze the
 * frame for a beat (owner-reported, session 18 -- invisible in Azahar, whose
 * sdmc is a host folder with free flushes). */
static u64 s_logLastFlushTick = 0;

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

	if (s_logFile != NULL)
	{
		fputs(msg, s_logFile);
		fputc('\n', s_logFile);
	}

	/* Flush BOTH buffered sinks at most every ~2s (SD file and, since
	 * session 19, the netload stderr stream -- see the setvbuf in
	 * N3DS_NetloadStreamInit). Both are post-hoc trails that tolerate a
	 * bounded tail loss on a hard crash; svcOutputDebugString above is the
	 * per-line instant trail. */
	if ((s_logFile != NULL) || s_netloadStreamActive)
	{
		const u64 now = svcGetSystemTick();

		if ((now - s_logLastFlushTick) > (SYSCLOCK_ARM11 * 2ull))
		{
			if (s_logFile != NULL)
			{
				fflush(s_logFile);
				/* fflush alone moves bytes into the FS session; the FAT
				 * size/allocation metadata only commits on an fs-level
				 * flush. Without this, a hard freeze loses EVERYTHING
				 * after the first flush -- the crashed s19 netload run's
				 * log read 1.6KB (ending at vblank=120) despite minutes
				 * of 2s flushes, and s18's crash-run log was never
				 * recoverable for the same reason. One FSFILE_Flush per
				 * 2s is the durability we thought we already had. */
				fsync(fileno(s_logFile));
			}
			if (s_netloadStreamActive)
			{
				fflush(stderr);
			}
			s_logLastFlushTick = now;
		}
	}
}

/*
 * SELECT+R VRAM capture (CTR_INTERNAL, texture-corruption diagnostic,
 * 2026-07-20). The renderer TU owns vram[] but can't do the SELECT+R button
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
 * one race capture (overwritten by a later menu capture, 2026-07-21); the
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

void N3DS_GfxPresent(void)
{
	/*
	 * Only the bottom screen's console framebuffer goes through this path.
	 * The generic gfxSwapBuffers() swaps every screen's buffers, which would
	 * fight citro3d for ownership of the top screen -- C3D_FrameEnd (see
	 * N3DS_C3DEndFrame) already presents that one. gfxFlushBuffers() is
	 * still whole-system but harmless to call unconditionally: flushing a
	 * buffer citro3d already manages is redundant, not wrong.
	 */
	gfxFlushBuffers();
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

	for (y = 0; y < dstH; y++)
	{
		const int sy = (y * 240) / dstH;
		unsigned char *dstRow = dstRGB + ((size_t)y * (size_t)dstW * 3);

		for (x = 0; x < dstW; x++)
		{
			/* Sample the pillarboxed content region only (N3DS_TOP_CONTENT_*):
			 * the full-panel form re-ingested the black side bars, so the
			 * pause background showed them squeezed inside the picture
			 * (user-reported on hardware, session 18). */
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
static C3D_RenderTarget *s_renderTarget = NULL;
static C3D_Mtx s_projection;
static void *s_vertexBufferMem = NULL;
static int s_vertexCursor = 0;
static int s_vertexHighWater = 0;
static C3D_Tex s_atlasTextures[N3DS_ATLAS_COUNT];
static C3D_Tex s_vramDisplayTex[N3DS_VRAM_DISPLAY_TEX_COUNT];

/* Native track renderer's dedicated atlas pool (rung 2b) -- see the header
 * comment on N3DS_TrackAtlas_Alloc. N3DS_TRACK_ATLAS_DIM/MAX_UNITS live in
 * n3ds_backend.h, not here (review finding #5) -- native_track_mesh.c's
 * packer checks its own constants against those exact macros at compile
 * time. Sized to the pool's own max unit count, NOT allocated up front;
 * s_trackAtlasUnitsAllocated tracks how many of these are actually live
 * textures right now (0 until the first level load calls
 * N3DS_TrackAtlas_Alloc). */
static C3D_Tex s_trackAtlasTextures[N3DS_TRACK_ATLAS_MAX_UNITS];
static int s_trackAtlasUnitsAllocated = 0;

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

void N3DS_C3DSetProjection(float left, float right, float bottom)
{
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
	AttrInfo_AddLoader(attrInfo, 0, GPU_SHORT, 3);         /* v0 = position (x, y, z=emit-ordinal depth) */
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
	 * s29 ROOT CAUSE of "the resident track renderer hides karts/characters/UI".
	 *
	 * PICA200 vertex-shader float uniforms are PHYSICAL REGISTERS shared by
	 * every program -- a uniform "location" is a register index, not a
	 * per-program handle. The two shaders' declaration orders collide head-on:
	 *
	 *   ctr.v.pica        projection[4]            -> f0 f1 f2 f3
	 *   ctr_track.v.pica  qx, qy, qw               -> f0 f1 f2
	 *                     projection[4]            -> f3 f4 f5 f6
	 *                     hParam, depthRange       -> f7 f8   (matches the
	 *                       runtime log's own "uLocH=7 uLocDepth=8")
	 *
	 * So every N3DS_TrackDraw_Begin writes the track camera's qx/qy/qw over
	 * the packet shader's projection rows 0-2. That matrix was uploaded ONCE
	 * for the frame in N3DS_C3DBeginFrame -- BEFORE the track draw -- and
	 * nothing re-uploaded it, so every packet draw issued AFTER the track
	 * projected through the track's camera rows and landed nowhere visible.
	 *
	 * That is why the symptom tracked SUBMISSION ORDER and nothing else:
	 * with CTR_NATIVE_DRAW_MERGE off, packet content draws during the walk
	 * (before the track) and survives; with merge on, essentially all of it
	 * is deferred to N3DS_MergeFlush (after the track) and died. It also
	 * explains why the s29 depth model appeared to do nothing -- the content
	 * it was meant to sort was never on screen to be sorted.
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
}

int N3DS_C3DInit(void)
{
	/* 4x citro3d's 256KB default. Both hardware crashes to date
	 * (crash_dump_00000007, s18 build; crash_dump_00000008, s19 build) were
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

	s_shaderDvlb = DVLB_ParseFile((u32 *)ctr_shbin, ctr_shbin_size);
	if (s_shaderDvlb == NULL)
	{
		return 0;
	}
	shaderProgramInit(&s_shaderProgram);
	shaderProgramSetVsh(&s_shaderProgram, &s_shaderDvlb->DVLE[0]);
	C3D_BindProgram(&s_shaderProgram);
	s_uLocProjection = shaderInstanceGetUniformLocation(s_shaderProgram.vertexShader, "projection");

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
	 * N3DS_TexView per-texel STP views, session 18: punch-through for
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
 * DIAGNOSTIC (2026-07-21, session 14): real hardware freezes inside the
 * first boot-splash present with no further output. The two blocking
 * candidates are C3D_FrameBegin's SYNCDRAW wait (previous command list never
 * completed) and C3D_FrameEnd's queue handoff. Trace the first few frame
 * brackets so the stream names the exact call that never returns; bounded so
 * steady-state runs don't flood sdmc:/ctr3ds.log.
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
 * s29 occlusion split (docs/archive/HANDOFF_s29.md). The resident track renderer hides
 * karts/characters/skybox/HUD, and four theories for WHY were refuted in one
 * sitting (extra geometry, GPU state leak between the two paths, a track/HUD
 * depth tie, and the uiZ=0 "anomaly" -- which turned out to be an instrument
 * pointed at content that never reaches it, this project's third such miss).
 * Rather than reason further, this splits the remaining space by construction.
 * Every state is a VISUAL binary a human settles in one glance -- deliberately
 * not a counter, for the reason PORT_FEASIBILITY_3DS.md's "How to measure"
 * section exists:
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
 * dumpreq `depthtest [N]` in Azahar, SELECT+ZL on hardware (ZL is free since
 * s27 moved resolve-memo off it; a dumpreq file cannot be placed mid-race).
 */
int g_dbg_depthTest = 0;

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

void N3DS_C3DBeginFrame(void)
{
	N3DS_FrameTrace("[CTR-3DS] FrameBegin: enter (SYNCDRAW wait)");
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	N3DS_FrameTrace("[CTR-3DS] FrameBegin: returned");
	/* Depth clears to FAR. Mtx_OrthoTilt maps LARGER vertex z to SMALLER
	 * stored depth (verified empirically, see below), so far = max. Harmless
	 * when CTR_NATIVE_OT_DEPTH is off (depth buffer unused). */
	C3D_RenderTargetClear(s_renderTarget, C3D_CLEAR_ALL, 0x000000FF, 0x00FFFFFF);
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
	 * Pillarbox the top screen to the bottom screen's 4:3-equivalent width
	 * (2026-07-20). Top panel is 400 wide, bottom is 320 -- PSX content is
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
	 * inside the picture -- user-reported on hardware, session 18).
	 */
	C3D_SetViewport(0, N3DS_TOP_CONTENT_X, 240, N3DS_TOP_CONTENT_W);

	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uLocProjection, &s_projection);

	/* Every draw this frame appends into the SAME persistent buffer at a
	 * fresh offset (see the header comment on N3DS_C3DAppendVertices) --
	 * reset the write cursor once per frame, not per draw. */
	s_vertexCursor = 0;
}

void N3DS_C3DEndFrame(void)
{
	N3DS_FrameTrace("[CTR-3DS] FrameEnd: enter (submit)");
	C3D_FrameEnd(0);
	N3DS_FrameTrace("[CTR-3DS] FrameEnd: returned");
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
 * Was RGBA8 until 2026-07-20. The decode cache encoded a three-level alpha
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
 * blend (session 18) recovered the STP bit exactly as predicted here: from
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
 * rung 2b) -- generalized 2026-07-29 from a hardcoded s_atlasTextures[]/
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
 * sdmc:/atlas_<i>.bin for the texture-corruption investigation (2026-07-20).
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
	 * wrong tile or past the texture's backing memory (review finding #6). */
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
 * s30: rewrite the UV pair of `count` already-uploaded vertices in place.
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
 * s30: the same in-place batched rewrite, for POSITION and COLOR rather than
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

void N3DS_TrackDraw_Begin(const float *qx, const float *qy, const float *qw, float h)
{
	C3D_TexEnv *env;

	s_trackDrawActive = 0;

	if (!s_trackGeomReady)
	{
		return;
	}

	C3D_BindProgram(&s_trackShaderProgram);
	C3D_SetAttrInfo(&s_trackAttrInfo);
	C3D_SetBufInfo(&s_trackBufInfo);

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
	 * Review fix (FIX 2): assert opaque blending explicitly rather than
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

		C3D_FVUnifSet(GPU_VERTEX_SHADER, s_trackULocH, h, selTrueDepth, 1.0f - selTrueDepth, 0.0f);

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

void N3DS_TrackDraw_Leaf(int firstIndex, int indexCount, float otIndexLo, float ordinalLo, float ordinalSpan, float invIndexSpan, int atlasBin)
{
	const u16 *indexBase;

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
	 * The texture-enable selector rides hParam.w (see the shader): the vertex
	 * program cannot branch cheaply, so it always computes the atlas UV and the
	 * fragment side is what decides whether to sample.
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

	indexBase = (const u16 *)s_trackIndexPtr + firstIndex;
	C3D_DrawElements(GPU_TRIANGLES, indexCount, C3D_UNSIGNED_SHORT, indexBase);
}

void N3DS_TrackDraw_End(void)
{
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

#ifdef CTR_NATIVE_OT_DEPTH
	/* Undo state 2's read-only depth mask. Unconditional on the state rather
	 * than mirrored off a flag set in Begin: Begin can return early (geometry
	 * not ready) having changed nothing, and re-asserting the frame-wide pair
	 * is idempotent, so this cannot desync the way a paired flag could. */
	if (g_dbg_depthTest == 2)
	{
		C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
	}
#endif

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

void N3DS_TrackDraw_Leaf(int firstIndex, int indexCount, float otIndexLo, float ordinalLo, float ordinalSpan, float invIndexSpan, int atlasBin)
{
	(void)firstIndex;
	(void)indexCount;
	(void)otIndexLo;
	(void)ordinalLo;
	(void)ordinalSpan;
	(void)invIndexSpan;
	(void)atlasBin;
}

void N3DS_TrackDraw_End(void)
{
}

#endif /* CTR_NATIVE_TRACK_RENDERER */

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
 * to the content-side dumps; session 16 proved content byte-correct while
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
	/* SELECT+R (R down-edge while SELECT held): dump emulated PSX VRAM to
	 * sdmc:/vramdump.bin for the texture-corruption investigation. L+R is
	 * powersliding (core CTR mechanic) so it can't be used; SELECT is not a
	 * driving button. Checked here because hidScanInput() already ran above --
	 * a second scan elsewhere would consume the R down-edge. Runs before the
	 * textureless toggle so the two never fire on the same frame. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_R))
	{
		N3DS_DebugCaptureAll();
	}

	/* Host-scriptable trigger (2026-07-21): Azahar's SD card is a plain host
	 * folder, so an autonomous session can drive diagnostics by creating
	 * sdmc:/dumpreq.txt -- no window focus or key injection involved. The
	 * file's first word selects the action (session 16: the A/B toggles need
	 * to be reachable without a gamepad):
	 *   capture (or anything else) -- the full SELECT+R capture set
	 *   subpage     -- toggle g_dbg_disableSubPage (route all to full-page tier)
	 *   single      -- toggle g_dbg_forceSingleTexture
	 *   textureless -- toggle g_dbg_texturelessMode
	 *   triscopes   -- toggle g_dbg_perfTriScopes (per-triangle perf split; taxed)
	 *   drawmerge   -- toggle g_dbg_drawMerge (CTR_NATIVE_DRAW_MERGE builds only)
	 *   renderthread -- toggle g_dbg_renderThread (core-2 render worker routing;
	 *                  OFF falls back to synchronous main-thread rendering)
	 *   lodgeo [N]  -- track LOD distance, percent of retail (CTR_NATIVE_LOD_BIAS)
	 *   lodtex [N]  -- texture LOD distance, percent of retail (same gate)
	 *   fbtest      -- toggle g_dbg_skipFbTriTest (s27 probe: skip resolve's
	 *                  per-triangle fb-mirror test; freeze frames stale while ON)
	 *   otz         -- toggle g_dbg_otzWiden (widened depth stamps, track-
	 *                  renderer rung 1; [OTZ] occupancy line while on)
	 *   trackrender [N] -- native track renderer state (CTR_NATIVE_TRACK_RENDERER
	 *                  builds only): 0=off(retail) 1=on(resident replaces retail)
	 *                  2=both(diagnostic overlay). Bare token cycles 0->1->2->0;
	 *                  'trackrender N' sets a specific state. Also reachable on
	 *                  hardware via SELECT+ZR (a dumpreq file can't be placed
	 *                  mid-race there -- see the button handler below).
	 *   packetdepth -- toggle g_dbg_packetDepth (s29 depth model): packets stamp
	 *                  GTE sz3 and the track shader switches to the same
	 *                  true-depth axis, both at once. Hardware button: SELECT+L.
	 *   gtecapture [N] -- g_dbg_gteCapture (CTR_NATIVE_GTE_CAPTURE builds):
	 *                  1=full 2=store-only(no matrix note) 0=ablated, to price
	 *                  the capture subsystem and its matrix probe separately.
	 *                  Bare token cycles 1->2->0. Hardware button: SELECT+D-UP.
	 *   depthtest [N] -- g_dbg_depthTest, the s29 occlusion split: 0=normal
	 *                  1=no depth test (painter's order) 2=track tests but
	 *                  does not write depth. Bare token cycles 0->1->2->0.
	 *                  Hardware button: SELECT+ZL.
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
				char word[16];
				size_t got = fread(word, 1, sizeof(word) - 1, req);

				word[got] = '\0';
				fclose(req);
				remove("sdmc:/dumpreq.txt");

				if (strncmp(word, "subpage", 7) == 0)
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
				else if (strncmp(word, "fbtest", 6) == 0)
				{
					extern int g_dbg_skipFbTriTest;

					/* s27 probe: ablate resolve's per-triangle fb-mirror
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
				else if (strncmp(word, "trackafter", 10) == 0)
				{
					extern int g_dbg_trackAfterMerge;

					g_dbg_trackAfterMerge ^= 1;
					N3DS_DebugLog(g_dbg_trackAfterMerge ? "[CTR-3DS] track draw: AFTER merge flush" : "[CTR-3DS] track draw: before merge flush (default)");
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
#endif
				else
				{
					N3DS_DebugCaptureAll();
				}
			}
		}
	}

	/* SELECT held + B: automated config sweep. Drive one lap with this on and
	 * every config gets equal exposure to the same content -- see g_dbg_sweep. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_B))
	{
		extern int g_dbg_sweep;

		g_dbg_sweep ^= 1;
		N3DS_DebugLog(g_dbg_sweep ? "[CTR-3DS] sweep: ON (auto-cycling configs)" : "[CTR-3DS] sweep: off");
	}

	/* SELECT held + X: bottom-screen verbosity. Default off (s29) -- the stats
	 * block had grown into a wall the owner couldn't read the live toggles in. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_X))
	{
		extern int g_dbg_statsVerbose;

		g_dbg_statsVerbose ^= 1;
		N3DS_DebugLog(g_dbg_statsVerbose ? "[CTR-3DS] stats: VERBOSE" : "[CTR-3DS] stats: quiet");
	}

	/* SELECT held + Y: step the packet depth bias 0 -> 32 -> 64 -> 128 -> 256
	 * -> 0. Owner-dialled mid-race on hardware ("just pull them a little bit
	 * closer"), which is why it is a button and why the ladder is short: the
	 * judgement is "do racers still sink" vs "can I see them through a wall",
	 * and only a human mid-race can make it. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_Y))
	{
		extern int g_dbg_pktDepthBias;
		char msg[64];

		g_dbg_pktDepthBias = N3DS_NextPktBias(g_dbg_pktDepthBias);
		snprintf(msg, sizeof(msg), "[CTR-3DS] packet depth bias: %d (units nearer)", g_dbg_pktDepthBias);
		N3DS_DebugLog(msg);
	}

	/* SELECT held + ZL: depth-test state cycle 0->1->2->0 (see g_dbg_depthTest
	 * above N3DS_C3DBeginFrame for what each state proves). ZL has been free
	 * since s27 moved resolve-memo off it. Mid-race by design -- the question
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

	/* SELECT held + D-pad DOWN: toggle render-worker routing mid-run (mid-run
	 * flips bracket [PERF] windows by state, killing the cross-run content
	 * confound the same way textureless's combo does below). dumpreq
	 * 'renderthread' covers Azahar (host-writable SD folder), but on
	 * hardware nothing can place that file while the game runs -- FTP and
	 * the game are one-app-at-a-time -- so without a combo the feature's own
	 * hardware A/B would be cross-run. The OFF transition is safe mid-race
	 * by construction: Route() fences the worker before the first direct
	 * call (native_render_queue.c). */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_DDOWN))
	{
		extern int g_dbg_renderThread;

		g_dbg_renderThread ^= 1;
		N3DS_DebugLog(g_dbg_renderThread ? "[CTR-3DS] render worker routing: ON" : "[CTR-3DS] render worker routing: OFF (sync on main thread)");
	}

	/* SELECT held + D-pad RIGHT: textureless. Was bare SELECT (2026-07-20),
	 * mirroring PC's F2, but once other held-SELECT combos joined the family
	 * every stray SELECT tap was silently flipping textures too -- owner
	 * moved it onto the same held-SELECT d-pad convention as the rest
	 * (2026-07-29). SELECT still feeds N3DS_BTN_SELECT below as normal. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_DRIGHT))
	{
		g_dbg_texturelessMode ^= 1;
		N3DS_DebugLog(g_dbg_texturelessMode ? "[CTR-3DS] textureless mode: 1" : "[CTR-3DS] textureless mode: 0");
	}

#ifdef CTR_NATIVE_DIRECT_EMIT
	/* SELECT held + D-pad LEFT: direct-emit fast path A/B, part of the
	 * held-SELECT d-pad family (DOWN=render worker, RIGHT=textureless).
	 * Same rationale as the others: hardware cannot place a dumpreq file
	 * mid-race, and only a mid-run flip brackets [PERF] windows on
	 * identical content. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_DLEFT))
	{
		extern int g_dbg_directEmit;

		g_dbg_directEmit ^= 1;
		N3DS_DebugLog(g_dbg_directEmit ? "[CTR-3DS] direct emit: ON" : "[CTR-3DS] direct emit: OFF");
	}
#endif

#if defined(CTR_NATIVE_GTE_CAPTURE) && CTR_NATIVE_GTE_CAPTURE
	/* SELECT held + D-pad UP: GTE capture cost A/B (s29). The depth model the
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
	 * -- the s29 depth model's mid-race A/B. Flips BOTH sides at once (packets
	 * stamp sz3, track shader switches to the true-depth axis), because either
	 * alone puts the two geometry sources on different axes. This is the
	 * toggle the "karts through walls" objection gets tested against, so it
	 * has to be a button: a dumpreq file cannot be placed mid-race. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_L))
	{
		extern int g_dbg_packetDepth;

		g_dbg_packetDepth ^= 1;
		N3DS_DebugLog(g_dbg_packetDepth ? "[CTR-3DS] packet depth: ON (sz3 true-depth axis, both sources)"
		                                : "[CTR-3DS] packet depth: off (emit ordinals)");
	}
#endif

#ifdef CTR_NATIVE_TRACK_RENDERER
	/* SELECT held + ZR: native track renderer state cycle 0->1->2->0 (rung
	 * 3a). Reassigned from the s27 fbtest probe (design doc 12b: fbtest's
	 * question is closed/null) -- ZR was fbtest's combo, not resolve-memo's;
	 * the s26 resolve-memo combo lived on SELECT+ZL, which this pass simply
	 * frees (its dumpreq token 'resolvememo' stays reachable in Azahar, it
	 * just has no hardware button anymore). Both 'fbtest' and 'resolvememo'
	 * dumpreq tokens stay reachable in Azahar either way -- only the
	 * hardware BUTTON combos are freed here, because a mid-race hardware A/B
	 * needs a BUTTON (dumpreq is unplaceable while the game holds the SD
	 * card) and trackrender is now the toggle that matters for that. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_ZR))
	{
		extern int g_dbg_trackRender;
		static const char *const names[3] = {"OFF (retail)", "ON (resident replaces retail)", "BOTH (resident + retail overlay)"};
		char msg[64];

		/*
		 * TWO states on the button, not three. BOTH is not a way to play -- it
		 * draws the level twice -- and cycling through it mid-race costs a
		 * press every time the owner wants to A/B the thing that matters. It
		 * stays reachable as 'trackrender 2' via dumpreq, because drawing both
		 * at once is still the fastest visual check for the two renderers
		 * disagreeing about position or depth: they fight each other on screen
		 * in one frame instead of having to be inferred from a sunk kart.
		 *
		 * From 2 the button drops to 0 rather than sticking, so a dumpreq into
		 * BOTH can always be escaped from the console.
		 */
		g_dbg_trackRender = (g_dbg_trackRender == 0) ? 1 : 0;
		snprintf(msg, sizeof(msg), "[CTR-3DS] track render: %s", names[g_dbg_trackRender]);
		N3DS_DebugLog(msg);
	}
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
