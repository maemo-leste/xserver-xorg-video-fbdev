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

#include <errno.h>
#include <error.h>
#include <string.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/time.h>

/* all driver need this */
#include <X11/Xatom.h>
#include "xf86.h"
#include "xf86_OSproc.h"

#include "mipointer.h"
#include "mibstore.h"
#include "micmap.h"
#include "colormapst.h"
#include "xf86cmap.h"
#include "shadow.h"
#include "xacestr.h"
#include "xf86Xinput.h"

/* for visuals */
#include "fb.h"

#include "xf86Resources.h"
#include "xf86RAC.h"

#include "fbdevhw.h"

#include "xf86Crtc.h"
#include "xf86xv.h"

#include "fbdev.h"
#include <linux/fb.h>
#include <linux/omapfb.h>

#include "sgx_pvr2d.h"
#include "sgx_xv.h"
#include "omap_video.h"
#include "omap_tvout.h"

/* -------------------------------------------------------------------- */
/* prototypes                                                           */

static const OptionInfoRec *FBDevAvailableOptions(int chipid, int busid);
static void FBDevIdentify(int flags);
static Bool FBDevProbe(DriverPtr drv, int flags);
static Bool FBDevPreInit(ScrnInfoPtr pScrn, int flags);
static Bool FBDevScreenInit(int Index, ScreenPtr pScreen, int argc,
			    char **argv);
static Bool FBDevEnterVT(int scrnIndex, int flags);
static void FBDevLeaveVT(int scrnIndex, int flags);
static Bool FBDevCloseScreen(int scrnIndex, ScreenPtr pScreen);
static int fbdev_randr12_preinit(ScrnInfoPtr pScrn);
static void fbdev_randr12_uninit(ScrnInfoPtr pScrn);
static void fbdev_enable_fb_access(int scrn_index, Bool enable);
static void fbdev_property_callback(CallbackListPtr * pcbl, pointer unused,
				    pointer calldata);
static int fbdev_crtc_rotate(xf86CrtcPtr crtc, int rotation);

static int fbdev_crtc_rotate(xf86CrtcPtr crtc, int rotation);

/* -------------------------------------------------------------------- */

_X_EXPORT DriverRec FBDEV = {
	FBDEV_VERSION,
	FBDEV_DRIVER_NAME,
	FBDevIdentify,
	FBDevProbe,
	FBDevAvailableOptions,
	NULL,
	0,
	NULL,
};

/* Supported "chipsets" */
static SymTabRec FBDevChipsets[] = {
	{0, "fbdev"},
	{-1, NULL}
};

/* Supported options */
typedef enum {
	OPTION_FBDEV,
} FBDevOpts;

static const OptionInfoRec FBDevOptions[] = {
	{OPTION_FBDEV, "fbdev", OPTV_STRING, {0}, FALSE},
	{-1, NULL, OPTV_NONE, {0}, FALSE}
};

/* -------------------------------------------------------------------- */

static const char *fbSymbols[] = {
	"fbScreenInit",
	"fbPictureInit",
	NULL
};

static const char *fbdevHWSymbols[] = {
	"fbdevHWInit",
	"fbdevHWProbe",
	"fbdevHWSetVideoModes",
	"fbdevHWUseBuildinMode",

	"fbdevHWGetDepth",
	"fbdevHWGetLineLength",
	"fbdevHWGetName",
	"fbdevHWGetType",
	"fbdevHWGetVidmem",
	"fbdevHWLinearOffset",
	"fbdevHWLoadPalette",
	"fbdevHWMapVidmem",
	"fbdevHWUnmapVidmem",

	/* colormap */
	"fbdevHWLoadPalette",
	"fbdevHWLoadPaletteWeak",

	/* ScrnInfo hooks */
	"fbdevHWAdjustFrameWeak",
	"fbdevHWModeInit",
	"fbdevHWSwitchModeWeak",
	"fbdevHWValidModeWeak",

	"fbdevHWGetFD",

	NULL
};

#ifdef XFree86LOADER

MODULESETUPPROTO(FBDevSetup);

static XF86ModuleVersionInfo FBDevVersRec = {
	"fbdev",
	MODULEVENDORSTRING,
	MODINFOSTRING1,
	MODINFOSTRING2,
	XORG_VERSION_CURRENT,
	PACKAGE_VERSION_MAJOR,
	PACKAGE_VERSION_MINOR,
	PACKAGE_VERSION_PATCHLEVEL,
	ABI_CLASS_VIDEODRV,
	ABI_VIDEODRV_VERSION,
	NULL,
	{0, 0, 0, 0}
};

_X_EXPORT XF86ModuleData fbdevModuleData = { &FBDevVersRec, FBDevSetup, NULL };

