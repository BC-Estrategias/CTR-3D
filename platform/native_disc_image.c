#include "platform/native_disc_image.h"

#include <platform/native_path.h>

#include <limits.h>
#if !defined(_WIN32)
#include <dirent.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NATIVE_DISC_IMAGE_PATH_MAX          1024
#define NATIVE_DISC_IMAGE_BIN_PATH          "ctr-u.bin"
#define NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET 24u
#define NATIVE_DISC_IMAGE_FORM1_DATA_SIZE   2048u
#define NATIVE_DISC_IMAGE_MODE2_USER_OFFSET 16u
#define NATIVE_DISC_IMAGE_MODE2_USER_SIZE   2336u
#define NATIVE_DISC_IMAGE_PVD_LBA           16u
#define NATIVE_DISC_IMAGE_PVD_ROOT_RECORD   156u
#define NATIVE_DISC_IMAGE_DIRECTORY_FLAG    0x02u

// NOTE(aalhendi): This hardcodes only the common NTSC-U raw BIN layout:
// one MODE2/2352 data track at byte zero. The user still supplies all disc
// contents; native only uses this sector contract to read the image.

struct NativeDiscImageDirRecord
{
	u32 lba;
	u32 size;
	u8 flags;
	u8 nameLen;
	const u8 *name;
};

global_variable char s_nativeDiscImagePath[NATIVE_DISC_IMAGE_PATH_MAX];
global_variable FILE *s_nativeDiscImageFile;
global_variable struct NativeDiscImageFile s_nativeDiscImageRoot;
global_variable int s_nativeDiscImageAvailable;

/*
 * Scratch for every main-thread read path (level loads via NativeCD, STR,
 * whole-file reads). The audio worker owns its own -- see the header. If a
 * new disc reader ever appears on a third thread it needs its own scratch
 * too; there is no lock here and deliberately so, the fix for SD contention
 * is fewer reads, not serialized ones.
 */
global_variable struct NativeDiscImageScratch s_nativeDiscImageMainScratch = {NATIVE_DISC_IMAGE_OWNER_MAIN, {0}};

#if CTR_NATIVE_DISC_IO_PROFILE
/*
 * Per-owner disc-read counters. Each bucket has exactly ONE writer thread
 * (that is what `owner` means), so the increments need no atomics; only the
 * reporter reads across threads, and a torn window count is acceptable in a
 * diagnostic that exists to size an effect, not to gate correctness.
 *
 * `headerFails` is the interesting one beyond the timings: the image FILE*
 * and its file position are shared by both threads, and fseek+fread is not
 * atomic across the pair, so an interleaved read can land on the other
 * thread's position. That shows up here as a failed sector-header check
 * rather than as corruption, and nothing counted it before.
 */
struct NativeDiscImageIoProf
{
	u32 calls;
	u32 sectors;
	u32 runsOf1;
	u32 maxRun;
	u32 headerFails;
	u32 ioFails;
	u64 ticks;
	u64 worstTicks;
};

global_variable struct NativeDiscImageIoProf s_nativeDiscIoProf[NATIVE_DISC_IMAGE_OWNER_COUNT];

/*
 * Directory-walk reads, counted apart from payload. A mean sectors-per-call
 * of 1.7 can be one big batched load hiding behind two thousand one-sector
 * metadata reads, or it can be a load path that genuinely asks for 1.7
 * sectors at a time -- and those two want completely different fixes, so the
 * mean alone is not a finding. FindFile re-walks the tree from the root on
 * every open with nothing cached, which makes this the suspect worth
 * separating.
 */
global_variable u32 s_nativeDiscIoDirCalls;
global_variable u32 s_nativeDiscIoDirSectors;

/*
 * Call-site tallies. 95% of reads being single-sector says the batching has
 * nothing to batch, but not WHY -- and the fix differs completely depending
 * on which caller is asking one sector at a time. These are bumped at the
 * four sites that can produce a single-sector read, so the counter names the
 * caller instead of leaving it to be inferred. Declared here because
 * native_disc_image.c is first in the unity include order (main.c), so
 * native_audio.c and native_cd.c can both reach them.
 */
