#include <stdlib.h>
#include <psxapi.h>
#include <psxpad.h>
#include "psx.h"
#include "build.h"
#include "pragmas.h"
#include "kfile.h"
#include "texcache.h"
#include "palette.h"

/* Optional screenshot-test poses: an out-of-tree viewer_poses.h (add its directory with -I) defining
   static const long viewer_poses[][4] = {{x, y, z, ang}, ...}; START cycles through them. */
#if defined(__has_include)
#if __has_include("viewer_poses.h")
#include "viewer_poses.h"
#define HAVE_POSES
#endif
#endif

/* Engine viewer: loads a map and lets the pad fly around. Replaced by the game's main later. */

static long posx, posy, posz, horiz = 100;
static short ang, cursect;

static void preload_map_tiles(void)
{
	int i;
	for (i = 0; i < numsectors; i++) {
		if ((i & 63) == 0) dbg("preload sector %d res %d\n", i, tex_stat_resident);
		loadtile(sector[i].ceilingpicnum);
		loadtile(sector[i].floorpicnum);
	}
	dbg("sectors done\n");
	for (i = 0; i < numwalls; i++) {
		loadtile(wall[i].picnum);
		if (wall[i].overpicnum >= 0 && wall[i].overpicnum < MAXTILES) loadtile(wall[i].overpicnum);
	}
	dbg("walls done\n");
	for (i = 0; i < MAXSPRITES; i++)
		if (sprite[i].statnum < MAXSTATUS && sprite[i].picnum > 0) loadtile(sprite[i].picnum);
}

