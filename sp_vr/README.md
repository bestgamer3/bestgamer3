# MW2 Campaign VR (2009) - Phase 1

This folder contains the single-player campaign VR companion for `iw4sp.exe`.

Phase 1 is intentionally single-player-only and runs as a separate process. It does not inject code into the game. It initializes OpenXR, tracks the headset, maps head rotation to campaign mouse-look, and reapplies a configurable FOV to the stock MW2 2009 single-player process.

## Current scope

- Targets only `iw4sp.exe`.
- Launches MW2 Single Player through Steam when the campaign is not already running.
- Automatically waits for and attaches to `iw4sp.exe` after Steam starts it.
- OpenXR HMD session and head tracking.
- Head yaw/pitch -> campaign mouse-look.
- Configurable FOV (default 100).
- F8 recenter, F9 toggle head-look, F12 exit companion.
- No stereo eye rendering yet.
- No positional 6DoF translation yet.
- No motion controllers yet.

## Build

Requires Windows, CMake, Visual Studio 2022 and internet access for CMake to fetch Khronos OpenXR-SDK `release-1.1.61`.

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

The output is `MW2CampaignVR.exe` plus `openxr_loader.dll`.

## Run

Put both files in the MW2 2009 game folder beside `iw4sp.exe`, ensure your headset software is configured as the active OpenXR runtime, and make sure Steam is installed and signed in. Then run:

```bat
MW2CampaignVR.exe
```

If the campaign is not already running, the companion opens the official Steam launch URI for MW2 2009 Single Player (`steam://rungameid/10180`), waits for `iw4sp.exe`, and attaches automatically. If the campaign is already running, it attaches without launching a second copy.

Optional settings:

```bat
MW2CampaignVR.exe --fov 100 --gain 8
```

## Compatibility note

The FOV pointer offsets are based on the public CoD SCZ FoV Changer implementation for the Steam-era MW2 single-player executable. The companion computes them relative to the loaded `iw4sp.exe` module base and refuses to attach to multiplayer processes.
