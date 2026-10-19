/*
 * USER32: one full-screen window, its message queue and window procedure,
 * keyboard state, and string resources. Device input arrives as PC scan codes
 * (user32_key_event) and becomes WM_KEYDOWN/WM_KEYUP with the scan code in
 * lParam, which is how the game reads its configured controls.
 */
#include "shell.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define HWND_GAME ((HANDLE)(uintptr_t)0x00010010)
#define MAX_CLASSES 8

static struct { char name[64]; WNDPROC proc; } classes[MAX_CLASSES];
static WNDPROC game_proc;
static int window_made, activated, cursor_count;
static uint8_t key_down[256];   /* by virtual-key code */

/* ---- message queue ---------------------------------------------------------- */

#define QSIZE 256
static MSG queue[QSIZE];
static int qhead, qtail;

static void post(HANDLE hwnd, UINT msg, DWORD wp, LONG lp)
{
	int next = (qtail + 1) % QSIZE;
	if (next == qhead) {
		shell_log("user32: message queue full, dropping 0x%x", msg);
		return;
	}
	queue[qtail].hwnd = hwnd;
	queue[qtail].message = msg;
	queue[qtail].wParam = wp;
	queue[qtail].lParam = lp;
	queue[qtail].time = shell_ms();
	queue[qtail].pt.x = queue[qtail].pt.y = 0;
	qtail = next;
}

static LONG send(UINT msg, DWORD wp, LONG lp)
{
	if (!game_proc)
		return 0;
	TRACE("user32: send 0x%x (%u, %d)", msg, wp, lp);
	return game_proc(HWND_GAME, msg, wp, lp);
}

void user32_quit(void)
{
	post(NULL, WM_QUIT, 0, 0);
}

/* ---- keys ------------------------------------------------------------------- */