int main(void)
{
	int frame = 0;
	long t0, tick;
	char mapname[16] = "E1L1.MAP";

	video_init();
	prof_init();
	pad_init();
	pak_init();
	initengine();
	loadpics("tiles000.art");
	setgamemode(0, 320, 240);
	dbg("engine up: picsiz[89]=%d >>4=%d tilesizy=%d sizeof(char)=%d (char)200=%d\n", picsiz[89], picsiz[89]>>4, tilesizy[89], (int)sizeof(char), (int)(char)200);

	if (loadboard(mapname, &posx, &posy, &posz, &ang, &cursect) < 0) fatal("loadboard failed");
	dbg("map: %d sectors %d walls, start %ld,%ld,%ld sect %d\n", numsectors, numwalls, posx, posy, posz, cursect);
	t0 = totalclock;
	preload_map_tiles();
	dbg("preloaded %d tiles in %ld ticks\n", tex_stat_resident, totalclock - t0);
	posz -= 32 << 8; /* eye height */
#ifdef HAVE_POSES
	{ posx = viewer_poses[0][0]; posy = viewer_poses[0][1]; posz = viewer_poses[0][2]; ang = viewer_poses[0][3]; updatesector(posx, posy, &cursect); }
#endif

	for (;;) {
		pad_poll();
#ifdef HAVE_POSES
		if (pad.pressed & PAD_START) {
			static int pose;
			pose = (pose + 1) % (int)(sizeof(viewer_poses) / sizeof(viewer_poses[0]));
			posx = viewer_poses[pose][0]; posy = viewer_poses[pose][1]; posz = viewer_poses[pose][2]; ang = viewer_poses[pose][3];
			updatesector(posx, posy, &cursect);
		}
#endif
		if (pad.btn & PAD_LEFT) ang -= 24;
		if (pad.btn & PAD_RIGHT) ang += 24;
		ang &= 2047;
		{
			long dx = 0, dy = 0;
			if (pad.btn & PAD_UP) { dx += sintable[(ang + 512) & 2047] >> 3; dy += sintable[ang & 2047] >> 3; }
			if (pad.btn & PAD_DOWN) { dx -= sintable[(ang + 512) & 2047] >> 3; dy -= sintable[ang & 2047] >> 3; }
			if (pad.btn & PAD_SQUARE) { dx += sintable[ang & 2047] >> 3; dy -= sintable[(ang + 512) & 2047] >> 3; }
			if (pad.btn & PAD_CIRCLE) { dx -= sintable[ang & 2047] >> 3; dy += sintable[(ang + 512) & 2047] >> 3; }
			if (pad.analog) { dx += (pad.ly * -(sintable[(ang + 512) & 2047] >> 7)); dy += (pad.ly * -(sintable[ang & 2047] >> 7)); ang += pad.rx >> 2; }
			if (dx || dy) {
				clipmove(&posx, &posy, &posz, &cursect, dx << 14, dy << 14, 164L, 4L << 8, 4L << 8, CLIPMASK0);
				updatesector(posx, posy, &cursect);
			}
			if (pad.btn & PAD_L1) horiz += 2;
			if (pad.btn & PAD_R1) horiz -= 2;
			if (pad.btn & PAD_TRIANGLE) posz -= 1024;
			if (pad.btn & PAD_CROSS) posz += 1024;
			{
				long cz, fz;
				getzsofslope(cursect, posx, posy, &cz, &fz);
				if (posz > fz - (32 << 8)) posz = fz - (32 << 8);
				if (posz < cz + (4 << 8)) posz = cz + (4 << 8);
			}
		}
		{
			unsigned t0 = prof_now(), t1, t2, t3;
			int bytes;
			drawrooms(posx, posy, posz, ang, horiz, cursect);
			t1 = prof_now();
			drawmasks();
			t2 = prof_now();
			/* 2D test: status bar (multi-piece 320x34), a rotated/scaled item, a translucent one */
			rotatesprite(0, (200-34)<<16, 65536L, 0, 2462, 4, 0, 10+16, 0, 0, xdim-1, ydim-1);
#ifndef HAVE_POSES   /* animated test items off for deterministic screenshots */
			rotatesprite(60<<16, 40<<16, 65536L + (frame & 255) * 256, (frame * 8) & 2047, 21, 0, 0, 0, 0, 0, xdim-1, ydim-1);
#endif
			rotatesprite(260<<16, 40<<16, 65536L, 0, 23, 0, 0, 1, 0, 0, xdim-1, ydim-1);
#ifdef HAVE_POSES
			{	/* 1:1 glyph rows for pixel comparison against the ART tiles: HUD digits, menu font, minifont */
				int k;
				for (k = 0; k < 10; k++) rotatesprite((8 + k * 12) << 16, 8 << 16, 65536L, 0, 2930 + k, 0, 0, 8 + 16, 0, 0, xdim - 1, ydim - 1);
				for (k = 0; k < 8; k++) rotatesprite((8 + k * 20) << 16, 24 << 16, 65536L, 0, 2854 + k, 0, 0, 8 + 16, 0, 0, xdim - 1, ydim - 1);
				for (k = 0; k < 12; k++) rotatesprite((8 + k * 8) << 16, 50 << 16, 65536L, 0, 2504 + k, 0, 0, 8 + 16, 0, 0, xdim - 1, ydim - 1);
			}
#endif
#ifndef HAVE_POSES
			if ((frame & 127) > 100) pal_set_tint(63, 0, 0, (frame & 127) - 100); else pal_set_tint(0, 0, 0, 0);
#endif
			bytes = video_list_bytes();
			nextpage();
			t3 = prof_now();
			if ((frame & 63) == 0)
				dbg("f%d pos %ld,%ld,%ld a%d s%d | prims %dB drop %d | tex res %d miss %d loads %d | rooms %u masks %u flip %u (x0.0635ms)\n", frame, posx, posy, posz, ang, cursect,
					bytes, video_dropped_prims, tex_stat_resident, tex_stat_misses, tex_stat_loads, (t1-t0)&0xffff, (t2-t1)&0xffff, (t3-t2)&0xffff);
		}
		frame++;
	}
	return 0;
}
