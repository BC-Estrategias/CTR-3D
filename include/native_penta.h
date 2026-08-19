#ifndef NATIVE_PENTA_H
#define NATIVE_PENTA_H

/* Restore of the PAL/Japan "5/5/5" Penta Penguin class. ON by default (owner's
 * call -- Penta is always "God Penta"); turn off with the `pentapal` dumpreq. This
 * deliberately diverges from NTSC-U retail, unlike most native gates which default
 * to retail behaviour. In NTSC-U Penta is the Turn class (best handling
 * but the slowest); the PAL/Japan build gave him a maxed-out stat class (an
 * unfinished dev remnant) with full Speed, Acceleration and Turn bars. That extra
 * class column is Europe/Japan-exclusive, so it does not exist in this NTSC-U tree
 * -- instead we reconstruct it by writing best-in-class values on top of Penta at
 * kart birth (VehBirth_SetConsts). Turn rate and drift arc are already max on the
 * Turn class Penta starts from; we set them explicitly so the result holds even if
 * his base class ever changes. g_pentaPal is a plain runtime int for now, toggled
 * by the 3DS dumpreq `pentapal`; the eventual owner is the Options menu. */

#ifdef CTR_NATIVE

extern int g_pentaPal;

static inline void NativePenta_ApplyPalClass(struct Driver *driver)
{
	driver->const_Speed_ClassStat = 13900;      /* top speed: best-in-class (Speed) */
	driver->const_AccelSpeed_ClassStat = 15400; /* boosted-speed ceiling: best-in-class */
	driver->const_Accel_ClassStat = 544;        /* acceleration: best-in-class (Accel) */
	driver->const_TurnRate = 30;                /* turn rate: best-in-class (Turn) */
	driver->const_TurnInputDelay = 4000;        /* steering: snappiest (Speed) */
	driver->const_DriftTurnBase = 18;           /* drift: tightest/best (Turn) */
}

#endif /* CTR_NATIVE */

#endif /* NATIVE_PENTA_H */
