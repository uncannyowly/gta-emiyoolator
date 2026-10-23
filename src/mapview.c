/* Read-only city overview from the user's CMP. This never changes game state
 * beyond using the original F6 pause key while the overview is open. */
#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint32_t city[256 * 256], frame[640 * 480];
static int loaded, active, own_pause;
static char city_name[48];
int ddraw_in_level(void);

static unsigned rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }

void mapview_load(const char *path)
{
	static const uint32_t colors[8] = {
		0x182634, 0x204d79, 0xd1d5da, 0x888f96, 0x527447, 0x41494f, 0x41494f, 0x41494f
	};
	size_t len = strlen(path), size;
	FILE *f;
	uint8_t *data;
	long end;
	if (len < 4 || strcasecmp(path + len - 4, ".cmp")) return;
	active = own_pause = loaded = 0;
	if (!(f = fopen(path, "rb"))) return;
	if (fseek(f, 0, SEEK_END) || (end = ftell(f)) < 262172 || end > 32 * 1024 * 1024) {
		fclose(f); return;
	}
	size = (size_t)end;
	data = malloc(size);
	if (!data) { fclose(f); return; }
	rewind(f);
	if (fread(data, 1, size, f) != size) { fclose(f); free(data); return; }
	fclose(f);
	uint32_t columns = rd32(data + 16), blocks = rd32(data + 20);
	size_t base = 28 + 256 * 256 * 4;
	if (columns > size - base || blocks > size - base - columns) { free(data); return; }
	for (unsigned i = 0; i < 256 * 256; i++) {
		uint32_t off = rd32(data + 28 + i * 4);
		int type = 0;
		if (off <= columns && columns - off >= 2) {
			const uint8_t *col = data + base + off;
			unsigned empty = rd16(col);
			if (empty <= 6 && columns - off >= 2 + (6 - empty) * 2) {
				/* Prefer the road/pavement layer, so roofs do not hide roads
				 * in tunnels and railway decks do not obscure intersections. */
				for (unsigned n = 0; n < 6 - empty; n++) {
					unsigned bi = rd16(col + 2 + n * 2);
					if (bi >= blocks / 8) continue;
					int t = (rd16(data + base + columns + bi * 8) >> 4) & 7;
					if (!type && t) type = t;
					if (t == 2 || t == 3) { type = t; break; }
				}
			}
		}
		city[i] = colors[type];
	}
	free(data);
	const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
	snprintf(city_name, sizeof city_name, "%s", !strcasecmp(name, "NYC.CMP") ? "LIBERTY CITY" :
	         !strcasecmp(name, "SANB.CMP") ? "SAN ANDREAS" : !strcasecmp(name, "MIAMI.CMP") ? "VICE CITY" : "CITY MAP");
	loaded = 1;
	shell_log("map: overview loaded from %s", path);
}

static void pause_key(void)
{
	user32_key_event(0x40, 1);
	user32_key_event(0x40, 0);
}

int mapview_active(void) { return active; }

void mapview_close(void)
{
	if (!active) return;
	int paused = *(const uint8_t *)0x7846b8;
	active = 0;
	if (own_pause) pause_key();
	own_pause = 0;
	shell_log("map: closed (game was %s)", paused ? "paused" : "running");
}

void mapview_toggle(void)
{
	if (active) { mapview_close(); return; }
	if (!ddraw_in_level() || !loaded) return;
	own_pause = *(const uint8_t *)0x7846b8 == 0;
	if (own_pause) pause_key();
	active = 1;
	shell_log("map: opened%s", own_pause ? ", paused game" : " (already paused)");
}

/* Small original 3x5 UI alphabet, rows packed from top to bottom. */
static unsigned glyph(char c)
{
	static const char *const letters[] = {
		"010101111101101", "110101110101110", "011100100100011", "110101101101110",
		"111100110100111", "111100110100100", "011100101101011", "101101111101101",
		"111010010010111", "001001001101010", "101101110101101", "100100100100111",
		"101111111101101", "101111111111101", "010101101101010", "110101110100100",
		"010101101111011", "110101110101101", "011100010001110", "111010010010010",
		"101101101101111", "101101101101010", "101101111111101", "101101010101101",
		"101101010010010", "111001010100111"
	};
	if (c >= 'A' && c <= 'Z') {
		unsigned g = 0;
		for (int i = 0; i < 15; i++) g = g * 2 + (letters[c - 'A'][i] == '1');
		return g;
	}
	if (c == '/') return 0x12a4;
	if (c == '-') return 0x01c0;
	return 0;
}

static void text(int x, int y, const char *s, uint32_t color)
{
	for (; *s; s++, x += 16) {
		unsigned g = glyph(*s);
		for (int row = 0; row < 5; row++) for (int col = 0; col < 3; col++)
			if (g & (1u << (14 - row * 3 - col)))
				for (int dy = 0; dy < 3; dy++) for (int dx = 0; dx < 3; dx++)
					if (x + col * 4 + dx < 640 && y + row * 4 + dy < 480)
						frame[(y + row * 4 + dy) * 640 + x + col * 4 + dx] = color;
	}
}

const uint32_t *mapview_frame(void)
{
	if (!active || !ddraw_in_level()) { active = own_pause = 0; return NULL; }
	for (unsigned i = 0; i < 640 * 480; i++) frame[i] = 0x111820;
	text(24, 10, city_name, 0xffffff);
	/* Full city, north up. 384 pixels leaves room for a legend. */
	for (int y = 0; y < 384; y++) for (int x = 0; x < 384; x++)
		frame[(y + 40) * 640 + x + 24] = city[(y * 256 / 384) * 256 + x * 256 / 384];
	text(432, 60, "NORTH UP", 0xffffff);
	text(432, 112, "ROADS", 0xd1d5da);
	text(432, 152, "BUILDINGS", 0x888f96);
	text(432, 192, "WATER", 0x4488bb);
	text(432, 232, "PARKS", 0x77aa66);
	text(24, 448, "SELECT CLOSE / START PAUSE", 0xffffff);
	return frame;
}
