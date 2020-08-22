#include <obs-module.h>
#include <util/circlebuf.h>
#include <util/dstr.h>
#include "time-shift.h"
#include "easing.h"

struct frame {
	gs_texrender_t *render;
	uint64_t ts;
};

struct time_shift_info {
	obs_source_t *source;
	struct circlebuf frames;
	int time_mode;
	obs_hotkey_id delay_hotkey;
	obs_hotkey_id loop_hotkey;
	obs_hotkey_id live_hotkey;

	bool hotkeys_loaded;
	double max_duration;
	double speed;
	double start_speed;
	double target_speed;

	double fast_forward_speed;
	double slow_backward_speed;

	uint32_t cx;
	uint32_t cy;
	bool processed_frame;
	double time_diff;
	bool target_valid;

	uint32_t easing;
	float easing_duration;
	float easing_max_duration;
	uint64_t easing_started;

	char *text_source_name;
        int live_after_ticks;

};

static void time_shift_text(struct time_shift_info *c)
{
	if (!c->text_source_name || !strlen(c->text_source_name))
		return;
	obs_source_t *s = obs_get_source_by_name(c->text_source_name);
	if (!s)
		return;

	struct dstr sf;
	if (c->time_mode == TIME_SHIFT_MODE_LOOP) {
		dstr_init_copy(&sf, "Looping time");
	} else if (c->time_mode == TIME_SHIFT_MODE_DELAY) {
		if (c->speed > 1.0) {
			dstr_init(&sf);
			dstr_printf(&sf, "Fast-forwarding to real-time (%.1fs remaining)", c->time_diff);
		} else if (c->speed < 1.0) {
			dstr_init(&sf);
			dstr_printf(
				&sf,
				"Rewinding (%.1fs remaining)",
				(c->max_duration - c->time_diff)/(1.0 - c->speed));
		} else {
			dstr_init_copy(&sf, "Playing (delayed)");
		}
	}else {
		dstr_init_copy(&sf, "Playing (real-time)");
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", sf.array);
	obs_source_update(s, settings);
	obs_data_release(settings);
	dstr_free(&sf);
	obs_source_release(s);
}

static const char *time_shift_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("TimeShift");
}

static void free_textures(struct time_shift_info *f)
{
	obs_enter_graphics();
	while (f->frames.size) {
		struct frame frame;
		circlebuf_pop_front(&f->frames, &frame, sizeof(frame));
		gs_texrender_destroy(frame.render);
	}
	circlebuf_free(&f->frames);
	obs_leave_graphics();
}

static void time_shift_update(void *data, obs_data_t *settings)
{
	struct time_shift_info *d = data;
	double duration = obs_data_get_double(settings, S_DURATION);
	if (duration < d->max_duration) {
		free_textures(d);
	}
	d->max_duration = duration;
	d->easing = obs_data_get_int(settings, S_EASING);
	d->easing_max_duration =
		(float)obs_data_get_double(settings, S_EASING_DURATION);
	d->fast_forward_speed =
		obs_data_get_double(settings, S_FAST_FORWARD) / 100.0;
	d->slow_backward_speed =
		obs_data_get_double(settings, S_SLOW_BACKWARD) / 100.0;

	const char *text_source = obs_data_get_string(settings, S_TEXT_SOURCE);
	if (d->text_source_name) {
		if (strcmp(d->text_source_name, text_source) != 0) {
			bfree(d->text_source_name);
			d->text_source_name = bstrdup(text_source);
		}
	} else {
		d->text_source_name = bstrdup(text_source);
	}
}

static void *time_shift_create(obs_data_t *settings, obs_source_t *source)
{
	struct time_shift_info *d =
		bzalloc(sizeof(struct time_shift_info));
	d->source = source;
	d->speed = 1.0;
	d->target_speed = 1.0;
	d->start_speed = 1.0;
	time_shift_update(d, settings);
	return d;
}

