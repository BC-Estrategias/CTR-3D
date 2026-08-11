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

/* Included unconditionally: the accessors are stubs when
 * CTR_NATIVE_MODEL_RENDERER is off, so the topology half of this census
 * degrades to zeros rather than needing its own #ifdef at every use. */
#include "platform/native_model_mesh.h"

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

/*
 * LOD rungs reported separately. RenderBucket_SelectModelHeader (game/RenderBucket/
 * RenderBucket_QueueExecute.c:1264) walks model->headers[] in order and takes the first whose
 * maxDistanceLOD exceeds the projected distance, so header index IS the rung. Anything past
 * MODELCENSUS_MAX_RUNGS is bucketed rather than dropped.
 */
#define MODELCENSUS_MAX_RUNGS 4

/* Named scenery gated out of higher player counts, listed rather than just
 * counted -- the whole point of the gating report is the list. Capped so a
 * pathological level cannot flood the SD log on 3DS. */
#define MODELCENSUS_MAX_GATED_NAMES 24

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

	/*
	 * Topology fields, from platform/native_model_mesh.c. Separate from the fields above: those
	 * are about whether animation data fits in memory, these are about how much geometry a frame
	 * draws. Counted in the same walk only because it already has the deduped model population.
	 */
	unsigned meshStatic;
	unsigned meshAnimated;
	unsigned meshMismatch;
	unsigned meshNoCommands;
	unsigned meshCapped;
	unsigned meshTexOverflow;
	unsigned meshNoTexTable;
	unsigned meshMaxLayouts;
	unsigned meshMaxPages;
	unsigned rungHeaders[MODELCENSUS_MAX_RUNGS];
	unsigned rungVerts[MODELCENSUS_MAX_RUNGS];
	unsigned rungTris[MODELCENSUS_MAX_RUNGS];
	unsigned rungBeyond;
};

static struct Level *s_censusLastLevel = NULL;

/*
 * Content fingerprint, latched alongside the pointer. Two bugs made this
 * necessary and neither is hypothetical -- both were caught in attract runs.
 *
 * 1. The Level allocation is REUSED across loads, so a pointer-only latch
 *    reported the first level of a session and then went quiet for every level
 *    after it. A 5.5-minute run produced exactly one census block while
 *    [MODELFRAME] plainly showed the game moving between levels. For the byte
 *    census that only cost coverage; for the topology table it is a
 *    correctness problem, because NativeModelMesh_Reset runs off this same
 *    latch and a missed level change leaves ModelHeader pointers from the
 *    PREVIOUS level cached for any address the allocator reuses.
 *
 * 2. Adding the level ID to the latch fixed that and introduced a worse
 *    failure: the ID changes a frame BEFORE the Level struct is repopulated,
 *    so every level was reported twice -- once with the previous level's data
 *    under the new level's ID. Mislabelled data is worse than missing data,
 *    and this project has lost whole sessions to exactly that shape of error.
 *
 * So the latch is the level's own CONTENT, the same answer native_track_mesh.c
 * reached for the same reason (pointer + numQuadBlock/numVertex fingerprint).
 * The ID is read at report time and only tags the output;
 * it never decides whether to report, so it cannot lead the data it labels.
 */
struct NativeModelCensusFingerprint
{
	const void *instDefs;
	const void *modelArray;
	u32 numInstances;
	u32 numModels;
};

static struct NativeModelCensusFingerprint s_censusLastPrint;

static int NativeModelCensus_SameFingerprint(const struct NativeModelCensusFingerprint *a, const struct NativeModelCensusFingerprint *b)
{
	return (a->instDefs == b->instDefs) && (a->modelArray == b->modelArray) && (a->numInstances == b->numInstances) && (a->numModels == b->numModels);
}

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

/*
 * Topology half. Folds one header's command-list scan into the
 * per-rung totals. `rung` is the header's index within its Model, which is
 * exactly the lodIndex RenderBucket_SelectModelHeader would assign it.
 */
static void NativeModelCensus_HeaderMesh(const struct ModelHeader *mh, s16 rung, struct NativeModelCensusTotals *t)
{
	const struct NativeModelHeaderScan *scan = NativeModelMesh_Scan(mh);

	if (scan == NULL)
	{
		return;
	}

	if ((scan->flags & NATIVE_MODEL_SCAN_STATIC) != 0)
	{
		t->meshStatic++;
	}
	else
	{
		t->meshAnimated++;
	}

	if ((scan->flags & NATIVE_MODEL_SCAN_FRAME_MISMATCH) != 0)
	{
		t->meshMismatch++;
	}

	if ((scan->flags & NATIVE_MODEL_SCAN_NO_COMMANDS) != 0)
	{
		t->meshNoCommands++;
	}

	if ((scan->flags & NATIVE_MODEL_SCAN_WALK_CAPPED) != 0)
	{
		t->meshCapped++;
	}

	if ((scan->flags & NATIVE_MODEL_SCAN_TEX_OVERFLOW) != 0)
	{
		t->meshTexOverflow++;
	}

	if ((scan->flags & NATIVE_MODEL_SCAN_NO_TEXTABLE) != 0)
	{
		t->meshNoTexTable++;
	}

	if ((unsigned)scan->texLayouts > t->meshMaxLayouts)
	{
		t->meshMaxLayouts = scan->texLayouts;
	}

	if ((unsigned)scan->texPages > t->meshMaxPages)
	{
		t->meshMaxPages = scan->texPages;
	}

	if ((rung < 0) || (rung >= MODELCENSUS_MAX_RUNGS))
	{
		t->rungBeyond++;
		return;
	}

	t->rungHeaders[rung]++;
	t->rungVerts[rung] += scan->verts;
	t->rungTris[rung] += scan->tris;
}

