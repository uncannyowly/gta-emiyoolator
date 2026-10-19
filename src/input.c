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

/* ---- direct on-foot control -----------------------------------------------------
 * The original walks "tank style": Left/Right turn, Up goes forward. With
 * direct = 1 the D-pad instead gives the direction to walk in (8 ways): the
 * shell reads the player's facing from the game and holds the game's own
 * turn keys until it faces that way, then Forwards. Only on foot in a level;
 * in cars and menus the D-pad keys pass through unchanged.
 *
 * Game memory (WINO exe): player records 444 bytes at 0x74f148, local player
 * index at 0x74f83c; record +2 = ped id, +0xbc = 2 on foot / 0 in a car.
 * Peds are 256 bytes at 0x7284e0; ped +0xa8 = facing (0..1023, 0 south,
 * 256 east; Turn Right decreases it, about 48 per frame). */

int ddraw_in_level(void);

static int pad_scan[4];        /* up, down, left, right as configured */
static int dpad[4];
static uint8_t sent[0x200];    /* key state we have given the game */

static void send_key(int sc, int down)
{
	if (sc <= 0 || sc >= 0x200 || sent[sc] == down)
		return;
	sent[sc] = (uint8_t)down;
	user32_key_event(sc, down);
}

/* DirectInput code as stored in the registry (0x80 = extended) to our scan code */
static int dik_to_scan(int dik)
{
	return dik >= 0x80 ? (dik & 0x7F) | 0x100 : dik;
}

static int control_scan(int n)
{
	static const int defaults[3] = { 0xCB, 0xCD, 0xC8 };   /* turn left, turn right, forwards */
	char key[16];
	snprintf(key, sizeof key, "control%d", n);
	return dik_to_scan(config_int(key, defaults[n]));
}

/* the local player's ped when on foot in a level, else NULL */
static const uint8_t *foot_ped(void)
{
	const uint8_t *player;
	int16_t id;
	uint32_t index = *(const uint32_t *)0x74f83c;
	if (!ddraw_in_level() || index > 3)
		return NULL;
	player = (const uint8_t *)(uintptr_t)(0x74f148 + 444 * index);
	id = *(const int16_t *)(player + 2);
	if (*(const int32_t *)(player + 0xbc) != 2 || id < 0 || id >= 1024)
		return NULL;
	return (const uint8_t *)(uintptr_t)(0x7284e0 + 256 * id);
}

static void steer(void)
{
	const uint8_t *ped = config_int("direct", 1) ? foot_ped() : NULL;
	int left = control_scan(0), right = control_scan(1), fwd = control_scan(2);
	int dx = dpad[3] - dpad[2], dy = dpad[1] - dpad[0];

	if (!ped) {
		/* pass through: what the buttons say, nothing else */
		if (!dpad[2] && left != pad_scan[2]) send_key(left, 0);
		if (!dpad[3] && right != pad_scan[3]) send_key(right, 0);
		if (!dpad[0] && fwd != pad_scan[0]) send_key(fwd, 0);
		for (int i = 0; i < 4; i++)
			send_key(pad_scan[i], dpad[i]);
		return;
	}
	for (int i = 0; i < 4; i++)
		if (pad_scan[i] != left && pad_scan[i] != right && pad_scan[i] != fwd)
			send_key(pad_scan[i], 0);
	if (!dx && !dy) {
		send_key(left, 0);
		send_key(right, 0);
		send_key(fwd, 0);
		return;
	}
	{
		static const int16_t dir[3][3] = {   /* [dy+1][dx+1]: 0 south, 256 east, 512 north, 768 west */
			{ 640, 512, 384 }, { 768, -1, 256 }, { 896, 0, 128 },
		};
		int want = dir[dy + 1][dx + 1];
		int face = *(const int16_t *)(ped + 0xa8) & 1023;
		int diff = ((want - face + 512) & 1023) - 512;   /* -512..511, > 0: turn left */
		int tol = config_int("direct_tolerance", 24);
		send_key(left, diff > tol);
		send_key(right, diff < -tol);
		/* walk once roughly facing the way: no wide arcs, no stalling */
		send_key(fwd, diff >= -128 && diff <= 128);
	}
}

/* every key from a device or script goes through here */
static void pad_key(int sc, int down)
{
	for (int i = 0; i < 4; i++)
		if (sc == pad_scan[i]) {
			dpad[i] = down;
			steer();
			return;
		}
	user32_key_event(sc, down);
}

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
					pad_key(button_scan[b], ev[k].value != 0);
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
				pad_key(pc, type == 0x300);
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
		pad_key(script[script_pos].scan, script[script_pos].down);
		script_pos++;
	}
	return 0;
}

/* ---- interface ------------------------------------------------------------------------ */

void input_init(void)
{
	static const char *const names[4] = { "up", "down", "left", "right" };
	static const int defs[4] = { 0x148, 0x150, 0x14B, 0x14D };
	for (int i = 0; i < 4; i++)
		pad_scan[i] = config_int(names[i], defs[i]);
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

/* debug: watch=1 logs the local player's mode and ped record every second */
static void watch(void)
{
	static uint32_t last;
	const uint8_t *player, *ped;
	int w = config_int("watch", 0);
	if (!w || shell_ms() - last < (w == 2 ? 100u : 1000u))
		return;
	last = shell_ms();
	player = (const uint8_t *)(uintptr_t)(0x74f148 + 444 * *(const uint32_t *)0x74f83c);
	ped = (const uint8_t *)(uintptr_t)(0x7284e0 + 256 * *(const int16_t *)(player + 2));
	if (w == 2) {
		shell_log("watch2: mode %d a8 %d aa %d f0 %d x %d y %d", *(const int32_t *)(player + 0xbc),
		          *(const int16_t *)(ped + 0xa8), *(const int16_t *)(ped + 0xaa), *(const int16_t *)(ped + 0xf0),
		          *(const int32_t *)(ped + 0x90) >> 16, *(const int32_t *)(ped + 0x94) >> 16);
		return;
	}
	shell_log("watch: mode %d pedid %d", *(const int32_t *)(player + 0xbc), *(const int16_t *)(player + 2));
	for (int r = 0; r < 256; r += 64) {
		char line[64 * 3 + 1];
		for (int i = 0; i < 64; i++)
			sprintf(line + 3 * i, "%02x ", ped[r + i]);
		shell_log("watch %02x: %s", r, line);
	}
}

int input_poll(void)
{
	watch();
	steer();
	int quit = script_poll();
	if (backend == I_EVDEV)
		quit |= evdev_poll();
	else if (backend == I_SDL2)
		quit |= sdl2_poll();
	return quit;
}
