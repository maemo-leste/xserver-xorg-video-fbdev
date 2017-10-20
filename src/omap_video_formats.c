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

/**
 * None of the functions in this file need to deal with rotation:
 * we can rotate on scanout from the LCD controller, so all our
 * copies are unrotated, as rotating while copying is a horrific
 * speed hit.
 */

#ifdef HAVE_KDRIVE_CONFIG_H
#include <kdrive-config.h>
#endif

#include "fbdev.h"
#include "fourcc.h"
#include "omap_video_formats.h"

/**
 * Copy YUV422/YUY2 data with no scaling.
 */
void omap_copy_packed(CARD8 * src, CARD8 * dst,
		      int randr,
		      int srcPitch, int dstPitch,
		      int srcW, int srcH,
		      int left, int top,
		      int w, int h)
{
	src += top * srcPitch + (left << 1);

	/* memcpy FTW on ARM. */
	if (srcPitch == dstPitch && !left) {
		memcpy(dst, src, srcH * srcPitch);
	} else {
		while (srcH--) {
			memcpy(dst, src, srcW << 1);
			src += srcPitch;
			dst += dstPitch;
		}
	}
}

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
			    int dstW, int dstH)
{
	int x, y, srcx, srcy;
	int xinc = hscale ? (srcW << 16) / dstW : 0x10000;
	int yinc = vscale ? (srcH << 16) / dstH : 0x10000;

	if (randr != RR_Rotate_0) {
		ErrorF("omapCopyPackedData: rotation not supported\n");
		return;
	}

	if (top || left) {
		ErrorF("omapCopyPackedData: partial updates not supported\n");
		return;
	}

	if (srcW & 1 || dstW & 1) {
		DebugF
		    ("omapCopyPackedData: widths should be multiples of two\n");
		srcW &= ~1;
		dstW &= ~1;
	}

	w >>= 1;

	if (hscale)
		w = w * dstW / srcW;
	if (vscale)
		h = h * dstH / srcH;

	srcy = 0;
	for (y = 0; y < h; y++) {
		if (!hscale) {
			memcpy(dst, src, w << 2);
		} else {
			CARD32 *s = (CARD32 *) src;
			CARD32 *d = (CARD32 *) dst;

			srcx = 0;
			for (x = 0; x < w; x++) {
				*d++ = s[srcx >> 17];
				srcx += xinc << 1;
			}
		}

		dst += dstPitch;

		srcy += yinc;
		while (srcy > 0xffff) {
			src += srcPitch;
			srcy -= 0x10000;
		}
	}
}

/**
 * Copy I420/YV12 data to YUY2, with no scaling.  Originally from kxv.c.
 */
void omap_copy_planar(CARD8 * src, CARD8 * dst,
		      int randr,
		      int srcPitch, int srcPitch2, int dstPitch,
		      int srcW, int srcH,
		      int left, int top,
		      int w, int h,
		      int id)
{
	int i, j;
	CARD8 *src1, *src2, *src3, *dst1;

	/* compute source data pointers */
	src1 = src;
	src2 = src1 + h * srcPitch;
	src3 = src2 + (h >> 1) * srcPitch2;

	src += top * srcPitch + left;
	src2 += (top >> 1) * srcPitch2 + (left >> 1);
	src3 += (top >> 1) * srcPitch2 + (left >> 1);

	if (id == FOURCC_I420) {
		CARD8 *srct = src3;
		src3 = src2;
		src2 = srct;
	}

	dst1 = dst;

	srcW >>= 1;
	for (j = 0; j < srcH; j++) {
		CARD32 *dst = (CARD32 *) dst1;
		CARD16 *s1 = (CARD16 *) src1;
		CARD8 *s2 = src2;
		CARD8 *s3 = src3;

		for (i = 0; i < srcW; i++) {
			*dst++ =
			    (*s1 & 0x00ff) | ((*s1 & 0xff00) << 8) | (*s3 << 8)
			    | (*s2 << 24);
			s1++;
			s2++;
			s3++;
		}
		src1 += srcPitch;
		dst1 += dstPitch;
		if (j & 1) {
			src2 += srcPitch2;
			src3 += srcPitch2;
		}
	}
}

/**
 * Copy and expand planar (I420) -> packed (UYVY) video data, including
 * downscaling, by just removing two lines at a time.
 *
 * FIXME: Target for arg reduction.
 */
