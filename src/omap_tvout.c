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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fbdev.h"
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <linux/omapfb.h>

#include "omap_video.h"
#include "omap_sysfs.h"

struct omap_clone_info {
	int use_dss2;		/* use dss2 sysfs interface? */
	int fd;			/* cloning plane fd */
	int enabled;		/* wheter cloning is enabled or not */
	int id;			/* cloning plane id */
	int srcid;		/* eg. x in /dev/fbx of the source fb */
	int tv_standard;	/* one of the TV_STANDARD_xxx values */
	int tv_widescreen;	/* TV aspect ratio is 16:9 as opposed to 4:3? */
	int tv_standard_changed;
	int tv_widescreen_changed;
	unsigned int maxwidth;
	unsigned int maxheight;
	int tv_scale;		/* Scaling percentage */
};

/* FIXME should be per fbdev instance */
static struct omap_clone_info clone_info = {
	/* use_dss2              */ 0,
	/* fd                    */ -1,
	/* enabled               */ 0,
	/* id                    */ -1,
	/* srcid                 */ -1,
	/* tv_standard           */ TV_STANDARD_PAL,
	/* tv_widescreen         */ FALSE,
	/* tv_standard_changed   */ FALSE,
	/* tv_widescreen_changed */ FALSE,
	/* maxwidth              */ 720,
	/* maxheight   	         */ 574,
	/* tv_scale              */ 90,
};

/* the values below must match their counterparts
   in the kernel driver (venc.c) */
static const struct {
	int maxwidth;
	int maxheight;
} tv_resolution[TV_STANDARD_DIM] = {
	/* TV_STANDARD_PAL  */  {720, 574},
	/* TV_STANDARD_NTSC */  {720, 482},
};

/* these are used in aspect ratio calculations */
static const struct {
	int aspwidth;
	int aspheight;
} tv_aspect_resolution[TV_STANDARD_DIM] = {
	/* TV_STANDARD_PAL  */  {720, 576},
	/* TV_STANDARD_NTSC */  {720, 486},
};

/* magic offsets to center the picture. */
static int tv_xoffset[TV_STANDARD_DIM] = { 6, 30 };

static const struct {
	int xaspect;
	int yaspect;
} tv_aspect[2] = {
	{4, 3,},
	{16, 9,},
};

static const char dss2_overlay[] = "/sys/devices/platform/omapdss/overlay%d/%s";
static const char dss2_display[] = "/sys/devices/platform/omapdss/display%d/%s";
static const char dss2_fb[] = "/sys/devices/platform/omapfb/graphics:fb%d/%s";

static void print_plane(struct omapfb_plane_info *plane)
{
	ErrorF("videoep: setting plane info\n");
	ErrorF("    pos_x      : %d\n", plane->pos_x);
	ErrorF("    pos_y      : %d\n", plane->pos_y);
	ErrorF("    enabled    : %d\n", plane->enabled);
	ErrorF("    channel_out: %d\n", plane->channel_out);
	ErrorF("    mirror     : %d\n", plane->mirror);
	ErrorF("    clone_idx  : %d\n", plane->clone_idx);
	ErrorF("    out_width  : %d\n", plane->out_width);
	ErrorF("    out_height : %d\n", plane->out_height);
}

static int read_tv_standard(void)
{
	char buf[32];
	int ret;

	ret = sysfs_read(VENC_PATH, buf, sizeof buf);
	if (ret)
		return -1;

	if (!strncmp(buf, "pal", 3))
		ret = TV_STANDARD_PAL;
	else if (!strncmp(buf, "ntsc", 4))
		ret = TV_STANDARD_NTSC;
	else
		ret = -1;

	return ret;
}

static int write_tv_standard(int tv_standard)
{
	const char *name;
	int ret;

	switch (tv_standard) {
	case TV_STANDARD_PAL:
		name = "pal";
		break;
	case TV_STANDARD_NTSC:
		name = "ntsc";
		break;
	default:
		return -1;
	}

	ret = sysfs_write(VENC_PATH, name, strlen(name) + 1);
	if (ret)
		return -1;

	return 0;
}

static int dss2_read_tv_standard(void)
{
	char buf[64];
	int ret;

	ret = dss2_read_str(dss2_display, 1, "timings", buf, sizeof buf);
	if (ret)
		return -1;

	if (strstr(buf, ",574/"))
		ret = TV_STANDARD_PAL;
	else if (strstr(buf, ",482/"))
		ret = TV_STANDARD_NTSC;
	else
		ret = -1;

	return ret;
}

