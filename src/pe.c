/*
 * PE loader for the one exe we run: maps it at its image base (it has no
 * relocations), points every import at our implementation, and serves its
 * resources. An import we do not provide gets a stub that names itself when
 * called, so missing pieces show up in the log instead of as a crash.
 */
#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>

uint8_t *image_base;
static uint32_t entry_rva, rsrc_rva, rsrc_size;

static const ShimDll dlls[] = {
	{ "kernel32.dll", kernel32_funcs },
	{ "user32.dll", user32_funcs },
	{ "gdi32.dll", gdi32_funcs },
	{ "advapi32.dll", advapi32_funcs },
	{ "winmm.dll", winmm_funcs },
	{ "mss32.dll", mss32_funcs },
	{ "smackw32.dll", smackw32_funcs },
	{ "dplayx.dll", dplayx_funcs },
	{ "ddraw.dll", ddraw_funcs },
	{ "wdir32.dll", wdir32_funcs },
};
#define NDLLS (int)(sizeof(dlls) / sizeof(dlls[0]))

static int dll_index(const char *dll)
{
	char name[64];
	snprintf(name, sizeof name, "%s", dll);
	if (!strchr(name, '.'))
		strncat(name, ".dll", sizeof name - strlen(name) - 1);
	for (int i = 0; i < NDLLS; i++)
		if (!strcasecmp(dlls[i].dll, name))
			return i;
	return -1;
}

void *shim_lookup(const char *dll, const char *name, int ordinal)
{
	int d = dll_index(dll);
	if (d < 0)
		return NULL;
	for (const ShimFunc *f = dlls[d].funcs; f->fn; f++)
		if (name ? (f->name && !strcmp(f->name, name)) : (f->ordinal == ordinal))
			return f->fn;
	return NULL;
}

/* fake module handles: small odd numbers that cannot be real pointers */
HANDLE shim_module(const char *dll)
{
	int d = dll_index(dll);
	return d < 0 ? NULL : (HANDLE)(uintptr_t)(0x10000001u + (unsigned)d * 0x10000u);
}

const char *shim_module_name(HANDLE module)
{
	uintptr_t m = (uintptr_t)module;
	if (m >= 0x10000001u && (m - 0x10000001u) % 0x10000u == 0 && (m - 0x10000001u) / 0x10000u < (unsigned)NDLLS)
		return dlls[(m - 0x10000001u) / 0x10000u].dll;
	return NULL;
}

/* ---- stubs for imports we do not provide ---------------------------------- */

#define MAX_MISSING 256
static char missing_names[MAX_MISSING][80];
static int nmissing;

static void __attribute__((used, cdecl)) missing_called(int index)
{
	shell_fatal("the exe called %s, which gtashell does not provide yet", missing_names[index]);
}

static void patch_exe(void);

