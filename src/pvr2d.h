/*!****************************************************************************
@File          pvr2d.h
@Title         PVR2D external header file
@Author        Imagination Technologies
@Copyright     Copyright (c) by Imagination Technologies Limited.
				This specification is protected by copyright laws and contains
				material proprietary to Imagination Technologies Limited.
				You may use and distribute this specification free of charge for implementing
				the functionality therein, without altering or removing any trademark, copyright,
				or other notice from the specification.
@Platform      Generic
@Description   PVR2D definitions for PVR2D clients
******************************************************************************/


/******************************************************************************
Modifications :-
$Log: pvr2d.h $
******************************************************************************/

#ifndef _PVR2D_H_
#define _PVR2D_H_

#ifdef __cplusplus
extern "C" {
#endif

/* PVR2D Platform-specific definitions */
#define PVR2D_EXPORT
#define PVR2D_IMPORT


#define PVR2D_REV_MAJOR		2
#define PVR2D_REV_MINOR		1

typedef enum
{
	PVR2D_FALSE = 0,
	PVR2D_TRUE
} PVR2D_BOOL;


/* error codes */
typedef enum
{
	PVR2D_OK = 0,
	PVR2DERROR_INVALID_PARAMETER = -1,
	PVR2DERROR_DEVICE_UNAVAILABLE = -2,
	PVR2DERROR_INVALID_CONTEXT = -3,
	PVR2DERROR_MEMORY_UNAVAILABLE = -4,
	PVR2DERROR_DEVICE_NOT_PRESENT = -5,
	PVR2DERROR_IOCTL_ERROR = -6,
	PVR2DERROR_GENERIC_ERROR = -7,
	PVR2DERROR_BLT_NOTCOMPLETE = -8,
	PVR2DERROR_HW_FEATURE_NOT_SUPPORTED = -9,
	PVR2DERROR_NOT_YET_IMPLEMENTED = -10,
	PVR2DERROR_MAPPING_FAILED = -11
}PVR2DERROR;


/* pixel formats */
typedef enum
{
	PVR2D_1BPP = 0,
	PVR2D_RGB565,
	PVR2D_ARGB4444,
	PVR2D_RGB888,
	PVR2D_ARGB8888,
	PVR2D_ARGB1555,
	PVR2D_ALPHA8,
	PVR2D_ALPHA4,
	PVR2D_PAL2,
	PVR2D_PAL4,
	PVR2D_PAL8,
	PVR2D_VGAEMU,
	PVR2D_UYVY,
	PVR2D_YUY2,
	PVR2D_YV12,
	PVR2D_I420

}PVR2DFORMAT;


/* wrap surface type */
typedef enum
{
	PVR2D_WRAPFLAG_NONCONTIGUOUS = 0,
	PVR2D_WRAPFLAG_CONTIGUOUS = 1,

}PVR2DWRAPFLAGS;

/* flags for control information of additional blits */
typedef enum
{
	PVR2D_BLIT_DISABLE_ALL					= 0x0000,	/* disable all additional controls */
	PVR2D_BLIT_CK_ENABLE					= 0x0001,	/* enable colour key */
	PVR2D_BLIT_GLOBAL_ALPHA_ENABLE			= 0x0002,	/* enable standard global alpha */
	PVR2D_BLIT_PERPIXEL_ALPHABLEND_ENABLE	= 0x0004,	/* enable per-pixel alpha bleding */
	PVR2D_BLIT_PAT_SURFACE_ENABLE			= 0x0008,	/* enable pattern surf (disable fill) */
	PVR2D_BLIT_FULLY_SPECIFIED_ALPHA_ENABLE	= 0x0010,	/* enable fully specified alpha */
	PVR2D_BLIT_ROT_90						= 0x0020,	/* apply 90 degree rotation to the blt */
	PVR2D_BLIT_ROT_180						= 0x0040,	/* apply 180 degree rotation to the blt */
	PVR2D_BLIT_ROT_270						= 0x0080,	/* apply 270 degree rotation to the blt */
	PVR2D_BLIT_COPYORDER_TL2BR				= 0x0100,	/* copy order overrides */
	PVR2D_BLIT_COPYORDER_BR2TL				= 0x0200,
	PVR2D_BLIT_COPYORDER_TR2BL				= 0x0400,
	PVR2D_BLIT_COPYORDER_BL2TR				= 0x0800,
	PVR2D_BLIT_COLKEY_SOURCE				= 0x1000,	/* Key colour is on the source surface */
	PVR2D_BLIT_COLKEY_DEST					= 0x2000	/* Key colour is on the destination surface */

} PVR2DBLITFLAGS;

/* standard alpha-blending functions, AlphaBlendingFunc field of PVR2DBLTINFO */
typedef enum
{
	PVR2D_ALPHA_OP_SRC_DSTINV = 1,	/* source alpha : Cdst = Csrc*Asrc + Cdst*(1-Asrc) */
	PVR2D_ALPHA_OP_SRCP_DSTINV = 2	/* premultiplied source alpha : Cdst = Csrc + Cdst*(1-Asrc) */
} PVR2D_ALPHABLENDFUNC;

/* blend ops for fully specified alpha */
typedef enum
{
	PVR2D_BLEND_OP_ZERO = 0,
	PVR2D_BLEND_OP_ONE = 1,
	PVR2D_BLEND_OP_SRC = 2,
	PVR2D_BLEND_OP_DST = 3,
	PVR2D_BLEND_OP_GLOBAL = 4,
	PVR2D_BLEND_OP_SRC_PLUS_GLOBAL = 5,
	PVR2D_BLEND_OP_DST_PLUS_GLOBAL = 6
}PVR2D_BLEND_OP;

typedef enum
{
	PVR2D_FILTER_NEAREST = 0,
	PVR2D_FILTER_LINEAR
}PVR2D_FILTER;

typedef enum
{
	PVR2D_REPEAT_NONE = 0,
	PVR2D_REPEAT_NORMAL,
	PVR2D_REPEAT_PAD,
	PVR2D_REPEAT_MIRROR
}PVR2D_REPEAT;

typedef void* PVR2D_HANDLE;


/* Fully specified alpha blend :	pAlpha field of PVR2DBLTINFO structure					*/
/* a fully specified Alpha Blend operation is defined as									*/
/* DST (ALPHA) = (ALPHA_1 * SRC (ALPHA)) + (ALPHA_3 * DST (ALPHA))							*/
/* DST (RGB)   = (ALPHA_2 * SRC (RGB)) + (ALPHA_4 * DST (RGB))								*/
/* if the pre-multiplication stage is enabled then the equations become the following:		*/
/* PRE_MUL     = ((SRC(A)) * (Global Alpha Value))											*/
/* DST (ALPHA) = (ALPHA_1 * SRC (ALPHA)) + (PRE_MUL * DST (ALPHA))							*/
/* DST (RGB)   = (ALPHA_2 * SRC (RGB)) + (PRE_MUL * DST (RGB))								*/
/* if the transparent source alpha stage is enabled then a source alpha of zero forces the	*/
/* source to be transparent for that pixel regardless of the blend equation being used.		*/
typedef struct _PVR2D_ALPHABLT
{
	PVR2D_BLEND_OP	eAlpha1;
	PVR2D_BOOL		bAlpha1Invert;
	PVR2D_BLEND_OP	eAlpha2;
	PVR2D_BOOL		bAlpha2Invert;
	PVR2D_BLEND_OP	eAlpha3;
	PVR2D_BOOL		bAlpha3Invert;
	PVR2D_BLEND_OP	eAlpha4;
	PVR2D_BOOL		bAlpha4Invert;
	PVR2D_BOOL		bPremulAlpha;			/* enable pre-multiplication stage */
	PVR2D_BOOL		bTransAlpha;			/* enable transparent source alpha stage */
	PVR2D_BOOL		bUpdateAlphaLookup;		/* enable and update the 1555-Lookup alpha table */
	unsigned char	uAlphaLookup0;			/* 8 bit alpha when A=0 in a 1555-Lookup surface */
	unsigned char	uAlphaLookup1;			/* 8 bit alpha when A=1 in a 1555-Lookup surface */
	unsigned char	uGlobalRGB;				/* Global Alpha Value for RGB, 0=transparent 255=opaque */
	unsigned char	uGlobalA;				/* Global Alpha Value for Alpha */

} PVR2D_ALPHABLT, *PPVR2D_ALPHABLT;


/* surface memory info structure */
typedef struct _PVR2DMEMINFO
{
	void				*pBase;
	unsigned long		ui32MemSize;
	unsigned long		ui32DevAddr;
	unsigned long		ulFlags;
	void				*hPrivateData;
	void				*hPrivateMapData;

}PVR2DMEMINFO, *PPVR2DMEMINFO;


#define PVR2D_MAX_DEVICE_NAME 20

typedef struct _PVR2DDEVICEINFO
{
	unsigned long	ulDevID;
	char			szDeviceName[PVR2D_MAX_DEVICE_NAME];
}PVR2DDEVICEINFO;


typedef struct _PVR2DISPLAYINFO
{
	unsigned long	ulMaxFlipChains;
	unsigned long	ulMaxBuffersInChain;
	PVR2DFORMAT		eFormat;
	unsigned long	ulWidth;
	unsigned long	ulHeight;
	long			lStride;
	unsigned long	ulMinFlipInterval;
	unsigned long	ulMaxFlipInterval;

}PVR2DDISPLAYINFO;


typedef struct _PVR2DBLTINFO
{
	unsigned long	CopyCode;			/* rop code  */
	unsigned long	Colour;				/* fill colour */
	unsigned long	ColourKey;			/* colour key */
	unsigned char	GlobalAlphaValue;	/* global alpha blending */
	unsigned char	AlphaBlendingFunc;	/* per-pixel alpha-blending function */

	PVR2DBLITFLAGS	BlitFlags;			/* additional blit control information */

	PVR2DMEMINFO	*pDstMemInfo;		/* destination memory */
	unsigned long	DstOffset;			/* byte offset from start of allocation to destination surface pixel 0,0 */
	long			DstStride;			/* signed stride, the number of bytes from pixel 0,0 to 0,1 */
	long			DstX, DstY;			/* pixel offset from start of dest surface to start of blt rectangle */
	long			DSizeX,DSizeY;		/* blt size */
	PVR2DFORMAT		DstFormat;			/* dest format */
	unsigned long	DstSurfWidth;		/* size of dest surface in pixels */
	unsigned long	DstSurfHeight;		/* size of dest surface in pixels */

	PVR2DMEMINFO	*pSrcMemInfo;		/* source mem, (source fields are also used for patterns) */
	unsigned long	SrcOffset;			/* byte offset from start of allocation to src/pat surface pixel 0,0 */
	long			SrcStride;			/* signed stride, the number of bytes from pixel 0,0 to 0,1 */
	long			SrcX, SrcY;			/* pixel offset from start of surface to start of source rectangle */
										/* for patterns this is the start offset within the pattern */
	long			SizeX,SizeY;		/* source rectangle size or pattern size in pixels */
	PVR2DFORMAT		SrcFormat;			/* source/pattern format */
	PVR2DMEMINFO	*pPalMemInfo;		/* source/pattern palette memory containing argb8888 colour table */
	unsigned long	PalOffset;			/* byte offset from start of allocation to start of palette */
	unsigned long	SrcSurfWidth;		/* size of source surface in pixels */
	unsigned long	SrcSurfHeight;		/* size of source surface in pixels */

	PVR2DMEMINFO	*pMaskMemInfo;		/* mask memory, 1bpp format implied */
	unsigned long	MaskOffset;			/* byte offset from start of allocation to mask surface pixel 0,0 */
	long			MaskStride;			/* signed stride, the number of bytes from pixel 0,0 to 0,1 */
	long			MaskX, MaskY;		/* mask rect top left (mask size = blt size) */
	unsigned long	MaskSurfWidth;		/* size of mask surface in pixels */
	unsigned long	MaskSurfHeight;		/* size of mask surface in pixels */
	
	PPVR2D_ALPHABLT pAlpha;				/* fully specified alpha blend */

}PVR2DBLTINFO, *PPVR2DBLTINFO;

#define PVR2D_MAXSRCSURFACES 3

typedef struct _PVR2D_SRCSURFACEINFO {
	PVR2DMEMINFO	*pSrcMemInfo;		/* source mem */
	unsigned long	SrcOffset;		/* byte offset from start of allocation to srcsurface pixel 0,0 */
	long		SrcStride;		/* signed stride, the number of bytes from pixel 0,0 to 0,1 */
	PVR2DFORMAT	SrcFormat;		/* source format */
	unsigned long	SrcSurfWidth;		/* size of source surface in pixels */
	unsigned long	SrcSurfHeight;		/* size of source surface in pixels */
	PVR2D_FILTER	SrcFilterMode;		/* used for compositing */
	PVR2D_REPEAT	SrcRepeatMode;		/* used for compositing */
	unsigned long	SrcTexCoord;		/* which tex coord to use */

} PVR2DSRCSRFINFO;


typedef struct _PVR2DEXTBLTINFO
{

	PVR2DBLITFLAGS	BlitFlags;		/* additional blit control information */

	PVR2DMEMINFO	*pDstMemInfo;		/* destination memory */
	unsigned long	DstOffset;		/* byte offset from start of allocation to destination surface pixel 0,0 */
	long		DstStride;		/* signed stride, the number of bytes from pixel 0,0 to 0,1 */
	long		DstX, DstY;		/* pixel offset from start of dest surface to start of blt rectangle */
	long		DSizeX, DSizeY;		/* blt size */
	PVR2DFORMAT	DstFormat;		/* dest format */
	unsigned long	DstSurfWidth;		/* size of dest surface in pixels */
	unsigned long	DstSurfHeight;		/* size of dest surface in pixels */

	PVR2DSRCSRFINFO	SrcSurface[PVR2D_MAXSRCSURFACES];

}PVR2DEXTBLTINFO, *PPVR2DEXTBLTINFO;

typedef struct _PVR2DRECT
{
	long left, top;
	long right, bottom;
} PVR2DRECT;

typedef struct
{
	PVR2DMEMINFO	*pSurfMemInfo;		/* surface memory */
	unsigned long	SurfOffset;			/* byte offset from start of allocation to destination surface pixel 0,0 */
	long			Stride;				/* signed stride */
	PVR2DFORMAT		Format;
	unsigned long	SurfWidth;			/* surface size in pixels */
	unsigned long	SurfHeight;

} PVR2D_SURFACE, *PPVR2D_SURFACE;

typedef struct
{
	unsigned long	*pUseCode;					/* USSE code */
	unsigned long	UseCodeSize;				/* usse code size in bytes */

} PVR2D_USECODE, *PPVR2D_USECODE;

typedef struct
{
	PVR2D_SURFACE			sDst;				/* destination surface */
	PVR2D_SURFACE			sSrc;				/* source surface */
	PVR2DRECT				rcDest;				/* destination rectangle */
	PVR2DRECT				rcSource;			/* source rectangle */
	PVR2D_HANDLE			hUseCode;			/* custom USE code (NULL implies source copy) */
	unsigned long			UseParams[2];		/* per-blt params for use code */

} PVR2D_3DBLT, *PPVR2D_3DBLT;


#define MAKE_COPY_BLIT(src,soff,dest,doff,sx,sy,dx,dy,sz)

typedef void* PVR2DCONTEXTHANDLE;
typedef void* PVR2DFLIPCHAINHANDLE;


// CopyCode field of PVR2DBLTINFO structure:
// the CopyCode field of the PVR2DBLTINFO structure should contain a rop3 or rop4 code.
// a rop3 is an 8 bit code that describes a blt with three inputs : source dest and pattern
// rop4 is a 16 bit code that describes a blt with four inputs : source dest pattern and mask
// common rop3 codes are defined below
// a colour fill blt is processed in the pattern channel as a constant colour with a rop code of 0xF0
// PVR2D_BLIT_PAT_SURFACE_ENABLE defines whether the pattern channel is a surface or a fill colour.
// a rop4 is defined by two rop3 codes, and the 1 bit-per-pixel mask surface defines which is used.
// a common rop4 is 0xAAF0 which is the mask copy blt used for text glyphs.
// CopyCode is taken to be a rop4 when pMaskMemInfo is non zero, otherwise it is assumed to be a rop3
// use the PVR2DMASKROP4 macro below to construct a rop4 from two rop3's
// rop3a is the rop used when mask pixel = 1, and rop3b when mask = 0
#define PVR2DROP4(rop3b, rop3a)			((rop3b<<8)|rop3a)

/* common rop codes */
#define PVR2DROPclear				0x00       /* 0 (whiteness) */
#define PVR2DROPset					0xFF       /* 1 (blackness) */
#define PVR2DROPnoop				0xAA       /* dst (used for masked blts) */

/* source and  dest rop codes */
#define PVR2DROPand					0x88       /* src AND dst */
#define PVR2DROPandReverse			0x44       /* src AND NOT dst */
#define PVR2DROPcopy				0xCC       /* src (used for source copy and alpha blts) */
#define PVR2DROPandInverted			0x22       /* NOT src AND dst */
#define PVR2DROPxor					0x66       /* src XOR dst */
#define PVR2DROPor					0xEE       /* src OR dst */
#define PVR2DROPnor					0x11       /* NOT src AND NOT dst */
#define PVR2DROPequiv				0x99       /* NOT src XOR dst */
#define PVR2DROPinvert				0x55       /* NOT dst */
#define PVR2DROPorReverse			0xDD       /* src OR NOT dst */
#define PVR2DROPcopyInverted		0x33       /* NOT src */
#define PVR2DROPorInverted			0xBB       /* NOT src OR dst */
#define PVR2DROPnand				0x77       /* NOT src OR NOT dst */

/* pattern rop codes */
#define PVR2DPATROPand				0xA0       /* pat AND dst */
#define PVR2DPATROPandReverse		0x50       /* pat AND NOT dst */
#define PVR2DPATROPcopy				0xF0       /* pat (used for solid color fills and pattern blts) */
#define PVR2DPATROPandInverted		0x0A       /* NOT pat AND dst */
#define PVR2DPATROPxor				0x5A       /* pat XOR dst */
#define PVR2DPATROPor				0xFA       /* pat OR dst */
#define PVR2DPATROPnor				0x05       /* NOT pat AND NOT dst */
#define PVR2DPATROPequiv			0xA5       /* NOT pat XOR dst */
#define PVR2DPATROPinvert			0x55       /* NOT dst */
#define PVR2DPATROPorReverse		0xF5       /* pat OR NOT dst */
#define PVR2DPATROPcopyInverted		0x0F       /* NOT pat */
#define PVR2DPATROPorInverted		0xAF       /* NOT pat OR dst */
#define PVR2DPATROPnand				0x5F       /* NOT pat OR NOT dst */

/* common rop4 codes */
#define PVR2DROP4MaskedCopy              PVR2DROP4(PVR2DROPnoop,PVR2DROPcopy)		/* masked source copy blt (used for rounded window corners etc) */
#define PVR2DROP4MaskedFill              PVR2DROP4(PVR2DROPnoop,PVR2DPATROPcopy)	/* masked colour fill blt (used for text) */

/* Legacy support */
#define PVR2DROP3_PATMASK			PVR2DPATROPcopy
#define PVR2DROP3_SRCMASK			PVR2DROPcopy

/* pixmap memory alignment */
#define PVR2D_ALIGNMENT_4			4			/* DWORD alignment */
#define PVR2D_ALIGNMENT_ANY			0			/* no alignment    */
#define PVR2D_ALIGNMENT_PALETTE		16			/* 16 byte alignment is required for palettes */

/* Heap number for PVR2DGetFrameBuffer */
#define PVR2D_FB_PRIMARY_SURFACE 0

#define PVR2D_PRESENT_PROPERTY_SRCSTRIDE	(1 << 0)
#define PVR2D_PRESENT_PROPERTY_DSTSIZE		(1 << 1)
#define PVR2D_PRESENT_PROPERTY_DSTPOS		(1 << 2)
#define PVR2D_PRESENT_PROPERTY_CLIPRECTS	(1 << 3)
#define PVR2D_PRESENT_PROPERTY_INTERVAL		(1 << 4)


#define PVR2D_CREATE_FLIPCHAIN_SHARED		(1 << 0)
#define PVR2D_CREATE_FLIPCHAIN_QUERY		(1 << 1)

/* Functions that the library exports */

PVR2D_IMPORT
int PVR2DEnumerateDevices(PVR2DDEVICEINFO *pDevInfo);

PVR2D_IMPORT
PVR2DERROR PVR2DCreateDeviceContext(unsigned long ulDevID,
									PVR2DCONTEXTHANDLE* phContext,
									unsigned long ulFlags);

PVR2D_IMPORT
PVR2DERROR PVR2DDestroyDeviceContext(PVR2DCONTEXTHANDLE hContext);

PVR2D_IMPORT
PVR2DERROR PVR2DGetDeviceInfo(PVR2DCONTEXTHANDLE hContext,
							  PVR2DDISPLAYINFO *pDisplayInfo);

PVR2D_IMPORT
PVR2DERROR PVR2DGetScreenMode(PVR2DCONTEXTHANDLE hContext,
							  PVR2DFORMAT *pFormat,
							  long *plWidth,
							  long *plHeight,
							  long *plStride,
							  int *piRefreshRate);

PVR2D_IMPORT
PVR2DERROR PVR2DGetFrameBuffer(PVR2DCONTEXTHANDLE hContext,
							   int nHeap,
							   PVR2DMEMINFO **ppsMemInfo);

PVR2D_IMPORT
PVR2DERROR PVR2DFreeFrameBuffer(PVR2DCONTEXTHANDLE hContext,
	int nHeap);

PVR2D_IMPORT
PVR2DERROR PVR2DMemAlloc(PVR2DCONTEXTHANDLE hContext,
						 unsigned long ulBytes,
						 unsigned long ulAlign,
						 unsigned long ulFlags,
						 PVR2DMEMINFO **ppsMemInfo);

PVR2D_IMPORT
PVR2DERROR PVR2DMemWrap(PVR2DCONTEXTHANDLE hContext,
						void *pMem,
						unsigned long ulFlags,
						unsigned long ulBytes,
						unsigned long alPageAddress[],
						PVR2DMEMINFO **ppsMemInfo);

PVR2D_IMPORT
PVR2DERROR PVR2DMemMap(PVR2DCONTEXTHANDLE hContext,
						unsigned long ulFlags,
						void *hPrivateMapData,
						PVR2DMEMINFO **ppsDstMem);

PVR2D_IMPORT
PVR2DERROR PVR2DMemFree(PVR2DCONTEXTHANDLE hContext,
						PVR2DMEMINFO *psMemInfo);

PVR2D_IMPORT
PVR2DERROR PVR2DBlt(PVR2DCONTEXTHANDLE hContext,
					PVR2DBLTINFO *pBltInfo);

PVR2D_IMPORT
PVR2DERROR PVR2DBltClipped(PVR2DCONTEXTHANDLE hContext,
						   PVR2DBLTINFO *pBltInfo,
						   unsigned long ulNumClipRects,
						   PVR2DRECT *pClipRects);

PVR2D_IMPORT
PVR2DERROR PVR2DQueryBlitsComplete(PVR2DCONTEXTHANDLE hContext,
								   PVR2DMEMINFO *pMemInfo,
								   unsigned int uiWaitForComplete);

PVR2D_IMPORT
PVR2DERROR PVR2DSetPresentBltProperties(PVR2DCONTEXTHANDLE hContext,
										unsigned long ulPropertyMask,
										long lSrcStride,
										unsigned long ulDstWidth,
										unsigned long ulDstHeight,
										long lDstXPos,
										long lDstYPos,
										unsigned long ulNumClipRects,
										PVR2DRECT *pClipRects,
										unsigned long ulSwapInterval);

PVR2D_IMPORT
PVR2DERROR PVR2DPresentBlt(PVR2DCONTEXTHANDLE hContext,
						   PVR2DMEMINFO *pMemInfo,
						   long lRenderID);

PVR2D_IMPORT
PVR2DERROR PVR2DCreateFlipChain(PVR2DCONTEXTHANDLE hContext,
								unsigned long ulFlags,
								unsigned long ulNumBuffers,
								unsigned long ulWidth,
								unsigned long ulHeight,
								PVR2DFORMAT eFormat,
								long *plStride,
								unsigned long *pulFlipChainID,
								PVR2DFLIPCHAINHANDLE *phFlipChain);

PVR2D_IMPORT
PVR2DERROR PVR2DDestroyFlipChain(PVR2DCONTEXTHANDLE hContext,
								 PVR2DFLIPCHAINHANDLE hFlipChain);

PVR2D_IMPORT
PVR2DERROR PVR2DGetFlipChainBuffers(PVR2DCONTEXTHANDLE hContext,
									PVR2DFLIPCHAINHANDLE hFlipChain,
									unsigned long *pulNumBuffers,
									PVR2DMEMINFO *psMemInfo[]);

PVR2D_IMPORT
PVR2DERROR PVR2DSetPresentFlipProperties(PVR2DCONTEXTHANDLE hContext,
										 PVR2DFLIPCHAINHANDLE hFlipChain,
										 unsigned long ulPropertyMask,
										 long lDstXPos,
										 long lDstYPos,
										 unsigned long ulNumClipRects,
										 PVR2DRECT *pClipRects,
										 unsigned long ulSwapInterval);

PVR2D_IMPORT
PVR2DERROR PVR2DPresentFlip(PVR2DCONTEXTHANDLE hContext,
							PVR2DFLIPCHAINHANDLE hFlipChain,
							PVR2DMEMINFO *psMemInfo,
							long lRenderID);

PVR2D_IMPORT
PVR2DERROR PVR2DGetAPIRev(long *lRevMajor, long *lRevMinor);

PVR2D_IMPORT
PVR2DERROR PVR2DLoadUseCode (const PVR2DCONTEXTHANDLE hContext, const unsigned char	*pUseCode,
									const unsigned long UseCodeSize, PVR2D_HANDLE *pUseCodeHandle);
PVR2D_IMPORT
PVR2DERROR PVR2DFreeUseCode (const PVR2DCONTEXTHANDLE hContext, const PVR2D_HANDLE hUseCodeHandle);

PVR2D_IMPORT
PVR2DERROR PVR2DBlt3D (const PVR2DCONTEXTHANDLE hContext, const PPVR2D_3DBLT pBlt3D);

#if defined PVR2D_EXT_BLIT
PVR2D_IMPORT
PVR2DERROR PVR2DVideoBlt(PVR2DCONTEXTHANDLE hContext, PVR2DEXTBLTINFO *pExtBltInfo,
	float *texcoord_data, unsigned long *csc_data); 
#endif

PVR2D_IMPORT
PVR2DERROR PVR2DCacheFlushDRI(PVR2DCONTEXTHANDLE hContext, int lType, long lVirt, long lLength); 

#ifdef __cplusplus
}
#endif 

#endif /* _PVR2D_H_ */

/******************************************************************************
 End of file (pvr2d.h)
******************************************************************************/
