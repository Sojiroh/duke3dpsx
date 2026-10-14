// Build engine, PS1 port. Globals + initialisation + tile/palette loading.
// The visibility/collision core is included verbatim from parts/engine_core.c
// and the GPU rasteriser from parts/render.c (single translation unit so the
// original file-static globals stay shared).
#define ENGINE
#include <stdlib.h>
#include "psx.h"
#include "build.h"
#include "pragmas.h"
#include "kfile.h"
#include "texcache.h"
#include "palette.h"
#include "data_index.h"

#define MAXCLIPNUM 512
#define MAXPERMS 512
#define MAXWALLSB 1024
#define MAXCLIPDIST 1024
#define MAXYSAVES 8192
#define MAXNODESPERLINE 42   //Warning: This depends on MAXYSAVES & MAXYDIM!

long dmval;
#ifdef PS1_PROF
/* PS1 debug: per-section engine timers (hblank ticks, TIMER1) / event counters printed by game.c's PF_REPORT */
unsigned prof_drawalls, prof_scan, prof_most, prof_bunch, prof_prep, prof_sprite, prof_nsprites,
         prof_wall, prof_plane, prof_nwall, prof_nplane, prof_nquad;
unsigned pc_wcols, pc_wstrips, pc_pcols, pc_pstrips, pc_pverts, pc_spr, pc_sprcols;
#define PROF_BEGIN(t)    unsigned t = prof_now()
#define PROF_END(acc, t) ((acc) += (prof_now() - (t)) & 0xffff)
#define PROF_COUNT(c, n) ((c) += (n))
#else
#define PROF_BEGIN(t)
#define PROF_END(acc, t)
#define PROF_COUNT(c, n)
#endif
long transarea = 0, totalarea = 0, beforedrawrooms = 1;
static long oxdimen = -1, oviewingrange = -1, oxyaspect = -1;
static long curbrightness = 0;

static unsigned char tempbuf[MAXWALLS];
long artversion, mapversion;
char picsiz[MAXTILES];
unsigned short tilesect[MAXTILES];   /* piece 0 sector, relative to TILEDATA */
unsigned short tileavg[MAXTILES];    /* average colour (15-bit, bit 15 set) */
unsigned char tilenpieces[MAXTILES];
unsigned char tileflags[MAXTILES];     /* bit 0: half-res copy available (sector in tileaux) */
unsigned short tileaux[MAXTILES];      /* half-res sector, or first PIECES index for multi-piece tiles */

short radarang[1280];
static short radarang2[MAXXDIM];
static unsigned short sqrtable[4096], shlookup[4096+256];
char pow2char[8] = {1,2,4,8,16,32,64,128};
long pow2long[32] =
{
	1L,2L,4L,8L,
	16L,32L,64L,128L,
	256L,512L,1024L,2048L,
	4096L,8192L,16384L,32768L,
	65536L,131072L,262144L,524288L,
	1048576L,2097152L,4194304L,8388608L,
	16777216L,33554432L,67108864L,134217728L,
	268435456L,536870912L,1073741824L,2147483647L,
};
long reciptable[2048];
char britable[16][64];
unsigned char textfont[1024], smalltextfont[1024];

static long xb1[MAXWALLSB], yb1[MAXWALLSB], xb2[MAXWALLSB], yb2[MAXWALLSB];
static long rx1[MAXWALLSB], ry1[MAXWALLSB], rx2[MAXWALLSB], ry2[MAXWALLSB];
static short p2[MAXWALLSB], thesector[MAXWALLSB], thewall[MAXWALLSB];
static short bunchfirst[MAXWALLSB], bunchlast[MAXWALLSB];

static short smost[MAXYSAVES], smostcnt;
static short smoststart[MAXWALLSB];
static char smostwalltype[MAXWALLSB];
static long smostwall[MAXWALLSB], smostwallcnt = -1L;

static short maskwall[MAXWALLSB], maskwallcnt;
static long spritesx[MAXSPRITESONSCREEN];
static long spritesy[MAXSPRITESONSCREEN+1];
static long spritesz[MAXSPRITESONSCREEN];
static spritetype *tspriteptr[MAXSPRITESONSCREEN];

short umost[MAXXDIM], dmost[MAXXDIM];
static short bakumost[MAXXDIM], bakdmost[MAXXDIM];
short uplc[MAXXDIM], dplc[MAXXDIM];
static short uwall[MAXXDIM], dwall[MAXXDIM];
static long swplc[MAXXDIM], lplc[MAXXDIM];
static long swall[MAXXDIM], lwall[MAXXDIM+4];
long xdimen = -1, xdimenrecip, halfxdimen, xdimenscale, xdimscale;
long wx1, wy1, wx2, wy2, ydimen;
long viewoffset, frameoffset;

