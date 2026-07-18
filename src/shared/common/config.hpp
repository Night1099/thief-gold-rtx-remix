#pragma once

namespace shared::common
{
	class config
	{
	public:
		static config& get();

		void load(const std::string& ini_path);
		bool is_loaded() const { return loaded_; }

		int get_int(const char* section, const char* key, int default_val) const;
		std::string get_string(const char* section, const char* key, const char* default_val) const;
		float get_float(const char* section, const char* key, float default_val) const;
		bool get_bool(const char* section, const char* key, bool default_val) const;

		struct ffp_settings
		{
			bool enabled = true;
			int albedo_stage = 0;
		} ffp;

		struct skinning_settings
		{
			bool enabled = false;
		} skinning;

		struct diagnostics_settings
		{
			bool enabled = true;
			bool auto_capture = true;
			int delay_ms = 50000;
			int log_frames = 3;

			// Log categories (defaults, overridable from ImGui at runtime)
			bool log_draw_calls = true;
			bool log_vs_constants = true;
			bool log_vertex_data = true;
			bool log_declarations = true;
			bool log_textures = true;
			bool log_present_info = true;
		} diagnostics;

		struct unproject_settings
		{
			bool enabled = true;
			bool force_white_diffuse = false;
			float fov_ref_deg = 90.0f;   // engine FOV at 4:3 reference
			int yaw_sign = 1;
			float yaw_offset_deg = 0.0f;
			int pitch_sign = 1;
			float ui_rhw_epsilon = 0.001f; // relative rhw spread below which a draw is UI
			int debug_log_frames = 0;      // log per-DPUP classification for N in-mission frames
			float z_near = 0.25f;
			float z_far = 2048.0f;
			float quantize_grid = 64.0f;  // world-position snap (1/N units); 0 disables
		} unproject;

		struct worldrep_settings
		{
			bool enabled = true;       // submit whole worldrep polys (stable hashes) vs unproject clipped fans
			bool winding_flip = false; // flip triangle winding if faces are backwards
			std::vector<uint16_t> skip_tex_ids; // texture ids dropped from submission (249 = sky hack)
			float lightmap_attenuation = 0.0f; // 0=off .. 1=full: darken vertex color by baked lightmap luminance
		} worldrep;

		struct lights_settings
		{
			bool enabled = true;        // mirror engine lights into Remix as sphere lights
			float radiance_scale = 20.0f; // engine color units (~0..5) -> Remix radiance multiplier
			float emitter_radius = 0.4f; // sphere-light emitter size, engine units
			bool skip_infinite = false;  // skip radius==0 records (radius 0 is common on normal lights — leave off)
			bool force_spot = false;     // shape every light as a spot (engine dir or straight down)
			float cone_angle_deg = 70.0f; // cone half-angle for lights without valid cone data
			float cone_softness = 0.5f;   // penumbra fraction for forced spots
		} lights;

		struct remix_settings
		{
			bool enabled = true;
			std::string dll_name = "d3d9_remix.dll";
		} remix;

		struct chain_settings
		{
			std::string preload;   // semicolon-separated DLLs/ASIs loaded before d3d9 chain
			std::string postload;  // semicolon-separated DLLs/ASIs loaded after init
		} chain;

		struct tracer_settings
		{
			int backtrace_depth = 8;
			std::string output_dir = "captures";
		} tracer;

	private:
		std::string ini_path_;
		bool loaded_ = false;

		void parse_all();
	};
}
