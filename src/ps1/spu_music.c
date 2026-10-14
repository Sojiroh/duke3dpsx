/* SPU music sequencer: MUSIC_* API.
 *
 * Songs are "MSQ1" event streams (tools/mkmusic.py) stored in DUKE.DAT under the original .MID
 * names; they are streamed from the CD through two 4 KB ring blocks (music_update() prefetches the
 * next block with pak_read_async()).  The instrument bank ("MUSBANK", ADPCM samples + region table)
 * is uploaded to SPU RAM once by MUSIC_Init().  The sequencer runs from a 120 Hz timer-2 interrupt
 * on SPU voices 0..SPU_NMUSIC-1; everything the ISR touches is plain RAM + SPU registers.
 */
#include "spu_priv.h"
#include "music.h"
#include <psxapi.h>
#include <psxetc.h>

int MUSIC_ErrorCode = MUSIC_Ok;

#define NMV SPU_NMUSIC
#define BLK 4096
#define MAXREGIONS 120
#define MUSIC_LEVEL 0x2000        /* hardware volume of a full-velocity note (0x3FFF = max) */

typedef struct {
	u32 off, size;
	u16 rate;
	u8 lo, hi, root;
	s8 fine;
	u8 flags, gain;
	u16 adsr1, adsr2, rel_ticks, dur_ticks;
	s8 pan;
	u8 pad[3];
} region_t;

static u8 progfirst[128], prognum[128], drumreg[128];
static region_t regions[MAXREGIONS];
static int nregions;
static int m_inited, bank_ok;

typedef struct { u8 prog, vol, expr, pan; s16 bend; } chan_t;
typedef struct {
	u8 state;               /* 0 free, 1 held, 2 releasing */
	u8 ch, note, vel;
	u8 region, drum;
	u16 remain, rel;        /* ticks until key-off / until free */
	u16 relt;               /* release ticks after key-off */
	u32 age;
} mvoice_t;

static chan_t chan[16];
static mvoice_t mv[NMV];

/* stream state (ISR owns cur_*, rp, m_wait; main thread owns the ring slots that are not current) */
static u8 ring[2][BLK] __attribute__((aligned(4)));
static volatile u16 slotblk[2];
static volatile u8 cur_slot;
static volatile u32 cur_blk, rp;
static u32 nblk, song_sector, song_sectors;
static volatile u8 m_playing, m_paused, m_loop, m_stall;
static volatile u32 m_wait, m_ticks;
static int m_master = 255;
static volatile u8 rd_inflight, rd_done, rd_failed, rd_slot;
static volatile u16 rd_blk;
static u32 stat_stalls, stat_events, stat_blocks;

static const u8 pan_cos[32] = {
	255, 255, 255, 253, 251, 248, 244, 240, 235, 230, 224, 217, 210, 202, 194, 186,
	176, 167, 157, 146, 135, 124, 113, 101, 89, 77, 64, 52, 39, 26, 13, 0,
};

/* ------------------------------------------------------------------------------------------ */
/* voices                                                                                      */
/* ------------------------------------------------------------------------------------------ */

static void mv_vol(int i)
{
	mvoice_t *v = &mv[i];
	const chan_t *c = &chan[v->ch];
	const region_t *r = &regions[v->region];
	int a = v->vel * r->gain / 127;
	a = a * c->vol / 127;
	a = a * c->expr / 127;
	a = a * a / 127;
	int p = c->pan + r->pan;
	if (p < 0) p = 0;
	if (p > 127) p = 127;
	int pi = p >> 2;
	u32 t = (u32)a * pan_cos[pi];        /* left: full at pan 0 */
	u32 t2 = (u32)a * pan_cos[31 - pi];  /* right */
	t = t * (u32)m_master / 255;
	t2 = t2 * (u32)m_master / 255;
	SPU_CH_VOL_L(i) = (u16)(t * MUSIC_LEVEL / (127 * 256));
	SPU_CH_VOL_R(i) = (u16)(t2 * MUSIC_LEVEL / (127 * 256));
}

static u32 mv_pitch(const mvoice_t *v, u32 *scale_out)
{
	const region_t *r = &regions[v->region];
	int cents = ((int)v->note - (int)r->root) * 100 + r->fine;
	if (!v->drum) cents += chan[v->ch].bend;
	u32 scale = spu_pitch_scale(cents);
	if (scale_out) *scale_out = scale;
	return spu_pitch_reg(r->rate, scale);
}

