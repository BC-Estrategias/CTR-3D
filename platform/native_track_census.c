/*
 * Track censuses for the native track renderer --
 * docs/NATIVE_TRACK_RENDERER_DESIGN.md section 7, INST-1/2/3/6.
 *
 * Read-only over the game's own level structs, once per level (keyed on the
 * mesh_info pointer changing), behind CTR_NATIVE_TRACK_CENSUS. Compiled out
 * entirely when the gate is off; when on, emits a [TRKCENSUS] block to
 * Platform_Log at the first rendered frame of each level and touches nothing
 * else -- golden output is unchanged by construction (no game state written).
 *
 * What each census decides (the go/no-go inputs for renderer v1):
 *   INST-1  vertex sharing -- adjacent quadblocks reusing pool indices is the
 *           no-crack premise. Low sharing on some track = seam work returns.
 *   INST-2  exclusion share -- leaves referencing water/scenery-animated
 *           vertices stay on the packet path in v1; this counts what that
 *           costs per track (static tris are the renderer's inventory).
 *   INST-3  AnimTex inventory -- how many faces need load-time frame
 *           pre-decode + per-frame UV patching.
 *   INST-6  texture working set -- distinct (tpage,clut) pairs and 64px
 *           subtiles per LOD variant; sizes the dedicated track atlas and
 *           the variants-as-mips plan.
 *
 * PC-first: counts transfer, no Azahar needed.
 */

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_TRACK_CENSUS) && CTR_NATIVE_TRACK_CENSUS

/* Bounded hash set for INST-6 tuple counting. Load-time only; sized for the
 * worst plausible track working set (a few thousand tuples), linear probe,
 * counts saturate if full rather than growing. */
#define TRKCENSUS_SET_SLOTS 16384

#define TRKCENSUS_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define TRKCENSUS_MAX(a, b) (((a) > (b)) ? (a) : (b))

struct TrkCensusSet
{
	u64 keys[TRKCENSUS_SET_SLOTS];
	int count;
	int dropped;
};

static struct TrkCensusSet s_trkPairSet[3];
static struct TrkCensusSet s_trkSubSet[3];

static void TrkCensus_SetReset(struct TrkCensusSet *set)
{
	memset(set->keys, 0, sizeof(set->keys));
	set->count = 0;
	set->dropped = 0;
}

static void TrkCensus_SetAdd(struct TrkCensusSet *set, u64 key)
{
	/* key 0 is the empty sentinel; nudge real zero keys. */
	u32 slot = (u32)((key ^ (key >> 29)) * 0x9e3779b1u) % TRKCENSUS_SET_SLOTS;
	int probes = 0;

	if (key == 0)
	{
		key = 1;
	}

	while (set->keys[slot] != 0)
	{
		if (set->keys[slot] == key)
		{
			return;
		}

		slot = (slot + 1) % TRKCENSUS_SET_SLOTS;
		if (++probes >= TRKCENSUS_SET_SLOTS)
		{
			set->dropped++;
			return;
		}
	}

	set->keys[slot] = key;
	set->count++;
}

static int TrkCensus_PlausibleLayout(const struct TextureLayout *layout)
{
	/* Same plausibility test the 226 emitter applies before trusting a
	 * texture word (226_00_DrawLevelOvr1P.c IsPlausibleTextureLayout). */
	return (layout != NULL) && ((CTR_ReadU16LE(&layout->tpage) & 0xfe00) == 0);
}

/* Count one IconGroup4's far/middle/near variants into the per-variant sets.
 * Variant index: 0=near, 1=middle, 2=far -- accessed by TextureLayout
 * arithmetic, not field names (windows.h #defines near/far on MinGW). */
