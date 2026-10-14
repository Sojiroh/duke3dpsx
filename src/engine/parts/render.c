/* GPU back end for the Build engine (PS1 port).
 *
 * Build's visibility traversal produces per-column vertical spans with exact
 * occlusion (umost/dmost).  Instead of rasterising those spans in software we
 * convert runs of columns into GPU quads: walls as adaptive-width strips with
 * per-column texture u and depth-derived shading, floors/ceilings as strips
 * subdivided into depth bands so the affine interpolation stays close to the
 * true perspective mapping.  Every pixel is covered (nearly) once, so no depth
 * sorting is needed; primitives are drawn in emission order.  Chord edges that
 * miss the per-column boundary are hidden by EDGE_EXT (see below) and the game
 * clears the 3D window first, so rare uncovered pixels end up black.
 */

#define MAXSTRIP 32
#define MAXSPLIT_EXACT 8   /* exact period-boundary splits of non-pow2 tiles may recurse deeper than midpoint splits */
#ifndef PRIM_RESERVE
#define PRIM_RESERVE 6144  /* see prim_3d_full(); overridable with -DPRIM_RESERVE=... to test the 2D reserve */
#endif
#define MAXSPLIT 4

/* PS1: the frame buffer is FB_W (256) pixels wide while the engine works in 320 logical columns.  Every x that
   is written into a GPU primitive (or draw area / offset) goes through one of these two macros, so that
   neighbouring quads keep sharing exact endpoints.  SCR_X: logical 320-space pixel; SCR_X3D: 3D column index
   (the column engine runs at xdimen = width >> REND_XSHIFT = 160 columns, 1.6 physical pixels each). */
#define SCR_X(x)   (((x) * FB_W) / SCREEN_W)
#define SCR_X3D(x) (((x) * 8) / 5)

typedef struct { short x, y; int u, v; unsigned char c; } QV;  /* u,v unwrapped texels; c brightness 0..128 */

static u32 last_twin;
static int rend_sync_loads;      /* 2D path (rotatesprite): load missing tiles synchronously; also marks the 2D phase (no primitive reserve) */
int rend_dropped_3d;             /* 3D strips/quads dropped because only the 2D/tint reserve was left (stat) */
int rend_mirrors_supported = 0;  /* completemirror() cannot reverse the screen on the GPU path: the game skips its mirror pass */
static int keepalive_frame = -1;
static int keepalive_vsync;

/* Call from busy-wait loops: if no frame was presented since the last call, re-present the
   last one (with the current tint) once per vblank so static screens stay visible/fade. */
void rend_keepalive(void)
{
	int f = video_frame(), v = video_vsync_count();
	if (f != keepalive_frame) { keepalive_frame = f; keepalive_vsync = v; return; }
	if (v - keepalive_vsync >= 1) { rend_represent(); keepalive_vsync = video_vsync_count(); }
}

/* perspective bands for floors/ceilings: |y - horizon| cut positions */
static const short bandcuts[] = {2,3,4,6,8,11,15,20,27,36,48,64,85,113,150,200,266};
#define NBANDS ((int)(sizeof(bandcuts)/sizeof(bandcuts[0])))

static int cur_offx, cur_offy;   /* current GPU drawing offset relative to the frame buffer */

/* GPU drawing offset: 3D view primitives use window-relative coordinates */
static void set_draw_offset(int x, int y)
{
	DR_OFFSET *p;
	if (x == cur_offx && y == cur_offy) return;
	p = prim_alloc(sizeof(DR_OFFSET));
	if (!p) return;
	setDrawOffset(p, video_drawenv->clip.x + video_drawenv->ofs[0] + SCR_X(x), video_drawenv->clip.y + video_drawenv->ofs[1] + y);
	prim_link(p);
	cur_offx = x; cur_offy = y;
}

static void rend_setview(void) {}
static void rend_begin_frame(void) { set_draw_offset(windowx1, windowy1); }
static inline void rend_2d(void) { set_draw_offset(0, 0); }

static inline void set_twin(u32 twin)
{
	DR_TWIN *p;
	if (twin == last_twin) return;
	p = prim_alloc(sizeof(DR_TWIN));
	if (!p) return;
	setlen(p, 1);
	p->code[0] = twin;
	prim_link(p);
	last_twin = twin;
}

/* Present the last frame again with the current tint (palette fades on static screens). */
int rend_represent_enabled = 1;
void rend_represent(void)
{
	int r, g, b, k = pal_get_tint(&r, &g, &b);
	int abr;
	if (!rend_represent_enabled) return;
	if (k > 63) k = 63;
	if ((r | g | b) == 0) { abr = 2; r = g = b = 63; } else abr = 1;   /* black: subtractive, else additive (as rend_draw_tint) */
	video_represent(k > 0, abr, r * 4 * k / 63, g * 4 * k / 63, b * 4 * k / 63);
}

static void rend_draw_tint(void)
{
	int r, g, b, k = pal_get_tint(&r, &g, &b);
	POLY_F4 *p;
	DR_TPAGE *tp;
	int abr;
	video_mark_overlay();
	if (k <= 0) return;
	rend_2d();
	if (k > 63) k = 63;
	if ((r | g | b) == 0) { abr = 2; r = g = b = 63; }      /* fade to black: subtractive */
	else abr = 1;                                             /* colour flash: additive */
	tp = prim_alloc(sizeof(DR_TPAGE));
	if (!tp) return;
	setDrawTPage(tp, 0, 1, getTPage(0, abr, 0, 0));
	prim_link(tp);
	p = prim_alloc(sizeof(POLY_F4));
	if (!p) return;
	setPolyF4(p);
	setRGB0(p, r * 4 * k / 63, g * 4 * k / 63, b * 4 * k / 63);
	setXY4(p, 0, 0, FB_W, 0, 0, SCREEN_H, FB_W, SCREEN_H);
	setSemiTrans(p, 1);
	prim_link(p);
}

#ifdef PS1_PROF
/* PS1 debug: every 120 frames print texture-cache and primitive-buffer statistics */
static int prof_flat;   /* placeholder quads emitted by emit_flat (tile not resident) */
static void rend_prof_frame(void)
{
	static int n, maxbytes;
	int b = video_list_bytes();
	if (b > maxbytes) maxbytes = b;
	if (++n < 120) return;
	dbg("PROF tex: resident %d half-res %d vram %d%% active %d%% loads %d (half %d, old rule would be half %d) misses %d flat/frame %d.%02d | prims: max list %d bytes, dropped 3D %d (reserve %d)\n",
		tex_stat_resident, tex_stat_half, tex_vram_pct(), tex_active_pct(), tex_stat_loads, tex_stat_halfloads, tex_stat_oldhalf, tex_stat_misses,
		prof_flat / 120, prof_flat * 100 / 120 % 100, maxbytes, rend_dropped_3d, PRIM_RESERVE);
	n = 0; maxbytes = 0; rend_dropped_3d = 0; prof_flat = 0;
}
#endif

static void rend_end_frame(void)
{
	rend_draw_tint();
#ifdef PS1_PROF
	rend_prof_frame();
#endif
	video_wait_gpu();      /* GPU idle: texture uploads below execute immediately */
	tex_frame();
	clut_frame();
	video_flip();
	last_twin = 0;
	cur_offx = cur_offy = 0;
}

/* ---- quad emission with texture-range splitting ---------------------- */

static inline int floordiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }

/* PS1: the 3D emitters stop PRIM_RESERVE bytes before the end of the primitive buffer so that the 2D
   overlay emitted after the 3D pass (weapon, HUD, menus via rotatesprite, and the tint) always fits. */
static inline int prim_3d_full(void) { return !rend_sync_loads && (prim_end - prim_cur) < PRIM_RESERVE; }

