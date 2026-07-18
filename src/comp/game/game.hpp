#pragma once
#include "structs.hpp"

// Thief Gold (NewDark 1.27) — verified addresses and camera access.
// All VAs are static (image base 0x400000); resolve at runtime via rva().
// Evidence for every address: patches/ThiefGold/findings.md.
namespace comp::game
{
	constexpr uint32_t IMAGE_BASE = 0x400000u;

	// The display layer stores the RAW IDirect3DDevice9* (leaked past any d3d9
	// wrapper via an unwrapped child object) in these globals and issues all
	// scene rendering through them. Overwriting them with the wrapper pointer
	// routes the whole renderer through the proxy.
	constexpr uint32_t VA_RENDER_DEVICE_A = 0x9D915Cu;
	constexpr uint32_t VA_RENDER_DEVICE_B = 0xA1C748u;

	// Per-frame render-camera cache, written by UpdateRenderCamCache (0x538C70).
	// The cached position is the camera object's base origin: it excludes the
	// dynamic eye offset (head-bob while walking, crouch height) and lags
	// ~100ms behind the render position while moving.
	constexpr uint32_t VA_CAM_POS   = 0x8CD0F4u; // float[3] world x,y,z (z-up)
	constexpr uint32_t VA_CAM_PITCH = 0x8CD0E6u; // s16 Dark angle, up = negative
	constexpr uint32_t VA_CAM_YAW   = 0x8CD108u; // u16 Dark angle

	// Live render eye used by the portal renderer's backface test — written at
	// render time each frame, includes the dynamic eye offset (bob + crouch).
	// Live-verified 2026-07-18: z oscillates with walk bob and drops ~2.0 when
	// crouched while VA_CAM_POS stays flat; equals VA_CAM_POS + (0,0,2.6) at
	// rest standing. Zero until the first mission frame renders.
	constexpr uint32_t VA_CAM_LIVE_POS = 0xC19AC0u; // float[3] world x,y,z

	// Live render-camera orientation (duplicate camera copy written per frame
	// by 0x5BD820 alongside pos at 0xC21B40). The cached angles lead these by
	// up to ~15 deg mid-turn (live-verified), which shows as characters
	// swaying laterally during mouse-look; both agree at rest.
	constexpr uint32_t VA_CAM_LIVE_PITCH = 0xC21B52u; // s16 Dark angle, up = negative
	constexpr uint32_t VA_CAM_LIVE_YAW   = 0xC21B54u; // u16 Dark angle

	template <typename T>
	T* rva(const uint32_t static_va) {
		return reinterpret_cast<T*>(EXE_BASE + (static_va - IMAGE_BASE));
	}

	// Dark angle: 0..65535 = full turn
	constexpr float DARK_ANGLE_TO_RAD = 6.28318530718f / 65536.0f;

	struct camera_state
	{
		float pos[3];   // world, z-up
		float yaw;      // radians
		float pitch;    // radians, up = negative (raw engine convention)
		bool valid;
	};

	camera_state read_camera();

	// Replace the game's raw-device globals with the wrapper so scene draws
	// route through the proxy. Safe to call every frame; only writes when a
	// global currently holds the raw device pointer.
	void patch_render_device_globals(IDirect3DDevice9* wrapped, IDirect3DDevice9* raw);

	extern void init_game_addresses();
}
