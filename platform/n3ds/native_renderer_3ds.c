/*
 * Nintendo 3DS renderer backend -- phase 1 scaffold.
 *
 * Replaces platform/native_renderer.c (OpenGL 3.3 + glad), which cannot run on
 * the PICA200. This file satisfies every signature in
 * include/platform/native_renderer.h so the rest of the unity build compiles
 * unmodified.
 *
 * What is REAL here: the CPU-side PSX VRAM mirror and every operation that only
 * touches it (clear/copy/read/save-state). That code is pure memory work, it is
 * what the boot splash path uploads into, and phase 1c presents from it -- so it
 * is implemented properly rather than stubbed.
 *
 * What is STUBBED here: everything that would talk to a GPU (textures, shaders,
 * blend/stencil state, triangle submission). Those become citro3d in phase 2.
 *
 * The top screen currently hosts a libctru text console so that a running main
 * loop is observable before any real pixels exist.
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

internal u16 NativeRenderer3DS_PackBGR555(u8 r, u8 g, u8 b)
{
	return (u16)(((r >> 3) & 31) | (((g >> 3) & 31) << 5) | (((b >> 3) & 31) << 10));
}

int NativeRenderer_InitialiseRender(char *windowName, int width, int height, int fullscreen)
{
	(void)fullscreen;

	N3DS_GfxInit();

	s_rendererInitialised = 1;

	printf("\x1b[2J");
	printf("CTR-Native / Nintendo 3DS\n");
	printf("-------------------------\n");
	printf("%s\n", (windowName != NULL) ? windowName : "(no title)");
	printf("PSX display: %dx%d\n", width, height);
	printf("VRAM mirror: %d KiB\n", (int)(sizeof(vram) / 1024));
	printf("\nRenderer is a phase-1 stub: no geometry\nis drawn yet. VRAM mirror is live.\n\n");

	N3DS_DebugLog("[CTR-3DS] renderer init ok");

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
	if ((s_frameCounter & 63) == 0)
	{
		char line[96];

		snprintf(line, sizeof(line), "[CTR-3DS] frames=%lu vram[0]=%04x",
			(unsigned long)s_frameCounter, (unsigned)vram[0]);
		printf("\x1b[18;1H%s   ", line);
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
