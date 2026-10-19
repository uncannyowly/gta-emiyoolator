/*
 * DDRAW: DirectDraw as SciTech MGL (inside the exe) uses it. Surfaces live in
 * system memory; Flip swaps the primary with its back buffer and shows the
 * result, Unlock/Blt on a primary without a back buffer shows it at once.
 * Every method has its exact argument count (they are stdcall), including the
 * ones that only log and return DDERR_UNSUPPORTED.
 */
#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DD_OK 0
#define DDERR_GENERIC 0x80004005u
#define DDERR_UNSUPPORTED 0x80004001u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_NOTFOUND 0x887600FFu
#define E_NOINTERFACE 0x80004002u

/* DDSURFACEDESC field offsets */
#define SD_SIZE 0
#define SD_FLAGS 4
#define SD_HEIGHT 8
#define SD_WIDTH 12
#define SD_PITCH 16
#define SD_BACKBUFFERS 20
#define SD_SURFACE 36
#define SD_CKSRCBLT 64
#define SD_PIXFMT 72
#define SD_CAPS 104

#define DDSD_CAPS 0x1
#define DDSD_HEIGHT 0x2
#define DDSD_WIDTH 0x4
#define DDSD_PITCH 0x8
#define DDSD_BACKBUFFERCOUNT 0x20
#define DDSD_LPSURFACE 0x800
#define DDSD_PIXELFORMAT 0x1000

#define DDSCAPS_BACKBUFFER 0x4
#define DDSCAPS_COMPLEX 0x8
#define DDSCAPS_FLIP 0x10
#define DDSCAPS_FRONTBUFFER 0x20
#define DDSCAPS_OFFSCREENPLAIN 0x40
#define DDSCAPS_PRIMARYSURFACE 0x200
#define DDSCAPS_SYSTEMMEMORY 0x800
#define DDSCAPS_VIDEOMEMORY 0x4000
#define DDSCAPS_VISIBLE 0x8000

typedef struct Pal {
	const void *const *vtbl;
	int refs;
	uint32_t rgb[256];   /* 0x00RRGGBB */
	uint8_t raw[256][4]; /* PALETTEENTRY as given */
} Pal;

typedef struct Surf {
	const void *const *vtbl;
	int refs;
	int w, h, bpp, pitch;
	uint8_t *mem;
	DWORD caps;
	struct Surf *back;   /* primary: its back buffer */
	struct Surf *front;  /* back buffer: its primary */
	Pal *pal;
	DWORD ck_lo, ck_hi;
	int has_ck;
} Surf;

typedef struct {
	const void *const *vtbl;
	int refs;
} Obj;

/* display state, shared by every DirectDraw interface */
static int mode_w = 640, mode_h = 480, mode_bpp = 16;
static Surf *primary;
static uint32_t frames;

/* ---- presenting ------------------------------------------------------------- */

static void present(Surf *s)
{
	static const uint32_t grey[256];
	if (!s)
		return;
	frames++;
	video_present(s->mem, s->pitch, s->w, s->h, s->bpp, s->pal ? s->pal->rgb : (primary && primary->pal ? primary->pal->rgb : grey));
}

/* ---- pixel formats --------------------------------------------------------------- */

static void fill_pixfmt(uint8_t *pf, int bpp)
{
	uint32_t *p = (uint32_t *)pf;
	memset(pf, 0, 32);
	p[0] = 32;
	p[3] = (uint32_t)bpp;
	if (bpp == 8) {
		p[1] = 0x20 | 0x40;   /* DDPF_PALETTEINDEXED8 | DDPF_RGB */
	} else if (bpp == 16) {
		p[1] = 0x40;
		p[4] = 0xF800; p[5] = 0x07E0; p[6] = 0x001F;
	} else {
		p[1] = 0x40;
		p[4] = 0xFF0000; p[5] = 0x00FF00; p[6] = 0x0000FF;
	}
}

