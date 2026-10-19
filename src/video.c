/*
 * Showing the game's frames. ini key video =
 *   fbdev  Miyoo /dev/fb0: 32 bpp, double buffered, rotated 180 degrees (the
 *          panel is mounted upside down; rotate=0 to turn that off). The
 *          frame is centred when the panel is larger (Mini v4: 752x560).
 *   sdl2   a desktop window (SDL2 loaded at run time, no link dependency)
 *   none   nothing (with dump_dir: headless test runs)
 * dump_dir/dump_every save every Nth frame as a PPM for tests.
 */
#include "shell.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

enum { V_NONE, V_FBDEV, V_SDL2 };
static int backend;
static uint32_t *line;   /* one converted row, 0x00RRGGBB */
static uint32_t presented;

/* ---- conversion -------------------------------------------------------------- */

static void convert_row(const uint8_t *src, int w, int bpp, const uint32_t *pal, uint32_t *out)
{
	switch (bpp) {
	case 8:
		for (int x = 0; x < w; x++)
			out[x] = pal[src[x]];
		break;
	case 16:
		for (int x = 0; x < w; x++) {
			uint32_t p = ((const uint16_t *)src)[x];
			out[x] = ((p & 0xF800) << 8) | ((p & 0xE000) << 3) | ((p & 0x07E0) << 5) | ((p & 0x0600) >> 1) |
			         ((p & 0x001F) << 3) | ((p & 0x001C) >> 2);
		}
		break;
	case 24:
		for (int x = 0; x < w; x++)
			out[x] = src[3 * x] | (src[3 * x + 1] << 8) | ((uint32_t)src[3 * x + 2] << 16);
		break;
	default:
		for (int x = 0; x < w; x++)
			out[x] = ((const uint32_t *)src)[x] & 0xFFFFFF;
		break;
	}
}

/* ---- PPM dumps ------------------------------------------------------------------- */

static void dump(const void *pixels, int pitch, int w, int h, int bpp, const uint32_t *pal)
{
	const char *dir = config_str("dump_dir", NULL);
	int every = config_int("dump_every", 30);
	char path[512];
	FILE *f;
	if (!dir || !*dir || every <= 0 || presented % (uint32_t)every)
		return;
	snprintf(path, sizeof path, "%s/frame%05u.ppm", dir, presented);
	if (!(f = fopen(path, "wb")))
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int y = 0; y < h; y++) {
		convert_row((const uint8_t *)pixels + y * pitch, w, bpp, pal, line);
		for (int x = 0; x < w; x++) {
			uint8_t rgb[3] = { (uint8_t)(line[x] >> 16), (uint8_t)(line[x] >> 8), (uint8_t)line[x] };
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
}

/* ---- fbdev ------------------------------------------------------------------------- */

static int fb_fd = -1, fb_pages = 1, fb_page, fb_rotate;
static struct fb_var_screeninfo fb_var, fb_orig;
static struct fb_fix_screeninfo fb_fix;
static uint8_t *fb_mem;
static size_t fb_size;

static int fbdev_init(void)
{
	fb_fd = open(config_str("fbdev", "/dev/fb0"), O_RDWR);
	if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var) < 0)
		return -1;
	fb_orig = fb_var;
	fb_var.bits_per_pixel = 32;
	fb_var.yres_virtual = fb_var.yres * 2;
	fb_var.yoffset = 0;
	if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &fb_var) < 0) {
		fb_var = fb_orig;
		ioctl(fb_fd, FBIOPUT_VSCREENINFO, &fb_var);
	}
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var) < 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fb_fix) < 0)
		return -1;
	if (fb_var.bits_per_pixel != 32) {
		shell_log("video: fb depth %u not supported", fb_var.bits_per_pixel);
		return -1;
	}
	fb_size = fb_fix.smem_len;
	fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_mem == MAP_FAILED)
		return -1;
	fb_pages = (fb_var.yres_virtual >= fb_var.yres * 2 && fb_size >= (size_t)fb_fix.line_length * fb_var.yres * 2) ? 2 : 1;
	fb_rotate = config_int("rotate", 180) == 180;
	shell_log("video: fbdev %ux%u, %d page(s), rotate %d", fb_var.xres, fb_var.yres, fb_pages, fb_rotate ? 180 : 0);
	return 0;
}

static void fbdev_present(const uint8_t *pixels, int pitch, int w, int h, int bpp, const uint32_t *pal)
{
	int target = fb_pages == 2 ? fb_page ^ 1 : 0;
	uint8_t *base = fb_mem + (size_t)target * fb_var.yres * fb_fix.line_length;
	int pw = (int)fb_var.xres, ph = (int)fb_var.yres;
	int ox = (pw - w) / 2, oy = (ph - h) / 2;
	if (w > pw || h > ph) {
		w = w > pw ? pw : w;
		h = h > ph ? ph : h;
		ox = ox < 0 ? 0 : ox;
		oy = oy < 0 ? 0 : oy;
	}
	for (int y = 0; y < h; y++) {
		/* panel row for game row y, and where it starts */
		int py = fb_rotate ? ph - 1 - (oy + y) : oy + y;
		uint32_t *dst = (uint32_t *)(base + (size_t)py * fb_fix.line_length);
		convert_row(pixels + y * pitch, w, bpp, pal, line);
		if (fb_rotate) {
			uint32_t *d = dst + (pw - 1 - ox);
			for (int x = 0; x < w; x++)
				*d-- = line[x] | 0xFF000000u;
		} else {
			for (int x = 0; x < w; x++)
				dst[ox + x] = line[x] | 0xFF000000u;
		}
	}
	if (fb_pages == 2) {
		fb_var.yoffset = (unsigned)target * fb_var.yres;
		ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var);
		fb_page = target;
	}
}

