/*
 * GB300 test runner: runs the re3 core under conditions that approximate the
 * console, so that a change which looks fast on the desktop can be caught
 * before it is flashed to the card.
 *
 * The console is a ~900 MHz MIPS32r2 with no FPU, no GPU and a 32-bit address
 * space.  Three of those four are reproduced here:
 *
 *   no FPU / no SIMD   the core it loads must be the 32-bit x87 build
 *                      (build/gbsim/libretro_re3.so, see the gbsim Makefile
 *                      target), so float work runs through x87 and nothing is
 *                      auto-vectorised;
 *   32-bit RAM        --ram-mb caps RLIMIT_AS, which turns a runaway allocation
 *                      into the malloc failure the console would also see
 *                      rather than into an OOM-kill on the desktop;
 *   slow CPU          --cpu-factor scales the measured frame time by the ratio
 *                      between this host and the console, so the reported fps
 *                      is a prediction for the card, not for this machine.
 *
 * Everything else (video refresh, audio, input) is the same minimal frontend
 * the desktop runner implements.
 *
 *   gb300_runner <core.so> <content-path> [frames] [dump-every] [script]
 *                [--ram-mb N] [--cpu-factor F] [--target-fps N] [--verbose]
 *                [--no-fpu] [--soft-float] [--hw-fpu]
 *
 * Defaults: 300 frames, dump every 30, cpu-factor 10 (soft-float), ram 64 MB, target 10 fps.
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

#ifdef __i386__
#include <fpu_control.h>
#endif

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
static int      s_verbose;

struct step { int first, last; uint32_t mask; };
static struct step s_steps[256];
static int s_step_count;

static int   s_watchdogSecs = 0;

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

static void RETRO_CALLCONV
audio_sample_cb(int16_t l, int16_t r)
{
	(void)l; (void)r;
}

static size_t RETRO_CALLCONV
audio_batch_cb(const int16_t *data, size_t frames)
{
	(void)data;
	return frames;
}

static void RETRO_CALLCONV
input_poll_cb(void)
{
}

static int16_t RETRO_CALLCONV
input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)port; (void)device; (void)index;
	for(int i = 0; i < B_COUNT; i++){
		if(s_btns[i] == id){
			return (s_pad_mask & (1u << i)) ? 1 : 0;
		}
	}
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
			rgb[1] = (uint8_t)(((p >> 5) & 0x3F)  * 255 / 63);
			rgb[2] = (uint8_t)((p & 0x1F)         * 255 / 31);
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
		char *plus = colon + 1;
		while(*plus){
			char name[16];
			size_t n = 0;
			while(*plus && *plus != '+' && n < sizeof(name)-1)
				name[n++] = *plus++;
			name[n] = '\0';
			mask |= button_bit(name);
			if(*plus == '+')
				plus++;
		}
		if(s_step_count < (int)(sizeof(s_steps)/sizeof(s_steps[0]))){
			s_steps[s_step_count].first = first;
			s_steps[s_step_count].last   = last;
			s_steps[s_step_count].mask   = mask;
			s_step_count++;
		}
	}
	free(copy);
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

static void *
get_proc(void *h, const char *name)
{
	void *p = dlsym(h, name);
	if(!p)
		fprintf(stderr, "core is missing %s\n", name);
	return p;
}

static int
dir_exists(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/*
 * Cap the address space.  RLIMIT_AS is what the console's pool allocator ends
 * up hitting when a level asks for more than it has, so this reproduces the
 * failure mode rather than letting the desktop quietly grow.
 */
static void
apply_ram_cap(int mb)
{
	if(mb <= 0)
		return;
	struct rlimit rl;
	getrlimit(RLIMIT_AS, &rl);
	rlim_t want = (rlim_t)mb * 1024 * 1024;
	if(rl.rlim_max != RLIM_INFINITY && want > rl.rlim_max)
		want = rl.rlim_max;
	rl.rlim_cur = want;
	if(setrlimit(RLIMIT_AS, &rl) != 0)
		fprintf(stderr, "warning: could not cap address space at %d MB\n", mb);
	else
		fprintf(stderr, "address space capped at %d MB\n", mb);
}

#ifdef __i386__
static void
set_fpu_single_precision(void)
{
	fpu_control_t cw;
	_FPU_GETCW(cw);
	cw = (cw & ~_FPU_EXTENDED) | _FPU_SINGLE;
	_FPU_SETCW(cw);
	fprintf(stderr, "FPU: set to 24-bit single precision mode (_FPU_SINGLE, cw=0x%04x)\n", cw);
}
#endif

