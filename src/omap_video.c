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

#include <X11/Xatom.h>
#include <X11/extensions/Xv.h>
#include "fourcc.h"
#include "damage.h"

#include "windowstr.h"
#include "xf86Crtc.h"
#include "omap_video.h"
#include "omap_video_formats.h"
#include "omap_tvout.h"
#include "omap_procfs.h"
#include "sgx_xv.h"

struct omap_video_info {
	/* Immutable port/plane properties. */
	int id;
	int fd;
	CARD8 *mem;
	int mem_size;
	unsigned long caps;
	int manual_updates;

	/* Mutable video properties. */
	int dirty;

	DrawablePtr drawable;
	unsigned int visibility;
	RegionRec clip;
	Pixel ckey;
	int autopaint_ckey;
	int disable_ckey;

	enum {
		OMAP_VSYNC_NONE,
		OMAP_VSYNC_TEAR,
		OMAP_VSYNC_FORCE,
	} vsync;

	enum {
		OMAP_STATE_STOPPED,
		OMAP_STATE_ACTIVE,
	} state;

	int fourcc;
	short src_w, src_h;
	short dst_x, dst_y, dst_w, dst_h, dst_pitch;
	int hscale, vscale;

	/* Internal bits. */

	/* Enable color keying after a while if no frames were pushed. */
	OsTimerPtr ckey_timer;

	FBDevPtr fbdev;

	/* Hack: to be removed */
	int overlay_active;

	struct fb_var_screeninfo var;
	int double_buffer;
};
#define get_omap_video_info(fbdev, n) ((fbdev)->overlay_adaptor->pPortPrivates[n].ptr)

static XF86VideoEncodingRec DummyEncoding = {
	0, "XV_IMAGE", VIDEO_IMAGE_MAX_WIDTH, VIDEO_IMAGE_MAX_HEIGHT, {1, 1},
};

static XF86VideoFormatRec xv_formats[] = {
	{16, TrueColor},
	{24, TrueColor},
};

static XF86AttributeRec xv_attributes[] = {
	{XvSettable | XvGettable, OMAP_VSYNC_NONE, OMAP_VSYNC_FORCE,
	 "XV_OMAP_VSYNC"},
	{XvSettable | XvGettable, 0, 0xffffff, "XV_COLORKEY"},
	{XvSettable | XvGettable, 0, 1, "XV_AUTOPAINT_COLORKEY"},
	{XvSettable | XvGettable, 0, 1, "XV_DISABLE_COLORKEY"},
	{XvSettable | XvGettable, 0, 1, "XV_DOUBLE_BUFFER"},
	{XvSettable | XvGettable, 0, 1, "XV_OMAP_CLONE_TO_TVOUT"},
	{XvSettable | XvGettable, 0, 1, "XV_OMAP_TVOUT_STANDARD"},
	{XvSettable | XvGettable, 0, 1, "XV_OMAP_TVOUT_WIDESCREEN"},
	{XvSettable | XvGettable, 1, 100, "XV_OMAP_TVOUT_SCALE"},
	{XvGettable, 0, 1, "XV_OMAP_OVERLAY_ACTIVE"},
};

static Atom xv_ckey, xv_autopaint_ckey, xv_disable_ckey, xv_vsync;
static Atom xv_omap_clone_to_tvout, xv_omap_tvout_standard;
static Atom xv_omap_tvout_widescreen, xv_omap_tvout_scale;
static Atom xv_omap_overlay_active, xv_double_buffer;

static Atom _omap_video_overlay;	/* Window property, not Xv property. */

/* THIS IS NOT A ONE-TO-ONE MAP. */
static struct {
	XF86ImageRec xv_format;
	enum omapfb_color_format omapfb_format;
} video_format_map[] = {
	{XVIMAGE_YUY2, OMAPFB_COLOR_YUY422},
	{XVIMAGE_UYVY, OMAPFB_COLOR_YUV422},
	{XVIMAGE_I420, OMAPFB_COLOR_YUY422},
	{XVIMAGE_YV12, OMAPFB_COLOR_YUY422},
	{XVIMAGE_RV16, 0},
	{XVIMAGE_RV32, 0},
};

#define FB_FORMATS_MASK ((1 << OMAPFB_COLOR_YUY422) | \
                         (1 << OMAPFB_COLOR_YUV422) | \
                         (1 << OMAPFB_COLOR_YUV420))

#define OMAP_YV12_PITCH_LUMA(w)   (((w) + 3) & ~3)
#define OMAP_YV12_PITCH_CHROMA(w) (((OMAP_YV12_PITCH_LUMA(w) >> 1) + 3) & ~3)
#define OMAP_YUY2_PITCH(w)        ((((w) + 1) & ~1) << 1)
#define OMAP_RV16_PITCH(w)        ((w) << 1)
#define OMAP_RV32_PITCH(w)        ((w) << 2)

/*
 * Constants for calculating the maximum image
 * size which can fit in our video memory.
 */
enum {
	MAX_PLANES          = 2,
	MAX_BUFFERS         = 2,
	MAX_BYTES_PER_PIXEL = 2,
};

static XF86ImageRec xv_images[ARRAY_SIZE(video_format_map)];
static int num_xv_images = 0;

static void sync_gfx(FBDevPtr fbdev)
{
	ioctl(fbdev->fd, OMAPFB_SYNC_GFX);
}

static enum omapfb_color_format get_omapfb_format(struct omap_video_info
						  *video_info)
{
	int i;

	if (video_info->fourcc == FOURCC_RV16 ||
	    video_info->fourcc == FOURCC_RV32)
		return 0;

	for (i = 0; i < ARRAY_SIZE(video_format_map); i++) {
		if (video_format_map[i].xv_format.id == video_info->fourcc) {
			return video_format_map[i].omapfb_format;
		}
	}

	return 0;
}

static int get_bpp(struct omap_video_info *video_info)
{
	if (video_info->fourcc == FOURCC_RV16)
		return 16;
	else if (video_info->fourcc == FOURCC_RV32)
		return 32;
	else
		return 0;
}

static unsigned int calc_required_mem(struct omap_video_info *video_info)
{
	int src_w, src_h;
	int ret = 0;

	if (video_info->hscale)
		src_w = video_info->dst_w;
	else
		src_w = video_info->src_w;

	if (video_info->vscale)
		src_h = video_info->dst_h;
	else
		src_h = video_info->src_h;

	/* The kernel will round anything smaller to 8 pixels. */
	src_w = max(src_w, 8);
	src_h = max(src_h, 8);

	switch (video_info->fourcc) {
	case FOURCC_YV12:
	case FOURCC_I420:
	case FOURCC_YUY2:
	case FOURCC_UYVY:
		ret = OMAP_YUY2_PITCH(src_w) * src_h;
		break;
	case FOURCC_RV16:
		ret = OMAP_RV16_PITCH(src_w) * src_h;
		break;
	case FOURCC_RV32:
		ret = OMAP_RV32_PITCH(src_w) * src_h;
		break;
	}

	return ret << video_info->double_buffer;
}

