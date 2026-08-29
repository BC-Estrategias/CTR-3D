#ifndef NATIVE_AMNESIA_H
#define NATIVE_AMNESIA_H

/*
 * Amnesia-probe context hooks: game code pushes frame state and scene events;
 * the OT-walk profile's collapse detector (native_gpu.c, [AMNESIA]) logs them
 * when packet reuse collapses. Outside profile builds the hooks are no-op
 * inlines, so call sites stay unconditional inside their CTR_INTERNAL guards.
 */

#define AMN_EV_WEAPON_FIRE 1 /* a=weaponID, b=driverID */
#define AMN_EV_WEAPON_HIT  2 /* a=damageType, b=victim driverID */
#define AMN_EV_RESPAWN     3 /* a=driverID, b=spawnFlag */
#define AMN_EV_MASK        4 /* a=driverID */
#define AMN_EV_CAMCUT      5 /* a=cameraID */

#ifdef CTR_NATIVE_OTWALK_PROFILE
void NativeAmnesia_FrameState(s32 level, u32 mode1, u32 mode2, s32 clockFx, s32 fade, s32 camX, s32 camY, s32 camZ);
void NativeAmnesia_NoteEvent(u32 kind, s32 a, s32 b);
#else
static inline void NativeAmnesia_FrameState(s32 level, u32 mode1, u32 mode2, s32 clockFx, s32 fade, s32 camX, s32 camY, s32 camZ)
{
	(void)level;
	(void)mode1;
	(void)mode2;
	(void)clockFx;
	(void)fade;
	(void)camX;
	(void)camY;
	(void)camZ;
}
static inline void NativeAmnesia_NoteEvent(u32 kind, s32 a, s32 b)
{
	(void)kind;
	(void)a;
	(void)b;
}
#endif

#endif
