/* Memory card save files over the BIOS file API ("bu00:").
 *
 * Why the BIOS file API and not raw sectors: it handles the directory/FAT-like frame layout, bad
 * sector remapping, and produces files the console's memory card manager understands (and lets the
 * player copy/delete them there).  PCSX-Redux implements it in its HLE BIOS and on hardware it is
 * the real BIOS.  Constraints that matter on hardware:
 *   - InitCARD(1) must come after InitPAD/StartPAD (pad and card share the SIO0 interrupt);
 *   - file size in blocks is given when the file is created: open(name, FCREAT|(nblocks<<16));
 *   - read()/write() lengths and file offsets must be multiples of 128 (one card sector);
 *   - a file is created empty, "replace" = erase() + create;
 *   - the first access after boot may report a "new card"; we retry once after _new_card();
 *   - the music Timer 2 IRQ must be masked while the card is accessed (see hold_irqs()).
 * Each slot has two physical names ("...DUKE<n>" and "...DUKE<n>B") so a save can be written next to
 * the old one and the old one erased only after the new one verified (see savegame.c).
 * Everything is staged through one 512 byte buffer (4 sectors per BIOS call). */

#pragma GCC optimize ("Os")
#include <psxapi.h>
#include <string.h>
#include <stdio.h>
#include "memcard.h"
#include "psx.h"

#define FREAD   0x0001
#define FWRITE  0x0002
#define FCREAT  0x0200

#define STAGE 512

static unsigned char stage[STAGE] __attribute__((aligned(4)));
static int fd = -1;
static int spos;        /* read: next byte in stage / write: bytes staged */
static int mc_ready;

static const char name_base[] = "BASLUS-00000DUKE";   /* + slot digit [+ 'B' for the alternate copy] */
#define NAME_LEN (sizeof(name_base) - 1)

static void slot_name(char *dst, int slot, int alt)
{
	strcpy(dst, "bu00:");
	strcat(dst, name_base);
	dst[5 + NAME_LEN] = '0' + slot;
	dst[5 + NAME_LEN + 1] = alt ? 'B' : 0;
	dst[5 + NAME_LEN + 2] = 0;
}

/* Let an asynchronous CD read (texture/music streamers) finish before touching the card: both
   use IRQs and the card transfer is timing sensitive.  Nothing starts a new read while we are
   inside mc_* (the game is single threaded and we never call back). */
extern void fx_quiesce(void);   /* spu_fx.c: sound-effect sample read in flight / queued play requests */
static void quiesce(void)
{
	fx_quiesce();
	int t = video_vsync_count();
	while (pak_busy() && video_vsync_count() - t < 180)
		;
}

#define IRQ_T2 (1 << 6)
static unsigned short held_mask;
static int held;
/* Timer 2 (music sequencer) stays masked from *_open to *_close, so the music voices are keyed off
   first (music_hold): a card operation takes seconds and held notes would drone through it. */
static void hold_irqs(void)
{
	if (held) return;
	music_hold();
	held_mask = IRQ_MASK & IRQ_T2;
	IRQ_MASK &= ~IRQ_T2;
	held = 1;
}
static void release_irqs(void)
{
	if (!held) return;
	IRQ_MASK |= held_mask;
	held = 0;
	music_resume();
}

void mc_init(void)
{
	if (mc_ready) return;
	InitCARD(1);
	StartCARD();
	_bu_init();
	ChangeClearPAD(0);
	mc_ready = 1;
}

/* Card presence/format probe with the low level BIOS calls: _card_info() reports through HwCARD
   events (class 0xf0000011): 0x0004 ok, 0x2000 new card, 0x0100 timeout (nothing answered),
   0x8000 error.  Then frame 0 is read raw to check for the "MC" signature of a formatted card. */
#define EV_CLASS_HW 0xf0000011u
static const unsigned short ev_spec[4] = { 0x0004, 0x2000, 0x0100, 0x8000 };
#define EV_OK 0
#define EV_NEW 1
#define EV_TIMEOUT 2
#define EV_ERROR 3

static int card_wait(const int *ev)
{
	int t = video_vsync_count(), i;
	while (video_vsync_count() - t < 90)
		for (i = 0; i < 4; i++)
			if (TestEvent(ev[i])) return i;
	return EV_TIMEOUT;
}

