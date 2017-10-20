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

#include "xorg-server.h"
#include "xf86.h"
#include "exa.h"

#if USE_SHM && defined(DRI2)

#include <xf86drm.h>
#include "dri2.h"

#include "sgx_pvr2d.h"

//#define DebugF	ErrorF

typedef struct {
	PixmapPtr pPixmap;
} PVR2DDRI2BufferPrivateRec, *PVR2DDRI2BufferPrivatePtr;

static Bool PVR2DDRI2MigratePixmap(struct PVR2DPixmap *ppix)
{
	int shmsize;
	void *mallocaddr;

#if USE_MALLOC
	if (ppix->mallocaddr) {
		shmsize = ppix->shmsize;
		mallocaddr = ppix->mallocaddr;
		if (!PVR2DAllocSHM(ppix)) {
			return FALSE;
		}
		DebugF("%s: memcpy %d bytes from malloc (%p) to SHM (%p)\n", __func__, shmsize, mallocaddr, ppix->shmaddr);
		memcpy(ppix->shmaddr, mallocaddr, shmsize);
		xfree(mallocaddr);
		ppix->mallocaddr = NULL;
	}
#endif

	if (!ppix->shmaddr && ppix->pvr2dmem != pSysMemInfo) {
		if (QueryBlitsComplete(ppix, 1) != PVR2D_OK || !PVR2DAllocSHM(ppix)) {
			return FALSE;
		}

		DebugF("%s: memcpy %d bytes from PVR (%p) to SHM (%p)\n", __func__, ppix->shmsize, ppix->pvr2dmem->pBase, ppix->shmaddr);
		memcpy(ppix->shmaddr, ppix->pvr2dmem->pBase, ppix->shmsize);
		PVR2DMemFree(hPVR2DContext, ppix->pvr2dmem);
		ppix->pvr2dmem = NULL;
	}
	if (!PVR2DValidate(ppix, TRUE)) {
		ErrorF("%s: !PVR2DValidate()\n", __func__);
		return FALSE;
	}

	PVR2DPixmapOwnership_GPU(ppix);

	return TRUE;
}

static DRI2BufferPtr PVR2DDRI2CreateBuffers(DrawablePtr pDraw,
					    unsigned int *attachments,
					    int count)
{
	int i;
	ScreenPtr pScreen = pDraw->pScreen;
	DRI2BufferPtr buffers;
	PVR2DDRI2BufferPrivatePtr privates;
	struct PVR2DPixmap *ppix;

	buffers = xcalloc(count, sizeof *buffers);
	if (buffers == NULL)
		return NULL;
	privates = xcalloc(count, sizeof *privates);
	if (privates == NULL) {
		xfree(buffers);
		return NULL;
	}

	for (i = 0; i < count; i++) {
		buffers[i].attachment = attachments[i];
		buffers[i].driverPrivate = &privates[i];

		if (attachments[i] == DRI2BufferStencil || attachments[i] == DRI2BufferDepth) {
			continue;
		}

		if (attachments[i] == DRI2BufferFrontLeft) {
			if (pDraw->type == DRAWABLE_WINDOW)
				privates[i].pPixmap = (*pScreen->GetWindowPixmap) ((WindowPtr) pDraw);
			else
				privates[i].pPixmap = (PixmapPtr) pDraw;
			privates[i].pPixmap->refcnt++;
			privates[i].pPixmap->usage_hint = CREATE_PIXMAP_USAGE_BACKING_PIXMAP;
		} else {
			privates[i].pPixmap = (*pScreen->CreatePixmap) (pScreen, pDraw->width, pDraw->height, pDraw->depth, CREATE_PIXMAP_USAGE_BACKING_PIXMAP);
		}

		if (!privates[i].pPixmap) {
			goto err;
		}

		ppix = exaGetPixmapDriverPrivate(privates[i].pPixmap);
		ppix->dribuffer = (attachments[i] == DRI2BufferFrontLeft);

		if (!PVR2DDRI2MigratePixmap(ppix)) {
			goto err;
		}

		buffers[i].pitch = privates[i].pPixmap->devKind;
		buffers[i].cpp = privates[i].pPixmap->drawable.bitsPerPixel / 8;
		buffers[i].name = ppix->shmid;
	}

	return buffers;

err:
	/* clean up pixmaps we might have allocated */
	for (; i >= 0; i--) {
		if (attachments[i] != DRI2BufferStencil && attachments[i] != DRI2BufferDepth) {
			if (attachments[i] == DRI2BufferFrontLeft) {
				privates[i].pPixmap->refcnt--;
			} else {
				if (privates[i].pPixmap)
					(*pScreen->DestroyPixmap) (privates[i].pPixmap);
			}
		}
	}

	xfree(buffers);
	xfree(privates);

	return NULL;
}

