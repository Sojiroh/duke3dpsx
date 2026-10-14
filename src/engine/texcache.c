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
/* upload geometry matching mkdata.py: stride multiple of 16 texels (min 16), even row count */
static inline int piece_stride(int w) { int s = (w + 15) & ~15; return s < 16 ? 16 : s; }
static inline int piece_rows(int h) { return h + (h & 1); }

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
int tex_stat_resident, tex_stat_misses, tex_stat_loads;

/* ---- piece table (only multi-piece tiles) ------------------------------ */
typedef struct { u16 tile, xoff, yoff, w, h, pad; u32 sector; } PieceRec;
static PieceRec piecetab[DAT_NUMPIECES > 0 ? DAT_NUMPIECES : 1] __attribute__((aligned(4)));

/* ---- streaming --------------------------------------------------------- */
#define QLEN 64
static u16 queue[QLEN];
static int qhead, qtail;
static u8 pendingbit[(MAXTILES + 7) >> 3];
static u32 staging[65536 / 4];
static volatile int cd_done;
static int inflight_slot = -1;
static int inflight_stride, inflight_h;
static const PakEntry *tiledata;

/* returns 1 if the half-resolution copy was selected */
static int piece_info(int picnum, int piece, int *xoff, int *yoff, int *w, int *h, u32 *sector)
{
	if (tilenpieces[picnum] <= 1) {
		*xoff = *yoff = 0; *w = tilesizx[picnum]; *h = tilesizy[picnum]; *sector = tilesect[picnum];
		if ((tileflags[picnum] & 1) && vram_used > VRAM_PRESSURE) {
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
	qhead = qtail = 0;
	tiledata = pak_find("TILEDATA");
	if (!tiledata) fatal("TILEDATA missing");
	const PakEntry *e = pak_find("PIECES");
	if (e && DAT_NUMPIECES > 0) pak_read_bytes(e, 0, piecetab, sizeof(PieceRec) * DAT_NUMPIECES);
	tex_stat_resident = 0;
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
	if (sl->node >= 0) { vram_used -= (1 << nodes[sl->node].lw) * (1 << nodes[sl->node].lh); node_free(sl->node); }
	if (sl->piece == 0) tile2slot[sl->picnum] = 0xffff;
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
	s = alloc_slot();
	if (s < 0) return -1;
	s16 n = alloc_block(w, hh);
	if (n < 0) return -1;
	Slot *sl = &slots[s];
	nodes[n].state = 2; nodes[n].slot = s;
	sl->picnum = picnum; sl->piece = piece; sl->node = n; sl->state = 1; sl->lock = 0; sl->half = half;
	sl->w = w; sl->h = hh; sl->xoff = xoff; sl->yoff = yoff; sl->lastuse = texframe;
	vram_used += (1 << nodes[n].lw) * (1 << nodes[n].lh);
	if (piece == 0) tile2slot[picnum] = s;
	*stride = piece_stride(w); *h = piece_rows(hh); *sector = sec;
	tex_stat_resident++;
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

/* Preload (and optionally lock) the single-piece tiles first..last. Consecutive tiles that are adjacent
   on the disc are fetched with one read (fonts: ~100 tiny tiles, otherwise one CD seek each). */
void tex_preload_range(int first, int last, int lock)
{
	int t = first;
	while (inflight_slot >= 0) { if (cd_done) { finish_load(inflight_slot, inflight_stride, inflight_h); inflight_slot = -1; cd_done = 0; } }
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

void tex_preload(int picnum)
{
	int np = tilenpieces[picnum] ? tilenpieces[picnum] : 1, p;
	for (p = 0; p < np; p++) {
		int stride, h, s;
		u32 sector;
		if (find_slot(picnum, p) >= 0) continue;
		s = begin_load(picnum, p, &stride, &h, &sector);
		if (s < 0) return;
		if (pak_read(tiledata->sector + sector, (stride * h + 2047) / 2048, staging) < 0) { evict_slot(s); return; }
		finish_load(s, stride, h);
	}
}

static void cd_cb(void) { cd_done = 1; }

static void pump(void)
{
	if (inflight_slot >= 0) {
		if (!cd_done) return;
		finish_load(inflight_slot, inflight_stride, inflight_h);
		inflight_slot = -1;
		cd_done = 0;
	}
	if (pak_busy()) return;   /* a music block read is in flight or the previous read's pause has not completed: retry next frame */
	while (qhead != qtail) {
		int picnum = queue[qhead];
		int p, np;
		qhead = (qhead + 1) & (QLEN - 1);
		pendingbit[picnum >> 3] &= ~(1 << (picnum & 7));
		np = tilenpieces[picnum] ? tilenpieces[picnum] : 1;
		for (p = 0; p < np; p++) {
			int stride, h, s;
			u32 sector;
			if (find_slot(picnum, p) >= 0) continue;
			s = begin_load(picnum, p, &stride, &h, &sector);
			if (s < 0) return;
			if (np > 1) {   /* multi-piece: load synchronously (rare, menus) */
				if (pak_read(tiledata->sector + sector, (stride * h + 2047) / 2048, staging) < 0) { evict_slot(s); return; }
				finish_load(s, stride, h);
				continue;
			}
			cd_done = 0;
			if (!pak_read_async(tiledata->sector + sector, (stride * h + 2047) / 2048, staging, cd_cb)) {
				evict_slot(s);
				return;
			}
			inflight_slot = s; inflight_stride = stride; inflight_h = h;
			return;
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
	if (s < 0 && !(pendingbit[picnum >> 3] & (1 << (picnum & 7)))) {
		int next = (qtail + 1) & (QLEN - 1);
		if (next != qhead) {
			queue[qtail] = picnum; qtail = next;
			pendingbit[picnum >> 3] |= 1 << (picnum & 7);
		}
	}
	return 0;
}

int tex_npieces(int picnum) { return tilenpieces[picnum] ? tilenpieces[picnum] : 1; }

int tex_lookup_sync(int picnum, int piece, TexRef *out)
{
	if (tex_lookup(picnum, piece, out)) return 1;
	/* finish any in-flight async load first so the staging buffer is free */
	while (inflight_slot >= 0) { if (cd_done) { finish_load(inflight_slot, inflight_stride, inflight_h); inflight_slot = -1; cd_done = 0; } }
	tex_preload(picnum);
	return tex_lookup(picnum, piece, out);
}
int tex_vram_used(void) { return vram_used; }
/* CD sector of the (first piece of the) tile, used to sort preloads and avoid seeks. */
u32 tex_sector(int picnum) { return tilenpieces[picnum] > 1 ? piecetab[tileaux[picnum]].sector : tilesect[picnum]; }
/* Percentage of the cache VRAM that is allocated. */
int tex_vram_pct(void) { return (int)(vram_used * 100 / VRAM_TEXELS); }

/* Must be called once per frame while the GPU is idle (video_wait_gpu() first). */
void tex_frame(void)
{
	texframe++;
	pump();
}

void tex_flush(void)
{
	int s;
	while (inflight_slot >= 0) { if (cd_done) { finish_load(inflight_slot, inflight_stride, inflight_h); inflight_slot = -1; } }
	for (s = 0; s < MAXSLOTS; s++) if (slots[s].state) evict_slot(s);
	qhead = qtail = 0;
	memset(pendingbit, 0, sizeof(pendingbit));
}

void tex_lock(int picnum, int lock)
{
	int s = find_slot(picnum, 0);
	if (s >= 0) slots[s].lock = lock;
}
