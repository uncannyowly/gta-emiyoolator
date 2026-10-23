#include <assert.h>
#include <sys/syscall.h>
#include <unistd.h>
static int sync_count;
static int counted_fsync(int fd) { sync_count++; return syscall(SYS_fsync, fd); }
#define fsync counted_fsync
#include "../src/kernel32.c"
#undef fsync
void shell_log(const char *fmt, ...) {}
void mapview_load(const char *p) {}
char *files_host_path(const char *p, char *out, size_t n) { snprintf(out,n,"%s",p); return out; }
int main(void)
{
	DWORD written;
	const char *p = "build/tests/PLAYER_A.DAT";
	HANDLE h = k32_CreateFileA(p, 0x40000000u, 0, NULL, 2, 0, NULL);
	assert(h != INVALID_HANDLE_VALUE);
	assert(k32_WriteFile(h, "test", 4, &written, NULL) && written == 4);
	assert(k32_FlushFileBuffers(h) && sync_count == 1);
	assert(k32_CloseHandle(h) && sync_count == 2);
	FILE *f = fopen(p, "rb"); assert(f); char b[4]; assert(fread(b,1,4,f)==4 && !memcmp(b,"test",4)); fclose(f);
	h = k32_CreateFileA(p, 0x80000000u, 0, NULL, 3, 0, NULL);
	assert(k32_CloseHandle(h) && sync_count == 2);
	assert(!k32_FlushFileBuffers(INVALID_HANDLE_VALUE));
	puts("progress: profile bytes persist, explicit flush and close sync, read-only handles passed");
}
