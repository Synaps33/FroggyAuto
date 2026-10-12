/*
 * SF2000 / GB300 platform layer for re3.
 *
 * This replaces src/skel/{glfw,win,symbian} for the Data Frog handhelds.  It is
 * deliberately thin: the host (libretro) owns the screen, the joypad and the
 * clock, so this file only has to
 *
 *   - bring up the parts of the RenderWare skeleton that expect a platform
 *   - translate the console's joypad into the RsKeyCodes that re3's default
 *     control configuration already binds to (see
 *     CControllerConfigManager::InitDefaultControlConfiguration)
 *   - run the same game state machine glfw.cpp runs, one frame per call, so the
 *     host can drive it
 *   - expose the RGB565 framebuffer that librw's "soft" backend renders into
 *
 * There is no mouse and no keyboard on the console, hence CONSOLE_NO_MOUSE.
 */
#include "common.h"

/* fs_sync() is the firmware's own flush-to-flash; its libc has no POSIX
 * fsync().  Declared by the multicore frontend (include/stockfw.h). */
#ifdef RE3_CONSOLE_SDK
extern "C" int fs_sync(const char *path);
#endif

/* The on-screen boot log lives in the libretro layer, but the failure we are
 * chasing happens deep in here, so it is called from here too.  extern "C" is
 * required: the definitions in libretro/hacker_loader.h are C linkage, and a
 * plain extern here would ask for a C++-mangled name that does not exist.
 * sf2000LogInit() is idempotent and hacker_loader_log() is a no-op when no video
 * callback was installed (the native runner), so this is safe on both targets. */
