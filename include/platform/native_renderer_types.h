/*
 * Derived from REDRIVER2/PsyCross MIT source:
 * externals/PsyCross/include/PsyX/PsyX_render.h
 * See THIRD_PARTY_NOTICES.md for copyright and license details.
 */

#ifndef NATIVE_RENDERER_TYPES_H
#define NATIVE_RENDERER_TYPES_H

#include <macros.h>
#include <psx/libgte.h>
#include <psx/libgpu.h>

#define LUT_WIDTH              (256)
#define LUT_HEIGHT             (256)

#define VRAM_WIDTH             (1024)
#define VRAM_HEIGHT            (512)

#define TPAGE_WIDTH            (256)
#define TPAGE_HEIGHT           (256)

#define MAX_VERTEX_BUFFER_SIZE (1u << 16)

#pragma pack(push, 1)
typedef struct
{
	s16 x, y, page, clut;

	u8 u, v, bright, dither;
	u8 r, g, b, a;

	s8 tcx, tcy, _p0, _p1;

	/*
	 * Per-primitive emission ordinal, stamped in OT-walk order by the parse
	 * layer (native_gpu.c). All vertices of one primitive share a value;
	 * later primitives get larger ones. This is the depth substrate for the
	 * 3DS z-break play: submission order can be changed without changing
	 * layering, because depth = ordinal reproduces painter's order in the
	 * depth buffer. Appended field: PC's GL attribute loaders address fields
	 * by offset and never fetch it, so PC output is unaffected.
	 *
	 * OCCLUSION channel. Since the split below, this one and only this one
	 * decides what covers what.
	 */
	u16 emitOrdinal;

	/*
	 * PARALLAX channel (docs/FLAG_SPLIT_CHANNEL_SPEC.md). Owner's framing:
	 * "game world depth and 3d output depth don't need to be the same."
	 *
	 * Normally identical to emitOrdinal -- the depth a thing occludes at IS
	 * the depth it should appear at in stereo. The menu/loading checkered
	 * flag is the exception this exists for: retail's own layering wants it
	 * painted over everything mid-wipe and behind the menu models when
	 * parked (a per-pose OCCLUSION answer), while the stereo eye wants its
	 * wave's true relief continuously, in every pose, with no jump (one
	 * fixed PARALLAX band just behind the convergence plane). One depth
	 * field cannot say both; two can.
	 *
	 * Same units and axis as emitOrdinal (nearer = larger), so 3DS biases
	 * both into s16 identically and the vertex shader's d = 32767 - value
	 * inverse works unchanged on it. Appended for the same reason and with
	 * the same PC guarantee as emitOrdinal above: no GL loader fetches it.
	 */
	u16 parallaxOrdinal;
} GrVertex;
#pragma pack(pop)

typedef enum
{
	a_position,
	a_texcoord,
	a_color,
	a_extra,
} ShaderAttrib;

typedef enum
{
	BM_NONE,
	BM_AVERAGE,
	BM_ADD,
	BM_SUBTRACT,
	BM_ADD_QUATER_SOURCE
} BlendMode;

typedef enum
{
	TF_4_BIT,
	TF_8_BIT,
	TF_16_BIT,

	TF_32_BIT_RGBA
} TexFormat;

typedef u32 TextureID;
typedef u32 ShaderID;

#endif
