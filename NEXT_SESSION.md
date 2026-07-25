# Next Session

Written 2026-07-25. Read `HANDOFF.md` (project state) first, then
`PHASE4_LIGHTING_HUD.md` (overlay/injection architecture) if unfamiliar.

## Closed this session — drawn weapon no longer breaks path tracing

Verified in-game. Root cause was **duplicate scene submission**, not the
weapon's draw calls: equipping a weapon adds two off-screen `BeginScene`/
`EndScene` passes (item icon), and the Remix submission latch reset per scene,
so the worldrep + light set went in three times a frame. The latch now resets
at `Present`. Full write-up with the tracer evidence is in `findings.md`
("Drawn weapon dropped the frame to raster").

Two prior fixes built on a disproven "second perspective camera" theory were
deleted along with the `viewmodel` module and its `[Viewmodel]` INI section.
Do not rebuild them without re-reading that section — suppressing the weapon's
draws was measured to change nothing.

**Invariant: do not reintroduce a per-scene submission latch.**

Also fixed on the way through: `viewmodel::flush` used to set `ZENABLE`,
`CULLMODE`, `FOGENABLE` and texture-stage states without restoring them, which
leaked depth-testing-off into the HUD flush and the following frame (visible as
"meshes through walls" plus a large red quad). The module is gone, but the
lesson applies to any future EndScene replay path: **restore every state you
set** — `unproject::flush_overlay_ui` is the reference implementation.

## Open — FFP light mode, built + deployed, still needs the A/B test

Unchanged from 2026-07-24; nothing this session touched it.

