/*
 * re3 (GTA III) - libretro core for the Data Frog SF2000 / GB300.
 *
 * The host owns the screen, the clock and the joypad.  Everything in between
 * is the re3 engine plus two things this port adds:
 *
 *   src/skel/sf2000/sf2000.cpp        the RenderWare skeleton platform layer
 *   librw/src/soft/*                  a CPU rasteriser (the console has no GPU)
 *
 * so retro_run() is a very thin adapter: read the pad, run one frame of re3's
 * game state machine, hand the RGB565 framebuffer to the host.
 */
#include "libretro.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <stdint.h>

#include "rwcore.h"

typedef uint16_t uint16;
typedef int16_t  int16t_unused_placeholder;

/* From src/skel/sf2000/sf2000.cpp */
extern uint16 *sf2000Framebuffer(int *outWidth, int *outHeight);
extern bool    sf2000FrameReady(void);
extern void    sf2000ClearFrameReady(void);
extern void    sf2000Startup(const char *gameDir);
extern bool    sf2000StepFrame(void);
extern void    sf2000PushInput(const bool *buttons);
extern void    sf2000ReleaseAllInput(void);
extern int     sf2000ScreenWidth;
extern int     gta3MaxTextureSize;
extern int     sf2000ScreenHeight;
extern int     sf2000Language;
extern bool    sf2000ShowFps;

/* instrumentation from librw's soft backend */
namespace rw { namespace soft {
	extern int32 softTrianglesDrawn;
	extern int32 softTrianglesCulled;
} }

#define CORE_NAME    "re3 (GTA III)"
#define CORE_VERSION "0.1"
#define CORE_EXTS    "sf2k|g3|cgf"

#define FRAME_FPS      30.0
#define AUDIO_RATE     22050.0
#define AUDIO_FRAMES   735			/* 1/30 s at 22050 Hz */

static retro_environment_t        s_environ_cb;
static retro_video_refresh_t      s_video_cb;
static retro_audio_sample_t       s_audio_cb;
static retro_audio_sample_batch_t s_audio_batch_cb;
static retro_input_poll_t         s_input_poll_cb;
static retro_input_state_t        s_input_state_cb;

static bool  s_loaded;
static char  s_content_dir[512];
static bool  s_shutdown_requested;
static int16_t s_audio_buffer[AUDIO_FRAMES*2];
static double s_frame_ms_accum;

/* ---------------------------------------------------------- logging ----- */

static retro_log_printf_t s_log_cb;

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if(s_log_cb)
		s_log_cb(level, "%s", buf);
	else
		fprintf(stderr, "[re3] %s\n", buf);
}

static void grab_log_interface(void)
{
	if(s_environ_cb)
		s_environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &s_log_cb);
}

/* ------------------------------------------------------------ options --- */

static struct retro_core_option_definition option_defs[] = {
	{
		"re3_internal_resolution",
		"Internal resolution",
		"re3 renders in software, so this is the single biggest performance "
		"and memory knob.  The panel is 320x240.",
		{
			{ "160x120", "re3_res_160x120" },
			{ "208x156", "re3_res_208x156" },
			{ "240x180", "re3_res_240x180" },
			{ "256x192", "re3_res_256x192" },
			{ "320x240", "re3_res_320x240" },
			{ NULL, NULL },
		},
		"re3_res_256x192",
	},
	{
		"re3_language",
		"Language",
		NULL,
		{
			{ "English",  "English" },
			{ "French",   "French" },
			{ "German",   "German" },
			{ "Italian",  "Italian" },
			{ "Spanish",  "Spanish" },
			{ "Polish",   "Polish" },
			{ "Russian",  "Russian" },
			{ "Japanese", "Japanese" },
			{ NULL, NULL },
		},
		"English",
	},
	{
		"re3_texture_quality",
		"Texture resolution",
		"Textures are scaled down on load.  The console has no GPU and ~112 MB of "
		"RAM shared with the firmware, so 64 is the sane setting; 0 keeps the "
		"original size (desktop only).",
		{
			{ "Off (full size)", "re3_tex_0" },
			{ "32 px", "re3_tex_32" },
			{ "64 px", "re3_tex_64" },
			{ "128 px", "re3_tex_128" },
			{ "256 px", "re3_tex_256" },
			{ NULL, NULL },
		},
		"re3_tex_64",
	},
	{
		"re3_show_fps",
		"Show FPS overlay",
		NULL,
		{
			{ "On",  "on" },
			{ "Off", "off" },
			{ NULL, NULL },
		},
		"off",
	},
	{ NULL, NULL, NULL, {{0}}, NULL },
};

