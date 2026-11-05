# CLAUDE.md — SkyRecomp VR fork

Guidance for Claude Code (and humans) working on this repo.

## What this is

A fork of [SkyRecomp](https://github.com/Apolinario-coder/SkyRecomp) (static AArch64 → C recompilation of *Skyward Sword HD* v1.0.0) that adds **PC VR through OpenXR** (tested with SteamVR, Valve Index headset + controllers).

- `upstream` = Apolinario-coder/SkyRecomp, `origin` = marclemieux64/SkyRecomp
- VR work lives on `vr-minimal` (and snapshot branches like `vr-snapshot-*`).
- No game files are in the repo. Never commit extracted/romfs data.

## Build & run

- `make` builds `build/skyrecomp.exe` (see `Makefile`, `docs/`).
- Enable VR: `SKYRECOMP_VR=1` (SteamVR must be running).
- Unattended test runs: launch hidden (`Start-Process -WindowStyle Hidden`), **no stdout redirection** (the game may re-exec itself and block on a full pipe); use `SKYRECOMP_CONSOLE=0` so it writes `build/skyrecomp.log`.
- Useful env: `LARECOMP_NO_LAUNCHER=1`, `LARECOMP_NO_AUDIO=1`, `LARECOMP_NO_UPDATE_CHECK=1`, `LARECOMP_SAVE_DIR=<copy of a save>`, `LARECOMP_SETTINGS=<copy of ini>`. Never test against a user's real save.
- Scripted input: `SKYRECOMP_INPUT_RECORD=<file>` records; replays only stay reliable for short menu/walk sequences.
- `SKYRECOMP_VR_NO_INPUT=1` stops controller motion pressing buttons; `SKYRECOMP_JOYCON=0` skips the motion calibration screen.
- Run **one game instance at a time** — two instances fight over the headset.

## Code map (VR)

| Path | Role |
|---|---|
| `runtime/vr/vr_runtime.c`, `vr.c`, `vr_xr.h` | OpenXR session, swapchains, frame loop |
| `runtime/vr/vr_rig.c` | stereo camera rig: replaces the game's view per eye, 1st/3rd person, reflection detection |
| `runtime/vr/vr_input.c`, `vr_controls.c` | OpenXR actions → virtual Joy-Con buttons and motion |
| `runtime/vr/vr_hud.cpp` | 2D layer (HUD/menus) captured to a texture, drawn as a panel inside the eye images |
| `runtime/vr/vr_stereo_cull.cpp`, `runtime/hle/vr_cull.c` | culling with the headset frustum, draw-distance setting |
| `runtime/hle/vr_world.c` | world kept visible behind pause menus |
| `runtime/hle/motion.c` | Joy-Con six-axis emulation (game's native motion controls) |
| `runtime/hle/player.c` | Link's pose and game camera readers |
| `runtime/hle/door.c` | physical VR doors (grab a handle, swing the leaf) |

## Known game facts (measured, SS HD v1.0.0)

- Scene camera = **vertex UBO slot 0** (864 bytes): floats 0–15 row-major projection (reversed-Z), 16–27 row-major 3×4 view. Code inherited from the Link's Awakening recomp (e.g. `widescreen.c`) assumes otherwise — ignore it.
- True game camera view (3×4) at **`0x7002bfda90`** (arena, stable). Many draw blocks are view×model, not the camera.
- Link's eye point: **`main+0x1829a30`** (vec3; y = feet + 150). Actor ptr `*(main+0x18299d0)`, feet height actor+0x154. `main+0x182a9b0` is a wrong lead.
- Pause/menu flag: `main+0x1827370 != 0`. Item-aim flag: `main+0x1939760 == 1`.
- 2D layer = 192-byte ortho slot-0 draws, last in the frame, RGB-only write masks.
- Actor culling: camera via `main+0x2bf9958`, test `main+0xdf5ab4`, per-actor draw distance actor+0x23c. Level geometry is not frustum-culled.
- Joy-Con dual style needs NpadAttribute bits IsLeftConnected (2) and IsRightConnected (4). Menu pointer is absolute (K = 1.298 widths / 2.316 heights × tan); item aiming is relative and gated by the gyro.

Verify any claim against a live trace (`SKYRECOMP_VR_TRACE=1`, `SKYRECOMP_VR_CAM_TRACE=1`, `SKYRECOMP_PLAYER_TRACE=1`) before building on it.

## Design decisions

- **HUD drawn into the eyes**, not as an OpenXR quad layer: SteamVR quad layers made the content judder.
- **Native motion controls**: VR controllers drive the game's own Joy-Con motion scheme, not remapped buttons.
- 144 Hz is reachable; a 57 fps cap was caused by the desktop mirror's FIFO vsync (AMD).

## Conventions

- Commit style: `feat(vr): …`, `fix(vr): …`, `tune(vr): …` — one-line summary describing the visible effect.
- Hook game functions with `hle_override` at vtable-referenced functions (direct `bl` calls can't be hooked).
- Keep VR changes behind `SKYRECOMP_VR` / settings so the flat game is unaffected.