static void emit_quad_final(const TexRef *ref, u16 clut, const QV *q, int trans, int gouraud, int ubase, int vbase, int uadd, int vadd)
{
	if (prim_3d_full()) { rend_dropped_3d++; return; }
	PROF_COUNT(prof_nquad, 1);
	set_twin(ref->twin);
	if (gouraud) {
		POLY_GT4 *p = prim_alloc(sizeof(POLY_GT4));
		if (!p) return;
		setPolyGT4(p);
		setXY4(p, q[0].x, q[0].y, q[1].x, q[1].y, q[2].x, q[2].y, q[3].x, q[3].y);
		setUV4(p, q[0].u - ubase + uadd, q[0].v - vbase + vadd, q[1].u - ubase + uadd, q[1].v - vbase + vadd,
			   q[2].u - ubase + uadd, q[2].v - vbase + vadd, q[3].u - ubase + uadd, q[3].v - vbase + vadd);
		setRGB0(p, q[0].c, q[0].c, q[0].c); setRGB1(p, q[1].c, q[1].c, q[1].c);
		setRGB2(p, q[2].c, q[2].c, q[2].c); setRGB3(p, q[3].c, q[3].c, q[3].c);
		p->tpage = ref->tpage; p->clut = clut;
		setSemiTrans(p, trans);
		prim_link(p);
	} else {
		POLY_FT4 *p = prim_alloc(sizeof(POLY_FT4));
		if (!p) return;
		setPolyFT4(p);
		setXY4(p, q[0].x, q[0].y, q[1].x, q[1].y, q[2].x, q[2].y, q[3].x, q[3].y);
		setUV4(p, q[0].u - ubase + uadd, q[0].v - vbase + vadd, q[1].u - ubase + uadd, q[1].v - vbase + vadd,
			   q[2].u - ubase + uadd, q[2].v - vbase + vadd, q[3].u - ubase + uadd, q[3].v - vbase + vadd);
		setRGB0(p, q[0].c, q[0].c, q[0].c);
		p->tpage = ref->tpage; p->clut = clut;
		setSemiTrans(p, trans);
		prim_link(p);
	}
}

static inline void qv_mid(QV *m, const QV *a, const QV *b)
{
	m->x = (a->x + b->x) >> 1; m->y = (a->y + b->y) >> 1;
	m->u = (a->u + b->u) >> 1; m->v = (a->v + b->v) >> 1;
	m->c = (a->c + b->c) >> 1;
}

static inline void qv_lerp(QV *m, const QV *a, const QV *b, int t /* 0..65536 */)
{
	m->x = a->x + (((b->x - a->x) * t) >> 16); m->y = a->y + (((b->y - a->y) * t) >> 16);
	m->u = a->u + (int)(((long long)(b->u - a->u) * t) >> 16); m->v = a->v + (int)(((long long)(b->v - a->v) * t) >> 16);
	m->c = a->c + (((b->c - a->c) * t) >> 16);
}

/* parameter t (16.16) where value k lies between a and b, or -1 if outside */
static inline int lerp_param(int a, int b, int k)
{
	int d = b - a;
	if (d == 0) return -1;
	if ((k - a) * (long long)d < 0) return -1;
	if (klabs(k - a) > klabs(d)) return -1;
	return divscale(k - a, d, 16);
}

/* Split quad q along the level set (u == k, or v == k when usev) into a and b. 0 if impossible. */
static int quad_split_level(const QV *q, int k, int usev, QV *a, QV *b)
{
	int v0 = usev ? q[0].v : q[0].u, v1 = usev ? q[1].v : q[1].u, v2 = usev ? q[2].v : q[2].u, v3 = usev ? q[3].v : q[3].u;
	int tt = lerp_param(v0, v1, k), tb = lerp_param(v2, v3, k);
	if (tt >= 0 && tb >= 0) {
		QV pt, pb;
		qv_lerp(&pt, &q[0], &q[1], tt); qv_lerp(&pb, &q[2], &q[3], tb);
		if (usev) { pt.v = k; pb.v = k; } else { pt.u = k; pb.u = k; }
		a[0] = q[0]; a[1] = pt; a[2] = q[2]; a[3] = pb;
		b[0] = pt; b[1] = q[1]; b[2] = pb; b[3] = q[3];
		return 1;
	}
	{
		int tl = lerp_param(v0, v2, k), tr = lerp_param(v1, v3, k);
		if (tl >= 0 && tr >= 0) {
			QV pl, pr;
			qv_lerp(&pl, &q[0], &q[2], tl); qv_lerp(&pr, &q[1], &q[3], tr);
			if (usev) { pl.v = k; pr.v = k; } else { pl.u = k; pr.u = k; }
			a[0] = q[0]; a[1] = q[1]; a[2] = pl; a[3] = pr;
			b[0] = pl; b[1] = pr; b[2] = q[2]; b[3] = q[3];
			return 1;
		}
	}
	return 0;
}

static void emit_quad(const TexRef *ref, u16 clut, const QV *q, int trans, int gouraud, int depth);

/* Multi-piece tiles: coordinates are relative to the whole tile (already wrapped into
   [0,tw) x [0,th) by the caller); split at 256-texel piece boundaries and draw each part
   with the piece's own VRAM block. */
static void emit_quad_pieces(const TexRef *ref, u16 clut, const QV *q, int trans, int gouraud, int depth)
{
	int umin, umax, vmin, vmax, i, k;
	QV a[4], b[4];
	umin = umax = q[0].u; vmin = vmax = q[0].v;
	for (i = 1; i < 4; i++) {
		if (q[i].u < umin) umin = q[i].u;
		if (q[i].u > umax) umax = q[i].u;
		if (q[i].v < vmin) vmin = q[i].v;
		if (q[i].v > vmax) vmax = q[i].v;
	}
	if (depth < 8) {
		k = (umin / 256 + 1) * 256;
		if (k <= umax - 1 && quad_split_level(q, k, 0, a, b)) {
			emit_quad_pieces(ref, clut, a, trans, gouraud, depth + 1);
			emit_quad_pieces(ref, clut, b, trans, gouraud, depth + 1);
			return;
		}
		k = (vmin / 256 + 1) * 256;
		if (k <= vmax - 1 && quad_split_level(q, k, 1, a, b)) {
			emit_quad_pieces(ref, clut, a, trans, gouraud, depth + 1);
			emit_quad_pieces(ref, clut, b, trans, gouraud, depth + 1);
			return;
		}
	}
	/* single piece: look it up */
	{
		int pu = umin / 256, pv = vmin / 256, pw = (ref->tw + 255) / 256;
		int piece = pv * pw + pu;
		TexRef pref;
		QV c[4];
		if (piece < 0 || piece >= ref->npieces) return;
		if (!(rend_sync_loads ? tex_lookup_sync(ref->picnum, piece, &pref) : tex_lookup(ref->picnum, piece, &pref))) return;
		int ulim = (pref.u + pref.w <= 255) ? pref.w : pref.w - 1;   /* exclusive edge u==w is never sampled: allowed while the u8 coordinate fits */
		int vlim = (pref.v + pref.h <= 255) ? pref.h : pref.h - 1;
		for (i = 0; i < 4; i++) {
			c[i] = q[i];
			c[i].u -= pu * 256; c[i].v -= pv * 256;
			if (c[i].u < 0) c[i].u = 0;
			if (c[i].u > ulim) c[i].u = ulim;
			if (c[i].v < 0) c[i].v = 0;
			if (c[i].v > vlim) c[i].v = vlim;
		}
		emit_quad_final(&pref, clut, c, trans, gouraud, 0, 0, pref.u, pref.v);
	}
}

