#include <common.h>

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80031c1c-0x80031c58.
void LOAD_StringToUpper(char *path)
{
	for (u8 *letter = (u8 *)path; *letter != 0; letter++)
	{
		// if lowercase letter
		if ((u32)(*letter - 0x61) < 0x1a)
		{
			// uppercase
			*letter -= 0x20;
		}
	}
}

#ifdef CTR_NATIVE
#include <platform/native_cd.h>
#include <platform/native_audio.h>

// Resumable state for the boot crate-intro level-file prefetch (VRAM then LEV)
// into the native CD read cache. The crate group has no PTR entry (BI_NDBOX +
// LVI_PTR aliases the next group), so only these two extents.
internal struct
{
	int groupIndex; /* 0 = VRAM, 1 = LEV, 2 = done */
} s_lfCratePrefetch;

// Begin/step form of the crate-intro prefetch: call once per frame with a
// per-call sector budget (0 = uncapped) until it returns nonzero. Served from
// RAM, the mid-load disc/SD reads for these files cost nothing once cached,
// instead of stalling the per-vblank music sequencer.
int LOAD_NativePrefetchCrateIntroStep(struct BigHeader *bigfile, int budgetSectors)
{
	struct BigEntry *entry;
	int group[2];

	if (bigfile == NULL)
	{
		return 1;
	}
	if (s_lfCratePrefetch.groupIndex >= 2)
	{
		return 1;
	}

	entry = BIG_GETENTRY(bigfile);
	group[0] = LOAD_GetBigfileIndex(NAUGHTY_DOG_CRATE, LOAD_LEVEL_LOD_1P, LVI_VRAM);
	group[1] = LOAD_GetBigfileIndex(NAUGHTY_DOG_CRATE, LOAD_LEVEL_LOD_1P, LVI_LEV);

	while (s_lfCratePrefetch.groupIndex < 2)
	{
		int idx = group[s_lfCratePrefetch.groupIndex];

		if ((idx < 0) || (idx >= bigfile->numEntry))
		{
			s_lfCratePrefetch.groupIndex++;
			continue;
		}

		if (!NativeCD_PrefetchExtentBegin(bigfile->cdpos + entry[idx].offset, entry[idx].size))
		{
			// already cached, unavailable, or PC: nothing to pump for this extent
			s_lfCratePrefetch.groupIndex++;
			continue;
		}

		if (!NativeCD_PrefetchPump(budgetSectors))
		{
			return 0; // more of this extent remains
		}

		s_lfCratePrefetch.groupIndex++;
	}

	return 1;
}

// Orchestrates the two resumable boot-splash disc jobs for the crate intro:
// the XA voice-cache warm (MUSIC memo + GAME/EXTRA RAM caches) must finish
// before the CD-cache prefetch starts, so the crate intro's own voice lines
// never contend with its level-file reads for the same SD bandwidth. Held at
// the stage-6 call site (see LOAD_TenStages) until both report done.
//
// A slow SD card that can't finish inside a reasonable number of held frames
// forces the rest through synchronously here (bounded, logged) rather than
// letting the crate intro's audio start against a half-built cache -- same
// blocking cost the un-split code always paid, just as a last resort instead
// of the default path.
int LOAD_NativeCrateIntroSplashStep(struct BigHeader *bigfile)
{
	enum
	{
		LOAD_CRATE_SPLASH_SECTORS_PER_FRAME = 64,
		LOAD_CRATE_SPLASH_FALLBACK_FRAMES = 2400, // ~40s at 60fps: generous even for the slow-card target
	};

	local_persist u32 s_lfCrateSplashFrames;

	NativeAudio_VoiceCacheEnsureBegin();
	s_lfCrateSplashFrames++;

	if (s_lfCrateSplashFrames > LOAD_CRATE_SPLASH_FALLBACK_FRAMES)
	{
		Platform_Log("[SPLASHIO] fallback: forcing synchronous completion after %u held frames\n", s_lfCrateSplashFrames);
		while (!NativeAudio_VoiceCacheEnsureStep(0))
		{
		}
		while (!LOAD_NativePrefetchCrateIntroStep(bigfile, 0))
		{
		}
		s_lfCrateSplashFrames = 0;
		return 1;
	}

	if (!NativeAudio_VoiceCacheEnsureStep(LOAD_CRATE_SPLASH_SECTORS_PER_FRAME))
	{
		return 0;
	}
	if (!LOAD_NativePrefetchCrateIntroStep(bigfile, LOAD_CRATE_SPLASH_SECTORS_PER_FRAME))
	{
		return 0;
	}

	s_lfCrateSplashFrames = 0;
	return 1;
}

