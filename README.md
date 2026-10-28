# DS3 VR: Dark Souls III in VR

True stereo VR for Dark Souls III. Both eyes are rendered from the same game frame, so there is no
alternate-eye shimmer. The camera follows your head, the HUD sits on a floating panel you can size
and place, and the game runs at your headset's refresh rate. You play with a gamepad; there are no
motion controls.

DS3 VR works on top of **Helifax's Dark Souls 3 geo-11 fix**, which does the stereo rendering. This
mod adds the VR side: OpenXR output to your headset, head tracking, the HUD panel, the lock-on
marker in 3D, and an in-headset menu.

**Download:** see [Releases](../../releases). Each release is a zip you extract into the game's `Game` folder.

## What you need

| | |
|---|---|
| Game | DARK SOULS III on Steam, version 1.15.2 (the current Steam version) |
| VR runtime | An OpenXR runtime set as the active one. Thoroughly tested on SteamVR; Virtual Desktop (VDXR), Meta Quest Link and PimaxXR should work but have not been fully tested yet. |
| Headset | Any PC VR headset that runs on that runtime |
| Gamepad | Xbox, or others through Steam Input |
| Helifax's geo-11 fix | [Dark Souls 3 geo-11 fix](https://helixmod.blogspot.com/2016/04/dark-souls-3-3d-vision-fix.html) by Helifax and masterotaku (21 October 2023 version), which includes the geo-11 3D driver. You do not download it yourself: the install step fetches it from its official host. |

## Install

1. Download the latest DS3 VR zip from [Releases](../../releases) and extract all of it into the game's
   `Game` folder (the one with `DarkSoulsIII.exe`).
2. Run `install-ds3vr.bat` in the `Game` folder. This step exists only because geo-11 and Helifax's
   fix cannot be included in the download: it downloads Helifax's fix from its official host (it asks
   first, and checks that it is the geo-11 version), unpacks it, backs up every file it
   changes into `Game\ds3vr_backup`, switches geo-11 to VR output, and sets the game's graphics
   options for VR.
3. Start your VR runtime, then launch Dark Souls III from Steam as usual.

If the automatic download does not work, download `Dark_Souls_3_geo11_fix.7z` (the geo-11 version,
not the 3D Vision one) from the HelixMod page above, put it in the `Game` folder or your Downloads
folder, and run `install-ds3vr.bat` again.

To update, extract the new zip over the old one and run `install-ds3vr.bat` again; your settings are
kept. To uninstall, run `uninstall-ds3vr.bat`: it restores every file it changed and removes DS3 VR.

## In game

| Action | Gamepad | Keyboard |
|---|---|---|
| Open or close the VR menu | Click both thumbsticks together | Insert |
| Move in the menu | D-pad or left stick | Arrow keys |
| Select / close | A / B | Enter / Backspace |
| Recenter | Your runtime's recenter, or the menu | F8 |
| HUD size / HUD mode | The menu | F6 / F7 |

The VR menu has world scale (1.0 life size to 5.0), horizon lock (when on, the right stick no
longer tilts the view up or down; off by default), HUD mode (fixed, follows your head, hidden),
HUD size, distance and height, the resolution readout below, advanced settings (pose lag and head
movement, hidden until shown), developer keys, recenter and reset to defaults. Every change is saved
straight away.

### Graphics settings

The install step sets these for you. Keep Motion Blur and Depth of Field off; the rest is up to you.

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

**The first launch after installing renders wider than it needs to.** The width depends on your
headset's field of view. The game has to be told its size before it starts, but the headset only
reports its field of view once the game is running. So the first launch uses a wide default that
fits any headset, and DS3 VR remembers your headset's real shape for every launch after that. For
example: 5300 x 3700 per eye on the first launch, 4800 x 3700 from the second.

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

This produces `build/dinput8.dll` (the mod), `build/ds3vr-patcher.exe` (run by the install and
uninstall batch files) and `build/DS3VR-<version>/`, the release layout that players extract into the
game's `Game` folder. `dist` holds the batch files, the player README and the default settings.

| Folder | Contents |
|---|---|
| `src` | The mod: OpenXR output, head tracking, HUD panel, menu, resolution and frame rate handling |
| `installer` | The patcher: unpacks and checks Helifax's fix, patches it for VR, backs up and restores |
| `dist` | Files shipped as they are: install and uninstall batch files, player README, default settings |
| `tools` | A patch test that runs the installer's patches on Helifax's files outside Windows |
| `third_party` | Dear ImGui, MinHook, the OpenXR headers, EB Garamond (menu font, third_party/ebgaramond), and 7-Zip's C decoder |

No files from geo-11 or Helifax's fix are in this repository: their licences do not allow
redistribution. The install step only patches the copy each player downloads.

## Credits

- **geo-11** 3D driver: davegl1234 and the HelixMod team
- **Dark Souls 3 geo-11 fix:** Helifax, updated by masterotaku. Please support them; this mod is built on their work.
- **3Dmigoto:** bo3b, Flugan and DarkStarSword
- Ideas from Luke Ross's R.E.A.L. VR and praydog's UEVR
- Third-party code: see [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt)

## Licence

DS3 VR's own code is under the [MIT licence](LICENSE). geo-11 and Helifax's fix have their own
licences and are not part of this repository.
