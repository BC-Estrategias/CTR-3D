#ifndef NATIVE_DISC_IMAGE_H
#define NATIVE_DISC_IMAGE_H

#include <macros.h>

#include <stddef.h>

#define NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE 2352u

/*
 * Staging run for the bulk sector reader. Every disc-image read bottoms out
 * in one fseek+fread pair; on 3DS that pair is an FS-service IPC round trip
 * to the SD card, so what matters is how many of them a read costs, not how
 * many bytes it moves. Reading a run of raw sectors into this buffer and
 * splitting it into the caller's (narrower) destination stride turns an
 * N-sector read into N/NATIVE_DISC_IMAGE_CHUNK_SECTORS reads.
 *
 * Caller-owned rather than a file-scope static because TWO threads read the
 * disc image on 3DS: the main thread (level loads, CD, STR) and the audio
 * worker (the XA pump, n3ds_audio_thread.c). Same disjointness contract as
 * the per-stream XA decode scratch -- whoever owns the scratch owns the read
 * -- which is the contract commit 87cfc24 had to introduce after a shared
 * static wedged run 5. The audio worker's stack is 32KB, so this cannot live
 * on the stack either.
 *
 * `owner` labels the profile counters only (NATIVE_DISC_IMAGE_OWNER_*); it is
 * unused unless CTR_NATIVE_DISC_IO_PROFILE is on. It names a ROLE -- who the
 * read is for -- not a thread: XA streaming vs everything else. On 3DS the
 * two roles happen to be the two threads, which is what makes the split
 * readable as contention there; on PC both roles run on the vblank thread and
 * the split is still the useful one, because it separates the music stream's
 * steady drip from the level loader's bursts.
 */
#define NATIVE_DISC_IMAGE_CHUNK_SECTORS 16

#define NATIVE_DISC_IMAGE_OWNER_MAIN  0
#define NATIVE_DISC_IMAGE_OWNER_AUDIO 1
#define NATIVE_DISC_IMAGE_OWNER_COUNT 2

struct NativeDiscImageScratch
{
	int owner;
	u8 raw[NATIVE_DISC_IMAGE_CHUNK_SECTORS * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE];
};

struct NativeDiscImageFile
{
	u32 lba;
	u32 size;
};

int NativeDiscImage_Init(const char *assetsDir);
int NativeDiscImage_FindFile(const char *path, struct NativeDiscImageFile *fileOut);

/*
 * The unsuffixed entry points read through a shared main-thread scratch and
 * are therefore MAIN-THREAD ONLY. Any caller that can run on the audio worker
 * must own a struct NativeDiscImageScratch and use the *Scratch variants.
 */
int NativeDiscImage_ReadDataSectors(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst);
int NativeDiscImage_ReadRawSectors(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst);
int NativeDiscImage_ReadDataSectorsScratch(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst, struct NativeDiscImageScratch *scratch);
int NativeDiscImage_ReadRawSectorsScratch(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst, struct NativeDiscImageScratch *scratch);
int NativeDiscImage_ReadFileBytes(const char *path, int rawSectors, u8 **dataOut, int *sizeOut);

#endif
