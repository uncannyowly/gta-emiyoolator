/*
 * KERNEL32: memory, files, modules, time and the locale functions the exe's
 * statically linked C runtime calls at start-up. The machine reports itself as
 * Windows 98, the platform the game was released for.
 */
#include "shell.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <malloc.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static DWORD last_error;

/* ---- handles -------------------------------------------------------------- */

enum { H_FREE, H_FILE, H_STD, H_MUTEX, H_FIND };
#define MAX_HANDLES 128
static struct { int kind, fd; } handles[MAX_HANDLES];

static HANDLE handle_new(int kind, int fd)
{
	for (int i = 1; i < MAX_HANDLES; i++)
		if (handles[i].kind == H_FREE) {
			handles[i].kind = kind;
			handles[i].fd = fd;
			return (HANDLE)(uintptr_t)(0x100 + 4 * i);
		}
	return INVALID_HANDLE_VALUE;
}

static int handle_index(HANDLE h)
{
	uintptr_t v = (uintptr_t)h;
	if (v < 0x100 || (v - 0x100) % 4 || (v - 0x100) / 4 >= MAX_HANDLES)
		return -1;
	return handles[(v - 0x100) / 4].kind == H_FREE ? -1 : (int)((v - 0x100) / 4);
}

static int handle_fd(HANDLE h)
{
	int i = handle_index(h);
	return i < 0 ? -1 : handles[i].fd;
}

/* ---- memory --------------------------------------------------------------- */

static HANDLE WINAPI k32_HeapCreate(DWORD opts, DWORD initial, DWORD max)
{
	(void)opts; (void)initial; (void)max;
	return (HANDLE)0x00150000;
}

static BOOL WINAPI k32_HeapDestroy(HANDLE h) { (void)h; return TRUE; }

static void *WINAPI k32_HeapAlloc(HANDLE h, DWORD flags, DWORD size)
{
	(void)h;
	return (flags & 8) ? calloc(1, size ? size : 1) : malloc(size ? size : 1);
}

static void *WINAPI k32_HeapReAlloc(HANDLE h, DWORD flags, void *mem, DWORD size)
{
	size_t old = mem ? malloc_usable_size(mem) : 0;
	void *p;
	(void)h;
	if (flags & 0x10)   /* HEAP_REALLOC_IN_PLACE_ONLY */
		return size <= old ? mem : NULL;
	p = realloc(mem, size ? size : 1);
	if (p && (flags & 8) && size > old)
		memset((char *)p + old, 0, size - old);
	return p;
}

static BOOL WINAPI k32_HeapFree(HANDLE h, DWORD flags, void *mem)
{
	(void)h; (void)flags;
	free(mem);
	return TRUE;
}

/* reservations, so MEM_COMMIT inside one and MEM_RELEASE know their extent */
#define MAX_REGIONS 256
static struct { uintptr_t base; size_t size; } regions[MAX_REGIONS];

static void *WINAPI k32_VirtualAlloc(void *addr, DWORD size, DWORD type, DWORD protect)
{
	void *p;
	(void)protect;
	if (addr) {
		uintptr_t a = (uintptr_t)addr;
		for (int i = 0; i < MAX_REGIONS; i++)
			if (regions[i].size && a >= regions[i].base && a < regions[i].base + regions[i].size)
				return (void *)(a & ~(uintptr_t)0xFFF);
		TRACE("VirtualAlloc at %p outside any reservation", addr);
	}
	size = (size + 0xFFFF) & ~0xFFFFu;
	p = mmap(addr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		last_error = 8;
		return NULL;
	}
	for (int i = 0; i < MAX_REGIONS; i++)
		if (!regions[i].size) {
			regions[i].base = (uintptr_t)p;
			regions[i].size = size;
			break;
		}
	TRACE("VirtualAlloc(%p, 0x%x, 0x%x) = %p", addr, size, type, p);
	return p;
}

