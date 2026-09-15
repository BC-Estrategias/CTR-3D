#ifndef PLATFORM_NATIVE_AUDIO_H
#define PLATFORM_NATIVE_AUDIO_H

#include <macros.h>
#include <psx/libspu.h>

s32 NativeAudio_SpuInit(void);
u32 NativeAudio_SpuSetTransferStartAddr(u32 addr);
u32 NativeAudio_SpuWrite(const u8 *addr, u32 size);
void NativeAudio_SpuSetVoiceAttr(SpuVoiceAttr *psxAttrib);
void NativeAudio_SpuSetKey(s32 on_off, u32 voice_bit);
s32 NativeAudio_SpuSetReverb(s32 on_off);
s32 NativeAudio_SpuSetReverbModeParam(SpuReverbAttr *attr);
void NativeAudio_SpuSetReverbModeDepth(s16 left, s16 right);
u32 NativeAudio_SpuSetReverbVoice(s32 on_off, u32 voice_bit);
void NativeAudio_SpuSetCommonMasterVolume(s16 left, s16 right);
void NativeAudio_SpuSetCommonCDMix(s32 enabled);
void NativeAudio_SpuSetCommonCDVolume(s16 left, s16 right);
void NativeAudio_SpuSetCommonCDReverb(s32 enabled);

int NativeAudio_PlayXATrack(int categoryID, int xaID, int volumeLeft, int volumeRight);
// One-time (per boot) XA voice-line preload: reads every GAME-category track
// plus the wrong-way EXTRA barks into RAM so mid-race voice-line opens do zero
// disc reads. One-shot blocking form (loops the resumable step below with no
// budget cap); self-gates after the first call. No-op off the 3DS (PC disk
// reads never stalled a frame).
void NativeAudio_VoiceCacheEnsure(void);
// Resumable form, so the caller's frame loop keeps running while the warm
// happens: call Begin once, then Step once per frame (budgetSectors = max
// disc/SD sectors read this call, 0 = uncapped) until it returns nonzero.
// Nothing on screen waits on it -- opens before it completes take the cold
// path -- so it must be driven from a frame where no other reader owns the
// card, never from a stage the game is holding.
void NativeAudio_VoiceCacheEnsureBegin(void);
int NativeAudio_VoiceCacheEnsureStep(int budgetSectors);
int NativeAudio_PlayXAFile(const char *relativePath, int channelFilter, int volumeLeft, int volumeRight);
int NativeAudio_GetXATrackLength(int categoryID, int xaID);
int NativeAudio_IsXAPlaying(void);
int NativeAudio_GetXACurrOffset(void);
int NativeAudio_GetXAMaxSample(void);
int NativeAudio_GetXAMaxSampleAtOffset(int xaCurrOffset);
void NativeAudio_SetXAVolume(int volumeLeft, int volumeRight);
void NativeAudio_StopXA(void);
// Drops all decoded voice PCM and resets the decode arena. Needed on restart
// paths that reload no audio banks; see the comment on the definition.
void NativeAudio_ResetVoicePcmArena(void);

void NativeAudio_StepVBlank(void);
int NativeAudio_RenderFrames(s16 *out, int frameCount);
void NativeAudio_SetDeterministicRenderMode(int enabled);
int NativeAudio_IsDeterministicRenderMode(void);
int NativeAudio_QueueRenderedFrames(const s16 *frames, int frameCount);
void NativeAudio_ClearOutputQueue(void);
void NativeAudio_Shutdown(void);
#ifdef CTR_INTERNAL
void NativeAudio_GetOutputStats(int *underrunFrames, int *overflowFrames, int *queuedFrames);
#endif
#if defined(CTR_INTERNAL)
/*
 * Mixer golden-output regression harness -- see the block comment above the
 * implementation in platform/native_audio.c. Returns nonzero when it has run a
 * comparison and main() should exit without starting the game.
 */
int NativeAudio_GoldenConfigureFromArgs(int argc, char **argv);
void NativeAudio_GoldenCaptureTick(void);
#endif

int NativeAudio_GetStateSize(void);
int NativeAudio_CaptureState(void *dst, int dstSize);
int NativeAudio_RestoreState(const void *src, int srcSize);

#endif