static void fill_desc(uint8_t *d, const Surf *s, const RECT *r)
{
	DWORD size = *(DWORD *)d;
	uint8_t *mem = s->mem;
	if (size < 108)
		size = 108;
	memset(d, 0, size);
	*(DWORD *)(d + SD_SIZE) = size;
	*(DWORD *)(d + SD_FLAGS) = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_LPSURFACE;
	*(DWORD *)(d + SD_HEIGHT) = (DWORD)s->h;
	*(DWORD *)(d + SD_WIDTH) = (DWORD)s->w;
	*(LONG *)(d + SD_PITCH) = s->pitch;
	*(DWORD *)(d + SD_BACKBUFFERS) = s->back ? 1 : 0;
	if (r)
		mem += r->top * s->pitch + r->left * (s->bpp / 8);
	*(uint32_t *)(d + SD_SURFACE) = (uint32_t)(uintptr_t)mem;
	fill_pixfmt(d + SD_PIXFMT, s->bpp);
	*(DWORD *)(d + SD_CAPS) = s->caps;
}

/* ---- palette ----------------------------------------------------------------------- */

static const void *const pal_vtbl[];

static void pal_set(Pal *p, DWORD start, DWORD n, const uint8_t *e)
{
	for (DWORD i = 0; i < n && start + i < 256; i++) {
		memcpy(p->raw[start + i], e + 4 * i, 4);
		p->rgb[start + i] = ((uint32_t)e[4 * i] << 16) | ((uint32_t)e[4 * i + 1] << 8) | e[4 * i + 2];
	}
}

static HRESULT WINAPI pal_QueryInterface(Pal *p, const DWORD *iid, void **out) { (void)p; (void)iid; *out = NULL; return E_NOINTERFACE; }
static DWORD WINAPI pal_AddRef(Pal *p) { return (DWORD)++p->refs; }
static DWORD WINAPI pal_Release(Pal *p) { return (DWORD)--p->refs; }   /* palettes are kept: surfaces may still point at them */
static HRESULT WINAPI pal_GetCaps(Pal *p, DWORD *caps) { (void)p; *caps = 0x4 | 0x40; return DD_OK; }   /* 8BIT | ALLOW256 */

static HRESULT WINAPI pal_GetEntries(Pal *p, DWORD flags, DWORD start, DWORD n, uint8_t *e)
{
	(void)flags;
	for (DWORD i = 0; i < n && start + i < 256; i++)
		memcpy(e + 4 * i, p->raw[start + i], 4);
	return DD_OK;
}

static HRESULT WINAPI pal_Initialize(Pal *p, void *dd, DWORD flags, const uint8_t *e) { (void)p; (void)dd; (void)flags; (void)e; return DD_OK; }

static HRESULT WINAPI pal_SetEntries(Pal *p, DWORD flags, DWORD start, DWORD n, const uint8_t *e)
{
	(void)flags;
	pal_set(p, start, n, e);
	/* the screen palette changed: show it (fades work this way) */
	if (primary && primary->pal == p && primary->bpp == 8)
		present(primary);
	return DD_OK;
}

static const void *const pal_vtbl[] = {
	pal_QueryInterface, pal_AddRef, pal_Release, pal_GetCaps, pal_GetEntries, pal_Initialize, pal_SetEntries,
};

/* ---- clipper ----------------------------------------------------------------------- */