// Prefetch all nine Oxide-intro cutscene scenes (VRAM/LEV/PTR per scene,
// contiguous in BIGFILE) into the CD read cache as one spanning extent, so
// every scene-switch read — and the adventure-mode replay of the same intro —
// is served from RAM and the SD card stays exclusive to the intro's XA stream.
// Called once per frame during the scene-1 load; each call reads a bounded
// chunk so the per-vblank music sequencer keeps ticking, and the pump must
// finish before the level files are queued (the extent serves no reads until
// complete). Returns nonzero when there is nothing left to pump (finished,
// already cached, unavailable, or PC).
int LOAD_NativeIntroCutscenePrefetchStep(struct BigHeader *bigfile)
{
	// 64 sectors (128KB) per pumped frame keeps each SD read well under a
	// frame on the slow-card target while still finishing the ~4.4MB intro
	// span in under a second of load screen.
	enum
	{
		LOAD_INTRO_PREFETCH_SECTORS_PER_FRAME = 64
	};

	struct BigEntry *entry;
	int firstIndex;
	int lastIndex;
	int spanBytes;

	if (bigfile == NULL)
	{
		return 1;
	}

	firstIndex = LOAD_GetBigfileIndex(INTRO_RACE_TODAY, LOAD_LEVEL_LOD_1P, LVI_VRAM);
	lastIndex = LOAD_GetBigfileIndex(INTRO_OXIDE, LOAD_LEVEL_LOD_1P, LVI_PTR);

	if ((firstIndex < 0) || (lastIndex < firstIndex) || (lastIndex >= bigfile->numEntry))
	{
		return 1;
	}

	entry = BIG_GETENTRY(bigfile);
	spanBytes = (entry[lastIndex].offset - entry[firstIndex].offset) * LOAD_CD_DATA_SECTOR_SIZE + entry[lastIndex].size;

	if (!NativeCD_PrefetchExtentBegin(bigfile->cdpos + entry[firstIndex].offset, spanBytes))
	{
		return 1;
	}

	return NativeCD_PrefetchPump(LOAD_INTRO_PREFETCH_SECTORS_PER_FRAME);
}
#endif

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x8007c118-0x8007c208.
int LOAD_InitCDvol(void)
{
#ifndef CTR_NATIVE
	if ((SPU_CURRENT_VOL_L == 0) && (SPU_CURRENT_VOL_R == 0))
	{
		SPU_MASTER_VOL_L = 0x3fff;
		SPU_MASTER_VOL_R = 0x3fff;
	}

	SPU_CD_VOL_L = 0x3fff;
	SPU_CD_VOL_R = 0x3fff;
	SPU_CTRL = 0xc001;

	CD_REG(0) = 2;
	CD_REG(2) = 0x80;
	CD_REG(3) = 0;
	CD_REG(0) = 3;
	CD_REG(1) = 0x80;
	CD_REG(2) = 0;
	CD_REG(3) = 0x20;
#else
	// NOTE(aalhendi): Retail CdInit calls this hook to reset SPU volume
	// state before howl_InitGlobals re-applies game defaults. Native has
	// no CdInit path (CDSYS_Init(0) skips it), so mirror the SPU CD
	// volume write through the native SPU API to keep parity with the
	// retail init ordering. Master volume defaults are already handled
	// by NativeAudio_SpuInit (0x3fff), matching retail's "only if zero"
	// guard.
	SpuSetCommonCDVolume(0x3fff, 0x3fff);
#endif

	return 0;
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80031c58-0x80031c78 for the retail path.
void LOAD_InitCD()
{
#ifdef CTR_NATIVE
	NativeCD_Init();
	CDSYS_Init(0);
	// NOTE(aalhendi): Retail chains LOAD_InitCD -> CDSYS_Init(1) -> CdInit
	// -> LOAD_InitCDvol. Native skips CdInit (no disc), so call the volume
	// hook explicitly to preserve the same init ordering.
	LOAD_InitCDvol();
	return;
#endif

	CDSYS_Init(1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80031c78-0x80031d30
void *LOAD_ReadDirectory(char *filename)
{
	CdlFILE cdlFile;
	u8 buf[8];

	CDSYS_SetMode_StreamData();

	if (CdSearchFile(&cdlFile, filename) == NULL)
	{
		return NULL;
	}

	struct BigHeader *bh = MEMPACK_AllocMem(LOAD_BIGFILE_HEADER_ALLOC_BYTES /*, filename*/);

	// Search for file on disc
	// Set Cd laser to file position
	// Read the bigfile header
	// Wait for read to end
	CdControl(CdlSetloc, (u8 *)&cdlFile, buf);
	if (CdRead(LOAD_BIGFILE_HEADER_SECTORS, (u32 *)bh, CdlModeSpeed) == 0)
	{
		return NULL;
	}

	if (CdReadSync(0, 0) != 0)
	{
		return NULL;
	}

	// Save position
	bh->cdpos = CdPosToInt(&cdlFile.pos);

	// undo header allocation, only use "needed" size
	MEMPACK_ReallocMem(sizeof(struct BigHeader) + sizeof(struct BigEntry) * bh->numEntry);

	sdata->ptrBigfileCdPos_2 = bh;
	return bh;
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 PS1 path 0x80031d30-0x80031e00.
void LOAD_DramFileCallback(struct LoadQueueSlot *lqs)
{
	char *fileBuf = lqs->ptrDestination;
	void (*callback)(struct LoadQueueSlot *) = lqs->callbackFuncPtr;

	if (fileBuf != NULL)
	{
		int ptrMapOffset = *(int *)&fileBuf[0];
		char *realFileBuf = &fileBuf[4];

		if (ptrMapOffset >= 0)
		{
			struct DramPointerMap *dpm = (struct DramPointerMap *)&realFileBuf[ptrMapOffset];

			LOAD_RunPtrMap(realFileBuf, (int *)DRAM_GETOFFSETS(dpm), dpm->numBytes >> 2);

#if defined(CTR_NATIVE)
			if ((lqs->flags & LT_MEMPACK) != 0)
#else
			if ((lqs->flags & LT_SETADDR) != 0)
#endif
			{
				MEMPACK_ReallocMem(ptrMapOffset + 4);
			}
		}
		else
		{
			lqs->flags |= LT_GETADDR;
		}

		lqs->ptrDestination = &fileBuf[4];
	}

#if defined(CTR_NATIVE)
	// NOTE(aalhendi): CTR_NATIVE keeps host callback pointers and queue sentinels.
	if ((callback != NULL) && (callback != LOAD_DramFileCallback) && (callback != (void (*)(struct LoadQueueSlot *))-1) &&
	    (callback != LOAD_QUEUE_CALLBACK_SET_POINTER))
#else
	if ((callback != NULL) && (((u32)(uintptr_t)callback & 0xff000000) == 0x80000000))
#endif
	{
		callback(lqs);
	}

	sdata->queueReady = 1;
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80031e00-0x80031ee4.
void *LOAD_DramFile(void *bigfilePtr, int subfileIndex, void *ptrDestination, u32 *sizePtr, int callbackOrFlags)
{
	struct LoadQueueSlot lqs;
	void *loadedFile;

	if (callbackOrFlags == -1)
	{
		loadedFile = LOAD_ReadFile_ex(bigfilePtr, LT_GETADDR, subfileIndex, ptrDestination, sizePtr, NULL);

		lqs.ptrBigfileCdPos_UNUSED = bigfilePtr;
		lqs.flags = 0;
		lqs.type_UNUSED = LT_DRAM;
		lqs.subfileIndex = subfileIndex;
		lqs.ptrDestination = loadedFile;
		lqs.size_UNUSED = *sizePtr;
		lqs.callbackFuncPtr = NULL;

		LOAD_DramFileCallback(&lqs);

		return loadedFile;
	}

	if (callbackOrFlags == -2)
	{
		loadedFile = LOAD_ReadFile_ex(bigfilePtr, LT_GETADDR, subfileIndex, NULL, sizePtr, LOAD_DramFileCallback);
		data.currSlot.ptrDestination = loadedFile;
		*(void **)ptrDestination = loadedFile;
		return loadedFile;
	}

	return LOAD_ReadFile_ex(bigfilePtr, LT_GETADDR, subfileIndex, ptrDestination, sizePtr, LOAD_DramFileCallback);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80031ee4-0x80031fdc.
void LOAD_VramFileCallback(struct LoadQueueSlot *lqs)
{
	int *vramBuf = lqs->ptrDestination;

	struct VramHeader *vh = (struct VramHeader *)vramBuf;

	// if just one TIM
	if ((vramBuf != NULL) && (vramBuf[0] != 0x20))
	{
		LoadImage(&vh->rect, VRAMHEADER_GETPIXLES(vh));
	}

	// if multiple TIMs are packed together
	if ((vramBuf != NULL) && (vramBuf[0] == 0x20))
	{
		int size;
		vramBuf++;

		size = vramBuf[0];
		vh = (struct VramHeader *)&vramBuf[1];

		while (size != 0)
		{
			LoadImage(&vh->rect, VRAMHEADER_GETPIXLES(vh));

			// goto next
			vramBuf = (int *)((u8 *)vh + size);

			size = vramBuf[0];
			vh = (struct VramHeader *)&vramBuf[1];
		}
	}

	// LOAD_NextQueuedFile waits 3 vsync frames before releasing the queue.
	sdata->frameFinishedVRAM = sdata->gGT->frameTimer_VsyncCallback;
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80031fdc-0x80032110.
void *LOAD_VramFile(void *bigfilePtr, int subfileIndex, void *ptrDestination, u32 *sizePtr, int callbackOrFlags)
{
	struct LoadQueueSlot lqs;
	void *loadedFile;

	if (ptrDestination == NULL)
	{
		MEMPACK_PushState();
	}

	if (callbackOrFlags == -1)
	{
		loadedFile = LOAD_ReadFile_ex(bigfilePtr, LT_VRAM, subfileIndex, ptrDestination, sizePtr, NULL);

#ifdef CTR_NATIVE
		// DIAGNOSTIC (2026-07-21, session 14): synchronous-load breadcrumbs for
		// the real-hardware first-boot stall; each candidate wait in this rare
		// (-1) path gets its own line. See LOAD_TenStages.c.
		Platform_Log("[CTR LOAD] VramFile: read done, loading images\n");
#endif

		lqs.ptrBigfileCdPos_UNUSED = bigfilePtr;
		lqs.flags = 0;
		lqs.type_UNUSED = LT_VRAM;
		lqs.subfileIndex = subfileIndex;
		lqs.ptrDestination = loadedFile;
		lqs.size_UNUSED = *sizePtr;
		lqs.callbackFuncPtr = NULL;

		LOAD_VramFileCallback(&lqs);

#ifdef CTR_NATIVE
		Platform_Log("[CTR LOAD] VramFile: images loaded, VSync(2)\n");
#endif
		VSync(2);
#ifdef CTR_NATIVE
		Platform_Log("[CTR LOAD] VramFile: VSync(2) done\n");
#endif
		sdata->frameFinishedVRAM = 0;

		if (ptrDestination == NULL)
		{
			MEMPACK_PopState();
		}

		return loadedFile;
	}

	if (callbackOrFlags == -2)
	{
		loadedFile = LOAD_ReadFile_ex(bigfilePtr, LT_VRAM, subfileIndex, NULL, sizePtr, LOAD_VramFileCallback);
		data.currSlot.ptrDestination = loadedFile;
		*(void **)ptrDestination = loadedFile;
		return loadedFile;
	}

	return LOAD_ReadFile_ex(bigfilePtr, LT_VRAM, subfileIndex, ptrDestination, sizePtr, LOAD_VramFileCallback);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80032110-0x800321b4.
void LOAD_ReadFileASyncCallback(u8 result, u8 *unk)
{
	(void)unk;
	CdReadCallback(0);
	result &= 0xff;

	struct LoadQueueSlot *lqs = &data.currSlot;

	if (result == CdlComplete)
	{
#if defined(CTR_NATIVE)
		if ((lqs->flags & LT_MEMPACK) != 0)
#else
		if ((lqs->flags & LT_SETADDR) != 0)
#endif
		{
			MEMPACK_ReallocMem(lqs->size_UNUSED);
		}

		if (sdata->callbackCdReadSuccess != NULL)
		{
			sdata->callbackCdReadSuccess(lqs);
		}
	}

	// CdlDiskError
	else
	{
#if defined(CTR_NATIVE)
		if ((lqs->flags & LT_MEMPACK) != 0)
#else
		if ((lqs->flags & LT_SETADDR) != 0)
#endif
		{
			// undo allocation, try again
			MEMPACK_ReallocMem(0);
		}

		sdata->queueRetry = 1;
	}
}

void *LOAD_ReadFile_ex(struct BigHeader *bigfile, u32 loadType, int subfileIndex, void *ptrDst, u32 *sizePtr, void (*callback)(struct LoadQueueSlot *))
{
	int uVar5;
	CdlLOC cdLoc;
	u8 paramOutput[8];
	void *originalDst;
	int sectorSize;
	int sectorCount;
	int readComplete;

	// NOTE(aalhendi): ASM-verified NTSC-U 926 PS1 path 0x800321b4-0x80032344.
	(void)loadType;
	CDSYS_SetMode_StreamData();

#if defined(CTR_NATIVE)
	// NOTE(aalhendi): CTR_NATIVE preserves existing queues that pass 0 for the
	// default bigfile; retail callers are expected to pass the real pointer.
	if (bigfile == NULL)
	{
		bigfile = sdata->ptrBigfile1;
	}
#endif

	// get size and offset of subfile
	struct BigEntry *entry = BIG_GETENTRY(bigfile);
	int eSize = entry[subfileIndex].size;
	int eOffs = entry[subfileIndex].offset;

	*sizePtr = eSize;

	CdIntToPos(bigfile->cdpos + eOffs, &cdLoc);

	struct LoadQueueSlot *lqs = &data.currSlot;
	originalDst = ptrDst;
	sectorCount = (eSize + LOAD_CD_DATA_SECTOR_ROUND_MASK) >> LOAD_CD_DATA_SECTOR_SHIFT;
	readComplete = 1;

	// If no address given, then find one.
	if (ptrDst == NULL)
	{
#if defined(CTR_NATIVE)
		lqs->flags |= LT_MEMPACK;
#else
		lqs->flags |= LT_SETADDR;
#endif

		// allocate room for all sectors,
		// remove alignment before next Read
		sectorSize = sectorCount << LOAD_CD_DATA_SECTOR_SHIFT;
		ptrDst = (void *)MEMPACK_AllocMem(sectorSize); // "FILE"
		if (ptrDst == NULL)
		{
			return NULL;
		}
	}
	else
	{
#if defined(CTR_NATIVE)
		lqs->flags &= ~LT_MEMPACK;
#else
		lqs->flags &= ~LT_SETADDR;
#endif
	}

#if defined(CTR_NATIVE)
	// NOTE(aalhendi): native CD reads can call back before wrapper callers store
	// the returned pointer back into data.currSlot.
	lqs->ptrDestination = ptrDst;
	lqs->size_UNUSED = eSize;
#endif

	while (1)
	{
		uVar5 = CdControl(CdlSetloc, (u8 *)&cdLoc, &paramOutput[0]);

		if (callback != NULL)
		{
			sdata->callbackCdReadSuccess = callback;
			CdReadCallback(LOAD_ReadFileASyncCallback);
		}
		else
		{
			sdata->callbackCdReadSuccess = NULL;
			CdReadCallback(NULL);
		}

		uVar5 &= CdRead(sectorCount, ptrDst, CdlModeSpeed);

		if (callback == NULL)
		{
			// Wait for all sectors to finish
			readComplete = CdReadSync(0, (u8 *)0x0) < 1;
		}

		// If either command failed, or sync read did not finish, retry.
		if ((uVar5 != 0) && (readComplete != 0))
		{
			break;
		}
	}

	if ((callback == NULL) && (originalDst == NULL))
	{
		MEMPACK_ReallocMem(*sizePtr);
	}

	return ptrDst;
}

// Used for XNF and only the XNF
// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80032344-0x80032438
void *LOAD_XnfFile(char *filename, void *ptrDestination, int *size)
{
	CdlFILE cdlFile;

	LOAD_StringToUpper(filename);
	CDSYS_SetMode_StreamData();

	if (CdSearchFile(&cdlFile, filename) == 0)
	{
		return 0;
	}

	*size = cdlFile.size;

	int allocated = ptrDestination == NULL;
	if (allocated)
	{
		// allocate room for all sectors,
		// remove alignment before next Read
		int sectorSize = (cdlFile.size + LOAD_CD_DATA_SECTOR_ROUND_MASK) & LOAD_CD_DATA_SECTOR_ALIGN_MASK;
		ptrDestination = MEMPACK_AllocMem(sectorSize /*, fileName*/);
		if (ptrDestination == NULL)
		{
			return NULL;
		}
	}

	u8 buf[8];
	CdControl(CdlSetloc, (u8 *)&cdlFile, buf);

	if (CdRead((cdlFile.size + LOAD_CD_DATA_SECTOR_ROUND_MASK) >> LOAD_CD_DATA_SECTOR_SHIFT, ptrDestination, CdlModeSpeed) == 0)
	{
		return 0;
	}

	if (CdReadSync(0, 0))
	{
		return 0;
	}

	if (allocated)
	{
		MEMPACK_ReallocMem(cdlFile.size);
	}

	return ptrDestination;
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80032438-0x80032498
int LOAD_FindFile(char *filename, CdlFILE *cdlFile)
{
	if (filename == 0)
	{
		return 0;
	}
	if (cdlFile == 0)
	{
		return 0;
	}

	CDSYS_SetMode_StreamData();
	LOAD_StringToUpper(filename);

	return (CdSearchFile(cdlFile, filename) != 0);
}
