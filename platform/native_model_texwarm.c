/*
 * native_model_texwarm.c -- one-shot texture pre-warm at the level-load
 * boundary, __3DS__ only.
 *
 * The crate-intro cutscene's kart-spawn frame pays every kart/driver texture's
 * decode+upload synchronously on the draw path the first time it is sampled
 * (hardware-measured [TEXDEC] mdlUs=44622 in one frame, plus cold [TIREMISS]
 * misses on the tire pages). A model's struct TextureLayout table is a
 * load-time property: tpage/clut/UV corners are baked per header and never
 * change between instances or frames (native_model_mesh.c's own comment on
 * why its topology scan can run once). That means every (tpage, clut, format,
 * view[, tile]) key N3DS_ResolveTriangleTexture will eventually ask
 * N3DS_FindOrDecodeTexture / N3DS_FindOrDecodeSubPageTexture for is already
 * computable by walking the model's command list once, before anything is
 * drawn -- and warming through those SAME functions with the SAME derived
 * key (same tile-split test, same sampV0) guarantees the first real draw is a
 * cache hit rather than a hopeful pre-decode of some other combination.
 *
 * A face that ends up routed through a runtime-only view override this scan
 * cannot see (ghost composite, forceSemiTrans, ABE-by-side specular) still
 * benefits: its page/clut is warmed under the default view, so only the
 * override's own cell is a genuine miss, not the whole page.
 *
 * Also warms gGT->iconGroup[0], the level-shared tire animation sprite set
 * every driver's wheelSprites points at (game/Vehicle/VehBirth.c) -- its
 * per-icon struct TextureLayout is the same shape as a model's, so it reuses
 * the identical key derivation and decode calls.
 *
 * Counts and decodes only -- draws nothing, allocates nothing beyond its own
 * fixed dedupe tables.
 */

#include <platform/native_log.h>

#if defined(__3DS__)

/* Command-list walk cap, same reasoning as native_model_mesh.c's: a header
 * whose list doesn't reach 0xffffffff inside this many steps is corrupt data,
 * not content -- bail rather than run into the heap. */
#define TEXWARM_MAX_COMMANDS   65536u
/* Texture-table index is 9 bits (native_model_mesh.c's own comment); track it
 * in a full bitset per header so a busy header can't alias two distinct
 * layout entries into "already seen". */
#define TEXWARM_TEXINDEX_WORDS 16
/* Budget: one-shot on the load screen, capped so a pathological level cannot
 * turn the warm into its own hitch. */
#define TEXWARM_MAX_KEYS   256
#define TEXWARM_MAX_MODELS 256

int g_dbg_texWarm = 1;

struct NativeTexWarmKey
{
	s16 tpage;
	s16 clut;
	u8 format;
	u8 view;
	s8 tileX; /* -1 = full-page tier, no subpage split */
	s8 tileY;
};

static struct NativeTexWarmKey s_texwarmKeys[TEXWARM_MAX_KEYS];
static unsigned s_texwarmKeyCount;
static unsigned s_texwarmPages;
static unsigned s_texwarmSubpages;
static unsigned s_texwarmSkipped;
static unsigned s_texwarmTireKeys;

static const struct Model *s_texwarmSeenModels[TEXWARM_MAX_MODELS];
static unsigned s_texwarmSeenCount;

static int NativeTexWarm_MarkModelSeen(const struct Model *model)
{
	unsigned i;

	for (i = 0; i < s_texwarmSeenCount; i++)
	{
		if (s_texwarmSeenModels[i] == model)
		{
			return 0;
		}
	}

	if (s_texwarmSeenCount < TEXWARM_MAX_MODELS)
	{
		s_texwarmSeenModels[s_texwarmSeenCount++] = model;
	}

	return 1;
}

static int NativeTexWarm_KeySeen(s16 tpage, s16 clut, u8 format, u8 view, s8 tileX, s8 tileY)
{
	unsigned i;

	for (i = 0; i < s_texwarmKeyCount; i++)
	{
		const struct NativeTexWarmKey *k = &s_texwarmKeys[i];

		if ((k->tpage == tpage) && (k->clut == clut) && (k->format == format) && (k->view == view) && (k->tileX == tileX) &&
		    (k->tileY == tileY))
		{
			return 1;
		}
	}

	return 0;
}