static void check_options(void)
{
	struct retro_variable var;

	var.key = "re3_internal_resolution";
	var.value = NULL;
	if(s_environ_cb && s_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value){
		int w = 0, h = 0;
		if(sscanf(var.value, "%dx%d", &w, &h) == 2 && w > 0 && h > 0){
			sf2000ScreenWidth = w;
			sf2000ScreenHeight = h;
		}
	}

	var.key = "re3_language";
	var.value = NULL;
	if(s_environ_cb && s_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value){
		static const char *names[] = { "English", "French", "German", "Italian",
		                               "Spanish", "Polish", "Russian", "Japanese" };
		for(int i = 0; i < 8; i++)
			if(strcmp(var.value, names[i]) == 0){ sf2000Language = i; break; }
	}

	var.key = "re3_texture_quality";
	var.value = NULL;
	if(s_environ_cb && s_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value){
		int v = atoi(var.value + strlen("re3_tex_"));
		gta3MaxTextureSize = v;
	}

	var.key = "re3_show_fps";
	var.value = NULL;
	if(s_environ_cb && s_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		sf2000ShowFps = strcmp(var.value, "on") == 0;
}

/* -------------------------------------------------------------- input --- */

enum {
	BTN_UP = 0, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
	BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R,
	BTN_START, BTN_SELECT, BTN_L2, BTN_R2,
	BTN_STICK_UP, BTN_STICK_DOWN, BTN_STICK_LEFT, BTN_STICK_RIGHT,
	BTN_COUNT
};

/* The console's A/B/X/Y are the four face buttons; which one is "confirm"
 * depends on the shell's own menu, so keep A as confirm here too (that is what
 * every other SF2000 core does). */
static const uint16 s_bindings[BTN_COUNT] = {
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
	/* The thumb stick only reports 8 directions, so treat the four
	 * cardinal ones as a second d-pad for the camera. */
	RETRO_DEVICE_ID_JOYPAD_UP,
	RETRO_DEVICE_ID_JOYPAD_DOWN,
	RETRO_DEVICE_ID_JOYPAD_LEFT,
	RETRO_DEVICE_ID_JOYPAD_RIGHT,
};

static bool s_prev_buttons[BTN_COUNT];

static bool button_down(uint16 id)
{
	if(s_input_state_cb == NULL)
		return false;
	return s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id) != 0;
}

static void poll_input(void)
{
	bool buttons[BTN_COUNT];

	if(s_input_poll_cb)
		s_input_poll_cb();

	for(int i = 0; i < BTN_COUNT; i++)
		buttons[i] = button_down(s_bindings[i]);

	sf2000PushInput(buttons);
	for(int i = 0; i < BTN_COUNT; i++)
		s_prev_buttons[i] = buttons[i];
}

/* ------------------------------------------------------ content lookup -- */

static bool file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool dir_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* re3 opens everything relative to the current directory, so all this has to
 * do is point at the folder that holds data/, models/, TEXT/ and the .txd
 * files.  models/menu.txd is the cheapest thing that proves a real GTA III
 * installation is there, so it is what the check looks for; data/main.cgf is
 * only needed once a level actually loads. */
static bool resolve_content_dir(const struct retro_game_info *info)
{
	char candidate[512];
	const char *path = info ? info->path : NULL;

	if(path && path[0]){
		/* Strip the file name: the frontend always hands us a file. */
		strncpy(candidate, path, sizeof(candidate)-1);
		candidate[sizeof(candidate)-1] = '\0';
		char *slash = strrchr(candidate, '/');
		if(slash){
			*slash = '\0';
			strncpy(s_content_dir, candidate, sizeof(s_content_dir)-1);
			s_content_dir[sizeof(s_content_dir)-1] = '\0';
		}else{
			s_content_dir[0] = '\0';
		}
		if(s_content_dir[0] && dir_exists(s_content_dir)){
			char probe[600];
			snprintf(probe, sizeof(probe), "%s/models/menu.txd", s_content_dir);
			if(file_exists(probe))
				return true;
			/* Some layouts keep the game one level down. */
			snprintf(probe, sizeof(probe), "%s/re3/models/menu.txd", s_content_dir);
			if(file_exists(probe)){
				char inner[600];
				snprintf(inner, sizeof(inner), "%s/re3", s_content_dir);
				strncpy(s_content_dir, inner, sizeof(s_content_dir)-1);
				s_content_dir[sizeof(s_content_dir)-1] = '\0';
				return true;
			}
		}
	}

	/* Ask the host where content lives. */
	const char *dir = NULL;
	if(s_environ_cb && s_environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir){
		strncpy(s_content_dir, dir, sizeof(s_content_dir)-1);
		s_content_dir[sizeof(s_content_dir)-1] = '\0';
		size_t l = strlen(s_content_dir);
		if(l && s_content_dir[l-1] == '/')
			s_content_dir[l-1] = '\0';
		char probe[600];
		snprintf(probe, sizeof(probe), "%s/models/menu.txd", s_content_dir);
		if(file_exists(probe))
			return true;
	}

	/* Last resort: the working directory the multicore started us in. */
	if(file_exists("models/menu.txd")){
		strcpy(s_content_dir, ".");
		return true;
	}
	return false;
}

