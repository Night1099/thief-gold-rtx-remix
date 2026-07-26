#pragma once

#include <unordered_set>

struct IDirect3DDevice9;

namespace comp
{
	/*
	 * ThiefGold engine-light injection.
	 *
	 * Reads the Dark engine's master runtime light table (game::light_table)
	 * each frame and mirrors every active light into the D3D9 fixed-function
	 * light state (SetLight/LightEnable, table index = light index). The
	 * Remix runtime converts enabled FFP lights to path-traced sphere lights,
	 * writes them into game captures, and matches them against toolkit light
	 * replacements. Animated (flickering) lights need no special handling:
	 * the engine rewrites their color fields in the same table every frame,
	 * so re-reading the record picks up the flicker.
	 *
	 * Requires rtx.conf to pin rtx.lightConversionSphereLightFixedRadius to
	 * EmitterRadius (radiance is encoded in Range) and to raise
	 * d3d9.maxEnabledLights beyond the runtime's 8-slot default.
	 */
	class engine_lights final : public shared::common::loader::component_module
	{
	public:
		engine_lights();
		~engine_lights();

		static inline engine_lights* p_this = nullptr;
		static engine_lights* get() { return p_this; }

		static bool is_initialized()
		{
			const auto mod = get();
			return mod && mod->m_initialized;
		}

		bool enabled() const { return m_enabled; }

		// Submit all active engine lights for this frame. Called from
		// submit_scene_to_remix before the worldrep draws so FFP light state
		// is flushed with this frame's geometry.
		void submit(IDirect3DDevice9* device);

		// Drop all light state (device reset / mission unload).
		void reset();

		// Stats (read by ImGui)
		uint32_t m_active_lights = 0;
		uint32_t m_skipped_lights = 0;

	private:
		bool m_initialized = false;
		bool m_enabled = true;

		float m_radiance_scale = 1.0f;
		float m_point_scale = 1.0f;
		float m_emitter_radius = 0.4f;
		bool m_skip_infinite = true;
		bool m_force_spot = false;
		float m_cone_angle_deg = 70.0f;
		float m_cone_softness = 0.5f;
		float m_spot_z_offset = 0.0f;

		int m_last_light_count = 0;
		std::unordered_set<int> m_ffp_enabled; // table indices with LightEnable(TRUE)
	};
}