extern "C" {
	void hacker_loader_log(const char *tag, const char *msg, int pct);
	void hacker_loader_stop(void);
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "rwcore.h"
#include "soft/rwsoft.h"
#include "skel/skeleton.h"
#include "skel/platform.h"
#include "skel/events.h"

#include "crossplatform.h"
#include "platform.h"

#include "main.h"
#include "Camera.h"
#include "ControllerConfig.h"
#include "Frontend.h"
#include "Game.h"
#include "Pad.h"
#include "Timer.h"
#include "Text.h"
#include "FileMgr.h"
#include "TxdStore.h"
#include "Directory.h"
#include "Renderer.h"


/* ---------------------------------------------------------- configuration */

int  sf2000ScreenWidth  = 320;
int  sf2000ScreenHeight = 240;
int  sf2000Language     = 0;	/* index into gta3_languages */
bool sf2000ShowFps      = false;

/* ------------------------------------------------------------ video output */

static bool sFrameReady = false;

uint16 *
sf2000Framebuffer(int *outWidth, int *outHeight)
{
	if(outWidth)  *outWidth  = rw::soft::frameBufferWidth;
	if(outHeight) *outHeight = rw::soft::frameBufferHeight;
	return rw::soft::frameBuffer;
}

#ifdef RE3_CONSOLE_SDK
// From the console SDK (include/stockfw.h).  Declared here because the timer
// lives below and a language linkage specification is only valid at namespace
// scope.  This newlib has no clock_gettime() or nanosleep() at all.
extern "C" uint32_t os_get_tick_count(void);
extern "C" int dly_tsk(unsigned ms);
#endif

bool sf2000FrameReady(void) { return sFrameReady; }
void sf2000ClearFrameReady(void) { sFrameReady = false; }

/* --------------------------------------------------------------- timer */

#if defined CLOCK_MONOTONIC && !defined _WIN32 && !defined __SYMBIAN32__
#define RE3_HAVE_CLOCK_GETTIME 1
#endif

// The console's newlib has no clock_gettime(), so fall back to time(), whose
// one-second resolution is enough for the frame pacing and the game's
// time-based logic on a device that runs at 30 fps.
static double
timerNow(void)
{
#ifdef RE3_CONSOLE_SDK
	// The console has no clock_gettime(); this is its millisecond tick counter.
	return (double)os_get_tick_count()/1000.0;
#elif defined RE3_HAVE_CLOCK_GETTIME
	struct timespec ts;
#ifdef CLOCK_MONOTONIC
	clock_gettime(CLOCK_MONOTONIC, &ts);
#else
	clock_gettime(CLOCK_REALTIME, &ts);
#endif
	return (double)ts.tv_sec + (double)ts.tv_nsec/1000000000.0;
#else
	return (double)time(nil);
#endif
}

double
psTimerSeconds(void)
{
	return timerNow();
}

double
psTimer(void)
{
	return timerNow()*1000.0;
}

/* ---------------------------------------------------------- boot logging */

/* Everything re3 does during startup goes through here.  The firmware's xlog
 * only flushes every 64th line (debug.c:260), so on a crash the tail is lost -
 * hence sf2000LogSync() below, which forces it out at each step that matters.
 *
 * Enabled whenever /mnt/sda1/bios/enable_xlog.txt exists, which is what the
 * multicore itself keys off, so one flag turns on both this and the firmware
 * log.  Set RE3_BOOTLOG=0 to silence it while keeping the firmware log. */
static int s_bootLog = -1;

void
sf2000LogInit(void)
{
	if(s_bootLog < 0){
		if(getenv("RE3_BOOTLOG") != nil)
			s_bootLog = getenv("RE3_BOOTLOG")[0] != '0';
		else
			s_bootLog = access("/mnt/sda1/bios/enable_xlog.txt", F_OK) == 0;
	}
}

void
sf2000Log(const char *fmt, ...)
{
	if(!s_bootLog)
		return;

	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	/* The firmware multiplexes stderr into the same log file. Flush the stream
	 * buffer, but do NOT call fs_sync(): syncing flash is extremely slow and
	 * causes massive frame drops on SD storage. */
	fflush(stderr);
}

/* Probe a file and log the verdict.  re3 reports a missing file as a bare
 * "Couldn't open file X" from three different layers, which is not enough to
 * tell "wrong directory" from "file really absent"; this spells it out. */
static void
sf2000LogProbe(const char *path)
{
	int fd = open(path, O_RDONLY);
	if(fd >= 0){
		sf2000Log("[boot]   have %s (%ld bytes)\n", path, (long)lseek(fd, 0, SEEK_END));
		close(fd);
	} else {
		sf2000Log("[boot]   MISSING %s (errno %d)\n", path, errno);
	}
}

/* Dump the first directory entries, to tell an empty dir from a wrong one. */
static void
sf2000LogDir(const char *path)
{
	DIR *d = opendir(path);
	if(!d){
		sf2000Log("[boot]   opendir('%s') FAILED (errno %d)\n", path, errno);
		return;
	}
	int n = 0;
	struct dirent *e;
	while((e = readdir(d)) != nil && n < 8){
		sf2000Log("[boot]     %s\n", e->d_name);
		n++;
	}
	if(n == 0)
		sf2000Log("[boot]     (empty)\n");
	closedir(d);
}

/* ---------------------------------------------------------- game startup */

static psGlobalType PsGlobal;
static RwBool RwInitialised = FALSE;
static RwBool ForegroundApp = TRUE;

/* re3 tests this in a handful of places to pick Win98-era behaviour.  A modern
 * console is closer to XP than to 95, and that is also what the glfw skeleton
 * reports, so say OS_WINXP. */
long _dwOperatingSystemVersion = OS_WINXP;

/* ---- RAM budget ---------------------------------------------------------
 *
 * The GB300/SF2000 multicore patches the firmware's RAMSIZE to 0x87000000
 * (main.c: "FIXME! all of it!!"), so the machine enforces nothing: if the core
 * walks past the end, malloc runs into the framebuffer instead of failing
 * cleanly.  re3's own CMemoryHeap is not in play either - USE_CUSTOM_ALLOCATOR
 * is off in config.h - so every allocation is a plain malloc against that
 * unbounded heap.  The budget therefore has to be enforced by hand:
 *
 *     total                60 MB   <- what we allow ourselves to use
 *     - static image        8 MB   <- .text/.data/.bss of this core, ~5.8 MB
 *     = heap               52 MB   <- everything malloc may hand out
 *
 * CStreaming takes a fixed slice of that heap (see Streaming.cpp) and the
 * remainder covers the world pools, audio and the savegame.  _dwMemAvailPhys
 * is therefore the heap figure, not the total; RE3_MEM_MB overrides it while
 * testing on a PC.
 */
#define RE3_RAM_BUDGET   (60 * 1024 * 1024)
#define RE3_STATIC_IMAGE ( 8 * 1024 * 1024)
size_t _dwMemAvailPhys = RE3_RAM_BUDGET - RE3_STATIC_IMAGE;

/* The top level state machine lives in the platform layer on every other
 * re3 target, and re3 reads it from rw/TexRead.cpp. */
RwUInt32 gGameState = GS_START_UP;

static RwSubSystemInfo GsubSysInfo[1];
static RwInt32 GnumSubSystems = 0;
static RwInt32 GcurSel = 0, GcurSelVM = 0;

void CapturePad(int padID);

static RwChar *_VMList[2] = { RWSTRING("320x240"), RWSTRING("") };

RwBool
psInitialize(void)
{
	memset(&PsGlobal, 0, sizeof(PsGlobal));
	PsGlobal.fullScreen = FALSE;
	PsGlobal.cursorIsInWindow = TRUE;
	PsGlobal.lastMousePos.x = 0;
	PsGlobal.lastMousePos.y = 0;
	RsGlobal.ps = &PsGlobal;

	/* The panel is 320x240.  Rendering at the panel's own size keeps the
	 * software rasteriser's fill cost down and lets showRaster be a plain
	 * memcpy.  re3 scales its HUD through RsGlobal.width/height. */
	RsGlobal.maximumWidth  = sf2000ScreenWidth;
	RsGlobal.maximumHeight = sf2000ScreenHeight;
	RsGlobal.width  = sf2000ScreenWidth;
	RsGlobal.height = sf2000ScreenHeight;
	RsGlobal.maxFPS = 30;

	GsubSysInfo[0].name[0] = '\0';
	GnumSubSystems = 0;

	if(const char *mem = getenv("RE3_MEM_MB"))
		_dwMemAvailPhys = (size_t)atoi(mem)*1024*1024;

	return TRUE;
}

void
psTerminate(void)
{
	ForegroundApp = TRUE;
}

RwBool
psCameraBeginUpdate(RwCamera *camera)
{
	if(!RwCameraBeginUpdate(camera))
	{
		ForegroundApp = FALSE;
		return FALSE;
	}
	ForegroundApp = TRUE;
	return TRUE;
}

extern void sf2000PresentFrame(void);

#if (defined(SF2000) || defined(GB300V2) || defined(TARGET_SF2000)) && !defined(__i386__) && !defined(__x86_64__)
extern "C" int fs_sync(const char *path);
#endif

void
psSyncFile(const char *path)
{
#if (defined(SF2000) || defined(GB300V2) || defined(TARGET_SF2000)) && !defined(__i386__) && !defined(__x86_64__)
	fs_sync(path ? path : "");
#else
	(void)path;
	sync();
#endif
}

void
psCameraShowRaster(RwCamera *camera)
{
	(void)camera;
	/* librw's soft backend has already finished drawing into
	 * rw::soft::frameBuffer; the host picks it up after this returns. */
	sFrameReady = true;
	sf2000PresentFrame();
}

RwImage *
psGrabScreen(RwCamera *pCamera)
{
	rw::Image *image = RwCameraGetRaster(pCamera)->toImage();
	if(image)
		image->removeMask();
	return image;
}

void psMouseSetPos(RwV2d *pos) { (void)pos; }

RwMemoryFunctions*
psGetMemoryFunctions(void)
{
#ifdef USE_CUSTOM_ALLOCATOR
	return &memFuncs;
#else
	return nil;
#endif
}

RwBool psInstallFileSystem(void) { return TRUE; }

/* CFileMgr::SetDirMyDocuments() lands here.  There are no user documents on a
 * console, so saves go into a "saves" folder next to the game data. */
const char *
_psGetUserFilesFolder(void)
{
	return ".";
}

void
_psCreateFolder(const char *path)
{
	(void)mkdir(path, 0777);
}

/* The soft backend keeps every raster in plain CPU memory, so there is no
 * native texture format to speak of: always take re3's generic path. */
RwBool psNativeTextureSupport(void) { return FALSE; }

void _InputTranslateShiftKeyUpDown(RsKeyCodes *rs) { (void)rs; }
long _InputInitialiseMouse(void) { return 0; }
void _InputInitialiseJoys(void) { }
RwBool IsForegroundApp(void) { return ForegroundApp; }

void
HandleExit(void)
{
	RsGlobal.quit = TRUE;
}

void
_psSelectScreenVM(RwInt32 videoMode)
{
	GcurSelVM = videoMode;
}

RwBool
psSelectDevice(void)
{
	ForegroundApp = TRUE;
	return TRUE;
}

RwInt32 _psGetNumVideModes(void) { return 1; }

RwChar **
_psGetVideoModeList(void)
{
	_VMList[0] = RWSTRING("320x240");
	return _VMList;
}

RwBool
_psSetVideoMode(RwInt32 subSystem, RwInt32 videoMode)
{
	(void)subSystem;
	GcurSelVM = videoMode;
	return TRUE;
}

/* re3 keeps its own locale list; pick the language the host asked for. */
static const char *gta3_languages[] = {
	"ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH", "POLISH", "RUSSIAN", "JAPANESE"
};

void
InitialiseLanguage(void)
{
	int32 idx = sf2000Language;
	if(idx < 0) idx = 0;
	if(idx >= (int32)(sizeof(gta3_languages)/sizeof(gta3_languages[0])))
		idx = 0;
	CMenuManager::m_PrefsLanguage = idx;
}

void
psPostRWinit(void)
{
	/* Make sure no button is stuck down from a previous frame. */
	CPad::GetPad(0)->Clear(true);
	CPad::GetPad(1)->Clear(true);
}

/* ------------------------------------------------------------- input ---- */

/*
 * The console pad is mapped onto the RsKeyCodes that re3's default control
 * configuration binds actions to.  That way every action the game supports
 * (walking, driving, shooting, camera, menus) works straight from the pad,
 * without the in-game configuration screen - which a 320x240 panel could not
 * host anyway.
 *
 * The bindings below are the ones in
 * CControllerConfigManager::InitDefaultControlConfiguration():
 *
 *   rsUP/rsDOWN/rsLEFT/rsRIGHT  walk, drive, look behind
 *   rsENTER                     confirm, enter/exit vehicle
 *   rsESC                       back, cancel, pause menu
 *   rsLCTRL                     fire
 *   rsRCTRL                     jump, handbrake
 *   rsLSHIFT / rsRSHIFT         sprint / horn
 *   rsPADUP / rsPAD5            camera up / down
 *   rsPADLEFT / rsPADRIGHT      camera left / right (also turret)
 *   rsPGUP / rsPGDN             sniper zoom in / out
 *   rsHOME                      change camera
 *   rsTAB                       menus
 */
enum {
	SF_UP = 0, SF_DOWN, SF_LEFT, SF_RIGHT,
	SF_A, SF_B, SF_X, SF_Y, SF_L, SF_R,
	SF_START, SF_SELECT, SF_L2, SF_R2,
	SF_STICK_UP, SF_STICK_DOWN, SF_STICK_LEFT, SF_STICK_RIGHT,
	SF_COUNT
};

static const RsKeyCodes sf2000_keymap[SF_COUNT] = {
	rsUP,        /* SF_UP           - Move Forward / Accelerate */
	rsDOWN,      /* SF_DOWN         - Move Backward / Reverse */
	rsLEFT,      /* SF_LEFT         - Turn Left / Steer Left */
	rsRIGHT,     /* SF_RIGHT        - Turn Right / Steer Right */
	rsLCTRL,     /* SF_A (Circle)   - Fire / Punch / Handbrake */
	rsLSHIFT,    /* SF_B (Cross)    - Sprint / Accelerate / Confirm */
	rsENTER,     /* SF_X (Triangle) - Enter/Exit Vehicle / Cancel */
	rsRCTRL,     /* SF_Y (Square)   - Jump / Brake */
	rsPADDEL,    /* SF_L            - Cycle Weapon Left / Look Left */
	rsPADENTER,  /* SF_R            - Cycle Weapon Right / Target / Look Right */
	rsESC,       /* SF_START        - Pause Menu */
	rsHOME,      /* SF_SELECT       - Change Camera */
	rsPADEND,    /* SF_L2           */
	rsPADDOWN,   /* SF_R2           */
	rsUP,        /* SF_STICK_UP     */
	rsDOWN,      /* SF_STICK_DOWN   */
	rsLEFT,      /* SF_STICK_LEFT   */
	rsRIGHT,     /* SF_STICK_RIGHT  */
};

static bool sCurrentButtons[SF_COUNT];
static bool sPrevButtons[SF_COUNT];

static void
sendKey(int idx, bool down)
{
	RsKeyStatus ks;
	ks.keyCharCode = sf2000_keymap[idx];
	RsKeyboardEventHandler(down ? rsKEYDOWN : rsKEYUP, &ks);
}

void
sf2000PushInput(const bool *buttons)
{
	for(int i = 0; i < SF_COUNT; i++){
		if(buttons[i] != sPrevButtons[i]){
			sPrevButtons[i] = buttons[i];
			sendKey(i, buttons[i]);
		}
		sCurrentButtons[i] = buttons[i];
	}
}

void
sf2000ReleaseAllInput(void)
{
	for(int i = 0; i < SF_COUNT; i++){
		sCurrentButtons[i] = false;
		if(sPrevButtons[i]){
			sPrevButtons[i] = false;
			sendKey(i, false);
		}
	}
}

void
CapturePad(int padID)
{
	if(padID != 0)
		return;

	CPad *pad = CPad::GetPad(0);
	if(!pad)
		return;

	// D-Pad / Stick: movement on foot (strzałki) & steering in vehicle
	if(sCurrentButtons[SF_UP] || sCurrentButtons[SF_STICK_UP]){
		pad->PCTempJoyState.DPadUp = 255;
		pad->PCTempJoyState.LeftStickY = -128;
	}
	if(sCurrentButtons[SF_DOWN] || sCurrentButtons[SF_STICK_DOWN]){
		pad->PCTempJoyState.DPadDown = 255;
		pad->PCTempJoyState.LeftStickY = 128;
	}
	if(sCurrentButtons[SF_LEFT] || sCurrentButtons[SF_STICK_LEFT]){
		pad->PCTempJoyState.DPadLeft = 255;
		pad->PCTempJoyState.LeftStickX = -128;
	}
	if(sCurrentButtons[SF_RIGHT] || sCurrentButtons[SF_STICK_RIGHT]){
		pad->PCTempJoyState.DPadRight = 255;
		pad->PCTempJoyState.LeftStickX = 128;
	}

	// PS2 ABXY layout:
	// Button B (South) = PS2 Cross (Sprint on foot, Accelerate in vehicle, Confirm in menu)
	if(sCurrentButtons[SF_B])
		pad->PCTempJoyState.Cross = 255;

	// Button A (East) = PS2 Circle (Fire/Attack on foot, Handbrake in vehicle, Cancel in menu)
	if(sCurrentButtons[SF_A])
		pad->PCTempJoyState.Circle = 255;

	// Button X (North) = PS2 Triangle (Enter/Exit vehicle)
	if(sCurrentButtons[SF_X])
		pad->PCTempJoyState.Triangle = 255;

	// Button Y (West) = PS2 Square (Jump on foot, Reverse/Brake in vehicle)
	if(sCurrentButtons[SF_Y])
		pad->PCTempJoyState.Square = 255;

	// Shoulder L = Weapon cycle left (foot) / Look left (vehicle)
	if(sCurrentButtons[SF_L] || sCurrentButtons[SF_L2]){
		pad->PCTempJoyState.LeftShoulder1 = 255;
		pad->PCTempJoyState.LeftShoulder2 = 255;
	}

	// Shoulder R = Weapon cycle right / Lock target (foot) / Handbrake / Look right (vehicle)
	if(sCurrentButtons[SF_R] || sCurrentButtons[SF_R2]){
		pad->PCTempJoyState.RightShoulder1 = 255;
		pad->PCTempJoyState.RightShoulder2 = 255;
	}

	// START = Menu / Pause
	if(sCurrentButtons[SF_START])
		pad->PCTempJoyState.Start = 255;

	// SELECT = Change camera
	if(sCurrentButtons[SF_SELECT])
		pad->PCTempJoyState.Select = 255;
}

/* ------------------------------------------------------------ frame step */

void
sf2000Startup(const char *gameDir)
{
	sf2000LogInit();

	sf2000Log("[boot] === retro_load_game begin ===\n");
	sf2000Log("[boot] gameDir='%s'\n", gameDir ? gameDir : "(null)");
	hacker_loader_log("DIR", "STARTUP ENTERED", 22);

	/* re3 opens every asset with a relative path (MODELS/FONTS.TXD and so on),
	 * so this chdir is load-bearing: if it fails, every open below misses and
	 * the game dies on a missing font rather than on the real cause.  Log the
	 * errno and the directory state so the failure is unambiguous. */
	char cwd[512];
	if(getcwd(cwd, sizeof(cwd)) == nil)
		sf2000Log("[boot] cwd before chdir: <getcwd failed, errno %d>\n", errno);
	else
		sf2000Log("[boot] cwd before chdir: '%s'\n", cwd);

	if(gameDir && gameDir[0]){
		if(chdir(gameDir) != 0){
			sf2000Log("[boot] chdir('%s') FAILED errno=%d (%s)\n", gameDir, errno, strerror(errno));
			sf2000LogProbe(gameDir);
			sf2000LogDir(gameDir);
			hacker_loader_log("VFS", "CHDIR FAILED", 25);
		} else {
			sf2000Log("[boot] chdir('%s') ok\n", gameDir);
			hacker_loader_log("VFS", "CHDIR OK", 28);
		}
	}

	if(getcwd(cwd, sizeof(cwd)) == nil)
		sf2000Log("[boot] cwd after chdir: <getcwd failed, errno %d>\n", errno);
	else
		sf2000Log("[boot] cwd after chdir: '%s'\n", cwd);

	/* Probe the assets the very next librw step needs. */
	sf2000LogProbe("MODELS/FONTS.TXD");
	sf2000LogProbe("models/fonts.txd");
	sf2000LogDir("MODELS");
	/* The single most useful line on this screen: if the probe says MISSING the
	 * chdir is the culprit, and nothing after this point needs to be trusted. */
	{
		int fd = open("MODELS/FONTS.TXD", O_RDONLY);
		hacker_loader_log("TXR", fd >= 0 ? "FONTS.TXD FOUND" : "FONTS.TXD MISSING", 32);
		if(fd >= 0) close(fd);
	}

	sf2000Log("[boot] InitialiseLanguage\n");
	hacker_loader_log("LOC", "INITIALISE LANGUAGE", 36);
	InitialiseLanguage();

	ControlsManager.MakeControllerActionsBlank();
	ControlsManager.InitDefaultControlConfiguration();

	sf2000Log("[boot] rsINITIALIZE\n");
	hacker_loader_log("RW", "rsINITIALIZE", 40);
	if(RsEventHandler(rsINITIALIZE, nil) == rsEVENTERROR){
		fprintf(stderr, "[sf2000] rsINITIALIZE failed\n");
		sf2000Log("[boot] rsINITIALIZE FAILED\n");
		hacker_loader_log("RW", "rsINITIALIZE FAILED", 0);
		return;
	}

	rw::EngineOpenParams openParams;
	memset(&openParams, 0, sizeof(openParams));
	openParams.width = RsGlobal.maximumWidth;
	openParams.height = RsGlobal.maximumHeight;
	openParams.windowtitle = RsGlobal.appName;

	sf2000Log("[boot] rsRWINITIALIZE %dx%d\n", openParams.width, openParams.height);
	hacker_loader_log("RW", "rsRWINITIALIZE", 45);
	{
		RsEventStatus st = RsEventHandler(rsRWINITIALIZE, &openParams);
		if(st == rsEVENTERROR){
			fprintf(stderr, "[sf2000] rsRWINITIALIZE failed\n");
			sf2000Log("[boot] rsRWINITIALIZE FAILED\n");
			RsEventHandler(rsTERMINATE, nil);
			return;
		}
	}

	sf2000Log("[boot] psPostRWinit\n");
	hacker_loader_log("RW", "psPostRWinit", 50);
	psPostRWinit();

	{
		RwRect r;
		r.x = 0; r.y = 0;
		r.w = RsGlobal.maximumWidth;
		r.h = RsGlobal.maximumHeight;
		sf2000Log("[boot] rsCAMERASIZE %dx%d\n", r.w, r.h);
		RsEventHandler(rsCAMERASIZE, &r);
	}

	/* Load the user configuration (options, display, audio, controls) from gta3.set,
	 * but enforce default console pad layout afterwards. */
	FrontEndMenuManager.LoadSettings();
	/* Ensure solid draw distance on console (avoid disappearing geometry) */
	CRenderer::ms_drawDistScale = 1.0f;
	CRenderer::ms_lodDistScale = 1.0f;
#if defined(RW_SOFT)
	/* The console rasterises on the CPU; a saved RenderResolution of 1.0 would
	 * quadruple the 3D fill cost and drop the device to a slideshow.  Clamp the
	 * world pass to half linear resolution no matter what gta3.set says. */
	rw::soft::worldRenderRes = 0.5f;
#endif
	ControlsManager.InitDefaultControlConfiguration();

	sf2000Log("[boot] === retro_load_game done, state=%u ===\n", (unsigned)gGameState);
	hacker_loader_log("RW", "STARTUP COMPLETE", 65);
}

/*
 * One iteration of the game state machine, the same one glfw.cpp runs inside
 * its main loop.  Returns false once the game wants to quit or restart.
 */
bool
sf2000StepFrame(void)
{
	static RwBool firstFrame = TRUE;
	RwV2d pos;

	if(firstFrame){
		firstFrame = FALSE;
		pos.x = RsGlobal.maximumWidth*0.5f;
		pos.y = RsGlobal.maximumHeight*0.5f;
		RsMouseSetPos(&pos);
		RwInitialised = TRUE;
#if defined(RW_SOFT)
		/* Pick the world's reduced render resolution before anything
		 * allocates a framebuffer; the 2D layer stays at native resolution. */
		SetWorldRenderScale();
		sf2000Log("[boot] world render scale = %d\n",
		          (int)rw::soft::worldRenderScale);
#endif
	}

	if(RsGlobal.quit)
		return false;

	/* Per-frame state trace.  Enabled by the same flag as the boot log, because
	 * the question it answers - did retro_run get called at all, and how far did
	 * the state machine get - is the one thing a black screen leaves open.  The
	 * first 8 frames are logged unconditionally so a crash on frame 3 is still
	 * visible; after that only state changes, which keeps the log readable
	 * instead of one line per frame. */
	static int s_frameNo = 0;
	static unsigned s_lastState = 0xffffffff;
	s_frameNo++;
	if(s_frameNo <= 8 || gGameState != s_lastState){
		sf2000Log("[run] frame %d state %u -> %u\n", s_frameNo, s_lastState, (unsigned)gGameState);
		if(s_frameNo <= 8)
			hacker_loader_log("RUN", "FIRST FRAMES", 70);
		s_lastState = gGameState;
	}
	/* If the state machine never leaves GS_START_UP we are crashing in the very
	 * first frame; say so explicitly rather than leaving it to be inferred. */
	if(s_frameNo == 40)
		sf2000Log("[run] survived 40 frames, state=%u\n", (unsigned)gGameState);

	switch(gGameState){
	case GS_START_UP:
		sf2000Log("[run] GS_START_UP -> GS_INIT_ONCE\n");
		hacker_loader_log("GME", "GS_START_UP", 72);
		gGameState = GS_INIT_ONCE;
		break;

	case GS_INIT_ONCE:
		/* LoadingScreen opens the txd of the given name and is the first thing
		 * that really needs the asset files, so it is where a bad chdir or a
		 * missing font turns into a crash. */
		sf2000Log("[run] GS_INIT_ONCE LoadingScreen(loadsc0)\n");
		hacker_loader_log("GME", "LOADING SCREEN TXD", 74);
		LoadingScreen(nil, nil, "loadsc0");
		sf2000Log("[run] InitialiseOnceAfterRW\n");
		hacker_loader_log("GME", "INITIALISE ONCE", 76);
		if(!CGame::InitialiseOnceAfterRW())
			RsGlobal.quit = TRUE;
		sf2000Log("[run] GS_INIT_ONCE done\n");
		gGameState = GS_INIT_FRONTEND;
		break;

	case GS_INIT_FRONTEND:
		sf2000Log("[run] GS_INIT_FRONTEND LoadingScreen(loadsc0)\n");
		hacker_loader_log("GME", "FRONTEND LOADING", 80);
		LoadingScreen(nil, nil, "loadsc0");
		sf2000Log("[run] GS_INIT_FRONTEND done\n");
		if(getenv("RE3_AUTORUN") != nil && getenv("RE3_AUTORUN")[0] != '0'){
			FrontEndMenuManager.m_bGameNotLoaded = true;
			CMenuManager::m_bStartUpFrontEndRequested = false;
			FrontEndMenuManager.m_bMenuActive = false;
			gGameState = GS_INIT_PLAYING_GAME;
		}else{
			FrontEndMenuManager.m_bGameNotLoaded = true;
			CMenuManager::m_bStartUpFrontEndRequested = true;
			gGameState = GS_FRONTEND;
		}
		break;

	case GS_FRONTEND:
		RsEventHandler(rsFRONTENDIDLE, nil);
		/* Once the frontend is up and drawing, the boot log has done its job:
		 * hold it at 100% rather than letting it scroll forever. */
		hacker_loader_stop();
		if(!FrontEndMenuManager.m_bMenuActive || FrontEndMenuManager.m_bWantToLoad || FrontEndMenuManager.m_bWantToRestart)
			gGameState = GS_INIT_PLAYING_GAME;
		if(FrontEndMenuManager.m_bWantToLoad || FrontEndMenuManager.m_bWantToRestart){
			InitialiseGame();
			FrontEndMenuManager.m_bGameNotLoaded = false;
			FrontEndMenuManager.m_bWantToLoad = false;
			FrontEndMenuManager.m_bWantToRestart = false;
			gGameState = GS_PLAYING_GAME;
			s_bootLog = 0;
		}
		break;

	case GS_INIT_PLAYING_GAME:
		InitialiseGame();
		FrontEndMenuManager.m_bGameNotLoaded = false;
		FrontEndMenuManager.m_bWantToLoad = false;
		FrontEndMenuManager.m_bWantToRestart = false;
		gGameState = GS_PLAYING_GAME;
		s_bootLog = 0;
		break;

	case GS_PLAYING_GAME:
		if(FrontEndMenuManager.m_bWantToRestart){
			InitialiseGame();
			FrontEndMenuManager.m_bGameNotLoaded = false;
			FrontEndMenuManager.m_bWantToLoad = false;
			FrontEndMenuManager.m_bWantToRestart = false;
		}
		if(RwInitialised){
			RsEventHandler(rsIDLE, (void*)TRUE);
		}
		break;

	default:
		break;
	}

	return !RsGlobal.quit;
}
