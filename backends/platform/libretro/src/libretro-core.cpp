/* Copyright (C) 2023 Giovanni Cascione <ing.cascione@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "audio/mixer_intern.h"
#include "base/main.h"
#include "common/scummsys.h"
#include "common/str.h"
#include "common/fs.h"
#include "common/error.h"
#include "common/array.h"
#include "common/savefile.h"
#include "common/config-manager.h"
#include "engines/engine.h"
#include "streams/file_stream.h"
#include <file/file_path.h>
#include <retro_dirent.h>
#include "graphics/surface.h"
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#ifndef _MSC_VER
/**
 * Include libgen.h for basename() and dirname().
 * @see http://linux.die.net/man/3/basename
 */
#include <libgen.h>
#endif

#include <features/features_cpu.h> // cpu_features_get_time_usec()
#include <retro_atomic.h>
#include <retro_miscellaneous.h> // PATH_MAX_LENGTH

/**
 * Include base/internal_version.h to allow access to SCUMMVM_VERSION.
 * @see retro_get_system_info()
 */
#define INCLUDED_FROM_BASE_VERSION_CPP
#include "base/internal_version.h"

#include "graphics/managed_surface.h"

#include "backends/platform/libretro/include/libretro-defs.h"
#include "backends/platform/libretro/include/libretro-core.h"
#include "backends/platform/libretro/include/libretro-gl-context-handoff.h"
#include "backends/platform/libretro/include/libretro-threads.h"
#include "backends/platform/libretro/include/libretro-core-options.h"
#include "backends/platform/libretro/include/libretro-os.h"
#include "backends/platform/libretro/include/libretro-fs.h"
#include "backends/platform/libretro/include/libretro-mapper.h"

static struct retro_game_info game_buf;
static struct retro_game_info *game_buf_ptr;
/* retro_game_info::path is a frontend-owned pointer; retro_reset() reuses
 * game_buf_ptr to relaunch, so it must not depend on that pointer staying
 * valid after the initial retro_load_game() call. Own a stable copy instead. */
static char game_buf_path[PATH_MAX_LENGTH];

retro_log_printf_t retro_log_cb = NULL;
retro_input_state_t retro_input_cb = NULL;
static retro_video_refresh_t video_cb = NULL;
static retro_audio_sample_batch_t audio_batch_cb = NULL;
static retro_environment_t environ_cb = NULL;
static retro_input_poll_t poll_cb = NULL;
static int retro_input_device = RETRO_DEVICE_JOYPAD;

// MIDI interface
struct retro_midi_interface *retro_midi_interface = nullptr;

// Default deadzone: 15%
static int analog_deadzone = (int)(0.15f * ANALOG_RANGE);

static float gamepad_cursor_speed = 1.0f;
static bool analog_response_is_quadratic = false;

static float mouse_speed = 1.0f;
static float gamepad_acceleration_time = 0.2f;
static int mouse_fine_control_speed_reduction = 4;
static int pointer_device = RETRO_DEVICE_JOYPAD; // default pointer/mouse device

char cmd_params[20][200];
char cmd_params_num;

static uint8 video_hw_mode = 0;

static char render_mode_setting[16] = "default";
static unsigned base_width = RES_W_OVERLAY;
static unsigned base_height = RES_H_OVERLAY;
static unsigned gui_width = RES_W_OVERLAY;
static unsigned gui_height = RES_H_OVERLAY;
static unsigned max_width = RES_INIT_MAX_W;
static unsigned max_height = RES_INIT_MAX_H;

static uint16 av_status = AUDIO_STATUS_MUTE;

static float frame_rate = 0;
static uint16 sample_rate = 0;
static float audio_samples_per_frame   = 0.0f; // length in samples per frame
static float audio_samples_accumulator = 0.0f;
static retro_time_t audio_last_time_usec = 0; // timestamp of the previous audio_run()

static int16 *audio_sample_buffer = NULL; // pointer to output buffer

static bool input_bitmask_supported = false;
static bool browsing_mode_authorized = false;
static bool gmm_save_enabled = false;
static bool updating_variables = false;

#ifdef USE_OPENGL
static struct retro_hw_render_callback hw_render;

/* Set on the frontend thread by context_reset(), consumed on the emulation
 * thread by retro_consume_context_reset(). */
static retro_atomic_int_t context_reset_pending = RETRO_ATOMIC_INT_INITIALIZER(0);

void retro_set_context_reset_pending(void) {
	retro_atomic_store_release_int(&context_reset_pending, 1);
}

bool retro_consume_context_reset(void) {
	if (!retro_atomic_load_acquire_int(&context_reset_pending))
		return false;
	retro_atomic_store_release_int(&context_reset_pending, 0);
	return true;
}

static void context_reset(void) {
	retro_log_cb(RETRO_LOG_DEBUG, "HW context reset\n");
	/* The reset re-creates the GL context and reloads all GL entry points,
	   which must happen on the emulation thread where the context is current.
	   Defer it instead of calling it here on the frontend thread. */
	if (retro_emu_thread_started())
		retro_set_context_reset_pending();
}

static void context_destroy(void) {
	retro_log_cb(RETRO_LOG_DEBUG, "HW context destroy\n");
}

uintptr_t retro_get_hw_fb(void) {
	return hw_render.get_current_framebuffer();
}

void *retro_get_proc_address(const char *name) {
	return (void *)(hw_render.get_proc_address(name));
}
#endif

#ifdef USE_HIGHRES
static void retro_gui_res_reset() {
	if (retro_emu_thread_started()) {
		LIBRETRO_G_SYSTEM->beginGFXTransaction();
		LIBRETRO_G_SYSTEM->initSize(0, 0, nullptr);
		LIBRETRO_G_SYSTEM->endGFXTransaction();
	}
}
#endif

/* Single-producer / single-consumer ring. The producer is ScummVM's MIDI
   driver, the consumer is retro_midi_queue_drain() in retro_run(). */
static retro_midi_event_t midi_queue[MIDI_QUEUE_SIZE];
static retro_atomic_int_t midi_head = RETRO_ATOMIC_INT_INITIALIZER(0); /* published by producer */
static retro_atomic_int_t midi_tail = RETRO_ATOMIC_INT_INITIALIZER(0); /* published by consumer */

static void setup_hw_rendering(void) {

	enum retro_pixel_format pixel_fmt;
#ifdef USE_OPENGL
	/* ScummVM issues its GL calls from the emulation thread, so the frontend's
	   context has to travel with control at every thread switch. Without a
	   backend for that handoff those calls would land on a thread with no
	   current context, so stay on the software renderer instead. */
	if ((video_hw_mode & VIDEO_GRAPHIC_MODE_REQUEST_HW) && !retro_gl_context_handoff_available()) {
		if (retro_log_cb)
			retro_log_cb(RETRO_LOG_WARN, "No GL context handoff backend available, falling back to software.\n");
		retro_osd_notification("HW rendering unavailable on this platform.");
		video_hw_mode = VIDEO_GRAPHIC_MODE_REQUEST_SW;
	}

	if (video_hw_mode & VIDEO_GRAPHIC_MODE_REQUEST_HW) {
		pixel_fmt = RETRO_PIXEL_FORMAT_XRGB8888;
		if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pixel_fmt) && retro_log_cb)
			retro_log_cb(RETRO_LOG_WARN, "RETRO_PIXEL_FORMAT_XRGB8888 not supported.\n");
		hw_render.context_reset = context_reset;
		hw_render.context_destroy = context_destroy;
		hw_render.cache_context = false;
		hw_render.bottom_left_origin = true;
#if defined(HAVE_OPENGL)
		hw_render.context_type = RETRO_HW_CONTEXT_OPENGL;
		video_hw_mode |= VIDEO_GRAPHIC_MODE_HAVE_OPENGL;
#elif defined(HAVE_OPENGLES2)
		hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES2;
		video_hw_mode |= VIDEO_GRAPHIC_MODE_HAVE_OPENGLES2;
#endif
		if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render)) {
			retro_log_cb(RETRO_LOG_WARN, "Failed to set up hardware rendering, falling back to software.\n");
			retro_osd_notification("Failed to set up HW rendering.");
			video_hw_mode = VIDEO_GRAPHIC_MODE_REQUEST_SW;
		}
	}
#endif
	if ((video_hw_mode & VIDEO_GRAPHIC_MODE_REQUEST_SW) || !video_hw_mode) {
		pixel_fmt = RETRO_PIXEL_FORMAT_RGB565;
		if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pixel_fmt) && retro_log_cb)
			retro_log_cb(RETRO_LOG_WARN, "RETRO_PIXEL_FORMAT_RGB565 not supported.\n");
	}
}

