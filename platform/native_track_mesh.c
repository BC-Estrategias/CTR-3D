/*
 * Native track renderer -- load-time mesh builder (s27 scaffold).
 * docs/NATIVE_TRACK_RENDERER_DESIGN.md sections 4.1/4.2.
 *
 * Ladder step 1 (PC count-only): builds the resident-mesh representation --
 * per-static-leaf vertex/index ranges over the game's own quadblock data --
 * and logs a [TRKMESH] block. Nothing is drawn yet on any platform and the
 * gate is OFF by default, so golden output is untouched. The 3DS draw side
 * (linear-heap upload, track atlas, the (otIndex<<k)|bias depth shader)
 * lands on top of exactly these structures in a later step.
 *
 * Leaf classification mirrors the v1 packet-path exclusions (design 4.4):
 * water-flagged leaves and leaves referencing water/scenery-animated
 * vertices are skipped here and keep their retail draw. The s27 census
 * measured one scenery-heavy level where that excludes ~2/3 of quads --
 * the planned v2 answer is re-uploading those verts (they're enumerable),
 * not widening the exclusion.
 */

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER

/* 16 bytes, matches the design's budget. Positions are model-space straight
 * from LevVertex; colors are the near set (color_hi); u/v are page-space
 * texel bytes from the near-variant TextureLayout corner assigned to this
 * grid vertex (atlas translation happens at upload, not here); texRef
 * indexes the per-level layout table below; drawOrder is the signed
 * per-face OT byte folded into depth by the shader (GetProjectedOtIndex's
 * own formula, 226:1956). */
struct NativeTrackVert
{
	s16 x, y, z;
	s8 drawOrder;
	u8 faceFlags; /* bit0 doubleSided; bits 1-3 faceMode/rotation scratch */
	u8 r, g, b, a;
	u8 u, v;
	u16 texRef;
};

CTR_STATIC_ASSERT(sizeof(struct NativeTrackVert) == 16);

struct NativeTrackLeafRange
{
	u16 bspIndex;    /* index into mesh->bspRoot -- visibility lookup key */
	u16 atlasBin;    /* reserved: draws are binned per (leaf x atlas) */
	u32 firstIndex;  /* into s_trackIndices */
	u32 indexCount;
};

struct NativeTrackMeshState
{
	struct mesh_info *builtFor; /* rebuild latch, same idiom as the census */

	struct NativeTrackVert *verts;
	u32 vertCount;
	u16 *indices;
	u32 indexCount;
	struct NativeTrackLeafRange *leafRanges;
	u32 leafCount;

	/* Per-level table of near-variant TextureLayout pointers; texRef fields
	 * index it. AnimTex faces record the AnimTex instead (low bit set,
	 * matching the game's own encoding) so per-frame UV patching can find
	 * the live frame. */
	uintptr_t *texTable;
	u32 texCount;
};

global_variable struct NativeTrackMeshState s_trackMesh;

/* The four mid-LOD quads of the 3x3 grid (design section 2 / ctr-tools),
 * counter-clockwise in grid space; triangulated (0,2,1)(1,2,3) per quad,
 * the same diagonal ctr-tools exports. */
static const u8 sTrackFaceCorners[4][4] = {
    {0, 4, 5, 6},
    {4, 1, 6, 7},
    {5, 6, 2, 8},
    {6, 7, 8, 3},
};

static void NativeTrackMesh_Free(void)
{
	free(s_trackMesh.verts);
	free(s_trackMesh.indices);
	free(s_trackMesh.leafRanges);
	free(s_trackMesh.texTable);
	memset(&s_trackMesh, 0, sizeof(s_trackMesh));
}

static u16 NativeTrackMesh_InternTexture(uintptr_t texPtr)
{
	/* Small linear intern -- track texture tables measured at <=119 pairs
	 * (s27 census), so linear search at build time is fine. */
	for (u32 i = 0; i < s_trackMesh.texCount; i++)
	{
		if (s_trackMesh.texTable[i] == texPtr)
		{
			return (u16)i;
		}
	}

	s_trackMesh.texTable[s_trackMesh.texCount] = texPtr;
	return (u16)s_trackMesh.texCount++;
}

