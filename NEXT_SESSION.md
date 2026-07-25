# Next Session

Updated 2026-07-25 (PM): FFP light mode verified and shipped — current
release is v0.0.1 "Fixed Remix captures and no more API needed" (history
reset; v1.x deleted). See the closed sections below. Read `HANDOFF.md` (project state) first, then
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
`lights/light_*.usd` sublayer files (fix commit f4376cc).
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

**Follow-up done same day:** the api path is retired — FFP is the only
submission mode. `remix_api` module, `deps/bridge_api`, the `[Lights] Mode`
key, and the shipped `bridge.conf` are all removed (commit "Retire the
Remix API light path"). Verified in-game. Release history was reset:
all v1.x releases/tags were deleted and versioning restarted at v0.0.1
("Fixed Remix captures and no more API needed").

## Closed this session — Escape menu visible again (frozen-raster fixed)

Verified in-game 2026-07-25 (PM). The Escape menu turned out to be the
**0x142 XYZ variant** (3 draws/frame, `other:fvf`, no clears at all in a menu
frame — live capture `captures/dxtrace_20260725_144249.jsonl`), so fix-plan
branch 3 applied: the EndScene-fallback `submit_scene_to_remix()` was burying
the menu under the frozen worldrep + lights every paused frame and dropping
the frame to raster (same failure family as the drawn-weapon bug).

**Fix:** the EndScene fallback submission is now gated on
`unproject::m_converted_draws_frame > 0` (world draws this frame — gameplay
frames log world>=27, menu frames exactly 0). The z-only-clear submission
path and the per-frame latch are untouched; menu frames now reach Remix as
just the passthrough 0x142 quads, same as the working launch menu. Full
write-up in `findings.md` ("Escape menu showed frozen raster world").

**Invariant: keep the EndScene fallback gated on world draws** (and still no
per-scene latch — both invariants now live in findings.md).

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
