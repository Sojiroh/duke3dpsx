/*
 * PS1 platform glue for the Duke Nukem 3D game code: replacements for the DOS timer, the
 * (single player only) multiplayer layer, the engine cache allocator and small libc/mact helpers.
 */

#include "duke3d.h"

/* ---- timer ---------------------------------------------------------------------------------
   totalclock is incremented at 120 Hz by the vsync IRQ (psx.h), no timer task is needed. */
void inittimer(void) {}
void uninittimer(void) {}

long Z_AvailHeap(void) { return 0; }

/* ---- single player "multiplayer" layer ---------------------------------------------------- */

short numplayers = 1, myconnectindex = 0;
short connecthead = 0, connectpoint2[MAXPLAYERS];
extern char playerswhenstarted;

void initmultiplayers(char damultioption, char dacomrateoption, char dapriority)
{
    int i;

    numplayers = 1;
    myconnectindex = 0;
    connecthead = 0;
    for(i=0;i<MAXPLAYERS;i++) connectpoint2[i] = -1;
    playerswhenstarted = 1;
    ready2send = 1;
}

void uninitmultiplayers(void) {}
void sendpacket(long other, unsigned char *bufptr, long messleng) {}
short getpacket(short *other, unsigned char *bufptr) { return 0; }
long getoutputcirclesize(void) { return 0; }
void setsocket(short newsocket) {}
void sendlogon(void) {}
void sendlogoff(void) {}
void genericmultifunction(short other, char *bufptr, short messleng, short command) {}
void setpackettimeout(long datimeoutcount, long damodemtimeoutcount) {}
void flushpackets(void) {}

/* ---- engine cache allocator ---------------------------------------------------------------
   The original allocache() was the Build cache1d allocator.  Its few remaining users want
   permanent blocks, so this is a bump allocator over a small static arena that is never freed. */

#define CACHE_ARENA (4*1024)
static unsigned char cache_arena[CACHE_ARENA] __attribute__((aligned(8)));
static long cache_used;

void allocache(long *newhandle, long newbytes, char *newlockptr)
{
    long n = (newbytes+7)&~7;

    if(cache_used+n > CACHE_ARENA)
        fatal("allocache: arena exhausted (%ld bytes)",newbytes);
    *newhandle = (long)&cache_arena[cache_used];
    cache_used += n;
}

/* ---- util_lib / libc replacements --------------------------------------------------------- */

int stricmp(const char *a, const char *b)
{
    int ca, cb;

    do
    {
        ca = tolower((unsigned char)*a++);
        cb = tolower((unsigned char)*b++);
    } while(ca && ca == cb);
    return ca-cb;
}

int FindDistance2D(int ix, int iy)
{
    int t;

    ix = ix < 0 ? -ix : ix;
    iy = iy < 0 ? -iy : iy;
    if(ix < iy) { t = ix; ix = iy; iy = t; }
    t = iy + (iy>>1);
    return ix - (ix>>5) - (ix>>7) + (t>>2) + (t>>6);
}

int FindDistance3D(int ix, int iy, int iz)
{
    int t;

    ix = ix < 0 ? -ix : ix;
    iy = iy < 0 ? -iy : iy;
    iz = iz < 0 ? -iz : iz;
    if(ix < iy) { t = ix; ix = iy; iy = t; }
    if(ix < iz) { t = ix; ix = iz; iz = t; }
    t = iy + iz;
    return ix - (ix>>4) + (t>>2) + (t>>3);
}

/* defined by the engine in the original game; the PS1 engine has no tile cache debug */
char cachedebug;
