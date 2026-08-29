/* Native options menu and main-menu row filter.
 *
 * game/230/MM_MenuFlow.c and game/MAIN/MainMain.c call the three NativeOptions_*
 * hooks at the bottom of this file. On PC every hook is a passthrough/no-op --
 * the retail main menu must stay untouched there (replay inputs navigate menus
 * by position, so PC row layout is behavioral surface). On 3DS the main menu
 * loses VS and BATTLE (one screen, one pad -- both modes are dead ends) and
 * gains an OPTIONS row opening a RectMenu of runtime toggles; each row writes
 * the same global its dumpreq word writes, so the menu and dumpreq stay
 * interchangeable.
 *
 * Row labels live in retail's own string table: LNG_UNUSED_0E4..0E8 are real
 * slots inside the loaded LANG range that no retail code references, repointed
 * once at boot (LOAD_LangFile runs exactly once on this NTSC-U tree) and on
 * every toggle. s_optMenu is not checkpoint-tracked (native_checkpoint.c
 * relocates only its enumerated regions); acceptable because the PC build, the
 * only one with F5/F8 checkpoints, never links this menu into the hierarchy.
 */

#ifdef __3DS__

extern int g_pentaPal;      /* platform/native_gpu.c, dumpreq `pentapal` */
extern int g_reservesMeter; /* platform/native_gpu.c, dumpreq `reserves` */
extern int g_dbg_btmMap;    /* platform/n3ds/native_renderer_3ds.c, dumpreq `btmmap` */
#if defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
/* Both live in the n3ds_backend TU; the flag is plain int and the call is
 * void(void), so they cross the scalar boundary without header help. */
extern int g_dbg_trackBilinear; /* dumpreq `bilinear` */
void N3DS_TrackAtlas_ApplyFilter(void);
#endif

struct NativeOptToggle
{
	s16 lngSlot;
	int *flag;
	const char *labelOff;
	const char *labelOn;
	void (*applyFn)(void); /* extra work a flip needs beyond the flag write */
};

/* The box is re-measured from the live strings every frame and DecalFont is
 * monospace (every plain character, space included, advances font_charPixWidth;
 * only button glyphs and ':'/'.' differ) -- so each OFF/ON pair MUST have the
 * same character count or the box resizes on toggle. The ON legs carry an
 * internal pad space, which also right-aligns the state word. Keep labels short
 * enough (small font) that the box clears the screen edge at the main menu's
 * right-of-center anchor. */
static struct NativeOptToggle s_optToggles[] = {
    {LNG_UNUSED_0E5, &g_pentaPal, "PAL PENTA OFF", "PAL PENTA  ON", NULL},
    {LNG_UNUSED_0E6, &g_reservesMeter, "RESERVES METER OFF", "RESERVES METER  ON", NULL},
    {LNG_UNUSED_0E7, &g_dbg_btmMap, "BOTTOM MAP OFF", "BOTTOM MAP  ON", NULL},
#if defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
    {LNG_UNUSED_0E8, &g_dbg_trackBilinear, "TEXTURE FILTER OFF", "TEXTURE FILTER  ON", N3DS_TrackAtlas_ApplyFilter},
#endif
};
#define OPT_TOGGLE_COUNT ((s32)(sizeof(s_optToggles) / sizeof(s_optToggles[0])))

/* Nav targets must mirror s_optToggles row for row; the terminator is not a
 * valid nav target, so the last real row's down points at itself. */
static struct MenuRow s_optMenuRows[] = {
    {LNG_UNUSED_0E5, 0, 1, 0, 0},
#if defined(CTR_NATIVE_TRACK_RENDERER) && CTR_NATIVE_TRACK_RENDERER
    {LNG_UNUSED_0E6, 0, 2, 1, 1},
    {LNG_UNUSED_0E7, 1, 3, 2, 2},
    {LNG_UNUSED_0E8, 2, 3, 3, 3},
#else
    {LNG_UNUSED_0E6, 0, 2, 1, 1},
    {LNG_UNUSED_0E7, 1, 2, 2, 2},
#endif
    {RECTMENU_STRING_NONE, 0, 0, 0, 0},
};

