#include "std_include.hpp"
#include "engine_lights.hpp"

#include "shared/common/config.hpp"

#include <algorithm>
#include <cmath>

namespace comp
{
	void engine_lights::submit(IDirect3DDevice9* device)
	{
		m_active_lights = 0;
		m_skipped_lights = 0;

		if (!m_enabled || !device) {
			return;
		}

		constexpr float PI = 3.14159265f;
		constexpr float DEG2RAD = 0.01745329252f;

		// dxvk-remix rtx_lights.h: radiance threshold the runtime targets at the
		// attenuation end-distance when converting a D3DLIGHT9 to a sphere light.
		constexpr float NEW_LIGHT_END_VALUE = 0.01f;

		const int count = std::min(game::light_count(), game::LIGHT_TABLE_CAPACITY);

		// Count shrinks on mission unload — disable every stale device light.
		if (count < m_last_light_count)
		{
			for (const auto idx : m_ffp_enabled) {
				device->LightEnable(idx, FALSE);
			}
			m_ffp_enabled.clear();
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
				if (const auto it = m_ffp_enabled.find(i); it != m_ffp_enabled.end())
				{
					device->LightEnable(i, FALSE);
					m_ffp_enabled.erase(it);
				}
				m_skipped_lights++;
				continue;
			}

			// SetLight must be issued every frame even when nothing changed:
			// the runtime only re-touches FFP lights on a DirtyLights frame,
			// and untouched lights are garbage-collected after
			// rtx.numFramesToKeepLights (100) frames. Skipping unchanged
			// lights makes every static light vanish ~1.5s after load.
			D3DLIGHT9 l = {};
			l.Diffuse = { rec.color[0], rec.color[1], rec.color[2], 1.0f };
			l.Position = { rec.pos[0], rec.pos[1], rec.pos[2] };

			// cone_inner_cos == -1 is a full sphere (omni); anything tighter
			// with a valid direction is an engine spotlight. With ForceSpot,
			// omni records with a direction are shaped too (torches carry
			// straight-down); cone from the record when valid, else the
			// configured default.
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

			// Radiance is encoded in Range: with Attenuation0=1 and no
			// falloff terms the runtime uses Range as the attenuation
			// end-distance and derives sphere radiance as
			//   NEW_LIGHT_END_VALUE / (pi * r^2) * Range^2,
			// r = rtx.lightConversionSphereLightFixedRadius, which rtx.conf
			// pins to EmitterRadius. Inverted here so the max color
			// component * RadianceScale matches the intended radiance.
			// Omni/point lights additionally take PointLightScale; spots don't.
			const float type_scale = l.Type == D3DLIGHT_POINT ? m_point_scale : 1.0f;
			const float target_radiance =
				std::max({ rec.color[0], rec.color[1], rec.color[2] }) * m_radiance_scale * type_scale;
			l.Range = m_emitter_radius * std::sqrt(target_radiance * PI / NEW_LIGHT_END_VALUE);
			l.Attenuation0 = 1.0f;

			if (SUCCEEDED(device->SetLight(i, &l)))
			{
				if (m_ffp_enabled.insert(i).second) {
					device->LightEnable(i, TRUE);
				}
				m_active_lights++;
			}
			else
			{
				m_ffp_enabled.erase(i);
				m_skipped_lights++;
			}
		}
	}

	void engine_lights::reset()
	{
		// Device light state dies with the reset itself.
		m_ffp_enabled.clear();
		m_last_light_count = 0;
	}

	engine_lights::engine_lights()
	{
		p_this = this;

		const auto& cfg = shared::common::config::get();
		m_enabled = cfg.lights.enabled;
		m_radiance_scale = cfg.lights.radiance_scale;
		m_point_scale = cfg.lights.point_scale;
		m_emitter_radius = cfg.lights.emitter_radius;
		m_skip_infinite = cfg.lights.skip_infinite;
		m_force_spot = cfg.lights.force_spot;
		m_cone_angle_deg = cfg.lights.cone_angle_deg;
		m_cone_softness = cfg.lights.cone_softness;

		m_initialized = true;
		shared::common::log("Lights",
			std::format("Module initialized (enabled={}, radiance_scale={}, emitter_radius={}).",
				m_enabled, m_radiance_scale, m_emitter_radius),
			shared::common::LOG_TYPE::LOG_TYPE_GREEN, false);
	}

	engine_lights::~engine_lights()
	{
		p_this = nullptr;
	}
}