pointer FBDevSetup(pointer module, pointer opts, int *errmaj, int *errmin)
{
	static Bool setupDone = FALSE;

	if (!setupDone) {
		setupDone = TRUE;
		xf86AddDriver(&FBDEV, module, 0);
		LoaderRefSymLists(fbSymbols, fbdevHWSymbols, NULL);
		return (pointer) 1;
	} else {
		if (errmaj)
			*errmaj = LDR_ONCEONLY;
		return NULL;
	}
}

#endif /* XFree86LOADER */

static Bool FBDevGetRec(ScrnInfoPtr pScrn)
{
	if (pScrn->driverPrivate != NULL)
		return TRUE;

	pScrn->driverPrivate = xnfcalloc(sizeof(FBDevRec), 1);
	return TRUE;
}

static void FBDevFreeRec(ScrnInfoPtr pScrn)
{
	if (pScrn->driverPrivate == NULL)
		return;
	xfree(pScrn->driverPrivate);
	pScrn->driverPrivate = NULL;
}

/* -------------------------------------------------------------------- */

static const OptionInfoRec *FBDevAvailableOptions(int chipid, int busid)
{
	return FBDevOptions;
}

static void FBDevIdentify(int flags)
{
	xf86PrintChipsets(FBDEV_NAME, "driver for framebuffer", FBDevChipsets);
}

static Bool FBDevProbe(DriverPtr drv, int flags)
{
	int i;
	ScrnInfoPtr pScrn;
	GDevPtr *devSections;
	int numDevSections;
	char *dev;
	int entity;
	Bool foundScreen = FALSE;

	ENTER();

	/* For now, just bail out for PROBE_DETECT. */
	if (flags & PROBE_DETECT)
		return FALSE;

	numDevSections = xf86MatchDevice(FBDEV_DRIVER_NAME, &devSections);
	if (numDevSections <= 0)
		return FALSE;

	if (!xf86LoadDrvSubModule(drv, "fbdevhw"))
		return FALSE;

	xf86LoaderReqSymLists(fbdevHWSymbols, NULL);

	for (i = 0; i < numDevSections; i++) {
		dev = xf86FindOptionValue(devSections[i]->options, "fbdev");
		if (fbdevHWProbe(NULL, dev, NULL)) {
			entity = xf86ClaimFbSlot(drv, 0, devSections[i], TRUE);
			pScrn =
			    xf86ConfigFbEntity(NULL, 0, entity, NULL, NULL,
					       NULL, NULL);
			if (!pScrn)
				continue;

			foundScreen = TRUE;

			pScrn->driverVersion = FBDEV_VERSION;
			pScrn->driverName = FBDEV_DRIVER_NAME;
			pScrn->name = FBDEV_NAME;
			pScrn->Probe = FBDevProbe;
			pScrn->PreInit = FBDevPreInit;
			pScrn->ScreenInit = FBDevScreenInit;
			pScrn->SwitchMode = fbdevHWSwitchModeWeak();
			pScrn->AdjustFrame = fbdevHWAdjustFrameWeak();
			pScrn->EnterVT = FBDevEnterVT;
			pScrn->LeaveVT = FBDevLeaveVT;
			pScrn->ValidMode = fbdevHWValidModeWeak();

			xf86DrvMsg(pScrn->scrnIndex, X_INFO, "using %s\n",
				   dev ? dev : "default device");
		}
	}
	xfree(devSections);
	LEAVE();
	return foundScreen;
}