static unsigned int get_mem_size(struct omap_video_info *video_info)
{
	struct omapfb_mem_info mem_info;

	if (ioctl(video_info->fd, OMAPFB_QUERY_MEM, &mem_info) != 0)
		return 0;

	return mem_info.size;
}

static int alloc_plane_mem(struct omap_video_info *video_info,
			   unsigned int size)
{
	struct omapfb_mem_info mem_info;

	/* Not everyone has (enough) SRAM, and allocating into SRAM usually
	 * ends very badly anyway (kernel bugs), so we hardcode SDRAM here. */
	mem_info.type = OMAPFB_MEMTYPE_SDRAM;
	mem_info.size = size;

	if (ioctl(video_info->fd, OMAPFB_SETUP_MEM, &mem_info) != 0)
		return 0;

	return 1;
}

/**
 * Check if plane attributes have changed.
 */
static _X_INLINE int is_dirty(struct omap_video_info *video_info, int fourcc,
			      int src_w, int src_h, int dst_x, int dst_y,
			      int dst_w, int dst_h)
{
	if (video_info->dirty || video_info->fourcc != fourcc
	    || video_info->src_w != src_w || video_info->src_h != src_h
	    || video_info->dst_x != dst_x || video_info->dst_y != dst_y
	    || video_info->dst_w != dst_w || video_info->dst_h != dst_h)
		return 1;
	else
		return 0;
}

/**
 * Consider the given position (x, y) and dimensions (w, h) and adjust (clip)
 * the final target values to fit the screen dimensions.
 */
static Bool clip_image_to_fit(short *src_x, short *src_y, short *dst_x,
			      short *dst_y, short *src_w, short *src_h,
			      short *dst_w, short *dst_h, short width,
			      short height, RegionPtr clip_boxes)
{
	BoxRec dstBox;
	long xa = *src_x;
	long xb = *src_x + *src_w;
	long ya = *src_y;
	long yb = *src_y + *src_h;

	dstBox.x1 = *dst_x;
	dstBox.x2 = *dst_x + *dst_w;
	dstBox.y1 = *dst_y;
	dstBox.y2 = *dst_y + *dst_h;

#if 0
	DebugF("omap/clip: before (%d, %d, %d, %d) to (%d, %d, %d, %d)\n",
		*src_x, *src_y, *src_w, *src_h, *dst_x, *dst_y, *dst_w, *dst_h);
#endif

	/* Failure here means simply that one is requesting the image to be
	 * positioned entirely outside the screen. Existing video drivers seem to
	 * treat this as implicit success, and hence this behaviour is adopted here
	 * also.
	 */
	if (!xf86XVClipVideoHelper
	    (&dstBox, &xa, &xb, &ya, &yb, clip_boxes, width, height))
		return FALSE;

	/* This should be taken cared of by 'xf86XVClipVideoHelper()', but for
	 * safety's sake, one re-checks.
	 */
	if (dstBox.x2 <= dstBox.x1 || dstBox.y2 <= dstBox.y1)
		return FALSE;

	*dst_x = dstBox.x1;
	*dst_w = dstBox.x2 - dstBox.x1;
	*dst_y = dstBox.y1;
	*dst_h = dstBox.y2 - dstBox.y1;

 	*src_x = xa >> 16;
 	*src_w = (xb - xa) >> 16;
 	*src_y = ya >> 16;
 	*src_h = (yb - ya) >> 16;

#if 0
	DebugF("omap/clip: after (%d, %d, %d, %d) to (%d, %d, %d, %d)\n",
		*src_x, *src_y, *src_w, *src_h, *dst_x, *dst_y, *dst_w, *dst_h);
#endif

	return TRUE;
}

static CARD32 default_ckey(ScrnInfoPtr pScrn)
{
	return (((0x00 << pScrn->offset.red) & pScrn->mask.red) |
		((0xff << pScrn->offset.green) & pScrn->mask.green) |
		((0x00 << pScrn->offset.blue) & pScrn->mask.blue));
}

static int setup_colorkey(struct omap_video_info *video_info,
			  Bool force_enable)
{
	ScreenPtr screen = video_info->fbdev->screen;
	ScrnInfoPtr xf86screen = xf86Screens[screen->myNum];
	struct omapfb_color_key ckey;

	if (ioctl(video_info->fd, OMAPFB_GET_COLOR_KEY, &ckey) != 0) {
		ErrorF("omap/video: couldn't get colourkey info\n");
		return 0;
	}

	ckey.trans_key = video_info->ckey & (xf86screen->mask.red |
					     xf86screen->mask.green |
					     xf86screen->mask.blue);
	ckey.key_type = (video_info->disable_ckey && !force_enable) ?
		OMAPFB_COLOR_KEY_DISABLED : OMAPFB_COLOR_KEY_GFX_DST;
	ckey.channel_out = OMAPFB_CHANNEL_OUT_LCD;
	ckey.background = 0;

	if (ioctl(video_info->fd, OMAPFB_SET_COLOR_KEY, &ckey) != 0) {
		ErrorF("omap/video: couldn't set colourkey info\n");
		return 0;
	}

	return 1;
}

/**
 * Enable color keying after a while so that a misbehaving client
 * can't prevent popup notifications indefinitely.
 */
static CARD32 ckey_timer(OsTimerPtr timer, CARD32 now, pointer data)
{
	struct omap_video_info *video_info = data;

	DebugF("omap/video: ckey_timer: timeout on plane %d\n",
	       video_info->id);

	TimerFree(video_info->ckey_timer);
	video_info->ckey_timer = NULL;

	/* Enable color keying */
	setup_colorkey(video_info, TRUE);

	/*
	 * Make sure color keying gets disabled again
	 * when the client resumes sending frames.
	 */
	video_info->dirty = TRUE;

	return 0;
}

static void cancel_ckey_timer(struct omap_video_info *video_info)
{
	if (!video_info->ckey_timer)
		return;

	TimerFree(video_info->ckey_timer);
	video_info->ckey_timer = NULL;
}

static void set_ckey_timer(struct omap_video_info *video_info)
{
	video_info->ckey_timer = TimerSet(video_info->ckey_timer, 0, 1000,
					  ckey_timer, video_info);
}

/**
 * Sets up a video plane.
 */