/*
 * Derives and warms exactly the key(s) N3DS_ResolveTriangleTexture would
 * compute for a face using this layout: minU/maxU/minV/maxV from the three UV
 * corners a model triangle actually takes (NativeModelEmit_Triangle's own
 * comment -- corner i goes to vertex i, no strip-parity swap), the same
 * tileX0==tileX1 && tileY0==tileY1 subpage-routing test gated by the same
 * g_dbg_disableSubPage, and sampV0 = minV (the face's own minimum V, not the
 * page anchor -- see N3DS_FindOrDecodeTexture's own comment on why anchor-
 * keying is wrong for content that doesn't live at the page origin).
 *
 * View selection mirrors NativeModelDraw_FaceBlend's page-only default (ABR 3
 * = opaque, else semi-transparent PASS1+PASS2): the only branch this load-time
 * scan can evaluate without a live instance.
 */
static void NativeTexWarm_WarmLayout(const struct TextureLayout *layout)
{
	TexFormat format;
	int minU, maxU, minV, maxV;
	int tileX0, tileX1, tileY0, tileY1;
	int abr;
	N3DS_TexView views[2];
	int viewCount, vi;

	if (layout == NULL)
	{
		return;
	}

	format = GetTPageFormat((int)(s16)layout->tpage);

	minU = maxU = layout->u0;
	minV = maxV = layout->v0;

	if (layout->u1 < minU) { minU = layout->u1; }
	if (layout->u1 > maxU) { maxU = layout->u1; }
	if (layout->v1 < minV) { minV = layout->v1; }
	if (layout->v1 > maxV) { maxV = layout->v1; }
	if (layout->u2 < minU) { minU = layout->u2; }
	if (layout->u2 > maxU) { maxU = layout->u2; }
	if (layout->v2 < minV) { minV = layout->v2; }
	if (layout->v2 > maxV) { maxV = layout->v2; }

	tileX0 = minU / N3DS_SUBPAGE_DIM;
	tileX1 = maxU / N3DS_SUBPAGE_DIM;
	tileY0 = minV / N3DS_SUBPAGE_DIM;
	tileY1 = maxV / N3DS_SUBPAGE_DIM;

	abr = ((int)layout->tpage >> 5) & 3;

	if (abr == 3)
	{
		views[0] = N3DS_TEXVIEW_OPAQUE;
		viewCount = 1;
	}
	else
	{
		views[0] = N3DS_TEXVIEW_PASS1;
		views[1] = N3DS_TEXVIEW_PASS2;
		viewCount = 2;
	}

	for (vi = 0; vi < viewCount; vi++)
	{
		const int useSubPage = !g_dbg_disableSubPage && (tileX0 == tileX1) && (tileY0 == tileY1);
		const s8 keyTileX = useSubPage ? (s8)tileX0 : -1;
		const s8 keyTileY = useSubPage ? (s8)tileY0 : -1;

		if (s_texwarmKeyCount >= TEXWARM_MAX_KEYS)
		{
			s_texwarmSkipped++;
			continue;
		}

		if (NativeTexWarm_KeySeen((s16)layout->tpage, (s16)layout->clut, (u8)format, (u8)views[vi], keyTileX, keyTileY))
		{
			continue;
		}

		s_texwarmKeys[s_texwarmKeyCount].tpage = (s16)layout->tpage;
		s_texwarmKeys[s_texwarmKeyCount].clut = (s16)layout->clut;
		s_texwarmKeys[s_texwarmKeyCount].format = (u8)format;
		s_texwarmKeys[s_texwarmKeyCount].view = (u8)views[vi];
		s_texwarmKeys[s_texwarmKeyCount].tileX = keyTileX;
		s_texwarmKeys[s_texwarmKeyCount].tileY = keyTileY;
		s_texwarmKeyCount++;

		if (useSubPage)
		{
			N3DS_FindOrDecodeSubPageTexture((s16)layout->tpage, (s16)layout->clut, format, views[vi], tileX0, tileY0, minV);
			s_texwarmSubpages++;
		}
		else
		{
			N3DS_FindOrDecodeTexture((s16)layout->tpage, (s16)layout->clut, format, views[vi], minV);
			s_texwarmPages++;
		}
	}
}