static HRESULT WINAPI clip_QueryInterface(Obj *o, const DWORD *iid, void **out) { (void)o; (void)iid; *out = NULL; return E_NOINTERFACE; }
static DWORD WINAPI clip_AddRef(Obj *o) { return (DWORD)++o->refs; }
static DWORD WINAPI clip_Release(Obj *o) { return (DWORD)--o->refs; }
static HRESULT WINAPI clip_GetClipList(Obj *o, RECT *r, void *data, DWORD *size) { (void)o; (void)r; (void)data; if (size) *size = 0; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI clip_GetHWnd(Obj *o, HANDLE *h) { (void)o; *h = (HANDLE)(uintptr_t)0x00010010; return DD_OK; }
static HRESULT WINAPI clip_Initialize(Obj *o, void *dd, DWORD flags) { (void)o; (void)dd; (void)flags; return DD_OK; }
static HRESULT WINAPI clip_IsClipListChanged(Obj *o, BOOL *changed) { (void)o; *changed = FALSE; return DD_OK; }
static HRESULT WINAPI clip_SetClipList(Obj *o, void *data, DWORD flags) { (void)o; (void)data; (void)flags; return DD_OK; }
static HRESULT WINAPI clip_SetHWnd(Obj *o, DWORD flags, HANDLE h) { (void)o; (void)flags; (void)h; return DD_OK; }

static const void *const clip_vtbl[] = {
	clip_QueryInterface, clip_AddRef, clip_Release, clip_GetClipList, clip_GetHWnd, clip_Initialize,
	clip_IsClipListChanged, clip_SetClipList, clip_SetHWnd,
};

static Obj *new_clipper(void)
{
	Obj *c = calloc(1, sizeof *c);
	c->vtbl = clip_vtbl;
	c->refs = 1;
	return c;
}

/* ---- surfaces ----------------------------------------------------------------------- */

static const void *const surf_vtbl[];

static Surf *new_surface(int w, int h, int bpp, DWORD caps)
{
	Surf *s = calloc(1, sizeof *s);
	s->vtbl = surf_vtbl;
	s->refs = 1;
	s->w = w;
	s->h = h;
	s->bpp = bpp;
	s->pitch = ((w * (bpp / 8)) + 3) & ~3;
	s->mem = calloc((size_t)s->pitch, (size_t)h);
	s->caps = caps;
	return s;
}

#define UNSUPPORTED(name) shell_log("ddraw: %s is not supported", name)

static HRESULT WINAPI s_QueryInterface(Surf *s, const DWORD *iid, void **out)
{
	/* IDirectDrawSurface, 2 and 3 share one vtable (each extends the last) */
	if (iid[0] == 0x6C14DB81u || iid[0] == 0x57805885u || iid[0] == 0xDA044E00u) {
		s->refs++;
		*out = s;
		return DD_OK;
	}
	shell_log("ddraw: surface QueryInterface %08x: no", iid[0]);
	*out = NULL;
	return E_NOINTERFACE;
}

static DWORD WINAPI s_AddRef(Surf *s) { return (DWORD)++s->refs; }

static DWORD WINAPI s_Release(Surf *s)
{
	if (--s->refs > 0)
		return (DWORD)s->refs;
	if (s == primary)
		primary = NULL;
	if (s->back && s->back->front == s)
		s->back->front = NULL;
	if (s->front && s->front->back == s) {
		s->front->back = NULL;
		return 0;   /* owned by its primary's flip chain memory: free with it */
	}
	free(s->mem);
	free(s);
	return 0;
}

static HRESULT WINAPI s_AddAttachedSurface(Surf *s, Surf *a) { s->back = a; a->front = s; a->refs++; return DD_OK; }
static HRESULT WINAPI s_AddOverlayDirtyRect(Surf *s, RECT *r) { (void)s; (void)r; return DDERR_UNSUPPORTED; }

static uint32_t read_px(const Surf *s, int x, int y)
{
	const uint8_t *p = s->mem + y * s->pitch + x * (s->bpp / 8);
	switch (s->bpp) {
	case 8: return p[0];
	case 16: return *(const uint16_t *)p;
	case 24: return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16);
	default: return *(const uint32_t *)p;
	}
}

static void write_px(Surf *s, int x, int y, uint32_t v)
{
	uint8_t *p = s->mem + y * s->pitch + x * (s->bpp / 8);
	switch (s->bpp) {
	case 8: p[0] = (uint8_t)v; break;
	case 16: *(uint16_t *)p = (uint16_t)v; break;
	case 24: p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); break;
	default: *(uint32_t *)p = v; break;
	}
}

static void clip_rect(RECT *r, const Surf *s)
{
	if (r->left < 0) r->left = 0;
	if (r->top < 0) r->top = 0;
	if (r->right > s->w) r->right = s->w;
	if (r->bottom > s->h) r->bottom = s->h;
}

