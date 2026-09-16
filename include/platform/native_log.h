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

/*
 * "<label> NN%" on one 3DS bottom-screen console row, for a pre-game hold long
 * enough that a still screen reads as a hung console. It claims the first free
 * row under the boot text already on the panel and repaints THAT row in place:
 * no clear, no scroll, no SD write, no netload send -- none of the paths that
 * have made console text a frame hitch here.
 *
 * Call it only when the text changes. Each call blits one row of glyph cells
 * into a column-major rotated framebuffer; a full-screen console repaint is
 * what cost ~50ms a window. Clear blanks the row and releases it, so the line
 * does not survive into the game. Both are no-ops off the 3DS.
 */
void NativeLog_BootStatus(const char *label, int percent);
void NativeLog_BootStatusClear(void);

#endif
