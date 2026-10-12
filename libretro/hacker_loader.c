/* On-screen boot log, ported from openbor_sf2000/libretro/hacker_loader.c.
 *
 * Why: on this console a crash during startup leaves a black screen, and a
 * black screen looks identical to a hang.  The firmware's MULTICORE.log is the
 * usual answer, but the loader only fsyncs every 64th line (debug.c:260), so a
 * hard fault swallows the very lines that would say where it died - and there
 * is no way to read the SD card without taking the console apart.  So render
 * the log into the framebuffer instead: whatever the last step to reach the
 * screen was is what stays on the LCD.
 *
 * RGB565, 8x8 font, same layout as openbor's so the two look identical.
 */

#include "hacker_loader.h"
#include "font8x8.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

#define HACKER_MAX_LOGS 14
#define HACKER_LINE_MAX 42

#define COLOR_BLACK        0x0000
#define COLOR_SCANLINE     0x0842  /* subtle dark grey, background banding */
#define COLOR_MATRIX_GREEN 0x07E0
#define COLOR_AMBER        0xFFE0
#define COLOR_WHITE        0xFFFF
#define COLOR_CYAN         0x07FF

static uint16_t s_loader_fb[320 * 240];
static retro_video_refresh_t s_video_cb = NULL;

static char s_hacker_logs[HACKER_MAX_LOGS][HACKER_LINE_MAX];
static int  s_hacker_log_count = 0;
static char s_rom_name[48] = {0};
static uint32_t s_boot_start_ms = 0;
static int  s_boot_active = 0;
static int  s_hacker_progress = 0;

/* The firmware blits whatever geometry it is handed, so the console's native
 * 320x240 is used rather than re3's 256x192 internal resolution: a boot log
 * squeezed into 256 pixels would wrap and hide lines. */
#define SCREEN_W 320
#define SCREEN_H 240

static uint32_t get_time_ms(void)
{
	struct timeval tv;
	if(gettimeofday(&tv, NULL) == 0)
		return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
	return 0;
}

void hacker_loader_set_video_cb(retro_video_refresh_t cb)
{
	s_video_cb = cb;
}

static void draw_char8x8(int x, int y, char c, uint16_t fg, uint16_t bg)
{
	if(c < 32 || c > 126) c = ' ';
	const uint8_t *glyph = font8x8_basic[c - 32];
	for(int row = 0; row < 8; row++){
		int py = y + row;
		if(py < 0 || py >= SCREEN_H) continue;
		uint16_t *dst = &s_loader_fb[py * SCREEN_W + x];
		uint8_t line = glyph[row];
		for(int col = 0; col < 8; col++){
			int px = x + col;
			if(px >= 0 && px < SCREEN_W){
				if(line & (0x80 >> col)) dst[col] = fg;
				else if(bg != 0) dst[col] = bg;
			}
		}
	}
}

static void draw_text(int x, int y, const char *str, uint16_t fg, uint16_t bg)
{
	if(!str) return;
	int cur_x = x;
	while(*str){
		if(*str == '\n'){
			cur_x = x;
			y += 10;
		} else {
			draw_char8x8(cur_x, y, *str, fg, bg);
			cur_x += 8;
			if(cur_x + 8 > SCREEN_W) break;
		}
		str++;
	}
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
	if(x < 0){ w += x; x = 0; }
	if(y < 0){ h += y; y = 0; }
	if(x + w > SCREEN_W) w = SCREEN_W - x;
	if(y + h > SCREEN_H) h = SCREEN_H - y;
	if(w <= 0 || h <= 0) return;

	for(int row = 0; row < h; row++){
		uint16_t *line = &s_loader_fb[(y + row) * SCREEN_W + x];
		for(int col = 0; col < w; col++)
			line[col] = color;
	}
}