/* q: 0=TL 1=TR 2=BL 3=BR; u,v in full-resolution texels of the tile */
static int emit_single_v;   /* set by wall_spans for masked quads: V never repeats (see emit_quad) */
static void emit_quad(const TexRef *ref, u16 clut, const QV *q, int trans, int gouraud, int depth)
{
	int umin, umax, vmin, vmax, i, ubase, vbase, ulim, vlim, uadd, vadd, uneed, vneed;
	int wrapw = (ref->w == ref->bw), wraph = (ref->h == ref->bh);
	QV hq[4], vq[4];
	if (depth == 0 && prim_3d_full()) { rend_dropped_3d++; return; }

	if (ref->half && depth == 0) {
		for (i = 0; i < 4; i++) { hq[i] = q[i]; hq[i].u >>= 1; hq[i].v >>= 1; }
		q = hq;
	}
	umin = umax = q[0].u; vmin = vmax = q[0].v;
	for (i = 1; i < 4; i++) {
		if (q[i].u < umin) umin = q[i].u;
		if (q[i].u > umax) umax = q[i].u;
		if (q[i].v < vmin) vmin = q[i].v;
		if (q[i].v > vmax) vmax = q[i].v;
	}
	if (ref->npieces > 1) {
		/* whole-tile coordinates wrapped into one period, then split per piece */
		QV c[4];
		int tw = ref->tw, th = ref->th;
		ubase = floordiv(umin, tw) * tw; vbase = floordiv(vmin, th) * th;
		if ((umax - ubase > tw || vmax - vbase > th) && depth < MAXSPLIT) {
			QV a[4], b[4];
			int k;
			k = umax - ubase > tw ? ubase + tw : vbase + th;
			if (quad_split_level(q, k, umax - ubase <= tw, a, b)) {
				emit_quad(ref, clut, a, trans, gouraud, depth + 1);
				emit_quad(ref, clut, b, trans, gouraud, depth + 1);
				return;
			}
		}
		for (i = 0; i < 4; i++) {
			c[i] = q[i]; c[i].u -= ubase; c[i].v -= vbase;
			if (c[i].u < 0) c[i].u = 0;
			if (c[i].u > tw) c[i].u = tw;      /* u == tw: exclusive edge */
			if (c[i].v < 0) c[i].v = 0;
			if (c[i].v > th) c[i].v = th;
		}
		emit_quad_pieces(ref, clut, c, trans, gouraud, 0);
		return;
	}
	if (wrapw) { ubase = umin & ~(ref->w - 1); ulim = 255; uadd = 0; }
	else {
		ubase = floordiv(umin, ref->w) * ref->w; uadd = ref->u;
		/* PS1: the exclusive right edge (u == w) is never sampled, so it may sit on the period boundary
		   without a split, as long as the 8-bit coordinate window holds it */
		ulim = (ref->u + ref->w <= 255) ? ref->w : ref->w - 1;
	}
	if (wraph) { vbase = vmin & ~(ref->h - 1); vlim = 255; vadd = 0; }
	else {
		vbase = floordiv(vmin, ref->h) * ref->h; vadd = ref->v;
		vlim = (ref->v + ref->h <= 255) ? ref->h : ref->h - 1;
		/* PS1: the software renderer's V wraps at the next power of two above the tile height
		   (vplc >> globalshiftval, shift = 32 - ceil(log2 h)), and sprites / masked walls (one image, never
		   tiled vertically) start at an arbitrary multiple of it.  Wrapping at h instead rotated a non-pow2
		   sprite (tile 52, h 27: v 65..92 was drawn as rows 11..26 then 0..11).  For single-image quads use
		   the power-of-two period's base and clamp the few rounding texels that stick out into the padding. */
		if (emit_single_v) {
			int period = 1;
			while (period < ref->th) period <<= 1;
			if (ref->half) period >>= 1;
			if (period > ref->h) {
				int pb = ((vmin + vmax) >> 1) & ~(period - 1);
				QV *vc = vq;
				for (i = 0; i < 4; i++) {
					vc[i] = q[i];
					if (vc[i].v < pb) vc[i].v = pb;
					if (vc[i].v > pb + vlim) vc[i].v = pb + vlim;
				}
				q = vc; vbase = pb;
				vmin = vmax = q[0].v;
				for (i = 1; i < 4; i++) { if (q[i].v < vmin) vmin = q[i].v; if (q[i].v > vmax) vmax = q[i].v; }
			}
		}
	}
	uneed = (umax - ubase > ulim);
	vneed = (vmax - vbase > vlim);
	if ((uneed || vneed) && depth < MAXSPLIT_EXACT) {
		QV a[4], b[4];
		/* PS1: a non-power-of-2 axis repeats with period w (h): cut exactly at the period boundary so the
		   repeat is seamless (midpoint splitting + clamping smeared the last texel of every repeat) */
		if ((uneed && !wrapw && umax - ubase > ref->w && quad_split_level(q, ubase + ref->w, 0, a, b)) ||
		    (vneed && !wraph && vmax - vbase > ref->h && quad_split_level(q, vbase + ref->h, 1, a, b))) {
			emit_quad(ref, clut, a, trans, gouraud, depth + 1);
			emit_quad(ref, clut, b, trans, gouraud, depth + 1);
			return;
		}
		if (depth < MAXSPLIT) {
			int du_h = klabs(q[1].u - q[0].u) + klabs(q[3].u - q[2].u) + klabs(q[1].v - q[0].v) + klabs(q[3].v - q[2].v);
			int du_v = klabs(q[2].u - q[0].u) + klabs(q[3].u - q[1].u) + klabs(q[2].v - q[0].v) + klabs(q[3].v - q[1].v);
			if (du_h >= du_v && (q[1].x - q[0].x) > 1) {
				/* split into left / right */
				a[0] = q[0]; qv_mid(&a[1], &q[0], &q[1]); a[2] = q[2]; qv_mid(&a[3], &q[2], &q[3]);
				b[0] = a[1]; b[1] = q[1]; b[2] = a[3]; b[3] = q[3];
			} else if ((q[2].y - q[0].y) > 1 || (q[3].y - q[1].y) > 1) {
				/* split into top / bottom */
				a[0] = q[0]; a[1] = q[1]; qv_mid(&a[2], &q[0], &q[2]); qv_mid(&a[3], &q[1], &q[3]);
				b[0] = a[2]; b[1] = a[3]; b[2] = q[2]; b[3] = q[3];
			} else goto clamp;
			emit_quad(ref, clut, a, trans, gouraud, depth + 1);
			emit_quad(ref, clut, b, trans, gouraud, depth + 1);
			return;
		}
	}
clamp:
	if (uneed || vneed) {
		/* too dense to represent: clamp coordinates (far-away repeats) */
		QV c[4];
		for (i = 0; i < 4; i++) {
			c[i] = q[i];
			if (c[i].u - ubase > ulim) c[i].u = ubase + ulim;
			if (c[i].v - vbase > vlim) c[i].v = vbase + vlim;
		}
		emit_quad_final(ref, clut, c, trans, gouraud, ubase, vbase, uadd, vadd);
		return;
	}
	emit_quad_final(ref, clut, q, trans, gouraud, ubase, vbase, uadd, vadd);
}

/* untextured fallback while a tile is streaming in */
static void emit_flat(const QV *q, u16 col15, int trans)
{
#ifdef PS1_PROF
	prof_flat++;
#endif
	POLY_G4 *p = prim_3d_full() ? NULL : prim_alloc(sizeof(POLY_G4));
	int r = (col15 & 31) << 3, g = ((col15 >> 5) & 31) << 3, b = ((col15 >> 10) & 31) << 3, i;
	if (!p) { rend_dropped_3d++; return; }
	setPolyG4(p);
	setXY4(p, q[0].x, q[0].y, q[1].x, q[1].y, q[2].x, q[2].y, q[3].x, q[3].y);
	{
		int c0 = q[0].c, c1 = q[1].c, c2 = q[2].c, c3 = q[3].c;
		/* c is brightness 0..128 (128 = neutral) */
		setRGB0(p, r * c0 >> 7, g * c0 >> 7, b * c0 >> 7);
		setRGB1(p, r * c1 >> 7, g * c1 >> 7, b * c1 >> 7);
		setRGB2(p, r * c2 >> 7, g * c2 >> 7, b * c2 >> 7);
		setRGB3(p, r * c3 >> 7, g * c3 >> 7, b * c3 >> 7);
	}
	(void)i;
	setSemiTrans(p, trans);
	prim_link(p);
}

/* ---- wall-type spans ---------------------------------------------------- */

static inline int wall_bright(long swal)
{
	return shadebright[getpalookup(mulscale16(swal, globvis), globalshade)];
}

/* v texel (unwrapped) at column with scale sw, screen row y */
static inline int wall_v(long sw, long y)
{
	long vince = sw * globalyscale;   /* 32-bit like the software renderer */
	long long v = (long long)(unsigned long)globalzd + (long long)vince * (y - globalhoriz + 1);
	v >>= globalshiftval;
	if (v > (1 << 28)) v = (1 << 28);
	if (v < -(1 << 28)) v = -(1 << 28);
	return (int)v;
}


/* Right edge of a strip of columns xa..xb: the next column's range if it has one, else a linear
   extrapolation of the last two columns (clamped to the view). */
