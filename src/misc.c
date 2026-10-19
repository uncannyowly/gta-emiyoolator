/*
 * The small DLLs: GDI32 (only MGL's fallback path; DirectDraw is the one we
 * serve), ADVAPI32 registry (replacing what GTA Settings.exe wrote), WINMM
 * joystick, DirectPlay (no network play) and Smacker (videos not played yet).
 */
#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- GDI32 --------------------------------------------------------------------- */

#define FAKE(v) ((HANDLE)(uintptr_t)(v))

static UINT WINAPI gdi_SetSystemPaletteUse(HANDLE dc, UINT use) { (void)dc; (void)use; return 1; }
static UINT WINAPI gdi_GetSystemPaletteUse(HANDLE dc) { (void)dc; return 1; }

static int WINAPI gdi_GetDeviceCaps(HANDLE dc, int index)
{
	(void)dc;
	switch (index) {
	case 8: return 640;      /* HORZRES */
	case 10: return 480;     /* VERTRES */
	case 12: return 16;      /* BITSPIXEL */
	case 14: return 1;       /* PLANES */
	case 104: return 256;    /* SIZEPALETTE */
	case 106: return 20;     /* NUMRESERVED */
	case 108: return 18;     /* COLORRES */
	default:
		TRACE("GetDeviceCaps(%d)", index);
		return 0;
	}
}

static HANDLE WINAPI gdi_SelectPalette(HANDLE dc, HANDLE pal, BOOL bg) { (void)dc; (void)bg; return pal; }
static UINT WINAPI gdi_RealizePalette(HANDLE dc) { (void)dc; return 0; }
static HANDLE WINAPI gdi_SelectObject(HANDLE dc, HANDLE obj) { (void)dc; return obj; }
static BOOL WINAPI gdi_DeleteDC(HANDLE dc) { (void)dc; return TRUE; }
static BOOL WINAPI gdi_DeleteObject(HANDLE obj) { (void)obj; return TRUE; }
static HANDLE WINAPI gdi_CreateCompatibleDC(HANDLE dc) { (void)dc; return FAKE(0x00040010); }
static HANDLE WINAPI gdi_CreatePalette(const void *lp) { (void)lp; return FAKE(0x00040020); }

static UINT WINAPI gdi_GetSystemPaletteEntries(HANDLE dc, UINT start, UINT n, uint8_t *pe)
{
	(void)dc; (void)start;
	if (pe)
		memset(pe, 0, n * 4);
	return n;
}

static HANDLE WINAPI gdi_CreateDCA(const char *drv, const char *dev, const char *out, const void *mode)
{
	(void)dev; (void)out; (void)mode;
	TRACE("CreateDCA(%s)", drv ? drv : "");
	return FAKE(0x00040030);
}

static int WINAPI gdi_Escape(HANDLE dc, int esc, int n, const char *in, void *out)
{
	(void)dc; (void)n; (void)in; (void)out;
	TRACE("Escape(%d)", esc);
	return 0;
}

static int WINAPI gdi_GetDIBits(HANDLE dc, HANDLE bm, UINT start, UINT lines, void *bits, void *bmi, UINT use)
{
	(void)dc; (void)bm; (void)start; (void)lines; (void)bits; (void)bmi; (void)use;
	return 0;
}

static HANDLE WINAPI gdi_CreateCompatibleBitmap(HANDLE dc, int w, int h) { (void)dc; (void)w; (void)h; return FAKE(0x00040040); }
static BOOL WINAPI gdi_AnimatePalette(HANDLE pal, UINT start, UINT n, const void *pe) { (void)pal; (void)start; (void)n; (void)pe; return TRUE; }

static BOOL WINAPI gdi_BitBlt(HANDLE d, int x, int y, int w, int h, HANDLE s, int sx, int sy, DWORD rop)
{
	(void)d; (void)x; (void)y; (void)w; (void)h; (void)s; (void)sx; (void)sy; (void)rop;
	TRACE("BitBlt");
	return TRUE;
}

static BOOL WINAPI gdi_StretchBlt(HANDLE d, int x, int y, int w, int h, HANDLE s, int sx, int sy, int sw, int sh, DWORD rop)
{
	(void)d; (void)x; (void)y; (void)w; (void)h; (void)s; (void)sx; (void)sy; (void)sw; (void)sh; (void)rop;
	TRACE("StretchBlt");
	return TRUE;
}

