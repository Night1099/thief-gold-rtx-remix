# ThiefGold RTX Remix Port — Session Handoff

Last updated: 2026-07-18. Read this first, then `PHASE4_LIGHTING_HUD.md`
(lighting/HUD architecture + pitfalls), `findings.md` (detailed technical log)
and `kb.h` (addresses/structs). Self-contained remix-comp-proxy project under
`patches/ThiefGold/`.

## What this project is

Porting **Thief Gold (NewDark 1.27)** to render through **RTX Remix** (path
tracing). The Dark engine is a software-T&L engine: it CPU-transforms geometry
to screen space and feeds Remix un-raytraceable pre-transformed (XYZRHW)
vertices. A custom `d3d9.dll` proxy reconstructs real world-space geometry so
Remix can path-trace it.

## Current state — WORKING through phase 4

1. **Setup**: Steam Thief Gold + TFix Lite (NewDark 1.27), windowed 1920x1080.
   `cam_ext.cfg` has MSAA/atoc/distortion off, `force_windowed`. Do NOT run
   Steam "verify integrity" (reverts NewDark).
2. **Path tracing works**: RTX Remix 1.5.2 (`d3d9_remix.dll` + `.trex/`).
   Required confs in game dir: `bridge.conf` (`exposeRemixApi = True` — Remix
   API dies with code 11 without it), `rtx.conf` (`rtx.orthographicIsUI = True`
   — the HUD depends on it; canonical copy in `assets/rtx.conf`).
   **PITFALL**: "Save Settings" in the Remix menu writes `user.conf` and can
   DELETE `rtx.conf` (observed 2026-07-18 — HUD/exposure keys silently lost).
   After any Remix-menu save, verify `rtx.conf` still exists; restore from
   `assets/rtx.conf` if not. `user.conf` overrides `rtx.conf` on overlap.
3. **Geometry is stable-hash** (phase 3): world submitted from the resident
   worldrep cell array as whole polygons, textured from engine bitmaps.
   Geometry cache revalidates via a per-cell-count fingerprint each frame —
   cells populate after the array base/count are final, so a mid-load build
   would otherwise snapshot a partial world (the "missing ground" bug).
4. **Engine lights drive the path tracer** (phase 4): `engine_lights` module
   mirrors the runtime light table (`0x9EA660`, count `0xA02CA0`) into Remix
   sphere/spot lights every frame; torch flicker tracks automatically (the
   engine rewrites table colors per frame). `[Lights]` INI: `RadianceScale=10`,
   `SkipInfinite=0` (radius 0 is common on real lights), `ForceSpot=0`
   (optional: shape directed records as spots — tested, spheres preferred).
   Remix auto-exposure disabled in rtx.conf (fights stealth darkness).
5. **Lightmap albedo attenuation** (darkness calibration): worldrep vertices
   carry diffuse color = engine static lightmap luminance, so designed-dark
   areas eat bounce light. `[Worldrep] LightmapAttenuation=0.7` (0=off).
   WRLightmapInfo fully decoded in `worldrep.hpp`; base layer only, animlight
   dynamics come from the path-traced lights. Confirmed working in-game.
6. **HUD works over the traced scene** (phase 4): overlay pass detected via the
   engine's mid-scene z-only `Clear`, world+lights submitted BEFORE the first
   UI draw (UI triggers Remix RTX injection), overlay draws captured and
   replayed at EndScene depth-sorted as ortho UI. Gem/shields stable at all
   pitches, no flicker. See `PHASE4_LIGHTING_HUD.md` for the six pitfalls —
   do not re-learn them.
