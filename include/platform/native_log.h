#ifndef NATIVE_LOG_H
#define NATIVE_LOG_H

#include <macros.h>

int Platform_LogSetPath(const char *path);
const char *Platform_LogGetPath(void);
void Platform_LogInit(const char *appName);
void Platform_LogShutdown(void);
void Platform_LogFlush(void);
void Platform_Log(const char *fmt, ...) CTR_PRINTF_FORMAT(1, 2);

/*
 * Windowed INSTRUMENT lines ([MODELFRAME], [PKTDEPTH], [DEMIT], ...) as
 * opposed to diagnostics. Same text, different sink on 3DS: N3DS_DebugLog,
 * which buffers and flushes at most every ~2s, instead of Platform_Log, which
 * fputs to the bottom-screen console (software text rendering, and it scrolls
 * the stats block away) AND fflushes the SD log on EVERY line.
 *
 * That per-line flush is deliberate for Platform_Log -- hang and crash
 * checkpoints are worthless if the tail is lost -- but an instrument emitting
 * a burst of five lines every 64 frames then puts five SD flushes on ONE
 * frame, roughly every two seconds. Owner reported periodic hitching and
 * suspected exactly these lines; the cost is real, not nocebo.
 *
 * Off-3DS this is Platform_Log unchanged. Callers pass a trailing newline as
 * usual; it is stripped for N3DS_DebugLog, which appends its own.
 */
void Platform_LogInstrument(const char *fmt, ...) CTR_PRINTF_FORMAT(1, 2);
void Platform_LogWarn(const char *fmt, ...) CTR_PRINTF_FORMAT(1, 2);
void Platform_LogError(const char *fmt, ...) CTR_PRINTF_FORMAT(1, 2);

#endif
