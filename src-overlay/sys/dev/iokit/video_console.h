/*
 * NextBSD video console, private interface between iokit_platform.c (the
 * platform expert's console role) and video_console.c (initialize_screen
 * and the boot spinner). Public entry points are in <sys/pexpert.h>.
 */

#ifndef _DEV_IOKIT_VIDEO_CONSOLE_H_
#define	_DEV_IOKIT_VIDEO_CONSOLE_H_

#include <sys/sysctl.h>

struct fb_info;

/*
 * The framebuffer an op is about. Called by PE_initialize_console_fb()
 * before initialize_screen(): kPEBaseAddressChange and kPEEnableScreen
 * with a non-NULL fb make it the console's mapping (under the spinner's
 * lock); kPERefreshBootGraphics paints into it (NULL: the console's own).
 */
void	vc_console_fb(struct fb_info *fb, int op);

/* PE_panic_console(): no locks, no sleeping. */
void	vc_panic(void);

/* hw.iokit.console.state */
int	vc_sysctl_state(SYSCTL_HANDLER_ARGS);

#endif /* !_DEV_IOKIT_VIDEO_CONSOLE_H_ */