uint8 retro_get_video_hw_mode(void) {
	return video_hw_mode;
}

void process_key_event_wrapper(bool down, unsigned keycode, uint32_t character, uint16_t key_modifiers) {
	LIBRETRO_G_SYSTEM->processKeyEvent(down, keycode, character, key_modifiers);
}

static void log_scummvm_exit_code(void) {
	if (retro_get_scummvm_res() == Common::kNoError)
		retro_log_cb(RETRO_LOG_INFO, "ScummVM exited successfully.\n");
	else if (retro_get_scummvm_res() < Common::kNoError)
		retro_log_cb(RETRO_LOG_WARN, "Unknown ScummVM exit code.\n");
	else
		retro_log_cb(RETRO_LOG_ERROR, "ScummVM exited with error %d.\n", retro_get_scummvm_res());
}

static void audio_buffer_init(uint16 sample_rate, uint16 frame_rate) {
	audio_samples_accumulator = 0.0f;
	audio_last_time_usec      = cpu_features_get_time_usec();
	audio_samples_per_frame   = (float)sample_rate / (float)frame_rate;
	uint32 audio_sample_buffer_size  = ((uint32)retro_setting_get_audio_samples_buffer_size()) * 2 * sizeof(int16);
	audio_sample_buffer       = audio_sample_buffer ? (int16 *)realloc(audio_sample_buffer, audio_sample_buffer_size) : (int16 *)malloc(audio_sample_buffer_size);

	if (audio_sample_buffer)
		memset(audio_sample_buffer, 0, audio_sample_buffer_size);
	else
		retro_log_cb(RETRO_LOG_ERROR, "audio_buffer_init error.\n");
}

static void audio_run(void) {
	int16 *audio_buffer_ptr;
	uint32 samples_to_read;
	uint32 samples_produced;
	uint16 samples_buffer_size = retro_setting_get_audio_samples_buffer_size();

	/* Pace audio against elapsed wall time instead of a fixed sample_rate/frame_rate
	 * batch, so output stays at sample_rate even when the frontend runs retro_run()
	 * above or below frame_rate (a slow frontend would otherwise underrun the audio
	 * buffer). Video stays frame-locked. */
	retro_time_t now_usec = cpu_features_get_time_usec();
	float samples_target = (float)sample_rate * (float)(now_usec - audio_last_time_usec) / 1000000.0f;
	audio_last_time_usec = now_usec;

	/* Samples_target is decimal; get integer component */
	samples_to_read = (uint32)samples_target;

	/* Account for fractional component */
	audio_samples_accumulator += samples_target - (float)samples_to_read;

	if (audio_samples_accumulator >= 1.0f) {
		samples_to_read++;
		audio_samples_accumulator -= 1.0f;
	}

	/* Bound the batch to the mix buffer: after a long stall (content load,
	 * save-state) the elapsed interval would otherwise request a multi-second
	 * batch and overrun audio_sample_buffer. */
	if (samples_to_read > samples_buffer_size)
		samples_to_read = samples_buffer_size;

	samples_produced = ((Audio::MixerImpl *)g_system->getMixer())->mixCallback((byte *) audio_sample_buffer, samples_to_read * 2 * sizeof(int16));

	/* Workaround as currently there's no way to detect silence-only buffers from the mixer */
	if (samples_produced) {
		int i = 0;
		for (; i < samples_produced; i += 2)
			/* SID streams constant crap */
			if (READ_UINT16(audio_sample_buffer + i) > 32)
				break;
		samples_produced = i >= samples_produced ? 0 : samples_produced;
	}

	if (samples_produced)
		av_status &= ~AUDIO_STATUS_MUTE;
	else {
		av_status |= AUDIO_STATUS_MUTE;
		return;
	}

	/* Workaround for a RetroArch audio driver
	 * limitation: a maximum of 1024 frames
	 * can be written per call of audio_batch_cb(),
	 * so we have to send samples in chunks */
	audio_buffer_ptr = audio_sample_buffer;
	while (samples_produced > 0) {
		uint32 samples_to_write = (samples_produced > AUDIO_BATCH_FRAMES_MAX) ? AUDIO_BATCH_FRAMES_MAX : samples_produced;

		audio_batch_cb(audio_buffer_ptr, samples_to_write);

		samples_produced -= samples_to_write;
		audio_buffer_ptr += samples_to_write << 1;
	}
}

void retro_osd_notification(const char *msg) {
	if (!msg || *msg == '\0')
		return;
	struct retro_message_ext retro_msg;
	retro_msg.type = RETRO_MESSAGE_TYPE_NOTIFICATION;
	retro_msg.target = RETRO_MESSAGE_TARGET_OSD;
	retro_msg.duration = 3000;
	retro_msg.msg = msg;
	environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &retro_msg);
}

static void update_variables(void) {
	struct retro_variable var;
	updating_variables = true;

	var.key = "scummvm_pointer_device";
	var.value = NULL;
	pointer_device = RETRO_DEVICE_JOYPAD;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		if (strcmp(var.value, "mouse") == 0)
			pointer_device = RETRO_DEVICE_MOUSE;
		else if (strcmp(var.value, "pointer") == 0)
			pointer_device = RETRO_DEVICE_POINTER;
		/* else if (strcmp(var.value, "retropad") == 0)
			pointer_device = RETRO_DEVICE_JOYPAD; */
	}

	var.key = "scummvm_gamepad_cursor_speed";
	var.value = NULL;
	gamepad_cursor_speed = 1.0f;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		gamepad_cursor_speed = (float)atof(var.value);
	}

	var.key = "scummvm_gamepad_cursor_acceleration_time";
	var.value = NULL;
	gamepad_acceleration_time = 0.2f;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		gamepad_acceleration_time = (float)atof(var.value);
	}

	var.key = "scummvm_analog_response";
	var.value = NULL;
	analog_response_is_quadratic = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		if (strcmp(var.value, "quadratic") == 0)
			analog_response_is_quadratic = true;
	}

	var.key = "scummvm_analog_deadzone";
	var.value = NULL;
	analog_deadzone = (int)(0.15f * ANALOG_RANGE);
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		analog_deadzone = (int)(atoi(var.value) * 0.01f * ANALOG_RANGE);
	}

	var.key = "scummvm_mouse_speed";
	var.value = NULL;
	mouse_speed = 1.0f;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mouse_speed = (float)atof(var.value);
	}

	var.key = "scummvm_mouse_fine_control_speed_reduction";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mouse_fine_control_speed_reduction = (int)atoi(var.value);
	}

	var.key = "scummvm_framerate";
	var.value = NULL;
	float old_frame_rate = frame_rate;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		if (strcmp(var.value, "disabled") == 0)
			frame_rate = environ_cb(RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE, &frame_rate) ? frame_rate : DEFAULT_REFRESH_RATE;
		else {
			char frame_rate_var[3] = {0};
			strncpy(frame_rate_var, var.value, 2);
			frame_rate = (float)atof(frame_rate_var);
		}
	} else
		frame_rate = DEFAULT_REFRESH_RATE;

	var.key = "scummvm_samplerate";
	var.value = NULL;
	uint16 old_sample_rate = sample_rate;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		char sample_rate_var[6] = {0};
		strncpy(sample_rate_var, var.value, 5);
		sample_rate = atoi(sample_rate_var);
	} else
		sample_rate = DEFAULT_SAMPLE_RATE;

	var.key = "scummvm_browsing_mode";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		browsing_mode_authorized = (strcmp(var.value, "authorized") == 0);
	else
#ifdef ANDROID
		browsing_mode_authorized = true;
#else
		browsing_mode_authorized = false;
