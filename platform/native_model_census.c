/*
 * native_model_census.c -- one-shot accounting of a level's model/animation
 * data, printed once per level load.
 *
 * WHY THIS EXISTS
 *
 * The open campaign question is whether karts, drivers, crates and weapons can
 * be moved off the packet path and onto the PICA the way the track already was.
 * The technique is standard (upload every keyframe once, hand the shader two
 * position streams plus a blend factor and the instance matrix), and the
 * blocker is not the shader -- it is whether the keyframes FIT. The 3DS build
 * runs with roughly 1.5MB free once the track atlases are resident, and no
 * number anywhere in this project said what CTR's animation data weighs. This
 * prints that number instead of estimating it.
 *
 * WHAT MAKES THE ANSWER NON-OBVIOUS
 *
 * CTR does not store plain vertex arrays. struct ModelAnim carries an optional
 * ptrDeltaArray; when it is non-NULL the frames are a bit-packed delta stream
 * decoded serially with a temporal accumulator (see
 * RenderBucket_UncompressAnimationFrame), with a per-vertex u32 saying how many
 * bits each component costs. A GPU morph target cannot consume that -- it needs
 * random access to frame N's vertex K -- so a compressed animation would have
 * to be EXPANDED at load, and its stored size is a floor on the real cost, not
 * an estimate of it. Uncompressed animations are already GPU-shaped: three u8
 * components per vertex (RenderBucketCompVertex), which PICA can take directly
 * as a GPU_UNSIGNED_BYTE attribute, so those cost stored*4/3 padded and no
 * more.
 *
 * The split between the two therefore decides the whole plan, which is why
 * this reports them separately rather than as one total.
 *
 * Counts only -- reads model metadata, allocates nothing, changes nothing.
 */

#if defined(CTR_INTERNAL)

/*
 * Per-frame header size. struct ModelFrame is pos(6) + pad(2) + unk16(16) +
 * vertexOffset(4); retail sets vertexOffset to 0x1C on every frame it emits
 * (see namespace_Instance.h) and MODELFRAME_GETVERT trusts that, so the vertex
 * payload of an UNCOMPRESSED frame is frameSize - 0x1C bytes at three bytes per
 * vertex. If that subtraction ever fails to divide evenly the assumption is
 * wrong somewhere and the count is reported as suspect rather than silently
 * rounded.
 */
#define MODELCENSUS_FRAME_HEADER 0x1C
#define MODELCENSUS_COMP_VERTEX  3

struct NativeModelCensusTotals
{
	unsigned models;
	unsigned headers;
	unsigned anims;
	unsigned frames;

	unsigned animsCompressed;
	unsigned animsPlain;

	/* Bytes exactly as they sit in the level right now. */
	unsigned storedCompressedBytes;
	unsigned storedPlainBytes;

	/* Vertices resolvable from plain frames, and the padded-to-4 GPU cost of
	 * every plain frame that carries them. */
	unsigned plainVerts;
	unsigned gpuPlainBytes;

	/* Same, for delta-compressed animations, whose vertex counts come from
	 * walking the bit-width table rather than from a frame size. */
	unsigned compVerts;
	unsigned gpuCompBytes;

	/* Headers whose vertex count could not be recovered because every one of
	 * their animations is delta-compressed. Their expansion cost is the
	 * unknown in the estimate and is reported as such. */
	unsigned headersVertsUnknown;
	unsigned framesVertsUnknown;
	unsigned suspectFrameSizes;
};

static struct Level *s_censusLastLevel = NULL;

/*
 * Dedupe set. The level's model array and the global library are NOT disjoint
 * -- the first version of this census reported "lvl 1483 KiB + lib 1483 KiB =
 * 2966" for a level whose two populations were largely the same pointers, and
 * a doubled memory figure would have killed the retained-models plan on
 * arithmetic rather than on evidence. Every Model is counted once toward the
 * combined total no matter how many arrays list it.
 */
#define MODELCENSUS_MAX_UNIQUE 1024
static const struct Model *s_censusSeen[MODELCENSUS_MAX_UNIQUE];
static unsigned s_censusSeenCount;

static int NativeModelCensus_MarkSeen(const struct Model *model)
{
	for (unsigned i = 0; i < s_censusSeenCount; i++)
	{
		if (s_censusSeen[i] == model)
		{
			return 0;
		}
	}

	if (s_censusSeenCount < MODELCENSUS_MAX_UNIQUE)
	{
		s_censusSeen[s_censusSeenCount++] = model;
	}

	return 1;
}