static void hacker_render_screen(void)
{
	/* Background with subtle scanlines */
	for(int y = 0; y < SCREEN_H; y++){
		uint16_t line_color = (y & 3) ? COLOR_BLACK : COLOR_SCANLINE;
		uint16_t *line = &s_loader_fb[y * SCREEN_W];
		for(int x = 0; x < SCREEN_W; x++)
			line[x] = line_color;
	}

	char rom_line[42];
	snprintf(rom_line, sizeof(rom_line), "ROM:  %.32s", s_rom_name[0] ? s_rom_name : "-");
	draw_text(8, 6, rom_line, COLOR_CYAN, 0);

	draw_text(8, 18, "CORE: re3 / GTA III (SF2000)", COLOR_AMBER, 0);

	fill_rect(8, 29, SCREEN_W - 16, 1, COLOR_SCANLINE);

	int log_start_y = 34;
	for(int i = 0; i < s_hacker_log_count && i < HACKER_MAX_LOGS; i++){
		int text_y = log_start_y + i * 11;
		uint16_t text_color = (i == s_hacker_log_count - 1) ? COLOR_WHITE : COLOR_MATRIX_GREEN;
		draw_text(8, text_y, s_hacker_logs[i], text_color, 0);
	}

	fill_rect(8, 193, SCREEN_W - 16, 1, COLOR_SCANLINE);

	char status_str[32];
	snprintf(status_str, sizeof(status_str), "BOOTING: %3d%%", s_hacker_progress);
	draw_text(8, 197, status_str, COLOR_CYAN, 0);

	int bar_x = 8, bar_y = 210;
	int bar_w = SCREEN_W - 16;
	int bar_h = 10;

	fill_rect(bar_x, bar_y, bar_w, 1, COLOR_SCANLINE);
	fill_rect(bar_x, bar_y + bar_h - 1, bar_w, 1, COLOR_SCANLINE);
	fill_rect(bar_x, bar_y, 1, bar_h, COLOR_SCANLINE);
	fill_rect(bar_x + bar_w - 1, bar_y, 1, bar_h, COLOR_SCANLINE);

	int max_fill = bar_w - 4;
	int fill_w = (s_hacker_progress * max_fill) / 100;
	if(fill_w > max_fill) fill_w = max_fill;
	if(fill_w > 0)
		fill_rect(bar_x + 2, bar_y + 2, fill_w, bar_h - 4, COLOR_MATRIX_GREEN);

	/* Bottom center branding */
	draw_text((SCREEN_W - 8 * 8) / 2, 226, "synaps33", COLOR_AMBER, 0);
}

static void hacker_flush(void)
{
	hacker_render_screen();
	if(s_video_cb)
		s_video_cb(s_loader_fb, SCREEN_W, SCREEN_H, SCREEN_W * sizeof(uint16_t));
}

void hacker_loader_init(const char *rom_path)
{
	s_boot_active = 1;
	s_hacker_log_count = 0;
	s_hacker_progress = 5;
	s_boot_start_ms = get_time_ms();

	if(rom_path && *rom_path){
		const char *b = strrchr(rom_path, '/');
		b = b ? b + 1 : rom_path;
		snprintf(s_rom_name, sizeof(s_rom_name), "%.36s", b);
	} else {
		strcpy(s_rom_name, "ROM_STREAM");
	}

	hacker_loader_log("SYS", "CORE INIT: MIPS32r2 SOFT-FLOAT", 5);
	hacker_loader_log("VFS", "MOUNT /dev/sdcard ... [OK]", 8);
}

void hacker_loader_log(const char *tag, const char *msg, int pct)
{
	if(!s_boot_active) return;

	if(pct > s_hacker_progress){
		s_hacker_progress = pct;
		if(s_hacker_progress > 100) s_hacker_progress = 100;
	}

	if(s_boot_start_ms == 0)
		s_boot_start_ms = get_time_ms();
	uint32_t now = get_time_ms();
	uint32_t elapsed_ms = (now >= s_boot_start_ms) ? (now - s_boot_start_ms) : 0;

	char formatted[HACKER_LINE_MAX];
	snprintf(formatted, sizeof(formatted), "[%u.%02us] [%-4.4s] %.24s",
	         (unsigned)(elapsed_ms / 1000), (unsigned)((elapsed_ms % 1000) / 10),
	         tag ? tag : "SYS", msg ? msg : "");

	if(s_hacker_log_count < HACKER_MAX_LOGS){
		strncpy(s_hacker_logs[s_hacker_log_count], formatted, HACKER_LINE_MAX - 1);
		s_hacker_logs[s_hacker_log_count][HACKER_LINE_MAX - 1] = '\0';
		s_hacker_log_count++;
	} else {
		memmove(&s_hacker_logs[0], &s_hacker_logs[1], (HACKER_MAX_LOGS - 1) * HACKER_LINE_MAX);
		strncpy(s_hacker_logs[HACKER_MAX_LOGS - 1], formatted, HACKER_LINE_MAX - 1);
		s_hacker_logs[HACKER_MAX_LOGS - 1][HACKER_LINE_MAX - 1] = '\0';
	}

	hacker_flush();
}

void hacker_loader_stop(void)
{
	if(!s_boot_active) return;
	s_hacker_progress = 100;
	hacker_flush();
	s_boot_active = 0;
}
