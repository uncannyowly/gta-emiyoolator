#include <assert.h>
#include <sys/mman.h>
#include "../src/mapview.c"
static int level = 1;
int ddraw_in_level(void) { return level; }
void shell_log(const char *fmt, ...) {}
void user32_key_event(int sc, int down) { if (sc == 0x40 && down) *(uint8_t *)0x7846b8 ^= 1; }
int main(void)
{
	assert(mmap((void *)0x700000, 0x100000, PROT_READ|PROT_WRITE,
	            MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0) != MAP_FAILED);
	uint8_t *data = calloc(1, 262184); assert(data);
	data[16] = 4; data[20] = 8; data[262172] = 5; data[262176] = 2 << 4;
	FILE *f = fopen("build/tests/test.CMP", "wb"); assert(f);
	assert(fwrite(data, 1, 262184, f) == 262184); fclose(f); free(data);
	mapview_load("build/tests/test.CMP"); assert(loaded && city[0] == 0xd1d5da);
	mapview_toggle(); assert(active && own_pause && *(uint8_t *)0x7846b8);
	assert(mapview_frame() && frame[40 * 640 + 24] == 0xd1d5da);
	mapview_close(); assert(!active && !*(uint8_t *)0x7846b8);
	*(uint8_t *)0x7846b8 = 1;
	mapview_toggle(); assert(active && !own_pause);
	mapview_close(); assert(*(uint8_t *)0x7846b8);
	level = 0; mapview_toggle(); assert(!active);
	f = fopen("build/tests/short.CMP", "wb"); assert(f); fputs("bad", f); fclose(f);
	mapview_load("build/tests/short.CMP"); assert(!loaded);
	puts("map: CMP decoding, truncated input, pause ownership, frontend gating passed");
}
