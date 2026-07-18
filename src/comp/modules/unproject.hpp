#pragma once

namespace comp
{
	/*
	 * ThiefGold RHW unprojection.
	 *
	 * NewDark renders the world as CPU-pre-transformed screen-space triangle
	 * fans (XYZRHW, rhw = 1/view_depth) with identity W/V/P — geometry RTX
	 * Remix cannot path-trace. This module intercepts those draws, rebuilds
	 * view-space positions from screen coords + rhw, transforms them to world
	 * space using the engine's live camera state (read from memory), generates
	 * per-fan normals, and re-submits untransformed XYZ geometry with real
	 * View/Projection matrices.
	 *
	 * Correctness property: the GPU reprojects with the same V/P used for
	 * unprojection, so rasterized output is pixel-identical to the original
	 * even while camera-model calibration is imperfect — calibration errors
	 * only affect world-space placement (i.e. Remix lighting), never the image.
	 */
	class unproject final : public shared::common::loader::component_module
	{
	public:
		unproject();
		~unproject();

		static inline unproject* p_this = nullptr;
		static unproject* get() { return p_this; }

		static bool is_initialized()
		{
			const auto mod = get();
			return mod && mod->m_initialized;
		}

		// Called from the device wrapper. dev is the RAW device (draws must not
		// re-enter the wrapper). Returns true if the draw was handled.
		bool on_draw_primitive_up(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT prim_count,
			const void* vertex_data, UINT stride, HRESULT* out_hr);

		// The engine clears ONLY the z-buffer mid-scene right before drawing the
		// HUD overlay models (gem, shields, viewmodel); everything after that
		// clear until the next scene is screen-space UI.
		void on_overlay_clear() { m_overlay_phase = true; }
		void on_scene_begin();

		// Replay captured overlay draws depth-sorted as ortho UI. Called at
		// EndScene, after the ray-traced scene submission.
		void flush_overlay_ui(IDirect3DDevice9* dev);

		void on_present();

		// Live stats for ImGui/log
		uint32_t m_converted_draws_frame = 0;
		uint32_t m_passthrough_ui_frame = 0;
		uint32_t m_passthrough_other_frame = 0;
		uint32_t m_converted_draws_prev = 0;
		uint32_t m_passthrough_ui_prev = 0;
		uint32_t m_passthrough_other_prev = 0;
		game::camera_state m_cam = {};

	private:
		void update_camera_if_needed();
		void build_matrices(float viewport_w, float viewport_h);

		struct overlay_draw
		{
			std::vector<uint8_t> verts;
			UINT out_stride = 0;
			DWORD out_fvf = 0;
			D3DPRIMITIVETYPE type = D3DPT_TRIANGLEFAN;
			UINT prim_count = 0;
			IDirect3DBaseTexture9* tex = nullptr; // AddRef'd until flush
			float sort_z = 0.0f;                  // mean engine screen-z
		};
		std::vector<overlay_draw> m_overlay_draws;

		// Per-DPUP classification logging (armed via [Unproject] DebugLogFrames);
		// consumes one armed frame per present that carried real draw volume.
		void dbg_log_draw(IDirect3DDevice9* dev, const char* verdict, DWORD fvf, UINT stride,
			UINT prim_count, DWORD zenable, float rhw_min, float rhw_max, const float* v0);
		void dbg_frame_boundary();

		bool m_initialized = false;
		bool m_cam_dirty = true;
		bool m_overlay_phase = false;
		int m_dbg_frames_left = 0;
		FILE* m_dbg_file = nullptr;

		D3DXMATRIX m_view;
		D3DXMATRIX m_proj;
		float m_focal = 0.0f;     // pixels; derived from ref FOV + viewport
		float m_center_x = 0.0f;
		float m_center_y = 0.0f;
		float m_basis_right[3] = {};
		float m_basis_up[3] = {};
		float m_basis_fwd[3] = {};

		std::vector<uint8_t> m_scratch; // converted vertex buffer, reused
	};
}