static void time_shift_destroy(void *data)
{
	struct time_shift_info *c = data;
	obs_hotkey_unregister(c->delay_hotkey);
	obs_hotkey_unregister(c->loop_hotkey);
	obs_hotkey_unregister(c->live_hotkey);
	free_textures(c);
	if (c->text_source_name)
		bfree(c->text_source_name);
	bfree(c);
}

void time_shift_delay_hotkey(void *data, obs_hotkey_id id,
				   obs_hotkey_t *hotkey, bool pressed)
{
	if (!pressed)
		return;
	struct time_shift_info *d = data;
	if (d->time_mode == TIME_SHIFT_MODE_DELAY) {
		if (d->speed < 1.0) {
			d->speed = 1.0;
			d->start_speed = 1.0;
			d->target_speed = 1.0;
			d->easing_started = 0;
		} else {
			d->start_speed = d->speed;
			d->target_speed = d->fast_forward_speed;
			d->easing_started = 0;
		}
	} else if (d->time_mode == TIME_SHIFT_MODE_LIVE) {
		d->start_speed = d->slow_backward_speed;
		d->target_speed = d->slow_backward_speed;
		d->speed = d->slow_backward_speed;
		d->easing_started = 0;
		d->time_mode = TIME_SHIFT_MODE_DELAY;
	}

}

void time_shift_loop_hotkey(void *data, obs_hotkey_id id,
					obs_hotkey_t *hotkey, bool pressed)
{
	if (!pressed)
		return;
	struct time_shift_info *d = data;
	if (d->time_mode == TIME_SHIFT_MODE_LIVE)
	  d->time_mode = TIME_SHIFT_MODE_LOOP;
}

void time_shift_live_hotkey(void *data, obs_hotkey_id id,
				obs_hotkey_t *hotkey, bool pressed)
{
	if (!pressed)
		return;
	struct time_shift_info *d = data;
	if (d->time_diff <= 0.0)
		return;
	d->live_after_ticks = 2;
}

static void time_shift_load_hotkeys(void *data)
{
	struct time_shift_info *d = data;
	obs_source_t *parent = obs_filter_get_parent(d->source);
	if (parent) {
		d->delay_hotkey = obs_hotkey_register_source(
			parent, "delay", obs_module_text("RewindDelay"),
			time_shift_delay_hotkey, data);
		d->loop_hotkey = obs_hotkey_register_source(
			parent, "loop", obs_module_text("Loop"),
			time_shift_loop_hotkey, data);
		d->live_hotkey = obs_hotkey_register_source(
			parent, "live", obs_module_text("Live"),
			time_shift_live_hotkey, data);
		d->hotkeys_loaded = true;
	}
}

static void time_shift_load(void *data, obs_data_t *settings)
{
	time_shift_load_hotkeys(data);
	time_shift_update(data, settings);
}

static void draw_frame(struct time_shift_info *d)
{
	struct frame *frame = NULL;
	if (!d->frames.size)
		return;
	const size_t count = d->frames.size / sizeof(struct frame);
	if (d->time_diff <= 0.0) {
		frame = circlebuf_data(&d->frames,
				       (count - 1) * sizeof(struct frame));
	} else {
		size_t i = 0;
		uint64_t ts;
		if (d->time_mode == TIME_SHIFT_MODE_LOOP) {
			frame = circlebuf_data(
				&d->frames, (count - 1) * sizeof(struct frame));
			ts = frame->ts;
		} else {
			ts = obs_get_video_frame_time();
		}
		while (i < count) {
			frame = circlebuf_data(&d->frames,
					       i * sizeof(struct frame));
			if (ts - frame->ts <
			    (uint64_t)(d->time_diff * 1000000000.0))
				break;
			i++;
		}
	}
	if (!frame)
		return;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_texture_t *tex = gs_texrender_get_texture(frame->render);
	if (tex) {
		gs_eparam_t *image =
			gs_effect_get_param_by_name(effect, "image");
		gs_effect_set_texture(image, tex);

		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(tex, 0, d->cx, d->cy);
	}
}

