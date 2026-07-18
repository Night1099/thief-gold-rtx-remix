#include "std_include.hpp"
#include "unproject.hpp"

#include "shared/common/config.hpp"
#include "camera_math.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace comp
{
	namespace
	{
		struct rhw_vertex_layout
		{
			bool valid = false;
			int off_diffuse = -1;
			int off_specular = -1;
			int tex_count = 0;
			uint32_t stride = 0;
			DWORD out_fvf = 0;
			uint32_t out_stride = 0;
		};

		rhw_vertex_layout parse_rhw_fvf(const DWORD fvf, const UINT stream_stride)
		{
			rhw_vertex_layout l = {};

			if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW) {
				return l;
			}

			uint32_t off = 16; // x,y,z,rhw
			l.out_fvf = D3DFVF_XYZ | D3DFVF_NORMAL;
			l.out_stride = 24;

			if (fvf & D3DFVF_DIFFUSE) {
				l.off_diffuse = static_cast<int>(off);
				off += 4;
				l.out_fvf |= D3DFVF_DIFFUSE;
				l.out_stride += 4;
			}
			if (fvf & D3DFVF_SPECULAR) {
				l.off_specular = static_cast<int>(off);
				off += 4;
				l.out_fvf |= D3DFVF_SPECULAR;
				l.out_stride += 4;
			}

			l.tex_count = static_cast<int>((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
			off += l.tex_count * 8; // 2D coord sets only (all observed Thief FVFs)
			l.out_fvf |= (fvf & D3DFVF_TEXCOUNT_MASK);
			l.out_stride += l.tex_count * 8;

			l.stride = off;
			l.valid = l.stride == stream_stride && l.tex_count <= 2;
			return l;
		}

		uint32_t vertex_count_for(const D3DPRIMITIVETYPE type, const UINT prim_count)
		{
			switch (type)
			{
			case D3DPT_TRIANGLEFAN:
			case D3DPT_TRIANGLESTRIP: return prim_count + 2;
			case D3DPT_TRIANGLELIST:  return prim_count * 3;
			default: return 0;
			}
		}

		inline void cross3(const float* a, const float* b, float* out)
		{
			out[0] = a[1] * b[2] - a[2] * b[1];
			out[1] = a[2] * b[0] - a[0] * b[2];
			out[2] = a[0] * b[1] - a[1] * b[0];
		}

		inline float dot3(const float* a, const float* b)
		{
			return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		}

		inline bool normalize3(float* v)
		{
			const float len_sq = dot3(v, v);
			if (len_sq < 1e-12f) {
				return false;
			}
			const float inv = 1.0f / std::sqrt(len_sq);
			v[0] *= inv; v[1] *= inv; v[2] *= inv;
			return true;
		}

	}

	// ----

	void unproject::on_scene_begin()
	{
		m_overlay_phase = false;
		for (auto& od : m_overlay_draws)
		{
			if (od.tex) {
				od.tex->Release();
			}
		}
		m_overlay_draws.clear();
	}

	void unproject::flush_overlay_ui(IDirect3DDevice9* dev)
	{
		if (m_overlay_draws.empty()) {
			return;
		}

		D3DVIEWPORT9 vp = {};
		dev->GetViewport(&vp);
		if (!vp.Width || !vp.Height) {
			on_scene_begin();
			return;
		}

		// The engine alternates the submission order of overlapping overlay
		// elements (gem jewel vs frame) and relies on z-testing; Remix's UI
		// raster path has no usable depth, so sort far-to-near ourselves.
		std::stable_sort(m_overlay_draws.begin(), m_overlay_draws.end(),
			[](const overlay_draw& a, const overlay_draw& b) { return a.sort_z > b.sort_z; });

		D3DXMATRIX prev_world, prev_view, prev_proj, ortho;
		dev->GetTransform(D3DTS_WORLD, &prev_world);
		dev->GetTransform(D3DTS_VIEW, &prev_view);
		dev->GetTransform(D3DTS_PROJECTION, &prev_proj);
		D3DXMatrixOrthoOffCenterLH(&ortho,
			0.0f, static_cast<float>(vp.Width),
			static_cast<float>(vp.Height), 0.0f, 0.0f, 1.0f);
		dev->SetTransform(D3DTS_WORLD, &shared::globals::IDENTITY);
		dev->SetTransform(D3DTS_VIEW, &shared::globals::IDENTITY);
		dev->SetTransform(D3DTS_PROJECTION, &ortho);

		DWORD prev_fvf = 0, prev_zenable = D3DZB_TRUE, prev_zwrite = TRUE, prev_lighting = FALSE;
		DWORD prev_ablend = FALSE, prev_src = D3DBLEND_ONE, prev_dst = D3DBLEND_ZERO, prev_atest = FALSE;
		IDirect3DBaseTexture9* prev_tex = nullptr;
		dev->GetFVF(&prev_fvf);
		dev->GetTexture(0, &prev_tex);
		dev->GetRenderState(D3DRS_ZENABLE, &prev_zenable);
		dev->GetRenderState(D3DRS_ZWRITEENABLE, &prev_zwrite);
		dev->GetRenderState(D3DRS_LIGHTING, &prev_lighting);
		dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &prev_ablend);
		dev->GetRenderState(D3DRS_SRCBLEND, &prev_src);
		dev->GetRenderState(D3DRS_DESTBLEND, &prev_dst);
		dev->GetRenderState(D3DRS_ALPHATESTENABLE, &prev_atest);

		dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);

		for (const auto& od : m_overlay_draws)
		{
			dev->SetTexture(0, od.tex);
			dev->SetFVF(od.out_fvf);
			dev->DrawPrimitiveUP(od.type, od.prim_count, od.verts.data(), od.out_stride);
		}

		dev->SetFVF(prev_fvf);
		dev->SetTexture(0, prev_tex);
		if (prev_tex) {
			prev_tex->Release();
		}
		dev->SetRenderState(D3DRS_ZENABLE, prev_zenable);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, prev_zwrite);
		dev->SetRenderState(D3DRS_LIGHTING, prev_lighting);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, prev_ablend);
		dev->SetRenderState(D3DRS_SRCBLEND, prev_src);
		dev->SetRenderState(D3DRS_DESTBLEND, prev_dst);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, prev_atest);
		dev->SetTransform(D3DTS_WORLD, &prev_world);
		dev->SetTransform(D3DTS_VIEW, &prev_view);
		dev->SetTransform(D3DTS_PROJECTION, &prev_proj);

		for (auto& od : m_overlay_draws)
		{
			if (od.tex) {
				od.tex->Release();
			}
		}
		m_overlay_draws.clear();
	}

	void unproject::update_camera_if_needed()
	{
		if (!m_cam_dirty) {
			return;
		}
		m_cam_dirty = false;
		m_cam = game::read_camera();
	}

	void unproject::build_matrices(const float viewport_w, const float viewport_h)
	{
		const camera_frame f = build_camera_frame(m_cam, viewport_w, viewport_h);
		m_view = f.view;
		m_proj = f.proj;
		m_focal = f.focal;
		m_center_x = f.center_x;
		m_center_y = f.center_y;
		std::memcpy(m_basis_right, f.right, sizeof(m_basis_right));
		std::memcpy(m_basis_up, f.up, sizeof(m_basis_up));
		std::memcpy(m_basis_fwd, f.fwd, sizeof(m_basis_fwd));
	}

	// ----

	bool unproject::on_draw_primitive_up(IDirect3DDevice9* dev, const D3DPRIMITIVETYPE type, const UINT prim_count,
		const void* vertex_data, const UINT stride, HRESULT* out_hr)
	{
		const auto& cfg = shared::common::config::get();
		if (!cfg.unproject.enabled || !vertex_data) {
			return false;
		}

		DWORD fvf = 0;
		dev->GetFVF(&fvf);

		const auto layout = parse_rhw_fvf(fvf, stride);
		const auto vert_count = vertex_count_for(type, prim_count);
		if (!layout.valid || !vert_count) {
			dbg_log_draw(dev, "other:fvf", fvf, stride, prim_count, 99, 0, 0, nullptr);
			m_passthrough_other_frame++;
			return false;
		}

		const auto* src = static_cast<const uint8_t*>(vertex_data);
		const auto* dbg_v0 = reinterpret_cast<const float*>(src);

		DWORD zenable = D3DZB_TRUE;
		dev->GetRenderState(D3DRS_ZENABLE, &zenable);

		float rhw_min = FLT_MAX, rhw_max = -FLT_MAX;
		bool bad_rhw = false;
		for (uint32_t i = 0; i < vert_count; i++)
		{
			const float rhw = reinterpret_cast<const float*>(src + i * stride)[3];
			if (rhw <= 0.0f) {
				bad_rhw = true;
				break;
			}
			rhw_min = std::min(rhw_min, rhw);
			rhw_max = std::max(rhw_max, rhw);
		}
		if (bad_rhw) {
			dbg_log_draw(dev, "other:rhw<=0", fvf, stride, prim_count, zenable, 0, 0, dbg_v0);
			m_passthrough_other_frame++;
			return false;
		}

		// Engine overlay pass (see on_overlay_clear): HUD models draw after a
		// mid-scene z-only clear. They are screen-space UI for Remix —
		// reconstructing them into world space gets them path-traced
		// (scene-lit, occluded by geometry).
		if (m_overlay_phase)
		{
			dbg_log_draw(dev, "ui:overlay", fvf, stride, prim_count, zenable, rhw_min, rhw_max, dbg_v0);
			m_passthrough_ui_frame++;

			// Queue for the depth-sorted EndScene flush; suppress the original
			// (raw RHW draws are dropped by this Remix build anyway).
			overlay_draw od = {};
			od.out_stride = layout.stride - 4;
			od.out_fvf = (fvf & ~D3DFVF_POSITION_MASK) | D3DFVF_XYZ;
			od.type = type;
			od.prim_count = prim_count;
			od.verts.resize(static_cast<size_t>(vert_count) * od.out_stride);

			float z_sum = 0.0f;
			for (uint32_t i = 0; i < vert_count; i++)
			{
				const auto* in = src + i * stride;
				auto* out = od.verts.data() + static_cast<size_t>(i) * od.out_stride;
				std::memcpy(out, in, 12);                           // x, y, z(screen)
				std::memcpy(out + 12, in + 16, layout.stride - 16); // tail after rhw
				z_sum += reinterpret_cast<const float*>(in)[2];
			}
			od.sort_z = z_sum / static_cast<float>(vert_count);

			dev->GetTexture(0, &od.tex); // AddRef'd; released after flush
			m_overlay_draws.push_back(std::move(od));

			*out_hr = D3D_OK;
			return true;
		}

		// Flat-rhw draws outside the overlay pass are NOT reliable UI — distant
		// world fans also have near-constant rhw. Pass through (dropped by
		// Remix; worldrep already renders the world), never paint as ortho UI.
		if ((rhw_max - rhw_min) <= rhw_max * cfg.unproject.ui_rhw_epsilon) {
			dbg_log_draw(dev, "ui:flat-rhw", fvf, stride, prim_count, zenable, rhw_min, rhw_max, dbg_v0);
			m_passthrough_ui_frame++;
			return false;
		}

		update_camera_if_needed();
		if (!m_cam.valid) {
			dbg_log_draw(dev, "other:no-cam", fvf, stride, prim_count, zenable, rhw_min, rhw_max, dbg_v0);
			m_passthrough_other_frame++;
			return false;
		}

		dbg_log_draw(dev, "world:convert", fvf, stride, prim_count, zenable, rhw_min, rhw_max, dbg_v0);

		D3DVIEWPORT9 vp = {};
		dev->GetViewport(&vp);
		if (!vp.Width || !vp.Height) {
			m_passthrough_other_frame++;
			return false;
		}
		build_matrices(static_cast<float>(vp.Width), static_cast<float>(vp.Height));

		m_scratch.resize(static_cast<size_t>(vert_count) * layout.out_stride);

		const float quant = cfg.unproject.quantize_grid;

		// Unproject: view depth from rhw, lateral offsets from screen coords,
		// then out to world via the camera basis.
		for (uint32_t i = 0; i < vert_count; i++)
		{
			const auto* in = src + i * stride;
			const auto* in_f = reinterpret_cast<const float*>(in);
			auto* out = m_scratch.data() + static_cast<size_t>(i) * layout.out_stride;
			auto* out_f = reinterpret_cast<float*>(out);

			const float depth = 1.0f / in_f[3];
			const float view_r = (in_f[0] - m_center_x) * depth / m_focal;
			const float view_u = -(in_f[1] - m_center_y) * depth / m_focal;

			out_f[0] = m_cam.pos[0] + m_basis_fwd[0] * depth + m_basis_right[0] * view_r + m_basis_up[0] * view_u;
			out_f[1] = m_cam.pos[1] + m_basis_fwd[1] * depth + m_basis_right[1] * view_r + m_basis_up[1] * view_u;
			out_f[2] = m_cam.pos[2] + m_basis_fwd[2] * depth + m_basis_right[2] * view_r + m_basis_up[2] * view_u;

			// Static world geometry must hash identically every frame: snap
			// positions to a grid far below texel size to cancel the per-frame
			// floating-point wobble inherent to reconstruct-from-screen-space.
			if (quant > 0.0f)
			{
				out_f[0] = std::round(out_f[0] * quant) / quant;
				out_f[1] = std::round(out_f[1] * quant) / quant;
				out_f[2] = std::round(out_f[2] * quant) / quant;
			}
			// normal filled below

			auto* tail = out + 24;
			if (layout.off_diffuse >= 0)
			{
				auto color = *reinterpret_cast<const DWORD*>(in + layout.off_diffuse);
				if (cfg.unproject.force_white_diffuse) {
					color |= 0x00FFFFFFu;
				}
				*reinterpret_cast<DWORD*>(tail) = color;
				tail += 4;
			}
			if (layout.off_specular >= 0)
			{
				*reinterpret_cast<DWORD*>(tail) = *reinterpret_cast<const DWORD*>(in + layout.off_specular);
				tail += 4;
			}
			std::memcpy(tail, in + 16 + (layout.off_diffuse >= 0 ? 4 : 0) + (layout.off_specular >= 0 ? 4 : 0),
				static_cast<size_t>(layout.tex_count) * 8);
		}

		// One plane normal per primitive (portal polygons are planar), oriented
		// toward the camera. Falls back to the view direction when degenerate.
		float normal[3] = { -m_basis_fwd[0], -m_basis_fwd[1], -m_basis_fwd[2] };
		for (uint32_t i = 0; i + 2 < vert_count; i++)
		{
			const auto* v0 = reinterpret_cast<const float*>(m_scratch.data());
			const auto* v1 = reinterpret_cast<const float*>(m_scratch.data() + static_cast<size_t>(i + 1) * layout.out_stride);
			const auto* v2 = reinterpret_cast<const float*>(m_scratch.data() + static_cast<size_t>(i + 2) * layout.out_stride);
			const float e1[3] = { v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2] };
			const float e2[3] = { v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2] };
			float n[3];
			cross3(e1, e2, n);
			if (normalize3(n))
			{
				const float to_cam[3] = {
					m_cam.pos[0] - v0[0], m_cam.pos[1] - v0[1], m_cam.pos[2] - v0[2] };
				if (dot3(n, to_cam) < 0.0f) {
					n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
				}
				// Same hash-stability argument as positions: snap and renormalize.
				if (cfg.unproject.quantize_grid > 0.0f)
				{
					n[0] = std::round(n[0] * 256.0f) / 256.0f;
					n[1] = std::round(n[1] * 256.0f) / 256.0f;
					n[2] = std::round(n[2] * 256.0f) / 256.0f;
					normalize3(n);
				}
				normal[0] = n[0]; normal[1] = n[1]; normal[2] = n[2];
				break;
			}
		}
		for (uint32_t i = 0; i < vert_count; i++)
		{
			auto* out_f = reinterpret_cast<float*>(m_scratch.data() + static_cast<size_t>(i) * layout.out_stride);
			out_f[3] = normal[0];
			out_f[4] = normal[1];
			out_f[5] = normal[2];
		}

		// Submit with real transforms. Engine FVF cache (0x9D9164) must stay
		// coherent, so the incoming FVF is restored after the draw.
		dev->SetTransform(D3DTS_WORLD, &shared::globals::IDENTITY);
		dev->SetTransform(D3DTS_VIEW, &m_view);
		dev->SetTransform(D3DTS_PROJECTION, &m_proj);

		DWORD prev_lighting = FALSE;
		dev->GetRenderState(D3DRS_LIGHTING, &prev_lighting);
		if (prev_lighting) {
			dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		}

		dev->SetFVF(layout.out_fvf);
		*out_hr = dev->DrawPrimitiveUP(type, prim_count, m_scratch.data(), layout.out_stride);
		dev->SetFVF(fvf);

		if (prev_lighting) {
			dev->SetRenderState(D3DRS_LIGHTING, prev_lighting);
		}

		m_converted_draws_frame++;
		return true;
	}

	void unproject::dbg_log_draw(IDirect3DDevice9* dev, const char* verdict, const DWORD fvf, const UINT stride,
		const UINT prim_count, const DWORD zenable, const float rhw_min, const float rhw_max, const float* v0)
	{
		if (m_dbg_frames_left <= 0) {
			return;
		}
		if (!m_dbg_file)
		{
			CreateDirectoryA("rtx_comp", nullptr);
			m_dbg_file = fopen("rtx_comp\\unproject_debug.log", "w");
			if (!m_dbg_file) {
				m_dbg_frames_left = 0;
				return;
			}
		}

		IDirect3DBaseTexture9* tex = nullptr;
		dev->GetTexture(0, &tex);
		if (tex) {
			tex->Release();
		}

		const float depth_min = rhw_max > 0.0f ? 1.0f / rhw_max : 0.0f;
		const float depth_max = rhw_min > 0.0f && rhw_min != FLT_MAX ? 1.0f / rhw_min : 0.0f;
		fprintf(m_dbg_file, "%-13s fvf=0x%03X stride=%-2u prims=%-3u z=%lu depth=[%8.2f..%8.2f] v0=(%7.1f,%7.1f) tex=%p\n",
			verdict, fvf, stride, prim_count, zenable, depth_min, depth_max,
			v0 ? v0[0] : 0.0f, v0 ? v0[1] : 0.0f, static_cast<void*>(tex));
	}

	void unproject::dbg_frame_boundary()
	{
		if (m_dbg_frames_left <= 0 || !m_dbg_file) {
			return;
		}
		const uint32_t total = m_converted_draws_frame + m_passthrough_ui_frame + m_passthrough_other_frame;
		if (total == 0) {
			return;
		}
		fprintf(m_dbg_file, "==== PRESENT (world=%u ui=%u other=%u) ====\n",
			m_converted_draws_frame, m_passthrough_ui_frame, m_passthrough_other_frame);
		fflush(m_dbg_file);

		// Only consume an armed frame once real in-mission draw volume flows.
		if (total >= 30)
		{
			if (--m_dbg_frames_left == 0)
			{
				fclose(m_dbg_file);
				m_dbg_file = nullptr;
				shared::common::log("Unproject", "Debug log complete: rtx_comp\\unproject_debug.log",
					shared::common::LOG_TYPE::LOG_TYPE_GREEN, false);
			}
		}
	}

	void unproject::on_present()
	{
		dbg_frame_boundary();
		m_converted_draws_prev = m_converted_draws_frame;
		m_passthrough_ui_prev = m_passthrough_ui_frame;
		m_passthrough_other_prev = m_passthrough_other_frame;
		m_converted_draws_frame = 0;
		m_passthrough_ui_frame = 0;
		m_passthrough_other_frame = 0;
		m_cam_dirty = true;
	}

	unproject::unproject()
	{
		p_this = this;
		m_scratch.reserve(64 * 48);
		m_dbg_frames_left = shared::common::config::get().unproject.debug_log_frames;
		m_initialized = true;
		shared::common::log("Unproject", "Module initialized (RHW world-space reconstruction).", shared::common::LOG_TYPE::LOG_TYPE_GREEN, false);
	}

	unproject::~unproject()
	{
		p_this = nullptr;
	}
}
