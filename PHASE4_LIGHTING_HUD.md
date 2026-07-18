# Phase 4 — Engine Light Injection + HUD Overlay (2026-07-17/18)

What was built, how it was found, and every pitfall hit along the way. Companion
to `HANDOFF.md` (current state) and `findings.md` (raw evidence log). Addresses
are static VAs (image base 0x400000; runtime +0x480000 confirmed).

## Accomplished

1. **Thief's real lights now drive RTX Remix path tracing.** The proxy reads the
   engine's runtime light table every frame and mirrors each active light into
   Remix as a sphere light (spotlights get cone shaping). Animated lights
   (torch flicker) work with zero extra machinery because the engine rewrites
   their color fields in the same table each frame.
2. **The full HUD renders correctly over the path-traced scene** — health
   shields, light gem (with its engine-driven stealth reading), at every camera
   angle, no flicker — via a proxy-side ortho UI pipeline built to match how
   the deployed Remix runtime actually classifies UI.

## The light system (what to know)

- **Master light table** `g_lightTable @ 0x9EA660`, stride 0x30, capacity 768
  (loader reads 0x9000 bytes). **Count at `0xA02CA0`** — live-verified as the
  exact table boundary. (`0x9CE4F4` reads a constant 2048 and is NOT the count;
  the first static-analysis pass mislabeled these.)
- Record layout (live-verified): `+0x00` pos xyz · `+0x0C` unit spot direction
  (zero for omni) · `+0x18` color RGB in engine units (~0..5 observed) ·
  `+0x24` cos(inner cone half-angle), **-1.0 = omni** · `+0x28` cos(outer) ·
  `+0x2C` radius/range, **0 = unbounded and COMMON on normal lights** (torches!).
- Animated lights: `AnimLight_Tick 0x5A2180` (per frame) →
  `AnimLight_ComputeBrightness 0x58D4F0` (flip/slide/random modes) →
  `AnimLight_Apply 0x58D2E0` writes color into the table + a brightness byte
  array (ptr @ `0xA7152C`). Colors are zeroed while a light is off — the
  module's "lit" check is simply any color component > 0.
- Module: `src/comp/modules/engine_lights.{hpp,cpp}` + `src/comp/game/lights.hpp`.
  Stable Remix hash = constant base + table index; light handles are recreated
  only when a record's pos/color/radius actually changes. INI: `[Lights]`
  (`Enabled`, `RadianceScale` — engine units → radiance, default 20,
  `EmitterRadius`, `SkipInfinite`).

### Lighting pitfalls solved

| Pitfall | Resolution |
|---|---|
| Remix API failed: `Code 11 (NOT_INITIALIZED)` | The bridge requires **`bridge.conf` with `exposeRemixApi = True`** in the game dir (file didn't exist at all). Without it `CreateLight` silently never runs. |
| Torches invisible with `SkipInfinite=1` | Radius 0 does **not** mean "ambient only" — most normal lights (incl. torches) have radius 0. Default is now off. |
| "Anim marker" misread | `+0x24 = -1.0` is cos(180°) = omni light, not a static/animated flag. The middle fields are a spotlight definition. |
| Engine sim pauses on focus loss | The sim clock (and HUD submission) freeze when the game window loses focus — all live tracing/captures of per-frame behavior need the window focused (scripted via `SetForegroundWindow` + Alt-key unlock). |

## The HUD saga (the hard one)

Symptom chain: HUD invisible → visible but dark, side-lit by torches, swallowed
by the floor when pitching down → each "obvious" fix failed until the real
mechanisms were pinned down. Final architecture:

1. **Overlay detection**: the engine issues a **mid-scene `Clear(D3DCLEAR_ZBUFFER
   only)` immediately before drawing HUD models** (frame-start clears carry
   TARGET|ZBUFFER). `d3d9ex::Clear` flags the overlay phase; `BeginScene` resets it.
2. **Scene-first ordering**: worldrep geometry + engine lights are submitted at
   that overlay clear (with an EndScene fallback + per-scene latch) — **before**
   any UI draw, because UI triggers Remix's RTX injection and everything after
   it is raster, not path-traced.
3. **UI conversion**: overlay DPUPs are captured (suppressed at their call
   site), converted RHW→XYZ, and **replayed at EndScene sorted far-to-near by
   engine screen-z**, under an explicit orthographic projection with Z off —
   Remix classifies ortho draws as UI (`rtx.orthographicIsUI = True` in
   rtx.conf) and rasterizes them over the traced scene.

### HUD pitfalls solved (each cost a debugging round)

1. **`rtx.preTransformedVerticesIsUI` does not exist in the deployed Remix
   runtime.** Phase-1 findings were based on newer dxvk-remix *source*; the
   shipped `.trex\d3d9.dll` has no such option — the flag in rtx.conf was a
   silent no-op and raw RHW (pretransformed) draws are simply **dropped**.
   Lesson: verify config options against the deployed binary's option table
   (strings search), not against upstream source.
2. **This runtime's UI paths are**: `rtx.uiTextures` (raster-on-top, but *the
   first UI-textured draw triggers RTX injection* — mid-frame use cuts the
   traced scene short), `rtx.worldSpaceUiTextures` (unlit world billboard), and
   `rtx.orthographicIsUI` (ortho draws = UI — how the F4 ImGui overlay works).
   Texture tagging is unusable for this engine because the recycled texture
   cache shares D3D texture objects between HUD and world draws.
