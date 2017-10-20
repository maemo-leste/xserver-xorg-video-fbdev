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

#ifndef OMAP_VIDEO_FORMATS_H
#define OMAP_VIDEO_FORMATS_H

void omap_copy_packed(CARD8 * src, CARD8 * dst,
		      int randr,
		      int srcPitch, int dstPitch,
		      int srcW, int srcH,
		      int left, int top,
		      int w, int h);

/**
 * Copy packed video data for downscaling, where 'scaling' in this case
 * means just removing lines.  Unfortunately, since we don't want to get
 * into the business of swapping the U and V channels, we remove a
 * macroblock (2x1) at a time.
 */
void omap_copy_scale_packed(Bool hscale, Bool vscale,
			    CARD8 * src, CARD8 * dst,
			    int randr,
			    int srcPitch, int dstPitch,
			    int srcW, int srcH,
			    int left, int top,
			    int w, int h,
			    int dstW, int dstH);

/**
 * Copy I420/YV12 data to YUY2, with no scaling.  Originally from kxv.c.
 */
void omap_copy_planar(CARD8 * src, CARD8 * dst,
		      int randr,
		      int srcPitch, int srcPitch2, int dstPitch,
		      int srcW, int srcH,
		      int left, int top,
		      int w, int h,
		      int id);

/**
 * Copy and expand planar (I420) -> packed (UYVY) video data, including
 * downscaling, by just removing two lines at a time.
 */
void omap_copy_scale_planar(Bool hscale, Bool vscale,
			    CARD8 * src, CARD8 * dstb,
			    int randr,
			    int srcPitch, int srcPitch2, int dstPitch,
			    int srcW, int srcH,
			    int left, int top,
			    int w, int h,
			    int id,
			    int dstW, int dstH);

/**
 * Copy I420 data to the custom 'YUV420' format, which is actually:
 * y11 u11,u12,u21,u22 u13,u14,u23,u24 y12 y14 y13
 * y21 v11,v12,v21,v22 v13,v14,v23,v24 y22 y24 y23
 *
 * The third and fourth luma components are swapped.  Yes, this is weird.
 *
 * So, while we have the same 2x2 macroblocks in terms of colour granularity,
 * we actually require 4x2.  We lop off the last 1-3 lines if width is not a
 * multiple of four, and let the hardware expand.
 */
void omap_copy_yuv420(CARD8 * srcb, CARD8 * dstb,
		      int randr,
		      int srcPitch, int srcPitch2, int dstPitch,
		      int srcW, int srcH,
		      int left, int top,
		      int w, int h,
		      int id);


void omap_copy_16(CARD8 * src, CARD8 * dst,
		  int randr,
		  int srcPitch, int dstPitch,
		  int srcW, int srcH,
		  int left, int top,
		  int w, int h);

void omap_copy_scale_16(Bool hscale, Bool vscale,
			CARD8 * src, CARD8 * dst,
			int randr,
			int srcPitch, int dstPitch,
			int srcW, int srcH,
			int left, int top,
			int w, int h,
			int dstW, int dstH);

void omap_copy_32(CARD8 * src, CARD8 * dst,
		  int randr,
		  int srcPitch, int dstPitch,
		  int srcW, int srcH,
		  int left, int top,
		  int w, int h);

void omap_copy_scale_32(Bool hscale, Bool vscale,
			CARD8 * src, CARD8 * dst,
			int randr,
			int srcPitch, int dstPitch,
			int srcW, int srcH,
			int left, int top,
			int w, int h,
			int dstW, int dstH);

#endif /* OMAP_VIDEO_FORMATS_H */
