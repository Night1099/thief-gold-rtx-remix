#pragma once

// Dark Engine (NewDark 1.27) runtime light table — all mission lights
// (baked-static, animated, dynamic) in one master array. Animated lights get
// their color fields rewritten by AnimLight_Apply (0x58D2E0), so reading the
// table each frame yields live flicker values for free.
// Static VAs (image base 0x400000); resolve via game::rva.
// Layout live-verified 2026-07-18 (Bafford's: 365 records, exact zero-fill
// after the last). Full evidence: findings.md "Lighting injection".
namespace comp::game
{
	constexpr uint32_t VA_LIGHT_TABLE = 0x9EA660u; // LightRecord[768] (loader reads 0x9000 bytes)
	constexpr uint32_t VA_LIGHT_COUNT = 0xA02CA0u; // int32, number of valid records

	constexpr int LIGHT_TABLE_CAPACITY = 768;

#pragma pack(push, 1)

	// 0x30 = 48 bytes = 12 floats.
	struct LightRecord
	{
		float pos[3];        // +0x00 world x,y,z (z-up, engine units)
		float dir[3];        // +0x0C unit direction (spotlights; zero for omni)
		float color[3];      // +0x18 RGB, engine brightness units (~0..5 observed);
		                     //       zeroed while an animlight is off
		float cone_inner_cos;// +0x24 cos(inner cone half-angle); -1 = omni (full sphere)
		float cone_outer_cos;// +0x28 cos(outer cone half-angle); 0 for omni
		float radius;        // +0x2C range; 0 = infinite (ambient-style base lights)
	};

#pragma pack(pop)

	static_assert(sizeof(LightRecord) == 0x30, "LightRecord");

	inline const LightRecord* light_table() { return rva<LightRecord>(VA_LIGHT_TABLE); }
	inline int light_count() { return *rva<int>(VA_LIGHT_COUNT); }
}
