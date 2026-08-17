# MW2 Campaign VR (2009) - Experimental Phase 2A

This folder contains the single-player campaign VR companion for `iw4sp.exe`.
It uses the legitimate Steam launch path, attaches only to the campaign process,
and does not bypass Steam or modify multiplayer.

## Phase 2A scope

- Targets only `iw4sp.exe`.
- OpenXR HMD session with full orientation + positional tracking.
- Head yaw/pitch -> campaign mouse-look.
- Experimental view-only 6DoF translation. The companion scans writable
  `iw4sp.exe` image memory for a high-confidence IW4 `refdef` matching the live
  render resolution/FOV/camera-axis structure before any camera-origin write is
  allowed. If no safe candidate is found, positional translation stays off.
- OpenXR projection swapchains for both headset eyes.
- Captures the visible MW2 campaign window and submits it to both eyes.
- Configurable FOV (default 100) and experimental world scale (default 40 game
  units per meter).
- F8 recenter, F9 toggle head-look, F10 toggle 6DoF, F12 exit companion.

### Important stereo limitation

Phase 2A sends the campaign image to both eyes, but it currently duplicates the
same IW4 render into the left and right OpenXR swapchains. This gives real
headset eye output/lens composition, but **not binocular parallax yet**. True
stereoscopic depth requires an in-process IW4 renderer hook that renders the
scene twice with separate eye origins and projection matrices. That is the next
renderer stage.

The window-capture path is experimental and works best when the MW2 window is
visible on the desktop. Exclusive-fullscreen capture behavior varies by GPU and
Windows compositor; windowed/borderless mode is preferred while testing.

## Build

Requires Windows, CMake, Visual Studio 2022 and internet access for CMake to
fetch Khronos OpenXR-SDK `release-1.1.61`.

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

The output is `MW2CampaignVR.exe` plus `openxr_loader.dll`.

## Run

Put both files in the MW2 2009 game folder beside `iw4sp.exe`, ensure your
headset software is configured as the active OpenXR runtime, then run:

```bat
MW2CampaignVR.exe
```

If the campaign is not running, the companion launches the Steam app via
`steam://rungameid/10180`, waits for `iw4sp.exe`, and attaches automatically.

Optional settings:

```bat
MW2CampaignVR.exe --fov 100 --gain 8 --world-scale 40
MW2CampaignVR.exe --no-6dof
```

If positional movement feels too large or too small, adjust `--world-scale`.
F8 establishes a fresh positional and rotational center.

## Compatibility note

The FOV pointer offsets are based on the public CoD SCZ FoV Changer
implementation for the Steam-era MW2 single-player executable. The 6DoF path
does not use a guessed static camera address; it validates the live IW4 refdef
shape before writing, and disables itself when validation fails.
