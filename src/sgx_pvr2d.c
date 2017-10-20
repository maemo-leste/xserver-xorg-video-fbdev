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
#include "services.h"

#if USE_SHM
#include <sys/shm.h>
#endif

/* These should really be tracked per screen but are global for now */
PVR2DCONTEXTHANDLE hPVR2DContext;
PVR2DMEMINFO *pSysMemInfo;

#ifdef SGX_PVR2D_CALL_STATS
sgx_pvr2d_call_stats callStats;
#endif

/*
 * PVR2D_PostFBReset
 *
 * Call this function after resetting the framebuffer.
 *
 * Maps the framebuffer.
 * Resets memory mapping in all 'screen' pixmaps to new
 * system memory.
 */
Bool PVR2D_PostFBReset(void)
{
	PVR2DMEMINFO *pMemInfo = pSysMemInfo;
	PVR2DERROR ePVR2DStatus;
	PVR2DFORMAT Format;
	long lWidth;
	long lHeight;
	long lStride;
	int iRefreshRate;

	/* query display device for new settings */
	ePVR2DStatus = PVR2DGetScreenMode(hPVR2DContext,
			&Format,
			&lWidth,
			&lHeight,
			&lStride,
			&iRefreshRate);
	if (ePVR2DStatus != PVR2D_OK) {
		ErrorF("PVR2DGetScreenMode failed\n");
		return FALSE;
	}
	if (!pSysMemInfo) {
		ePVR2DStatus =
			PVR2DGetFrameBuffer(hPVR2DContext,
					PVR2D_FB_PRIMARY_SURFACE,
					&pSysMemInfo);
		if (ePVR2DStatus != PVR2D_OK)
			ErrorF("PVR2DGetFrameBuffer failed\n");
	}

	if (pMemInfo != pSysMemInfo)
		SysMemInfoChanged();

	return TRUE;
}

/*
 * PVR2D_PreFBReset
 *
 * Call this function before resetting the framebuffer.
 *
 * Makes sure that all blits to the framebuffer are complete.
 * Releases the framebuffer.
 * Resets memory mapping in all 'screen' pixmaps to NULL.
 */
Bool PVR2D_PreFBReset(void)
{
	PVR2DQueryBlitsComplete(hPVR2DContext, pSysMemInfo, TRUE);
	if (PVR2DFreeFrameBuffer(hPVR2DContext, PVR2D_FB_PRIMARY_SURFACE)
			!= PVR2D_OK) {
		ErrorF("PVR2DFreeFramebuffer failed\n");
		return FALSE;
	} else {
		pSysMemInfo = NULL;
		SysMemInfoChanged();
	}
	return TRUE;
}


Bool PVR2D_Init(void)
{
	PVR2DERROR ePVR2DStatus;
	PVR2DDEVICEINFO *pDevInfo = 0;
	int nDeviceNum;
	int nDevices;
	long lRevMajor = 0;
	long lRevMinor = 0;

#if SGX_CACHE_SEGMENTS
	InitSharedSegments();
#endif

	if (pSysMemInfo)
		return TRUE;

	PVR2DGetAPIRev(&lRevMajor, &lRevMinor);

	if (lRevMajor != PVR2D_REV_MAJOR || lRevMinor != PVR2D_REV_MINOR) {
		ErrorF("Warning - PVR2D API revision mismatch\n");
	}

	/* Get number of devices */
	nDevices = PVR2DEnumerateDevices(0);

	if ((nDevices < PVR2D_OK) || (nDevices == 0)) {
		ErrorF("PowerVR device not found\n");
		return FALSE;
	}

	/* Allocate memory for devices */
	pDevInfo =
	    (PVR2DDEVICEINFO *) malloc(nDevices * sizeof(PVR2DDEVICEINFO));

	if (!pDevInfo) {
		ErrorF("malloc failed\n");
		return FALSE;
	}

	/* Get the devices */
	ePVR2DStatus = PVR2DEnumerateDevices(pDevInfo);

	if (ePVR2DStatus != PVR2D_OK) {
		ErrorF("PVR2DEnumerateDevices failed\n");
		return FALSE;
	}

	/* Choose the first display device */
	nDeviceNum = pDevInfo[0].ulDevID;

	/* Create the device context */
	ePVR2DStatus = PVR2DCreateDeviceContext(nDeviceNum, &hPVR2DContext, 0);

	if (ePVR2DStatus != PVR2D_OK) {
		return FALSE;
	}

	ePVR2DStatus =
	    PVR2DGetFrameBuffer(hPVR2DContext, PVR2D_FB_PRIMARY_SURFACE,
				&pSysMemInfo);

	if (ePVR2DStatus != PVR2D_OK) {
		ErrorF("PVR2DGetFrameBuffer failed\n");
		return FALSE;
	}

	return TRUE;
}