static long rxi[8], ryi[8], rzi[8], rxi2[8], ryi2[8], rzi2[8];
static long xsi[8], ysi[8];
static long horizlookup[MAXYDIM*4], horizlookup2[MAXYDIM*4], horizycent;

long globalposx, globalposy, globalposz, globalhoriz;
short globalang, globalcursectnum;
long globalpal, cosglobalang, singlobalang;
long cosviewingrangeglobalang, sinviewingrangeglobalang;
char *globalpalwritten;
long globaluclip, globaldclip, globvis;
long globalvisibility, globalhisibility, globalpisibility, globalcisibility;
char globparaceilclip, globparaflorclip;

long xyaspect, viewingrangerecip;

long asm1, asm2, asm3, asm4;
char globalxshift, globalyshift;
long globalxpanning, globalypanning, globalshade;
short globalpicnum, globalshiftval;
long globalzd, globalbufplc, globalyscale, globalorientation;
long globalx1, globaly1, globalx2, globaly2, globalx3, globaly3, globalzx;
long globalx, globaly, globalz;

static short sectorborder[256], sectorbordercnt;
static char tablesloaded = 0;
long pageoffset, ydim16, qsetmode = 0;
long startposx, startposy, startposz;
short startang, startsectnum;
short pointhighlight, linehighlight, highlightcnt;
static long lastx[MAXYDIM];
char paletteloaded = 0;

static short clipnum, hitwalls[4];
long hitscangoalx = (1<<29)-1, hitscangoaly = (1<<29)-1;

typedef struct { long x1, y1, x2, y2; } linetype;
static linetype clipit[MAXCLIPNUM];
static short clipsectorlist[MAXCLIPNUM], clipsectnum;
static short clipobjectval[MAXCLIPNUM];

static long permhead = 0, permtail = 0;

short numscans, numhits, numbunches;
static short posfil, capturecount = 0, hitcnt;

short editstatus = 0;
short searchit;
long searchx = -1, searchy;                          //search input
short searchsector, searchwall, searchstat;     //search output

static char inpreparemirror = 0;
static long mirrorsx1, mirrorsy1, mirrorsx2, mirrorsy2;

long totalclocklock;
long clipmoveboxtracenum = 3;

#define addclipline(dax1, day1, dax2, day2, daoval)      \
{                                                        \
	clipit[clipnum].x1 = dax1; clipit[clipnum].y1 = day1; \
	clipit[clipnum].x2 = dax2; clipit[clipnum].y2 = day2; \
	clipobjectval[clipnum] = daoval;                      \
	clipnum++;                                            \
}

/* forward decls for things used by engine_core.c before definition */
static void rend_setview(void);
static void rend_begin_frame(void);
static void wallscan(long x1, long x2, short *uwal, short *dwal, long *swal, long *lwal);
static void maskwallscan(long x1, long x2, short *uwal, short *dwal, long *swal, long *lwal);
static void transmaskwallscan(long x1, long x2);
static void ceilspritescan(long x1, long x2);
static void dorotatesprite(long sx, long sy, long z, short a, short picnum, signed char dashade, char dapalnum, char dastat, long cx1, long cy1, long cx2, long cy2);
static long clippoly4(long cx1, long cy1, long cx2, long cy2);
long wallfront(long l1, long l2);
long spritewallfront(spritetype *s, long w);
long bunchfront(long b1, long b2);
void drawalls(long bunch);
void prepwall(long z, walltype *wal);
void ceilscan(long x1, long x2, long sectnum);
void florscan(long x1, long x2, long sectnum);
void grouscan(long dax1, long dax2, long sectnum, char dastat);
void parascan(long dax1, long dax2, long sectnum, char dastat, long bunch);
void scansector(short sectnum);
void drawsprite(long snum);
void drawmaskwall(short damaskwallcnt);
long owallmost(short *mostbuf, long w, long z);
long wallmost(short *mostbuf, long w, long sectnum, char dastat);
void keepaway(long *x, long *y, long w);
long raytrace(long x3, long y3, long *x4, long *y4);
void dosetaspect(void);
void faketimerhandler(void);
void initksqrt(void);
static void rend_end_frame(void);

static inline long nsqrtasm(unsigned long a)
{
	unsigned long cx;
	if (a & 0xff000000) cx = shlookup[(a>>24)+4096]; else cx = shlookup[a>>12];
	a >>= (cx & 0xff);
	return (long)(sqrtable[a & 4095] >> (cx >> 8));
}

