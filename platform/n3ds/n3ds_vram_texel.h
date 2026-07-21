#ifndef N3DS_VRAM_TEXEL_H
#define N3DS_VRAM_TEXEL_H

/*
 * Single home for the PSX texture-page texel addressing math, shared by the
 * decode cache (N3DS_DecodeAndTilePage) and the offscreen software raster
 * (n3ds_offscreen_raster.c) so there is exactly one copy to get wrong.
 * tools/texdump/cache_verify.py is the independent host-side reference
 * implementation this must keep agreeing with.
 *
 * Units, since they have bitten before: page origins are VRAM WORDS
 * (u16 columns), texel coordinates are TEXELS. A page is 256 TEXELS wide in
 * every mode (64 words at 4bpp, 128 at 8bpp, 256 at 16bpp).
 *
 * CLUT rows wrap within their own VRAM scanline (masked with VRAM_WIDTH-1):
 * PC's VRAM sampler inherits GL_REPEAT and wraps S independently of T, so it
 * can never walk into the next scanline; a flat array index silently would.
 * An 8-bit CLUT (256 entries from a 16-aligned origin) genuinely can
 * overflow; the 4-bit mask is kept for symmetry.
 *
 * No libctru dependency: this header is also compiled host-side by
 * tools/offscreen_raster_test.
 */

#include <platform/native_renderer_types.h>

static inline u16 N3DS_VramFetchTexel(const u16 *vramData, s16 texPage, s16 clut, TexFormat format, int pageU, int pageV)
{
	const int pageOriginX = (texPage & 0xF) * 64;
	const int pageOriginY = (texPage & 0x10) ? 256 : 0;
	const int wordRow = pageOriginY + pageV;

	switch (format)
	{
	case TF_4_BIT:
	{
		const int wordCol = (pageOriginX + (pageU >> 2)) & (VRAM_WIDTH - 1);
		const u16 word = vramData[wordRow * VRAM_WIDTH + wordCol];
		const int nibble = (word >> ((pageU & 3) * 4)) & 0xF;
		const int clutOriginX = (clut & 0x3F) * 16;
		const int clutOriginY = clut >> 6;

		return vramData[clutOriginY * VRAM_WIDTH + ((clutOriginX + nibble) & (VRAM_WIDTH - 1))];
	}
	case TF_8_BIT:
	{
		const int wordCol = (pageOriginX + (pageU >> 1)) & (VRAM_WIDTH - 1);
		const u16 word = vramData[wordRow * VRAM_WIDTH + wordCol];
		const int index = (word >> ((pageU & 1) * 8)) & 0xFF;
		const int clutOriginX = (clut & 0x3F) * 16;
		const int clutOriginY = clut >> 6;

		return vramData[clutOriginY * VRAM_WIDTH + ((clutOriginX + index) & (VRAM_WIDTH - 1))];
	}
	case TF_16_BIT:
	case TF_32_BIT_RGBA: /* unreachable from the raster/decode paths; see their headers */
	default:
	{
		return vramData[wordRow * VRAM_WIDTH + ((pageOriginX + pageU) & (VRAM_WIDTH - 1))];
	}
	}
}

#endif
