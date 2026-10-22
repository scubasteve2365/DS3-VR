DS3 VR: DARK SOULS III in VR
============================

True stereo VR for Dark Souls III. Both eyes are rendered from the same game frame, so there is no
alternate-eye shimmer. The camera follows your head, the HUD sits on a floating panel you can size
and place, and the game runs at your headset's refresh rate. You play with a gamepad; there are no
motion controls.

Project page, updates and bug reports: https://github.com/scubasteve2365/DS3-VR


WHAT YOU NEED
-------------
- DARK SOULS III on Steam (the current version, 1.15.2)
- A PC VR headset with an OpenXR runtime set as the active one: SteamVR, Virtual Desktop (VDXR),
  Meta Quest Link or PimaxXR. Thoroughly tested on SteamVR; the others should work but have not been
  fully tested yet.
- A gamepad (Xbox, or others through Steam Input)
- Helifax's Dark Souls 3 geo-11 fix: the install step downloads it for you (see step 2)


STEP 1: EXTRACT DS3 VR
----------------------
Extract the whole DS3 VR zip into the game's Game folder, the folder with DarkSoulsIII.exe.
You get dinput8.dll, install-ds3vr.bat, uninstall-ds3vr.bat and a ds3vr folder next to it.

Game folder example: C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS III\Game


STEP 2: RUN install-ds3vr.bat
-----------------------------
Double-click install-ds3vr.bat in the Game folder (Windows may ask whether to run a downloaded file:
choose Run). It:
- downloads Helifax's Dark Souls 3 geo-11 fix (it asks first), checks that it is the geo-11 version,
  and unpacks it
- backs up every file it changes into Game\ds3vr_backup
- switches geo-11 to VR output and adds a few lines to five of the fix's shaders (HUD, lock-on
  marker and sun rays)
- sets the game's graphics options for VR (windowed; motion blur, depth of field, ambient occlusion
  and antialiasing off). Your previous settings are saved too.
Every change is checked before anything is written. If something does not fit, nothing is changed.

If you start the game before running it, DS3 VR stays off and tells you to run it.

Why this step exists: DS3 VR is built on Helifax's fix and the geo-11 3D driver. Their licences do
not allow anyone else to pass their files on, so they cannot be in the DS3 VR download. The install
step fetches them from their official host (masterotaku's server, linked from Helifax's page) and
adapts that copy for VR.

If the automatic download does not work:
1. Open https://helixmod.blogspot.com/2016/04/dark-souls-3-3d-vision-fix.html
2. Download Dark_Souls_3_geo11_fix.7z (the geo-11 version, not the 3D Vision one).
3. Put it in the Game folder or your Downloads folder (no need to extract it) and run
   install-ds3vr.bat again.


PLAY
----
- Start your VR runtime and make sure it is the active OpenXR runtime (in SteamVR: Settings >
  OpenXR > Set SteamVR as OpenXR runtime). Then launch Dark Souls III from Steam as usual.
- The game renders at the resolution your VR runtime asks for; you do not set it in the game.
- The first launch takes a little longer while geo-11 prepares its shaders.
- Graphics: keep Motion Blur and Depth of Field off. Do not change the resolution or screen mode in
  the game's menu while playing (the game would rebuild its graphics, which the stereo driver does
  not survive). Everything else is up to you.


CONTROLS
--------
  VR menu ............. click both thumbsticks together (keyboard: Insert)
  In the menu ......... D-pad or left stick to move, A to select, B to close
  Recenter ............ your runtime's recenter, the menu, or F8
  HUD size / mode ..... the menu, or F6 / F7

The VR menu has world scale (1.0 life size to 5.0), horizon lock (when on, the right stick no longer
tilts the view up or down; off by default), HUD mode, size, distance and height, the
resolution readout, advanced settings (pose lag and head movement), developer keys, recenter and
reset to defaults. Every change is saved straight away.


RESOLUTION
----------
The VR menu shows the resolution per eye: what the game renders now, what your VR runtime asks for,
and what the next launch will use. To change it, change the resolution in your runtime (SteamVR,
Virtual Desktop, the Meta app or Pimax Play) and restart the game.

The first launch after installing renders wider than it needs to. The height always matches your
VR runtime, but the width depends on your headset's field of view, which the game can only be told
before it starts, and the headset reports it only once the game is running. So the first launch uses
a wide default that fits any headset, and DS3 VR remembers your headset's real shape for every launch
after that. Example: 5300 x 3700 per eye on the first launch, 4800 x 3700 from the second. Nothing
needs doing; just expect the first launch to run a little heavier.


UPDATING
--------
Extract the new DS3 VR zip over the old one and run install-ds3vr.bat again. Your settings are kept.


UNINSTALL
---------
Run uninstall-ds3vr.bat in the Game folder. It puts Helifax's files and your graphics settings back
and removes DS3 VR. To also remove Helifax's fix, run his uninstall.bat afterwards.


PROBLEMS
--------
- The game only shows on the monitor: your VR runtime is not running or not the active OpenXR runtime.
- Anything else: report it at https://github.com/scubasteve2365/DS3-VR/issues and attach
  Game\ds3vr.log (and Game\ds3vr_prev.log if you relaunched after the problem).


CREDITS
-------
- geo-11 3D driver: davegl1234 and the HelixMod team (personal-use licence, LICENSE.txt in the fix)
- Dark Souls 3 geo-11 fix: Helifax, updated by masterotaku. Please support them.
- 3Dmigoto: bo3b, Flugan and DarkStarSword
- Ideas from Luke Ross's R.E.A.L. VR and praydog's UEVR
- DS3 VR is MIT licensed (LICENSE.txt). Third-party code: THIRD-PARTY-NOTICES.txt
