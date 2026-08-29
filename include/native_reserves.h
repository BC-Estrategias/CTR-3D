#ifndef NATIVE_RESERVES_H
#define NATIVE_RESERVES_H

/* Community-standard boost-reserves meter: a thin HUD bar of driver->reserves
 * matching the scene's canonical implementation (CTR-ModSDK
 * mods/Modules/ReservesMeter by Superstarxalien) -- same slot-8 anchor, 49px
 * width (5px tall vs the mod's 3, for 3DS-screen legibility),
 * reserves*0xE/0x960 fill scale, and red/yellow/green/blue-when-pegged
 * colors, so it reads identically to the meter players know from mods. Drawn
 * wherever the retail slide meter draws (races, not battle mode; hidden once the
 * race is finished): 1P directly below the slide bar like the mod, splitscreen
 * above it (no room below those anchors -- the mod is 1P-only and never faced
 * this). ON by default (owner's call -- scene mods ship it enabled); turn off
 * with the `reserves` dumpreq. g_reservesMeter is a plain runtime int for now;
 * the eventual owner is the Options menu. */

#ifdef CTR_NATIVE

extern int g_reservesMeter;

void UI_DrawReservesMeter(s16 posX, s16 posY, struct Driver *driver);

#endif /* CTR_NATIVE */

#endif /* NATIVE_RESERVES_H */
