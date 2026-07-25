# ThiefGold — Findings

Game: Thief Gold (Steam) + TFix Lite / NewDark 1.27, windowed 1920x1080
Game dir: `F:\SteamLibrary\steamapps\common\thief_gold`
Binary: `thief.exe` (NewDark 1.27, 32-bit PE, ~4.7MB). Static base 0x400000, observed runtime base 0x880000 (rebase delta +0x480000).

## 2026-07-17 — Live renderer architecture (livetools session)

Goal: determine if NewDark's D3D9 renderer is Remix-compatible FFP.

**Renderer facts (verified live, in-mission):**
- Pure fixed-function: `SetVertexShader(NULL)` once/frame, zero shader binds.
- All world rendering via `DrawPrimitiveUP` (~91%, D3DPT_TRIANGLEFAN, 3-7 prims/call, ~40/frame) + `DrawPrimitive` (~9%). Zero DrawIndexedPrimitive / DIPUP.
- FVFs observed (all with XYZRHW — CPU-pre-transformed screen-space):
  - 0x1C4 = XYZRHW|DIFFUSE|SPECULAR|TEX1
  - 0x144 = XYZRHW|DIFFUSE|TEX1
  - 0x2C4 = XYZRHW|DIFFUSE|SPECULAR|TEX2 (lightmap pass, stride 40)
- `SetTransform` called once/frame each for WORLD(256)/VIEW(2)/PROJECTION(3) — **all identity**. Camera never reaches D3D.
- Vertex sample (stride 40): screen pixels x/y, z=depth 0..1, rhw≈1/view_depth (e.g. 0.082 → ~12.2 units), diffuse, specular, 2 uv pairs.
- => RTX Remix cannot ingest this directly (screen-space geometry, no matrices). Un-projection via rhw + engine camera state is the candidate strategy.

**Device-pointer leak (why d3d9 wrappers get bypassed):**
- Game obtains the RAW IDirect3DDevice9 (via unwrapped child object, e.g. surface/swapchain GetDevice) and stores it in globals: runtime `0x00E5915C` and `0x00E5C748` (static: `0xA1915C` / `0xA1C748`, delta -0x480000).
- All scene draws go through the raw device; a proxy d3d9.dll only sees TestCooperativeLevel/Present/CreateTexture (menu path). Any comp-proxy for this game MUST wrap child objects / plug GetDevice leaks or hook vtable instead.
- Device vtable is heap-copied (0x0AF715FC) — likely Steam overlay vtable swap; draw slots still point into system d3d9.dll.

**Interesting runtime callers (rebase -0x480000 for static):**
- `SetTransform` caller: runtime 0x00A8E106 → static 0x60E106
- `DrawPrimitiveUP` caller: runtime 0x00A8CF9D → static 0x60CF9D
These are the NewDark D3D9 display-layer dispatch — entry points for finding the upstream transform pipeline and camera state.

**Traces:** `patches/ThiefGold/traces/drawcount.jsonl`, `stateinfo.jsonl`.

**Static mapping of the draw dispatch (index.db queries):**
- `DrawPrimitiveUP` call site 0x60CF9D is inside `FUN_0060ce80` (0x60CE80–0x60CFC8, 328 bytes) — the DPUP dispatcher.
- Its callers: `FUN_0060cb70`, `FUN_0060d010` (2 sites), `FUN_0060d420` — NewDark display-layer cluster, prime targets for decompilation to find vertex-source + camera state upstream.
- `SetTransform` call site 0x60E106 falls in a gap with no function in index.db — region needs a manual look.

## 2026-07-17 — Ghidra backend (static-analyzer)

- Analyzed in 118s. Project: `patches/ThiefGold/ghidra/thief/thief.gpr`
- 16,214 functions; index.db seeded: xrefs=225,149, basic blocks=149,289 → use `python -m retools.query ThiefGold "SQL"`.
- Bootstrap (kb.h RTTI/CRT seeding) running separately.

## 2026-07-17 — RTX Remix + pre-transformed geometry research (web-researcher)

- **dxvk-remix has NO un-project path for XYZRHW/POSITIONT** — tracked unimplemented as `REMIX-760` in `src/d3d9/d3d9_rtx.cpp`. Default: RHW draws are *dropped entirely*. With `rtx.preTransformedVerticesIsUI = true`: rasterized flat as UI (no BLAS, no path tracing, FFP lighting force-disabled for POSITIONT draws).
- The ThiefGuild "Thief 2 RTX mod" is just an rtx.conf preset — flat-rasterized world + Remix post/texture stack, not path-traced geometry. Confirms naive Remix drop-in is a dead end for NewDark.
- **Precedent: `DeusExEchelonRenderer`** (github.com/onnoj/DeusExEchelonRenderer), Deus Ex/UE1 — same software-T&L problem ("pre-transformed vertex soup"). Solution: replacement renderer that re-emits untransformed geometry with real W/V/P matrices and feeds real lights from map data instead of lightmaps. Community consensus: memory/source-level proxy is the only viable technique for RHW engines.
- Normals: Remix does not invent normals for RHW content; proxy must supply per-vertex normals (Remix 1.5 "Smooth Normals" only smooths captured non-RHW geometry, opt-in per texture).
- Implication for lighting phase: Dark engine light data (dynamic light list, lightmap sources) should eventually be fed to Remix as real lights; vertex diffuse is pre-lit and should be neutralized when unprojecting.

## 2026-07-17 — MILESTONE: Thief Gold path-tracing under RTX Remix

Unprojection proxy (patches/ThiefGold, `unproject` module) deployed with Remix 1.5.2:
- Visual parity confirmed without Remix (reprojection-exactness held), then Remix enabled: world path-traces, HUD renders via `rtx.preTransformedVerticesIsUI=True`.
- Verified live: device-leak global patched (wrapper vtable in proxy DLL), real non-identity VIEW matrices reaching d3d9 from proxy code.
- Known issue: geometry hashes churn while camera moves. QuantizeGrid=64 stabilized the static-camera case.

## 2026-07-17 — Remix hashing research (web-researcher)

- Remix hashes raw vertex/index bytes (XXH3) per draw; no config exists to key instance identity off texture+transform. `rtx.geometryAssetHashRuleString` can narrow components but can't fix per-frame content changes.
- For CPU-clipped/portal engines this churn is a documented known limitation ("older games frequently exhibit unstable hashes in world geometry due to culling"). Consequences: BLAS rebuild each frame (perf), unreliable mesh replacement + light anchoring. **Texture/material replacement is keyed on texture hash and works regardless** — the PBR remaster workflow is viable today.
- Ecosystem mitigations: "Anchor Assets" (stable placeholder meshes) for replacements; the real fix is architectural — submit whole unclipped polygons per cell (worldrep-sourced), as DeusExEchelonRenderer did for UE1 ("level meshes are stable; object meshes are not").
- Relevant knobs to experiment: `rtx.antiCulling.object.enable`/`hashInstanceWithBoundingBoxHash` (explicitly for primitive-culling games), `rtx.numFramesToKeepInstances/BLAS`, `rtx.enableAlwaysCalculateAABB`.
- Next diagnostic: drift test — does a static vertex reconstruct to the same world pos as the camera moves? Discriminates camera-model error (fixable in proxy, must fix) from clipping churn (architectural).

**Drift test RESULT (2026-07-17, traces/worldpos_{drift,turn,walk}.jsonl):** with verified camera translation (pos (18,-1.7,3)→(13.2,-2.2,5)→(31.7,-0.8,3)), converted stride-48 draws showed only 27 distinct first-vertex X values over 76,772 records, zero singletons, per-frame-uniform counts — **reconstructed world coordinates are bit-stable under camera motion; the camera model (f=h·2/3, center=w/2,h/2, yaw/pitch basis) is confirmed correct.** Residual Remix hash churn is attributable to per-frame portal/frustum clipping (vertex sets/counts change), not reconstruction error. Fix path: worldrep-sourced whole-polygon submission (Echelon-style, phase 3); interim: texture-hash-keyed replacement unaffected, Remix anti-culling knobs may reduce flicker.

## 2026-07-17 — Worldrep on-disk format (reference, via openDarkEngine cross-check)

Reference layout of the .mis WR/WRRGB chunk (NewDark uses WREXT — geometry structs believed shared, lightmap stride differs; the IN-MEMORY NewDark layout from static analysis is authoritative and may differ). All packed, little-endian.

- **WRHeader (8B):** u32 unk(version) · u32 numCells
- **WRCellHeader (31B):** u8 numVertices · u8 numPolygons · u8 numTextured · u8 numPortals · u8 numPlanes · u8 mediaType(1=air,2=water) · u8 cellFlags · u32 nxn · u16 polymapSize · u8 numAnimLights · u8 flowGroup · float3 center · float radius
- **Cell body order:** vertices[numVertices] (float3, ABSOLUTE WORLD COORDS) → faceMaps[numPolygons] (WRPolygon 8B) → faceInfos[numTextured] (WRPolygonTexturing 48B) → u32 num_indices → flat index list (per poly: WRPolygon.count × u8 indices into cell vertices) → planes[numPlanes] (float3 normal + float d, 16B) → lighting
- **WRPolygon (8B):** u8 flags · u8 count(vtx index count) · u8 plane · u8 unk · u16 tgtCell(portal target) · u8 unk1 · u8 unk2. Textured render polys = indices [0,numTextured); portals = [numPolygons-numPortals, numPolygons).
- **WRPolygonTexturing (48B):** float3 axisU · float3 axisV (NOT normalized; encode scale) · i16 u · i16 v (÷1024 for shift) · u8 txt(texture id) · u8 originVertex · u16 unk · float scale · float3 center
- **Triangulation:** convex fan from local vertex 0. Render polys emit (v0, v[t+1], v[t]) for t=1..count-2 (winding flipped vs portal path). Per-vertex normal = cell plane normal.
- **UV derivation (per vertex):** u = (dot(vertexPos, axisU)/|axisU|² + u_shift/1024) etc.; texture pixel size feeds final scale. Reproduce for FFP TEX0.
- **WREXT caveat:** openDarkEngine does not parse WREXT; only WR(8-bit)/WRRGB(16-bit) lightmaps. NewDark 32-bit path unverified against this ref — trust binary analysis for runtime.

## 2026-07-17 — Worldrep supplement: BSP, texture resolution, UV math

