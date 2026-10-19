/*
 * Start-up: configuration, the Windows thread environment block (FS), the
 * x87 control word, then the exe's own entry point (WinMainCRTStartup).
 *
 *   gtashell [path/to/gtashell.ini]
 */
#include "shell.h"

#include <asm/ldt.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

int shell_trace;
static FILE *log_file;

void shell_log(const char *fmt, ...)
{
	va_list ap;
	FILE *f = log_file ? log_file : stderr;
	va_start(ap, fmt);
	fprintf(f, "[%7.3f] ", shell_ms() / 1000.0);
	vfprintf(f, fmt, ap);
	fputc('\n', f);
	fflush(f);
	va_end(ap);
}

void shell_fatal(const char *fmt, ...)
{
	va_list ap;
	FILE *f = log_file ? log_file : stderr;
	va_start(ap, fmt);
	fputs("FATAL: ", f);
	vfprintf(f, fmt, ap);
	fputc('\n', f);
	fflush(f);
	va_end(ap);
	video_shutdown();
	_exit(1);
}

uint32_t shell_ms(void)
{
	static struct timespec t0;
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	if (t0.tv_sec == 0 && t0.tv_nsec == 0)
		t0 = t;
	return (uint32_t)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
}

/* ---- ini: key = value lines, '#' or ';' comments -------------------------- */

#define MAX_KEYS 128
static char keys[MAX_KEYS][48], vals[MAX_KEYS][256];
static int nkeys;

static void trim(char *s)
{
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	while (isspace((unsigned char)*s))
		memmove(s, s + 1, strlen(s));
}

void config_load(const char *path)
{
	char line[512];
	FILE *f = fopen(path, "r");
	if (!f) {
		shell_log("config: %s not found, using defaults", path);
		return;
	}
	while (fgets(line, sizeof line, f) && nkeys < MAX_KEYS) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || line[0] == ';' || line[0] == '[' || !eq)
			continue;
		*eq = 0;
		trim(line);
		trim(eq + 1);
		snprintf(keys[nkeys], sizeof keys[0], "%s", line);
		snprintf(vals[nkeys], sizeof vals[0], "%s", eq + 1);
		nkeys++;
	}
	fclose(f);
}

const char *config_str(const char *key, const char *def)
{
	for (int i = nkeys - 1; i >= 0; i--)
		if (!strcasecmp(keys[i], key))
			return vals[i];
	return def;
}

int config_int(const char *key, int def)
{
	const char *v = config_str(key, NULL);
	return v ? (int)strtol(v, NULL, 0) : def;
}

/* ---- Windows thread environment block ------------------------------------ */

/* Windows code reaches its TEB through FS: fs:[0] is the SEH chain head,
 * fs:[0x18] the TEB's own address, fs:[0x30] the PEB. */
static uint8_t teb[0x1000] __attribute__((aligned(4096)));
static uint8_t peb[0x1000] __attribute__((aligned(4096)));

static void setup_fs(void)
{
	struct user_desc ud;
	uint16_t sel;
	uint32_t *t = (uint32_t *)teb;
	uintptr_t sp = (uintptr_t)__builtin_frame_address(0);

	t[0] = 0xFFFFFFFFu;             /* ExceptionList: end of chain */
	t[1] = (uint32_t)(sp + 0x10000); /* StackBase (approximate) */
	t[2] = (uint32_t)(sp - 0x100000);
	t[0x18 / 4] = (uint32_t)(uintptr_t)teb;
	t[0x20 / 4] = (uint32_t)getpid();
	t[0x24 / 4] = (uint32_t)getpid();
	t[0x30 / 4] = (uint32_t)(uintptr_t)peb;

	memset(&ud, 0, sizeof ud);
	ud.entry_number = 0;
	ud.base_addr = (unsigned)(uintptr_t)teb;
	ud.limit = 0xfff;
	ud.seg_32bit = 1;
	ud.useable = 1;
	if (syscall(SYS_modify_ldt, 1, &ud, sizeof ud) != 0)
		shell_fatal("modify_ldt failed: cannot give the exe its FS segment");
	sel = (uint16_t)((ud.entry_number << 3) | 7);   /* LDT, ring 3 */
	__asm__ volatile("movw %0, %%fs" : : "r"(sel));
	shell_log("fs: TEB at %p, selector 0x%x", (void *)teb, sel);
}

int pe_load(const char *path);
uint32_t pe_entry(void);

int main(int argc, char **argv)
{
	const char *ini = argc > 1 ? argv[1] : "gtashell.ini";
	const char *logpath;
	uint32_t entry;
	uint16_t cw = 0x027F;   /* Windows default: 53-bit precision, all exceptions masked */

	shell_ms();
	config_load(ini);
	logpath = config_str("log", NULL);
	if (logpath && *logpath)
		log_file = fopen(logpath, "w");
	shell_trace = config_int("trace", 0);
	shell_log("gtashell starting, config %s", ini);

	if (pe_load(config_str("exe", "WINO/Grand Theft Auto.exe")) != 0)
		shell_fatal("cannot load the exe");
	if (video_init() != 0)
		shell_fatal("no video output");
	input_init();
	setup_fs();

	entry = pe_entry();
	shell_log("calling the exe entry point at 0x%08x", entry);
	__asm__ volatile("fninit\n\tfldcw %0" : : "m"(cw));
	((void (WINAPI *)(void))(uintptr_t)entry)();
	shell_log("entry point returned");
	video_shutdown();
	return 0;
}