static void mv_free_all(void)
{
	spu_key_off(((1u << NMV) - 1));
	for (int i = 0; i < NMV; i++) mv[i].state = 0;
}

static int mv_alloc(void)
{
	int best = -1;
	for (int i = 0; i < NMV; i++)
		if (mv[i].state == 0) return i;
	for (int i = 0; i < NMV; i++)
		if (mv[i].state == 2 && (best < 0 || mv[i].rel < mv[best].rel)) best = i;
	if (best >= 0) return best;
	for (int i = 0; i < NMV; i++)
		if (best < 0 || mv[i].age < mv[best].age) best = i;
	return best;
}

static const region_t *find_region(int ch, int note, int *idx)
{
	int r;
	if (ch == 9) {
		r = drumreg[note & 127];
		if (r == 0xFF || r >= nregions) return 0;
	} else {
		int prog = chan[ch].prog & 127;
		int n = prognum[prog];
		int first = progfirst[prog];
		if (!n) { n = prognum[0]; first = progfirst[0]; }
		if (!n) return 0;
		r = first;
		for (int k = 0; k < n; k++) {
			const region_t *c = &regions[first + k];
			if (note >= c->lo && note <= c->hi) { r = first + k; break; }
		}
		if (r >= nregions) return 0;
	}
	*idx = r;
	return &regions[r];
}

static void note_on(int ch, int note, int vel, u32 dur)
{
	int ri;
	const region_t *r = find_region(ch, note, &ri);
	if (!r || vel <= 0) return;
	int i = mv_alloc();
	mvoice_t *v = &mv[i];
	spu_key_off(1u << i);
	v->ch = (u8)ch;
	v->note = (u8)note;
	v->vel = (u8)vel;
	v->region = (u8)ri;
	v->drum = (ch == 9);
	v->age = m_ticks;
	u32 scale;
	u32 pitch = mv_pitch(v, &scale);
	u32 remain = dur ? dur : 1;
	u32 relt = r->rel_ticks;
	if (!(r->flags & 1)) { /* one-shot: ends by itself after its natural length / pitch ratio */
		u32 life = ((u32)r->dur_ticks << 16) / scale + 1;
		if (v->drum || remain >= life) {
			remain = life;
			relt = 0;
		}
	}
	if (remain > 0xFFFF) remain = 0xFFFF;
	v->remain = (u16)remain;
	v->relt = (u16)relt;
	v->rel = 0;
	v->state = 1;
	mv_vol(i);
	SPU_CH_FREQ(i) = (u16)pitch;
	SPU_CH_ADDR(i) = (u16)((SPU_MUSIC_BASE + r->off + 7) / 8);
	SPU_CH_ADSR1(i) = r->adsr1;
	SPU_CH_ADSR2(i) = r->adsr2;
	spu_key_on(1u << i);
}

/* ------------------------------------------------------------------------------------------ */
/* stream reading (ISR)                                                                        */
/* ------------------------------------------------------------------------------------------ */

static inline u32 rb(void)
{
	u32 v = ring[cur_slot][rp++];
	if (rp == BLK) {
		cur_slot ^= 1;
		cur_blk = (cur_blk + 1 < nblk) ? cur_blk + 1 : 0;
		rp = 0;
	}
	return v;
}

static u32 rvlq(void)
{
	u32 v = 0, b;
	do {
		b = rb();
		v = (v << 7) | (b & 0x7F);
	} while (b & 0x80);
	return v;
}

/* 1 if at least 16 bytes can be read from the current position without waiting for the CD */
static int seq_need(void)
{
	if (nblk <= 1) return 1;
	u32 other = cur_slot ^ 1;
	if (rp + 16 > BLK) {
		u32 nxt = (cur_blk + 1 < nblk) ? cur_blk + 1 : 0;
		return slotblk[other] == nxt;
	}
	if (cur_blk == nblk - 1) return slotblk[other] == 0; /* the loop target must be resident */
	return 1;
}

static void chan_reset(void)
{
	for (int c = 0; c < 16; c++) {
		chan[c].prog = 0;
		chan[c].vol = 100;
		chan[c].expr = 127;
		chan[c].pan = 64;
		chan[c].bend = 0;
	}
}

static void seq_rewind(void)
{
	if (nblk <= 1) cur_slot = 0;
	else cur_slot = (slotblk[0] == 0) ? 0 : 1;
	cur_blk = 0;
	rp = 16;
	chan_reset();
}

