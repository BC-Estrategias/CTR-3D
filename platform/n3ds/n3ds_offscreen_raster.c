/*
 * CPU software raster for dfe=0 (offscreen render-to-VRAM) splits -- the 3DS
 * implementation of the compositing path PC serves with an FBO + readback.
 * Design and evidence chain: docs/OFFSCREEN_RENDER_VRAM_DESIGN.md.
 *
 * Consumes the exact same post-expansion GrVertex triangle stream that
 * NativeRenderer_DrawTriangles receives (positions clip-rect-local, see
 * native_gpu.c DrawEnvOffset for the dfe=0 branch) and writes texels
 * straight into the CPU VRAM mirror at the drawenv clip rect. The decode
 * cache picks the content up through the normal dirty-cell invalidation --
 * no delivery path of its own.
 *
 * Reference behavior is PC's GL pipeline (native_renderer.c), because that
 * is what the Phase 1 golden harness diffs against -- notably:
 *  - coverage at pixel centers with a top-left fill rule (all-integer
 *    half-space test on 2x-scaled coordinates; GL rasterizes at centers,
 *    and matching it keeps golden mismatches confined to edges);
 *  - texel index = floor(u interpolated at the pixel center) -- the GL
 *    NEAREST sample of the 4/8/16-bit samplePSX shaders;
 *  - textured modulate = texel * vertexColor * 2 (bright=2 pipeline),
 *    computed in 8 bits and truncated to 5 at the VRAM write, matching
 *    shader-float -> RGBA8 FBO -> CopyRGBAFramebufferToVRAM (>>3);
 *  - punch-through: raw texel 0x0000 never writes;
 *  - textured semi-trans blends ONLY texels with STP set, in one per-texel
 *    pass (equivalent to PC's two-pass shader split, see DrawSplit);
 *  - written bit15 = maskSet OR (textured semi-trans-capable AND sampled
 *    texel STP), matching the fragment alpha PC's flush packs into VRAM.
 *
 * Known, accepted deviation: PC accumulates a whole session in an RGBA8 FBO
 * and truncates to 5 bits once at flush, so STACKED semi-transparent layers
 * blend in 8-bit there but in 5-bit here (as real PS1 hardware did). Single
 * blended layers stay within +-1 per channel of PC; deep stacks may not.
 * The Phase 1 harness quantifies this instead of hiding it.
 *
 * No libctru dependency -- also compiled host-side by
 * tools/offscreen_raster_test for the golden diff.
 */

#include "platform/n3ds/n3ds_vram_texel.h"

/* Round-to-nearest of num/den for den > 0, num >= 0. */
#define OFSRAST_DIV_ROUND(num, den) (((num) * 2 + (den)) / ((den) * 2))

typedef struct
{
	s32 stepX;
	s32 stepY;
	s32 row; /* value at the bounding box's first sample point */
} OfsRastEdge;