/* a device-independent bitmap MGL draws into: real memory, fake handle */
static HANDLE WINAPI gdi_CreateDIBSection(HANDLE dc, const uint8_t *bmi, UINT usage, void **bits, HANDLE section, DWORD off)
{
	int32_t w, h;
	uint16_t bpp;
	size_t stride;
	(void)dc; (void)usage; (void)section; (void)off;
	memcpy(&w, bmi + 4, 4);
	memcpy(&h, bmi + 8, 4);
	memcpy(&bpp, bmi + 14, 2);
	stride = (size_t)(((w * bpp + 31) / 32) * 4);
	*bits = calloc(stride, (size_t)(h < 0 ? -h : h));
	shell_log("CreateDIBSection(%dx%d, %d bpp) = %p", w, h, bpp, *bits);
	return *bits ? FAKE(0x00070000 + ((uintptr_t)*bits & 0xFFFF)) : NULL;
}

static UINT WINAPI gdi_SetDIBColorTable(HANDLE dc, UINT start, UINT n, const void *rgb) { (void)dc; (void)start; (void)rgb; return n; }

static int WINAPI gdi_SetStretchBltMode(HANDLE dc, int mode) { (void)dc; (void)mode; return 1; }
static HANDLE WINAPI gdi_GetStockObject(int i) { return FAKE(0x00050000 + i); }

const ShimFunc gdi32_funcs[] = {
	{ "SetSystemPaletteUse", 0, gdi_SetSystemPaletteUse },
	{ "GetSystemPaletteUse", 0, gdi_GetSystemPaletteUse },
	{ "GetDeviceCaps", 0, gdi_GetDeviceCaps },
	{ "SelectPalette", 0, gdi_SelectPalette },
	{ "RealizePalette", 0, gdi_RealizePalette },
	{ "SelectObject", 0, gdi_SelectObject },
	{ "DeleteDC", 0, gdi_DeleteDC },
	{ "DeleteObject", 0, gdi_DeleteObject },
	{ "CreateCompatibleDC", 0, gdi_CreateCompatibleDC },
	{ "CreatePalette", 0, gdi_CreatePalette },
	{ "GetSystemPaletteEntries", 0, gdi_GetSystemPaletteEntries },
	{ "CreateDCA", 0, gdi_CreateDCA },
	{ "Escape", 0, gdi_Escape },
	{ "GetDIBits", 0, gdi_GetDIBits },
	{ "CreateCompatibleBitmap", 0, gdi_CreateCompatibleBitmap },
	{ "AnimatePalette", 0, gdi_AnimatePalette },
	{ "BitBlt", 0, gdi_BitBlt },
	{ "StretchBlt", 0, gdi_StretchBlt },
	{ "SetStretchBltMode", 0, gdi_SetStretchBltMode },
	{ "CreateDIBSection", 0, gdi_CreateDIBSection },
	{ "SetDIBColorTable", 0, gdi_SetDIBColorTable },
	{ "GetStockObject", 0, gdi_GetStockObject },
	{ NULL, 0, NULL },
};

/* ---- ADVAPI32 registry ------------------------------------------------------------ */

#define KEY_GTA FAKE(0x00060010)
#define KEY_CONTROLS FAKE(0x00060020)
#define KEY_POLICY FAKE(0x00060030)

/* GTA Settings.exe "Default Keys": arrows, Space, Enter, Ctrl, X, Z, Tab */
static const DWORD default_controls[10] = { 0xCB, 0xCD, 0xC8, 0xD0, 0x39, 0x1C, 0x1D, 0x2D, 0x2C, 0x0F };
static DWORD policy_autorun = 0x95;

static LONG WINAPI reg_RegOpenKeyExA(HANDLE key, const char *sub, DWORD opt, DWORD sam, HANDLE *out)
{
	(void)opt; (void)sam;
	shell_log("RegOpenKeyExA(%p, \"%s\")", key, sub ? sub : "");
	if (!sub)
		return 2;
	if (!strcasecmp(sub, "SOFTWARE\\DMA Design\\Grand Theft Auto"))
		*out = KEY_GTA;
	else if (!strcasecmp(sub, "SOFTWARE\\DMA Design\\Grand Theft Auto\\Controls"))
		*out = KEY_CONTROLS;
	else if (!strcasecmp(sub, "Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer"))
		*out = KEY_POLICY;
	else
		return 2;   /* ERROR_FILE_NOT_FOUND */
	return 0;
}

static LONG WINAPI reg_RegQueryValueExA(HANDLE key, const char *name, DWORD *res, DWORD *type, uint8_t *data, DWORD *size)
{
	DWORD v;
	char cfg[32];
	(void)res;
	if (key == KEY_GTA && !strcasecmp(name, "Language")) {
		v = (DWORD)config_int("language", 0);
	} else if (key == KEY_CONTROLS && !strncasecmp(name, "Control ", 8) && name[8] >= '0' && name[8] <= '9' && !name[9]) {
		snprintf(cfg, sizeof cfg, "control%c", name[8]);
		v = (DWORD)config_int(cfg, (int)default_controls[name[8] - '0']);
	} else if (key == KEY_POLICY && !strcasecmp(name, "NoDriveTypeAutoRun")) {
		v = policy_autorun;
	} else {
		shell_log("RegQueryValueExA(%p, \"%s\"): not found", key, name ? name : "");
		return 2;
	}
	if (type)
		*type = 4;   /* REG_DWORD */
	if (data) {
		if (!size || *size < 4)
			return 234;   /* ERROR_MORE_DATA */
		memcpy(data, &v, 4);
	}
	if (size)
		*size = 4;
	TRACE("RegQueryValueExA(\"%s\") = 0x%x", name, v);
	return 0;
}

