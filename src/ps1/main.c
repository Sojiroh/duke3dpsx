#include "psx.h"

extern int duke_main(void);   /* src/game/game.c */

int main(void)
{
	video_init();
	prof_init();
	pad_init();
	pak_init();
	return duke_main();
}