void omap_copy_scale_planar(Bool hscale, Bool vscale,
			    CARD8 * src, CARD8 * dstb,
			    int randr,
			    int srcPitch, int srcPitch2, int dstPitch,
			    int srcW, int srcH,
			    int left, int top,
			    int w, int h,
			    int id,
			    int dstW, int dstH)
{
	CARD8 *src1, *src2, *src3, *dst1;
	int x, y, srcx, srcy1, srcy2;
	int xinc = hscale ? (srcW << 16) / dstW : 0x10000;
	int yinc = vscale ? (srcH << 16) / dstH : 0x10000;

	if (randr != RR_Rotate_0) {
		ErrorF("omapExpandPlanarData: rotation not supported\n");
		return;
	}
	if (top || left) {
		ErrorF("omapExpandPlanarData: partial updates not supported\n");
		return;
	}

	/* compute source data pointers */
	src1 = src;
	src2 = src1 + h * srcPitch;
	src3 = src2 + (h >> 1) * srcPitch2;

	if (id == FOURCC_I420) {
		CARD8 *tmp = src2;
		src2 = src3;
		src3 = tmp;
	}

	dst1 = dstb;

	w >>= 1;

	if (hscale)
		w = w * dstW / srcW;
	if (vscale)
		h = h * dstH / srcH;

	srcy1 = 0;
	srcy2 = 0;
	for (y = 0; y < h; y++) {
		CARD32 *d = (CARD32 *) dstb;

		srcx = 0;
		for (x = 0; x < w; x++) {
			CARD8 s1l = src1[(srcx) >> 16];
			CARD8 s1r = src1[(srcx + xinc) >> 16];
			CARD8 s2 = src2[(srcx) >> 17];
			CARD8 s3 = src3[(srcx) >> 17];

			*d++ = s1l | (s1r << 16) | (s3 << 8) | (s2 << 24);
			srcx += xinc << 1;
		}

		dstb += dstPitch;

		srcy1 += yinc;
		while (srcy1 > 0xffff) {
			src1 += srcPitch;
			srcy1 -= 0x10000;
		}
		srcy2 += yinc;
		while (srcy2 > 0x1ffff) {
			src2 += srcPitch2;
			src3 += srcPitch2;
			srcy2 -= 0x20000;
		}
	}
}

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
 *
 * FIXME: Target for arg reduction.
 */
void omap_copy_yuv420(CARD8 * srcb, CARD8 * dstb,
		      int randr,
		      int srcPitch, int srcPitch2, int dstPitch,
		      int srcW, int srcH,
		      int left, int top,
		      int w, int h,
		      int id)
{
	CARD8 *srcy, *srcu, *srcv, *dst;
	CARD16 *d1;
	CARD32 *d2;
	int i, j;

	if (randr != RR_Rotate_0) {
		ErrorF("omapCopyPlanarData: rotation not supported\n");
		return;
	}

	if (top || left || h != srcH || w != srcW) {
		ErrorF("omapCopyPlanarData: offset updates not supported\n");
		return;
	}

	srcy = srcb;
	srcv = srcy + h * srcPitch;
	srcu = srcv + (h >> 1) * srcPitch2;
	dst = dstb;

	if (id == FOURCC_I420) {
		CARD8 *tmp = srcv;
		srcv = srcu;
		srcu = tmp;
	}

	w >>= 2;
	for (i = 0; i < h; i++) {
		CARD32 *sy = (CARD32 *) srcy;
		CARD16 *sc;

		sc = (CARD16 *) ((i & 1) ? srcv : srcu);
		d1 = (CARD16 *) dst;

		for (j = 0; j < w; j++) {
			if (((unsigned long)d1) & 3) {
				/* Luma 1, chroma 1. */
				*d1++ =
				    (*sy & 0x000000ff) | ((*sc & 0x00ff) << 8);
				/* Chroma 2, luma 2. */
				*d1++ =
				    ((*sc & 0xff00) >> 8) | (*sy & 0x0000ff00);
			} else {
				d2 = (CARD32 *) d1;
				/* Luma 1, chroma 1, chroma 2, luma 2. */
				*d2++ =
				    (*sy & 0x000000ff) | (*sc << 8) |
				    ((*sy & 0x0000ff00) << 16);
				d1 = (CARD16 *) d2;
			}
			/* Luma 4, luma 3. */
			*d1++ =
			    ((*sy & 0xff000000) >> 24) | ((*sy & 0x00ff0000) >>
							  8);
			sy++;
			sc++;
		}

		dst += dstPitch;
		srcy += srcPitch;
		if (i & 1) {
			srcu += srcPitch2;
			srcv += srcPitch2;
		}
	}
}

/**
 * Copy 16 bpp data with no scaling.
 */
