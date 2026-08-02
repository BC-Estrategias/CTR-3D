/*
 * native_model_mesh.c -- per-ModelHeader topology scan (s30 rung 0).
 *
 * WHY THIS EXISTS
 *
 * s30's direction (docs/HANDOFF_s30_pica_native.md) is to stop converting
 * instanced models into PS1 packets and hand the PICA real geometry instead --
 * the treatment the track already got. Instances were measured at ~9ms in an
 * Arcade frame on hardware, and `draw_all_splits` + `draw_otag_parse` together
 * are ~28ms of a 55ms frame with none of it track.
 *
 * Nothing here draws. This walks each model's command list once and reports
 * what the eventual draw side would have to move: vertices per header,
 * triangles per LOD rung, textures per header, and static-versus-animated.
 * The project's own history is the argument for measuring first -- three of the
 * last four confident performance conclusions came from content that did not
 * exercise the thing being measured.
 *
 * WHY A SCAN IS ENOUGH
 *
 * A model's command list is frame-invariant. RenderBucket_DrawFunc_Normal
 * (game/RenderBucket/RenderBucket_QueueExecute.c:4350) walks the same stream in
 * the same order on every frame of every animation: the same strip restarts,
 * the same vertex order, the same texture per primitive. What changes per frame
 * is only the vertex POSITIONS, which are delta-decoded serially against a
 * temporal accumulator. So topology is a load-time property, and this is the
 * load-time half of the model renderer.
 *
 * WHAT THE COUNTS ARE AND ARE NOT
 *
 * `tris` is a topology maximum. The retail walk additionally drops primitives
 * that fail projection (backfacing, off-screen, degenerate) or texture lookup,
 * and both are camera-dependent -- a static scan cannot know them. The per-
 * frame sampler is what turns this into "triangles actually queued"; treating
 * this number as a frame cost would repeat the exact mistake this file exists
 * to avoid.
 *
 * Counts only -- reads model metadata, allocates nothing, changes nothing.
 */

#include "platform/native_model_mesh.h"

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_MODEL_RENDERER) && CTR_NATIVE_MODEL_RENDERER

/*
 * A level's whole model population is a few hundred headers (the s29 census
 * reported models in the low hundreds per level across both the level array and
 * the global library), so a fixed table is right and growth is not worth the
 * allocator. Overflow is reported, never silent -- a truncated census that
 * looks complete is how "the retained-models campaign is dead" got said out
 * loud once already.
 */
#define MODELMESH_MAX_HEADERS 2048

/*
 * Walk cap. The terminator is 0xffffffff; a header whose list is malformed (or
 * whose pointer is stale mid-load) must bound the walk rather than run off into
 * the heap. Real command lists are hundreds of words -- a header that needs
 * more than this is corrupt data, not content, and says so via WALK_CAPPED.
 */
#define MODELMESH_MAX_COMMANDS 65536u

/*
 * Two texture counts, because the first version conflated them and produced a
 * number that read as a draw-call floor and was nothing of the kind.
 *
 * A command's low nine bits index mh->ptrTexLayout, and a struct TextureLayout
 * holds four UV corners as well as tpage and clut -- model primitives take
 * their UVs from that entry rather than from vertex attributes, so one entry is
 * a FACE's UV record. An attract run measured 579 distinct entries across nine
 * instances; a 32-then-128 cap pinned at both values in turn, which is what
 * exposed the misreading. The index is nine bits, so tracking it in a full
 * 512-bit set removes the cap and the question together.
 *
 * The pair that actually costs a state change is (tpage, clut) -- the same
 * batching key the track atlas packs by. A handful per header is expected, so
 * a small table with an honest overflow flag is right for that one.
 */
#define MODELMESH_TEXINDEX_BITS  512
#define MODELMESH_TEXINDEX_WORDS (MODELMESH_TEXINDEX_BITS / 32)
/* 32 was not enough either -- an intro level pinned maxPages at exactly 32 with
 * texOver=1, the same tell as the two texture caps before it. 128 costs half a
 * kilobyte of stack in a load-time function and leaves the flag in place to
 * catch a third surprise. */
