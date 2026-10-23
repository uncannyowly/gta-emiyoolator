#include <assert.h>
#define main gtaaudio_main
#include "../tools/gtaaudio.c"
#undef main
int main(void)
{
	assert(volume_db(-1) == -60 && volume_db(0) == -60);
	assert(volume_db(1) == -46 && volume_db(3) == -31);
	assert(volume_db(10) == -10 && volume_db(20) == 3 && volume_db(99) == 3);
	for (int i = 1; i <= 20; i++) assert(volume_db(i) > volume_db(i - 1));
	puts("volume: Onion curve, mute level and range clamping passed");
}
