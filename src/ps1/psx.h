/* PS1 platform layer for the Duke Nukem 3D port (PSn00bSDK). */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;

#define SCREEN_W 320   /* logical width: what the game and the engine think the screen is (xdim) */
#define SCREEN_H 240
#define FB_W 256       /* PS1: physical frame buffer / display width (256-pixel GPU mode; render.c scales x by FB_W/SCREEN_W) */

/* ---- video.c: display, double buffering, primitive list ---- */
void video_init(void);
extern u8 *prim_cur;                /* next free byte in this frame's primitive buffer */
extern u8 *prim_end;
extern u32 *prim_tail;              /* last linked primitive (list is drawn in call order) */
extern int video_dropped_prims;
static inline void *prim_alloc(int bytes)   /* NULL if the frame's primitive buffer is full */
{
	u8 *p = prim_cur;
	bytes = (bytes + 3) & ~3;
	if (p + bytes > prim_end) { video_dropped_prims++; return NULL; }
	prim_cur = p + bytes;
	return p;
}
static inline void prim_link(void *prim)    /* append to this frame's draw list */
{
	((P_TAG *)prim)->addr = 0xffffff;
	((P_TAG *)prim_tail)->addr = (u32)prim;
	prim_tail = (u32 *)prim;
}
void video_flip(void);              /* submit list, swap buffers (waits for GPU + vsync) */
void video_wait_gpu(void);          /* wait until the GPU queue is empty */
void video_represent(int on, int abr, int r, int g, int b);  /* draw the last presented list again with a tint overlay (see video.c) */
void video_mark_overlay(void);      /* call right before appending the frame's tint overlay (see video_represent) */
int video_frame(void);              /* frames flipped so far */
int video_list_bytes(void);         /* bytes used in the current list (stats) */
extern DRAWENV *video_drawenv;      /* current draw environment (for clip rect) */
int video_vsync_count(void);        /* vblanks since boot */

/* ---- pak.c: DUKE.DAT access on CD ---- */
typedef struct {
	char name[16];
	u32 sector;   /* relative to DUKE.DAT start, 2048-byte units */
	u32 size;     /* bytes */
	u32 aux;
} PakEntry;

int pak_init(void);
const PakEntry *pak_find(const char *name);
int pak_read(u32 sector, int nsectors, void *dst);           /* synchronous, dst must hold nsectors*2048 */
/* Asynchronous read; done(ok) runs in IRQ context: ok = 1 data valid, 0 = CD error or timeout (the read has been
   cancelled and the drive is free again).  Returns 0 if the drive is busy or the read could not be started
   (done is not called then). */
int pak_read_async(u32 sector, int nsectors, void *dst, void (*done)(int ok));
/* PS1: optional main-thread progress hook (loading screen): called by the synchronous pak_read() after each
   successful read with the number of bytes read.  NULL when unused. */
extern void (*pak_progress)(unsigned long bytes);
int pak_busy(void);
void pak_tick(void);                /* vblank IRQ: async read timeout watchdog */
int pak_read_bytes(const PakEntry *e, u32 offset, void *dst, u32 len); /* synchronous, arbitrary byte ranges via bounce */

/* ---- spu_music.c: silence the sequencer without losing its position (memory card access) ---- */
void music_hold(void);
void music_resume(void);

/* ---- pad.c ---- */
typedef struct {
	u16 btn;        /* PAD_* bits, 1 = pressed */
	u16 pressed;    /* newly pressed this poll */
	u16 released;
	u8 analog;      /* 1 = analog sticks valid */
	s8 lx, ly, rx, ry; /* -128..127, 0 = centred, deadzone applied */
} PadState;
void pad_init(void);
void pad_poll(void);
extern PadState pad;

/* ---- timer ---- */
extern volatile long totalclock;    /* 120 Hz tick counter (Build TICRATE) */

/* ---- misc ---- */
void fatal(const char *fmt, ...) __attribute__((noreturn));
#define dbg printf

/* ---- profiling (hblank counter, ~63.5us per tick NTSC) ---- */
#include <hwregs_c.h>
static inline void prof_init(void) { TIMER_CTRL(1) = 0x0100; }
static inline unsigned prof_now(void) { return TIMER_VALUE(1) & 0xffff; }
#define PROF_MS(dt) (((dt) & 0xffff) * 635 / 10000)
