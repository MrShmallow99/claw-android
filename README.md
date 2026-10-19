# Claw for Android: a native port of the CrazyHook edition

A native Android port of the Windows PC game **Captain Claw** (Monolith Productions, 1997),
in the community **CrazyHook** edition. The game's original 32-bit `CLAW.exe` is
statically recompiled to C. The Windows, DirectX and middleware APIs it uses are
re-implemented on SDL2, along with the CrazyHook mod engine (LuaJIT scripts, custom levels,
plugins). There is no emulator: the game runs as native ARM64 code.

**This repository contains no game files.** To play, you need your own copy of the
supported release (below).

> **Legal notice.** Captain Claw and all related names, characters, artwork, music and
> other content are the property of their respective rights holders. This project is not
> affiliated with, endorsed by or connected to them, and its authors claim no rights to
> the game. This repository contains no game files, no game code and no game assets: it
> holds only the port's own source code, build scripts and documentation (the launcher
> icon is the emblem of the community CrazyHook edition). The game files
> must come from a copy you obtained legitimately. An APK you build that includes game
> files is for your personal use only and must not be redistributed.

---

## Supported release

This port is built for one specific release, and only that release is supported:

**Claw v1.4.5.4 "CrazyHook" + level pack + cutscenes**
(`CLAW-1.4.5.4-Complete-Pack.zip`)

| Hash | Value |
|---|---|
| SHA-1 | `5d1ce45460462f32e0a52dd4c387e801a13f44bb` |
| SHA-256 | `f74f908527e5c11f04682d6c944c76dd86d6926d5465e76f7093c7da39180eb7` |

The recompiled code is tied to that release's `CLAW.exe`
(SHA-256 `33cf6e2663823f073d769c3d05a1ad5e13e3e4993ad2cb6822617737395004c6`); at start-up
the app checks that the executable's code matches before running it. Other versions of the
game or of CrazyHook won't work.

Check your file before you start:

```sh
sha256sum CLAW-1.4.5.4-Complete-Pack.zip                 # Linux
```
```powershell
Get-FileHash CLAW-1.4.5.4-Complete-Pack.zip -Algorithm SHA256   # Windows PowerShell
```

---

## Features

- **The original game, unmodified:** your game files are used exactly as they are and are
  never written to. Saves and settings go to the app's own storage.
- **Full CrazyHook support:** the Lua engine, the 640 custom levels and their picker,
  plugins, and CrazyHook's extra cheats.
- **Touch controls** (a floating joystick and context-sensitive buttons) that can be
  switched off, and **game controllers** (any pad Android recognises).
- **An in-game cheat menu** listing every cheat the game knows, including those added
  by CrazyHook and custom levels, plus a field to type any code.
- **Widescreen (optional):** fills any screen shape by showing more of the level, never
  by stretching the picture.
- **The DVD-quality cutscenes** shipped with the pack, and **MIDI music** through a bundled
  General MIDI soundfont.
- **Optional on-device compression** of the imported files: lossless, verified file by
  file, about 600 MB → 260 MB.

See **[docs/USER_GUIDE.md](docs/USER_GUIDE.md)** for how to play and how every feature works.

---

## Source code only

This project is distributed as **source code only**. No prebuilt APKs are provided: the
app's native code is produced by recompiling the game's executable, so every APK is built
by its user, from their own copy of the game, with the scripts in this repository.

The build produces either of two APKs:

| APK | Contents |
|---|---|
| `Claw-folder-picker.apk` | The app only. On first launch it imports your game folder from the device. |
| `Claw-with-game-files.apk` | The app with your game files packed inside (lossless). **Personal use only.** |

Requirements: a 64-bit ARM Android device (arm64-v8a) running Android 7.0 or newer.

### Using Claw-folder-picker.apk

1. Extract `CLAW-1.4.5.4-Complete-Pack.zip` and copy the `CLAW-1.4.5.4` folder (the one with
   `CLAW.exe`, `CrazyHook.lua`, `Assets` and `Custom`) to the device, for example into `Download/`.
2. Install and open the app.
3. Optionally tick **Compress the game files after importing**.
4. Tap **Select game folder** and choose the folder.

The import takes a few minutes; after that the game starts directly.

---

## Building from source

Both builds need your copy of the supported pack, because the app's native code is
produced by recompiling its `CLAW.exe`. Everything happens on your computer. The build:
1. verifies the pack's hash;
2. downloads the third-party sources at pinned versions (each one checksum-verified);
3. recompiles the executable;
4. builds the APK.

The APK lands in `out/`.

| Mode | Command argument | Result |
|---|---|---|
| Folder picker | `picker` | `out/Claw-folder-picker.apk` |
| With game files | `bundled` | `out/Claw-with-game-files.apk` |