static Bool FBDevPreInit(ScrnInfoPtr pScrn, int flags)
{
	FBDevPtr fPtr;
	int default_depth, fbbpp;
	rgb rgb_zeros = { 0, 0, 0 };
	Gamma gamma_zeros = { 0.0, 0.0, 0.0 };
	char *path;

	if (flags & PROBE_DETECT)
		return FALSE;

	ENTER();

	if (pScrn->numEntities != 1)
		return FALSE;

	pScrn->monitor = pScrn->confScreen->monitor;

	FBDevGetRec(pScrn);
	fPtr = FBDEVPTR(pScrn);

	fPtr->pEnt = xf86GetEntityInfo(pScrn->entityList[0]);

	path = xf86FindOptionValue(fPtr->pEnt->device->options, "fbdev");
	if (!path)
		path = "/dev/fb0";

	/* open device */
	if (!fbdevHWInit(pScrn, NULL, path))
		return FALSE;
	fPtr->fd = fbdevHWGetFD(pScrn);
	default_depth = fbdevHWGetDepth(pScrn, &fbbpp);
	if (!xf86SetDepthBpp
	    (pScrn, default_depth, default_depth, fbbpp, Support32bppFb))
		return FALSE;
	xf86PrintDepthBpp(pScrn);

	if (!xf86SetWeight(pScrn, rgb_zeros, rgb_zeros))
		return FALSE;
	if (!xf86SetGamma(pScrn, gamma_zeros))
		return FALSE;

	/* visual init */
	if (!xf86SetDefaultVisual(pScrn, -1))
		return FALSE;

	if (pScrn->defaultVisual != TrueColor) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "requested default visual (%s) is not supported\n",
			   xf86GetVisualName(pScrn->defaultVisual));
		return FALSE;
	}

	if (pScrn->bitsPerPixel != 16 && pScrn->bitsPerPixel != 24
	    && pScrn->bitsPerPixel != 32) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "requested depth %d is not supported\n",
			   pScrn->depth);
		return FALSE;
	}

	if (fbdevHWGetType(pScrn) != FBDEVHW_PACKED_PIXELS) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "internal error: unsupported fbdevhw hardware type %d\n",
			   fbdevHWGetType(pScrn));
		return FALSE;
	}

	pScrn->progClock = TRUE;
	pScrn->rgbBits = 8;
	pScrn->chipset = "fbdev";
	pScrn->videoRam = fbdevHWGetVidmem(pScrn);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "hardware: %s (video memory:" " %dkB)\n",
		   fbdevHWGetName(pScrn), pScrn->videoRam / 1024);

	/* handle options */
	xf86CollectOptions(pScrn, NULL);
	fPtr->Options = xalloc(sizeof(FBDevOptions));
	if (!fPtr->Options)
		return FALSE;
	memcpy(fPtr->Options, FBDevOptions, sizeof(FBDevOptions));
	xf86ProcessOptions(pScrn->scrnIndex, fPtr->pEnt->device->options,
			   fPtr->Options);

	if (!fbdev_randr12_preinit(pScrn)) {
		FBDevFreeRec(pScrn);
		return FALSE;
	}

	if (!xf86LoadSubModule(pScrn, "fb")) {
		FBDevFreeRec(pScrn);
		return FALSE;
	}
	xf86LoaderReqSymLists(fbSymbols, NULL);

	LEAVE();

	return TRUE;
}

Bool
fbdevSaveScreen(ScreenPtr pScreen, int mode)
{
	return TRUE;
}

static Bool FBDevScreenInit(int scrnIndex, ScreenPtr pScreen, int argc,
			    char **argv)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	FBDevPtr fPtr = FBDEVPTR(pScrn);
	VisualPtr visual;

	ENTER();

#if DEBUG
	ErrorF("\tbitsPerPixel=%d, depth=%d, defaultVisual=%s\n"
	       "\tmask: %x,%x,%x, offset: %d,%d,%d\n", pScrn->bitsPerPixel,
	       pScrn->depth, xf86GetVisualName(pScrn->defaultVisual),
	       pScrn->mask.red, pScrn->mask.green, pScrn->mask.blue,
	       pScrn->offset.red, pScrn->offset.green, pScrn->offset.blue);
