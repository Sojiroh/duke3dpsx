#include "texcache.h"
#include "build.h"
#include "data_index.h"

extern unsigned short tilesect[MAXTILES];
extern unsigned char tilenpieces[MAXTILES];
extern unsigned char tileflags[MAXTILES];
extern unsigned short tileaux[MAXTILES];

/* ---- VRAM layout ------------------------------------------------------ */
#define NPAGES 12
typedef struct { u16 cellx, celly; u16 w, h; } Page; /* w,h in texels */
static const Page pages[NPAGES] = {
	{320,0,256,256},{448,0,256,256},{576,0,256,256},{704,0,256,256},{832,0,256,256},{960,0,128,256},
	{320,256,256,256},{448,256,256,256},{576,256,256,256},{704,256,256,256},{832,256,256,256},{960,256,128,256},
};

/* ---- kd-tree block allocator ------------------------------------------ */
#define MAXNODES 1536
typedef struct {
	u8 x, y;        /* texel position within page (0..255); 256 cannot occur for a block origin */
	u8 lw, lh;      /* log2 size */
	u8 page, state; /* state: 0 free leaf, 1 split, 2 used */
	s16 child0, child1, parent;
	s16 slot;
} Node;
static Node nodes[MAXNODES];
static s16 nodefree;        /* free list head (via child0) */
static s16 roots[NPAGES];

static s16 node_alloc(void)
{
	s16 n = nodefree;
	if (n < 0) return -1;
	nodefree = nodes[n].child0;
	nodes[n].child0 = nodes[n].child1 = -1;
	nodes[n].state = 0; nodes[n].slot = -1;
	return n;
}
static void node_release(s16 n) { nodes[n].child0 = nodefree; nodefree = n; }

static int log2ceil(int v) { int l = 3; while ((1 << l) < v) l++; return l; }
/* upload geometry matching mkdata.py: stride multiple of 16 texels (min 16), rows multiple of 4,
   so every LoadImage is a multiple of 64 bytes (the SDK's 16-word DMA chunk) */
static inline int piece_stride(int w) { int s = (w + 15) & ~15; return s < 16 ? 16 : s; }
static inline int piece_rows(int h) { return (h + 3) & ~3; }

/* find a free leaf >= (lw,lh) under node n; split down to exact size; returns node or -1 */
static s16 node_find(s16 n, int lw, int lh)
{
	Node *nd = &nodes[n];
	if (nd->state == 2) return -1;
	if (nd->lw < lw || nd->lh < lh) return -1;
	if (nd->state == 1) {
		s16 r = node_find(nd->child0, lw, lh);
		if (r < 0) r = node_find(nd->child1, lw, lh);
		return r;
	}
	/* free leaf: split until exact */
	while (nd->lw > lw || nd->lh > lh) {
		s16 a = node_alloc(), b;
		if (a < 0) return -1;
		b = node_alloc();
		if (b < 0) { node_release(a); return -1; }
		Node *ca = &nodes[a], *cb = &nodes[b];
		*ca = *nd; *cb = *nd;
		ca->parent = cb->parent = n; ca->state = cb->state = 0;
		ca->child0 = ca->child1 = cb->child0 = cb->child1 = -1;
		if ((nd->lw - lw) >= (nd->lh - lh)) { /* split width */
			ca->lw = cb->lw = nd->lw - 1;
			cb->x = nd->x + (1 << cb->lw);
		} else {
			ca->lh = cb->lh = nd->lh - 1;
			cb->y = nd->y + (1 << cb->lh);
		}
		nd->state = 1; nd->child0 = a; nd->child1 = b;
		n = a; nd = ca;
	}
	return n;
}

static void node_free(s16 n)
{
	nodes[n].state = 0; nodes[n].slot = -1;
	for (;;) {
		s16 p = nodes[n].parent;
		if (p < 0) return;
		Node *pp = &nodes[p];
		if (nodes[pp->child0].state != 0 || nodes[pp->child1].state != 0) return;
		node_release(pp->child0); node_release(pp->child1);
		pp->child0 = pp->child1 = -1; pp->state = 0;
		n = p;
	}
}

