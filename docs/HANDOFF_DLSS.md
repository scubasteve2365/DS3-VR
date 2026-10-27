# DS3 VR v2: handoff for the DLSS (upscaling) session

Written 2026-10-27 at the end of the DLAA session. Read with `DS3VR/DLAA_FRAME_MAP.md` (how DS3 draws a frame under geo-11) and `DS3VR/V2_ROADMAP.md`.

## Where things stand

| | |
|---|---|
| Repo | `scubasteve2365/DS3-VR` (renamed from ds3-vr; set the remote to the new URL), branch `v2`, latest commit `ad948b0` |
| Public release | 1.0.1 on `main` |
| Steve's Game folder | v2 with DLAA, `dinput8.dll` md5 `7e3ab3809a531b3e5bd5531f27980f89`, also in `D:\Projects\DS3VR\switch\v2\dinput8.dll` |
| Parked for A/B | `D:\Projects\DS3VR\switch\pre-dlaa\dinput8.dll`: commit `ac5c3b6`, the last build before any DLAA code (has first person, world scale 0.25 to 10, draw recording), md5 `4d325116007a7b3f608be3e629b13b15` |
| NGX helper | `G:\...\Game\ds3vr\ds3vr_ngx.dll` (md5 `627ab36867b646902ea27708c13e5afd`), built by `.github/workflows/ngx.yml` on GitHub (Visual Studio), committed to the `ngx-build` branch |
| NVIDIA DLSS dll | `nvngx_dlss.dll` (59 MB) in the Game folder, copied by Steve (too large for the bridge, 30 MB limit). The Nexus mod author gave permission to ship it |
| Leftovers in the Game folder | `d3d11_dlss.dll`, `DS3_DLSS.ini`, `DS3_DLSS.log` from the Nexus DLAA mod. Not loaded (`proxy_d3d11` is commented out in `d3dx.ini`). Can be deleted before release |

## Working rules learned this session

- Claude does every switch and deploy on Steve's PC. Steve does not run the switch scripts.
- **Deploy with a new file name every time.** `device_commit_files` reused a cached copy when the staged file name (`/mnt/user-data/outputs/dinput8.dll`) was the same as an earlier deploy, and reported success. Copy the build to `/mnt/user-data/outputs/dinput8-<something new>.dll`, commit that, then re-stage the target and compare md5. The hash check caught this.
- Heads-up to Steve before each write to his PC (Windows Defender).
- `build.sh` takes 8 to 10 minutes (asmjit). Run it in the background.

## DLAA: done (test-quality, working)

Steve's verdict: visibly less shimmer and aliasing, correct stereo in both eyes, no breakage. Jitter + and Eyes + (the defaults) looked best.

### How it works (`src/dlss.cpp`, `src/dlss.h`, hooks in `src/drawlog.cpp`)

