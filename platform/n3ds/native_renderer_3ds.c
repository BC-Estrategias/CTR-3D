/*
 * Nintendo 3DS renderer backend -- phase 2a in progress.
 *
 * Replaces platform/native_renderer.c (OpenGL 3.3 + glad), which cannot run on
 * the PICA200. This file satisfies every signature in
 * include/platform/native_renderer.h so the rest of the unity build compiles
 * unmodified. Design: docs/CITRO3D_RENDERER_DESIGN.md.
 *
 * What is REAL here: the CPU-side PSX VRAM mirror and every operation that only
 * touches it (clear/copy/read/save-state); citro3d init, the compiled vertex
 * shader, and the top-screen render target (all behind n3ds_backend.h -- this
 * file is part of the unity build and can never see libctru/citro3d types
 * directly, same reason as everything else that crosses that boundary).
 *
 * What is STILL STUBBED here: the CPU texture decode cache and the real
 * GrVertex -> N3DS_Vertex conversion (design doc sections 1 and 6) --
 * NativeRenderer_SwapWindow currently draws one hardcoded test triangle each
 * frame instead, to verify the citro3d pipeline itself (shader compile,
 * attribute/buffer setup, draw, present) independent of the real data path.
 * Textures, blending, stencil, offscreen, framebuffer readback, and stereo
 * are all still stubbed/deferred per the design doc's staging.
 *
 * The bottom screen hosts a libctru text console (the stats block) so a
 * running main loop stays observable while the top screen is citro3d's.
 */

#include <stdio.h>
#include <string.h>

#include <macros.h>
#include <platform/native_renderer.h>

#include "n3ds_backend.h"

/*
 * PSX VRAM mirror: 1024x512 16-bit. native_renderer.c owns this on the host and
 * nothing outside the renderer references it, so the 3DS backend owns its own.
 */
u16 vram[VRAM_WIDTH * VRAM_HEIGHT];

/*
 * Renderer config the PSX GPU layer reads directly (native_gpu.c consults it
 * when building texcoords). Defined by native_renderer.c on the host, so the
 * 3DS backend has to carry it too. Nearest-neighbour matches PSX output.
 */
int g_cfg_bilinearFiltering = 0;

global_variable int s_rendererInitialised = 0;
global_variable u32 s_frameCounter = 0;
global_variable unsigned long s_lastStatFrames = 0;
global_variable unsigned long s_lastStatVblanks = 0;

/*
 * TEMPORARY (phase 2a pipeline verification only -- see the file header).
 * A fixed triangle in PSX screen-pixel space, drawn every frame through the
 * real N3DS_C3DUploadVertices/DrawTriangles path so that path is exercised
 * before the real GrVertex conversion (design doc section 6) replaces this.
 * Remove once that lands.
 */