static inline void strip_right_edge(const short *top, const short *bot, long x2, long xa, long xb, int *topR, int *botR)
{
	if (xb + 1 <= x2 && top[xb + 1] < bot[xb + 1]) { *topR = top[xb + 1]; *botR = bot[xb + 1]; }
	else if (xb > xa) { *topR = 2 * top[xb] - top[xb - 1]; *botR = 2 * bot[xb] - bot[xb - 1]; }
	else { *topR = top[xa]; *botR = bot[xa]; }
	if (*topR < 0) *topR = 0;
	if (*botR > ydimen) *botR = ydimen;
	if (*botR < *topR) *botR = *topR;
}

/* PS1: the GPU draws a strip's top/bottom edge as a straight chord, which differs by a pixel or two from the
   per-column boundary the neighbouring primitive (wall vs floor/ceiling, or wall vs the sector seen through
   a portal) follows, so 1-pixel gaps are left that keep stale framebuffer content.  Build draws front to back:
   whatever is drawn first towards a boundary may therefore safely run EDGE_EXT pixels past it into the
   still free rows (clamped to umost/dmost); the primitive drawn later repaints them. */
#ifndef EDGE_EXT
#define EDGE_EXT 4
#endif

/* The strip is drawn as one quad whose edges are the chords between its end columns.  The merge test
   only looks at second differences, which lets a polyline corner of a few degrees slip through and the
   chord then cuts off a wedge of the real top/bottom boundary.  Verify the chord against every column. */
#ifndef STRIP_EPS
#define STRIP_EPS 2
#endif
/* 65536/n for strip lengths up to MAXSTRIP+1 */
static const unsigned short strip_recip[MAXSTRIP + 2] = {
	0, 65535, 32768, 21845, 16384, 13107, 10923, 9362, 8192, 7282, 6554, 5958, 5461, 5041, 4681, 4369, 4096,
	3855, 3641, 3449, 3277, 3121, 2979, 2849, 2731, 2621, 2521, 2427, 2341, 2260, 2185, 2114, 2048, 1986 };
static inline int chord_ok2(const short *top, const short *bot, long xa, long xb, int tR, int bR)
{
	long n = xb + 1 - xa, k;
	int tstep, bstep, tacc, bacc;
	if (n <= 2) return 1;
	tstep = (tR - top[xa]) * (int)strip_recip[n];
	bstep = (bR - bot[xa]) * (int)strip_recip[n];
	tacc = (top[xa] << 16) + (1 << 15);
	bacc = (bot[xa] << 16) + (1 << 15);
	for (k = xa + 1; k <= xb; k++) {
		tacc += tstep; bacc += bstep;
		if (klabs((tacc >> 16) - top[k]) > STRIP_EPS || klabs((bacc >> 16) - bot[k]) > STRIP_EPS) return 0;
	}
	return 1;
}

/* shrink xb until the chord edges follow the boundary within STRIP_EPS */
static inline long strip_fit(const short *top, const short *bot, long x2, long xa, long xb)
{
	int tR, bR;
	for (;;) {
		strip_right_edge(top, bot, x2, xa, xb, &tR, &bR);
		if (xb <= xa || chord_ok2(top, bot, xa, xb, tR, bR)) return xb;
		xb = xa + ((xb - xa) >> 1);
	}
}

/* mode: 0 solid (clip umost/dmost), 1 masked (clip view window), 2 translucent masked */
static void wall_spans(long x1, long x2, short *uwal, short *dwal, long *swal, long *lwal, int mode)
{
	long x, xa, xb, xr, tsizx, tsizy;
	int have, trans = (mode == 2), ext_t, ext_b;
	TexRef ref;
	u16 clut;
	short *clipT, *clipB;
	static short top[MAXXDIM + 2], bot[MAXXDIM + 2];

	tsizx = tilesizx[globalpicnum]; tsizy = tilesizy[globalpicnum];
	if ((tsizx <= 0) || (tsizy <= 0)) return;
	if ((uwal[x1] > ydimen) && (uwal[x2] > ydimen)) return;
	if ((dwal[x1] < 0) && (dwal[x2] < 0)) return;
	if (x1 < 0) x1 = 0;
	if (x2 >= xdimen) x2 = xdimen - 1;
	if (x1 > x2) return;

	setgotpic(globalpicnum);
	have = tex_lookup(globalpicnum, 0, &ref);
	clut = clut_get(globalpal, mode == 0);

	if (mode == 0) { clipT = umost; clipB = dmost; }
	else { clipT = &startumost[windowx1]; clipB = &startdmost[windowx1]; }
	/* PS1: edges that border rows not drawn yet: bottom of an upper wall / ceiling sky, top of a lower wall / floor sky */
	ext_b = (mode == 0) && (dwal == dwall || dwal == uplc);
	ext_t = (mode == 0) && (uwal == uwall || uwal == dplc);

	for (x = x1; x <= x2; x++) {
		int t = uwal[x], b = dwal[x];
		int ct = clipT[x], cb = clipB[x];
		if (mode != 0) { ct -= windowy1; cb -= windowy1; }
		if (t < ct) t = ct;
		if (b > cb) b = cb;
		if (t < 0) t = 0;
		if (b > ydimen) b = ydimen;
		if (t < b) {   /* PS1: run past the boundary into the free rows (see EDGE_EXT) */
			if (ext_t) { int e = uwal[x] - EDGE_EXT; if (e < ct) e = ct; if (e < 0) e = 0; if (e < t) t = e; }
			if (ext_b) { int e = dwal[x] + EDGE_EXT; if (e > cb) e = cb; if (e > ydimen) e = ydimen; if (e > b) b = e; }
		}
		top[x] = t; bot[x] = b;
	}

	PROF_COUNT(pc_wcols, x2 - x1 + 1);
	x = x1;
	while (x <= x2) {
		long swa, swr, ua, ur;
		QV q[4];
		int topL, botL, topR, botR;
		if (top[x] >= bot[x]) { x++; continue; }
		if (prim_3d_full()) { rend_dropped_3d++; return; }
		xa = x; xb = x;
		swa = swal[xa];
		while (xb + 1 <= x2 && (xb + 1 - xa) < MAXSTRIP) {
			long xn = xb + 1;
			long swn = swal[xn];
			if (top[xn] >= bot[xn]) break;
			if (swn * 4 > swa * 5 || swn * 5 < swa * 4) break;
			if (xn - xa >= 2) {
				if (klabs(top[xn] - 2 * top[xn - 1] + top[xn - 2]) > 1) break;
				if (klabs(bot[xn] - 2 * bot[xn - 1] + bot[xn - 2]) > 1) break;
			}
			xb = xn;
		}
		xb = strip_fit(top, bot, x2, xa, xb);
		xr = xb + 1; PROF_COUNT(pc_wstrips, 1);
		topL = top[xa]; botL = bot[xa];
		if (xb + 1 <= x2 && top[xb + 1] < bot[xb + 1]) { topR = top[xb + 1]; botR = bot[xb + 1]; }
		else if (xb > xa) { topR = 2 * top[xb] - top[xb - 1]; botR = 2 * bot[xb] - bot[xb - 1]; }
		else { topR = topL; botR = botL; }
		if (topR < 0) topR = 0;
		if (botR > ydimen) botR = ydimen;
		if (botR < topR) botR = topR;
		ua = lwal[xa] + globalxpanning;
		if (xb + 1 <= x2) { ur = lwal[xb + 1] + globalxpanning; swr = swal[xb + 1]; }
		else if (xb > xa) { ur = 2 * lwal[xb] - lwal[xb - 1] + globalxpanning; swr = 2 * swal[xb] - swal[xb - 1]; }
		else { ur = ua + 1; swr = swa; }
		if (swr <= 0) swr = swa;

		q[0].x = SCR_X3D(xa); q[0].y = topL; q[0].u = ua; q[0].v = wall_v(swa, topL); q[0].c = wall_bright(swa);
		q[1].x = SCR_X3D(xr); q[1].y = topR; q[1].u = ur; q[1].v = wall_v(swr, topR); q[1].c = wall_bright(swr);
		q[2].x = SCR_X3D(xa); q[2].y = botL; q[2].u = ua; q[2].v = wall_v(swa, botL); q[2].c = q[0].c;
		q[3].x = SCR_X3D(xr); q[3].y = botR; q[3].u = ur; q[3].v = wall_v(swr, botR); q[3].c = q[1].c;
		if (have) { emit_single_v = (mode != 0); emit_quad(&ref, clut, q, trans, 1, 0); emit_single_v = 0; }
		else if (mode == 0) emit_flat(q, tileavg[globalpicnum], 0);
		x = xb + 1;
	}
}

