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

#endif
