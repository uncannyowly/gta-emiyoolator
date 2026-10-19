/*
 * Buttons and keys become PC scan codes for user32 (0x100 = E0-prefixed).
 * ini key input = evdev (Miyoo buttons) | sdl2 (desktop keyboard) | none;
 * script = file of timed key events for tests:  <ms> <scancode> <1|0>
 * or "<ms> quit". Miyoo button mapping (ini key = scan code, hex allowed):
 *   up/down/left/right: arrows      a: Ctrl (Attack)     b: Space (Brake)
 *   y: Enter (Enter/Exit vehicle, menu select)           x: Tab (Special)
 *   l1: Z (Previous weapon)   r1: X (Next weapon)        start: Enter
 *   select: Esc   l2: P   r2: Delete   menu+start held: quit the shell
 */
#include "shell.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { I_NONE, I_EVDEV, I_SDL2 };
static int backend;
static int ev_fd = -1;

/* ---- Miyoo buttons ------------------------------------------------------------- */

static const struct { int code; const char *name; int def; } buttons[] = {
	{ KEY_UP, "up", 0x148 }, { KEY_DOWN, "down", 0x150 }, { KEY_LEFT, "left", 0x14B }, { KEY_RIGHT, "right", 0x14D },
	{ KEY_SPACE, "a", 0x1D }, { KEY_LEFTCTRL, "b", 0x39 }, { KEY_LEFTSHIFT, "x", 0x0F }, { KEY_LEFTALT, "y", 0x1C },
	{ KEY_E, "l1", 0x2C }, { KEY_T, "r1", 0x2D }, { KEY_TAB, "l2", 0x19 }, { KEY_BACKSPACE, "r2", 0x153 },
	{ KEY_RIGHTCTRL, "select", 0x01 }, { KEY_ENTER, "start", 0x1C }, { KEY_ESC, "menu", 0x01 },
};
#define NBUTTONS (int)(sizeof buttons / sizeof buttons[0])
static int button_scan[NBUTTONS];
static int menu_held, start_held;

static int evdev_poll(void)
{
	struct input_event ev[16];
	ssize_t n;
	while ((n = read(ev_fd, ev, sizeof ev)) > 0) {
		for (int k = 0; k < (int)(n / (ssize_t)sizeof ev[0]); k++) {
			if (ev[k].type != EV_KEY || ev[k].value == 2)   /* ignore autorepeat */
				continue;
			if (ev[k].code == KEY_ESC)
				menu_held = ev[k].value;
			if (ev[k].code == KEY_ENTER)
				start_held = ev[k].value;
			if (menu_held && start_held)
				return 1;
			for (int b = 0; b < NBUTTONS; b++)
				if (buttons[b].code == ev[k].code && button_scan[b] > 0)
					user32_key_event(button_scan[b], ev[k].value != 0);
		}
	}
	return 0;
}

/* ---- desktop keyboard (SDL2) --------------------------------------------------------- */

extern void *sdl2_lib;
static int (*SDL_PollEvent_)(void *);

/* SDL scancode (USB HID usage) to PC set-1 scan code */
static int hid_to_pc(int s)
{
	static const uint16_t letters[26] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
	                                      0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };
	if (s >= 4 && s <= 29)
		return letters[s - 4];
	if (s >= 30 && s <= 38)
		return 0x02 + (s - 30);   /* 1..9 */
	if (s == 39) return 0x0B;    /* 0 */
	if (s >= 58 && s <= 67)
		return 0x3B + (s - 58);   /* F1..F10 */
	switch (s) {
	case 40: return 0x1C;   /* return */
	case 41: return 0x01;   /* escape */
	case 42: return 0x0E;   /* backspace */
	case 43: return 0x0F;   /* tab */
	case 44: return 0x39;   /* space */
	case 79: return 0x14D;  /* right */
	case 80: return 0x14B;  /* left */
	case 81: return 0x150;  /* down */
	case 82: return 0x148;  /* up */
	case 76: return 0x153;  /* delete */
	case 224: return 0x1D;  /* left ctrl */
	case 225: return 0x2A;  /* left shift */
	case 226: return 0x38;  /* left alt */
	case 228: return 0x11D; /* right ctrl */
	case 229: return 0x36;  /* right shift */
	default: return 0;
	}
}

static int sdl2_poll(void)
{
	uint8_t ev[64];
	while (SDL_PollEvent_(ev)) {
		uint32_t type = *(uint32_t *)ev;
		if (type == 0x100)   /* SDL_QUIT */
			return 1;
		if ((type == 0x300 || type == 0x301) && !ev[13]) {   /* KEYDOWN/KEYUP, not a repeat */
			int pc = hid_to_pc(*(int32_t *)(ev + 16));
			if (pc)
				user32_key_event(pc, type == 0x300);
		}
	}
	return 0;
}

/* ---- scripted input (tests) ----------------------------------------------------------- */

#define MAX_SCRIPT 512
static struct { uint32_t ms; int scan, down; } script[MAX_SCRIPT];
static int script_len, script_pos;

static void script_load(const char *path)
{
	char line[128];
	FILE *f = fopen(path, "r");
	if (!f) {
		shell_log("input: script %s not found", path);
		return;
	}
	while (fgets(line, sizeof line, f) && script_len < MAX_SCRIPT) {
		unsigned ms;
		char a[32];
		int down = 1;
		if (line[0] == '#' || sscanf(line, "%u %31s %d", &ms, a, &down) < 2)
			continue;
		script[script_len].ms = ms;
		script[script_len].scan = strcmp(a, "quit") ? (int)strtol(a, NULL, 16) : -1;
		script[script_len].down = down;
		script_len++;
	}
	fclose(f);
	shell_log("input: %d scripted events from %s", script_len, path);
}

static int script_poll(void)
{
	while (script_pos < script_len && shell_ms() >= script[script_pos].ms) {
		if (script[script_pos].scan < 0)
			return 1;
		user32_key_event(script[script_pos].scan, script[script_pos].down);
		script_pos++;
	}
	return 0;
}

/* ---- interface ------------------------------------------------------------------------ */

void input_init(void)
{
	const char *in = config_str("input", "evdev");
	const char *sc = config_str("script", NULL);
	if (sc && *sc)
		script_load(sc);
	if (!strcmp(in, "evdev")) {
		ev_fd = open(config_str("evdev", "/dev/input/event0"), O_RDONLY | O_NONBLOCK);
		if (ev_fd >= 0)
			backend = I_EVDEV;
		for (int b = 0; b < NBUTTONS; b++)
			button_scan[b] = config_int(buttons[b].name, buttons[b].def);
	} else if (!strcmp(in, "sdl2") && sdl2_lib && (*(void **)&SDL_PollEvent_ = dlsym(sdl2_lib, "SDL_PollEvent"))) {
		backend = I_SDL2;
	}
	shell_log("input: %s", backend == I_EVDEV ? "evdev" : backend == I_SDL2 ? "sdl2" : "none");
}

int input_poll(void)
{
	int quit = script_poll();
	if (backend == I_EVDEV)
		quit |= evdev_poll();
	else if (backend == I_SDL2)
		quit |= sdl2_poll();
	return quit;
}
