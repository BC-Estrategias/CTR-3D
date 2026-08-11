/*
 * Bake-parity diff -- see include/platform/native_bake_parity.h for the
 * contract and docs/PORT_FEASIBILITY_3DS.md "track bake pre-export" for the
 * campaign context. Unity-TU file, included from main.c AFTER
 * native_track_mesh.c on purpose: the perm-conviction report reads that
 * file's sTrackUvPerm table directly (same-TU file-scope access, the same
 * arrangement native_track_frame.c uses for the mesh accessors).
 *
 * Threading: everything here runs either on the game thread during a mesh
 * build (BuildReset/BuildNoteVert) or inside the OT walk (BeginWalk/Note*).
 * On PC the walk runs synchronously on the game thread (the async render
 * queue is a 3DS render-thread feature), so there is no concurrent access to
 * reason about; the #error below keeps this file off 3DS where that argument
 * would not hold.
 */

#if defined(CTR_NATIVE) && defined(CTR_NATIVE_BAKE_PARITY) && CTR_NATIVE_BAKE_PARITY

#if defined(__3DS__)
#error "CTR_NATIVE_BAKE_PARITY is a PC-only diagnostic (static tables sized for PC RAM; single-thread walk assumption)"
#endif

#include "platform/native_bake_parity.h"

/* Mesh caps verts at 0xffff (u16 index format), so faces cap at 16383. */
#define BP_MAX_VERTS 65536
#define BP_MAX_FACES (BP_MAX_VERTS / 4)
#define BP_HASH_BITS 18
#define BP_HASH_SLOTS (1u << BP_HASH_BITS) /* 262144 slots >= 2x worst-case keys (2 per vert) */
#define BP_MAX_CAND_VERTS 64
#define BP_MAX_CAND_FACES 32
#define BP_REPORT_WALKS 600
#define BP_EXEMPLARS 4

/* Pre-weld source position + color_lo per baked vert, recorded at build time.
 * Welded verts change position and would never match retail's GTE inputs by
 * their final key alone; color_lo is the FULL_DYNAMIC far-fade source the
 * color classifier needs alongside the baked color_hi. */
struct BpSrcVert
{
	s16 x, y, z;
	u8 lr, lg, lb;
	u8 valid;
};

/* One face the diff flagged, kept verbatim for the report. */
struct BpExemplar
{
	u32 face;
	u32 leafBsp;
	u8 rotA, rotT; /* perm class only; 8 = not a valid perm row */
	u8 pktU[4], pktV[4], bakU[4], bakV[4];
	u8 pktR[4], pktG[4], pktB[4], bakR[4], bakG[4], bakB[4];
};

static struct BpSrcVert s_bpSrc[BP_MAX_VERTS];

static u64 s_bpKeys[BP_HASH_SLOTS];
static u32 s_bpVals[BP_HASH_SLOTS];
static u32 s_bpKeyCount;
static u32 s_bpFaceLeafBsp[BP_MAX_FACES];

static int s_bpIndexed;
static const struct NativeTrackVert *s_bpVerts;
static unsigned s_bpVertCount;

static struct
{
	u64 walks, polys, lkMiss, noCand, noFace, ambig, degen, matchedTri, matchedQuad;
	u64 uvExact, uvPerm, uvNoCorner, uvDup;
	/* nocorner direction split: bakBigger = bake stored a HIGHER-res variant
	 * than retail drew (benign -- sharper than retail at distance);
	 * pktBigger = retail drew higher res than the bake stored (a real
	 * visual downgrade); sameSize = same variant footprint but shifted
	 * (animated frames land here). Quads only. */
	u64 ncBakBigger, ncPktBigger, ncSameSize;
	u64 rotFaces[8], rotExact[8];
	u64 permHist[8][9]; /* [assigned rot][observed rot, 8 = not a valid row] */
	u64 colHi, colLo, colBetween, colOther, colChecked;
} s_bpStat;

static struct BpExemplar s_bpExPerm[BP_EXEMPLARS];
static struct BpExemplar s_bpExNoCorner[BP_EXEMPLARS];
static struct BpExemplar s_bpExColOther[BP_EXEMPLARS];
static u32 s_bpExPermN, s_bpExNoCornerN, s_bpExColOtherN;

/* noFace exemplars carry model-space corner positions instead of face data
 * (there IS no matched face) -- the localizer for "retail draws a face here
 * that the bake lacks or moved". Sampled every 25000th and rotated so the
 * kept set spreads across the level instead of freezing on frame one. */
struct BpGeoExemplar
{
	s16 mv[4][3];
	u8 n;
};

static struct BpGeoExemplar s_bpExNoFace[BP_EXEMPLARS];
static u32 s_bpExNoFaceSeen;