/*
 * Command-list walk, deliberately kept parallel to
 * native_model_mesh.c:NativeModelMesh_Walk (same stream, same strip
 * bookkeeping) rather than sharing it: that scan is gated on
 * CTR_NATIVE_MODEL_RENDERER (a retained-geometry feature question) and this
 * one must run whenever the game is on 3DS at all, because the packet path
 * resolves through the very same decode cache this warms.
 */
static void NativeTexWarm_WalkHeader(const struct ModelHeader *mh)
{
	const u32 *cmd;
	u32 texIndexSeen[TEXWARM_TEXINDEX_WORDS];
	unsigned stripLength = 0;
	unsigned steps;

	if ((mh == NULL) || (mh->ptrCommandList == NULL) || (mh->ptrTexLayout == NULL))
	{
		return;
	}

	memset(texIndexSeen, 0, sizeof(texIndexSeen));
	cmd = (const u32 *)mh->ptrCommandList;
	cmd++;

	for (steps = 0; steps < TEXWARM_MAX_COMMANDS; steps++)
	{
		u32 command = *cmd++;
		u32 flags;
		u16 texIndex;

		if (command == 0xffffffffu)
		{
			return;
		}

		if ((command >> 16) == 0)
		{
			continue;
		}

		flags = (command >> 24) & 0xff;

		if ((flags & 0x80) != 0)
		{
			stripLength = 0;
		}

		if (stripLength < 2)
		{
			stripLength++;
			continue;
		}

		stripLength++;

		texIndex = (u16)(command & 0x1ff);

		if (texIndex == 0)
		{
			continue;
		}

		if ((texIndexSeen[texIndex >> 5] & (1u << (texIndex & 31))) != 0)
		{
			continue;
		}

		texIndexSeen[texIndex >> 5] |= 1u << (texIndex & 31);

		if (s_texwarmKeyCount >= TEXWARM_MAX_KEYS)
		{
			return;
		}

		NativeTexWarm_WarmLayout(mh->ptrTexLayout[texIndex - 1]);
	}
}

static void NativeTexWarm_WalkModel(const struct Model *model)
{
	s16 h;

	if ((model == NULL) || (model->headers == NULL) || (model->numHeaders <= 0) || (model->numHeaders > 64))
	{
		return;
	}

	for (h = 0; h < model->numHeaders; h++)
	{
		if (s_texwarmKeyCount >= TEXWARM_MAX_KEYS)
		{
			return;
		}

		NativeTexWarm_WalkHeader(&model->headers[h]);
	}
}

/*
 * Tire animation sprites (game/Vehicle/VehBirth.c:VehBirth_TireSprites) come
 * from gGT->iconGroup[0], one shared IconGroup per level -- every driver's
 * struct Driver::wheelSprites points at the SAME 17-entry array regardless of
 * kart colour or driverID, so warming it once per level covers every kart
 * that will spawn. Each struct Icon carries a struct TextureLayout exactly
 * like a model's texture table, so the same key-derivation and decode calls
 * apply unchanged.
 */
static void NativeTexWarm_WalkIconGroup(struct IconGroup *ig)
{
	struct Icon **icons;
	s16 i;

	if ((ig == NULL) || (ig->numIcons <= 0) || (ig->numIcons > 256))
	{
		return;
	}

	icons = ICONGROUP_GETICONS(ig);

	for (i = 0; (i < ig->numIcons) && (s_texwarmKeyCount < TEXWARM_MAX_KEYS); i++)
	{
		unsigned before;

		if (icons[i] == NULL)
		{
			continue;
		}

		before = s_texwarmKeyCount;
		NativeTexWarm_WarmLayout(&icons[i]->texLayout);
		s_texwarmTireKeys += s_texwarmKeyCount - before;
	}
}

