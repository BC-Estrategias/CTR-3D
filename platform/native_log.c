#include "platform/native_log.h"

#include <macros.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include "platform/native_win32.h"
#endif

global_variable FILE *s_logStream = NULL;
global_variable char s_logPath[512]; // TODO(aalhendi): yeah this is an issue waiting to happen. w/e

internal void Platform_LogWrite(FILE *consoleStream, const char *text)
{
	FILE *stream = (consoleStream != NULL) ? consoleStream : stdout;

#ifdef _WIN32
	OutputDebugStringA(text);
#endif

#ifdef __3DS__
	/* stdout is the bottom-screen debug console, and glyphs land in its
	 * shared framebuffer at print time -- an ungated fputs scribbles over the
	 * bottom-screen minimap and keeps the panel chattering in a release build
	 * ([TRKFRAME]'s periodic census was the line caught doing both). The
	 * console-visibility policy lives in one place, N3DS_ConsolePaintAllowed
	 * (map ownership / dumpreq 'stats' / boot grace); the netload stderr
	 * mirror below and the SD sink still carry every line regardless. */
	if ((stream != stdout) || N3DS_ConsolePaintAllowed())
	{
		fputs(text, stream);
	}
#else
	fputs(text, stream);
#endif

#ifdef __3DS__
	// When netloaded, stderr is the live stream back to the sending PC
	// (n3ds_backend.c). stdout-bound lines would otherwise exist only on the
	// bottom-screen console, which the stats block periodically clears -- and
	// the LOAD/hang checkpoints in game/ go through Platform_Log, not
	// N3DS_DebugLog, so they must be mirrored here to reach the host.
	if ((stream != stderr) && N3DS_NetloadStreamActive())
	{
		fputs(text, stderr);
	}
#endif

	if (s_logStream != NULL)
	{
#ifdef __3DS__
		/* Gated on g_dbg_sdLog, the same switch as ctr3ds.log, so one control
		 * governs every SD write. The WRITE is gated, not just the flush: a
		 * buffered write is only free until stdio's buffer fills, and it then
		 * flushes wherever it happens to be -- mid-corner, unthrottled, which
		 * is the stall the switch exists to remove. This card charges per call,
		 * not per byte. Callers on the frame path must still rate-limit
		 * themselves; the switch defaults off and is not a licence to log every
		 * frame. Off still leaves the bottom-screen console and the netload
		 * stream above, both free.
		 *
		 * Kept per-line rather than throttled like N3DS_DebugLog's: this sink
		 * carries the LOAD/hang checkpoints out of game/, whose whole value is
		 * being on disk when the console stops responding. Switching it on is
		 * an explicit "I want the log, charge me for it". */
		if (!N3DS_SdLogEnabled())
		{
			return;
		}
#endif
		fputs(text, s_logStream);
		fflush(s_logStream);
	}
}

internal void Platform_LogV(FILE *consoleStream, const char *fmt, va_list args)
{
	char text[4096];
	int written = vsnprintf(text, sizeof(text), fmt, args);

	if (written < 0)
	{
		return;
	}

	text[sizeof(text) - 1] = '\0';
	Platform_LogWrite(consoleStream, text);
}

int Platform_LogSetPath(const char *path)
{
	int written;

	if (s_logStream != NULL)
	{
		return 0;
	}

	if ((path == NULL) || (path[0] == '\0'))
	{
		s_logPath[0] = '\0';
		return 1;
	}

	written = snprintf(s_logPath, sizeof(s_logPath), "%s", path);
	if ((written < 0) || ((size_t)written >= sizeof(s_logPath)))
	{
		s_logPath[0] = '\0';
		fprintf(stderr, "[CTR 3D] Error: log path is too long\n");
		return 0;
	}

	return 1;
}

const char *Platform_LogGetPath(void)
{
	return s_logPath;
}

void Platform_LogInit(const char *appName)
{
	if (s_logPath[0] == '\0')
	{
		int written = snprintf(s_logPath, sizeof(s_logPath), "%s.log", appName);

		if ((written < 0) || ((size_t)written >= sizeof(s_logPath)))
		{
			fprintf(stderr, "[CTR 3D] Error: log filename is too long\n");
			s_logPath[0] = '\0';
			return;
		}
	}

	s_logStream = fopen(s_logPath, "wb");

	if (s_logStream == NULL)
	{
		fprintf(stderr, "[CTR 3D] Error: cannot create log file '%s'\n", s_logPath);
	}
}

void Platform_LogShutdown(void)
{
	Platform_LogWarn("---- LOG CLOSED ----\n");

	if (s_logStream != NULL)
	{
		fclose(s_logStream);
	}

	s_logStream = NULL;
}

void Platform_LogFlush(void)
{
	if (s_logStream != NULL)
	{
		fflush(s_logStream);
	}
}

void Platform_Log(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	Platform_LogV(stdout, fmt, args);
	va_end(args);
}

void Platform_LogInstrument(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
#if defined(__3DS__)
	{
		char text[512];
		int written = vsnprintf(text, sizeof(text), fmt, args);

		if (written > 0)
		{
			text[sizeof(text) - 1] = '\0';
			/* N3DS_DebugLog appends its own newline and would otherwise
			 * emit a blank line per instrument line. */
			if ((written < (int)sizeof(text)) && (written > 0) && (text[written - 1] == '\n'))
			{
				text[written - 1] = '\0';
			}
			N3DS_DebugLog(text);
		}
	}
#else
	Platform_LogV(stdout, fmt, args);
#endif
	va_end(args);
}

void Platform_LogWarn(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	Platform_LogV(stdout, fmt, args);
	va_end(args);
}

void Platform_LogError(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	Platform_LogV(stderr, fmt, args);
	va_end(args);
}

void NativeLog_BootStatus(const char *label, int percent)
{
#ifdef __3DS__
	char line[40];

	if (label == NULL)
	{
		return;
	}
	if (percent < 0)
	{
		percent = 0;
	}
	if (percent > 100)
	{
		percent = 100;
	}

	/* Percent right-aligned so the sign holds its column as the number
	 * widens and the row does not jitter. */
	snprintf(line, sizeof(line), "%s %3d%%", label, percent);
	N3DS_ConsoleStatusLine(line);
#else
	(void)label;
	(void)percent;
#endif
}

void NativeLog_BootStatusClear(void)
{
#ifdef __3DS__
	N3DS_ConsoleStatusClear();
#endif
}
