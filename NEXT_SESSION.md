# Next Session

Updated 2026-07-25 (PM): FFP light mode verified and released as v1.2.0 —
see the closed section below. Read `HANDOFF.md` (project state) first, then
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

## Closed this session — FFP light mode verified, captures contain lights

Verified in-game 2026-07-25 (`Mode=ffp`, mission lighting matches api path)
and in a capture: `captures/capture_2026-07-25_13-49-14.usd` references 362
`lights/light_*.usd` sublayer files. Released as v1.2.0 (commit f4376cc).
Note: light prims live in the capture's `lights/` subfolder — a grep of the
top-level .usd alone finds only the sublayer reference.

The first ffp test showed almost no lights ("torches only", then "black
world" — both were the same symptom: near-zero lights landing). Two
independent runtime constraints, both fixed:

1. **GC on untouched lights.** The runtime re-adds FFP lights only on a
   DirtyLights frame (a frame with a `SetLight` on an enabled light) and
   garbage-collects lights untouched for `rtx.numFramesToKeepLights` (100)
   frames. `submit_ffp` now re-issues `SetLight` for every lit record every
   frame — do not reintroduce a change-gate.
2. **8-light enable cap.** dxvk-remix's `enabledLightIndices` slot table is
   sized by `d3d9.maxEnabledLights` (default 8); `LightEnable` beyond 8
   concurrent lights silently no-ops (`d3d9_device.cpp` LightEnable — no
   free slot, returns D3D_OK). `assets/rtx.conf` now sets
   `d3d9.maxEnabledLights = 768` (= LIGHT_TABLE_CAPACITY); the runtime reads
   `d3d9.*` options from rtx.conf (verified in the remix-1.5.2 log:
   "Effective Combined Config for DXVK Options").

**Follow-up decision, not yet made:** flip the shipped INI default from
`Mode=api` to `ffp` and retire the api path + `bridge.conf`
(`exposeRemixApi`) requirement. v1.2.0 still ships `Mode=api` default; the
game-dir INI is on `ffp`.

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

1. Capture the Escape menu's draw signature. The 2026-07-25 attempt with
   `DebugLogFrames=5` failed to catch it: launch-menu frames log freely (<30
   draws don't decrement the armed count), so the log burned ~575 launch-menu
   frames and then the 5 armed frames on the first gameplay frames — it ended
   before Escape was pressed. Use a large value (e.g. 1500) and hit Escape
   promptly after loading. The launch menu showed both variants again
   (`ui:flat-rhw` 0x144 pre-resolution-change, `other:fvf` 0x142 after);
   in-mission Escape signature still unknown.
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