static int dss2_write_tv_standard(int tv_standard)
{
	const char *name;
	int ret;

	switch (tv_standard) {
	case TV_STANDARD_PAL:
		name = "pal";
		break;
	case TV_STANDARD_NTSC:
		name = "ntsc";
		break;
	default:
		return -1;
	}

	ret = dss2_write_str(dss2_display, 1, "timings", name);
	if (ret)
		return -1;

	return 0;
}

static int dss1_stop_cloning_plane(void)
{
	struct omapfb_plane_info plane_info;

	ioctl(clone_info.fd, OMAPFB_SYNC_GFX);

	if (ioctl(clone_info.fd, OMAPFB_QUERY_PLANE, &plane_info) < 0) {
		ErrorF("omap/video: couldn't get plane info: %s\n",
		       strerror(errno));
		return -1;
	}

	plane_info.enabled = 0;
	plane_info.clone_idx = 0;

	if (ioctl(clone_info.fd, OMAPFB_SETUP_PLANE, &plane_info) != 0) {
		ErrorF("omap/video: couldn't set plane info: %s\n",
		       strerror(errno));
		print_plane(&plane_info);
	}

	ioctl(clone_info.fd, OMAPFB_SYNC_GFX);

	return 0;
}

static int dss2_stop_cloning_plane(void)
{
	dss2_write_one_int(dss2_overlay, clone_info.id, "enabled", 0);

	dss2_write_str(dss2_overlay, clone_info.id, "manager", "lcd");

	dss2_write_one_int(dss2_fb, clone_info.srcid, "overlays",
			   clone_info.srcid);
	dss2_write_one_int(dss2_fb, clone_info.id, "overlays", clone_info.id);

	return 0;
}

static int stop_cloning_plane(void)
{
	int ret;

	if (!clone_info.enabled || clone_info.id < 0 || clone_info.srcid < 0)
		return 0;

	DebugF("Stop cloning /dev/fb%d\n", clone_info.srcid);

	if (clone_info.use_dss2)
		ret = dss2_stop_cloning_plane();
	else
		ret = dss1_stop_cloning_plane();

	if (ret < 0) {
		ErrorF("omap/video: failed to stop cloning of /dev/fb%d\n",
		       clone_info.srcid);
		return -1;
	}

	clone_info.srcid = -1;
	clone_info.id = -1;

	DebugF("Cloning stopped\n");

	return 0;
}

enum {
	WSS_4_3,
	WSS_16_9,
	WSS_LETTERBOX_14_9,
	WSS_LETTERBOX_16_9,
};

/* IEC 61880 */
static unsigned char crc(unsigned short data)
{
	const unsigned char poly = 0x30;
	unsigned char crc = 0x3f;
	int i;

	for (i = 0; i < 14; i++) {
		if ((crc ^ data) & 1)
			crc = (crc >> 1) ^ poly;
		else
			crc = (crc >> 1);
		data >>= 1;
	}

	return crc;
}

/* IEC 61880 */
static unsigned int ntsc_wss(unsigned char aspect)
{
	static const unsigned char ntsc_aspects[] = {
		[WSS_4_3]            = 0x0,
		[WSS_16_9]           = 0x1,
		[WSS_LETTERBOX_14_9] = 0x0, /* doesn't exist for NTSC */
		[WSS_LETTERBOX_16_9] = 0x2,
	};
	unsigned int wss = 0;

	if (aspect >= ARRAY_SIZE(ntsc_aspects))
		return 0;

	/* word 0 */
	wss |= ntsc_aspects[aspect];
	/* word 1 */
	wss |= 0x0 << 2;
	/* word 2 */
	wss |= 0x00 << 6;
	/* crc */
	wss |= crc(wss) << 14;

	return wss;
}

/* ETSI EN 300 294 */
static unsigned int pal_wss(unsigned char aspect)
{
	static const unsigned char pal_aspects[] = {
		[WSS_4_3]            = 0x8,
		[WSS_16_9]           = 0x7,
		[WSS_LETTERBOX_14_9] = 0x1,
		[WSS_LETTERBOX_16_9] = 0xb,
	};
	unsigned int wss = 0;

	if (aspect >= ARRAY_SIZE(pal_aspects))
		return 0;

	/* group 1 */
	wss |= pal_aspects[aspect];
	/* group 2 */
	wss |= 0x0 << 4;
	/* group 3 */
	wss |= 0x0 << 8;
	/* group 4 */
	wss |= 0x0 << 11;

	return wss;
}