/* Empirical face-table map (the Coco Park stripe class): retail indexes
 * the face's FULL 5-bit value (rot | mode<<3) into a 24-word corner-selection
 * table (R226.scratchInitTable) -- modes 1/2 select corner REPEATS, i.e.
 * triangles drawn as degenerate quads -- while the bake applies rotation-only
 * UV perms to a fixed 4-corner emit. Rather than reverse the selector bit
 * packing, learn the table from retail's own packets: for every matched face,
 * record which GRID corner (the matcher's position assignment) and which
 * layout UV INDEX (membership among the face's baked UVs) retail put at each
 * packet corner; latch the first complete mapping per (value, shape) and
 * count agreement after. A near-unanimous map IS the table, measured. */
struct BpModeMap
{
	u8 valid;
	u8 n;
	u8 grid[4];
	u8 idx[4];
	u32 agree, disagree;
};

static struct BpModeMap s_bpModeMap[24][2]; /* [5-bit face value][0=tri shape, 1=quad shape] */

static const char *const s_bpRotName[9] = {"None", "Rot90", "Rot180", "Rot270", "FRot270", "FRot180", "FRot90", "Flip", "?!"};

static void BakeParity_Report(void);

static u64 BakeParity_PackPos(s16 x, s16 y, s16 z)
{
	/* Same packing as NativeTrackMesh_PackPos: +1 keeps 0 free as the empty
	 * sentinel. */
	return (((u64)(u16)x << 32) | ((u64)(u16)y << 16) | (u64)(u16)z) + 1;
}

static u32 BakeParity_Hash(u64 key)
{
	return (u32)((key * 0x9E3779B97F4A7C15ull) >> (64 - BP_HASH_BITS));
}

static void BakeParity_Insert(u64 key, u32 vertIndex)
{
	u32 slot = BakeParity_Hash(key);

	if (s_bpKeyCount >= BP_HASH_SLOTS - 1)
	{
		return;
	}

	while (s_bpKeys[slot] != 0)
	{
		slot = (slot + 1) & (BP_HASH_SLOTS - 1);
	}

	s_bpKeys[slot] = key;
	s_bpVals[slot] = vertIndex;
	s_bpKeyCount++;
}

static int BakeParity_Collect(u64 key, u32 *out, int cap)
{
	u32 slot = BakeParity_Hash(key);
	int n = 0;

	while (s_bpKeys[slot] != 0)
	{
		if (s_bpKeys[slot] == key && n < cap)
		{
			out[n++] = s_bpVals[slot];
		}

		slot = (slot + 1) & (BP_HASH_SLOTS - 1);
	}

	return n;
}

void BakeParity_BuildReset(void)
{
	memset(s_bpSrc, 0, sizeof(s_bpSrc));
	s_bpIndexed = 0;
}

void BakeParity_BuildNoteVert(unsigned vertIndex, const struct LevVertex *src)
{
	if (vertIndex >= BP_MAX_VERTS)
	{
		return;
	}

	s_bpSrc[vertIndex].x = src->pos.x;
	s_bpSrc[vertIndex].y = src->pos.y;
	s_bpSrc[vertIndex].z = src->pos.z;
	s_bpSrc[vertIndex].lr = src->color_lo[0];
	s_bpSrc[vertIndex].lg = src->color_lo[1];
	s_bpSrc[vertIndex].lb = src->color_lo[2];
	s_bpSrc[vertIndex].valid = 1;
}

static void BakeParity_Reindex(void)
{
	const struct NativeTrackVert *verts = NativeTrackMesh_Verts();
	unsigned vertCount = NativeTrackMesh_VertCount();
	const u16 *indices = NativeTrackMesh_Indices();
	unsigned indexCount = NativeTrackMesh_IndexCount();
	unsigned leafCount = NativeTrackMesh_LeafCount();

	/* Final flush for the outgoing level: without this, a level that ends
	 * between report boundaries loses its most complete numbers. */
	if ((s_bpStat.matchedTri + s_bpStat.matchedQuad) > 0)
	{
		Platform_Log("[BAKEPAR] -- final report for outgoing level --\n");
		BakeParity_Report();
	}

	memset(s_bpKeys, 0, sizeof(s_bpKeys));
	memset(&s_bpStat, 0, sizeof(s_bpStat));
	memset(s_bpFaceLeafBsp, 0xff, sizeof(s_bpFaceLeafBsp));
	memset(s_bpModeMap, 0, sizeof(s_bpModeMap));
	s_bpKeyCount = 0;
	s_bpExPermN = 0;
	s_bpExNoCornerN = 0;
	s_bpExColOtherN = 0;
	s_bpExNoFaceSeen = 0;

	if (vertCount > BP_MAX_VERTS)
	{
		vertCount = BP_MAX_VERTS;
	}

	for (u32 i = 0; i < vertCount; i++)
	{
		u64 key = BakeParity_PackPos(verts[i].x, verts[i].y, verts[i].z);

		BakeParity_Insert(key, i);

		/* Welded vert: index its pre-weld source position too, since that is
		 * what retail's packets will resolve to through the capture table. */
		if (s_bpSrc[i].valid)
		{
			u64 srcKey = BakeParity_PackPos(s_bpSrc[i].x, s_bpSrc[i].y, s_bpSrc[i].z);

			if (srcKey != key)
			{
				BakeParity_Insert(srcKey, i);
			}
		}
	}

	for (unsigned l = 0; l < leafCount; l++)
	{
		unsigned bsp, first, count;

		if (!NativeTrackMesh_LeafAt(l, &bsp, &first, &count))
		{
			continue;
		}

		for (unsigned i = first; i < first + count && i < indexCount; i++)
		{
			u32 face = (u32)indices[i] / 4;

			if (face < BP_MAX_FACES)
			{
				s_bpFaceLeafBsp[face] = bsp;
			}
		}
	}

	s_bpVerts = verts;
	s_bpVertCount = vertCount;
	s_bpIndexed = 1;

	Platform_Log("[BAKEPAR] indexed verts=%u keys=%u leaves=%u -- diffing retail packets against the bake\n", vertCount, s_bpKeyCount, leafCount);
}