internal s32 OfsRast_EdgeInit(OfsRastEdge *edge, s32 ax, s32 ay, s32 bx, s32 by, s32 px, s32 py)
{
	/* Half-space function for edge a->b evaluated at p, y-down screen space:
	 * positive on the interior side for a clockwise-on-screen triangle. */
	edge->stepX = -(by - ay) * 2; /* per +1 pixel: sample point moves +2 in doubled units */
	edge->stepY = (bx - ax) * 2;
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

internal int OfsRast_EdgeIsTopLeft(s32 ax, s32 ay, s32 bx, s32 by)
{
	/* Top edge: exactly horizontal, running left (end left of start). Left
	 * edge: running down. Screen-clockwise winding, y-down. */
	if (ay == by)
	{
		return bx < ax;
	}
	return by > ay;
}

internal u16 OfsRast_BlendPSX(BlendMode blendMode, u16 dstColor, int srcR5, int srcG5, int srcB5)
{
	int dstR5 = dstColor & 0x1F;
	int dstG5 = (dstColor >> 5) & 0x1F;
	int dstB5 = (dstColor >> 10) & 0x1F;
	int outR5, outG5, outB5;

	switch (blendMode)
	{
	case BM_AVERAGE:
	{
		outR5 = (dstR5 + srcR5 + 1) >> 1;
		outG5 = (dstG5 + srcG5 + 1) >> 1;
		outB5 = (dstB5 + srcB5 + 1) >> 1;
		break;
	}
	case BM_ADD:
	{
		outR5 = dstR5 + srcR5;
		outG5 = dstG5 + srcG5;
		outB5 = dstB5 + srcB5;
		break;
	}
	case BM_SUBTRACT:
	{
		outR5 = dstR5 - srcR5;
		outG5 = dstG5 - srcG5;
		outB5 = dstB5 - srcB5;
		break;
	}
	case BM_ADD_QUATER_SOURCE:
	{
		outR5 = dstR5 + ((srcR5 + 2) >> 2);
		outG5 = dstG5 + ((srcG5 + 2) >> 2);
		outB5 = dstB5 + ((srcB5 + 2) >> 2);
		break;
	}
	case BM_NONE:
	default:
	{
		outR5 = srcR5;
		outG5 = srcG5;
		outB5 = srcB5;
		break;
	}
	}

	if (outR5 < 0) { outR5 = 0; }
	if (outR5 > 31) { outR5 = 31; }
	if (outG5 < 0) { outG5 = 0; }
	if (outG5 > 31) { outG5 = 31; }
	if (outB5 < 0) { outB5 = 0; }
	if (outB5 > 31) { outB5 = 31; }

	return (u16)(outR5 | (outG5 << 5) | (outB5 << 10));
}

/*
 * Wipe the accumulation rect to 0x0000 -- the PC-contract session start.
 * PC clears its FBO to transparent black at enable and its flush writes the
 * WHOLE rect back unconditionally (CopyRGBAFramebufferToVRAM has no alpha
 * test), so undrawn pixels become
 * 0x0000: PS1's transparent-black punch-through texel, which the decal
 * billboards rely on for their background. Retail never auto-cleared
 * (isbg=0 at both call sites) but PC's wipe is the proven-good behavior the
 * golden harness verifies against.
 */
void N3DS_OffscreenRasterClear(u16 *vramData, const RECT16 *clip)
{
	int x0 = clip->x;
	int y0 = clip->y;
	int x1 = clip->x + clip->w;
	int y1 = clip->y + clip->h;
	int y;

	if (x0 < 0) { x0 = 0; }
	if (y0 < 0) { y0 = 0; }
	if (x1 > VRAM_WIDTH) { x1 = VRAM_WIDTH; }
	if (y1 > VRAM_HEIGHT) { y1 = VRAM_HEIGHT; }
	if ((x0 >= x1) || (y0 >= y1))
	{
		return;
	}

	for (y = y0; y < y1; y++)
	{
		memset(&vramData[y * VRAM_WIDTH + x0], 0, (size_t)(x1 - x0) * sizeof(u16));
	}
}

/*
 * Rasterize one split's triangles into the mirror. verts/triangles are the
 * split's slice of the frame vertex buffer (positions clip-local);
 * blendMode/texFormat and the flags mirror what DrawSplit hands the
 * renderer contract for this split:
 *   textured           split textureId != white sentinel
 *   texturedSemiTrans  psxTexturedSemiTrans (per-texel STP decides blending)
 *   textureOutputSTP   psxTextureOutputSTP (written bit15 carries texel STP)
 *   maskSet            E6 force-set: written bit15 = 1
 *   maskCheck          E6 mask-check: pixels with dst bit15 set are skipped
 */
void N3DS_OffscreenRasterTriangles(u16 *vramData, const RECT16 *clip, const GrVertex *verts, int triangles, BlendMode blendMode, TexFormat texFormat,
                                   int textured, int texturedSemiTrans, int textureOutputSTP, int maskSet, int maskCheck)
{
	const int untexturedBlend = (!textured) && (blendMode != BM_NONE);
	int tri;

	for (tri = 0; tri < triangles; tri++)
	{
		const GrVertex *v0 = &verts[tri * 3 + 0];
		const GrVertex *v1 = &verts[tri * 3 + 1];
		const GrVertex *v2 = &verts[tri * 3 + 2];

		/* Doubled coordinates: sample points sit at pixel centers
		 * (2x+1, 2y+1) so the integer half-space test reproduces GL's
		 * center-sampled coverage exactly. */
		s32 x0 = (s32)v0->x * 2, y0 = (s32)v0->y * 2;
		s32 x1 = (s32)v1->x * 2, y1 = (s32)v1->y * 2;
		s32 x2 = (s32)v2->x * 2, y2 = (s32)v2->y * 2;

		s32 area2 = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);

		if (area2 == 0)
		{
			continue;
		}
		if (area2 < 0)
		{
			/* Wind screen-clockwise; attributes follow their vertices. */
			const GrVertex *swapVert = v1;
			s32 swapX, swapY;
			v1 = v2;
			v2 = swapVert;
			swapX = x1; x1 = x2; x2 = swapX;
			swapY = y1; y1 = y2; y2 = swapY;
			area2 = -area2;
		}

		/* Bounding box in pixels, clipped to the accumulation rect (verts
		 * are clip-local, so the rect is [0,w)x[0,h)) and to VRAM once the
		 * clip origin is added back at write time. */
		{
			int minPX = (int)((x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2))) / 2;
			int maxPX = (int)((x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2)) + 1) / 2;
			int minPY = (int)((y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2))) / 2;
			int maxPY = (int)((y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2)) + 1) / 2;

			OfsRastEdge e01, e12, e20;
			s32 w0Row, w1Row, w2Row;
			s32 bias0, bias1, bias2;
			int px, py;

			if (minPX < 0) { minPX = 0; }
			if (minPY < 0) { minPY = 0; }
			if (maxPX > clip->w) { maxPX = clip->w; }
			if (maxPY > clip->h) { maxPY = clip->h; }
			if (clip->x + maxPX > VRAM_WIDTH) { maxPX = VRAM_WIDTH - clip->x; }
			if (clip->y + maxPY > VRAM_HEIGHT) { maxPY = VRAM_HEIGHT - clip->y; }
			if ((minPX >= maxPX) || (minPY >= maxPY))
			{
				continue;
			}

			/* w0 pairs with v0 (edge v1->v2), w1 with v1 (v2->v0), w2 with
			 * v2 (v0->v1); w0+w1+w2 == area2 at every sample point. */
			w0Row = OfsRast_EdgeInit(&e12, x1, y1, x2, y2, minPX * 2 + 1, minPY * 2 + 1);
			w1Row = OfsRast_EdgeInit(&e20, x2, y2, x0, y0, minPX * 2 + 1, minPY * 2 + 1);
			w2Row = OfsRast_EdgeInit(&e01, x0, y0, x1, y1, minPX * 2 + 1, minPY * 2 + 1);

			bias0 = OfsRast_EdgeIsTopLeft(x1, y1, x2, y2) ? 0 : -1;
			bias1 = OfsRast_EdgeIsTopLeft(x2, y2, x0, y0) ? 0 : -1;
			bias2 = OfsRast_EdgeIsTopLeft(x0, y0, x1, y1) ? 0 : -1;

			for (py = minPY; py < maxPY; py++)
			{
				s32 w0 = w0Row;
				s32 w1 = w1Row;
				s32 w2 = w2Row;
				u16 *dstRow = &vramData[(clip->y + py) * VRAM_WIDTH + clip->x];

				for (px = minPX; px < maxPX; px++)
				{
					if (((w0 + bias0) | (w1 + bias1) | (w2 + bias2)) >= 0)
					{
						u16 dstColor = dstRow[px];

						if (maskCheck && (dstColor & 0x8000u))
						{
							goto nextPixel;
						}

						{
							/* Interpolated vertex color, rounded to 8 bits
							 * (GL interpolates exactly; one rounding here
							 * stays within +-1 of its rational value). */
							int colR8 = (int)OFSRAST_DIV_ROUND((s64)w0 * v0->r + (s64)w1 * v1->r + (s64)w2 * v2->r, (s64)area2);
							int colG8 = (int)OFSRAST_DIV_ROUND((s64)w0 * v0->g + (s64)w1 * v1->g + (s64)w2 * v2->g, (s64)area2);
							int colB8 = (int)OFSRAST_DIV_ROUND((s64)w0 * v0->b + (s64)w1 * v1->b + (s64)w2 * v2->b, (s64)area2);
							int srcR5, srcG5, srcB5;
							int blendThisPixel;
							int stpBit;

							if (textured)
							{
								/* floor(interp(u)) -- the GL NEAREST fetch. */
								int texelU = (int)(((s64)w0 * v0->u + (s64)w1 * v1->u + (s64)w2 * v2->u) / area2) & 0xFF;
								int texelV = (int)(((s64)w0 * v0->v + (s64)w1 * v1->v + (s64)w2 * v2->v) / area2) & 0xFF;
								u16 texel = N3DS_VramFetchTexel(vramData, v0->page, v0->clut, texFormat, texelU, texelV);
								int texR5, texG5, texB5;
								int r8, g8, b8;

								if (texel == 0)
								{
									/* PS1 punch-through: transparent black. */
									goto nextPixel;
								}

								texR5 = texel & 0x1F;
								texG5 = (texel >> 5) & 0x1F;
								texB5 = (texel >> 10) & 0x1F;

								/* texel * color * 2 (bright=2), in 8 bits like
								 * PC's shader/FBO, truncated to 5 at write. */
								r8 = OFSRAST_DIV_ROUND((s64)texR5 * colR8 * 2, 31);
								g8 = OFSRAST_DIV_ROUND((s64)texG5 * colG8 * 2, 31);
								b8 = OFSRAST_DIV_ROUND((s64)texB5 * colB8 * 2, 31);
								if (r8 > 255) { r8 = 255; }
								if (g8 > 255) { g8 = 255; }
								if (b8 > 255) { b8 = 255; }
								srcR5 = r8 >> 3;
								srcG5 = g8 >> 3;
								srcB5 = b8 >> 3;

								blendThisPixel = texturedSemiTrans && ((texel & 0x8000u) != 0);
								stpBit = (textureOutputSTP && ((texel & 0x8000u) != 0)) ? 1 : 0;
							}
							else
							{
								/* bright=1: vertex color straight through. */
								srcR5 = colR8 >> 3;
								srcG5 = colG8 >> 3;
								srcB5 = colB8 >> 3;
								blendThisPixel = untexturedBlend;
								stpBit = 0;
							}

							if (maskSet)
							{
								stpBit = 1;
							}

							{
								u16 outColor;

								if (blendThisPixel)
								{
									outColor = OfsRast_BlendPSX(blendMode, dstColor, srcR5, srcG5, srcB5);
								}
								else
								{
									outColor = (u16)(srcR5 | (srcG5 << 5) | (srcB5 << 10));
								}

								dstRow[px] = (u16)(outColor | (stpBit ? 0x8000u : 0u));
							}
						}
					}
				nextPixel:
					w0 += e12.stepX;
					w1 += e20.stepX;
					w2 += e01.stepX;
				}

				w0Row += e12.stepY;
				w1Row += e20.stepY;
				w2Row += e01.stepY;
			}
		}
	}
}
