#include "std_include.hpp"
#include "engine_lights.hpp"

#include "shared/common/config.hpp"
#include "shared/common/remix_api.hpp"

#include <algorithm>
#include <cmath>

namespace comp
{
	namespace
	{
		// Stable per-table-index Remix light hash. Remix identifies a light
		// across frames by this value; the table index is stable per mission.
		constexpr uint64_t LIGHT_HASH_BASE = 0x7401EF11C4700000ull;

		inline bool nearly_equal(const float a, const float b)
		{
			return std::fabs(a - b) < 1e-4f;
		}
	}

	void engine_lights::submit(IDirect3DDevice9* device)
	{
		m_active_lights = 0;
		m_skipped_lights = 0;

		if (!m_enabled) {
			return;
		}

		if (m_ffp)
		{
			if (device) {
				submit_ffp(device);
			}
			return;
		}

		if (shared::common::remix_api::is_initialized()) {
			submit_api();
		}
	}

	void engine_lights::submit_api()
	{
		auto& api = shared::common::remix_api::get();

		const int count = std::min(game::light_count(), game::LIGHT_TABLE_CAPACITY);

		// Count shrinks on mission unload — drop every stale handle.
		if (count < m_last_light_count) {
			reset();
		}
		m_last_light_count = count;

		const auto* table = game::light_table();

		for (int i = 0; i < count; ++i)
		{
			const auto& rec = table[i];

			// Animlights that are off have zeroed colors; slot 0 is the engine's
			// reserved/ambient slot and also fails this test when unused.
			const bool lit = rec.color[0] > 0.0f || rec.color[1] > 0.0f || rec.color[2] > 0.0f;
			const bool infinite = rec.radius <= 0.0f;

			if (!lit || (infinite && m_skip_infinite))
			{
				if (const auto it = m_tracked.find(i); it != m_tracked.end())
				{
					if (it->second.handle) {
						api.m_bridge.DestroyLight(it->second.handle);
					}
					m_tracked.erase(it);
				}
				m_skipped_lights++;
				continue;
			}

			auto& t = m_tracked[i];

			const bool changed = !t.handle
				|| !nearly_equal(t.pos[0], rec.pos[0])
				|| !nearly_equal(t.pos[1], rec.pos[1])
				|| !nearly_equal(t.pos[2], rec.pos[2])
				|| !nearly_equal(t.color[0], rec.color[0])
				|| !nearly_equal(t.color[1], rec.color[1])
				|| !nearly_equal(t.color[2], rec.color[2])
				|| !nearly_equal(t.radius, rec.radius);

			if (changed)
			{
				if (t.handle)
				{
					api.m_bridge.DestroyLight(t.handle);
					t.handle = nullptr;
				}

				remixapi_LightInfoSphereEXT ext = {};
				ext.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
				ext.position = remixapi_Float3D{ rec.pos[0], rec.pos[1], rec.pos[2] };
				ext.radius = m_emitter_radius;

				// cone_inner_cos == -1 is a full sphere (omni); anything tighter
				// with a valid direction is an engine spotlight. With ForceSpot,
				// omni records are shaped too: engine direction when present
				// (torches carry straight-down), else down; cone from +0x28 when
				// it encodes a real angle, else the configured default.
				constexpr float RAD2DEG = 57.29577951f;
				const bool has_dir = rec.dir[0] != 0.0f || rec.dir[1] != 0.0f || rec.dir[2] != 0.0f;
				const bool engine_spot = rec.cone_inner_cos > -0.999f && has_dir;

				if (engine_spot)
				{
					const float inner_rad = std::acos(std::clamp(rec.cone_inner_cos, -1.0f, 1.0f));
					const float outer_rad = std::acos(std::clamp(rec.cone_outer_cos, -1.0f, 1.0f));

					ext.shaping_hasvalue = TRUE;
					ext.shaping_value.direction = remixapi_Float3D{ rec.dir[0], rec.dir[1], rec.dir[2] };
					ext.shaping_value.coneAngleDegrees = outer_rad * RAD2DEG;
					ext.shaping_value.coneSoftness = outer_rad > 0.0f
						? std::clamp((outer_rad - inner_rad) / outer_rad, 0.0f, 1.0f)
						: 0.0f;
					ext.shaping_value.focusExponent = 0.0f;
				}
				else if (m_force_spot && has_dir)
				{
					// No direction in the record -> stays a sphere light.
					const bool cone_valid = rec.cone_outer_cos > 0.0f && rec.cone_outer_cos < 0.999f;
					const float angle_deg = cone_valid
						? std::acos(rec.cone_outer_cos) * RAD2DEG
						: m_cone_angle_deg;

					ext.shaping_hasvalue = TRUE;
					ext.shaping_value.direction = remixapi_Float3D{ rec.dir[0], rec.dir[1], rec.dir[2] };
					ext.shaping_value.coneAngleDegrees = angle_deg;
					ext.shaping_value.coneSoftness = m_cone_softness;
					ext.shaping_value.focusExponent = 0.0f;
				}

				remixapi_LightInfo info = {};
				info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
				info.pNext = &ext;
				info.hash = LIGHT_HASH_BASE + static_cast<uint64_t>(i);
				info.radiance = remixapi_Float3D{
					rec.color[0] * m_radiance_scale,
					rec.color[1] * m_radiance_scale,
					rec.color[2] * m_radiance_scale
				};

				if (api.m_bridge.CreateLight(&info, &t.handle) != REMIXAPI_ERROR_CODE_SUCCESS)
				{
					t.handle = nullptr;
					m_tracked.erase(i);
					m_skipped_lights++;
					continue;
				}

				std::memcpy(t.pos, rec.pos, sizeof(t.pos));
				std::memcpy(t.color, rec.color, sizeof(t.color));
				t.radius = rec.radius;
			}

			api.m_bridge.DrawLightInstance(t.handle);
			m_active_lights++;
		}
	}