/**
 * Calculate the scaled size taking the
 * source aspect ratio, TV out resolution, and the
 * physical TV aspect ratio into account.
 */
static void calc_scaling(int in_w, int in_h, int *out_x, int *out_y, int *out_w,
			 int *out_h, unsigned int *out_wss)
{
	int maxwidth, maxheight;
	int aspwidth, aspheight;
	int xaspect, yaspect;
	int width, height;
	int widescreen = clone_info.tv_widescreen;
	unsigned char wss_aspect;

	/* Start with the aspect adjusted source size. */
	width = in_w;
	height = in_h;

	if (!widescreen) {
		if (9 * width >= 16 * height)
			wss_aspect = WSS_LETTERBOX_16_9;
		else if (9 * width >= 14 * height)
			wss_aspect = WSS_LETTERBOX_14_9;
		else
			wss_aspect = WSS_4_3;
	} else {
		/*
		 * This gives more horizontal resolution
		 * without sacrificing vertical resolution.
		 */
		if (3 * width <= 4 * height) {
			widescreen = 0;
			wss_aspect = WSS_4_3;
		} else {
			wss_aspect = WSS_16_9;
		}
	}

	switch (clone_info.tv_standard) {
	case TV_STANDARD_NTSC:
		*out_wss = ntsc_wss(wss_aspect);
		break;
	case TV_STANDARD_PAL:
		*out_wss = pal_wss(wss_aspect);
		break;
	default:
		*out_wss = 0;
		break;
	}

	maxwidth = clone_info.maxwidth - tv_xoffset[clone_info.tv_standard];
	maxheight = clone_info.maxheight;

	aspwidth = tv_aspect_resolution[clone_info.tv_standard].aspwidth;
	aspheight = tv_aspect_resolution[clone_info.tv_standard].aspheight;

	xaspect = tv_aspect[widescreen].xaspect;
	yaspect = tv_aspect[widescreen].yaspect;

	/* Make it full-height. */
	if (height != maxheight) {
		width = width * maxheight / height;
		height = maxheight;
	}

	/* Adjust for TV aspect ratio and TV out resolution. */
	width = width * yaspect * aspwidth / (xaspect * aspheight);

	/* Scale it down if it doesn't fit. */
	if (width > maxwidth) {
		height = height * maxwidth / width;
		width = maxwidth;
	}

	/* Apply scaling adjustment. */
	width = width * clone_info.tv_scale / 100;
	height = height * clone_info.tv_scale / 100;

	/* Center it. */
	*out_x = (maxwidth - width) / 2 + tv_xoffset[clone_info.tv_standard];
	*out_y = (maxheight - height) / 2;
	*out_w = width;
	*out_h = height;

	DebugF("omap/video: Cloning %dx%d -> %dx%d (TV: %s %d:%d)\n", in_w,
	       in_h, width, height, clone_info.tv_standard ? "NTSC" : "PAL",
	       xaspect, yaspect);
}