static void wallscan(long x1, long x2, short *uwal, short *dwal, long *swal, long *lwal)
{
	PROF_BEGIN(t);
	wall_spans(x1, x2, uwal, dwal, swal, lwal, 0);
	PROF_END(prof_wall, t); PROF_COUNT(prof_nwall, 1);
}

static void maskwallscan(long x1, long x2, short *uwal, short *dwal, long *swal, long *lwal)
{
	wall_spans(x1, x2, uwal, dwal, swal, lwal, 1);
}

static void transmaskwallscan(long x1, long x2)
{
	wall_spans(x1, x2, uwall, dwall, swall, lwall, 2);
}

/* ---- plane-type spans (floors, ceilings, floor sprites, slopes) ------- */

enum { PM_FLAT = 0, PM_FLOORSPRITE = 1, PM_SLOPE = 2 };
static int pm_mode;
static long pm_dax1;     /* first column for slope accumulators */
static int pm_xbits, pm_ybits;

static inline long horiz_r(long y)
{
	long i = y - globalhoriz + horizycent;
	if (i < 0) i = 0;
	if (i >= ydim * 4) i = ydim * 4 - 1;
	return horizlookup2[i];
}

static inline long horiz_r1(long y)
{
	long i = y - globalhoriz + horizycent;
	if (i < 0) i = 0;
	if (i >= ydim * 4) i = ydim * 4 - 1;
	return horizlookup[i];
}

static inline int clamp28(long long v)
{
	if (v > (1 << 28)) return (1 << 28);
	if (v < -(1 << 28)) return -(1 << 28);
	return (int)v;
}

/* texel coordinates + brightness at screen point (x,y) for the current plane */
static void plane_uv(long x, long y, int *u, int *v, int *c)
{
	long long uu, vv;
	switch (pm_mode) {
	case PM_FLAT: {
		long r = horiz_r(y);
		uu = ((long long)r * (globaly1 + globalx1 * x) + (long long)globalxpanning) >> (32 - pm_xbits);
		vv = ((long long)r * (globalx2 + globaly2 * x) + (long long)globalypanning) >> (32 - pm_ybits);
		*c = shadebright[getpalookup(mulscale16(r, globvis), globalshade)];
		break;
	}
	case PM_FLOORSPRITE: {
		long vy = mulscale20(globalzd, horiz_r1(y));
		uu = ((((long long)(globalx2 * x + globalx1) * vy) >> 14) + (long long)globalxpanning) >> (32 - pm_xbits);
		vv = ((((long long)(globaly2 * x + globaly1) * vy) >> 14) + (long long)globalypanning) >> (32 - pm_ybits);
		*c = shadebright[getpalookup(mulscale28(klabs(vy), globvis), globalshade)];
		break;
	}
	default: { /* PM_SLOPE */
		long dx = x - pm_dax1;
		long gx2 = globalx2 + globalx * dx, gy2 = globaly2 + globaly * dx;
		long D = mulscale16(y, globalzd) + ((globalzx + globalz * dx) >> 6);
		long rec;
		if (D == 0) D = 1;
		rec = krecipasm(D >> 3);
		uu = ((long long)globalx1 + (((long long)(gx2 >> 10) * rec) << 3)) >> (32 - pm_xbits);
		vv = ((long long)globaly1 + (((long long)(gy2 >> 10) * rec) << 3)) >> (32 - pm_ybits);
		*c = shadebright[getpalookup(mulscale24(krecipasm(D), globvis), globalshade)];
		break;
	}
	}
	*u = clamp28(uu);
	*v = clamp28(vv);
}

static inline void plane_vertex(QV *q, long x, long y)
{
	int u, v, c;
	PROF_COUNT(pc_pverts, 1);
	plane_uv(x, y, &u, &v, &c);
	q->x = SCR_X3D(x); q->y = y; q->u = u; q->v = v; q->c = c;
}

/* emit one cell given its four corners (TL,TR,BL,BR already evaluated) */
static inline void plane_emit(const TexRef *ref, int have, u16 clut, const QV *q, int trans)
{
	if ((q[0].y >= q[2].y) && (q[1].y >= q[3].y)) return;
	if (have) emit_quad(ref, clut, q, trans, 1, 0);
	else emit_flat(q, tileavg[globalpicnum], trans);
}


/* Adjacent strips share their edge column: the right-edge vertices of one strip are the left-edge
   vertices of the next when the strip ends because of the strip length limit or a smooth boundary.
   Remember them (keyed by row) so plane_uv() runs once per shared vertex. */
#define VC_MAX 40
typedef struct { long x; int n; short y[VC_MAX]; QV v[VC_MAX]; } VertCache;
static VertCache vc_a, vc_b;
static VertCache *vc_prev = &vc_a, *vc_cur = &vc_b;

static inline void plane_vertex_left(QV *q, long x, long y)
{
	VertCache *c = vc_prev;
	if (c->x == x) {
		int i;
		for (i = 0; i < c->n; i++)
			if (c->y[i] == y) { *q = c->v[i]; return; }
	}
	plane_vertex(q, x, y);
}

static inline void plane_vertex_right(QV *q, long x, long y)
{
	VertCache *c = vc_cur;
	plane_vertex(q, x, y);
	if (c->n < VC_MAX) { c->y[c->n] = y; c->v[c->n] = *q; c->n++; }
}

/* top[] inclusive, bot[] exclusive, both already clipped */
static void plane_spans(long x1, long x2, const short *top, const short *bot, int trans)
{
	long x, xa, xb, xr;
	TexRef ref;
	u16 clut;
	int have;

	if (x1 < 0) x1 = 0;
	if (x2 >= xdimen) x2 = xdimen - 1;
	if (x1 > x2) return;
	setgotpic(globalpicnum);
	have = tex_lookup(globalpicnum, 0, &ref);
	clut = clut_get(globalpal, !trans && pm_mode != PM_FLOORSPRITE);

	PROF_COUNT(pc_pcols, x2 - x1 + 1);
	vc_prev->x = -1; vc_cur->x = -1;
	x = x1;
	while (x <= x2) {
		int topL, botL, topR, botR, yT, yB, i, c0, c1;
		if (top[x] >= bot[x]) { x++; continue; }
		if (prim_3d_full()) { rend_dropped_3d++; return; }
		xa = x; xb = x;
		while (xb + 1 <= x2 && (xb + 1 - xa) < MAXSTRIP) {
			long xn = xb + 1;
			if (top[xn] >= bot[xn]) break;
			if (xn - xa >= 2) {
				if (klabs(top[xn] - 2 * top[xn - 1] + top[xn - 2]) > 1) break;
				if (klabs(bot[xn] - 2 * bot[xn - 1] + bot[xn - 2]) > 1) break;
			}
			xb = xn;
		}
		xb = strip_fit(top, bot, x2, xa, xb);
		xr = xb + 1; PROF_COUNT(pc_pstrips, 1);
		topL = top[xa]; botL = bot[xa];
		if (xb + 1 <= x2 && top[xb + 1] < bot[xb + 1]) { topR = top[xb + 1]; botR = bot[xb + 1]; }
		else if (xb > xa) { topR = 2 * top[xb] - top[xb - 1]; botR = 2 * bot[xb] - bot[xb - 1]; }
		else { topR = topL; botR = botL; }
		if (topR < 0) topR = 0;
		if (botR > ydimen) botR = ydimen;
		if (botR < topR) botR = topR;

		yT = max(topL, topR); yB = min(botL, botR);
		{
			QV q[4];
			int first = 1;
			vc_cur->n = 0; vc_cur->x = xr;
			plane_vertex_left(&q[0], xa, topL);
			plane_vertex_right(&q[1], xr, topR);
			if (yT < yB) {
				/* band cuts strictly inside (yT, yB), ascending; the slanted top/bottom edges
				   are kept intact by only cutting the interior band */
				for (i = NBANDS - 1; i >= -NBANDS; i--) {
					int cut = (i >= 0) ? globalhoriz - bandcuts[i] : globalhoriz + bandcuts[-i - 1];
					if (cut <= yT || cut >= yB) continue;
					plane_vertex_left(&q[2], xa, cut);
					plane_vertex_right(&q[3], xr, cut);
					plane_emit(&ref, have, clut, q, trans);
					q[0] = q[2]; q[1] = q[3];
					first = 0;
				}
			}
			(void)first;
			plane_vertex_left(&q[2], xa, botL);
			plane_vertex_right(&q[3], xr, botR);
			plane_emit(&ref, have, clut, q, trans);
		}
		{ VertCache *t = vc_prev; vc_prev = vc_cur; vc_cur = t; }
		x = xb + 1;
	}
}

