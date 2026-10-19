/*
 * MSS32 (Miles Sound System), the game's sound library.
 *
 * Timers run on their own threads and call the game's callback at the set
 * rate: the frame limiter (0x47dc80) busy-waits on the counter its 70 Hz
 * callback increments.
 *
 * Sound: samples (effects, from the game's own memory) and streams (music,
 * the WAV files) are mixed by a thread into signed 16-bit stereo at 22050 Hz
 * and written to an output command's stdin (ini key audio_cmd): on the Miyoo
 * the native helper gtaaudio (MI_AO), on a PC e.g. "aplay -q -f S16_LE -r
 * 22050 -c 2". audio = none silences it; audio_wav = file records it. The pipe
 * write blocks when the player is full, which paces the mixer.
 */
#include "shell.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RATE 22050
#define MIX_FRAMES 512

#define SMP_FREE 1
#define SMP_DONE 2
#define SMP_PLAYING 4
#define SMP_STOPPED 8

/* Miles sample formats and flags */
#define DIG_F_16BITS_MASK 1
#define DIG_F_STEREO_MASK 2
#define DIG_PCM_SIGN 1

static pthread_mutex_t mix_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int shutting_down;

/* ---- timers ----------------------------------------------------------------------- */

typedef void (WINAPI *TimerProc)(DWORD user);

#define MAX_TIMERS 8
static struct {
	TimerProc proc;
	DWORD user;
	volatile int hz, running, alive;
	pthread_t thread;
} timers[MAX_TIMERS];

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

static void release_timer(int t)
{
	if (timers[t].alive) {
		timers[t].running = 0;
		timers[t].alive = 0;
		pthread_join(timers[t].thread, NULL);
	}
}

static void WINAPI ail_release_timer_handle(LONG t) { if (t >= 0 && t < MAX_TIMERS) release_timer((int)t); }
static DWORD WINAPI ail_ms_count(void) { return shell_ms(); }

/* ---- voices --------------------------------------------------------------------------- */

typedef struct Sample {
	int used, status;
	const uint8_t *data;
	DWORD len;           /* bytes */
	LONG format, flags, rate, volume, pan, loops;
	uint64_t pos;        /* frames, 32.32 fixed point */
} Sample;

typedef struct Stream {
	int used, status;
	FILE *f;
	char name[260];
	long data_start;
	DWORD data_len, pos;   /* bytes of PCM data */
	int channels, bits, rate;
	LONG volume, loops;
	int16_t buf[4096];     /* decoded frames waiting to be mixed (stereo) */
	int buf_frames, buf_pos;
	uint64_t frac;         /* resampling, 32.32 */
} Stream;

#define MAX_SAMPLES 32
#define MAX_STREAMS 16
static Sample samples[MAX_SAMPLES];
static Stream streams[MAX_STREAMS];
static LONG master_volume = 127;

/* one source frame of a sample as left/right in -32768..32767 */
static void sample_frame(const Sample *s, uint32_t i, int *l, int *r)
{
	int ch = (s->format & DIG_F_STEREO_MASK) ? 2 : 1;
	if (s->format & DIG_F_16BITS_MASK) {
		const int16_t *p = (const int16_t *)s->data + (size_t)i * ch;
		*l = p[0];
		*r = ch == 2 ? p[1] : p[0];
	} else {
		const uint8_t *p = s->data + (size_t)i * ch;
		int a = (s->flags & DIG_PCM_SIGN) ? (int8_t)p[0] : p[0] - 128;
		int b = ch == 2 ? ((s->flags & DIG_PCM_SIGN) ? (int8_t)p[1] : p[1] - 128) : a;
		*l = a << 8;
		*r = b << 8;
	}
}