`--game` accepts either the pack `.zip` or the extracted game folder.

### Windows (PowerShell + Docker Desktop)

1. Install [Docker Desktop](https://www.docker.com/products/docker-desktop/) (WSL 2 backend)
   and [Git](https://git-scm.com/download/win), and start Docker Desktop.
2. In PowerShell:

```powershell
git clone https://github.com/MrShmallow99/claw-android.git
cd claw-android

# the folder-picker APK
.\tools\build.ps1 picker -Game "$HOME\Downloads\CLAW-1.4.5.4-Complete-Pack.zip"

# or: the APK with your game files inside (personal use only)
.\tools\build.ps1 bundled -Game "$HOME\Downloads\CLAW-1.4.5.4-Complete-Pack.zip"
```

If PowerShell refuses to run the script, allow it for this session first:
`Set-ExecutionPolicy -Scope Process Bypass`.

The first run builds the build container (Android SDK, NDK, compilers) and compiles
LuaJIT and FFmpeg, which takes 20–40 minutes depending on the machine and connection.
Later builds take a few minutes. Other options: `-Abis "arm64-v8a x86_64"` (adds
emulator support), `-SkipHashCheck`, `-RebuildImage`.

### Linux (Docker)

No toolchain needed on the host other than Docker and Git:

```sh
git clone https://github.com/MrShmallow99/claw-android.git
cd claw-android
tools/build-docker.sh picker  --game ~/Downloads/CLAW-1.4.5.4-Complete-Pack.zip
tools/build-docker.sh bundled --game ~/Downloads/CLAW-1.4.5.4-Complete-Pack.zip
```

### Linux (native toolchain)

Install the tools listed below, then run `tools/build.sh` directly:

- Packages (Ubuntu/Debian): `build-essential cmake ninja-build pkg-config python3 python3-venv git curl unzip p7zip-full xz-utils openjdk-17-jdk-headless`
- The Android SDK with `platforms;android-36`, `build-tools;34.0.0` and `ndk;27.2.12479018`

```sh
export ANDROID_HOME=$HOME/Android/Sdk
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/27.2.12479018
tools/build.sh picker --game ~/Downloads/CLAW-1.4.5.4-Complete-Pack.zip
```

`tools/build.sh --help` lists all options.

### Signing

By default the APK is signed with your Android SDK's debug key. To sign with your own
key (so that later builds install over each other and keep your saves), set these before building:

| Variable | Meaning |
|---|---|
| `CLAW_KEYSTORE` | Path to a `.jks` keystore |
| `CLAW_KEYSTORE_PASSWORD` | Keystore password |
| `CLAW_KEY_ALIAS` | Key alias |
| `CLAW_KEY_PASSWORD` | Key password (defaults to the keystore password) |

---

## Continuous integration

[`.github/workflows/build.yml`](.github/workflows/build.yml) checks every push and pull
request in the same Docker environment. It builds the dependencies, the packer and the
Android app shell. It doesn't build the game's native code, which needs the game files,
and it produces no APK.

---

## How it works

| Part | What it does |
|---|---|
| `recomp/` | Static recompiler: disassembles the 32-bit x86 executable and translates every function to C. CrazyHook's start-up code patches are applied first; code that the scripts patch at run time is translated with run-time checks. |
| `runtime/` | Runs the translated code: guest memory, Win32 (kernel32, user32 with dialogs and controls, gdi32), DirectDraw, DirectSound, MCI video, Smacker, Miles/XMIDI music (TinySoundFont), the touch overlay, controllers, the cheat menu, widescreen. |
| `runtime/lua/` | LuaJIT patched to run CrazyHook's scripts against the game's 32-bit memory, and a native re-implementation of `CrazyHook.dll`. |
| `runtime/pack/` | The lossless pack format and reader (LZMA, solid groups, FLAC), and the packer used on the device and by the build. |
| `android/` | The Android app: SDL2 Java glue, the setup screen (folder import, optional compression) and the game activity. |
| `games/claw/game.ini` | Everything specific to this game: controls, cheat labels, packing rules, widescreen. |
| `tools/` | Build scripts, the dependency fetcher, the packer CLI (`devicepack`) and developer tools. |

The app verifies `CLAW.exe` against the hash the code was recompiled from, and refuses to
run with any other executable.

---

## Third-party software

The APK includes SDL2, LuaJIT, FFmpeg (a minimal LGPL build), TinySoundFont, dr_flac,
stb, the LZMA SDK, the Liberation Sans fonts and the GeneralUser GS soundfont. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for their licenses. The build downloads them
from their official sources at pinned, checksum-verified versions; none are stored in
this repository.

## License

The source code of this port is released under the [MIT License](LICENSE). The license
covers only the port's own code; it grants no rights to the game.
