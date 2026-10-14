#include <psxcd.h>
#include "psx.h"

#define MAXENTRIES 400

static PakEntry dir[MAXENTRIES];
static int ndir;
static u32 base_lba;
static volatile int busy;                 /* an async read is in flight */
static volatile int armed;                /* ... and CdRead() has returned (the SDK read state is valid) */
static void (*volatile done_cb)(int ok);
static volatile int busy_ticks, busy_limit;   /* vblanks the in-flight read has taken / may take */
static int sync_wait, sync_t0;            /* drive_idle(): waiting for the pause INT2 since this vsync */
extern CdlCB _cd_override_callback;       /* cdrom.c: CdRead()'s sector callback hook */

/* PS1: an async read that has not completed after ASYNC_TIMEOUT vblanks (+ nsectors/2, double speed
   is ~2.5 sectors/vblank) is cancelled and reported as failed (lost CD interrupt / bad disc). */
#define ASYNC_TIMEOUT 120
#define PAUSE_TIMEOUT 90      /* vblanks to wait for the CdlPause that ends a read before sending another */
static u32 bounce[2048 / 4];
void (*pak_progress)(unsigned long bytes);   /* see psx.h */

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

/* The drive may take a new command only when no asynchronous read is in flight (busy is cleared by
   its CD IRQ callback or by pak_tick()) and the CdlPause that ends every read has completed (its
   INT2, tracked by CdSync): the controller does not process commands reliably while that pause is
   still running.  If that INT2 never arrives the pause is repeated after PAUSE_TIMEOUT vblanks.
   CD commands are only ever issued from the main thread, and only while the drive is idle. */
static int drive_idle(void)
{
	if (busy) return 0;
	if (CdSync(1, 0) != CdlNoIntr) { sync_wait = 0; return 1; }
	if (!sync_wait) {
		sync_wait = 1;
		sync_t0 = video_vsync_count();
	} else if (video_vsync_count() - sync_t0 > PAUSE_TIMEOUT) {
		sync_wait = 0;
		CdControl(CdlPause, 0, 0);   /* PS1: lost INT2; a second pause completes again (BLOCKING command) */
	}
	return 0;
}

int pak_read(u32 sector, int nsectors, void *dst)
{
	CdlLOC loc;
	while (busy) ;   /* bounded: pak_tick() cancels a read that does not complete */
	sync_wait = 0;
	CdSync(0, 0);   /* waits (bounded) for the pause of the previous read to complete */
	/* the library's read callback is shared with the async path: a sync read must not fire it */
	CdReadCallback(0);
	CdIntToPos(base_lba + sector, &loc);
	for (int attempt = 0; attempt < 4; attempt++) {
		if (!CdControl(CdlSetloc, &loc, 0)) continue;
		if (!CdRead(nsectors, (u32 *)dst, CdlModeSpeed)) continue;
		if (CdReadSync(0, 0) == 0) {
			if (pak_progress) pak_progress((unsigned long)nsectors * 2048);
			return 0;
		}
	}
	return -1;
}

/* IRQ context: the async read is over (completed, failed or cancelled by the watchdog) */
static void finish(int ok)
{
	void (*cb)(int) = done_cb;
	done_cb = 0;
	armed = 0;
	busy = 0;
	if (cb) cb(ok);
}

/* CD IRQ context: end of an async read (the SDK calls this after the last sector or on an error) */
static void read_done(CdlIntrResult res, u8 *result)
{
	(void)result;
	/* PS1: after an error the SDK keeps refusing CdRead() until its 60 vblank cooldown has passed;
	   aborting the read state lets the caller retry at once */
	if (res != CdlDataReady) CdReadBreak();
	finish(res == CdlDataReady);
}

/* Vblank IRQ: watchdog for the in-flight async read.  CdReadSync(1) reports < 0 when the SDK gave up
   on it (no sector progress for 180 vblanks); we also give up after ASYNC_TIMEOUT.  The drive is
   told to pause (fire and forget: no interrupts can be waited for here) and CdRead()'s sector hook is
   removed so that a late sector cannot be copied into a buffer the caller already reused. */
void pak_tick(void)
{
	if (!busy || !armed) return;
	if (CdReadSync(1, 0) >= 0 && ++busy_ticks <= busy_limit) return;
	CdReadBreak();
	_cd_override_callback = 0;
	CdCommandF(CdlPause, 0, 0);
	finish(0);
}

int pak_read_async(u32 sector, int nsectors, void *dst, void (*done)(int ok))
{
	CdlLOC loc;
	if (!drive_idle()) return 0;
	sync_wait = 0;
	CdIntToPos(base_lba + sector, &loc);
	/* PS1: without an acknowledged Setloc the read would start at the previous position and return wrong data */
	if (!CdControl(CdlSetloc, &loc, 0)) return 0;
	done_cb = done;
	busy_ticks = 0;
	busy_limit = ASYNC_TIMEOUT + nsectors / 2;
	armed = 0;
	busy = 1;
	CdReadCallback(read_done);
	if (!CdRead(nsectors, (u32 *)dst, CdlModeSpeed)) {
		done_cb = 0;
		busy = 0;
		return 0;
	}
	if (busy) armed = 1;
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