/* ---- resident slots ---------------------------------------------------- */
#define MAXSLOTS 768
typedef struct {
	u16 picnum;
	u8 piece, state;   /* state: 0 empty, 1 loading, 2 resident */
	s16 node;
	u8 lock;
	u8 half;
	u32 lastuse;
	u16 w, h, xoff, yoff;
} Slot;
static Slot slots[MAXSLOTS];
static u16 tile2slot[MAXTILES];   /* piece 0 slot or 0xffff */
static u32 texframe = 1;
static u32 vram_used;             /* resident block texels */
#define VRAM_TEXELS (704 * 512 * 2)
#define VRAM_PRESSURE (VRAM_TEXELS * 6 / 10)
#define PRESSURE_WINDOW 120       /* frames: only slots used this recently (or locked) count as VRAM pressure */
int tex_stat_resident, tex_stat_misses, tex_stat_loads, tex_stat_half;
#ifdef PS1_PROF
int tex_stat_halfloads, tex_stat_oldhalf;   /* cumulative loads: chosen half-res / would have been half-res under the old "vram_used > 60%" rule */
#endif

/* PS1: half-resolution copies are chosen when the *working set* is large, not when VRAM merely holds stale
   tiles (LRU eviction frees those on demand). active_texels = block texels of locked slots and of slots
   touched in the last PRESSURE_WINDOW frames; recomputed once per frame, then kept current by
   begin_load/evict_slot. */
static u32 active_texels, active_frame;
static inline u32 node_texels(int n) { return (1u << nodes[n].lw) * (1u << nodes[n].lh); }
static inline int slot_active(const Slot *sl) { return sl->lock || sl->lastuse + PRESSURE_WINDOW >= texframe; }
static u32 active_texels_now(void)
{
	if (active_frame != texframe) {
		u32 t = 0;
		int s;
		for (s = 0; s < MAXSLOTS; s++)
			if (slots[s].state && slots[s].node >= 0 && slot_active(&slots[s])) t += node_texels(slots[s].node);
		active_texels = t; active_frame = texframe;
	}
	return active_texels;
}

/* ---- piece table (only multi-piece tiles) ------------------------------ */
typedef struct { u16 tile, xoff, yoff, w, h, pad; u32 sector; } PieceRec;
static PieceRec piecetab[DAT_NUMPIECES > 0 ? DAT_NUMPIECES : 1] __attribute__((aligned(4)));

/* ---- streaming --------------------------------------------------------- */
#define QLEN 64
static u16 queue[QLEN];
static int qhead, qtail;
static int nprio;               /* queue entries at the head that are sprite requests (see queue_tile_prio) */
static u8 pendingbit[(MAXTILES + 7) >> 3];
static u32 staging[65536 / 4];
void *tex_staging(void) { return staging; }
static volatile int cd_done;
/* async coalesced run in flight (see start_run): slots and their sector offset inside the read */
#define MAXRUN 64
#define TEX_WIN 32               /* sectors per run (64 KB staging buffer) */
#ifndef TEX_READAHEAD
#define TEX_READAHEAD 1          /* 1 = also load unrequested tiles that lie inside the run's window */
#endif
#ifndef TEX_RA_SECS
#define TEX_RA_SECS 8            /* read-ahead may extend a run up to this many sectors (a window tile in the run's span is free) */
#endif
#ifndef TEX_RA_ACTIVE_PCT
#define TEX_RA_ACTIVE_PCT 50     /* ... only while the recent working set uses less than this % of the cache VRAM (stale tiles are evicted first) */
#endif
static s16 run_slot[MAXRUN];
static u8 run_off[MAXRUN];
static int nrun, nreq;           /* nrun slots in flight; the first nreq were requested, the rest read-ahead */
static u32 evict_count;   /* number of LRU evictions: a read-ahead run stops at its first eviction */

#ifdef PS1_PROF
/* PS1 debug: streaming statistics, reported every 120 frames */
#define QF(i) qf[(i) & (QLEN - 1)]
static u16 qf[QLEN];             /* vsync count when each queued tile was requested */
static u16 run_qf[MAXRUN];
static u32 prof_cmds, prof_ra, prof_lat, prof_latn, prof_rahit;
#define prof_cmds_inc() (prof_cmds++)
static void prof_report(void)
{
	static int n, lvs, lloads; static u32 lcmds, lra, llat, llatn, lrahit;
	int vs, dv, ld;
	if (++n < 120) return;
	n = 0; vs = video_vsync_count(); dv = vs - lvs; lvs = vs;
	if (dv <= 0) dv = 1;
	ld = tex_stat_loads - lloads; lloads = tex_stat_loads;
	dbg("PROF stream: window %d vs: pieces %d cmds %d readahead %d latsum %d latn %d meanlat %d vs frametime %d.%02d vs queue %d rahit %d\n",
		dv, ld, (int)(prof_cmds - lcmds), (int)(prof_ra - lra), (int)(prof_lat - llat), (int)(prof_latn - llatn),
		(prof_latn - llatn) ? (int)((prof_lat - llat) / (prof_latn - llatn)) : 0,
		dv / 120, dv * 100 / 120 % 100, (qtail - qhead) & (QLEN - 1), (int)(prof_rahit - lrahit));
	lcmds = prof_cmds; lra = prof_ra; llat = prof_lat; llatn = prof_latn; lrahit = prof_rahit;
}
#else
#define prof_cmds_inc() ((void)0)
#endif
static const PakEntry *tiledata;

