#pragma once

#include "shared/common/config.hpp"

namespace comp
{
	// Camera basis + D3D matrices derived from the engine's live camera state.
	// Shared by the unproject (per-draw RHW reconstruction) and worldrep
	// (whole-polygon submission) paths so both agree on the world transform.
	struct camera_frame
	{
		D3DXMATRIX view;
		D3DXMATRIX proj;
		float right[3];
		float up[3];
		float fwd[3];
		float focal;    // pixels
		float center_x;
		float center_y;
	};

	inline float cm_dot3(const float* a, const float* b)
	{
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	}

	inline void cm_cross3(const float* a, const float* b, float* out)
	{
		out[0] = a[1] * b[2] - a[2] * b[1];
		out[1] = a[2] * b[0] - a[0] * b[2];
		out[2] = a[0] * b[1] - a[1] * b[0];
	}

	inline camera_frame build_camera_frame(const game::camera_state& cam, const float viewport_w, const float viewport_h)
	{
		const auto& cfg = shared::common::config::get();
		camera_frame f = {};

		f.center_x = viewport_w * 0.5f;
		f.center_y = viewport_h * 0.5f;

		// Engine FOV is defined for a 4:3 reference; square pixels → one focal
		// length for both axes.
		const float fov_ref_rad = cfg.unproject.fov_ref_deg * 0.01745329252f;
		f.focal = (viewport_h * (4.0f / 3.0f) * 0.5f) / std::tan(fov_ref_rad * 0.5f);

		const float yaw = cam.yaw * static_cast<float>(cfg.unproject.yaw_sign)
			+ cfg.unproject.yaw_offset_deg * 0.01745329252f;
		const float pitch_v = -cam.pitch * static_cast<float>(cfg.unproject.pitch_sign);

		const float cy = std::cos(yaw), sy = std::sin(yaw);
		const float cp = std::cos(pitch_v), sp = std::sin(pitch_v);

		// World is z-up. Level forward = (cos yaw, sin yaw, 0).
		f.fwd[0] = cy * cp; f.fwd[1] = sy * cp; f.fwd[2] = sp;
		f.right[0] = sy;    f.right[1] = -cy;   f.right[2] = 0.0f;
		cm_cross3(f.right, f.fwd, f.up);

		const float* eye = cam.pos;
		f.view = D3DXMATRIX(
			f.right[0], f.up[0], f.fwd[0], 0.0f,
			f.right[1], f.up[1], f.fwd[1], 0.0f,
			f.right[2], f.up[2], f.fwd[2], 0.0f,
			-cm_dot3(f.right, eye), -cm_dot3(f.up, eye), -cm_dot3(f.fwd, eye), 1.0f);

		const float fovy = 2.0f * std::atan((viewport_h * 0.5f) / f.focal);
		D3DXMatrixPerspectiveFovLH(&f.proj, fovy, viewport_w / viewport_h,
			cfg.unproject.z_near, cfg.unproject.z_far);
		return f;
	}
}