	void engine_lights::submit_ffp(IDirect3DDevice9* device)
	{
		constexpr float PI = 3.14159265f;
		constexpr float DEG2RAD = 0.01745329252f;

		// dxvk-remix rtx_lights.h: radiance threshold the runtime targets at the
		// attenuation end-distance when converting a D3DLIGHT9 to a sphere light.
		constexpr float NEW_LIGHT_END_VALUE = 0.01f;

		const int count = std::min(game::light_count(), game::LIGHT_TABLE_CAPACITY);

		// Count shrinks on mission unload — disable every stale device light.
		if (count < m_last_light_count)
		{
			for (const auto& [idx, t] : m_tracked)
			{
				if (t.ffp_enabled) {
					device->LightEnable(idx, FALSE);
				}
			}
			m_tracked.clear();
		}
		m_last_light_count = count;

		const auto* table = game::light_table();

		for (int i = 0; i < count; ++i)
		{
			const auto& rec = table[i];

			const bool lit = rec.color[0] > 0.0f || rec.color[1] > 0.0f || rec.color[2] > 0.0f;
			const bool infinite = rec.radius <= 0.0f;

			if (!lit || (infinite && m_skip_infinite))
			{
				if (const auto it = m_tracked.find(i); it != m_tracked.end())
				{
					if (it->second.ffp_enabled) {
						device->LightEnable(i, FALSE);
					}
					m_tracked.erase(it);
				}
				m_skipped_lights++;
				continue;
			}

			auto& t = m_tracked[i];

			const bool changed = !t.ffp_enabled
				|| !nearly_equal(t.pos[0], rec.pos[0])
				|| !nearly_equal(t.pos[1], rec.pos[1])
				|| !nearly_equal(t.pos[2], rec.pos[2])
				|| !nearly_equal(t.color[0], rec.color[0])
				|| !nearly_equal(t.color[1], rec.color[1])
				|| !nearly_equal(t.color[2], rec.color[2])
				|| !nearly_equal(t.radius, rec.radius);

			if (changed)
			{
				D3DLIGHT9 l = {};
				l.Diffuse = { rec.color[0], rec.color[1], rec.color[2], 1.0f };
				l.Position = { rec.pos[0], rec.pos[1], rec.pos[2] };

				// Radiance is encoded in Range: with Attenuation0=1 and no
				// falloff terms the runtime uses Range as the attenuation
				// end-distance and derives sphere radiance as
				//   NEW_LIGHT_END_VALUE / (pi * r^2) * Range^2,
				// r = rtx.lightConversionSphereLightFixedRadius, which rtx.conf
				// pins to EmitterRadius. Inverted here so the max color
				// component * RadianceScale matches the API path's radiance.
				const float target_radiance =
					std::max({ rec.color[0], rec.color[1], rec.color[2] }) * m_radiance_scale;
				l.Range = m_emitter_radius * std::sqrt(target_radiance * PI / NEW_LIGHT_END_VALUE);
				l.Attenuation0 = 1.0f;

				const bool has_dir = rec.dir[0] != 0.0f || rec.dir[1] != 0.0f || rec.dir[2] != 0.0f;
				const bool engine_spot = rec.cone_inner_cos > -0.999f && has_dir;

				if (engine_spot)
				{
					l.Type = D3DLIGHT_SPOT;
					l.Direction = { rec.dir[0], rec.dir[1], rec.dir[2] };
					l.Theta = 2.0f * std::acos(std::clamp(rec.cone_inner_cos, -1.0f, 1.0f));
					l.Phi = 2.0f * std::acos(std::clamp(rec.cone_outer_cos, -1.0f, 1.0f));
				}
				else if (m_force_spot && has_dir)
				{
					const bool cone_valid = rec.cone_outer_cos > 0.0f && rec.cone_outer_cos < 0.999f;
					const float outer_rad = cone_valid
						? std::acos(rec.cone_outer_cos)
						: m_cone_angle_deg * DEG2RAD;

					l.Type = D3DLIGHT_SPOT;
					l.Direction = { rec.dir[0], rec.dir[1], rec.dir[2] };
					l.Phi = 2.0f * outer_rad;
					l.Theta = 2.0f * outer_rad * (1.0f - m_cone_softness);
				}
				else
				{
					l.Type = D3DLIGHT_POINT;
				}

				const bool is_new = !t.ffp_enabled;
				if (SUCCEEDED(device->SetLight(i, &l)))
				{
					if (is_new) {
						device->LightEnable(i, TRUE);
					}
					t.ffp_enabled = true;
					std::memcpy(t.pos, rec.pos, sizeof(t.pos));
					std::memcpy(t.color, rec.color, sizeof(t.color));
					t.radius = rec.radius;
				}
				else
				{
					m_tracked.erase(i);
					m_skipped_lights++;
					continue;
				}
			}

			m_active_lights++;
		}
	}