/*
 * Vertex count of a DELTA-COMPRESSED animation, recovered statically.
 *
 * Every frame of one animation is the same number of bits long, because they
 * all consume the same per-vertex bit-width table (ptrDeltaArray, one u32 per
 * vertex; see RenderBucket_UncompressAnimationFrame for the field layout).
 * RenderBucket_ReadDeltaComponentFromStream reads bits+1 bits per component,
 * so vertex K costs (xBits+1)+(zBits+1)+(yBits+1) bits, and walking the table
 * until the running total fills frameSize*8 counts the vertices exactly.
 *
 * This matters because the whole retained-models question turns on the
 * expansion ratio, and every animation in the game turned out to be
 * compressed -- the first run of this census found plain anims = 0 across ten
 * levels, so there is no uncompressed animation anywhere to read a vertex
 * count off. Without this the estimate stays a 3x-wide bracket.
 *
 * Frames are byte-padded, so the loop can read at most one u32 past the last
 * real entry when the padding happens to fit a minimum-width vertex. That is
 * an accepted overread in a CTR_INTERNAL-only counter, not a live path.
 */
static unsigned NativeModelCensus_CompressedVerts(const struct ModelAnim *anim)
{
	const u32 *delta = anim->ptrDeltaArray;
	unsigned totalBits = (unsigned)anim->frameSize * 8u;
	unsigned accum = 0;
	unsigned verts = 0;

	if (delta == NULL)
	{
		return 0;
	}

	while ((accum + 3u) <= totalBits)
	{
		u32 temporal = delta[verts];
		unsigned cost = (unsigned)(((temporal >> 6) & 7u) + ((temporal >> 3) & 7u) + (temporal & 7u) + 3u);

		if ((accum + cost) > totalBits)
		{
			break;
		}

		accum += cost;
		verts++;

		/* A model with thousands of vertices is corrupt data, not content;
		 * bail rather than walk an arbitrary distance through the heap. */
		if (verts >= 8192u)
		{
			return 0;
		}
	}

	return verts;
}

/*
 * All animations of one ModelHeader share a vertex count -- the header's
 * command list indexes into whatever the current frame supplies -- so a single
 * uncompressed animation is enough to learn it for the header's compressed
 * ones too. Returns 0 when nothing in this header gives it away.
 */
static unsigned NativeModelCensus_HeaderVerts(const struct ModelHeader *mh, unsigned *suspect)
{
	if (mh->ptrFrameData != NULL)
	{
		/* Static model: one frame, no ModelAnim to read a size from. Its size
		 * is not recorded anywhere reachable, so it contributes structure but
		 * not bytes. */
		return 0;
	}

	if ((mh->ptrAnimations == NULL) || (mh->numAnimations == 0) || (mh->numAnimations > 4096))
	{
		return 0;
	}

	for (u32 a = 0; a < mh->numAnimations; a++)
	{
		const struct ModelAnim *anim = mh->ptrAnimations[a];

		if ((anim == NULL) || (anim->ptrDeltaArray != NULL))
		{
			continue;
		}

		int payload = (int)anim->frameSize - MODELCENSUS_FRAME_HEADER;

		if (payload <= 0)
		{
			continue;
		}

		if ((payload % MODELCENSUS_COMP_VERTEX) != 0)
		{
			(*suspect)++;
			continue;
		}

		return (unsigned)(payload / MODELCENSUS_COMP_VERTEX);
	}

	return 0;
}

static void NativeModelCensus_Header(const struct ModelHeader *mh, struct NativeModelCensusTotals *t)
{
	unsigned verts = NativeModelCensus_HeaderVerts(mh, &t->suspectFrameSizes);

	t->headers++;

	if ((mh->ptrAnimations == NULL) || (mh->numAnimations == 0) || (mh->numAnimations > 4096))
	{
		return;
	}

	for (u32 a = 0; a < mh->numAnimations; a++)
	{
		const struct ModelAnim *anim = mh->ptrAnimations[a];
		unsigned bytes;

		if (anim == NULL)
		{
			continue;
		}

		/* Bound the arithmetic, not the data: a wild numFrames/frameSize pair
		 * from a half-loaded level should skew nothing rather than overflow. */
		/* frameSize is s16, so <= 0 already bounds it above; numFrames is u16
		 * and a level that reports thousands of frames for one animation is
		 * half-loaded rather than large. */
		if ((anim->numFrames == 0) || (anim->frameSize <= 0) || (anim->numFrames > 4096))
		{
			continue;
		}

		bytes = (unsigned)anim->numFrames * (unsigned)anim->frameSize;

		t->anims++;
		t->frames += anim->numFrames;

		if (anim->ptrDeltaArray != NULL)
		{
			unsigned animVerts = NativeModelCensus_CompressedVerts(anim);

			t->animsCompressed++;
			t->storedCompressedBytes += bytes;

			if (animVerts == 0)
			{
				animVerts = verts;
			}

			if (animVerts == 0)
			{
				t->framesVertsUnknown += anim->numFrames;
			}
			else
			{
				t->compVerts += animVerts;
				t->gpuCompBytes += (unsigned)anim->numFrames * animVerts * 4u;
			}
		}
		else
		{
			t->animsPlain++;
			t->storedPlainBytes += bytes;
			t->plainVerts += verts;
			/* Padded to 4: PICA attribute strides are element-aligned, so the
			 * three-byte position costs a fourth byte on the GPU side. */
			t->gpuPlainBytes += (unsigned)anim->numFrames * verts * 4u;
		}
	}

	if (verts == 0)
	{
		t->headersVertsUnknown++;
	}
}