static void TrkCensus_CountGroup(const struct IconGroup4 *group)
{
	static const int variantStep[3] = {2, 1, 0}; /* near, middle, far */

	for (int variant = 0; variant < 3; variant++)
	{
		const struct TextureLayout *layout = (const struct TextureLayout *)group + variantStep[variant];

		if (!TrkCensus_PlausibleLayout(layout))
		{
			continue;
		}

		u16 tpage = CTR_ReadU16LE(&layout->tpage);
		u16 clut = CTR_ReadU16LE(&layout->clut);
		u64 pairKey = ((u64)tpage << 16) | clut;

		TrkCensus_SetAdd(&s_trkPairSet[variant], pairKey);

		/* 64px subtile coverage of the face's UV bbox -- the decode
		 * cache's own sub-page granularity, 8KB decoded RGBA5551 each. */
		u8 u0 = layout->u0, u1 = layout->u1, u2 = layout->u2, u3 = layout->u3;
		u8 v0 = layout->v0, v1 = layout->v1, v2 = layout->v2, v3 = layout->v3;
		int minU = TRKCENSUS_MIN(TRKCENSUS_MIN(u0, u1), TRKCENSUS_MIN(u2, u3)) >> 6;
		int maxU = TRKCENSUS_MAX(TRKCENSUS_MAX(u0, u1), TRKCENSUS_MAX(u2, u3)) >> 6;
		int minV = TRKCENSUS_MIN(TRKCENSUS_MIN(v0, v1), TRKCENSUS_MIN(v2, v3)) >> 6;
		int maxV = TRKCENSUS_MAX(TRKCENSUS_MAX(v0, v1), TRKCENSUS_MAX(v2, v3)) >> 6;

		for (int tv = minV; tv <= maxV; tv++)
		{
			for (int tu = minU; tu <= maxU; tu++)
			{
				TrkCensus_SetAdd(&s_trkSubSet[variant], (pairKey << 4) | (u64)((tv << 2) | tu));
			}
		}
	}
}