static BOOL WINAPI k32_VirtualFree(void *addr, DWORD size, DWORD type)
{
	if (type & 0x8000) {   /* MEM_RELEASE */
		for (int i = 0; i < MAX_REGIONS; i++)
			if (regions[i].base == (uintptr_t)addr && regions[i].size) {
				munmap(addr, regions[i].size);
				regions[i].size = 0;
				return TRUE;
			}
		return FALSE;
	}
	if (size)   /* MEM_DECOMMIT: pages read back as zero when committed again */
		madvise(addr, size, MADV_DONTNEED);
	return TRUE;
}

static BOOL WINAPI k32_IsBadPtr(const void *p, DWORD n) { (void)n; return p == NULL; }
static BOOL WINAPI k32_IsBadCodePtr(const void *p) { return p == NULL; }

/* ---- process, modules, versions ------------------------------------------- */

static DWORD WINAPI k32_GetVersion(void) { return 0xC0000A04u; }   /* Windows 98 */

static BOOL WINAPI k32_GetVersionExA(uint32_t *info)
{
	if (info[0] < 148)
		return FALSE;
	info[1] = 4;      /* major */
	info[2] = 10;     /* minor: 98 */
	info[3] = 2222;   /* build */
	info[4] = 1;      /* VER_PLATFORM_WIN32_WINDOWS */
	strcpy((char *)&info[5], " A ");
	return TRUE;
}

static char command_line[512];

static char *WINAPI k32_GetCommandLineA(void)
{
	if (!command_line[0])
		snprintf(command_line, sizeof command_line, "\"%s\"%s%s", files_exe_winpath(),
		         *config_str("args", "") ? " " : "", config_str("args", ""));
	return command_line;
}

static void WINAPI k32_GetStartupInfoA(uint32_t *si)
{
	memset(si, 0, 68);
	si[0] = 68;
}

static HANDLE WINAPI k32_GetModuleHandleA(const char *name)
{
	if (!name)
		return image_base;
	return shim_module(name);
}

static DWORD WINAPI k32_GetModuleFileNameA(HANDLE module, char *buf, DWORD size)
{
	const char *dll = shim_module_name(module);
	char path[300];
	if (!module || module == image_base)
		snprintf(path, sizeof path, "%s", files_exe_winpath());
	else
		snprintf(path, sizeof path, "C:\\WINDOWS\\SYSTEM\\%s", dll ? dll : "unknown.dll");
	snprintf(buf, size, "%s", path);
	return (DWORD)strlen(buf);
}

static HANDLE WINAPI k32_LoadLibraryA(const char *name)
{
	const char *base = strrchr(name, '\\');
	HANDLE h;
	base = base ? base + 1 : name;
	h = shim_module(base);
	shell_log("LoadLibraryA(\"%s\") = %p%s", name, h, h ? "" : " (not provided)");
	if (!h)
		last_error = 126;   /* ERROR_MOD_NOT_FOUND */
	return h;
}

static void *WINAPI k32_GetProcAddress(HANDLE module, const char *name)
{
	const char *dll = shim_module_name(module);
	void *fn = NULL;
	int by_ordinal = (uintptr_t)name < 0x10000;
	if (dll)
		fn = by_ordinal ? shim_lookup(dll, NULL, (int)(uintptr_t)name) : shim_lookup(dll, name, 0);
	if (by_ordinal)
		shell_log("GetProcAddress(%s, #%d) = %p", dll ? dll : "?", (int)(uintptr_t)name, fn);
	else
		shell_log("GetProcAddress(%s, \"%s\") = %p", dll ? dll : "?", name, fn);
	if (!fn)
		last_error = 127;   /* ERROR_PROC_NOT_FOUND */
	return fn;
}

static UINT WINAPI k32_GetSystemDirectoryA(char *buf, UINT size)
{
	snprintf(buf, size, "C:\\WINDOWS\\SYSTEM");
	return (UINT)strlen(buf);
}

static DWORD WINAPI k32_GetLastError(void) { return last_error; }
static HANDLE WINAPI k32_GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }

static void __attribute__((noreturn)) shell_exit(UINT code)
{
	shell_log("exe exited with code %u", code);
	mss32_shutdown();
	video_shutdown();
	_exit((int)code);
}