/* Bipartite match: packet corner k -> distinct baked corner j of face f with
 * an equal model-space position (either the vert's final position or, for a
 * welded vert, its recorded pre-weld source position). Depth-4 backtracking
 * over candidate masks -- at most 4! probes, in practice one. */
static int BakeParity_AssignCorners(u32 faceBase, const s16 mv[4][3], int n, u8 outAssign[4])
{
	u8 candMask[4];

	for (int k = 0; k < n; k++)
	{
		candMask[k] = 0;

		for (int j = 0; j < 4; j++)
		{
			const struct NativeTrackVert *v = &s_bpVerts[faceBase + j];
			int match = (v->x == mv[k][0]) && (v->y == mv[k][1]) && (v->z == mv[k][2]);

			if (!match && s_bpSrc[faceBase + j].valid)
			{
				match = (s_bpSrc[faceBase + j].x == mv[k][0]) && (s_bpSrc[faceBase + j].y == mv[k][1]) && (s_bpSrc[faceBase + j].z == mv[k][2]);
			}

			if (match)
			{
				candMask[k] |= (u8)(1 << j);
			}
		}

		if (candMask[k] == 0)
		{
			return 0;
		}
	}

	/* Iterative DFS with used-mask; k is the depth. */
	u8 pick[4] = {0, 0, 0, 0};
	u8 used = 0;
	int k = 0;

	while (k >= 0)
	{
		int advanced = 0;

		for (int j = pick[k]; j < 4; j++)
		{
			if ((candMask[k] & (1 << j)) && !(used & (1 << j)))
			{
				pick[k] = (u8)j;
				used |= (u8)(1 << j);
				outAssign[k] = (u8)j;
				advanced = 1;
				break;
			}
		}

		if (!advanced)
		{
			pick[k] = 0;
			k--;

			if (k >= 0)
			{
				used &= (u8)~(1 << outAssign[k]);
				pick[k] = (u8)(outAssign[k] + 1);
			}

			continue;
		}

		if (++k >= n)
		{
			return 1;
		}
	}

	return 0;
}

static void BakeParity_SaveExemplar(struct BpExemplar *slot, u32 face, int n, const u8 assign[4], const u8 pu[4], const u8 pv[4], const u8 pr[4],
                                    const u8 pg[4], const u8 pb[4], u8 rotA, u8 rotT)
{
	memset(slot, 0, sizeof(*slot));
	slot->face = face;
	slot->leafBsp = (face < BP_MAX_FACES) ? s_bpFaceLeafBsp[face] : 0xffffffffu;
	slot->rotA = rotA;
	slot->rotT = rotT;

	for (int k = 0; k < n; k++)
	{
		const struct NativeTrackVert *v = &s_bpVerts[face * 4 + assign[k]];

		slot->pktU[k] = pu[k];
		slot->pktV[k] = pv[k];
		slot->bakU[k] = v->u;
		slot->bakV[k] = v->v;
		slot->pktR[k] = pr[k];
		slot->pktG[k] = pg[k];
		slot->pktB[k] = pb[k];
		slot->bakR[k] = v->r;
		slot->bakG[k] = v->g;
		slot->bakB[k] = v->b;
	}
}