static void time_shift_video_render(void *data, gs_effect_t *effect)
{
	struct time_shift_info *d = data;
	obs_source_t *target = obs_filter_get_target(d->source);
	obs_source_t *parent = obs_filter_get_parent(d->source);

	if (!d->target_valid || !target || !parent) {
		obs_source_skip_video_filter(d->source);
		return;
	}
	if (d->processed_frame) {
		draw_frame(d);
		return;
	}
	if (d->time_mode != TIME_SHIFT_MODE_LOOP) {

		const uint64_t ts = obs_get_video_frame_time();
		struct frame frame;
		frame.render = NULL;
		if (d->frames.size) {
			circlebuf_peek_front(&d->frames, &frame, sizeof(frame));
			if (ts - frame.ts <
			    (uint64_t)(d->max_duration * 1000000000.0)) {
				frame.render = NULL;
			} else {
				circlebuf_pop_front(&d->frames, &frame,
						    sizeof(frame));
			}
		}
		if (!frame.render) {
			frame.render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		} else {
			gs_texrender_reset(frame.render);
		}
		frame.ts = ts;

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		if (gs_texrender_begin(frame.render, d->cx, d->cy)) {
			uint32_t parent_flags =
				obs_source_get_output_flags(target);
			bool custom_draw =
				(parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
			bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
			struct vec4 clear_color;

			vec4_zero(&clear_color);
			gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
			gs_ortho(0.0f, (float)d->cx, 0.0f, (float)d->cy,
				 -100.0f, 100.0f);

			if (target == parent && !custom_draw && !async)
				obs_source_default_render(target);
			else
				obs_source_video_render(target);

			gs_texrender_end(frame.render);
		}

		gs_blend_state_pop();

		circlebuf_push_back(&d->frames, &frame, sizeof(frame));
	}

	draw_frame(d);
	d->processed_frame = true;
}

static obs_properties_t *time_shift_properties(void *data)
{
	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p = obs_properties_add_float(
		ppts, S_DURATION, obs_module_text("Delay"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, "s");

	p = obs_properties_add_list(ppts, S_SLOW_BACKWARD,
				    obs_module_text("RewindSpeed"),
				    OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "Half Speed", -50);
	obs_property_list_add_int(p, "Full Speed", -100);
	return ppts;
}

void time_shift_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, S_DURATION, 10.0);
	obs_data_set_default_double(settings, S_EASING_DURATION, 1.0);
	obs_data_set_default_int(settings, S_EASING, EASING_CUBIC);
	obs_data_set_default_double(settings, S_FAST_FORWARD, 200.0);
	obs_data_set_default_double(settings, S_SLOW_BACKWARD, -100.0);
	obs_data_set_default_string(settings, S_TEXT_SOURCE, "TimeShiftStatus");
}

static inline void check_size(struct time_shift_info *d)
{
	obs_source_t *target = obs_filter_get_target(d->source);

	d->target_valid = !!target;
	if (!d->target_valid)
		return;

	const uint32_t cx = obs_source_get_base_width(target);
	const uint32_t cy = obs_source_get_base_height(target);

	d->target_valid = !!cx && !!cy;
	if (!d->target_valid)
		return;

	if (cx != d->cx || cy != d->cy) {
		d->cx = cx;
		d->cy = cy;
		free_textures(d);
	}
}

