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

#include "fbdev.h"

#include "sgx_pvr2d.h"
#if USE_SHM && defined(DRI2)
#include "sgx_dri2.h"
#endif

#include "exa.h"
#include "x-hash.h"

/* XXX: RENDER acceleration is slow and lockup prone. Enable at your own risk. */
#undef PVR2D_EXT_BLIT

//#define SGX_EXA_EXTRA_STATS

/* These should really be tracked per screen but are global for now */
static Bool CreateScreenPixmap;
static PVR2DBLTINFO pvr2dblt;
static Pixel colour;
static x_hash_table *pixmapsHT;

#ifdef SGX_PVR2D_CALL_STATS
extern sgx_pvr2d_call_stats callStats;
#endif

static Bool PVR2DPrepareAccess(PixmapPtr pPix, int index);

static void PVR2DFinishAccess(PixmapPtr pPix, int index);

Bool getDrawableInfo(DrawablePtr pDraw, PVR2DMEMINFO ** ppMemInfo, long *pXoff,
		     long *pYoff)
{
	PixmapPtr pPixmap =
	    pDraw->type ==
	    DRAWABLE_WINDOW ? pDraw->pScreen->
	    GetWindowPixmap((WindowPtr) pDraw) : (PixmapPtr) pDraw;
	struct PVR2DPixmap *ppix = exaGetPixmapDriverPrivate(pPixmap);

	if (!ppix || !PVR2DValidate(ppix, TRUE))
		return FALSE;

	*ppMemInfo = ppix->pvr2dmem;
#ifdef COMPOSITE
	*pXoff = -pPixmap->screen_x + pPixmap->drawable.x;
	*pYoff = -pPixmap->screen_y + pPixmap->drawable.y;
#else
	*pXoff = 0;
	*pYoff = 0;
#endif

	return TRUE;
}

int getSGXPitchAlign(int width)
{
	return width <
	    EURASIA_TAG_STRIDE_THRESHOLD ? EURASIA_TAG_STRIDE_ALIGN0 :
	    EURASIA_TAG_STRIDE_ALIGN1;
}

Bool GetPVR2DFormat(int depth, PVR2DFORMAT * format)
{
	DBG("%s: %d\n", __func__, depth);

	switch (depth) {
	case 8:
		*format = PVR2D_ALPHA8;
		return TRUE;
	case 15:
		*format = PVR2D_ARGB1555;
		return TRUE;
	case 16:
		*format = PVR2D_RGB565;
		return TRUE;
	case 24:
		*format = PVR2D_RGB888;
		return TRUE;
	case 32:
		*format = PVR2D_ARGB8888;
		return TRUE;
	default:
		DBG("%s: depth %d not supported\n", __func__, depth);
		return FALSE;
	}
}

#ifdef SGX_EXA_EXTRA_STATS
unsigned int solidCounters[GXset + 1];
#endif /* SGX_EXA_EXTRA_STATS */

/* Heuristics for choosing between software and hardware rendering.
 * The heuristics will choose the solution that will take less CPU time
 * returns	TRUE  : Software solid fill is faster
 * 			FALSE : Hardware solid fill is faster
 */
static Bool IsSWSolidFillFaster(struct PVR2DPixmap *pdst, PVR2DBLTINFO * pBlt)
{
	/* flushing a page takes ~31 usec, we want to avoid cache flushing, so give a bigger penalty */
	const int flush_penalty = 40;
	/* 3d set-up time is ~160 usec. We round it up a little bit */
	int hw_time = 200;
	int pixels = pBlt->DSizeX * pBlt->DSizeY;
	/* time required for software rendering (pixels/speed) in usec. the speed is 32 px/usec */
	int sw_time = pixels >> 5;
	int pages;

	/* pixmap owned by GPU and sw_time + flush_time < hw_time */
	if (QueryBlitsComplete(pdst, 0) != PVR2D_OK) {
		/* surface is busy with SGX, use hardware rendering */
		return FALSE;
	}

	if (sw_time > hw_time) {
		if (pdst->owner == PVR2D_OWNER_GPU) {
			/* pixmap owned by GPU, hw_time < sw_time */
			return FALSE;
		}
		pages = (PVR2DGetFlushSize(pdst) + 4096 - 1) >> 12;
		hw_time += pages * flush_penalty;
		if (sw_time > hw_time) {
			/* pixmap requires flushing, but sw_time > hw_time + flush_time */
			return FALSE;
		}
		/* pixmap owned by CPU and sw_time < hw_time + flush_time */
		return TRUE;
	} else {		/* sw_time <= hw_time */
		if (pdst->owner == PVR2D_OWNER_CPU) {
			/* pixmap owned by CPU and sw_time < hw_time */
			return TRUE;
		}
		pages = (PVR2DGetFlushSize(pdst) + 4096 - 1) >> 12;
		sw_time += pages * flush_penalty;
		if (sw_time > hw_time) {
			/* pixmap owned by GPU and sw_time + flush_time > hw_time */
			return FALSE;
		}
	}

	return TRUE;
}

static void SWSolidFill32(PVR2DBLTINFO * pBlt, Pixel colour)
{
	int x, y;
	unsigned char *line =
	    (unsigned char *)pBlt->pDstMemInfo->pBase + pBlt->DstOffset;
	line += pBlt->DstY * pBlt->DstStride + pBlt->DstX * 4;
	unsigned long *p = (unsigned long *)line;
	for (y = 0; y < pBlt->DSizeY; y++) {
		for (x = 0; x < pBlt->DSizeX; x++) {
			*p++ = colour;
		}
		line += pBlt->DstStride;
		p = (unsigned long *)line;
	}
}

static void SWSolidFill16(PVR2DBLTINFO * pBlt, Pixel colour)
{
	int x, y;
	unsigned char *line =
	    (unsigned char *)pBlt->pDstMemInfo->pBase + pBlt->DstOffset;
	line += pBlt->DstY * pBlt->DstStride + pBlt->DstX * 2;
	unsigned short *p = (unsigned short *)line;
	for (y = 0; y < pBlt->DSizeY; y++) {
		for (x = 0; x < pBlt->DSizeX; x++) {
			*p++ = colour;
		}
		line += pBlt->DstStride;
		p = (unsigned short *)line;
	}
}