/* ---- ceilscan / florscan (setup copied from the original) -------------- */

static void planesetup_flat(sectortype *sec, int isfloor)
{
	long i, j, ox, oy;
	if ((globalorientation&64) == 0)
	{
		globalx1 = singlobalang; globalx2 = singlobalang;
		globaly1 = cosglobalang; globaly2 = cosglobalang;
		globalxpanning = (globalposx<<20);
		globalypanning = -(globalposy<<20);
	}
	else
	{
		j = sec->wallptr;
		ox = wall[wall[j].point2].x - wall[j].x;
		oy = wall[wall[j].point2].y - wall[j].y;
		i = nsqrtasm(ox*ox+oy*oy); if (i == 0) i = 1024; else i = 1048576/i;
		globalx1 = mulscale10(dmulscale10(ox,singlobalang,-oy,cosglobalang),i);
		globaly1 = mulscale10(dmulscale10(ox,cosglobalang,oy,singlobalang),i);
		globalx2 = -globalx1;
		globaly2 = -globaly1;

		ox = ((wall[j].x-globalposx)<<6); oy = ((wall[j].y-globalposy)<<6);
		i = dmulscale14(oy,cosglobalang,-ox,singlobalang);
		j = dmulscale14(ox,cosglobalang,oy,singlobalang);
		ox = i; oy = j;
		globalxpanning = globalx1*ox - globaly1*oy;
		globalypanning = globaly2*ox + globalx2*oy;
	}
	globalx2 = mulscale16(globalx2,viewingrangerecip);
	globaly1 = mulscale16(globaly1,viewingrangerecip);
	globalxshift = (8-(picsiz[globalpicnum]&15));
	globalyshift = (8-(picsiz[globalpicnum]>>4));
	if (globalorientation&8) { globalxshift++; globalyshift++; }

	if ((globalorientation&0x4) > 0)
	{
		i = globalxpanning; globalxpanning = globalypanning; globalypanning = i;
		i = globalx2; globalx2 = -globaly1; globaly1 = -i;
		i = globalx1; globalx1 = globaly2; globaly2 = i;
	}
	if ((globalorientation&0x10) > 0) globalx1 = -globalx1, globaly1 = -globaly1, globalxpanning = -globalxpanning;
	if ((globalorientation&0x20) > 0) globalx2 = -globalx2, globaly2 = -globaly2, globalypanning = -globalypanning;
	globalx1 <<= globalxshift; globaly1 <<= globalxshift;
	globalx2 <<= globalyshift;  globaly2 <<= globalyshift;
	globalxpanning <<= globalxshift; globalypanning <<= globalyshift;
	if (isfloor) {
		globalxpanning += (((long)sec->floorxpanning)<<24);
		globalypanning += (((long)sec->floorypanning)<<24);
	} else {
		globalxpanning += (((long)sec->ceilingxpanning)<<24);
		globalypanning += (((long)sec->ceilingypanning)<<24);
	}
	globaly1 = (-globalx1-globaly1)*halfxdimen;
	globalx2 = (globalx2-globaly2)*halfxdimen;

	globalx1 = mulscale16(globalx1,globalzd);
	globalx2 = mulscale16(globalx2,globalzd);
	globaly1 = mulscale16(globaly1,globalzd);
	globaly2 = mulscale16(globaly2,globalzd);
	globvis = klabs(mulscale10(globvis,globalzd));
	pm_mode = PM_FLAT;
	pm_xbits = picsiz[globalpicnum]&15;
	pm_ybits = picsiz[globalpicnum]>>4;
}

void ceilscan(long x1, long x2, long sectnum)
{
	long x;
	sectortype *sec;
	static short top[MAXXDIM], bot[MAXXDIM];

	sec = &sector[sectnum];
	globalzd = sec->ceilingz-globalposz;
	if (globalzd > 0) return;
	globalpicnum = sec->ceilingpicnum;
	if ((unsigned)globalpicnum >= (unsigned)MAXTILES) globalpicnum = 0;
	setgotpic(globalpicnum);
	if ((tilesizx[globalpicnum] <= 0) || (tilesizy[globalpicnum] <= 0)) return;
	if (picanm[globalpicnum]&192) globalpicnum += animateoffs((short)globalpicnum,(short)sectnum);

	globalshade = (long)sec->ceilingshade;
	globvis = globalcisibility;
	if (sec->visibility != 0) globvis = mulscale4(globvis,(long)((unsigned char)(sec->visibility+16)));
	globalorientation = (long)sec->ceilingstat;
	globalpal = sec->ceilingpal;
	planesetup_flat(sec, 0);

	for (x = x1; x <= x2; x++) {
		int t = umost[x], b = min(uplc[x], dmost[x]);
		if (t < 0) t = 0;
		if (b > ydimen) b = ydimen;
		if (t < b) {   /* PS1: ceiling's bottom edge runs down into the wall rows drawn after it (see EDGE_EXT); stay above the horizon */
			int e = uplc[x] + EDGE_EXT, h = max(b, globalhoriz);
			if (e > dmost[x]) e = dmost[x];
			if (e > h) e = h;
			if (e > ydimen) e = ydimen;
			if (e > b) b = e;
		}
		top[x] = t; bot[x] = b;
	}
	{ PROF_BEGIN(t); plane_spans(x1, x2, top, bot, (globalorientation & 0x180) >= 256); PROF_END(prof_plane, t); PROF_COUNT(prof_nplane, 1); }
}

void florscan(long x1, long x2, long sectnum)
{
	long x;
	sectortype *sec;
	static short top[MAXXDIM], bot[MAXXDIM];

	sec = &sector[sectnum];
	globalzd = globalposz-sec->floorz;
	if (globalzd > 0) return;
	globalpicnum = sec->floorpicnum;
	if ((unsigned)globalpicnum >= (unsigned)MAXTILES) globalpicnum = 0;
	setgotpic(globalpicnum);
	if ((tilesizx[globalpicnum] <= 0) || (tilesizy[globalpicnum] <= 0)) return;
	if (picanm[globalpicnum]&192) globalpicnum += animateoffs((short)globalpicnum,(short)sectnum);

	globalshade = (long)sec->floorshade;
	globvis = globalcisibility;
	if (sec->visibility != 0) globvis = mulscale4(globvis,(long)((unsigned char)(sec->visibility+16)));
	globalorientation = (long)sec->floorstat;
	globalpal = sec->floorpal;
	planesetup_flat(sec, 1);

	for (x = x1; x <= x2; x++) {
		int t = max(dplc[x], umost[x]), b = dmost[x];
		if (t < 0) t = 0;
		if (b > ydimen) b = ydimen;
		if (t < b) {   /* PS1: floor's top edge runs up into the wall rows drawn after it (see EDGE_EXT); stay below the horizon */
			int e = dplc[x] - EDGE_EXT, h = min(t, globalhoriz + 1);
			if (e < umost[x]) e = umost[x];
			if (e < h) e = h;
			if (e < 0) e = 0;
			if (e < t) t = e;
		}
		top[x] = t; bot[x] = b;
	}
	{ PROF_BEGIN(t); plane_spans(x1, x2, top, bot, (globalorientation & 0x180) >= 256); PROF_END(prof_plane, t); PROF_COUNT(prof_nplane, 1); }
}

/* floor sprites: drawsprite already computed uwall/dwall and the mapping globals */
static void ceilspritescan(long x1, long x2)
{
	long x;
	static short top[MAXXDIM], bot[MAXXDIM];
	for (x = x1; x <= x2; x++) {
		int t = uwall[x], b = dwall[x];
		if (t < 0) t = 0;
		if (b > ydimen) b = ydimen;
		top[x] = t; bot[x] = b;
	}
	pm_mode = PM_FLOORSPRITE;
	pm_xbits = picsiz[globalpicnum]&15;
	pm_ybits = picsiz[globalpicnum]>>4;
	plane_spans(x1, x2, top, bot, (globalorientation & 2) != 0);
}

