#pragma once

#include <unordered_map>
#include <vector>

namespace comp
{
	/*
	 * ThiefGold worldrep renderer — phase 3 (hash stability).
	 *
	 * The engine's per-frame render clips world polygons against portals, so the
	 * clipped RHW fans that reach D3D differ frame to frame and Remix's content
	 * hashes churn under camera motion. This module instead submits geometry
	 * from the resident, UN-clipped worldrep cell array (game::g_wrCells): each
	 * render polygon is emitted once as whole world-space triangles with fixed
	 * vertex ordering, so a given surface hashes identically every frame.
	 *
	 * Textures are mapped by snooping the engine's own resolution: the world
	 * texture-resolve callback is hooked to capture the active texture_id, and
	 * the next SetTexture(0,...) on the wrapped device associates it with the
	 * bound IDirect3DTexture9. World geometry is built once and cached (the cell
	 * array is static per mission); the engine's world fans are suppressed.
	 */
	class worldrep_render final : public shared::common::loader::component_module
	{
	public:
		worldrep_render();
		~worldrep_render();

		static inline worldrep_render* p_this = nullptr;
		static worldrep_render* get() { return p_this; }

		static bool is_initialized()
		{
			const auto mod = get();
			return mod && mod->m_initialized;
		}

		bool enabled() const { return m_enabled; }

		// Return true if a DPUP with this engine return address is a world fan
		// (to be suppressed when worldrep submission is active).
		bool is_world_dpup(uintptr_t return_addr) const;

		// Resolver detour records texId -> engine bitmap object (stable per id).
		void note_resolved(uint16_t tex_id, void* engine_obj);

		// Submit cached worldrep geometry with real matrices. Called at EndScene.
		void submit(IDirect3DDevice9* dev);

		void on_reset();

		// Stats
		uint32_t m_buckets = 0;
		uint32_t m_triangles = 0;
		uint32_t m_mapped_textures = 0;

	private:
		struct out_vertex
		{
			float pos[3];
			float normal[3];
			DWORD color;  // baked lightmap attenuation (white when disabled)
			float u, v;   // pre-scale (texel/64 applied via texture matrix at submit)
		};

		struct bucket
		{
			std::vector<out_vertex> verts; // triangle list
		};

		struct cached_tex
		{
			IDirect3DTexture9* tex = nullptr;
			float scale_u = 1.0f; // texel/64 UV scale baked into the FFP texture matrix
			float scale_v = 1.0f;
		};

		bool build_geometry();
		void install_resolver_hook();
		// Create (once) a stable D3D texture from the engine's inline bitmap.
		const cached_tex* get_texture(IDirect3DDevice9* dev, uint16_t tex_id);

		bool m_initialized = false;
		bool m_enabled = true;
		bool m_geometry_built = false;
		const void* m_built_cells_base = nullptr;
		int m_built_cell_count = 0;
		uint32_t m_built_fp = 0;

		std::unordered_map<uint16_t, bucket> m_buckets_by_tex;
		std::unordered_map<uint16_t, size_t> m_skipped_tris_by_tex;
		std::unordered_map<uint16_t, void*> m_obj_by_id;   // texId -> engine bitmap object
		std::unordered_map<uint16_t, cached_tex> m_tex_by_id;
	};
}