#endif

	fPtr->fbmem = fbdevHWMapVidmem(pScrn);
	if (!fPtr->fbmem) {
		xf86DrvMsg(scrnIndex, X_ERROR, "video memory map failed\n");
		return FALSE;
	}
	fPtr->fboff = fbdevHWLinearOffset(pScrn);

	fbdevHWSave(pScrn);

	if (!fbdevHWModeInit(pScrn, pScrn->currentMode)) {
		xf86DrvMsg(scrnIndex, X_ERROR, "mode initialization failed\n");
		return FALSE;
	}
	fbdevHWAdjustFrame(scrnIndex, 0, 0, 0);

	/* mi layer */
	miClearVisualTypes();
	if (!miSetVisualTypes
	    (pScrn->depth, TrueColorMask, pScrn->rgbBits, TrueColor)) {
		xf86DrvMsg(scrnIndex, X_ERROR,
			   "visual type setup failed"
			   " for %d bits per pixel [1]\n", pScrn->bitsPerPixel);
		return FALSE;
	}
	if (!miSetPixmapDepths()) {
		xf86DrvMsg(scrnIndex, X_ERROR, "pixmap depth setup failed\n");
		return FALSE;
	}

	/* FIXME: this doesn't work for all cases, e.g. when each
	   scanline has a padding which is independent from the
	   depth (controlfb) */
	pScrn->displayWidth =
	    fbdevHWGetLineLength(pScrn) / (pScrn->bitsPerPixel / 8);

	/* Set display resolution */
	xf86SetDpi(pScrn, 0, 0);

	if (pScrn->displayWidth != pScrn->virtualX) {
		xf86DrvMsg(scrnIndex, X_INFO,
			   "Pitch updated to %d after ModeInit\n",
			   pScrn->displayWidth);
	}

	/* unused */
	fPtr->fbstart = fPtr->fbmem + fPtr->fboff;

	if (!fbScreenInit
	    (pScreen, (void *)~0UL, pScrn->virtualX, pScrn->virtualY,
	     pScrn->xDpi, pScrn->yDpi, pScrn->displayWidth,
	     pScrn->bitsPerPixel)) {
		xf86DrvMsg(scrnIndex, X_ERROR, "fbScreenInit failed\n");
		return FALSE;
	}

	/* Fixup RGB ordering */
	visual = pScreen->visuals + pScreen->numVisuals;
	while (--visual >= pScreen->visuals) {
		if ((visual->class | DynamicClass) == DirectColor) {
			visual->offsetRed = pScrn->offset.red;
			visual->offsetGreen = pScrn->offset.green;
			visual->offsetBlue = pScrn->offset.blue;
			visual->redMask = pScrn->mask.red;
			visual->greenMask = pScrn->mask.green;
			visual->blueMask = pScrn->mask.blue;
		}
	}

	/* must be after RGB ordering fixed */
	if (!fbPictureInit(pScreen, NULL, 0)) {
		xf86DrvMsg(scrnIndex, X_ERROR,
			   "Render extension initialisation failed\n");
		return FALSE;
	}

	EXA_Init(pScreen);

	fbdev_init_video(pScreen);

	xf86SetBlackWhitePixels(pScreen);
	miInitializeBackingStore(pScreen);
	xf86SetBackingStore(pScreen);

	pScrn->vtSema = TRUE;

	/* software cursor */
	miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

	if (!xf86SetDesiredModes(pScrn))
		return FALSE;

	if (!xf86CrtcScreenInit(pScreen))
		return FALSE;

	if (!miCreateDefColormap(pScreen)) {
		xf86DrvMsg(scrnIndex, X_ERROR,
			   "internal error: failed to create colormap\n");
		return FALSE;
	}
	if (!xf86HandleColormaps
	    (pScreen, 256, 8, fbdevHWLoadPaletteWeak(), NULL,
	     CMAP_PALETTED_TRUECOLOR))
		return FALSE;

	/* Setup the screen saver */
	pScreen->SaveScreen = fbdevSaveScreen;

	/* Wrap the current CloseScreen function */
	fPtr->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = FBDevCloseScreen;

	/* Set up our own Enable/Disable FB access handler. */
	fPtr->enable_fb_access = pScrn->EnableDisableFBAccess;
	fPtr->suppress_reconfig = 0;
	pScrn->EnableDisableFBAccess = fbdev_enable_fb_access;
	fPtr->suppress_reconfig_prop = MakeAtom(SUPPRESS_RECONFIG_PROP_NAME,
						sizeof(SUPPRESS_RECONFIG_PROP_NAME) - 1, TRUE);
	XaceRegisterCallback(XACE_PROPERTY_ACCESS, fbdev_property_callback, NULL);

	LEAVE();

	return TRUE;
}

static Bool FBDevEnterVT(int scrnIndex, int flags)
{
	ScrnInfoPtr pScrn = xf86Screens[scrnIndex];

	return xf86SetDesiredModes(pScrn);
}

static void FBDevLeaveVT(int scrnIndex, int flags)
{
}

static Bool FBDevCloseScreen(int scrnIndex, ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
	FBDevPtr fPtr = FBDEVPTR(pScrn);

	/* Rotate back to landscape to prevent Nokia logo from being messed up
	 * if shutdown from portrait mode. Do this before EXA_Fini on purpose,
	 * because pixmap hash table is released there but used on rotation. */
	fbdev_crtc_rotate (fPtr->crtc_lcd, RR_Rotate_0);
	//fbdev_randr12_uninit (pScrn);

	EXA_Fini(pScreen);

	fbdev_fini_video (pScreen);

	fbdevHWRestore(pScrn);
	fbdevHWUnmapVidmem(pScrn);
	pScrn->vtSema = FALSE;

	pScreen->CloseScreen = fPtr->CloseScreen;
	pScrn->EnableDisableFBAccess = fPtr->enable_fb_access;
	return (*pScreen->CloseScreen) (scrnIndex, pScreen);
}

