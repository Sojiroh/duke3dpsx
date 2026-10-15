#include <stdarg.h>
#include <stdlib.h>
#include "psx.h"

#define PRIMBUF_SIZE (56 * 1024)

static DISPENV disp[2];
static DRAWENV draw[2];
static u8 primbuf[2][PRIMBUF_SIZE] __attribute__((aligned(16)));
static u32 othead[2];
u8 *prim_cur, *prim_end;
u32 *prim_tail;
static int cur;
int video_framecount;
static volatile int vsyncs;
int video_dropped_prims;
DRAWENV *video_drawenv;
volatile long totalclock;

static void vsync_cb(void)
{
	vsyncs++;
	totalclock += 2; /* 59.94 Hz * 2 ~= Build's 120 Hz TICRATE */
	pak_tick();      /* async CD read watchdog (pak.c) */
}

static u32 *overlay_tail;   /* list tail just before the frame's full-screen tint overlay (NULL = none marked) */

static void begin_frame(void)
{
	othead[cur] = 0x00ffffff; /* empty list terminator */
	prim_tail = &othead[cur];
	prim_cur = primbuf[cur];
	prim_end = primbuf[cur] + PRIMBUF_SIZE;
	video_drawenv = &draw[cur];
	overlay_tail = NULL;
}

/* The renderer calls this right before it appends the tint overlay: a re-presented list replays
   only what precedes it (the overlay of that frame is replaced by the current tint). */
void video_mark_overlay(void) { overlay_tail = prim_tail; }

void video_init(void)
{
	ResetGraph(0);
	SetDefDispEnv(&disp[0], 0, 0, FB_W, SCREEN_H);
	SetDefDispEnv(&disp[1], 0, SCREEN_H, FB_W, SCREEN_H);
	SetDefDrawEnv(&draw[0], 0, SCREEN_H, FB_W, SCREEN_H);
	SetDefDrawEnv(&draw[1], 0, 0, FB_W, SCREEN_H);
	draw[0].isbg = draw[1].isbg = 0;
	draw[0].dtd = draw[1].dtd = 1; /* dithering on */
	PutDispEnv(&disp[0]);
	PutDrawEnv(&draw[0]);
	SetDispMask(1);
	VSyncCallback(vsync_cb);
	cur = 0;
	begin_frame();
}

void video_wait_gpu(void) { DrawSync(0); }

static u32 *last_head, *last_tail;

/* Draw the last submitted list (without its tint overlay) into the buffer of draw environment `env`,
   followed by a full-screen tint quad when `on` (abr 1 = additive, 2 = subtractive; r,g,b 0..255).
   `env` MUST be the environment the list was built for: the list contains DR_AREA/DR_OFFSET
   primitives with absolute VRAM coordinates of that buffer.  The tint tail restores the full draw
   area and the buffer's origin first, because the list may end inside a clipped area or with the
   3D window offset still set. */
static void replay(DRAWENV *env, int on, int abr, int r, int g, int b)
{
	static DR_AREA area;
	static DR_TPAGE tp;
	static DR_OFFSET off;
	static POLY_F4 quad;
	u32 saved = ((P_TAG *)last_tail)->addr;
	if (on) {
		setDrawArea(&area, &env->clip);
		setDrawTPage(&tp, 0, 1, getTPage(0, abr, 0, 0));
		setDrawOffset(&off, env->clip.x + env->ofs[0], env->clip.y + env->ofs[1]);
		setPolyF4(&quad);
		setRGB0(&quad, r, g, b);
		setXY4(&quad, 0, 0, FB_W, 0, 0, SCREEN_H, FB_W, SCREEN_H);
		setSemiTrans(&quad, 1);
		((P_TAG *)&area)->addr = (u32)&tp;
		((P_TAG *)&tp)->addr = (u32)&off;
		((P_TAG *)&off)->addr = (u32)&quad;
		((P_TAG *)&quad)->addr = 0xffffff;
		((P_TAG *)last_tail)->addr = (u32)&area;
	} else {
		((P_TAG *)last_tail)->addr = 0xffffff;
	}
	PutDrawEnv(env);
	DrawOTag(last_head);
	DrawSync(0);
	((P_TAG *)last_tail)->addr = saved;
}