/* returns 1 if the half-resolution copy was selected */
static int piece_info(int picnum, int piece, int *xoff, int *yoff, int *w, int *h, u32 *sector)
{
	if (tilenpieces[picnum] <= 1) {
		*xoff = *yoff = 0; *w = tilesizx[picnum]; *h = tilesizy[picnum]; *sector = tilesect[picnum];
		if ((tileflags[picnum] & 1) && active_texels_now() > VRAM_PRESSURE) {
			*w >>= 1; *h >>= 1; *sector = tileaux[picnum];
			return 1;
		}
	} else {
		PieceRec *p = &piecetab[tileaux[picnum] + piece];
		*xoff = p->xoff; *yoff = p->yoff; *w = p->w; *h = p->h; *sector = p->sector;
	}
	return 0;
}

void tex_init(void)
{
	int i;
	for (i = 0; i < MAXNODES; i++) nodes[i].child0 = i + 1;
	nodes[MAXNODES - 1].child0 = -1;
	nodefree = 0;
	for (i = 0; i < NPAGES; i++) {
		s16 n = node_alloc();
		Node *nd = &nodes[n];
		nd->x = nd->y = 0; nd->lw = log2ceil(pages[i].w); nd->lh = log2ceil(pages[i].h);
		nd->page = i; nd->parent = -1;
		roots[i] = n;
	}
	for (i = 0; i < MAXSLOTS; i++) slots[i].state = 0;
	for (i = 0; i < MAXTILES; i++) tile2slot[i] = 0xffff;
	memset(pendingbit, 0, sizeof(pendingbit));
	qhead = qtail = nprio = 0;
	tiledata = pak_find("TILEDATA");
	if (!tiledata) fatal("TILEDATA missing");
	const PakEntry *e = pak_find("PIECES");
	if (e && DAT_NUMPIECES > 0) pak_read_bytes(e, 0, piecetab, sizeof(PieceRec) * DAT_NUMPIECES);
	tex_stat_resident = tex_stat_half = 0;
	active_texels = 0; active_frame = 0;
}

static int find_slot(int picnum, int piece)
{
	int s = tile2slot[picnum];
	if (piece == 0) return (s == 0xffff) ? -1 : s;
	for (s = 0; s < MAXSLOTS; s++)
		if (slots[s].state && slots[s].picnum == picnum && slots[s].piece == piece) return s;
	return -1;
}

static void evict_slot(int s)
{
	Slot *sl = &slots[s];
	if (sl->node >= 0) {
		u32 n = node_texels(sl->node);
		vram_used -= n;
		if (active_frame == texframe && slot_active(sl)) active_texels = active_texels > n ? active_texels - n : 0;
		node_free(sl->node);
	}
	if (sl->piece == 0) tile2slot[sl->picnum] = 0xffff;
	if (sl->half) tex_stat_half--;
	sl->state = 0; sl->node = -1;
	tex_stat_resident--;
}

static int evict_lru(void)
{
	int s, best = -1;
	u32 oldest = 0xffffffff;
	for (s = 0; s < MAXSLOTS; s++) {
		Slot *sl = &slots[s];
		if (sl->state != 2 || sl->lock) continue;
		if (sl->lastuse >= texframe) continue;   /* used this frame */
		if (sl->lastuse < oldest) { oldest = sl->lastuse; best = s; }
	}
	if (best < 0) return 0;
	evict_slot(best);
	evict_count++;
	return 1;
}

static s16 alloc_block(int w, int h)
{
	int lw = log2ceil(piece_stride(w)), lh = log2ceil(piece_rows(h)), i;
	for (;;) {
		for (i = 0; i < NPAGES; i++) {
			s16 n = node_find(roots[i], lw, lh);
			if (n >= 0) return n;
		}
		if (!evict_lru()) return -1;
	}
}

static int alloc_slot(void)
{
	int s;
	for (s = 0; s < MAXSLOTS; s++) if (!slots[s].state) return s;
	if (!evict_lru()) return -1;
	for (s = 0; s < MAXSLOTS; s++) if (!slots[s].state) return s;
	return -1;
}

