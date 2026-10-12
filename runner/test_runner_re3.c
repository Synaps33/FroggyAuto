/*
 * Headless libretro runner for the re3 SF2000/GB300 core.
 *
 * Loads the x86-64 build of the core, feeds it a scripted joypad timeline and
 * writes PPM screenshots, which is the only way to check what the software
 * rasteriser actually produces without the console in the loop.
 *
 *   test_runner_re3 <core.so> <content-path> [frames] [dump-every] [script]
 *
 * content-path may be the GTA III folder itself or any file inside it.
 * script is "first-last:button[+button],..." where button is one of
 *   up down left right a b x y l r start select l2 r2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>

#include "libretro.h"

#define MAX_W 512
#define MAX_H 512

enum {
	B_UP = 0, B_DOWN, B_LEFT, B_RIGHT,
	B_A, B_B, B_X, B_Y, B_L, B_R,
	B_START, B_SELECT, B_L2, B_R2,
	B_COUNT
};

static const uint16_t s_btns[B_COUNT] = {
	RETRO_DEVICE_ID_JOYPAD_UP,
	RETRO_DEVICE_ID_JOYPAD_DOWN,
	RETRO_DEVICE_ID_JOYPAD_LEFT,
	RETRO_DEVICE_ID_JOYPAD_RIGHT,
	RETRO_DEVICE_ID_JOYPAD_A,
	RETRO_DEVICE_ID_JOYPAD_B,
	RETRO_DEVICE_ID_JOYPAD_X,
	RETRO_DEVICE_ID_JOYPAD_Y,
	RETRO_DEVICE_ID_JOYPAD_L,
	RETRO_DEVICE_ID_JOYPAD_R,
	RETRO_DEVICE_ID_JOYPAD_START,
	RETRO_DEVICE_ID_JOYPAD_SELECT,
	RETRO_DEVICE_ID_JOYPAD_L2,
	RETRO_DEVICE_ID_JOYPAD_R2,
};

static int      s_w = 256, s_h = 192;
static uint16_t s_frame[MAX_W*MAX_H];
static int      s_frames_dumped;
static unsigned s_shutdown;
static uint32_t s_pad_mask;
static double   s_last_ms;

/* ---- scripted joypad timeline ---- */
struct step { int first, last; uint32_t mask; };
static struct step s_steps[256];
static int s_step_count;

/*
 * If a frame does not finish in this many seconds something is looping inside
 * the engine, and a stack trace is the only practical way to find out where.
 */
static int s_watchdogSecs = 0;

static void
watchdog(int sig)
{
	(void)sig;
	static const char msg[] = "\n*** frame watchdog fired, backtrace follows ***\n";
	ssize_t ignored = write(2, msg, sizeof(msg)-1);
	(void)ignored;
	void *frames[64];
	int n = backtrace(frames, 64);
	backtrace_symbols_fd(frames, n, 2);
	_exit(3);
}

static void
armWatchdog(int secs)
{
	s_watchdogSecs = secs;
	if(secs <= 0){
		alarm(0);
		return;
	}
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = watchdog;
	sigaction(SIGALRM, &sa, NULL);
	alarm((unsigned)secs);
}

static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec*1000.0 + (double)ts.tv_nsec/1000000.0;
}

static retro_log_printf_t s_log_cb;

static void RETRO_CALLCONV
log_cb(enum retro_log_level level, const char *fmt, ...)
{
	va_list ap;
	(void)level;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static void
video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
	if(!data)
		return;
	if(width > MAX_W)  width = MAX_W;
	if(height > MAX_H) height = MAX_H;
	for(unsigned y = 0; y < height; y++){
		const uint8_t *src = (const uint8_t*)data + y*pitch;
		uint16_t *dst = &s_frame[y*MAX_W];
		for(unsigned x = 0; x < width; x++)
			dst[x] = ((const uint16_t*)src)[x];
	}
	s_w = width;
	s_h = height;
}

static size_t
audio_batch_cb(const int16_t *data, size_t frames)
{
	(void)data;
	return frames;
}

static void
audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }

static void
input_poll_cb(void) { }

static int16_t
input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)port; (void)device; (void)index;
	for(int i = 0; i < B_COUNT; i++)
		if(s_btns[i] == id)
			return (s_pad_mask & (1u << i)) ? 1 : 0;
	return 0;
}