/* sloped floors/ceilings */
void grouscan(long dax1, long dax2, long sectnum, char dastat)
{
	long i, j, l, x, y, dx, dy, wx, wy, daz;
	long daslope, dasqr;
	walltype *wal;
	sectortype *sec;
	static short top[MAXXDIM], bot[MAXXDIM];

	sec = &sector[sectnum];

	if (dastat == 0)
	{
		if (globalposz <= getceilzofslope(sectnum,globalposx,globalposy))
			return;  //Back-face culling
		globalorientation = sec->ceilingstat;
		globalpicnum = sec->ceilingpicnum;
		globalshade = sec->ceilingshade;
		globalpal = sec->ceilingpal;
		daslope = sec->ceilingheinum;
		daz = sec->ceilingz;
	}
	else
	{
		if (globalposz >= getflorzofslope(sectnum,globalposx,globalposy))
			return;  //Back-face culling
		globalorientation = sec->floorstat;
		globalpicnum = sec->floorpicnum;
		globalshade = sec->floorshade;
		globalpal = sec->floorpal;
		daslope = sec->floorheinum;
		daz = sec->floorz;
	}

	if ((picanm[globalpicnum]&192) != 0) globalpicnum += animateoffs(globalpicnum,sectnum);
	setgotpic(globalpicnum);
	if ((tilesizx[globalpicnum] <= 0) || (tilesizy[globalpicnum] <= 0)) return;

	wal = &wall[sec->wallptr];
	wx = wall[wal->point2].x - wal->x;
	wy = wall[wal->point2].y - wal->y;
	dasqr = krecipasm(nsqrtasm(wx*wx+wy*wy));
	i = mulscale21(daslope,dasqr);
	wx *= i; wy *= i;

	globalx = -mulscale19(singlobalang,xdimenrecip);
	globaly = mulscale19(cosglobalang,xdimenrecip);
	globalx1 = (globalposx<<8);
	globaly1 = -(globalposy<<8);
	i = (dax1-halfxdimen)*xdimenrecip;
	globalx2 = mulscale16(cosglobalang<<4,viewingrangerecip) - mulscale27(singlobalang,i);
	globaly2 = mulscale16(singlobalang<<4,viewingrangerecip) + mulscale27(cosglobalang,i);
	globalzd = (xdimscale<<9);
	globalzx = -dmulscale17(wx,globaly2,-wy,globalx2) + mulscale10(1-globalhoriz,globalzd);
	globalz = -dmulscale25(wx,globaly,-wy,globalx);

	if (globalorientation&64)  //Relative alignment
	{
		dx = mulscale14(wall[wal->point2].x-wal->x,dasqr);
		dy = mulscale14(wall[wal->point2].y-wal->y,dasqr);

		i = nsqrtasm(daslope*daslope+16777216);

		x = globalx; y = globaly;
		globalx = dmulscale16(x,dx,y,dy);
		globaly = mulscale12(dmulscale16(-y,dx,x,dy),i);

		x = ((wal->x-globalposx)<<8); y = ((wal->y-globalposy)<<8);
		globalx1 = dmulscale16(-x,dx,-y,dy);
		globaly1 = mulscale12(dmulscale16(-y,dx,x,dy),i);

		x = globalx2; y = globaly2;
		globalx2 = dmulscale16(x,dx,y,dy);
		globaly2 = mulscale12(dmulscale16(-y,dx,x,dy),i);
	}
	if (globalorientation&0x4)
	{
		i = globalx; globalx = -globaly; globaly = -i;
		i = globalx1; globalx1 = globaly1; globaly1 = i;
		i = globalx2; globalx2 = -globaly2; globaly2 = -i;
	}
	if (globalorientation&0x10) { globalx1 = -globalx1, globalx2 = -globalx2, globalx = -globalx; }
	if (globalorientation&0x20) { globaly1 = -globaly1, globaly2 = -globaly2, globaly = -globaly; }

	daz = dmulscale9(wx,globalposy-wal->y,-wy,globalposx-wal->x) + ((daz-globalposz)<<8);
	globalx2 = mulscale20(globalx2,daz); globalx = mulscale28(globalx,daz);
	globaly2 = mulscale20(globaly2,-daz); globaly = mulscale28(globaly,-daz);

	i = 8-(picsiz[globalpicnum]&15); j = 8-(picsiz[globalpicnum]>>4);
	if (globalorientation&8) { i++; j++; }
	globalx1 <<= (i+12); globalx2 <<= i; globalx <<= i;
	globaly1 <<= (j+12); globaly2 <<= j; globaly <<= j;

	if (dastat == 0)
	{
		globalx1 += (((long)sec->ceilingxpanning)<<24);
		globaly1 += (((long)sec->ceilingypanning)<<24);
	}
	else
	{
		globalx1 += (((long)sec->floorxpanning)<<24);
		globaly1 += (((long)sec->floorypanning)<<24);
	}

	globvis = globalvisibility;
	if (sec->visibility != 0) globvis = mulscale4(globvis,(long)((unsigned char)(sec->visibility+16)));
	globvis = mulscale13(globvis,daz);
	globvis = mulscale16(globvis,xdimscale);

	pm_mode = PM_SLOPE;
	pm_dax1 = dax1;
	pm_xbits = picsiz[globalpicnum]&15;
	pm_ybits = picsiz[globalpicnum]>>4;
	(void)l;

	for (x = dax1; x <= dax2; x++) {
		int t, b;
		if (dastat == 0) { t = umost[x]; b = min(dmost[x],uplc[x]); }
		else { t = max(umost[x],dplc[x]); b = dmost[x]; }
		if (t < 0) t = 0;
		if (b > ydimen) b = ydimen;
		if (t < b) {   /* PS1: run past the wall boundary into the free rows (see EDGE_EXT) */
			int e;
			if (dastat == 0) { e = uplc[x] + EDGE_EXT; if (e > dmost[x]) e = dmost[x]; if (e > ydimen) e = ydimen; if (e > b) b = e; }
			else { e = dplc[x] - EDGE_EXT; if (e < umost[x]) e = umost[x]; if (e < 0) e = 0; if (e < t) t = e; }
		}
		top[x] = t; bot[x] = b;
	}
	plane_spans(dax1, dax2, top, bot, 0);
}

/* ---- 2D: rotatesprite --------------------------------------------------- */

static void set_cliprect(long cx1, long cy1, long cx2, long cy2)
{
	DR_AREA *p = prim_alloc(sizeof(DR_AREA));
	if (!p) return;
	if (cx1 < 0) cx1 = 0;
	if (cy1 < 0) cy1 = 0;
	if (cx2 >= xdim) cx2 = xdim - 1;
	if (cy2 >= ydim) cy2 = ydim - 1;
	setDrawAreaXY(p, video_drawenv->clip.x + SCR_X(cx1), video_drawenv->clip.y + cy1,
				  video_drawenv->clip.x + SCR_X(cx2 + 1) - 1, video_drawenv->clip.y + cy2);
	prim_link(p);
}