static void BakeParity_NotePoly(int inN, const s16 inSx[4], const s16 inSy[4], const u8 inPu[4], const u8 inPv[4], const u8 inPr[4], const u8 inPg[4],
                                const u8 inPb[4], int hasVertColors)
{
	if (!s_bpIndexed)
	{
		return;
	}

	s_bpStat.polys++;

	/* Degenerate-quad dedup (the Coco Park stripe class): retail draws
	 * faceMode!=0 faces as GT4s with a REPEATED corner -- a triangle wearing
	 * a quad's packet. The matcher's distinct-vert constraint would drop
	 * exactly the faces the mode map below exists to learn, so collapse
	 * repeats (by screen coordinate, preserving packet order) and treat the
	 * survivor set as the real shape. */
	s16 sx[4], sy[4];
	u8 pu[4], pv[4], pr[4], pg[4], pb[4];
	int n = 0;

	for (int k = 0; k < inN; k++)
	{
		int dup = 0;

		for (int m = 0; m < n; m++)
		{
			if (sx[m] == inSx[k] && sy[m] == inSy[k])
			{
				dup = 1;
				break;
			}
		}

		if (dup)
		{
			continue;
		}

		sx[n] = inSx[k];
		sy[n] = inSy[k];
		pu[n] = inPu[k];
		pv[n] = inPv[k];
		pr[n] = inPr[k];
		pg[n] = inPg[k];
		pb[n] = inPb[k];
		n++;
	}

	if (n < inN)
	{
		s_bpStat.degen++;
	}

	if (n < 3)
	{
		return;
	}

	s16 mv[4][3];

	for (int k = 0; k < n; k++)
	{
		if (!GTECapture_LookupModel(sx[k], sy[k], mv[k]))
		{
			s_bpStat.lkMiss++;
			return;
		}
	}

	/* Candidate faces come from corner 0's position; the other corners then
	 * have to complete the same face. */
	u32 candVerts[BP_MAX_CAND_VERTS];
	int candVertN = BakeParity_Collect(BakeParity_PackPos(mv[0][0], mv[0][1], mv[0][2]), candVerts, BP_MAX_CAND_VERTS);

	if (candVertN == 0)
	{
		s_bpStat.noCand++;
		return;
	}

	u32 candFaces[BP_MAX_CAND_FACES];
	int candFaceN = 0;

	for (int i = 0; i < candVertN && candFaceN < BP_MAX_CAND_FACES; i++)
	{
		u32 face = candVerts[i] / 4;
		int seen = 0;

		for (int f = 0; f < candFaceN; f++)
		{
			if (candFaces[f] == face)
			{
				seen = 1;
				break;
			}
		}

		if (!seen)
		{
			candFaces[candFaceN++] = face;
		}
	}

	u32 matchedFace = 0;
	u8 assign[4] = {0, 0, 0, 0};
	int matched = 0;

	for (int f = 0; f < candFaceN; f++)
	{
		u8 tryAssign[4];

		if ((u64)candFaces[f] * 4 + 3 >= s_bpVertCount)
		{
			continue;
		}

		if (BakeParity_AssignCorners(candFaces[f] * 4, mv, n, tryAssign))
		{
			if (matched)
			{
				/* Two faces both complete: only a problem if their diffable
				 * content differs under the assignment (overlapping leaf
				 * ranges can legitimately duplicate a face). */
				int identical = 1;

				for (int k = 0; k < n; k++)
				{
					const struct NativeTrackVert *a = &s_bpVerts[matchedFace * 4 + assign[k]];
					const struct NativeTrackVert *b = &s_bpVerts[candFaces[f] * 4 + tryAssign[k]];

					if (a->u != b->u || a->v != b->v || a->r != b->r || a->g != b->g || a->b != b->b || a->faceFlags != b->faceFlags)
					{
						identical = 0;
						break;
					}
				}

				if (!identical)
				{
					s_bpStat.ambig++;
					return;
				}

				continue;
			}

			matched = 1;
			matchedFace = candFaces[f];
			memcpy(assign, tryAssign, sizeof(assign));
		}
	}

	if (!matched)
	{
		s_bpStat.noFace++;

		if ((s_bpStat.noFace % 25000) == 1)
		{
			struct BpGeoExemplar *ex = &s_bpExNoFace[s_bpExNoFaceSeen % BP_EXEMPLARS];

			memset(ex, 0, sizeof(*ex));
			ex->n = (u8)n;

			for (int k = 0; k < n; k++)
			{
				ex->mv[k][0] = mv[k][0];
				ex->mv[k][1] = mv[k][1];
				ex->mv[k][2] = mv[k][2];
			}

			s_bpExNoFaceSeen++;
		}

		return;
	}

	if (n == 3)
	{
		s_bpStat.matchedTri++;
	}
	else
	{
		s_bpStat.matchedQuad++;
	}

	const struct NativeTrackVert *fv = &s_bpVerts[matchedFace * 4];

	/* ---- Empirical face-table map (see struct BpModeMap's comment) ---- */
	{
		u8 rowA[4];
		u8 faceMode = (u8)((fv[0].faceFlags >> 1) & 3);
		u8 rotA = 8;

		for (int j = 0; j < 4; j++)
		{
			rowA[j] = (u8)((fv[j].faceFlags >> 3) & 3);
		}

		for (int r = 0; r < 8; r++)
		{
			if (rowA[0] == sTrackUvPerm[r][0] && rowA[1] == sTrackUvPerm[r][1] && rowA[2] == sTrackUvPerm[r][2] && rowA[3] == sTrackUvPerm[r][3])
			{
				rotA = (u8)r;
				break;
			}
		}

		if (rotA < 8 && faceMode < 3)
		{
			struct BpModeMap *map = &s_bpModeMap[(faceMode << 3) | rotA][(n == 4) ? 1 : 0];
			u8 grid[4] = {0, 0, 0, 0};
			u8 idx[4] = {0, 0, 0, 0};
			int known = 1;

			for (int k = 0; k < n; k++)
			{
				grid[k] = assign[k];
				idx[k] = 0xE;

				for (int m = 0; m < 4; m++)
				{
					if (fv[m].u == pu[k] && fv[m].v == pv[k])
					{
						idx[k] = rowA[m];
						break;
					}
				}

				if (idx[k] == 0xE)
				{
					known = 0;
				}
			}

			if (known)
			{
				if (!map->valid)
				{
					map->valid = 1;
					map->n = (u8)n;
					memcpy(map->grid, grid, 4);
					memcpy(map->idx, idx, 4);
					map->agree = 1;
				}
				else
				{
					int same = (map->n == (u8)n);

					for (int k = 0; same && k < n; k++)
					{
						if (map->grid[k] != grid[k] || map->idx[k] != idx[k])
						{
							same = 0;
						}
					}

					if (same)
					{
						map->agree++;
					}
					else
					{
						map->disagree++;
					}
				}
			}
		}
	}

	/* ---- UV diff ---- */
	int uvExact = 1;

	for (int k = 0; k < n; k++)
	{
		if (fv[assign[k]].u != pu[k] || fv[assign[k]].v != pv[k])
		{
			uvExact = 0;
			break;
		}
	}

	if (uvExact)
	{
		s_bpStat.uvExact++;

		if (n == 4)
		{
			/* Row bookkeeping so "rot X: 100% exact" is a statement with a
			 * denominator, not an absence of complaints. */
			u8 rowA[4];

			for (int j = 0; j < 4; j++)
			{
				rowA[j] = (u8)((fv[j].faceFlags >> 3) & 3);
			}

			for (int r = 0; r < 8; r++)
			{
				if (rowA[0] == sTrackUvPerm[r][0] && rowA[1] == sTrackUvPerm[r][1] && rowA[2] == sTrackUvPerm[r][2] && rowA[3] == sTrackUvPerm[r][3])
				{
					s_bpStat.rotFaces[r]++;
					s_bpStat.rotExact[r]++;
					break;
				}
			}
		}
	}
	else if (n == 4)
	{
		/* Mismatch. If every packet UV equals SOME corner's baked UV, the
		 * face's texture content is right but the corner ORDER is wrong -- a
		 * permutation error, attributable to one sTrackUvPerm row. rowT is
		 * reconstructed in layout-index space: baked corner m carries layout
		 * index A_m in its faceFlags, so packet corner k matching baked
		 * corner m's UV means retail assigned layout index A_m there. */
		u8 rowA[4], rowT[4];
		int noCorner = 0, dup = 0;

		for (int j = 0; j < 4; j++)
		{
			rowA[j] = (u8)((fv[j].faceFlags >> 3) & 3);
		}

		for (int k = 0; k < n; k++)
		{
			int found = -1;

			for (int m = 0; m < 4; m++)
			{
				if (fv[m].u == pu[k] && fv[m].v == pv[k])
				{
					if (found >= 0 && rowA[m] != rowA[found])
					{
						dup = 1;
					}

					if (found < 0)
					{
						found = m;
					}
				}
			}

			if (found < 0)
			{
				noCorner = 1;
				break;
			}

			rowT[assign[k]] = rowA[found];
		}

		if (noCorner)
		{
			/* Animated-texture faces land here by construction: retail is
			 * sampling a different frame's layout, whose UVs are not any
			 * corner of the frame the bake stored. LOD-variant switches are
			 * the other resident class -- split by UV box area so "the bake
			 * is sharper than retail here" (benign) and "retail is sharper
			 * than the bake" (visual downgrade) stop sharing a bucket. */
			s_bpStat.uvNoCorner++;

			{
				int pMinU = 255, pMaxU = 0, pMinV = 255, pMaxV = 0;
				int bMinU = 255, bMaxU = 0, bMinV = 255, bMaxV = 0;

				for (int k = 0; k < n; k++)
				{
					const struct NativeTrackVert *v = &fv[assign[k]];

					pMinU = (pu[k] < pMinU) ? pu[k] : pMinU;
					pMaxU = (pu[k] > pMaxU) ? pu[k] : pMaxU;
					pMinV = (pv[k] < pMinV) ? pv[k] : pMinV;
					pMaxV = (pv[k] > pMaxV) ? pv[k] : pMaxV;
					bMinU = (v->u < bMinU) ? v->u : bMinU;
					bMaxU = (v->u > bMaxU) ? v->u : bMaxU;
					bMinV = (v->v < bMinV) ? v->v : bMinV;
					bMaxV = (v->v > bMaxV) ? v->v : bMaxV;
				}

				{
					long pArea = (long)(pMaxU - pMinU) * (pMaxV - pMinV);
					long bArea = (long)(bMaxU - bMinU) * (bMaxV - bMinV);

					if (bArea > pArea)
					{
						s_bpStat.ncBakBigger++;
					}
					else if (pArea > bArea)
					{
						s_bpStat.ncPktBigger++;
					}
					else
					{
						s_bpStat.ncSameSize++;
					}
				}
			}

			if (s_bpExNoCornerN < BP_EXEMPLARS)
			{
				BakeParity_SaveExemplar(&s_bpExNoCorner[s_bpExNoCornerN++], matchedFace, n, assign, pu, pv, pr, pg, pb, 8, 8);
			}
		}
		else if (dup)
		{
			s_bpStat.uvDup++;
		}
		else
		{
			u8 rotA = 8, rotT = 8;

			for (int r = 0; r < 8; r++)
			{
				if (rowA[0] == sTrackUvPerm[r][0] && rowA[1] == sTrackUvPerm[r][1] && rowA[2] == sTrackUvPerm[r][2] && rowA[3] == sTrackUvPerm[r][3])
				{
					rotA = (u8)r;
				}

				if (rowT[0] == sTrackUvPerm[r][0] && rowT[1] == sTrackUvPerm[r][1] && rowT[2] == sTrackUvPerm[r][2] && rowT[3] == sTrackUvPerm[r][3])
				{
					rotT = (u8)r;
				}
			}

			s_bpStat.uvPerm++;

			if (rotA < 8)
			{
				s_bpStat.rotFaces[rotA]++;
				s_bpStat.permHist[rotA][rotT]++;
			}

			if (s_bpExPermN < BP_EXEMPLARS)
			{
				BakeParity_SaveExemplar(&s_bpExPerm[s_bpExPermN++], matchedFace, n, assign, pu, pv, pr, pg, pb, rotA, rotT);
			}
		}
	}
	else
	{
		/* Tri packets carry too few corners to reconstruct a full perm row;
		 * they still count as mismatches so the exact rate stays honest. */
		s_bpStat.uvNoCorner++;
	}

	/* ---- Color diff (gouraud packets only; flat packets replicate r0) ---- */
	if (hasVertColors)
	{
		int worst = 0; /* 0 exactHi, 1 exactLo, 2 between, 3 other */

		s_bpStat.colChecked++;

		for (int k = 0; k < n; k++)
		{
			const struct NativeTrackVert *v = &fv[assign[k]];
			const struct BpSrcVert *sv = &s_bpSrc[matchedFace * 4 + assign[k]];
			int cls;

			if (pr[k] == v->r && pg[k] == v->g && pb[k] == v->b)
			{
				cls = 0;
			}
			else if (sv->valid && pr[k] == sv->lr && pg[k] == sv->lg && pb[k] == sv->lb)
			{
				cls = 1;
			}
			else if (sv->valid && pr[k] >= (v->r < sv->lr ? v->r : sv->lr) && pr[k] <= (v->r > sv->lr ? v->r : sv->lr) &&
			         pg[k] >= (v->g < sv->lg ? v->g : sv->lg) && pg[k] <= (v->g > sv->lg ? v->g : sv->lg) &&
			         pb[k] >= (v->b < sv->lb ? v->b : sv->lb) && pb[k] <= (v->b > sv->lb ? v->b : sv->lb))
			{
				cls = 2;
			}
			else
			{
				cls = 3;
			}

			if (cls > worst)
			{
				worst = cls;
			}
		}

		if (worst == 0)
		{
			s_bpStat.colHi++;
		}
		else if (worst == 1)
		{
			s_bpStat.colLo++;
		}
		else if (worst == 2)
		{
			s_bpStat.colBetween++;
		}
		else
		{
			s_bpStat.colOther++;

			if (s_bpExColOtherN < BP_EXEMPLARS)
			{
				BakeParity_SaveExemplar(&s_bpExColOther[s_bpExColOtherN++], matchedFace, n, assign, pu, pv, pr, pg, pb, 8, 8);
			}
		}
	}
}

