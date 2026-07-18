#pragma once

// Dark Engine (NewDark 1.27) worldrep — resident level geometry.
// Layout from static analysis (WRParseCells_ext 0x54A900 alloc/store +
// render-side reads 0x4D1FB0/0x4D1900), cross-checked against openDarkEngine's
// on-disk WR structs. Static VAs (image base 0x400000); resolve via game::rva.
// Full evidence: patches/ThiefGold/findings.md "Worldrep in-memory layout".
//
// The cell array is the stable, world-space, UN-clipped source of world
// geometry — submitting from here (instead of the engine's per-frame clipped
// RHW fans) is what makes Remix geometry hashes stable across camera motion.
namespace comp::game
{
	constexpr uint32_t VA_WR_CELLS = 0xA35EE0u;      // WRCell* g_wrCells[]
	constexpr uint32_t VA_WR_CELL_COUNT = 0xA5DEB8u; // int32
	constexpr uint32_t VA_TEX_RESOLVE_PTR = 0x7A84D8u; // fn ptr slot: __cdecl (uint32 texId) -> engine tex
	constexpr uint32_t VA_LM_32BIT = 0x9CDC48u;      // lightmap depth mode: 0 = 16-bit, else 32-bit ARGB

	// FlushPortalCacheDPUP (0x60CE80–0x60CFC8): the world-geometry DrawPrimitiveUP
	// site. A DPUP whose return address is in this range is a world fan (vs an
	// object/particle draw).
	constexpr uint32_t VA_PORTAL_FLUSH_LO = 0x60CE80u;
	constexpr uint32_t VA_PORTAL_FLUSH_HI = 0x60CFC8u;

	// __cdecl resolve(uint32 texId) -> engine texture object. Stable per id.
	// The object holds the raw bitmap inline: width/height (uint16) at +0x08,
	// A8R8G8B8 pixels at +0x40. (The engine uploads these to a recycled surface
	// cache at bind time, so we build our own stable textures from the bitmap.)
	typedef void* (__cdecl* tex_resolve_t)(uint32_t tex_id);
	constexpr uint32_t ENGTEX_DIM_OFF = 0x08;    // uint16 width, uint16 height
	constexpr uint32_t ENGTEX_PIXELS_OFF = 0x40; // A8R8G8B8, width*height

#pragma pack(push, 1)

	// 8 bytes. One per polygon (render polys first, then portals).
	struct WRPoly
	{
		uint8_t flags;
		uint8_t num_verts;   // count of entries this poly consumes in the index list
		uint8_t plane_id;    // index into cell planes
		uint8_t pad;
		uint16_t dest_cell;  // portal target leaf (portals only)
		uint16_t unk;
	};

	// 0x34 = 52 bytes. One per render (textured) poly.
	struct WRRenderPoly
	{
		float tex_u_axis[3]; // +0x00 texture U basis (NOT normalized; encodes scale)
		float tex_v_axis[3]; // +0x0C texture V basis
		int16_t u_base_fx;   // +0x18 fixed16, /4096 for float shift
		int16_t v_base_fx;   // +0x1A
		uint8_t origin_vertex; // +0x1C poly-local index of texturing origin
		uint8_t pad0[3];
		uint16_t texture_id; // +0x20 -> resolve via VA_TEX_RESOLVE_PTR
		uint16_t cache_handle; // +0x22 runtime
		uint8_t tail[0x34 - 0x24];
	};

	// 0x10 = 16 bytes. Plane equation a*x+b*y+c*z+d=0.
	struct WRPlane
	{
		float a, b, c, d;
	};

	// 0x14 = 20 bytes. Per render-poly lightmap descriptor. Pixel buffer is
	// allocated at mission load and stable until WorldRep_Free; layer 0 is the
	// static base, then one w*h layer per set bit of light_mask (LSB-first).
	struct WRLightmapInfo
	{
		int16_t lm_u_base;   // +0x00 luxel-space U origin
		int16_t lm_v_base;   // +0x02 luxel-space V origin
		int16_t lm_width;    // +0x04 luxel width
		uint8_t lm_height;   // +0x06 luxel height
		uint8_t pad;         // +0x07
		void* p_lightmap;    // +0x08 texels, bpp from VA_LM_32BIT (4 default, 2 in mode 0)
		uint32_t dyn_lightmap; // +0x0C runtime dynamic composite ptr (0 at load)
		uint32_t light_mask; // +0x10 animlight bitmask
	};

	// 0x54 = 84 bytes. Header with uint8 counts + malloc'd sub-arrays.
	struct WRCell
	{
		uint8_t num_vertices;    // +0x00
		uint8_t num_polys;       // +0x01
		uint8_t num_render_polys;// +0x02
		uint8_t num_portal_polys;// +0x03
		uint8_t num_planes;      // +0x04
		uint8_t pad0;
		uint8_t flags;           // +0x06
		uint8_t render_disable;  // +0x07
		float (*p_vertices)[3];  // +0x08 world-space float3[num_vertices]
		WRPoly* p_poly_list;     // +0x0C [num_polys]
		WRPoly* p_portal_polys;  // +0x10
		WRRenderPoly* p_render_polys; // +0x14 [num_render_polys]
		uint8_t* p_vertex_index_list; // +0x18 flat uint8 run per poly
		uint8_t pad1[0x24 - 0x1C];
		WRPlane* p_planes;       // +0x24 [num_planes]
		uint8_t pad2[0x3C - 0x28];
		WRLightmapInfo* p_lightmap_info; // +0x3C [num_render_polys]
		uint8_t pad3[0x44 - 0x40];
		float center[3];         // +0x44 bounding-sphere center (interior side)
		uint8_t pad4[0x54 - 0x50];
	};

#pragma pack(pop)

	static_assert(sizeof(WRPoly) == 8, "WRPoly");
	static_assert(sizeof(WRRenderPoly) == 0x34, "WRRenderPoly");
	static_assert(sizeof(WRPlane) == 0x10, "WRPlane");
	static_assert(sizeof(WRCell) == 0x54, "WRCell");

	inline WRCell** wr_cells() { return rva<WRCell*>(VA_WR_CELLS); }
	inline int wr_cell_count() { return *rva<int>(VA_WR_CELL_COUNT); }
}