static void SWSolidFill8(PVR2DBLTINFO * pBlt, Pixel colour)
{
	int x, y;
	unsigned char *line =
	    (unsigned char *)pBlt->pDstMemInfo->pBase + pBlt->DstOffset;
	line += pBlt->DstY * pBlt->DstStride + pBlt->DstX * 1;
	unsigned char *p = (unsigned char *)line;
	for (y = 0; y < pBlt->DSizeY; y++) {
		for (x = 0; x < pBlt->DSizeX; x++) {
			*p++ = colour;
		}
		line += pBlt->DstStride;
		p = (unsigned char *)line;
	}
}

/* software solid fill, colour is in pixmap's format */
static void SWSolidFill(PVR2DBLTINFO * pBlt, Pixel colour)
{
	switch (pBlt->DstFormat) {
	case PVR2D_ALPHA8:
		return SWSolidFill8(pBlt, colour);
	case PVR2D_RGB565:
	case PVR2D_ARGB1555:
		return SWSolidFill16(pBlt, colour);
	case PVR2D_RGB888:
	case PVR2D_ARGB8888:
		return SWSolidFill32(pBlt, colour);
	default:
		DBG("%s: format %d not supported\n", __func__, pBlt->DstFormat);
	}
}

static Bool PVR2DPrepareSolid(PixmapPtr pPixmap, int alu, Pixel planemask,
			      Pixel fg)
{
	struct PVR2DPixmap *pdst = exaGetPixmapDriverPrivate(pPixmap);

#ifdef SGX_EXA_EXTRA_STATS
	if ((alu >= 0) && (alu <= GXset)) {
		solidCounters[alu]++;
	}
#endif

	if (alu != GXcopy) {
		DBG("%s: FALSE: (alu != GXcopy)\n", __func__);
		return FALSE;
	}

	if (pPixmap->drawable.bitsPerPixel < 8) {
		DBG("%s: FALSE: (pPixmap->drawable.bitsPerPixel < 8)\n",
		    __func__);
		return FALSE;
	}

	if (!PVR2DValidate(pdst, TRUE)) {
		DBG("%s: FALSE: (!PVR2DValidate(pdst))\n", __func__);
		return FALSE;
	}

	if (!EXA_PM_IS_SOLID(&pPixmap->drawable, planemask)) {
		DBG("%s: FALSE: (!EXA_PM_IS_SOLID(&pPixmap->drawable, planemask))\n", __func__);
		return FALSE;
	}

	if (!GetPVR2DFormat(pPixmap->drawable.depth, &pvr2dblt.DstFormat)) {
		DBG("%s: FALSE: (!GetPVR2DFormat(pPixmap->drawable.depth, &pvr2dblt.DstFormat))\n", __func__);
		return FALSE;
	}

	if (pdst->owner == PVR2D_OWNER_CPU) {
		DBG("%s: destination owned by CPU\n", __func__);
	} else {
		DBG("%s: destination owned by GPU\n", __func__);
	}

	switch (pPixmap->drawable.depth) {
	case 32:
	case 24:
		pvr2dblt.Colour = fg;
		break;
	case 16:
		pvr2dblt.Colour =
		    ((fg & 0xf800) << 8) | ((fg & 0x7e0) << 5) | ((fg & 0x1f) << 3) | 0x70307;
		break;
	case 15:
		pvr2dblt.Colour =
		    ((fg & 0x7c00) << 9) | ((fg & 0x3e0) << 6) | ((fg & 0x1f) << 3) | 0x70707;
		break;
	case 8:
		pvr2dblt.Colour = fg & 0xff;
		break;
	default:
		DBG("%s: depth %d not supported for solid fill on SGX\n",
		    __func__, pPixmap->drawable.depth);
		return FALSE;
	}

	pvr2dblt.CopyCode = PVR2DPATROPcopy;
	pvr2dblt.BlitFlags = PVR2D_BLIT_DISABLE_ALL;

	pvr2dblt.pDstMemInfo = pdst->pvr2dmem;
	pvr2dblt.DstSurfWidth = pPixmap->drawable.width;
	pvr2dblt.DstSurfHeight = pPixmap->drawable.height;
	pvr2dblt.DstStride = pPixmap->devKind;

	colour = fg;
	return TRUE;
}

static void PVR2DSolid(PixmapPtr pDstPixmap, int x1, int y1, int x2, int y2)
{
	PVR2DERROR result;
	struct PVR2DPixmap *pdst = exaGetPixmapDriverPrivate(pDstPixmap);

	pvr2dblt.DSizeX = x2 - x1;
	pvr2dblt.DSizeY = y2 - y1;
	pvr2dblt.DstX = x1;
	pvr2dblt.DstY = y1;

	if (IsSWSolidFillFaster(pdst, &pvr2dblt)) {
		if (!PVR2DPixmapOwnership_CPU(pdst)) {
			return;
		}
		SWSolidFill(&pvr2dblt, colour);
		pdst->bCPUWrites = TRUE;
		DBG("%s SW(%p, %d, %d, %d, %d)\n", __func__, pDstPixmap, x1, y1, x2, y2);
	} else {
		PVR2DPixmapOwnership_GPU(pdst);
		result = PVR2DBlt(hPVR2DContext, &pvr2dblt);
		DBG("%s HW(%p, %d, %d, %d, %d) => %d\n", __func__, pDstPixmap, x1, y1, x2, y2, result);
#ifdef SGX_PVR2D_CALL_STATS
		callStats.solidOP++;
#endif
	}
}

static void PVR2DDoneSolid(PixmapPtr pDstPixmap)
{
}

/* Heuristics for choosing between software and hardware copy.
 * The heuristics will choose the solution that will take less CPU time
 * returns	TRUE  : Software solid fill is faster
 * 			FALSE : Hardware solid fill is faster
 */