#endif

	var.key = "scummvm_gmm_save";
	var.value = NULL;
	gmm_save_enabled = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && (strcmp(var.value, "enabled") == 0);

	var.key = "scummvm_mapper_up";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_UP, var.value);
	}

	var.key = "scummvm_mapper_down";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_DOWN, var.value);
	}

	var.key = "scummvm_mapper_left";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_LEFT, var.value);
	}

	var.key = "scummvm_mapper_right";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_RIGHT, var.value);
	}

	var.key = "scummvm_mapper_a";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_A, var.value);
	}

	var.key = "scummvm_mapper_b";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_B, var.value);
	}

	var.key = "scummvm_mapper_x";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_X, var.value);
	}

	var.key = "scummvm_mapper_y";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_Y, var.value);
	}

	var.key = "scummvm_mapper_select";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_SELECT, var.value);
	}

	var.key = "scummvm_mapper_start";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_START, var.value);
	}

	var.key = "scummvm_mapper_l";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_L, var.value);
	}

	var.key = "scummvm_mapper_r";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_R, var.value);
	}

	var.key = "scummvm_mapper_l2";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_L2, var.value);
	}

	var.key = "scummvm_mapper_r2";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_R2, var.value);
	}

	var.key = "scummvm_mapper_l3";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_L3, var.value);
	}

	var.key = "scummvm_mapper_r3";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_R3, var.value);
	}

	var.key = "scummvm_mapper_lu";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_LU, var.value);
	}

	var.key = "scummvm_mapper_ld";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_LD, var.value);
	}

	var.key = "scummvm_mapper_ll";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_LL, var.value);
	}

	var.key = "scummvm_mapper_lr";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_LR, var.value);
	}

	var.key = "scummvm_mapper_ru";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_RU, var.value);
	}

	var.key = "scummvm_mapper_rd";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_RD, var.value);
	}

	var.key = "scummvm_mapper_rl";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_RL, var.value);
	}

	var.key = "scummvm_mapper_rr";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		mapper_set_device_keys(RETRO_DEVICE_ID_JOYPAD_RR, var.value);
	}

	var.key = "scummvm_video_hw_acceleration";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		if (strcmp(var.value, "enabled") == 0) {
			if (video_hw_mode & VIDEO_GRAPHIC_MODE_REQUEST_SW) {
				// video_hw_mode = VIDEO_GRAPHIC_MODE_REQUEST_HW;
				video_hw_mode |= VIDEO_GRAPHIC_MODE_RESET_PENDING;
			} else if (!video_hw_mode)
				video_hw_mode = VIDEO_GRAPHIC_MODE_REQUEST_HW;
		} else {
			if (video_hw_mode & VIDEO_GRAPHIC_MODE_REQUEST_HW) {
				// video_hw_mode = VIDEO_GRAPHIC_MODE_REQUEST_SW;
				video_hw_mode |= VIDEO_GRAPHIC_MODE_RESET_PENDING;
			} else if (!video_hw_mode)
				video_hw_mode = VIDEO_GRAPHIC_MODE_REQUEST_SW;
		}
	}

#ifdef USE_HIGHRES
	var.key = "scummvm_gui_h_res";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		uint16 new_gui_height = (int)atoi(var.value);
		av_status |= new_gui_height != gui_height && LIBRETRO_G_SYSTEM && LIBRETRO_G_SYSTEM->inLauncher() ? AV_STATUS_UPDATE_GUI : 0;
		gui_height = new_gui_height;
	}

	var.key = "scummvm_render_mode";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		snprintf(render_mode_setting, sizeof(render_mode_setting), "%s", var.value);

	var.key = "scummvm_gui_aspect_ratio";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		uint8 num = 4;
		uint8 den = 3;
		if (atoi(var.value)) {
			num = 16;
			den = 9;
		}
		uint16 new_gui_width = gui_height * num / den + (gui_height * num % den != 0);
		av_status |= (new_gui_width != gui_width) && LIBRETRO_G_SYSTEM && LIBRETRO_G_SYSTEM->inLauncher() ? AV_STATUS_UPDATE_GUI : 0;
		gui_width = new_gui_width;
	}
#endif

	if (old_frame_rate != frame_rate || old_sample_rate != sample_rate) {
		av_status |= AUDIO_STATUS_UPDATE_LATENCY;
		audio_buffer_init(sample_rate, (uint16) frame_rate);
		if (g_system)
			av_status |= (AV_STATUS_UPDATE_AV_INFO | AV_STATUS_RESET_PENDING);
	}

	if (video_hw_mode & VIDEO_GRAPHIC_MODE_RESET_PENDING) {
		/* TODO: evaluate if setting can be applied on the fly
		setup_hw_rendering();
		LIBRETRO_G_SYSTEM->resetGraphicsManager();
		retro_reset(); */
		retro_osd_notification("Core reload is needed to apply HW acceleration setting change.");
		video_hw_mode &= ~VIDEO_GRAPHIC_MODE_RESET_PENDING;
	}

	updating_variables = false;
}

static void retro_set_options_display(void) {
	/*struct retro_core_option_display option_display;

	option_display.visible = opt_frameskip_threshold_display;
	option_display.key = "scummvm_frameskip_threshold";
	environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

	option_display.visible = opt_frameskip_no_display;
	option_display.key = "scummvm_frameskip_no";
	environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);*/
}

static bool retro_update_options_display(void) {
	if (updating_variables)
		return false;

	/* Core options */
	bool updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
		update_variables();
		LIBRETRO_G_SYSTEM->refreshRetroSettings();
		retro_set_options_display();
	}
	return updated;
}

int retro_setting_get_analog_deadzone(void) {
	return analog_deadzone;
}

float retro_setting_get_gamepad_cursor_speed(void) {
	return gamepad_cursor_speed;
}

bool retro_setting_get_analog_response_is_quadratic(void) {
	return analog_response_is_quadratic;
}

float retro_setting_get_mouse_speed(void) {
	return mouse_speed;
}

int retro_setting_get_mouse_fine_control_speed_reduction(void) {
	return mouse_fine_control_speed_reduction;
}

float retro_setting_get_gamepad_acceleration_time(void) {
	return gamepad_acceleration_time;
}

int retro_setting_get_pointer_device(void) {
	return pointer_device;
}

float retro_setting_get_frame_rate(void) {
	return frame_rate;
}

int retro_setting_get_gui_res_w(void) {
	return gui_width;
}

int retro_setting_get_gui_res_h(void) {
	return gui_height;
}

bool retro_get_input_bitmask_supported(void) {
	return input_bitmask_supported;
}

uint16 retro_setting_get_sample_rate(void) {
	return sample_rate;
}

bool retro_setting_get_browsing_mode_authorized(void) {
	return browsing_mode_authorized;
}

bool retro_setting_get_gmm_save_enabled(void) {
	return gmm_save_enabled;
}


static uint32 next_pow2(uint32 x) {
	if (x <= 1) return 1;
	x--;
	x |= x >> 1; x |= x >> 2; x |= x >> 4;
	x |= x >> 8; x |= x >> 16;
	return x + 1;
}

uint16 retro_setting_get_audio_samples_buffer_size(void) {
	/* ScummVM audio buffer size is normally between 512 and 8192, but the value
	must be one of: 256, 512, 1024, 2048, 4096, 8192, 16384, or 32768. */
	static const uint16 allowed[] = {256,512,1024,2048,4096,8192,16384,32768};
	uint32 target = (uint32)(audio_samples_per_frame * 2.0f + 0.5f); // stereo
	uint32 pow2   = next_pow2(target);
	for (uint16 v : allowed) {
		if (pow2 <= v) return v;
	}
	return allowed[ARRAYSIZE(allowed) - 1];
}

void init_command_params(void) {
	memset(cmd_params, 0, sizeof(cmd_params));
	cmd_params_num = 1;
	strcpy(cmd_params[0], "scummvm\0");
}

int retro_get_input_device(void) {
	return retro_input_device;
}

/* Bounds-checked append to cmd_params: silently drops parameters past the
 * fixed-size array capacity/width instead of overflowing them. */
static bool append_command_param(const char *param) {
	if (cmd_params_num >= ARRAYSIZE(cmd_params)) {
		if (retro_log_cb)
			retro_log_cb(RETRO_LOG_ERROR, "[scummvm] Too many command line parameters, ignoring '%s'.\n", param);
		return false;
	}

	size_t param_len = strlen(param);

	if (param_len >= sizeof(cmd_params[cmd_params_num])) {
		if (retro_log_cb)
			retro_log_cb(RETRO_LOG_ERROR, "[scummvm] Command line parameter too long, ignoring '%s'.\n", param);
		return false;
	}

	memcpy(cmd_params[cmd_params_num], param, param_len + 1);
	cmd_params_num++;
	return true;
}

void parse_command_params(char *cmdline) {
	int j = 0;
	int cmdlen = strlen(cmdline);
	bool quotes = false;

	if (!cmdlen) return;

	/* Append a new line to the end of the command to signify it's finished. */
	cmdline[cmdlen] = '\n';
	cmdline[++cmdlen] = '\0';

	/* parse command line into array of arguments */
	for (int i = 0; i < cmdlen; i++) {
		switch (cmdline[i]) {
		case '\"':
			if (quotes) {
				cmdline[i] = '\0';
				append_command_param(cmdline + j);
				quotes = false;
			} else
				quotes = true;
			j = i + 1;
			break;
		case ' ':
		case '\n':
			if (!quotes) {
				if (i != j && !quotes) {
					cmdline[i] = '\0';
					append_command_param(cmdline + j);
				}
				j = i + 1;
			}
			break;
		}
	}
}

