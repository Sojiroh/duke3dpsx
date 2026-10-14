/* PS1: loading screen with a progress bar for the long waits (boot, level load, game load).

   The look is the game's own: big red menutext() letters on black or on the LOADSCREEN art (what
   dofrontscreens() shows), plus a bar drawn with flat palette colours.  The bar is a list of stages:
   loadscreen_stage(end_pct, bytes) starts a stage that ends at end_pct; while it runs the bar moves with
   the CD bytes read by pak_read() (pak_progress hook), or with loadscreen_progress() for work that is
   not CD bound.  Everything is main thread only; the screen is redrawn and presented (nextpage) every
   LS_STEP percent, and it only touches resident tiles (fonts, LOADSCREEN), so a redraw never reads the CD. */

#include "duke3d.h"
#include "texcache.h"
#include "loadscreen.h"

#define BAR_X0 60          /* border box, screen pixels (320x240) */
#define BAR_X1 259
#define BAR_Y0 176
#define BAR_Y1 185
#define LS_STEP 2          /* the bar is redrawn every LS_STEP percent: a redraw costs about half a vsync of waiting */
#define BAR_ROWS (BAR_Y1 - BAR_Y0 - 1)   /* inner rows: one fill colour each (light red at the top, dark at the bottom) */

static const char *ls_line1, *ls_line2;
static int ls_active, ls_pic;
static int ls_pct = -1;                      /* percentage currently on screen (-1 = nothing drawn yet) */
static int ls_busy;                          /* inside a redraw: nextpage() must not re-enter */
static int ls_start, ls_end;                 /* current stage: from ls_start to ls_end percent */
static unsigned long ls_total, ls_read;      /* current stage: expected CD bytes / bytes read so far */
static int ls_border, ls_inner, ls_fill[BAR_ROWS];
#ifdef PS1_PROF
static int ls_t0;
static unsigned long ls_bytes;               /* CD bytes read since the stage began (stage timing report) */
#endif

static void ls_render(void)
{
    int i, w;

    if(ls_pic) rotatesprite(320<<15,200<<15,65536L,0,LOADSCREEN,0,0,2+8+64,0,0,xdim-1,ydim-1);
    else clearallviews(0L);

    if(ls_line1) menutext(160,90,0,0,(char *)ls_line1);
    if(ls_line2) menutext(160,114,0,0,(char *)ls_line2);

    rend_fillrect(BAR_X0,BAR_Y0,BAR_X1,BAR_Y1,ls_border);
    rend_fillrect(BAR_X0+1,BAR_Y0+1,BAR_X1-1,BAR_Y1-1,ls_inner);
    w = (BAR_X1-BAR_X0-1)*ls_pct/100;
    if(w > 0)
        for(i=0;i<BAR_ROWS;i++)
            rend_fillrect(BAR_X0+1,BAR_Y0+1+i,BAR_X0+w,BAR_Y0+1+i,ls_fill[i]);
}

static void ls_draw(void)
{
    ls_busy = 1;
    ls_render();
    nextpage();
    ls_busy = 0;
}

/* pak_read() hook: advance the current byte-counted stage */
static void ls_pak(unsigned long bytes)
{
#ifdef PS1_PROF
    ls_bytes += bytes;
#endif
    if(ls_total == 0) return;
    ls_read += bytes;
    if(ls_read > ls_total) ls_read = ls_total;
    loadscreen_set(ls_start + (int)((ls_end-ls_start)*ls_read/ls_total));
}

void loadscreen_begin(const char *line1, const char *line2, int showpic)
{
    int i;

    ls_line1 = line1; ls_line2 = line2; ls_pic = showpic;
    ls_start = ls_end = 0; ls_total = ls_read = 0;
    ls_pct = 0;
    ls_active = 1;
#ifdef PS1_PROF
    ls_t0 = video_vsync_count(); ls_bytes = 0;
#endif

    ls_border = getclosestcol(63,63,63);
    ls_inner = getclosestcol(6,0,0);
    for(i=0;i<BAR_ROWS;i++)
        ls_fill[i] = getclosestcol(63-i*36/(BAR_ROWS-1),0,0);

    video_wait_gpu();
    tex_quiesce();                    /* no streaming read may start (or use the staging buffer) while the screen is up */
    if(showpic)
    {
        tex_preload(LOADSCREEN);      /* resident and locked: the redraws below never read the CD */
        tex_lock(LOADSCREEN,1);
    }
    pak_progress = ls_pak;
    ls_draw();
}

/* change the text of the screen (used once the fonts are resident); shown from the next redraw on */
void loadscreen_text(const char *line1, const char *line2)
{
    if(!ls_active) return;
    ls_line1 = line1; ls_line2 = line2;
    ls_draw();
}

void loadscreen_set(int pct)
{
    if(!ls_active || ls_busy) return;
    if(pct > 100) pct = 100;
    if(pct < ls_pct + LS_STEP && (pct < 100 || ls_pct >= 100)) return;   /* never backwards; one redraw per LS_STEP % */
    ls_pct = pct;
    ls_draw();
}

void loadscreen_stage(int end_pct, unsigned long expected_bytes)
{
    if(!ls_active) return;
#ifdef PS1_PROF
    dbg("LS: stage -> %d%% took %d vsyncs, %lu CD bytes (screen %d%%)\n",ls_end,video_vsync_count()-ls_t0,ls_bytes,ls_pct);
    ls_t0 = video_vsync_count(); ls_bytes = 0;
#endif
    loadscreen_set(ls_end);        /* the previous stage is over */
    ls_start = ls_end > ls_pct ? ls_end : ls_pct;
    ls_end = end_pct > ls_start ? end_pct : ls_start;
    ls_total = expected_bytes; ls_read = 0;
}

void loadscreen_progress(unsigned long done, unsigned long total)
{
    if(!ls_active || total == 0) return;
    if(done > total) done = total;
    loadscreen_set(ls_start + (int)((ls_end-ls_start)*done/total));
}

void loadscreen_end(void)
{
    if(!ls_active) return;
#ifdef PS1_PROF
    dbg("LS: stage -> %d%% took %d vsyncs, %lu CD bytes (screen %d%%)\n",ls_end,video_vsync_count()-ls_t0,ls_bytes,ls_pct);
#endif
    loadscreen_set(100);
    pak_progress = 0;
    ls_active = 0;
    if(ls_pic)
    {
        /* the caller goes on to build its first real frame (the menu or the game, ~1 s of cache misses): draw
           the 100% screen into that frame's list too, so that whatever presents it in between (the menu loop's
           nextpage) shows this instead of stale primitives */
        ls_render();
        tex_lock(LOADSCREEN,0);          /* the art may be evicted again once the level runs */
    }
}