static Bool IsSWCopyFaster(struct PVR2DPixmap *psrc, struct PVR2DPixmap *pdst,
			   PVR2DBLTINFO * pBlt)
{
	/* the values below are taken blindly from SolidFill heuristics */
	/* flushing a page takes ~31 usec, we want to avoid cache flushing, so give a bigger penalty */
	const int flush_penalty = 40;
	/* 3d set-up time is ~160 usec. We round it up a little bit */
	int hw_time = 200;
	int pixels = pBlt->DSizeX * pBlt->DSizeY;
	/* time required for software rendering (pixels/speed) in usec. the speed is 32 px/usec */
	int sw_time = pixels >> 5;
	int pages_src, pages_dst;

	if ((QueryBlitsComplete(pdst, 0) != PVR2D_OK)
	    || (QueryBlitsComplete(psrc, 0) != PVR2D_OK)) {
		/* surface is busy with SGX, use hardware rendering */
		return FALSE;
	}

	pages_dst = (PVR2DGetFlushSize(pdst) + 4096 - 1) >> 12;
	pages_src = (PVR2DGetFlushSize(psrc) + 4096 - 1) >> 12;
	if (pdst->owner == PVR2D_OWNER_CPU)
		hw_time += pages_dst * flush_penalty;
	else
		sw_time += pages_dst * flush_penalty;
	if (psrc->owner == PVR2D_OWNER_CPU)
		hw_time += pages_src * flush_penalty;
	else
		sw_time += pages_src * flush_penalty;
	if (hw_time < sw_time) {
		/* use HW rendering */
		return FALSE;
	}

	return TRUE;
}

#ifdef SGX_EXA_EXTRA_STATS
unsigned int copyCounters[GXset + 1];
#endif /* SGX_EXA_EXTRA_STATS */

#ifdef SGX_TEST_COPYOP
PixmapPtr pSavedSrcPixmap;
#endif

/* Variables needed for software copy */
PixmapPtr pSourcePixmap;
GCPtr pGC;

static Bool PVR2DPrepareCopy(PixmapPtr pSrcPixmap, PixmapPtr pDstPixmap, int dx,
			     int dy, int alu, Pixel planemask)
{
	struct PVR2DPixmap *psrc = exaGetPixmapDriverPrivate(pSrcPixmap);
	struct PVR2DPixmap *pdst = exaGetPixmapDriverPrivate(pDstPixmap);

#ifdef SGX_EXA_EXTRA_STATS
	if ((alu >= 0) && (alu <= GXset)) {
		copyCounters[alu]++;
	}
#endif /* SGX_EXA_EXTRA_STATS */

	if (alu != GXcopy) {
		DBG("%s: FALSE: (alu != GXcopy)\n", __func__);
		return FALSE;
	}

	if (!PVR2DValidate(pdst, TRUE)) {
		DBG("%s: FALSE: (!PVR2DValidate(pdst))\n", __func__);
		return FALSE;
	}

	if (!PVR2DValidate(psrc, FALSE)) {
		DBG("%s: FALSE: (!PVR2DValidate(psrc))\n", __func__);
		return FALSE;
	}

	if (!EXA_PM_IS_SOLID(&pDstPixmap->drawable, planemask)) {
		DBG("%s: FALSE: (!EXA_PM_IS_SOLID(&pDstPixmap->drawable, planemask))\n", __func__);
		return FALSE;
	}

	if (pDstPixmap->drawable.bitsPerPixel < 8) {
		DBG("%s: FALSE: (pDstPixmap->drawable.bitsPerPixel < 8)\n",
		    __func__);
		return FALSE;
	}

	if (!GetPVR2DFormat(pDstPixmap->drawable.depth, &pvr2dblt.DstFormat)) {
		DBG("%s: FALSE: (!GetPVR2DFormat(pDstPixmap->drawable.depth, &pvr2dblt.DstFormat))\n", __func__);
		return FALSE;
	}

	if (!GetPVR2DFormat(pSrcPixmap->drawable.depth, &pvr2dblt.SrcFormat)) {
		DBG("%s: FALSE: (!GetPVR2DFormat(pSrcPixmap->drawable.depth, &pvr2dblt.SrcFormat))\n", __func__);
		return FALSE;
	}

	if (pdst->owner == PVR2D_OWNER_CPU) {
		DBG("%s: destination owned by CPU\n", __func__);
	} else {
		DBG("%s: destination owned by GPU\n", __func__);
	}

#ifdef SGX_TEST_COPYOP
	pSavedSrcPixmap = pSrcPixmap;
#endif

	pvr2dblt.CopyCode = PVR2DROPcopy;
	pvr2dblt.BlitFlags = PVR2D_BLIT_DISABLE_ALL;

	pvr2dblt.pDstMemInfo = pdst->pvr2dmem;
	pvr2dblt.DstSurfWidth = pDstPixmap->drawable.width;
	//pvr2dblt.DstSurfWidth =  pDstPixmap->devKind * 8 / pDstPixmap->drawable.depth ;
	pvr2dblt.DstSurfHeight = pDstPixmap->drawable.height;
	pvr2dblt.DstStride = pDstPixmap->devKind;

	pvr2dblt.pSrcMemInfo = psrc->pvr2dmem;
	pvr2dblt.SrcSurfWidth = pSrcPixmap->drawable.width;
	//pvr2dblt.SrcSurfWidth =  pSrcPixmap->devKind * 8 / pSrcPixmap->drawable.depth ; 
	pvr2dblt.SrcSurfHeight = pSrcPixmap->drawable.height;
	pvr2dblt.SrcStride = pSrcPixmap->devKind;

	DBG("%s: pSrcPixmap=%p, BlitFlags=0x%x, DstFormat=0x%x, SrcFormat=0x%x\n", __func__, pSrcPixmap, pvr2dblt.BlitFlags, pvr2dblt.DstFormat, pvr2dblt.SrcFormat);

	pSourcePixmap = pSrcPixmap;

	return TRUE;
}

