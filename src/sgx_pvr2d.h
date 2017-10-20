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

#ifndef SGX_PVR2D_H

#define SGX_PVR2D_H 1

#include "xorg-server.h"
#include "xf86.h"

#define PVR2D_EXT_BLIT 1
#include "pvr2d.h"
#include "sgxfeaturedefs.h"
#include "sgxdefs.h"

#if USE_SHM
#include <xf86drm.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "sgx_exa.h"
#include "sgx_cache.h"

struct PVR2DPixmap {
	PVR2DMEMINFO *pvr2dmem;
	enum {
		PVR2D_OWNER_UNDEFINED = 0,
		PVR2D_OWNER_CPU,
		PVR2D_OWNER_GPU
	} owner;

	IMG_UINT32 ui32WriteOpsComplete;	// how many operations GPU has completed on the pixmap
	Bool bCPUWrites;	// has PreparedAccess been called with EXA_PREPARE_DEST

	Bool dribuffer;
#if USE_SHM
	Bool screen;
	int shmid;
	int shmsize;
	void *shmaddr;
#if USE_MALLOC
	void *mallocaddr;
#endif /* USE_MALLOC */
#endif
	int usage_hint;
};

enum drm_pvr2d_cflush_type {
	DRM_PVR2D_CFLUSH_FROM_GPU = 1,
	DRM_PVR2D_CFLUSH_TO_GPU = 2
};

//#define SGX_PVR2D_CALL_STATS

#ifdef SGX_PVR2D_CALL_STATS
/* call counters  */
typedef struct _sgx_pvr2d_call_stats {
	unsigned int solidOP;	// solid fill operations
	unsigned int copyOP;	// copy operations
	unsigned int flushOP;	// flush cache operations
	unsigned int invOP;	// invalidate cache operations
} sgx_pvr2d_call_stats;
#endif

extern PVR2DCONTEXTHANDLE hPVR2DContext;
extern PVR2DMEMINFO *pSysMemInfo;

#if defined(SGX_PVR2D_CALL_STATS)
extern sgx_pvr2d_call_stats callStats;
#endif
Bool PVR2D_Init(void);
void PVR2D_DeInit(void);
Bool PVR2D_PostFBReset(void);
Bool PVR2D_PreFBReset(void);
#if USE_MALLOC && USE_SHM
Bool PVR2DAllocNormal(struct PVR2DPixmap *ppix);
#endif
#if USE_SHM
Bool PVR2DAllocSHM(struct PVR2DPixmap *ppix);
int PVR2DGetFlushSize(struct PVR2DPixmap *ppix);
void PVR2DFlushCache(struct PVR2DPixmap *ppix);
#endif
PVR2DERROR QueryBlitsComplete(struct PVR2DPixmap *ppix, unsigned int wait);
void PVR2DInvalidate(struct PVR2DPixmap *ppix);
void PVR2DDelayedMemDestroy(Bool wait);
void DestroyPVR2DMemory(struct PVR2DPixmap *ppix);
void PVR2DPixmapOwnership_GPU(struct PVR2DPixmap *ppix);
Bool PVR2DPixmapOwnership_CPU(struct PVR2DPixmap *ppix);
Bool PVR2DValidate(struct PVR2DPixmap *ppix, Bool cleanup);

#endif /* SGX_PVR2D_H */
