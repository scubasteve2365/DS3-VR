DS3 VR: DARK SOULS III in VR
============================

True stereo VR for Dark Souls III. Both eyes are rendered from the same game frame, so there is no
alternate-eye shimmer. The camera follows your head, the HUD sits on a floating panel you can size
and place, and the game runs at your headset's refresh rate. On NVIDIA RTX graphics cards, DLAA
smooths jagged edges and DLSS upscaling raises the frame rate. You play with a gamepad; there are no
motion controls.

Project page, updates and bug reports: https://github.com/scubasteve2365/DS3-VR


WHAT YOU NEED
-------------
- DARK SOULS III on Steam (the current version, 1.15.2)
- A PC VR headset with an OpenXR runtime set as the active one: SteamVR, Virtual Desktop (VDXR),
  Meta Quest Link or PimaxXR. Thoroughly tested on SteamVR; the others should work but have not been
  fully tested yet.
- A gamepad (Xbox, or others through Steam Input)
- For DLAA and DLSS (optional): an NVIDIA RTX graphics card and a recent driver. Everything else works
  on any graphics card.
- Helifax's Dark Souls 3 geo-11 fix: the install step downloads it for you (see step 2)


STEP 1: EXTRACT DS3 VR
----------------------
Extract the whole DS3 VR zip into the game's Game folder, the folder with DarkSoulsIII.exe.
You get dinput8.dll, install-ds3vr.bat, uninstall-ds3vr.bat and a ds3vr folder next to it (the ds3vr
folder also holds the two NVIDIA DLSS files).

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
  Menu tabs ........... LB / RB or LT / RT (keyboard: Page Up / Page Down)
  Recenter ............ your runtime's recenter, the menu, or F8
  HUD size / mode ..... the menu, or F6 / F7

The VR menu has three tabs. Every change is saved straight away.
- General: world scale (0.25 to 10; 1.0 is life size, higher makes the world smaller with more 3D
  depth), decoupled pitch, head movement, the HUD (mode, size, distance, height) and recenter.
- Graphics: NVIDIA DLSS, DLSS model, sharpening and the resolution readout.
- Developer: test settings for troubleshooting. Normal play does not need them.

Decoupled pitch keeps the horizon level, so the right stick never tilts the view up or down; only
your head does. "Level" still lets the stick raise and lower the camera around your character;
"Level + stick lock" ignores the stick's up and down entirely. Off is the flat game's camera.


NVIDIA DLSS (RTX GRAPHICS CARDS)
-------------------------------
Graphics tab, NVIDIA DLSS:
- Off: the game's own image.
- DLAA: anti-aliasing at full resolution. Edges and fine detail stop shimmering; costs a little
  frame rate. Takes effect straight away.
- Quality, Balanced, Performance, Ultra Perf: upscaling. The game renders each eye smaller and DLSS
  rebuilds it at your headset's resolution, for more frame rate. Takes effect the next time you
  start the game.
DLSS model: CNN is the default and the fastest; Transformer can look a little better and costs more.
Sharpening (Off to Max) works with any setting, DLSS or not, if the image looks too soft for you.
The game's own anti-aliasing should stay off (the install step sets it off).


RESOLUTION
----------
The VR menu shows the resolution per eye: what the game renders now, what your VR runtime asks for,
and what the next launch will use. To change it, change the resolution in your runtime (SteamVR,
Virtual Desktop, the Meta app or Pimax Play) and restart the game.

With a DLSS upscaling mode on, the menu shows both sizes: what the game renders, and what your
headset gets after DLSS.


UPDATING
--------
Extract the new DS3 VR zip over the old one and overwrite the old files. Your settings are kept.
From 1.0.x to 2.0 you do not need to run install-ds3vr.bat again.


UNINSTALL
---------
Run uninstall-ds3vr.bat in the Game folder. It puts Helifax's files and your graphics settings back
and removes DS3 VR. To also remove Helifax's fix, run his uninstall.bat afterwards.


TROUBLESHOOTING
---------------
The game crashes on start, or renders at the wrong resolution:
  DS3 VR reads the render resolution from your VR runtime as the game starts, and occasionally it
  does not arrive in time. This mostly happens on the first launch after installing or updating.
  Close the game and launch it again; once it has started correctly it stays correct until the next
  update or reinstall. The VR menu's Graphics tab shows whether the next launch will use a
  different size.

The game only shows on the monitor:
  Your VR runtime is not running, or it is not the active OpenXR runtime. In SteamVR: Settings >
  OpenXR > Set SteamVR as OpenXR runtime. Start the runtime before the game.

DS3 VR says it is off and asks you to run the install:
  Run install-ds3vr.bat in the Game folder. If you reinstalled Helifax's fix, run it again.

The game crashed after changing graphics settings:
  Do not change the resolution or screen mode in the game's own menu while playing. Restart the
  game; the mod sets the size itself.

Low frame rate:
  Lower the resolution in your VR runtime and restart the game, or pick a DLSS upscaling mode
  (Quality or Balanced) on an RTX card and restart. DLAA costs a little frame rate rather than
  saving it. Close other VR apps and overlays you are not using. The first launch after installing
  can stutter while geo-11 prepares its shaders.

DLSS options are unavailable:
  DLSS needs an NVIDIA RTX graphics card and a recent driver. The status line under the DLSS setting
  says why it is off. Check that ds3vr_ngx.dll, nvngx_dlss.dll and NVIDIA_DLSS_LICENSE.txt are in
  Game\ds3vr. An upscaling mode only applies after a restart.

The image looks soft:
  Try Sharpening on the Graphics tab (Low or Medium is usually enough).

The view is tilted or off to one side:
  Recenter from the VR menu, with F8, or with your runtime's recenter. If camera pitch from the
  right stick is uncomfortable, try Decoupled pitch on the General tab.

Another mod stopped working, or DS3 VR did not load:
  DS3 VR is dinput8.dll in the Game folder. Other mods that use a dinput8.dll there (such as the
  original DS3 Mod Engine) replace it, and only one can be installed at a time.

Windows or your antivirus removed or blocked a file:
  Mods that hook into a game are sometimes flagged. Restore dinput8.dll (or ds3vr\ds3vr-patcher.exe)
  from the zip, and add an exception if you trust the download. Only download DS3 VR from
  https://github.com/scubasteve2365/DS3-VR/releases

Anything else:
  Report it at https://github.com/scubasteve2365/DS3-VR/issues and attach Game\ds3vr.log (and
  Game\ds3vr_prev.log if you relaunched after the problem).


SUPPORT
-------
DS3 VR is free. If you enjoy it and want to support more VR mods:
https://buymeacoffee.com/scubasteve2365


CREDITS
-------
- geo-11 3D driver: davegl1234 and the HelixMod team (personal-use licence, LICENSE.txt in the fix)
- Dark Souls 3 geo-11 fix: Helifax, updated by masterotaku. Please support them.
- 3Dmigoto: bo3b, Flugan and DarkStarSword
- NVIDIA DLSS: NVIDIA Corporation (NVIDIA_DLSS_LICENSE.txt in the ds3vr folder)
- Sharpening: AMD FidelityFX Contrast Adaptive Sharpening
- Ideas from Luke Ross's R.E.A.L. VR and praydog's UEVR
- DS3 VR is MIT licensed (LICENSE.txt). Third-party code: THIRD-PARTY-NOTICES.txt