static void *missing_stub(const char *dll, const char *name, int ordinal)
{
	static uint8_t *code;
	static int used;
	uint8_t *p;
	int32_t rel;

	if (nmissing >= MAX_MISSING)
		shell_fatal("too many missing imports");
	if (!code) {
		code = mmap(NULL, 65536, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (code == MAP_FAILED)
			shell_fatal("mmap for import stubs failed");
	}
	if (name)
		snprintf(missing_names[nmissing], sizeof missing_names[0], "%s!%s", dll, name);
	else
		snprintf(missing_names[nmissing], sizeof missing_names[0], "%s!#%d", dll, ordinal);
	p = code + used;
	used += 16;
	p[0] = 0x68;                              /* push index */
	memcpy(p + 1, &nmissing, 4);
	p[5] = 0xE8;                              /* call missing_called (never returns) */
	rel = (int32_t)((uintptr_t)missing_called - (uintptr_t)(p + 10));
	memcpy(p + 6, &rel, 4);
	nmissing++;
	return p;
}

/* ---- loading -------------------------------------------------------------- */

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

int pe_load(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t *file;
	long size;
	uint32_t pe, opt, nsec, image_size, header_size, imp_rva;
	int resolved = 0, stubbed = 0;

	if (!f) {
		shell_log("pe: cannot open %s", path);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	file = malloc(size);
	if (!file || fread(file, 1, size, f) != (size_t)size) {
		fclose(f);
		return -1;
	}
	fclose(f);

	pe = rd32(file + 0x3c);
	if (memcmp(file + pe, "PE\0\0", 4) != 0 || rd16(file + pe + 4) != 0x14c)
		shell_fatal("%s is not a 32-bit x86 PE file", path);
	nsec = rd16(file + pe + 6);
	opt = pe + 24;
	if (rd32(file + opt + 28) != IMAGE_BASE)
		shell_fatal("unexpected image base 0x%x", rd32(file + opt + 28));
	entry_rva = rd32(file + opt + 16);
	image_size = rd32(file + opt + 56);
	header_size = rd32(file + opt + 60);
	imp_rva = rd32(file + opt + 96 + 8 * 1);
	rsrc_rva = rd32(file + opt + 96 + 8 * 2);
	rsrc_size = rd32(file + opt + 96 + 8 * 2 + 4);

	/* a hint, checked below: the Mini's 4.9 kernel predates MAP_FIXED_NOREPLACE */
	image_base = mmap((void *)(uintptr_t)IMAGE_BASE, image_size, PROT_READ | PROT_WRITE | PROT_EXEC,
	                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (image_base == MAP_FAILED || image_base != (uint8_t *)(uintptr_t)IMAGE_BASE)
		shell_fatal("cannot map the exe at 0x%x (address in use?)", IMAGE_BASE);
	memcpy(image_base, file, header_size);
	for (uint32_t i = 0; i < nsec; i++) {
		const uint8_t *s = file + opt + rd16(file + pe + 20) + i * 40;
		uint32_t va = rd32(s + 12), raw_size = rd32(s + 16), raw = rd32(s + 20);
		if (raw_size)
			memcpy(image_base + va, file + raw, raw_size);
		shell_log("pe: section %-8.8s at 0x%08x, %u bytes", (const char *)s, IMAGE_BASE + va, rd32(s + 8));
	}
	free(file);

	/* imports */
	for (const uint8_t *d = image_base + imp_rva; rd32(d + 12); d += 20) {
		const char *dll = (const char *)image_base + rd32(d + 12);
		uint32_t ilt = rd32(d) ? rd32(d) : rd32(d + 16);
		uint32_t *iat = (uint32_t *)(image_base + rd32(d + 16));
		for (const uint32_t *t = (const uint32_t *)(image_base + ilt); *t; t++, iat++) {
			const char *name = NULL;
			int ordinal = 0;
			void *fn;
			if (*t & 0x80000000u)
				ordinal = (int)(*t & 0xFFFF);
			else
				name = (const char *)image_base + *t + 2;
			fn = shim_lookup(dll, name, ordinal);
			if (fn)
				resolved++;
			else {
				fn = missing_stub(dll, name, ordinal);
				stubbed++;
				TRACE("pe: no %s!%s%d", dll, name ? name : "#", name ? 0 : ordinal);
			}
			*iat = (uint32_t)(uintptr_t)fn;
		}
	}
	shell_log("pe: %s loaded, %d imports provided, %d not yet", path, resolved, stubbed);
	patch_exe();
	return 0;
}

/* ---- patches --------------------------------------------------------------------- */

/* The frame limiter (0x47dc80) spins "cmp [0x775580], eax; jb" until the 70 Hz
 * timer has counted eax ticks, burning a whole core the Mini needs for sound.
 * The loop becomes a call to wait_ticks, which sleeps in small steps instead;
 * applied only when the bytes are exactly the expected ones. */
static void __attribute__((used, cdecl)) wait_ticks(uint32_t target)
{
	struct timespec ts = { 0, 500000 };
	while (*(volatile uint32_t *)(uintptr_t)0x775580 < target)
		nanosleep(&ts, NULL);
}

__asm__(".text\n"
        "wait_ticks_stub:\n"
        "\tpush %eax\n\tpush %ecx\n\tpush %edx\n"
        "\tpush %eax\n\tcall wait_ticks\n\tadd $4, %esp\n"
        "\tpop %edx\n\tpop %ecx\n\tpop %eax\n\tret\n");
extern char wait_ticks_stub[];

static void patch_exe(void)
{
	static const uint8_t spin[8] = { 0x39, 0x05, 0x80, 0x55, 0x77, 0x00, 0x72, 0xF8 };
	uint8_t *p = image_base + (0x47dc96 - IMAGE_BASE);
	int32_t rel;
	if (!config_int("patch_spin", 1))
		return;
	if (memcmp(p, spin, sizeof spin) != 0) {
		shell_log("pe: frame limiter not where expected, left as it is");
		return;
	}
	rel = (int32_t)((uintptr_t)wait_ticks_stub - (uintptr_t)(p + 5));
	p[0] = 0xE8;
	memcpy(p + 1, &rel, 4);
	p[5] = p[6] = p[7] = 0x90;
	shell_log("pe: frame limiter sleeps instead of spinning");
}

uint32_t pe_entry(void)
{
	return IMAGE_BASE + entry_rva;
}

/* ---- resources ------------------------------------------------------------ */

/* entry in a resource directory with this integer id, or the first one when id < 0 */
static const uint8_t *res_find(const uint8_t *dir, int id)
{
	const uint8_t *root = image_base + rsrc_rva;
	int named = rd16(dir + 12), ids = rd16(dir + 14);
	const uint8_t *e = dir + 16;
	for (int i = 0; i < named + ids; i++, e += 8) {
		uint32_t nm = rd32(e), off = rd32(e + 4);
		if (id < 0 || (!(nm & 0x80000000u) && (int)nm == id))
			return root + (off & 0x7FFFFFFFu);   /* subdirectory or data entry */
	}
	return NULL;
}

const void *pe_resource(int type, int id, uint32_t *size)
{
	const uint8_t *root, *d;
	if (!rsrc_rva)
		return NULL;
	root = image_base + rsrc_rva;
	if (!(d = res_find(root, type)) || !(d = res_find(d, id)) || !(d = res_find(d, -1)))
		return NULL;
	if (size)
		*size = rd32(d + 4);
	return image_base + rd32(d);
}