static void fbdev_output_create_resources(xf86OutputPtr output)
{
        Atom prop, val;
        int err;
        FBDevPtr fPtr = output->driver_private;

	if (output != fPtr->output_lcd)
		return;

        prop = MakeAtom(RR_PROPERTY_CONNECTOR_TYPE,
                        sizeof(RR_PROPERTY_CONNECTOR_TYPE) - 1, TRUE);
	val = MakeAtom("Panel", sizeof("Panel") - 1, TRUE);
        err = RRConfigureOutputProperty(output->randr_output, prop,
			                FALSE, FALSE, TRUE, 0, NULL);
        if (err == 0) {
                err = RRChangeOutputProperty(output->randr_output,
                                             prop, XA_ATOM, 32,
                                             PropModeReplace, 1, &val, FALSE,
                                             FALSE);
                if (err != 0)
                        xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
                                   "Failed to set ConnectorType\n");
        }
        else {
                xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
                           "Failed to initialise ConnectorType\n");
        }

        prop = MakeAtom(RR_PROPERTY_SIGNAL_FORMAT,
                        sizeof(RR_PROPERTY_SIGNAL_FORMAT) - 1, TRUE);
        val = MakeAtom("LVDS", sizeof("LVDS") - 1, TRUE);
        err = RRConfigureOutputProperty(output->randr_output, prop,
			                FALSE, FALSE, TRUE, 0, NULL);
        if (err == 0) {
                err = RRChangeOutputProperty(output->randr_output,
                                             prop, XA_ATOM, 32,
                                             PropModeReplace, 1, &val, FALSE,
                                             FALSE);
                if (err != 0)
                        xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
                                   "Failed to set ConnectorType\n");
        }
        else {
                xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
                           "Failed to initialise ConnectorType\n");
        }
}

static void fbdev_output_dpms(xf86OutputPtr output, int mode)
{
	return;
}

static void fbdev_output_save(xf86OutputPtr output)
{
	return;
}

static void fbdev_output_restore(xf86OutputPtr output)
{
	return;
}

static int fbdev_output_mode_valid(xf86OutputPtr output, DisplayModePtr mode)
{
	FBDevPtr fPtr = output->driver_private;

	/* The rest is fine, as we'll just fake it in the CRTC's mode_fixup. */
	if (mode->HDisplay != fPtr->builtin->HDisplay
	    || mode->VDisplay != fPtr->builtin->VDisplay)
		return MODE_ONE_SIZE;

	return MODE_OK;
}

static Bool fbdev_output_mode_fixup(xf86OutputPtr output, DisplayModePtr mode,
				    DisplayModePtr adjusted_mode)
{
	return TRUE;
}

static void fbdev_output_prepare(xf86OutputPtr output)
{
	return;
}

static void fbdev_output_mode_set(xf86OutputPtr output, DisplayModePtr mode,
				  DisplayModePtr adjusted_mode)
{
	return;
}

static void fbdev_output_commit(xf86OutputPtr output)
{
	return;
}

static xf86OutputStatus fbdev_output_detect(xf86OutputPtr output)
{
	return XF86OutputStatusConnected;
}

static DisplayModePtr fbdev_output_get_modes(xf86OutputPtr output)
{
	FBDevPtr fPtr = output->driver_private;

	return xf86DuplicateMode(fPtr->builtin);
}

static Bool fbdev_output_set_property(xf86OutputPtr output, Atom property,
				      RRPropertyValuePtr value)
{
	return FALSE;
}

static Bool fbdev_output_get_property(xf86OutputPtr output, Atom property)
{
	return FALSE;
}

static xf86CrtcPtr fbdev_output_crtc_get(xf86OutputPtr output)
{
	FBDevPtr fPtr = output->driver_private;

	return fPtr->crtc_lcd;
}

static void fbdev_output_destroy(xf86OutputPtr output)
{
	return;
}

static void fbdev_crtc_save(xf86CrtcPtr crtc)
{
	return;			/* XXX implement */
}

static void fbdev_crtc_restore(xf86CrtcPtr crtc)
{
	return;			/* XXX implement */
}

static Bool fbdev_crtc_lock(xf86CrtcPtr crtc)
{
	int fd = fbdevHWGetFD(crtc->scrn);

	return (ioctl(fd, OMAPFB_SYNC_GFX) == 0);
}

static void fbdev_crtc_unlock(xf86CrtcPtr crtc)
{
	return;
}

static void fbdev_crtc_dpms(xf86CrtcPtr crtc, int mode)
{
	return;			/* XXX implement */
}

static Bool fbdev_crtc_mode_fixup(xf86CrtcPtr crtc, DisplayModePtr mode,
				  DisplayModePtr adjusted_mode)
{
	FBDevPtr fPtr = crtc->driver_private;

	/* Anything's fine, as long as it's the right resolution; we fake the
	 * rest anyway, so just silently rewrite the modeline. */
	if (mode->HDisplay != fPtr->builtin->HDisplay
	    || mode->VDisplay != fPtr->builtin->VDisplay)
		return FALSE;

	if (fbdevHWValidMode(crtc->scrn->scrnIndex, fPtr->builtin, 0, 0) != MODE_OK) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
			   "Failed to set builtin mode!\n");
		return FALSE;
	}

	*adjusted_mode = *fPtr->builtin;

	return TRUE;
}

