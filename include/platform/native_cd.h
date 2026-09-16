#ifndef NATIVE_CD_H
#define NATIVE_CD_H

// NOTE(aalhendi): Native Cd* facade for extracted assets and disc-image fallback.
int NativeCD_Init(void);

// Read a disc subfile into a persistent RAM extent so later reads of that same
// range are served from memory instead of the disc/SD card. encodedLoc is the
// retail CdlLOC int (fileIndex in the high byte, start sector in the low 24
// bits); byteSize is the subfile size. Only caches on the disc-image source;
// a no-op on host-file overrides, on allocation failure, and off the 3DS.
void NativeCD_PrefetchExtent(int encodedLoc, int byteSize);

// Chunked variant for extents too large to read in one synchronous call while
// the per-vblank music sequencer must keep ticking. Begin returns nonzero when
// a pending prefetch exists to pump (it is a no-op returning 0 if the range is
// already cached, unavailable, allocation fails, or off the 3DS); the caller
// then calls Pump once per frame with a sector budget until it returns nonzero
// (done). The extent only serves reads after the final Pump publishes it, so
// the consumer must finish pumping before issuing the reads it wants cached.
int NativeCD_PrefetchExtentBegin(int encodedLoc, int byteSize);
int NativeCD_PrefetchPump(int maxSectors);

// Whether the whole extent is resident in the read cache, for a consumer that
// must know its reads will not reach the disc. Published extents are held for
// the life of the process -- nothing evicts them -- so a 0 means the range was
// never cached, not that it was dropped. Returns -1 where the platform has no
// CD read cache at all and the question has no answer.
int NativeCD_PrefetchExtentCached(int encodedLoc, int byteSize);

#endif