static int setup_plane(struct omap_video_info *video_info)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	struct omapfb_plane_info plane_info;
	unsigned int src_w, src_h, dst_w, dst_h;

	/* Do we need to reallocate? */
	if (get_mem_size(video_info) != calc_required_mem(video_info)) {
		if (!alloc_plane_mem(video_info, calc_required_mem(video_info))) {
			ErrorF
			    ("omap/video: couldn't allocate memory for video plane!\n");
			goto unwind_start;
		}
	}

	/* Hilariously, this will actually trigger upscaling for odd widths,
	 * since we'll output n-1 pixels, and have the engine scale to n;
	 * dispc hangs if you try to feed it sub-macroblocks.
	 *
	 * When feeding YUV420 to the external controller, we have 4x2
	 * macroblocks in essence, so lop off up to the last three lines, and
	 * let the hardware scale.
	 */
	if (video_info->hscale)
		src_w = video_info->dst_w;
	else
		src_w = video_info->src_w;

	if (video_info->vscale)
		src_h = video_info->dst_h;
	else
		src_h = video_info->src_h;

	switch (video_info->fourcc) {
	case FOURCC_YV12:
	case FOURCC_I420:
	case FOURCC_YUY2:
	case FOURCC_UYVY:
		src_w &= ~1;
		break;
	default:
		break;
	}

	dst_w = video_info->dst_w;
	dst_h = video_info->dst_h;

	if (ioctl(video_info->fd, FBIOGET_VSCREENINFO, &var) != 0) {
		ErrorF("omap/video: couldn't get var info\n");
		goto unwind_mem;
	}
	var.xres = src_w;
	var.yres = src_h;
	var.xres_virtual = max(src_w, 8);
	var.yres_virtual = max(src_h, 8) << video_info->double_buffer;
	var.xoffset = 0;
	var.yoffset = 0;
	var.rotate = 0;		/* nb: relative to gfx */
	var.grayscale = 0;
	var.bits_per_pixel = get_bpp(video_info);
	var.nonstd = get_omapfb_format(video_info);
	var.activate = FB_ACTIVATE_NOW;
	memset(&var.red, 0, sizeof var.red);
	memset(&var.green, 0, sizeof var.green);
	memset(&var.blue, 0, sizeof var.blue);
	memset(&var.transp, 0, sizeof var.transp);
	if (ioctl(video_info->fd, FBIOPUT_VSCREENINFO, &var) != 0) {
		ErrorF("omap/video: couldn't set var info\n");
		goto unwind_mem;
	}

	video_info->var = var;

	if (!setup_colorkey(video_info, FALSE))
		goto unwind_mem;

	if (ioctl(video_info->fd, OMAPFB_QUERY_PLANE, &plane_info) != 0) {
		ErrorF("omap/video: couldn't get plane info\n");
		goto unwind_mem;
	}

	plane_info.pos_x = video_info->dst_x;
	plane_info.pos_y = video_info->dst_y;
	plane_info.enabled = 0;
	plane_info.channel_out = OMAPFB_CHANNEL_OUT_LCD;
	plane_info.clone_idx = 0;
	plane_info.out_width = dst_w;
	plane_info.out_height = dst_h;

	if (ioctl(video_info->fd, OMAPFB_SETUP_PLANE, &plane_info) != 0) {
		ErrorF("omap/video: couldn't set plane info\n");
		goto unwind_mem;
	}

	if (ioctl(video_info->fd, FBIOGET_FSCREENINFO, &fix) != 0) {
		ErrorF("omap/video: couldn't get fixed info\n");
		goto unwind_setup;
	}
	video_info->mem =
	    mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		 video_info->fd, 0L);
	if (video_info->mem == MAP_FAILED) {
		ErrorF("omap/video: couldn't mmap plane\n");
		goto unwind_setup;
	}
	video_info->mem += fix.smem_start % getpagesize();
	video_info->mem_size = fix.smem_len;
	video_info->dst_pitch = fix.line_length;
	video_info->dirty = 0;

	return 1;

unwind_setup:
	plane_info.enabled = 0;
	plane_info.clone_idx = 0;
	(void)ioctl(video_info->fd, OMAPFB_SETUP_PLANE, &plane_info);
unwind_mem:
	(void)alloc_plane_mem(video_info, 0);
unwind_start:
	return 0;
}

static int flip_plane(struct omap_video_info *video_info)
{
	if (ioctl(video_info->fd, FBIOPAN_DISPLAY, &video_info->var))
		return 0;

	video_info->var.yoffset = video_info->var.yoffset ? 0 : video_info->var.yres;

	return 1;
}

/*
 * Enabled the plane
 */
static int enable_plane(struct omap_video_info *video_info)
{
	struct omapfb_plane_info plane_info;

	if (ioctl(video_info->fd, OMAPFB_QUERY_PLANE, &plane_info) != 0) {
		ErrorF("omap/video: couldn't get plane info\n");
		goto fail;
	}

	plane_info.enabled = 1;

	if (ioctl(video_info->fd, OMAPFB_SETUP_PLANE, &plane_info) != 0) {
		ErrorF("omap/video: couldn't set plane info\n");
		goto fail;
	}

	omap_tvout_resume(video_info->fbdev);

	return 1;

 fail:
	omap_tvout_resume(video_info->fbdev);

	return 0;
}

/**
 * Does what it says on the box.
 */
static void disable_plane(struct omap_video_info *video_info)
{
	struct omapfb_plane_info plane_info;

	if (video_info->mem)
		munmap(video_info->mem, video_info->mem_size);

	video_info->mem = NULL;
	video_info->mem_size = 0;
	video_info->dst_pitch = 0;

	if (ioctl(video_info->fd, OMAPFB_QUERY_PLANE, &plane_info) != 0) {
		ErrorF("omap/video: couldn't get plane info\n");
		return;
	}

	plane_info.enabled = 0;
	plane_info.clone_idx = 0;

	if (ioctl(video_info->fd, OMAPFB_SETUP_PLANE, &plane_info) != 0) {
		ErrorF("omap/video: couldn't disable plane\n");
		return;
	}

	if (!alloc_plane_mem(video_info, 0)) {
		ErrorF("omap/video: couldn't deallocate plane\n");
		return;
	}
}

static void drawable_destroyed(FBDevPtr fbdev, DrawablePtr drawable)
{
	int i;
	struct omap_video_info *video_info;

	for (i = 0; i < fbdev->num_video_ports; i++) {
		video_info = get_omap_video_info(fbdev, i);
		if (video_info->drawable == drawable)
			video_info->drawable = NULL;
	}
}

static Bool destroy_pixmap_hook(PixmapPtr pixmap)
{
	Bool ret;
	ScreenPtr screen = pixmap->drawable.pScreen;
	ScrnInfoPtr xf86screen = xf86Screens[screen->myNum];
	FBDevPtr fbdev = xf86screen->driverPrivate;

	drawable_destroyed(fbdev, (DrawablePtr) pixmap);

	screen->DestroyPixmap = fbdev->video_destroy_pixmap;
	ret = screen->DestroyPixmap(pixmap);
	screen->DestroyPixmap = destroy_pixmap_hook;

	return ret;
}