static int dss1_start_cloning_plane(FBDevPtr fbdev, int srcid)
{
	struct omapfb_plane_info plane_info;
	struct omapfb_plane_info srcplane;
	int width, height;
	int xoffs, yoffs;
	int clone_id, clone_fd, srcfd;
	unsigned int wss;

	if (srcid > OMAPFB_CLONE_MASK) {
		ErrorF("omap/video: Invalid device /dev/fb%d to clone\n",
		       srcid);
		return -1;
	}

	srcfd = srcid ? omap_video_get_plane_fd(fbdev, srcid - 1) : fbdev->fd;
	if (srcfd < 0)
		return -1;

	clone_id = omap_video_get_free_plane(fbdev) + 1;
	if (!clone_id)
		return -1;

	clone_fd = omap_video_get_plane_fd(fbdev, clone_id - 1);
	if (clone_fd < 0)
		return -1;

	/* The driver will refuse TV standard changes while venc is enabled. */
	if (clone_info.srcid >= 0 && clone_info.tv_standard_changed)
		stop_cloning_plane();

	if (clone_info.tv_standard_changed) {
		if (!write_tv_standard(clone_info.tv_standard))
			clone_info.tv_standard_changed = FALSE;
	}

	DebugF("Start cloning /dev/fb%d\n", srcid);

	if (ioctl(srcfd, OMAPFB_QUERY_PLANE, &srcplane) < 0) {
		ErrorF("omap/video: couldn't get plane info: %s\n",
		       strerror(errno));
		srcplane.out_width = 800;
		srcplane.out_height = 480;
	}

	calc_scaling(srcplane.out_width, srcplane.out_height, &xoffs, &yoffs,
		     &width, &height, &wss);

	if (ioctl(clone_fd, OMAPFB_QUERY_PLANE, &plane_info) != 0) {
		ErrorF("omap/video: couldn't get plane info: %s\n",
		       strerror(errno));
		return -1;
	}

	plane_info.pos_x = xoffs;
	plane_info.pos_y = yoffs;
	plane_info.enabled = 1;
	plane_info.channel_out = OMAPFB_CHANNEL_OUT_DIGIT;
	plane_info.clone_idx = OMAPFB_CLONE_ENABLED | srcid;
	plane_info.out_width = width;
	plane_info.out_height = height;

	if (ioctl(clone_fd, OMAPFB_SETUP_PLANE, &plane_info) != 0) {
		ErrorF("omap/video: couldn't set plane info: %s\n",
		       strerror(errno));
		print_plane(&plane_info);
		return -1;
	}

	clone_info.fd = clone_fd;
	clone_info.id = clone_id;
	clone_info.srcid = srcid;
	clone_info.tv_widescreen_changed = FALSE;

	DebugF("Cloning succeeded\n");

	return 0;
}

static int dss2_start_cloning_plane(FBDevPtr fbdev, int srcid)
{
	int srcw = 800, srch = 480;
	int rotate = 0, ovl_rotate = 0;
	int width, height;
	int xoffs, yoffs;
	int id;
	unsigned int wss;

	id = omap_video_get_free_plane(fbdev) + 1;
	if (!id)
		return -1;

	/* The output size will change when TV standard is changed so
	 * the plane coordinates may not fit the output. To avoid problems
	 * just stop cloning before changing the TV standard.
	 */
	if (clone_info.srcid != srcid || clone_info.tv_standard_changed)
		stop_cloning_plane();

	dss2_read_two_ints(dss2_overlay, srcid, "output_size", &srcw, &srch);
	dss2_read_one_int(dss2_fb, srcid, "rotate", &rotate);
	dss2_read_one_int(dss2_fb, srcid, "overlays_rotate", &ovl_rotate);

	if (((rotate + ovl_rotate) % 4) & 1) {
		int tmp = srcw;
		srcw = srch;
		srch = tmp;
	}

	calc_scaling(srcw, srch, &xoffs, &yoffs, &width, &height, &wss);

	if (clone_info.tv_standard_changed)
		dss2_write_tv_standard(clone_info.tv_standard);

	if (clone_info.srcid != srcid || clone_info.tv_standard_changed) {
		dss2_write_str(dss2_fb, id, "overlays", "");
		dss2_write_two_ints(dss2_fb, srcid, "overlays", srcid, id);
		dss2_write_two_ints(dss2_fb, srcid, "overlays_rotate",
				    ovl_rotate, (4 - rotate) % 4);
		dss2_write_str(dss2_overlay, id, "manager", "tv");
	}

	/* Workaround: prevent the plane from moving offscreen */
	dss2_write_two_ints(dss2_overlay, id, "position", 0, 0);
	dss2_write_two_ints(dss2_overlay, id, "output_size", width, height);
	dss2_write_two_ints(dss2_overlay, id, "position", xoffs, yoffs);

	if (clone_info.srcid != srcid || clone_info.tv_standard_changed) {
		dss2_write_one_int(dss2_overlay, id, "enabled", 1);
	}

	dss2_write_one_int(dss2_display, 1, "wss", wss);

	clone_info.id = id;
	clone_info.srcid = srcid;
	clone_info.tv_standard_changed = FALSE;
	clone_info.tv_widescreen_changed = FALSE;

	DebugF("Cloning succeeded\n");

	return 0;
}

static int start_cloning_plane(FBDevPtr fbdev, int srcid)
{
	if (!clone_info.enabled)
		return 0;

	if (srcid < 0)
		return -1;

	/* Already cloning the same plane? */
	if (clone_info.srcid == srcid && !clone_info.tv_standard_changed
	    && !clone_info.tv_widescreen_changed)
		return 0;

	if (clone_info.use_dss2)
		return dss2_start_cloning_plane(fbdev, srcid);
	else
		return dss1_start_cloning_plane(fbdev, srcid);
}