#define MODELMESH_MAX_TEXPAGES   128

static struct NativeModelHeaderScan s_scans[MODELMESH_MAX_HEADERS];
static unsigned s_scanCount;
static int s_scanOverflow;

void NativeModelMesh_Reset(void)
{
	s_scanCount = 0;
	s_scanOverflow = 0;
	memset(s_scans, 0, sizeof(s_scans));
}

unsigned NativeModelMesh_ScannedCount(void)
{
	return s_scanCount;
}

int NativeModelMesh_Overflowed(void)
{
	return s_scanOverflow;
}

/*
 * Open-addressed by pointer identity. ModelHeaders are 0x40 bytes and usually
 * consecutive within one Model, so the low bits alone would collide on every
 * sibling LOD rung -- shifting past the struct size spreads them.
 */
static unsigned NativeModelMesh_Slot(const struct ModelHeader *mh)
{
	unsigned key = (unsigned)(u32)mh;

	key >>= 4;
	key ^= key >> 11;
	return key & (MODELMESH_MAX_HEADERS - 1);
}

CTR_STATIC_ASSERT((MODELMESH_MAX_HEADERS & (MODELMESH_MAX_HEADERS - 1)) == 0);

static struct NativeModelHeaderScan *NativeModelMesh_Find(const struct ModelHeader *mh, int *foundOut)
{
	unsigned slot = NativeModelMesh_Slot(mh);
	unsigned probe;

	*foundOut = 0;

	for (probe = 0; probe < MODELMESH_MAX_HEADERS; probe++)
	{
		struct NativeModelHeaderScan *entry = &s_scans[slot];

		if (entry->mh == mh)
		{
			*foundOut = 1;
			return entry;
		}

		if (entry->mh == NULL)
		{
			return entry;
		}

		slot = (slot + 1) & (MODELMESH_MAX_HEADERS - 1);
	}

	return NULL;
}

/*
 * The strip walk, kept deliberately parallel to RenderBucket_DrawFunc_Normal
 * (:4356-4457) so a reader can diff the two:
 *
 *   - word 0 of the command list is the colour-table count, so commands start
 *     at word 1 (retail's `pCmd++` before the loop);
 *   - `command >> 16 == 0` is a colour-only command -- no vertex, no primitive;
 *   - bit 0x04000000 (flags & 4) reuses a cached vertex instead of consuming
 *     the next one from the stream, so the vertex counter advances only when it
 *     is CLEAR;
 *   - bit 0x80000000 (flags & 0x80) restarts the strip;
 *   - a strip emits its first triangle on its third command, one per command
 *     thereafter.
 *
 * Texture index is the low 9 bits (RenderBucket_GetCommandTexture:2777); index
 * 0 means untextured (retail's explicit G3 path) and is not counted as a
 * texture. Primitives are always triangles -- every DrawInstPrim_* writer emits
 * a 3-vertex POLY_*3.
 */