static Bool destroy_window_hook(WindowPtr window)
{
	Bool ret;
	ScreenPtr screen = window->drawable.pScreen;
	ScrnInfoPtr xf86screen = xf86Screens[screen->myNum];
	FBDevPtr fbdev = xf86screen->driverPrivate;

	drawable_destroyed(fbdev, (DrawablePtr) window);

	screen->DestroyWindow = fbdev->video_destroy_window;
	ret = screen->DestroyWindow(window);
	screen->DestroyWindow = destroy_window_hook;

	return ret;
}

static void change_overlay_property(struct omap_video_info *video_info, int val)
{
	WindowPtr window;
	int err;

	if (video_info->drawable
	    && video_info->drawable->type == DRAWABLE_WINDOW) {
		/* Walk the tree to get the top-level window. */
		for (window = (WindowPtr) video_info->drawable;
		     window && window->parent; window = window->parent) {
			err =
			    ChangeWindowProperty(window, _omap_video_overlay,
						 XA_INTEGER, 8, PropModeReplace,
						 1, &val, FALSE);
			if (err != Success)
				ErrorF
				    ("change_overlay_property: failed to change property\n");
		}
	} else {
		if (video_info->drawable)
			DebugF
			    ("change_overlay_property: not changing property: type is %d\n",
			     video_info->drawable->type);
		else
			DebugF
			    ("change_overlay_property: not changing property: no overlay\n");
	}

	video_info->overlay_active = val;
}

static void empty_clip(struct omap_video_info *video_info)
{
	change_overlay_property(video_info, 0);

	if (REGION_NOTEMPTY(video_info->fbdev->screen, &video_info->clip)) {
		if (video_info->drawable && video_info->autopaint_ckey) {
			RegionRec clip;
			REGION_INIT(video_info->fbdev->screen, &clip, NullBox, 0);
			REGION_COPY(video_info->fbdev->screen, &clip, &video_info->clip);
			xf86XVFillKeyHelperDrawable(video_info->drawable, video_info->fbdev->screen->blackPixel, &clip);
			DamageDamageRegion(video_info->drawable, &clip);
		}
		video_info->drawable = NULL;
		REGION_EMPTY(video_info->fbdev->screen, &video_info->clip);
	}
}

/**
 * Stop the video overlay.
 */
static void stop_video(struct omap_video_info *video_info)
{
	/* Stop TV-out if it's cloning this plane. */
	if (omap_tvout_src_plane(video_info->fbdev) == video_info->id + 1)
		omap_tvout_stop(video_info->fbdev);

	change_overlay_property(video_info, 0);

	if (video_info->visibility >= VisibilityFullyObscured)
		empty_clip(video_info);

	if (video_info->state == OMAP_STATE_ACTIVE)
		disable_plane(video_info);

	video_info->state = OMAP_STATE_STOPPED;

	cancel_ckey_timer(video_info);

	DebugF("omap stop_video: stopped plane %d\n", video_info->id);
}

static void push_frame(struct omap_video_info *video_info)
{
	struct omapfb_update_window update_window;

	update_window.x = 0;
	update_window.y = 0;
	update_window.out_x = 0;
	update_window.out_y = 0;
	update_window.out_width = video_info->dst_w;
	update_window.out_width = video_info->dst_h;

	update_window.format = get_omapfb_format(video_info);
	update_window.width = video_info->dst_w;
	update_window.width = video_info->dst_h;

	if (video_info->vsync == OMAP_VSYNC_TEAR)
		update_window.format |= OMAPFB_FORMAT_FLAG_TEARSYNC;
	else if (video_info->vsync == OMAP_VSYNC_FORCE)
		update_window.format |= OMAPFB_FORMAT_FLAG_FORCE_VSYNC;

	/* This fails when the screen's off, so ignore it. */
	(void)ioctl(video_info->fd, OMAPFB_UPDATE_WINDOW, &update_window);
}

/**
 * When the clip on a window changes, check it and stash it away, so we
 * don't end up with any clipped windows on the external controller.
 */
static void omap_video_clip_notify(ScrnInfoPtr screen, void *data,
				   WindowPtr window, int dx, int dy)
{
	struct omap_video_info *video_info = data;

	video_info->visibility = window->visibility;
}

/**
 * Xv attributes get/set support.
 */
static int omap_video_get_attribute(ScrnInfoPtr screen, Atom attribute,
				    INT32 * value, pointer data)
{
	struct omap_video_info *video_info = data;

	ENTER();

	if (attribute == xv_vsync) {
		*value = video_info->vsync;
		LEAVE();
		return Success;
	} else if (attribute == xv_double_buffer) {
		*value = video_info->double_buffer;
		LEAVE();
		return Success;
	} else if (attribute == xv_ckey) {
		*value = video_info->ckey;
		LEAVE();
		return Success;
	} else if (attribute == xv_autopaint_ckey) {
		*value = video_info->autopaint_ckey;
		LEAVE();
		return Success;
	} else if (attribute == xv_disable_ckey) {
		*value = video_info->disable_ckey;
		LEAVE();
		return Success;
	} else if (attribute == xv_omap_clone_to_tvout) {
		*value = omap_tvout_enabled(video_info->fbdev);
		LEAVE();
		return Success;
	} else if (attribute == xv_omap_tvout_standard) {
		*value = omap_tvout_tv_standard(video_info->fbdev);
		LEAVE();
		return Success;
	} else if (attribute == xv_omap_tvout_widescreen) {
		*value = omap_tvout_tv_widescreen(video_info->fbdev);
		LEAVE();
		return Success;
	} else if (attribute == xv_omap_tvout_scale) {
		*value = omap_tvout_tv_scale(video_info->fbdev);
		LEAVE();
		return Success;
	} else if (attribute == xv_omap_overlay_active) {
		*value = video_info->overlay_active;
		LEAVE();
		return Success;
	}

	LEAVE();
	return BadMatch;
}

