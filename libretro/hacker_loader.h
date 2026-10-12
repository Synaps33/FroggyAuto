#ifndef RE3_HACKER_LOADER_H
#define RE3_HACKER_LOADER_H

#include <libretro.h>

/* The on-screen boot log used by froggykvm/openbor: instead of writing to a
 * file, it renders a scrolling list of timestamped lines straight into the
 * framebuffer, so a crash is visible on the LCD even when no log file can be
 * read back afterwards.  A black screen is otherwise indistinguishable from a
 * hang, which is exactly the situation this was written for. */

#ifdef __cplusplus
extern "C" {
#endif

void hacker_loader_set_video_cb(retro_video_refresh_t cb);
void hacker_loader_init(const char *rom_path);
void hacker_loader_log(const char *tag, const char *msg, int pct);
void hacker_loader_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* RE3_HACKER_LOADER_H */