static void PVR2DCopy(PixmapPtr pDstPixmap, int srcX, int srcY, int dstX,
		      int dstY, int width, int height)
{
	PVR2DERROR result;
	RegionPtr pReg;

	pvr2dblt.SizeX = pvr2dblt.DSizeX = width;
	pvr2dblt.SizeY = pvr2dblt.DSizeY = height;
	pvr2dblt.DstX = dstX;
	pvr2dblt.DstY = dstY;
	pvr2dblt.SrcX = srcX;
	pvr2dblt.SrcY = srcY;

#if SGX_TEST_COPYOP
	unsigned long srcCrc = 0, dstCrc = 0;
	if (pvr2dblt.DstFormat == pvr2dblt.SrcFormat) {

		struct PVR2DPixmap *psrc =
		    exaGetPixmapDriverPrivate(pSavedSrcPixmap);
		PVR2DQueryBlitsComplete(hPVR2DContext, psrc->pvr2dmem, 1);
		PVR2DFlushCache(psrc);

		int BPP = pDstPixmap->drawable.bitsPerPixel / 8;

		unsigned char *linesrc, *p1;
		p1 = psrc->pvr2dmem->pBase;
		p1 += srcY * pvr2dblt.SrcStride + srcX * BPP;
		linesrc = p1;
		int i, j;
		for (j = 0; j < height; j++) {
			for (i = 0; i < width * BPP; i++) {
				srcCrc += *p1++ ^ i;
			}
			p1 = linesrc += pvr2dblt.SrcStride;
		}
	}
#endif

	if (IsSWCopyFaster(exaGetPixmapDriverPrivate(pSourcePixmap), exaGetPixmapDriverPrivate(pDstPixmap), &pvr2dblt)) {
		if (!pGC) {
			pGC = GetScratchGC(pDstPixmap->drawable.depth, pDstPixmap->drawable.pScreen);
			ValidateGC(&pDstPixmap->drawable, pGC);
		}
		PVR2DPrepareAccess(pDstPixmap, EXA_PREPARE_DEST);
		PVR2DPrepareAccess(pSourcePixmap, EXA_PREPARE_SRC);
		pReg =
		    fbCopyArea(&pSourcePixmap->drawable, &pDstPixmap->drawable,
			       pGC, srcX, srcY, width, height, dstX, dstY);
		if (pReg)
			miRegionDestroy(pReg);
		PVR2DFinishAccess(pSourcePixmap, EXA_PREPARE_SRC);
		PVR2DFinishAccess(pDstPixmap, EXA_PREPARE_DEST);
		DBG("%s SW(%p, %d, %d, %d, %d, %d, %d)\n", __func__, pDstPixmap,
		    srcX, srcY, dstX, dstY, width, height);
	} else {
		PVR2DPixmapOwnership_GPU(exaGetPixmapDriverPrivate(pDstPixmap));
		PVR2DPixmapOwnership_GPU(exaGetPixmapDriverPrivate(pSourcePixmap));
		result = PVR2DBlt(hPVR2DContext, &pvr2dblt);
		DBG("%s HW(%p, %d, %d, %d, %d, %d, %d) => %d\n", __func__,
		    pDstPixmap, srcX, srcY, dstX, dstY, width, height, result);
	}

	DBG("%s (pDstMemInfo = %p, DstSurfWidth = %d, DstSurfHeight = %d, DstStride = %d)\n", __func__, pvr2dblt.pDstMemInfo, pvr2dblt.DstSurfWidth, pvr2dblt.DstSurfHeight, pvr2dblt.DstStride);
	DBG("%s (pSrcMemInfo = %p, SrcSurfWidth = %d, SrcSurfHeight = %d, SrcStride = %d)\n", __func__, pvr2dblt.pSrcMemInfo, pvr2dblt.SrcSurfWidth, pvr2dblt.SrcSurfHeight, pvr2dblt.SrcStride);

#ifdef SGX_PVR2D_CALL_STATS
	callStats.copyOP++;
	static int counter = 0;
	if (++counter == 100) {
		counter = 0;
		ErrorF("Stats: copy=%u,solid=%u,flush=%u,invalidate=%u\n",
		       callStats.copyOP, callStats.solidOP, callStats.flushOP,
		       callStats.invOP);
	}
#endif

#if SGX_TEST_COPYOP
	if (pvr2dblt.DstFormat == pvr2dblt.SrcFormat) {

		struct PVR2DPixmap *pdst =
		    exaGetPixmapDriverPrivate(pDstPixmap);
		PVR2DQueryBlitsComplete(hPVR2DContext, pdst->pvr2dmem, 1);
		PVR2DFlushCache(pdst);

		int BPP = pDstPixmap->drawable.bitsPerPixel / 8;

		unsigned char *linedst, *p2;
		p2 = pdst->pvr2dmem->pBase;
		p2 += dstY * pvr2dblt.DstStride + dstX * BPP;
		linedst = p2;
		int i, j;
		for (j = 0; j < height; j++) {
			for (i = 0; i < width * BPP; i++) {
				dstCrc += *p2++ ^ i;
			}
			p2 = linedst += pvr2dblt.DstStride;
		}
		if (srcCrc != dstCrc) {
			ErrorF("Copy operation failed\n");
		}
	}
#endif
}

static void PVR2DDoneCopy(PixmapPtr pDstPixmap)
{
	if (pGC) {
		FreeScratchGC(pGC);
		pGC = NULL;
	}
}

#ifdef PVR2D_EXT_BLIT

static PVR2DEXTBLTINFO pvr2dextblt;
static PicturePtr pSrcPict[2];

static Bool PVR2DCheckComposite(int op, PicturePtr pSrc, PicturePtr pMask,
				PicturePtr pDst)
{
	PVR2DFORMAT unused;
	Bool retValue;
	retValue = op <= PictOpAdd && pSrc->filter <= PictFilterBilinear
	    && pDst->pDrawable && pDst->pDrawable->bitsPerPixel >= 8
	    && GetPVR2DFormat(pDst->pDrawable->depth, &unused)
	    && pSrc->pDrawable
	    && GetPVR2DFormat(pSrc->pDrawable->depth, &unused)
	    && (!pMask || (pMask-> filter <= PictFilterBilinear && pMask-> pDrawable && GetPVR2DFormat (pMask-> pDrawable-> depth, &unused)));

	return retValue;
}

static Bool PVR2DSetFilterRepeat(PicturePtr pPict, PVR2DSRCSRFINFO * pSrcInfo)
{
	switch (pPict->filter) {
	case PictFilterNearest:
		pSrcInfo->SrcFilterMode = PVR2D_FILTER_NEAREST;
		break;
	case PictFilterBilinear:
		pSrcInfo->SrcFilterMode = PVR2D_FILTER_LINEAR;
		break;
	default:
		ErrorF("Unsupported filter 0x%x\n", pPict->filter);
		return FALSE;
	}

	switch (pPict->repeatType) {
	case RepeatNone:
		/* RENDER spec requires that sampling outside the source picture results
		 * in alpha=0 pixels (even if the source itself has no alpha). The X
		 * server clips operations such that we never sample outside of
		 * untransformed sources.
		 */
		if (pPict->transform)
			return FALSE;

		pSrcInfo->SrcRepeatMode = PVR2D_REPEAT_NONE;
		break;
	case RepeatNormal:
		pSrcInfo->SrcRepeatMode = PVR2D_REPEAT_NORMAL;
		break;
	case RepeatPad:
		pSrcInfo->SrcRepeatMode = PVR2D_REPEAT_PAD;
		break;
	case RepeatReflect:
		pSrcInfo->SrcRepeatMode = PVR2D_REPEAT_MIRROR;
		break;
	default:
		ErrorF("Unsupported repeat type 0x%x\n", pPict->repeatType);
		return FALSE;
	}

	colour = fg;
	return TRUE;
}

