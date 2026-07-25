#include "std_include.hpp"
#include "shared/common/flags.hpp"
#include "shared/common/config.hpp"

#include <algorithm>

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

	namespace
	{
		int s_nocull_mode = 0;

		// Verify-then-write: refuses to patch if the site doesn't hold the
		// expected bytes (wrong exe version), so a mismatch can't corrupt code.
		bool patch_code(const uint32_t va, const std::initializer_list<uint8_t> expected,
			const std::initializer_list<uint8_t> patched, const char* what)
		{
			auto* p = rva<uint8_t>(va);
			if (!std::equal(expected.begin(), expected.end(), p))
			{
				shared::common::log("Game",
					std::format("NoCull: {} at 0x{:X} has unexpected bytes ({:02X} {:02X}) — skipped",
						what, va, p[0], expected.size() > 1 ? p[1] : 0),
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				return false;
			}

			DWORD old_protect = 0;
			if (!VirtualProtect(p, patched.size(), PAGE_EXECUTE_READWRITE, &old_protect)) {
				return false;
			}
			std::copy(patched.begin(), patched.end(), p);
			VirtualProtect(p, patched.size(), old_protect, &old_protect);

			shared::common::log("Game",
				std::format("NoCull: patched {} at 0x{:X}", what, va),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
			return true;
		}
	}

	void apply_no_cull()
	{
		s_nocull_mode = shared::common::config::get().nocull.mode;
		if (s_nocull_mode < 1) {
			return;
		}

		patch_code(VA_NOCULL_OBJ_REJECT, { 0x01 }, { 0x00 },
			"object clip-rect reject (g_objHidden write)");

		if (s_nocull_mode >= 2)
		{
			patch_code(VA_NOCULL_EXPAND_JE, { 0x74, 0x77 }, { 0x90, 0x90 },
				"portal_expand_cell flags je (flood-fill)");
		}
	}

	void no_cull_tick()
	{
		if (s_nocull_mode >= 1) {
			*rva<int>(VA_PORTAL_DRAW_DIST) = 0;
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
