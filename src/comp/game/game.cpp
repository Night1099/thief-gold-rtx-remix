#include "std_include.hpp"
#include "shared/common/flags.hpp"

namespace comp::game
{
	camera_state read_camera()
	{
		camera_state cam = {};

		// Prefer the live render eye (bob + crouch inclusive); the cached camera
		// is the base origin and reconstructing RHW draws with it leaves the
		// engine's per-frame eye offset baked in as world-space error (objects
		// bob against the static world while the player walks).
		const auto* live = rva<const float>(VA_CAM_LIVE_POS);
		const bool live_valid = live[0] != 0.0f || live[1] != 0.0f || live[2] != 0.0f;
		const auto* pos = live_valid ? live : rva<const float>(VA_CAM_POS);
		cam.pos[0] = pos[0];
		cam.pos[1] = pos[1];
		cam.pos[2] = pos[2];

		const auto yaw_raw = live_valid
			? *rva<const uint16_t>(VA_CAM_LIVE_YAW) : *rva<const uint16_t>(VA_CAM_YAW);
		const auto pitch_raw = live_valid
			? *rva<const int16_t>(VA_CAM_LIVE_PITCH) : *rva<const int16_t>(VA_CAM_PITCH);
		cam.yaw = static_cast<float>(yaw_raw) * DARK_ANGLE_TO_RAD;
		cam.pitch = static_cast<float>(pitch_raw) * DARK_ANGLE_TO_RAD;

		// Cache is zero until the first mission frame
		cam.valid = cam.pos[0] != 0.0f || cam.pos[1] != 0.0f || cam.pos[2] != 0.0f;
		return cam;
	}

	void patch_render_device_globals(IDirect3DDevice9* wrapped, IDirect3DDevice9* raw)
	{
		if (!wrapped || !raw) {
			return;
		}

		for (const auto va : { VA_RENDER_DEVICE_A, VA_RENDER_DEVICE_B })
		{
			auto* slot = rva<IDirect3DDevice9*>(va);
			if (*slot == raw)
			{
				*slot = wrapped;
				shared::common::log("Game",
					std::format("Patched raw device global 0x{:X}: {} -> {}", va, static_cast<void*>(raw), static_cast<void*>(wrapped)),
					shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
			}
		}
	}

	void init_game_addresses()
	{
		shared::common::log("Game",
			std::format("ThiefGold addresses resolved at base 0x{:X}: devA=0x{:X} devB=0x{:X} camPos=0x{:X}",
				static_cast<uint32_t>(EXE_BASE),
				reinterpret_cast<uint32_t>(rva<void>(VA_RENDER_DEVICE_A)),
				reinterpret_cast<uint32_t>(rva<void>(VA_RENDER_DEVICE_B)),
				reinterpret_cast<uint32_t>(rva<void>(VA_CAM_POS))),
			shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}
}
