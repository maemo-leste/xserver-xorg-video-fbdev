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

typedef struct _seginfo {
	int shmid;
	void *addr;
	void *pvr2dmem;
	void *mallocaddr;
} seginfo;

typedef struct _segments {
	int count;		//current number of elements on the list;
	int maxsize;		//maximum number of elements on the list
	seginfo *table;
} cache_segment;

/*
 * we keep a pool of SHM segments for a few, small and frequently used segment
 * sizes
 */
#define NUM_CACHE_SEGS	4
static cache_segment segments[NUM_CACHE_SEGS];

static int InitCacheSegment(cache_segment * seg, int size)
{
	CALLTRACE("%s:Init segment %p\n", __func__, seg);
	seg->table = 0;
	seg->table = malloc(size * sizeof(seginfo));
	if (!seg->table)
		return 0;
	seg->count = 0;
	seg->maxsize = size;
	return 1;
}

static void CleanupCacheSegment(cache_segment * seg)
{
	int i;
	for (i = 0; i < seg->count; i++) {
		if (seg->table[i].addr) {
			shmdt(seg->table[i].addr);
			shmctl(seg->table[i].shmid, IPC_RMID, NULL);
		}
		if (seg->table[i].pvr2dmem) {
			PVR2DMemFree(hPVR2DContext, seg->table[i].pvr2dmem);
		}
		if (seg->table[i].mallocaddr) {
			xfree(seg->table[i].mallocaddr);
		}
	}
	seg->count = 0;
}

static void DeInitCacheSegment(cache_segment * seg)
{
	/*
	 * At this point the memory should be detached
	 */
	CALLTRACE("%s:DeInit segment %p\n", __func__, seg);
	CleanupCacheSegment(seg);
	if (seg->table)
		free(seg->table);
	seg->table = 0;
	seg->maxsize = 0;
}

int InitSharedSegments(void)
{
	int i;
	int ret = 0;

	for (i = 0; i < NUM_CACHE_SEGS; i++)
		ret |= InitCacheSegment(&segments[i], 8);

	return ret;
}

void DeInitSharedSegments(void)
{
	int i;

	for (i = 0; i < NUM_CACHE_SEGS; i++)
		DeInitCacheSegment(&segments[i]);
}

void CleanupSharedSegments(void)
{
	int i;

	for (i = 0; i < NUM_CACHE_SEGS; i++)
		CleanupCacheSegment(&segments[i]);
}

/*
 * Try to add an SHM to the cache
 * inputs:
 * 	shmid:	SHM ID
 * 	pointer:	address where the memory is attached as returned by shmat
 * 	pvr2dmem:	address where the memory is mapped in PVR2D, may be NULL
 * 	mallocaddr:	address where the memory is mapped by malloc, may be NULL
 * 	size:		size of allocation
 * returns:
 * 	0:	SHM has not been added
 * 	1:	SHM has been stored in cache
 */
int AddToCache(int shmid, void *pointer, void *pvr2dmem, void *mallocaddr,
	       int size)
{
	int size_in_pages =
	    ((size + getpagesize() - 1) & ~(getpagesize() - 1)) / getpagesize();
	CALLTRACE("%s: Start %i\n", __func__, size_in_pages);
	if ((shmid < 0) || (!pointer))
		return 0;
	// check if this segments of this size are cached
	if (size_in_pages < 1 || size_in_pages >= NUM_CACHE_SEGS) {
		return 0;
	}
	CALLTRACE("%s: Mid 1\n", __func__);
	CALLTRACE("%s: Mid 2\n", __func__);

	cache_segment *seg = &segments[size_in_pages - 1];
	CALLTRACE("%s: Seg %p \n", __func__, seg);
	CALLTRACE("%s: count %i, maxsize %i\n", __func__, seg->count,
		  seg->maxsize);
	// check if there is space in the list
	if (seg->count == seg->maxsize)
		return 0;
	seg->table[seg->count].shmid = shmid;
	seg->table[seg->count].addr = pointer;
	seg->table[seg->count].pvr2dmem = pvr2dmem;
	seg->table[seg->count].mallocaddr = mallocaddr;
	seg->count++;
	DebugF("%s: Added %i,%p, size %i pages\n", __func__, shmid, pointer,
	    size_in_pages);
	return 1;
}

/*
 * Try to add an SHM to the cache
 * inputs:
 * 	size:	requested size of memory
 * outputs:	
 *      shmid:  SHM ID
 *      pointer:        address where the memory is attached as returned by shmat
 *      pvr2dmem:       address where the memory is mapped in PVR2D, may be NULL
 *      mallocaddr:     address where the memory is mapped by malloc, may be NULL
 * returns:
 *      0:      no shared segment of this size is available
 *      1:      a segment has been found and the outputs have been filled
 */

int GetFromCache(int *shmid, void **pointer, void **pvr2dmem, void **mallocaddr,
		 int size)
{
	int size_in_pages =
	    ((size + getpagesize() - 1) & ~(getpagesize() - 1)) / getpagesize();
	CALLTRACE("%s: Start %i\n", __func__, size_in_pages);

	// check if segments of this size are cached
	if (size_in_pages < 1 || size_in_pages >= NUM_CACHE_SEGS) {
		CALLTRACE("%s: size %i, return\n", __func__, size_in_pages);
		return 0;
	}

	CALLTRACE("%s: Mid %i\n", __func__, size_in_pages);
	CALLTRACE("%s: Mid 2 %i\n", __func__, size_in_pages);
	cache_segment *seg = &segments[size_in_pages - 1];
	CALLTRACE("%s: Seg %p \n", __func__, seg);
	CALLTRACE("%s: count %i, maxsize %i\n", __func__, seg->count, seg->maxsize);

	// check if the list is non-empty
	if (seg->count == 0)
		return 0;
	seg->count--;

	if (shmid)
		*shmid = seg->table[seg->count].shmid;
	if (pointer)
		*pointer = seg->table[seg->count].addr;
	if (pvr2dmem)
		*pvr2dmem = seg->table[seg->count].pvr2dmem;
	if (mallocaddr)
		*mallocaddr = seg->table[seg->count].mallocaddr;

	DebugF("%s: Reused %i,%p, size %i pages\n", __func__, *shmid, *pointer, size_in_pages);

	return 1;
}