static Bool PVR2DPrepareComposite(int op, PicturePtr pSrc, PicturePtr pMask,
				  PicturePtr pDst, PixmapPtr pSrcPixmap,
				  PixmapPtr pMaskPixmap, PixmapPtr pDstPixmap)
{
	struct PVR2DPixmap *psrc = exaGetPixmapDriverPrivate(pSrcPixmap);
	struct PVR2DPixmap *pmsk =
	    pMask ? exaGetPixmapDriverPrivate(pMaskPixmap) : NULL;
	struct PVR2DPixmap *pdst = exaGetPixmapDriverPrivate(pDstPixmap);

	/*
	 * There is a bug when Mask == Source. As a workaround turn mask off
	 * and use a set filters which uses Source as a Mask.
	 * The downside is when a composite operation MUST requests the same coordinates for
	 * source and mask or else we will get corrupted output.
	 *
	 */
	if (pmsk == psrc) {
		DBGCOMPOSITE("%s: src == mask unsupported.\n", __func__);
		pvr2dextblt.bDuplicatedSource = PVR2D_TRUE;
		pmsk = NULL;
		//return FALSE;         
	} else {
		pvr2dextblt.bDuplicatedSource = PVR2D_FALSE;
	}

	if (!PVR2DValidate(pdst) || !PVR2DValidate(psrc)
	    || (pmsk && !PVR2DValidate(pmsk))
	    || !GetPVR2DFormat(pDstPixmap->drawable.depth, &pvr2dextblt.DstFormat)
	    || !GetPVR2DFormat(pSrcPixmap->drawable.depth, &pvr2dextblt.SrcSurface[0].SrcFormat)
	    || (pmsk && !GetPVR2DFormat(pMaskPixmap->drawable.depth, &pvr2dextblt.SrcSurface[1].SrcFormat))
	    || !PVR2DSetFilterRepeat(pSrc, &pvr2dextblt.SrcSurface[0])
	    || (pmsk && !PVR2DSetFilterRepeat(pMask, &pvr2dextblt.SrcSurface[1])))
	{
		DBGCOMPOSITE("%s: prerequisities failed\n", __func__);
		return FALSE;
	}

	PVR2DPixmapOwnership_GPU(pdst);
	PVR2DPixmapOwnership_GPU(psrc);
	PVR2DPixmapOwnership_GPU(pmsk);

	pvr2dextblt.BlitFlags = PVR2D_BLIT_DISABLE_ALL;

	pvr2dextblt.pDstMemInfo = pdst->pvr2dmem;
	pvr2dextblt.DstSurfWidth = pDstPixmap->drawable.width;
	pvr2dextblt.DstSurfHeight = pDstPixmap->drawable.height;
	pvr2dextblt.DstStride = pDstPixmap->devKind;

	pvr2dextblt.SrcSurface[0].pSrcMemInfo = psrc->pvr2dmem;
	pvr2dextblt.SrcSurface[0].SrcSurfWidth = pSrcPixmap->drawable.width;
	pvr2dextblt.SrcSurface[0].SrcSurfHeight = pSrcPixmap->drawable.height;
	pvr2dextblt.SrcSurface[0].SrcStride = pSrcPixmap->devKind;

	if (pmsk) {
		pvr2dextblt.SrcSurface[1].pSrcMemInfo = pmsk->pvr2dmem;
		pvr2dextblt.SrcSurface[1].SrcSurfWidth =
		    pMaskPixmap->drawable.width;
		pvr2dextblt.SrcSurface[1].SrcSurfHeight =
		    pMaskPixmap->drawable.height;
		pvr2dextblt.SrcSurface[1].SrcStride = pMaskPixmap->devKind;
		pvr2dextblt.SrcSurface[1].SrcTexCoord = 1;
	} else
		pvr2dextblt.SrcSurface[1].pSrcMemInfo = NULL;

	DBGCOMPOSITE
	    ("%s: pSrcPixmap=%p, BlitFlags=0x%x, DstFormat=0x%x, SrcFormat=0x%x, op=%i\n",
	     __func__, pSrcPixmap, pvr2dextblt.BlitFlags, pvr2dextblt.DstFormat,
	     pvr2dextblt.SrcSurface[0].SrcFormat, op);
	DBGCOMPOSITE("%s: DST, width = %i, height=%i, stride=%i\n", __func__,
		     pvr2dextblt.DstSurfWidth, pvr2dextblt.DstSurfHeight,
		     pvr2dextblt.DstStride);
	DBGCOMPOSITE("%s: SRC, width = %i, height=%i, stride=%i\n", __func__,
		     pvr2dextblt.SrcSurface[0].SrcSurfWidth,
		     pvr2dextblt.SrcSurface[0].SrcSurfHeight,
		     pvr2dextblt.SrcSurface[0].SrcStride);
	if (pmsk) {
		DBGCOMPOSITE("%s: MskFormat=0x%x\n", __func__,
			     pvr2dextblt.SrcSurface[1].SrcFormat);
		DBGCOMPOSITE("%s: Mask, width = %i, height=%i, stride=%i\n",
			     __func__, pvr2dextblt.SrcSurface[1].SrcSurfWidth,
			     pvr2dextblt.SrcSurface[1].SrcSurfHeight,
			     pvr2dextblt.SrcSurface[1].SrcStride);
	}

	if (PVR2DPrepareCompositeBlt(hPVR2DContext, &pvr2dextblt, op) !=
	    PVR2D_OK) {
		ErrorF("PVR2DPrepareCompositeBlt() failed\n");
		return FALSE;
	}

	pSrcPict[0] = pSrc;
	pSrcPict[1] = (pmsk ? pMask : NULL);

	return TRUE;
}