static void NativeModelMesh_Walk(const struct ModelHeader *mh, struct NativeModelHeaderScan *out)
{
	const u32 *cmd = (const u32 *)mh->ptrCommandList;
	u32 texIndexSeen[MODELMESH_TEXINDEX_WORDS];
	u32 texPageSeen[MODELMESH_MAX_TEXPAGES];
	unsigned texIndexCount = 0;
	unsigned texPageCount = 0;
	unsigned stripLength = 0;
	unsigned steps;

	memset(texIndexSeen, 0, sizeof(texIndexSeen));

	if (cmd == NULL)
	{
		out->flags |= NATIVE_MODEL_SCAN_NO_COMMANDS;
		return;
	}

	cmd++;

	for (steps = 0; steps < MODELMESH_MAX_COMMANDS; steps++)
	{
		u32 command = *cmd++;
		u32 flags;

		if (command == 0xffffffffu)
		{
			return;
		}

		if ((command >> 16) == 0)
		{
			out->colorOnly++;
			continue;
		}

		flags = (command >> 24) & 0xff;

		if ((flags & 4) == 0)
		{
			out->verts++;
		}

		if ((flags & 0x80) != 0)
		{
			out->strips++;
			stripLength = 0;
		}

		if (stripLength < 2)
		{
			stripLength++;
			continue;
		}

		stripLength++;
		out->tris++;

		{
			u16 texIndex = (u16)(command & 0x1ff);
			const struct TextureLayout *layout;
			u32 pageKey;
			unsigned i;

			/* Index 0 is retail's explicit untextured path -- a real
			 * primitive, but not a texture reference. */
			if (texIndex == 0)
			{
				continue;
			}

			if ((texIndexSeen[texIndex >> 5] & (1u << (texIndex & 31))) != 0)
			{
				continue;
			}

			texIndexSeen[texIndex >> 5] |= 1u << (texIndex & 31);
			texIndexCount++;
			out->texLayouts = (u16)texIndexCount;

			if (mh->ptrTexLayout == NULL)
			{
				out->flags |= NATIVE_MODEL_SCAN_NO_TEXTABLE;
				continue;
			}

			/* Same dereference RenderBucket_GetCommandTexture does (:2795),
			 * including its note that a null table ENTRY is legal and reaches
			 * the untextured writer -- so a null here is skipped, not
			 * reported as a broken model. */
			layout = mh->ptrTexLayout[texIndex - 1];

			if (layout == NULL)
			{
				continue;
			}

			pageKey = ((u32)layout->tpage << 16) | (u32)layout->clut;

			for (i = 0; i < texPageCount; i++)
			{
				if (texPageSeen[i] == pageKey)
				{
					break;
				}
			}

			if (i < texPageCount)
			{
				continue;
			}

			if (texPageCount < MODELMESH_MAX_TEXPAGES)
			{
				texPageSeen[texPageCount++] = pageKey;
				out->texPages = (u16)texPageCount;
			}
			else
			{
				out->flags |= NATIVE_MODEL_SCAN_TEX_OVERFLOW;
			}
		}
	}

	out->flags |= NATIVE_MODEL_SCAN_WALK_CAPPED;
}

const struct NativeModelHeaderScan *NativeModelMesh_Scan(const struct ModelHeader *mh)
{
	struct NativeModelHeaderScan *entry;
	int found;

	if (mh == NULL)
	{
		return NULL;
	}

	entry = NativeModelMesh_Find(mh, &found);

	if (found)
	{
		return entry;
	}

	if ((entry == NULL) || (s_scanCount >= MODELMESH_MAX_HEADERS))
	{
		s_scanOverflow = 1;
		return NULL;
	}

	memset(entry, 0, sizeof(*entry));
	entry->mh = mh;
	s_scanCount++;

	/*
	 * Classification, taken from the reader that actually decides at draw time
	 * (RenderBucket_GetFrame:1947): ptrAnimations == 0 means the fixed
	 * ptrFrameData frame. ptrFrameData's own comment claims it is "null if
	 * there are animations", so the two should be exact complements -- if they
	 * ever aren't, the split reported off them is not trustworthy and says so
	 * rather than being quietly resolved one way.
	 */
	if ((mh->ptrAnimations == NULL) || (mh->numAnimations == 0))
	{
		entry->flags |= NATIVE_MODEL_SCAN_STATIC;

		if (mh->ptrFrameData == NULL)
		{
			entry->flags |= NATIVE_MODEL_SCAN_FRAME_MISMATCH;
		}
	}
	else
	{
		entry->flags |= NATIVE_MODEL_SCAN_ANIMATED;

		if (mh->ptrFrameData != NULL)
		{
			entry->flags |= NATIVE_MODEL_SCAN_FRAME_MISMATCH;
		}
	}

	NativeModelMesh_Walk(mh, entry);
	return entry;
}

#else

void NativeModelMesh_Reset(void)
{
}

const struct NativeModelHeaderScan *NativeModelMesh_Scan(const struct ModelHeader *mh)
{
	(void)mh;
	return NULL;
}

unsigned NativeModelMesh_ScannedCount(void)
{
	return 0;
}

int NativeModelMesh_Overflowed(void)
{
	return 0;
}

#endif