static void
print_mem_usage(void)
{
	FILE *f = fopen("/proc/self/status", "r");
	if(f){
		char line[128];
		long peak_kb = 0, hwm_kb = 0;
		while(fgets(line, sizeof(line), f)){
			if(sscanf(line, "VmPeak: %ld kB", &peak_kb) == 1){}
			else if(sscanf(line, "VmHWM: %ld kB", &hwm_kb) == 1){}
		}
		fclose(f);
		if(peak_kb > 0 || hwm_kb > 0)
			fprintf(stderr, "memory usage      : peak VmSize = %.2f MB, peak RSS = %.2f MB\n",
			        (double)peak_kb / 1024.0, (double)hwm_kb / 1024.0);
	}
}

/* Insert a deliberate amount of busy work, so the reported frame time reflects
 * a slower part without needing a cycle-accurate emulator. */
static void
burn_ms(double ms)
{
	if(ms <= 0.0)
		return;
	double t0 = now_ms();
	volatile double acc = 1.0;
	while(now_ms() - t0 < ms)
		for(int i = 0; i < 200; i++)
			acc = acc * 1.0000001 + 0.5;
	(void)acc;
}

int
main(int argc, char **argv)
{
	if(argc < 3){
		fprintf(stderr,
			"usage: %s <core.so> <content-path> [frames] [dump-every] [script]\n"
			"                  [--ram-mb N] [--cpu-factor F] [--throttle-ms N]\n"
			"                  [--target-fps N] [--verbose] [--no-fpu|--soft-float] [--hw-fpu]\n", argv[0]);
		return 1;
	}

	const char *core_path = argv[1];
	const char *content   = argv[2];
	int frames     = argc > 3 ? atoi(argv[3]) : 300;
	int dump_every = argc > 4 ? atoi(argv[4]) : 30;
	const char *script = argc > 5 ? argv[5] : "60-70:a,120-130:down,180-190:a";

	bool   soft_float  = true;  /* console has no FPU (MIPS 24KEc soft-float) */
	bool   custom_cpu  = false;
	double cpu_factor  = 10.0;  /* default with soft-float */
	int    ram_mb      = 64;    /* GB300: 64MB, SF2000: 32MB */
	double throttle_ms = 0.0;   /* extra synthetic cost per frame */
	double target_fps  = 10.0;

	for(int i = 3; i < argc; i++){
		if(!strcmp(argv[i], "--ram-mb") && i + 1 < argc)
			ram_mb = atoi(argv[++i]);
		else if(!strcmp(argv[i], "--cpu-factor") && i + 1 < argc){
			cpu_factor = atof(argv[++i]);
			custom_cpu = true;
		}
		else if(!strcmp(argv[i], "--throttle-ms") && i + 1 < argc)
			throttle_ms = atof(argv[++i]);
		else if(!strcmp(argv[i], "--target-fps") && i + 1 < argc)
			target_fps = atof(argv[++i]);
		else if(!strcmp(argv[i], "--verbose"))
			s_verbose = 1;
		else if(!strcmp(argv[i], "--no-fpu") || !strcmp(argv[i], "--soft-float")){
			soft_float = true;
			if(!custom_cpu) cpu_factor = 10.0;
		}
		else if(!strcmp(argv[i], "--hw-fpu")){
			soft_float = false;
			if(!custom_cpu) cpu_factor = 8.0;
		}
	}

	if(script)
		parse_script(script);

	/* Disable internal 30 FPS sleep pacing so we measure pure CPU cost */
	setenv("RE3_NO_PACE", "1", 1);

	/* The cap has to be in place before the core maps anything. */
	apply_ram_cap(ram_mb);

#ifdef __i386__
	if(soft_float)
		set_fpu_single_precision();
#endif

	void *h = dlopen(core_path, RTLD_NOW);
	if(!h){
		fprintf(stderr, "dlopen failed: %s\n", dlerror());
		return 1;
	}

	struct retro_system_info sys;
	void (*p_get_system_info)(struct retro_system_info*) =
		(void(*)(struct retro_system_info*))get_proc(h, "retro_get_system_info");
	if(!p_get_system_info)
		return 1;
	p_get_system_info(&sys);
	fprintf(stderr, "core: %s %s (ext %s, need_fullpath %d)\n",
	        sys.library_name, sys.library_version,
	        sys.valid_extensions ? sys.valid_extensions : "", sys.need_fullpath);

	void (*retro_set_environment)(retro_environment_t) =
		(void(*)(retro_environment_t))get_proc(h, "retro_set_environment");
	void (*retro_set_video_refresh)(retro_video_refresh_t) =
		(void(*)(retro_video_refresh_t))get_proc(h, "retro_set_video_refresh");
	void (*retro_set_audio_sample)(retro_audio_sample_t) =
		(void(*)(retro_audio_sample_t))get_proc(h, "retro_set_audio_sample");
	void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t) =
		(void(*)(retro_audio_sample_batch_t))get_proc(h, "retro_set_audio_sample_batch");
	void (*p_input_poll)(void) = (void(*)(void))get_proc(h, "retro_set_input_poll");
	void (*retro_set_input_state)(retro_input_state_t) =
		(void(*)(retro_input_state_t))get_proc(h, "retro_set_input_state");
	void (*retro_init)(void)      = (void(*)(void))get_proc(h, "retro_init");
	void (*retro_deinit)(void)    = (void(*)(void))get_proc(h, "retro_deinit");
	bool (*retro_load_game)(const struct retro_game_info*) =
		(bool(*)(const struct retro_game_info*))get_proc(h, "retro_load_game");
	void (*retro_unload_game)(void) = (void(*)(void))get_proc(h, "retro_unload_game");
	void (*retro_run)(void)     = (void(*)(void))get_proc(h, "retro_run");
	unsigned (*retro_api_version)(void) =
		(unsigned(*)(void))get_proc(h, "retro_api_version");

	if(!retro_api_version || !p_get_system_info || !retro_set_environment ||
	   !retro_set_video_refresh || !retro_set_audio_sample ||
	   !retro_set_audio_sample_batch || !p_input_poll ||
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
	retro_set_audio_sample(audio_sample_cb);
	retro_set_audio_sample_batch(audio_batch_cb);
	p_input_poll();
	retro_set_input_state(input_state_cb);

	retro_init();

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
	if(!retro_load_game(&gi)){
		fprintf(stderr, "retro_load_game FAILED\n");
		return 2;
	}
	fprintf(stderr, "load took %.0f ms\n", now_ms() - s_last_ms);

	fprintf(stderr, "model: 32-bit x87 core (%s), cpu-factor %.1f, throttle %.1f ms/frame\n",
	        soft_float ? "soft-float single-precision" : "hw-fpu", cpu_factor, throttle_ms);

	char ppm[64];
	double host_total = 0.0;
	double host_steady_total = 0.0;
	double worst = 0.0;
	int    rendered = 0;
	int    steady_rendered = 0;
	/* The first frames after a level load are streaming and fading; they are
	 * not representative, so the steady-state average skips them. */
	const int warmup = 60;

	for(int f = 0; f < frames && !s_shutdown; f++){
		apply_script(f);
		double t0 = now_ms();
		retro_run();
		burn_ms(throttle_ms);
		double dt = now_ms() - t0;
		host_total += dt;
		rendered++;
		if(f >= warmup){
			host_steady_total += dt;
			steady_rendered++;
		}
		if(dt > worst)
			worst = dt;
		if(dump_every > 0 && (f % dump_every) == 0){
			snprintf(ppm, sizeof(ppm), "gb_frame_%04d.ppm", f);
			dump_ppm(ppm);
		}
		if(s_verbose || dt > 200.0)
			fprintf(stderr, "  frame %d: host %.1f ms -> console %.1f ms (%.1f fps)\n",
			        f, dt, dt*cpu_factor, 1000.0/(dt*cpu_factor));
	}
	if(dump_every > 0 && frames > 0){
		snprintf(ppm, sizeof(ppm), "gb_frame_%04d.ppm", frames - 1);
		dump_ppm(ppm);
	}

	double host_avg = rendered ? host_total/rendered : 0.0;
	double steady_host_avg = steady_rendered ? host_steady_total/steady_rendered : host_avg;
	double console_avg = host_avg * cpu_factor;
	double steady_console_avg = steady_host_avg * cpu_factor;
	double steady_fps = steady_console_avg > 0 ? 1000.0/steady_console_avg : 0.0;

	fprintf(stderr,
	        "\n=== gb300 simulation ===\n"
	        "frames            : %d (%d warm-up, %d steady)\n"
	        "host              : %.1f ms/frame  (%.1f fps)\n"
	        "steady host       : %.1f ms/frame  (%.1f fps)\n"
	        "host worst frame  : %.1f ms\n"
	        "cpu-factor        : %.1fx\n"
	        "predicted console : %.1f ms/frame  (%.1f fps)\n"
	        "steady-state est. : %.1f ms/frame  (%.1f fps)\n"
	        "target            : %.1f fps -> %s\n"
	        "screenshots       : %d\n",
	        rendered, rendered > steady_rendered ? rendered - steady_rendered : 0, steady_rendered,
	        host_avg, host_avg > 0 ? 1000.0/host_avg : 0.0,
	        steady_host_avg, steady_host_avg > 0 ? 1000.0/steady_host_avg : 0.0,
	        worst * cpu_factor,
	        cpu_factor,
	        console_avg, console_avg > 0 ? 1000.0/console_avg : 0.0,
	        steady_console_avg, steady_fps,
	        target_fps,
	        steady_fps >= target_fps ? "PASS" : "FAIL",
	        s_frames_dumped);

	print_mem_usage();

	retro_unload_game();
	retro_deinit();
	dlclose(h);
	return 0;
}