static void BakeParity_LogExemplar(const char *tag, const struct BpExemplar *ex)
{
	Platform_Log("[BAKEPAR] ex %s face=%u bsp=%u rotA=%s rotT=%s pktUV=(%u,%u)(%u,%u)(%u,%u)(%u,%u) bakUV=(%u,%u)(%u,%u)(%u,%u)(%u,%u) "
	             "pktRGB0=(%u,%u,%u) bakRGB0=(%u,%u,%u)\n",
	             tag, ex->face, ex->leafBsp, s_bpRotName[ex->rotA], s_bpRotName[ex->rotT], ex->pktU[0], ex->pktV[0], ex->pktU[1], ex->pktV[1], ex->pktU[2],
	             ex->pktV[2], ex->pktU[3], ex->pktV[3], ex->bakU[0], ex->bakV[0], ex->bakU[1], ex->bakV[1], ex->bakU[2], ex->bakV[2], ex->bakU[3],
	             ex->bakV[3], ex->pktR[0], ex->pktG[0], ex->pktB[0], ex->bakR[0], ex->bakG[0], ex->bakB[0]);
}

static void BakeParity_Report(void)
{
	Platform_Log("[BAKEPAR] walks=%llu polys=%llu lkMiss=%llu noCand=%llu noFace=%llu ambig=%llu degen=%llu matched tri=%llu quad=%llu\n",
	             (unsigned long long)s_bpStat.walks, (unsigned long long)s_bpStat.polys, (unsigned long long)s_bpStat.lkMiss,
	             (unsigned long long)s_bpStat.noCand, (unsigned long long)s_bpStat.noFace, (unsigned long long)s_bpStat.ambig,
	             (unsigned long long)s_bpStat.degen, (unsigned long long)s_bpStat.matchedTri, (unsigned long long)s_bpStat.matchedQuad);
	Platform_Log("[BAKEPAR] uv exact=%llu perm=%llu nocorner=%llu dup=%llu | col hi=%llu lo=%llu between=%llu other=%llu of %llu\n",
	             (unsigned long long)s_bpStat.uvExact, (unsigned long long)s_bpStat.uvPerm, (unsigned long long)s_bpStat.uvNoCorner,
	             (unsigned long long)s_bpStat.uvDup, (unsigned long long)s_bpStat.colHi, (unsigned long long)s_bpStat.colLo,
	             (unsigned long long)s_bpStat.colBetween, (unsigned long long)s_bpStat.colOther, (unsigned long long)s_bpStat.colChecked);
	Platform_Log("[BAKEPAR] nocorner split: bakeSharper=%llu retailSharper=%llu sameSize=%llu\n", (unsigned long long)s_bpStat.ncBakBigger,
	             (unsigned long long)s_bpStat.ncPktBigger, (unsigned long long)s_bpStat.ncSameSize);

	for (u32 i = 0; i < s_bpExNoFaceSeen && i < BP_EXEMPLARS; i++)
	{
		const struct BpGeoExemplar *ex = &s_bpExNoFace[i];

		Platform_Log("[BAKEPAR] ex noface n=%u mv=(%d,%d,%d)(%d,%d,%d)(%d,%d,%d)(%d,%d,%d)\n", ex->n, ex->mv[0][0], ex->mv[0][1], ex->mv[0][2],
		             ex->mv[1][0], ex->mv[1][1], ex->mv[1][2], ex->mv[2][0], ex->mv[2][1], ex->mv[2][2], ex->mv[3][0], ex->mv[3][1], ex->mv[3][2]);
	}

	for (int r = 0; r < 8; r++)
	{
		if (s_bpStat.rotFaces[r] == 0)
		{
			continue;
		}

		char line[256];
		int len = snprintf(line, sizeof(line), "[BAKEPAR] perm %s: faces=%llu exact=%llu", s_bpRotName[r], (unsigned long long)s_bpStat.rotFaces[r],
		                   (unsigned long long)s_bpStat.rotExact[r]);

		for (int t = 0; t < 9; t++)
		{
			if (s_bpStat.permHist[r][t] > 0 && len > 0 && len < (int)sizeof(line))
			{
				len += snprintf(line + len, sizeof(line) - (size_t)len, " retail-says=%s:%llu", s_bpRotName[t], (unsigned long long)s_bpStat.permHist[r][t]);
			}
		}

		Platform_Log("%s\n", line);
	}

	/* Learned face-table map. Mode-0 quads with full agreement are the
	 * already-verified baseline -- print only what carries information:
	 * every mode!=0 entry, every tri-shape entry, and anything disagreeing. */
	for (int v = 0; v < 24; v++)
	{
		for (int shape = 0; shape < 2; shape++)
		{
			const struct BpModeMap *map = &s_bpModeMap[v][shape];

			if (!map->valid)
			{
				continue;
			}

			if ((v >> 3) == 0 && shape == 1 && map->disagree == 0)
			{
				continue;
			}

			Platform_Log("[BAKEPAR] facemap v=%d rot=%s mode=%d %s n=%u map=(g%u,i%u)(g%u,i%u)(g%u,i%u)(g%u,i%u) agree=%u disagree=%u\n", v,
			             s_bpRotName[v & 7], v >> 3, shape ? "quad" : "tri", map->n, map->grid[0], map->idx[0], map->grid[1], map->idx[1], map->grid[2],
			             map->idx[2], map->grid[3], map->idx[3], map->agree, map->disagree);
		}
	}

	for (u32 i = 0; i < s_bpExPermN; i++)
	{
		BakeParity_LogExemplar("perm", &s_bpExPerm[i]);
	}

	for (u32 i = 0; i < s_bpExNoCornerN; i++)
	{
		BakeParity_LogExemplar("nocorner", &s_bpExNoCorner[i]);
	}

	for (u32 i = 0; i < s_bpExColOtherN; i++)
	{
		BakeParity_LogExemplar("colother", &s_bpExColOther[i]);
	}
}

