#pragma once

#include <unordered_map>

namespace comp
{
	/*
	 * ThiefGold engine-light injection.
	 *
	 * Reads the Dark engine's master runtime light table (game::light_table)
	 * each frame and mirrors every active light into RTX Remix as a sphere
	 * light via the Remix bridge API. Animated (flickering) lights need no
	 * special handling: the engine rewrites their color fields in the same
	 * table every frame, so re-reading the record picks up the flicker.
	 *
	 * Each table index maps to a stable Remix light hash, so Remix tracks a
	 * given torch as one persistent light. Handles are recreated only when a
	 * record's values actually change; unchanged lights just re-submit.
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

		// Submit all active engine lights for this frame. Called at EndScene.
		void submit();

		// Destroy all Remix light handles (device reset / mission unload).
		void reset();

		// Stats (read by ImGui)
		uint32_t m_active_lights = 0;
		uint32_t m_skipped_lights = 0;

	private:
		struct tracked_light
		{
			remixapi_LightHandle handle = nullptr;
			float pos[3] = {};
			float color[3] = {};
			float radius = 0.0f;
		};

		bool m_initialized = false;
		bool m_enabled = true;

		float m_radiance_scale = 1.0f;
		float m_emitter_radius = 0.4f;
		bool m_skip_infinite = true;
		bool m_force_spot = false;
		float m_cone_angle_deg = 70.0f;
		float m_cone_softness = 0.5f;

		int m_last_light_count = 0;
		std::unordered_map<int, tracked_light> m_tracked; // table index -> handle
	};
}