static void
dump_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	if(!f){
		fprintf(stderr, "cannot write %s\n", path);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", s_w, s_h);
	for(int y = 0; y < s_h; y++){
		for(int x = 0; x < s_w; x++){
			uint16_t p = s_frame[y*MAX_W + x];
			uint8_t rgb[3];
			rgb[0] = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
			rgb[1] = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
			rgb[2] = (uint8_t)((p & 0x1F) * 255 / 31);
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
	s_frames_dumped++;
	fprintf(stderr, "  wrote %s (%dx%d)\n", path, s_w, s_h);
}

static uint32_t
button_bit(const char *name)
{
	for(int i = 0; i < B_COUNT; i++)
		if(strcmp(name, (const char*[]){ "up","down","left","right","a","b","x","y",
		                                 "l","r","start","select","l2","r2" }[i]) == 0)
			return 1u << i;
	fprintf(stderr, "unknown button '%s'\n", name);
	return 0;
}

static void
parse_script(const char *script)
{
	s_step_count = 0;
	char *copy = strdup(script);
	char *save1 = NULL;
	for(char *tok = strtok_r(copy, ",", &save1); tok; tok = strtok_r(NULL, ",", &save1)){
		char *colon = strchr(tok, ':');
		if(!colon)
			continue;
		*colon = '\0';
		int first = 0, last = 0;
		if(sscanf(tok, "%d-%d", &first, &last) == 2){
			/* ok */
		}else if(sscanf(tok, "%d", &first) == 1){
			last = first;
		}else{
			continue;
		}
		uint32_t mask = 0;
		char *save2 = NULL;
		for(char *b = strtok_r(colon+1, "+", &save2); b; b = strtok_r(NULL, "+", &save2))
			mask |= button_bit(b);
		if(s_step_count < 256){
			s_steps[s_step_count].first = first;
			s_steps[s_step_count].last = last;
			s_steps[s_step_count].mask = mask;
			s_step_count++;
		}
	}
	free(copy);
	fprintf(stderr, "script: %d step(s)\n", s_step_count);
}

static void
apply_script(int frame)
{
	uint32_t mask = 0;
	for(int i = 0; i < s_step_count; i++)
		if(frame >= s_steps[i].first && frame <= s_steps[i].last)
			mask |= s_steps[i].mask;
	s_pad_mask = mask;
}

/*
 * The runner is the "frontend": it answers the handful of environment calls the
 * core makes.  Unknown commands return false, which is what a real frontend
 * does too.
 */
static bool RETRO_CALLCONV
environment_cb(unsigned cmd, void *data)
{
	switch(cmd){
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		return *(enum retro_pixel_format*)data == RETRO_PIXEL_FORMAT_RGB565;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		*(retro_log_printf_t*)data = log_cb;
		return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned*)data = 1;
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
	case RETRO_ENVIRONMENT_GET_VARIABLE:
	case RETRO_ENVIRONMENT_SET_VARIABLES:
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		/* Let the core keep its compiled-in defaults. */
		if(cmd == RETRO_ENVIRONMENT_GET_VARIABLE){
			struct retro_variable *v = (struct retro_variable*)data;
			if(v) v->value = NULL;
			return false;
		}
		if(cmd == RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE){
			*(bool*)data = false;
			return true;
		}
		return true;
	default:
		return false;
	}
}

static bool
dir_exists(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int
main(int argc, char **argv)
{
	if(argc < 3){
		fprintf(stderr,
			"usage: %s <core.so> <content-path> [frames] [dump-every] [script] [watchdog-secs]\n",
			argv[0]);
		return 1;
	}
	const char *core_path = argv[1];
	const char *content   = argv[2];
	int frames      = argc > 3 ? atoi(argv[3]) : 300;
	int dump_every  = argc > 4 ? atoi(argv[4]) : 30;
	const char *script = argc > 5 ? argv[5] : "60-70:a,120-130:down,180-190:a";
	int watchdog_secs  = argc > 6 ? atoi(argv[6]) : 60;

	if(script)
		parse_script(script);

	void *h = dlopen(core_path, RTLD_NOW);
	if(!h){
		fprintf(stderr, "dlopen failed: %s\n", dlerror());
		return 1;
	}

	retro_get_proc_address_t get_proc =
		(retro_get_proc_address_t)dlsym(h, "retro_get_proc_address");
	if(!get_proc){
		fprintf(stderr, "retro_get_proc_address missing\n");
		return 1;
	}

	struct retro_system_info sys;
	void (*p_get_system_info)(struct retro_system_info*) =
		(void(*)(struct retro_system_info*))get_proc("retro_get_system_info");
	if(!p_get_system_info){
		fprintf(stderr, "retro_get_system_info missing\n");
		return 1;
	}
	p_get_system_info(&sys);
	fprintf(stderr, "core: %s %s (ext %s, need_fullpath %d)\n",
	        sys.library_name, sys.library_version,
	        sys.valid_extensions ? sys.valid_extensions : "", sys.need_fullpath);

	void (*retro_set_environment)(retro_environment_t) =
		(void(*)(retro_environment_t))get_proc("retro_set_environment");
	void (*retro_set_video_refresh)(retro_video_refresh_t) =
		(void(*)(retro_video_refresh_t))get_proc("retro_set_video_refresh");
	void (*retro_set_audio_sample)(retro_audio_sample_t) =
		(void(*)(retro_audio_sample_t))get_proc("retro_set_audio_sample");
	void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t) =
		(void(*)(retro_audio_sample_batch_t))get_proc("retro_set_audio_sample_batch");
	void (*retro_set_input_poll)(retro_input_poll_t) =
		(void(*)(retro_input_poll_t))get_proc("retro_set_input_poll");
	void (*retro_set_input_state)(retro_input_state_t) =
		(void(*)(retro_input_state_t))get_proc("retro_set_input_state");
	void (*retro_init)(void)      = (void(*)(void))get_proc("retro_init");
	void (*retro_deinit)(void)    = (void(*)(void))get_proc("retro_deinit");
	bool (*retro_load_game)(const struct retro_game_info*) =
		(bool(*)(const struct retro_game_info*))get_proc("retro_load_game");
	void (*retro_unload_game)(void) = (void(*)(void))get_proc("retro_unload_game");
	void (*retro_run)(void)     = (void(*)(void))get_proc("retro_run");
	unsigned (*retro_api_version)(void) =
		(unsigned(*)(void))get_proc("retro_api_version");

	if(!retro_api_version || !p_get_system_info || !retro_set_environment ||
	   !retro_set_video_refresh || !retro_set_audio_sample ||
	   !retro_set_audio_sample_batch || !retro_set_input_poll ||
	   !retro_set_input_state || !retro_init || !retro_deinit ||
	   !retro_load_game || !retro_unload_game || !retro_run){
		fprintf(stderr, "core is missing one or more mandatory symbols\n");
		return 1;
	}
	if(retro_api_version() != RETRO_API_VERSION){
		fprintf(stderr, "libretro API mismatch: core %u, runner %u\n",
		        retro_api_version(), (unsigned)RETRO_API_VERSION);
		return 1;
	}

	retro_set_environment(environment_cb);
	retro_set_video_refresh(video_cb);
	retro_set_audio_sample(audio_cb);
	retro_set_audio_sample_batch(audio_batch_cb);
	retro_set_input_poll(input_poll_cb);
	retro_set_input_state(input_state_cb);

	retro_init();

	/* hand the core a path inside the content folder */
	char path[512];
	struct stat st;
	if(stat(content, &st) == 0 && S_ISDIR(st.st_mode))
		snprintf(path, sizeof(path), "%s/re3.sf2k", content);
	else
		snprintf(path, sizeof(path), "%s", content);

	struct retro_game_info gi;
	memset(&gi, 0, sizeof(gi));
	gi.path = path;

	fprintf(stderr, "loading content: %s\n", path);
	s_last_ms = now_ms();
	armWatchdog(watchdog_secs);
	if(!retro_load_game(&gi)){
		armWatchdog(0);
		fprintf(stderr, "retro_load_game FAILED\n");
		return 2;
	}
	armWatchdog(0);
	fprintf(stderr, "load took %.0f ms\n", now_ms() - s_last_ms);

	char ppm[64];
	double total = 0.0;
	int rendered = 0;
	for(int f = 0; f < frames && !s_shutdown; f++){
		apply_script(f);
		double t0 = now_ms();
		armWatchdog(watchdog_secs);
		retro_run();
		armWatchdog(0);
		double dt = now_ms() - t0;
		total += dt;
		rendered++;
		if(dump_every > 0 && (f % dump_every) == 0){
			snprintf(ppm, sizeof(ppm), "frame_%04d.ppm", f);
			dump_ppm(ppm);
		}
		if(dt > 200.0 && (f % 10) == 0)
			fprintf(stderr, "  frame %d took %.0f ms\n", f, dt);
	}
	if(dump_every > 0 && frames > 0){
		snprintf(ppm, sizeof(ppm), "frame_%04d.ppm", frames - 1);
		dump_ppm(ppm);
	}

	fprintf(stderr, "\n%d frames, %.1f ms/frame (%.1f fps), %d screenshots\n",
	        rendered, rendered ? total/rendered : 0.0,
	        rendered ? 1000.0/(total/rendered) : 0.0, s_frames_dumped);

	struct rusage ru;
	if(getrusage(RUSAGE_SELF, &ru) == 0)
		fprintf(stderr, "peak RSS: %.2f MB\n", (double)ru.ru_maxrss / 1024.0);

	retro_unload_game();
	retro_deinit();
	dlclose(h);
	return 0;
}