global_variable u32 s_nativeDiscIoSiteCd;        /* NativeCD_ReadSectors -- the game's CdRead */
global_variable u32 s_nativeDiscIoSiteCdSectors; /* ...and the sector counts it asks for */
global_variable u32 s_nativeDiscIoSiteXaCount;   /* NativeAudio_XAStreamCountAudioSectors' header scan */
global_variable u32 s_nativeDiscIoSiteSingle;    /* the single-sector helper (PVD / root lookup) */

internal unsigned long long NativeDiscImage_ProfTick(void)
{
#if defined(__3DS__)
	return N3DS_GetTickCount();
#else
	return 0ull;
#endif
}

internal void NativeDiscImage_ProfAccumulate(struct NativeDiscImageIoProf *prof, unsigned long long beginTick)
{
	unsigned long long elapsed = NativeDiscImage_ProfTick() - beginTick;

	prof->ticks += elapsed;
	if (elapsed > prof->worstTicks)
	{
		prof->worstTicks = elapsed;
	}
}
#endif

internal int NativeDiscImage_FindHostImagePath(char *dst, size_t dstSize, NativeStr8 assetsDir)
{
#if defined(_WIN32)
	return NativePath_Join(dst, dstSize, assetsDir, NATIVE_STR8_LIT(NATIVE_DISC_IMAGE_BIN_PATH));
#else
	char dirPath[NATIVE_DISC_IMAGE_PATH_MAX];
	DIR *dir;
	struct dirent *entry;
	int found = 0;

	if (!NativePath_NormalizeSlashes(dirPath, sizeof(dirPath), assetsDir))
	{
		return 0;
	}

	dir = opendir(dirPath);
	if (dir == NULL)
	{
		return 0;
	}

	while ((entry = readdir(dir)) != NULL)
	{
		NativeStr8 entryName = NativeStr8_FromCString(entry->d_name);

		if (!NativeStr8_EqualsIgnoreCaseAscii(entryName, NATIVE_STR8_LIT(NATIVE_DISC_IMAGE_BIN_PATH)))
		{
			continue;
		}

		found = NativePath_Join(dst, dstSize, NativeStr8_FromCString(dirPath), entryName);
		break;
	}

	closedir(dir);
	return found;
#endif
}