static void PVR2DDRI2ValidateBuffers(DrawablePtr pDraw, DRI2BufferPtr buffers,
				     int count)
{
	int i;

	for (i = 0; i < count; i++) {
		PVR2DDRI2BufferPrivatePtr private = buffers[i].driverPrivate;

		if (private->pPixmap) {
			struct PVR2DPixmap *ppix =
			    exaGetPixmapDriverPrivate(private->pPixmap);

			if (ppix->dribuffer) {
				PVR2DFlushCache(ppix);
			}
		}
	}
}

static void PVR2DDRI2DestroyBuffers(DrawablePtr pDraw, DRI2BufferPtr buffers,
				    int count)
{
	ScreenPtr pScreen = pDraw->pScreen;
	int i;

	for (i = 0; i < count; i++) {
		PVR2DDRI2BufferPrivatePtr private = buffers[i].driverPrivate;

		if (private->pPixmap)
			(*pScreen->DestroyPixmap) (private->pPixmap);
	}

	if (buffers) {
		xfree(buffers[0].driverPrivate);
		xfree(buffers);
	}
}

static DrawablePtr PVR2DDRI2ChooseBufferDrawable(DrawablePtr pDraw,
						 DRI2BufferPtr pBuffer)
{
	if (pBuffer->attachment == DRI2BufferFrontLeft)
		return pDraw;
	else {
		PVR2DDRI2BufferPrivatePtr priv = pBuffer->driverPrivate;

		return &priv->pPixmap->drawable;
	}
}

static void PVR2DDRI2CopyRegion(DrawablePtr pDraw, RegionPtr pRegion,
				DRI2BufferPtr pDstBuffer,
				DRI2BufferPtr pSrcBuffer)
{
	DrawablePtr pSrcDraw = PVR2DDRI2ChooseBufferDrawable(pDraw, pSrcBuffer);
	DrawablePtr pDstDraw = PVR2DDRI2ChooseBufferDrawable(pDraw, pDstBuffer);
	ScreenPtr pScreen = pDstDraw->pScreen;
	RegionPtr pCopyClip = REGION_CREATE(pScreen, NULL, 0);
	GCPtr pGC;

	pGC = GetScratchGC(pDstDraw->depth, pScreen);
	REGION_COPY(pScreen, pCopyClip, pRegion);
	(*pGC->funcs->ChangeClip) (pGC, CT_REGION, pCopyClip, 0);
	ValidateGC(pDstDraw, pGC);

	(*pGC->ops->CopyArea) (pSrcDraw, pDstDraw, pGC, 0, 0, pDraw->width,
			       pDraw->height, 0, 0);

	FreeScratchGC(pGC);
}

Bool DRI2_Init(ScreenPtr pScreen)
{
	if (xf86LoadSubModule(xf86Screens[pScreen->myNum], "dri2")) {
		DRI2InfoRec info;

		info.driverName = "pvr2d";
		info.deviceName = NULL;
		info.version = 2;
		info.fd = 0; /* drmFD is no longer used */

		info.CreateBuffers = PVR2DDRI2CreateBuffers;
		info.ValidateBuffers = PVR2DDRI2ValidateBuffers;
		info.DestroyBuffers = PVR2DDRI2DestroyBuffers;
		info.CopyRegion = PVR2DDRI2CopyRegion;
		info.Wait = NULL;

		return DRI2ScreenInit(pScreen, &info);
	}

	return FALSE;
}

#endif /* USE_SHM && defined(DRI2) */