static void fbdev_crtc_prepare(xf86CrtcPtr crtc)
{
	int fd = fbdevHWGetFD(crtc->scrn);

	(void)ioctl(fd, OMAPFB_SYNC_GFX);
}

static void fbdev_crtc_mode_set(xf86CrtcPtr crtc, DisplayModePtr mode,
				DisplayModePtr adjusted_mode, int x, int y)
{
	/* Offsets are not currently supported. */
	if (x || y) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
			   "Offset mode requested but currently unsupported\n");
		return;
	}

	if (!fbdevHWSwitchMode(crtc->scrn->scrnIndex, adjusted_mode, FALSE))
		FatalError("Failed mode_set even though check succeeded\n");
}

static void fbdev_crtc_commit(xf86CrtcPtr crtc)
{
	return;
}

static Bool toggle_plane (int fd, int enabled)
{
	struct omapfb_plane_info plane_info;

	if (ioctl(fd, OMAPFB_QUERY_PLANE, &plane_info) != 0)
		return FALSE;
	plane_info.enabled = enabled;
	if (ioctl(fd, OMAPFB_SETUP_PLANE, &plane_info) != 0)
		return FALSE;

	return TRUE;
}

static int fbdev_crtc_rotate(xf86CrtcPtr crtc, int rotation)
{
	struct fb_var_screeninfo var;
	ScrnInfoPtr pScrn = crtc->scrn;
	int fd = fbdevHWGetFD(pScrn);
	FBDevPtr fPtr = FBDEVPTR(pScrn);
	struct timeval t1, t2;
	WindowPtr win = NULL;

	gettimeofday(&t1, NULL);

	omap_tvout_stop(fPtr);

	if (!toggle_plane(fd, 0)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "rotate: couldn't disable plane!\n");
		return FALSE;
	}

	/* Wait for overlay to disappear. */
	ioctl(fd, OMAPFB_WAITFORVSYNC);

	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "rotate: couldn't get var info!\n");
		return FALSE;
	}

	var.xres = var.xres_virtual = xf86ModeWidth(fPtr->builtin, rotation);
	var.yres = var.yres_virtual = xf86ModeHeight(fPtr->builtin, rotation);

	switch (rotation) {
	case RR_Rotate_0:
		var.rotate = FB_ROTATE_UR;
		break;
	case RR_Rotate_90:
		var.rotate = FB_ROTATE_CCW;
		break;
	case RR_Rotate_180:
		var.rotate = FB_ROTATE_UD;
		break;
	case RR_Rotate_270:
		var.rotate = FB_ROTATE_CW;
		break;
	}

	PVR2D_PreFBReset();

	fbdevHWUnmapVidmem(pScrn);

	if (ioctl(fd, FBIOPUT_VSCREENINFO, &var) != 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "rotate: couldn't set var info\n");
		return FALSE;
	}

	fbdevHWReload(pScrn);

	fPtr->fbmem = fbdevHWMapVidmem(pScrn);
	if (!fPtr->fbmem) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "video memory map failed\n");
		return FALSE;
	}
	fPtr->fboff = fbdevHWLinearOffset(pScrn);
	pScrn->videoRam = fbdevHWGetVidmem(pScrn);
	memset(fPtr->fbmem, 0, pScrn->videoRam);

	if (!PVR2D_PostFBReset()) {
		ErrorF("Failed to reset PVR2D context\n");
		return FALSE;
	}

	if (pScrn->pScreen)
		win = WindowTable[pScrn->pScreen->myNum];

#if 0
	if (win) {
		ErrorF
		    ("fbdev_crtc_rotate: reconfiguring root window to %d x %d\n",
		     var.xres, var.yres);
		XID vlist[2];
		vlist[0] = var.xres;
		vlist[1] = var.yres;
		ConfigureWindow(win, CWWidth | CWHeight, vlist, wClient(win));
		ErrorF
		    ("fbdev_crtc_rotate: root window reconfiguration finished\n");
	}
#endif

	if (!toggle_plane(fd, 1)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "rotate: couldn't enable plane!\n");
		return FALSE;
	}

	omap_tvout_resume(fPtr);

	gettimeofday(&t2, NULL);
	DebugF("DDX Rotation in %ld microseconds.\n",
	       ((t2.tv_sec - t1.tv_sec) * 1000000) + t2.tv_usec - t1.tv_usec);

	xf86InputRotationNotify(rotation);

	return TRUE;
}

#define get_fb_stride(width, bpp) \
    (((width * bpp + FB_MASK) >> FB_SHIFT) * sizeof(FbBits))

