/*
 * MSS32 (Miles Sound System). Timers are real: each runs on its own thread
 * and calls the game's callback at the set rate, because the game's frame
 * limiter busy-waits on the counter its 70 Hz callback increments (0x47dc80),
 * as it could on Windows. Sound is silent for now: samples finish at once and
 * streams play (silently) until closed.
 */
#include "shell.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SMP_DONE 2
#define SMP_PLAYING 4
#define SMP_STOPPED 8

typedef void (WINAPI *TimerProc)(DWORD user);

#define MAX_TIMERS 8
static struct {
	TimerProc proc;
	DWORD user;
	volatile int hz, running, alive;
	pthread_t thread;
} timers[MAX_TIMERS];
static volatile int shutting_down;

static void *timer_thread(void *arg)
{
	int t = (int)(intptr_t)arg;
	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);
	while (!shutting_down && timers[t].alive) {
		long step = 1000000000L / (timers[t].hz > 0 ? timers[t].hz : 100);
		next.tv_nsec += step;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		if (timers[t].running && timers[t].proc)
			timers[t].proc(timers[t].user);
	}
	return NULL;
}

static LONG WINAPI ail_register_timer(TimerProc proc)
{
	for (int t = 0; t < MAX_TIMERS; t++)
		if (!timers[t].alive) {
			timers[t].proc = proc;
			timers[t].user = 0;
			timers[t].hz = 100;
			timers[t].running = 0;
			timers[t].alive = 1;
			if (pthread_create(&timers[t].thread, NULL, timer_thread, (void *)(intptr_t)t) != 0) {
				timers[t].alive = 0;
				return -1;
			}
			shell_log("AIL_register_timer(%p) = %d", (void *)proc, t);
			return t;
		}
	return -1;
}

static void WINAPI ail_set_timer_frequency(LONG t, DWORD hz)
{
	if (t >= 0 && t < MAX_TIMERS) {
		timers[t].hz = (int)hz;
		shell_log("AIL_set_timer_frequency(%d, %u Hz)", (int)t, hz);
	}
}

static void WINAPI ail_start_timer(LONG t) { if (t >= 0 && t < MAX_TIMERS) timers[t].running = 1; }
static void WINAPI ail_stop_timer(LONG t) { if (t >= 0 && t < MAX_TIMERS) timers[t].running = 0; }

static void WINAPI ail_release_timer_handle(LONG t)
{
	if (t >= 0 && t < MAX_TIMERS && timers[t].alive) {
		timers[t].running = 0;
		timers[t].alive = 0;
		pthread_join(timers[t].thread, NULL);
	}
}

void mss32_shutdown(void)
{
	shutting_down = 1;
	for (int t = 0; t < MAX_TIMERS; t++)
		if (timers[t].alive) {
			timers[t].alive = 0;
			pthread_join(timers[t].thread, NULL);
		}
}

static DWORD WINAPI ail_ms_count(void) { return shell_ms(); }

/* ---- driver ------------------------------------------------------------------- */

static int driver_dummy;

static LONG WINAPI ail_startup(void) { shell_log("AIL_startup"); return 1; }
static void WINAPI ail_shutdown(void) { mss32_shutdown(); }
static LONG WINAPI ail_set_preference(DWORD n, LONG v) { (void)n; (void)v; return 0; }

static LONG WINAPI ail_waveOutOpen(void **drv, void **wave, LONG dev, const void *fmt)
{
	(void)dev; (void)fmt;
	if (drv)
		*drv = &driver_dummy;
	if (wave)
		*wave = NULL;
	shell_log("AIL_waveOutOpen: digital driver open (silent)");
	return 0;
}

static void WINAPI ail_waveOutClose(void *drv) { (void)drv; }

static void WINAPI ail_digital_configuration(void *drv, LONG *rate, LONG *format, char *string)
{
	(void)drv;
	if (rate) *rate = 22050;
	if (format) *format = 3;   /* DIG_F_STEREO_16 */
	if (string) strcpy(string, "gtashell");
}

static void WINAPI ail_set_digital_master_volume(void *drv, LONG v) { (void)drv; (void)v; }
static void *WINAPI ail_mem_alloc_lock(DWORD size) { return calloc(1, size ? size : 1); }
static void WINAPI ail_mem_free_lock(void *p) { free(p); }

/* ---- samples ------------------------------------------------------------------- */

typedef struct {
	int status;
	const void *data;
	DWORD len;
	LONG rate, volume, pan, loops, format;
} Sample;

static void *WINAPI ail_allocate_sample_handle(void *drv)
{
	Sample *s = calloc(1, sizeof *s);
	(void)drv;
	s->status = SMP_DONE;
	return s;
}

static void WINAPI ail_release_sample_handle(Sample *s) { free(s); }
static void WINAPI ail_init_sample(Sample *s) { if (s) { s->status = SMP_DONE; s->volume = 127; s->pan = 64; s->loops = 1; } }
static void WINAPI ail_set_sample_type(Sample *s, LONG format, DWORD flags) { (void)flags; if (s) s->format = format; }
static void WINAPI ail_set_sample_address(Sample *s, const void *p, DWORD len) { if (s) { s->data = p; s->len = len; } }
static void WINAPI ail_set_sample_playback_rate(Sample *s, LONG r) { if (s) s->rate = r; }
static void WINAPI ail_set_sample_volume(Sample *s, LONG v) { if (s) s->volume = v; }
static void WINAPI ail_set_sample_pan(Sample *s, LONG p) { if (s) s->pan = p; }
static void WINAPI ail_set_sample_loop_count(Sample *s, LONG n) { if (s) s->loops = n; }
/* silent: a one-shot sample is over as soon as it starts; loops play until ended */
static void WINAPI ail_start_sample(Sample *s) { if (s) s->status = s->loops == 0 ? SMP_PLAYING : SMP_DONE; }
static void WINAPI ail_end_sample(Sample *s) { if (s) s->status = SMP_DONE; }
static DWORD WINAPI ail_sample_status(Sample *s) { return s ? (DWORD)s->status : SMP_DONE; }

