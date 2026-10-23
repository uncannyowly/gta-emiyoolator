/* Check that all PCM formats survive a refill boundary without dropped frames. */
#include <assert.h>
#include "../src/mss32.c"
int config_int(const char *k, int d) { return d; }
const char *config_str(const char *k, const char *d) { return d; }
uint32_t shell_ms(void) { return 0; }
void shell_log(const char *fmt, ...) {}
char *files_host_path(const char *p, char *out, size_t n) { snprintf(out, n, "%s", p); return out; }

int main(void)
{
	for (int bits = 8; bits <= 16; bits += 8) for (int ch = 1; ch <= 2; ch++) {
		Stream s = {0};
		s.f = tmpfile(); assert(s.f);
		s.bits = bits; s.channels = ch; s.rate = RATE; s.volume = 127; s.loops = 1;
		s.status = SMP_PLAYING; s.data_len = 5000 * ch * bits / 8;
		for (int i = 0; i < 5000; i++) for (int c = 0; c < ch; c++) {
			if (bits == 8) fputc(128 + (i % 100), s.f);
			else { int16_t v = i; fwrite(&v, 2, 1, s.f); }
		}
		fflush(s.f);
		for (int i = 0; i < 5000; i++) {
			int32_t out[2] = {0}; mix_stream(&s, out, 1);
			int want = bits == 8 ? (i % 100) * 256 : i;
			assert(out[0] == want && out[1] == want);
		}
		int32_t out[2] = {0}; mix_stream(&s, out, 1); assert(s.status == SMP_DONE);
		fclose(s.f);
	}
	puts("audio: 5000 contiguous frames in mono/stereo, 8/16-bit PCM passed");
}