void PVR2D_DeInit(void)
{
#if SGX_CACHE_SEGMENTS
	DeInitSharedSegments();
#endif
}

Bool PVR2DAllocSHM(struct PVR2DPixmap *ppix)
{
#if USE_SHM
	CALLTRACE("%s: Start\n", __func__);

	ppix->shmsize = (ppix->shmsize + getpagesize() - 1) & ~(getpagesize() - 1);

#if SGX_CACHE_SEGMENTS
	if (GetFromCache
	    (&ppix->shmid, &ppix->shmaddr, (void *)&ppix->pvr2dmem, NULL,
	     ppix->shmsize))
		if (ppix->shmaddr)
			return TRUE;
#endif

	ppix->shmid = shmget(IPC_PRIVATE, ppix->shmsize, IPC_CREAT | 0666);

	if (ppix->shmid == -1) {
		perror("shmget failed");
		return FALSE;
	}

	/* lock the SHM segment. This prevents swapping out the memory.
	 * Cache flush/invalidate could cause unhandled page fault
	 * if shared memory was swapped out
	 */
	if (0 != shmctl(ppix->shmid, SHM_LOCK, 0))
		ErrorF("shmctl(SHM_LOCK) failed\n");

	ppix->shmaddr = shmat(ppix->shmid, NULL, 0);

	if (!ppix->shmaddr) {
		perror("shmat failed");
		shmctl(ppix->shmid, IPC_RMID, NULL);
		return FALSE;
	}

	return TRUE;
#else
	return FALSE;
#endif
}

#if USE_MALLOC
Bool PVR2DAllocNormal(struct PVR2DPixmap *ppix)
{
#if SGX_CACHE_SEGMENTS
	ppix->shmsize = (ppix->shmsize + getpagesize() - 1) & ~(getpagesize() - 1);

	if (GetFromCache
	    (&ppix->shmid, &ppix->shmaddr, (void *)&ppix->pvr2dmem, &ppix->mallocaddr,
	     ppix->shmsize))
		return TRUE;
#endif

	ppix->mallocaddr = xcalloc(1, ppix->shmsize);
	if (ppix->mallocaddr)
		return TRUE;

	return FALSE;
}
#endif

#if USE_SHM

/* returns how much memory PVR2DFlushCache would flush */
int PVR2DGetFlushSize(struct PVR2DPixmap *ppix)
{
	if (ppix->pvr2dmem == pSysMemInfo || ppix->shmid == -1 || !ppix->shmaddr
	    || !ppix->shmsize)
		return 0;
	if ((ppix->owner == PVR2D_OWNER_GPU) && (ppix->pvr2dmem)) {
		PVRSRV_CLIENT_MEM_INFO *pMemInfo =
		    (PVRSRV_CLIENT_MEM_INFO *) ppix->pvr2dmem->hPrivateData;
		IMG_UINT32 ui32WriteOpsComplete =
		    pMemInfo->psClientSyncInfo->psSyncData->
		    ui32WriteOpsComplete;
		if (ui32WriteOpsComplete == ppix->ui32WriteOpsComplete)
			return 0;
	}
	if ((ppix->owner == PVR2D_OWNER_CPU) && (!ppix->bCPUWrites)) {
		return 0;
	}
	return ppix->shmsize;
}

void PVR2DFlushCache(struct PVR2DPixmap *ppix)
{
	unsigned int cflush_type;
	unsigned long cflush_virt;
	unsigned int cflush_length;
	Bool bNeedFlush = FALSE;

	if (ppix->pvr2dmem == pSysMemInfo || ppix->shmid == -1 || !ppix->shmaddr
	    || !ppix->shmsize)
		return;

	/* check if pixmap was modified
	 * by GPU: ui32WriteOpsComplete changed
	 * by CPU: PrepareAccess was called with EXA_PREPARE_DEST
	 */
	if ((ppix->owner == PVR2D_OWNER_CPU) && (ppix->pvr2dmem)) {
		bNeedFlush = ppix->bCPUWrites;
	} else if ((ppix->owner == PVR2D_OWNER_GPU) && (ppix->pvr2dmem)) {
		PVRSRV_CLIENT_MEM_INFO *pMemInfo =
		    (PVRSRV_CLIENT_MEM_INFO *) ppix->pvr2dmem->hPrivateData;
		IMG_UINT32 ui32WriteOpsComplete =
		    pMemInfo->psClientSyncInfo->psSyncData->
		    ui32WriteOpsComplete;
		bNeedFlush = ui32WriteOpsComplete != ppix->ui32WriteOpsComplete;
		ppix->ui32WriteOpsComplete = ui32WriteOpsComplete;
	} else if (ppix->bCPUWrites) {
		bNeedFlush = TRUE;
	}
	ppix->bCPUWrites = FALSE;
	//ErrorF("%s: Need flush? %s\n",__func__, (bNeedFlush ? "true" : "false") );

	if (bNeedFlush) {
		cflush_type =
		    ppix->owner ==
		    PVR2D_OWNER_GPU ? DRM_PVR2D_CFLUSH_FROM_GPU :
		    DRM_PVR2D_CFLUSH_TO_GPU;
		cflush_virt = (uint32_t) ppix->shmaddr;
		cflush_length = ppix->shmsize;

#ifdef SGX_PVR2D_CALL_STATS
		if (cflush_type == DRM_PVR2D_CFLUSH_FROM_GPU)
			callStats.invOP++;
		else
			callStats.flushOP++;
#endif

		if (PVR2D_OK !=
			PVR2DCacheFlushDRI(hPVR2DContext, cflush_type, cflush_virt, cflush_length)) {
			ErrorF("DRM_PVR2D_CFLUSH ioctl failed\n");
		}
	}
}