int mc_status(void)
{
	int ev[4], i, r, st, first_r;
	int t_start = video_vsync_count();

	mc_init();
	quiesce();
	hold_irqs();
	for (i = 0; i < 4; i++) {
		ev[i] = OpenEvent(EV_CLASS_HW, ev_spec[i], 0x2000, 0);
		EnableEvent(ev[i]);
	}
	for (i = 0; i < 4; i++) TestEvent(ev[i]);   /* drop stale events */
	_card_info(0);
	r = card_wait(ev);
	first_r = r;
	if (r == EV_TIMEOUT) st = MC_NOCARD;
	else if (r == EV_ERROR) st = MC_ERROR;
	else {
		st = MC_OK;
		/* frame 0 must start with "MC"; a "new card" answer is repeated once, after _new_card(): the
		   card-change flag makes reads fail until the BIOS has been told about the new card (psx-spx) */
		for (i = 0; i < 2; i++) {
			stage[0] = stage[1] = 0;
			if (i) _new_card();
			_card_read(0, 0, stage);
			r = card_wait(ev);
			if (r == EV_NEW) continue;
			if (r != EV_OK) st = MC_ERROR;
			else if (stage[0] != 'M' || stage[1] != 'C') st = MC_UNFORMATTED;
			break;
		}
	}
	for (i = 0; i < 4; i++) { DisableEvent(ev[i]); CloseEvent(ev[i]); }
	release_irqs();
#ifdef PS1_SAVETEST
	printf("mc_status: info event %d, status %d, %d vsyncs\n", first_r, st, video_vsync_count() - t_start);
#endif
	return st;
}

static unsigned scan_once(void)
{
	struct DIRENTRY de;
	struct DIRENTRY *e;
	unsigned mask = 0;
	static const char pat[] = "bu00:BASLUS-00000DUKE*";

	for (e = firstfile(pat, &de); e; e = nextfile(&de)) {
		if (strncmp(e->name, name_base, NAME_LEN) == 0) {
			char c = e->name[NAME_LEN];
			if (c >= '0' && c < '0' + MC_SLOTS) {
				if (e->name[NAME_LEN + 1] == 0) mask |= 1u << (c - '0');
				else if (e->name[NAME_LEN + 1] == 'B' && e->name[NAME_LEN + 2] == 0)
					mask |= 1u << (16 + c - '0');
			}
		}
	}
	return mask;
}

unsigned mc_scan(void)
{
	unsigned m;
	mc_init();
	quiesce();
	hold_irqs();
	m = scan_once();
	if (!m) {            /* maybe just the "new card" state after boot: re-check once */
		_new_card();
		m = scan_once();
	}
	release_irqs();
	return m;
}

int mc_free_blocks(void)
{
	struct DIRENTRY de;
	struct DIRENTRY *e;
	int used = 0, n = 0;

	mc_init();
	quiesce();
	hold_irqs();
	for (e = firstfile("bu00:*", &de); e; e = nextfile(&de)) {
		used += (e->size + MC_BLOCK - 1) / MC_BLOCK;
		n++;
	}
	release_irqs();
	return MC_MAX_BLOCKS - used;
}

/* blocks used by an existing save file (0 if absent) */
int mc_file_blocks(int slot, int alt)
{
	struct DIRENTRY de;
	char name[32];
	int n = 0;
	mc_init();
	quiesce();
	hold_irqs();
	slot_name(name, slot, alt);
	if (firstfile(name, &de)) n = (de.size + MC_BLOCK - 1) / MC_BLOCK;
	release_irqs();
	return n;
}

int mc_erase(int slot, int alt)
{
	char name[32];
	int rc;
	mc_init();
	quiesce();
	hold_irqs();
	slot_name(name, slot, alt);
	rc = erase(name);
	release_irqs();
	return rc ? 0 : -1;
}

/* ---- reading -------------------------------------------------------------------------------- */

static int refill(void)
{
	if (read(fd, stage, STAGE) != STAGE) return -1;
	spos = 0;
	return 0;
}

int mc_read_open(int slot, int alt)
{
	char name[32];
	mc_init();
	quiesce();
	hold_irqs();
	slot_name(name, slot, alt);
	fd = open(name, FREAD);
	if (fd < 0) { _new_card(); fd = open(name, FREAD); }
	if (fd < 0) { release_irqs(); return -1; }
	if (refill() < 0) { close(fd); fd = -1; release_irqs(); return -1; }
	spos = MC_HDR_BYTES;     /* skip header + icon frames */
	return 0;
}