1. **Camera constants.** `cbSceneParam` (register b8, 960 bytes) is found as the game binds it (`VSSetConstantBuffers` slot 8). Uploads are caught in `Map`/`Unmap` and `UpdateSubresource(1)`, on the immediate and deferred contexts (deferred contexts have their own vtable; all hooked). Layout (float4 registers): c1 `FC_ScreenSize`, c3 `SC_CameraPos`, c16 to c19 `FC_MatrixProj`, c24 to c27 `VC_MatrixViewProj`. Each register is one row of clip = M * p. DS3 draws camera-relative (row 3 w = 0).
2. **Jitter.** Halton (2,3), 16 phases, added to rows 0 and 1 of both matrices (`row += j_ndc * row3`) for uploads whose screen size is full size (or half size). Shadow maps and other sizes are left alone. The phase advances right after DLAA runs (so every upload before that point belongs to the frame DLAA resolved), or at Present when DLAA did not run. The jitter reaches both eyes correctly through geo-11 (Steve's jitter test).
3. **Main camera.** About 1 in 5 full-size uploads per frame is another view. DLAA uses the view the frame uploaded most.
4. **Scene targets.** Found in `OMSetRenderTargets` (all contexts): RT0 RGBA16F, 2 slices, with a R32G8X24 2-slice depth of the same size. DS3 also lights a half-size copy the same way; only the largest is kept.
5. **Injection.** First immediate-context `Draw` whose PS slot 0 reads the scene colour (the post chain's first pass, ps114 in the frame map).
6. **Per eye** (in a clean `ID3DDeviceContextState`, so the game's and geo-11's bindings come back untouched): copy the slice to a plain RGBA16F texture, a compute shader (compiled at runtime with d3dcompiler_47) writes R32F depth and camera motion vectors (undoing geo-11's per-eye shift `S * (1 - C / w)`, S = +/- separation per slice, C = convergence, both read from geo-11's StereoParams), DLSS evaluates (one feature per eye, so each has its own history), the result is copied back into the slice.
7. **DLSS settings.** Quality mode DLAA, flags IS_HDR, MV_LOW_RES, AUTO_EXPOSURE. Depth is not inverted (cleared to 1, LESS_EQUAL). History resets on a mode or direction change, a camera jump over 5 m, or the first frame.
8. **Menu.** VR menu row "DLAA (test)": Off / On / Jitter test (8x jitter, DLAA off). With On: direction buttons (Jitter +/-, Eyes +/-) and the model (CNN, Transformer, Driver default). Status line under it. Off frees the DLSS features and work textures.
9. **ds3vr.ini keys:** `DlaaMode` (0 off, 1 DLAA, 2 jitter test), `DlaaJitterSign`, `DlaaEyeSign`, `DlaaPreset` (6 = CNN F, the default; 11 = transformer K; 0 = driver default, which picks K), `DlssSelfTest`.
10. **Log, every 10 s:** camera uploads (main, another view, half, other), DLSS evaluations, stale-camera count, and GPU time (total, DLSS part, copies and motion vectors part) from timestamp queries.

### Measured on Steve's PC (RTX 5090, Pimax Dream Air, 4896x3772 per eye)

| | GPU time per frame (both eyes) | Frame rate seen |
|---|---|---|
| DLAA off | 0 | 62 to 65 fps (CPU-bound, GPU 65 to 70%) |
| CNN (F) | 2.4 to 3.1 ms (DLSS 2.0 to 2.5) | 57 to 62 fps |
| Transformer (K) / driver default | 4.5 to 5.1 ms (DLSS 3.7 to 4.6) | 52 to 56 fps |

Copies and motion vectors cost 0.5 to 0.7 ms. Steve saw no quality difference between the models, so CNN is the default.

### Not done (optional, can come later)

- **Object motion:** enemies and the player have camera motion only, so fast movers can trail slightly. DS3's motion blur pass (when on) draws object velocity with `VC_PrevMatrixWVP` / `cbPrevObjMatrix` constant buffers (seen in the Nexus mod's strings); it was not seen in our recording because Motion Blur was off.
- **Reactive mask** for fire, spells and particles (no motion vectors of their own).
- **Texture mip bias** (the Nexus mod uses -0.5 with DLAA). More important for DLSS upscaling (see below).
- The menu still labels it "(test)". No automatic change of the game's AA setting: the installer already sets Anti-aliasing OFF and Motion Blur DISABLE in GraphicsConfig.xml.

## Performance finding (not caused by the v2 code)

Steve felt v2 got slower. The logs show the game now renders **4894x3772 per eye**, against **3066x2160** in an early v2 run on 2026-10-26 (14:34, a steady 72 fps). In that run the VR runtime started asking for 3772 per eye and the mod moved to it from the next launch (as designed: `RenderHeight=0` follows the runtime). That is about 2.8x the pixels. With DLAA off, today's frame rate (62 to 65) matches the last build before DLAA (64.6 to 65.8) and the Nexus-mod run this morning (60 to 72). A/B check: the parked pre-DLAA build, and/or `RenderHeight=2160` in `ds3vr.ini`, or the runtime's resolution slider.

## DLSS upscaling: plan

The DLAA pieces carry over: NGX helper, jitter, camera motion vectors with the geo-11 shift undone, one feature per eye, clean-state work, GPU timing, menu row. What is new:

1. **Render the 3D scene smaller than the headset image.** DS3's render size is its window size, and every target (scene, post chain, final image) follows it. Two ways:
   - **A. Upscale at the end (recommended first).** Run the whole game at the smaller size (our existing RenderSize code sets the game's resolution), and upscale each eye's final image (after post-processing, before the HUD) to the headset size where `xr.cpp` copies geo-11's shared stereo texture (KATANGA) to the OpenXR swapchain. Needs: the final colour per eye before HUD draws (`R16`, RGBA8, after ps138 at the end of the post chain), depth and motion vectors at render size (we have them), DLSS create flags without IS_HDR (LDR input), output textures at headset size, and the XR copy reading from them. Post effects (bloom, depth of field, film grain) get upscaled with the image; the HUD is already a separate panel, so it stays sharp.
   - **B. Upscale before post-processing** (higher quality, like DLAA's injection point) would need DS3's post-chain and final targets at headset size while the scene renders smaller. Much more invasive; only if A looks wrong.
2. **Mip bias** (texture LOD bias about log2(render/output) - 1) so textures are not blurrier than native. The Nexus mod gives sampler states a biased twin (CreateSamplerState / PSSetSamplers hooks).
3. **Menu:** DLSS modes (DLAA, Quality, Balanced, Performance) with the render size from `GetOptimal`. A size change needs a game restart (the game's resolution is set at launch), so the menu says "applies at next launch".
4. **Release packaging:** ship `ds3vr_ngx.dll` and `nvngx_dlss.dll` in the `ds3vr` folder, credit NVIDIA (licence: object code only, attribution), update the README and installer.

**Estimate:** about 2 to 3 sessions like the DLAA one (DLAA took one long day from frame mapping to a working build), if path A works. The DLAA work removed the riskiest parts (jitter through geo-11, motion vectors per eye, NGX on the game's device). New risks: geo-11 sizing its shared texture from the render size (the XR copy must take the small eyes and the large output), and how blurry post-processing looks once upscaled.

**Expected gain on Steve's PC:** limited, because DS3 VR is CPU-bound there (GPU 65 to 70%). DLSS Quality renders about 44% of the pixels, which removes GPU load, not CPU load. The big win is for players with weaker GPUs, or for Steve if he raises the headset resolution further.