	void engine_lights::reset()
	{
		if (shared::common::remix_api::is_initialized())
		{
			auto& api = shared::common::remix_api::get();
			for (auto& [idx, t] : m_tracked)
			{
				if (t.handle) {
					api.m_bridge.DestroyLight(t.handle);
				}
			}
		}
		m_tracked.clear();
		m_last_light_count = 0;
	}

	engine_lights::engine_lights()
	{
		p_this = this;

		const auto& cfg = shared::common::config::get();
		m_enabled = cfg.lights.enabled;
		m_ffp = cfg.lights.ffp;
		m_radiance_scale = cfg.lights.radiance_scale;
		m_emitter_radius = cfg.lights.emitter_radius;
		m_skip_infinite = cfg.lights.skip_infinite;
		m_force_spot = cfg.lights.force_spot;
		m_cone_angle_deg = cfg.lights.cone_angle_deg;
		m_cone_softness = cfg.lights.cone_softness;

		m_initialized = true;
		shared::common::log("Lights",
			std::format("Module initialized (enabled={}, mode={}, radiance_scale={}, emitter_radius={}).",
				m_enabled, m_ffp ? "ffp" : "api", m_radiance_scale, m_emitter_radius),
			shared::common::LOG_TYPE::LOG_TYPE_GREEN, false);
	}

	engine_lights::~engine_lights()
	{
		p_this = nullptr;
	}
}
