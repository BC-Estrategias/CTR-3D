/*
 * Boundary between the game's unity translation unit and libctru.
 *
 * This header deliberately exposes NOTHING from <3ds.h> -- no libctru types,
 * no libctru constants -- and is safe to include from the unity build. That
 * separation is not stylistic. Pulling <3ds.h> into the unity TU collides with
 * the decompilation in three concrete ways:
 *
 *   1. include/psx/r3000.h defines the MIPS assembler aliases `flush` and
 *      `inval`, which mangle libctru's own parameter names (boss.h and nfc.h
 *      both declare parameters called `inval`).
 *   2. libctru typedefs `ThreadFunc` as void(*)(void*); the game already has
 *      its own ThreadFunc taking a struct Thread*.
 *   3. libctru typedefs u8/u32/s32 to the stdint types, which is exactly the
 *      binding include/macros.h has to avoid on ARM (see the note there).
 *
 * n3ds_backend.c is therefore compiled as its own translation unit -- it sees
 * libctru and never sees the game headers. Everything crossing this line uses
 * plain C scalar types.
 */

#ifndef N3DS_BACKEND_H
#define N3DS_BACKEND_H

/* --------------------------------------------------------------- timing --- */
/* Monotonic tick counter and its rate. Backed by the ARM11 system tick, which
 * runs at a fixed frequency regardless of New3DS CPU speed mode. */
unsigned long long N3DS_GetTickCount(void);
unsigned long long N3DS_GetTickFrequency(void);
void N3DS_SleepMs(unsigned int ms);

/* ------------------------------------------------------------- graphics --- */
void N3DS_GfxInit(void);
void N3DS_GfxExit(void);
/* Push the current framebuffer to the screen. Does NOT wait for vblank --
 * frame pacing belongs to VSync() in the platform layer. */
void N3DS_GfxPresent(void);

/* ---------------------------------------------------------------- input --- */

#define N3DS_BTN_A      (1u << 0)
#define N3DS_BTN_B      (1u << 1)
#define N3DS_BTN_X      (1u << 2)
#define N3DS_BTN_Y      (1u << 3)
#define N3DS_BTN_L      (1u << 4)
#define N3DS_BTN_R      (1u << 5)
#define N3DS_BTN_START  (1u << 6)
#define N3DS_BTN_SELECT (1u << 7)
#define N3DS_BTN_UP     (1u << 8)
#define N3DS_BTN_DOWN   (1u << 9)
#define N3DS_BTN_LEFT   (1u << 10)
#define N3DS_BTN_RIGHT  (1u << 11)

/* Samples the pad and returns held buttons as N3DS_BTN_* bits. D-pad and
 * circle pad are folded together. */
unsigned int N3DS_ReadButtons(void);

/* -------------------------------------------------------------- logging --- */
/* Emits to the console's debug string service. Azahar records these in its log
 * and Luma shows them on hardware, so this is the one output channel that works
 * without a screen or a writable SD card. */
void N3DS_DebugLog(const char *msg);

/* Points stderr at that same debug channel. Call before anything that can fail,
 * so early startup errors are visible even though the screen console does not
 * exist until the renderer initialises. */
void N3DS_EarlyInit(void);

/* ---------------------------------------------------------------- memory --- */
/* Bytes currently handed out by malloc, and free space in the linear heap.
 * Reported on screen so runaway allocation is visible as it happens. */
unsigned long N3DS_HeapUsedBytes(void);
unsigned long N3DS_LinearFreeBytes(void);

/* ------------------------------------------------------------------ app --- */
/* Zero once the user has closed the applet (HOME/power), meaning the process
 * should shut down. */
int N3DS_MainLoopActive(void);

#endif