static void WINAPI k32_ExitProcess(UINT code) { shell_exit(code); }

static BOOL WINAPI k32_TerminateProcess(HANDLE h, UINT code)
{
	(void)h;
	shell_exit(code);
}

static void *top_filter;

static void *WINAPI k32_SetUnhandledExceptionFilter(void *filter)
{
	void *old = top_filter;
	top_filter = filter;
	return old;
}

static LONG WINAPI k32_UnhandledExceptionFilter(void *pointers)
{
	shell_fatal("unhandled Windows exception (%p)", pointers);
}

static void WINAPI k32_RtlUnwind(void *frame, void *ip, void *record, void *value)
{
	shell_fatal("RtlUnwind(%p, %p, %p, %p): exception unwinding is not supported yet", frame, ip, record, value);
}

static HANDLE WINAPI k32_CreateMutexA(void *sa, BOOL owner, const char *name)
{
	(void)sa; (void)owner;
	TRACE("CreateMutexA(%s)", name ? name : "(null)");
	last_error = 0;
	return handle_new(H_MUTEX, -1);
}

static BOOL WINAPI k32_ReleaseMutex(HANDLE h) { (void)h; return TRUE; }

static void WINAPI k32_Sleep(DWORD ms)
{
	if (ms == 0)
		sched_yield();
	else
		usleep(ms * 1000);
}

/* ---- environment ------------------------------------------------------------ */

static char env_a[2];
static WCHAR env_w[2];

static char *WINAPI k32_GetEnvironmentStrings(void) { return env_a; }
static WCHAR *WINAPI k32_GetEnvironmentStringsW(void) { return env_w; }
static BOOL WINAPI k32_FreeEnvironmentStrings(void *p) { (void)p; return TRUE; }

static DWORD WINAPI k32_GetEnvironmentVariableA(const char *name, char *buf, DWORD size)
{
	(void)buf; (void)size;
	TRACE("GetEnvironmentVariableA(%s)", name);
	last_error = 203;   /* ERROR_ENVVAR_NOT_FOUND */
	return 0;
}

static BOOL WINAPI k32_SetEnvironmentVariableA(const char *name, const char *value)
{
	(void)name; (void)value;
	return TRUE;
}

/* ---- files ------------------------------------------------------------------ */

static HANDLE WINAPI k32_GetStdHandle(DWORD which)
{
	static HANDLE std[3];
	int n = which == (DWORD)-10 ? 0 : which == (DWORD)-11 ? 1 : which == (DWORD)-12 ? 2 : -1;
	if (n < 0)
		return INVALID_HANDLE_VALUE;
	if (!std[n])
		std[n] = handle_new(H_STD, n);
	return std[n];
}

static BOOL WINAPI k32_SetStdHandle(DWORD which, HANDLE h) { (void)which; (void)h; return TRUE; }
static UINT WINAPI k32_SetHandleCount(UINT n) { return n; }

static DWORD WINAPI k32_GetFileType(HANDLE h)
{
	int i = handle_index(h);
	if (i < 0)
		return 0;
	return handles[i].kind == H_STD ? 2 : 1;
}

static HANDLE WINAPI k32_CreateFileA(const char *name, DWORD access, DWORD share, void *sa, DWORD creation,
                                     DWORD flags, HANDLE templ)
{
	char host[1024];
	int mode = 0, fd;
	(void)share; (void)sa; (void)flags; (void)templ;

	files_host_path(name, host, sizeof host);
	if ((access & 0x80000000u) && (access & 0x40000000u))
		mode = O_RDWR;
	else if (access & 0x40000000u)
		mode = O_WRONLY;
	else
		mode = O_RDONLY;
	switch (creation) {
	case 1: mode |= O_CREAT | O_EXCL; break;    /* CREATE_NEW */
	case 2: mode |= O_CREAT | O_TRUNC; break;   /* CREATE_ALWAYS */
	case 4: mode |= O_CREAT; break;             /* OPEN_ALWAYS */
	case 5: mode |= O_TRUNC; break;             /* TRUNCATE_EXISTING */
	default: break;                             /* OPEN_EXISTING */
	}
	fd = open(host, mode, 0644);
	shell_log("CreateFileA(\"%s\") -> %s: %s", name, host, fd < 0 ? strerror(errno) : "ok");
	if (fd < 0) {
		last_error = errno == ENOENT ? 2 : errno == EEXIST ? 80 : 5;
		return INVALID_HANDLE_VALUE;
	}
	last_error = 0;
	return handle_new(H_FILE, fd);
}