static void seq_stop_notes(int ch)
{
	for (int i = 0; i < NMV; i++) {
		mvoice_t *v = &mv[i];
		if (v->state == 1 && (ch < 0 || v->ch == ch)) {
			spu_key_off(1u << i);
			v->state = v->relt ? 2 : 0;
			v->rel = v->relt;
		}
	}
}

static void seq_event(void)
{
	u32 cmd = rb();
	int ch = cmd & 15;
	stat_events++;
	switch (cmd >> 4) {
	case 0x0: {
		u32 note = rb();
		u32 vel = rb();
		u32 dur = rvlq();
		note_on(ch, note & 127, vel, dur);
		break;
	}
	case 0x1: chan[ch].prog = (u8)(rb() & 127); break;
	case 0x2:
		chan[ch].vol = (u8)(rb() & 127);
		for (int i = 0; i < NMV; i++) if (mv[i].state && mv[i].ch == ch) mv_vol(i);
		break;
	case 0x3:
		chan[ch].pan = (u8)(rb() & 127);
		for (int i = 0; i < NMV; i++) if (mv[i].state && mv[i].ch == ch) mv_vol(i);
		break;
	case 0x4:
		chan[ch].expr = (u8)(rb() & 127);
		for (int i = 0; i < NMV; i++) if (mv[i].state && mv[i].ch == ch) mv_vol(i);
		break;
	case 0x5: {
		u32 lo = rb(), hi = rb();
		chan[ch].bend = (s16)(lo | (hi << 8));
		if (ch != 9)
			for (int i = 0; i < NMV; i++)
				if (mv[i].state == 1 && mv[i].ch == ch) SPU_CH_FREQ(i) = (u16)mv_pitch(&mv[i], 0);
		break;
	}
	case 0x6: seq_stop_notes(ch); break;
	case 0xF:
		/* END: loop (or stop) */
		if (!m_loop) {
			m_playing = 0;
			seq_stop_notes(-1);
			return;
		}
		seq_rewind();
		break;
	default: break;
	}
}

static void music_isr(void)
{
	if (!m_playing || m_paused) return;
	m_ticks++;
	for (int i = 0; i < NMV; i++) {
		mvoice_t *v = &mv[i];
		if (v->state == 1) {
			if (--v->remain == 0) {
				spu_key_off(1u << i);
				if (v->relt) { v->state = 2; v->rel = v->relt; }
				else v->state = 0;
			}
		} else if (v->state == 2) {
			if (--v->rel == 0) v->state = 0;
		}
	}
	if (m_wait) m_wait--;
	while (m_wait == 0 && m_playing) {
		if (!seq_need()) { m_stall = 1; stat_stalls++; return; }
		m_stall = 0;
		seq_event();
		if (!m_playing) return;
		m_wait = rvlq();
	}
}

/* ------------------------------------------------------------------------------------------ */
/* main-thread side: stream prefetch                                                           */
/* ------------------------------------------------------------------------------------------ */

/* IRQ context; ok = 0: CD error or timeout (the ring slot stays invalid and is requested again) */
static void rd_cb(int ok) { if (ok) rd_done = 1; else rd_failed = 1; }

static u32 blk_sectors(u32 blk)
{
	u32 first = blk * (BLK / 2048);
	u32 n = song_sectors - first;
	return n > BLK / 2048 ? BLK / 2048 : n;
}

void music_update(void)
{
	if (!m_playing || nblk <= 1) return;
	if (rd_inflight) {
		if (!rd_done && !rd_failed) return;
		/* PS1: a failed read leaves the slot marked invalid (slotblk = 0xFFFF set when it was issued),
		   so the code below requests the same block again */
		if (rd_done) slotblk[rd_slot] = rd_blk;
		rd_inflight = 0;
#ifdef PS1_PROF
		if (rd_done && stat_blocks++ < 3)
			dbg("music: streamed block %u/%u into ring slot %u (%u blocks so far, ticks %u, events %u, stalls %u)\n",
			    (unsigned)rd_blk, (unsigned)nblk, (unsigned)rd_slot, (unsigned)stat_blocks, (unsigned)m_ticks,
			    (unsigned)stat_events, (unsigned)stat_stalls);
#endif
	}
	u32 other = cur_slot ^ 1;
	u32 nxt = (cur_blk + 1 < nblk) ? cur_blk + 1 : 0;
	if (slotblk[other] == nxt || pak_busy()) return;
	slotblk[other] = 0xFFFF;
	rd_slot = (u8)other;
	rd_blk = (u16)nxt;
	rd_done = rd_failed = 0;
	rd_inflight = 1;
	if (!pak_read_async(song_sector + nxt * (BLK / 2048), blk_sectors(nxt), ring[other], rd_cb)) {
		rd_inflight = 0;
		slotblk[other] = 0xFFFF;
	}
}