static void mix_sample(Sample *s, int32_t *out, int n)
{
	int ch = (s->format & DIG_F_STEREO_MASK) ? 2 : 1;
	int bytes = (s->format & DIG_F_16BITS_MASK) ? 2 : 1;
	uint32_t frames = s->len / (uint32_t)(ch * bytes);
	uint64_t step = ((uint64_t)(s->rate > 0 ? s->rate : 11025) << 32) / RATE;
	/* Miles: volume 0..127, pan 0 (left) .. 64 .. 127 (right) */
	int vol = s->volume * master_volume;
	int lv = vol * (s->pan <= 64 ? 64 : 127 - s->pan) / 64;
	int rv = vol * (s->pan >= 64 ? 64 : s->pan) / 64;

	if (!s->data || frames == 0) {
		s->status = SMP_DONE;
		return;
	}
	for (int i = 0; i < n; i++) {
		uint32_t f = (uint32_t)(s->pos >> 32);
		int l, r;
		if (f >= frames) {
			if (s->loops == 1) {
				s->status = SMP_DONE;
				return;
			}
			if (s->loops > 1)
				s->loops--;
			s->pos -= (uint64_t)frames << 32;
			f = (uint32_t)(s->pos >> 32);
			if (f >= frames)
				f = 0, s->pos = 0;
		}
		sample_frame(s, f, &l, &r);
		out[2 * i] += l * lv / (127 * 127);
		out[2 * i + 1] += r * rv / (127 * 127);
		s->pos += step;
	}
}

/* refill a stream's decode buffer from its file; 0 at the end of the data */
static int stream_refill(Stream *s)
{
	uint8_t raw[8192];
	int bpf = s->channels * s->bits / 8, want, got;
	if (s->pos >= s->data_len) {
		if (s->loops == 1)
			return 0;
		if (s->loops > 1)
			s->loops--;
		s->pos = 0;
	}
	want = (int)((s->data_len - s->pos) < sizeof raw ? (s->data_len - s->pos) : sizeof raw);
	want -= want % bpf;
	if (want <= 0 || fseek(s->f, s->data_start + (long)s->pos, SEEK_SET) != 0)
		return 0;
	got = (int)fread(raw, 1, (size_t)want, s->f);
	got -= got % bpf;
	if (got <= 0)
		return 0;
	s->pos += (DWORD)got;
	s->buf_frames = got / bpf;
	if (s->buf_frames > 2048)
		s->buf_frames = 2048;
	for (int i = 0; i < s->buf_frames; i++) {
		int l, r;
		if (s->bits == 16) {
			const int16_t *p = (const int16_t *)raw + i * s->channels;
			l = p[0];
			r = s->channels == 2 ? p[1] : p[0];
		} else {
			const uint8_t *p = raw + i * s->channels;
			l = (p[0] - 128) << 8;
			r = s->channels == 2 ? (p[1] - 128) << 8 : l;
		}
		s->buf[2 * i] = (int16_t)l;
		s->buf[2 * i + 1] = (int16_t)r;
	}
	s->buf_pos = 0;
	return 1;
}

static void mix_stream(Stream *s, int32_t *out, int n)
{
	uint64_t step = ((uint64_t)(s->rate > 0 ? s->rate : RATE) << 32) / RATE;
	int vol = s->volume * master_volume;
	for (int i = 0; i < n; i++) {
		while (s->buf_pos >= s->buf_frames) {
			if (!stream_refill(s)) {
				s->status = SMP_DONE;
				return;
			}
		}
		out[2 * i] += s->buf[2 * s->buf_pos] * vol / (127 * 127);
		out[2 * i + 1] += s->buf[2 * s->buf_pos + 1] * vol / (127 * 127);
		s->frac += step;
		s->buf_pos += (int)(s->frac >> 32);
		s->frac &= 0xFFFFFFFFu;
	}
}

static void mix(int16_t *out, int n)
{
	static int32_t acc[MIX_FRAMES * 2];
	memset(acc, 0, sizeof acc);
	pthread_mutex_lock(&mix_lock);
	for (int i = 0; i < MAX_SAMPLES; i++)
		if (samples[i].used && samples[i].status == SMP_PLAYING)
			mix_sample(&samples[i], acc, n);
	for (int i = 0; i < MAX_STREAMS; i++)
		if (streams[i].used && streams[i].status == SMP_PLAYING && streams[i].f)
			mix_stream(&streams[i], acc, n);
	pthread_mutex_unlock(&mix_lock);
	for (int i = 0; i < 2 * n; i++)
		out[i] = (int16_t)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
}

/* ---- output: a pipe into the player command, or a WAV file ------------------------ */

static pthread_t mix_thread;
static int mix_running, out_fd = -1;
static pid_t out_pid;
static FILE *wav;