/*
 * Level-change latch, same shape and same reasoning as
 * native_model_census.c's: the Level allocation is reused across loads, so a
 * pointer-only latch would report the first level of a session and go quiet
 * after; the fingerprint is the level's own content, not its ID (the ID
 * changes a frame before the Level struct is repopulated).
 */
struct NativeTexWarmFingerprint
{
	const void *instDefs;
	const void *modelArray;
	u32 numInstances;
	u32 numModels;
};

static const struct Level *s_texwarmLastLevel;
static struct NativeTexWarmFingerprint s_texwarmLastPrint;
static int s_texwarmHasRun;

void NativeModelTexWarm_OnLevelLoad(struct GameTracker *gGT, struct Level *level)
{
	struct NativeTexWarmFingerprint print;
	unsigned m;
	unsigned long long t0, freq, elapsed;

	if (!g_dbg_texWarm || (gGT == NULL) || (level == NULL))
	{
		return;
	}

	print.instDefs = (const void *)level->ptrInstDefs;
	print.modelArray = (const void *)level->ptrModelsPtrArray;
	print.numInstances = level->numInstances;
	print.numModels = level->numModels;

	if (s_texwarmHasRun && (level == s_texwarmLastLevel) && (print.instDefs == s_texwarmLastPrint.instDefs) &&
	    (print.modelArray == s_texwarmLastPrint.modelArray) && (print.numInstances == s_texwarmLastPrint.numInstances) &&
	    (print.numModels == s_texwarmLastPrint.numModels))
	{
		return;
	}

	s_texwarmHasRun = 1;
	s_texwarmLastLevel = level;
	s_texwarmLastPrint = print;

	s_texwarmKeyCount = 0;
	s_texwarmPages = 0;
	s_texwarmSubpages = 0;
	s_texwarmSkipped = 0;
	s_texwarmSeenCount = 0;
	s_texwarmTireKeys = 0;

	t0 = N3DS_GetTickCount();
	freq = N3DS_GetTickFrequency();

	/* Two populations, same split native_model_census.c's own comment
	 * explains: level->ptrModelsPtrArray is track scenery, karts/drivers/
	 * weapons live in the global library (gGT->modelPtr[]). Both feed the
	 * crate-intro's cast. */
	if ((level->ptrModelsPtrArray != NULL) && (level->numModels > 0) && (level->numModels <= 8192))
	{
		for (m = 0; (m < level->numModels) && (s_texwarmKeyCount < TEXWARM_MAX_KEYS); m++)
		{
			const struct Model *model = level->ptrModelsPtrArray[m];

			if ((model != NULL) && NativeTexWarm_MarkModelSeen(model))
			{
				NativeTexWarm_WalkModel(model);
			}
		}
	}

	for (m = 0; (m < (unsigned)(sizeof(gGT->modelPtr) / sizeof(gGT->modelPtr[0]))) && (s_texwarmKeyCount < TEXWARM_MAX_KEYS); m++)
	{
		const struct Model *model = gGT->modelPtr[m];

		if ((model != NULL) && NativeTexWarm_MarkModelSeen(model))
		{
			NativeTexWarm_WalkModel(model);
		}
	}

	/* Tire sprites (game/Vehicle/VehBirth.c:VehBirth_TireSprites) come from
	 * gGT->iconGroup[0], a level-shared asset, not a per-model texture table --
	 * warm it separately through the same key derivation and decode calls. */
	NativeTexWarm_WalkIconGroup(gGT->iconGroup[0]);

	elapsed = N3DS_GetTickCount() - t0;
	Platform_LogInstrument("[TEXWARM] f=%lu pages=%u subpages=%u tires=%u us=%lu skipped=%u\n", (unsigned long)s_frameCounter, s_texwarmPages,
	                       s_texwarmSubpages, s_texwarmTireKeys, (unsigned long)((elapsed * 1000000ULL) / (freq ? freq : 1)), s_texwarmSkipped);
}

#else

int g_dbg_texWarm = 0;

void NativeModelTexWarm_OnLevelLoad(struct GameTracker *gGT, struct Level *level)
{
	(void)gGT;
	(void)level;
}

#endif