static void exit_to_frontend(void) {
	environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}

// Bound on how many times close_emu_thread() will re-request a quit and
// resume the emulator thread before giving up. Each iteration costs one
// thread timeslice, so this is roughly a five second grace period.
static const int LIBRETRO_QUIT_MAX_SWITCHES = 600;

static void close_emu_thread(void) {
	// This loop used to be unbounded. Every iteration pushes another
	// EVENT_QUIT and hands the emulator thread a timeslice, waiting for the
	// engine to observe the event and return from scummvm_main(). An engine
	// that keeps yielding but never observes the quit spins here forever,
	// and because this runs on the frontend's own thread the whole browser
	// tab locks up with nothing logged. Griffon does exactly that: its
	// checkInputs() returns early while _attacking or _forcePause is set,
	// and that early return sits above its EVENT_QUIT check, so the event is
	// discarded (upstream ScummVM has the same ordering). Give up after a
	// bounded number of attempts and tear down anyway -- a leaked engine
	// thread on a core that is being unloaded is far cheaper than a hung
	// tab. Mirrors LIBRETRO_SAVESTATE_MAX_SWITCHES in the save-state bridge.
	int switches = 0;
	while (retro_emu_thread_started() && !retro_emu_thread_exited()) {
		if (switches++ >= LIBRETRO_QUIT_MAX_SWITCHES) {
			if (retro_log_cb)
				retro_log_cb(RETRO_LOG_WARN,
					"[scummvm] engine did not acknowledge quit after %d attempts; forcing teardown\n",
					LIBRETRO_QUIT_MAX_SWITCHES);
			break;
		}
		LIBRETRO_G_SYSTEM->requestQuit();
		retro_switch_to_emu_thread();
	}
	retro_deinit_emu_thread();
}

#if defined(WIIU) || defined(__SWITCH__) || defined(_MSC_VER) || defined(_3DS)
#include <stdio.h>
#include <string.h>
char *dirname(char *path) {
	char *p;
	if (path == NULL || *path == '\0')
		return ".";
	p = path + strlen(path) - 1;
	while (*p == '/') {
		if (p == path)
			return path;
		*p-- = '\0';
	}
	while (p >= path && *p != '/')
		p--;
	return p < path ? "." : p == path ? "/" : (*p = '\0', path);
}
#endif

#if (defined(GEKKO) && !defined(WIIU)) || defined(__CELLOS_LV2__)
int access(const char *path, int amode) {
	RFILE *f;
	int mode;

	switch (amode) {
	// we don't really care if a file exists but isn't readable
	case F_OK:
	case R_OK:
		mode = RETRO_VFS_FILE_ACCESS_READ;
		break;

	case W_OK:
		mode = RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
		break;

	default:
		return -1;
	}

	f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);

	if (f) {
		filestream_close(f);
		return 0;
	}

	return -1;
}
#endif

void retro_set_video_refresh(retro_video_refresh_t cb) {
	video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb) {}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
	audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb) {
	poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb) {
	retro_input_cb = cb;
}

void retro_set_environment(retro_environment_t cb) {
	environ_cb = cb;

	bool tmp = true;
	bool has_categories;
	environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &tmp);
	libretro_fill_options_mapper_data(environ_cb);
	libretro_set_core_options(environ_cb, &has_categories);

	/* Core option display callback */
	struct retro_core_options_update_display_callback update_display_callback = {retro_update_options_display};
	environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK, &update_display_callback);
}

unsigned retro_api_version(void) {
	return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info) {
	info->library_name = CORE_NAME;
#if defined GIT_TAG
#define __GIT_VERSION GIT_TAG
#elif defined GIT_HASH
#define __GIT_VERSION GIT_HASH "-" SCUMMVM_VERSION
#else
#define __GIT_VERSION ""
#endif
	info->library_version = __GIT_VERSION;
	info->valid_extensions = "scummvm";
	info->need_fullpath = true;
	info->block_extract = false;
}

void retro_set_size(unsigned width, unsigned height) {
	if (base_width == width && base_height == height) {
		return;
	} else if (width > max_width || height > max_height) {
		max_width = width;
		max_height = height;
		av_status |= AV_STATUS_UPDATE_AV_INFO;
	} else
		av_status |= AV_STATUS_UPDATE_GEOMETRY;

	base_width = width;
	base_height = height;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
	info->geometry.base_width = base_width;
	info->geometry.base_height = base_height;
	info->geometry.max_width = max_width;
	info->geometry.max_height = max_height;
	info->geometry.aspect_ratio = (float)base_width / (float)base_height;
	info->timing.fps = frame_rate;
	info->timing.sample_rate = sample_rate;
}

const char *retro_get_core_dir(void) {
	const char *coredir = NULL;

	environ_cb(RETRO_ENVIRONMENT_GET_LIBRETRO_PATH, &coredir);

	return coredir;
}

const char *retro_get_system_dir(void) {
	const char *sysdir = NULL;

	environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sysdir);

	return sysdir;
}

const char *retro_get_file_browser_start_dir(void) {
	const char *startdir = NULL;

	if (!environ_cb || !environ_cb(RETRO_ENVIRONMENT_GET_FILE_BROWSER_START_DIRECTORY, &startdir))
		return NULL;

	return startdir;
}

const char *retro_get_save_dir(void) {
	const char *savedir = NULL;

	environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &savedir);

	return savedir;
}

const char *retro_get_playlist_dir(void) {
	const char *playlistdir = NULL;

	environ_cb(RETRO_ENVIRONMENT_GET_PLAYLIST_DIRECTORY, &playlistdir);

	return playlistdir;
}

void retro_midi_queue_push(uint8 byte, uint32 delta_us) {
	/* The producer owns head, so it can read it plainly; tail needs an
	   acquire load to pair with the consumer's release below. */
	int head = retro_atomic_load_acquire_int(&midi_head);
	int next = (head + 1) & (MIDI_QUEUE_SIZE - 1);

	if (next == retro_atomic_load_acquire_int(&midi_tail)) {
		/* Queue full → drop event (acceptable for MIDI) */
		return;
	}

	midi_queue[head].byte     = byte;
	midi_queue[head].delta_us = delta_us;

	/* Release: the two stores above are visible to any thread that
	   acquire-loads this index. */
	retro_atomic_store_release_int(&midi_head, next);
}

static void retro_midi_queue_drain(void) {
	if (!retro_midi_interface)
		return;
	if (!retro_midi_interface->output_enabled)
		return;
	if (!retro_midi_interface->output_enabled())
		return;

	bool did_write = false;
	int tail = retro_atomic_load_acquire_int(&midi_tail);
	int head = retro_atomic_load_acquire_int(&midi_head);

	while (tail != head) {
		retro_midi_event_t ev = midi_queue[tail];
		tail = (tail + 1) & (MIDI_QUEUE_SIZE - 1);

		retro_midi_interface->write(ev.byte, ev.delta_us);
		did_write = true;
	}

	if (did_write) {
		/* Publish once: the producer only needs to know the slots are
		   free, not how far along the drain got. */
		retro_atomic_store_release_int(&midi_tail, tail);
		retro_midi_interface->flush();
	}
}

/* Authorized storage roots (e.g. Android SAF trees) as granted through the
 * frontend. */
static void refresh_authorized_locations(void) {
	LibRetroFilesystemNode::clearAuthorizedLocations();

	struct retro_vfs_authorized_locations locations;
	memset(&locations, 0, sizeof(locations));

	if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VFS_AUTHORIZED_LOCATIONS, &locations) &&
			locations.locations) {
		if (retro_log_cb)
			retro_log_cb(RETRO_LOG_DEBUG, "SAF locations count: %zu\n", locations.count);
		for (size_t i = 0; i < locations.count; ++i) {
			const char *path = locations.locations[i].path;
			const char *label = locations.locations[i].label;

			if (path && *path)
				LibRetroFilesystemNode::addAuthorizedLocation(
						Common::String(path),
						label ? Common::String(label) : Common::String());
		}
	}
}