int mc_read(void *dst, int len)
{
	unsigned char *d = dst;
	int n = len;
	if (fd < 0) return -1;
	while (n > 0) {
		int k;
		if (spos == STAGE && refill() < 0) return -1;
		k = STAGE - spos;
		if (k > n) k = n;
		memcpy(d, stage + spos, k);
		spos += k; d += k; n -= k;
	}
	return len;
}

void mc_read_close(void)
{
	if (fd >= 0) close(fd);
	fd = -1;
	release_irqs();
}

/* ---- writing -------------------------------------------------------------------------------- */

/* 16x16 icon, 4 bits per pixel: '.' transparent, k black, r red, y yellow */
static const char icon_art[16][17] = {
	"kkkkkkkkkkkkkkkk",
	"krrrrrrrrrrrrrrk",
	"krrrrrrrrrrrrrrk",
	"krryyyyyyyrrrrrk",
	"krryyyyyyyyrrrrk",
	"krryyrrrrryyrrrk",
	"krryyrrrrrryyrrk",
	"krryyrrrrrryyrrk",
	"krryyrrrrrryyrrk",
	"krryyrrrrrryyrrk",
	"krryyrrrrryyrrrk",
	"krryyyyyyyyrrrrk",
	"krryyyyyyyrrrrrk",
	"krrrrrrrrrrrrrrk",
	"krrrrrrrrrrrrrrk",
	"kkkkkkkkkkkkkkkk",
};

static void build_header(int nblocks, const char *title)
{
	static const unsigned short pal[4] = { 0x0000, 0x8000, 0x001f, 0x03ff };  /* BGR555 */
	unsigned char *h = stage;
	int i, x, y;

	memset(stage, 0, STAGE);
	h[0] = 'S'; h[1] = 'C';
	h[2] = 0x11;                 /* one icon frame, static */
	h[3] = (unsigned char)nblocks;
	for (i = 0; i < 32 && title[i]; i++) {   /* Shift-JIS full-width ASCII */
		unsigned c = (unsigned char)title[i], code;
		if (c >= 'a' && c <= 'z') c -= 32;
		if (c >= 'A' && c <= 'Z') code = 0x8260 + (c - 'A');
		else if (c >= '0' && c <= '9') code = 0x824f + (c - '0');
		else code = 0x8140;      /* full-width space */
		h[4 + i * 2] = code >> 8;
		h[5 + i * 2] = code & 255;
	}
	for (i = 0; i < 4; i++) {    /* CLUT: 16 entries at 0x60, only the first 4 used */
		h[0x60 + i * 2] = pal[i] & 255;
		h[0x61 + i * 2] = pal[i] >> 8;
	}
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++) {
			char c = icon_art[y][x];
			int v = c == 'k' ? 1 : c == 'r' ? 2 : c == 'y' ? 3 : 0;
			h[0x80 + y * 8 + x / 2] |= (x & 1) ? v << 4 : v;
		}
	spos = MC_HDR_BYTES;
}

int mc_write_open(int slot, int alt, int nblocks, const char *title)
{
	char name[32];
	mc_init();
	quiesce();
	if (nblocks < 1 || nblocks > MC_MAX_BLOCKS) return -1;
	hold_irqs();
	slot_name(name, slot, alt);
	{
		struct DIRENTRY de;
		if (firstfile(name, &de)) erase(name);   /* replace an existing save */
	}
	fd = open(name, FCREAT | FWRITE | (nblocks << 16));
	if (fd < 0) { _new_card(); fd = open(name, FCREAT | FWRITE | (nblocks << 16)); }
	if (fd < 0) { release_irqs(); return -1; }
	build_header(nblocks, title);
	return 0;
}

static int flush_stage(void)
{
	if (write(fd, stage, STAGE) != STAGE) return -1;
	spos = 0;
	return 0;
}

int mc_write(const void *src, int len)
{
	const unsigned char *s = src;
	if (fd < 0) return -1;
	while (len > 0) {
		int k = STAGE - spos;
		if (k > len) k = len;
		memcpy(stage + spos, s, k);
		spos += k; s += k; len -= k;
		if (spos == STAGE && flush_stage() < 0) return -1;
	}
	return 0;
}

int mc_write_close(void)
{
	int rc = 0;
	if (fd < 0) return -1;
	if (spos > 0) {              /* zero-pad to a whole number of 128 byte sectors and flush those */
		int n = (spos + 127) & ~127;
		memset(stage + spos, 0, n - spos);
		if (write(fd, stage, n) != n) rc = -1;
	}
	close(fd);
	fd = -1;
	release_irqs();
	return rc;
}
