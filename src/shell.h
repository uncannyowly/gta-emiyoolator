/*
 * gtashell: runs the original Windows "Grand Theft Auto.exe" (GTA1) by
 * loading it at its image base and supplying, natively, the Windows functions
 * it imports. Built as 32-bit x86 Linux code: it runs directly on a PC and
 * under box86 on the Miyoo Mini.
 */
#ifndef SHELL_H
#define SHELL_H

#include <stddef.h>
#include <stdint.h>

/* ---- Windows types ------------------------------------------------------- */

#define WINAPI __attribute__((stdcall))

typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef int BOOL;
typedef unsigned int UINT;
typedef void *HANDLE;
typedef uint32_t HRESULT;
typedef uint16_t WCHAR;

#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef struct {
	HANDLE hwnd;
	UINT message;
	DWORD wParam;
	LONG lParam;
	DWORD time;
	POINT pt;
} MSG;

typedef LONG (WINAPI *WNDPROC)(HANDLE, UINT, DWORD, LONG);

/* window messages used */
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_NCCREATE 0x0081
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_TIMER 0x0113

/* ---- shell services ------------------------------------------------------ */

void shell_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void shell_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
/* verbose call tracing, enabled with trace=1 in the ini */
extern int shell_trace;
#define TRACE(...) do { if (shell_trace) shell_log(__VA_ARGS__); } while (0)

/* a function the exe may import: by name, or by ordinal when name is NULL */
typedef struct {
	const char *name;
	int ordinal;
	void *fn;
} ShimFunc;

typedef struct {
	const char *dll;
	const ShimFunc *funcs;
} ShimDll;

extern const ShimFunc kernel32_funcs[], user32_funcs[], gdi32_funcs[], advapi32_funcs[],
	winmm_funcs[], mss32_funcs[], smackw32_funcs[], dplayx_funcs[], ddraw_funcs[], wdir32_funcs[];

/* finds a shim by DLL and name (or ordinal); NULL when not provided */
void *shim_lookup(const char *dll, const char *name, int ordinal);
/* a fake module handle for a DLL we provide, NULL for others */
HANDLE shim_module(const char *dll);
const char *shim_module_name(HANDLE module);

/* ---- configuration (gtashell.ini) ---------------------------------------- */

const char *config_str(const char *key, const char *def);
int config_int(const char *key, int def);
void config_load(const char *path);

/* ---- the loaded exe ------------------------------------------------------ */

#define IMAGE_BASE 0x400000u
extern uint8_t *image_base;
/* resource lookup in the exe: type/name ids (strings not supported) */
const void *pe_resource(int type, int id, uint32_t *size);

/* ---- files: Windows paths onto the host --------------------------------- */

/* maps a Windows path to a host path (case-insensitive lookup); returns out */
char *files_host_path(const char *winpath, char *out, size_t outlen);
const char *files_exe_winpath(void);

/* ---- video / input / time ------------------------------------------------ */

int video_init(void);
/* shows a w x h frame: bpp 8 (with a 256-entry 0x00RRGGBB palette) or 16 (RGB565) or 32 */
void video_present(const void *pixels, int pitch, int w, int h, int bpp, const uint32_t *palette);
void video_shutdown(void);

void mapview_load(const char *path);
int mapview_active(void);
void mapview_toggle(void);
void mapview_close(void);
const uint32_t *mapview_frame(void);

void input_init(void);
/* reads pending device events and posts key messages; returns 1 on quit */
int input_poll(void);

uint32_t shell_ms(void);

/* user32: key events from input backends (PC set-1 scan code, 0x100 = E0 prefix) */
void user32_key_event(int scancode, int down);
void user32_quit(void);

/* mss32: timer thread control */
void mss32_shutdown(void);

#endif