void retro_init(void) {
	struct retro_log_callback log;
	if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
		retro_log_cb = log.log;
	else
		retro_log_cb = NULL;

	if (retro_log_cb)
		retro_log_cb(RETRO_LOG_DEBUG, "ScummVM core version: %s\n", __GIT_VERSION);

	struct retro_vfs_interface_info vfs_iface;
	vfs_iface.required_interface_version = STAT64_REQUIRED_VFS_VERSION;
	vfs_iface.iface = nullptr;

	bool vfs_ok = environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface);

	if (vfs_ok) {
		filestream_vfs_init(&vfs_iface);
		path_vfs_init(&vfs_iface);
		dirent_vfs_init(&vfs_iface);
	}

	refresh_authorized_locations();

	update_variables();

	if (retro_setting_get_browsing_mode_authorized() && !LibRetroFilesystemNode::hasAuthorizedLocations()) {
		if (retro_log_cb)
			retro_log_cb(RETRO_LOG_WARN, "Browsing mode set to 'Authorized storage' but no authorized locations are available; falling back to local filesystem. Authorize folders from the frontend and restart the core.\n");
		retro_osd_notification("No authorized storage available, using local filesystem.");
	}

	max_width = gui_width > max_width ? gui_width : max_width;
	max_height = gui_height > max_height ? gui_height : max_height;

	retro_set_options_display();

	init_command_params();

	setup_hw_rendering();

	environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, retro_input_desc);

	environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)retro_controller_lists);

	retro_keyboard_callback cb = {process_key_event_wrapper};
	environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &cb);

	if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
		input_bitmask_supported = true;

	// Initialize MIDI interface
	static struct retro_midi_interface midi_interface;
	if (environ_cb(RETRO_ENVIRONMENT_GET_MIDI_INTERFACE, &midi_interface)) {
		retro_midi_interface = &midi_interface;
		if (retro_log_cb)
			retro_log_cb(RETRO_LOG_INFO, "MIDI interface initialized\n");
	} else {
		retro_midi_interface = nullptr;
		if (retro_log_cb)
			retro_log_cb(RETRO_LOG_INFO, "MIDI interface unavailable\n");
	}

	g_system = new OSystem_libretro();
}

void retro_deinit(void) {
	LIBRETRO_G_SYSTEM->destroy();

	if (audio_sample_buffer)
		free(audio_sample_buffer);

	audio_sample_buffer       = NULL;
	audio_samples_per_frame   = 0.0f;
	audio_samples_accumulator = 0.0f;
	log_scummvm_exit_code();
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
	if (port != 0) {
		if (retro_log_cb)
			retro_log_cb(RETRO_LOG_WARN, "Invalid controller port %d, expected port 0 (#1)\n", port);
		return;
	}

	switch (device) {
	case RETRO_DEVICE_JOYPAD:
	case RETRO_DEVICE_MOUSE:
	case RETRO_DEVICE_KEYBOARD:
	case RETRO_DEVICE_ANALOG:
	case RETRO_DEVICE_POINTER:
		retro_input_device = device;
		break;
	default:
		if (retro_log_cb)
			retro_log_cb(RETRO_LOG_WARN, "Invalid controller device class %d.\n", device);
		break;
	}
}

bool retro_load_game(const struct retro_game_info *game) {
	if (!g_system) {
		if (retro_log_cb)
			retro_log_cb(RETRO_LOG_ERROR, "[scummvm] Failed to initialize ScummVM.\n");
		return false;
	}

#ifdef LIBRETRO_DEBUG
	char debug_buf[] = "--debuglevel=11";
	parse_command_params(debug_buf);
#endif

	if (game) {
		game_buf_ptr = &game_buf;
		memcpy(game_buf_ptr, game, sizeof(retro_game_info));
		if (game->path) {
			strncpy(game_buf_path, game->path, sizeof(game_buf_path) - 1);
			game_buf_path[sizeof(game_buf_path) - 1] = '\0';
		} else {
			game_buf_path[0] = '\0';
		}
		game_buf.path = game_buf_path;
		// Retrieve the game path.
		Common::FSNode detect_target = Common::FSNode(game->path);
#ifdef EMSCRIPTEN
		// In this WASM/EmulatorJS deployment, the virtual filesystem root
		// ("/") is always exactly the current ROM's fully-extracted content
		// tree -- EmulatorJS's downloadRom() extracts every zip entry
		// preserving its relative path under "/", and nothing else is ever
		// mounted there for this core. Scanning from the anchor file's own
		// parent directory (the upstream default below, correct for a real
		// shared filesystem where many games' content might coexist in
		// sibling folders) is an unnecessary restriction here: it makes
		// detection depend on which arbitrary file EmulatorJS's
		// fileNames[0] heuristic happened to pick as game->path, breaking
		// any ROM whose zip doesn't happen to have its detection-relevant
		// anchor at the true top level. Using the true root instead lets
		// ScummVM's own directoryGlobs-based scan (already correct on
		// desktop platforms) work for any packaging. Guarded to this WASM
		// build only -- on a native build of this shared source, "/" is the
		// real OS root and this override would be wrong.
		Common::FSNode parent_dir = Common::FSNode(Common::Path("/"));
#else
		Common::FSNode parent_dir = detect_target.getParent();
#endif
		char target_id[400] = {0};
		char buffer[400] = {0};
		int test_game_status = TEST_GAME_KO_NOT_FOUND;

		const char *target_file_ext = ".scummvm";
		int target_file_ext_pos = strlen(game->path) - strlen(target_file_ext);

		// See if we are loading a .scummvm file.
		if (!(target_file_ext_pos < 0) && strstr(game->path + target_file_ext_pos, target_file_ext) != NULL) {
			// Open the file.
			RFILE *gamefile = filestream_open(game->path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
			if (!gamefile) {
				retro_log_cb(RETRO_LOG_ERROR, "[scummvm] Failed to load given game file '%s'.\n", game->path);
				return false;
			}

			// Load the file data.
			if (filestream_gets(gamefile, target_id, sizeof(target_id)) == NULL) {
				filestream_close(gamefile);
				retro_log_cb(RETRO_LOG_ERROR, "[scummvm] Failed to load contents of game file '%s'.\n", game->path);
				return false;
			}
			filestream_close(gamefile);

			Common::String tmp = target_id;
			tmp.trim();
			strcpy(target_id, tmp.c_str());

			if (strlen(target_id) == 0) {
				retro_log_cb(RETRO_LOG_ERROR, "[scummvm] Game file '%s' does not contain any target id.\n", game->path);
				return false;
			}

			test_game_status = LIBRETRO_G_SYSTEM->testGame(target_id, false);
		} else {
			if (detect_target.isDirectory()) {
				parent_dir = detect_target;
			} else {
				// If this node has no parent node, then it returns a duplicate of this node.
				if (detect_target.getPath().equals(parent_dir.getPath())) {
					retro_log_cb(RETRO_LOG_ERROR, "[scummvm] Autodetect not possible. No parent directory detected in '%s'.\n", game->path);
					return false;
				}
			}

			test_game_status = LIBRETRO_G_SYSTEM->testGame(parent_dir.getPath().toString().c_str(), true);
		}

		// Preliminary game scan results
		switch (test_game_status) {
		case TEST_GAME_OK_ID_FOUND:
			snprintf(buffer, sizeof(buffer), "-p \"%s\" %s", parent_dir.getPath().toString().c_str(), target_id);
			retro_log_cb(RETRO_LOG_DEBUG, "[scummvm] launch via target id and game dir\n");
			break;
		case TEST_GAME_OK_TARGET_FOUND:
			snprintf(buffer, sizeof(buffer), "%s", target_id);
			retro_log_cb(RETRO_LOG_DEBUG, "[scummvm] launch via target id and scummvm.ini\n");
			break;
		case TEST_GAME_OK_ID_AUTODETECTED:
			if (strcmp(render_mode_setting, "default") != 0)
				snprintf(buffer, sizeof(buffer), "-p \"%s\" --auto-detect --render-mode=%s", parent_dir.getPath().toString().c_str(), render_mode_setting);
			else
				snprintf(buffer, sizeof(buffer), "-p \"%s\" --auto-detect", parent_dir.getPath().toString().c_str());
			retro_log_cb(RETRO_LOG_DEBUG, "[scummvm] launch via autodetect\n");
			break;
		case TEST_GAME_KO_MULTIPLE_RESULTS:
			retro_log_cb(RETRO_LOG_WARN, "[scummvm] Multiple targets found for '%s' in scummvm.ini\n", target_id);
			retro_osd_notification("Multiple targets found");
			break;
		case TEST_GAME_KO_NOT_FOUND:
		default:
			retro_log_cb(RETRO_LOG_WARN, "[scummvm] Game not found. Check path and content of '%s'\n", game->path);
			retro_osd_notification("Game not found");
		}

		parse_command_params(buffer);
	} else {
		game_buf_ptr = NULL;
	}

	if (!retro_init_emu_thread()) {
		if (retro_log_cb)
			retro_log_cb(RETRO_LOG_ERROR, "[scummvm] Failed to initialize emulation thread!\n");
		return false;
	}
	return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info) {
	return false;
}

void retro_run(void) {
	/* Settings change is covered by RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK
	except in case of core options reset to defaults, for which the following call is needed*/
	retro_update_options_display();

	if (av_status & AV_STATUS_RESET_PENDING) {
		av_status &= ~AV_STATUS_RESET_PENDING;
		retro_reset();
		return;
	}

#ifdef USE_HIGHRES
	if (av_status & AV_STATUS_UPDATE_GUI) {
		retro_gui_res_reset();
		av_status &= ~AV_STATUS_UPDATE_GUI;
	}
#endif

	if (av_status & (AV_STATUS_UPDATE_AV_INFO | AV_STATUS_UPDATE_GEOMETRY)) {
		struct retro_system_av_info info;
		retro_get_system_av_info(&info);
		if (av_status & AV_STATUS_UPDATE_AV_INFO)
			environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);
		else
			environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &info);

		av_status &= ~(AV_STATUS_UPDATE_AV_INFO | AV_STATUS_UPDATE_GEOMETRY);
	}

	if (av_status & AUDIO_STATUS_UPDATE_LATENCY) {
		uint32 audio_latency;
		float frame_time_msec = 1000.0f / frame_rate;

		audio_latency = (uint32)((8.0f * frame_time_msec) + 0.5f);
		audio_latency = (audio_latency + 0x1F) & ~0x1F;

		/* This can only be called from within retro_run() */
		environ_cb(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY, &audio_latency);
		av_status &= ~AUDIO_STATUS_UPDATE_LATENCY;
	}

	/* Setting RA's video or audio driver to null will disable video/audio bits */
	int audio_video_enable = 0;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &audio_video_enable))
		/* If this flag is not supported, the core assumes that the frontend will not skip any steps, as per API contract */
		audio_video_enable = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;

	if (g_system) {
		/* Switch to ScummVM thread */
		retro_switch_to_emu_thread();

		if (retro_emu_thread_exited()) {
			exit_to_frontend();
			return;
		}

		/* Retrieve audio */
		if (audio_video_enable & 2)
			audio_run();

		/* Retrieve video */
		if (audio_video_enable & 1) {
			if (video_hw_mode & VIDEO_GRAPHIC_MODE_REQUEST_SW) {
				const Graphics::ManagedSurface *screen;
				LIBRETRO_G_SYSTEM->getScreen(screen);
				video_cb(screen->getPixels(), screen->w, screen->h, screen->pitch);
			} else
				video_cb(RETRO_HW_FRAME_BUFFER_VALID, LIBRETRO_G_SYSTEM->getScreenWidth(),  LIBRETRO_G_SYSTEM->getScreenHeight(), 0);

		}

		poll_cb();
		LIBRETRO_G_SYSTEM->processInputs();
	}

	retro_midi_queue_drain();
}