static BOOL WINAPI k32_ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *ov)
{
	int fd = handle_fd(h);
	ssize_t r;
	(void)ov;
	if (fd < 0)
		return FALSE;
	r = read(fd, buf, n);
	if (got)
		*got = r < 0 ? 0 : (DWORD)r;
	return r >= 0;
}

static BOOL WINAPI k32_WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *put, void *ov)
{
	int fd = handle_fd(h);
	ssize_t r;
	(void)ov;
	if (fd < 0)
		return FALSE;
	r = write(fd, buf, n);
	if (put)
		*put = r < 0 ? 0 : (DWORD)r;
	return r >= 0;
}

static DWORD WINAPI k32_SetFilePointer(HANDLE h, LONG low, LONG *high, DWORD method)
{
	int fd = handle_fd(h);
	off_t r;
	if (fd < 0)
		return 0xFFFFFFFFu;
	r = lseek(fd, low, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
	if (high)
		*high = 0;
	return r < 0 ? 0xFFFFFFFFu : (DWORD)r;
}

static BOOL WINAPI k32_SetEndOfFile(HANDLE h)
{
	int fd = handle_fd(h);
	return fd >= 0 && ftruncate(fd, lseek(fd, 0, SEEK_CUR)) == 0;
}

static BOOL WINAPI k32_FlushFileBuffers(HANDLE h) { (void)h; return TRUE; }

static BOOL WINAPI k32_CloseHandle(HANDLE h)
{
	int i = handle_index(h);
	if (i < 0)
		return FALSE;
	if (handles[i].kind == H_FILE && handles[i].fd >= 0)
		close(handles[i].fd);
	if (handles[i].kind != H_STD)
		handles[i].kind = H_FREE;
	return TRUE;
}

/* WIN32_FIND_DATAA: attributes, three FILETIMEs, size high/low, two reserved, name[260], alt[14] */
static HANDLE WINAPI k32_FindFirstFileA(const char *pattern, uint8_t *data)
{
	char host[1024], dir[1024], *slash, *pat;
	DIR *d;
	struct dirent *e;

	/* system DLLs we provide "exist" (MGL checks for ddraw.dll before loading it) */
	if (!strncasecmp(pattern, "C:\\WINDOWS\\SYSTEM\\", 18) && shim_module(pattern + 18)) {
		memset(data, 0, 320);
		data[0] = 0x20;
		snprintf((char *)data + 44, 260, "%s", pattern + 18);
		shell_log("FindFirstFileA(\"%s\"): provided by gtashell", pattern);
		return handle_new(H_FIND, -1);
	}
	files_host_path(pattern, host, sizeof host);
	snprintf(dir, sizeof dir, "%s", host);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = 0;
		pat = slash + 1;
	} else {
		snprintf(dir, sizeof dir, ".");
		pat = host;
	}
	d = opendir(dir);
	if (d) {
		while ((e = readdir(d)) != NULL) {
			char full[1300];
			struct stat st;
			if (fnmatch(pat, e->d_name, FNM_CASEFOLD) != 0)
				continue;
			snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
			memset(data, 0, 320);
			if (stat(full, &st) == 0) {
				uint32_t attr = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
				uint32_t sz = (uint32_t)st.st_size;
				memcpy(data, &attr, 4);
				memcpy(data + 32, &sz, 4);
			}
			snprintf((char *)data + 44, 260, "%s", e->d_name);
			closedir(d);
			shell_log("FindFirstFileA(\"%s\") found %s", pattern, e->d_name);
			return handle_new(H_FIND, -1);
		}
		closedir(d);
	}
	shell_log("FindFirstFileA(\"%s\"): none", pattern);
	last_error = 2;
	return INVALID_HANDLE_VALUE;
}