void NativeTrackCensus_Frame(struct Level *level, struct mesh_info *mesh)
{
	/* Latch on pointer + content fingerprint, not pointer identity alone:
	 * under the arena heap model a different level's mesh_info can land at a
	 * previously-seen address. */
	static struct mesh_info *s_lastMesh = NULL;
	static int s_lastQb = -1, s_lastVtx = -1;

	if ((level == NULL) || (mesh == NULL))
	{
		return;
	}

	if ((mesh == s_lastMesh) && (mesh->numQuadBlock == s_lastQb) && (mesh->numVertex == s_lastVtx))
	{
		return;
	}

	s_lastMesh = mesh;
	s_lastQb = mesh->numQuadBlock;
	s_lastVtx = mesh->numVertex;

	int numQuadBlock = mesh->numQuadBlock;
	int numVertex = mesh->numVertex;
	struct QuadBlock *quadBlocks = mesh->ptrQuadBlockArray;
	struct LevVertex *vertexArray = mesh->ptrVertexArray;

	if ((quadBlocks == NULL) || (vertexArray == NULL) || (numQuadBlock <= 0) || (numVertex <= 0) || (numQuadBlock > 20000) || (numVertex > 200000))
	{
		Platform_Log("[TRKCENSUS] lev=%p skipped (qb=%d vtx=%d)\n", (void *)level, numQuadBlock, numVertex);
		return;
	}

	/* ---- INST-1: vertex sharing ---------------------------------------- */
	u8 *refcount = (u8 *)calloc((size_t)numVertex, 1);
	u8 *animMark = (u8 *)calloc((size_t)numVertex, 1);

	if ((refcount == NULL) || (animMark == NULL))
	{
		free(refcount);
		free(animMark);
		return;
	}

	for (int qb = 0; qb < numQuadBlock; qb++)
	{
		for (int slot = 0; slot < 9; slot++)
		{
			u16 vi = quadBlocks[qb].index[slot];

			if ((int)vi < numVertex && refcount[vi] < 255)
			{
				refcount[vi]++;
			}
		}
	}

	/* Index sharing is sufficient but not necessary for crack-freedom: two
	 * pool entries with IDENTICAL positions project identically under one
	 * matrix, so only border verts that are BOTH unshared and position-unique
	 * can open a seam (and those are usually genuine track borders). Count
	 * position twins via a hash set over packed x/y/z. */
	static u64 s_posKeys[TRKCENSUS_SET_SLOTS];
	static s32 s_posFirst[TRKCENSUS_SET_SLOTS];
	u8 *posTwin = (u8 *)calloc((size_t)numVertex, 1);

	memset(s_posKeys, 0, sizeof(s_posKeys));

	if (posTwin != NULL)
	{
		for (int vi = 0; vi < numVertex; vi++)
		{
			/* 48-bit packed position; +1 keeps 0 free as the empty sentinel. */
			u64 key = (((u64)(u16)vertexArray[vi].pos.x << 32) | ((u64)(u16)vertexArray[vi].pos.y << 16) | (u64)(u16)vertexArray[vi].pos.z) + 1;
			u32 slot = (u32)((key ^ (key >> 29)) * 0x9e3779b1u) % TRKCENSUS_SET_SLOTS;
			int probes = 0;

			while ((s_posKeys[slot] != 0) && (s_posKeys[slot] != key))
			{
				slot = (slot + 1) % TRKCENSUS_SET_SLOTS;
				if (++probes >= TRKCENSUS_SET_SLOTS)
				{
					break;
				}
			}

			if (probes >= TRKCENSUS_SET_SLOTS)
			{
				continue;
			}

			if (s_posKeys[slot] == 0)
			{
				s_posKeys[slot] = key;
				s_posFirst[slot] = vi;
			}
			else
			{
				s32 firstVi = s_posFirst[slot];

				posTwin[vi] = 1;
				if (firstVi >= 0 && firstVi < numVertex)
				{
					posTwin[firstVi] = 1;
				}
			}
		}
	}

	/* The definitive crack test (v2 of this census): match quadblock OUTER
	 * EDGES by endpoint positions. An edge seen twice with the same corner
	 * positions is a true adjacency; if the two sides' EDGE-MID positions
	 * differ, that is a genuine T-junction/crack candidate. Edges seen once
	 * are borders (cannot crack). This subsumes the per-vertex proxy below,
	 * which stays for continuity with run-2 numbers. */
	static u64 s_edgeKeys[TRKCENSUS_SET_SLOTS];
	static u64 s_edgeMid[TRKCENSUS_SET_SLOTS];
	int edgeMatched = 0, edgeBorder = 0, edgeCrack = 0, edgeDropped = 0;

	memset(s_edgeKeys, 0, sizeof(s_edgeKeys));

	/* Outer edges of the 3x3 grid: corner pair + the mid between them. */
	static const u8 outerEdges[4][3] = {{0, 1, 4}, {0, 2, 5}, {1, 3, 7}, {2, 3, 8}};

	for (int qb = 0; qb < numQuadBlock; qb++)
	{
		for (int e = 0; e < 4; e++)
		{
			u16 ia = quadBlocks[qb].index[outerEdges[e][0]];
			u16 ib = quadBlocks[qb].index[outerEdges[e][1]];
			u16 im = quadBlocks[qb].index[outerEdges[e][2]];

			if ((int)ia >= numVertex || (int)ib >= numVertex || (int)im >= numVertex)
			{
				continue;
			}

			u64 ka = (((u64)(u16)vertexArray[ia].pos.x << 32) | ((u64)(u16)vertexArray[ia].pos.y << 16) | (u64)(u16)vertexArray[ia].pos.z) + 1;
			u64 kb = (((u64)(u16)vertexArray[ib].pos.x << 32) | ((u64)(u16)vertexArray[ib].pos.y << 16) | (u64)(u16)vertexArray[ib].pos.z) + 1;
			u64 km = (((u64)(u16)vertexArray[im].pos.x << 32) | ((u64)(u16)vertexArray[im].pos.y << 16) | (u64)(u16)vertexArray[im].pos.z) + 1;

			/* Unordered endpoint pair -> orientation-independent edge key.
			 * xor+mix keeps both endpoints load-bearing. */
			u64 lo = (ka < kb) ? ka : kb;
			u64 hi = (ka < kb) ? kb : ka;
			u64 edgeKey = lo * 0x9e3779b97f4a7c15ull ^ (hi + 0x517cc1b727220a95ull);

			if (edgeKey == 0)
			{
				edgeKey = 1;
			}

			u32 slot = (u32)((edgeKey ^ (edgeKey >> 31)) % TRKCENSUS_SET_SLOTS);
			int probes = 0;

			while ((s_edgeKeys[slot] != 0) && (s_edgeKeys[slot] != edgeKey))
			{
				slot = (slot + 1) % TRKCENSUS_SET_SLOTS;
				if (++probes >= TRKCENSUS_SET_SLOTS)
				{
					break;
				}
			}

			if (probes >= TRKCENSUS_SET_SLOTS)
			{
				edgeDropped++;
				continue;
			}

			/* km fits in 49 bits; bit 63 marks "already matched once" so an
			 * edge seen 3+ times (stacked/overlapping geometry) only moves
			 * the border count on its second sighting. */
			if (s_edgeKeys[slot] == 0)
			{
				s_edgeKeys[slot] = edgeKey;
				s_edgeMid[slot] = km;
				edgeBorder++; /* provisional; matched below if seen again */
			}
			else
			{
				edgeMatched++;

				if ((s_edgeMid[slot] >> 63) == 0)
				{
					edgeBorder--; /* first sighting was not a border after all */
					s_edgeMid[slot] |= (1ull << 63);
				}

				if ((s_edgeMid[slot] & ~(1ull << 63)) != km)
				{
					edgeCrack++;
				}
			}
		}
	}

	/* Grid slots: corners 0-3, edge mids 4/5/7/8, center 6. A slot instance
	 * counts as shared when its pool vertex is referenced by 2+ quadblocks;
	 * "risk" = neither shared nor position-twinned. */
	int midShared = 0, midTotal = 0, cornerShared = 0, cornerTotal = 0, centerShared = 0;
	int midRisk = 0, cornerRisk = 0;
	static const int edgeMidSlots[4] = {4, 5, 7, 8};

	for (int qb = 0; qb < numQuadBlock; qb++)
	{
		for (int s = 0; s < 4; s++)
		{
			u16 vi = quadBlocks[qb].index[edgeMidSlots[s]];

			if ((int)vi < numVertex)
			{
				midTotal++;
				midShared += (refcount[vi] >= 2);
				midRisk += (refcount[vi] < 2) && (posTwin != NULL) && !posTwin[vi];
			}

			vi = quadBlocks[qb].index[s]; /* corners are slots 0-3 */
			if ((int)vi < numVertex)
			{
				cornerTotal++;
				cornerShared += (refcount[vi] >= 2);
				cornerRisk += (refcount[vi] < 2) && (posTwin != NULL) && !posTwin[vi];
			}
		}

		u16 ci = quadBlocks[qb].index[6];

		if ((int)ci < numVertex)
		{
			centerShared += (refcount[ci] >= 2);
		}
	}

	free(posTwin);

	/* ---- INST-2: animated-vertex / water exclusion ----------------------
	 * Retail branches every reader of these two systems on configFlags & 4
	 * (water vs SCVert are mutually exclusive per level; the INACTIVE side's
	 * fields are unvalidated asset data). Respect the
	 * same discriminant: read only the active side. */
	int waterVtx = 0, scVtx = 0;

	if ((level->configFlags & 4) == 0)
	{
		for (int i = 0; i < level->numWaterVertices; i++)
		{
			struct LevVertex *v = level->ptr_water[i].v;
			ptrdiff_t vi = v - vertexArray;

			if (vi >= 0 && vi < numVertex)
			{
				animMark[vi] = 1;
				waterVtx++;
			}
		}
	}
	else
	{
		for (int i = 0; i < level->numSCVert; i++)
		{
			struct LevVertex *v = level->ptrSCVert[i].v;
			ptrdiff_t vi = v - vertexArray;

			if (vi >= 0 && vi < numVertex)
			{
				animMark[vi] = 1;
				scVtx++;
			}
		}
	}

	int leaves = 0, waterLeaves = 0, animLeaves = 0, staticLeaves = 0;
	int waterQuads = 0, animQuads = 0, staticQuads = 0;

	for (int n = 0; n < mesh->numBspNodes; n++)
	{
		struct BSP *bsp = &mesh->bspRoot[n];

		if ((bsp->flag & BSP_NODE_FLAG_LEAF) == 0)
		{
			continue;
		}

		leaves++;

		int numQuads = bsp->data.leaf.numQuads;
		struct QuadBlock *range = bsp->data.leaf.ptrQuadBlockArray;
		int touchesAnim = 0;

		if ((range != NULL) && (numQuads > 0) && (numQuads <= numQuadBlock))
		{
			for (int q = 0; q < numQuads && !touchesAnim; q++)
			{
				for (int slot = 0; slot < 9; slot++)
				{
					u16 vi = range[q].index[slot];

					if ((int)vi < numVertex && animMark[vi])
					{
						touchesAnim = 1;
						break;
					}
				}
			}
		}

		if ((bsp->flag & BSP_LEAF_FLAG_WATER) != 0)
		{
			waterLeaves++;
			waterQuads += numQuads;
		}
		else if (touchesAnim)
		{
			animLeaves++;
			animQuads += numQuads;
		}
		else
		{
			staticLeaves++;
			staticQuads += numQuads;
		}
	}

	/* ---- INST-3: AnimTex inventory -------------------------------------- */
	int animTexStructs = 0, animTexFramesTot = 0, animTexFramesMax = 0, animFaces = 0;
	struct AnimTex *animStart = level->ptr_anim_tex;

	if (animStart != NULL)
	{
		struct AnimTex *cur = animStart;

		/* Same termination idiom as CTR_CycleTex_LEV: the chain ends when a
		 * struct's first word points back at the chain start. Guarded. */
		while ((*(int *)cur != (int)animStart) && (animTexStructs < 4096))
		{
			animTexStructs++;
			animTexFramesTot += cur->numFrames;

			if (cur->numFrames > animTexFramesMax)
			{
				animTexFramesMax = cur->numFrames;
			}

			struct IconGroup4 **frames = ANIMTEX_GETARRAY(cur);

			cur = (struct AnimTex *)&frames[cur->numFrames];
		}
	}

	/* ---- INST-6: texture working set ------------------------------------ */
	for (int variant = 0; variant < 3; variant++)
	{
		TrkCensus_SetReset(&s_trkPairSet[variant]);
		TrkCensus_SetReset(&s_trkSubSet[variant]);
	}

	for (int qb = 0; qb < numQuadBlock; qb++)
	{
		for (int face = 0; face < 4; face++)
		{
			uintptr_t texPtr = (uintptr_t)quadBlocks[qb].ptr_texture_mid[face];

			if (texPtr == 0)
			{
				continue;
			}

			if ((texPtr & 1) != 0)
			{
				/* AnimTex face: count EVERY frame's group -- the atlas holds
				 * all frames resident (design section 4.1). */
				struct AnimTex *anim = (struct AnimTex *)(texPtr - 1);

				animFaces++;

				if ((anim->numFrames > 0) && (anim->numFrames <= 256))
				{
					struct IconGroup4 **frames = ANIMTEX_GETARRAY(anim);

					for (int f = 0; f < anim->numFrames; f++)
					{
						if (frames[f] != NULL)
						{
							TrkCensus_CountGroup(frames[f]);
						}
					}
				}
			}
			else
			{
				TrkCensus_CountGroup((const struct IconGroup4 *)texPtr);
			}
		}
	}

	/* ---- report ---------------------------------------------------------- */
	int dropped = 0;

	for (int variant = 0; variant < 3; variant++)
	{
		dropped += s_trkPairSet[variant].dropped + s_trkSubSet[variant].dropped;
	}

	Platform_Log("[TRKCENSUS] lev=%p qb=%d vtx=%d bspNodes=%d leaves=%d\n", (void *)level, numQuadBlock, numVertex, mesh->numBspNodes, leaves);
	Platform_Log("[TRKCENSUS] share: mid=%d/%d (%d%%) corner=%d/%d (%d%%) centerShared=%d | seamRisk mid=%d corner=%d (unshared+no pos twin)\n", midShared,
	             midTotal, midTotal ? (midShared * 100) / midTotal : 0, cornerShared, cornerTotal, cornerTotal ? (cornerShared * 100) / cornerTotal : 0,
	             centerShared, midRisk, cornerRisk);
	Platform_Log("[TRKCENSUS] edges: matched=%d border=%d CRACK=%d dropped=%d (of %d)\n", edgeMatched, edgeBorder, edgeCrack, edgeDropped, numQuadBlock * 4);
	Platform_Log("[TRKCENSUS] excl: waterVtx=%d scVtx=%d | leaves water=%d anim=%d static=%d | quads water=%d anim=%d static=%d (staticTris=%d)\n", waterVtx,
	             scVtx, waterLeaves, animLeaves, staticLeaves, waterQuads, animQuads, staticQuads, staticQuads * 8);
	Platform_Log("[TRKCENSUS] animtex: structs=%d framesTot=%d framesMax=%d animFaces=%d\n", animTexStructs, animTexFramesTot, animTexFramesMax, animFaces);
	Platform_Log("[TRKCENSUS] tex pairs n/m/f=%d/%d/%d sub64 n/m/f=%d/%d/%d estKB near=%d all=%d dropped=%d\n", s_trkPairSet[0].count, s_trkPairSet[1].count,
	             s_trkPairSet[2].count, s_trkSubSet[0].count, s_trkSubSet[1].count, s_trkSubSet[2].count, s_trkSubSet[0].count * 8,
	             (s_trkSubSet[0].count + s_trkSubSet[1].count + s_trkSubSet[2].count) * 8, dropped);

	free(refcount);
	free(animMark);
}

#endif /* CTR_NATIVE_TRACK_CENSUS */