/* PC set-1 scan code (0x100 = E0 prefix) to Windows virtual-key code */
static int scan_to_vk(int sc)
{
	static const uint8_t base[0x59] = {
		[0x01] = 0x1B, [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5', [0x07] = '6',
		[0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0', [0x0C] = 0xBD, [0x0D] = 0xBB, [0x0E] = 0x08,
		[0x0F] = 0x09, [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T', [0x15] = 'Y',
		[0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P', [0x1C] = 0x0D, [0x1D] = 0x11, [0x1E] = 'A',
		[0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
		[0x26] = 'L', [0x2A] = 0x10, [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
		[0x31] = 'N', [0x32] = 'M', [0x36] = 0x10, [0x38] = 0x12, [0x39] = 0x20, [0x3B] = 0x70, [0x3C] = 0x71,
		[0x3D] = 0x72, [0x3E] = 0x73, [0x3F] = 0x74, [0x40] = 0x75, [0x41] = 0x76, [0x42] = 0x77, [0x43] = 0x78,
		[0x44] = 0x79, [0x57] = 0x7A, [0x58] = 0x7B,
	};
	switch (sc) {
	case 0x148: return 0x26;   /* up */
	case 0x150: return 0x28;   /* down */
	case 0x14B: return 0x25;   /* left */
	case 0x14D: return 0x27;   /* right */
	case 0x153: return 0x2E;   /* delete */
	case 0x152: return 0x2D;   /* insert */
	case 0x147: return 0x24;   /* home */
	case 0x14F: return 0x23;   /* end */
	case 0x149: return 0x21;   /* page up */
	case 0x151: return 0x22;   /* page down */
	case 0x11C: return 0x0D;   /* keypad enter */
	case 0x11D: return 0x11;   /* right ctrl */
	case 0x138: return 0x12;   /* right alt */
	default: break;
	}
	return (sc >= 0 && sc < 0x59) ? base[sc] : 0;
}

void user32_key_event(int scancode, int down)
{
	int vk = scan_to_vk(scancode);
	int was = vk ? key_down[vk] : 0;
	LONG lp = 1 | ((scancode & 0xFF) << 16) | ((scancode & 0x100) ? 1 << 24 : 0);

	if (vk)
		key_down[vk] = (uint8_t)down;
	if (down)
		lp |= was ? 1 << 30 : 0;
	else
		lp |= (1 << 30) | (1u << 31);
	TRACE("user32: key 0x%x vk 0x%x %s", scancode, vk, down ? "down" : "up");
	post(HWND_GAME, down ? WM_KEYDOWN : WM_KEYUP, (DWORD)vk, lp);
}

static short WINAPI u32_GetKeyState(int vk)
{
	return (vk >= 0 && vk < 256 && key_down[vk]) ? (short)0x8000 : 0;
}

/* ---- window ----------------------------------------------------------------- */

static WORD WINAPI u32_RegisterClassA(const uint32_t *wc)
{
	const char *name = (const char *)(uintptr_t)wc[9];
	for (int i = 0; i < MAX_CLASSES; i++)
		if (!classes[i].proc) {
			snprintf(classes[i].name, sizeof classes[i].name, "%s", (uintptr_t)name > 0xFFFF ? name : "#atom");
			classes[i].proc = (WNDPROC)(uintptr_t)wc[1];
			shell_log("RegisterClassA(\"%s\", proc %p)", classes[i].name, (void *)classes[i].proc);
			return (WORD)(0xC000 + i);
		}
	return 0;
}

static void activate(void)
{
	if (activated || !game_proc)
		return;
	activated = 1;
	send(WM_ACTIVATEAPP, 1, 0);
	send(WM_ACTIVATE, 1, 0);
	send(WM_SETFOCUS, 0, 0);
}

static HANDLE WINAPI u32_CreateWindowExA(DWORD exstyle, const char *cls, const char *title, DWORD style, int x,
                                         int y, int w, int h, HANDLE parent, HANDLE menu, HANDLE inst, void *param)
{
	uint32_t cs[12];
	(void)parent;
	for (int i = 0; i < MAX_CLASSES; i++)
		if (classes[i].proc && ((uintptr_t)cls <= 0xFFFF ? (uintptr_t)cls == 0xC000u + (unsigned)i
		                                                  : !strcasecmp(classes[i].name, cls)))
			game_proc = classes[i].proc;
	shell_log("CreateWindowExA(\"%s\", \"%s\", style 0x%x, %dx%d)", (uintptr_t)cls > 0xFFFF ? cls : "#atom",
	          title ? title : "", style, w, h);
	if (!game_proc)
		return NULL;
	window_made = 1;
	cs[0] = (uint32_t)(uintptr_t)param;
	cs[1] = (uint32_t)(uintptr_t)inst;
	cs[2] = (uint32_t)(uintptr_t)menu;
	cs[3] = 0;
	cs[4] = (uint32_t)h;
	cs[5] = (uint32_t)w;
	cs[6] = (uint32_t)y;
	cs[7] = (uint32_t)x;
	cs[8] = style;
	cs[9] = (uint32_t)(uintptr_t)title;
	cs[10] = (uint32_t)(uintptr_t)cls;
	cs[11] = exstyle;
	if (send(WM_NCCREATE, 0, (LONG)(uintptr_t)cs) == 0)
		TRACE("user32: WM_NCCREATE returned 0");
	if (send(WM_CREATE, 0, (LONG)(uintptr_t)cs) == -1)
		return NULL;
	if (style & 0x10000000u)   /* WS_VISIBLE */
		activate();
	return HWND_GAME;
}

static BOOL WINAPI u32_ShowWindow(HANDLE h, int cmd)
{
	(void)h;
	TRACE("ShowWindow(%d)", cmd);
	if (cmd != 0) {
		send(WM_SHOWWINDOW, 1, 0);
		activate();
	}
	return TRUE;
}

static BOOL WINAPI u32_DestroyWindow(HANDLE h)
{
	(void)h;
	send(WM_DESTROY, 0, 0);
	return TRUE;
}

static LONG WINAPI u32_SetWindowLongA(HANDLE h, int index, LONG value)
{
	LONG old = 0;
	(void)h;
	if (index == -4) {   /* GWL_WNDPROC */
		old = (LONG)(uintptr_t)game_proc;
		game_proc = (WNDPROC)(uintptr_t)value;
		shell_log("SetWindowLongA(GWL_WNDPROC) = %p", (void *)game_proc);
	}
	return old;
}

static LONG WINAPI u32_DefWindowProcA(HANDLE h, UINT msg, DWORD wp, LONG lp)
{
	(void)h; (void)wp; (void)lp;
	switch (msg) {
	case WM_NCCREATE: return 1;
	case WM_CLOSE: user32_quit(); return 0;
	default: return 0;
	}
}

static BOOL WINAPI u32_GetClientRect(HANDLE h, RECT *r)
{
	(void)h;
	r->left = r->top = 0;
	r->right = 640;
	r->bottom = 480;
	return TRUE;
}

static BOOL WINAPI u32_GetWindowPlacement(HANDLE h, uint32_t *wp)
{
	(void)h;
	memset(wp, 0, 44);
	wp[0] = 44;
	wp[2] = 1;   /* SW_SHOWNORMAL */
	wp[9] = 640;
	wp[10] = 480;
	return TRUE;
}

static BOOL WINAPI u32_SetWindowPlacement(HANDLE h, const void *wp) { (void)h; (void)wp; return TRUE; }

static BOOL WINAPI u32_SetWindowPos(HANDLE h, HANDLE after, int x, int y, int cx, int cy, UINT flags)
{
	(void)h; (void)after; (void)x; (void)y; (void)cx; (void)cy; (void)flags;
	return TRUE;
}

static HANDLE WINAPI u32_SetActiveWindow(HANDLE h) { activate(); return h; }
static BOOL WINAPI u32_SetForegroundWindow(HANDLE h) { (void)h; activate(); return TRUE; }
static HANDLE WINAPI u32_SetFocus(HANDLE h) { return h; }
static HANDLE WINAPI u32_GetActiveWindow(void) { return window_made ? HWND_GAME : NULL; }
static BOOL WINAPI u32_EnableWindow(HANDLE h, BOOL on) { (void)h; (void)on; return FALSE; }

static HANDLE WINAPI u32_GetDC(HANDLE h) { (void)h; return (HANDLE)(uintptr_t)0x00020020; }
static int WINAPI u32_ReleaseDC(HANDLE h, HANDLE dc) { (void)h; (void)dc; return 1; }

static HANDLE WINAPI u32_SetCursor(HANDLE c) { (void)c; return NULL; }
static HANDLE WINAPI u32_LoadCursorA(HANDLE inst, const char *name) { (void)inst; (void)name; return (HANDLE)(uintptr_t)0x00030030; }
static HANDLE WINAPI u32_LoadIconA(HANDLE inst, const char *name) { (void)inst; (void)name; return (HANDLE)(uintptr_t)0x00030040; }
static int WINAPI u32_ShowCursor(BOOL show) { return cursor_count += show ? 1 : -1; }
static BOOL WINAPI u32_SetCursorPos(int x, int y) { (void)x; (void)y; return TRUE; }

/* ---- messages ----------------------------------------------------------------- */

static BOOL WINAPI u32_PostMessageA(HANDLE h, UINT msg, DWORD wp, LONG lp)
{
	TRACE("PostMessageA(0x%x)", msg);
	post(h, msg, wp, lp);
	return TRUE;
}

static BOOL WINAPI u32_PeekMessageA(MSG *m, HANDLE h, UINT min, UINT max, UINT remove)
{
	(void)h;
	if (input_poll())
		user32_quit();
	for (int i = qhead; i != qtail; i = (i + 1) % QSIZE) {
		if ((min || max) && (queue[i].message < min || queue[i].message > max))
			continue;
		*m = queue[i];
		if (remove & 1) {
			/* remove entry i, keeping order */
			for (int j = i; j != qhead; j = (j - 1 + QSIZE) % QSIZE)
				queue[j] = queue[(j - 1 + QSIZE) % QSIZE];
			qhead = (qhead + 1) % QSIZE;
		}
		return TRUE;
	}
	return FALSE;
}

static BOOL WINAPI u32_TranslateMessage(const MSG *m)
{
	int vk = (int)m->wParam, ch = 0;
	if (m->message != WM_KEYDOWN)
		return FALSE;
	if ((vk >= '0' && vk <= '9') || vk == ' ')
		ch = vk;
	else if (vk >= 'A' && vk <= 'Z')
		ch = key_down[0x10] ? vk : vk + 32;
	else if (vk == 0x0D || vk == 0x08 || vk == 0x1B || vk == 0x09)
		ch = vk;
	if (ch)
		post(m->hwnd, WM_CHAR, (DWORD)ch, m->lParam);
	return ch != 0;
}

static LONG WINAPI u32_DispatchMessageA(const MSG *m)
{
	if (m->message == WM_QUIT || !game_proc)
		return 0;
	return game_proc(m->hwnd ? m->hwnd : HWND_GAME, m->message, m->wParam, m->lParam);
}

/* ---- resources and dialogs ------------------------------------------------- */

static int WINAPI u32_LoadStringA(HANDLE inst, UINT id, char *buf, int max)
{
	uint32_t size;
	const WORD *p = pe_resource(6, (int)(id >> 4) + 1, &size);   /* RT_STRING, blocks of 16 */
	int n = 0;
	(void)inst;
	if (max <= 0)
		return 0;
	buf[0] = 0;
	if (!p)
		return 0;
	for (UINT i = 0; i < (id & 15); i++)
		p += 1 + *p;
	for (n = 0; n < *p && n < max - 1; n++)
		buf[n] = (char)p[1 + n];
	buf[n] = 0;
	return n;
}

static int WINAPI u32_MessageBoxA(HANDLE h, const char *text, const char *caption, UINT type)
{
	(void)h;
	shell_log("MessageBoxA(\"%s\": \"%s\", type 0x%x)", caption ? caption : "", text ? text : "", type);
	return 1;   /* IDOK */
}

const ShimFunc user32_funcs[] = {
	{ "RegisterClassA", 0, u32_RegisterClassA },
	{ "CreateWindowExA", 0, u32_CreateWindowExA },
	{ "ShowWindow", 0, u32_ShowWindow },
	{ "DestroyWindow", 0, u32_DestroyWindow },
	{ "SetWindowLongA", 0, u32_SetWindowLongA },
	{ "DefWindowProcA", 0, u32_DefWindowProcA },
	{ "GetClientRect", 0, u32_GetClientRect },
	{ "GetWindowPlacement", 0, u32_GetWindowPlacement },
	{ "SetWindowPlacement", 0, u32_SetWindowPlacement },
	{ "SetWindowPos", 0, u32_SetWindowPos },
	{ "SetActiveWindow", 0, u32_SetActiveWindow },
	{ "SetForegroundWindow", 0, u32_SetForegroundWindow },
	{ "SetFocus", 0, u32_SetFocus },
	{ "GetActiveWindow", 0, u32_GetActiveWindow },
	{ "EnableWindow", 0, u32_EnableWindow },
	{ "GetDC", 0, u32_GetDC },
	{ "ReleaseDC", 0, u32_ReleaseDC },
	{ "SetCursor", 0, u32_SetCursor },
	{ "LoadCursorA", 0, u32_LoadCursorA },
	{ "LoadIconA", 0, u32_LoadIconA },
	{ "ShowCursor", 0, u32_ShowCursor },
	{ "SetCursorPos", 0, u32_SetCursorPos },
	{ "GetKeyState", 0, u32_GetKeyState },
	{ "PostMessageA", 0, u32_PostMessageA },
	{ "PeekMessageA", 0, u32_PeekMessageA },
	{ "TranslateMessage", 0, u32_TranslateMessage },
	{ "DispatchMessageA", 0, u32_DispatchMessageA },
	{ "LoadStringA", 0, u32_LoadStringA },
	{ "MessageBoxA", 0, u32_MessageBoxA },
	{ NULL, 0, NULL },
};