/* --------------------------------------------------------- libretro API - */

unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof(*info));
	info->library_name     = CORE_NAME;
	info->library_version  = CORE_VERSION;
	info->valid_extensions = CORE_EXTS;
	/* The "ROM" is only a launcher stub; the game is read from disk. */
	info->need_fullpath    = true;
	info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
	memset(info, 0, sizeof(*info));
	info->geometry.base_width   = sf2000ScreenWidth;
	info->geometry.base_height  = sf2000ScreenHeight;
	info->geometry.max_width    = sf2000ScreenWidth;
	info->geometry.max_height   = sf2000ScreenHeight;
	info->geometry.aspect_ratio = 4.0f/3.0f;
	info->timing.fps            = FRAME_FPS;
	info->timing.sample_rate    = AUDIO_RATE;
}

void retro_set_environment(retro_environment_t cb)            { s_environ_cb        = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb)        { s_video_cb          = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)          { s_audio_cb          = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { s_audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb)              { s_input_poll_cb     = cb; }
void retro_set_input_state(retro_input_state_t cb)            { s_input_state_cb    = cb; }

void retro_init(void)
{
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
	sf2000ScreenWidth  = 256;
	sf2000ScreenHeight = 192;
	sf2000Language     = 0;
	sf2000ShowFps      = false;
	gta3MaxTextureSize = 64;
	s_loaded = false;
	s_shutdown_requested = false;
	s_frame_ms_accum = 1000.0/FRAME_FPS;
	for(int i = 0; i < BTN_COUNT; i++)
		s_prev_buttons[i] = false;
	memset(s_audio_buffer, 0, sizeof(s_audio_buffer));
	if(s_environ_cb){
		s_environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
		unsigned ver = 0;
		if(s_environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &ver) && ver >= 2)
			s_environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, option_defs);
		else
			s_environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, option_defs);
	}
	grab_log_interface();
}

void retro_deinit(void)
{
	if(s_loaded){
		sf2000ReleaseAllInput();
		s_loaded = false;
	}
}

bool retro_load_game(const struct retro_game_info *info)
{
	if(!resolve_content_dir(info)){
		core_log(RETRO_LOG_ERROR,
		         "[re3] no GTA III data found next to '%s' (expected models/menu.txd)\n",
		         (info && info->path) ? info->path : "(null)");
		return false;
	}
	core_log(RETRO_LOG_INFO, "[re3] content dir: %s\n", s_content_dir);
	check_options();
	sf2000Startup(s_content_dir);
	s_loaded = true;
	return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
	(void)type; (void)info; (void)num;
	return false;
}

void retro_unload_game(void)
{
	retro_deinit();
}

void retro_reset(void)
{
}

size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{ (void)index; (void)enabled; (void)code; }
unsigned retro_get_region(void) { return 0; }
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }

/*
 * Port device plumbing.  This core has exactly one input type - a gamepad - so
 * every port answers RETRO_DEVICE_JOYPAD and switching to anything else is a
 * no-op.  A frontend is still allowed to call these, so they have to exist.
 */
static unsigned
port_device(unsigned port)
{
	return s_input_state_cb ? port : RETRO_DEVICE_JOYPAD;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
	(void)port; (void)device;
}

unsigned retro_get_controller_port_device(unsigned port)
{
	return port_device(port);
}

unsigned retro_get_input_device_capabilities(unsigned port, unsigned id, unsigned type)
{
	(void)port;
	if(id == 0 && type == RETRO_DEVICE_JOYPAD)
		return (1 << RETRO_DEVICE_ID_JOYPAD_B) | (1 << RETRO_DEVICE_ID_JOYPAD_Y) |
		       (1 << RETRO_DEVICE_ID_JOYPAD_SELECT) | (1 << RETRO_DEVICE_ID_JOYPAD_START) |
		       (1 << RETRO_DEVICE_ID_JOYPAD_UP) | (1 << RETRO_DEVICE_ID_JOYPAD_DOWN) |
		       (1 << RETRO_DEVICE_ID_JOYPAD_LEFT) | (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT) |
		       (1 << RETRO_DEVICE_ID_JOYPAD_L) | (1 << RETRO_DEVICE_ID_JOYPAD_R) |
		       (1 << RETRO_DEVICE_ID_JOYPAD_L2) | (1 << RETRO_DEVICE_ID_JOYPAD_R2) |
		       (1 << RETRO_DEVICE_ID_JOYPAD_L3) | (1 << RETRO_DEVICE_ID_JOYPAD_R3);
	return 0;
}