/* Prepare a slot + VRAM block for (picnum, piece). Returns slot or -1. */
static int begin_load(int picnum, int piece, int *stride, int *h, u32 *sector)
{
	int xoff, yoff, w, hh, s, half;
	u32 sec;
	half = piece_info(picnum, piece, &xoff, &yoff, &w, &hh, &sec);
	if (w <= 0 || hh <= 0) return -1;
	active_texels_now();   /* current before slots change (alloc_slot may evict) */
#ifdef PS1_PROF
	if (tilenpieces[picnum] <= 1 && (tileflags[picnum] & 1)) { if (half) tex_stat_halfloads++; if (vram_used > VRAM_PRESSURE) tex_stat_oldhalf++; }
#endif
	s = alloc_slot();
	if (s < 0) return -1;
	s16 n = alloc_block(w, hh);
	if (n < 0) return -1;
	Slot *sl = &slots[s];
	nodes[n].state = 2; nodes[n].slot = s;
	sl->picnum = picnum; sl->piece = piece; sl->node = n; sl->state = 1; sl->lock = 0; sl->half = half;
	sl->w = w; sl->h = hh; sl->xoff = xoff; sl->yoff = yoff; sl->lastuse = texframe;
	{ u32 blk = node_texels(n); vram_used += blk; active_texels += blk; }
	if (piece == 0) tile2slot[picnum] = s;
	*stride = piece_stride(w); *h = piece_rows(hh); *sector = sec;
	tex_stat_resident++;
	if (half) tex_stat_half++;
	return s;
}

static void finish_load_from(int s, int stride, int h, const void *src)
{
	Slot *sl = &slots[s];
	Node *n = &nodes[sl->node];
	const Page *pg = &pages[n->page];
	RECT r;
	r.x = pg->cellx + (n->x >> 1); r.y = pg->celly + n->y; r.w = stride >> 1; r.h = h;
	LoadImage(&r, (u32 *)src);
	DrawSync(0);   /* called while the GPU is idle (see tex_frame); the staging buffer is reused right after */
	sl->state = 2;
	tex_stat_loads++;
}

static void finish_load(int s, int stride, int h) { finish_load_from(s, stride, h, staging); }

static void queue_tile(int picnum)
{
	int next = (qtail + 1) & (QLEN - 1);
	if ((pendingbit[picnum >> 3] & (1 << (picnum & 7))) || next == qhead) return;
	queue[qtail] = picnum;
#ifdef PS1_PROF
	QF(qtail) = (u16)video_vsync_count();
#endif
	qtail = next;
	pendingbit[picnum >> 3] |= 1 << (picnum & 7);
}

/* PS1: sprite tiles jump ahead of the wall/floor tiles waiting in the queue (a sprite whose tile is not
   resident is not drawn at all, while a wall shows a flat placeholder). The first nprio queue entries are
   the sprite requests, in request order; a tile already queued as a wall/floor tile is moved up. */
static void queue_tile_prio(int picnum)
{
	int n = (qtail - qhead) & (QLEN - 1), i, j;
#ifdef PS1_PROF
	u16 qf0 = (u16)video_vsync_count();
#endif
	if (pendingbit[picnum >> 3] & (1 << (picnum & 7))) {
		for (i = 0; i < n; i++) if (queue[(qhead + i) & (QLEN - 1)] == picnum) break;
		if (i < nprio || i >= n) return;   /* already in the priority zone */
		j = i;
#ifdef PS1_PROF
		qf0 = QF(qhead + i);
#endif
	} else {
		if (n == QLEN - 1) {   /* full: the newest wall/floor request is re-issued by the next frame that needs it */
			qtail = (qtail - 1) & (QLEN - 1);
			pendingbit[queue[qtail] >> 3] &= ~(1 << (queue[qtail] & 7));
			n--;
			if (nprio > n) nprio = n;
		}
		qtail = (qtail + 1) & (QLEN - 1);
		pendingbit[picnum >> 3] |= 1 << (picnum & 7);
		j = n;
	}
	for (; j > nprio; j--) {
		queue[(qhead + j) & (QLEN - 1)] = queue[(qhead + j - 1) & (QLEN - 1)];
#ifdef PS1_PROF
		QF(qhead + j) = QF(qhead + j - 1);
#endif
	}
	queue[(qhead + nprio) & (QLEN - 1)] = picnum;
#ifdef PS1_PROF
	QF(qhead + nprio) = qf0;
#endif
	nprio++;
}

/* cd_done: 0 = read pending, 1 = staged data valid, -1 = CD error (the staging buffer is not valid) */
static void cd_cb(int ok) { cd_done = ok ? 1 : -1; }

