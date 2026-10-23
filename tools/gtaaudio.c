/*
 * gtaaudio: native ARM helper that plays what gtashell mixes. gtashell runs as
 * x86 code under box86 and cannot call the Miyoo's MI_AO audio driver, so it
 * pipes signed 16-bit stereo PCM at 22050 Hz into this program's stdin.
 * MI_AO_SendFrame blocks while the driver queue is full, which paces the
 * whole chain (the pipe fills, gtashell's mixer waits). Without MI_AO the
 * input is drained at the same rate, so the game never stalls on sound.
 * Based on the MI_AO code of ../gta1-miyoo (src/platform/platform_miyoo.c).
 */
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mi_ao.h>
#include <mi_sys.h>

#define RATE 22050
#define FRAMES 512
#define AO_DEV 0
#define AO_CHN 0

static MI_S32 (*p_MI_SYS_Init)(void);
static MI_S32 (*p_MI_AO_SetPubAttr)(MI_AUDIO_DEV, MI_AUDIO_Attr_t *);
static MI_S32 (*p_MI_AO_Enable)(MI_AUDIO_DEV);
static MI_S32 (*p_MI_AO_Disable)(MI_AUDIO_DEV);
static MI_S32 (*p_MI_AO_EnableChn)(MI_AUDIO_DEV, MI_AO_CHN);
static MI_S32 (*p_MI_AO_DisableChn)(MI_AUDIO_DEV, MI_AO_CHN);
static MI_S32 (*p_MI_AO_SendFrame)(MI_AUDIO_DEV, MI_AO_CHN, MI_AUDIO_Frame_t *, MI_S32);
static MI_S32 (*p_MI_AO_SetVolume)(MI_AUDIO_DEV, MI_S32);
static MI_S32 (*p_MI_AO_GetVolume)(MI_AUDIO_DEV, MI_S32 *);
static MI_S32 (*p_MI_AO_SetMute)(MI_AUDIO_DEV, MI_BOOL);
static MI_S32 (*p_MI_AO_GetMute)(MI_AUDIO_DEV, MI_BOOL *);

/* Match Onion's logarithmic volume curve, not a linear dB ramp.
 * https://github.com/OnionUI/Onion/blob/main/src/common/system/volume.h
 * /appconfigs is the firmware fallback; Onion keeps settings on the card. */
static int volume_db(int vol)
{
	if (vol < 0) vol = 0;
	if (vol > 20) vol = 20;
	return vol == 0 ? -60 : (int)lround(48.0 * log10(1.0 + vol)) - 60;
}

static int system_volume_db(void)
{
	char buf[2048];
	FILE *f = fopen("/mnt/SDCARD/system.json", "r");
	int vol = 10;
	if (!f) f = fopen("/appconfigs/system.json", "r");
	if (f) {
		size_t n = fread(buf, 1, sizeof buf - 1, f);
		char *v;
		buf[n] = 0;
		fclose(f);
		v = strstr(buf, "\"vol\"");
		if (v && (v = strchr(v, ':')))
			vol = atoi(v + 1);
	}
	return volume_db(vol);
}

static void *load(const char *name)
{
	char path[128];
	void *h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
	if (!h) {
		snprintf(path, sizeof path, "/config/lib/%s", name);
		h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
	}
	return h;
}