/* ------------------------------------------------------------------------------------------ */
/* public API                                                                                  */
/* ------------------------------------------------------------------------------------------ */

char *MUSIC_ErrorString(int ErrorNumber)
{
	switch (ErrorNumber) {
	case MUSIC_Warning:
	case MUSIC_Error: return "Music error.";
	case MUSIC_Ok: return "Music ok.";
	case MUSIC_MidiError: return "Error reading MIDI data.";
	case MUSIC_SoundCardError: return "SPU instrument bank not available.";
	}
	return "Unknown music error code.";
}

#ifndef MUSIC_TEST_SPEED
#define MUSIC_TEST_SPEED 1      /* host test builds only: run the sequencer faster */
#endif
#define TIMER2_PERIOD (35279 / MUSIC_TEST_SPEED)  /* sysclk/8 = 4.2336 MHz -> 120 Hz */

int MUSIC_Init(int SoundCard, int Address)
{
	(void)SoundCard; (void)Address;
	if (m_inited) return MUSIC_Ok;
	spu_hw_init();
	const PakEntry *e = pak_find("MUSBANK");
	if (!e) {
		dbg("music: MUSBANK not found in DUKE.DAT\n");
		MUSIC_ErrorCode = MUSIC_SoundCardError;
		return MUSIC_Error;
	}
	u8 *hdr = spu_staging();
	if (pak_read(e->sector, 2, hdr) < 0 || memcmp(hdr, "MBK1", 4)) {
		MUSIC_ErrorCode = MUSIC_SoundCardError;
		return MUSIC_Error;
	}
	nregions = *(u16 *)(hdr + 4);
	u32 data_off = *(u32 *)(hdr + 8);
	u32 data_size = *(u32 *)(hdr + 12);
	if (nregions > MAXREGIONS || data_size > SPU_MUSIC_LIMIT - SPU_MUSIC_BASE) {
		dbg("music: bad MUSBANK (regions %d size %u)\n", nregions, (unsigned)data_size);
		MUSIC_ErrorCode = MUSIC_SoundCardError;
		return MUSIC_Error;
	}
	memcpy(progfirst, hdr + 0x10, 128);
	memcpy(prognum, hdr + 0x90, 128);
	memcpy(drumreg, hdr + 0x110, 128);
	memcpy(regions, hdr + 0x190, nregions * sizeof(region_t));
	if (spu_load_adpcm(e->sector + data_off / 2048, data_size, SPU_MUSIC_BASE, 0) < 0) {
		MUSIC_ErrorCode = MUSIC_SoundCardError;
		return MUSIC_Error;
	}
	dbg("music: bank %d regions, %u bytes at SPU 0x%x\n", nregions, (unsigned)data_size, SPU_MUSIC_BASE);
	bank_ok = 1;
	m_playing = m_paused = 0;
	m_loop = 1;
	chan_reset();
	memset(mv, 0, sizeof(mv));
	InterruptCallback(IRQ_TIMER2, music_isr);
	TIMER_CTRL(2) = 0;
	TIMER_VALUE(2) = 0;
	TIMER_RELOAD(2) = TIMER2_PERIOD;
	TIMER_CTRL(2) = 0x0258; /* sysclk/8, IRQ on target, repeat, reset counter at target */
	m_inited = 1;
	return MUSIC_Ok;
}

int MUSIC_Shutdown(void)
{
	if (!m_inited) return MUSIC_Ok;
	MUSIC_StopSong();
	TIMER_CTRL(2) = 0;
	InterruptCallback(IRQ_TIMER2, 0);
	m_inited = 0;
	return MUSIC_Ok;
}

void MUSIC_SetVolume(int volume)
{
	if (volume < 0) volume = 0;
	if (volume > 255) volume = 255;
	EnterCriticalSection();
	m_master = volume;
	for (int i = 0; i < NMV; i++)
		if (mv[i].state) mv_vol(i);
	ExitCriticalSection();
}