static inline void transformPoint(PictTransform * transform,
				  xPointFixed * point)
{
	PictVector v;
	v.vector[0] = point->x;
	v.vector[1] = point->y;
	v.vector[2] = xFixed1;
	PictureTransformPoint(transform, &v);
	point->x = v.vector[0];
	point->y = v.vector[1];
}

#define xFixedToFloat(f) (((float) (f)) / 65536)

static void PVR2DComposite(PixmapPtr pDst, int srcX, int srcY, int maskX,
			   int maskY, int dstX, int dstY, int width, int height)
{
	float vertices[4 * 3 * 2];
	float *coord = vertices;
	xPointFixed srcTopLeft, srcBottomLeft, srcTopRight, srcBottomRight;
	xPointFixed maskTopLeft, maskBottomLeft, maskTopRight, maskBottomRight;

	float srcW = (float)pvr2dextblt.SrcSurface[0].SrcSurfWidth;
	float srcH = (float)pvr2dextblt.SrcSurface[0].SrcSurfHeight;
	float maskW = (float)pvr2dextblt.SrcSurface[1].SrcSurfWidth;
	float maskH = (float)pvr2dextblt.SrcSurface[1].SrcSurfHeight;

	DBGCOMPOSITE
	    ("%s: src=(%i,%i),mask=(%i,%i),dst=(%i,%i), width=%i, height=%i\n",
	     __func__, srcX, srcY, maskX, maskY, dstX, dstY, width, height);

	srcTopLeft.x = IntToxFixed(srcX);
	srcTopLeft.y = IntToxFixed(srcY);
	srcBottomLeft.x = IntToxFixed(srcX);
	srcBottomLeft.y = IntToxFixed(srcY + height);
	srcTopRight.x = IntToxFixed(srcX + width);
	srcTopRight.y = IntToxFixed(srcY);
	srcBottomRight.x = IntToxFixed(srcX + width);
	srcBottomRight.y = IntToxFixed(srcY + height);

	if (pSrcPict[0]->transform) {
		transformPoint(pSrcPict[0]->transform, &srcTopLeft);
		transformPoint(pSrcPict[0]->transform, &srcBottomLeft);
		transformPoint(pSrcPict[0]->transform, &srcTopRight);
		transformPoint(pSrcPict[0]->transform, &srcBottomRight);
	}

	if (pSrcPict[1]) {
		maskTopLeft.x = IntToxFixed(maskX);
		maskTopLeft.y = IntToxFixed(maskY);
		maskBottomLeft.x = IntToxFixed(maskX);
		maskBottomLeft.y = IntToxFixed(maskY + height);
		maskTopRight.x = IntToxFixed(maskX + width);
		maskTopRight.y = IntToxFixed(maskY);
		maskBottomRight.x = IntToxFixed(maskX + width);
		maskBottomRight.y = IntToxFixed(maskY + height);

		if (pSrcPict[1]->transform) {
			transformPoint(pSrcPict[1]->transform, &maskTopLeft);
			transformPoint(pSrcPict[1]->transform, &maskBottomLeft);
			transformPoint(pSrcPict[1]->transform, &maskTopRight);
			transformPoint(pSrcPict[1]->transform,
				       &maskBottomRight);
		}
	}

	*coord++ = (float)dstX;
	*coord++ = (float)dstY;
	*coord++ = xFixedToFloat(srcTopLeft.x) / srcW;
	*coord++ = xFixedToFloat(srcTopLeft.y) / srcH;
	if (pvr2dextblt.SrcSurface[1].pSrcMemInfo) {
		*coord++ = xFixedToFloat(maskTopLeft.x) / maskW;
		*coord++ = xFixedToFloat(maskTopLeft.y) / maskH;
	}

	*coord++ = (float)dstX;
	*coord++ = (float)(dstY + height);
	*coord++ = xFixedToFloat(srcBottomLeft.x) / srcW;
	*coord++ = xFixedToFloat(srcBottomLeft.y) / srcH;
	if (pvr2dextblt.SrcSurface[1].pSrcMemInfo) {
		*coord++ = xFixedToFloat(maskBottomLeft.x) / maskW;
		*coord++ = xFixedToFloat(maskBottomLeft.y) / maskH;
	}

	*coord++ = (float)(dstX + width);
	*coord++ = (float)dstY;
	*coord++ = xFixedToFloat(srcTopRight.x) / srcW;
	*coord++ = xFixedToFloat(srcTopRight.y) / srcH;
	if (pvr2dextblt.SrcSurface[1].pSrcMemInfo) {
		*coord++ = xFixedToFloat(maskTopRight.x) / maskW;
		*coord++ = xFixedToFloat(maskTopRight.y) / maskH;
	}

	*coord++ = (float)(dstX + width);
	*coord++ = (float)(dstY + height);
	*coord++ = xFixedToFloat(srcBottomRight.x) / srcW;
	*coord++ = xFixedToFloat(srcBottomRight.y) / srcH;
	if (pvr2dextblt.SrcSurface[1].pSrcMemInfo) {
		*coord++ = xFixedToFloat(maskBottomRight.x) / maskW;
		*coord++ = xFixedToFloat(maskBottomRight.y) / maskH;
	}

	if (PVR2DCompositeBlt(hPVR2DContext, &pvr2dextblt, vertices) !=
	    PVR2D_OK) {
		ErrorF("PVR2DCompositeBlt() failed\n");
	}
}

static void PVR2DDoneComposite(PixmapPtr pDst)
{
	DBGCOMPOSITE("%s\n", __func__);
	if (PVR2DFinishCompositeBlt(hPVR2DContext, &pvr2dextblt) != PVR2D_OK) {
		ErrorF("PVR2DFinishComposite() failed\n");
	}
#ifdef SGX_EXA_EXTRA_STATS
	static int period = 0;
	if (period++ == 10) {
		int i;
		for (i = 0; i <= GXset; i++) {
			DBGCOMPOSITE("OP %i, solid %u, copy %u\n", i,
				     solidCounters[i], copyCounters[i]);
		}
		period = 0;
	}
#endif /* SGX_EXA_EXTRA_STATS */
}

#endif // def PVR2D_EXT_BLIT

static void PVR2DWaitMarker(ScreenPtr pScreen, int marker)
{
}