static int mi_ao_open(void)
{
	void *sys, *ao;
	MI_AUDIO_Attr_t attr;
	MI_S32 e1, e2, e3, vol = -99;
	MI_BOOL mute = FALSE;

	/* libmi_ao uses symbols of SigmaStar's OS wrapper without linking it */
	load("libcam_os_wrapper.so");
	load("libmi_common.so");
	sys = load("libmi_sys.so");
	ao = load("libmi_ao.so");
	if (!sys || !ao) {
		fprintf(stderr, "gtaaudio: MI_AO unavailable: %s\n", dlerror());
		return -1;
	}
	p_MI_SYS_Init = (MI_S32 (*)(void))dlsym(sys, "MI_SYS_Init");
	p_MI_AO_SetPubAttr = (MI_S32 (*)(MI_AUDIO_DEV, MI_AUDIO_Attr_t *))dlsym(ao, "MI_AO_SetPubAttr");
	p_MI_AO_Enable = (MI_S32 (*)(MI_AUDIO_DEV))dlsym(ao, "MI_AO_Enable");
	p_MI_AO_Disable = (MI_S32 (*)(MI_AUDIO_DEV))dlsym(ao, "MI_AO_Disable");
	p_MI_AO_EnableChn = (MI_S32 (*)(MI_AUDIO_DEV, MI_AO_CHN))dlsym(ao, "MI_AO_EnableChn");
	p_MI_AO_DisableChn = (MI_S32 (*)(MI_AUDIO_DEV, MI_AO_CHN))dlsym(ao, "MI_AO_DisableChn");
	p_MI_AO_SendFrame = (MI_S32 (*)(MI_AUDIO_DEV, MI_AO_CHN, MI_AUDIO_Frame_t *, MI_S32))dlsym(ao, "MI_AO_SendFrame");
	p_MI_AO_SetVolume = (MI_S32 (*)(MI_AUDIO_DEV, MI_S32))dlsym(ao, "MI_AO_SetVolume");
	p_MI_AO_GetVolume = (MI_S32 (*)(MI_AUDIO_DEV, MI_S32 *))dlsym(ao, "MI_AO_GetVolume");
	p_MI_AO_SetMute = (MI_S32 (*)(MI_AUDIO_DEV, MI_BOOL))dlsym(ao, "MI_AO_SetMute");
	p_MI_AO_GetMute = (MI_S32 (*)(MI_AUDIO_DEV, MI_BOOL *))dlsym(ao, "MI_AO_GetMute");
	if (!p_MI_SYS_Init || !p_MI_AO_SetPubAttr || !p_MI_AO_Enable || !p_MI_AO_EnableChn || !p_MI_AO_SendFrame) {
		fprintf(stderr, "gtaaudio: MI_AO library lacks functions\n");
		return -1;
	}
	p_MI_SYS_Init();
	memset(&attr, 0, sizeof attr);
	attr.eSamplerate = (MI_AUDIO_SampleRate_e)RATE;
	attr.eBitwidth = E_MI_AUDIO_BIT_WIDTH_16;
	attr.eWorkmode = E_MI_AUDIO_MODE_I2S_MASTER;
	attr.eSoundmode = E_MI_AUDIO_SOUND_MODE_STEREO;
	attr.u32FrmNum = 6;
	attr.u32PtNumPerFrm = FRAMES;
	attr.u32ChnCnt = 2;
	e1 = p_MI_AO_SetPubAttr(AO_DEV, &attr);
	e2 = e1 == 0 ? p_MI_AO_Enable(AO_DEV) : -1;
	e3 = e2 == 0 ? p_MI_AO_EnableChn(AO_DEV, AO_CHN) : -1;
	if (e1 || e2 || e3) {
		fprintf(stderr, "gtaaudio: MI_AO setup failed (%#x %#x %#x): is the audioserver running?\n",
		        (unsigned)e1, (unsigned)e2, (unsigned)e3);
		return -1;
	}
	vol = system_volume_db();
	mute = vol <= -60 || access("/mnt/SDCARD/.tmp_update/config/.muteVolume", F_OK) == 0;
	if (p_MI_AO_SetVolume && p_MI_AO_SetVolume(AO_DEV, vol) != 0)
		fprintf(stderr, "gtaaudio: could not set volume\n");
	if (p_MI_AO_SetMute && p_MI_AO_SetMute(AO_DEV, mute) != 0)
		fprintf(stderr, "gtaaudio: could not set mute\n");
	/* what the driver reports, for launch.log: the speaker level the game gets */
	if (p_MI_AO_GetVolume && p_MI_AO_GetVolume(AO_DEV, &vol) != 0)
		vol = -99;
	if (p_MI_AO_GetMute && p_MI_AO_GetMute(AO_DEV, &mute) != 0)
		mute = FALSE;
	fprintf(stderr, "gtaaudio: MI_AO at %d Hz, volume %d dB%s\n", RATE, (int)vol, mute ? ", muted" : "");
	return 0;
}

/* reads exactly n bytes; 0 at end of input */
static int read_full(void *buf, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(0, (char *)buf + got, n - got);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0)
			return 0;
		got += (size_t)r;
	}
	return 1;
}

int main(void)
{
	static int16_t buf[FRAMES * 2];
	int ao = mi_ao_open() == 0;
	struct timespec next;
	unsigned long chunks = 0, active_chunks = 0;
	int peak = 0;

	clock_gettime(CLOCK_MONOTONIC, &next);
	while (read_full(buf, sizeof buf)) {
		chunks++;
		int active = 0;
		for (unsigned i = 0; i < FRAMES * 2; i++) {
			int level = buf[i] < 0 ? -(int)buf[i] : buf[i];
			if (level > peak) peak = level;
			active |= level != 0;
		}
		active_chunks += active;
		if (ao) {
			MI_AUDIO_Frame_t frame;
			memset(&frame, 0, sizeof frame);
			frame.eBitwidth = E_MI_AUDIO_BIT_WIDTH_16;
			frame.eSoundmode = E_MI_AUDIO_SOUND_MODE_STEREO;
			frame.u32Len = sizeof buf;
			frame.apVirAddr[0] = buf;
			MI_S32 error = 0;
			int tries;
			for (tries = 0; tries < 20; tries++) {
				error = p_MI_AO_SendFrame(AO_DEV, AO_CHN, &frame, 100);
				if (!error) break;
				usleep(2000);
			}
			if (error) {
				fprintf(stderr, "gtaaudio: SendFrame failed repeatedly (%#x); draining audio\n", (unsigned)error);
				if (p_MI_AO_DisableChn) p_MI_AO_DisableChn(AO_DEV, AO_CHN);
				if (p_MI_AO_Disable) p_MI_AO_Disable(AO_DEV);
				ao = 0;
				clock_gettime(CLOCK_MONOTONIC, &next);
			}
		} else {
			/* no device: keep real-time pace so the game's music timing holds */
			next.tv_nsec += (long)(1000000000LL * FRAMES / RATE);
			while (next.tv_nsec >= 1000000000L) {
				next.tv_nsec -= 1000000000L;
				next.tv_sec++;
			}
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		}
	}
	if (ao) {
		if (p_MI_AO_DisableChn) p_MI_AO_DisableChn(AO_DEV, AO_CHN);
		if (p_MI_AO_Disable) p_MI_AO_Disable(AO_DEV);
	}
	fprintf(stderr, "gtaaudio: played %.1f s, non-silent %.1f s, PCM peak %d/32768\n",
	        chunks * FRAMES / (double)RATE, active_chunks * FRAMES / (double)RATE, peak);
	return 0;
}