static void *mix_loop(void *arg)
{
	static int16_t buf[MIX_FRAMES * 2];
	struct timespec next;
	(void)arg;
	clock_gettime(CLOCK_MONOTONIC, &next);
	while (mix_running && !shutting_down) {
		mix(buf, MIX_FRAMES);
		if (wav)
			fwrite(buf, 1, sizeof buf, wav);
		if (out_fd >= 0) {
			size_t done = 0;
			while (done < sizeof buf) {
				ssize_t w = write(out_fd, (char *)buf + done, sizeof buf - done);
				if (w <= 0) {
					shell_log("audio: the player stopped; sound off");
					close(out_fd);
					out_fd = -1;
					break;
				}
				done += (size_t)w;
			}
		} else {
			/* no player to pace us: keep real time */
			next.tv_nsec += 1000000000L / RATE * MIX_FRAMES;
			while (next.tv_nsec >= 1000000000L) {
				next.tv_nsec -= 1000000000L;
				next.tv_sec++;
			}
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		}
	}
	return NULL;
}

static void audio_start(void)
{
	const char *cmd = config_str("audio_cmd", "./gtaaudio");
	const char *wavpath = config_str("audio_wav", NULL);
	int fds[2];

	if (mix_running || !config_int("audio", 1))
		return;
	signal(SIGPIPE, SIG_IGN);
	if (wavpath && *wavpath && (wav = fopen(wavpath, "wb")) != NULL) {
		/* header with open-ended sizes: players read to the end */
		static const uint8_t hdr[44] = { 'R', 'I', 'F', 'F', 0xFF, 0xFF, 0xFF, 0x7F, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
		                                 16, 0, 0, 0, 1, 0, 2, 0, 0x22, 0x56, 0, 0, 0x88, 0x58, 1, 0, 4, 0, 16, 0,
		                                 'd', 'a', 't', 'a', 0xFF, 0xFF, 0xFF, 0x7F };
		fwrite(hdr, 1, sizeof hdr, wav);
	}
	if (cmd && *cmd && pipe(fds) == 0) {
		out_pid = fork();
		if (out_pid == 0) {
			dup2(fds[0], 0);
			close(fds[0]);
			close(fds[1]);
			execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		close(fds[0]);
		if (out_pid > 0) {
			out_fd = fds[1];
#ifdef F_SETPIPE_SZ
			fcntl(out_fd, F_SETPIPE_SZ, 8192);   /* about 90 ms of sound in flight */
#endif
			shell_log("audio: playing through \"%s\" (pid %d)", cmd, (int)out_pid);
		} else {
			close(fds[1]);
		}
	}
	mix_running = 1;
	if (pthread_create(&mix_thread, NULL, mix_loop, NULL) != 0)
		mix_running = 0;
}

void mss32_shutdown(void)
{
	shutting_down = 1;
	for (int t = 0; t < MAX_TIMERS; t++)
		release_timer(t);
	if (mix_running) {
		mix_running = 0;
		pthread_join(mix_thread, NULL);
	}
	if (out_fd >= 0) {
		close(out_fd);
		out_fd = -1;
		if (out_pid > 0)
			waitpid(out_pid, NULL, 0);
	}
	if (wav)
		fclose(wav);
	wav = NULL;
}

/* ---- driver ------------------------------------------------------------------------- */

static int driver_dummy;

static LONG WINAPI ail_startup(void) { shell_log("AIL_startup"); return 1; }

/* the game shuts Miles down and starts it again between front end and levels:
 * release the timers that exist (later ones must still run) and stop sounds */
static void WINAPI ail_shutdown(void)
{
	for (int t = 0; t < MAX_TIMERS; t++)
		release_timer(t);
	pthread_mutex_lock(&mix_lock);
	for (int i = 0; i < MAX_SAMPLES; i++)
		samples[i].status = samples[i].used ? SMP_DONE : 0;
	pthread_mutex_unlock(&mix_lock);
}

static LONG WINAPI ail_set_preference(DWORD n, LONG v) { (void)n; (void)v; return 0; }

static LONG WINAPI ail_waveOutOpen(void **drv, void **wave, LONG dev, const void *fmt)
{
	(void)dev; (void)fmt;
	if (drv)
		*drv = &driver_dummy;
	if (wave)
		*wave = NULL;
	audio_start();
	return 0;
}

static void WINAPI ail_waveOutClose(void *drv) { (void)drv; }

static void WINAPI ail_digital_configuration(void *drv, LONG *rate, LONG *format, char *string)
{
	(void)drv;
	if (rate) *rate = RATE;
	if (format) *format = 3;   /* DIG_F_STEREO_16 */
	if (string) strcpy(string, "gtashell");
}

static void WINAPI ail_set_digital_master_volume(void *drv, LONG v) { (void)drv; master_volume = v < 0 ? 0 : v > 127 ? 127 : v; }
static void *WINAPI ail_mem_alloc_lock(DWORD size) { return calloc(1, size ? size : 1); }
static void WINAPI ail_mem_free_lock(void *p) { free(p); }

/* ---- samples -------------------------------------------------------------------------- */

static Sample *WINAPI ail_allocate_sample_handle(void *drv)
{
	(void)drv;
	pthread_mutex_lock(&mix_lock);
	for (int i = 0; i < MAX_SAMPLES; i++)
		if (!samples[i].used) {
			memset(&samples[i], 0, sizeof samples[i]);
			samples[i].used = 1;
			samples[i].status = SMP_DONE;
			samples[i].volume = 127;
			samples[i].pan = 64;
			samples[i].loops = 1;
			samples[i].rate = 11025;
			pthread_mutex_unlock(&mix_lock);
			return &samples[i];
		}
	pthread_mutex_unlock(&mix_lock);
	shell_log("audio: out of sample handles");
	return NULL;
}

#define LOCKED(stmt) do { pthread_mutex_lock(&mix_lock); stmt; pthread_mutex_unlock(&mix_lock); } while (0)

static void WINAPI ail_release_sample_handle(Sample *s) { if (s) LOCKED(s->used = 0; s->status = SMP_FREE); }

static void WINAPI ail_init_sample(Sample *s)
{
	if (s)
		LOCKED(s->status = SMP_DONE; s->format = 0; s->flags = 0; s->rate = 11025; s->volume = 127; s->pan = 64;
		       s->loops = 1; s->pos = 0; s->data = NULL; s->len = 0);
}

static void WINAPI ail_set_sample_type(Sample *s, LONG format, DWORD flags) { if (s) LOCKED(s->format = format; s->flags = (LONG)flags); }
static void WINAPI ail_set_sample_address(Sample *s, const void *p, DWORD len) { if (s) LOCKED(s->data = p; s->len = len; s->pos = 0); }
static void WINAPI ail_set_sample_playback_rate(Sample *s, LONG r) { if (s) LOCKED(s->rate = r); }
static void WINAPI ail_set_sample_volume(Sample *s, LONG v) { if (s) LOCKED(s->volume = v < 0 ? 0 : v > 127 ? 127 : v); }
static void WINAPI ail_set_sample_pan(Sample *s, LONG p) { if (s) LOCKED(s->pan = p < 0 ? 0 : p > 127 ? 127 : p); }
static void WINAPI ail_set_sample_loop_count(Sample *s, LONG n) { if (s) LOCKED(s->loops = n); }
static void WINAPI ail_start_sample(Sample *s) { if (s) LOCKED(s->pos = 0; s->status = SMP_PLAYING); }
static void WINAPI ail_end_sample(Sample *s) { if (s) LOCKED(s->status = SMP_DONE); }

static DWORD WINAPI ail_sample_status(Sample *s)
{
	DWORD st;
	if (!s)
		return SMP_DONE;
	LOCKED(st = (DWORD)s->status);
	return st;
}

/* ---- streams (music) ------------------------------------------------------------------- */

static int open_wav(Stream *s, const char *name)
{
	char host[1024];
	uint8_t h[12], ck[8];
	files_host_path(name, host, sizeof host);
	s->f = fopen(host, "rb");
	if (!s->f || fread(h, 1, 12, s->f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
		goto fail;
	while (fread(ck, 1, 8, s->f) == 8) {
		uint32_t len = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
		if (!memcmp(ck, "fmt ", 4)) {
			uint8_t fmt[16];
			if (len < 16 || fread(fmt, 1, 16, s->f) != 16)
				goto fail;
			s->channels = fmt[2];
			s->rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16;
			s->bits = fmt[14];
			fseek(s->f, (long)(len - 16 + (len & 1)), SEEK_CUR);
		} else if (!memcmp(ck, "data", 4)) {
			s->data_start = ftell(s->f);
			s->data_len = len;
			if (s->channels < 1 || s->channels > 2 || (s->bits != 8 && s->bits != 16))
				goto fail;
			return 0;
		} else {
			fseek(s->f, (long)(len + (len & 1)), SEEK_CUR);
		}
	}
fail:
	shell_log("audio: cannot play \"%s\" (%s)", name, host);
	if (s->f)
		fclose(s->f);
	s->f = NULL;
	return -1;
}

static Stream *WINAPI ail_open_stream(void *drv, const char *name, LONG mem)
{
	(void)drv; (void)mem;
	for (int i = 0; i < MAX_STREAMS; i++)
		if (!streams[i].used) {
			Stream *s = &streams[i];
			memset(s, 0, sizeof *s);
			snprintf(s->name, sizeof s->name, "%s", name ? name : "");
			if (open_wav(s, s->name) != 0)
				return NULL;
			s->volume = 127;
			s->loops = 1;
			s->status = SMP_STOPPED;
			LOCKED(s->used = 1);
			shell_log("AIL_open_stream(\"%s\"): %d Hz, %d ch, %d bit, %u bytes", s->name, s->rate, s->channels,
			          s->bits, s->data_len);
			return s;
		}
	return NULL;
}

static void WINAPI ail_close_stream(Stream *s)
{
	if (!s)
		return;
	pthread_mutex_lock(&mix_lock);
	s->used = 0;
	s->status = SMP_FREE;
	if (s->f)
		fclose(s->f);
	s->f = NULL;
	pthread_mutex_unlock(&mix_lock);
}

static void WINAPI ail_start_stream(Stream *s) { if (s) LOCKED(s->status = SMP_PLAYING); }
static void WINAPI ail_pause_stream(Stream *s, LONG on) { if (s) LOCKED(s->status = on ? SMP_STOPPED : SMP_PLAYING); }
static void WINAPI ail_set_stream_volume(Stream *s, LONG v) { if (s) LOCKED(s->volume = v < 0 ? 0 : v > 127 ? 127 : v); }
static void WINAPI ail_set_stream_loop_count(Stream *s, LONG n) { if (s) LOCKED(s->loops = n); }

static void WINAPI ail_set_stream_position(Stream *s, LONG pos)
{
	if (!s)
		return;
	pthread_mutex_lock(&mix_lock);
	s->pos = pos < 0 ? 0 : (DWORD)pos > s->data_len ? s->data_len : (DWORD)pos;
	s->pos -= s->pos % (DWORD)(s->channels * s->bits / 8);
	s->buf_frames = s->buf_pos = 0;
	pthread_mutex_unlock(&mix_lock);
}

/* bytes played so far: file position minus what is still waiting in the buffer */
static LONG WINAPI ail_stream_position(Stream *s)
{
	LONG p;
	if (!s)
		return 0;
	pthread_mutex_lock(&mix_lock);
	p = (LONG)s->pos - (LONG)((s->buf_frames - s->buf_pos) * s->channels * s->bits / 8);
	pthread_mutex_unlock(&mix_lock);
	return p < 0 ? 0 : p;
}

static LONG WINAPI ail_stream_status(Stream *s)
{
	LONG st;
	if (!s)
		return SMP_DONE;
	LOCKED(st = s->status);
	return st;
}

static LONG WINAPI ail_service_stream(Stream *s, LONG fill) { (void)s; (void)fill; return 0; }

static void WINAPI ail_stream_info(Stream *s, LONG *rate, LONG *type, LONG *length, LONG *memory)
{
	if (rate) *rate = s ? s->rate * s->channels * s->bits / 8 : 0;
	if (type) *type = s ? (s->bits == 16 ? 1 : 0) | (s->channels == 2 ? 2 : 0) : 3;
	if (length) *length = s ? (LONG)s->data_len : 0;
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