int MUSIC_GetVolume(void) { return m_master; }

void MUSIC_SetLoopFlag(int loopflag) { m_loop = loopflag != 0; }

int MUSIC_SongPlaying(void) { return m_playing; }

void MUSIC_Continue(void) { m_paused = 0; }

void MUSIC_Pause(void)
{
	if (!m_inited) return;
	EnterCriticalSection();
	m_paused = 1;
	mv_free_all();
	ExitCriticalSection();
}

/* PS1: silence the sequencer for the duration of a memory card operation (Timer 2 is masked then, and
   keyed-on voices would drone on).  Position and channel state are kept; notes that were sounding are
   dropped, like MUSIC_Pause().  A pause requested by the game stays in force. */
static u8 hold_set;
void music_hold(void)
{
	if (!m_inited || hold_set) return;
	EnterCriticalSection();
	if (!m_paused) {
		m_paused = 1;
		mv_free_all();
		hold_set = 1;
	}
	ExitCriticalSection();
}

void music_resume(void)
{
	if (!hold_set) return;
	hold_set = 0;
	m_paused = 0;
}

int MUSIC_StopSong(void)
{
	if (!m_inited) return MUSIC_Ok;
	EnterCriticalSection();
	m_playing = 0;
	m_paused = 0;
	mv_free_all();
	ExitCriticalSection();
	return MUSIC_Ok;
}

int MUSIC_PlaySong(unsigned char *song, int loopflag)
{
	char name[16];
	if (!m_inited || !bank_ok) return MUSIC_Error;
	int n = 0;
	for (const char *s = (const char *)song; *s && n < 15; s++, n++)
		name[n] = (*s >= 'a' && *s <= 'z') ? *s - 32 : *s;
	name[n] = 0;
	MUSIC_StopSong();
	const PakEntry *e = pak_find(name);
	if (!e) {
		dbg("music: song %s not found\n", name);
		MUSIC_ErrorCode = MUSIC_MidiError;
		return MUSIC_Error;
	}
	song_sector = e->sector;
	song_sectors = (e->size + 2047) / 2048;
	if (pak_read(song_sector, blk_sectors(0), ring[0]) < 0 || memcmp(ring[0], "MSQ1", 4)) {
		MUSIC_ErrorCode = MUSIC_MidiError;
		return MUSIC_Error;
	}
	u32 total = 16 + *(u32 *)(ring[0] + 4);
	nblk = (total + BLK - 1) / BLK;
	rd_inflight = 0;
	slotblk[0] = 0;
	slotblk[1] = 0xFFFF;
	if (nblk > 1) {
		if (pak_read(song_sector + BLK / 2048, blk_sectors(1), ring[1]) < 0) {
			MUSIC_ErrorCode = MUSIC_MidiError;
			return MUSIC_Error;
		}
		slotblk[1] = 1;
	}
	EnterCriticalSection();
	m_loop = loopflag != 0;
	m_stall = 0;
	m_ticks = 0;
	seq_rewind();
	m_wait = rvlq();
	m_playing = 1;
	ExitCriticalSection();
	return MUSIC_Ok;
}

void MUSIC_RegisterTimbreBank(unsigned char *timbres) { (void)timbres; }

#ifdef PS1_PROF
void music_debug(void)
{
	int act = 0;
	for (int i = 0; i < NMV; i++) act += mv[i].state != 0;
	dbg("music: playing=%d paused=%d loop=%d ticks=%u blk=%u/%u rp=%u wait=%u stalls=%u events=%u active=%d timer=%08x\n",
	    m_playing, m_paused, m_loop, (unsigned)m_ticks, (unsigned)cur_blk, (unsigned)nblk, (unsigned)rp, (unsigned)m_wait,
	    (unsigned)stat_stalls, (unsigned)stat_events, act, (unsigned)TIMER_VALUE(2));
	for (int i = 0; i < NMV; i++)
		if (mv[i].state)
			dbg("  mv%d st=%d ch=%d note=%d vel=%d reg=%d pitch=%04x vol=%04x/%04x adsr=%04x/%04x\n", i, mv[i].state,
			    mv[i].ch, mv[i].note, mv[i].vel, mv[i].region, SPU_CH_FREQ(i), SPU_CH_VOL_L(i), SPU_CH_VOL_R(i),
			    SPU_CH_ADSR1(i), SPU_CH_ADSR2(i));
}
#endif