#endif // USE_SHM

PVR2DERROR QueryBlitsComplete(struct PVR2DPixmap *ppix, unsigned int wait)
{
	if (ppix->owner == PVR2D_OWNER_CPU)
		return PVR2D_OK;

	return PVR2DQueryBlitsComplete(hPVR2DContext, ppix->pvr2dmem, wait);
}

/* PVR2D memory can only be freed once all PVR2D operations using it have
 * completed. In order to avoid waiting for this synchronously, defer freeing
 * of PVR2D memory with outstanding operations until an appropriate time.
 */
static struct PVR2DMemDestroy {
	struct PVR2DPixmap pix;
	struct PVR2DMemDestroy *next;
} *DelayedPVR2DMemDestroy;

static void DoDestroyPVR2DMemory(struct PVR2DPixmap *ppix)
{
	CALLTRACE("%s: Start\n", __func__);

#if SGX_CACHE_SEGMENTS
	if (AddToCache
	    (ppix->shmid, ppix->shmaddr, ppix->pvr2dmem, ppix->mallocaddr,
	     ppix->shmsize))
		return;
#endif

	if (ppix->pvr2dmem)
		PVR2DMemFree(hPVR2DContext, ppix->pvr2dmem);

#if USE_SHM
	if (ppix->shmid != -1) {
		shmdt(ppix->shmaddr);
		shmctl(ppix->shmid, IPC_RMID, NULL);
		ppix->shmaddr = NULL;
		ppix->shmid = -1;
	}
#endif
#if USE_MALLOC
	if (ppix->mallocaddr) {
		xfree(ppix->mallocaddr);
		ppix->mallocaddr = NULL;
	}
#endif /* USE_MALLOC */
}

/* PVR2DInvalidate
 * Unmap the pixmap from GPU.
 */
void PVR2DInvalidate(struct PVR2DPixmap *ppix)
{
#if USE_SHM
	if ((ppix->shmid != -1) && (ppix->pvr2dmem)) {
		DBG("%s: size %u\n", __func__, ppix->shmsize);
		PVR2DMemFree(hPVR2DContext, ppix->pvr2dmem);
		ppix->pvr2dmem = NULL;
	}
#endif
}

void PVR2DDelayedMemDestroy(Bool wait)
{
	while (DelayedPVR2DMemDestroy) {
		struct PVR2DMemDestroy *destroy = DelayedPVR2DMemDestroy;
		Bool complete =
		    QueryBlitsComplete(&destroy->pix, wait) == PVR2D_OK;

		if (complete || wait) {
			/* This should never happen, but in case it does... */
			if (!complete)
				ErrorF
				    ("Freeing PVR2D memory %p despite incomplete blits! SGX "
				     "may lock up...\n", destroy->pix.pvr2dmem);

			DoDestroyPVR2DMemory(&destroy->pix);

			DelayedPVR2DMemDestroy = destroy->next;
			xfree(destroy);
		} else
			return;
	}
}

void DestroyPVR2DMemory(struct PVR2DPixmap *ppix)
{
	struct PVR2DMemDestroy *destroy;

	if (ppix->pvr2dmem == pSysMemInfo)
		return;

	PVR2DDelayedMemDestroy(FALSE);

	/* Can we free PVR2D memory right away? */
	if (!ppix->pvr2dmem || QueryBlitsComplete(ppix, 0) == PVR2D_OK) {
		DoDestroyPVR2DMemory(ppix);
		return;
	}

	/* No, schedule for delayed freeing */
	destroy = xalloc(sizeof(*destroy));

	destroy->pix = *ppix;
	destroy->next = DelayedPVR2DMemDestroy;
	DelayedPVR2DMemDestroy = destroy;
}