static int omap_video_set_attribute(ScrnInfoPtr screen, Atom attribute,
				    INT32 value, pointer data)
{
	struct omap_video_info *video_info = data;

	ENTER();

	if (attribute == xv_vsync) {
		if (value < OMAP_VSYNC_NONE || value > OMAP_VSYNC_FORCE) {
			LEAVE();
			return BadValue;
		}

		if (!(video_info->caps & OMAPFB_CAPS_TEARSYNC) && value) {
			ErrorF
			    ("omap_video_set_attribute: requested vsync on a non-sync "
			     "capable port\n");
			LEAVE();
			return BadValue;
		}

		video_info->vsync = value;
		LEAVE();
		return Success;
	} else if (attribute == xv_double_buffer) {
		if (value != 0 && value != 1) {
			LEAVE();
			return BadValue;
		}

		video_info->double_buffer = value;
		video_info->dirty = TRUE;
		LEAVE();
		return Success;
	} else if (attribute == xv_ckey) {
		if (value < 0 || value > 0xffffff) {
			LEAVE();
			return BadValue;
		}

		video_info->ckey = value;
		video_info->dirty = TRUE;
		LEAVE();
		return Success;
	} else if (attribute == xv_autopaint_ckey) {
		if (value != 0 && value != 1) {
			LEAVE();
			return BadValue;
		}

		video_info->autopaint_ckey = value;
		video_info->dirty = TRUE;
		LEAVE();
		return Success;
	} else if (attribute == xv_disable_ckey) {
		if (value != 0 && value != 1) {
			LEAVE();
			return BadValue;
		}

		video_info->disable_ckey = value;
		if (video_info->state == OMAP_STATE_ACTIVE) {
			setup_colorkey(video_info, FALSE);
			cancel_ckey_timer(video_info);
			if (value)
				set_ckey_timer(video_info);
		} else
			video_info->dirty = TRUE;
		LEAVE();
		return Success;
	} else if (attribute == xv_omap_clone_to_tvout) {
		int ret = omap_tvout_enable(video_info->fbdev, value);
		LEAVE();
		return ret;
	} else if (attribute == xv_omap_tvout_standard) {
		int ret = omap_tvout_set_tv_standard(video_info->fbdev, value);
		LEAVE();
		return ret;
	} else if (attribute == xv_omap_tvout_widescreen) {
		int ret =
		    omap_tvout_set_tv_widescreen(video_info->fbdev, value);
		LEAVE();
		return ret;
	} else if (attribute == xv_omap_tvout_scale) {
		int ret = omap_tvout_set_tv_scale(video_info->fbdev, value);
		LEAVE();
		return ret;
	}

	LEAVE();
	return BadMatch;
}

/**
 * Clip the image size to the visible screen.
 */
static void omap_video_query_best_size(ScrnInfoPtr screen, Bool motion,
				       short vid_w, short vid_h, short dst_w,
				       short dst_h, unsigned int *p_w,
				       unsigned int *p_h, pointer data)
{
	int maxvdownscale = vid_w > 1024 ? 2 : 4;

	/* Clip the image size to the visible screen. */
	if (dst_w > screen->virtualX)
		dst_w = screen->virtualX;

	if (dst_h > screen->virtualY)
		dst_h = screen->virtualY;

	/* Respect hardware scaling limits. */
	if (dst_w > vid_w * 8)
		dst_w = vid_w * 8;
	else if (dst_w < vid_w / 4)
		dst_w = vid_w / 4;

	if (dst_h > vid_h * 8)
		dst_h = vid_h * 8;
	else if (dst_h < vid_h / maxvdownscale)
		dst_h = vid_h / maxvdownscale;

	*p_w = dst_w;
	*p_h = dst_h;
}

/**
 * Start the video overlay; relies on data in video_info being sensible for
 * the current frame.
 */
static int start_video(struct omap_video_info *video_info)
{
	/* Stop TV-out if it's using this plane. */
	if (omap_tvout_clone_plane(video_info->fbdev) == video_info->id + 1)
		omap_tvout_stop(video_info->fbdev);

	if (video_info->state == OMAP_STATE_ACTIVE) {
		DebugF("omap/start_video: plane %d still active!\n",
		       video_info->id);
		stop_video(video_info);
	}

	if (!setup_plane(video_info)) {
		DebugF("omap/start_video: couldn't enable plane %d\n",
		       video_info->id);

		omap_tvout_resume(video_info->fbdev);

		return 0;
	}

	video_info->state = OMAP_STATE_ACTIVE;

	change_overlay_property(video_info, 1);

	DebugF("omap/start_video: enabled plane %d\n", video_info->id);

	return 1;
}

/**
 * Stop an overlay.  exit is whether or not the client's exiting.
 */
void omap_video_stop(ScrnInfoPtr screen, pointer data, Bool exit)
{
	FBDevPtr fbdev = screen->driverPrivate;
	struct omap_video_info *video_info = data;

	ENTER();

	stop_video(video_info);

	video_info->dirty = TRUE;

	if (exit) {
		empty_clip(video_info);

		if (video_info->caps & OMAPFB_CAPS_TEARSYNC)
			video_info->vsync = OMAP_VSYNC_TEAR;
		else
			video_info->vsync = OMAP_VSYNC_NONE;

		video_info->visibility = VisibilityPartiallyObscured;
		video_info->state = OMAP_STATE_STOPPED;
		video_info->ckey = default_ckey(screen);
		video_info->autopaint_ckey = 1;
		video_info->disable_ckey = 0;
	}

	video_info->drawable = NULL;

	/* Resume cloning */
	omap_tvout_resume(fbdev);

	LEAVE();
}

/**
 * Set up video_info with the specified parameters, and start the overlay.
 */
static Bool setup_overlay(ScrnInfoPtr screen,
			  struct omap_video_info *video_info, int id, int src_w,
			  int src_h, int dst_x, int dst_y, int dst_w, int dst_h,
			  DrawablePtr drawable)
{
	WindowPtr window;
	int maxvdownscale;

	ENTER();

	if (video_info->state == OMAP_STATE_ACTIVE) {
		DebugF("omap/setup_overlay: restarting overlay %d\n",
		       video_info->id);
		stop_video(video_info);
	}

	maxvdownscale = src_w > 1024 ? 2 : 4;

	if (src_w > dst_w * 4 || src_w * 8 < dst_w)
		video_info->hscale = TRUE;
	else
		video_info->hscale = FALSE;

	if (src_h > dst_h * maxvdownscale || src_h * 8 < dst_h)
		video_info->vscale = TRUE;
	else
		video_info->vscale = FALSE;

	video_info->src_w = src_w;
	video_info->src_h = src_h;
	video_info->dst_w = dst_w;
	video_info->dst_h = dst_h;
	video_info->dst_x = dst_x;
	video_info->dst_y = dst_y;
	video_info->fourcc = id;
	video_info->drawable = drawable;

	switch (id) {
	case FOURCC_YV12:
	case FOURCC_I420:
	case FOURCC_YUY2:
	case FOURCC_UYVY:
	case FOURCC_RV16:
	case FOURCC_RV32:
		break;
	default:
		ErrorF("omap_setup_overlay: bad FourCC %d!\n",
		       video_info->fourcc);
		LEAVE();
		return FALSE;
	}

	if (drawable->type == DRAWABLE_WINDOW) {
		window = (WindowPtr) drawable;
		video_info->visibility = window->visibility;
	} else {
		video_info->visibility = VisibilityPartiallyObscured;
	}

	video_info->dirty = TRUE;

	LEAVE();

	return start_video(video_info);
}

