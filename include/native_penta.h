#ifndef NATIVE_PENTA_H
#define NATIVE_PENTA_H

/* Restore of the PAL/Japan "5/5/5" Penta Penguin class. ON by default (owner's
 * call -- Penta is always "God Penta"); turn off with the `pentapal` dumpreq. This
 * deliberately diverges from NTSC-U retail, unlike most native gates which default
 * to retail behaviour. In NTSC-U Penta is the Turn class (best handling
 * but the slowest); the PAL/Japan build gave him the fifth engine class (an
 * unfinished dev remnant). That class column is Europe/Japan-exclusive, so it does
 * not exist in this NTSC-U tree -- instead we write its values on top of Penta at
 * kart birth (VehBirth_SetConsts). The real class-4 MetaPhys column differs from
 * Penta's TURN column in exactly three rows (speed, boosted-speed ceiling,
 * acceleration); the turn/drift fields equal TURN's and are written anyway so the
 * result holds even if his base class ever changes. His engineID stays 3 (TURN)
 * on purpose: the stat-bar and engine-SFX tables index by engineID and have no
 * class-4 rows in the NTSC-U data. g_pentaPal is a plain runtime int for now,
 * toggled by the 3DS dumpreq `pentapal`; the eventual owner is the Options menu. */

#ifdef CTR_NATIVE

extern int g_pentaPal;

static inline void NativePenta_ApplyPalClass(struct Driver *driver)
{
	driver->const_Speed_ClassStat = 13900;      /* class 4 (TURN has 12950) */
	driver->const_AccelSpeed_ClassStat = 15400; /* class 4 (TURN has 14450) */
	driver->const_Accel_ClassStat = 544;        /* class 4 (TURN has 512) */
	driver->const_TurnRate = 30;                /* class 4 == TURN */
	driver->const_TurnInputDelay = 5500;        /* class 4 == TURN */
	driver->const_DriftTurnBase = 18;           /* class 4 == TURN */
}

/* Character-select stat bars for the class Penta doesn't officially have:
 * gGarage.statBarTargetLengths is hard-sized 4 classes x 3 bars, so the class-4
 * look is supplied here instead of via engineID. "5/5/5" = all three bars full;
 * 0x50 is the table's full-bar value. Engine sound stays the TURN engine on
 * purpose: engine SFX entries live in KART.HWL and the NTSC-U disc ships only 4
 * classes' worth -- the lookup is bounds-checked, so forcing class 4 there
 * would silence the kart, not change its note. */
static inline s16 NativePenta_StatBarLength(s16 retailLen, struct MetaDataCHAR *mdc)
{
	if (g_pentaPal && (mdc == &data.MetaDataCharacters[PENTA_PENGUIN]))
	{
		return 0x50;
	}
	return retailLen;
}

#endif /* CTR_NATIVE */

#endif /* NATIVE_PENTA_H */