/* ---- SDL2 (desktop) ------------------------------------------------------------------ */

void *sdl2_lib;
static void *sdl_window, *sdl_renderer, *sdl_texture;
static int (*SDL_Init_)(uint32_t);
static void *(*SDL_CreateWindow_)(const char *, int, int, int, int, uint32_t);
static void *(*SDL_CreateRenderer_)(void *, int, uint32_t);
static void *(*SDL_CreateTexture_)(void *, uint32_t, int, int, int);
static int (*SDL_UpdateTexture_)(void *, const void *, const void *, int);
static int (*SDL_RenderCopy_)(void *, void *, const void *, const void *);
static void (*SDL_RenderPresent_)(void *);
static void (*SDL_Quit_)(void);

static int sdl2_init(void)
{
	sdl2_lib = dlopen("libSDL2-2.0.so.0", RTLD_NOW);
	if (!sdl2_lib)
		return -1;
#define LOAD(f) if (!(*(void **)&f##_ = dlsym(sdl2_lib, #f))) return -1
	LOAD(SDL_Init); LOAD(SDL_CreateWindow); LOAD(SDL_CreateRenderer); LOAD(SDL_CreateTexture);
	LOAD(SDL_UpdateTexture); LOAD(SDL_RenderCopy); LOAD(SDL_RenderPresent); LOAD(SDL_Quit);
#undef LOAD
	if (SDL_Init_(0x20 | 0x200) != 0)   /* VIDEO | EVENTS */
		return -1;
	sdl_window = SDL_CreateWindow_("GTA-eMiYoolator", 0x2FFF0000, 0x2FFF0000, 640, 480, 0);
	sdl_renderer = sdl_window ? SDL_CreateRenderer_(sdl_window, -1, 0) : NULL;
	sdl_texture = sdl_renderer ? SDL_CreateTexture_(sdl_renderer, 0x16362004, 1, 640, 480) : NULL;   /* ARGB8888, streaming */
	return sdl_texture ? 0 : -1;
}

static void sdl2_present(const uint8_t *pixels, int pitch, int w, int h, int bpp, const uint32_t *pal)
{
	static uint32_t frame[640 * 480];
	if (w > 640) w = 640;
	if (h > 480) h = 480;
	memset(frame, 0, sizeof frame);
	for (int y = 0; y < h; y++)
		convert_row(pixels + y * pitch, w, bpp, pal, frame + y * 640);
	SDL_UpdateTexture_(sdl_texture, NULL, frame, 640 * 4);
	SDL_RenderCopy_(sdl_renderer, sdl_texture, NULL, NULL);
	SDL_RenderPresent_(sdl_renderer);
}

/* ---- interface --------------------------------------------------------------------- */

int video_init(void)
{
	const char *v = config_str("video", "fbdev");
	line = calloc(4096, 4);
	if (!strcmp(v, "fbdev")) {
		if (fbdev_init() != 0) {
			shell_log("video: fbdev failed");
			return -1;
		}
		backend = V_FBDEV;
	} else if (!strcmp(v, "sdl2")) {
		if (sdl2_init() != 0) {
			shell_log("video: SDL2 not available");
			return -1;
		}
		backend = V_SDL2;
	} else {
		backend = V_NONE;
	}
	shell_log("video: %s", v);
	return 0;
}

void video_present(const void *pixels, int pitch, int w, int h, int bpp, const uint32_t *palette)
{
	static uint32_t last_ms, count;
	presented++;
	dump(pixels, pitch, w, h, bpp, palette);
	if (backend == V_FBDEV)
		fbdev_present(pixels, pitch, w, h, bpp, palette);
	else if (backend == V_SDL2)
		sdl2_present(pixels, pitch, w, h, bpp, palette);
	count++;
	if (shell_ms() - last_ms >= 5000) {
		shell_log("video: %.1f fps (%dx%d, %d bpp)", count * 1000.0 / (shell_ms() - last_ms + 1), w, h, bpp);
		last_ms = shell_ms();
		count = 0;
	}
}

void video_shutdown(void)
{
	if (backend == V_FBDEV && fb_mem) {
		memset(fb_mem, 0, fb_size);
		fb_var.yoffset = 0;
		ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var);
		ioctl(fb_fd, FBIOPUT_VSCREENINFO, &fb_orig);
		munmap(fb_mem, fb_size);
		fb_mem = NULL;
	} else if (backend == V_SDL2 && SDL_Quit_) {
		SDL_Quit_();
	}
	backend = V_NONE;
}
