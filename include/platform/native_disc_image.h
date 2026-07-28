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
/*
 * 64, raised from 16 on hardware evidence (session 23 runs 1-2). An SD read
 * costs ~45ms per CALL and barely notices the size: measured, the read half
 * is ~26ms fixed + ~0.24ms per sector, and the seek half is ~11.5ms flat, so
 * 12.7x the data cost 14% more time. Both halves are per-call, so call count
 * is the only lever and this constant is the whole of it. 26,011 sectors went
 * 2,102 calls / 94.1s at 16; at 64 that is ~407 calls / ~21s.
 *
 * Costs 64 * 2352 = 147KB per scratch, and there are three (main, XA live, XA
 * pending) plus the XA header-scan chunk. Going higher keeps paying but less
 * each time (the fixed 26ms is amortised over more sectors), and each doubling
 * doubles that footprint AND the length of a single blocking read -- which the
 * audio worker feels directly, since a read it is inside is a read the DSP
 * queue is draining behind.
 */
#define NATIVE_DISC_IMAGE_CHUNK_SECTORS 64

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