static void NativeModelCensus_Header(const struct ModelHeader *mh, s16 rung, struct NativeModelCensusTotals *t)
{
	unsigned verts = NativeModelCensus_HeaderVerts(mh, &t->suspectFrameSizes);

	NativeModelCensus_HeaderMesh(mh, rung, t);

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
		NativeModelCensus_Header(&model->headers[h], h, t);
	}
}

static void NativeModelCensus_Print(const char *tag, int levelID, const struct NativeModelCensusTotals *t)
{
	unsigned storedKiB = (t->storedPlainBytes + t->storedCompressedBytes) / 1024u;
	unsigned gpuKiB = (t->gpuPlainBytes + t->gpuCompBytes) / 1024u;

	/* lvl= on every line for the same reason [TRKMESH] gained it: these
	 * blocks now print once per level rather than once per session, and an
	 * untagged number in a rotation log is a number nobody can attribute. */
	Platform_Log("[MODELCENSUS] lvl=%d %s models=%u headers=%u anims=%u(plain %u) frames=%u verts=%u\n", levelID, tag, t->models, t->headers, t->anims,
	             t->animsPlain, t->frames, t->plainVerts + t->compVerts);
	Platform_Log("[MODELCENSUS] lvl=%d %s storedKiB=%u gpuKiB=%u ratio=%u%%  unresolved: headers=%u frames=%u suspect=%u\n", levelID, tag, storedKiB, gpuKiB,
	             (storedKiB > 0) ? ((gpuKiB * 100u) / storedKiB) : 0u, t->headersVertsUnknown, t->framesVertsUnknown, t->suspectFrameSizes);
}

/*
 * Topology report, split out from the byte report above because the two answer different
 * questions. Read the rung columns as the price list for "racers use their multiplayer models":
 * rung 0 is what a close instance draws today, rung 1 what it would draw one step down. The
 * per-frame [MODELFRAME] line says how many instances actually sit on each rung -- these totals
 * are the whole population, drawn or not.
 */
static void NativeModelCensus_PrintMesh(const char *tag, int levelID, const struct NativeModelCensusTotals *t)
{
	Platform_Log("[MODELMESH] lvl=%d %s static=%u anim=%u maxLayouts=%u maxPages=%u  suspect: mismatch=%u noCmd=%u capped=%u texOver=%u noTexTable=%u "
	             "beyondRung%u=%u%s\n",
	             levelID, tag, t->meshStatic, t->meshAnimated, t->meshMaxLayouts, t->meshMaxPages, t->meshMismatch, t->meshNoCommands, t->meshCapped,
	             t->meshTexOverflow, t->meshNoTexTable, MODELCENSUS_MAX_RUNGS, t->rungBeyond, NativeModelMesh_Overflowed() ? " TABLE-FULL(partial)" : "");

	for (unsigned r = 0; r < MODELCENSUS_MAX_RUNGS; r++)
	{
		if (t->rungHeaders[r] == 0)
		{
			continue;
		}

		Platform_Log("[MODELMESH] lvl=%d %s rung%u headers=%u verts=%u tris=%u\n", levelID, tag, r, t->rungHeaders[r], t->rungVerts[r], t->rungTris[r]);
	}
}

/*
 * Player-count gating. Bits 0-3 of Instance.flags are a per-instance visibility set indexed by
 * player count: RenderBucket_QueueDraw tests `inst->flags & lodMask` (RenderBucket_QueueExecute.c:2053),
 * lodMask = sdata->LOD[] = {1,2,4,4,8,8,8,8} indexed by player count, or (players | 4) for Time Trial
 * and Relic Race (regionsEXE.h:3209):
 *
 *   bit 0 = drawn in 1P    bit 1 = drawn in 2P
 *   bit 2 = drawn in 3P/4P bit 3 = drawn in Time Trial / Relic Race
 *
 * Read from InstDef rather than the live Instance: INSTANCE_LevInitAll copies InstDef bytes [0,0x20)
 * plus three more words to Instance+8, landing InstDef.flags (0x20) on Instance.flags (0x28), so these
 * ARE the flags QueueDraw tests -- and InstDef is walkable at level load without depending on
 * instance-pool allocation having succeeded.
 */