static BOOL WINAPI k32_FindClose(HANDLE h) { return k32_CloseHandle(h); }

/* ---- time ------------------------------------------------------------------- */

static void fill_systemtime(WORD *st, int local)
{
	struct timeval tv;
	struct tm tm;
	gettimeofday(&tv, NULL);
	if (local)
		localtime_r(&tv.tv_sec, &tm);
	else
		gmtime_r(&tv.tv_sec, &tm);
	st[0] = (WORD)(tm.tm_year + 1900);
	st[1] = (WORD)(tm.tm_mon + 1);
	st[2] = (WORD)tm.tm_wday;
	st[3] = (WORD)tm.tm_mday;
	st[4] = (WORD)tm.tm_hour;
	st[5] = (WORD)tm.tm_min;
	st[6] = (WORD)tm.tm_sec;
	st[7] = (WORD)(tv.tv_usec / 1000);
}

static void WINAPI k32_GetLocalTime(WORD *st) { fill_systemtime(st, 1); }
static void WINAPI k32_GetSystemTime(WORD *st) { fill_systemtime(st, 0); }

static void WINAPI k32_GetSystemTimeAsFileTime(uint32_t *ft)
{
	struct timeval tv;
	uint64_t t;
	gettimeofday(&tv, NULL);
	t = ((uint64_t)tv.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)tv.tv_usec * 10;
	ft[0] = (uint32_t)t;
	ft[1] = (uint32_t)(t >> 32);
}

static DWORD WINAPI k32_GetTimeZoneInformation(void *tzi)
{
	memset(tzi, 0, 172);
	return 0;   /* TIME_ZONE_ID_UNKNOWN */
}

/* ---- code pages and locale (single-byte Windows-1252, treated as Latin-1) --- */

static UINT WINAPI k32_GetACP(void) { return 1252; }
static UINT WINAPI k32_GetOEMCP(void) { return 437; }

static BOOL WINAPI k32_GetCPInfo(UINT cp, uint8_t *info)
{
	(void)cp;
	memset(info, 0, 20);
	info[0] = 1;     /* MaxCharSize */
	info[4] = '?';   /* DefaultChar */
	return TRUE;
}

static int WINAPI k32_MultiByteToWideChar(UINT cp, DWORD flags, const char *src, int n, WCHAR *dst, int max)
{
	(void)cp; (void)flags;
	if (n < 0)
		n = (int)strlen(src) + 1;
	if (max == 0)
		return n;
	if (n > max)
		n = max;
	for (int i = 0; i < n; i++)
		dst[i] = (unsigned char)src[i];
	return n;
}

static int WINAPI k32_WideCharToMultiByte(UINT cp, DWORD flags, const WCHAR *src, int n, char *dst, int max,
                                          const char *def, BOOL *used)
{
	(void)cp; (void)flags;
	if (used)
		*used = FALSE;
	if (n < 0) {
		n = 0;
		while (src[n])
			n++;
		n++;
	}
	if (max == 0)
		return n;
	if (n > max)
		n = max;
	for (int i = 0; i < n; i++) {
		if (src[i] > 255) {
			dst[i] = def ? *def : '?';
			if (used)
				*used = TRUE;
		} else {
			dst[i] = (char)src[i];
		}
	}
	return n;
}

static WORD ctype1(unsigned c)
{
	WORD t = 0;
	if (c > 127)
		return c >= 0xC0 ? 0x100 | (c < 0xE0 ? 1 : 2) : 0;
	if (isupper(c)) t |= 1;
	if (islower(c)) t |= 2;
	if (isdigit(c)) t |= 4;
	if (isspace(c)) t |= 8;
	if (ispunct(c)) t |= 0x10;
	if (iscntrl(c)) t |= 0x20;
	if (c == ' ' || c == '\t') t |= 0x40;
	if (isxdigit(c)) t |= 0x80;
	if (isalpha(c)) t |= 0x100;
	return t;
}

