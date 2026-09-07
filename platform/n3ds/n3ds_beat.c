/*
 * Storage for the cross-thread heartbeat counters -- see n3ds_beat.h for the
 * contract. Its own translation unit for the same reason as
 * n3ds_audio_thread.c and n3ds_render_thread.c: libctru's u32/u8 must not
 * meet the decomp's (see n3ds_backend.h's top-of-file comment).
 */

#include <3ds.h>
#include <stddef.h>

#include "n3ds_beat.h"

typedef struct
{
	volatile u32 counter;
	volatile u8 where;
} N3DSBeatSlot;

static N3DSBeatSlot s_beatSlot[N3DS_BEAT_THREAD_COUNT];

void N3DS_BeatMark(int threadId, int whereTag)
{
	if ((threadId < 0) || (threadId >= N3DS_BEAT_THREAD_COUNT))
	{
		return;
	}

	s_beatSlot[threadId].where = (u8)whereTag;
	s_beatSlot[threadId].counter++;
}

int N3DS_BeatRead(int threadId, unsigned int *outCounter, int *outWhere)
{
	if ((threadId < 0) || (threadId >= N3DS_BEAT_THREAD_COUNT))
	{
		if (outCounter != NULL)
		{
			*outCounter = 0;
		}
		if (outWhere != NULL)
		{
			*outWhere = 0;
		}
		return 0;
	}

	if (outCounter != NULL)
	{
		*outCounter = s_beatSlot[threadId].counter;
	}
	if (outWhere != NULL)
	{
		*outWhere = s_beatSlot[threadId].where;
	}
	return 1;
}

const char *N3DS_BeatWhereName(int whereTag)
{
	switch (whereTag)
	{
		case N3DS_BEAT_WHERE_OK:
		{
			return "OK";
		}
		case N3DS_BEAT_WHERE_FRAMEBEGIN:
		{
			return "FBEGIN";
		}
		case N3DS_BEAT_WHERE_FRAMEEND:
		{
			return "FEND";
		}
		case N3DS_BEAT_WHERE_QUEUE_POST_WAIT:
		{
			return "PWAIT";
		}
		case N3DS_BEAT_WHERE_QUEUE_TICKET_WAIT:
		{
			return "TWAIT";
		}
		case N3DS_BEAT_WHERE_EXEC_JOB:
		{
			return "EXEC";
		}
		case N3DS_BEAT_WHERE_AUDIO_RENDER:
		{
			return "AREND";
		}
		case N3DS_BEAT_WHERE_GSP_IPC:
		{
			return "GSPIPC";
		}
		case N3DS_BEAT_WHERE_LOGWAIT:
		{
			return "LOGWAIT";
		}
		default:
		{
			return "?";
		}
	}
}