/**
 * XvPutImage hook.  This does not deal with rotation or partial updates.
 *
 * Calls out to omapCopyPlanarData (unobscured planar video),
 * omapExpandPlanarData (downscaled planar),
 * omapCopyPackedData (downscaled packed), xf86XVCopyPlanarData (obscured planar),
 * or xf86XVCopyPackedData (packed).
 */
static int omap_video_put(ScrnInfoPtr screen, short src_x, short src_y,
			  short dst_x, short dst_y, short src_w, short src_h,
			  short dst_w, short dst_h, int id, unsigned char *buf,
			  short width, short height, Bool sync,
			  RegionPtr clip_boxes, pointer data,
			  DrawablePtr drawable)
{
	struct omap_video_info *video_info = (struct omap_video_info *)data;
	int need_ckey = 0;
	int enable = 0;
	CARD8 *mem;

	/* Failure here means simply that there is nothing to draw */
	if (!clip_image_to_fit(&src_x, &src_y, &dst_x, &dst_y, &src_w, &src_h, &dst_w, &dst_h, width, height, clip_boxes)) {
		ErrorF("omap/put_image: skipping - (x=%d, y=%d, w=%d, h=%d)\n", dst_x, dst_y, dst_w, dst_h);
		return Success;
	}

	if (is_dirty(video_info, id, src_w, src_h, dst_x, dst_y, dst_w, dst_h)
	    || !video_info->mem) {
		if (!setup_overlay(screen, video_info, id, src_w, src_h, dst_x, dst_y, dst_w, dst_h, drawable)) {
			ErrorF
			    ("omap/put_image: failed to set up overlay: from (%d, %d) "
			     "to (%d, %d) at (%d, %d) on plane %d\n", src_w, src_h, dst_w, dst_h, dst_x, dst_y, video_info->id);
			return BadAlloc;
		}

		need_ckey = 1;
		enable = 1;
	}

	if (enable)
		DebugF("omap/put_image: putting image from (%d, %d, %d, %d) to "
		       "(%d, %d, %d, %d)\n", src_x, src_y, src_w,
		       src_h, dst_x, dst_y, dst_w, dst_h);

	/* Sync the engine first, so we don't draw over something that's still
	 * being scanned out. */
	if (video_info->vsync != OMAP_VSYNC_NONE)
		sync_gfx(video_info->fbdev);

	mem = video_info->mem + video_info->var.yoffset * video_info->dst_pitch;

	switch (id) {
	case FOURCC_RV32:
		if (video_info->hscale || video_info->vscale)
			omap_copy_scale_32(video_info->hscale, video_info->vscale, buf, mem, RR_Rotate_0,
					   OMAP_RV32_PITCH(width), video_info->dst_pitch, src_w, src_h, src_x, src_y, width,
					   height, dst_w, dst_h);
		else
			omap_copy_32(buf, mem, RR_Rotate_0, OMAP_RV32_PITCH(width), video_info->dst_pitch, src_w,
				     src_h, src_x, src_y, width, height);
		break;

	case FOURCC_RV16:
		if (video_info->hscale || video_info->vscale)
			omap_copy_scale_16(video_info->hscale, video_info->vscale, buf, mem, RR_Rotate_0,
					   OMAP_RV16_PITCH(width), video_info->dst_pitch, src_w, src_h, src_x, src_y, width,
					   height, dst_w, dst_h);
		else
			omap_copy_16(buf, mem, RR_Rotate_0, OMAP_RV16_PITCH(width), video_info->dst_pitch, src_w,
				     src_h, src_x, src_y, width, height);
		break;

	case FOURCC_UYVY:
	case FOURCC_YUY2:
		if (video_info->hscale || video_info->vscale)
			omap_copy_scale_packed(video_info->hscale, video_info->vscale, buf, mem, RR_Rotate_0,
					       OMAP_YUY2_PITCH(width), video_info->dst_pitch, src_w, src_h, src_x, src_y, width,
					       height, dst_w, dst_h);
		else
			omap_copy_packed(buf, mem, RR_Rotate_0, OMAP_YUY2_PITCH(width), video_info->dst_pitch,
					 src_w, src_h, src_x, src_y, width, height);
		break;

	case FOURCC_YV12:
	case FOURCC_I420:
		if (video_info->hscale || video_info->vscale)
			omap_copy_scale_planar(video_info->hscale, video_info->vscale, buf, mem, RR_Rotate_0,
					       OMAP_YV12_PITCH_LUMA(width), OMAP_YV12_PITCH_CHROMA(width),
					       video_info->dst_pitch, src_w, src_h, src_x, src_y, width, height, id, dst_w,
					       dst_h);
		else
			omap_copy_planar(buf, mem, RR_Rotate_0, OMAP_YV12_PITCH_LUMA(width),
					 OMAP_YV12_PITCH_CHROMA(width), video_info->dst_pitch, src_w, src_h, src_x, src_y,
					 width, height, id);
		break;
	}

	cancel_ckey_timer(video_info);
	if (video_info->disable_ckey)
		set_ckey_timer(video_info);

	if (video_info->double_buffer)
		flip_plane(video_info);

	if (enable)
		enable_plane(video_info);

	if (video_info->manual_updates)
		push_frame(video_info);

	if (!REGION_EQUAL(screen->pScreen, &video_info->clip, clip_boxes)) {
		REGION_COPY(screen->pScreen, &video_info->clip, clip_boxes);
		need_ckey = 1;
	}

	if (need_ckey && video_info->autopaint_ckey) {
		xf86XVFillKeyHelperDrawable(drawable, video_info->ckey, clip_boxes);
		DamageDamageRegion(drawable, clip_boxes);
	}

	return Success;
}

/**
 * Give image size and pitches.
 */
static int omap_video_query_attributes(ScrnInfoPtr screen, int id,
				       unsigned short *w_out,
				       unsigned short *h_out, int *pitches,
				       int *offsets)
{
	int size = 0, tmp = 0;
	int w, h;

	if (*w_out > DummyEncoding.width)
		*w_out = DummyEncoding.width;
	if (*h_out > DummyEncoding.height)
		*h_out = DummyEncoding.height;

	w = *w_out;
	h = *h_out;

	if (offsets)
		offsets[0] = 0;

	switch (id) {
	case FOURCC_I420:
	case FOURCC_YV12:
		w = (w + 3) & ~3;
		h = (h + 1) & ~1;
		size = w;
		if (pitches)
			pitches[0] = size;
		size *= h;
		if (offsets)
			offsets[1] = size;
		tmp = w >> 1;
		tmp = (tmp + 3) & ~3;
		if (pitches)
			pitches[1] = pitches[2] = tmp;
		tmp *= h >> 1;
		size += tmp;
		if (offsets)
			offsets[2] = size;
		size += tmp;
		break;
	case FOURCC_UYVY:
	case FOURCC_YUY2:
		w = (w + 1) & ~1;
		size = w << 1;
		if (pitches)
			pitches[0] = size;
		size *= h;
		break;
	case FOURCC_RV16:
		size = w << 1;
		if (pitches)
			pitches[0] = size;
		size *= h;
		break;
	case FOURCC_RV32:
		size = w << 2;
		if (pitches)
			pitches[0] = size;
		size *= h;
		break;
	default:
		return 0;
	}

	return size;
}