static void *fbdev_crtc_shadow_allocate(xf86CrtcPtr crtc, int width, int height)
{
	ScreenPtr screen = crtc->scrn->pScreen;
	PixmapPtr root = screen->GetScreenPixmap(screen);
	Bool ret;

	/* Make root pixmap use new backing storage, and rotation write
	 * directly to the framebuffer. */
	ret = screen->ModifyPixmapHeader(root, 0, 0, 0, 0, 0, NULL);
	if (!ret) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
			   "failed to modify root pixmap!\n");
		return NULL;
	}

	return (void *)~0UL;
}

static PixmapPtr fbdev_crtc_shadow_create(xf86CrtcPtr crtc, void *data,
					  int width, int height)
{
	ScreenPtr screen = crtc->scrn->pScreen;
	PixmapPtr pixmap;
	int depth, bpp, stride;
	Bool ret;

	depth = crtc->scrn->depth;
	bpp = crtc->scrn->bitsPerPixel;
	stride = get_fb_stride(width, bpp);

	pixmap = screen->CreatePixmap(screen, width, height, depth, CREATE_PIXMAP_USAGE_BACKING_PIXMAP);
	if (pixmap == NullPixmap)
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR, "failed to create backing rotation pixmap\n");
	ret = screen->ModifyPixmapHeader(pixmap, width, height, 0, 0, stride, (void *)~0UL);
	if (!ret) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR, "failed to modify rotated pixmap\n");
		screen->DestroyPixmap(pixmap);
		return NullPixmap;
	}

	return pixmap;
}

static void fbdev_crtc_shadow_destroy(xf86CrtcPtr crtc, PixmapPtr pixmap,
				      void *data)
{
	ScreenPtr screen = crtc->scrn->pScreen;
	PixmapPtr root = screen->GetScreenPixmap(screen);
	Bool ret;

	if (pixmap)
		screen->DestroyPixmap(pixmap);
	if (data && data != (void *)~0UL)
		xfree(data);

	ret = screen->ModifyPixmapHeader(root, 0, 0, 0, 0, 0, (void *)~0UL);
	/* Well, the display's dead, so let's call it fatal. */
	if (!ret)
		FatalError("failed to reset root pixmap from rotation\n");
}

static void fbdev_crtc_gamma_set(xf86CrtcPtr crtc, CARD16 * red, CARD16 * green,
				 CARD16 * blue, int size)
{
	/* FIXME: Implement. */
	return;
}

static void fbdev_crtc_destroy(xf86CrtcPtr crtc)
{
	return;
}

static Bool fbdev_crtc_config_resize(ScrnInfoPtr scrn, int width, int height)
{
	scrn->virtualX = width;
	scrn->virtualY = height;

	return TRUE;
}

static const xf86CrtcFuncsRec fbdev_crtc_funcs = {
	.destroy = fbdev_crtc_destroy,
	.dpms = fbdev_crtc_dpms,
	.save = fbdev_crtc_save,
	.restore = fbdev_crtc_restore,
	.lock = fbdev_crtc_lock,
	.unlock = fbdev_crtc_unlock,
	.mode_fixup = fbdev_crtc_mode_fixup,
	.prepare = fbdev_crtc_prepare,
	.mode_set = fbdev_crtc_mode_set,
	.commit = fbdev_crtc_commit,
	.rotate = fbdev_crtc_rotate,
	.shadow_allocate = fbdev_crtc_shadow_allocate,
	.shadow_create = fbdev_crtc_shadow_create,
	.shadow_destroy = fbdev_crtc_shadow_destroy,
	.gamma_set = fbdev_crtc_gamma_set,
};

static const xf86OutputFuncsRec fbdev_output_funcs = {
	.create_resources = fbdev_output_create_resources,
	.destroy = fbdev_output_destroy,
	.dpms = fbdev_output_dpms,
	.save = fbdev_output_save,
	.restore = fbdev_output_restore,
	.mode_valid = fbdev_output_mode_valid,
	.mode_fixup = fbdev_output_mode_fixup,
	.prepare = fbdev_output_prepare,
	.mode_set = fbdev_output_mode_set,
	.commit = fbdev_output_commit,
	.detect = fbdev_output_detect,
	.get_modes = fbdev_output_get_modes,
#ifdef RANDR_12_INTERFACE
	.set_property = fbdev_output_set_property,
#endif
#ifdef RANDR_13_INTERFACE	/* not a typo */
	.get_property = fbdev_output_get_property,
#endif
#ifdef RANDR_GET_CRTC_INTERFACE
	.get_crtc = fbdev_output_crtc_get,
#endif
};

