// "Build Engine & Tools" Copyright (c) 1993-1997 Ken Silverman
// Ken Silverman's official web site: "http://www.advsys.net/ken"
// See the included license file "BUILDLIC.TXT" for license info.
//
// PS1 port: limits reduced for 2MB RAM, prototypes added, software
// rasterizer replaced by a GPU back end (render.c).
#pragma once

#define MAXSECTORS 1024
#define MAXWALLS 6144   /* largest v1.3D map uses 5471 */
#define MAXSPRITES 2048

#define MAXTILES 4096
#define MAXSTATUS 1024
#define MAXPLAYERS 16
#define MAXXDIM 320
#define MAXYDIM 240
#define MAXPALOOKUPS 32
#define MAXPSKYTILES 256
#define MAXSPRITESONSCREEN 512

#define CLIPMASK0 (((1L)<<16)+1L)
#define CLIPMASK1 (((256L)<<16)+64L)

#ifdef ENGINE
	#define EXTERN
#else
	#define EXTERN extern
#endif

//ceilingstat/floorstat:
//   bit 0: 1 = parallaxing, 0 = not                                 "P"
//   bit 1: 1 = groudraw, 0 = not
//   bit 2: 1 = swap x&y, 0 = not                                    "F"
//   bit 3: 1 = double smooshiness                                   "E"
//   bit 4: 1 = x-flip                                               "F"
//   bit 5: 1 = y-flip                                               "F"
//   bit 6: 1 = Align texture to first wall of sector                "R"
//   bits 7-8:                                                       "T"
//          00 = normal floors
//          01 = masked floors
//          10 = transluscent masked floors
//          11 = reverse transluscent masked floors
//   bits 9-15: reserved

	//40 bytes
typedef struct
{
	short wallptr, wallnum;
	long ceilingz, floorz;
	short ceilingstat, floorstat;
	short ceilingpicnum, ceilingheinum;
	signed char ceilingshade;
	char ceilingpal, ceilingxpanning, ceilingypanning;
	short floorpicnum, floorheinum;
	signed char floorshade;
	char floorpal, floorxpanning, floorypanning;
	char visibility, filler;
	short lotag, hitag, extra;
} sectortype;

//cstat:
//   bit 0: 1 = Blocking wall (use with clipmove, getzrange)         "B"
//   bit 1: 1 = bottoms of invisible walls swapped, 0 = not          "2"
//   bit 2: 1 = align picture on bottom (for doors), 0 = top         "O"
//   bit 3: 1 = x-flipped, 0 = normal                                "F"
//   bit 4: 1 = masking wall, 0 = not                                "M"
//   bit 5: 1 = 1-way wall, 0 = not                                  "1"
//   bit 6: 1 = Blocking wall (use with hitscan / cliptype 1)        "H"
//   bit 7: 1 = Transluscence, 0 = not                               "T"
//   bit 8: 1 = y-flipped, 0 = normal                                "F"
//   bit 9: 1 = Transluscence reversing, 0 = normal                  "T"
//   bits 10-15: reserved

	//32 bytes
typedef struct
{
	long x, y;
	short point2, nextwall, nextsector, cstat;
	short picnum, overpicnum;
	signed char shade;
	char pal, xrepeat, yrepeat, xpanning, ypanning;
	short lotag, hitag, extra;
} walltype;

//cstat:
//   bit 0: 1 = Blocking sprite (use with clipmove, getzrange)       "B"
//   bit 1: 1 = transluscence, 0 = normal                            "T"
//   bit 2: 1 = x-flipped, 0 = normal                                "F"
//   bit 3: 1 = y-flipped, 0 = normal                                "F"
//   bits 5-4: 00 = FACE sprite (default)                            "R"
//             01 = WALL sprite (like masked walls)
//             10 = FLOOR sprite (parallel to ceilings&floors)
//   bit 6: 1 = 1-sided sprite, 0 = normal                           "1"
//   bit 7: 1 = Real centered centering, 0 = foot center             "C"
//   bit 8: 1 = Blocking sprite (use with hitscan / cliptype 1)      "H"
//   bit 9: 1 = Transluscence reversing, 0 = normal                  "T"
//   bits 10-14: reserved
//   bit 15: 1 = Invisible sprite, 0 = not invisible

	//44 bytes