static void dorotatesprite(long sx, long sy, long z, short a, short picnum, signed char dashade, char dapalnum, char dastat, long cx1, long cy1, long cx2, long cy2)
{
	long cosang, sinang, i, x;
	long xsiz, ysiz, xoff, yoff, xv, yv, xv2, yv2;
	long px[4], py[4];
	int bright, trans, clipped;
	u16 clut;

	rend_2d();
	xsiz = tilesizx[picnum]; ysiz = tilesizy[picnum];
	if (dastat&16) { xoff = 0; yoff = 0; }
	else
	{
		xoff = (long)((signed char)((picanm[picnum]>>8)&255))+(xsiz>>1);
		yoff = (long)((signed char)((picanm[picnum]>>16)&255))+(ysiz>>1);
	}

	if (dastat&4) yoff = ysiz-yoff;

	cosang = sintable[(a+512)&2047]; sinang = sintable[a&2047];

	if ((dastat&2) != 0)  //Auto window size scaling
	{
		if ((dastat&8) == 0)
		{
			x = xdimenscale;   //= scale(xdimen,yxaspect,320);
			sx = ((cx1+cx2+2)<<15)+scale(sx-(320<<15),xdimen<<REND_XSHIFT,320);
			sy = ((cy1+cy2+2)<<15)+mulscale16(sy-(200<<15),x);
		}
		else
		{
			  //If not clipping to startmosts, & auto-scaling on, as a
			  //hard-coded bonus, scale to full screen instead
			x = scale(xdim,yxaspect>>REND_XSHIFT,320);
			sx = (xdim<<15)+32768+scale(sx-(320<<15),xdim,320);
			sy = (ydim<<15)+32768+mulscale16(sy-(200<<15),x);
		}
		z = mulscale16(z,x);
	}

	xv = mulscale14(cosang,z);
	yv = mulscale14(sinang,z);
	if (((dastat&2) != 0) || ((dastat&8) == 0)) //Don't aspect unscaled perms
	{
		xv2 = mulscale16(xv,(xyaspect<<REND_XSHIFT));
		yv2 = mulscale16(yv,(xyaspect<<REND_XSHIFT));
	}
	else
	{
		xv2 = xv;
		yv2 = yv;
	}

	py[0] = sy - (yv*xoff + xv*yoff);
	py[1] = py[0] + yv*xsiz;
	py[3] = py[0] + xv*ysiz;
	py[2] = py[1]+py[3]-py[0];
	i = (cy1<<16); if ((py[0]<i) && (py[1]<i) && (py[2]<i) && (py[3]<i)) return;
	i = (cy2<<16); if ((py[0]>i) && (py[1]>i) && (py[2]>i) && (py[3]>i)) return;

	px[0] = sx - (xv2*xoff - yv2*yoff);
	px[1] = px[0] + xv2*xsiz;
	px[3] = px[0] - yv2*ysiz;
	px[2] = px[1]+px[3]-px[0];
	i = (cx1<<16); if ((px[0]<i) && (px[1]<i) && (px[2]<i) && (px[3]<i)) return;
	i = (cx2<<16); if ((px[0]>i) && (px[1]>i) && (px[2]>i) && (px[3]>i)) return;

	clipped = (cx1 > 0 || cy1 > 0 || cx2 < xdim - 1 || cy2 < ydim - 1);
	if (clipped) set_cliprect(cx1, cy1, cx2, cy2);

	bright = shadebright[getpalookup(0, dashade)];
	trans = (dastat & 1) != 0;
	clut = clut_get(dapalnum, 0);
	rend_sync_loads = 1;
	{
		TexRef ref;
		QV q[4];
		int k;
		if (!tex_lookup_sync(picnum, 0, &ref)) { if (clipped) set_cliprect(0, 0, xdim - 1, ydim - 1); return; }
		int ufar = xsiz, vfar = ysiz;
		/* PS1: far corners sit on the exclusive edge xsiz/ysiz so a 1:1 glyph samples every texel (the GPU
		   reads texel floor(u) at a pixel's left/top edge); -1 only if the 8-bit window cannot hold it */
		if (ref.npieces == 1) {
			int hw = xsiz >> ref.half, hh = ysiz >> ref.half;
			if (ref.w == ref.bw ? hw > 255 : ref.u + hw > 255) ufar--;
			if (ref.h == ref.bh ? hh > 255 : ref.v + hh > 255) vfar--;
		}
		/* corners: 0 TL, 1 TR, 3 BL, 2 BR of the whole tile */
		for (k = 0; k < 4; k++) {
			int c = (k == 0) ? 0 : (k == 1) ? 1 : (k == 2) ? 3 : 2;
			q[k].x = SCR_X(px[c] >> 16); q[k].y = py[c] >> 16;
			q[k].u = (k == 0 || k == 2) ? 0 : ufar;
			q[k].v = (k < 2) ? 0 : vfar;
			if (dastat & 4) q[k].v = (k < 2) ? ysiz - 1 : 0;   /* y-flipped: top row first (texel floor(v) samples) */
			q[k].c = bright;
		}
		emit_quad(&ref, clut, q, trans, 0, 0);
	}
	rend_sync_loads = 0;
	if (clipped) set_cliprect(0, 0, xdim - 1, ydim - 1);
}

/* ---- misc 2D ----------------------------------------------------------- */

/* PS1: flat palette-colour rectangle (inclusive corners); also draws the loading screen progress bar */
void rend_fillrect(long x1, long y1, long x2, long y2, long dacol)
{
	TILE *t;
	long sx1 = SCR_X(x1), sx2 = SCR_X(x2 + 1);
	if (sx2 <= sx1) return;
	rend_2d();
	t = prim_alloc(sizeof(TILE));
	if (!t) return;
	setTile(t);
	setRGB0(t, palette[dacol*3] << 2, palette[dacol*3+1] << 2, palette[dacol*3+2] << 2);
	setXY0(t, sx1, y1);
	setWH(t, sx2 - sx1, y2 - y1 + 1);
	prim_link(t);
}

void clearview(long dacol)
{
	rend_fillrect(windowx1, windowy1, windowx2, windowy2, dacol);
}

void clearallviews(long dacol)
{
	rend_fillrect(0, 0, xdim - 1, ydim - 1, dacol);
}

/* Lines are clipped to the 3D window here: the GPU drops primitives whose vertices are more than 1023
   (x) / 511 (y) apart, and the overhead map has many lines that start far off-screen. */
void drawline256(long x1, long y1, long x2, long y2, char col)
{
	LINE_F2 *l;
	long long dx = x2 - x1, dy = y2 - y1, t0 = 0, t1 = 65536;
	long long lim[4], d[4];
	int i;

	lim[0] = x1 - ((long long)windowx1 << 12);         d[0] = -dx;   /* x >= left */
	lim[1] = ((long long)(windowx2 + 1) << 12) - 1 - x1; d[1] = dx;    /* x <= right */
	lim[2] = y1 - ((long long)windowy1 << 12);         d[2] = -dy;   /* y >= top */
	lim[3] = ((long long)(windowy2 + 1) << 12) - 1 - y1; d[3] = dy;    /* y <= bottom */
	for (i = 0; i < 4; i++) {
		if (d[i] == 0) { if (lim[i] < 0) return; continue; }
		{
			long long t = (lim[i] << 16) / d[i];
			if (d[i] < 0) { if (t > t0) t0 = t; }      /* entering the half-plane */
			else { if (t < t1) t1 = t; }               /* leaving it */
		}
	}
	if (t0 >= t1) return;
	{
		long nx1 = x1 + (long)((dx * t0) >> 16), ny1 = y1 + (long)((dy * t0) >> 16);
		long nx2 = x1 + (long)((dx * t1) >> 16), ny2 = y1 + (long)((dy * t1) >> 16);
		x1 = nx1; y1 = ny1; x2 = nx2; y2 = ny2;
	}
	rend_2d();
	l = prim_alloc(sizeof(LINE_F2));
	if (!l) return;
	setLineF2(l);
	setRGB0(l, palette[col*3] << 2, palette[col*3+1] << 2, palette[col*3+2] << 2);
	setXY2(l, SCR_X(x1 >> 12), y1 >> 12, SCR_X(x2 >> 12), y2 >> 12);
	prim_link(l);
}

void printext256(long xpos, long ypos, short col, short backcol, char *name, char fontsize)
{
	/* debug text: not rendered on PS1 (HUD text uses tile fonts) */
	(void)xpos; (void)ypos; (void)col; (void)backcol; (void)name; (void)fontsize;
}

void drawmapview(long dax, long day, long zoome, short daang)
{
	(void)dax; (void)day; (void)zoome; (void)daang;
}

void rotatesprite(long sx, long sy, long z, short a, short picnum, signed char dashade, char dapalnum, char dastat, long cx1, long cy1, long cx2, long cy2)
{
	if ((cx1 > cx2) || (cy1 > cy2)) return;
	if (z <= 16) return;
	if (picanm[picnum]&192) picnum += animateoffs(picnum,(short)0xc000);
	if ((tilesizx[picnum] <= 0) || (tilesizy[picnum] <= 0)) return;
	dorotatesprite(sx,sy,z,a,picnum,dashade,dapalnum,dastat,cx1,cy1,cx2,cy2);
}

void completemirror(void)
{
	/* mirrors are not supported on the GPU path (rend_mirrors_supported == 0): the mirror wall keeps its texture */
}