/* Complete the in-flight async run once its read has finished; returns 1 when nothing is in flight. */
static int inflight_finish(void)
{
	int i;
	if (!nrun) return 1;
	if (!cd_done) return 0;
	for (i = 0; i < nrun; i++) {
		int s = run_slot[i];
		if (cd_done > 0) {
			finish_load_from(s, piece_stride(slots[s].w), piece_rows(slots[s].h), (const u8 *)staging + run_off[i] * 2048);
#ifdef PS1_PROF
			if (i < nreq) { prof_lat += (u16)((u16)video_vsync_count() - run_qf[i]); prof_latn++; }
#endif
		} else {   /* PS1: failed read: never mark the tiles valid; free the slots and request the wanted tiles again */
			int pic = slots[s].picnum;
			evict_slot(s);
			if (i < nreq) queue_tile(pic);
		}
	}
	nrun = 0; cd_done = 0;
	return 1;
}

/* Preload (and optionally lock) the single-piece tiles first..last. Consecutive tiles that are adjacent
   on the disc are fetched with one read (fonts: ~100 tiny tiles, otherwise one CD seek each). */
void tex_preload_range(int first, int last, int lock)
{
	int t = first;
	while (!inflight_finish()) ;
	while (t <= last) {
		struct { u16 pic; u16 stride, h; u16 off; s16 slot; } run[64];
		int n = 0, i;
		u32 base = 0, total = 0;
		for (; t <= last && n < 64; t++) {
			int xoff, yoff, w, h, secs;
			u32 sec;
			if ((unsigned)t >= MAXTILES || tilesizx[t] <= 0 || tilesizy[t] <= 0 || tilenpieces[t] > 1) continue;
			if (find_slot(t, 0) >= 0) { if (lock) tex_lock(t, 1); continue; }
			piece_info(t, 0, &xoff, &yoff, &w, &h, &sec);
			secs = (piece_stride(w) * piece_rows(h) + 2047) / 2048;
			if (n > 0 && (sec != base + total || total + secs > 32)) break;   /* start a new run */
			if (n == 0) base = sec;
			{
				int stride, hh, s;
				u32 dummy;
				s = begin_load(t, 0, &stride, &hh, &dummy);
				if (s < 0) { if (n == 0) return; break; }
				run[n].pic = t; run[n].stride = stride; run[n].h = hh; run[n].off = total; run[n].slot = s;
				n++; total += secs;
			}
		}
		if (n == 0) continue;
		prof_cmds_inc();
		if (pak_read(tiledata->sector + base, total, staging) < 0) {
			for (i = 0; i < n; i++) evict_slot(run[i].slot);
			return;
		}
		for (i = 0; i < n; i++) {
			finish_load_from(run[i].slot, run[i].stride, run[i].h, (const u8 *)staging + run[i].off * 2048);
			if (lock) slots[run[i].slot].lock = 1;
		}
	}
}

/* PS1: preload the tiles of a list (ordered by tex_sector), skipping resident ones, with as few CD reads as
   possible: tiles that lie completely inside the same 32-sector window (gaps allowed) share one read.
   Multi-piece tiles are loaded first, one by one (tex_preload). */
void tex_preload_list(const short *tiles, int n)
{
	int t = 0, i;
	while (!inflight_finish()) ;
	for (i = 0; i < n; i++) {
		int pic = tiles[i];
		if ((unsigned)pic < MAXTILES && tilesizx[pic] > 0 && tilesizy[pic] > 0 && tilenpieces[pic] > 1) tex_preload(pic);
	}
	while (t < n) {
		struct { s16 slot; u8 off; } run[MAXRUN];
		int cnt = 0, end = 0;
		u32 base = 0;
		for (; t < n && cnt < MAXRUN; t++) {
			int pic = tiles[t], xoff, yoff, w, h, secs, stride, hh, s;
			u32 sec, dummy;
			if ((unsigned)pic >= MAXTILES || tilesizx[pic] <= 0 || tilesizy[pic] <= 0 || tilenpieces[pic] > 1) continue;
			if (find_slot(pic, 0) >= 0) continue;
			piece_info(pic, 0, &xoff, &yoff, &w, &h, &sec);
			secs = (piece_stride(w) * piece_rows(h) + 2047) / 2048;
			if (cnt > 0 && (sec < base || sec + secs > base + TEX_WIN)) break;   /* start a new run */
			s = begin_load(pic, 0, &stride, &hh, &dummy);
			if (s < 0) { if (cnt == 0) return; break; }
			if (cnt == 0) base = dummy;
			else if (dummy < base || dummy + (stride * hh + 2047) / 2048 > base + TEX_WIN) { evict_slot(s); break; }
			run[cnt].slot = s; run[cnt].off = dummy - base;
			if ((int)(dummy - base) + (stride * hh + 2047) / 2048 > end) end = dummy - base + (stride * hh + 2047) / 2048;
			cnt++;
		}
		if (cnt == 0) continue;
		prof_cmds_inc();
		if (pak_read(tiledata->sector + base, end, staging) < 0) {
			for (i = 0; i < cnt; i++) evict_slot(run[i].slot);
			return;
		}
		for (i = 0; i < cnt; i++)
			finish_load_from(run[i].slot, piece_stride(slots[run[i].slot].w), piece_rows(slots[run[i].slot].h), (const u8 *)staging + run[i].off * 2048);
	}
}

