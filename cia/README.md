# CIA packaging

`package-cia.sh` creates an installable New 3DS CIA from the native 3DS ELF.
It uses the artwork at `cia-assets/ctr3d-art-source.png` for the HOME Menu
icon and banner by default. Set `CTR_ART` to use another image, or set
`CTR_BANNER_PNG` to provide a banner already composed at 256x128 pixels.

The CIA does not contain the Crash Team Racing disc image. Keep your own raw
NTSC-U MODE2/2352 dump at:

```text
/3ds/ctr_native/assets/ctr-u.bin
```

After building, install the resulting CIA with FBI. The app still writes saves
to `/3ds/ctr_native/memcards/` and uses the same DSP firmware dump as the
3DSX build.

The package requires `makerom`, `bannertool`, and the 3DS devkit toolchain.
If either tool is installed outside `PATH`, set `MAKEROM=/path/to/makerom`
and/or `BANNERTOOL=/path/to/bannertool` when running the script.
The local Title ID uses unique ID `0xC7A3D`; change `cia/ctr3d.rsf` before
publishing a fork to avoid colliding with another homebrew title.