static LONG WINAPI reg_RegSetValueExA(HANDLE key, const char *name, DWORD res, DWORD type, const uint8_t *data, DWORD size)
{
	(void)res; (void)type;
	if (key == KEY_POLICY && size >= 4)
		memcpy(&policy_autorun, data, 4);
	TRACE("RegSetValueExA(\"%s\")", name ? name : "");
	return 0;
}

static LONG WINAPI reg_RegCloseKey(HANDLE key) { (void)key; return 0; }

const ShimFunc advapi32_funcs[] = {
	{ "RegOpenKeyExA", 0, reg_RegOpenKeyExA },
	{ "RegQueryValueExA", 0, reg_RegQueryValueExA },
	{ "RegSetValueExA", 0, reg_RegSetValueExA },
	{ "RegCloseKey", 0, reg_RegCloseKey },
	{ NULL, 0, NULL },
};

/* ---- WINMM: no joystick (the buttons arrive as keys) ------------------------------- */

static UINT WINAPI mm_joyGetPosEx(UINT id, void *info) { (void)id; (void)info; return 167; }   /* JOYERR_UNPLUGGED */
static UINT WINAPI mm_joyGetDevCapsA(UINT id, void *caps, UINT size) { (void)id; (void)caps; (void)size; return 165; }
static DWORD WINAPI mm_timeGetTime(void) { return shell_ms(); }

const ShimFunc winmm_funcs[] = {
	{ "joyGetPosEx", 0, mm_joyGetPosEx },
	{ "joyGetDevCapsA", 0, mm_joyGetDevCapsA },
	{ "timeGetTime", 0, mm_timeGetTime },
	{ NULL, 0, NULL },
};

/* ---- DPLAYX: no network play ---------------------------------------------------- */

static HRESULT WINAPI dp_DirectPlayCreate(const void *guid, void **dp, void *outer)
{
	(void)guid; (void)outer;
	if (dp)
		*dp = NULL;
	shell_log("DirectPlayCreate: network play is not available");
	return 0x80004005u;   /* E_FAIL */
}

static HRESULT WINAPI dp_DirectPlayEnumerateA(void *cb, void *ctx)
{
	(void)cb; (void)ctx;
	return 0;   /* no service providers */
}

const ShimFunc dplayx_funcs[] = {
	{ NULL, 1, dp_DirectPlayCreate },
	{ NULL, 2, dp_DirectPlayEnumerateA },
	{ "DirectPlayCreate", 0, dp_DirectPlayCreate },
	{ "DirectPlayEnumerateA", 0, dp_DirectPlayEnumerateA },
	{ NULL, 0, NULL },
};

/* ---- SMACKW32: videos are skipped for now ------------------------------------------ */

static void *WINAPI smk_SmackOpen(const char *name, DWORD flags, DWORD extra)
{
	(void)flags; (void)extra;
	shell_log("SmackOpen(\"%s\"): videos not supported yet, skipping", name ? name : "");
	return NULL;
}

static void WINAPI smk_SmackClose(void *s) { (void)s; }
static DWORD WINAPI smk_SmackDoFrame(void *s) { (void)s; return 0; }
static void WINAPI smk_SmackNextFrame(void *s) { (void)s; }
static void WINAPI smk_SmackToScreen(void *s, DWORD l, DWORD t, DWORD bps, const void *tbl, DWORD force)
{
	(void)s; (void)l; (void)t; (void)bps; (void)tbl; (void)force;
}
static DWORD WINAPI smk_SmackToBufferRect(void *s, DWORD surface) { (void)s; (void)surface; return 0; }
static DWORD WINAPI smk_SmackWait(void *s) { (void)s; return 0; }
static DWORD WINAPI smk_SmackSoundUseMSS(void *dig) { (void)dig; return 1; }

const ShimFunc smackw32_funcs[] = {
	{ NULL, 14, smk_SmackOpen },
	{ NULL, 18, smk_SmackClose },
	{ NULL, 19, smk_SmackDoFrame },
	{ NULL, 21, smk_SmackNextFrame },
	{ NULL, 23, smk_SmackToScreen },
	{ NULL, 28, smk_SmackToBufferRect },
	{ NULL, 32, smk_SmackWait },
	{ NULL, 33, smk_SmackSoundUseMSS },
	{ NULL, 0, NULL },
};