/* copy src_r of src to dst_r of dst (nearest-neighbour scaling), optionally keyed */
static void blit(Surf *dst, RECT d, Surf *src, RECT sr, int keyed)
{
	int dw = d.right - d.left, dh = d.bottom - d.top, sw = sr.right - sr.left, sh = sr.bottom - sr.top;
	if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
		return;
	if (!keyed && dw == sw && dh == sh && dst->bpp == src->bpp) {
		int bytes = dw * (dst->bpp / 8);
		for (int y = 0; y < dh; y++)
			memmove(dst->mem + (d.top + y) * dst->pitch + d.left * (dst->bpp / 8),
			        src->mem + (sr.top + y) * src->pitch + sr.left * (src->bpp / 8), (size_t)bytes);
		return;
	}
	for (int y = 0; y < dh; y++) {
		int sy = sr.top + y * sh / dh;
		for (int x = 0; x < dw; x++) {
			uint32_t v = read_px(src, sr.left + x * sw / dw, sy);
			if (keyed && v >= src->ck_lo && v <= src->ck_hi)
				continue;
			write_px(dst, d.left + x, d.top + y, v);
		}
	}
}

static void maybe_present(Surf *s)
{
	if (s == primary && !s->back)
		present(s);
}

static HRESULT WINAPI s_Blt(Surf *s, RECT *dr, Surf *src, RECT *sr, DWORD flags, const uint8_t *fx)
{
	RECT d = dr ? *dr : (RECT){ 0, 0, s->w, s->h };
	clip_rect(&d, s);
	if (flags & 0x400) {   /* DDBLT_COLORFILL */
		uint32_t c = fx ? *(const uint32_t *)(fx + 80) : 0;
		for (int y = d.top; y < d.bottom; y++)
			for (int x = d.left; x < d.right; x++)
				write_px(s, x, y, c);
	} else if (src) {
		RECT r = sr ? *sr : (RECT){ 0, 0, src->w, src->h };
		clip_rect(&r, src);
		blit(s, d, src, r, (flags & 0x8000) && src->has_ck);   /* DDBLT_KEYSRC */
	} else {
		TRACE("ddraw: Blt flags 0x%x without source", flags);
	}
	maybe_present(s);
	return DD_OK;
}

static HRESULT WINAPI s_BltBatch(Surf *s, void *b, DWORD n, DWORD f) { (void)s; (void)b; (void)n; (void)f; UNSUPPORTED("BltBatch"); return DDERR_UNSUPPORTED; }

static HRESULT WINAPI s_BltFast(Surf *s, DWORD x, DWORD y, Surf *src, RECT *sr, DWORD trans)
{
	RECT r = sr ? *sr : (RECT){ 0, 0, src->w, src->h };
	RECT d = { (LONG)x, (LONG)y, (LONG)x + (r.right - r.left), (LONG)y + (r.bottom - r.top) };
	blit(s, d, src, r, (trans & 1) && src->has_ck);   /* DDBLTFAST_SRCCOLORKEY */
	maybe_present(s);
	return DD_OK;
}

static HRESULT WINAPI s_DeleteAttachedSurface(Surf *s, DWORD f, Surf *a) { (void)f; if (s->back == a) s->back = NULL; return DD_OK; }

static HRESULT WINAPI s_EnumAttachedSurfaces(Surf *s, void *ctx, HRESULT (WINAPI *cb)(Surf *, uint8_t *, void *))
{
	if (s->back) {
		uint8_t desc[108];
		*(DWORD *)desc = 108;
		fill_desc(desc, s->back, NULL);
		s->back->refs++;
		cb(s->back, desc, ctx);
	}
	return DD_OK;
}

static HRESULT WINAPI s_EnumOverlayZOrders(Surf *s, DWORD f, void *c, void *cb) { (void)s; (void)f; (void)c; (void)cb; return DDERR_UNSUPPORTED; }

static HRESULT WINAPI s_Flip(Surf *s, Surf *target, DWORD flags)
{
	Surf *b = target ? target : s->back;
	uint8_t *t;
	(void)flags;
	if (!b)
		return DDERR_NOTFOUND;
	t = s->mem;
	s->mem = b->mem;
	b->mem = t;
	present(s);
	return DD_OK;
}

static HRESULT WINAPI s_GetAttachedSurface(Surf *s, const DWORD *caps, Surf **out)
{
	(void)caps;
	if (!s->back) {
		*out = NULL;
		return DDERR_NOTFOUND;
	}
	s->back->refs++;
	*out = s->back;
	return DD_OK;
}