static const xf86CrtcConfigFuncsRec fbdev_crtc_config_funcs = {
	.resize = fbdev_crtc_config_resize,
};

static int fbdev_randr12_preinit(ScrnInfoPtr pScrn)
{
	FBDevPtr fPtr = FBDEVPTR(pScrn);
	int min_res, max_res;

	fbdevHWUseBuildinMode(pScrn);	/* sets pScrn->modes */
	pScrn->modes = xf86DuplicateMode(pScrn->modes);	/* well done, fbdevhw. */
	pScrn->modes->name = NULL;	/* fbdevhw string can't be freed */
	pScrn->modes->type = M_T_DRIVER | M_T_PREFERRED;
	pScrn->currentMode = pScrn->modes;
	fPtr->builtin = xf86DuplicateMode(pScrn->modes);

	pScrn->virtualX = fPtr->builtin->HDisplay;
	pScrn->virtualY = fPtr->builtin->VDisplay;
	xf86CrtcConfigInit(pScrn, &fbdev_crtc_config_funcs);
	min_res = min(fPtr->builtin->HDisplay, fPtr->builtin->VDisplay);
	max_res = max(fPtr->builtin->HDisplay, fPtr->builtin->VDisplay);
	xf86CrtcSetSizeRange(pScrn, min_res, min_res, max_res, max_res);

	fPtr->crtc_lcd = xf86CrtcCreate(pScrn, &fbdev_crtc_funcs);
	if (!fPtr->crtc_lcd)
		goto bail;
	fPtr->crtc_lcd->driver_private = fPtr;

	fPtr->output_lcd = xf86OutputCreate(pScrn, &fbdev_output_funcs, "LCD");
	if (!fPtr->output_lcd)
		goto bail_crtc;
	fPtr->output_lcd->possible_crtcs = (1 << 0);	/* FIXME: Walk list of CRTCs */
	fPtr->output_lcd->possible_clones = 0;
	fPtr->output_lcd->interlaceAllowed = FALSE;
	fPtr->output_lcd->doubleScanAllowed = FALSE;
	fPtr->output_lcd->driver_private = fPtr;
	fPtr->output_lcd->status = XF86OutputStatusConnected;

	if (!xf86InitialConfiguration(pScrn, FALSE)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Impossible initial config\n");
		goto bail_output;
	}

	return 1;

bail_output:
	xf86OutputDestroy(fPtr->output_lcd);
	fPtr->output_lcd = NULL;
bail_crtc:
	xf86CrtcDestroy(fPtr->crtc_lcd);
	fPtr->crtc_lcd = NULL;
bail:
	return 0;
}

static void fbdev_randr12_uninit(ScrnInfoPtr pScrn)
{
	FBDevPtr fPtr = FBDEVPTR(pScrn);

	fbdev_crtc_rotate(fPtr->crtc_lcd, 0);
	if (fPtr->output_lcd)
		xf86OutputDestroy(fPtr->output_lcd);
	fPtr->output_lcd = NULL;
	if (fPtr->crtc_lcd)
		xf86CrtcDestroy(fPtr->crtc_lcd);
	fPtr->crtc_lcd = NULL;
}

/* Forgive me, Father, for I have sinned. */
static void fbdev_enable_fb_access(int scrn_index, Bool enable)
{
	ScrnInfoPtr scrn = xf86Screens[scrn_index];
	FBDevPtr fbdev = FBDEVPTR(scrn);

	if (!fbdev->suppress_reconfig) {
		scrn->EnableDisableFBAccess = fbdev->enable_fb_access;
		scrn->EnableDisableFBAccess(scrn_index, enable);
		fbdev->enable_fb_access = scrn->EnableDisableFBAccess;
		scrn->EnableDisableFBAccess = fbdev_enable_fb_access;
	}
}

static void fbdev_property_callback(CallbackListPtr * pcbl, pointer unused, pointer calldata)
{
	XacePropertyAccessRec *rec = calldata;
	PropertyPtr prop = *rec->ppProp;
	ScreenPtr screen = rec->pWin->drawable.pScreen;
	ScrnInfoPtr scrn = xf86Screens[screen->myNum];
	FBDevPtr fbdev = FBDEVPTR(scrn);
	CARD32 data = 0;

	if (prop->propertyName != fbdev->suppress_reconfig_prop)
		return;

	if (rec->access_mode & DixWriteAccess) {
		if (prop->format != 32 || prop->size != 1)
			return;
		memcpy(&data, prop->data, 4);

		if (data != 1)
			return;
	} else if (rec->access_mode & DixDestroyAccess) {
		data = 0;
        }

	if (fbdev->suppress_reconfig != data) {
		fbdev->suppress_reconfig = data;
		xf86RandR12InhibitEvents(screen, data);
	}
}
