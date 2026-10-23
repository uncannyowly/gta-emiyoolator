/* Test physical button combinations and transitions with synthetic game memory. */
#include <assert.h>
#include <sys/mman.h>
#include "../src/input.c"

void *sdl2_lib;
static int in_level = 1, car_buttons = 1, overview, map_toggles, esc_presses, pause_presses;
static uint8_t keys[512];
int ddraw_in_level(void) { return in_level; }
int config_int(const char *k, int d) { return !strcmp(k, "car_buttons") ? car_buttons : d; }
const char *config_str(const char *k, const char *d) { return d; }
uint32_t shell_ms(void) { return 0; }
void shell_log(const char *fmt, ...) {}
void user32_key_event(int sc, int down) {
 keys[sc] = down;
 if (down && sc == 1) esc_presses++;
 if (down && sc == 0x40) pause_presses++;
}
int mapview_active(void) { return overview; }
void mapview_toggle(void) { if (in_level) { overview = !overview; map_toggles++; } }
void mapview_close(void) { overview = 0; }

static void button(int code, int down)
{
	device_key(code, down);
	steer();
}

int main(void)
{
	assert(mmap((void *)0x700000, 0x100000, PROT_READ | PROT_WRITE,
	            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED);
	input_init();
	for (int b = 0; b < NBUTTONS; b++) button_scan[b] = buttons[b].def;
	int32_t *mode = (int32_t *)(0x74f148 + 0xbc);
	*mode = 2;
	button(KEY_SPACE, 1); assert(keys[0x1d]); /* A attacks on foot */
	*mode = 0; steer(); assert(!keys[0x1d] && keys[0x148]); /* held A becomes throttle */
	button(KEY_UP, 1); button(KEY_SPACE, 0); assert(keys[0x148]);
	button(KEY_SPACE, 1); button(KEY_UP, 0); assert(keys[0x148]);
	button(KEY_SPACE, 0); assert(!keys[0x148]);
	button(KEY_LEFTCTRL, 1); assert(keys[0x150] && !keys[0x39]);
	button(KEY_LEFTCTRL, 0);
	button(KEY_LEFTSHIFT, 1); assert(keys[0x39] && !keys[0xf]);
	button(KEY_LEFTSHIFT, 0);
	button(KEY_TAB, 1); assert(keys[0xf]); button(KEY_TAB, 0);
	button(KEY_ENTER, 1); button(KEY_ENTER, 0); assert(pause_presses == 1);
	button(KEY_ESC, 1); assert(esc_presses == 0);
	button(KEY_RIGHTCTRL, 1); assert(esc_presses == 1 && map_toggles == 0);
	button(KEY_ESC, 0); button(KEY_RIGHTCTRL, 0); assert(map_toggles == 0);
	button(KEY_RIGHTCTRL, 1); button(KEY_ESC, 1); assert(esc_presses == 2);
	button(KEY_RIGHTCTRL, 0); button(KEY_ESC, 0); assert(map_toggles == 0);
	button(KEY_RIGHTCTRL, 1); button(KEY_RIGHTCTRL, 0); assert(overview && map_toggles == 1);
	button(KEY_SPACE, 1); assert(!keys[0x148] && !keys[0x1d]); button(KEY_SPACE, 0);
	button(KEY_ENTER, 1); button(KEY_ENTER, 0); assert(!overview && pause_presses == 2);
	button(KEY_RIGHTCTRL, 1); button(KEY_RIGHTCTRL, 0);
	button(KEY_RIGHTCTRL, 1); button(KEY_RIGHTCTRL, 0); assert(!overview);
	in_level = 0;
	button(KEY_LEFTALT, 1); button(KEY_ENTER, 1); button(KEY_LEFTALT, 0);
	button(KEY_ENTER, 0); in_level = 1;
	button(KEY_SPACE, 1); *mode = 2; steer(); assert(keys[0x1d] && !keys[0x148]);
	button(KEY_SPACE, 0);
	/* Facing south: Down walks forward; releasing it clears synthetic input. */
	button(KEY_DOWN, 1); assert(keys[0x148] && !keys[0x150]);
	button(KEY_DOWN, 0); assert(!keys[0x148]);
	in_level = 0; button(KEY_DOWN, 1); assert(keys[0x150] && !keys[0x148]);
	button(KEY_DOWN, 0); button(KEY_SPACE, 1); assert(keys[0x1d] && !keys[0x148]);
	button(KEY_SPACE, 0); in_level = 1; *mode = 0; car_buttons = 0;
	button(KEY_SPACE, 1); assert(keys[0x1d] && !keys[0x148]);
	button(KEY_SPACE, 0);
	pad_key(0x32, 1); steer(); assert(!keys[0x32]);
	pad_key(0x32, 0); steer(); assert(overview);
	puts("input: car/foot/menu transitions, walking, START, map and both chord orders passed");
}