static HRESULT WINAPI s_GetBltStatus(Surf *s, DWORD f) { (void)s; (void)f; return DD_OK; }
static HRESULT WINAPI s_GetCaps(Surf *s, DWORD *caps) { *caps = s->caps; return DD_OK; }
static HRESULT WINAPI s_GetClipper(Surf *s, void **c) { (void)s; *c = NULL; return DDERR_NOTFOUND; }

static HRESULT WINAPI s_GetColorKey(Surf *s, DWORD f, DWORD *ck)
{
	(void)f;
	if (!s->has_ck)
		return DDERR_NOTFOUND;
	ck[0] = s->ck_lo;
	ck[1] = s->ck_hi;
	return DD_OK;
}

static HRESULT WINAPI s_GetDC(Surf *s, HANDLE *dc) { (void)s; *dc = NULL; UNSUPPORTED("Surface::GetDC"); return DDERR_UNSUPPORTED; }
static HRESULT WINAPI s_GetFlipStatus(Surf *s, DWORD f) { (void)s; (void)f; return DD_OK; }
static HRESULT WINAPI s_GetOverlayPosition(Surf *s, LONG *x, LONG *y) { (void)s; (void)x; (void)y; return DDERR_UNSUPPORTED; }

static HRESULT WINAPI s_GetPalette(Surf *s, Pal **p)
{
	if (!s->pal)
		return DDERR_NOTFOUND;
	s->pal->refs++;
	*p = s->pal;
	return DD_OK;
}

static HRESULT WINAPI s_GetPixelFormat(Surf *s, uint8_t *pf) { fill_pixfmt(pf, s->bpp); return DD_OK; }
static HRESULT WINAPI s_GetSurfaceDesc(Surf *s, uint8_t *d) { fill_desc(d, s, NULL); return DD_OK; }
static HRESULT WINAPI s_Initialize(Surf *s, void *dd, void *d) { (void)s; (void)dd; (void)d; return DD_OK; }
static HRESULT WINAPI s_IsLost(Surf *s) { (void)s; return DD_OK; }

static HRESULT WINAPI s_Lock(Surf *s, RECT *r, uint8_t *d, DWORD flags, HANDLE ev)
{
	(void)flags; (void)ev;
	fill_desc(d, s, r);
	return DD_OK;
}

static HRESULT WINAPI s_ReleaseDC(Surf *s, HANDLE dc) { (void)s; (void)dc; return DD_OK; }
static HRESULT WINAPI s_Restore(Surf *s) { (void)s; return DD_OK; }
static HRESULT WINAPI s_SetClipper(Surf *s, void *c) { (void)s; (void)c; return DD_OK; }

static HRESULT WINAPI s_SetColorKey(Surf *s, DWORD flags, const DWORD *ck)
{
	if (!ck) {
		s->has_ck = 0;
		return DD_OK;
	}
	if (flags & 0x8) {   /* DDCKEY_SRCBLT */
		s->ck_lo = ck[0];
		s->ck_hi = ck[1] < ck[0] ? ck[0] : ck[1];
		s->has_ck = 1;
	}
	return DD_OK;
}

static HRESULT WINAPI s_SetOverlayPosition(Surf *s, LONG x, LONG y) { (void)s; (void)x; (void)y; return DDERR_UNSUPPORTED; }

static HRESULT WINAPI s_SetPalette(Surf *s, Pal *p)
{
	s->pal = p;
	if (p)
		p->refs++;
	if (s == primary && s->bpp == 8)
		present(s);
	return DD_OK;
}

static HRESULT WINAPI s_Unlock(Surf *s, void *p)
{
	(void)p;
	maybe_present(s);
	return DD_OK;
}

