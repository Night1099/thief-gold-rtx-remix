#include "std_include.hpp"
#include "config.hpp"

namespace shared::common
{
	config& config::get()
	{
		static config instance;
		return instance;
	}

	void config::load(const std::string& path)
	{
		ini_path_ = path;

		// Check if the INI file actually exists — GetPrivateProfileInt silently
		// returns defaults for missing files, making it look like settings are ignored.
		if (GetFileAttributesA(ini_path_.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			log("Config", std::format("INI NOT FOUND: {} — using all defaults!", ini_path_),
				LOG_TYPE::LOG_TYPE_ERROR, true);
			loaded_ = false;
			return;
		}

		loaded_ = true;
		parse_all();
	}

	int config::get_int(const char* section, const char* key, int default_val) const
	{
		if (!loaded_) return default_val;
		return GetPrivateProfileIntA(section, key, default_val, ini_path_.c_str());
	}

	std::string config::get_string(const char* section, const char* key, const char* default_val) const
	{
		if (!loaded_) return default_val;
		char buf[512];
		GetPrivateProfileStringA(section, key, default_val, buf, sizeof(buf), ini_path_.c_str());
		return buf;
	}

	float config::get_float(const char* section, const char* key, float default_val) const
	{
		auto str = get_string(section, key, "");
		if (str.empty()) return default_val;
		try { return std::stof(str); }
		catch (...) { return default_val; }
	}

	bool config::get_bool(const char* section, const char* key, bool default_val) const
	{
		return get_int(section, key, default_val ? 1 : 0) != 0;
	}

	void config::parse_all()
	{
		// [Remix]
		remix.enabled = get_bool("Remix", "Enabled", true);
		remix.dll_name = get_string("Remix", "DLLName", "d3d9_remix.dll");

		// [Chain]
		chain.preload = get_string("Chain", "PreLoad", "");
		chain.postload = get_string("Chain", "PostLoad", "");

		// [FFP]
		ffp.enabled = get_bool("FFP", "Enabled", true);
		ffp.albedo_stage = get_int("FFP", "AlbedoStage", 0);
		if (ffp.albedo_stage < 0 || ffp.albedo_stage > 7)
			ffp.albedo_stage = 0;

		// [Skinning]
		skinning.enabled = get_bool("Skinning", "Enabled", false);

		// [Unproject] — ThiefGold RHW world-space reconstruction
		unproject.enabled = get_bool("Unproject", "Enabled", true);
		unproject.force_white_diffuse = get_bool("Unproject", "ForceWhiteDiffuse", false);
		unproject.fov_ref_deg = get_float("Unproject", "FovRefDeg", 90.0f);
		unproject.yaw_sign = get_int("Unproject", "YawSign", 1);
		unproject.yaw_offset_deg = get_float("Unproject", "YawOffsetDeg", 0.0f);
		unproject.pitch_sign = get_int("Unproject", "PitchSign", 1);
		unproject.ui_rhw_epsilon = get_float("Unproject", "UiRhwEpsilon", 0.001f);
		unproject.debug_log_frames = get_int("Unproject", "DebugLogFrames", 0);
		unproject.z_near = get_float("Unproject", "ZNear", 0.25f);
		unproject.z_far = get_float("Unproject", "ZFar", 2048.0f);
		unproject.quantize_grid = get_float("Unproject", "QuantizeGrid", 64.0f);

		// [Worldrep] — phase 3 stable-hash whole-polygon submission
		worldrep.enabled = get_bool("Worldrep", "Enabled", true);
		worldrep.winding_flip = get_bool("Worldrep", "WindingFlip", false);
		worldrep.lightmap_attenuation = std::clamp(get_float("Worldrep", "LightmapAttenuation", 0.0f), 0.0f, 1.0f);
		worldrep.skip_tex_ids.clear();
		{
			const auto list = get_string("Worldrep", "SkipTexIds", "");
			size_t pos = 0;
			while (pos < list.size())
			{
				size_t end = list.find(',', pos);
				if (end == std::string::npos) end = list.size();
				try {
					const int id = std::stoi(list.substr(pos, end - pos));
					if (id >= 0 && id <= 0xFFFF) {
						worldrep.skip_tex_ids.push_back(static_cast<uint16_t>(id));
					}
				}
				catch (...) {}
				pos = end + 1;
			}
		}

		// [Lights] — engine light table -> Remix sphere lights
		lights.enabled = get_bool("Lights", "Enabled", true);
		lights.radiance_scale = get_float("Lights", "RadianceScale", 20.0f);
		lights.emitter_radius = get_float("Lights", "EmitterRadius", 0.4f);
		lights.skip_infinite = get_bool("Lights", "SkipInfinite", false);
		lights.force_spot = get_bool("Lights", "ForceSpot", false);
		lights.cone_angle_deg = get_float("Lights", "ConeAngleDeg", 70.0f);
		lights.cone_softness = get_float("Lights", "ConeSoftness", 0.5f);

		// [Diagnostics]
		diagnostics.enabled = get_bool("Diagnostics", "Enabled", true);
		diagnostics.auto_capture = get_bool("Diagnostics", "AutoCapture", true);
		diagnostics.delay_ms = get_int("Diagnostics", "DelayMs", 50000);
		diagnostics.log_frames = get_int("Diagnostics", "LogFrames", 3);
		diagnostics.log_draw_calls = get_bool("Diagnostics", "LogDrawCalls", true);
		diagnostics.log_vs_constants = get_bool("Diagnostics", "LogVSConstants", true);
		diagnostics.log_vertex_data = get_bool("Diagnostics", "LogVertexData", true);
		diagnostics.log_declarations = get_bool("Diagnostics", "LogDeclarations", true);
		diagnostics.log_textures = get_bool("Diagnostics", "LogTextures", true);
		diagnostics.log_present_info = get_bool("Diagnostics", "LogPresentInfo", true);

		// [Tracer]
		tracer.backtrace_depth = get_int("Tracer", "BacktraceDepth", 8);
		tracer.output_dir = get_string("Tracer", "OutputDir", "captures");

		log("Config", std::format("Loaded from: {}", ini_path_));
		log("Config", std::format("FFP={} AlbedoStage={}", ffp.enabled ? 1 : 0, ffp.albedo_stage));
		if (skinning.enabled)
			log("Config", "Skinning ENABLED", LOG_TYPE::LOG_TYPE_WARN);
	}
}