#ifdef RE3_CONSOLE_SDK
// The console's newlib has no clock_gettime/nanosleep; the SDK's tick counter
// and task delay are the equivalents.
extern "C" uint32_t os_get_tick_count(void);
extern "C" int dly_tsk(unsigned ms);
static double now_ms(void)
{
	return (double)os_get_tick_count();
}
#else
static double now_ms(void)
{
	struct timespec ts;
#ifdef CLOCK_MONOTONIC
	clock_gettime(CLOCK_MONOTONIC, &ts);
#else
	clock_gettime(CLOCK_REALTIME, &ts);
#endif
	return (double)ts.tv_sec*1000.0 + (double)ts.tv_nsec/1000000.0;
}
#endif

/*
 * The console's loop calls retro_run() as fast as it can, so the frame rate is
 * capped here instead.  It is a ceiling, not a target: a host that is already
 * slower is never slowed down further.
 */
static void pace_frame(void)
{
	static double s_next_due;
	static int    s_ready;

	if(!s_ready){
		s_next_due = now_ms();
		s_ready = 1;
	}
	double due = s_next_due + 1000.0/FRAME_FPS;
	for(;;){
		double t = now_ms();
		if(t >= due){
			/* Never try to catch up more than two frames. */
			s_next_due = (due > t - 2000.0/FRAME_FPS) ? due : t;
			return;
		}
		double sleep_ms = due - t;
		if(sleep_ms > 2.0){
#ifdef RE3_CONSOLE_SDK
			dly_tsk((unsigned)(sleep_ms > 100.0 ? 100.0 : sleep_ms));
#else
			struct timespec req;
			req.tv_sec = 0;
			req.tv_nsec = (long)(sleep_ms*1000000.0);
			if(req.tv_nsec > 2000000L)
				req.tv_nsec = 2000000L;
			nanosleep(&req, NULL);
#endif
		}
	}
}

void retro_run(void)
{
	if(!s_loaded)
		return;

	pace_frame();
	poll_input();

	rw::soft::softTrianglesDrawn = 0;
	rw::soft::softTrianglesCulled = 0;

	if(!sf2000StepFrame()){
		s_shutdown_requested = true;
		if(s_environ_cb)
			s_environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
		return;
	}

	if(sf2000FrameReady()){
		int w = 0, h = 0;
		uint16 *fb = sf2000Framebuffer(&w, &h);
		if(s_video_cb && fb)
			s_video_cb(fb, (unsigned)w, (unsigned)h, (size_t)w*sizeof(uint16));
		sf2000ClearFrameReady();
	}

	if(s_audio_batch_cb && s_audio_cb){
		/* Audio arrives through the sampman backend when it is enabled; until
		 * then this is silence at the right rate so the host's buffer keeps
		 * moving. */
		memset(s_audio_buffer, 0, sizeof(s_audio_buffer));
		s_audio_batch_cb(s_audio_buffer, AUDIO_FRAMES);
	}

	if(sf2000ShowFps){
		char buf[64];
		snprintf(buf, sizeof(buf), "%.1f fps  tri %d/%d",
		         1000.0/(1000.0/FRAME_FPS),
		         rw::soft::softTrianglesDrawn, rw::soft::softTrianglesCulled);
		core_log(RETRO_LOG_INFO, "[re3] %s\n", buf);
	}
}

/*
 * Standard libretro entry point for frontends that use symbol lookup rather
 * than dlopen.  Only the symbols a host is allowed to ask for are exported.
 */
extern "C" void *RETRO_CALLCONV retro_get_proc_address(const char *sym)
{
	if(sym == NULL)
		return NULL;
#define SYM(name) if(strcmp(sym, #name) == 0) return (void*)&name;
	SYM(retro_api_version)
	SYM(retro_get_system_info)
	SYM(retro_get_system_av_info)
	SYM(retro_set_environment)
	SYM(retro_set_video_refresh)
	SYM(retro_set_audio_sample)
	SYM(retro_set_audio_sample_batch)
	SYM(retro_set_input_poll)
	SYM(retro_set_input_state)
	SYM(retro_init)
	SYM(retro_deinit)
	SYM(retro_load_game)
	SYM(retro_load_game_special)
	SYM(retro_unload_game)
	SYM(retro_run)
	SYM(retro_reset)
	SYM(retro_serialize_size)
	SYM(retro_serialize)
	SYM(retro_unserialize)
	SYM(retro_cheat_reset)
	SYM(retro_cheat_set)
	SYM(retro_get_region)
	SYM(retro_get_memory_data)
	SYM(retro_get_memory_size)
#undef SYM
	return NULL;
}