global_variable const N3DS_Vertex s_testTriangle[3] = {
	{200.0f, 40.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
	{100.0f, 200.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f},
	{300.0f, 200.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f},
};

internal u16 NativeRenderer3DS_PackBGR555(u8 r, u8 g, u8 b)
{
	return (u16)(((r >> 3) & 31) | (((g >> 3) & 31) << 5) | (((b >> 3) & 31) << 10));
}

int NativeRenderer_InitialiseRender(char *windowName, int width, int height, int fullscreen)
{
	(void)fullscreen;

	N3DS_GfxInit();

	if (!N3DS_C3DInit())
	{
		N3DS_DebugLog("[CTR-3DS] citro3d init FAILED");
		return 0;
	}

	s_rendererInitialised = 1;

	printf("\x1b[2J");
	printf("CTR-Native / Nintendo 3DS\n");
	printf("-------------------------\n");
	printf("%s\n", (windowName != NULL) ? windowName : "(no title)");
	printf("PSX display: %dx%d\n", width, height);
	printf("VRAM mirror: %d KiB\n", (int)(sizeof(vram) / 1024));
	printf("\nPhase 2a: citro3d is live on the top\nscreen (see file header for scope).\n\n");

	N3DS_DebugLog("[CTR-3DS] renderer init ok, citro3d ready");

	return 1;
}

int NativeRenderer_InitialisePSX(void)
{
	memset(vram, 0, sizeof(vram));
	return 1;
}

void NativeRenderer_Shutdown(void)
{
	if (!s_rendererInitialised)
	{
		return;
	}

	s_rendererInitialised = 0;
	N3DS_C3DExit();
	N3DS_GfxExit();
}

void NativeRenderer_ResetDevice(void)
{
}

void NativeRenderer_BeginScene(void)
{
}

void NativeRenderer_EndScene(void)
{
}

void NativeRenderer_UpdateSwapIntervalState(int swapInterval)
{
	(void)swapInterval;
}

void NativeRenderer_SwapWindow(void)
{
	if (!s_rendererInitialised)
	{
		return;
	}

	/*
	 * Frame pacing lives in VSync() (platform/native_platform.c), which runs off
	 * the tick clock. Only push the console's backing buffer here; do not wait on
	 * a second vblank source or the 30fps gate would be paced twice.
	 */
	s_frameCounter++;

	/* TEMPORARY -- see s_testTriangle's comment. Real path replaces this. */
	N3DS_C3DBeginFrame();
	N3DS_C3DUploadVertices(s_testTriangle, 3);
	N3DS_C3DDrawTriangles(0, 1);
	N3DS_C3DEndFrame();

	if ((s_frameCounter & 31) == 0)
	{
		/*
		 * On-screen stats block. Getting files off the console is awkward (the
		 * SD is under the backplate and only one homebrew app runs at a time,
		 * so an FTP server cannot be up while this is), and a photo of the top
		 * screen carries everything worth knowing. Also mirrored to the log.
		 */
		const unsigned long vblanks = (unsigned long)Platform_GetVBlankCount();
		const unsigned long frames = (unsigned long)s_frameCounter;
		const unsigned long heapKiB = N3DS_HeapUsedBytes() / 1024UL;
		unsigned long avgX10 = 0;
		unsigned long nowX10 = 0;
		char line[128];

		if (vblanks > 0)
		{
			// The retail gate is two vblanks per flip, i.e. 30fps.
			avgX10 = (600UL * frames) / vblanks;
		}

		/*
		 * Report the rate over the last window as well as the average since
		 * boot. A falling average hides how bad the current rate is, because
		 * every earlier good frame keeps dragging it up.
		 */
		if (vblanks > s_lastStatVblanks)
		{
			nowX10 = (600UL * (frames - s_lastStatFrames)) / (vblanks - s_lastStatVblanks);
		}
		s_lastStatFrames = frames;
		s_lastStatVblanks = vblanks;

		printf("\x1b[16;1H---- 3DS port stats ----------------\n");
		printf("frames %-8lu vblanks %-8lu\n", frames, vblanks);
		printf("fps    %lu.%lu now / %lu.%lu avg (target 30)   \n",
		       nowX10 / 10UL, nowX10 % 10UL, avgX10 / 10UL, avgX10 % 10UL);
		/* Break the heap down: XA now reports ring occupancy (sectors buffered
		 * / ring depth) and underrun count instead of arena KiB -- this is the
		 * same instrumentation that originally proved the old arena-growth bug
		 * (BUGREPORT_audio_arena_growth.md); after the fix it should show a
		 * small number that stays flat across track transitions instead of
		 * one that doubles over a session. */
		printf("heap   %-6lu  xaBuf %d/%d underrun %-4lu voice %-5d KiB\n", heapKiB,
		       NativeAudio_XAStreamSectorsBuffered(), NATIVE_AUDIO_XA_RING_SECTORS, s_audio.xaStream.underrunEvents,
		       s_audio.voicePcmArena.capacity / 1024);
		printf("state  %-4d vram[0] %04x            \n", (int)sdata->mainGameState, (unsigned)vram[0]);

		snprintf(line, sizeof(line),
		         "[CTR-3DS] frames=%lu vbl=%lu fps_now=%lu.%lu heapKiB=%lu xaBuf=%d/%d underrun=%lu voiceKiB=%d state=%d",
		         frames, vblanks, nowX10 / 10UL, nowX10 % 10UL, heapKiB,
		         NativeAudio_XAStreamSectorsBuffered(), NATIVE_AUDIO_XA_RING_SECTORS, s_audio.xaStream.underrunEvents,
		         s_audio.voicePcmArena.capacity / 1024, (int)sdata->mainGameState);
		N3DS_DebugLog(line);
	}

	N3DS_GfxPresent();
}

void NativeRenderer_StoreFrameBuffer(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
}

void NativeRenderer_PresentVRAMDisplay(void)
{
}

void NativeRenderer_PresentVRAMRect(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
}

void NativeRenderer_SaveVRAM(const char *outputFileName, int x, int y, int width, int height, int readFromFramebuffer)
{
	(void)outputFileName; (void)x; (void)y; (void)width; (void)height; (void)readFromFramebuffer;
}

void NativeRenderer_Clear(int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
	(void)x; (void)y; (void)w; (void)h; (void)r; (void)g; (void)b;
}

void NativeRenderer_ClearVRAM(int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
	const u16 colour = NativeRenderer3DS_PackBGR555(r, g, b);
	int row;

	if ((w <= 0) || (h <= 0) || (x < 0) || (y < 0))
	{
		return;
	}

	if (((x + w) > VRAM_WIDTH) || ((y + h) > VRAM_HEIGHT))
	{
		return;
	}

	for (row = 0; row < h; row++)
	{
		u16 *dst = vram + x + ((y + row) * VRAM_WIDTH);
		int col;

		for (col = 0; col < w; col++)
		{
			dst[col] = colour;
		}
	}
}

void NativeRenderer_CopyVRAM(u16 *src, int x, int y, int w, int h, int dstX, int dstY)
{
	int stride = w;
	u16 *dst;
	int row;

	if ((w <= 0) || (h <= 0))
	{
		return;
	}

	if (src == NULL)
	{
		/* MoveImage: VRAM-to-VRAM. No host framebuffer exists to reconcile yet. */
		src = vram;
		stride = VRAM_WIDTH;
	}

	src += x + (y * stride);
	dst = vram + dstX + (dstY * VRAM_WIDTH);

	for (row = 0; row < h; row++)
	{
		memcpy(dst, src, (size_t)w * sizeof(u16));
		dst += VRAM_WIDTH;
		src += stride;
	}
}

void NativeRenderer_ReadVRAM(u16 *dst, int x, int y, int dstW, int dstH)
{
	u16 *src = vram + x + (VRAM_WIDTH * y);
	int row;

	for (row = 0; row < dstH; row++)
	{
		memcpy(dst, src, (size_t)dstW * sizeof(u16));
		dst += dstW;
		src += VRAM_WIDTH;
	}
}

void NativeRenderer_UpdateVRAM(void)
{
}

void NativeRenderer_ReadFramebufferDataToVRAM(void)
{
}

void NativeRenderer_DiscardFramebufferReadback(void)
{
}

int NativeRenderer_GetVRAMStateSize(void)
{
	return (int)sizeof(vram);
}

int NativeRenderer_CaptureVRAMState(void *dst, int dstSize)
{
	if ((dst == NULL) || (dstSize < (int)sizeof(vram)))
	{
		return 0;
	}

	memcpy(dst, vram, sizeof(vram));
	return 1;
}

int NativeRenderer_RestoreVRAMState(const void *src, int srcSize)
{
	if ((src == NULL) || (srcSize < (int)sizeof(vram)))
	{
		return 0;
	}

	memcpy(vram, src, sizeof(vram));
	return 1;
}

TextureID NativeRenderer_GetVRAMTexture(void)
{
	return 0;
}

TextureID NativeRenderer_GetWhiteTexture(void)
{
	return 0;
}

void NativeRenderer_SetBlendMode(BlendMode blendMode)
{
	(void)blendMode;
}

void NativeRenderer_SetStencilMode(int drawPrim)
{
	(void)drawPrim;
}

void NativeRenderer_SetOffscreenState(const RECT16 *offscreenRect, const DISPENV *displayEnv, int enable)
{
	(void)offscreenRect; (void)displayEnv; (void)enable;
}

void NativeRenderer_SetupClipMode(const RECT16 *clipRect, const DISPENV *displayEnv, int enable)
{
	(void)clipRect; (void)displayEnv; (void)enable;
}

void NativeRenderer_SetTexture(TextureID texture, TexFormat texFormat)
{
	(void)texture; (void)texFormat;
}

void NativeRenderer_SetOverrideTextureSize(int width, int height)
{
	(void)width; (void)height;
}

void NativeRenderer_SetPSXTextureSemiTransPass(int pass)
{
	(void)pass;
}

void NativeRenderer_SetPSXTextureOutputSTP(int enabled)
{
	(void)enabled;
}

void NativeRenderer_SetPSXDrawMaskSet(int maskSet)
{
	(void)maskSet;
}

void NativeRenderer_UpdateVertexBuffer(const GrVertex *vertices, int count)
{
	(void)vertices; (void)count;
}

void NativeRenderer_DrawTriangles(int startVertex, int triangles)
{
	(void)startVertex; (void)triangles;
}

void NativeRenderer_PushDebugLabel(const char *label)
{
	(void)label;
}

void NativeRenderer_PopDebugLabel(void)
{
}