static void NativeOptions_RefreshLabels(void)
{
	for (s32 i = 0; i < OPT_TOGGLE_COUNT; i++)
	{
		sdata->lngStrings[s_optToggles[i].lngSlot] = (char *)(*s_optToggles[i].flag ? s_optToggles[i].labelOn : s_optToggles[i].labelOff);
	}
}

static void NativeOptions_MenuProc(struct RectMenu *menu)
{
	/* The engine also invokes funcPtr outside input events; only a real
	 * Cross/Triangle event (funcState INPUT) may toggle, or a held frame
	 * would flip the row every vblank. */
	if (menu->funcState != RECTMENU_FUNC_STATE_INPUT)
	{
		return;
	}

	s16 row = menu->rowSelected;

	/* rowSelected -1 is the engine's back/close signal */
	if (row == -1)
	{
		menu->ptrPrevBox_InHierarchy->state &= ~(ONLY_DRAW_TITLE | DRAW_NEXT_MENU_IN_HIERARCHY);
		return;
	}

	if ((row >= 0) && (row < OPT_TOGGLE_COUNT))
	{
		*s_optToggles[row].flag ^= 1;

		if (s_optToggles[row].applyFn != NULL)
		{
			s_optToggles[row].applyFn();
		}

		NativeOptions_RefreshLabels();
	}
}

/* No own title: the parent main menu already renders "OPTIONS" above the child
 * box (retail parents draw their selected row while ONLY_DRAW_TITLE). */
static struct RectMenu s_optMenu = {
    .stringIndexTitle = RECTMENU_STRING_NONE,
    .state = CENTER_ON_X | USE_SMALL_FONT,
    .rows = &s_optMenuRows[0],
    .funcPtr = NativeOptions_MenuProc,
};

/* 3DS main-menu row sets: retail minus VS/BATTLE, plus OPTIONS last. */
static struct MenuRow s_optMainRowsBasic[] = {
    {LNG_ADVENTURE, 0, 1, 0, 0},
    {LNG_TIME_TRIAL, 0, 2, 1, 1},
    {LNG_ARCADE, 1, 3, 2, 2},
    {LNG_HIGH_SCORE, 2, 4, 3, 3},
    {LNG_UNUSED_0E4, 3, 4, 4, 4},
    {RECTMENU_STRING_NONE, 0, 0, 0, 0},
};

static struct MenuRow s_optMainRowsScrapbook[] = {
    {LNG_ADVENTURE, 0, 1, 0, 0},
    {LNG_TIME_TRIAL, 0, 2, 1, 1},
    {LNG_ARCADE, 1, 3, 2, 2},
    {LNG_HIGH_SCORE, 2, 4, 3, 3},
    {LNG_SCRAPBOOK, 3, 5, 4, 4},
    {LNG_UNUSED_0E4, 4, 5, 5, 5},
    {RECTMENU_STRING_NONE, 0, 0, 0, 0},
};

#endif /* __3DS__ */

/* Called every frame by MM_MenuProc_Main after retail's own scrapbook swap, so
 * `rows` is retail's choice for this frame; returning it unchanged keeps PC
 * retail-identical. */
struct MenuRow *NativeOptions_MainMenuRows(struct MenuRow *rows)
{
#ifdef __3DS__
	NativeOptions_RefreshLabels();

	if (rows == &D230.rowsMainMenuWithScrapbook[0])
	{
		return &s_optMainRowsScrapbook[0];
	}

	return &s_optMainRowsBasic[0];
#else
	return rows;
#endif
}

s32 NativeOptions_HandleMainMenuChoice(struct RectMenu *mainMenu, s32 choose)
{
#ifdef __3DS__
	if (choose == LNG_UNUSED_0E4)
	{
		mainMenu->ptrNextBox_InHierarchy = &s_optMenu;
		mainMenu->state |= DRAW_NEXT_MENU_IN_HIERARCHY;
		return 1;
	}
#else
	(void)mainMenu;
	(void)choose;
#endif
	return 0;
}

void NativeOptions_InstallStrings(void)
{
#ifdef __3DS__
	sdata->lngStrings[LNG_UNUSED_0E4] = (char *)"OPTIONS";
	NativeOptions_RefreshLabels();
#endif
}