static void *PVR2DCreatePixmap2(ScreenPtr pScreen, int width, int height,
				int depth, int usage_hint, int bitsPerPixel)
{
	struct PVR2DPixmap *ppix = calloc(1, sizeof(struct PVR2DPixmap));

#if USE_SHM
	ppix->shmid = -1;
#endif
	ppix->usage_hint = usage_hint;

	if (CreateScreenPixmap) {
		ppix->pvr2dmem = pSysMemInfo;
#if USE_SHM
		ppix->shmaddr = pSysMemInfo->pBase;
#endif
		ppix->screen = TRUE;

		CreateScreenPixmap = FALSE;
	} else {
		ppix->screen = FALSE;
	}
	if (pixmapsHT)
		x_hash_table_insert(pixmapsHT, ppix, ppix);

	ppix->bCPUWrites = FALSE;

	DBG("%s(%p, %d, %d, %d, %d, %d) => %p\n", __func__, pScreen, width,
	    height, depth, usage_hint, bitsPerPixel);

	return ppix;
}

static void (*SavedBlockHandler) (int, pointer, pointer, pointer);

static void PVR2DBlockHandler(int i, pointer blockData, pointer pTimeout,
			      pointer pReadmask)
{
	ScreenPtr pScreen = screenInfo.screens[i];

	pScreen->BlockHandler = SavedBlockHandler;
	(*pScreen->BlockHandler) (i, blockData, pTimeout, pReadmask);
	pScreen->BlockHandler = PVR2DBlockHandler;

	PVR2DDelayedMemDestroy(FALSE);
}

static void PVR2DDestroyPixmap(ScreenPtr pScreen, void *driverPriv)
{
	struct PVR2DPixmap *ppix = driverPriv;

	DBG("%s(%p)\n", __func__, ppix);

	if (pixmapsHT)
		x_hash_table_remove(pixmapsHT, ppix);

	if (ppix) {
		DestroyPVR2DMemory(ppix);
		free(ppix);
	}
}

static Bool PVR2DModifyPixmapHeader(PixmapPtr pPixmap, int width, int height,
				    int depth, int bitsPerPixel, int devKind,
				    pointer pPixData)
{
	struct PVR2DPixmap *ppix = exaGetPixmapDriverPrivate(pPixmap);
	int pitch_align;
	int pitch;

	if (!ppix)
		return FALSE;

	DBG("%s(%p %d=>%d, %d=>%d, %d=>%d, %d=>%d, %d=>%d, %p=>%p)\n", __func__,
	    pPixmap, pPixmap->drawable.width, width, pPixmap->drawable.height,
	    height, pPixmap->drawable.depth, depth,
	    pPixmap->drawable.bitsPerPixel, bitsPerPixel, pPixmap->devKind,
	    devKind, pPixmap->devPrivate.ptr, pPixData);

	/* Wrapping memory is apparently tricky and doesn't quite work yet,
	 * so we only let you either newly allocate it, or change it to the
	 * visible framebuffer (with ~0UL). */
	if (pPixData && pPixData != (void *)~0UL && !ppix->screen)
		return FALSE;

	if (width <= 0)
		width = pPixmap->drawable.width;

	if (bitsPerPixel <= 0)
		bitsPerPixel = pPixmap->drawable.bitsPerPixel;

	if (devKind < 0)
		devKind = (width * bitsPerPixel + 7) / 8;
	else if (devKind == 0)
		devKind = pPixmap->devKind;

	pitch_align = getSGXPitchAlign(width);

	pitch =
	    (devKind + pitch_align * bitsPerPixel / 8 -
	     1) & ~(pitch_align * bitsPerPixel / 8 - 1);

	if (height <= 0)
		height = pPixmap->drawable.height;

	if (depth <= 0)
		depth = pPixmap->drawable.depth;

	if (pitch != pPixmap->devKind || height != pPixmap->drawable.height
	    || bitsPerPixel != pPixmap->drawable.bitsPerPixel || pPixData
	    || (ppix->pvr2dmem == pSysMemInfo && !pPixData)) {

		DestroyPVR2DMemory(ppix);
		ppix->screen = FALSE;
		ppix->pvr2dmem = NULL;
#if USE_SHM
#if USE_MALLOC
		ppix->mallocaddr = NULL;
#endif /* USE_MALLOC */
		ppix->shmaddr = NULL;
		ppix->shmid = -1;
#endif

		if (!pPixData) {
#if USE_SHM
			ppix->shmsize = pitch * height;
			if (ppix->usage_hint == CREATE_PIXMAP_USAGE_BACKING_PIXMAP) {
				PVR2DAllocSHM(ppix);
			}
#if USE_MALLOC
			else {
				PVR2DAllocNormal(ppix);
				ppix->owner = PVR2D_OWNER_CPU;
			}
#endif

			if (!ppix->shmaddr
#if USE_MALLOC
			    && !ppix->mallocaddr
#endif /* USE_MALLOC */
			    )
#endif /* USE_SHM */
			{
				if (height * pitch) {
					if (PVR2DMemAlloc
					    (hPVR2DContext, height * pitch, 4,
					     0, &ppix->pvr2dmem) != PVR2D_OK) {
						ppix->pvr2dmem = NULL;
						return FALSE;
					}
				}
			}
		} else if (pPixData == (void *)~0UL) {
			ppix->pvr2dmem = pSysMemInfo;
			ppix->screen = TRUE;
#if USE_SHM
			ppix->shmid = -1;
			ppix->shmaddr = pSysMemInfo->pBase;
#endif
		}
	}

	/* Allocating PVR2D memory for caller supplied data doesn't work quite
	 * correctly yet, and may not be too useful anyway (mostly affects scratch
	 * pixmaps).
	 */
#if 0
	else if (pPixData) {
		DBG("%s: Syncing %p\n", __func__, ppix->pvr2dmem);
		QueryBlitsComplete(ppix, 1);
	}

	if (pPixData) {
		if (pitch == devKind)
			memcpy(ppix->pvr2dmem->pBase, pPixData,
			       height * devKind);
		else {
			int i;
			char *src = pPixData, *dst = ppix->pvr2dmem->pBase;

			for (i = 0; i < height;
			     i++, src += devKind, dst += pitch)
				memcpy(dst, src, devKind);
		}
	}
#endif

	return miModifyPixmapHeader(pPixmap, width, height, depth, bitsPerPixel,
				    pitch, NULL);
}