static void NativeModelCensus_ReportGating(struct Level *level, int levelID)
{
	unsigned total = 0;
	unsigned drawn1P = 0;
	unsigned drawn2P = 0;
	unsigned drawn4P = 0;
	unsigned drawnTT = 0;
	unsigned droppedAt4P = 0;
	unsigned droppedAt4PTris = 0;
	unsigned named = 0;

	if ((level->ptrInstDefs == NULL) || (level->numInstances == 0) || (level->numInstances > 8192))
	{
		return;
	}

	for (u32 i = 0; i < level->numInstances; i++)
	{
		const struct InstDef *def = &level->ptrInstDefs[i];
		u32 flags = def->flags;

		total++;

		if ((flags & 1) != 0)
		{
			drawn1P++;
		}
		if ((flags & 2) != 0)
		{
			drawn2P++;
		}
		if ((flags & 4) != 0)
		{
			drawn4P++;
		}
		if ((flags & 8) != 0)
		{
			drawnTT++;
		}

		/* Drawn in 1P, dropped at 3P/4P -- the population a 1P build could
		 * shed by adopting the 4P mask.
		 *
		 * NOT "1P-only", which is what this was first called and what the
		 * first run's output claimed: measured flags are 0x0b on Roo's Tubes
		 * mushrooms (1P + 2P + Time Trial, absent only at 3P/4P) and 0x09 on
		 * the CTR letter tokens. Naming the set after a property it does not
		 * have is the same class of mistake as tagging a report with the next
		 * level's ID. */
		if (((flags & 1) == 0) || ((flags & 4) != 0))
		{
			continue;
		}

		droppedAt4P++;

		/* Rung 0 is the right rung to price it at: this instance is being
		 * counted precisely because it is drawn, and a drawn instance close
		 * enough to matter is on rung 0. Models with no headers contribute
		 * nothing rather than being skipped from the count. */
		if ((def->model != NULL) && (def->model->headers != NULL) && (def->model->numHeaders > 0))
		{
			const struct NativeModelHeaderScan *scan = NativeModelMesh_Scan(&def->model->headers[0]);

			if (scan != NULL)
			{
				droppedAt4PTris += scan->tris;
			}
		}

		if (named < MODELCENSUS_MAX_GATED_NAMES)
		{
			named++;
			/* modelID as well as the name, because the names in LEV InstDefs
			 * are terse (a first run printed single letters) and an ID maps
			 * straight onto the STATIC_* enum in namespace_Instance.h, which
			 * is how anyone would actually identify the object. */
			Platform_Log("[MODELGATE] lvl=%d   no4P: '%.16s' model='%.16s' id=%d flags=%08x\n", levelID, def->name,
			             (def->model != NULL) ? def->model->name : "<none>", (def->model != NULL) ? (int)def->model->id : -1, (unsigned)flags);
		}
	}

	Platform_Log("[MODELGATE] lvl=%d instances=%u drawn: 1P=%u 2P=%u 4P=%u TT=%u | droppedAt4P=%u (tris=%u, listed %u)\n", levelID, total, drawn1P, drawn2P,
	             drawn4P, drawnTT, droppedAt4P, droppedAt4PTris, named);
}

void NativeModelCensus_Report(struct GameTracker *gGT, struct Level *level)
{
	struct NativeModelCensusTotals lvlTotals;
	struct NativeModelCensusTotals libTotals;

	int levelID;
	struct NativeModelCensusFingerprint print;

	if ((gGT == NULL) || (level == NULL))
	{
		return;
	}

	print.instDefs = (const void *)level->ptrInstDefs;
	print.modelArray = (const void *)level->ptrModelsPtrArray;
	print.numInstances = level->numInstances;
	print.numModels = level->numModels;

	if ((level == s_censusLastLevel) && NativeModelCensus_SameFingerprint(&print, &s_censusLastPrint))
	{
		return;
	}

	s_censusLastLevel = level;
	s_censusLastPrint = print;
	levelID = NativePerf_CurrentLevelID();

	memset(&lvlTotals, 0, sizeof(lvlTotals));
	memset(&libTotals, 0, sizeof(libTotals));
	s_censusSeenCount = 0;

	/* ModelHeader pointers are into level/library allocations and do not
	 * survive a load, so the topology cache is per-level by construction. This
	 * is also what makes the per-frame sampler's pointer lookups safe: nothing
	 * in the table can outlive the level it was scanned from. */
	NativeModelMesh_Reset();

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

	NativeModelCensus_Print("lvl ", levelID, &lvlTotals);
	NativeModelCensus_Print("lib+", levelID, &libTotals);
	NativeModelCensus_PrintMesh("lvl ", levelID, &lvlTotals);
	NativeModelCensus_PrintMesh("lib+", levelID, &libTotals);
	NativeModelCensus_ReportGating(level, levelID);
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
