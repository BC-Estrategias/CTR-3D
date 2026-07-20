/*
 * libctru side of the backend boundary. This file is its own translation unit
 * and must never include game headers -- see n3ds_backend.h for why.
 */

#include <3ds.h>
#include <citro3d.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "n3ds_backend.h"
#include "ctr_shbin.h"

/*
 * Diagnostics go to two places because neither alone is sufficient:
 *   - svcOutputDebugString, which Luma surfaces on hardware, but which Azahar's
 *     release build compiles out (its LOG_DEBUG is stripped).
 *   - an absolute-path file on the SD card, which survives on both and can be
 *     read back from the host without a screenshot. The path is absolute
 *     because the working directory is not dependable this early.
 */
#define N3DS_LOG_PATH "sdmc:/ctr3ds.log"

static FILE *s_logFile = NULL;

void N3DS_EarlyInit(void)
{
	consoleDebugInit(debugDevice_SVC);

	s_logFile = fopen(N3DS_LOG_PATH, "w");
}

void N3DS_DebugLog(const char *msg)
{
	if (msg == NULL)
	{
		return;
	}

	svcOutputDebugString(msg, (int)strlen(msg));

	if (s_logFile != NULL)
	{
		fputs(msg, s_logFile);
		fputc('\n', s_logFile);
		fflush(s_logFile);
	}
}

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
static C3D_Tex s_atlasTextures[N3DS_ATLAS_COUNT];

int N3DS_C3DInit(void)
{
	C3D_AttrInfo *attrInfo;
	C3D_BufInfo *bufInfo;
	C3D_TexEnv *env;

	if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE))
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
	 * Screen-pixel space -> clip space, matching native_renderer.c's own
	 * Projection uniform in spirit: PSX screen coordinates go straight in,
	 * there is no camera. bottom=240/top=0 (inverted from the usual
	 * math convention) reproduces PSX/PC's Y-down screen convention -- Y=0
	 * is the top of the image, Y increases downward.
	 */
	Mtx_OrthoTilt(&s_projection, 0.0f, 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);

	attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 2); /* v0 = position (x, y) */
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2); /* v1 = texcoord (u, v) */
	AttrInfo_AddLoader(attrInfo, 2, GPU_FLOAT, 4); /* v2 = color (r, g, b, a) */

	s_vertexBufferMem = linearAlloc((size_t)N3DS_MAX_FRAME_VERTICES * sizeof(N3DS_Vertex));
	if (s_vertexBufferMem == NULL)
	{
		return 0;
	}

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

	/*
	 * Punch-through sentinel (design doc section 2): the decode cache encodes
	 * alpha as 0 = "PS1 pure-black-is-invisible" texel, 128 = opaque, 255 =
	 * STP-on. Phase 2a is single-pass only (no STP 2-pass blending yet, that's
	 * 2b), so one GEQUAL-1 test rejects exactly the punch-through case and
	 * passes everything else -- exactly the design doc's "single-pass" mode.
	 * Set once here rather than per-draw since 2a never needs a different test.
	 */
	C3D_AlphaTest(true, GPU_GEQUAL, 1);

	C3D_CullFace(GPU_CULL_NONE);

	{
		int i;
		for (i = 0; i < N3DS_ATLAS_COUNT; i++)
		{
			if (!C3D_TexInit(&s_atlasTextures[i], N3DS_ATLAS_DIM, N3DS_ATLAS_DIM, GPU_RGBA8))
			{
				return 0;
			}
			C3D_TexSetFilter(&s_atlasTextures[i], GPU_NEAREST, GPU_NEAREST);
			C3D_TexSetWrap(&s_atlasTextures[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		}
	}

	return 1;
}

void N3DS_C3DExit(void)
{
	int i;

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

void N3DS_C3DBeginFrame(void)
{
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C3D_RenderTargetClear(s_renderTarget, C3D_CLEAR_ALL, 0x000000FF, 0);
	C3D_FrameDrawOn(s_renderTarget);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uLocProjection, &s_projection);

	/* Every draw this frame appends into the SAME persistent buffer at a
	 * fresh offset (see the header comment on N3DS_C3DAppendVertices) --
	 * reset the write cursor once per frame, not per draw. */
	s_vertexCursor = 0;
}

void N3DS_C3DEndFrame(void)
{
	C3D_FrameEnd(0);
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

	return base;
}

void N3DS_C3DDrawTriangles(int startVertex, int triangleCount)
{
	C3D_DrawArrays(GPU_TRIANGLES, startVertex, triangleCount * 3);
}

/* 8x8 texels * 4 bytes/texel (RGBA8) -- one PICA200 tile, tiled contiguously. */
#define N3DS_TILE_BYTES (64 * 4)

void N3DS_C3DUploadDecodedCell(int atlasIndex, int cellX, int cellY, const void *tiled256RGBA8)
{
	/*
	 * Manual sub-rect write -- see the header comment for why C3D_TexUpload
	 * can't be used here. PICA200 tiling addresses 8x8-texel tiles in simple
	 * row-major order across a texture's FULL width (verified against
	 * tex3ds's reference swizzle, see native_renderer_3ds.c). A cell is a
	 * 32x32-tile square (256/8) positioned at tile offset (cellX*32, cellY*32)
	 * within the atlas's 128x128-tile grid (1024/8); each of its 32 tile-rows
	 * is 32 CONSECUTIVE tiles, which -- because tile-column is the
	 * fastest-varying index in row-major order -- are contiguous in the
	 * atlas's memory too, so one memcpy per tile-row is exact and correct,
	 * not an approximation.
	 */
	const int atlasTilesPerRow = N3DS_ATLAS_DIM / 8;
	const int cellTilesPerRow = N3DS_DECODE_TEXTURE_DIM / 8;
	const int cellBaseTileRow = cellY * cellTilesPerRow;
	const int cellBaseTileCol = cellX * cellTilesPerRow;
	const u8 *src = (const u8 *)tiled256RGBA8;
	u8 *atlasData;
	int localTileRow;

	if ((atlasIndex < 0) || (atlasIndex >= N3DS_ATLAS_COUNT))
	{
		return;
	}

	atlasData = (u8 *)s_atlasTextures[atlasIndex].data;

	for (localTileRow = 0; localTileRow < cellTilesPerRow; localTileRow++)
	{
		const int atlasTileRow = cellBaseTileRow + localTileRow;
		u8 *dstRow = atlasData + (size_t)((atlasTileRow * atlasTilesPerRow) + cellBaseTileCol) * N3DS_TILE_BYTES;
		const u8 *srcRow = src + (size_t)localTileRow * cellTilesPerRow * N3DS_TILE_BYTES;

		memcpy(dstRow, srcRow, (size_t)cellTilesPerRow * N3DS_TILE_BYTES);
	}
}

void N3DS_C3DBindAtlas(int atlasIndex)
{
	if ((atlasIndex < 0) || (atlasIndex >= N3DS_ATLAS_COUNT))
	{
		return;
	}

	C3D_TexBind(0, &s_atlasTextures[atlasIndex]);
}

unsigned int N3DS_ReadButtons(void)
{
	unsigned int out = 0;
	u32 held;

	hidScanInput();
	held = hidKeysHeld();

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