static void time_shift_tick(void *data, float t)
{
	struct time_shift_info *d = data;
	if (!d->hotkeys_loaded)
		time_shift_load_hotkeys(data);
	d->processed_frame = false;
	if (d->live_after_ticks > 0) {
		d->live_after_ticks--;
		if (d->live_after_ticks == 0) {
			d->time_mode = TIME_SHIFT_MODE_LIVE;
			d->time_diff = 0.0;
			d->start_speed = 1.0;
			d->target_speed = 1.0;
			d->speed = 1.0;
			d->easing_started = 0;
		}
		
	}
	if (d->speed != d->target_speed) {
		const uint64_t ts = obs_get_video_frame_time();
		if (!d->easing_started)
			d->easing_started = ts;
		const double duration =
			(double)(ts - d->easing_started) / 1000000000.0;
		if (duration > d->easing_max_duration ||
		    d->easing_max_duration <= 0.0) {
			d->speed = d->target_speed;
		} else {
			double t2 = duration / d->easing_max_duration;
			if (d->easing == EASING_QUADRATIC) {
				t2 = QuadraticEaseInOut(t2);
			} else if (d->easing == EASING_CUBIC) {
				t2 = CubicEaseInOut(t2);
			} else if (d->easing == EASING_QUARTIC) {
				t2 = QuarticEaseInOut(t2);
			} else if (d->easing == EASING_QUINTIC) {
				t2 = QuinticEaseInOut(t2);
			} else if (d->easing == EASING_SINE) {
				t2 = SineEaseInOut(t2);
			} else if (d->easing == EASING_CIRCULAR) {
				t2 = CircularEaseInOut(t2);
			} else if (d->easing == EASING_EXPONENTIAL) {
				t2 = ExponentialEaseInOut(t2);
			} else if (d->easing == EASING_ELASTIC) {
				t2 = ElasticEaseInOut(t2);
			} else if (d->easing == EASING_BOUNCE) {
				t2 = BounceEaseInOut(t2);
			} else if (d->easing == EASING_BACK) {
				t2 = BackEaseInOut(t2);
			}
			d->speed = d->start_speed +
				   (d->target_speed - d->start_speed) * t2;
		}
	} else if (d->easing_started) {
		d->easing_started = 0;
	}
	if (d->time_mode != TIME_SHIFT_MODE_LOOP && d->speed > 1.0 &&
	    d->target_speed > 1.0 &&
	    d->time_diff < d->easing_max_duration / 2.0) {
		d->start_speed = d->speed;
		d->target_speed = 1.0;
		d->easing_started = 0;
	}

	double time_diff = d->time_diff;
	if (d->time_mode == TIME_SHIFT_MODE_LOOP) {
		time_diff += -d->speed * t;
	} else {
		time_diff += (1.0 - d->speed) * t;
	}
	if (time_diff <= 0.0 && d->time_mode != TIME_SHIFT_MODE_LOOP)
		d->time_mode = TIME_SHIFT_MODE_LIVE;
	if (time_diff > 0.0 &&d->time_mode == TIME_SHIFT_MODE_LIVE)
		d->time_mode = TIME_SHIFT_MODE_DELAY;
	if (time_diff < 0.0) {
		time_diff = 0.0;
		if (d->speed > 0.0 && d->time_mode == TIME_SHIFT_MODE_LOOP) {
			d->speed = -1.0;
			d->start_speed = -1.0;
			d->target_speed = -1.0;
			d->easing_started = 0;
		} else if ( d->speed > 1.0) {
			d->speed = 1.0;
			d->start_speed = 1.0;
			d->target_speed = 1.0;
			d->easing_started = 0;
		}
			
	}
	if (time_diff > d->max_duration) {
		time_diff = d->max_duration;
		if (d->speed < 1.0) {
			d->speed = 1.0;
			d->start_speed = 1.0;
			d->target_speed = 1.0;
			d->easing_started = 0;
		}
	}
	d->time_diff = time_diff;
	
	time_shift_text(d);
	check_size(d);
}



struct obs_source_info time_shift_filter = {
	.id = "time_shift_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_OUTPUT_VIDEO,
	.get_name = time_shift_get_name,
	.create = time_shift_create,
	.destroy = time_shift_destroy,
	.load = time_shift_load,
	.update = time_shift_update,
	.video_render = time_shift_video_render,
	.get_properties = time_shift_properties,
	.get_defaults = time_shift_defaults,
	.video_tick = time_shift_tick,
};

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Exeldro");
OBS_MODULE_USE_DEFAULT_LOCALE("time-shift", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("TimeShift");
}

extern struct obs_source_info time_glitch_filter;

bool obs_module_load(void)
{
	obs_register_source(&time_shift_filter);
	obs_register_source(&time_glitch_filter);
	return true;
}
