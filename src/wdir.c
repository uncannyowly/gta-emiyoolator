/*
 * WDIR32.DLL: SciTech WinDirect, which MGL (linked into the exe) always
 * loads. MGL looks up every function by name (cdecl, so a stub that ignores
 * its arguments cannot unbalance the stack). WD_* manage the full-screen
 * window and MGL's event queue; PM_* are real-mode helpers for direct VGA/VESA
 * access, which the DirectDraw path should not need: they log when called.
 */
#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* every call is logged once with its first arguments, then counted */
#define STUB(fn, ret)                                                                      \
	static int fn##_calls;                                                                 \
	static int fn(int a, int b, int c, int d)                                              \
	{                                                                                      \
		if (fn##_calls++ < 3)                                                              \
			shell_log("wdir32: %s(0x%x, 0x%x, 0x%x, 0x%x)", #fn + 2, a, b, c, d);          \
		return ret;                                                                        \
	}

STUB(w_PM_availableMemory, 0)
STUB(w_PM_callRealMode, 0)
STUB(w_PM_createSelector, 0)
STUB(w_PM_freeRealSeg, 0)
STUB(w_PM_freeSelector, 0)
STUB(w_PM_getBIOSSelector, 0)
STUB(w_PM_getByte, 0)
STUB(w_PM_getLong, 0)
STUB(w_PM_getWord, 0)
STUB(w_PM_int86, 0)
STUB(w_PM_int86x, 0)
STUB(w_PM_mapPhysicalAddr, 0)
STUB(w_PM_mapRealPointer, 0)
STUB(w_PM_memcpyfn, 0)
STUB(w_PM_memcpynf, 0)
STUB(w_PM_saveDS, 0)
STUB(w_PM_setBankA, 0)
STUB(w_PM_setBankAB, 0)
STUB(w_PM_setByte, 0)
STUB(w_PM_setCRTStart, 0)
STUB(w_PM_setLong, 0)
STUB(w_PM_setWord, 0)
STUB(w_WD_changeResolution, 0)
STUB(w_WD_flushEvent, 0)
STUB(w_WD_getMousePos, 0)
STUB(w_WD_haltEvent, 0)
STUB(w_WD_isSafeMode, 0)
STUB(w_WD_postEvent, 0)
STUB(w_WD_registerEventProc, 0)
STUB(w_WD_registerFullScreenWindow, 0)
STUB(w_WD_restoreGDI, 0)
STUB(w_WD_setAppInstance, 0)
STUB(w_WD_setMouseCallback, 0)
STUB(w_WD_setMousePos, 0)
STUB(w_WD_setSuspendAppCallback, 0)
STUB(w_WD_setTimerTick, 0)
STUB(w__WD_doSuspendApp, 0)
STUB(w__PM_setRMvect, 0)
STUB(w__PM_getRMvect, 0)
STUB(w_VF_available, 0)
STUB(w_VF_init, 0)
STUB(w_VF_exit, 0)

/* "real-mode" memory for BIOS probes: an ordinary buffer with a fake segment.
 * No BIOS answers (PM_int86 leaves registers alone), so VESA probing finds
 * nothing and MGL stays on DirectDraw. */
static void *w_PM_allocRealSeg(unsigned size, unsigned *seg, unsigned *off)
{
	static unsigned next_seg = 0x8000;
	void *p = calloc(1, size ? size : 16);
	if (seg)
		*seg = next_seg;
	if (off)
		*off = 0;
	next_seg += (size + 15) / 16;
	shell_log("wdir32: PM_allocRealSeg(%u) = %p", size, p);
	return p;
}

/* the full-screen window is the game's own window */
static int w_WD_startFullScreen(int hwnd, int x, int y)
{
	shell_log("wdir32: WD_startFullScreen(hwnd 0x%x, %dx%d)", hwnd, x, y);
	return hwnd;
}

/* MGL's event queue: empty for now (keys arrive as window messages) */
static int w_WD_getEvent(void *evt, int mask) { (void)evt; (void)mask; return 0; }
static int w_WD_peekEvent(void *evt, int mask) { (void)evt; (void)mask; return 0; }

#define F(n) { #n, 0, w##n }
const ShimFunc wdir32_funcs[] = {
	F(_PM_allocRealSeg), F(_PM_availableMemory), F(_PM_callRealMode), F(_PM_createSelector),
	F(_PM_freeRealSeg), F(_PM_freeSelector), F(_PM_getBIOSSelector), F(_PM_getByte), F(_PM_getLong),
	F(_PM_getWord), F(_PM_int86), F(_PM_int86x), F(_PM_mapPhysicalAddr), F(_PM_mapRealPointer),
	F(_PM_memcpyfn), F(_PM_memcpynf), F(_PM_saveDS), F(_PM_setBankA), F(_PM_setBankAB), F(_PM_setByte),
	F(_PM_setCRTStart), F(_PM_setLong), F(_PM_setWord),
	F(_WD_changeResolution), F(_WD_flushEvent), F(_WD_getEvent), F(_WD_getMousePos), F(_WD_haltEvent),
	F(_WD_isSafeMode), F(_WD_peekEvent), F(_WD_postEvent), F(_WD_registerEventProc),
	F(_WD_registerFullScreenWindow), F(_WD_restoreGDI), F(_WD_setAppInstance), F(_WD_setMouseCallback),
	F(_WD_setMousePos), F(_WD_setSuspendAppCallback), F(_WD_setTimerTick), F(_WD_startFullScreen),
	F(__WD_doSuspendApp), F(__PM_setRMvect), F(__PM_getRMvect), F(_VF_available), F(_VF_init), F(_VF_exit),
	{ NULL, 0, NULL },
};
