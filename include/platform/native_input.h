#ifndef PLATFORM_NATIVE_INPUT_H
#define PLATFORM_NATIVE_INPUT_H

#include <macros.h>

#define PLATFORM_INPUT_PAD_COUNT 4

struct PlatformInputPadSnapshot
{
	u8 status;
	u8 id;
	u8 buttons[2];
	u8 analog[4];
	u8 connected;
	u8 reserved[3];
};

int Platform_InputInit(void);
void Platform_InputShutdown(void);
void Platform_InputUpdate(void);
void Platform_InputControllerAdded(int deviceIndex);
void Platform_InputControllerRemoved(int instanceId);
int Platform_InputCycleKeyboardController(void);
int Platform_InputCycleGamepadController(void);

/* Virtual multitap control ("padmux", default OFF -- the 3DS renderer has no
 * split-screen support yet, and off means retail's own "insert controller"
 * gate keeps VS/battle unenterable): op 0 = off (retail single-pad bus),
 * 1..4 = on with the physical input driving that pad, anything else =
 * advance (cycle the driven pad, enabling if off). Returns the driven pad
 * number 1..4, or 0 when off. Plain-int signature on purpose: the 3DS
 * backend TU calls it across the enum-width ABI boundary. */
int Platform_InputDebugPadMux(int op);

void Platform_InputPadInit(int slot, unsigned char *padData);
int Platform_InputPadGetState(int port);
void Platform_InputPadVibrate(int port, unsigned char *table, int len);
int Platform_InputCapturePadSnapshots(struct PlatformInputPadSnapshot *dst, int count);
int Platform_InputInstallPadSnapshots(const struct PlatformInputPadSnapshot *src, int count);
void Platform_InputClearInstalledPadSnapshots(void);
int Platform_InputGetStateSize(void);
int Platform_InputCaptureState(void *dst, int dstSize);
int Platform_InputRestoreState(const void *src, int srcSize);

#endif