void retro_unload_game(void) {
	close_emu_thread();
}

void retro_reset(void) {
	close_emu_thread();
	init_command_params();
	refresh_authorized_locations();
	if (!retro_load_game(game_buf_ptr) && retro_log_cb)
		retro_log_cb(RETRO_LOG_ERROR, "[scummvm] Failed to reinitialize emulation thread on reset.\n");
	LIBRETRO_G_SYSTEM->resetQuit();
}

// Stubs
void *retro_get_memory_data(unsigned type) {
	return NULL;
}
size_t retro_get_memory_size(unsigned type) {
	return 0;
}

/*
 * Save-state bridge.
 *
 * ScummVM has no API to serialize a running engine's state into a memory
 * buffer -- Engine::saveGameState()/loadGameState() only know how to read
 * and write named slots via the SaveFileManager. So retro_serialize() and
 * retro_unserialize() are implemented by driving a *real* engine save/load
 * into a reserved slot, then copying that slot's save file bytes to/from
 * the buffer libretro gives us. This reuses the exact same save mechanism
 * as ScummVM's own in-game "Save"/"Load" menu (which already works, and
 * whose saves already persist across a reload) -- retro_serialize just
 * makes that mechanism reachable through EmulatorJS's/RetroArch's own
 * save-state UI (Save State, Exit & Save) instead of requiring the GMM.
 *
 * retro_serialize()/retro_unserialize() are called by the frontend from
 * what this backend calls the "main" thread -- see libretro-threads.cpp.
 * g_engine and everything reachable from it belong to the "emu thread",
 * which is a real pthread parked wherever the running engine last yielded
 * (see retro_switch_to_emu_thread()/retro_switch_to_main_thread()); it is
 * NOT safe to call g_engine->saveGameState()/loadGameState() directly from
 * here. Instead we set a pending-operation flag, drive the emu thread
 * forward with retro_switch_to_emu_thread() (exactly as retro_run() does
 * once per frame), and let retro_process_pending_savestate_op() -- called
 * from OSystem_libretro::pollEvent(), which runs on the emu thread --
 * actually do the work and report back.
 *
 * A further wrinkle: some engines' saveGameState()/loadGameState() (e.g.
 * SCUMM's) don't do the file I/O synchronously -- they just arm an
 * internal flag for their own main loop to act on later (see
 * Engine::isSaveOrLoadPending()). retro_process_pending_savestate_op()
 * accounts for this by waiting for isSaveOrLoadPending() to clear before
 * treating the request as finished, which may take more than one
 * pollEvent() call.
 */

// Slot 0 is generally the autosave slot, and SCUMM treats slot 100 as a
// special temporary-restart slot (see ScummEngine::requestLoad) -- 200 is
// comfortably outside both the autosave slot and SCUMM's normal UI-visible
// save slot range (0-99, see ScummMetaEngine::getMaximumSaveSlot()).
// Must also fit in a byte: ScummEngine::_saveLoadSlot (scumm.h) is declared
// `byte`, so a slot >255 (990 was tried first) silently truncates mod 256
// -- SCUMM actually wrote and read a completely different slot than the one
// this code asked for and later tried to read back, with no error anywhere
// in the chain to indicate the mismatch.
static const int LIBRETRO_SAVESTATE_SLOT = 200;

// retro_serialize_size() used to return a flat 8 MiB. The frontend writes
// whatever that call reports -- it never learns how much was actually used
// (tasks/task_save.c takes _len from core_serialize_size() and writes the
// whole buffer) -- so every save state was 8 MiB of mostly zero padding, and
// raising the bound to fit a large-save engine would have inflated all of
// them. The size is now estimated from the saves the target actually has, so
// most states are a fraction of that and the ceiling can be generous without
// anyone paying for it. These two clamp the estimate.
//
// The floor matters more than the ceiling: an estimate that comes in under
// what packing needs makes retro_serialize() fail, so err high.
static const size_t LIBRETRO_SAVESTATE_MIN_SIZE = 1 * 1024 * 1024;
static const size_t LIBRETRO_SAVESTATE_MAX_SIZE = 64 * 1024 * 1024;

// What to ask for when the target has no saves at all and the estimate has
// nothing to go on. This is the size the core always used to report, so a
// first save state can never fail where it previously succeeded -- which it
// would if an engine's first save happened to exceed a smaller floor. Once
// any save exists, including the reserved slot this very state writes, the
// estimate has real sizes to work from and drops well below this.
static const size_t LIBRETRO_SAVESTATE_UNKNOWN_SIZE = 8 * 1024 * 1024;

// The budget the frontend actually allocated for the state in flight. The
// packer runs on the emu thread and cannot see retro_serialize()'s size
// argument, so it is handed over here rather than assumed.
static size_t s_saveStateBudget = LIBRETRO_SAVESTATE_MIN_SIZE;

// Generous bound on how many times retro_serialize()/retro_unserialize()
// will resume the emu thread while waiting for a request to finish, before
// giving up and reporting failure rather than hanging the frontend forever.
static const int LIBRETRO_SAVESTATE_MAX_SWITCHES = 600;

// Frames to keep re-asking an engine that answered "not right now" before
// giving up on the request. canSave/canLoadGameStateCurrently() report whether
// this instant is safe, not whether the operation is ever possible: Riven
// refuses while _scriptMan->hasQueuedScripts() is true, so any animation on
// screen blocks it, and SCI refuses while _gamestate->executionStackBase is
// non-zero, which covers most of a game's startup. Both clear on their own a
// short time later. Long enough for those to pass, short enough that a
// genuine refusal does not freeze the frontend for long.
static const int LIBRETRO_SAVESTATE_MAX_REFUSALS = 180;
static int s_saveOpRefusals = 0;