internal u32 NativeDiscImage_ReadLE32(const u8 *data)
{
	return ((u32)data[0]) | ((u32)data[1] << 8) | ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

internal int NativeDiscImage_CheckRawSectorHeader(const u8 *sector)
{
	u32 i;

	if ((sector[0] != 0x00) || (sector[11] != 0x00) || (sector[15] != 0x02))
	{
		return 0;
	}

	for (i = 1; i < 11; i++)
	{
		if (sector[i] != 0xff)
		{
			return 0;
		}
	}

	return 1;
}

/*
 * THE choke point: every byte the game reads off the disc image arrives
 * through here, and each call is one seek plus one read. Before this took a
 * run, the caller-facing "bulk" reads were loops around a single-sector
 * version of it, so a 2000-sector level load cost 2000 SD round trips and
 * the XA pump's 32-sector span cost 32. Reading the whole run at once is the
 * entire point -- keep it that way; do not reintroduce a per-sector loop
 * above a per-sector read.
 */
internal int NativeDiscImage_ReadRawRun(u32 lba, u32 count, u8 *raw, int owner)
{
	u64 offset;
	size_t bytes;
	u32 i;
#if CTR_NATIVE_DISC_IO_PROFILE
	struct NativeDiscImageIoProf *prof = &s_nativeDiscIoProf[(owner == NATIVE_DISC_IMAGE_OWNER_AUDIO) ? NATIVE_DISC_IMAGE_OWNER_AUDIO : NATIVE_DISC_IMAGE_OWNER_MAIN];
	unsigned long long beginTick;
#else
	(void)owner;
#endif

	if ((s_nativeDiscImageFile == NULL) || (count == 0))
	{
		return 0;
	}

	offset = (u64)lba * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE;
	bytes = (size_t)count * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE;

#if CTR_NATIVE_DISC_IO_PROFILE
	prof->calls++;
	prof->sectors += count;
	if (count == 1u)
	{
		prof->runsOf1++;
	}
	if (count > prof->maxRun)
	{
		prof->maxRun = count;
	}
	beginTick = NativeDiscImage_ProfTick();
#endif

	if ((offset > (u64)LONG_MAX) || (fseek(s_nativeDiscImageFile, (long)offset, SEEK_SET) != 0) || (fread(raw, 1, bytes, s_nativeDiscImageFile) != bytes))
	{
#if CTR_NATIVE_DISC_IO_PROFILE
		prof->ioFails++;
		NativeDiscImage_ProfAccumulate(prof, beginTick);
#endif
		return 0;
	}

#if CTR_NATIVE_DISC_IO_PROFILE
	NativeDiscImage_ProfAccumulate(prof, beginTick);
#endif

	for (i = 0; i < count; i++)
	{
		if (!NativeDiscImage_CheckRawSectorHeader(&raw[(size_t)i * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE]))
		{
#if CTR_NATIVE_DISC_IO_PROFILE
			prof->headerFails++;
#endif
			return 0;
		}
	}

	return 1;
}

internal int NativeDiscImage_ReadRawSector(u32 lba, u8 *sector)
{
#if CTR_NATIVE_DISC_IO_PROFILE
	s_nativeDiscIoSiteSingle++;
#endif
	return NativeDiscImage_ReadRawRun(lba, 1, sector, NATIVE_DISC_IMAGE_OWNER_MAIN);
}

internal int NativeDiscImage_ParseDirRecord(const u8 *src, size_t available, struct NativeDiscImageDirRecord *record)
{
	u8 length;

	if ((src == NULL) || (record == NULL) || (available < 34))
	{
		return 0;
	}

	length = src[0];
	if ((length == 0) || (length > available) || (length < 34))
	{
		return 0;
	}

	record->lba = NativeDiscImage_ReadLE32(&src[2]);
	record->size = NativeDiscImage_ReadLE32(&src[10]);
	record->flags = src[25];
	record->nameLen = src[32];
	record->name = &src[33];

	if ((u32)record->nameLen + 33u > length)
	{
		return 0;
	}

	return 1;
}

internal int NativeDiscImage_NameEquals(const struct NativeDiscImageDirRecord *record, NativeStr8 component)
{
	size_t recordLen;
	size_t componentLen;
	size_t i;

	if ((record == NULL) || (record->name == NULL) || (component.ptr == NULL))
	{
		return 0;
	}

	recordLen = record->nameLen;
	componentLen = component.len;

	while ((recordLen > 0) && (record->name[recordLen - 1u] == ' '))
	{
		recordLen--;
	}

	if ((recordLen > 2) && (record->name[recordLen - 2u] == ';') && (record->name[recordLen - 1u] == '1'))
	{
		recordLen -= 2;
	}

	if ((componentLen > 2) && (component.ptr[componentLen - 2u] == ';') && (component.ptr[componentLen - 1u] == '1'))
	{
		componentLen -= 2;
	}

	if (recordLen != componentLen)
	{
		return 0;
	}

	for (i = 0; i < componentLen; i++)
	{
		if (NativeStr8_ToUpperAscii(record->name[i]) != NativeStr8_ToUpperAscii(component.ptr[i]))
		{
			return 0;
		}
	}

	return 1;
}

internal NativeStr8 NativeDiscImage_NextPathComponent(NativeStr8 *path)
{
	NativeStr8 result = *path;
	size_t i;

	while ((result.len != 0) && NativePath_IsSeparator(result.ptr[0]))
	{
		result = NativeStr8_Skip(result, 1);
	}

	for (i = 0; i < result.len; i++)
	{
		if (NativePath_IsSeparator(result.ptr[i]))
		{
			NativeStr8 component = {result.ptr, i};
			*path = NativeStr8_Skip(result, i + 1u);
			return component;
		}
	}

	*path = NativeStr8_Skip(result, result.len);
	return result;
}

internal u32 NativeDiscImage_DataSectorCount(u32 size)
{
	return (size + (NATIVE_DISC_IMAGE_FORM1_DATA_SIZE - 1u)) / NATIVE_DISC_IMAGE_FORM1_DATA_SIZE;
}

internal u32 NativeDiscImage_RawSectorCount(u32 size)
{
	if ((size != 0) && ((size % NATIVE_DISC_IMAGE_MODE2_USER_SIZE) == 0))
	{
		return size / NATIVE_DISC_IMAGE_MODE2_USER_SIZE;
	}

	return NativeDiscImage_DataSectorCount(size);
}

internal int NativeDiscImage_ReadDirectoryBytes(const struct NativeDiscImageFile *dir, u8 **dataOut, int *sizeOut)
{
	u32 sectorCount;
	u8 *data;

	*dataOut = NULL;
	*sizeOut = 0;

	if ((dir == NULL) || (dir->size == 0) || (dir->size > 0x7fffffff))
	{
		return 0;
	}

	sectorCount = NativeDiscImage_DataSectorCount(dir->size);
	if (sectorCount == 0)
	{
		return 0;
	}

	data = (u8 *)malloc((size_t)sectorCount * NATIVE_DISC_IMAGE_FORM1_DATA_SIZE);
	if (data == NULL)
	{
		return 0;
	}

	/* Batched like every other multi-sector read. This one is easy to write
	 * off as small -- a directory is a handful of sectors -- but FindFile
	 * re-walks the tree from the root on EVERY file open, one
	 * ReadDirectoryBytes per path component, and nothing caches the result.
	 * Main-thread only, like FindFile itself. */
#if CTR_NATIVE_DISC_IO_PROFILE
	s_nativeDiscIoDirCalls++;
	s_nativeDiscIoDirSectors += sectorCount;
#endif

	if (!NativeDiscImage_ReadDataSectorsScratch(dir, 0, sectorCount, data, &s_nativeDiscImageMainScratch))
	{
		free(data);
		return 0;
	}

	*dataOut = data;
	*sizeOut = (int)dir->size;
	return 1;
}

internal int NativeDiscImage_FindInDirectory(const struct NativeDiscImageFile *dir, NativeStr8 component, struct NativeDiscImageFile *fileOut, u8 *flagsOut)
{
	u8 *data;
	int size;
	int offset;
	int found = 0;

	if (!NativeDiscImage_ReadDirectoryBytes(dir, &data, &size))
	{
		return 0;
	}

	offset = 0;
	while (offset < size)
	{
		struct NativeDiscImageDirRecord record;
		u8 length = data[offset];

		if (length == 0)
		{
			offset = (offset + (int)NATIVE_DISC_IMAGE_FORM1_DATA_SIZE) & ~((int)NATIVE_DISC_IMAGE_FORM1_DATA_SIZE - 1);
			continue;
		}

		if ((length < 34) || (offset + length > size))
		{
			break;
		}

		if (NativeDiscImage_ParseDirRecord(&data[offset], (size_t)(size - offset), &record) && NativeDiscImage_NameEquals(&record, component))
		{
			fileOut->lba = record.lba;
			fileOut->size = record.size;
			*flagsOut = record.flags;
			found = 1;
			break;
		}

		offset += length;
	}

	free(data);
	return found;
}

internal int NativeDiscImage_LoadRoot(void)
{
	u8 sector[NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE];
	struct NativeDiscImageDirRecord root;

	if (!NativeDiscImage_ReadRawSector(NATIVE_DISC_IMAGE_PVD_LBA, sector))
	{
		return 0;
	}

	if ((memcmp(&sector[NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET + 1], "CD001", 5) != 0) || (sector[NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET] != 1) ||
	    (sector[NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET + 6] != 1))
	{
		return 0;
	}

	if (!NativeDiscImage_ParseDirRecord(&sector[NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET + NATIVE_DISC_IMAGE_PVD_ROOT_RECORD],
	                                    NATIVE_DISC_IMAGE_FORM1_DATA_SIZE - NATIVE_DISC_IMAGE_PVD_ROOT_RECORD, &root))
	{
		return 0;
	}

	s_nativeDiscImageRoot.lba = root.lba;
	s_nativeDiscImageRoot.size = root.size;
	return 1;
}

int NativeDiscImage_Init(const char *assetsDir)
{
	char path[NATIVE_DISC_IMAGE_PATH_MAX];

	s_nativeDiscImageAvailable = 0;
	s_nativeDiscImagePath[0] = '\0';

	if (s_nativeDiscImageFile != NULL)
	{
		fclose(s_nativeDiscImageFile);
		s_nativeDiscImageFile = NULL;
	}

	if ((assetsDir == NULL) || !NativeDiscImage_FindHostImagePath(path, sizeof(path), NativeStr8_FromCString(assetsDir)))
	{
		return 0;
	}

	s_nativeDiscImageFile = fopen(path, "rb");
	if (s_nativeDiscImageFile == NULL)
	{
		return 0;
	}

	if (!NativeDiscImage_LoadRoot())
	{
		fclose(s_nativeDiscImageFile);
		s_nativeDiscImageFile = NULL;
		return 0;
	}

	if (!NativePath_NormalizeSlashes(s_nativeDiscImagePath, sizeof(s_nativeDiscImagePath), NativeStr8_FromCString(path)))
	{
		fclose(s_nativeDiscImageFile);
		s_nativeDiscImageFile = NULL;
		return 0;
	}

	s_nativeDiscImageAvailable = 1;
	return 1;
}

int NativeDiscImage_FindFile(const char *path, struct NativeDiscImageFile *fileOut)
{
	NativeStr8 remaining = NativePath_SkipLeadingSeparators(NativeStr8_FromCString(path));
	struct NativeDiscImageFile current = s_nativeDiscImageRoot;
	u8 flags = NATIVE_DISC_IMAGE_DIRECTORY_FLAG;

	if (!s_nativeDiscImageAvailable || (path == NULL) || (fileOut == NULL))
	{
		return 0;
	}

	while (remaining.len != 0)
	{
		NativeStr8 component = NativeDiscImage_NextPathComponent(&remaining);

		if (component.len == 0)
		{
			continue;
		}

		if ((flags & NATIVE_DISC_IMAGE_DIRECTORY_FLAG) == 0)
		{
			return 0;
		}

		if (!NativeDiscImage_FindInDirectory(&current, component, &current, &flags))
		{
			return 0;
		}
	}

	if ((flags & NATIVE_DISC_IMAGE_DIRECTORY_FLAG) != 0)
	{
		return 0;
	}

	*fileOut = current;
	return 1;
}

internal int NativeDiscImage_ReadDataBytesScratch(const struct NativeDiscImageFile *file, u32 offset, void *dst, size_t size, struct NativeDiscImageScratch *scratch)
{
	u8 *out = (u8 *)dst;

	if ((file == NULL) || (dst == NULL) || (scratch == NULL) || ((u64)offset + size > file->size))
	{
		return 0;
	}

	while (size != 0)
	{
		u32 sectorIndex = offset / NATIVE_DISC_IMAGE_FORM1_DATA_SIZE;
		u32 sectorOffset = offset % NATIVE_DISC_IMAGE_FORM1_DATA_SIZE;
		u64 touched = ((u64)sectorOffset + size + NATIVE_DISC_IMAGE_FORM1_DATA_SIZE - 1u) / NATIVE_DISC_IMAGE_FORM1_DATA_SIZE;
		u32 run = (touched > NATIVE_DISC_IMAGE_CHUNK_SECTORS) ? NATIVE_DISC_IMAGE_CHUNK_SECTORS : (u32)touched;
		u32 i;

		if (!NativeDiscImage_ReadRawRun(file->lba + sectorIndex, run, scratch->raw, scratch->owner))
		{
			return 0;
		}

		for (i = 0; (i < run) && (size != 0); i++)
		{
			size_t copySize = NATIVE_DISC_IMAGE_FORM1_DATA_SIZE - sectorOffset;

			if (copySize > size)
			{
				copySize = size;
			}

			memcpy(out, &scratch->raw[((size_t)i * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE) + NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET + sectorOffset], copySize);
			out += copySize;
			offset += (u32)copySize;
			size -= copySize;
			sectorOffset = 0;
		}
	}

	return 1;
}

int NativeDiscImage_ReadDataSectorsScratch(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst, struct NativeDiscImageScratch *scratch)
{
	u8 *out = (u8 *)dst;
	u32 fileSectorCount;

	if ((file == NULL) || (dst == NULL) || (scratch == NULL))
	{
		return 0;
	}

	fileSectorCount = NativeDiscImage_DataSectorCount(file->size);
	if ((sector > fileSectorCount) || (sectorCount > fileSectorCount - sector))
	{
		return 0;
	}

	while (sectorCount != 0)
	{
		u32 run = (sectorCount > NATIVE_DISC_IMAGE_CHUNK_SECTORS) ? NATIVE_DISC_IMAGE_CHUNK_SECTORS : sectorCount;
		u32 i;

		if (!NativeDiscImage_ReadRawRun(file->lba + sector, run, scratch->raw, scratch->owner))
		{
			return 0;
		}

		for (i = 0; i < run; i++)
		{
			memcpy(&out[(size_t)i * NATIVE_DISC_IMAGE_FORM1_DATA_SIZE], &scratch->raw[((size_t)i * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE) + NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET],
			       NATIVE_DISC_IMAGE_FORM1_DATA_SIZE);
		}

		out += (size_t)run * NATIVE_DISC_IMAGE_FORM1_DATA_SIZE;
		sector += run;
		sectorCount -= run;
	}

	return 1;
}

int NativeDiscImage_ReadRawSectorsScratch(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst, struct NativeDiscImageScratch *scratch)
{
	u8 *out = (u8 *)dst;
	u32 fileSectorCount;

	if ((file == NULL) || (dst == NULL) || (scratch == NULL))
	{
		return 0;
	}

	fileSectorCount = NativeDiscImage_RawSectorCount(file->size);
	if ((sector > fileSectorCount) || (sectorCount > fileSectorCount - sector))
	{
		return 0;
	}

	while (sectorCount != 0)
	{
		u32 run = (sectorCount > NATIVE_DISC_IMAGE_CHUNK_SECTORS) ? NATIVE_DISC_IMAGE_CHUNK_SECTORS : sectorCount;
		u32 i;

		if (!NativeDiscImage_ReadRawRun(file->lba + sector, run, scratch->raw, scratch->owner))
		{
			return 0;
		}

		for (i = 0; i < run; i++)
		{
			memcpy(&out[(size_t)i * NATIVE_DISC_IMAGE_MODE2_USER_SIZE], &scratch->raw[((size_t)i * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE) + NATIVE_DISC_IMAGE_MODE2_USER_OFFSET],
			       NATIVE_DISC_IMAGE_MODE2_USER_SIZE);
		}

		out += (size_t)run * NATIVE_DISC_IMAGE_MODE2_USER_SIZE;
		sector += run;
		sectorCount -= run;
	}

	return 1;
}

int NativeDiscImage_ReadDataSectors(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst)
{
	return NativeDiscImage_ReadDataSectorsScratch(file, sector, sectorCount, dst, &s_nativeDiscImageMainScratch);
}

int NativeDiscImage_ReadRawSectors(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst)
{
	return NativeDiscImage_ReadRawSectorsScratch(file, sector, sectorCount, dst, &s_nativeDiscImageMainScratch);
}

int NativeDiscImage_ReadFileBytes(const char *path, int rawSectors, u8 **dataOut, int *sizeOut)
{
	struct NativeDiscImageFile file;
	u32 sectorCount;
	u32 size;
	u8 *data;

	*dataOut = NULL;
	*sizeOut = 0;

	if (!NativeDiscImage_FindFile(path, &file))
	{
		return 0;
	}

	if (rawSectors)
	{
		sectorCount = NativeDiscImage_RawSectorCount(file.size);
		size = sectorCount * NATIVE_DISC_IMAGE_MODE2_USER_SIZE;
	}
	else
	{
		sectorCount = NativeDiscImage_DataSectorCount(file.size);
		size = file.size;
	}

	if ((sectorCount == 0) || (size > 0x7fffffffu))
	{
		return 0;
	}

	data = (u8 *)malloc((size_t)size);
	if (data == NULL)
	{
		return 0;
	}

	if (rawSectors)
	{
		if (!NativeDiscImage_ReadRawSectors(&file, 0, sectorCount, data))
		{
			free(data);
			return 0;
		}
	}
	else if (!NativeDiscImage_ReadDataBytesScratch(&file, 0, data, size, &s_nativeDiscImageMainScratch))
	{
		free(data);
		return 0;
	}

	*dataOut = data;
	*sizeOut = (int)size;
	return 1;
}

#if CTR_NATIVE_DISC_IO_PROFILE
/*
 * One [DISCIO] line per owner for native_perf.c's window report (same unity
 * TU; perf forward-declares this). Window counters reset inside the format
 * call so the report and the reset cannot drift apart -- same contract as
 * NativeAudio_DebugFormatVoiceProfWindow. Integers only: devkitARM newlib
 * does not link float formatting unless asked.
 *
 * `perCall` is the number to read first: it is sectors/calls, so it says
 * directly whether a read run actually batched (~16 for a level load) or
 * collapsed back to one sector per SD round trip (1).
 */
internal void NativeDiscImage_DebugFormatIoProfWindow(char *buf, int bufSize, int owner)
{
	struct NativeDiscImageIoProf *prof = &s_nativeDiscIoProf[(owner == NATIVE_DISC_IMAGE_OWNER_AUDIO) ? NATIVE_DISC_IMAGE_OWNER_AUDIO : NATIVE_DISC_IMAGE_OWNER_MAIN];
	unsigned long long freq = 1ull;
	u32 perCall;

#if defined(__3DS__)
	freq = N3DS_GetTickFrequency();
#endif
	if (freq == 0ull)
	{
		freq = 1ull;
	}

	perCall = (prof->calls == 0) ? 0u : (prof->sectors / prof->calls);

	if (owner == NATIVE_DISC_IMAGE_OWNER_AUDIO)
	{
		snprintf(buf, (size_t)bufSize, "[DISCIO] audio calls=%u sec=%u perCall=%u runs1=%u maxRun=%u us=%d worstUs=%d hdrFail=%u ioFail=%u", prof->calls, prof->sectors, perCall,
		         prof->runsOf1, prof->maxRun, (int)((prof->ticks * 1000000ull) / freq), (int)((prof->worstTicks * 1000000ull) / freq), prof->headerFails, prof->ioFails);
	}
	else
	{
		snprintf(buf, (size_t)bufSize, "[DISCIO] main calls=%u sec=%u runs1=%u maxRun=%u dir=%u cd=%u/%u xaCnt=%u sng=%u us=%d worstUs=%d hdrFail=%u ioFail=%u", prof->calls,
		         prof->sectors, prof->runsOf1, prof->maxRun, s_nativeDiscIoDirCalls, s_nativeDiscIoSiteCd, s_nativeDiscIoSiteCdSectors, s_nativeDiscIoSiteXaCount,
		         s_nativeDiscIoSiteSingle, (int)((prof->ticks * 1000000ull) / freq), (int)((prof->worstTicks * 1000000ull) / freq), prof->headerFails, prof->ioFails);
		s_nativeDiscIoDirCalls = 0;
		s_nativeDiscIoDirSectors = 0;
		s_nativeDiscIoSiteCd = 0;
		s_nativeDiscIoSiteCdSectors = 0;
		s_nativeDiscIoSiteXaCount = 0;
		s_nativeDiscIoSiteSingle = 0;
	}

	prof->calls = 0;
	prof->sectors = 0;
	prof->runsOf1 = 0;
	prof->maxRun = 0;
	prof->ticks = 0;
	prof->worstTicks = 0;
	prof->headerFails = 0;
	prof->ioFails = 0;
}
#endif