static void start_cloning(FBDevPtr fbdev)
{
	int srcid;

	srcid = omap_video_get_active_plane(fbdev) + 1;

	start_cloning_plane(fbdev, srcid);
}

static void enable_tvout(void)
{
	if (!clone_info.use_dss2)
		return;

	dss2_write_one_int(dss2_display, 1, "enabled", 1);
}

static void enable_cloning(FBDevPtr fbdev)
{
	if (clone_info.enabled)
		return;

	DebugF("Enable cloning\n");

	clone_info.enabled = 1;

	enable_tvout();

	start_cloning(fbdev);
}

static void disable_tvout(void)
{
	if (!clone_info.use_dss2)
		return;

	dss2_write_one_int(dss2_display, 1, "enabled", 0);
}

static void disable_cloning(void)
{
	if (!clone_info.enabled)
		return;

	DebugF("Disable cloning\n");

	stop_cloning_plane();

	disable_tvout();

	clone_info.enabled = 0;
}

int omap_tvout_src_plane(FBDevPtr fbdev)
{
	return clone_info.srcid;
}

int omap_tvout_clone_plane(FBDevPtr fbdev)
{
	return clone_info.id;
}

int omap_tvout_enabled(FBDevPtr fbdev)
{
	return clone_info.enabled;
}

int omap_tvout_tv_standard(FBDevPtr fbdev)
{
	return clone_info.tv_standard;
}

int omap_tvout_tv_widescreen(FBDevPtr fbdev)
{
	return clone_info.tv_widescreen;
}

int omap_tvout_tv_scale(FBDevPtr fbdev)
{
	return clone_info.tv_scale;
}

int omap_tvout_enable(FBDevPtr fbdev, int value)
{
	if (value != 0 && value != 1)
		return BadValue;

	if (value)
		enable_cloning(fbdev);
	else
		disable_cloning();

	return Success;
}

int omap_tvout_set_tv_standard(FBDevPtr fbdev, int value)
{
	if (value != TV_STANDARD_PAL && value != TV_STANDARD_NTSC)
		return BadValue;

	if (clone_info.tv_standard == value)
		return Success;

	clone_info.tv_standard = value;
	clone_info.tv_standard_changed = TRUE;
	clone_info.maxwidth = tv_resolution[value].maxwidth;
	clone_info.maxheight = tv_resolution[value].maxheight;

	if (clone_info.enabled)
 		start_cloning(fbdev);

	return Success;
}

int omap_tvout_set_tv_widescreen(FBDevPtr fbdev, int value)
{
	if (value != 0 && value != 1)
		return BadValue;

	if (clone_info.tv_widescreen == value)
		return Success;

	clone_info.tv_widescreen = value;
	clone_info.tv_widescreen_changed = TRUE;

	if (clone_info.enabled)
 		start_cloning(fbdev);

	return Success;
}

int omap_tvout_set_tv_scale(FBDevPtr fbdev, int value)
{
	if (value < 1 || value > 100)
		return BadValue;

	if (clone_info.tv_scale == value)
		return Success;

	clone_info.tv_scale = value;
	clone_info.tv_widescreen_changed = TRUE;

	if (clone_info.enabled)
 		start_cloning(fbdev);

	return Success;
}

void omap_tvout_stop(FBDevPtr fbdev)
{
	if (!clone_info.enabled)
		return;

	stop_cloning_plane();
}

void omap_tvout_resume(FBDevPtr fbdev)
{
	if (!clone_info.enabled)
		return;

	start_cloning(fbdev);
}

void omap_tvout_init(void)
{
	int tv_standard;
	int tmp;

	if (!dss2_read_one_int(dss2_overlay, 0, "enabled", &tmp)) {
		clone_info.use_dss2 = 1;

		tv_standard = dss2_read_tv_standard();
		if (tv_standard < 0)
			tv_standard = 0;
        } else {
		tv_standard = read_tv_standard();
		if (tv_standard < 0)
			tv_standard = 0;
	}

	clone_info.tv_standard = tv_standard;
	clone_info.maxwidth = tv_resolution[tv_standard].maxwidth;
	clone_info.maxheight = tv_resolution[tv_standard].maxheight;
}