static inline long msqrtasm(unsigned long c)
{
	unsigned long a = 0x40000000, b = 0x20000000;
	do {
		if (c >= a) { c -= a; a += b*4; }
		a -= b; a >>= 1; b >>= 2;
	} while (b);
	if (c >= a) a++;
	return (long)(a>>1);
}

static inline long krecipasm(long i)
{
	if (i == 0) return 0x7fffffff;
	return (1L<<30)/i;   /* 32-bit division: 2^30/i fits for any i != 0 */
}

static inline void settransnormal(void) {}
static inline void settransreverse(void) {}

static inline void setgotpic(long tilenume)
{
	if (walock[tilenume] < 200) walock[tilenume] = 199;
	gotpic[tilenume>>3] |= pow2char[tilenume&7];
}

/* ---------------------------------------------------------------------- */

void loadtables(void)
{
	long i, fil;

	if (tablesloaded == 0)
	{
		initksqrt();

		for(i=0;i<2048;i++) reciptable[i] = divscale30(2048L,i+2048);

		if ((fil = kopen4load("TABLES",0)) != -1)
		{
			kread(fil,sintable,2048*2);
			kread(fil,radarang,640*2);
			for(i=0;i<640;i++) radarang[1279-i] = -radarang[i];
			kread(fil,textfont,1024);
			kread(fil,smalltextfont,1024);
			kread(fil,britable,1024);
			kclose(fil);
		}
		else fatal("TABLES missing");
		tablesloaded = 1;
	}
}

void loadpalette(void)
{
	pal_init();
	memcpy(palette, pal_basepal(0), 768);
	numpalookups = 32;
	paletteloaded = 1;
}

void initengine(void)
{
	long i;

	loadtables();
	xyaspect = -1;
	pskyoff[0] = 0; pskybits = 0;
	parallaxtype = 2; parallaxyoffs = 0L; parallaxyscale = 65536;
	showinvisibility = 0;
	paletteloaded = 0;
	searchit = 0; searchstat = -1;
	for(i=0;i<MAXPALOOKUPS;i++) palookup[i] = NULL;
	clearbuf(&waloff[0],MAXTILES,0L);
	clearbuf(&show2dsector[0],(MAXSECTORS+3)>>5,0L);
	clearbuf(&show2dsprite[0],(MAXSPRITES+3)>>5,0L);
	clearbuf(&show2dwall[0],(MAXWALLS+3)>>5,0L);
	automapping = 0;
	pointhighlight = -1;
	linehighlight = -1;
	highlightcnt = 0;
	totalclock = 0;
	visibility = 512;
	parallaxvisibility = 512;
	numpages = 1;
	loadpalette();
}

void uninitengine(void) {}

long setgamemode(char davidoption, long daxdim, long daydim)
{
	long i;
	vidoption = davidoption;
	xdim = SCREEN_W; ydim = SCREEN_H;
	for(i=0;i<=ydim;i++) ylookup[i] = i*xdim;
	horizycent = ((ydim*4)>>1);
	setview(0L,0L,xdim-1,ydim-1);
	clearallviews(0L);
	setbrightness((char)curbrightness,&palette[0]);
	oxyaspect = oxdimen = oviewingrange = -1;
	qsetmode = 0;
	return 0;
}

void nextpage(void)
{
	rend_end_frame();
	numframes++;
	beforedrawrooms = 1;
}

/* ---- tiles ------------------------------------------------------------- */

long loadpics(char *filename)
{
	const PakEntry *e = pak_find("TILEIDX");
	unsigned char *buf = tex_staging();   /* 64 KB, unused until tex_init(): one bulk read instead of 32 seeks */
	long i, k;
	if (!e) fatal("TILEIDX missing");
	if (MAXTILES * 16 > 65536) fatal("TILEIDX larger than the staging buffer");
	numtiles = MAXTILES;
	if (pak_read(e->sector, (MAXTILES * 16) / 2048, buf) < 0) fatal("TILEIDX read failed");
	for (k = 0; k < MAXTILES; k += 128) {
		for (i = 0; i < 128; i++) {
			unsigned char *r = buf + (k + i) * 16;
			long t = k + i;
			tilesizx[t] = r[0] | (r[1]<<8);
			tilesizy[t] = r[2] | (r[3]<<8);
			picanm[t] = (long)(r[4] | (r[5]<<8) | (r[6]<<16) | ((unsigned long)r[7]<<24));
			tilesect[t] = r[8] | (r[9]<<8);
			tileavg[t] = r[10] | (r[11]<<8);
			tilenpieces[t] = r[12];
			tileflags[t] = r[13];
			tileaux[t] = r[14] | (r[15]<<8);
			waloff[t] = 0;
			walock[t] = 0;
		}
	}
	for(i=0;i<MAXTILES;i++)
	{
		long j = 15;
		while ((j > 1) && (pow2long[j] > tilesizx[i])) j--;
		picsiz[i] = ((char)j);
		j = 15;
		while ((j > 1) && (pow2long[j] > tilesizy[i])) j--;
		picsiz[i] += ((char)(j<<4));
	}
	tex_init();
	artversion = 1;
	return 0;
}

