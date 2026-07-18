# Thief Gold — RTX Remix

Path-traced **Thief Gold** (NewDark 1.27) via [RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix), built on the [remix-comp-proxy](https://github.com/xoxor4d/remix-comp-base) framework from the [Vibe Reverse Engineering](https://github.com/Ekozmaster/Vibe-Reverse-Engineering) toolkit.

The Dark engine is a software-T&L engine: it CPU-transforms geometry to screen space and feeds Remix un-raytraceable pre-transformed (XYZRHW) vertices. This `d3d9.dll` proxy reconstructs real world-space geometry, mirrors the engine's light table into Remix path-traced lights, and replays the HUD as ortho UI — so Remix can actually trace the scene.

## What works

- **World geometry** submitted as stable-hash whole polygons from the resident worldrep cell array, textured from engine bitmaps
- **Engine lights drive the path tracer** — the runtime light table is mirrored into Remix sphere/spot lights every frame; torch flicker tracks automatically
- **Lightmap albedo attenuation** — designed-dark areas stay dark (stealth-calibrated)
- **HUD** (gem, shields, weapon) replayed depth-sorted as ortho UI over the traced scene
- **Characters/objects** reconstructed per frame from RHW draws using the engine's live render camera (head-bob, crouch, and mouse-look correct)
- **Sky** handled Remix-side; the engine's sky-hack surface is dropped from submission

## Install (release package)

Grab the latest zip from [Releases](../../releases) and follow `INSTALL.txt` inside. Short version:

1. Thief Gold (Steam) + **TFix Lite** (NewDark 1.27). Never run Steam "verify integrity" afterwards.
2. **RTX Remix runtime 1.5.2+** ([download](https://github.com/NVIDIAGameWorks/rtx-remix/releases)): `d3d9_remix.dll` + `.trex/` into the game dir.
3. From the release zip into the game dir: `d3d9.dll`, `remix-comp-proxy.ini`, `rtx.conf`, `bridge.conf`.
4. `cam_ext.cfg`: `force_windowed`, MSAA/atoc/distortion off.

> **Pitfall:** "Save Settings" in the Remix menu writes `user.conf` and can silently delete `rtx.conf` (HUD/exposure settings live there). Restore it from the release zip if that happens.

## Building from source

```
build.bat release --name ThiefGold
```

Requires Visual Studio 2022 (any edition, Build Tools suffice). Output: `build/bin/release/d3d9.dll`. All dependencies are vendored under `deps/`.

Tagged pushes (`v*`) auto-build and publish a release zip via GitHub Actions.

## Configuration

`remix-comp-proxy.ini` sections: `[Worldrep]` (world submission, `SkipTexIds`, `LightmapAttenuation`), `[Unproject]` (RHW reconstruction, `DebugLogFrames=N` for per-draw diagnostics), `[Lights]` (`RadianceScale`, spot shaping), `[Remix]`. Everything is runtime-toggled — no rebuild needed to fall back.

Diagnostics land in the game dir under `rtx_comp\` (`console.log`, `unproject_debug.log`). F4 toggles the ImGui debug overlay.

## Development docs

- `HANDOFF.md` — current state, verified engine addresses, session handoff
- `PHASE4_LIGHTING_HUD.md` — lighting/HUD architecture and its six pitfalls
- `findings.md` — the full reverse-engineering log
- `kb.h` — knowledge base of engine functions/globals/structs (Ghidra-compatible)

## Contributors

| Who | What | Support |
|-----|------|---------|
| [xoxor4d](https://github.com/xoxor4d) | Original [remix-comp-base](https://github.com/xoxor4d/remix-comp-base) framework, D3D9 proxy architecture, ImGui integration, module system | [Ko-Fi](https://ko-fi.com/xoxor4d) / [Patreon](https://patreon.com/xoxor4d) |
| [kim2091](https://github.com/kim2091) | FFP conversion system, skinning module, diagnostic logging, tracer integration, INI config, toolkit integration | [Ko-Fi](https://ko-fi.com/kim20913944) |
| [momo5502](https://github.com/momo5502) | Initial codebase that remix-comp-base was built on | |

Thief Gold port (worldrep reconstruction, engine lights, HUD replay, camera work): this repo.

## Dependencies

- [Dear ImGui](https://github.com/ocornut/imgui) — debug overlay
- [MinHook](https://github.com/TsudaKageyu/minhook) — function hooking
- [RTX Remix Bridge API](https://github.com/NVIDIAGameWorks/rtx-remix) — Remix integration

## License

MIT. See [LICENSE](LICENSE). Contains no game assets; requires your own copy of Thief Gold.
