#include "psx.h"

extern int duke_main(void);   /* src/game/game.c */

/* PS1: stack high-water probe (profiling builds only; -DPS1_NOSTACKPROBE=1 turns it off so frame-time
   measurements are not disturbed).  main() paints the free RAM between the end of .bss (_end) and the
   stack with a pattern; once per second (60 vsyncs, checked at each video_flip, in the main thread) the first word the
   stack has overwritten is looked up and the untouched size printed.  video_flip is hooked with the
   linker (-Wl,--wrap=video_flip, added by CMakeLists.txt when PS1_PROF is in EXTRA_CFLAGS): the game
   sources are not touched, and nothing runs in an IRQ context (the SDK's 4 KB IRQ stack and the BIOS
   printf are not safe for a probe that prints from an interrupt).  The SDK heap (sbrk from _end, only
   used by CdSearchFile and the sound loader) lives at the bottom of the same region, so the scan starts
   above the heap reservation reported by GetHeapUsage(). */
#ifdef PS1_PROF
extern void __real_video_flip(void);

#ifndef PS1_NOSTACKPROBE
#include <stdlib.h>

extern char _end[];
#define SP_PAT 0xA5C3A5C3u
#define SP_TOP 0x80200000u

static void stack_report(void)
{
	static unsigned lowest = SP_TOP;   /* lowest stack address ever seen touched */
	HeapUsage hu;
	volatile u32 *p, *end = (volatile u32 *)(SP_TOP - 64);
	unsigned start;
	GetHeapUsage(&hu);
	start = ((unsigned)_end + hu.heap + 16 + 3) & ~3u;   /* +16: malloc's end-of-heap marker words */
	for (p = (volatile u32 *)start; p < end && *p == SP_PAT; p++) ;
	if ((unsigned)p < lowest) lowest = (unsigned)p;
	printf("PROF stack: peak use %u B (lowest touched 0x%08x), free above heap %u B (_end=0x%08x, heap+16 = 0x%08x)\n",
	       SP_TOP - lowest, lowest, lowest - start, (unsigned)_end, start);
}

static void stack_probe_init(void)
{
	unsigned sp;
	volatile u32 *p;
	__asm__ volatile("move %0, $sp" : "=r"(sp));
	/* paint [_end, sp - 256): the live frames of main()/init above are left alone */
	for (p = (volatile u32 *)(((unsigned)_end + 3) & ~3u); (unsigned)p < sp - 256; p++) *p = SP_PAT;
}

void __wrap_video_flip(void)
{
	static int last;
	__real_video_flip();
	if (video_vsync_count() - last >= 60) { last = video_vsync_count(); stack_report(); }
}
#else
void __wrap_video_flip(void) { __real_video_flip(); }
#define stack_probe_init()
#endif
#else
#define stack_probe_init()
#endif

int main(void)
{
	stack_probe_init();
	video_init();
	prof_init();
	pad_init();
	pak_init();
	return duke_main();
}
