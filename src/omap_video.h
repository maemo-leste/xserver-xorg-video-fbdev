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

#ifndef OMAP_VIDEO_H
#define OMAP_VIDEO_H

#define FOURCC_RV16 0x36315652
#define XVIMAGE_RV16 \
	{ \
		FOURCC_RV16, \
		XvRGB, \
		LSBFirst, \
		{'R','V','1','6', \
		 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, \
		16, \
		XvPacked, \
		1, \
		16, 0xF800, 0x07E0, 0x001F, \
		0, 0, 0, \
		0, 0, 0, \
		0, 0, 0, \
		{'R', 'G', 'B',0, \
		 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, \
		XvTopToBottom \
	}

#define FOURCC_RV32 0x32335652
#define XVIMAGE_RV32 \
	{ \
		FOURCC_RV32, \
		XvRGB, \
		LSBFirst, \
		{'R','V','3','2', \
		 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, \
		32, \
		XvPacked, \
		1, \
		24, 0xFF0000, 0x00FF00, 0x0000FF, \
		0, 0, 0, \
		0, 0, 0, \
		0, 0, 0, \
		{'R', 'G', 'B',0, \
		 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, \
		XvTopToBottom \
	}

void omap_video_stop(ScrnInfoPtr screen, pointer data, Bool exit);
int omap_video_get_active_plane(FBDevPtr fbdev);
int omap_video_get_free_plane(FBDevPtr fbdev);
int omap_video_get_plane_fd(FBDevPtr fbdev, int id);
Bool fbdev_init_video(ScreenPtr screen);
void fbdev_fini_video(ScreenPtr screen);

#endif /* OMAP_VIDEO_H */