- **After the cell array** the WR chunk continues: u32 extraPlaneCount · Plane[extraPlaneCount] · u32 bspRows · WRBSPNode[bspRows] (20B: u32 ndn_fl [low24=node#, hi8=flags, bit0=leaf], i32 cell, i32 plane, u32 front, u32 back) · global light table. BSP lets us cull at cell granularity if needed (leaf → cell index).
- **Texture resolution (TXLIST chunk):** WRPolygonTexturing.txt is a 0-based index into DarkDBTXLIST_texture[] (20B: u8 one, u8 fam(1-based), u16 zero, char name[16]). Family name from DarkDBTXLIST_fam[fam-1] (16B name). Path = "family/name". Sentinels: txt 0 = JORGE (missing-tex checker), 249 = SKY (skybox, no lightmap). Runtime binding: the engine already has these loaded as D3D textures — better to capture the engine's SetTexture per draw than re-resolve TXLIST, OR map txt→loaded texture via the engine's texture table (find via static analysis).
- **Per-vertex UV (TEX0), for FFP:** origin = vtx[polyIndices[originVertex]]; vrel = pos - origin; with axisU/axisV (unnormalized), mag2_u=|axisU|², mag2_v=|axisV|², dotp=axisU·axisV.
  - orthogonal (dotp≈0): proj_u = (axisU·vrel)/mag2_u; proj_v = (axisV·vrel)/mag2_v
  - else: corr=1/(mag2_u·mag2_v − dotp²); proj_u = (axisU·vrel)·corr·mag2_v − (axisV·vrel)·corr·dotp; proj_v = (axisV·vrel)·corr·mag2_u − (axisU·vrel)·corr·dotp
  - texU = (proj_u + u/4096)/(texWpx/64); texV = (proj_v + v/4096)/(texHpx/64)
- **Winding** is inconsistent between openDarkEngine's two code paths — verify against the real binary's D3DRS_CULLMODE + actual vertex order at runtime rather than assuming.
- **Runtime pointer**: neither research stream found a documented in-memory worldrep global for retail/NewDark; the script API only exposes indirect queries (PortalRaycast). Static analysis (in progress) is locating it empirically from the WR/WRRGB/WREXT chunk-loader xrefs + the byte-packed cell-header read loop.

## 2026-07-17 — Phase 3 worldrep renderer built (hash stability)

Live-verified the worldrep in a loaded mission: g_wrCellCount=4991, cell[0] header/pointer offsets match static analysis exactly (arithmetic checks: vertices at cell+0x54, then poly_list, portal, render_polys, index_list each where prior array ends). Vertices are world-space (e.g. 147,170,0). Resolver 0xA3C710 confirmed `__cdecl resolve(texId)->engineTex`, stable per-id, called only on texture change then immediately bound.

New `worldrep_render` module (patches/ThiefGold/src/comp/modules/worldrep_render.{hpp,cpp}):
- Builds geometry once (cached until cell array changes): walks g_wrCells → render polys → whole convex fans, world-space, fixed ordering. Normal from cell plane oriented toward cell center. UVs per RenderPoly_ProjectUV formula (tex axes + u/v base /4096), pre-scale; texel/64 applied via per-texture FFP texture matrix at submit.
- Texture map: resolver hooked (MinHook) to capture texId; next SetTexture(0) on wrapper associates → IDirect3DTexture9 (consume-once, safe because resolve→bind is 1:1).
- Suppresses engine world fans in DrawPrimitiveUP by return address in portal-flush range (0x60CE80–0x60CFC8); objects still go through unproject.
- Submits at EndScene with real V/P (shared camera_math.hpp, also now used by unproject).
- Config [Worldrep] Enabled/WindingFlip; falls back to pure-unproject if disabled.
- Whole level submitted each frame via DrawPrimitiveUP (identical bytes → stable hash). Perf: optimize to static VB / cull if needed.

**RESULT — WORKING (2026-07-17):** worldrep whole-polygon submission renders the world correctly under Remix; hashes stabilized. Two fixes after first test:
1. Floor/see-through-map → engine world polys have mixed winding; forcing D3DRS_CULLMODE=NONE during submission (two-sided for the path tracer) restored the floor.
2. Textures churning on camera move → the engine keeps textures as CPU bitmaps and uploads them to a RECYCLED surface cache, so snooping SetTexture bound transient cache surfaces. Fix: resolver returns a stable-per-id engine object holding the raw bitmap inline (uint16 w/h at +0x08, A8R8G8B8 pixels at +0x40); build our own persistent D3DPOOL_MANAGED texture per texId from that bitmap (also the un-lightmapped base — correct for path tracing). All texIds resolved up front so the geometry set is complete/stable from frame 1.

Phase 3 complete: world geometry is now stable-hash and path-traced. Objects remain on the unproject path (expected churn). Follow-ups: perf (whole level via DPUP each frame → move to static VB), lighting injection (feed engine light lists as Remix lights + neutralize baked vertex lighting), verify UV/winding edge cases across missions.

## Environment / setup notes

- TFix Lite package extracted at `C:\Users\ben\Downloads\TFix_Lite_NewDark_1.27_18jul2026` and copied into game dir (excluded stray crash.dmp/THIEF.log debris). Backups: `patches/ThiefGold/backups/2026-07-17_1550_pre-newdark-install/`.
- cam_ext.cfg changes for Remix/debug: `multisampletype 0`, atoc off, distortionfx off, `force_windowed`; cam.cfg `game_screen_size 1920 1080`. Bloom/postprocess already off by default.
- DX9 tracer proxy (`d3d9.dll` + `proxy.ini`) currently deployed in game dir — remove before Remix testing.
- Do NOT run Steam "verify integrity" (reverts NewDark). Re-apply TFix Lite instead if needed.

## Bootstrap — thief.exe (NewDark 1.27) — 2026-07-17

### Summary
Bootstrapped `F:\SteamLibrary\steamapps\common\thief_gold\thief.exe` (Thief Gold, NewDark 1.27 Dark Engine, 32-bit PE, 4.66 MB). Seeded `patches/ThiefGold/kb.h` with **3905 KB entries**, all `@` function/vtable entries. Compiler identified as **MSVC (70% confidence)** — consistent with the VC9-era toolchain. The dominant yield is **1733 RTTI classes**, each written as a `cClassName_vtable;` label at its static vtable VA with the full demangled RTTI name and base-class hierarchy chain preserved in `// [rtti]` / `// hierarchy:` comments above it. All addresses are **static VAs at preferred image base 0x400000** (range 0x401420–0x80887C); runtime relocates to 0x880000, so add delta 0x480000 when correlating with a live process.

Note: bootstrap first failed writing to a bare `--project ThiefGold` (wrote to CWD-relative `ThiefGold\kb.h`, which failed under background CWD). Re-ran with `--project patches/ThiefGold` and fixed all doc examples to use the `patches/`-prefixed path.

### Seeded Counts (bootstrap_report.txt)
| Metric | Count |
|--------|-------|
| KB entries written | 3905 |
| RTTI classes | 1733 |
| Signature DB matches | 154 (mostly generic CRT `crt_xmatch_*` labels) |
| Imports cataloged | 361 |
| Error strings seeded | 433 |
| Propagated labels | 1585 |
| Compiler | msvc (70%) |

### Key Engine Class Vtables (static VAs)
| Static VA | Class | Subsystem |
|-----------|-------|-----------|
| 0x6F5D64 | cCameraSrv | Camera service (ICameraScriptService) |
| 0x713198 | cCameraOps | Camera data-ops / property serialization |
| 0x708D0C | cAICamera | AI camera device (cAIDevice hierarchy) |
| 0x6FEAF4 | cAIDarkCameraBehaviorSet | AI camera behavior set |
| 0x72B258 | cRenderParams | Render parameters property |
| 0x724584 | cInvRenderOps | Inventory render ops |
| 0x72AE34 / 0x72AF54 | cMeshTexOps / cMeshTexProperty | Mesh texture replacement |
| 0x72AB14 / 0x72ABCC | cMeshAttachOps / cMeshAttachProp | Mesh attachment |
| 0x72D5E0 / 0x72D704 | cModelLodOps / cModelLodProperty | Model LOD |
| 0x72A100 / 0x72A244 | cLightOps / cLightProperty | Lighting |
| 0x71FE94 | cMotionCoordinator | Motion/animation |
| 0x71FF10 / 0x720C54 | cMotionDatabase / cMotionSet | Motion data |
| 0x7182A4 | cObjectSystem | Object system core |
| 0x702EC4 | cDarkGameSrv | Dark game service |
| 0x703278 | cDarkUISrv | Dark UI service |
| 0x7015A0 | cDarkInvSrv | Dark inventory service |
| 0x6F66EC | cObjectSrv | Object service |
| 0x6F6428 | cLightScrSrv | Light script service |
| 0x712AEC | cDeviceOps | Device ops |

### Notes on Renderer Subsystem
No single `cDarkRenderer`/`cRenderer` RTTI vtable is present — the Dark Engine's core render/portal/rasterization classes appear to lack RTTI (common for hot-path engine internals), so they were not captured by the RTTI pass. What is captured is the **property/ops layer around rendering**: cRenderParams, cInvRenderOps, cMeshTexOps, cModelLodOps, cLightOps. The actual renderer entry points will need to be located via string xrefs (e.g. render mode / rendlod / "portal" config strings) or by tracing draw calls live — a follow-up static task, not covered by bootstrap.

### Suggested Live Verification
- Attach and set the runtime base delta (+0x480000). Trace vtable-slot calls on cCameraSrv (0x6F5D64→runtime 0xB75D64) to confirm the camera update path.
- To find the core renderer (no RTTI), breakpoint on D3D/DirectDraw present/blit or search strings for render config keys, then walk callers.

### KB / Index Status
- `patches/ThiefGold/kb.h` — 3905 entries, 13450 lines (RTTI hierarchy comments inline).
- `patches/ThiefGold/index.db` — seeded by bootstrap (provisional `source='bootstrap'` rows). Next step: `pyghidra_backend.py analyze` then `export` to upgrade to authoritative Ghidra facts.

## Camera State & RHW Transform for Remix Unprojection — 2026-07-17

### Summary
The engine's authoritative camera lives in a "location" object referenced by the global pointer `$0x9CDB4C` (the script/current camera) with the active render-camera id at `$0x9CDB48`. Camera world position is stored as 3 floats at object+0x08/+0x0C/+0x10; orientation is 3 packed 16-bit Dark-Engine angles at object+0x14/+0x16/+0x18 (0..65535 = full turn). The `cCameraSrv` methods (vtable 0x6F5D64) read these fields directly, so decompiling them exposed the layout. A per-frame render-camera cache (used for camera-motion detection) is written by `FUN_00538C70` into globals `0x8CD0E4..0x8CD108` (angles) and `0x8CD0F4/F8/FC` (position). The DPUP world draw (`FUN_0060CE80`) only flushes pre-transformed 40-byte XYZRHW vertices via `device->DrawPrimitiveUP(TRIANGLEFAN, ...,0x28)` — the world→screen projection is baked upstream in the cell/portal renderer, reading the camera object transform via `FUN_0053C410`. The `0x60E106` "SetTransform" site is NOT a matrix set — it is a 2D device-wrapper thunk calling `device_vtbl+0xB0` (a render-mode setter).

### Key Addresses
| Address | Description |
|---------|-------------|
| 0x9CDB4C | $g_pScriptCamera — ptr to active camera "location" object (pos@+8/+C/+10 float, angles@+14/+16/+18 u16, parent id@+20, type tag@+0) |
| 0x9CDB48 | $g_curCameraLocId — active render-camera location id (source of the "real" camera when world-fixed) |
| 0x9CDB24 | $g_pCamAttachMgr — object/property manager vtable for camera-attached objects (used in 0x53C410 neg-id path) |
| 0x9CDB28 | $g_pLocationTable — transform table; (*0x9CDB28+0x18)[id*4] = object transform ptr |
| 0x9CDB40 | $g_pCameraOverlay — camera overlay/stack object (0x402640/0x402690 activate/deactivate) |
| 0x9CDB10 | $g_camClipThreshold (float) — compared to 0x8CD0F0/0x8CD110 for near/clip test |
| 0x9CDB14 | $g_renderCamCacheValid (flag) |
| 0x9CDB18 | $g_camAngularDelta (u32, per-frame) |
| 0x9CDB1C | $g_camPositionDeltaSq (float, per-frame) |
| 0x9CDAAC | $g_renderDepthCounter |
| 0x8CD0F4 / F8 / FC | $g_renderCamPos x/y/z (float) — prev-frame cache (write@0x538E9C) |
| 0x8CD0E4 / E6 / E8 | $g_renderCamAng x/y/z (u16 packed) — prev-frame cache (write@0x538E94) |
| 0x8CD104 / 106 / 108 | additional angle/transform components of the render-cam cache |
| 0x73D380, 0x73D338 | angle→radian scale consts (BSS, runtime-init ≈ 2π/65536) used by GetFacing |
| 0x6F5D64 | cCameraSrv_vtable |
| 0x402790 | cCameraSrv::GetPosition(out vec3) → reads cam+8/+C/+10 |
| 0x4027C0 | cCameraSrv::GetFacing(out vec3 rad) → cam+14/16/18 (u16) * 0x73D380 * 0x73D338 |
| 0x402830 | cCameraSrv::GetFacingVecA(out) — builds rot matrix via trig (0x65EAA0/0x65EA00) |
| 0x4028F0 | cCameraSrv::GetFacingVecB(out) — sibling of 0x402830 |
| 0x402730 | cCameraSrv::GetCameraParent → cam+0x20 |
| 0x4026E0 | cCameraSrv::IsAttachedTo(id) → cam+0x20 |
| 0x402750 | cCameraSrv::IsCameraForced → cmp cam+0x20 vs 0x9CDB48 |
| 0x53C410 | GetLocationTransform(id) — returns camera/object transform ptr (pos[0..2], angles[4],[5],[0x12]) |
| 0x538C70 | UpdateRenderCamCache — reads camera transform, writes 0x8CD0E4..108, computes motion deltas |
| 0x5385C0 | RenderCameraObject(loc, ctx) — draws camera-attached geometry; frustum test vs 0x8CD0F0/110 |
| 0x60CE80 | DPUP portal-cache flush → device->DrawPrimitiveUP(TRIANGLEFAN, cnt, vtx, stride=0x28) |
| 0x9D915C | $g_pRenderDevice — D3D device wrapper (COM): +0x14C DrawPrimitiveUP, +0x164 SetFVF, +0xAC/+0xB0 2D thunks, +0x98 |
| 0x9D9128 | $g_pRasterState — render/rasterizer state mgr: +0x24 buffer toggle, +0x7C query, [0xF]/[0x10] cur/prev state blocks (+0x58/+0x5C tex handles) |
| 0x9D9160 | $g_curFVF_flag |
| 0x9D9164 | $g_curFVF (cached, e.g. 0x2C4) |
| 0x78ACBC | $g_portalCacheSeq — LRU frame counter for portal vertex cache |
| 0x60DFF0 | Draw2DPrim thunk — scales int coords by 0x78ACA0/A4 (both 1.0), ftol 0x6B32E0, calls dev_vtbl+0xAC |
| 0x60E0C0 | SetDrawMode thunk (incl 0x60E106) — mode{0,1,2}→{0x100,2,3}, calls dev_vtbl+0xB0 (NOT SetTransform) |

### Details

**Camera object field layout** (base = *0x9CDB4C, confirmed via GetPosition/GetFacing):
```
+0x00  int    typeTag       // 3,4 = attached-to-object variants (0x5385C0/0x538C70 branch on it)
+0x08  float  pos.x
+0x0C  float  pos.y
+0x10  float  pos.z
+0x14  uint16 ang.x         // packed Dark angle
+0x16  uint16 ang.y
+0x18  uint16 ang.z
+0x20  int    parentObjId
```
GetPosition (0x402790): `out[0..2] = *(cam+8), *(cam+0xC), *(cam+0x10)` (zeros if cam==NULL).
GetFacing  (0x4027C0): `out[i] = (uint16)*(cam+0x14+2*i) * f(0x73D380) * f(0x73D338)` — packed-angle→radian.

**Render-cam cache write (FUN_00538C70)** stores the current camera transform (from FUN_0053C410) into `0x8CD0E4/E6/E8` (angles) and `0x8CD0F4/F8/FC` (position); these are read back only within the same function to compute `$0x9CDB1C` (positional delta²) and `$0x9CDB18` (angular delta). So they are a *motion-detection snapshot*, not the live projection matrix — the live camera for projection is the location transform itself (via 0x53C410 on id *0x9CDB48).

**DPUP flush (FUN_0060CE80)** — proves the RHW/stride-40/TRIANGLEFAN pipeline:
```
if (g_curFVF != 0x2C4) { g_curFVF=0x2C4; device->[+0x164](device,0x2C4); }   // SetFVF
device->[+0x14C](device, 4 /*D3DPT_TRIANGLEFAN*/, primCount, vtxPtr, 0x28 /*stride 40*/); // DrawPrimitiveUP
```
Vertices (piVar1[4]) are already screen-space; they are produced upstream in the cell/portal renderer, NOT here.

**0x60E106 clarification**: the region 0x60DFF0–0x60E110 is three small device-wrapper thunks Ghidra failed to bound. 0x60E106 is inside `FUN_0060E0C0`, which maps a mode arg to a render-state value and calls `device_vtbl+0xB0(dev, value, param)` — a render-state/mode setter, not a D3D SetTransform of a matrix. No camera math here.

### LIVE-VERIFIED camera cache semantics (2026-07-17, session 2)

Verified in-mission by moving/turning the player (gamectl + user mouse look):
- `$0x9CDB4C` script camera ptr is **NULL during normal play** (script/cutscene cameras only); `$0x9CDB48` id and `$0x9CDB28` table also 0. The render-cam cache is the reliable live source.
- **Position**: floats at `0x8CD0F4/F8/FC` (runtime 0xD4D0F4) = world x, y, z. Z-up; standing eye height z=5.52 constant while walking. Tracks movement in real time.
- **Pitch**: u16 at `0x8CD0E6` (runtime 0xD4D0E6), signed Dark angle, **up = negative** (looking max up read 0xC304 ≈ -85.8°). Level ≈ 0.
- **Yaw**: u16 at `0x8CD108` (runtime 0xD4D108) — moved 0x2BED→0x7E26 as player rotated. (Matches the non-contiguous transform layout angles[4],[5],[0x12] from 0x53C410.)
- Roll: TBD (expected in one of the zero words 0x8CD0E4/0x8CD104/0x8CD106; only non-zero while leaning).
- Angle scale: value * (2π/65536) rad (consts 0x73D380/0x73D338).
- Cache written per frame by `FUN_00538C70` (runtime 0x9B8C70) — freezes when game unfocused/paused (windowed mode keeps it live).

### Suggested Live Verification
1. Breakpoint `0x538C70`; read `*0x9CDB4C`, then dump cam+0x08..+0x18 to confirm live position/angles match the in-game camera. Watch `0x8CD0F4/F8/FC` update per frame.
2. To find the exact projection scale/screen-center (deliverable #3, not yet pinned to a global): breakpoint the DPUP flush `0x60CE80`, read the vertex buffer at `piVar1[4]` (stride 40), then set a memory read-watch on the camera position floats and single-step upward into the cell renderer to catch the `screen = center + scale*(view/z)` math. The scale/center constants will be the globals read right before the XYZRHW x/y are written.
3. Trace `0x53C410` with the id from `*0x9CDB48` to dump the live camera transform matrix used for rendering.

## Worldrep (portal-cell world geometry database) — 2026-07-17

### Summary
The loaded worldrep is a static array of cell pointers at **g_wrCells = 0x00A35EE0**, count at **g_wrCellCount = 0x00A5DEB8** (int32). Both are populated by the worldrep chunk parsers (`WRParseCells_v0` 0x54A240 for WR/WRRGB, `WRParseCells_ext` 0x54A900 for WREXT/NewDark) which store one malloc'd `WRCell*` per cell into `g_wrCells[i]`. The in-memory `WRCell` layout is identical across file formats (only the file parse differs). Each cell holds world-space float3 vertices (+0x08), a vertex-index list (+0x18), render polys with texture mapping (+0x14, stride 0x34) and portal polys (+0x10). The world→screen projection runs `WRRenderAllCells (0x4CE030)` → `WRRenderCell (0x4D1FB0)` → `WRDrawRenderPoly (0x4D1900)`, which reads cell vertices via the index list, transforms into a 44-byte-stride cache, clips, and emits the RHW fans flushed by `FlushPortalCacheDPUP (0x60CE80)`. Texture id lives at render-poly+0x20 and is resolved via the callback at 0x7A84D8.

### Key Addresses
| Address | Description |
|---------|-------------|
| 0x00A35EE0 | g_wrCells[] — static array of WRCell* (root of worldrep) |
| 0x00A5DEB8 | g_wrCellCount — int32 number of loaded cells |
| 0x0054B4F0 | WorldRepLoadMsgHandler — references WRRGB/WREXT/WRSIZE chunk names |
| 0x0054A240 | WRParseCells_v0 — WR/WRRGB parser, fills g_wrCells |
| 0x0054A900 | WRParseCells_ext — WREXT (NewDark) parser, fills g_wrCells |
| 0x004CE030 | WRRenderAllCells — top-level loop over g_wrCells[0..count] |
| 0x004D1FB0 | WRRenderCell(cellIdx) — sets g_curCell* globals, loops render polys |
| 0x004D1900 | WRDrawRenderPoly — gathers vtx indices, clips, emits screen fan |
| 0x004D1600 | WRPolyPlaneSide — render-poly plane index (+2) vs camera plane test |
| 0x00A71DE0 | g_curCellVertices (= cell+8, world float3) |
| 0x00A71DE8 | g_curCellVtxCache (transformed-vertex cache, stride 0x2C=44) |
| 0x00A71DF0 | g_curCellVtxIndex (= cell+0x18, uint8 index list) |
| 0x007A84D8 | PTR_texResolveById — fn ptr (uint16 texId)->tex, for render-poly+0x20 |

### Details

**Deliverable 1 — root globals.** In both parsers the loop is `for (i=0; i < g_wrCellCount(0xA5DEB8); i++) { cell=malloc(0x54); (&DAT_00A35EE0)[i]=cell; ... }`. `g_wrCellCount` is read as 4 bytes from the chunk stream (`puStack_5c=&DAT_00a5deb8; size=4; stream->read()`). `g_wrCells` (0xA35EE0) is thus `WRCell* g_wrCells[maxCells]` in .bss, indexed directly. It is read by 100+ functions across the 0x4C–0x5D render/portal engine; the top-level consumer is WRRenderAllCells (0x4CE030) which walks `while (i < g_wrCellCount)` calling WRRenderCell(i).

**Deliverable 2 — WRCell layout (0x54 header, uint8 counts).** Derived from the alloc/store sequence in 0x54A240 / 0x54A900 (identical) and confirmed by render-side reads in 0x4D1FB0 / 0x4D1900 / 0x4D1600:
- +0x00 u8 num_vertices; +0x01 u8 num_polys; +0x02 u8 num_render_polys; +0x03 u8 num_portal_polys; +0x04 u8 num_planes; +0x06 u8 flags; +0x07 u8 render_disable
- +0x08 p_vertices → malloc(num_vertices*0x0C) — world-space float3 (12 bytes each)
- +0x0C p_poly_list → malloc(num_polys*8) — WRPoly[8]
- +0x10 p_portal_polys = p_poly_list + (num_polys-num_portal_polys)*8
- +0x14 p_render_polys → malloc(num_render_polys*0x34) — WRRenderPoly[52]
- +0x18 p_vertex_index_list (uint8 run per poly; render base g_curCellVtxIndex 0xA71DF0)
- +0x24 p_planes → malloc(num_planes*0x10) — float4 {a,b,c,d}
- +0x28 p_clip_scratch (runtime; [1] = transformed-vertex cache base)
- +0x34 anim_light_bits (mask ANDed against WRLightmapInfo.light_mask)
- +0x38 p_animlight_pal (uint16[num_animlights @ +0x32])
- +0x3C p_lightmap_info → malloc(num_render_polys*0x14) — WRLightmapInfo[20]

WRPoly (8B): flags/num_verts/plane_id/pad + uint16 dest_cell. WRRenderPoly (0x34): texture U/V axis vectors, u_base/v_base = fixed16/4096 (0.00024414063 = 1/4096) at +0x18/+0x1C, texture_id (uint16) at +0x20, runtime cache_handle at +0x22. WRLightmapInfo (0x14): lm_width (+0x04), lightmap pixel ptr (+0x08), light_mask (+0x10); lightmap data size = w*h*(2 or 4 bpp)*num_active_lights.

**Deliverable 3 — world→screen projection.** WRRenderCell (0x4D1FB0) loads per-cell render globals: g_curCellVertices=cell+8, g_curCellVtxCache=*(cell+0x28)[1], g_curCellVtxIndex=cell+0x18, then loops the render polys (stride 0x34) and the poly list (stride 8) calling WRRenderPolyDispatch/WRDrawRenderPoly. WRDrawRenderPoly (0x4D1900) reads `num_verts=poly[1]` vertex indices from `g_curCellVtxIndex + base + i`, converts each to a cache slot `idx*0x2C + g_curCellVtxCache` (44-byte transformed-vertex records), clips via FUN_004D13C0, and emits the fan. The screen-space RHW fans are batched and flushed by FlushPortalCacheDPUP (0x60CE80, DrawPrimitiveUP stride 40). WRPolyPlaneSide (0x4D1600) does the plane-side/near test using cell planes (cell+0x24) and camera plane at 0xC19AC0/0x73D244.

**Deliverable 4 — texture resolution.** Each WRRenderPoly stores a 16-bit `texture_id` at +0x20. In WRDrawRenderPoly it is passed to the callback pointer `PTR_texResolveById` (0x7A84D8, default target ~0x4D8630) which returns the bound texture handle. The UV for a vertex is computed from the render-poly texture axes (dot of world vertex with tex_u_axis/tex_v_axis + u_base/v_base). This callback is installed per render mode (multiple installers write 0x7A84D8), so live confirmation of the exact texture-manager global is recommended.

### Suggested Live Verification
1. Read g_wrCellCount (0xA5DEB8) and dump g_wrCells (0xA35EE0) live after a mission loads; walk cell[0] header bytes to confirm counts and pointer offsets (+0x08 vertices, +0x14 render polys, +0x3C lightmap).
2. Breakpoint WRRenderCell (0x4D1FB0), read cell+0x08 as float3[num_vertices] to confirm world-space geometry, and cell+0x18 index list.
3. Trace WRDrawRenderPoly (0x4D1900) to capture render-poly texture_id (+0x20) and correlate with the D3D texture bound after PTR_texResolveById(0x7A84D8) fires — this maps polygon → IDirect3DTexture9 for stable-hash submission.
4. For the proxy: iterate g_wrCells directly (world-space, unclipped) rather than intercepting the clipped 40-byte fans — read p_vertices + per-poly index runs + render-poly UV axes to reconstruct whole world polygons.

## Worldrep in-memory layout — 2026-07-17

### Summary
The Dark Engine worldrep (static level geometry / BSP cells) is resident in three
globals: a cell pointer table `g_cellArray @ 0x00A35EE0` (`WRCell*[MAX_CELLS]`, MAX_CELLS ≈ 0x2000),
a total cell count `g_numCells (_cells) @ 0x00A5DEB8`, and a raw data pool base
`g_worldrepPoolBase @ 0x009CDC64` (size @0x9CDC68) into which each cell's sub-arrays
point. Iterating `for(i=0;i<*(int*)0xA5DEB8;i++){ WRCell* c=((WRCell**)0xA35EE0)[i]; if(c) ... }`
walks every static cell independent of the per-frame portal cull. Each `WRCell` holds a
float3 vertex array (+0x08), a uint8 vertex-index list (+0x18), an 8-byte-stride poly
descriptor list (+0x0C, [1]=vertex count), a 0x34-stride render-poly list (+0x14) whose
+0x20 is the texture id and whose s/t axes + base give planar UV, and a 0x10-stride plane
array (+0x24). The projection routine `RenderPoly_ProjectUV @ 0x4DD100` reads exactly these
fields, so the same arrays feed a proxy that walks static geometry.

### Key Addresses
| Address | Description |
|---------|-------------|
| 0x00A35EE0 | g_cellArray — WRCell*[MAX_CELLS≈0x2000]; `[idx*4+0xA35EE0]` |
| 0x00A5DEB8 | g_numCells (_cells) — total resident cell count (0 when no worldrep) |
| 0x00A5DEC0 | per-cell medium byte array (0x7FF8 B), parallel to g_cellArray |
| 0x00A55EC0 | second per-cell parallel byte array (0x7FF8 B) |
| 0x009CDC60 | g_worldrepStruct (header ptr) |
| 0x009CDC64 | g_worldrepPoolBase — raw worldrep data blob base |
| 0x009CDC68 | g_worldrepPoolSize |
| 0x009CDC6C | g_worldrepPoolCur (= base after load) |
| 0x00C19AD8 | g_travFrontierCount — DYNAMIC portal frontier (reset to 1/frame, not total) |
| 0x00C28880 | g_travProcessedCount — visible-cell count this frame |
| 0x00B22840 | g_travQueue[] — cell-index visit queue |
| 0x00C19AC0/AC4/AC8 | camera pos x/y/z (portal backface test) |
| 0x0054B4F0 | WorldRep_RenderSetup (WR/WRRGB/WREXT dispatch) |
| 0x00549BE0 | WorldRep_Free (frees cells; canonical field-offset evidence) |
| 0x004CDAD0 | portal_traverse_scene (BSP walk) |
| 0x004CD1C0 | examine_portals (plane-vs-camera portal test) |
| 0x004CE130 | RenderScene_WorldGeom (iterate visible cells) |
| 0x004D29B0 | RenderCell — sets cell ctx globals, loops render polys |
| 0x004DD100 | RenderPoly_ProjectUV — vertex fetch + UV projection |
| 0x0060CE80 | PortalCache_DrawFlush — DrawPrimitiveUP stride-0x28 XYZRHW |

### Cell-context globals set by RenderCell (0x4D29B0) per cell
- `0xA71DEC = cell ptr`
- `0xA71DE0 = cell->p_vertices` (float3 array base)  ← used by rasterizer as `*(0xA71DE0 + idx*0xC)`
- `0xA71DF0 = cell->p_index_list` (uint8 index base) ← `puVar1 = 0xA71DF0 + poly_vbase; idx = puVar1[i]`
- `0xA71DE8 = region->[1]` (projected-vertex output buffer, stride 0x2C; u@+0x24 v@+0x28)
- `0xA71DE4 = region->[0]`, `0xA71DF4 = cell+0x38`

### Decompilation evidence (field → code)
- **num_render_polys (+0x02)**: `RenderCell` `uVar3 = *(cell+2)` loop count; `portal_traverse_scene` `*(byte*)(cell+2)`.
- **num_portal_polys (+0x03)**: `examine_portals` `uStack_c = *(cell+3)`.
- **medium (+0x05)**: `FUN_00465FF0` `cVar1 = *(cell+5)` branch on 1/2.
- **p_vertices (+0x08)**: `RenderCell` `0xA71DE0 = *(cell+8)`; `RenderPoly_ProjectUV` `*(0xA71DE0 + idx*0xC){+0,+4,+8}` = x,y,z.
- **p_polys (+0x0C)**: `RenderCell` `puVar6 = *(cell+0xC)`; `puVar6[0]&4`=skip, `iVar4 += puVar6[1]` (vert count), `puVar6 += 8`.
- **p_portal_polys (+0x10) / p_planes (+0x24)**: `examine_portals` `iVar3=*(cell+0x10)`, entry stride 8 (uint16 plane_idx@+2, dest_cell@+4); plane test `pfVar7 = plane_idx*0x10 + *(cell+0x24)`, dot(plane.xyz, camPos) + plane.d vs threshold.
- **p_render_polys (+0x14)**: `portal_traverse_scene` `*(ushort*)(*(cell+0x14) + 0x20 + i*0x34)` = texture id; stride 0x34.
- **p_index_list (+0x18)**: `RenderCell` `0xA71DF0 = *(cell+0x18)`.
- **p_lightmap_info (+0x3C)**: `RenderCell` `iVar5 = *(cell+0x3C)`, `iVar5 += 0x14`/poly (WRLightmapInfo[num_render_polys], stride 0x14), passed as `param_2` (int16 lightmap u/v base) to rasterizer; `WorldRep_Free` frees the lightmap pixel sub-ptrs at entry+8/+0xC. Texture UV comes from the render-poly (+0x14) s/t axes, NOT this array — matches the prior 'Worldrep (portal-cell world geometry database)' section above.
- **center (+0x44..0x4C)**: `portal_traverse_scene` cycle warning `%g %g %g` = `*(float*)(cell+0x44/0x48/0x4C)`.
- **_cells / g_cellArray / MAX_CELLS**: `WorldRep_Free` disasm — `mov [edi*4+0xA35EE0], ebx` then `cmp edi, [0xA5DEB8]`; `memset(0xA5DEC0,0,0x7FF8)` and `memset(0xA55EC0,0,0x7FF8)` ⇒ ~0x2000 cell cap.
- **worldrep pool**: `WorldRep_RenderSetup` sets `0x9CDC64 = alloc(); 0x9CDC6C = 0x9CDC64`; `WorldRep_Free` clears 0x9CDC60/64/68/6C/70.

### RenderPoly_ProjectUV (0x4DD100) projection math
For each poly vertex (`n = polyDesc[1]`, capped 0x20): `idx = index_list[vbase+i]`;
`v = p_vertices[idx]`; relative to poly origin `p_vertices[index_list[vbase]]`; computes
texture-space u,v from render-poly s/t axes (`in_EAX[0..2]`, `[3..5]`) and bases (`[6],[7]`),
scaled by texture dims (`0x7A8084[...]`), writing `proj[idx*0x2C + 0x24]=u`, `+0x28=v`.
Screen XYZRHW is produced by the transform helper `FUN_004D4530` (0x9CD8F0 gate) and the
projected verts are handed to `(**0x9D8728)` → material buckets → `PortalCache_DrawFlush`
`DrawPrimitiveUP(dev, primtype=4, verts, count, stride=0x28)`.

### Suggested Live Verification
1. `livetools` read `int` @ 0xEDDEB8 (runtime = 0xA5DEB8 + 0x480000) -> g_numCells; expect >0 in a loaded mission.
2. Read pointer table @ 0xEB5EE0 (=0xA35EE0+0x480000); for i in 0..numCells read `WRCell* = [0xEB5EE0 + i*4]`.
3. For a non-null cell: read bytes +0x02/+0x03 (poly counts), ptr +0x08 (verts), then dump `numVerts` float3s to confirm world coords match level scale.
4. bp 0x4DD100 (runtime 0x95D100) and read `0xA71DE0`/`0xA71DF0` (verts/index ctx) + EAX (render poly) to watch live cell rendering; correlate texture id @ renderPoly+0x20 with 0x60CE80 DrawPrimitiveUP bucket.
5. Confirm MAX_CELLS cap by reading past g_numCells entries (should be null/stale).

### Reconciliation with prior Worldrep sections
This section is consistent with and extends the earlier 'Worldrep (portal-cell world geometry
database)' section: `g_cellArray`/`g_wrCells` = 0xA35EE0 and `g_numCells`/`g_wrCellCount` = 0xA5DEB8
are the same globals (the prior run showed 0xA5DEB8 is read as 4 bytes straight from the WR chunk
stream by parsers WRParseCells_v0 0x54A240 / WRParseCells_ext 0x54A900 -> so 0xA5DEB8 is the
DEFINITIVE static total cell count; 0xC19AD8 is only the per-frame portal-traversal frontier and
must NOT be used as the cell count). New facts added here: (a) the worldrep raw-data POOL globals
0x9CDC60/64/68/6C (blob base/size/cursor) that cell sub-arrays point into; (b) MAX_CELLS approx
0x2000 from the WorldRep_Free memset sizes (0x7FF8) on parallel arrays 0xA5DEC0 (medium)/0xA55EC0;
(c) WorldRep_Free 0x549BE0 as the canonical field-offset evidence source; (d) a SECOND render path
0x4CE130 -> 0x4D29B0 -> 0x4DD100 that parallels the 0x4CE030 -> 0x4D1FB0 -> 0x4D1900 path (likely a
different render mode/LOD) but reads the identical WRCell and the same cell-context globals
0xA71DE0/DE8/DF0. For a proxy, iterate g_cellArray[0..g_numCells] directly (world-space, unclipped);
cell+0x08 = float3 verts, cell+0x18 = uint8 index runs, cell+0x14 render polys (tex id +0x20, s/t
axes for UV), cell+0x24 planes.

## Lighting injection — static analysis (2026-07-17)

### Summary
Located the Dark engine's **runtime light table** and the **per-frame animated-light (flicker) system**. All lights (baked-static, animated, dynamic) live in one master array at **0x9EA660** (stride 0x30, capacity 768). Each frame the engine (a) rebuilds a per-cell culled visible list at **0xA026A0** (max 32 lights), and (b) walks every animated light in property store 0x9CE4B4 via tick **0x5A2180**, recomputes brightness by mode (flip/slide/random) in **0x58D4F0**, and writes the result into the master record's color fields (+0x18/+0x1C/+0x20) and into a per-light brightness byte-array at **0xA7152C** via **0x58D2E0**. The cLightScrSrv vtable (0x6F6428) turned out to be shared MSVC template thunks — not the data path; the real path is the worldrep light table + animlight property tick. A d3d9 proxy should read the master table 0x9EA660 (pos/color/radius) each frame after the animlight tick has run, using 0xA7152C for live flicker brightness.

### Key Addresses
| Address | Description |
|---------|-------------|
| 0x9EA660 | g_lightTable — master light record array, stride 0x30, cap 768. pos+0/4/8, RGB +0x18/1C/20, radius +0x2C |
| 0x9CE4F4 | g_lightCount — running light count / next index into g_lightTable (reset 0 on unload) |
| 0xA02CA0 | g_staticLightCount — static-light base index (reset 1 on unload); ADDRESS 0xA02CA0 = end of visible list |
| 0x9EA640 | g_animLightCount — dynamic/anim light count (capacity portion) |
| 0xA026A0 | g_visibleLights — per-cell culled/active light list, stride 0x30, MAX 32 entries |
| 0xA02660 | g_light0 / ambient light slot (0x30, memset on unload) |
| 0xA7152C | g_animLightBrightness — PTR to per-light current-brightness byte array [2048], indexed by light record idx. **Flicker value** |
| 0x9CDFA0 | g_animCellListCount — count for animlight cell-dirty pair list |
| 0xA03EA0 | g_animCellList — flat uint16 pairs (cell_index, light_bit); used to OR g_wrCells[cell]+0x34 |=1<<bit |
| 0x9CE4B4 | g_animLightPropStore — IPropertyStore for sAnimLightProp (iterated by tick) |
| 0x5A2180 | AnimLight_Tick — per-frame loop over all animlights (registered via 0x5BB790) |
| 0x58D4F0 | AnimLight_ComputeBrightness — mode animation (flip/slide/random) on runtime state record |
| 0x58D2E0 | AnimLight_Apply — writes color to g_lightTable[idx]+0x18/1C/20, brightness byte to g_animLightBrightness[idx], dirties cells |
| 0x58D450 | AnimLight_Off — zeros color + brightness, dirties cells |
| 0x58D2B0 | AnimLight_ResetTable — clears 0xA7152C[0..0x800], g_animCellListCount=0 |
| 0x54AF80 | WR_LoadLightTable_fast — bulk read (0x9000 bytes) of g_lightTable from .mis |
| 0x54B040 | WR_LoadLightTable_versioned — per-record 0x28→0x30 expand read |
| 0x54B4F0 | WorldRepLoadMsgHandler — msg dispatch (0=free/WorldRep_Free, 2=load) |
| 0x549BE0 | WorldRep_Free — frees cells + resets light counts (0x9CE4F4=0, 0xA02CA0=1) |
| 0x5A9390 | Light_BrightnessAtPoint — (R*w1+G*w2+B*w2)/dist using record color |
| 0x5AB210 | Light_CullTestToVisibleList — copies g_lightTable→g_visibleLights if in range/visible (cap 32) |
| 0x5AB3E0 / 0x5AB890 / 0x5AB9A0 / 0x5ABD80 | per-vertex/per-object light gather (reads cell+0x40 light index list, accumulates) |

### Light record layout (g_lightTable @ 0x9EA660, stride 0x30 = 12 floats)
| off | type | meaning | confidence |
|-----|------|---------|-----------|
| +0x00 | float | world X | HIGH (written from light origin in 0x5AB3E0/0x58D2E0) |
| +0x04 | float | world Y | HIGH |
| +0x08 | float | world Z | HIGH |
| +0x0C | float | secondary vec X (dir? unconfirmed) | LOW |
| +0x10 | float | secondary vec Y | LOW |
| +0x14 | float | secondary vec Z | LOW |
| +0x18 | float | color R (overwritten each frame for animlights) | HIGH (0x5A9390 luma-weight, 0x58D2E0 writer) |
| +0x1C | float | color G | HIGH |
| +0x20 | float | color B | HIGH |
| +0x24 | float | animlight marker (== -1.0f @0x73D320 sentinel = static, else animated) | MED |
| +0x28 | float | aux (paired with +0x24) | LOW |
| +0x2C | float | radius/range (0 = infinite; range test dist² < r²) | HIGH (0x5AB210, 0x5AB890) |

### Animlight runtime state record (in property store 0x9CE4B4; dwords, from 0x58D4F0)
| off | meaning |
|-----|---------|
| +0x0C | time_on (ms, floored to 5, default 0x3f) |
| +0x10 | time_off (ms) |
| +0x14 | min brightness |
| +0x18 | max brightness |
| +0x1C | current brightness |
| +0x20 | phase/state toggle (0/1) |
| +0x24 | countdown timer |
| +0x28 | mode (int16): 0/3=on-off flip, 1/6/7=slide interp min↔max, 9=random(RNG tbl 0x80A2A8), 6/7=one-shot |
| +0x28 (dword[10]) | inactive/done flag |

### Cell fields (g_wrCells[cell], base 0xA35EE0) relevant to lighting
- +0x34 anim_light dirty bitmask (set by 0x58D2E0/0x58D450, consumed by lightmap rebuild)
- +0x38 animlight palette uint16[num_animlights@+0x32] — maps layer bit → global animlight
- +0x40 per-cell light index list: uint16 count then 32-bit masks into g_lightTable (read by 0x5AB3E0 etc.)
- +0x3C WRLightmapInfo[num_render_polys] (stride 0x14): lm_width+0x04, pixel ptr+0x08, light_mask+0x10

### Ambient / global light constants
- 0xB22820/24/28 (+ mirror 0x7E0C19/1A/1B bytes) = ambient RGB, default 0x80,0x80,0x80 (set in WorldRepLoadMsgHandler on unload)
- 0xA71950/54/58 = global directional light vector (sun dir), added * brightness to light pos in 0x5AB3E0/0x5AB890
- 0x73D288 / 0x73D520 / 0x73D518 / 0x73D298 = luma / color-scale constants
- 0x73D8D0 = global brightness offset scalar
- 0x73D320 = float -1.0f (animlight "none" sentinel)
- WR light-table chunk name at 0x7A2C4C = "LM_PARAM"

### Suggested Live Verification (frida / livetools)
1. After a mission loads, read `int g_lightCount = *0x9CE4F4` and dump `g_lightTable @ 0x9EA660` for `g_lightCount` records (0x30 stride): print pos (+0/4/8), RGB (+0x18/1C/20), radius (+0x2C). Cross-check positions against known torches/lamps.
2. `bp 0x58D2E0` (AnimLight_Apply) — reads EDI = animlight descriptor; watch it write g_lightTable[idx]+0x18/1C/20 and `(*0xA7152C)[idx]`. Confirms flicker each frame.
3. `memwatch` on a specific `g_lightTable[idx]+0x18` while a flickering torch is on-screen — should oscillate.
4. Read `*0xA7152C` (ptr) then dump 256 bytes — per-light current brightness (0..255); index by the light's record index.
5. `bp 0x5A2180` (AnimLight_Tick) once per frame — confirm it is the per-frame driver (registered at 0x5BB790).
6. Ambient: read bytes at 0x7E0C19..1B and floats at 0xB22820.

### Notes / caveats
- pyghidra_backend `decompile --project` opens the raw binary WITHOUT the analyzed program (no functions) → use r2ghidra (`retools.decompiler --backend pdg`) here. The Ghidra daemon left a stale lock; cleaned. index.db `funcs`/`xrefs` are authoritative for addresses.
- cLightScrSrv vtable 0x6F6428 is shared MSVC cScriptServiceImplBase template thunks (QueryInterface etc.) — NOT a data path to the light tables. Ignore for injection.
- Record +0x0C/+0x10/+0x14 and +0x24/+0x28 need live confirmation (marked LOW/MED).

### Proxy implementation status (2026-07-17, same session)

`engine_lights` module implemented and building warning-clean (NOT yet live-verified or deployed):
- `src/comp/game/lights.hpp` — LightRecord (0x30) + table/count accessors (VAs 0x9EA660/0x9CE4F4/0xA02CA0).
- `src/comp/modules/engine_lights.{hpp,cpp}` — per-frame mirror of the master light table into Remix sphere lights. Stable hash = 0x7401EF11C4700000 + table index; handle recreated only when pos/color/radius change; lit-check = any color component > 0 (AnimLight_Off zeroes colors); radius==0 records skipped via [Lights] SkipInfinite.
- Wired: comp.cpp registration, d3d9ex.cpp EndScene submit (after worldrep) + device-reset -> reset(), [Lights] INI section (Enabled/RadianceScale/EmitterRadius/SkipInfinite), config parsing.
- Backup: backups/*_engine-lights/. TO VERIFY LIVE before deploy: table base/count VAs, color scale (RadianceScale default 1.0 is a guess), whether slot 0 / indices < g_staticLightBase are real lights, radius semantics.

### Live verification (2026-07-18, Bafford miss02, pid 31060, base 0x880000 => delta +0x480000 confirmed)

- g_lightTable 0x9EA660 CONFIRMED: records match stride 0x30; table ends EXACTLY at index 365 (records 362-364 valid, 365+ zero-filled).
- **Count global is 0xA02CA0** (= 365, matches table boundary exactly). 0x9CE4F4 reads constant 0x800 (2048) — NOT the count (likely animlight-brightness capacity); static-analyzer labels for these two were swapped/wrong.
- **Record +0x0C..+0x14 = unit spot DIRECTION, +0x24 = cos(inner cone), +0x28 = cos(outer cone)** — observed (0.262,-0.490,-0.831) unit vector with cos 0.9659/0.9511 (15.0/18.0 deg spotlight, record 363). For omni lights: dir=0, +0x24 = -1.0 (cos 180). The earlier "anim marker" reading of +0x24 was wrong.
- Color units are SMALL: observed 0.9375, 3.96875, 4.6875 (equal RGB = white lights). RadianceScale default set to 20.
- Radius +0x2C confirmed: 0 on ambient-style records, 6.0 on record 362.
- Camera cache still valid (0x8CD0F4).
- AnimLight_Tick (0x5A2180) fires per frame BUT returned constant sim time 0x69A with game unfocused => **sim pauses when unfocused**; AnimLight_Apply + brightness-array writes require focused gameplay to observe (verification pending).
- engine_lights module updated to match: count VA 0xA02CA0, spot shaping from dir/cone fields, RadianceScale 20.

### Flicker verification (2026-07-18, second session, 139-light mission)

- memwatch on animlight brightness array (ptr 0xA7152C -> heap) caught a write at +0x4C = **light index 76** during focused play; writer runtime IP 0x009564B3 (static 0x4D64B3 — lightmap-side, NOT AnimLight_Apply; note the sim fully pauses on focus loss, so all anim verification needs the game focused).
- Record 76 = the torch the user was facing: pos (33.89,-2.87,8.05) vs camera (15.71,2.84,3.03) ~20 units away, elevated; color white 1.5625; radius 0; dir (0,0,-1) with +0x24=-1.0, +0x28=0.866 => torches carry a down direction + cos30 in +0x28 while +0x24=-1 (module treats inner_cos=-1 as omni — correct visual default; +0x28 semantics still open).
- NOTE: this torch has radius 0 => with [Lights] SkipInfinite=1 it would be SKIPPED. Torches must render: radius 0 is common, not just ambient. Changed module policy: submit radius-0 lights as omni; SkipInfinite now only skips truly ambient-style records (radius 0 AND no direction AND huge coverage)? -> Simplest correct policy: submit all lit records; SkipInfinite default OFF pending in-game tuning.

### UI/HUD regression + fix (2026-07-18)

Symptom: HUD invisible in-game; marking textures as UI in the Remix overlay had no effect.
Root cause: `rtx.preTransformedVerticesIsUI = True` was missing from BOTH rtx.conf and user.conf in the game dir. dxvk-remix drops RHW/POSITIONT draws entirely without it (REMIX-760, see phase-1 findings) — texture UI-tagging never applies because the draws are discarded first. The flag was present in earlier phases; likely lost to an overlay settings-save or the Remix redeploy.
Fix: re-added to rtx.conf (backup: backups/*_engine-lights/rtx.conf.before-ui-fix).
LESSON: rtx.conf is part of the deployment set — treat `rtx.preTransformedVerticesIsUI = True` as REQUIRED for this port; keep a canonical rtx.conf copy under patches/ThiefGold/assets/ and re-check after any Remix overlay "save settings".

## HUD call-site analysis (2026-07-18)

### Summary
Analyzed `dxtrace_20260718_015359.jsonl` (324 records, 1 frame, 106 DrawPrimitiveUP, all DPUP — no DIP/DPUP-indexed/DrawPrimitive). **CRITICAL BLOCKER: the capture contains ZERO backtrace / return-address data** — the tracer reports `Empty backtraces: 324 (100.0%)` and the raw JSONL has no `bt`/`ret`/`caller`/`addr` field (top-level keys are only `args, data, frame, method, seq, slot, ts`; `slot` is the D3D9 vtable index, not a return address). **BacktraceDepth was not actually recorded**, so no static VAs can be derived from this capture. The goal (classify HUD DPUPs by return address) cannot be met until a capture with real backtraces is taken. Below is the frame-order / render-state clustering that IS recoverable, plus the re-capture requirement.

### Frame structure — three passes by FVF + render state (frame order)
| Pass | seq range | draws | FVF | stride | textures | notes |
|------|-----------|-------|-----|--------|----------|-------|
| A — world | 11–234 | 96 | 708 = 0x2C4 (XYZRHW\|DIFFUSE\|SPECULAR\|TEX2) | 40 | 2 stages (diffuse + lightmap) | Dark-engine multitextured world flush; ZWRITE on, ZFUNC=LESSEQUAL |
| B — post-world alpha | 241–272 | 10 | 452 = 0x1C4 (XYZRHW\|DIFFUSE\|SPECULAR\|TEX1) | 32 | 1 stage, tex1=NULL | transparent/decal/particle pass; ZWRITE off, SRCBLEND=SRCALPHA, DESTBLEND=INVSRCALPHA |
| C — 3D overlay/viewmodel setup | 279–320 | **0** | 322 = 0x142 (XYZ\|DIFFUSE\|TEX1, NON-RHW) | — | — | SetViewport(280), ZENABLE off(288), CULLMODE=NONE, LIGHTING off(302), then real D3DTS_VIEW(rotation)+D3DTS_PROJECTION set at seq 318–320. Set up but **no geometry drawn before Present** — the first-person weapon/3D-overlay draws fall past this frame boundary. |

All 106 DPUP are PrimitiveType=4 (TRIANGLEFAN), VDecl NULL (fixed-function via SetFVF).

### Pass B (the "late" cluster in this frame) — per-draw
| seq | prim cnt | stride | tex0 | tex1 |
|-----|----------|--------|------|------|
| 241 | 175 | 32 | 0x14688BE8 | NULL |
| 243 | 2 | 32 | 0x19B8B350 | NULL |
| 245 | 36 | 32 | 0x19B8B0B0 | NULL |
| 247 | 10 | 32 | 0x13A537D0 | NULL |
| 249 | 4 | 32 | 0x19B8A0F0 | NULL |
| 251 | 24 | 32 | 0x19B89E50 | NULL |
| 253 | 6 | 32 | 0x13A531B0 | NULL |
| 268 | 192 | 32 | 0x13A53D10 | NULL |
| 270 | 6 | 32 | 0x13A53C30 | NULL |
| 272 | 6 | 32 | 0x142F5540 | NULL |
The large fans (175, 192 tris) indicate this is transparent WORLD geometry / water / glass, not 2D HUD icons. No small (cnt≈2) single-quad UI icon run is isolated here, so Pass B is most likely NOT the HUD either.

### Render-state transitions (pass boundaries)
- seq 254–259: enter Pass B — ZWRITEENABLE=0, ALPHABLENDENABLE toggled, SRCBLEND=5 (SRCALPHA), DESTBLEND=6 (INVSRCALPHA).
- seq 279–302: enter Pass C — SetFVF=322 (non-RHW), SetViewport @280, FILLMODE=SOLID(3), SHADEMODE=GOURAUD(2), ZWRITE=0(285), CULLMODE=NONE(287), **ZENABLE=0(288)**, ALPHABLENDENABLE=1(289), LIGHTING=0(302).
- seq 318–320: SetTransform VIEW (rotation/affine) + PROJECTION (real perspective) — confirms Pass C is a 3D viewmodel pass, not a 2D ortho HUD.

### Why no static VAs
The one deliverable that requires return addresses (HUD call-site VAs) is impossible from this capture — backtraces are 100% empty. The expected world-flush return range 0x60CE80–0x60CFC8 (static) could not be confirmed present either, for the same reason.

### Suggested live verification / next capture
1. **Re-capture with backtraces actually populated** (BacktraceDepth=8) — the current file has none. Without the `bt` array per DPUP record, return-address classification is not possible.
2. Ensure the captured frame includes an on-screen HUD (health/light-gem/weapon). This frame Present'd with only world (A) + transparent (B) passes and the viewmodel pass (C) set up but undrawn — likely captured on a loading/transition frame or before the overlay was submitted. Capture during active gameplay with weapon drawn and light gem visible.
3. Once a backtrace-bearing capture exists, re-run `--callers DrawPrimitiveUP` and `--resolve-addrs THIEF.EXE`; subtract 0x480000 from runtime frames to get static VAs; world flush should land in 0x60CE80–0x60CFC8, and any late DPUP cluster with return addresses OUTSIDE that range (drawn after the world pass, likely with ZENABLE=0 + viewport reset) is the HUD.

### DIFF: focused/HUD-visible capture (dxtrace_20260718_020034.jsonl) vs unfocused

**Backtraces: still 100% empty (362/362).** Same schema keys only (`args,data,frame,method,seq,slot,ts`). No return-address data — static-VA classification remains impossible from either capture.

**Both captures are a single BeginScene→EndScene→Present cycle, and ALL draws happen before EndScene:**
| | unfocused | focused |
|--|--|--|
| records | 324 | 362 |
| DrawPrimitiveUP | 106 | 121 |
| DrawIndexed/DrawPrimitive | 0 | 0 |
| BeginScene / EndScene / Present seq | 0 / 273 / 323 | 0 / 311 / 361 |

Draws by FVF (frame order), focused:
- FVF 0x2C4 (XYZRHW·DIFFUSE·SPECULAR·TEX2, stride 40) — world, **111 draws**, seq 11–272.
- FVF 0x1C4 (XYZRHW·…·TEX1, stride 32) — transparent world, **10 draws**, seq 279–310.
- FVF 0x142 (XYZ·DIFFUSE·TEX1) — set at seq 317 **but 0 draws** (same as unfocused).

**(1) The viewmodel/overlay pass STILL contains no draws.** In the focused capture the FVF-0x142 pass is again only *set up*, never drawn, and now it is clearly set up **after EndScene (seq 311)** and before Present (seq 361). The setup even binds an indexed stream (SetStreamSource stride 24 @315 + SetIndices @316) — i.e. the weapon viewmodel is drawn via **DrawIndexedPrimitive from a VB/IB, not DPUP** — but that draw lands in the *next* frame, past the capture window. The 2D HUD ortho pass is likewise set up (matrices below) with no draw captured.

**(2) No new small-quad DPUP HUD cluster appears.** The FVF-0x1C4 "late" pass is the SAME content as the unfocused capture — same texture pointers (0x14688BE8, 0x13A53D10, 0x13A53C30, 0x142F5540 …) and near-identical primitive counts (175→180, 192, 6, 6). It is view-dependent transparent WORLD geometry (water/glass/decals), not HUD. Conclusion: **neither capture contains any HUD/overlay draw calls.** The single-frame capture consistently grabs only the world+transparent Present; the HUD (2D ortho) and weapon (3D indexed) draws execute in the following, uncaptured frame.

**(3) HUD/overlay pass state signature** — full setup between EndScene (311) and Present (361), focused capture. The proxy can recognize the overlay pass by this signature:
```
EndScene
SetStreamSource stream0 stride=24 ; SetIndices           <- weapon VB/IB (indexed)
SetFVF 0x142 (XYZ|DIFFUSE|TEX1)
SetViewport
SetPixelShader NULL ; SetVertexShader NULL               <- FFP
FILLMODE=SOLID(3)  SHADEMODE=GOURAUD(2)
ZWRITEENABLE=0  ALPHATESTENABLE=0  CULLMODE=NONE(1)  ZENABLE=0
ALPHABLENDENABLE=1  SRCBLEND(19)=5(SRCALPHA)  DESTBLEND(20)=6(INVSRCALPHA)
FOGENABLE=0  SPECULARENABLE=0  LIGHTING=0
TSS stage0 COLOROP=MODULATE ... ; stage1 disabled
```
Discriminator vs world pass: **ZENABLE=0 + ZWRITEENABLE=0 + LIGHTING=0 + CULLMODE=NONE + no depth**, established immediately after EndScene alongside a SetViewport.

**(4) The two SetTransform matrix sets in the overlay tail (focused capture):**

2D HUD overlay — ORTHOGRAPHIC (seq 353 WORLD / 354 VIEW / 355 PROJECTION):
```
WORLD  = identity
VIEW   = identity
PROJ   = [ 0.00104   0.00000   0.00000   0.00000
           0.00000  -0.00185   0.00000   0.00000
           0.00000   0.00000   0.50000   0.00000
          -1.00052   1.00093   0.50000   1.00000 ]
```
(m00≈2/1922, m11≈-2/1081 → screen-space ortho ~1920×1080, z 0..1; classic 2D HUD projection.)

3D weapon viewmodel — PERSPECTIVE (seq 356 WORLD / 357 VIEW / 358 PROJECTION):
```
WORLD  = identity
VIEW   = [ 0.07481   0.49060  -0.86817   0.00000
           0.99720  -0.03680   0.06513   0.00000
           0.00000   0.87061   0.49198   0.00000
          -6.64596  12.73942 -28.74545   1.00000 ]
PROJ   = [ 0.75000   0.00000   0.00000   0.00000
           0.00000   1.33333   0.00000   0.00000
           0.00000   0.00000   1.00012   1.00000
           0.00000   0.00000  -0.25003   0.00000 ]
```
(m00=0.75, m11=1.3333 → 16:9 perspective; near≈0.25.)

### Revised recommendation
Return-address HUD classification is blocked two ways: (a) backtraces are empty in every record, and (b) the HUD/weapon draws are not even in these single-frame captures — they run in the next Present cycle. To locate the HUD DPUP/indexed draws AND their call sites, the next capture must (i) enable real backtraces (BacktraceDepth populated) and (ii) span multiple frames OR trigger on the frame containing the overlay draws (i.e. capture the frame whose SetTransform uses the ortho PROJ above). Until then the proxy can classify the overlay pass by STATE SIGNATURE (ZENABLE=0 + ortho PROJ matrix m00≈0.00104/m11≈-0.00185) rather than by return address.

### 3-frame capture (dxtrace_20260718_020527.jsonl) — DEFINITIVE: no HUD draws exist in the DPUP stream

589 records, **2 complete Present cycles** (frame field 1 & 2). Backtraces still 100% empty. All draw methods: **DrawPrimitiveUP ×184 only** — zero DrawIndexedPrimitive, zero DrawPrimitive, zero DrawIndexedPrimitiveUP.

**Scene structure (both Present cycles are structurally identical):**
```
Cycle 1: BeginScene(0)
           76x DPUP FVF=0x2C4 (world, dual-tex RHW, stride 40)        seq 13-193
           13x DPUP FVF=0x1C4 (transparent world, single-tex, str 32) seq 201-225
            3x DPUP FVF=0x1C4 (cnt 192,6,6)                           seq 240-244
         EndScene(245)
           [state-block priming block, seq 246-294 — NO DRAWS]
         Present(295)
Cycle 2: BeginScene(296)  ... identical: 76x 0x2C4, 13x 0x1C4, 3x 0x1C4 ... EndScene(538)
           [same priming block, seq 539-587 — NO DRAWS]
         Present(588)
```

**(1) Scene structure answer:** one BeginScene/EndScene pair per Present cycle. ALL draws are inside the scene, and they are world (0x2C4) then transparent-world (0x1C4). Nothing is drawn between EndScene and Present, and nothing overlay-like is drawn at the start of the next cycle (cycle 2 opens straight into world DPUP at seq 309).

**(2) HUD draws — NOT PRESENT.** There are no draws using the ortho PROJ, no draws using the weapon perspective PROJ, no FVF 0x142 draws, and no indexed draws anywhere in 2 full frames with the HUD visible on screen. Every DPUP is RHW world/transparent geometry with world/object texture pointers (0x13A5xxxx / 0x19B8xxxx / 0x1468xxxx) and world-typical counts. The small cnt=1/2 draws in the 0x2C4 pass are small world facets (dual-textured, lightmapped), not UI quads.

**The "overlay setup" block after EndScene is a CreateStateBlock priming sequence, not HUD geometry.** seq 246-294 (and 539-587) is bracketed by `SetRenderState 194 (SRGBWRITEENABLE)=1 … =0`, contains `CreateStateBlock(Type=1 D3DSBT_ALL)`, binds a weapon VB/IB (SetStreamSource stride 24 + SetIndices + SetFVF 0x142), sets the full ZENABLE=0/LIGHTING=0/alpha state, and uploads the ortho + perspective matrix pairs — but issues **no draw**. It runs every frame identically to capture/prime a state block; the ortho and perspective matrices seen in the single-frame captures were this same artifact, not evidence of drawn HUD.

**(3) State signature:** irrelevant for draw-classification because no draws follow it. It is a per-frame state-block record, not a render pass.

**(4) Compact timeline:** `Frame N: BeginScene → 76x world DPUP(0x2C4) → 13x transp DPUP(0x1C4) → 3x transp DPUP(0x1C4) → EndScene → [CreateStateBlock priming, 0 draws] → Present`. Repeats identically for both frames.

### Root-cause conclusion (blocks the original approach)
The HUD/overlay/weapon in Thief Gold (Dark engine / NewDark) is **not rendered through the D3D9 DrawPrimitiveUP path the tracer hooks** — across three captures spanning two full HUD-visible Present cycles, the 2D HUD, light gem, weapon viewmodel, text, and inventory icons produce **zero** DrawPrimitive/UP/Indexed calls in the trace. Therefore:
- There is **no HUD DPUP to classify by return address** — the original proxy plan (tag late DPUPs as UI by return VA) cannot work; those draws do not exist in this device's draw stream.
- The HUD must reach the framebuffer by a mechanism the current tracer does not record: a different/child D3D9 device or swapchain, an ID3DXSprite/StretchRect/ColorFill/UpdateSurface blit path, or the Dark engine's separate 2D overlay surface (historically a lock-and-blit / DirectDraw-style path in the Dark engine, distinct from the 3D DrawPrimitiveUP world renderer).

### Next steps to actually find the HUD path
1. Extend the tracer hook set to cover **StretchRect, ColorFill, UpdateSurface, GetBackBuffer/GetFrontBufferData, ID3DXSprite::Draw/DrawPrimitiveUP on secondary devices, and Lock/Unlock on non-world surfaces**, then re-capture — the HUD likely surfaces there.
2. Alternatively confirm live: `livetools` breakpoint on the world-flush DPUP path (static 0x60CE80–0x60CFC8 → runtime +0x480000) to verify it is world-only, then `dipcnt`/`memwatch` while toggling the HUD to see which D3D9 entrypoint's call count tracks HUD visibility.
3. Return-address classification remains impossible until (a) a capture actually contains HUD draws and (b) backtraces are populated (all three captures: 0% backtraces).

### HUD fix implementation (2026-07-18)

Tracer evidence chain: (1) backtraces impossible (engine FPO breaks CaptureStackBackTrace, 100% empty); (2) 3-frame gameplay capture shows ONE scene per present, all draws are RHW DPUPs (0x2C4 world dual-tex / 0x1C4 transparent), no separate HUD API path; the post-EndScene "overlay setup" (ortho + weapon matrices, FVF 0x142, VB/IB bind) is a CreateStateBlock priming artifact with zero draws. Conclusion: Thief HUD objects (shields/gem/viewmodel) flow through the SAME RHW DPUP path as world/objects and were being unprojected into world space (=> path-traced: dark, side-lit, floor-occluded; exactly the reported symptoms).
Fix: unproject.cpp classifies ZENABLE==FALSE RHW draws as overlay/HUD -> UI passthrough (NewDark draws the overlay pass Z-disabled, world z-tested with ZWRITE on per capture). INI: [Unproject] UiWhenZDisabled=1 (kill switch). Remix rasterizes the passthrough flat via rtx.preTransformedVerticesIsUI=True.
Open follow-ups: 2D text/icons never appeared in any capture (possible second device — console shows two Direct3DCreate9 calls — or blit path); flat-UI darkness under auto-exposure; weapon viewmodel could later go to Remix viewModel category instead of flat UI.

### HUD root cause CONFIRMED + real fix (2026-07-18, live)

Live DPUP tracing through the wrapper vtable (slot 83 @ d3d9.dll+0xE80) + capture timeline analysis:
- Two engine DPUP call sites: 0x60CF9D (world flush, stride 40, FVF 0x2C4) and 0x60CE4F (objects + HUD, stride 32, FVF 0x1C4). HUD (gem=192 prims bottom-left, shields=2x6 prims bottom-center) always LAST in the 0x60CE4F stream — same call site as objects, so return address can NOT separate HUD from objects.
- HUD raw screen-z (0.9658/0.9707) is inside the world z band — no vertex-level discriminator. ZENABLE=1 on HUD draws (fix #2 assumption wrong).
- **Frame timeline shows the real marker: `Clear(Flags=D3DCLEAR_ZBUFFER only, Z=1)` mid-scene immediately before the HUD draws** (frame-start clear is Flags=3 TARGET|ZBUFFER). The engine z-clears then draws HUD models so they always win the z-test — the classic overlay pattern.
- Fix: d3d9ex::Clear sets unproject overlay_phase on z-only clear; BeginScene resets it; unproject passes all overlay-phase RHW draws through as UI ("ui:overlay" in debug log). ZENABLE heuristic removed (dead). Note: HUD only draws while the game window has FOCUS (sim pause also stops HUD submission — affects tracing, not gameplay).

### UI: the REAL Remix-side mechanism (2026-07-18)

Binary analysis of the DEPLOYED .trex\d3d9.dll (Remix 1.5.2 runtime):
- **`rtx.preTransformedVerticesIsUI` DOES NOT EXIST in this build** — the phase-1 findings were based on newer dxvk-remix source; the flag has been a silent no-op all session. Raw RHW (pretransformed) draws are simply DROPPED. Removed from rtx.conf.
- This build's UI paths: (1) `rtx.uiTextures` — texture-hash raster-on-top, BUT "the first UI texture encountered triggers RTX injection" (mid-frame UI textures cut the traced scene short — explains "marking as UI stops ray tracing" with the shared recycled texture cache); (2) `rtx.worldSpaceUiTextures` — unlit billboard in world; (3) **`rtx.orthographicIsUI` — "draw calls that are orthographic will be considered as UI"** — the modern replacement, and how the F4 ImGui overlay (XYZ + ortho, imgui_impl_dx9) renders.
- Fix: unproject re-issues UI-classified RHW draws (overlay-phase after the z-only clear + flat-rhw quads) as XYZ under D3DXMatrixOrthoOffCenterLH(0,W,H,0,0,1), Z/lighting off, engine textures kept (gem states stay live). `rtx.orthographicIsUI = True` set explicitly in rtx.conf. This also covers the previously-missing 2D text/icons (flat-rhw draws were passthrough → dropped).

### Ordering fix: submit scene before UI (2026-07-18)

Ortho-UI conversion alone broke path tracing + hid the HUD: ortho/UI draws trigger Remix RTX injection ("scene end"), and worldrep+lights were still submitted at EndScene — AFTER the UI — so the whole world landed post-injection as raster overdraw. Fix: d3d9ex submits worldrep+lights at the overlay z-only Clear (before any UI draw), with a per-scene latch (m_scene_submitted, reset at BeginScene) and EndScene as fallback for scenes without an overlay pass. Frame order is now: engine world/objects (traced) -> worldrep+lights submit -> z-clear -> HUD as ortho raster on top.

### HUD success + gem flicker root cause (2026-07-18)

Ortho-UI conversion + pre-UI scene submission WORKS: path tracing + full HUD (shields row = the 192-prim fan; light gem = TWO 6-prim elements, jewel + frame, center-bottom). Screenshots + debug log nailed the gem flicker: the jewel/frame draw ORDER ALTERNATES per frame; vanilla resolves overlap by z-testing in the freshly z-cleared overlay buffer, but the ortho conversion forced ZENABLE off + flattened z to 0.5 => painter order => frame covers jewel on alternate frames. Fix: keep raw screen-z (0..1 maps straight through the 0..1 ortho) and leave engine Z states untouched in draw_as_ortho_ui.
Also reverted: flat-rhw draws outside the overlay pass are passthrough again (ortho-painting them showed distant world fans as screen rectangles — near-constant rhw false positives).
Separate known issue (task #6): large white untextured worldrep plane (sky/special surface?) — pre-existing, not UI-related.

### White world plane SOLVED: sky-hack texture id 249 (2026-07-18)

The large white untextured worldrep plane (task #6) is the Dark engine sky-hack
surface, worldrep texture id 249 — its bitmap resolves but is a placeholder the
engine never draws normally (sky is painted separately). Fix: `[Worldrep]
SkipTexIds=249` (comma-separated id list, parsed in config.cpp) drops matching
render polys in worldrep_render::build_geometry before bucketing. The INI key
existed in the deployed game-dir INI but was previously unimplemented in code;
implemented + rebuilt + redeployed this session. Sky visuals are Remix-side
(rtx.conf), not proxy-submitted geometry.

## Sky-hack texture identification (static)

### Summary
The worldrep renderer identifies a sky-hack polygon by comparing its `texture_id`
(WRRenderPoly +0x20 in the software path, +0x8 in the hardware path) against a
**global sky-texture-id variable at VA 0x7A803C** (`g_wrSkyTexId`). That global is
**not** read from the mission file — it is set to the hardcoded constant **0xF9 (249)**
whenever New Sky rendering is enabled, and to **0xFFFFFFFF (-1, "never match")** when
it is disabled. So the sky texture family index is the well-known Dark constant 249,
gated behind a global enable flag. There is no per-mission sky texture id; the mission
only toggles the sky *mode* (`g_newSkyEnable` at 0x78CC94), which in turn selects
0xF9 vs -1 for the id global.

A second, independent sky/no-texture path also exists: the worldrep texture resolver
`PTR_FUN_007a84d8(texture_id)` returning 0 (null handle) routes the poly into the
untextured sky-clip path (`if (iVar4 == 0 && DAT_009cd8c0 == 0)` at the top of
FUN_004d1900). The explicit `== 0x7A803C` comparison is the definitive sky-hack test;
the null-resolver path handles polys whose texture simply failed to resolve.

### Key Addresses
| Address | Description |
|---------|-------------|
| 0x7A803C | g_wrSkyTexId — sky texture id global; = 0xF9 (249) when sky on, 0xFFFFFFFF when off |
| 0x78CC94 | g_newSkyEnable — sky mode toggle (0=off, 1=new sky); selects value of g_wrSkyTexId |
| 0x9CD8F4 | g_wrSkyPassMode — worldrep sky-poly pass mode (1=collect via FUN_004d5730, 2=alt, 3=skip/return) |
| 0x7A84D8 | PTR_FUN_007a84d8 — worldrep texture-id -> texture-handle resolver (null handle = untextured sky path) |
| 0x5D3120 | FUN_005d3120 — sets g_newSkyEnable + writes g_wrSkyTexId (=0xF9 / =0xFFFFFFFF) |
| 0x5D3250 | FUN_005d3250 — sky subsystem (re)init; also writes g_wrSkyTexId (=0xF9 / =0xFFFFFFFF) |
| 0x4D1900 | FUN_004d1900 — software worldrep poly draw; contains `texture_id(+0x20)==g_wrSkyTexId` sky test |
| 0x4DAFF0 | FUN_004daff0 — hardware worldrep poly draw; same test, `texture_id(+0x8)==g_wrSkyTexId` |
| 0x4D5730 | FUN_004d5730 — collects sky-hack poly (sky-pass mode 1) |
| 0x4D1FB0 | FUN_004d1fb0 — per-cell poly iterator (stride 0x34 WRRenderPoly), calls FUN_004d1f30 |
| 0x4D1F30 | FUN_004d1f30 — per-poly dispatch: cull (FUN_004d1600) then HW/SW/alt draw |
| 0x4CE030 | FUN_004ce030 — top-level worldrep render (iterates g_wrCells 0xA35EE0, count 0xA5DEB8) |

### Details

Setter (FUN_005d3120), EAX = requested sky mode:
```c
if (in_EAX == 0) { DAT_007a803c = 0xffffffff; return 0; }   // sky off -> id never matches
else             { DAT_007a803c = 0xf9; return in_EAX; }     // sky on  -> id = 249
DAT_0078cc94 = in_EAX;   // g_newSkyEnable
```
FUN_005d3250 (sky re-init) repeats the identical branch: `DAT_0078cc94==0 ? 0xffffffff : 0xf9`.

Sky test in the software draw path FUN_004d1900 (poly ptr = param_2, texture_id at +0x20):
```c
if ((*(ushort *)(param_2 + 0x20) == DAT_007a803c) && (DAT_009cd8c0 == 0)) {
    if      (DAT_009cd8f4 == 1) { DAT_009cd91c += FUN_004d5730(piStack_c4, uStack_bc); }  // collect sky poly
    else if (DAT_009cd8f4 == 3) { return 1; }                                             // skip sky poly
}
```
Identical logic in hardware path FUN_004daff0 (texture_id at +0x8), with an extra
`DAT_009cd8f4 == 2` branch.

Top of FUN_004d1900 — null-resolver (untextured) sky path:
```c
iVar4 = (*(code *)PTR_FUN_007a84d8)(*(undefined2 *)(param_2 + 0x20));  // resolve tex handle
if ((iVar4 == 0) && (DAT_009cd8c0 == 0)) { /* untextured sky-clip render */ }
```

Conclusion: **constant, not per-mission.** Sky texture id = hardcoded 0xF9 (249),
held in global g_wrSkyTexId (0x7A803C) purely so it can be neutralized to 0xFFFFFFFF
when sky rendering is disabled. The mission-controllable piece is the enable flag
g_newSkyEnable (0x78CC94), not the texture index itself.

### Suggested Live Verification
- `livetools mem read 0x7A803C u32` in-mission: expect 0x000000F9 with sky on, 0xFFFFFFFF with sky off.
- Breakpoint 0x5D3120 to watch the mode->id selection when a mission loads / sky toggles.
- Trace 0x4D5730 hit-count to confirm sky-poly collection frequency; watch g_wrSkyPassMode (0x9CD8F4).
- To disable the sky hack at runtime: write 0xFFFFFFFF to 0x7A803C (id will never match any poly).

### Ground plane vanishing: mid-load geometry snapshot race (2026-07-18)

Symptom: world chunks (ground plane) intermittently missing for a whole session.
Two runs of the SAME mission logged identical cell counts (4991) but wildly
different builds: bad run 102 textures / 29,683 tris, good run 141 textures /
74,599 tris. Cause: worldrep_render::build_geometry caches on (cell array base,
count), both of which are final BEFORE the mission finishes populating cells —
a build taken mid-load snapshots a partial world and never rebuilds.
SkipTexIds=249 was exonerated: in the good run it dropped only 1791 tris while
the ground rendered (and static analysis confirms 249 is engine-hardcoded via
g_wrSkyTexId @ 0x7A803C = 0xF9 whenever NewDark sky is on — mission-independent).
Fix: build_geometry fingerprints per-cell counts (FNV over num_polys/
num_render_polys/num_vertices, null cells sentineled) every call and rebuilds
when the fingerprint changes. Also added the previously-missing "Buckets
(texId:tris)" and "SkipTexIds dropped" log lines to console.log.

## Worldrep lightmap in-memory layout (static) — 2026-07-18

### Summary
Fully decoded the in-memory `WRLightmapInfo` (cell+0x3C, stride 0x14) by decompiling the two
NewDark worldrep parsers (`0x54A240` WRRGB / `0x54A900` WR-mono), the UV projector `0x4DD100`,
the per-poly lightmap-atlas resolver `0x4D4530`, and the depth-mode setter `0x549940`. The
20-byte record is: `int16 lm_u_base` (+0x00), `int16 lm_v_base` (+0x02), `int16 lm_width` (+0x04),
`uint8 lm_height` (+0x06), `uint8 pad` (+0x07), `void* p_lightmap` (+0x08), `uint32 dyn` (+0x0C,
0 at load), `uint32 light_mask` (+0x10). The pixel buffer at +0x08 is allocated at mission load,
holds all layers contiguously — `size = lm_width*lm_height*bpp*(1+popcount(light_mask))` — with the
static base as layer 0 followed by one w*h block per set bit of `light_mask` (LSB-first). `bpp` is
chosen by global `g_lm32bit @ 0x9CDC48`: default build value 1 (non-zero) => **4 bytes/texel
ARGB8888**; only depth-mode 0 gives 2 bytes/texel 16-bit. Mono lightmaps are read 1 byte/texel and
grayscale-expanded to ARGB8888 (`[R,G,B,A]=[lum,lum,lum,0xFF]`); RGB lightmaps are copied verbatim.
Buffers are stable for the mission lifetime (freed only by `WorldRep_Free 0x549BE0`), not recycled
per frame. So a proxy can read `p_lightmap` as A8R8G8B8 and take luminance directly.

### Key Addresses
| Address | Description |
|---------|-------------|
| 0x0054A240 | WRParseCells_v0 (WRRGB) — writes lightmap size `w*h*bpp*layers`, copies pixels verbatim into lminfo+8 |
| 0x0054A900 | WRParseCells_ext (WR mono) — reads 1B/texel, expands to ARGB8888 into lminfo+8 |
| 0x00549940 | WorldRep_SetLightmapDepth(int mode{0,1,2,4}) — sets g_lm32bit(0x9CDC48) + format codes |
| 0x009CDC48 | g_lm32bit — 0 => 2B/texel(16-bit); !=0 => 4B/texel ARGB8888 (default 1) |
| 0x007DF428 | g_lmDepthMirror (copy of g_lm32bit) |
| 0x004DD100 | RenderPoly_ProjectUV — reads lminfo+0x00/+0x02 as lm u/v base (int16) |
| 0x004D4530 | LM_ResolvePolyAtlas — rp.cache_handle(+0x22) -> g_lmSurfCacheTable(0x867B4C); atlas origin + surface |
| 0x00867B4C | g_lmSurfCacheTable (stride 0x14): +0xC/+0xD atlas origin u/v, +0x10 surface index |
| 0x00867B60 | g_lmSurfaceArray (stride 0x1C) |
| 0x007A8084 | g_texSizeRecipLUT (reciprocal texture-size LUT, index = log2dim*-4) |
| 0x00549BE0 | WorldRep_Free — frees lminfo+0x08 pixel buffers (mission unload) |

### Q1 — completed WRLightmapInfo (20 bytes, in-memory)
```
+0x00 int16  lm_u_base    // lightmap luxel-space U origin  (0x4DD100 reads *(int16*)param_2)
+0x02 int16  lm_v_base    // lightmap luxel-space V origin  (0x4DD100 reads param_2[1])
+0x04 int16  lm_width     // luxel width   (both parsers: *(int16*)(+4) in size calc)
+0x06 uint8  lm_height    // luxel height  (both parsers: *(uint8*)(+6) in size calc)  <-- the height field
+0x07 uint8  pad/legacy   // untouched by parse/render paths
+0x08 void*  p_lightmap   // pixel buffer, allocated at load (see Q2/Q3)
+0x0C uint32 dyn_lightmap  // 0 at load; runtime dynamic/animated composite ptr
+0x10 uint32 light_mask   // animlight bitmask; popcount = # of overlay layers
```
The previously-undecoded +0x00-0x03 are the **lightmap u/v base** (two int16). +0x05 is the high
byte of the 16-bit `lm_width`. +0x0C is a runtime dynamic-lightmap pointer (zeroed at load).

### Q2 — pixel format
Chosen by `g_lm32bit @ 0x9CDC48` (set by `WorldRep_SetLightmapDepth 0x549940`, mode arg in {0,1,2,4},
default 1). In both parsers `bpp = ((g_lm32bit != 0) * 2 + 2)`:
- `g_lm32bit == 0` (mode 0 only): **2 bytes/texel** — 16-bit lightmap (legacy RGB565/555).
- `g_lm32bit != 0` (modes 1/2/4, incl. default): **4 bytes/texel ARGB8888**.
Mono WR (0x54A900): source is 1 byte/texel, expanded in-memory to `[R,G,B,A]=[lum,lum,lum,0xFF]`
(byte order R,G,B,A at ascending addresses = D3DFMT_A8B8G8R8 as a DWORD; grayscale so channel choice
is irrelevant). RGB WRRGB (0x54A240): pixels copied verbatim from the chunk (no expansion). **The
mode VA to read is 0x9CDC48** (mirror at 0x7DF428). Practically (default depth 1) lightmaps are
ARGB8888 — sample as A8R8G8B8 and take any RGB channel for mono, real RGB for colored.

### Q3 — layer layout (CONFIRMED, both parsers identical)
```
numLayers  = 1 + popcount(light_mask)                 // 1 static base + 1 per set anim-light bit
totalBytes = lm_width * lm_height * bpp * numLayers    // single contiguous alloc at p_lightmap
```
Layer 0 = static base lightmap (first `w*h` block). Then one `w*h` block per set bit of
`light_mask`, iterated **LSB-first** (`for(m=mask; m; m>>=1) if(m&1) ...`). All layers share the
same `lm_width`/`lm_height`. Block `i` starts at `p_lightmap + i*lm_width*lm_height*bpp`.

### Q4 — lightmap UV mapping (per vertex)
The render path computes a single planar projection (identical to the texture UV path — see the
"Per-vertex UV (TEX0)" section above) and folds the lightmap base into the surface-cache tile:
`0x4DD100` computes `cacheU = texScale*(rp.u_base + proj_u + (atlasOrigU - lm_u_base)*0.25)` where
`proj_u` is the planar projection `(axisU.vrel)/|axisU|^2`, `atlasOrigU = LM_ResolvePolyAtlas` result
(`g_lmSurfCacheTable[rp.cache_handle].origin_u + 0.5`), and `lm_u_base = WRLightmapInfo+0x00`. The
`*0.25` factor = 4 luxels per planar/texture unit (lightmaps are 1/4 the base-texture UV rate).
This build **pre-combines texture*lightmap into a cached surface tile**, so there is no separate
lightmap UV at draw time — the atlas origin is only where the poly's lightmap is packed in the cache
surface.

For a proxy that samples the **raw** per-poly lightmap (ignoring the atlas packing), the luxel
coordinate is the planar projection scaled to luxels and offset by the lightmap base:
```
proj_u = (axisU . (v - v_origin)) / |axisU|^2         // same planar coords as texture (orthogonal case;
proj_v = (axisV . (v - v_origin)) / |axisV|^2         //   general/skew form in the TEX0 section above)
luxelU = (proj_u + rp.u_base)*4 - lm_u_base + 0.5     // rp.u_base = WRRenderPoly+0x18 (fixed16/4096)
luxelV = (proj_v + rp.v_base)*4 - lm_v_base + 0.5     // clamp to [0,lm_width-1] / [0,lm_height-1]
lum    = p_lightmap[ (int(luxelV)*lm_width + int(luxelU)) * bpp ]   // layer 0 (static base)
```
The `*4` (=1/0.25) and the sign of the `lm_base` subtraction are inferred from the engine's
combined-tile formula and SHOULD be live-verified (bp 0x4DD100, read EAX=render poly, param_2=lminfo,
compare computed luxel vs the atlas placement). The `+0.5` is the texel-center bias seen in
`LM_ResolvePolyAtlas`.

### Q5 — lifetime
`p_lightmap` (lminfo+0x08) is allocated once during worldrep load — either sub-allocated from the
worldrep bump pool (`g_worldrepPoolBase 0x9CDC64`, cursor 0x9CDC6C) when it fits, or `malloc`d
otherwise — and is freed only by `WorldRep_Free 0x549BE0` at mission unload. It is **stable for the
mission's lifetime, not recycled per frame**. The separate +0x0C field (0 at load) is the runtime
dynamic/animated-lightmap composite pointer; animated light contribution is layered via the
`light_mask` overlay blocks + the surface cache, not by mutating the base layer-0 buffer.

### Suggested Live Verification
1. Read `int @ 0x9CDC48` (runtime +0x480000 = 0xE5DC48) — expect 1 (default 32-bit) in a loaded mission.
2. For cell[k]: read `WRLightmapInfo* = *(cell+0x3C)`; dump entry[0]: int16 u_base/v_base/width, byte height, ptr+0x08, u32 light_mask. Confirm `alloc size = w*h*4*(1+popcount(mask))` via pool cursor delta.
3. bp `0x4DD100` (runtime 0x95D100): read EAX (WRRenderPoly, +0x22 cache_handle), param_2 (WRLightmapInfo). Verify lm_u_base/v_base = *(int16*)param_2 / param_2[1]; confirm luxel formula vs `g_lmSurfCacheTable[cache_handle]` atlas origin.
4. Dump the first w*h*4 bytes of p_lightmap for a known-lit vs known-dark poly to confirm luminance and that mono => R==G==B, A==0xFF.

### Lightmap albedo attenuation SHIPPED (2026-07-18)

Per-vertex sampling of the static base lightmap layer (WRLightmapInfo fully
decoded — see "Worldrep lightmap in-memory layout") darkens worldrep vertex
color: f = 1 - LightmapAttenuation*(1-luminance), peak-channel luminance,
D3DFVF_DIFFUSE added to the worldrep FVF. Animlight overlay layers ignored
(path-traced lights supply dynamics). INI: [Worldrep] LightmapAttenuation=0.7.
User confirms designed-dark areas now read dark under path tracing. The x4
luxel scale assumption held up visually. Also added [Lights] ForceSpot/
ConeAngleDeg/ConeSoftness (directed records as spots) — currently OFF after
testing; sphere lights + attenuation is the preferred look. Remix engineer
guidance (via user): spots-over-omnis and killing diffuse are the standard
tricks for shadow-stealth games; albedo attenuation implements the latter
from the engine's own bake.

## Live render camera (bob-inclusive) — 2026-07-18

### Summary
The live world-render camera the engine uses each frame is cached into fixed globals by
**FUN_004cdc40 @ 0x4CDC40** (call it `WRCacheRenderCam`). It copies the *active render
camera transform* (passed in EAX) straight into two global blocks at the very start of
world rendering, before portal traversal. The primary portal-camera position lives at
**0xC19AC0 / 0xC19AC4 / 0xC19AC8** (world-space float X/Y/Z). A second, fuller copy of the
same transform (pos + orientation) is written to **0xC21B40..0xC21B54**. Because these are
copied from the exact transform the world is rendered with in the same frame — not from the
separate `UpdateRenderCamCache` (0x538C70 → 0x8CD0F4) snapshot the proxy currently reads —
they track the walking head-bob that the visible world exhibits. This is the camera to read
for bob-correct unprojection.

### Key Addresses
| Address | Description |
|---------|-------------|
| 0x4CDC40 | WRCacheRenderCam — writes all the camera globals below from EAX (render cam transform) |
| 0xC19AC0 | LIVE render camera pos X (float, world-space) — read by portal backface/dist tests |
| 0xC19AC4 | LIVE render camera pos Y (float) |
| 0xC19AC8 | LIVE render camera pos Z (float) — carries head-bob vertical offset |
| 0xC21B40 | Duplicate cam pos X (float) — read by FUN_00458330, FUN_005c3aa0 |
| 0xC21B44 | Duplicate cam pos Y |
| 0xC21B48 | Duplicate cam pos Z |
| 0xC21B4C | Cam orientation dword (Dark u16 angles: heading@+0, pitch@+2) — from src[+0xC] |
| 0xC21B50 | Cam orientation dword (bank + ...) — from src[+0x10] |
| 0xC21B54 | Cam orientation u16 — from src[+0x14] |
| 0x860BF0/BF4/BF8 | Staging pos X/Y/Z inside WRCacheRenderCam (= *EAX, EAX[1], EAX[2]) |
| 0x860BFC/C00/C04 | Staging angle fields (= EAX[3], EAX[4], (u16)EAX[5]) |
| 0x9EA600..0x9EA614 | Alternate whole-transform copy (pos 0x9EA600/604/608, angles 0x9EA60C/610/614) written by FUN_005bd820 |
| 0x5BD820 | WRRenderScene dispatcher — snapshots same EAX transform to 0x9EA600, then calls the 0x4CDC40 chain |
| 0x5BDBC0 | Caller that fetches the active camera object DAT_009cdb4c and drives the render (camera source) |
| 0x9CDB4C | ptr to active render camera object; pos at [+8..+0x10], angles at [+0x14],[+0x18] |

### Details
Writer body (FUN_004cdc40, decompiled, authoritative):
```
DAT_00860bf0 = *in_EAX;                 // pos X   (in_EAX = render camera transform)
DAT_00860bf4 = in_EAX[1];               // pos Y
DAT_00860bf8 = in_EAX[2];               // pos Z
DAT_00860bfc = in_EAX[3];               // angle field 0
DAT_00860c00 = in_EAX[4];               // angle field 1
DAT_00860c04 = *(u16*)(in_EAX + 5);     // angle field 2 (u16)
...
DAT_00c19ac0 = DAT_00860bf0;            // <-- portal-camera pos X/Y/Z
DAT_00c19ac4 = DAT_00860bf4;
_DAT_00c19ac8 = DAT_00860bf8;
_DAT_00c21b40 = DAT_00860bf0;           // <-- full pos+orientation copy
_DAT_00c21b44 = DAT_00860bf4;
_DAT_00c21b48 = DAT_00860bf8;
_DAT_00c21b4c = DAT_00860bfc;
_DAT_00c21b50 = DAT_00860c00;
_DAT_00c21b54 = DAT_00860c04;
```

Call chain establishing the camera source (all pass the same EAX transform down):
`FUN_005bdbc0 (0x5BDBC0)` fetches `DAT_009cdb4c` (active camera object), builds the render
transform from its fields `[2..4]` = pos and `[5]/[6]` = angles → `FUN_005bd820 (0x5BD820)`
(snapshots to 0x9EA600) → `FUN_004ce030 (0x4CE030, WRRenderScene)` → `FUN_004cdc40 (0x4CDC40)`
writes the globals. This is the identical transform used to render the worldrep, so its Z
includes the walking head-bob (the same offset that makes the world bob relative to the
proxy's bob-less unprojection).

Task 1 (writer/source): writer is 0x4CDC40; source is the live render-camera transform in
EAX, ultimately `DAT_009cdb4c`'s position — NOT a separate bob-less cache. No sin/explicit
"+bob" add appears in this path; the bob is already baked into the camera object the engine
renders from (the player-view camera). The bob offset therefore arrives pre-applied in
in_EAX[2] (pos Z), so 0xC19AC8 = bobbed eye Z.

Task 2 (angles): live orientation is stored alongside the duplicate pos at 0xC21B4C/50/54
(and equivalently at 0x9EA60C/610/614). These are Dark packed u16 angles copied from the
camera transform. Note 0xC21B4C..54 are written but have no other static readers (a pure
cache), while 0x9EA60C.. feed later scene code — either is readable per frame at draw time.

Task 3 (object renderer): the DPUP object flush path FUN_0060cda0 @ 0x60CDA0 (draw at
0x60CE4F/0x60CE80) does NOT read 0xC19AC0 / 0xC21B40 / 0x8CD0F4 directly. Objects are
transformed by the frame's view/projection matrix, which is set up from the SAME camera
(same 0x5BDBC0 → 0x5BD820 chain) as the world. So object (character/item) vertices use the
same bobbed camera as the worldrep — consistent with the observed bug (world+objects bob
together, only the proxy's cached-cam unprojection lacks bob).

Task 4 (space): 0xC19AC0 is WORLD-space, not view-relative. Evidence: portal backface tests
compute `(portal_vertex - [0xC19AC0]) · normal` (FUN_004d2c80 fsub block), and squared-dist
`FUN_0065e390(eax=0xC19AC0, ecx=object_world_pos)` treats it as a world eye point.

### Caveats
- Static analysis shows 0xC19AC0 is copied from the render transform, distinct from the
  0x8CD0F4 cache; it cannot 100% prove the numeric Z differs. **Live-verify**: read float
  Z at 0xC19AC8 and at 0x8CD0FC while walking — 0xC19AC8 should oscillate (~+/- bob),
  0x8CD0FC should stay flat (the task's constant 5.52).
- 0xC19AC0 is written once per world-render frame (top of WRRenderScene). Read it at DP/DPUP
  draw time (after the frame's render setup) to get that frame's value; it is stable for the
  whole frame.
- Angle-field packing (heading/pitch/bank order and radians-vs-u16) inferred from the copy
  widths (dword,dword,u16 over src offsets 0xC/0x10/0x14 = 3x u16 + pad). Confirm exact
  packing live before feeding orientation into the unproject; position alone (0xC19AC0) is
  the higher-confidence fix for the bob mismatch.

### Suggested Live Verification
1. `livetools` read float32 x3 at 0xC19AC0 and at 0x8CD0F4 while standing, then while walking;
   diff the Z components to confirm 0xC19AC8 carries bob and 0x8CD0FC does not.
2. Breakpoint 0x4CDC40 (or memwatch 0xC19AC8) to confirm one write per frame and capture EAX
   (the camera transform pointer = source) for angle-layout confirmation.
3. Swap the proxy's unproject camera source from 0x8CD0F4 → 0xC19AC0 (pos) and A/B the
   character bob.

### LIVE-VERIFIED live-camera semantics + bob/sway fix (2026-07-18, main session)

Verified in-mission (walk via held W, crouch, held LEFT turn; runtime base 0xE90000):

- **0xC19AC8 (live cam Z) carries the bob**: oscillates -2.44..-2.77 while walking,
  settles at exactly `0x8CD0FC + 2.6` at rest. `0x8CD0FC` (cached Z) stays dead flat
  while walking — the cached camera is the camera object base origin, no eye offset.
- **Crouch**: live Z drops ~2.0 (offset vs cached becomes 0.57); cached Z barely moves
  (+0.025). The 2.6 standing offset is the dynamic eye offset, NOT a constant.
- **Cache desync while moving**: cached X leads live X by ~1 unit at walk speed
  (~100ms), so the old camera source also caused depth/lateral swim, not just bob.
- **Angle packing at 0xC21B4C (corrects the inferred table above)**: u16 words read
  [+0x4C]=0x1378, [+0x4E]=0x1378, [+0x50]=0, [+0x52]=pitch (s16, matches 0x8CD0E6),
  [+0x54]=yaw (matches 0x8CD108). So **live pitch = 0xC21B52, live yaw = 0xC21B54**;
  +0x4C/+0x4E purpose unknown (0x1378 twice), +0x50 likely roll (0 at rest).
- **Angle lag**: during a held turn, cached yaw leads live yaw by up to 0xAE0 (~15 deg),
  converging at rest — the "characters sway during mouse-look" artifact.

**Fix (deployed)**: `game::read_camera()` now reads pos from 0xC19AC0 and pitch/yaw from
0xC21B52/0xC21B54 whenever the live pos is nonzero (falls back to the 0x8CD0F4 cache
until the first rendered frame). Walk-bob on unprojected characters confirmed fixed
in-game; the same change also corrects crouch eye height and standing eye height (+2.6).

---

## Drawn weapon dropped the frame to raster — ROOT CAUSE + FIX (2026-07-25)

**Symptom:** drawing the sword/bow knocked the whole frame out of path tracing
until the weapon was holstered.

**Root cause: duplicate scene submission, not the weapon's geometry.**

Equipping a weapon makes NewDark render two extra off-screen passes (the
inventory item icon), each wrapped in its own `BeginScene`/`EndScene`:

```
scene 1  backbuffer     Clear(TARGET|Z) -> 95 world DPUP -> z-only Clear -> overlay draws
scene 2  off-screen     SetRenderTarget(0x0333CC38), SetDepthStencilSurface(NULL),
                        ColorFill, 1 DPUP           <- NO depth surface
scene 3  restore        SetRenderTarget(0x0337E8B0) + depth 0x0337E9A8, 1 DPUP
```

`submit_scene_to_remix()` latched per-*scene* (`m_scene_submitted` reset at
`BeginScene`), so a weapon-out frame submitted the full 74,599-triangle
worldrep plus the entire engine light set **three times a frame** — twice into
an off-screen surface, one with no depth buffer. Remix dropped to raster.

**Fix:** reset the latch at `Present` instead of `BeginScene` — the ray-traced
scene is injected exactly once per frame. The main scene always submits first
(at its z-only overlay clear, else its `EndScene`), so the later off-screen
passes find the latch already set.

**A render-target guard was tried and rejected.** Gating submission on
`GetRenderTarget(0) == GetBackBuffer(0,0,...)` blocked the *main* scene too and
produced a gray, worldrep-less scene: COM interface identity is not preserved
through the Remix bridge wrapper, so the pointers never compared equal. The
per-frame latch needs no pointer comparison.

### What this disproves

`findings.md` previously suggested — and `NEXT_SESSION.md` (2026-07-24) asserted
— that the weapon exposed a *second perspective camera* to Remix via its own
draws. That is **wrong**. Two fixes built on it were removed:

1. Capture/replay of `DrawIndexedPrimitive`. The engine issues **zero** indexed
   draws; the hook never once fired. The FVF 0x142 + VB/IB + perspective-matrix
   block after `EndScene` is a `CreateStateBlock` priming artifact with no draw
   (already documented in the 3-frame capture section above — the 2026-07-24
   work was built on the superseded single-frame reading).
2. Capture/replay of the weapon's `DrawPrimitiveUP` draws. The hook fired
   correctly, but **suppressing the weapon draws entirely did not restore path
   tracing** — which is what ruled the second-camera theory out.

The weapon now passes through as ordinary main-scene geometry and the frame
stays path-traced. The `viewmodel` module and its `[Viewmodel]` INI section were
deleted (recoverable: `backups/2026-07-25_0310_viewmodel-dpup-fix/`).

### Weapon draw signature (for reference)

The weapon *is* two non-RHW `DrawPrimitiveUP` draws — FVF 0x142
(`XYZ|DIFFUSE|TEX1`), stride 28, prims 10 and 2 — classified `other:fvf` by
`unproject` and passed through. Correlation was exact over a 1500-frame
`[Unproject] DebugLogFrames` capture: 517 holstered gameplay frames had **0**,
and all 983 drawn frames had **exactly 2**. Harmless; documented only so a
future reader does not re-derive it as a suspect. Note the menu draws the same
0x142 signature, and it must keep passing through (the launch menu depends on
it).

### Evidence

- `captures/dxtrace_20260725_031448.jsonl` — holstered, 2 frames, 292 records
  each: 1 `BeginScene`, 0 `SetRenderTarget`, 0 `ColorFill`, 1 `SetViewport`.
- `captures/dxtrace_20260725_031526.jsonl` — drawn, same scene: 3 `BeginScene`,
  2 `SetRenderTarget`, 2 `SetDepthStencilSurface`, 2 `ColorFill`,
  5 `SetViewport`.

Capture live with the game running (no relaunch needed):
`python -m graphics.directx.dx9.tracer trigger --game-dir <DIR> --frames 3 --wait`
(the CLI's "Expected output not found" warning is a stale path in the helper —
the proxy writes to `captures/dxtrace_<timestamp>.jsonl`).

**Invariant: do not reintroduce a per-scene submission latch.** Thief runs
multiple scenes per frame whenever a weapon is equipped.

## Escape menu showed frozen raster world — ROOT CAUSE + FIX (2026-07-25)

**Symptom:** pressing Escape in-mission displayed the frozen world in raster
instead of the menu. The launch menu was fine.

**Root cause: the EndScene-fallback scene submission buried the menu.**

A paused menu frame is (live capture `captures/dxtrace_20260725_144249.jsonl`):

```
BeginScene -> 3x DrawPrimitiveUP (tri-fan, stride 28) -> EndScene -> Present
```

No Clear of any kind — the sim is paused, so no world geometry, no HUD, and
no mid-scene z-only overlay clear. `submit_scene_to_remix()` therefore fell
through to the EndScene fallback and injected the frozen worldrep + full
light set AFTER the menu quads, every menu frame. The menu pixels were
covered and, with no clear and a stale camera context, Remix dropped the
frame to raster — the same failure family as the drawn-weapon bug (worldrep
submitted into a scene context Remix does not treat as the main camera pass).

**Menu draw signature** (fresh `unproject_debug.log`, closes the open question
from NEXT_SESSION): the Escape menu is the **0x142 XYZ variant**, not RHW —
3 draws/frame classified `other:fvf` (fvf=0x142, stride 28, prims 2,
v0=(0,0); textured background + two untextured pillarbox bars),
`world=0 ui=0 other=3` on every menu frame.

**Fix (d3d9ex.cpp EndScene):** the EndScene fallback now submits only when
the engine drew world geometry this frame (`unproject::m_converted_draws_frame
> 0`; gameplay frames log world>=27, menu frames exactly 0). The z-only-clear
submission path is untouched, so gameplay frames are unaffected and the
per-frame latch invariant from the weapon fix still holds. With submission
skipped, Remix sees only the three passthrough 0x142 quads — the exact
situation the launch menu already renders correctly. Verified in-game
2026-07-25: menu visible, gameplay still path-traced, drawn weapon still fine.

**Invariant: the EndScene fallback must stay gated on world draws.** An
unconditional fallback resubmits the frozen scene on every paused frame.

## Engine-side culling disable — static analysis (2026-07-25)

### Summary
Thief's per-frame visibility is a **two-stage** system, and the premise in the task
brief needs one correction: `examine_portals (0x4CD1C0)` is **not** the gate that
decides which cells get visited. It only builds the per-cell *outgoing-portal table*
(`g_outgoingPortals 0x810BF0`, count `0x860C34`, cap 0x28000) used for BFS ordering
(region+0x1b "pending predecessors") and for dedup (`0x4CD170`). Patching it changes
traversal *order*, not traversal *membership*.

The real cell gate is **`portal_expand_cell (0x4CC0F0)`**, called once per processed
cell from `portal_traverse_scene` at `0x4CDB63`. For each outgoing portal it applies
three tests before calling `portal_visit_cell (0x4CD490)` (which does
`g_travQueue[g_travFrontierCount++] = destCell`):

| # | Test | Site | Failure branch |
|---|------|------|----------------|
| G1 | portal backface: `dot(plane.xyz, camPos)+plane.d > g_portalPlaneThreshold(0x73D244 = 0.0f)` | `0x4CC206 fcomp` | `0x4CC211: 75 78  jne 0x4CC28B` |
| G2 | screen-space clip of the portal polygon against the parent region's clip rect (`0x4D3340` returns NULL) | `0x4CC221 call` | `0x4CC22D: 74 5C  je 0x4CC28B` |
| G3 | portal draw-distance (only when `g_portalDistCullEnable 0x860C20 != 0`) via `0x4CC0A0` vs `g_portalDistThreshold 0x860D44` | `0x4CC247 call` | `0x4CC251: 75 19  jne 0x4CC26C` (inverted; failure falls through to the free-region path at 0x4CC253) |

**G2 cannot be NOP'd** — `EDI` is the freshly allocated clip-region pointer that
`0x4CD490` consumes; forcing the branch not-taken passes garbage/NULL downstream.

**The engine already contains a fully un-culled expand path.** At the top of
`0x4CC0F0`:

```
0x004CC12B: F6 C1 20      test cl, 0x20        ; cl = WRCell->flags (+0x06)
0x004CC12E: 74 77         je   0x4CC1A7        ; -> normal, culled path
;   fall-through 0x4CC130..0x4CC198 = UNCONDITIONAL path:
;   every portal enqueued via portal_visit_cell_unclipped (0x4CD6C0), which
;   allocates a FULL-SCREEN clip region: 0x4D0A40(*(0x9E4BE8+8), *(0x9E4BE8+0xA))
;   -> no plane test, no screen clip, no distance test.
```

Cell flag bit 0x20 is therefore an engine-native "expand this cell's portals without
culling" flag. Forcing that path for every cell is the single cleanest way to make
the portal walk visit the whole level, and it has the very useful side effect of
giving every visited cell a **full-screen clip rect**, which in turn makes the
object screen-rect test (stage 2) pass automatically.

### Object culling mechanism (the key unknown — now resolved)

Objects are **not** culled by a per-object frustum test. They are culled by
*cell membership plus a screen-rect test against their owning cell's portal clip rect*,
and the final draw is gated by one byte array.

Pipeline, all driven from `WRRenderScene (0x4CE030)`:

1. `memset(g_objInRenderList 0xC255E0, 0, min(numObjs, 0x2000))` — per-frame dedup reset (`0x4CE4B6`).
2. **Gather** — `for i in 0..g_travProcessedCount: obj_gather_in_cell(g_wrCells[g_travQueue[i]])`
   (`0x4CCF00`, driven at `0x4CE4EE`). Walks the cell's resident-object linked list
   (`WRCell+0x2C` into node array `*0x9E51C0`; node `+4`=objId, `+8`=next) and, for each
   object not already flagged in `g_objInRenderList`:
   - creates a display node in `g_objNodes 0xB13820` (stride 10; `+0` objId, `+4` next,
     `+6` cellPtr/type), count `g_objNodeCount 0xC19ACC`, **cap 0x1800**;
   - links it into the cell region's list head `region+0x1c`;
   - records `g_objOwnerCell[objId] = cell` (`0xC19B40`) and `g_objHeadNode[objId]` (`0xC288A0`);
   - appends objId to `g_objRenderList 0xB3B9A0` (count `0xB1378C`, **cap 0x800**).

   **Objects in cells the portal walk never reached are never gathered.** `0x4CCF00`
   dereferences `*(cell+0x28)` (the per-frame region), which is NULL for unvisited
   cells — so you cannot simply widen the driver loop to all cells; it would crash.
3. **Cull test** — `for j in 0..g_objRenderListCount: obj_cull_test(g_objRenderList[j])`
   (`0x4CC950`, driven at `0x4CE511`). Gets the object's screen AABB from `0x5BF340`,
   compares it against the owning cell region's 8-int clip rect (`**(cell+0x28)`), then
   runs a cross-cell occlusion scan (`0x4CC5C0` over `g_travQueue[visitOrder..g_travProcessedCount]`).
   On rect failure it sets **`g_objHidden[objId] = 1`**.
4. `obj_cull_post_pass (0x4CCDE0)`.
5. **Draw** — inside each cell's render (`RenderCell` 0x4D1FB0 / 0x4D2410 / 0x4D2AD0),
   `RenderCell_DrawObjects (0x4D3000)` walks `region+0x1c` and calls the object draw
   callback `(**0x7A84E4)(objId, ctx, nodeType)` **only when `g_objHidden[objId] == 0`**.
   `0x4D2FA0` is the single-object variant.

**`g_objHidden` = byte array at `0x00B39980`, indexed by object id, is THE object
visibility gate.** Writers of `1`: `0x4CC969` (object has no render data — legitimate),
`0x4CCA2E` (clip-rect reject — the cull we want to defeat), `0x4CCD34` (batch pass
`0x4CCBF0`). Readers: `0x4D2FAE`, `0x4D3074`, `0x4D30AF`, `0x4CCAAF`, `0x4CCE3A`, `0x4CD05B`.

`g_useNodeObjPass (0x78BBB9)` = 1 in the shipped image, so the `0x4CC950` / `0x4CCDE0`
path is the live one; the `0x4CCBF0` batch path is the `==0` alternative.

### Light visible-list (task 3) — lighting only, gates nothing

`Light_CullTestToVisibleList (0x5AB210)` has exactly **one** caller:
`Light_GatherAtVertex (0x5AB3E0)`, itself reached from `Light_ShadeObject (0x5AB6D0)`
per shaded object/point. It copies a 0x30-byte light record from
`g_lightTable 0x9EA660` into `g_visibleLights 0xA026A0` (cap 32) and returns
`count < 0x20`. Crucially, the candidate set comes from **the object's own cell's
light index list at `WRCell+0x40`** — *not* from the portal traversal — plus a
radius + LOS test (`0x4DF540`) for dynamic lights. The filled array is consumed only
by `Light_AccumVertexColor (0x5A9450)` to produce a software vertex colour.

**Conclusion:** the 32-entry list is pure software vertex lighting for objects. It
does not gate submission, draw calls, or which lights exist. No patch is needed for
the Remix proxy, which reads the master table at `0x9EA660` directly. The only
observable effect of the 32 cap would be object *shading* accuracy in dense-light
cells, and it is unaffected by cell visibility.

### Proposed patches

All VAs are file/preferred-base (0x400000). The live process observed so far loads at
+0x480000 delta — add that for `livetools mem write`.

**P1 — force un-culled portal expansion for every cell (2 bytes) [primary]**

| | |
|---|---|
| VA | `0x004CC12E` |
| Original | `74 77`  (`je 0x4CC1A7`) |
| Patched | `90 90`  (NOP NOP) |
| Effect | every cell takes the `flags & 0x20` unconditional path: all outgoing portals enqueued via `0x4CD6C0`, no plane/clip/distance test, each visited cell gets a **full-screen** clip region. Traversal flood-fills the whole portal-connected level. |

**P2 — never mark an object hidden on the clip-rect reject (1 byte) [recommended companion]**

| | |
|---|---|
| VA | `0x004CCA34` (imm8 of `C6 85 80 99 B3 00 01` at `0x004CCA2E`) |
| Original | `01` |
| Patched | `00`  (yields `mov byte [ebp+0xB39980], 0`) |
| Effect | the screen-rect reject in `obj_cull_test` no longer hides the object; the "no render data" reject at `0x4CC969` is deliberately left intact. |

**P3 — ignore `g_objHidden` at the draw sites (2 bytes x3) [only if P2 proves insufficient]**

| VA | Original | Patched | Where |
|---|---|---|---|
| `0x004D307A` | `75 08` | `90 90` | `RenderCell_DrawObjects` collect loop |
| `0x004D30B5` | `75 29` | `90 90` | `RenderCell_DrawObjects` overflow loop |
| `0x004D2FB5` | `75 3D` | `90 90` | `RenderCell_DrawObject1` |

Riskier than P2: it also forces the draw callback for objects whose
`PTR_objGetRenderData (0x7A84E8)` returned 0.

**P4 — narrow alternative to P1: kill only the portal backface test (2 bytes)**

| | |
|---|---|
| VA | `0x004CC211` |
| Original | `75 78` (`jne 0x4CC28B`) |
| Patched | `90 90` |
| Effect | back-facing portals are still screen-clipped by G2, so this adds only a handful of cells. Not sufficient for "all cells" — listed for completeness / as a low-risk experiment. |

**P5 — data-only knobs (no code patch, fully reversible with `livetools mem write`)**

- `*(int*)0x00860C20 = 0` — disables the portal draw-distance cull (`0x4CC0A0` / `0x860D44`). Zero risk, small gain.
- `*(float*)0x0073D244` — the portal/poly backface threshold. **Do not touch**: it is shared with world-poly backface tests (`0x4D1600`) and would invert world geometry rendering.

### Capacity / guard analysis for P1

| Guard | Site | Limit | Behaviour on overflow |
|---|---|---|---|
| Region pool | `0x4CD2DC cmp edi,0x5000` | 0x5000 = 20480 regions, one per visited cell | `region_alloc_for_cell` returns 1, cell silently not visited. MAX_CELLS is approx 0x2000, so **safe**. |
| Traversal iteration cap | `0x4CDB69 cmp ebp,0x5000` / `0x4CDB6F 74 10` | 20480 iterations | prints `WARNING: Apparent cell cycle in portal_traverse_scene (cell %d, center %g %g %g)` and clamps `g_travProcessedCount = g_travFrontierCount`. One iteration per cell, so **safe** at <=0x2000 cells. If it ever trips, patch `0x4CDB6F: 74 10 -> 90 90`. |
| Outgoing-portal table | `0x4CD27D cmp eax,0x28000` | 163840 entries | prints `examine_portals: Overflowed outgoing_portals table.`; **safe**. |
| Object display nodes | `0x4CCF00` `g_objNodeCount < 0x1800` | 6144 nodes | sets `g_objGatherLoopBound (0x860C1C) = 0`, aborting the remaining cell gather. **Graceful truncation — expect this on large missions.** |
| Object render list | `0x4CCF00` `count < 0x800` | 2048 objects | silently dropped. **Graceful; likely hit on object-dense missions.** |
| Clip-region pool | `0x7A821C == -2` reached at `0x4CE23C` | pool `0xA73320`, stride 0x20 | prints `ClipAlloc: Scene complexity too high.` **This is the real risk with P1** — one region per portal per cell. Monitor it live. |

### Perf / side effects

- P1 makes `WRRenderScene` software-transform (`WRXformCellVerts`) and rasterize world
  geometry for **every** portal-connected cell, not just the visible set. Thief's world
  render is CPU-bound software T&L plus span fill; expect a large frame-time increase on
  missions with hundreds of cells. Since the proxy freezes/submits world geometry
  separately, that raster work is discarded — pure waste.
- **You cannot stub the cell render to recover it**: object drawing happens *inside*
  `RenderCell` via `RenderCell_DrawObjects (0x4D3000)`. A perf patch would have to skip
  only the render-poly loop inside `0x4D1FB0` / `0x4D2410` / `0x4D2AD0` while preserving
  the `0x4D3000` call. That is a larger, per-renderer patch — treat as follow-up work,
  not part of the minimal set.
- P1 also sets `g_sawUnclippedCell (0x9CD890) = 1`, which makes `WRRenderScene` call
  `0x5F2AB0` once per frame when `0xA6D0E0 != 0`. Verify live that this is benign.
- P2 alone (no P1) is cheap and has essentially zero perf cost: it only rescues objects
  whose cell *was* reached but whose screen AABB fell outside the portal clip rect —
  i.e. NPCs partially or fully behind a doorway edge. It does **not** reach objects in
  unvisited cells.

### Recommended minimal set

1. Start with **P2 alone** (1 byte at `0x4CCA34`) plus **P5** (`*0x860C20 = 0`). Zero
   perf cost; confirms the `g_objHidden` gate is the right lever and fixes the common
   "NPC pops out at the doorway edge" case.
2. If full-level object/light submission is required, add **P1** (2 bytes at `0x4CC12E`).
   With P1's full-screen clip rects, P2 becomes largely redundant but harmless.
3. Reserve **P3** for the case where objects still fail to appear after P1+P2, and
   **P4** only as a diagnostic.

### Suggested live verification

1. Attach, read `g_travProcessedCount` (`0xC28880` + 0x480000 = `0x10A8880`) and
   `g_wrCellCount` (`0xEDDEB8`) each frame. Baseline: processed much less than total.
   After P1 they should converge.
2. `livetools bp 0x94C950` (= `0x4CC950` + 0x480000) and log `g_objHidden[objId]`
   before/after to count how many objects the rect test currently kills per frame.
3. `livetools trace 0x953000` (= `0x4D3000`) counting `(**0x7A84E4)` invocations per
   frame — this is the true "objects submitted" number. Compare pre/post patch.
4. Watch `g_objNodeCount 0xC19ACC` (`0x1099ACC`) and `g_objRenderListCount 0xB1378C`
   (`0xF9378C`) against their 0x1800 / 0x800 caps after P1 to see whether truncation
   kicks in on the target mission.
5. Watch for the console strings `ClipAlloc: Scene complexity too high.` and
   `Apparent cell cycle in portal_traverse_scene` after enabling P1.
6. `mem write` `*0x860C20` (`0xCE0C20`) = 0 first — it is a pure data toggle and needs
   no code patching, so it is the safest thing to try before any byte patch.

### Tooling note
`pyghidra_backend.py decompile` returned `no function found at <va>` for every address
this session even though `status` reports the project analyzed and `funcs` in index.db
has correct boundaries (`0x4CD1C0`, size 263, source=ghidra). The warm daemon on port
27043 also blocked `--cold`. All decompilation above came from the r2ghidra backend
(`--backend pdg`), cross-checked against `retools.disasm` byte output for every patch
site quoted. The pyghidra address-resolution bug is worth a separate look.

## Resident object mesh source — static analysis (2026-07-25)

### Summary
Object (prop/model) geometry is fully resident and camera-independent, exactly like the
worldrep. The chain is **objId -> g_pLocationTable -> world transform**, and separately
**objId -> object render-info slot -> cModelRenderObj -> LGMD model blob**. The LGMD blob is
the *raw on-disk model header kept verbatim in memory* (only rescaled at load), so all its
self-relative offsets (`+0x46 objs, +0x4A mats, +0x4E uv, +0x56 verts, +0x5A light,
+0x5E norms, +0x62 pgons, +0x66 nodes`) are valid at runtime — `Model_SetupPointers
(0x5E33B0)` does nothing but add the model base to each one and publish them to globals.
Model-space vertices are `float3[num_verts]` at `model + *(u32*)(model+0x56)`; polygons are
variable-length records reachable through the model's BSP node tree; per-poly texture is a
material **slot** byte that indexes a 256-entry global handle table the engine fills per
object. Object world transform = `pos float3 @ loc+0x00` + `uint16 angles[3] @ loc+0x10`.
This is everything a proxy needs to submit object meshes without touching the screen-space
RHW path.

The task brief's lead `0x5C33E0` turned out **not** to be the mesh draw — it is a deferred
render-node *enqueue* (see "Correction to the 0x5C33E0 lead" below).

### Key Addresses
| Address | Description |
|---------|-------------|
| 0x009CDB28 | g_pLocationTable — `+0x18` = `Location** byObjId` (stride 4) |
| 0x008FBC00 | g_objRenderInfo[0x800] — stride 0x14, object render-info slots |
| 0x00905C00 | end of g_objRenderInfo (= 0x8FBC00 + 0x800*0x14); also mesh-tex remap table base |
| 0x00790A5C | g_objRenderTypeVtbl[] — stride 0x1C, 7 fn ptrs per render type |
| 0x0094E490 | g_modelTexSlots[256] — texture handle per material slot (pgon.data) |
| 0x0094E790 | g_modelColorSlots[] — flat-colour handle per slot (type&0x60 == 0x40) |
| 0x009D881C | g_mdl_cur — current LGMD model base ptr |
| 0x009D8820/24/28/30/34/38/3C/40 | g_mdl_pObjs / pMats / pUVs / pVerts / pLights / pNorms / pPgons / pNodes |
| 0x009D8848 | g_mdl_pXformVerts — transformed-vertex output buffer base |
| 0x009D884C | g_mdl_pVertColors — per-vertex lighting source (stride 4 or 0xC) |
| 0x009D8850 | g_mdl_pXformNorms — transformed normals (stride 4 in the backface test) |
| 0x009D8860 | g_mdl_polyVertPtrs[] — per-poly array of transformed-vertex pointers |
| 0x009D885C | g_mdl_pJointAngles — ptr to current object's joint float array |
| 0x009D8D80 | g_mdl_vertStrideTable — `[i] = i * g_vertStride`, rebuilt by 0x5E33B0 |
| 0x009D8718 | g_vertStride — transformed-vertex stride, 0x2C or 0x34 |
| 0x009D8750 | g_renderState — `+0x44` current 0x34-byte transform, `+0x188` matrix stack ptr |
| 0x009EA62C / 0x9EA638 | g_objModelPath ("obj\") / g_meshPath ("mesh\") |
| 0x009EA630 / 0x9EA634 | g_objTexPath / g_meshTexPath (obj\txt\, obj\txt16\) |
| 0x005E33B0 | Model_SetupPointers — model base + header offsets -> globals |
| 0x005E3840 | Model_WalkNodes — LGMD BSP node tree walk, emits pgons |
| 0x005E3D10 | Model_DrawPolygon — per-pgon vertex/UV/colour fetch + texture bind |
| 0x005E3450 / 0x5E3560 | Model_PushSubObjXform / Model_PopSubObjXform |
| 0x005E5B60 | Model_CopyAndScale — load-time rescale of a whole LGMD blob |
| 0x005AD290 | Model_BindTextures — fills g_modelTexSlots from the model's material list |
| 0x005BFCA0 | render_queue_raw — "render_queue_raw: Bad model pointer" |
| 0x005D7D60 | Render_SetObjectTransform(Location* pos /*EDI = pos+0x10 angles*/) |
| 0x005BF4C0 | Model_SetupCellClip — objId -> loc, model+0x10 radius -> 0x4D3240 |
| 0x005BEBA0 | Model_ComputeScreenExtent — objId -> loc -> SetObjectTransform |
| 0x005AECE0 | ObjRender_GetBBox — render-type dispatch through 0x790A5C |
| 0x005ACD10 | ObjRender_Init — clears g_objRenderInfo, proves base/stride/count |
| 0x005BF5E0 | ObjJoints_Get — per-object joint angle array (movement_type 1/2) |
| 0x009DBD9C | g_objScreenCache — `*(*0x9DBD9C + objId*4)` lazily-alloc'd bbox/screen-rect record |

### Chain 1 — objId -> world transform (HIGH confidence)

`Model_ComputeScreenExtent (0x5BEBA0)`, disasm at 0x5BEBDF and 0x5BEC5A:

```
0x005BEBDF: mov  ecx, dword ptr [0x9cdb28]    ; g_pLocationTable
0x005BEBE5: mov  edx, dword ptr [ecx + 0x18]  ; -> Location** byObjId
0x005BEBE8: mov  esi, dword ptr [edx + edi*4] ; edi = objId  -> Location*
...
0x005BEC5A: lea  edi, [esi + 0x10]            ; EDI = &loc->angles
0x005BEC5D: push esi                          ; arg = &loc->pos
0x005BEC5E: call 0x5d7d60                     ; Render_SetObjectTransform
```

`Render_SetObjectTransform (0x5D7D60)` then (disasm 0x5D7D73..0x5D7DAE):

```
movzx eax, word [edi+4]  -> call 0x65e7d0   ; rot about 3rd axis
movzx ecx, word [edi+2]  -> call 0x65eaa0   ; rot about 2nd axis
movzx ecx, word [edi+0]  -> call 0x65ea00   ; rot about 1st axis
mov eax,[ebx+8] / mov ecx,[ebx] / mov edx,[ebx+4]   ; translation = loc->pos
... 0x65e0c0 composes into g_renderState+0x44 (0x34 bytes), stack at +0x188
```

So:
```c
struct Location {           // >= 0x16 bytes, rest unmapped
  float  pos[3];            // +0x00 world position
  /* +0x0C unknown (4 bytes) */
  uint16 angles[3];         // +0x10 h,p,b - 65536 == 360 deg, applied [2],[1],[0]
};
```
`Location* loc = ((Location**)(*(char**)0x9CDB28 + 0x18))[objId];`

**Confidence: HIGH** for `pos@+0x00` and `angles@+0x10` (both are direct disasm reads at a
single call site). **MEDIUM** for the exact axis each angle drives — 0x65E7D0/0x65EAA0/0x65EA00
were not decompiled; the Dark convention is heading/pitch/bank but the compose order must be
live-checked. `+0x0C` is unidentified.

`0x5BF4C0` independently confirms the same expression with `objId` in EAX and pairs it with
`model+0x10` (bounding radius) for the cell-plane clip test in `0x4D3240`.

### Chain 2 — objId -> LGMD model (HIGH confidence)

`ObjRender_GetBBox (0x5AECE0)`, `0x5AA0E0` (at 0x5AA1EB/0x5AA1F4) and `0x5BF4C0`:

```c
// slot index is a small int, range-checked [0, 0x800)
if (slot >= 0 && slot < 0x800 && *(uint8*)(0x8FBC0E + slot*0x14) != 0) {
    void** obj = *(void***)(0x8FBC00 + slot*0x14);      // cModelRenderObj*
    LGMDModel* model = obj ? ((GetModel_t)obj[0][0x38/4])(obj) : NULL;  // vtable +0x38
}
```

`ObjRender_Init (0x5ACD10)` proves base/stride/count exactly:
```
puVar2 = 0x8fbc04; do { ...clear +0x00,+0x04,+0x08,+0x0A,+0x0C,+0x0E,+0x0F,+0x10...
                        puVar2 += 5 /*dwords = 0x14*/ } while (puVar2 < 0x905c04);
```
`(0x905C00 - 0x8FBC00) / 0x14 = 0x800`.

```c
struct ObjRenderInfo {      // 0x14 bytes @ 0x8FBC00, [0x800]
  void*  renderObj;         // +0x00 C++ obj, vtable slot 0x38 = GetModel()
  void** texRefs;           // +0x04 per-textured-material resource ptr array (stride 4)
  int    unk08;             // +0x08
  uint16 unk0A;             // +0x0A
  uint16 renderType;        // +0x0C index into g_objRenderTypeVtbl (0x790A5C, stride 0x1C)
  uint8  inUse;             // +0x0E nonzero = slot valid
  uint8  texSubstSet;       // +0x0F index into the 0x905C00 mesh-tex remap table
  int    unk10;             // +0x10
};
```

**OPEN / needs live verification: is the 0x8FBC00 index the same number as `objId`?**
`0x5BF4C0` uses `objId` (EAX, from render-ctx+0x18) for the *location* lookup and a
*different* value (ECX, render-ctx+0x14) for the 0x8FBC00 lookup, so they are at minimum
carried separately. The 0x800 cap also matches `g_objRenderList`'s 0x800 cap, not the
0x2000 objId space used by `g_objInRenderList`/`g_objHidden`. Treat "slot == objId" as
**UNVERIFIED** — this is the single most important thing to check live.

### Chain 3 — LGMD in-memory layout (HIGH confidence)

`Model_SetupPointers (0x5E33B0)` is the proof — it is nothing but header offset + base:

```c
g_mdl_pObjs  (0x9D8820) = model + *(u32*)(model+0x46);
g_mdl_pMats  (0x9D8824) = model + *(u32*)(model+0x4A);
g_mdl_pUVs   (0x9D8828) = model + *(u32*)(model+0x4E);
g_mdl_pVerts (0x9D8830) = model + *(u32*)(model+0x56);
g_mdl_pLights(0x9D8834) = model + *(u32*)(model+0x5A);
g_mdl_pNorms (0x9D8838) = model + *(u32*)(model+0x5E);
g_mdl_pPgons (0x9D883C) = model + *(u32*)(model+0x62);
g_mdl_pNodes (0x9D8840) = model + *(u32*)(model+0x66);
if (model->version >= 4) { matFlags = *(u32*)(model+0x6E); /* bit0, bit1 */ }
numVerts = *(u16*)(model+0x3E);
```

Cross-confirmed by `Model_CopyAndScale (0x5E5B60)`, which `memcpy`s `*(u32*)(model+0x6A)`
bytes (model_size) and then rescales, in order: `+0x24/28/2C` (bbox min), `+0x18/1C/20`
(bbox max), `+0x30/34/38` (parent centre), `+0x10` (radius), `+0x14` (max poly radius),
the `+0x44`-count vhot array at `+0x52` (stride 0x10 = idx + float3), the `+0x3E`-count
vertex array at `+0x56` (stride 0xC), then per sub-object the `+0x39` float3 and the
normals at `+0x5E` — normals scaled by `(sy*sz, sx*sz, sx*sy)`, i.e. the adjugate /
inverse-transpose scale, which is definitive proof `+0x5E` holds **normals**, stride 0xC.

```c
struct LGMDModel {                 // == the on-disk header, kept verbatim
  char   magic[4];                 // +0x00 "LGMD"
  uint32 version;                  // +0x04  (>=4 enables mat_flags/mat_extra)
  char   name[8];                  // +0x08
  float  radius;                   // +0x10   <- used as object bounding radius
  float  max_poly_radius;          // +0x14
  float  bbox_max[3];              // +0x18
  float  bbox_min[3];              // +0x24
  float  parent_cen[3];            // +0x30
  uint16 num_pgons;                // +0x3C
  uint16 num_verts;                // +0x3E   <- CONFIRMED (0x5BFCFF movzx [edi+0x3e])
  uint16 num_parms;                // +0x40
  uint8  num_mats;                 // +0x42   <- CONFIRMED (0x5AD290 loop bound)
  uint8  num_vcalls;               // +0x43
  uint8  num_vhots;                // +0x44
  uint8  num_objs;                 // +0x45   <- CONFIRMED (0x5AA0E0 loop bound, stride 0x5D)
  uint32 off_objs, off_mats, off_uv, off_vhots;      // +0x46 +0x4A +0x4E +0x52
  uint32 off_verts, off_light, off_norms, off_pgons; // +0x56 +0x5A +0x5E +0x62
  uint32 off_nodes;                // +0x66
  uint32 model_size;               // +0x6A
  uint32 mat_flags;                // +0x6E   (v4+)
  uint32 off_mat_extra;            // +0x72   (v4+)
  uint32 size_mat_extra;           // +0x76   (v4+)
};
```

**Vertices are MODEL SPACE.** They are the load-time-rescaled source array; the only
transform applied is the per-object one pushed by `0x5D7D60` into `g_renderState+0x44`,
and the *output* of that goes to a separate buffer (`g_mdl_pXformVerts 0x9D8848`,
stride `g_vertStride` 0x2C/0x34). Nothing writes back into `model+off_verts`.

### Chain 4 — polygons (HIGH confidence)

`Model_DrawPolygon (0x5E3D10)` reads, with `p` = pgon record pointer:

| Field | Offset | Evidence in 0x5E3D10 |
|---|---|---|
| `uint16 index` | +0x00 | (unused in draw) |
| `uint16 data` | +0x02 | `*(*(p+2)*4 + 0x94E490)` texture handle; `*(*(p+2)*4 + 0x94E790)` colour |
| `uint8 type` | +0x04 | `(*(p+1) & 0x78A0AB) \| 0x9D87A5` -> `&7` 1=solid 2=wire 3=textured; `&0x18` lighting; `&0x60` colour source |
| `uint8 num_verts` | +0x05 | every loop bound (`*(p+5)`) |
| `uint16 norm` | +0x06 | backface: `*(g_mdl_pXformNorms + *(p+6)*4) + *(float*)(p+8) < 0x73D258` -> return |
| `float d` | +0x08 | same expression (plane distance) |
| `uint16 vert[n]` | +0x0C | `g_mdl_polyVertPtrs[i] = g_mdl_pXformVerts + g_mdl_vertStrideTable[vert[i]]` |
| `uint16 norm_idx[n]` | +0x0C + n*2 | `vtx[+0x20] = pVertColors[norm_idx[i]]` (or *0xC in RGB mode -> +0x20/+0x2C/+0x30) |
| `uint16 uv_idx[n]` | +0x0C + n*4 | `vtx[+0x24] = pUVs[uv_idx[i]*8+0]; vtx[+0x28] = pUVs[uv_idx[i]*8+4]` |

So the **UV array at `+0x4E` has stride 8 = two floats (u,v)**, indexed per-vertex-per-poly,
and a pgon record is `0x0C + n*2*(2 or 3)` bytes long (3 index arrays only when textured).
Polygon records are therefore **variable length and must be reached by offset**, never by
striding — which is exactly why the model carries a BSP node tree.

Transformed-vertex struct (stride `g_vertStride`, 0x2C default — the same struct the
worldrep path uses, see "Worldrep in-memory layout"): `+0x00/04/08` xyz, `+0x0C` cleared
per-vertex by `render_queue_raw`, `+0x20` colour, `+0x24` u, `+0x28` v, `+0x2C/+0x30`
extra RGB when stride is 0x34.

### Chain 5 — polygon enumeration via the model BSP (HIGH confidence)

`Model_WalkNodes (0x5E3840)` — `switch (*(uint8*)node)`, all pgon references are
`g_mdl_pPgons + (uint16 offset)` and all child references are `g_mdl_pNodes + (uint16 offset)`:

| type | Layout (from the decompile) |
|---|---|
| 0 RAW | `u8 type; float sphere[4] @+0x01; u16 count @+0x11; u16 pgon_off[count] @+0x13` |
| 1 SPLIT | `... u16 n_before @+0x11; u16 norm @+0x13; float d @+0x15; u16 behind @+0x19; u16 front @+0x1B; u16 n_after @+0x1D; u16 pgon_off[] @+0x1F` |
| 2 CALL | `... u16 n_before @+0x11; u16 call_node @+0x13; u16 n_after @+0x15; u16 pgon_off[] @+0x17` |
| 3 VCALL | `u8 type; u16 idx @+0x11` -> handler `*(void**)(0x94E280 + idx*4)` |
| 4 SUBOBJ | 3-byte header, `Model_PushSubObjXform(0x5E3450)` -> recurse at `node+3` -> `Model_PopSubObjXform(0x5E3560)` |

The SPLIT plane test is `g_mdl_pXformNorms[norm] + d < 0x73D258` — **camera-dependent front/back
ordering only**. A proxy can ignore the tree ordering entirely and walk *all* pgon offsets
from every node (types 0/1/2 before+after lists), or decode pgons linearly by walking
`num_pgons` variable-length records from `g_mdl_pPgons` — the linear walk needs `type&7`
to size each record, which is available in the record itself.

### Chain 6 — sub-objects / jointed models (MEDIUM-HIGH confidence)

`0x5AA0E0` strides the sub-object array by **0x5D (93) bytes** and tests `*(sub+0x08) != 0`;
`0x5E3450` reads `*(pSubObjs + idx*0x5D + 8)` as the movement type (1 = rotate, 2 = slide)
and `*(pSubObjs + idx*0x5D + 9)` as the joint index into `*g_mdl_pJointAngles (0x9D885C)`.
`Model_CopyAndScale` rescales `sub+0x39` as a float3.

```c
struct LGMDSubObject {       // stride 0x5D = 93
  char   name[8];            // +0x00
  uint8  movement;           // +0x08  0 = static, 1 = rotate joint, 2 = slide joint
  uint8  joint_idx;          // +0x09  index into the object's joint float array
  /* int32 parm / float min_range / max_range occupy +0x09..+0x14 */
  float  rot[9];             // +0x15  3x3 model-space basis        (MEDIUM)
  float  location[3];        // +0x39  translation                  (HIGH - rescaled by 0x5E5B60)
  /* +0x45 parent + 2 unmapped bytes vs. the 91-byte community layout */
  uint16 vhot_start, num_vhots;      // +0x49 +0x4B   (MEDIUM)
  uint16 point_start, num_points;    // +0x4D +0x4F   (MEDIUM)
  uint16 light_start, num_lights;    // +0x51 +0x53   (MEDIUM)
  uint16 norm_start,  num_norms;     // +0x55 +0x57   (HIGH - used by 0x5E5B60 normal rescale)
  uint16 node_start,  num_nodes;     // +0x59 +0x5B   (MEDIUM)
};
```
The engine's stride is 93, two bytes more than the widely-published 91-byte community
layout; the extra two bytes sit between `location` and `vhot_start` (the only placement
consistent with `0x5E5B60` scaling normals via `+0x55`/`+0x57`). **The tail offsets need
live verification.**

**Rigid models** (`num_objs <= 1`, or all `movement == 0`) need no sub-object handling at
all: one transform, one vertex array. That is the deliverable.

### Chain 7 — textures (HIGH confidence)

**Object textures are a completely separate space from worldrep `rp.texture_id`.**

- Resource paths (`0x5ACAF0`, `0x5ACC20`): world textures come from `fam\`, object textures
  from **`obj\txt\`** (or `obj\txt16\` when the `ObjTextures16` config is set) via
  `g_objTexPath 0x9EA630`; skinned-creature textures from `mesh\txt\` via `0x9EA634`.
  Hi-poly variants prepend `obj\hipoly\` / `mesh\hipoly\`.
- `Model_BindTextures (0x5AD290)`, called from `render_queue_raw` with ECX = render slot,
  EDX = model, walks the model's material list:

```c
uint8* m = model + model->off_mats + 0x11;     // -> mat.slot
for (i = 0; i < model->num_mats; ++i, m += 0x1A) {
    if (m[-1] == 0) {                          // mat.type == 0 -> textured
        void* res = ((void**)*(uint32*)(0x8FBC04 + slot*0x14))[k];
        g_modelTexSlots[ *m ] = res ? MakeTexHandle(res) : g_defaultTexHandle /*0x8FBBF8*/;
        k += 1;
    }
}
```
so the **material record is 0x1A (26) bytes**: `char name[16]; uint8 type; uint8 slot; ...8 more`.

- `Model_DrawPolygon` then binds `g_modelTexSlots[pgon.data]` — i.e. **`pgon.data` IS the
  material slot byte**, not a global texture id. The same function reads `+0x1C` off the
  bound handle for flags and `+0x880`/`+0x884` for u/v scale factors.
- Per-object texture substitution ("Mesh Texture Substitution" property) overrides up to 4
  slots via the `0x905C00` remap table indexed by `ObjRenderInfo.texSubstSet (+0x0F)`.

**How a proxy resolves these to the D3D textures it already sees:** the handle written into
`g_modelTexSlots` is the same engine texture object the renderer later hands to
`SetTexture`, so a proxy can key on it directly — read `g_modelTexSlots[pgon.data]` at
submit time and correlate with the `IDirect3DTexture9*` seen at the D3D9 boundary, the same
way the worldrep path correlates engine bitmaps. This is the **only part of the plan with a
real unknown**: the handle -> `IDirect3DTexture9*` step is not proven statically here.

### Correction to the 0x5C33E0 lead

`0x5C33E0` is a **deferred render-node enqueue**, not a mesh draw. Disasm 0x5C33E0..0x5C34E1:
it calls `(**0x9D7D30)->[0x50](objId, &sortKey)`, clamps the returned int16 sort key to
[-4, 3], appends a 0x14-byte record to the array at `0x91BE60` (counter `0x923828`, cap 0x100)
and links it into one of 8 bucket lists at `0x9180F0` (stride 8). Record layout: `+0x00 objId,
+0x04 arg1, +0x08 u16 arg2, +0x0A/+0x0B bytes from 0x9CD888/0x9CD88C, +0x0C flags, +0x10 next`.

The **flush** is `0x5C4810`, which temporarily swaps `_PTR_objDrawCallback (0x7A84E4)` to
`0x5C35B0` (the immediate-draw variant) and restores `0x5C33E0` at the end. The renderer's
default binding, set in `ObjRender_Init2 (0x5C4C40)`, is `0x7A84E4 = 0x5C2870`,
`0x7A84E0 = 0x5C2840`, `0x7A84EC = 0x5C3270`. So `0x7A84E4` holds three different callbacks
over a frame — a live trace that samples it once will mislead.

Neither `0x5C33E0`, `0x5C35B0` nor `0x5C2870` exists as a function in the Ghidra project
(they are only reachable through that pointer), which is why `pyghidra decompile` returned
"no function found" last session. That was **not** a stale-daemon problem.

### Render context passed to render_queue_raw (0x5BFCA0)
From the disasm `mov ebp,[esp+0x14]` then `[ebp+...]`:

| Off | Meaning |
|---|---|
| +0x00 | stored to 0x78B438 |
| +0x04 | **LGMD model ptr** (the "Bad model pointer" null-check target) |
| +0x08 | transformed-vertex output buffer base |
| +0x0C | start index into the sub-list array at 0x923830 |
| +0x10 | count |
| +0x14 | **object render slot** (index into g_objRenderInfo) — ECX to 0x5AD290 |
| +0x18 | **objId** — EAX to 0x5BF4C0, used for the g_pLocationTable lookup |
| +0x1C | byte flag (1 = clear per-vertex colour first) |

This is the clearest evidence that render slot and objId are carried as two separate values.

### Confidence summary

| Claim | Confidence |
|---|---|
| LGMD header offsets +0x3E/+0x42/+0x45/+0x46..+0x76, model = raw header in memory | HIGH — 0x5E33B0 is literally base+offset for all 8 arrays |
| Vertices float3 model-space @ model+off_verts, count model+0x3E | HIGH |
| Normals float3 @ model+off_norms (adjugate rescale proof) | HIGH |
| UVs float2 (stride 8) @ model+off_uv | HIGH |
| pgon layout +0x02 data / +0x04 type / +0x05 n / +0x06 norm / +0x08 d / +0x0C indices | HIGH |
| pgon variable length, reached via node tree | HIGH |
| Material stride 0x1A, slot byte @+0x11, pgon.data == slot | HIGH |
| Object textures separate space (obj\txt\), not worldrep texture ids | HIGH |
| Location: pos float3 @+0x00, uint16 angles[3] @+0x10 | HIGH |
| Angle -> axis assignment and compose order | MEDIUM |
| ObjRenderInfo base 0x8FBC00 / stride 0x14 / count 0x800 / +0x0E inUse / vtable+0x38 GetModel | HIGH |
| **0x8FBC00 slot index == objId** | **UNVERIFIED — verify first** |
| SubObject stride 0x5D, movement@+0x08, joint@+0x09, location@+0x39 | HIGH |
| SubObject rot@+0x15 and the +0x49..+0x5B tail | MEDIUM |
| g_modelTexSlots handle -> IDirect3DTexture9* correlation | UNPROVEN |
| g_mdl_pJointAngles == 0x9D885C (adjacent to 0x9D8860, easy to confuse) | MEDIUM |

### Suggested Live Verification
Runtime delta is +0x480000 (static 0x400000 -> observed 0x880000).

1. **Slot vs objId (do this first).** `bp 0x5AD290` (runtime 0xA2D290); read ECX (render slot)
   and EDX (model ptr). Separately `bp 0x5BF4C0` (0xA3F4C0) and read EAX (objId) plus the
   stack arg. If they differ, find the mapping before anything else.
2. **Location record.** Read `ptr @ 0xE4DB28` -> `+0x18` -> `[objId*4]`. Dump 0x18 bytes: the
   first 3 floats must be a plausible world position. Then read `uint16[3] @ +0x10` and rotate
   an object in-game (a door) to watch them change.
3. **Model blob.** From the slot, read `[0x8FBC00+slot*0x14]` -> vtable -> `+0x38` -> call it
   (or `bp 0x5E33B0` / runtime 0xA633B0 and read EAX = model ptr). Verify `magic == "LGMD"`
   at +0x00, sane `version` at +0x04, `num_verts` at +0x3E, and that
   `model + *(u32*)(model+0x56)` lands inside the blob (`< *(u32*)(model+0x6A)`).
4. **Vertices are model-space.** Dump `num_verts` float3s from `model+off_verts` and confirm
   they are centred near origin and bounded by `bbox_min/max` at +0x24/+0x18 — *not* world
   coords, and **unchanged as the camera moves** (re-read after moving; this is the whole
   premise of the module).
5. **Polygons.** `bp 0x5E3D10` (0xA63D10), read the pgon ptr, dump 0x0C + n*6 bytes; check
   `num_verts` at +0x05 is 3..8 and every `uint16` index at +0x0C is `< model->num_verts`.
6. **Textures.** At the same breakpoint read `g_modelTexSlots[pgon.data]` (0xDCE490 + data*4)
   and compare with the texture the proxy sees bound on the following draw call.
7. **Sub-objects.** For a multi-part object (door, lever), read `model+0x45` and dump
   `num_objs * 0x5D` bytes from `model+off_objs`; confirm `+0x08` is 1 or 2 and `+0x39` is a
   plausible float3 pivot. For rigid props expect `num_objs <= 1` / all `movement == 0`.
8. **Table walk sanity.** Loop slots 0..0x7FF, count those with `+0x0E != 0`; should roughly
   match the object count in the mission and stay stable frame to frame.

### Notes on skinned / animated models (task 4 — deliberately shallow)
- Two distinct systems. Rigid + jointed props are **LGMD** under `obj\`. Creatures are
  **LGMM-style skinned meshes** under `mesh\` (`g_meshPath 0x9EA638`,
  `g_meshTexPath 0x9EA634`), loaded by `0x5ACC20`/`0x5ACAF0`, and driven by the
  `IMesh` / `cCreature` / `cHumanoidCreature` hierarchy (`IMesh` vtable @ 0x71F46C).
  Errors "likely mismatch between creature type and mesh model for object %d" (0x71F3A0) and
  "corrupt mesh model cal/length data" (0x71F408) belong to that path. Motion data is
  `motiondb.bin` / `mschema\motiondb.bin`.
- Jointed **LGMD** models (doors, levers, chests) are *not* skinned — each sub-object is a
  rigid part with its own `rot`/`location` plus one joint scalar from `ObjJoints_Get
  (0x5BF5E0)`, which pulls the per-object joint float array through property manager
  `*0x9D8548` and caps at 6 joints. A proxy that wants these correct composes
  `objectXform * subObjectXform(joint)` per sub-object; a proxy that ignores joints renders
  them in their rest pose.
- Recommendation: ship rigid-only (`num_objs <= 1` or all `movement == 0`), skip anything
  reached through the `mesh\` path, and revisit jointed props after the rigid path is stable.