enum LibretroSaveOp {
	LIBRETRO_SAVEOP_NONE,
	LIBRETRO_SAVEOP_SAVE,
	LIBRETRO_SAVEOP_LOAD
};

static LibretroSaveOp s_pendingSaveOp = LIBRETRO_SAVEOP_NONE;
static bool s_saveOpArmed = false;
static bool s_saveOpSucceeded = false;
static Common::Array<byte> s_saveStateBytes;

// Save-state payload container.
//
// Originally the payload was just the reserved slot's save file, so a state
// carried exactly one save: restore it on another device and the game resumed
// at the right point with an empty ScummVM save list, because the player's own
// F5 saves live in the frontend's persistent storage and never travel. A
// normal emulator's state is a RAM dump that inherently contains the in-game
// save; this bridge proxies to an engine save instead, so it has to carry the
// rest deliberately.
//
// Layout: magic, entry count, then per entry a name and its bytes. The
// reserved slot is packed first so a state is still restorable when the
// remaining saves do not fit.
//
// A legacy payload is a bare save file, so its first four bytes are that
// file's own header -- nothing guarantees they cannot happen to equal the
// magic. Detection therefore does not rest on that: a payload whose magic
// matches but whose structure does not parse is treated as legacy rather
// than rejected, so a collision costs nothing.
static const uint32 LIBRETRO_SAVESTATE_MAGIC = MKTAG('S', 'V', 'M', '1');

static void libretro_append_u32(Common::Array<byte> &out, uint32 value) {
	out.push_back((byte)(value & 0xFF));
	out.push_back((byte)((value >> 8) & 0xFF));
	out.push_back((byte)((value >> 16) & 0xFF));
	out.push_back((byte)((value >> 24) & 0xFF));
}

static uint32 libretro_read_u32(const Common::Array<byte> &in, size_t offset) {
	return (uint32)in[offset] | ((uint32)in[offset + 1] << 8) |
	       ((uint32)in[offset + 2] << 16) | ((uint32)in[offset + 3] << 24);
}

static void libretro_append_entry(Common::Array<byte> &out, const Common::String &name, const Common::Array<byte> &data) {
	libretro_append_u32(out, name.size());
	for (uint i = 0; i < name.size(); i++)
		out.push_back((byte)name[i]);
	libretro_append_u32(out, data.size());
	for (uint i = 0; i < data.size(); i++)
		out.push_back(data[i]);
}

static bool libretro_read_save_file(const Common::String &name, Common::Array<byte> &out) {
	Common::InSaveFile *f = g_system->getSavefileManager()->openForLoading(name);
	if (!f)
		return false;
	out.resize(f->size());
	if (out.size())
		f->read(out.data(), out.size());
	delete f;
	return true;
}

// Packs the reserved slot plus every other save belonging to the running
// target. scummvm.ini lives in this same directory under Emscripten (it is the
// only persistent path the frontend offers), and is excluded deliberately: it
// is global, machine-specific configuration, not this game's state, and
// restoring it elsewhere would rewrite that device's other targets.
static void libretro_pack_savestate(Common::Array<byte> &out) {
	const Common::String reservedName = g_engine->getSaveStateName(LIBRETRO_SAVESTATE_SLOT);

	Common::Array<byte> reserved;
	if (!libretro_read_save_file(reservedName, reserved))
		return;

	Common::Array<byte> entries;
	uint32 count = 1;
	libretro_append_entry(entries, reservedName, reserved);

	const Common::String target = ConfMan.getActiveDomainName();
	const Common::String configName = g_system->getDefaultConfigFileName().baseName();
	if (!target.empty()) {
		Common::StringArray names = g_system->getSavefileManager()->listSavefiles(target + ".*");
		for (uint i = 0; i < names.size(); i++) {
			if (names[i] == reservedName || names[i] == configName)
				continue;

			Common::Array<byte> data;
			if (!libretro_read_save_file(names[i], data))
				continue;

			// Header, this entry, and the outer length prefix must all fit.
			const size_t projected = entries.size() + 12 + names[i].size() + data.size() + 4 + sizeof(uint32);
			if (projected > s_saveStateBudget)
				continue;

			libretro_append_entry(entries, names[i], data);
			count++;
		}
	}

	libretro_append_u32(out, LIBRETRO_SAVESTATE_MAGIC);
	libretro_append_u32(out, count);
	for (uint i = 0; i < entries.size(); i++)
		out.push_back(entries[i]);
}

// Writes back every save a packed payload carries. A payload without the magic
// is a legacy single-file state and is written to the reserved slot as before.
// Returns false only if the payload is malformed.
static bool libretro_unpack_savestate(const Common::Array<byte> &in) {
	const Common::String reservedName = g_engine->getSaveStateName(LIBRETRO_SAVESTATE_SLOT);

	Common::Array<Common::String> names;
	Common::Array<Common::Array<byte> > blobs;

	bool parsed = false;
	if (in.size() >= 8 && libretro_read_u32(in, 0) == LIBRETRO_SAVESTATE_MAGIC) {
		const uint32 count = libretro_read_u32(in, 4);
		size_t offset = 8;
		parsed = true;
		for (uint32 i = 0; i < count && parsed; i++) {
			// size_t is 32-bit on wasm32, so every bounds check subtracts from
			// the remaining length rather than adding to the offset: a corrupt
			// or truncated state could otherwise carry a length near UINT32_MAX
			// and wrap the addition past the check into an out-of-bounds read.
			// offset <= in.size() is an invariant -- it only advances after a
			// check -- so the subtractions below cannot underflow.
			if (in.size() - offset < 4) {
				parsed = false;
				break;
			}
			const uint32 nameLen = libretro_read_u32(in, offset);
			offset += 4;
			if (in.size() - offset < nameLen) {
				parsed = false;
				break;
			}
			Common::String name;
			for (uint32 n = 0; n < nameLen; n++)
				name += (char)in[offset + n];
			offset += nameLen;
			if (in.size() - offset < 4) {
				parsed = false;
				break;
			}
			const uint32 dataLen = libretro_read_u32(in, offset);
			offset += 4;
			if (in.size() - offset < dataLen) {
				parsed = false;
				break;
			}
			Common::Array<byte> data;
			data.resize(dataLen);
			for (uint32 d = 0; d < dataLen; d++)
				data[d] = in[offset + d];
			offset += dataLen;
			names.push_back(name);
			blobs.push_back(data);
		}
	}

	// Either it never looked like a container, or it did and did not hold up:
	// a bare save file for the reserved slot is the only other thing it can be.
	if (!parsed) {
		names.clear();
		blobs.clear();
		names.push_back(reservedName);
		blobs.push_back(in);
	}

	for (uint i = 0; i < names.size(); i++) {
		Common::OutSaveFile *f = g_system->getSavefileManager()->openForSaving(names[i]);
		if (!f) {
			// The reserved slot is the one that must land; the rest are a bonus.
			if (names[i] == reservedName)
				return false;
			continue;
		}
		if (blobs[i].size())
			f->write(blobs[i].data(), blobs[i].size());
		f->finalize();
		const bool ok = !f->err();
		delete f;
		if (!ok && names[i] == reservedName)
			return false;
	}
	return true;
}