void BakeParity_BeginWalk(void)
{
	if (!NativeTrackMesh_IsBuilt())
	{
		s_bpIndexed = 0;
		return;
	}

	if (!s_bpIndexed || NativeTrackMesh_Verts() != s_bpVerts || NativeTrackMesh_VertCount() != s_bpVertCount)
	{
		BakeParity_Reindex();
	}

	s_bpStat.walks++;

	if ((s_bpStat.walks % BP_REPORT_WALKS) == 0 && (s_bpStat.matchedTri + s_bpStat.matchedQuad) > 0)
	{
		BakeParity_Report();
	}
}

void BakeParity_NotePolyFT3(const void *p)
{
	const POLY_FT3 *poly = (const POLY_FT3 *)p;
	const s16 sx[4] = {poly->x0, poly->x1, poly->x2, 0};
	const s16 sy[4] = {poly->y0, poly->y1, poly->y2, 0};
	const u8 pu[4] = {poly->u0, poly->u1, poly->u2, 0};
	const u8 pv[4] = {poly->v0, poly->v1, poly->v2, 0};
	const u8 pr[4] = {poly->r0, poly->r0, poly->r0, 0};
	const u8 pg[4] = {poly->g0, poly->g0, poly->g0, 0};
	const u8 pb[4] = {poly->b0, poly->b0, poly->b0, 0};

	BakeParity_NotePoly(3, sx, sy, pu, pv, pr, pg, pb, 0);
}

