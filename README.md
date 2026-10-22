# DS3 VR: Dark Souls III in VR

True stereo VR for Dark Souls III. Both eyes are rendered from the same game frame, so there is no
alternate-eye shimmer. The camera follows your head, the HUD sits on a floating panel you can size
and place, and the game runs at your headset's refresh rate. You play with a gamepad; there are no
motion controls.

DS3 VR works on top of **Helifax's Dark Souls 3 geo-11 fix**, which does the stereo rendering. This
mod adds the VR side: OpenXR output to your headset, head tracking, the HUD panel, the lock-on
marker in 3D, and an in-headset menu.

**Download:** see [Releases](../../releases). Each release has a zip with the setup and its own README.

## What you need

| | |
|---|---|
| Game | DARK SOULS III on Steam, version 1.15.2 (the current Steam version) |
| VR runtime | Any OpenXR runtime set as the active one: SteamVR, Virtual Desktop (VDXR), Meta Quest Link, PimaxXR. Tested with SteamVR. |
| Headset | Any PC VR headset that runs on that runtime |
| Gamepad | Xbox, or others through Steam Input |
| Helifax's geo-11 fix | [Dark Souls 3 geo-11 fix](https://helixmod.blogspot.com/2016/04/dark-souls-3-3d-vision-fix.html) by Helifax and masterotaku (21 October 2023 version). It includes the geo-11 3D driver. |

## Install

1. Download `Dark_Souls_3_geo11_fix.7z` from the HelixMod page above (the geo-11 version, not the
   3D Vision one). Put it in the game's `Game` folder (the one with `DarkSoulsIII.exe`), or leave it
   in your Downloads folder. You do not need to extract it.
2. Download the latest DS3 VR zip from [Releases](../../releases), extract it anywhere, and run
   `DS3VR-Setup.exe`. Windows may warn that it is from an unknown publisher: click **More info**,
   then **Run anyway**.
3. The setup finds the game through Steam, unpacks Helifax's fix if needed, backs up every file it
   changes into `Game\ds3vr_backup`, switches geo-11 to VR output, and sets the game's graphics
   options for VR. Press **I** to install.
4. Start your VR runtime, then launch Dark Souls III from Steam as usual.

To uninstall, run the setup again and press **U**. It restores every file it changed.

## In game

| Action | Gamepad | Keyboard |
|---|---|---|
| Open or close the VR menu | Click both thumbsticks together | Insert |
| Move in the menu | D-pad or left stick | Arrow keys |
| Select / close | A / B | Enter / Backspace |
| Recenter | Your runtime's recenter, or the menu | F8 |
| HUD size / HUD mode | The menu | F6 / F7 |

The VR menu has world scale (1.0 life size to 5.0), HUD mode (fixed, follows your head, hidden),
HUD size, distance and height, the resolution readout below, advanced settings (pose lag and head
movement, hidden until shown), developer keys, recenter and reset to defaults. Every change is saved
straight away.

### Graphics settings

The setup sets these for you. Keep Motion Blur and Depth of Field off; the rest is up to you.

| Setting | Set to | Why |
|---|---|---|
| Screen Mode | Windowed | The mod sizes the game for your headset |
| Motion Blur | Off | It smears the image every time you turn your head |
| Depth of Field | Off | It blurs whatever you are not looking at in VR |
| Ambient Occlusion | Off | In 3D it can show noise in lit areas |
| Antialiasing | Off | DS3 VR was tested this way |

Do not change the resolution or screen mode in the game's own menu while playing: the game would
rebuild its graphics, which the stereo driver does not survive.

### Resolution and performance

The game renders at the resolution your VR runtime asks for, read when the game starts. To change
it, change the resolution in your runtime (SteamVR, Virtual Desktop, the Meta app or Pimax Play) and
restart the game. The VR menu shows, per eye:

- **Game now:** what the game renders this session
- **VR runtime asks for:** your runtime's current setting (checked each time the menu opens, or with **Check again**)
- **Next launch:** the size the next launch will use, and whether a restart is needed

The game renders a little wider than the runtime asks for, because DS3 can only draw a symmetric view
and has to cover the wider side of each eye. The height always matches. The frame rate is capped at
your headset's refresh rate.

## Reporting a problem

Open an [issue](../../issues) and attach `Game\ds3vr.log`. If a launch went wrong and you have since
relaunched, attach `Game\ds3vr_prev.log` too: it is the log of the launch before. The first three
minutes of every launch are always logged in full. For problems later in a session, turn on
**Developer keys** in the VR menu first.

## Building

The mod is built with mingw-w64 on Linux or WSL:

```
sudo apt install g++-mingw-w64-x86-64
./build.sh
```

This produces `build/dinput8.dll` (the mod) and `build/DS3VR-Setup.exe` (the installer).
`ds3vr.ini` is the default settings file that ships next to the dll.

| Folder | Contents |
|---|---|
| `src` | The mod: OpenXR output, head tracking, HUD panel, menu, resolution and frame rate handling |
| `installer` | The setup: finds the game, unpacks and checks Helifax's fix, patches it for VR, backs up and restores |
| `tools` | A patch test that runs the installer's patches on Helifax's files outside Windows |
| `third_party` | Dear ImGui, MinHook, the OpenXR headers and 7-Zip's C decoder |

No files from geo-11 or Helifax's fix are in this repository: their licences do not allow
redistribution. The installer only patches the copy each player downloads.

## Credits

- **geo-11** 3D driver: davegl1234 and the HelixMod team
- **Dark Souls 3 geo-11 fix:** Helifax, updated by masterotaku. Please support them; this mod is built on their work.
- **3Dmigoto:** bo3b, Flugan and DarkStarSword
- Ideas from Luke Ross's R.E.A.L. VR and praydog's UEVR
- Third-party code: see [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt)