static BOOL WINAPI k32_GetStringTypeA(DWORD lcid, DWORD type, const char *src, int n, WORD *out)
{
	(void)lcid; (void)type;
	if (n < 0)
		n = (int)strlen(src) + 1;
	for (int i = 0; i < n; i++)
		out[i] = ctype1((unsigned char)src[i]);
	return TRUE;
}

static BOOL WINAPI k32_GetStringTypeW(DWORD type, const WCHAR *src, int n, WORD *out)
{
	(void)type;
	if (n < 0) {
		n = 0;
		while (src[n])
			n++;
		n++;
	}
	for (int i = 0; i < n; i++)
		out[i] = ctype1(src[i] & 0xFF);
	return TRUE;
}

static unsigned map_case(unsigned c, DWORD flags)
{
	if (c < 128 && (flags & 0x100))
		return (unsigned)tolower((int)c);
	if (c < 128 && (flags & 0x200))
		return (unsigned)toupper((int)c);
	return c;
}

static int WINAPI k32_LCMapStringA(DWORD lcid, DWORD flags, const char *src, int n, char *dst, int max)
{
	(void)lcid;
	if (n < 0)
		n = (int)strlen(src) + 1;
	if (max == 0)
		return n;
	if (n > max)
		n = max;
	for (int i = 0; i < n; i++)
		dst[i] = (char)map_case((unsigned char)src[i], flags);
	return n;
}

static int WINAPI k32_LCMapStringW(DWORD lcid, DWORD flags, const WCHAR *src, int n, WCHAR *dst, int max)
{
	(void)lcid;
	if (n < 0) {
		n = 0;
		while (src[n])
			n++;
		n++;
	}
	if (max == 0)
		return n;
	if (n > max)
		n = max;
	for (int i = 0; i < n; i++)
		dst[i] = (WCHAR)map_case(src[i], flags);
	return n;
}

static int compare(const void *a, int na, const void *b, int nb, DWORD flags, int wide)
{
	int i;
	for (i = 0; i < na && i < nb; i++) {
		unsigned x = wide ? ((const WCHAR *)a)[i] : ((const unsigned char *)a)[i];
		unsigned y = wide ? ((const WCHAR *)b)[i] : ((const unsigned char *)b)[i];
		if (flags & 1) {
			x = map_case(x, 0x100);
			y = map_case(y, 0x100);
		}
		if (x != y)
			return x < y ? 1 : 3;
	}
	return na == nb ? 2 : na < nb ? 1 : 3;
}

static int WINAPI k32_CompareStringA(DWORD lcid, DWORD flags, const char *a, int na, const char *b, int nb)
{
	(void)lcid;
	if (na < 0) na = (int)strlen(a);
	if (nb < 0) nb = (int)strlen(b);
	return compare(a, na, b, nb, flags, 0);
}

static int WINAPI k32_CompareStringW(DWORD lcid, DWORD flags, const WCHAR *a, int na, const WCHAR *b, int nb)
{
	(void)lcid;
	if (na < 0) for (na = 0; a[na]; na++) ;
	if (nb < 0) for (nb = 0; b[nb]; nb++) ;
	return compare(a, na, b, nb, flags, 1);
}

