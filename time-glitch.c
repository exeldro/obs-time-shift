#include <obs-module.h>
#include "time-shift.h"

struct glitch_info {
	obs_source_t *source;
	gs_texrender_t *render;
	uint32_t cx;
	uint32_t cy;
	bool target_valid;
	bool processed_frame;
	obs_hotkey_pair_id hotkey;
	float duration;
	uint32_t duration_max;
};

static const char *glitch_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("TimeGlitch");
}

static void free_textures(struct glitch_info *f)
{
	if (!f->render)
		return;
	obs_enter_graphics();
	gs_texrender_destroy(f->render);
	f->render = NULL;
	obs_leave_graphics();
}

static inline bool check_size(struct glitch_info *f)
{
	obs_source_t *target = obs_filter_get_target(f->source);

	f->target_valid = !!target;
	if (!f->target_valid)
		return true;

	const uint32_t cx = obs_source_get_base_width(target);
	const uint32_t cy = obs_source_get_base_height(target);

	f->target_valid = !!cx && !!cy;
	if (!f->target_valid)
		return true;

	if (cx != f->cx || cy != f->cy) {
		f->cx = cx;
		f->cy = cy;
		free_textures(f);
		return true;
	}
	return false;
}

static void glitch_update(void *data, obs_data_t *settings)
{
	struct glitch_info *glitch = data;
	glitch->duration_max = obs_data_get_int(settings, "duration");
	if (glitch->duration_max <= 0)
		glitch->duration_max = 500;
}

static void *glitch_create(obs_data_t *settings, obs_source_t *source)
{
	struct glitch_info *glitch = bzalloc(sizeof(struct glitch_info));
	glitch->source = source;
	glitch->hotkey = OBS_INVALID_HOTKEY_ID;
	glitch_update(glitch, settings);
	return glitch;
}

static void glitch_destroy(void *data)
{
	struct glitch_info *glitch = data;
	if (glitch->hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(glitch->hotkey);
	}
	free_textures(glitch);
	bfree(glitch);
}

static void draw_frame(struct glitch_info *f)
{

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_texture_t *tex = gs_texrender_get_texture(f->render);
	if (tex) {
		gs_eparam_t *image =
			gs_effect_get_param_by_name(effect, "image");
		gs_effect_set_texture(image, tex);

		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(tex, 0, f->cx, f->cy);
	}
}

static void glitch_video_render(void *data, gs_effect_t *effect)
{
	struct glitch_info *glitch = data;
	obs_source_t *target = obs_filter_get_target(glitch->source);
	obs_source_t *parent = obs_filter_get_parent(glitch->source);

	if (!glitch->target_valid || !target || !parent) {
		obs_source_skip_video_filter(glitch->source);
		return;
	}
	if (glitch->processed_frame) {
		draw_frame(glitch);
		return;
	}
	if (!glitch->render) {
		glitch->render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	} else {
		gs_texrender_reset(glitch->render);
	}

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(glitch->render, glitch->cx, glitch->cy)) {
		uint32_t parent_flags = obs_source_get_output_flags(target);
		bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		struct vec4 clear_color;

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)glitch->cx, 0.0f, (float)glitch->cy,
			 -100.0f, 100.0f);

		if (target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);

		gs_texrender_end(glitch->render);
	}

	gs_blend_state_pop();
	draw_frame(glitch);
	glitch->processed_frame = true;
}



static obs_properties_t *glitch_properties(void *data)
{
	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p = obs_properties_add_int(
		ppts, "duration", obs_module_text("Duration"), 0, 100000, 1000);
	obs_property_int_set_suffix(p, "ms");
	return ppts;
}

void glitch_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "duration", 500);
}

bool glitch_enable_hotkey(void *data, obs_hotkey_pair_id id,
			  obs_hotkey_t *hotkey, bool pressed)
{
	struct glitch_info *glitch = data;
	if (!pressed)
		return false;

	if (obs_source_enabled(glitch->source))
		return false;

	obs_source_set_enabled(glitch->source, true);

	return true;
}

static void glitch_tick(void *data, float t)
{

	struct glitch_info *f = data;

	if (obs_source_enabled(f->source)) {
		f->duration += t;
		if (f->duration * 1000.0 > f->duration_max) {
			obs_source_set_enabled(f->source, false);
		}
	} else {
		f->processed_frame = false;
		f->duration = 0.0f;
	}
	if (f->hotkey == OBS_INVALID_HOTKEY_ID) {
		obs_source_t *parent = obs_filter_get_parent(f->source);
		if (parent) {
			f->hotkey = obs_hotkey_register_source(
				parent, "TimeGlitch.Enable",
				obs_module_text("TimeGlitchEnable"),
				glitch_enable_hotkey, f);
		}
	}
	check_size(f);
}

struct obs_source_info time_glitch_filter = {
	.id = "time_glitch_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_OUTPUT_VIDEO,
	.get_name = glitch_get_name,
	.create = glitch_create,
	.destroy = glitch_destroy,
	.load = glitch_update,
	.update = glitch_update,
	.video_render = glitch_video_render,
	.get_properties = glitch_properties,
	.get_defaults = glitch_defaults,
	.video_tick = glitch_tick,
};