void tex_preload(int picnum)
{
	int np = tilenpieces[picnum] ? tilenpieces[picnum] : 1, p;
	for (p = 0; p < np; p++) {
		int stride, h, s;
		u32 sector;
		if (find_slot(picnum, p) >= 0) continue;
		s = begin_load(picnum, p, &stride, &h, &sector);
		if (s < 0) return;
		prof_cmds_inc();
		if (pak_read(tiledata->sector + sector, (stride * h + 2047) / 2048, staging) < 0) { evict_slot(s); return; }
		finish_load(s, stride, h);
	}
}


/* PS1: coalesced streaming. The head request fixes a window of up to TEX_WIN sectors (the 64 KB staging
   buffer) on the disc; every other queued single-piece tile that lies completely inside the window is
   loaded by the same CD command (one seek + one read instead of one per tile). The window start is the
   piece start that covers the most queued tiles. With TEX_READAHEAD, tiles nobody asked for that also lie
   in the window are loaded too (as old LRU entries) while the recent working set is small. */
static void start_run(int picnum, u16 qfh)
{
	struct { u32 sec; u16 pic; u8 secs, qi; } c[QLEN];
	u8 taken[QLEN];
	int stride, hh, s, n = (qtail - qhead) & (QLEN - 1), k = 0, i, j, cnt, bestcnt;
	u32 hs, lo;
	int hsecs, end, minpic = picnum, maxpic = picnum;
	(void)qfh;
	s = begin_load(picnum, 0, &stride, &hh, &hs);
	if (s < 0) return;
	hsecs = (stride * hh + 2047) / 2048;
	for (i = 0; i < n; i++) {
		int p = queue[(qhead + i) & (QLEN - 1)], xo, yo, w, h;
		u32 sec;
		taken[i] = 0;
		if (tilenpieces[p] > 1 || tilesizx[p] <= 0 || tilesizy[p] <= 0 || tile2slot[p] != 0xffff) continue;
		piece_info(p, 0, &xo, &yo, &w, &h, &sec);
		if (w <= 0 || h <= 0) continue;
		c[k].sec = sec; c[k].pic = p; c[k].qi = i;
		c[k].secs = (piece_stride(w) * piece_rows(h) + 2047) / 2048;
		k++;
	}
	/* window start: head or a lower candidate, whichever includes the most candidates */
	lo = hs; bestcnt = -1;
	for (j = -1; j < k; j++) {
		u32 L = j < 0 ? hs : c[j].sec;
		if (L > hs || L + TEX_WIN < hs + hsecs) continue;
		for (cnt = 0, i = 0; i < k; i++) if (c[i].sec >= L && c[i].sec + c[i].secs <= L + TEX_WIN) cnt++;
		if (cnt > bestcnt) { bestcnt = cnt; lo = L; }
	}
	nrun = 1; run_slot[0] = s; run_off[0] = hs - lo; end = hs - lo + hsecs;
#ifdef PS1_PROF
	run_qf[0] = qfh;
#endif
	for (i = 0; i < k && nrun < MAXRUN; i++) {
		int st2, h2, s2, secs2;
		u32 sec2;
		if (c[i].sec < lo || c[i].sec + c[i].secs > lo + TEX_WIN) continue;
		s2 = begin_load(c[i].pic, 0, &st2, &h2, &sec2);
		if (s2 < 0) break;
		secs2 = (st2 * h2 + 2047) / 2048;
		if (sec2 < lo || sec2 + secs2 > lo + TEX_WIN) { evict_slot(s2); continue; }   /* half-res choice changed meanwhile */
		run_slot[nrun] = s2; run_off[nrun] = sec2 - lo;
		if ((int)(sec2 - lo) + secs2 > end) end = sec2 - lo + secs2;
#ifdef PS1_PROF
		run_qf[nrun] = QF(qhead + c[i].qi);
#endif
		taken[c[i].qi] = 1;
		if (c[i].pic < minpic) minpic = c[i].pic;
		if (c[i].pic > maxpic) maxpic = c[i].pic;
		nrun++;
	}
	nreq = nrun;
#if TEX_READAHEAD
	if (tex_active_pct() < TEX_RA_ACTIVE_PCT) {
		int ra = end > TEX_RA_SECS ? end : TEX_RA_SECS, t, t0 = minpic - 48, t1 = maxpic + 48;
		u32 ev = evict_count;
		if (ra > TEX_WIN) ra = TEX_WIN;
		if (t0 < 0) t0 = 0;
		if (t1 >= MAXTILES) t1 = MAXTILES - 1;
		for (t = t0; t <= t1 && nrun < MAXRUN; t++) {
			int xo, yo, w, h, st2, h2, s2, secs, off;
			u32 sec, sec2;
			if (tilesizx[t] <= 0 || tilesizy[t] <= 0 || tilenpieces[t] > 1 || tile2slot[t] != 0xffff) continue;
			if (piece_info(t, 0, &xo, &yo, &w, &h, &sec) || w <= 0 || h <= 0) continue;
			secs = (piece_stride(w) * piece_rows(h) + 2047) / 2048;
			if (sec < lo || (int)(sec - lo) + secs > ra) continue;
			s2 = begin_load(t, 0, &st2, &h2, &sec2);
			if (s2 < 0) break;
			off = sec2 - lo;
			if (sec2 < lo || off + (st2 * h2 + 2047) / 2048 > TEX_WIN) { evict_slot(s2); continue; }
			{   /* old LRU entry: evicted first, not part of the working set */
				Slot *sl = &slots[s2];
				sl->lastuse = 0;
				if (!slot_active(sl) && active_frame == texframe) { u32 b = node_texels(sl->node); active_texels = active_texels > b ? active_texels - b : 0; }
			}
			run_slot[nrun] = s2; run_off[nrun] = off;
			if (off + (st2 * h2 + 2047) / 2048 > end) end = off + (st2 * h2 + 2047) / 2048;
#ifdef PS1_PROF
			prof_ra++;
#endif
			nrun++;
			if (evict_count != ev) break;   /* the cache is not as empty as assumed */
		}
	}
#endif
	/* drop the taken (and now resident) requests from the queue */
	{
		int w = 0, np = 0;
		for (i = 0; i < n; i++) {
			int q = queue[(qhead + i) & (QLEN - 1)];
			if (taken[i]) { pendingbit[q >> 3] &= ~(1 << (q & 7)); continue; }
			queue[(qhead + w) & (QLEN - 1)] = q;
#ifdef PS1_PROF
			QF(qhead + w) = QF(qhead + i);
#endif
			if (i < nprio) np++;
			w++;
		}
		qtail = (qhead + w) & (QLEN - 1); nprio = np;
	}
	cd_done = 0;
#ifdef PS1_PROF
	prof_cmds++;
#endif
	if (!pak_read_async(tiledata->sector + lo, end, staging, cd_cb)) {
		for (i = 0; i < nrun; i++) evict_slot(run_slot[i]);   /* the tiles are requested again by the next frame that needs them */
		nrun = 0;
	}
}