static void add_xv_format(XF86ImagePtr format)
{
	int i;

	for (i = 0; i < num_xv_images; i++) {
		if (memcmp(&xv_images[i], format, sizeof(*format)) == 0)
			return;
	}

	xv_images[i] = *format;
	num_xv_images++;
}

/* Opens a plane; if formats is given, checks that it supports at least one
 * of those formats. */
static int open_plane(int num, int formats, struct omapfb_caps *caps_out)
{
	int fd, ret = -1;
	char plane_path[sizeof("/dev/fbNN")];
	struct omapfb_caps caps;

	snprintf(plane_path, sizeof(plane_path), "/dev/fb%d", num);
	fd = open(plane_path, O_RDWR);
	if (fd < 0) {
		return ret;
	}

	ret = ioctl(fd, OMAPFB_GET_CAPS, &caps);
	if (ret < 0) {
		ErrorF("omap/video: failed to get capabilities for %s (%d:%s)\n",
		       plane_path, errno, strerror(errno));
		close(fd);
		return ret;
	}

	if (caps_out)
		*caps_out = caps;

	return fd;
}

static int omap_video_count_planes(FBDevPtr fbdev)
{
	int i, j, fd;

	ENTER();

	/* Range: fb1 -> fb99; we assume that all planes are both omapfb and
	 * contiguous. */
	for (i = 1, j = 0; i < 100; i++) {
		fd = open_plane(i, FB_FORMATS_MASK, NULL);
		if (fd < 0)
			break;
		close(fd);
		j++;
	}

	DebugF("omap/video: %d video planes available\n", j);

	LEAVE();

	return j;
}

static void omap_video_modify_encoding(FBDevPtr fbdev)
{
	unsigned int vram, max_w, max_h;
	struct omapfb_vram_info vram_info;

	ENTER();

	/* Calculate the max image size (of a certain aspect ratio) which will
	 * fit into the available memory. */
	if (!(ioctl(fbdev->fd, OMAPFB_GET_VRAM_INFO, &vram_info)))
		vram = vram_info.largest_free_block;
	else if (!(vram = omap_vram_get_avail()))
		ErrorF("omap/video: couldn't get vram info\n");

	vram /= MAX_PLANES;
	vram -= vram % getpagesize();
	vram /= MAX_BYTES_PER_PIXEL * MAX_BUFFERS;

	/* Try 16:9 first. */
	max_h = sqrt(9 * vram / 16);
	max_w = 16 * max_h / 9;

	/* Only use 16:9 if 720p is possible, otherwise fall back to 4:3. */
	if (max_w < 1280 || max_h < 720) {
		max_h = sqrt(3 * vram / 4);
		max_w = 4 * max_h / 3;
	}

	/* Hardware limits */
	if (max_w > VIDEO_IMAGE_MAX_WIDTH) {
		max_h =
		    min(VIDEO_IMAGE_MAX_WIDTH, vram / VIDEO_IMAGE_MAX_WIDTH);
		max_w = VIDEO_IMAGE_MAX_WIDTH;
	}
	if (max_h > VIDEO_IMAGE_MAX_HEIGHT) {
		max_h = VIDEO_IMAGE_MAX_HEIGHT;
		max_w =
		    min(VIDEO_IMAGE_MAX_HEIGHT, vram / VIDEO_IMAGE_MAX_HEIGHT);
	}

	/* Only full macropixels */
	max_w &= ~1;

	DummyEncoding.width = max_w;
	DummyEncoding.height = max_h;

	LEAVE();
}

static Bool omap_video_setup_private(ScreenPtr screen,
				     struct omap_video_info *video_info, int i)
{
	ScrnInfoPtr xf86screen = xf86Screens[screen->myNum];
	FBDevPtr fbdev = xf86screen->driverPrivate;
	struct omapfb_caps plane_caps;

	ENTER();

	video_info->fd = open_plane(i, FB_FORMATS_MASK, &plane_caps);
	if (video_info->fd < 0) {
		ErrorF("omap/video: failed to open plane %d\n", i);
		LEAVE();
		return FALSE;
	}

	/* Planes start with their memory fully allocated, so free it when
	 * we take control.
	 * FIXME: I think this only applies to the GFX plane? -- Oliver.
	 */
	if (!alloc_plane_mem(video_info, 0)) {
		ErrorF("omap/video: couldn't allocate plane mem\n");
		LEAVE();
		return FALSE;
	}

	video_info->caps = plane_caps.ctrl;
	video_info->manual_updates =
	    !!(video_info->caps & OMAPFB_CAPS_MANUAL_UPDATE);

	video_info->id = i - 1;
	video_info->state = OMAP_STATE_STOPPED;
	video_info->fbdev = fbdev;
	REGION_INIT(pScreen, &video_info->clip, NullBox, 0);

	video_info->visibility = VisibilityPartiallyObscured;
	video_info->autopaint_ckey = 1;
	video_info->disable_ckey = 0;
	video_info->ckey = default_ckey(xf86screen);
	if (video_info->caps & OMAPFB_CAPS_TEARSYNC)
		video_info->vsync = OMAP_VSYNC_TEAR;
	else
		video_info->vsync = OMAP_VSYNC_NONE;

	LEAVE();

	return TRUE;
}

static void omap_video_free_adaptor(FBDevPtr fbdev, XF86VideoAdaptorPtr adapt)
{
	int i;
	struct omap_video_info *video_info;

	if (adapt->pPortPrivates)
		for (i = 1; i <= fbdev->num_video_ports; ++i) {
			video_info = adapt->pPortPrivates[i - 1].ptr;
			close(video_info->fd);
			xfree(video_info);
		}
	xfree(adapt->pPortPrivates);
	xfree(adapt);
}

/**
 * Set up all our internal structures.
 */