void retro_process_pending_savestate_op(void) {
	if (s_pendingSaveOp == LIBRETRO_SAVEOP_NONE)
		return;

	if (!g_engine) {
		s_saveOpSucceeded = false;
		s_pendingSaveOp = LIBRETRO_SAVEOP_NONE;
		return;
	}

	// Wait for any save/load already in flight (ours, once armed below; or,
	// in principle, an autosave that happened to be running) to finish
	// before touching the engine's save/load request slot.
	if (g_engine->isSaveOrLoadPending())
		return;

	if (!s_saveOpArmed) {
		// Engines refuse save/load outside the states where it is meaningful,
		// and say so through these two. Ignoring them is not merely impolite:
		// an engine asked to save before it has loaded anything will happily
		// walk into its own uninitialised state. Griffon does exactly that --
		// canSaveGameStateCurrently() is false outside kGameModePlay, and
		// saving anyway at its title screen reaches drawView() with no map
		// loaded and faults. The frontend offers save-state buttons whenever a
		// core is running, so this is reachable by a plain button press.
		//
		// Checked before the load branch writes anything, so a refused load
		// leaves no half-written slot file behind.
		const bool opAllowed = (s_pendingSaveOp == LIBRETRO_SAVEOP_SAVE)
		                       ? g_engine->canSaveGameStateCurrently()
		                       : g_engine->canLoadGameStateCurrently();
		if (!opAllowed) {
			// Leave the request pending and come back next frame: the engine
			// is mid-script or mid-animation, not permanently closed. Only
			// once the window has stayed shut for the whole budget is this a
			// real refusal worth reporting.
			if (++s_saveOpRefusals < LIBRETRO_SAVESTATE_MAX_REFUSALS)
				return;

			if (retro_log_cb)
				retro_log_cb(RETRO_LOG_WARN, "[scummvm] %s refused for %d frames, giving up.\n",
				             s_pendingSaveOp == LIBRETRO_SAVEOP_SAVE ? "Save" : "Load",
				             s_saveOpRefusals);
			retro_osd_notification(s_pendingSaveOp == LIBRETRO_SAVEOP_SAVE
			                       ? "Saving is not available right now"
			                       : "Loading is not available right now");
			s_saveOpSucceeded = false;
			s_pendingSaveOp = LIBRETRO_SAVEOP_NONE;
			return;
		}

		Common::Error err;
		if (s_pendingSaveOp == LIBRETRO_SAVEOP_SAVE) {
			err = g_engine->saveGameState(LIBRETRO_SAVESTATE_SLOT, "libretro savestate");
		} else {
			if (!libretro_unpack_savestate(s_saveStateBytes)) {
				if (retro_log_cb)
					retro_log_cb(RETRO_LOG_ERROR, "[scummvm] Save state payload could not be written to the save directory.\n");
				s_saveOpSucceeded = false;
				s_pendingSaveOp = LIBRETRO_SAVEOP_NONE;
				return;
			}
			err = g_engine->loadGameState(LIBRETRO_SAVESTATE_SLOT);
		}

		if (err.getCode() != Common::kNoError) {
			if (retro_log_cb)
				retro_log_cb(RETRO_LOG_ERROR, "[scummvm] Engine refused the %s: %s\n",
				             s_pendingSaveOp == LIBRETRO_SAVEOP_SAVE ? "save" : "load",
				             err.getDesc().c_str());
			s_saveOpSucceeded = false;
			s_pendingSaveOp = LIBRETRO_SAVEOP_NONE;
			return;
		}

		s_saveOpArmed = true;

		// Deferred engines (e.g. SCUMM) have only armed their internal flag at
		// this point; wait for a later call to see it clear. Engines whose
		// saveGameState()/loadGameState() do the I/O synchronously (the
		// Engine base class default) already report nothing pending -- fall
		// straight through and finish now instead of waiting an extra round trip.
		if (g_engine->isSaveOrLoadPending())
			return;
	}

	if (s_pendingSaveOp == LIBRETRO_SAVEOP_SAVE) {
		s_saveStateBytes.clear();
		libretro_pack_savestate(s_saveStateBytes);
		s_saveOpSucceeded = !s_saveStateBytes.empty();
	} else {
		// loadGameState() reporting kNoError only means the request was
		// accepted, not that the load actually succeeded (see the comment on
		// Engine::isSaveOrLoadPending()) -- a genuinely corrupt/incompatible
		// save may still surface as an in-engine error dialog rather than an
		// error retro_unserialize() can observe. Treating "request finished"
		// as success here matches how libretro frontends already use this
		// API elsewhere: best-effort, not a strict guarantee.
		s_saveOpSucceeded = true;
	}

	s_saveOpArmed = false;
	s_pendingSaveOp = LIBRETRO_SAVEOP_NONE;
}

// Estimates the buffer the next state needs from the saves the target already
// has, so the frontend allocates and writes roughly what is required instead
// of a flat maximum.
//
// Runs on the main thread. That is safe here for a specific reason: the main
// and emu threads never run concurrently -- retro_switch_to_emu_thread()
// blocks on a condition variable until the emu thread hands control back (see
// libretro-threads.cpp) -- so there is no data race. What would be unsafe is
// re-entering the engine, which is parked at an arbitrary yield point; this
// only reads g_engine's save-state name and goes through the SaveFileManager,
// a backend service, so it never re-enters.
static size_t libretro_estimate_savestate_size(void) {
	// Container header plus the outer length prefix.
	size_t needed = 16;
	size_t largest = 0;

	const Common::String target = ConfMan.getActiveDomainName();
	if (!target.empty()) {
		const Common::String configName = g_system->getDefaultConfigFileName().baseName();
		Common::StringArray names = g_system->getSavefileManager()->listSavefiles(target + ".*");
		for (uint i = 0; i < names.size(); i++) {
			if (names[i] == configName)
				continue;
			Common::InSaveFile *f = g_system->getSavefileManager()->openForLoading(names[i]);
			if (!f)
				continue;
			const size_t entrySize = (size_t)f->size();
			delete f;
			needed += 8 + names[i].size() + entrySize;
			if (entrySize > largest)
				largest = entrySize;
		}
	}

	// Nothing on disk to reason from: fall back to the historical fixed size
	// rather than guess low and fail a save that used to work.
	if (!largest)
		return LIBRETRO_SAVESTATE_UNKNOWN_SIZE;

	// The reserved slot is written fresh by this save and may be larger than
	// anything already on disk, so reserve another save's worth for it.
	needed += largest;

	// Slack: saves grow as a game progresses, and an estimate that comes in
	// short makes retro_serialize() fail outright.
	needed += needed / 4;

	if (needed < LIBRETRO_SAVESTATE_MIN_SIZE)
		needed = LIBRETRO_SAVESTATE_MIN_SIZE;
	if (needed > LIBRETRO_SAVESTATE_MAX_SIZE)
		needed = LIBRETRO_SAVESTATE_MAX_SIZE;
	return needed;
}

size_t retro_serialize_size(void) {
	if (!g_engine)
		return 0;
	return libretro_estimate_savestate_size();
}

bool retro_serialize(void *data, size_t size) {
	if (!g_engine || !data || size < sizeof(uint32))
		return false;

	s_pendingSaveOp = LIBRETRO_SAVEOP_SAVE;
	s_saveOpArmed = false;
	s_saveOpRefusals = 0;
	s_saveOpSucceeded = false;
	s_saveStateBytes.clear();
	// The packer drops extra saves that do not fit; tell it what the frontend
	// actually allocated rather than letting it assume the ceiling.
	s_saveStateBudget = size;

	// Drive the emu thread forward (exactly as retro_run() does once per
	// frame) until retro_process_pending_savestate_op(), called from
	// pollEvent() on the emu thread, finishes the request.
	for (int i = 0; s_pendingSaveOp != LIBRETRO_SAVEOP_NONE && i < LIBRETRO_SAVESTATE_MAX_SWITCHES; i++)
		retro_switch_to_emu_thread();

	if (s_pendingSaveOp != LIBRETRO_SAVEOP_NONE || !s_saveOpSucceeded)
		return false;

	uint32 payloadSize = (uint32)s_saveStateBytes.size();
	if (sizeof(payloadSize) + (size_t)payloadSize > size)
		return false;

	memset(data, 0, size);
	memcpy(data, &payloadSize, sizeof(payloadSize));
	memcpy((byte *)data + sizeof(payloadSize), s_saveStateBytes.data(), payloadSize);
	return true;
}

bool retro_unserialize(const void *data, size_t size) {
	if (!g_engine || !data || size < sizeof(uint32))
		return false;

	uint32 payloadSize;
	memcpy(&payloadSize, data, sizeof(payloadSize));
	if (sizeof(payloadSize) + (size_t)payloadSize > size)
		return false;

	s_saveStateBytes.resize(payloadSize);
	memcpy(s_saveStateBytes.data(), (const byte *)data + sizeof(payloadSize), payloadSize);

	s_pendingSaveOp = LIBRETRO_SAVEOP_LOAD;
	s_saveOpArmed = false;
	s_saveOpRefusals = 0;
	s_saveOpSucceeded = false;

	for (int i = 0; s_pendingSaveOp != LIBRETRO_SAVEOP_NONE && i < LIBRETRO_SAVESTATE_MAX_SWITCHES; i++)
		retro_switch_to_emu_thread();

	return s_pendingSaveOp == LIBRETRO_SAVEOP_NONE && s_saveOpSucceeded;
}
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned unused, bool unused1, const char *unused2) {}

unsigned retro_get_region(void) {
	return RETRO_REGION_NTSC;
}