typedef struct
{
	long x, y, z;
	short cstat, picnum;
	signed char shade;
	char pal, clipdist, filler;
	unsigned char xrepeat, yrepeat;
	signed char xoffset, yoffset;
	short sectnum, statnum;
	short ang, owner, xvel, yvel, zvel;
	short lotag, hitag, extra;
} spritetype;

EXTERN sectortype sector[MAXSECTORS];
EXTERN walltype wall[MAXWALLS];
EXTERN spritetype sprite[MAXSPRITES];

EXTERN long spritesortcnt;
EXTERN spritetype tsprite[MAXSPRITESONSCREEN];

EXTERN char vidoption;
EXTERN long xdim, ydim, ylookup[MAXYDIM+1], numpages;
EXTERN long yxaspect, viewingrange;

EXTERN short numsectors, numwalls;
extern volatile long totalclock;
EXTERN long numframes, randomseed;
EXTERN short sintable[2048];
EXTERN char palette[768];
EXTERN short numpalookups;
EXTERN char *palookup[MAXPALOOKUPS];
EXTERN char parallaxtype, showinvisibility;
EXTERN long parallaxyoffs, parallaxyscale;
EXTERN long visibility, parallaxvisibility;

EXTERN long windowx1, windowy1, windowx2, windowy2;
EXTERN short startumost[MAXXDIM], startdmost[MAXXDIM];

EXTERN short pskyoff[MAXPSKYTILES], pskybits;

EXTERN short headspritesect[MAXSECTORS+1], headspritestat[MAXSTATUS+1];
EXTERN short prevspritesect[MAXSPRITES], prevspritestat[MAXSPRITES];
EXTERN short nextspritesect[MAXSPRITES], nextspritestat[MAXSPRITES];

EXTERN short tilesizx[MAXTILES], tilesizy[MAXTILES];
EXTERN char walock[MAXTILES];
EXTERN long numtiles, picanm[MAXTILES], waloff[MAXTILES];

EXTERN char show2dsector[(MAXSECTORS+7)>>3];
EXTERN char show2dwall[(MAXWALLS+7)>>3];
EXTERN char show2dsprite[(MAXSPRITES+7)>>3];
EXTERN char automapping;

EXTERN char gotpic[(MAXTILES+7)>>3];
EXTERN char gotsector[(MAXSECTORS+7)>>3];