const ShimFunc kernel32_funcs[] = {
	{ "HeapCreate", 0, k32_HeapCreate },
	{ "HeapDestroy", 0, k32_HeapDestroy },
	{ "HeapAlloc", 0, k32_HeapAlloc },
	{ "HeapReAlloc", 0, k32_HeapReAlloc },
	{ "HeapFree", 0, k32_HeapFree },
	{ "VirtualAlloc", 0, k32_VirtualAlloc },
	{ "VirtualFree", 0, k32_VirtualFree },
	{ "IsBadWritePtr", 0, k32_IsBadPtr },
	{ "IsBadReadPtr", 0, k32_IsBadPtr },
	{ "IsBadCodePtr", 0, k32_IsBadCodePtr },
	{ "GetVersion", 0, k32_GetVersion },
	{ "GetVersionExA", 0, k32_GetVersionExA },
	{ "GetCommandLineA", 0, k32_GetCommandLineA },
	{ "GetStartupInfoA", 0, k32_GetStartupInfoA },
	{ "GetModuleHandleA", 0, k32_GetModuleHandleA },
	{ "GetModuleFileNameA", 0, k32_GetModuleFileNameA },
	{ "LoadLibraryA", 0, k32_LoadLibraryA },
	{ "GetProcAddress", 0, k32_GetProcAddress },
	{ "GetSystemDirectoryA", 0, k32_GetSystemDirectoryA },
	{ "GetLastError", 0, k32_GetLastError },
	{ "GetCurrentProcess", 0, k32_GetCurrentProcess },
	{ "ExitProcess", 0, k32_ExitProcess },
	{ "TerminateProcess", 0, k32_TerminateProcess },
	{ "SetUnhandledExceptionFilter", 0, k32_SetUnhandledExceptionFilter },
	{ "UnhandledExceptionFilter", 0, k32_UnhandledExceptionFilter },
	{ "RtlUnwind", 0, k32_RtlUnwind },
	{ "CreateMutexA", 0, k32_CreateMutexA },
	{ "ReleaseMutex", 0, k32_ReleaseMutex },
	{ "Sleep", 0, k32_Sleep },
	{ "GetEnvironmentStrings", 0, k32_GetEnvironmentStrings },
	{ "GetEnvironmentStringsW", 0, k32_GetEnvironmentStringsW },
	{ "FreeEnvironmentStringsA", 0, k32_FreeEnvironmentStrings },
	{ "FreeEnvironmentStringsW", 0, k32_FreeEnvironmentStrings },
	{ "GetEnvironmentVariableA", 0, k32_GetEnvironmentVariableA },
	{ "SetEnvironmentVariableA", 0, k32_SetEnvironmentVariableA },
	{ "GetStdHandle", 0, k32_GetStdHandle },
	{ "SetStdHandle", 0, k32_SetStdHandle },
	{ "SetHandleCount", 0, k32_SetHandleCount },
	{ "GetFileType", 0, k32_GetFileType },
	{ "CreateFileA", 0, k32_CreateFileA },
	{ "ReadFile", 0, k32_ReadFile },
	{ "WriteFile", 0, k32_WriteFile },
	{ "SetFilePointer", 0, k32_SetFilePointer },
	{ "SetEndOfFile", 0, k32_SetEndOfFile },
	{ "FlushFileBuffers", 0, k32_FlushFileBuffers },
	{ "CloseHandle", 0, k32_CloseHandle },
	{ "FindFirstFileA", 0, k32_FindFirstFileA },
	{ "FindClose", 0, k32_FindClose },
	{ "GetLocalTime", 0, k32_GetLocalTime },
	{ "GetSystemTime", 0, k32_GetSystemTime },
	{ "GetSystemTimeAsFileTime", 0, k32_GetSystemTimeAsFileTime },
	{ "GetTimeZoneInformation", 0, k32_GetTimeZoneInformation },
	{ "GetACP", 0, k32_GetACP },
	{ "GetOEMCP", 0, k32_GetOEMCP },
	{ "GetCPInfo", 0, k32_GetCPInfo },
	{ "MultiByteToWideChar", 0, k32_MultiByteToWideChar },
	{ "WideCharToMultiByte", 0, k32_WideCharToMultiByte },
	{ "GetStringTypeA", 0, k32_GetStringTypeA },
	{ "GetStringTypeW", 0, k32_GetStringTypeW },
	{ "LCMapStringA", 0, k32_LCMapStringA },
	{ "LCMapStringW", 0, k32_LCMapStringW },
	{ "CompareStringA", 0, k32_CompareStringA },
	{ "CompareStringW", 0, k32_CompareStringW },
	{ NULL, 0, NULL },
};