static void NativeModelCensus_Model(const struct Model *model, struct NativeModelCensusTotals *t)
{
	if ((model == NULL) || (model->headers == NULL) || (model->numHeaders <= 0) || (model->numHeaders > 64))
	{
		return;
	}

	t->models++;

	for (s16 h = 0; h < model->numHeaders; h++)
	{
		NativeModelCensus_Header(&model->headers[h], t);
	}
}

static void NativeModelCensus_Print(const char *tag, const struct NativeModelCensusTotals *t)
{
	unsigned storedKiB = (t->storedPlainBytes + t->storedCompressedBytes) / 1024u;
	unsigned gpuKiB = (t->gpuPlainBytes + t->gpuCompBytes) / 1024u;

	Platform_Log("[MODELCENSUS] %s models=%u headers=%u anims=%u(plain %u) frames=%u verts=%u\n", tag, t->models, t->headers, t->anims, t->animsPlain,
	             t->frames, t->plainVerts + t->compVerts);
	Platform_Log("[MODELCENSUS] %s storedKiB=%u gpuKiB=%u ratio=%u%%  unresolved: headers=%u frames=%u suspect=%u\n", tag, storedKiB, gpuKiB,
	             (storedKiB > 0) ? ((gpuKiB * 100u) / storedKiB) : 0u, t->headersVertsUnknown, t->framesVertsUnknown, t->suspectFrameSizes);
}

void NativeModelCensus_Report(struct GameTracker *gGT, struct Level *level)
{
	struct NativeModelCensusTotals lvlTotals;
	struct NativeModelCensusTotals libTotals;

	if ((gGT == NULL) || (level == NULL) || (level == s_censusLastLevel))
	{
		return;
	}
	s_censusLastLevel = level;

	memset(&lvlTotals, 0, sizeof(lvlTotals));
	memset(&libTotals, 0, sizeof(libTotals));
	s_censusSeenCount = 0;

	/*
	 * TWO populations, and conflating them was the first version's mistake.
	 * level->ptrModelsPtrArray is the track's own props -- crates, boxes,
	 * scenery. Karts, drivers and weapons are not in it; they live in the
	 * global library sdata->modelPtr[], registered by LibraryOfModels_Store
	 * and indexed by the STATIC_* enum. A census of the level array alone
	 * therefore answers a question nobody asked, because the models the
	 * retained-geometry campaign is actually about are all in the other one.
	 */
	if ((level->ptrModelsPtrArray != NULL) && (level->numModels > 0) && (level->numModels <= 8192))
	{
		for (u32 m = 0; m < level->numModels; m++)
		{
			const struct Model *model = level->ptrModelsPtrArray[m];

			if ((model != NULL) && NativeModelCensus_MarkSeen(model))
			{
				NativeModelCensus_Model(model, &lvlTotals);
			}
		}
	}

	/* "lib+" is what the library adds ON TOP of the level, not the library
	 * itself, so the two lines below can be added without double-counting. */
	for (u32 m = 0; m < (u32)(sizeof(gGT->modelPtr) / sizeof(gGT->modelPtr[0])); m++)
	{
		const struct Model *model = gGT->modelPtr[m];

		if ((model != NULL) && NativeModelCensus_MarkSeen(model))
		{
			NativeModelCensus_Model(model, &libTotals);
		}
	}

	NativeModelCensus_Print("lvl ", &lvlTotals);
	NativeModelCensus_Print("lib+", &libTotals);
	Platform_Log("[MODELCENSUS] TOTAL uniqueModels=%u gpuKiB=%u (lvl %u + lib+ %u)\n", s_censusSeenCount,
	             (lvlTotals.gpuPlainBytes + lvlTotals.gpuCompBytes + libTotals.gpuPlainBytes + libTotals.gpuCompBytes) / 1024u,
	             (lvlTotals.gpuPlainBytes + lvlTotals.gpuCompBytes) / 1024u, (libTotals.gpuPlainBytes + libTotals.gpuCompBytes) / 1024u);
}

#else

void NativeModelCensus_Report(struct GameTracker *gGT, struct Level *level)
{
	(void)gGT;
	(void)level;
}

#endif
