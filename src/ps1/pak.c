#include <psxcd.h>
#include "psx.h"

#define MAXENTRIES 400

static PakEntry dir[MAXENTRIES];
static int ndir;
static u32 base_lba;
static volatile int busy;
static void (*done_cb)(void);
static u32 bounce[2048 / 4];

int pak_init(void)
{
	CdlFILE f;
	CdInit();
	if (!CdSearchFile(&f, "\\DUKE.DAT;1"))
		fatal("DUKE.DAT not found on disc");
	base_lba = CdPosToInt(&f.pos);
	if (pak_read(0, 1, bounce) < 0)
		fatal("cannot read DUKE.DAT header");
	if (memcmp(bounce, "DK3DPAK1", 8))
		fatal("bad DUKE.DAT header");
	ndir = bounce[2];
	if (ndir > MAXENTRIES) fatal("too many pak entries (%d)", ndir);
	int bytes = 12 + ndir * sizeof(PakEntry);
	int sectors = (bytes + 2047) / 2048;
	/* directory starts at byte 12 of sector 0; read all sectors into dir via bounce */
	u32 copied = 0;
	for (int s = 0; s < sectors; s++) {
		if (s) pak_read(s, 1, bounce);
		const u8 *src = (const u8 *)bounce + (s == 0 ? 12 : 0);
		int n = 2048 - (s == 0 ? 12 : 0);
		if (copied + n > (u32)ndir * sizeof(PakEntry)) n = ndir * sizeof(PakEntry) - copied;
		memcpy((u8 *)dir + copied, src, n);
		copied += n;
	}
	dbg("pak: %d entries, lba %u\n", ndir, (unsigned)base_lba);
	return 0;
}

const PakEntry *pak_find(const char *name)
{
	for (int i = 0; i < ndir; i++)
		if (!strncmp(dir[i].name, name, 16)) return &dir[i];
	return NULL;
}

int pak_trace;   /* debug: log every read (dbg) */

/* The drive may take a new command only when no asynchronous read is in flight (busy is cleared by
   its CD IRQ callback) and the CdlPause that ends every read has completed (its INT2, tracked by
   CdSync): the controller does not process commands reliably while that pause is still running.
   CD commands are only ever issued from the main thread, and only while the drive is idle. */
static int drive_idle(void)
{
	return !busy && CdSync(1, 0) != CdlNoIntr;
}

int pak_read(u32 sector, int nsectors, void *dst)
{
	CdlLOC loc;
	while (busy) ;
	CdSync(0, 0);   /* waits (bounded) for the pause of the previous read to complete */
	/* the library's read callback is shared with the async path: a sync read must not fire it */
	CdReadCallback(0);
	CdIntToPos(base_lba + sector, &loc);
	if (pak_trace) dbg("pak_read sec %u n %d vs %d\n", (unsigned)sector, nsectors, video_vsync_count());
	for (int attempt = 0; attempt < 4; attempt++) {
		if (!CdControl(CdlSetloc, &loc, 0)) continue;
		if (!CdRead(nsectors, (u32 *)dst, CdlModeSpeed)) continue;
		if (CdReadSync(0, 0) == 0) return 0;
	}
	return -1;
}

/* CD IRQ context: end of an async read */
static void read_done(CdlIntrResult res, u8 *result)
{
	void (*cb)(void) = done_cb;
	(void)res; (void)result;
	done_cb = 0;
	busy = 0;
	if (cb) cb();
}

int pak_read_async(u32 sector, int nsectors, void *dst, void (*done)(void))
{
	CdlLOC loc;
	if (!drive_idle()) return 0;
	busy = 1;
	done_cb = done;
	CdIntToPos(base_lba + sector, &loc);
	if (pak_trace) dbg("pak_async sec %u n %d vs %d\n", (unsigned)sector, nsectors, video_vsync_count());
	CdControl(CdlSetloc, &loc, 0);
	CdReadCallback(read_done);
	if (!CdRead(nsectors, (u32 *)dst, CdlModeSpeed)) {
		done_cb = 0;
		busy = 0;
		return 0;
	}
	return 1;
}

int pak_busy(void) { return !drive_idle(); }

int pak_read_bytes(const PakEntry *e, u32 offset, void *dst, u32 len)
{
	u8 *out = dst;
	if (offset + len > e->size) {
		if (offset >= e->size) return 0;
		len = e->size - offset;
	}
	u32 total = len;
	while (len) {
		u32 sec = offset / 2048, in = offset % 2048;
		u32 n = 2048 - in;
		if (n > len) n = len;
		if (in == 0 && n == 2048 && ((uintptr_t)out & 3) == 0) {
			/* whole aligned sectors: read as many as possible directly */
			int ns = len / 2048;
			if (ns > 32) ns = 32;
			if (pak_read(e->sector + sec, ns, out) < 0) return -1;
			n = ns * 2048;
		} else {
			if (pak_read(e->sector + sec, 1, bounce) < 0) return -1;
			memcpy(out, (u8 *)bounce + in, n);
		}
		out += n; offset += n; len -= n;
	}
	return total;
}