**Problem:** toolkit captures contain no lights. Remix-API lights
(`CreateLight`/`DrawLightInstance`) are structurally excluded from captures —
the capturer iterates only the game FFP light table and the internal
"externally tracked" table, never `m_externalLights` (verified in dxvk-remix at
the deployed tag `remix-1.5.2` AND current main — upgrading won't help). Only
D3D9 fixed-function `SetLight`/`LightEnable` lights are captured and matched
against toolkit light replacements.

**Built:** `[Lights] Mode=api|ffp` (default `api`, unchanged behavior). `ffp`
submits the engine light table via `SetLight(i)`/`LightEnable(i)` before the
worldrep draws so the runtime's DirtyLights flush lands with this frame's
geometry. Brightness is calibrated to match the api path: with Attenuation0=1
the runtime uses Range as the attenuation end-distance,
radiance = 0.01/(pi*r^2)*Range^2 with r =
`rtx.lightConversionSphereLightFixedRadius`; the proxy inverts that, so rtx.conf
pins the option to EmitterRadius (0.4). Light hash = f(position, cone shaping) —
color excluded, so torch flicker keeps a stable capture/replacement identity.
Spots map Theta=2*acos(+0x24), Phi=2*acos(+0x28).

**Test:** set `Mode=ffp` in the game-dir INI, launch, load a mission. Expect
identical-looking lighting (flicker included). Then Ctrl+Shift+Q capture and
confirm SphereLight prims in the capture USD
(`grep -ac SphereLight rtx-remix/captures/*.usd` > 0) and lights visible in the
toolkit. If brightness deviates, tune RadianceScale (conversion clamps at
`rtx.lightConversionMaxIntensity`). If ffp matches api visually, make `ffp` the
default and consider retiring the api path + `bridge.conf` requirement.

Files: `engine_lights.{hpp,cpp}`, `d3d9ex.cpp`, `config.{hpp,cpp}`,
`assets/remix-comp-proxy.ini`, `assets/rtx.conf`.
Backup: `backups/2026-07-24_1520_ffp-light-mode/`.

## Open — Escape menu (and HUD) invisible in-mission, diagnosed, not fixed

**Symptom:** pressing Escape in a mission shows no menu/HUD graphics — the
frozen world keeps displaying. The LAUNCH main menu works; only the in-mission
Escape menu is blank.

**Root cause (from code + a 470-frame menu-only `unproject_debug.log`):**

1. Menu screens are full-screen 2D quads drawn via DrawPrimitiveUP *outside*
   the overlay phase — paused frames issue no mid-scene z-only Clear (that
   clear precedes HUD model draws, and HUD submission stops when the sim
   pauses). So `unproject` captures nothing for the ortho EndScene replay.
2. The quads pass through raw. Two engine variants in the log (same 3
   quads/frame: textured background + two pillarbox bars):
   - `ui:flat-rhw` — FVF 0x144 (XYZRHW|DIFFUSE|TEX1), stride 28, depth exactly
     [1.00..1.00], **ZENABLE=0**. Raw RHW draws are silently DROPPED by this
     Remix build (PHASE4 pitfall #1) → invisible.
   - `other:fvf` — FVF 0x142 (XYZ|DIFFUSE|TEX1), stride 28, v0=(0,0). Non-RHW
     passthrough; renders raster. 1410 of these vs 6 RHW in the log.
3. **Why the launch menu works but the Escape menu doesn't:**
   `submit_scene_to_remix()` has no pause/menu guard. In-mission the worldrep is
   loaded and the camera valid, so the frozen scene + lights are submitted every
   menu frame AFTER the mid-scene menu draws — Remix's RTX injection then covers
   whatever menu pixels survived. At the launch menu `worldrep_render::submit`
   bails (no mission geometry), so the passthrough 0x142 quads are all Remix
   sees → visible.

**Fix plan (diagnostic first):**

1. Set `[Unproject] DebugLogFrames=5`, load a mission, hit Escape. The existing
   log only covers the LAUNCH menu — the Escape menu's actual draw signature
   (0x144 vs 0x142, zenable, depth) is needed before coding. Note
   `dbg_frame_boundary` only decrements armed frames when a frame has ≥30
   draws, so menu frames log freely; a large value (e.g. 1500) logs continuously
   through gameplay if you need to catch a transition.
2. If the Escape menu draws are RHW (0x144): capture instead of passing through
   when `ZENABLE==FALSE && flat rhw && depth≈1.0` — queue into
   `m_overlay_draws` so `flush_overlay_ui` replays them at EndScene after scene
   submission (the exact path the HUD already uses). This signature does not
   collide with pitfall #6 (distant world fans are flat-rhw at LARGE depth with
   Z enabled) or with HUD models (Z enabled, overlay phase).
3. If they are the 0x142 XYZ variant: they already render but get overwritten by
   the scene submission — either defer/replay them too, or skip
   `submit_scene_to_remix` for scenes that contained menu-signature draws and no
   overlay clear. **Careful:** any such guard must not break the weapon fix
   above — the submission must still happen exactly once on normal frames.
4. Frame-level fallback if the per-draw signature proves ambiguous: defer ALL
   flat-rhw draws to EndScene; drop them if the scene had an overlay clear
   (today's behavior — Remix drops them anyway), replay them ortho if it had
   none (menu frame). Behavior-identical in gameplay frames.

## Backlog

1. True path-traced viewmodel: give the weapon Remix's view-model treatment
   instead of leaving it as ordinary geometry. Note the deleted `viewmodel`
   module is **not** a useful starting point — it was built to solve a problem
   that turned out not to exist.
2. Objects still use per-frame `unproject` reconstruction (hash churn).
3. DrawPrimitiveUP perf.

## Resume checklist

- Working dir: `patches/ThiefGold` (its own git repo; parent repo ignores it).
- Build: `build.bat release --name ThiefGold` → `build/bin/release/d3d9.dll`.
  If you delete a source file, remove its stale `build/obj/release/comp/*.obj`
  or the PCH check fails the link.
- Deploy: copy `d3d9.dll` + `assets/remix-comp-proxy.ini` to
  `F:\SteamLibrary\steamapps\common\thief_gold` (game closed — the DLL is
  locked while it runs).
- Live tracer capture needs no relaunch:
  `python -m graphics.directx.dx9.tracer trigger --game-dir <DIR> --frames 3 --wait`
- Pitfall reminders: don't let a Remix-menu "Save Settings" delete `rtx.conf`;
  game sim freezes on focus loss; runtime image base varies per launch.