/*
 * Transfer ownership of a pixmap to GPU
 * It makes sure the cache is flushed.
 * Call PVR2DValidate before calling this
 */
void PVR2DPixmapOwnership_GPU(struct PVR2DPixmap *ppix)
{
	if (!ppix) {
		DBG("%s(%p) => FALSE\n", __func__, ppix);
		return;
	}

	if (ppix->owner != PVR2D_OWNER_GPU) {
		PVR2DFlushCache(ppix);
		ppix->owner = PVR2D_OWNER_GPU;
	}
}

/*
 * Transfer ownership of a pixmap to CPU
 * It makes sure the cache is flushed.
 */
Bool PVR2DPixmapOwnership_CPU(struct PVR2DPixmap *ppix)
{
	if (!ppix) {
		DBG("%s(%p, %d) => FALSE\n", __func__, ppix, index);
		return FALSE;
	}

	if (ppix->pvr2dmem) {
		if (PVR2DQueryBlitsComplete(hPVR2DContext, ppix->pvr2dmem, 0) != PVR2D_OK) {
			DBG("%s: Pending blits!\n", __func__);
			PVR2DQueryBlitsComplete(hPVR2DContext, ppix->pvr2dmem, 1);
		}
	}

	if (ppix->owner != PVR2D_OWNER_CPU) {
		PVR2DFlushCache(ppix);
		ppix->owner = PVR2D_OWNER_CPU;
	}

	//DBG("%s(%p, %d) => TRUE (%p)\n", __func__, pPix, index, pPix->devPrivate.ptr);

	return TRUE;
}

/* PVR2DValidate
 * Validate the pixmap for use in SGX
 * if PVR2DMemWrap fails and cleanup is set, then try to release
 * all memory mapped to GPU and try PVR2DMemWrap again */
Bool PVR2DValidate(struct PVR2DPixmap * ppix, Bool cleanup)
{
#if USE_SHM
	unsigned int num_pages;

	Bool ret;
	unsigned int contiguous = PVR2D_WRAPFLAG_NONCONTIGUOUS;

	if (!ppix) {
		DBG("%s: !pPix: FALSE\n", __func__);
		return FALSE;
	}

	if (ppix->pvr2dmem) {
		DBG("%s: pPix->pvr2dmem: TRUE\n", __func__);
		return TRUE;
	}

	if (!ppix->shmsize || ppix->shmid == -1 || !ppix->shmaddr) {
		DBG("%s: !SHM: FALSE\n", __func__);
		return FALSE;
	}

	num_pages =
	    (((unsigned long)ppix->shmsize + getpagesize() -
	      1) / getpagesize());

	ret = TRUE;

	if (num_pages == 1)
		contiguous = PVR2D_WRAPFLAG_CONTIGUOUS;

	if (PVR2DMemWrap (hPVR2DContext, ppix->shmaddr, contiguous, ppix->shmsize, NULL, &ppix->pvr2dmem) != PVR2D_OK) {
		/* Try again after freeing PVR2D memory asynchronously */
		PVR2DDelayedMemDestroy(FALSE);

		if (PVR2DMemWrap (hPVR2DContext, ppix->shmaddr, contiguous, ppix->shmsize, NULL, &ppix->pvr2dmem) != PVR2D_OK) {
			/* Last resort, try again after freeing PVR2D memory synchronously */
			PVR2DDelayedMemDestroy(TRUE);

			if (PVR2DMemWrap (hPVR2DContext, ppix->shmaddr, contiguous, ppix->shmsize, NULL, &ppix->pvr2dmem) != PVR2D_OK) {
				if (cleanup) {
#if SGX_CACHE_SEGMENTS
					CleanupSharedSegments();
#endif
					PVR2DUnmapAllPixmaps();
					if (PVR2DMemWrap (hPVR2DContext, ppix->shmaddr, contiguous, ppix->shmsize, NULL, &ppix->pvr2dmem) != PVR2D_OK) {
						ErrorF ("%s: Memory wrapping failed\n", __func__);
						ppix->pvr2dmem = NULL;
						ret = FALSE;
					}
				}
			}
		}
	}

	/*ErrorF("%s returning %d\n", __func__, ret); */

	return ret;

#else /* !USE_SHM */

	if (ppix && ppix->pvr2dmem) {
		PVRSRV_CLIENT_MEM_INFO *pMemInfo =
		    (PVRSRV_CLIENT_MEM_INFO *) ppix->pvr2dmem->hPrivateData;
		ppix->ui32WriteOpsComplete =
		    pMemInfo->psClientSyncInfo->psSyncData->
		    ui32WriteOpsComplete;
		ppix->owner = PVR2D_OWNER_CPU;
		return TRUE;
	}

	return FALSE;

#endif /* USE_SHM */
}