7. **White untextured world plane solved** (task #6): the plane was the Dark
   engine's sky-hack surface, worldrep texture id 249. `[Worldrep]
   SkipTexIds=249` drops it from submission (comma-separated list; the proxy
   log's "Buckets (texId:tris)" line lists ids). Sky rendering itself is
   Remix-side (`rtx.conf`).
8. Objects (characters/items) still use per-frame `unproject` reconstruction
   (expected hash churn; later phase). Weapon viewmodel renders flat as part of
   the overlay pass (vanilla look).
9. **Camera bob/sway solved** (2026-07-18): unprojected characters used to bob
   while walking and sway during mouse-look. Root cause: `read_camera()` read
   the engine's motion-detection snapshot (`0x8CD0F4`), which excludes the
   dynamic eye offset (head-bob + crouch) and lags the render camera ~100ms
   (~15° mid-turn). Now reads the live render camera (see addresses below);
   fix live-verified in-game. Side benefits: correct standing eye height
   (+2.6) and crouch height in the reconstruction.

## Build / deploy / test

```
cd patches/ThiefGold
build.bat release --name ThiefGold          # -> build/bin/release/d3d9.dll  (warning-clean, /W4 /WX)
```
Deploy `d3d9.dll` (+ `remix-comp-proxy.ini` if changed) to
`F:\SteamLibrary\steamapps\common\thief_gold` (game must be closed). Keep
`bridge.conf`/`rtx.conf` intact (see above). The user launches + loads a
mission; a console window shows the proxy log.

INI toggles (`[Worldrep]`, `[Unproject]`, `[Lights]`, `[Remix]`) allow falling
back without a rebuild. `[Unproject] DebugLogFrames=N` dumps per-draw
classification to `rtx_comp\unproject_debug.log` — the main diagnostic tool.
`rtx_comp\console.log` shows the worldrep build: "Built geometry" (a second
line mid-mission = the fingerprint rebuild self-healing a mid-load snapshot),
"Buckets (texId:tris)" (all texture ids by size), and "SkipTexIds dropped".

## Key modules (edit these)

- `src/comp/modules/engine_lights.cpp` + `src/comp/game/lights.hpp` — light
  table → Remix lights (phase 4).
- `src/comp/modules/unproject.cpp` — RHW→world reconstruction (objects) +
  overlay capture/sorted ortho-UI flush (phase 4 HUD).
- `src/comp/modules/worldrep_render.cpp` — phase-3 world submission.
- `src/comp/modules/d3d9ex.cpp` — overlay-clear detection, scene-submission
  ordering (`submit_scene_to_remix`), EndScene overlay flush.
- `src/comp/game/worldrep.hpp` / `game.{hpp,cpp}` — structs, camera, `rva()`.
- `src/shared/common/remix_api.{hpp,cpp}` — Remix bridge (CreateLight etc.).

## Verified addresses (static VAs, image base 0x400000; runtime +0x480000)

- **Camera — use the LIVE render camera**: pos `0xC19AC0` (float3, z-up,
  written per frame by `WRCacheRenderCam 0x4CDC40`, includes head-bob + crouch
  eye offset), pitch `0xC21B52` (s16, up=neg), yaw `0xC21B54` (u16). Dark
  angle ×2π/65536. The older cache (pos `0x8CD0F4`, pitch `0x8CD0E6`, yaw
  `0x8CD108`) is the camera object base origin: no bob/crouch offset, ~100ms
  desync while moving/turning — using it made unprojected characters bob when
  walking and sway during mouse-look (fixed 2026-07-18). It remains only as
  the pre-first-frame fallback in `game::read_camera()`.
- Worldrep: `g_wrCells` `0xA35EE0`, count `0xA5DEB8`. Texture resolver slot
  `0x7A84D8`. World DPUP flush (suppressed) `0x60CE80`–`0x60CFC8`; object+HUD
  flush call site `0x60CE4F`.
- **Lights**: table `0x9EA660` (stride 0x30: pos +0x00, spot dir +0x0C, RGB
  +0x18, cos inner/outer +0x24/+0x28 [-1=omni], radius +0x2C [0=unbounded]),
  count `0xA02CA0`. AnimLight: tick `0x5A2180`, apply `0x58D2E0`, brightness
  array ptr `0xA7152C`. NOTE: `0x9CE4F4` is NOT the count (constant 2048).
- **Sky**: `g_wrSkyTexId` `0x7A803C` = 0xF9 (249) when NewDark sky on, else
  0xFFFFFFFF; toggle `g_newSkyEnable` `0x78CC94`. Id 249 is hardcoded —
  mission-independent, so `SkipTexIds=249` is always safe.
- **Lightmaps**: WRLightmapInfo fully decoded (see `worldrep.hpp` / findings
  "Worldrep lightmap in-memory layout"). Depth mode `g_lm32bit` `0x9CDC48`
  (default 1 → 4bpp ARGB). Parsers `0x54A240`/`0x54A900`, UV reader
  `0x4DD100`, freed only by `WorldRep_Free` `0x549BE0` (stable per mission).
- Engine sim + HUD submission pause when the game window loses focus — live
  traces need forced focus.

## NEXT: ideas backlog (priority order)

1. **Object darkness parity**: world darkens via lightmap attenuation, but
   objects (unproject path) get no equivalent — they read too bright in dark
   rooms. Dark bakes per-vertex lighting into object RHW draws; try Remix's
   "Vertex Color is Lightmap" toggle first (verify the option exists in the
   DEPLOYED .trex binary's strings), else attenuate object albedo like the
   world path.
2. **Per-light radiance from engine falloff radius**: light table `+0x2C`
   holds each light's designed reach and is currently unused (one global
   `RadianceScale`). Scale radiance per light so braziers throw far and
   candles stay local.
3. **Light-gem ground truth**: find the gem value in memory (lead:
   `Light_BrightnessAtPoint 0x5A9390`), read it live, spot-check that
   "gem dark" spots look dark on screen. Makes calibration measurable.
4. **Ambient floor**: mission ambient RGB `0xB22820` (default 0x80) as a very
   dim Remix dome light so unlit areas aren't crushed to pure void.
5. **Attenuation polish**: bilinear lightmap sampling (nearest-neighbor now),
   exponent/gamma knob for contrast; large convex fans interpolate vertex
   color linearly — subdivision if smearing shows.
6. Lower priority: weapon viewmodel via Remix view-model treatment,
   DrawPrimitiveUP perf, object-path hash stability.
7. **Housekeeping**: today's work (SkipTexIds, mid-load fingerprint rebuild,
   ForceSpot, lightmap attenuation) is uncommitted — commit the working state.
   Stray note-files litter the repo root (`"g_wrCells @ ..."` etc.) — ask the
   user before deleting.

## Working method notes

- Main agent owns livetools (live verify/patch) + dx9tracer + user interaction.
  Delegate heavy static analysis to `static-analyzer` subagents (Ghidra project
  + index.db exist; prefer `python -m retools.query ThiefGold "SQL"`).
- Always verify static findings live before writing modules against them — and
  verify Remix config options against the DEPLOYED binary's strings, not
  upstream dxvk-remix source (phase-4 lesson).
- The user launches/closes the game and reports what they see; drive tests
  through them. Screenshots via `CopyFromScreen` on the (windowed) game work
  well for visual verification.
