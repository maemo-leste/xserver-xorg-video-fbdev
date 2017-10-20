/*
 * Copyright (c) 2008, 2009  Nokia Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef FBDEV_H
#define FBDEV_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "xf86.h"
#include "xf86_OSproc.h"

#include "dgaproc.h"

#include "xf86Crtc.h"

#include "xf86Resources.h"

#include "fbdevhw.h"

#include "xf86xv.h"

#include <linux/fb.h>
#include <linux/omapfb.h>

#define FBDEV_VERSION           4000
#define FBDEV_NAME              "FBDEV"
#define FBDEV_DRIVER_NAME       "fbdev"

#define CALLTRACE(...)		//ErrorF(__VA_ARGS__)
#define DBGCOMPOSITE(...)	//ErrorF(__VA_ARGS__)
#define DBG(...)		//ErrorF(__VA_ARGS__)

typedef struct {
	unsigned char *fbstart;
	unsigned char *fbmem;
	int fboff;
	int lineLength;
	CloseScreenProcPtr CloseScreen;
	void (*PointerMoved) (int index, int x, int y);
	EntityInfoPtr pEnt;
	OptionInfoPtr Options;
	ScreenPtr screen;

	/* The following are all used by omap_video.c. */
	int fd;
	int num_video_ports;
	XF86VideoAdaptorPtr overlay_adaptor;
	DestroyWindowProcPtr video_destroy_window;
	DestroyPixmapProcPtr video_destroy_pixmap;

	/* Avoid reconfiguring the root window -- abominable hack. */
	int suppress_reconfig;
	Atom suppress_reconfig_prop;
	xf86EnableDisableFBAccessProc *enable_fb_access;

	xf86CrtcPtr crtc_lcd;
	xf86OutputPtr output_lcd;
	DisplayModePtr builtin;

	struct fb_var_screeninfo saved_var;
} FBDevRec, *FBDevPtr;

#define FBDEVPTR(p) ((FBDevPtr)((p)->driverPrivate))
#define MAKE_ATOM(a) MakeAtom(a, sizeof(a) - 1, TRUE)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

#define VENC_PATH "/sys/devices/platform/omapfb/venc_tv_standard"
#define TV_STANDARD_PAL  0
#define TV_STANDARD_NTSC 1
#define TV_STANDARD_DIM  2

#define SUPPRESS_RECONFIG_PROP_NAME	"_MAEMO_SUPPRESS_ROOT_RECONFIGURATION"

#define ENTER() DebugF("Enter %s\n", __FUNCTION__)
#define LEAVE() DebugF("Leave %s\n", __FUNCTION__)

#ifndef max
#define max(x, y) (((x) >= (y)) ? (x) : (y))
#endif

#define ClipValue(v,min,max) ((v) < (min) ? (min) : (v) > (max) ? (max) : (v))

#define VIDEO_IMAGE_MAX_WIDTH 2048
#define VIDEO_IMAGE_MAX_HEIGHT 2048

#endif