static int NativeTrackMesh_LeafTouchesAnim(const struct BSP *bsp, const u8 *animMark, int numVertex, int numQuadBlock)
{
	int numQuads = bsp->data.leaf.numQuads;
	struct QuadBlock *range = bsp->data.leaf.ptrQuadBlockArray;

	/* Upper bound included (s27 review finding): a leaf's numQuads is asset
	 * data; never trust it past the mesh's real array size. */
	if ((range == NULL) || (numQuads <= 0) || (numQuads > numQuadBlock))
	{
		return 0;
	}

	for (int q = 0; q < numQuads; q++)
	{
		for (int slot = 0; slot < 9; slot++)
		{
			u16 vi = range[q].index[slot];

			if ((int)vi < numVertex && animMark[vi])
			{
				return 1;
			}
		}
	}

	return 0;
}

void NativeTrackMesh_Frame(struct Level *level, struct mesh_info *mesh)
{
	/* Latch = pointer + content fingerprint (arena heaps reuse addresses --
	 * s27 review finding), and it is only recorded on SUCCESSFUL builds; the
	 * cheap-validation and calloc-failure paths below return unlatched so a
	 * transient failure retries next frame instead of pinning an empty mesh
	 * to that level forever. */
	static int s_builtQb = -1, s_builtVtx = -1;

	if ((level == NULL) || (mesh == NULL))
	{
		return;
	}

	if ((mesh == s_trackMesh.builtFor) && (mesh->numQuadBlock == s_builtQb) && (mesh->numVertex == s_builtVtx))
	{
		return;
	}

	NativeTrackMesh_Free();

	int numQuadBlock = mesh->numQuadBlock;
	int numVertex = mesh->numVertex;
	struct QuadBlock *quadBlocks = mesh->ptrQuadBlockArray;
	struct LevVertex *vertexArray = mesh->ptrVertexArray;

	if ((quadBlocks == NULL) || (vertexArray == NULL) || (numQuadBlock <= 0) || (numVertex <= 0) || (numQuadBlock > 20000) || (numVertex > 200000))
	{
		return;
	}

	/* Animated-vertex marks, shared logic with the census (kept separate on
	 * purpose: the census is a temporary instrument, this is product).
	 * configFlags & 4 discriminates water vs SCVert exactly as every retail
	 * reader does -- the inactive side's fields are unvalidated asset data. */
	u8 *animMark = (u8 *)calloc((size_t)numVertex, 1);

	if (animMark == NULL)
	{
		return;
	}

	if ((level->configFlags & 4) == 0)
	{
		for (int i = 0; i < level->numWaterVertices; i++)
		{
			ptrdiff_t vi = level->ptr_water[i].v - vertexArray;

			if (vi >= 0 && vi < numVertex)
			{
				animMark[vi] = 1;
			}
		}
	}
	else
	{
		for (int i = 0; i < level->numSCVert; i++)
		{
			ptrdiff_t vi = level->ptrSCVert[i].v - vertexArray;

			if (vi >= 0 && vi < numVertex)
			{
				animMark[vi] = 1;
			}
		}
	}

	/* Worst case: every quadblock in a static leaf, 4 faces x 4 verts and
	 * 4 faces x 6 indices. Allocate flat, log the real usage. */
	size_t maxVerts = (size_t)numQuadBlock * 16;
	size_t maxIndices = (size_t)numQuadBlock * 24;

	s_trackMesh.verts = (struct NativeTrackVert *)malloc(maxVerts * sizeof(struct NativeTrackVert));
	s_trackMesh.indices = (u16 *)malloc(maxIndices * sizeof(u16));
	s_trackMesh.leafRanges = (struct NativeTrackLeafRange *)malloc((size_t)mesh->numBspNodes * sizeof(struct NativeTrackLeafRange));
	s_trackMesh.texTable = (uintptr_t *)malloc(1024 * sizeof(uintptr_t));

	if ((s_trackMesh.verts == NULL) || (s_trackMesh.indices == NULL) || (s_trackMesh.leafRanges == NULL) || (s_trackMesh.texTable == NULL))
	{
		NativeTrackMesh_Free();
		free(animMark);
		return;
	}

	int leavesTotal = 0, leavesExcluded = 0, facesDoubleSided = 0, facesNoTexture = 0, facesAnimTex = 0;

	for (int n = 0; n < mesh->numBspNodes; n++)
	{
		struct BSP *bsp = &mesh->bspRoot[n];

		if ((bsp->flag & BSP_NODE_FLAG_LEAF) == 0)
		{
			continue;
		}

		leavesTotal++;

		if (((bsp->flag & BSP_LEAF_FLAG_WATER) != 0) || NativeTrackMesh_LeafTouchesAnim(bsp, animMark, numVertex, numQuadBlock))
		{
			leavesExcluded++;
			continue;
		}

		int numQuads = bsp->data.leaf.numQuads;
		struct QuadBlock *range = bsp->data.leaf.ptrQuadBlockArray;

		if ((range == NULL) || (numQuads <= 0) || (numQuads > numQuadBlock))
		{
			continue;
		}

		struct NativeTrackLeafRange *leafRange = &s_trackMesh.leafRanges[s_trackMesh.leafCount];

		leafRange->bspIndex = (u16)n;
		leafRange->atlasBin = 0;
		leafRange->firstIndex = s_trackMesh.indexCount;

		for (int q = 0; q < numQuads; q++)
		{
			struct QuadBlock *block = &range[q];
			u32 drawOrderLow = CTR_ReadU32LE(&block->draw_order_low);
			int doubleSided = (drawOrderLow & QUADBLOCK_DRAW_ORDER_LOW_DOUBLE_SIDED) != 0;

			for (int face = 0; face < 4; face++)
			{
				uintptr_t texPtr = (uintptr_t)block->ptr_texture_mid[face];
				const struct TextureLayout *layout = NULL;

				if (texPtr == 0)
				{
					facesNoTexture++;
					continue;
				}

				if ((texPtr & 1) != 0)
				{
					/* AnimTex face: intern the AnimTex itself (low bit
					 * kept); resolve the live frame's near layout for the
					 * baked UVs -- the per-frame patcher rewrites them. */
					const struct AnimTex *anim = (const struct AnimTex *)(texPtr - 1);

					facesAnimTex++;
					if (anim->ptrActiveTex != NULL)
					{
						layout = (const struct TextureLayout *)anim->ptrActiveTex + 2; /* near */
					}
				}
				else
				{
					layout = (const struct TextureLayout *)texPtr + 2; /* near */
				}

				if ((layout == NULL) || ((CTR_ReadU16LE(&layout->tpage) & 0xfe00) != 0))
				{
					facesNoTexture++;
					continue;
				}

				if (s_trackMesh.texCount >= 1024)
				{
					continue;
				}

				/* u16 index buffer (PICA's native format): the biggest
				 * measured track builds ~44k verts; refuse past the format
				 * limit rather than wrap. Logged via the vert count below. */
				if (s_trackMesh.vertCount + 4 > 0xffff)
				{
					continue;
				}

				/* Allocation capacity guard (s27 review finding): the flat
				 * buffers are sized for a PARTITION of the quadblock array;
				 * overlapping leaf ranges could exceed it. Never write past. */
				if (((size_t)s_trackMesh.vertCount + 4 > maxVerts) || ((size_t)s_trackMesh.indexCount + 6 > maxIndices))
				{
					continue;
				}

				/* Vertex indices are asset data too -- skip the face if any
				 * corner would read outside the pool. */
				if ((block->index[sTrackFaceCorners[face][0]] >= numVertex) || (block->index[sTrackFaceCorners[face][1]] >= numVertex) ||
				    (block->index[sTrackFaceCorners[face][2]] >= numVertex) || (block->index[sTrackFaceCorners[face][3]] >= numVertex))
				{
					continue;
				}

				u16 texRef = NativeTrackMesh_InternTexture(texPtr);
				s8 drawOrder = (s8)((const u8 *)&block->draw_order_high)[face];
				u32 baseVert = s_trackMesh.vertCount;

				/* TextureLayout corners are (u0,v0)=TL (u1,v1)=TR (u2,v2)=BL
				 * (u3,v3)=BR in face-local space. Grid corner order in
				 * sTrackFaceCorners is TL,TR,BL,BR, so corner c takes UV c.
				 * faceFlags rotation/flip swizzles are NOT applied yet --
				 * design risk #4, audited against ctr-tools before the first
				 * textured draw. Scaffold records the raw assignment. */
				const u8 *uvBytes = &layout->u0;

				for (int c = 0; c < 4; c++)
				{
					const struct LevVertex *src = &vertexArray[block->index[sTrackFaceCorners[face][c]]];
					struct NativeTrackVert *dst = &s_trackMesh.verts[s_trackMesh.vertCount++];

					dst->x = src->pos.x;
					dst->y = src->pos.y;
					dst->z = src->pos.z;
					dst->drawOrder = drawOrder;
					dst->faceFlags = (u8)doubleSided;
					dst->r = src->color_hi[0];
					dst->g = src->color_hi[1];
					dst->b = src->color_hi[2];
					dst->a = 0xff;
					/* Corner UV pairs sit at +0,+4,+8,+10 in TextureLayout
					 * (clut/tpage interleave) -- use the named fields via
					 * offsets 0/4/8/10. */
					dst->u = uvBytes[(c == 0) ? 0 : (c == 1) ? 4 : (c == 2) ? 8 : 10];
					dst->v = uvBytes[(c == 0) ? 1 : (c == 1) ? 5 : (c == 2) ? 9 : 11];
					dst->texRef = texRef;
				}

				facesDoubleSided += doubleSided;

				/* ctr-tools' export split: (0,2,1)(1,2,3). */
				u16 *idx = &s_trackMesh.indices[s_trackMesh.indexCount];

				idx[0] = (u16)(baseVert + 0);
				idx[1] = (u16)(baseVert + 2);
				idx[2] = (u16)(baseVert + 1);
				idx[3] = (u16)(baseVert + 1);
				idx[4] = (u16)(baseVert + 2);
				idx[5] = (u16)(baseVert + 3);
				s_trackMesh.indexCount += 6;
			}
		}

		leafRange->indexCount = s_trackMesh.indexCount - leafRange->firstIndex;

		if (leafRange->indexCount > 0)
		{
			s_trackMesh.leafCount++;
		}
	}

	free(animMark);

	/* Latch only now, on success -- see the fingerprint comment up top. */
	s_trackMesh.builtFor = mesh;
	s_builtQb = numQuadBlock;
	s_builtVtx = numVertex;

	Platform_Log("[TRKMESH] lev=%p leaves=%u/%d (excl=%d) verts=%u idx=%u tex=%u | KB verts=%u idx=%u | faces ds=%d animtex=%d notex=%d\n", (void *)level,
	             s_trackMesh.leafCount, leavesTotal, leavesExcluded, s_trackMesh.vertCount, s_trackMesh.indexCount, s_trackMesh.texCount,
	             (u32)(s_trackMesh.vertCount * sizeof(struct NativeTrackVert) / 1024), (u32)(s_trackMesh.indexCount * sizeof(u16) / 1024), facesDoubleSided,
	             facesAnimTex, facesNoTexture);
}

#endif /* CTR_NATIVE_TRACK_RENDERER */