static void pump(void)
{
	if (!inflight_finish()) return;
	if (pak_busy()) return;   /* a music block read is in flight or the previous read's pause has not completed: retry next frame */
	while (qhead != qtail) {
		int picnum = queue[qhead];
		int p, np;
#ifdef PS1_PROF
		u16 qfh = QF(qhead);
#else
		u16 qfh = 0;
#endif
		qhead = (qhead + 1) & (QLEN - 1);
		if (nprio) nprio--;
		pendingbit[picnum >> 3] &= ~(1 << (picnum & 7));
		np = tilenpieces[picnum] ? tilenpieces[picnum] : 1;
		if (np == 1) {
			if (find_slot(picnum, 0) >= 0) continue;
			start_run(picnum, qfh);
			return;
		}
		for (p = 0; p < np; p++) {   /* multi-piece: load synchronously (rare, menus) */
			int stride, h, s;
			u32 sector;
			if (find_slot(picnum, p) >= 0) continue;
			s = begin_load(picnum, p, &stride, &h, &sector);
			if (s < 0) return;
#ifdef PS1_PROF
			prof_cmds++;
#endif
			if (pak_read(tiledata->sector + sector, (stride * h + 2047) / 2048, staging) < 0) { evict_slot(s); return; }
			finish_load(s, stride, h);
		}
	}
}