/* Present the last submitted frame again with a new tint (palette fades on static screens), once per
   vblank, without consuming the list being built.  video_flip() draws a list into draw[cur] and
   shows disp[cur], i.e. the buffer of the frame submitted *before*; the last list was built for the
   buffer that the next flip will show (draw[cur ^ 1]).  So it is replayed once into exactly that
   buffer (the one the list's absolute coordinates address) with the tint appended, and the display is
   switched to it right away.  `cur` is unchanged and consistent: the next flip draws its list into
   draw[cur] (the buffer that is not on screen now) and displays disp[cur], which is this buffer. */
void video_represent(int on, int abr, int r, int g, int b)
{
	if (!last_head) return;
	DrawSync(0);
	VSync(0);
	replay(&draw[cur ^ 1], on, abr, r, g, b);
	PutDispEnv(&disp[cur]);
}

#ifdef PS1_PROF
int video_validate = 0;

/* Debug: walk the display list and report primitives the GPU could choke on. */
static void validate_list(void)
{
	u32 *tag = &othead[cur];
	int n = 0;
	u8 *lo = primbuf[cur], *hi = primbuf[cur] + PRIMBUF_SIZE;
	for (;;) {
		u32 addr = *tag & 0xffffff;
		u32 len;
		u8 *p;
		if (addr == 0xffffff) break;
		p = (u8 *)(0x80000000 | addr);
		if (p < lo || p >= hi || ((u32)p & 3)) { printf("BADLIST: prim %d at %p outside buffer\n", n, p); break; }
		if (++n > 4000) { printf("BADLIST: cycle or too long\n"); break; }
		tag = (u32 *)p;
		len = *tag >> 24;
		{
			u32 code = tag[1] >> 24;
			int ok;
			switch (code & 0xfc) {
			case 0x20: case 0x24: ok = (len == 4 || len == 7); break;      /* F3/FT3 */
			case 0x28: case 0x2c: ok = (len == 5 || len == 9); break;      /* F4/FT4 */
			case 0x30: case 0x34: ok = (len == 6 || len == 9); break;      /* G3/GT3 */
			case 0x38: case 0x3c: ok = (len == 8 || len == 12); break;     /* G4/GT4 */
			case 0x40: case 0x44: ok = (len == 3); break;                  /* line */
			case 0x60: case 0x64: ok = (len == 3 || len == 4); break;      /* tile/sprite */
			case 0xe0: case 0xe4: ok = (len == 1 || len == 2); break;      /* env */
			default: ok = (code >= 0xe0 && code <= 0xe6 && len <= 2); break;
			}
			if (!ok) {
				static int dumped;
				printf("BADLIST: prim %d code %02x len %lu\n", n, (unsigned)code, (unsigned long)len);
				if (dumped < 4) {
					int k;
					dumped++;
					printf("  @%p:", p);
					for (k = 0; k < 12; k++) printf(" %08lx", (unsigned long)tag[k]);
					printf("\n");
				}
			}
		}
	}
}

unsigned prof_gwait, prof_vwait;   /* ticks (hblank) spent waiting for the GPU / the vblank in video_flip */
unsigned prof_dbg_ticks;           /* ticks (hblank) spent in dbg() output (psx.h) since game.c's frame profiler last cleared it */
#endif

void video_flip(void)
{
#ifdef PS1_PROF
	if (video_validate) validate_list();
	{ unsigned t0 = prof_now(), t1;
	  DrawSync(0);
	  t1 = prof_now(); prof_gwait += (t1 - t0) & 0xffff;
	  VSync(0);
	  prof_vwait += (prof_now() - t1) & 0xffff; }
#else
	DrawSync(0);
	VSync(0);
#endif
	PutDrawEnv(&draw[cur]);
	PutDispEnv(&disp[cur]);
	DrawOTag(&othead[cur]);
	last_head = &othead[cur]; last_tail = overlay_tail ? overlay_tail : prim_tail;
	video_framecount++;
	cur ^= 1;
	begin_frame();
}

int video_frame(void) { return video_framecount; }
int video_list_bytes(void) { return prim_cur - primbuf[cur]; }
int video_vsync_count(void) { return vsyncs; }

void fatal(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char buf[160];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	printf("FATAL: %s\n", buf);
	va_end(ap);
	for (;;) VSync(0);
}