void BakeParity_NotePolyFT4(const void *p)
{
	const POLY_FT4 *poly = (const POLY_FT4 *)p;
	const s16 sx[4] = {poly->x0, poly->x1, poly->x2, poly->x3};
	const s16 sy[4] = {poly->y0, poly->y1, poly->y2, poly->y3};
	const u8 pu[4] = {poly->u0, poly->u1, poly->u2, poly->u3};
	const u8 pv[4] = {poly->v0, poly->v1, poly->v2, poly->v3};
	const u8 pr[4] = {poly->r0, poly->r0, poly->r0, poly->r0};
	const u8 pg[4] = {poly->g0, poly->g0, poly->g0, poly->g0};
	const u8 pb[4] = {poly->b0, poly->b0, poly->b0, poly->b0};

	BakeParity_NotePoly(4, sx, sy, pu, pv, pr, pg, pb, 0);
}

void BakeParity_NotePolyGT3(const void *p)
{
	const POLY_GT3 *poly = (const POLY_GT3 *)p;
	const s16 sx[4] = {poly->x0, poly->x1, poly->x2, 0};
	const s16 sy[4] = {poly->y0, poly->y1, poly->y2, 0};
	const u8 pu[4] = {poly->u0, poly->u1, poly->u2, 0};
	const u8 pv[4] = {poly->v0, poly->v1, poly->v2, 0};
	const u8 pr[4] = {poly->r0, poly->r1, poly->r2, 0};
	const u8 pg[4] = {poly->g0, poly->g1, poly->g2, 0};
	const u8 pb[4] = {poly->b0, poly->b1, poly->b2, 0};

	BakeParity_NotePoly(3, sx, sy, pu, pv, pr, pg, pb, 1);
}

void BakeParity_NotePolyGT4(const void *p)
{
	const POLY_GT4 *poly = (const POLY_GT4 *)p;
	const s16 sx[4] = {poly->x0, poly->x1, poly->x2, poly->x3};
	const s16 sy[4] = {poly->y0, poly->y1, poly->y2, poly->y3};
	const u8 pu[4] = {poly->u0, poly->u1, poly->u2, poly->u3};
	const u8 pv[4] = {poly->v0, poly->v1, poly->v2, poly->v3};
	const u8 pr[4] = {poly->r0, poly->r1, poly->r2, poly->r3};
	const u8 pg[4] = {poly->g0, poly->g1, poly->g2, poly->g3};
	const u8 pb[4] = {poly->b0, poly->b1, poly->b2, poly->b3};

	BakeParity_NotePoly(4, sx, sy, pu, pv, pr, pg, pb, 1);
}

#endif /* CTR_NATIVE_BAKE_PARITY */
