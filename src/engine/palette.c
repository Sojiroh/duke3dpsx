#include "palette.h"
#include "build.h"
#include "data_index.h"

#define NUMCLUT 32
#define CLUT_X 0
#define CLUT_Y 480
#define MAXREMAP 32

static u8 pals[(DAT_PALS_SIZE + 2047) & ~2047] __attribute__((aligned(4)));
static const u8 *basepals;         /* 768 bytes: normal palette */
static const u8 *extrapals;        /* 5*768 */
unsigned char shadebright[32];
static u8 *remaps[MAXREMAP];
static u8 identity[256];
static u8 haveremap[MAXREMAP];
static u8 curpal[768];
static int curbright;

typedef struct { s8 pal; u8 opaque; u8 valid; u32 lastuse; } ClutSlot;
static ClutSlot slots[NUMCLUT];
static u32 clutframe;
static int tint_r, tint_g, tint_b, tint_i;

void pal_init(void)
{
	const PakEntry *e = pak_find("PALS");
	int numl, i;
	const u8 *p;
	if (!e) fatal("PALS missing");
	pak_read(e->sector, (e->size + 2047) / 2048, pals);
	numl = pals[0];
	basepals = pals + 1;
	p = pals + 1 + 768;
	for (i = 0; i < 256; i++) identity[i] = i;
	for (i = 0; i < MAXREMAP; i++) { remaps[i] = identity; haveremap[i] = 0; }
	for (i = 0; i < numl; i++) {
		int palnum = p[0];
		if (palnum < MAXREMAP) { remaps[palnum] = (u8 *)p + 1; haveremap[palnum] = 1; }
		p += 257;
	}
	extrapals = p;
	p += 5 * 768;
	memcpy(shadebright, p, 32);
	memcpy(curpal, basepals, 768);
	curbright = 0;
	for (i = 0; i < NUMCLUT; i++) slots[i].valid = 0;
}

const unsigned char *pal_basepal(int idx)
{
	if (idx == 0) return basepals;
	return extrapals + (idx - 1) * 768;
}

static void build_clut(int slot, int pal, int opaque);

/* The palette changed: CLUT slots that are in use keep their (pal, opaque) key but get the new colours
   at once, like the VGA palette of the original.  Lists that are drawn later (the frame already built,
   or a re-presented frame during a palette fade) then show the new palette instead of stale colours. */
static void rebuild_cluts(void)
{
	for (int i = 0; i < NUMCLUT; i++)
		if (slots[i].valid) build_clut(i, slots[i].pal, slots[i].opaque);
}

void pal_set_brightness(int bright, const char *dapal)
{
	if (bright == curbright && !memcmp(curpal, dapal, 768)) return;
	curbright = bright;
	memcpy(curpal, dapal, 768);
	rebuild_cluts();
}

void pal_set_remap(int palnum, const unsigned char *remap)
{
	if ((unsigned)palnum >= MAXREMAP) return;
	if (remaps[palnum] == identity) return;   /* lookups come from PALS already; identity pals stay identity */
	if (!memcmp(remaps[palnum], remap, 256)) return;
	memcpy(remaps[palnum], remap, 256);
	haveremap[palnum] = 1;
	for (int i = 0; i < NUMCLUT; i++)
		if (slots[i].pal == palnum) slots[i].valid = 0;
}

static void build_clut(int slot, int pal, int opaque)
{
	static u16 buf[256] __attribute__((aligned(4)));
	const u8 *remap = remaps[pal];
	const u8 *gam = (const u8 *)britable[curbright];
	RECT r = {CLUT_X, CLUT_Y + slot, 256, 1};
	for (int i = 0; i < 256; i++) {
		int c = remap[i];
		int rr = gam[curpal[c*3] & 63], gg = gam[curpal[c*3+1] & 63], bb = gam[curpal[c*3+2] & 63];
		buf[i] = 0x8000 | ((bb >> 1) << 10) | ((gg >> 1) << 5) | (rr >> 1);
	}
	if (!opaque) buf[255] = 0;
	LoadImage(&r, (u32 *)buf);
	DrawSync(0);   /* buf is reused by the next build */
	slots[slot].pal = pal; slots[slot].opaque = opaque; slots[slot].valid = 1;
}

u16 clut_get(int pal, int opaque)
{
	int i, best = -1;
	u32 oldest = 0xffffffff;
	if ((unsigned)pal >= MAXREMAP) pal = 0;
	for (i = 0; i < NUMCLUT; i++) {
		ClutSlot *s = &slots[i];
		if (s->valid && s->pal == pal && s->opaque == opaque) {
			s->lastuse = clutframe;
			return getClut(CLUT_X, CLUT_Y + i);
		}
	}
	for (i = 0; i < NUMCLUT; i++) {
		if (!slots[i].valid) { best = i; break; }
		if (slots[i].lastuse < oldest && slots[i].lastuse != clutframe) { oldest = slots[i].lastuse; best = i; }
	}
	if (best < 0) best = 0;
	build_clut(best, pal, opaque);
	slots[best].lastuse = clutframe;
	return getClut(CLUT_X, CLUT_Y + best);
}

void clut_frame(void) { clutframe++; }

int pal_shade_bright(int shade)
{
	if (shade < 0) shade = 0;
	if (shade > 31) shade = 31;
	return shadebright[shade];
}

void pal_set_tint(int r, int g, int b, int intensity)
{
	tint_r = r; tint_g = g; tint_b = b; tint_i = intensity;
}

int pal_get_tint(int *r, int *g, int *b)
{
	*r = tint_r; *g = tint_g; *b = tint_b;
	return tint_i;
}