/* ---- streams (music) -------------------------------------------------------------- */

typedef struct {
	int status;
	char name[260];
	LONG volume, loops;
	DWORD position;
} Stream;

static void *WINAPI ail_open_stream(void *drv, const char *name, LONG mem)
{
	Stream *s = calloc(1, sizeof *s);
	(void)drv; (void)mem;
	snprintf(s->name, sizeof s->name, "%s", name ? name : "");
	s->status = SMP_STOPPED;
	shell_log("AIL_open_stream(\"%s\")", s->name);
	return s;
}

static void WINAPI ail_close_stream(Stream *s) { free(s); }
static void WINAPI ail_start_stream(Stream *s) { if (s) s->status = SMP_PLAYING; }
static void WINAPI ail_pause_stream(Stream *s, LONG on) { if (s) s->status = on ? SMP_STOPPED : SMP_PLAYING; }
static void WINAPI ail_set_stream_volume(Stream *s, LONG v) { if (s) s->volume = v; }
static void WINAPI ail_set_stream_loop_count(Stream *s, LONG n) { if (s) s->loops = n; }
static void WINAPI ail_set_stream_position(Stream *s, LONG pos) { if (s) s->position = (DWORD)pos; }
static LONG WINAPI ail_stream_position(Stream *s) { return s ? (LONG)s->position : 0; }
static LONG WINAPI ail_stream_status(Stream *s) { return s ? s->status : SMP_DONE; }
static LONG WINAPI ail_service_stream(Stream *s, LONG fill) { (void)s; (void)fill; return 0; }

static void WINAPI ail_stream_info(Stream *s, LONG *rate, LONG *type, LONG *length, LONG *memory)
{
	(void)s;
	if (rate) *rate = 22050;
	if (type) *type = 3;
	if (length) *length = 22050 * 4 * 600;
	if (memory) *memory = 0;
}

const ShimFunc mss32_funcs[] = {
	{ "_AIL_startup@0", 0, ail_startup },
	{ "_AIL_shutdown@0", 0, ail_shutdown },
	{ "_AIL_set_preference@8", 0, ail_set_preference },
	{ "_AIL_waveOutOpen@16", 0, ail_waveOutOpen },
	{ "_AIL_waveOutClose@4", 0, ail_waveOutClose },
	{ "_AIL_digital_configuration@16", 0, ail_digital_configuration },
	{ "_AIL_set_digital_master_volume@8", 0, ail_set_digital_master_volume },
	{ "_AIL_mem_alloc_lock@4", 0, ail_mem_alloc_lock },
	{ "_AIL_mem_free_lock@4", 0, ail_mem_free_lock },
	{ "_AIL_register_timer@4", 0, ail_register_timer },
	{ "_AIL_set_timer_frequency@8", 0, ail_set_timer_frequency },
	{ "_AIL_start_timer@4", 0, ail_start_timer },
	{ "_AIL_stop_timer@4", 0, ail_stop_timer },
	{ "_AIL_release_timer_handle@4", 0, ail_release_timer_handle },
	{ "_AIL_ms_count@0", 0, ail_ms_count },
	{ "_AIL_allocate_sample_handle@4", 0, ail_allocate_sample_handle },
	{ "_AIL_release_sample_handle@4", 0, ail_release_sample_handle },
	{ "_AIL_init_sample@4", 0, ail_init_sample },
	{ "_AIL_set_sample_type@12", 0, ail_set_sample_type },
	{ "_AIL_set_sample_address@12", 0, ail_set_sample_address },
	{ "_AIL_set_sample_playback_rate@8", 0, ail_set_sample_playback_rate },
	{ "_AIL_set_sample_volume@8", 0, ail_set_sample_volume },
	{ "_AIL_set_sample_pan@8", 0, ail_set_sample_pan },
	{ "_AIL_set_sample_loop_count@8", 0, ail_set_sample_loop_count },
	{ "_AIL_start_sample@4", 0, ail_start_sample },
	{ "_AIL_end_sample@4", 0, ail_end_sample },
	{ "_AIL_sample_status@4", 0, ail_sample_status },
	{ "_AIL_open_stream@12", 0, ail_open_stream },
	{ "_AIL_close_stream@4", 0, ail_close_stream },
	{ "_AIL_start_stream@4", 0, ail_start_stream },
	{ "_AIL_pause_stream@8", 0, ail_pause_stream },
	{ "_AIL_set_stream_volume@8", 0, ail_set_stream_volume },
	{ "_AIL_set_stream_loop_count@8", 0, ail_set_stream_loop_count },
	{ "_AIL_set_stream_position@8", 0, ail_set_stream_position },
	{ "_AIL_stream_position@4", 0, ail_stream_position },
	{ "_AIL_stream_status@4", 0, ail_stream_status },
	{ "_AIL_service_stream@8", 0, ail_service_stream },
	{ "_AIL_stream_info@20", 0, ail_stream_info },
	{ NULL, 0, NULL },
};