static HRESULT WINAPI s_UpdateOverlay(Surf *s, RECT *r, Surf *d, RECT *dr, DWORD f, void *fx) { (void)s; (void)r; (void)d; (void)dr; (void)f; (void)fx; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI s_UpdateOverlayDisplay(Surf *s, DWORD f) { (void)s; (void)f; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI s_UpdateOverlayZOrder(Surf *s, DWORD f, Surf *r) { (void)s; (void)f; (void)r; return DDERR_UNSUPPORTED; }
/* IDirectDrawSurface2 */
static HRESULT WINAPI s_GetDDInterface(Surf *s, void **dd) { (void)s; *dd = NULL; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI s_PageLock(Surf *s, DWORD f) { (void)s; (void)f; return DD_OK; }
static HRESULT WINAPI s_PageUnlock(Surf *s, DWORD f) { (void)s; (void)f; return DD_OK; }
/* IDirectDrawSurface3 */
static HRESULT WINAPI s_SetSurfaceDesc(Surf *s, uint8_t *d, DWORD f) { (void)s; (void)d; (void)f; return DDERR_UNSUPPORTED; }

static const void *const surf_vtbl[] = {
	s_QueryInterface, s_AddRef, s_Release, s_AddAttachedSurface, s_AddOverlayDirtyRect, s_Blt, s_BltBatch,
	s_BltFast, s_DeleteAttachedSurface, s_EnumAttachedSurfaces, s_EnumOverlayZOrders, s_Flip,
	s_GetAttachedSurface, s_GetBltStatus, s_GetCaps, s_GetClipper, s_GetColorKey, s_GetDC, s_GetFlipStatus,
	s_GetOverlayPosition, s_GetPalette, s_GetPixelFormat, s_GetSurfaceDesc, s_Initialize, s_IsLost, s_Lock,
	s_ReleaseDC, s_Restore, s_SetClipper, s_SetColorKey, s_SetOverlayPosition, s_SetPalette, s_Unlock,
	s_UpdateOverlay, s_UpdateOverlayDisplay, s_UpdateOverlayZOrder, s_GetDDInterface, s_PageLock,
	s_PageUnlock, s_SetSurfaceDesc,
};

/* ---- IDirectDraw and IDirectDraw2 ---------------------------------------------------- */

static const void *const dd1_vtbl[], *const dd2_vtbl[];
static Obj dd1 = { dd1_vtbl, 0 }, dd2 = { dd2_vtbl, 0 };

static HRESULT WINAPI dd_QueryInterface(Obj *o, const DWORD *iid, void **out)
{
	(void)o;
	shell_log("ddraw: QueryInterface %08x", iid[0]);
	if (iid[0] == 0x6C14DB80u) { dd1.refs++; *out = &dd1; return DD_OK; }   /* IID_IDirectDraw */
	if (iid[0] == 0xB3A6F3E0u) { dd2.refs++; *out = &dd2; return DD_OK; }   /* IID_IDirectDraw2 */
	*out = NULL;
	return E_NOINTERFACE;
}

static DWORD WINAPI dd_AddRef(Obj *o) { return (DWORD)++o->refs; }
static DWORD WINAPI dd_Release(Obj *o) { return (DWORD)(o->refs > 0 ? --o->refs : 0); }
static HRESULT WINAPI dd_Compact(Obj *o) { (void)o; return DD_OK; }

static HRESULT WINAPI dd_CreateClipper(Obj *o, DWORD f, Obj **out, void *outer) { (void)o; (void)f; (void)outer; *out = new_clipper(); return DD_OK; }

static HRESULT WINAPI dd_CreatePalette(Obj *o, DWORD flags, const uint8_t *entries, Pal **out, void *outer)
{
	Pal *p = calloc(1, sizeof *p);
	(void)o; (void)outer;
	p->vtbl = pal_vtbl;
	p->refs = 1;
	if (entries)
		pal_set(p, 0, 256, entries);
	TRACE("ddraw: CreatePalette flags 0x%x", flags);
	*out = p;
	return DD_OK;
}

static HRESULT WINAPI dd_CreateSurface(Obj *o, const uint8_t *d, Surf **out, void *outer)
{
	DWORD flags = *(const DWORD *)(d + SD_FLAGS);
	DWORD caps = (flags & DDSD_CAPS) ? *(const DWORD *)(d + SD_CAPS) : 0;
	int w = mode_w, h = mode_h, bpp = mode_bpp;
	Surf *s;
	(void)o; (void)outer;

	if (!(caps & DDSCAPS_PRIMARYSURFACE)) {
		if (flags & DDSD_WIDTH) w = (int)*(const DWORD *)(d + SD_WIDTH);
		if (flags & DDSD_HEIGHT) h = (int)*(const DWORD *)(d + SD_HEIGHT);
		if (flags & DDSD_PIXELFORMAT) bpp = (int)*(const DWORD *)(d + SD_PIXFMT + 12);
	}
	s = new_surface(w, h, bpp, caps | (caps & DDSCAPS_PRIMARYSURFACE ? DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE : 0));
	if (caps & DDSCAPS_PRIMARYSURFACE) {
		primary = s;
		if ((flags & DDSD_BACKBUFFERCOUNT) && *(const DWORD *)(d + SD_BACKBUFFERS) > 0) {
			Surf *b = new_surface(w, h, bpp, DDSCAPS_BACKBUFFER | (caps & (DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_VIDEOMEMORY)));
			s->back = b;
			b->front = s;
		}
	}
	shell_log("ddraw: CreateSurface %dx%d %d bpp caps 0x%x%s%s", w, h, bpp, caps,
	          s == primary ? " (primary)" : "", s->back ? " + back buffer" : "");
	*out = s;
	return DD_OK;
}

static HRESULT WINAPI dd_DuplicateSurface(Obj *o, Surf *s, Surf **out) { (void)o; (void)s; *out = NULL; UNSUPPORTED("DuplicateSurface"); return DDERR_UNSUPPORTED; }

static HRESULT WINAPI dd_EnumDisplayModes(Obj *o, DWORD flags, const uint8_t *filter, void *ctx,
                                          HRESULT (WINAPI *cb)(uint8_t *, void *))
{
	static const int modes[][3] = { { 320, 200, 8 }, { 320, 240, 8 }, { 640, 480, 8 }, { 640, 480, 16 }, { 640, 480, 32 } };
	(void)o; (void)flags; (void)filter;
	for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; i++) {
		uint8_t d[108];
		Surf tmp = { 0 };
		tmp.w = modes[i][0];
		tmp.h = modes[i][1];
		tmp.bpp = modes[i][2];
		tmp.pitch = tmp.w * tmp.bpp / 8;
		*(DWORD *)d = 108;
		fill_desc(d, &tmp, NULL);
		*(DWORD *)(d + SD_FLAGS) &= ~(DWORD)(DDSD_CAPS | DDSD_LPSURFACE);
		*(DWORD *)(d + SD_SURFACE) = 0;
		if (cb(d, ctx) == 0)
			break;
	}
	return DD_OK;
}

static HRESULT WINAPI dd_EnumSurfaces(Obj *o, DWORD f, uint8_t *d, void *c, void *cb) { (void)o; (void)f; (void)d; (void)c; (void)cb; UNSUPPORTED("EnumSurfaces"); return DDERR_UNSUPPORTED; }
static HRESULT WINAPI dd_FlipToGDISurface(Obj *o) { (void)o; return DD_OK; }

static HRESULT WINAPI dd_GetCaps(Obj *o, uint8_t *hal, uint8_t *hel)
{
	uint8_t *caps[2] = { hal, hel };
	(void)o;
	for (int i = 0; i < 2; i++) {
		DWORD size;
		if (!caps[i])
			continue;
		size = *(DWORD *)caps[i];
		if (size < 8 || size > 512)
			size = 316;
		memset(caps[i] + 4, 0, size - 4);
		*(DWORD *)(caps[i] + 4) = 0x40 | 0x04000000;   /* DDCAPS_BLT | DDCAPS_BLTCOLORFILL */
		*(DWORD *)(caps[i] + 60) = 8u << 20;            /* video memory: total, free */
		*(DWORD *)(caps[i] + 64) = 8u << 20;
	}
	return DD_OK;
}

static HRESULT WINAPI dd_GetDisplayMode(Obj *o, uint8_t *d)
{
	Surf tmp = { 0 };
	(void)o;
	tmp.w = mode_w;
	tmp.h = mode_h;
	tmp.bpp = mode_bpp;
	tmp.pitch = mode_w * mode_bpp / 8;
	fill_desc(d, &tmp, NULL);
	return DD_OK;
}

static HRESULT WINAPI dd_GetFourCCCodes(Obj *o, DWORD *n, DWORD *codes) { (void)o; (void)codes; *n = 0; return DD_OK; }
static HRESULT WINAPI dd_GetGDISurface(Obj *o, Surf **s) { (void)o; *s = primary; if (primary) primary->refs++; return primary ? DD_OK : DDERR_NOTFOUND; }
static HRESULT WINAPI dd_GetMonitorFrequency(Obj *o, DWORD *hz) { (void)o; *hz = 60; return DD_OK; }
static HRESULT WINAPI dd_GetScanLine(Obj *o, DWORD *line) { (void)o; *line = 0; return DD_OK; }
static HRESULT WINAPI dd_GetVerticalBlankStatus(Obj *o, BOOL *vb) { (void)o; *vb = TRUE; return DD_OK; }
static HRESULT WINAPI dd_Initialize(Obj *o, void *guid) { (void)o; (void)guid; return DD_OK; }
static HRESULT WINAPI dd_RestoreDisplayMode(Obj *o) { (void)o; return DD_OK; }

static HRESULT WINAPI dd_SetCooperativeLevel(Obj *o, HANDLE h, DWORD flags)
{
	(void)o; (void)h;
	shell_log("ddraw: SetCooperativeLevel 0x%x", flags);
	return DD_OK;
}

static HRESULT set_mode(DWORD w, DWORD h, DWORD bpp)
{
	shell_log("ddraw: SetDisplayMode %ux%u %u bpp", w, h, bpp);
	if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
		return DDERR_INVALIDPARAMS;
	mode_w = (int)w;
	mode_h = (int)h;
	mode_bpp = (int)bpp;
	return DD_OK;
}

static HRESULT WINAPI dd1_SetDisplayMode(Obj *o, DWORD w, DWORD h, DWORD bpp) { (void)o; return set_mode(w, h, bpp); }
static HRESULT WINAPI dd2_SetDisplayMode(Obj *o, DWORD w, DWORD h, DWORD bpp, DWORD hz, DWORD f) { (void)o; (void)hz; (void)f; return set_mode(w, h, bpp); }
static HRESULT WINAPI dd_WaitForVerticalBlank(Obj *o, DWORD f, HANDLE ev) { (void)o; (void)f; (void)ev; return DD_OK; }

static HRESULT WINAPI dd2_GetAvailableVidMem(Obj *o, DWORD *caps, DWORD *total, DWORD *free_)
{
	(void)o; (void)caps;
	if (total) *total = 8u << 20;
	if (free_) *free_ = 8u << 20;
	return DD_OK;
}

#define DD_COMMON dd_QueryInterface, dd_AddRef, dd_Release, dd_Compact, dd_CreateClipper, dd_CreatePalette, \
	dd_CreateSurface, dd_DuplicateSurface, dd_EnumDisplayModes, dd_EnumSurfaces, dd_FlipToGDISurface, \
	dd_GetCaps, dd_GetDisplayMode, dd_GetFourCCCodes, dd_GetGDISurface, dd_GetMonitorFrequency, \
	dd_GetScanLine, dd_GetVerticalBlankStatus, dd_Initialize, dd_RestoreDisplayMode, dd_SetCooperativeLevel

static const void *const dd1_vtbl[] = { DD_COMMON, dd1_SetDisplayMode, dd_WaitForVerticalBlank };
static const void *const dd2_vtbl[] = { DD_COMMON, dd2_SetDisplayMode, dd_WaitForVerticalBlank, dd2_GetAvailableVidMem };

/* ---- exports ------------------------------------------------------------------------- */

static HRESULT WINAPI DirectDrawCreate(void *guid, Obj **out, void *outer)
{
	(void)guid; (void)outer;
	shell_log("DirectDrawCreate");
	dd1.refs++;
	*out = &dd1;
	return DD_OK;
}

static HRESULT WINAPI DirectDrawCreateClipper(DWORD flags, Obj **out, void *outer)
{
	(void)flags; (void)outer;
	*out = new_clipper();
	return DD_OK;
}

static HRESULT WINAPI DirectDrawEnumerateA(BOOL (WINAPI *cb)(void *, const char *, const char *, void *), void *ctx)
{
	cb(NULL, "Primary Display Driver", "display", ctx);
	return DD_OK;
}

const ShimFunc ddraw_funcs[] = {
	{ "DirectDrawCreate", 0, DirectDrawCreate },
	{ "DirectDrawCreateClipper", 0, DirectDrawCreateClipper },
	{ "DirectDrawEnumerateA", 0, DirectDrawEnumerateA },
	{ NULL, 0, NULL },
};