static Bool PVR2DPrepareAccess(PixmapPtr pPix, int index)
{
	struct PVR2DPixmap *ppix = exaGetPixmapDriverPrivate(pPix);

	if (!PVR2DPixmapOwnership_CPU(ppix)) {
		pPix->devPrivate.ptr = NULL;
		return FALSE;
	}

#if USE_SHM
#if USE_MALLOC
	if (ppix->mallocaddr) {
		pPix->devPrivate.ptr = ppix->mallocaddr;
	} else
#endif /* USE_MALLOC */
	if (ppix->shmaddr) {
		pPix->devPrivate.ptr = ppix->shmaddr;
	} else
#endif
	if (ppix->pvr2dmem)
		pPix->devPrivate.ptr = ppix->pvr2dmem->pBase;

	//DBG("%s(%p, %d) => TRUE (%p)\n", __func__, pPix, index, pPix->devPrivate.ptr);

	if (index == EXA_PREPARE_DEST)
		ppix->bCPUWrites = TRUE;

	return TRUE;
}

static void PVR2DFinishAccess(PixmapPtr pPix, int index)
{
	struct PVR2DPixmap *ppix = exaGetPixmapDriverPrivate(pPix);

	if (ppix &&
#if USE_SHM
	    (ppix->shmaddr
#if USE_MALLOC
	     || ppix->mallocaddr
#endif /* USE_MALLOC */
	    )
#else
	    ppix->pvr2dmem
#endif
	    ) {
		pPix->devPrivate.ptr = NULL;
	}
	//DBG("%s(%p, %d, %d)\n", __func__, pPix, index, ppix->screen);
}

static Bool PVR2DPixmapIsOffscreen(PixmapPtr pPixmap)
{
	//DBG("%s(%p)\n", __func__, pPixmap);

	return TRUE;
}

static void unmapCallback(void *k, void *v, void *data)
{
	struct PVR2DPixmap *ppix = (struct PVR2DPixmap *)k;
	/* check if pixmap is used by GPU */
	if (PVR2D_OK != QueryBlitsComplete(ppix, 0))
		return;
	PVR2DInvalidate(ppix);
}

/* PVR2DUnmapAllPixmaps
 * Attempt to unmap all pixmaps from GPU.
 * Don't unmap pixmaps in use
 */
void PVR2DUnmapAllPixmaps(void)
{
	if (pixmapsHT)
		x_hash_table_foreach(pixmapsHT, unmapCallback, NULL);
}

static void changescreenCallback(void *k, void *v, void *data)
{
	struct PVR2DPixmap *ppix = (struct PVR2DPixmap *)k;
	if (ppix->screen) {
		ppix->pvr2dmem = pSysMemInfo;
#if USE_SHM
		if (pSysMemInfo)
			ppix->shmaddr = pSysMemInfo->pBase;
		else
			ppix->shmaddr = NULL;
#endif
	}
}
void SysMemInfoChanged(void)
{
	if (pixmapsHT)
		x_hash_table_foreach(pixmapsHT, changescreenCallback, NULL);
}


Bool EXA_Init(ScreenPtr pScreen)
{
	ExaDriverPtr exa;
	XF86ModReqInfo exaReq = {.majorversion = EXA_VERSION_MAJOR,
		.minorversion = EXA_VERSION_MINOR
	};
	int errmaj, errmin;
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];

	if (!LoadSubModule
	    (pScrn->module, "exa", NULL, NULL, NULL, &exaReq, &errmaj,
	     &errmin)) {
		LoaderErrorMsg(NULL, "exa", errmaj, errmin);
		return FALSE;
	}

	exa = exaDriverAlloc();

	if (!exa) {
		FatalError("exaDriverAlloc() failed\n");
		return FALSE;
	}

	exa->exa_major = EXA_VERSION_MAJOR;
	exa->exa_minor = EXA_VERSION_MINOR;
	exa->flags = EXA_OFFSCREEN_PIXMAPS | EXA_HANDLES_PIXMAPS;

	exa->pixmapPitchAlign = 1;
	exa->maxX = 1 << 16;
	exa->maxY = 1 << 16;

	exa->PrepareSolid = PVR2DPrepareSolid;
	exa->Solid = PVR2DSolid;
	exa->DoneSolid = PVR2DDoneSolid;

	exa->PrepareCopy = PVR2DPrepareCopy;
	exa->Copy = PVR2DCopy;
	exa->DoneCopy = PVR2DDoneCopy;

#ifdef PVR2D_EXT_BLIT
	exa->CheckComposite = PVR2DCheckComposite;
	exa->PrepareComposite = PVR2DPrepareComposite;
	exa->Composite = PVR2DComposite;
	exa->DoneComposite = PVR2DDoneComposite;
#endif

	exa->WaitMarker = PVR2DWaitMarker;

	exa->PrepareAccess = PVR2DPrepareAccess;
	exa->FinishAccess = PVR2DFinishAccess;

	exa->PixmapIsOffscreen = PVR2DPixmapIsOffscreen;

	exa->CreatePixmap2 = PVR2DCreatePixmap2;
	exa->DestroyPixmap = PVR2DDestroyPixmap;
	exa->ModifyPixmapHeader = PVR2DModifyPixmapHeader;

	if (!exaDriverInit(pScreen, exa)) {
		FatalError("exaDriverInit() failed\n");
		return FALSE;
	}

	if (!PVR2D_Init()) {
		FatalError("PVR2D_Init() failed\n");
		return FALSE;
	}

	CreateScreenPixmap = TRUE;

#if USE_SHM && defined(DRI2)
	if (!DRI2_Init(pScreen))
		FatalError("DRI2_Init() failed\n");
#endif

	SavedBlockHandler = pScreen->BlockHandler;
	pScreen->BlockHandler = PVR2DBlockHandler;

	pixmapsHT = x_hash_table_new(NULL, NULL, NULL, NULL);
	return TRUE;
}

void EXA_Fini(ScreenPtr pScreen)
{
	if (pScreen->BlockHandler == PVR2DBlockHandler)
		pScreen->BlockHandler = SavedBlockHandler;

	PVR2DDelayedMemDestroy(TRUE);
	PVR2D_DeInit();

	if (pixmapsHT)
	{
		x_hash_table_free(pixmapsHT);
		pixmapsHT = NULL;
	}
}
