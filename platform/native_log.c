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

	fputs(text, stream);

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
		fprintf(stderr, "[CTR Native] Error: log path is too long\n");
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
			fprintf(stderr, "[CTR Native] Error: log filename is too long\n");
			s_logPath[0] = '\0';
			return;
		}
	}

	s_logStream = fopen(s_logPath, "wb");

	if (s_logStream == NULL)
	{
		fprintf(stderr, "[CTR Native] Error: cannot create log file '%s'\n", s_logPath);
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