static XF86VideoAdaptorPtr omap_video_setup_adaptor(ScreenPtr screen)
{
	ScrnInfoPtr xf86screen = xf86Screens[screen->myNum];
	FBDevPtr fbdev = xf86screen->driverPrivate;
	XF86VideoAdaptorPtr adapt;
	int i, num_video_ports = 0;
	struct omap_video_info *video_info;

	num_video_ports = omap_video_count_planes(fbdev);
	/* No usable video overlays. */
	if (!num_video_ports)
		return NULL;

	/* Modify the encoding to contain our real min/max values. */
	omap_video_modify_encoding(fbdev);

	if (!(adapt = xcalloc(1, sizeof(XF86VideoAdaptorRec))))
		return NULL;

	adapt->type = XvWindowMask | XvInputMask | XvImageMask;
	adapt->flags = VIDEO_CLIP_TO_VIEWPORT | VIDEO_OVERLAID_IMAGES;
	adapt->name = "OMAP Video Overlay";
	adapt->nEncodings = 1;
	adapt->pEncodings = &DummyEncoding;

	adapt->nFormats = ARRAY_SIZE(xv_formats);
	adapt->pFormats = xv_formats;

	adapt->nAttributes = ARRAY_SIZE(xv_attributes);
	adapt->pAttributes = xv_attributes;

	memset(xv_images, 0, sizeof(xv_images));
	num_xv_images = 0;
	for (i = 0; i < ARRAY_SIZE(video_format_map); i++)
		add_xv_format(&video_format_map[i].xv_format);
	adapt->nImages = num_xv_images;
	adapt->pImages = xv_images;

	adapt->PutImage = omap_video_put;
	adapt->StopVideo = omap_video_stop;
	adapt->GetPortAttribute = omap_video_get_attribute;
	adapt->SetPortAttribute = omap_video_set_attribute;
	adapt->QueryBestSize = omap_video_query_best_size;
	adapt->QueryImageAttributes = omap_video_query_attributes;
	adapt->ClipNotify = omap_video_clip_notify;

	adapt->pPortPrivates = (DevUnion *)
	    xcalloc(num_video_ports, sizeof(DevUnion));
	if (!adapt->pPortPrivates)
		goto unwind;

	for (i = 1; i <= num_video_ports; i++) {
		video_info = xcalloc(1, sizeof(struct omap_video_info));
		if (!video_info)
			goto unwind;

		if (!omap_video_setup_private(screen, video_info, i))
			goto unwind;

		adapt->pPortPrivates[i - 1].ptr = (pointer) video_info;
		adapt->nPorts++;
	}

	xv_ckey = MAKE_ATOM("XV_COLORKEY");
	xv_autopaint_ckey = MAKE_ATOM("XV_AUTOPAINT_COLORKEY");
	xv_disable_ckey = MAKE_ATOM("XV_DISABLE_COLORKEY");
	xv_double_buffer = MAKE_ATOM("XV_DOUBLE_BUFFER");
	xv_vsync = MAKE_ATOM("XV_OMAP_VSYNC");
	xv_omap_clone_to_tvout = MAKE_ATOM("XV_OMAP_CLONE_TO_TVOUT");
	xv_omap_tvout_standard = MAKE_ATOM("XV_OMAP_TVOUT_STANDARD");
	xv_omap_tvout_widescreen = MAKE_ATOM("XV_OMAP_TVOUT_WIDESCREEN");
	xv_omap_tvout_scale = MAKE_ATOM("XV_OMAP_TVOUT_SCALE");
	xv_omap_overlay_active = MAKE_ATOM("XV_OMAP_OVERLAY_ACTIVE");
	_omap_video_overlay = MAKE_ATOM("_OMAP_VIDEO_OVERLAY");

	fbdev->num_video_ports = num_video_ports;
	fbdev->overlay_adaptor = adapt;

	return adapt;

unwind:
	omap_video_free_adaptor(fbdev, adapt);

	fbdev->num_video_ports = 0;
	fbdev->overlay_adaptor = NULL;

	return NULL;
}

int omap_video_get_active_plane(FBDevPtr fbdev)
{
	struct omap_video_info *vi;
	int i;

	for (i = 0; i < fbdev->num_video_ports; i++) {
		vi = get_omap_video_info(fbdev, i);
		if (vi->overlay_active)
			return vi->id;
	}

	return -1;
}

int omap_video_get_free_plane(FBDevPtr fbdev)
{
	struct omap_video_info *vi;
	int i;

	for (i = fbdev->num_video_ports - 1; i >= 0; i--) {
		vi = get_omap_video_info(fbdev, i);
		if (!vi->overlay_active)
			return vi->id;
	}

	return -1;
}

int omap_video_get_plane_fd(FBDevPtr fbdev, int id)
{
	struct omap_video_info *vi;
	int i;

	for (i = 0; i < fbdev->num_video_ports; i++) {
		vi = get_omap_video_info(fbdev, i);
		if (vi->id == id)
			return vi->fd;
	}

	return -1;
}

Bool fbdev_init_video(ScreenPtr screen)
{
	ScrnInfoPtr xf86screen = xf86Screens[screen->myNum];
	FBDevPtr fbdev = xf86screen->driverPrivate;
	XF86VideoAdaptorPtr *adaptors = NULL, adaptor;
	int i = 0;

	fbdev->screen = screen;

	adaptors = realloc(adaptors, (i + 1) * sizeof(XF86VideoAdaptorPtr));
	if (!adaptors)
		return FALSE;

	if ((adaptor = omap_video_setup_adaptor(screen))) {
		adaptors[i] = adaptor;
		i++;

		omap_tvout_init(fbdev);
	}

	adaptors = realloc(adaptors, (i + 1) * sizeof(XF86VideoAdaptorPtr));
	if (!adaptors)
		return FALSE;

	if ((adaptor = pvr2dSetupTexturedVideo(screen))) {
		adaptors[i] = adaptor;
		i++;
	}

	xf86XVScreenInit(screen, adaptors, i);

	/* Hook drawable destruction, so we can ignore them if they go away. */
	fbdev->video_destroy_pixmap = screen->DestroyPixmap;
	screen->DestroyPixmap = destroy_pixmap_hook;
	fbdev->video_destroy_window = screen->DestroyWindow;
	screen->DestroyWindow = destroy_window_hook;

	if (adaptors)
		xfree(adaptors);

	return TRUE;
}

/**
 * Shut down Xv, also used on regeneration.  All videos should be stopped
 * by the time we get here.
 */
void fbdev_fini_video(ScreenPtr screen)
{
	ScrnInfoPtr xf86screen = xf86Screens[screen->myNum];
	FBDevPtr fbdev = xf86screen->driverPrivate;
	struct omap_video_info *video_info;
	int i;

	for (i = 0; i < fbdev->num_video_ports; i++) {
		video_info = get_omap_video_info(fbdev, i);

		close(video_info->fd);
	}

	screen->DestroyPixmap = fbdev->video_destroy_pixmap;
	screen->DestroyWindow = fbdev->video_destroy_window;
	fbdev->video_destroy_pixmap = NULL;
	fbdev->video_destroy_window = NULL;

	omap_video_free_adaptor(fbdev, fbdev->overlay_adaptor);
	fbdev->overlay_adaptor = NULL;
	fbdev->num_video_ports = 0;
}