void omap_copy_16(CARD8 * src, CARD8 * dst,
		  int randr,
		  int srcPitch, int dstPitch,
		  int srcW, int srcH,
		  int left, int top,
		  int w, int h)
{
	src += top * srcPitch + (left << 1);

	/* memcpy FTW on ARM. */
	if (srcPitch == dstPitch && !left) {
		memcpy(dst, src, srcH * srcPitch);
	} else {
		while (srcH--) {
			memcpy(dst, src, srcW << 1);
			src += srcPitch;
			dst += dstPitch;
		}
	}
}

/**
 * Copy 16 bit data with pixel replication/dropping scaling.
 */
void omap_copy_scale_16(Bool hscale, Bool vscale,
			CARD8 * src, CARD8 * dst,
			int randr,
			int srcPitch, int dstPitch,
			int srcW, int srcH,
			int left, int top,
			int w, int h,
			int dstW, int dstH)
{
	int srcx, srcy;
	int xinc = hscale ? (srcW << 16) / dstW : 0x10000;
	int yinc = vscale ? (srcH << 16) / dstH : 0x10000;

	if (randr != RR_Rotate_0) {
		ErrorF("omapCopyPackedData: rotation not supported\n");
		return;
	}


	if (top || left || h != srcH || w != srcW) {
		ErrorF("omapCopyPlanarData: offset updates not supported\n");
		return;
	}

	if (hscale)
		w = w * dstW / srcW;
	if (vscale)
		h = h * dstH / srcH;

	srcy = 0;
	while (h) {
		if (!hscale) {
			memcpy(dst, src, w << 1);
		} else {
			CARD16 *s16 = (CARD16 *) src;
			CARD32 *d32 = (CARD32 *) dst;
			int w2 = w;

			srcx = 0;

			if ((unsigned long) d32 & 2) {
				CARD16 *d16 = (CARD16 *) d32;
				*d16++ = s16[srcx >> 16];
				d32 = (CARD32 *) d16;
				srcx += xinc;
				w2--;
			}

			while (w2 > 1) {
				*d32++ = s16[srcx >> 16] |
					(s16[(srcx + xinc) >> 16] << 16);
				srcx += xinc << 1;
				w2 -= 2;
			}

			if (w2) {
				CARD16 *d16 = (CARD16 *) d32;
				*d16 = s16[srcx >> 16];
			}
		}

		dst += dstPitch;
		srcy += yinc;

		while (srcy > 0xffff) {
			src += srcPitch;
			srcy -= 0x10000;
		}

		h--;
	}
}

/**
 * Copy 32 bpp data with no scaling.
 */
void omap_copy_32(CARD8 * src, CARD8 * dst,
		  int randr,
		  int srcPitch, int dstPitch,
		  int srcW, int srcH,
		  int left, int top,
		  int w, int h)
{
	src += top * srcPitch + (left << 2);

	/* memcpy FTW on ARM. */
	if (srcPitch == dstPitch && !left) {
		memcpy(dst, src, srcH * srcPitch);
	} else {
		while (srcH--) {
			memcpy(dst, src, srcW << 2);
			src += srcPitch;
			dst += dstPitch;
		}
	}
}

/**
 * Copy 32 bit data with pixel replication/dropping scaling.
 */
void omap_copy_scale_32(Bool hscale, Bool vscale,
			CARD8 * src, CARD8 * dst,
			int randr,
			int srcPitch, int dstPitch,
			int srcW, int srcH,
			int left, int top,
			int w, int h,
			int dstW, int dstH)
{
	int srcx, srcy;
	int xinc = hscale ? (srcW << 16) / dstW : 0x10000;
	int yinc = vscale ? (srcH << 16) / dstH : 0x10000;

	if (randr != RR_Rotate_0) {
		ErrorF("omapCopyPackedData: rotation not supported\n");
		return;
	}


	if (top || left || h != srcH || w != srcW) {
		ErrorF("omapCopyPlanarData: offset updates not supported\n");
		return;
	}

	if (hscale)
		w = w * dstW / srcW;
	if (vscale)
		h = h * dstH / srcH;

	srcy = 0;
	while (h) {
		if (!hscale) {
			memcpy(dst, src, w << 2);
		} else {
			CARD32 *s32 = (CARD32 *) src;
			CARD32 *d32 = (CARD32 *) dst;
			int w2 = w;

			srcx = 0;
			while (w2) {
				*d32++ = s32[srcx >> 16];
				srcx += xinc;
				w2--;
			}
		}

		dst += dstPitch;
		srcy += yinc;

		while (srcy > 0xffff) {
			src += srcPitch;
			srcy -= 0x10000;
		}

		h--;
	}
}