int tex_lookup(int picnum, int piece, TexRef *out)
{
	int s = find_slot(picnum, piece);
	if (s >= 0 && slots[s].state == 2) {
		Slot *sl = &slots[s];
		Node *n = &nodes[sl->node];
		const Page *pg = &pages[n->page];
#ifdef PS1_PROF
		if (sl->lastuse == 0) prof_rahit++;   /* a read-ahead tile used for the first time */
#endif
		sl->lastuse = texframe;
		out->tpage = getTPage(1, 0, pg->cellx, pg->celly);
		out->u = n->x; out->v = n->y;
		out->w = sl->w; out->h = sl->h;
		out->bw = 1 << n->lw; out->bh = 1 << n->lh;
		out->xoff = sl->xoff; out->yoff = sl->yoff;
		out->picnum = picnum; out->tw = tilesizx[picnum]; out->th = tilesizy[picnum];
		out->npieces = tilenpieces[picnum] ? tilenpieces[picnum] : 1;
		out->half = sl->half;
		{
			int mw = (~((out->bw >> 3) - 1)) & 31, mh = (~((out->bh >> 3) - 1)) & 31;
			out->twin = 0xe2000000 | mw | (mh << 5) | (((n->x >> 3) & 31) << 10) | (((n->y >> 3) & 31) << 15);
		}
		return 1;
	}
	tex_stat_misses++;
	if (s < 0) queue_tile(picnum);
	return 0;
}

int tex_npieces(int picnum) { return tilenpieces[picnum] ? tilenpieces[picnum] : 1; }

/* PS1: 1 if every piece of the tile is resident in VRAM (does not touch LRU, no miss stat). When request is set
   and the tile is neither resident nor already loading, it is queued for streaming like a tex_lookup() miss. */
int tex_resident(int picnum, int request)
{
	int p, np = tilenpieces[picnum] ? tilenpieces[picnum] : 1, ok = 1;
	for (p = 0; p < np; p++) {
		int s = find_slot(picnum, p);
		if (s >= 0 && slots[s].state == 2) continue;
		ok = 0;
		if (s < 0 && request) queue_tile_prio(picnum);
		break;
	}
	return ok;
}

int tex_lookup_sync(int picnum, int piece, TexRef *out)
{
	if (tex_lookup(picnum, piece, out)) return 1;
	/* finish any in-flight async load first so the staging buffer is free */
	while (!inflight_finish()) ;
	tex_preload(picnum);
	return tex_lookup(picnum, piece, out);
}
int tex_vram_used(void) { return vram_used; }
/* CD sector of the (first piece of the) tile, used to sort preloads and avoid seeks. */
u32 tex_sector(int picnum) { return tilenpieces[picnum] > 1 ? piecetab[tileaux[picnum]].sector : tilesect[picnum]; }
/* Percentage of the cache VRAM that is allocated. */
int tex_vram_pct(void) { return (int)(vram_used * 100 / VRAM_TEXELS); }
/* Percentage of the cache VRAM that is in the recent working set (drives the half-res decision). */
int tex_active_pct(void) { return (int)(active_texels_now() * 100 / VRAM_TEXELS); }

/* Must be called once per frame while the GPU is idle (video_wait_gpu() first). */
void tex_frame(void)
{
	pump();        /* before texframe++: slots touched by the frame being submitted stay protected */
	texframe++;
#ifdef PS1_PROF
	prof_report();
#endif
}

/* PS1: finish the in-flight streaming read and drop the queued tiles, so that nothing touches the CD or
   the staging buffer until a 3D frame asks for tiles again (loading screens, memory card access). */
void tex_quiesce(void)
{
	while (!inflight_finish()) ;
	qhead = qtail = nprio = 0;
	memset(pendingbit, 0, sizeof(pendingbit));
}

void tex_flush(void)
{
	int s;
	tex_quiesce();
	/* PS1: locked slots (fonts, the loading screen art) survive: the level loader keeps drawing them */
	for (s = 0; s < MAXSLOTS; s++) if (slots[s].state && !slots[s].lock) evict_slot(s);
}

void tex_lock(int picnum, int lock)
{
	int p, np = tilenpieces[picnum] ? tilenpieces[picnum] : 1;
	for (p = 0; p < np; p++) {   /* PS1: every piece of a multi-piece tile (the loading screen art) */
		int s = find_slot(picnum, p);
		if (s >= 0) slots[s].lock = lock;
	}
}
