# Third-party software

The build downloads these components from their official sources at pinned versions
(see `tools/fetch_deps.sh` for the exact versions and checksums). They are not stored in
this repository. The Android app includes the components marked "in the APK".

| Component | Version | License | Use |
|---|---|---|---|
| [SDL](https://www.libsdl.org/) | 2.30.9 | zlib | Windowing, input, audio (in the APK; the Java glue in `android/app/src/main/java/org/libsdl` is part of SDL) |
| [LuaJIT](https://luajit.org/) | 2.1 (commit `dd3b2436`) | MIT | Runs the CrazyHook scripts; patched at build time by `tools/luajit_guest32.py` (in the APK) |
| [FFmpeg](https://ffmpeg.org/) | 7.1 | LGPL 2.1 or later | Smacker/AVI/MP3 decoding and FLAC encoding; a minimal static build configured by `tools/build_ffmpeg.sh` (in the APK). Its source is the official 7.1 release; build it again with that script to relink. |
| [TinySoundFont](https://github.com/schellingb/TinySoundFont) | commit `853a0a17` | MIT | MIDI synthesis (in the APK) |
| [dr_flac](https://github.com/mackron/dr_libs) | 0.12.42 | Public domain or MIT-0 | FLAC decoding (in the APK) |
| [stb_truetype, stb_vorbis](https://github.com/nothings/stb) | pinned commits | Public domain or MIT | Text rendering, Ogg Vorbis (in the APK) |
| [LZMA SDK](https://www.7-zip.org/sdk.html) | 25.01 | Public domain | Pack compression and decompression (in the APK) |
| [Liberation Sans](https://github.com/liberationfonts/liberation-fonts) | 2.1.5 | SIL Open Font License 1.1 | User interface font (in the APK) |
| [GeneralUser GS](https://github.com/mrbumpy409/GeneralUser-GS) | 2.0.3 | GeneralUser GS License v2.0 (free use and redistribution) | General MIDI soundfont for the game's music (in the APK) |
| [Capstone](https://www.capstone-engine.org/), [pefile](https://github.com/erocarrera/pefile) | 5.0.9, 2024.8.26 | BSD-3-Clause, MIT | Used by the recompiler at build time only |

## Application icon

The launcher icon (`games/claw/icon.png` and the generated `games/claw/res/`) is the
CrazyHook edition's emblem, as found in that community release. It identifies the
community edition this port targets.