/* Synchronous preload into VRAM (used by the level loader's cache pass). */
void loadtile(short tilenume)
{
	if ((unsigned)tilenume >= MAXTILES) return;
	if (tilesizx[tilenume] <= 0 || tilesizy[tilenume] <= 0) return;
	tex_preload(tilenume);
}

long allocatepermanenttile(short tilenume, long xsiz, long ysiz)
{
	/* Used by the game for the save-game thumbnail and mirrors; not supported on PS1. */
	if ((unsigned)tilenume >= MAXTILES) return 0;
	tilesizx[tilenume] = 0; tilesizy[tilenume] = 0;
	picanm[tilenume] = 0;
	return 0;
}

long loadboard(char *filename, long *daposx, long *daposy, long *daposz, short *daang, short *dacursectnum)
{
	short fil, i, numsprites;

	i = strlen(filename)-4;
	if (i >= 0 && filename[i] == '.') filename[i] = 0;
	if ((fil = kopen4load(filename,0)) == -1)
		{ mapversion = 7L; return(-1); }

	kread(fil,&mapversion,4);
	if (mapversion != 7L) return(-1);

	initspritelists();

	clearbuf(&show2dsector[0],(MAXSECTORS+3)>>5,0L);
	clearbuf(&show2dsprite[0],(MAXSPRITES+3)>>5,0L);
	clearbuf(&show2dwall[0],(MAXWALLS+3)>>5,0L);

	kread(fil,daposx,4);
	kread(fil,daposy,4);
	kread(fil,daposz,4);
	kread(fil,daang,2);
	kread(fil,dacursectnum,2);

	kread(fil,&numsectors,2);
	kread(fil,&sector[0],sizeof(sectortype)*numsectors);

	kread(fil,&numwalls,2);
	kread(fil,&wall[0],sizeof(walltype)*numwalls);

	kread(fil,&numsprites,2);
	kread(fil,&sprite[0],sizeof(spritetype)*numsprites);

	for(i=0;i<numsprites;i++)
		insertsprite(sprite[i].sectnum,sprite[i].statnum);

		//Must be after loading sectors, etc!
	updatesector(*daposx,*daposy,dacursectnum);

	kclose(fil);
	return(0);
}

/* ---- palette API used by the game ------------------------------------- */

void setbrightness(char dabrightness, char *dapal)
{
	curbrightness = min(max((long)dabrightness,0),15);
	pal_set_brightness(curbrightness, dapal);
}

void makepalookup(long palnum, char *remapbuf, signed char r, signed char g, signed char b, char dastat)
{
	pal_set_remap(palnum, (unsigned char *)remapbuf);
}

long getclosestcol(long r, long g, long b)
{
	long i, j, k, dist, mindist = 0x7fffffff, best = 0;
	for (i = 0; i < 255; i++) {
		j = palette[i*3]-r; k = palette[i*3+1]-g; dist = j*j+k*k; k = palette[i*3+2]-b; dist += k*k;
		if (dist < mindist) { mindist = dist; best = i; }
	}
	return best;
}

void setviewtotile(short tilenume, long xsiz, long ysiz) {}
void setviewback(void) {}
void squarerotatetile(short tilenume) {}
void copytilepiece(long tilenume1, long sx1, long sy1, long xsiz, long ysiz, long tilenume2, long sx2, long sy2) {}
void plotpixel(long x, long y, char col) {}
char getpixel(long x, long y) { return 0; }
void qsetmode640350(void) {}
void qsetmode640480(void) {}
void clear2dscreen(void) {}
void draw2dgrid(long posxe, long posye, short ange, long zoome, short gride) {}
void draw2dscreen(long posxe, long posye, short ange, long zoome, short gride) {}
void drawline16(long x1, long y1, long x2, long y2, char col) {}
void setpalookupaddress(char *dapalookup) {}
void faketimerhandler(void) {}

#include "parts/engine_core.c"
#include "parts/render.c"
