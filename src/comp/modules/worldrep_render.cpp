#include "std_include.hpp"
#include "worldrep_render.hpp"

#include "shared/common/config.hpp"
#include "camera_math.hpp"

#include <cmath>

namespace comp
{
	namespace
	{
		constexpr DWORD OUT_FVF = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;
		constexpr UINT OUT_STRIDE = sizeof(float) * 8 + sizeof(DWORD); // pos3 + normal3 + color + uv2

		game::tex_resolve_t g_resolve_orig = nullptr;

		void* __cdecl hk_resolve(uint32_t tex_id)
		{
			void* obj = g_resolve_orig(tex_id);
			if (auto* w = worldrep_render::get()) {
				w->note_resolved(static_cast<uint16_t>(tex_id), obj);
			}
			return obj;
		}

		inline float dot3(const float* a, const float* b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }

		inline void sub3(const float* a, const float* b, float* o)
		{
			o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2];
		}

		// Static-base-layer luminance (0..1) at texture-UV (u,v). Lightmap luxels
		// are 1/4 texture-UV units, offset by the poly's luxel origin.
		float lightmap_luminance(const game::WRLightmapInfo& lmi, const float u, const float v)
		{
			const int w = lmi.lm_width;
			const int h = lmi.lm_height;
			if (!lmi.p_lightmap || w <= 0 || h <= 0) {
				return 1.0f;
			}
			const int lu = std::clamp(static_cast<int>(u * 4.0f - lmi.lm_u_base + 0.5f), 0, w - 1);
			const int lv = std::clamp(static_cast<int>(v * 4.0f - lmi.lm_v_base + 0.5f), 0, h - 1);

			if (*game::rva<int>(game::VA_LM_32BIT) != 0)
			{
				const uint32_t px = static_cast<const uint32_t*>(lmi.p_lightmap)[lv * w + lu];
				const uint32_t r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
				return static_cast<float>(std::max({ r, g, b })) / 255.0f;
			}
			const uint16_t px = static_cast<const uint16_t*>(lmi.p_lightmap)[lv * w + lu];
			const float r = ((px >> 11) & 0x1F) / 31.0f, g = ((px >> 5) & 0x3F) / 63.0f, b = (px & 0x1F) / 31.0f;
			return std::max({ r, g, b });
		}
	}

	// ----

	bool worldrep_render::is_world_dpup(const uintptr_t return_addr) const
	{
		const auto lo = reinterpret_cast<uintptr_t>(game::rva<void>(game::VA_PORTAL_FLUSH_LO));
		const auto hi = reinterpret_cast<uintptr_t>(game::rva<void>(game::VA_PORTAL_FLUSH_HI));
		return return_addr >= lo && return_addr < hi;
	}

	void worldrep_render::note_resolved(const uint16_t tex_id, void* engine_obj)
	{
		if (engine_obj) {
			m_obj_by_id[tex_id] = engine_obj;
		}
	}

	const worldrep_render::cached_tex* worldrep_render::get_texture(IDirect3DDevice9* dev, const uint16_t tex_id)
	{
		if (const auto it = m_tex_by_id.find(tex_id); it != m_tex_by_id.end()) {
			return it->second.tex ? &it->second : nullptr;
		}

		// Engine bitmap object: from the resolver snoop, or resolve on demand so
		// the full level can be textured from the first frame (stable geometry set).
		void* obj = nullptr;
		if (const auto it = m_obj_by_id.find(tex_id); it != m_obj_by_id.end()) {
			obj = it->second;
		} else if (g_resolve_orig) {
			obj = g_resolve_orig(tex_id);
			if (obj) m_obj_by_id[tex_id] = obj;
		}

		cached_tex& slot = m_tex_by_id[tex_id]; // insert a null entry (negative cache)
		if (!obj) {
			return nullptr;
		}

		const auto* base = static_cast<const uint8_t*>(obj);
		const uint16_t w = *reinterpret_cast<const uint16_t*>(base + game::ENGTEX_DIM_OFF);
		const uint16_t h = *reinterpret_cast<const uint16_t*>(base + game::ENGTEX_DIM_OFF + 2);
		if (w == 0 || h == 0 || w > 2048 || h > 2048) {
			return nullptr;
		}

		IDirect3DTexture9* tex = nullptr;
		if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || !tex) {
			return nullptr;
		}
		D3DLOCKED_RECT lr = {};
		if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0)))
		{
			const auto* src = base + game::ENGTEX_PIXELS_OFF;
			for (uint32_t y = 0; y < h; y++) {
				std::memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, src + y * w * 4u, w * 4u);
			}
			tex->UnlockRect(0);
		}

		slot.tex = tex;
		slot.scale_u = 64.0f / static_cast<float>(w);
		slot.scale_v = 64.0f / static_cast<float>(h);
		m_mapped_textures = static_cast<uint32_t>(m_tex_by_id.size());
		return &slot;
	}

	// ----

	bool worldrep_render::build_geometry()
	{
		auto* cells = game::wr_cells();
		const int count = game::wr_cell_count();
		if (!cells || count <= 0 || count > 0x8000) {
			return false;
		}

		// Cells populate incrementally during mission load while the array base
		// and count are already final — fingerprint per-cell counts so a build
		// taken mid-load (missing ground/world chunks) is redone once the
		// worldrep finishes filling in.
		uint32_t fp = 2166136261u;
		for (int ci = 0; ci < count; ci++)
		{
			const game::WRCell* cell = cells[ci];
			const uint32_t v = cell
				? (cell->num_polys | (cell->num_render_polys << 8u) | (cell->num_vertices << 16u))
				: 0xFFFFFFFFu;
			fp = (fp ^ v) * 16777619u;
		}

		if (m_geometry_built && m_built_cells_base == cells && m_built_cell_count == count && m_built_fp == fp) {
			return true;
		}

		m_buckets_by_tex.clear();
		m_skipped_tris_by_tex.clear();
		m_triangles = 0;

		const auto& cfg = shared::common::config::get();
		const bool flip = cfg.worldrep.winding_flip;
		const auto& skip_ids = cfg.worldrep.skip_tex_ids;
		const float lm_atten = cfg.worldrep.lightmap_attenuation;

		for (int ci = 0; ci < count; ci++)
		{
			const game::WRCell* cell = cells[ci];
			if (!cell || !cell->p_vertices || !cell->p_poly_list ||
				!cell->p_render_polys || !cell->p_vertex_index_list || !cell->p_planes) {
				continue;
			}

			uint32_t idx_off = 0;
			for (int pi = 0; pi < cell->num_polys; pi++)
			{
				const game::WRPoly& poly = cell->p_poly_list[pi];
				const uint8_t* idx = cell->p_vertex_index_list + idx_off;
				idx_off += poly.num_verts;

				if (pi >= cell->num_render_polys || poly.num_verts < 3) {
					continue;
				}

				const game::WRRenderPoly& rp = cell->p_render_polys[pi];

				if (std::find(skip_ids.begin(), skip_ids.end(), rp.texture_id) != skip_ids.end()) {
					// e.g. the sky-hack surface — Remix sky comes from rtx.conf instead
					m_skipped_tris_by_tex[rp.texture_id] += poly.num_verts - 2;
					continue;
				}

				// Face normal from the cell plane, oriented toward the cell
				// interior (bounding-sphere center) so it faces the viewer.
				float n[3] = { 0, 0, 1 };
				if (poly.plane_id < cell->num_planes)
				{
					const game::WRPlane& pl = cell->p_planes[poly.plane_id];
					n[0] = pl.a; n[1] = pl.b; n[2] = pl.c;
					const float len = std::sqrt(dot3(n, n));
					if (len > 1e-6f) { n[0]/=len; n[1]/=len; n[2]/=len; }
					const float* v0 = cell->p_vertices[idx[0]];
					float to_center[3]; sub3(cell->center, v0, to_center);
					if (dot3(n, to_center) < 0.0f) { n[0]=-n[0]; n[1]=-n[1]; n[2]=-n[2]; }
				}

				// UV basis (per research / RenderPoly_ProjectUV).
				const float* au = rp.tex_u_axis;
				const float* av = rp.tex_v_axis;
				const float mag2u = dot3(au, au);
				const float mag2v = dot3(av, av);
				const float dotp = dot3(au, av);
				const float u_base = rp.u_base_fx / 4096.0f;
				const float v_base = rp.v_base_fx / 4096.0f;
				const uint8_t origin_local = rp.origin_vertex < poly.num_verts ? rp.origin_vertex : 0;
				const float* origin = cell->p_vertices[idx[origin_local]];
				const bool orthogonal = std::fabs(dotp) < 1e-6f;
				const float corr = orthogonal ? 0.0f : 1.0f / (mag2u * mag2v - dotp * dotp);

				auto compute_uv = [&](const float* pos, float& out_u, float& out_v)
				{
					float vrel[3]; sub3(pos, origin, vrel);
					const float pu = dot3(au, vrel);
					const float pv = dot3(av, vrel);
					float proj_u, proj_v;
					if (orthogonal) {
						proj_u = mag2u > 1e-12f ? pu / mag2u : 0.0f;
						proj_v = mag2v > 1e-12f ? pv / mag2v : 0.0f;
					} else {
						proj_u = pu * corr * mag2v - pv * corr * dotp;
						proj_v = pv * corr * mag2u - pu * corr * dotp;
					}
					out_u = proj_u + u_base;
					out_v = proj_v + v_base;
				};

				auto& bkt = m_buckets_by_tex[rp.texture_id];

				const game::WRLightmapInfo* lmi =
					(lm_atten > 0.0f && cell->p_lightmap_info) ? &cell->p_lightmap_info[pi] : nullptr;

				auto emit = [&](const uint8_t li)
				{
					out_vertex ov;
					const float* p = cell->p_vertices[idx[li]];
					ov.pos[0]=p[0]; ov.pos[1]=p[1]; ov.pos[2]=p[2];
					ov.normal[0]=n[0]; ov.normal[1]=n[1]; ov.normal[2]=n[2];
					compute_uv(p, ov.u, ov.v);
					ov.color = 0xFFFFFFFFu;
					if (lmi)
					{
						const float lum = lightmap_luminance(*lmi, ov.u, ov.v);
						const float f = 1.0f - lm_atten * (1.0f - lum);
						const auto c = static_cast<DWORD>(std::clamp(f, 0.0f, 1.0f) * 255.0f);
						ov.color = 0xFF000000u | (c << 16) | (c << 8) | c;
					}
					bkt.verts.push_back(ov);
				};

				// Convex fan from local vertex 0.
				for (int t = 1; t < poly.num_verts - 1; t++)
				{
					emit(0);
					if (flip) { emit(static_cast<uint8_t>(t)); emit(static_cast<uint8_t>(t + 1)); }
					else      { emit(static_cast<uint8_t>(t + 1)); emit(static_cast<uint8_t>(t)); }
					m_triangles++;
				}
			}
		}

		m_buckets = static_cast<uint32_t>(m_buckets_by_tex.size());
		m_built_cells_base = cells;
		m_built_cell_count = count;
		m_built_fp = fp;
		m_geometry_built = true;

		shared::common::log("Worldrep",
			std::format("Built geometry: {} cells, {} textures, {} triangles",
				count, m_buckets, m_triangles),
			shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);

		// Sorted bucket dump so SkipTexIds candidates can be read off the log.
		std::vector<std::pair<uint16_t, size_t>> by_size;
		by_size.reserve(m_buckets_by_tex.size());
		for (const auto& [id, bkt] : m_buckets_by_tex) {
			by_size.emplace_back(id, bkt.verts.size() / 3);
		}
		std::sort(by_size.begin(), by_size.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		std::string line;
		for (const auto& [id, tris] : by_size) {
			line += std::format("{}:{} ", id, tris);
		}
		shared::common::log("Worldrep", std::format("Buckets (texId:tris): {}", line), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, true);

		if (!skip_ids.empty())
		{
			std::string skipped;
			for (const uint16_t id : skip_ids) {
				const auto it = m_skipped_tris_by_tex.find(id);
				skipped += std::format("{}:{} ", id, it != m_skipped_tris_by_tex.end() ? it->second : 0);
			}
			shared::common::log("Worldrep", std::format("SkipTexIds dropped (texId:tris): {}", skipped), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}
		return true;
	}

	// ----

	void worldrep_render::submit(IDirect3DDevice9* dev)
	{
		if (!m_enabled || !dev) {
			return;
		}
		if (!build_geometry()) {
			return;
		}

		const auto cam = game::read_camera();
		if (!cam.valid) {
			return;
		}

		D3DVIEWPORT9 vp = {};
		dev->GetViewport(&vp);
		if (!vp.Width || !vp.Height) {
			return;
		}
		const camera_frame frame = build_camera_frame(cam, static_cast<float>(vp.Width), static_cast<float>(vp.Height));

		// State for FFP world submission. Remix reads bound texture as albedo
		// and our supplied normals; lighting is done by the path tracer.
		// Cull NONE: worldrep polys have mixed winding — let the path tracer
		// treat them two-sided rather than backface-culling (e.g. floors).
		DWORD prev_lighting = 0, prev_fvf = 0, prev_ttf0 = 0, prev_cull = 0;
		dev->GetRenderState(D3DRS_LIGHTING, &prev_lighting);
		dev->GetRenderState(D3DRS_CULLMODE, &prev_cull);
		dev->GetFVF(&prev_fvf);
		dev->GetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, &prev_ttf0);
		IDirect3DBaseTexture9* prev_tex0 = nullptr;
		dev->GetTexture(0, &prev_tex0);

		dev->SetTransform(D3DTS_WORLD, &shared::globals::IDENTITY);
		dev->SetTransform(D3DTS_VIEW, &frame.view);
		dev->SetTransform(D3DTS_PROJECTION, &frame.proj);
		dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		dev->SetFVF(OUT_FVF);
		dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);

		for (auto& [tex_id, bkt] : m_buckets_by_tex)
		{
			if (bkt.verts.empty()) {
				continue;
			}
			const cached_tex* ct = get_texture(dev, tex_id);
			if (!ct) {
				continue; // bitmap not resolvable yet
			}

			// Dark UVs are pre-scale (texel/64). Apply per-texture scale via the
			// FFP texture matrix so cached vertex UVs stay texture-agnostic.
			D3DXMATRIX texm;
			D3DXMatrixIdentity(&texm);
			texm._11 = ct->scale_u; texm._22 = ct->scale_v;
			dev->SetTransform(D3DTS_TEXTURE0, &texm);

			dev->SetTexture(0, ct->tex);
			const UINT tri = static_cast<UINT>(bkt.verts.size() / 3);
			dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, tri, bkt.verts.data(), OUT_STRIDE);
		}

		// Restore.
		dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, prev_ttf0);
		dev->SetTexture(0, prev_tex0);
		if (prev_tex0) prev_tex0->Release();
		dev->SetFVF(prev_fvf);
		dev->SetRenderState(D3DRS_CULLMODE, prev_cull);
		dev->SetRenderState(D3DRS_LIGHTING, prev_lighting);
	}

	void worldrep_render::on_reset()
	{
		// Release our created textures; they are recreated lazily after reset.
		// Geometry (from game memory) and the engine-object map are unaffected.
		for (auto& [id, ct] : m_tex_by_id) {
			if (ct.tex) ct.tex->Release();
		}
		m_tex_by_id.clear();
		m_mapped_textures = 0;
	}

	void worldrep_render::install_resolver_hook()
	{
		auto* slot = game::rva<game::tex_resolve_t>(game::VA_TEX_RESOLVE_PTR);
		void* target = reinterpret_cast<void*>(*slot);
		if (!target) {
			shared::common::log("Worldrep", "Resolver ptr null — texture mapping disabled.", shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}
		if (MH_CreateHook(target, &hk_resolve, reinterpret_cast<void**>(&g_resolve_orig)) != MH_OK ||
			MH_EnableHook(target) != MH_OK) {
			shared::common::log("Worldrep", "Failed to hook texture resolver.", shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}
		shared::common::log("Worldrep", std::format("Hooked texture resolver at {}.", target), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}

	worldrep_render::worldrep_render()
	{
		p_this = this;
		m_enabled = shared::common::config::get().worldrep.enabled;
		if (m_enabled) {
			install_resolver_hook();
		}
		m_initialized = true;
		shared::common::log("Worldrep",
			std::format("Module initialized (enabled={}).", m_enabled),
			shared::common::LOG_TYPE::LOG_TYPE_GREEN, false);
	}

	worldrep_render::~worldrep_render()
	{
		p_this = nullptr;
	}
}