/* ---- engine API (prototypes added for the port) ---- */
void initengine(void);
void uninitengine(void);
long setgamemode(char davidoption, long daxdim, long daydim);
void nextpage(void);
long loadboard(char *filename, long *daposx, long *daposy, long *daposz, short *daang, short *dacursectnum);
/* horizontal resolution of the 3D column engine = screen width >> REND_XSHIFT (see setview) */
#ifndef REND_XSHIFT
#define REND_XSHIFT 1
#endif
long loadpics(char *filename);
void loadtile(short tilenume);
long allocatepermanenttile(short tilenume, long xsiz, long ysiz);
void drawrooms(long daposx, long daposy, long daposz, short daang, long dahoriz, short dacursectnum);
void drawmasks(void);
void rotatesprite(long sx, long sy, long z, short a, short picnum, signed char dashade, char dapalnum, char dastat, long cx1, long cy1, long cx2, long cy2);
void flushperms(void);
void setview(long x1, long y1, long x2, long y2);
void setaspect(long daxrange, long daaspect);
void clearview(long dacol);
void clearallviews(long dacol);
void setbrightness(char dabrightness, char *dapal);
void makepalookup(long palnum, char *remapbuf, signed char r, signed char g, signed char b, char dastat);
long getclosestcol(long r, long g, long b);
void printext256(long xpos, long ypos, short col, short backcol, char *name, char fontsize);
void drawline256(long x1, long y1, long x2, long y2, char col);
void drawmapview(long dax, long day, long zoome, short daang);
void plotpixel(long x, long y, char col);
char getpixel(long x, long y);
void setviewtotile(short tilenume, long xsiz, long ysiz);
void setviewback(void);
void squarerotatetile(short tilenume);
void preparemirror(long dax, long day, long daz, short daang, long dahoriz, short dawall, short dasector, long *tposx, long *tposy, short *tang);
void completemirror(void);
extern int rend_mirrors_supported;   /* 0: the GPU back end cannot draw mirrors; the game skips its mirror pass */
long animateoffs(short tilenum, short fakevar);
long inside(long x, long y, short sectnum);
long getangle(long xvect, long yvect);
long ksqrt(long num);
long krecip(long num);
long krand(void);
void initspritelists(void);
long insertsprite(short sectnum, short statnum);
long insertspritesect(short sectnum);
long insertspritestat(short statnum);
long deletesprite(short spritenum);
long deletespritesect(short deleteme);
long deletespritestat(short deleteme);
long changespritesect(short spritenum, short newsectnum);
long changespritestat(short spritenum, short newstatnum);
long setsprite(short spritenum, long newx, long newy, long newz);
long nextsectorneighborz(short sectnum, long thez, short topbottom, short direction);
long cansee(long x1, long y1, long z1, short sect1, long x2, long y2, long z2, short sect2);
long hitscan(long xs, long ys, long zs, short sectnum, long vx, long vy, long vz, short *hitsect, short *hitwall, short *hitsprite, long *hitx, long *hity, long *hitz, unsigned long cliptype);
void neartag(long xs, long ys, long zs, short sectnum, short ange, short *neartagsector, short *neartagwall, short *neartagsprite, long *neartaghitdist, long neartagrange, char tagsearch);
long lintersect(long x1, long y1, long z1, long x2, long y2, long z2, long x3, long y3, long x4, long y4, long *intx, long *inty, long *intz);
long rintersect(long x1, long y1, long z1, long vx, long vy, long vz, long x3, long y3, long x4, long y4, long *intx, long *inty, long *intz);
void dragpoint(short pointhighlight, long dax, long day);
long lastwall(short point);
long clipmove(long *x, long *y, long *z, short *sectnum, long xvect, long yvect, long walldist, long ceildist, long flordist, unsigned long cliptype);
long pushmove(long *x, long *y, long *z, short *sectnum, long walldist, long ceildist, long flordist, unsigned long cliptype);
void updatesector(long x, long y, short *sectnum);
void rotatepoint(long xpivot, long ypivot, long x, long y, short daang, long *x2, long *y2);
void getzrange(long x, long y, long z, short sectnum, long *ceilz, long *ceilhit, long *florz, long *florhit, long walldist, unsigned long cliptype);
long sectorofwall(short theline);
long getceilzofslope(short sectnum, long dax, long day);
long getflorzofslope(short sectnum, long dax, long day);
void getzsofslope(short sectnum, long dax, long day, long *ceilz, long *florz);
void alignceilslope(short dasect, long x, long y, long z);
void alignflorslope(short dasect, long x, long y, long z);
long loopnumofsector(short sectnum, short wallnum);
void setfirstwall(short sectnum, short newfirstwall);
long clipinsidebox(long x, long y, short wallnum, long walldist);
long clipinsideboxline(long x, long y, long x1, long y1, long x2, long y2, long walldist);
void copytilepiece(long tilenume1, long sx1, long sy1, long xsiz, long ysiz, long tilenume2, long sx2, long sy2);
void drawline16(long x1, long y1, long x2, long y2, char col);
long getpalookup(long davis, long dashade);
void setpalookupaddress(char *dapalookup);
void qsetmode640350(void);
void qsetmode640480(void);
void clear2dscreen(void);
void draw2dgrid(long posxe, long posye, short ange, long zoome, short gride);
void draw2dscreen(long posxe, long posye, short ange, long zoome, short gride);

/* PS1 port additions */
extern long xdimen, halfxdimen, ydimen, xdimenscale, xdimscale, xdimenrecip, xyaspect, viewingrangerecip;
extern long globalposx, globalposy, globalposz, globalhoriz;
extern short globalang, globalcursectnum;
extern long globalvisibility;
extern char picsiz[MAXTILES];
extern short numscans, numhits, numbunches;
extern short searchit, searchsector, searchwall, searchstat;
extern long searchx, searchy;
extern unsigned char textfont[1024], smalltextfont[1024];
extern char britable[16][64];
extern long reciptable[2048];
extern short radarang[1280];
extern long pow2long[32];
extern char pow2char[8];
extern long hitscangoalx, hitscangoaly;
extern long clipmoveboxtracenum;
extern long transarea, totalarea, beforedrawrooms;
extern long totalclocklock;
void rend_represent(void);   /* PS1: re-present last frame with the current tint */
void rend_keepalive(void);   /* PS1: call from busy-wait loops (re-presents when no frame is flipped) */