3. **HUD elements are 3D models in the same draw stream as world objects.**
   Shields row = one 192-prim fan; light gem = TWO 6-prim elements (jewel +
   frame). They flush through the *same* engine call site as object draws
   (`0x60CE4F`; world flush is `0x60CF9D`), with Z **enabled** and raw screen-z
   inside the world band — no return-address, z-state, or vertex-level
   discriminator exists. The z-only mid-scene Clear is the only reliable marker.
4. **UI draws must come after the full scene submission.** First version
   submitted worldrep/lights at EndScene → after the converted UI → Remix
   injected at the UI draw and rasterized the whole world over the HUD (both
   "no path tracing" and "no HUD" from one ordering bug).
5. **Gem flicker = alternating submission order of overlapping elements.** The
   engine alternates jewel/frame draw order per frame and relies on z-testing
   against the freshly cleared z-buffer; Remix's UI raster path has no usable
   depth (z-tested UI fails against stale scene depth and disappears entirely).
   Fix: proxy-side far-to-near sort in a single deterministic EndScene batch.
6. **The old flat-rhw "UI" heuristic must never paint.** Distant world fans
   also have near-constant rhw; painting them as ortho UI put big screen-space
   rectangles over the view. Flat-rhw draws outside the overlay phase are
   passthrough (harmlessly dropped; worldrep renders the world anyway).
7. **Tracer limitations found**: engine is built with frame-pointer omission →
   `CaptureStackBackTrace` returns nothing (tracer backtraces 100% empty);
   single-frame captures end at the first Present and *miss the HUD pass*
   (use `--frames N`). Live wrapper-vtable tracing (`livetools trace` on the
   proxy's DPUP slot with `[esp]` reads) is the reliable way to get callers.

## Deployment set (all required)

- `d3d9.dll` + `remix-comp-proxy.ini` (build output) → game dir
- `bridge.conf` → game dir (`exposeRemixApi = True`)
- `rtx.conf` → game dir — must contain `rtx.orthographicIsUI = True`
  (canonical copy: `patches/ThiefGold/assets/rtx.conf`; re-check after any
  Remix overlay "save settings")

## Debug tooling added this phase

`[Unproject] DebugLogFrames=N` logs every DPUP's classification signals
(verdict, FVF, prim count, ZENABLE, depth range, first-vertex screen pos,
texture) to `rtx_comp\unproject_debug.log`. This is what cracked both the HUD
routing and the gem flicker — prefer it over guessing.

## Open items

- **White untextured world rectangle** (task #6): a worldrep-rendered plane
  with no texture (likely sky/special surface id) — pre-existing, brightness
  shifts with path-traced light.
- **Brightness calibration**: `RadianceScale=20` is a first guess; plan is to
  find the light-gem value in memory and tune visible darkness against the
  engine's own stealth reading (also consider disabling Remix auto-exposure —
  it fights Thief's darkness).
- Weapon viewmodel currently renders flat as part of the overlay pass
  (vanilla-look; Remix view-model treatment is a later step).
- `+0x28` cone-outer semantics on omni torches (0.866 observed) unconfirmed.
- Earlier backlog: per-frame DrawPrimitiveUP perf, object-path hash stability.
