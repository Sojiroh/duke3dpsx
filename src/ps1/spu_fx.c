/* SPU sound-effect layer: audiolib-compatible FX_* API over the PS1 SPU.
 *
 * - voices: SPU voices SPU_FIRSTFX..23 (FX_MAXVOICES = 14); music owns voices 0..9 (spu_music.c)
 * - samples: SPU-ADPCM blobs from DUKE.DAT, kept in an LRU cache in SPU RAM (SPU_FX_BASE..LIMIT)
 * - volume / pan / pitch math is audiolib's (MV_Pan3D / MV_PanTable / MIX_VOLUME / PITCH_GetScale)
 * - end of a voice is detected by software timing (sample length / pitch) in fx_update(), called
 *   from audio_update() on the main thread; FX_StopSound & voice stealing call the callback at once.
 */
#include "spu_priv.h"
#include "fx_man.h"

#define SPU_RVB(n) _MMIO16((IOBASE | 0x1dc0) + 2 * (n))

/* ------------------------------------------------------------------------------------------ */
/* hardware init, staging buffer, DMA, pitch                                                   */
/* ------------------------------------------------------------------------------------------ */

static u32 staging[SPU_STAGING_BYTES / 4] __attribute__((aligned(16)));
static int hw_inited;

u8 *spu_staging(void) { return (u8 *)staging; }

/* Monotonic 120 Hz clock. The game rewinds totalclock (level start, menu close), which
   would leave voices "active" for as long as the clock went back. */
static inline long fx_clock(void) { return (long)video_vsync_count() * 2; }

/* Room reverb preset (psx-spx "SPU Reverb Examples", size 0x26C0 bytes) */
static const u16 reverb_room[32] = {
	0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80,
	0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF,
	0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000,
};

void spu_dma_write(u32 addr, const void *src, u32 bytes)
{
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	SpuSetTransferStartAddr(addr);
	SpuWrite((const uint32_t *)src, bytes);
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
}

void spu_reverb_voices(u32 mask)
{
	SPU_REVERB_ON1 = (u16)mask;
	SPU_REVERB_ON2 = (u16)(mask >> 16);
}

void spu_hw_init(void)
{
	if (hw_inited) return;
	hw_inited = 1;
	SpuInit();
	SPU_MASTER_VOL_L = 0x3FFF;
	SPU_MASTER_VOL_R = 0x3FFF;
	spu_key_off(0xFFFFFF);
	for (int ch = 0; ch < 24; ch++) {
		SPU_CH_VOL_L(ch) = 0;
		SPU_CH_VOL_R(ch) = 0;
		SPU_CH_ADSR1(ch) = SPU_ADSR1_FLAT;
		SPU_CH_ADSR2(ch) = SPU_ADSR2_FLAT;
	}
	/* reverb: clear the work area, load the preset, output volume 0 until FX_SetReverb() */
	SPU_CTRL &= (u16)~0x80;
	memset(staging, 0, SPU_REVERB_SIZE);
	spu_dma_write(SPU_REVERB_BASE, staging, SPU_REVERB_SIZE);
	for (int i = 0; i < 32; i++) SPU_RVB(i) = reverb_room[i];
	SPU_REVERB_ADDR = (u16)(SPU_REVERB_BASE / 8);
	SPU_REVERB_VOL_L = 0;
	SPU_REVERB_VOL_R = 0;
	spu_reverb_voices(0);
	dbg("spu: init ctrl=%04x stat=%04x\n", SPU_CTRL, SPU_STAT);
}

/* audiolib PITCH.C table: 2^((note*25+detune)/(12*25)) as 16.16 */
#define MAXDETUNE 25
static const u32 PitchTable[12][MAXDETUNE] = {
	{ 0x10000, 0x10097, 0x1012f, 0x101c7, 0x10260, 0x102f9, 0x10392, 0x1042c, 0x104c6, 0x10561, 0x105fb, 0x10696, 0x10732,
	  0x107ce, 0x1086a, 0x10907, 0x109a4, 0x10a41, 0x10adf, 0x10b7d, 0x10c1b, 0x10cba, 0x10d59, 0x10df8, 0x10e98 },
	{ 0x10f38, 0x10fd9, 0x1107a, 0x1111b, 0x111bd, 0x1125f, 0x11302, 0x113a5, 0x11448, 0x114eb, 0x1158f, 0x11634, 0x116d8,
	  0x1177e, 0x11823, 0x118c9, 0x1196f, 0x11a16, 0x11abd, 0x11b64, 0x11c0c, 0x11cb4, 0x11d5d, 0x11e06, 0x11eaf },
	{ 0x11f59, 0x12003, 0x120ae, 0x12159, 0x12204, 0x122b0, 0x1235c, 0x12409, 0x124b6, 0x12563, 0x12611, 0x126bf, 0x1276d,
	  0x1281c, 0x128cc, 0x1297b, 0x12a2b, 0x12adc, 0x12b8d, 0x12c3e, 0x12cf0, 0x12da2, 0x12e55, 0x12f08, 0x12fbc },
	{ 0x1306f, 0x13124, 0x131d8, 0x1328d, 0x13343, 0x133f9, 0x134af, 0x13566, 0x1361d, 0x136d5, 0x1378d, 0x13846, 0x138fe,
	  0x139b8, 0x13a72, 0x13b2c, 0x13be6, 0x13ca1, 0x13d5d, 0x13e19, 0x13ed5, 0x13f92, 0x1404f, 0x1410d, 0x141cb },
	{ 0x1428a, 0x14349, 0x14408, 0x144c8, 0x14588, 0x14649, 0x1470a, 0x147cc, 0x1488e, 0x14951, 0x14a14, 0x14ad7, 0x14b9b,
	  0x14c5f, 0x14d24, 0x14dea, 0x14eaf, 0x14f75, 0x1503c, 0x15103, 0x151cb, 0x15293, 0x1535b, 0x15424, 0x154ee },
	{ 0x155b8, 0x15682, 0x1574d, 0x15818, 0x158e4, 0x159b0, 0x15a7d, 0x15b4a, 0x15c18, 0x15ce6, 0x15db4, 0x15e83, 0x15f53,
	  0x16023, 0x160f4, 0x161c5, 0x16296, 0x16368, 0x1643a, 0x1650d, 0x165e1, 0x166b5, 0x16789, 0x1685e, 0x16934 },
	{ 0x16a09, 0x16ae0, 0x16bb7, 0x16c8e, 0x16d66, 0x16e3e, 0x16f17, 0x16ff1, 0x170ca, 0x171a5, 0x17280, 0x1735b, 0x17437,
	  0x17513, 0x175f0, 0x176ce, 0x177ac, 0x1788a, 0x17969, 0x17a49, 0x17b29, 0x17c09, 0x17cea, 0x17dcc, 0x17eae },
	{ 0x17f91, 0x18074, 0x18157, 0x1823c, 0x18320, 0x18406, 0x184eb, 0x185d2, 0x186b8, 0x187a0, 0x18888, 0x18970, 0x18a59,
	  0x18b43, 0x18c2d, 0x18d17, 0x18e02, 0x18eee, 0x18fda, 0x190c7, 0x191b5, 0x192a2, 0x19391, 0x19480, 0x1956f },
	{ 0x1965f, 0x19750, 0x19841, 0x19933, 0x19a25, 0x19b18, 0x19c0c, 0x19d00, 0x19df4, 0x19ee9, 0x19fdf, 0x1a0d5, 0x1a1cc,
	  0x1a2c4, 0x1a3bc, 0x1a4b4, 0x1a5ad, 0x1a6a7, 0x1a7a1, 0x1a89c, 0x1a998, 0x1aa94, 0x1ab90, 0x1ac8d, 0x1ad8b },
	{ 0x1ae89, 0x1af88, 0x1b088, 0x1b188, 0x1b289, 0x1b38a, 0x1b48c, 0x1b58f, 0x1b692, 0x1b795, 0x1b89a, 0x1b99f, 0x1baa4,
	  0x1bbaa, 0x1bcb1, 0x1bdb8, 0x1bec0, 0x1bfc9, 0x1c0d2, 0x1c1dc, 0x1c2e6, 0x1c3f1, 0x1c4fd, 0x1c609, 0x1c716 },
	{ 0x1c823, 0x1c931, 0x1ca40, 0x1cb50, 0x1cc60, 0x1cd70, 0x1ce81, 0x1cf93, 0x1d0a6, 0x1d1b9, 0x1d2cd, 0x1d3e1, 0x1d4f6,
	  0x1d60c, 0x1d722, 0x1d839, 0x1d951, 0x1da69, 0x1db82, 0x1dc9c, 0x1ddb6, 0x1ded1, 0x1dfec, 0x1e109, 0x1e225 },
	{ 0x1e343, 0x1e461, 0x1e580, 0x1e6a0, 0x1e7c0, 0x1e8e0, 0x1ea02, 0x1eb24, 0x1ec47, 0x1ed6b, 0x1ee8f, 0x1efb4, 0x1f0d9,
	  0x1f1ff, 0x1f326, 0x1f44e, 0x1f576, 0x1f69f, 0x1f7c9, 0x1f8f3, 0x1fa1e, 0x1fb4a, 0x1fc76, 0x1fda3, 0x1fed1 },
};

/* audiolib PITCH_GetScale: 0x10000 == 1.0 for pitchoffset 0; 1200 per octave. Result is 16.16 *and*
   shifted by the octave, i.e. 2^(cents/1200) * 0x10000. */
u32 spu_pitch_scale(int pitchoffset)
{
	if (pitchoffset == 0) return 0x10000;
	if (pitchoffset > 4800) pitchoffset = 4800;
	if (pitchoffset < -4800) pitchoffset = -4800;
	int noteshift = pitchoffset % 1200;
	if (noteshift < 0) noteshift += 1200;
	int note = noteshift / 100;
	int detune = (noteshift % 100) / (100 / MAXDETUNE);
	int octaveshift = (pitchoffset - noteshift) / 1200;
	u32 scale = PitchTable[note][detune];
	/* PitchTable entries are 2^(n/12/25) * 0x10000 */
	if (octaveshift < 0) scale >>= -octaveshift;
	else scale <<= octaveshift;
	return scale;
}

u32 spu_pitch_reg(u32 rate, u32 scale)
{
	/* pitch = rate * (scale/65536) * 4096 / 44100 */
	u32 p = (rate * (scale >> 4)) / 44100;
	if (p < 1) p = 1;
	if (p > 0x3FFF) p = 0x3FFF;
	return p;
}

/* ------------------------------------------------------------------------------------------ */
/* loading SPU-ADPCM from the pack                                                             */
/* ------------------------------------------------------------------------------------------ */

int spu_load_adpcm(u32 sector, u32 bytes, u32 addr, int loop)
{
	u8 *buf = (u8 *)staging;
	u32 off = 0;
	while (off < bytes) {
		u32 n = bytes - off;
		if (n > SPU_STAGING_BYTES) n = SPU_STAGING_BYTES;
		u32 nsec = (n + 2047) / 2048;
		if (pak_read(sector + off / 2048, nsec, buf) < 0) return -1;
		if (loop) {
			u32 last = bytes - 32; /* last audio block; the final block is the silent terminator */
			if (off == 0) buf[1] |= 0x04;
			if (last >= off && last < off + n) buf[last - off + 1] |= 0x03;
		}
		u32 dn = (n + 63) & ~63u;
		spu_dma_write(addr + off, buf, dn);
		off += n;
	}
	return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* SPU RAM sample cache (LRU, variable size slots)                                             */
/* ------------------------------------------------------------------------------------------ */

#define NSLOTS 192
typedef struct {
	u32 addr, size;     /* size rounded up to 64 */
	fx_sample *s;       /* NULL = slot unused */
	u32 last;           /* fx_clock() of last use */
} cslot;
static cslot slots[NSLOTS];
static int cache_loads, cache_evicts;

/* voices */
typedef struct {
	u8 active, looped, prio, slot;
	u8 l, r;               /* pan levels 0..255 (audiolib left/right) */
	u32 serial;
	u32 cb;
	long start, end;       /* fx_clock() */
	fx_sample *s;
} fxvoice;
static fxvoice V[FX_MAXVOICES];

static int slot_in_use(int i)
{
	for (int v = 0; v < FX_MAXVOICES; v++)
		if (V[v].active && V[v].slot == i) return 1;
	return 0;
}

static void slot_free(int i)
{
	if (slots[i].s) slots[i].s->slot = 0;
	slots[i].s = 0;
}

/* first-fit search over the used slots sorted by address; returns 0 if nothing fits */
static u32 cache_find_gap(u32 size)
{
	u8 order[NSLOTS];
	int n = 0;
	for (int i = 0; i < NSLOTS; i++) {
		if (!slots[i].s) continue;
		int j = n++;
		while (j > 0 && slots[order[j - 1]].addr > slots[i].addr) { order[j] = order[j - 1]; j--; }
		order[j] = (u8)i;
	}
	u32 cur = SPU_FX_BASE;
	for (int k = 0; k < n; k++) {
		cslot *c = &slots[order[k]];
		if (c->addr >= cur && c->addr - cur >= size) return cur;
		if (c->addr + c->size > cur) cur = c->addr + c->size;
	}
	if (SPU_FX_LIMIT - cur >= size) return cur;
	return 0;
}

int FX_LoadSample(fx_sample *s)
{
	if (!s || s->magic != FX_SAMPLE_MAGIC) return 0;
	if (s->slot) {
		slots[s->slot - 1].last = fx_clock();
		return 1;
	}
	u32 size = (s->size + 63) & ~63u;
	if (size == 0 || size > SPU_FX_LIMIT - SPU_FX_BASE) return 0;
	u32 addr;
	while (!(addr = cache_find_gap(size))) {
		/* evict the least recently used sample that no voice is playing */
		int victim = -1;
		for (int i = 0; i < NSLOTS; i++)
			if (slots[i].s && !slot_in_use(i) && (victim < 0 || (long)(slots[victim].last - slots[i].last) > 0))
				victim = i;
		if (victim < 0) return 0;
		slot_free(victim);
		cache_evicts++;
	}
	int fi = -1;
	for (int i = 0; i < NSLOTS; i++)
		if (!slots[i].s) { fi = i; break; }
	if (fi < 0) { /* table full: drop the LRU one */
		int victim = -1;
		for (int i = 0; i < NSLOTS; i++)
			if (!slot_in_use(i) && (victim < 0 || (long)(slots[victim].last - slots[i].last) > 0)) victim = i;
		if (victim < 0) return 0;
		slot_free(victim);
		fi = victim;
	}
	if (spu_load_adpcm(s->sector, s->size, addr, s->loop) < 0) return 0;
	slots[fi].addr = addr;
	slots[fi].size = size;
	slots[fi].s = s;
	slots[fi].last = fx_clock();
	s->slot = (u8)(fi + 1);
	cache_loads++;
	return 1;
}

int FX_CacheUsed(void)
{
	u32 used = 0;
	for (int i = 0; i < NSLOTS; i++)
		if (slots[i].s) used += slots[i].size;
	return (int)used;
}

static void fx_free_sample(fx_sample *s)
{
	if (s->slot && !slot_in_use(s->slot - 1)) slot_free(s->slot - 1);
}

void fx_debug(void)
{
	u32 used = 0;
	int n = 0;
	for (int i = 0; i < NSLOTS; i++)
		if (slots[i].s) { used += slots[i].size; n++; }
	dbg("fx: cache %d samples, %u/%u bytes, loads %d evicts %d\n", n, (unsigned)used,
	    (unsigned)(SPU_FX_LIMIT - SPU_FX_BASE), cache_loads, cache_evicts);
	for (int v = 0; v < FX_MAXVOICES; v++)
		if (V[v].active)
			dbg("fx: voice %d ch%d cb=%u prio=%d l=%d r=%d loop=%d pitch=%04x vol=%04x/%04x adsr=%04x/%04x\n", v,
			    SPU_FIRSTFX + v, (unsigned)V[v].cb, V[v].prio, V[v].l, V[v].r, V[v].looped,
			    SPU_CH_FREQ(SPU_FIRSTFX + v), SPU_CH_VOL_L(SPU_FIRSTFX + v), SPU_CH_VOL_R(SPU_FIRSTFX + v),
			    SPU_CH_ADSR1(SPU_FIRSTFX + v), SPU_CH_ADSR2(SPU_FIRSTFX + v));
}

/* ------------------------------------------------------------------------------------------ */
/* FX API                                                                                      */
/* ------------------------------------------------------------------------------------------ */

#define MAXPANPOS 31
#define NUMPANPOS (MAXPANPOS + 1)
#define MIX_VOLUME(v) (((((v) < 0) ? 0 : (((v) > 255) ? 255 : (v))) * 64) >> 8)

static u8 pan_left[NUMPANPOS][64], pan_right[NUMPANPOS][64];
static int fx_installed, fx_volume = 255, fx_reverse, fx_reverb, fx_reverb_delay;
static u32 fx_serial;
static int fx_log_left = 8;     /* one-time diagnostics: the first few voices started */
static void (*fx_callback)(unsigned long);

#define FX_VOICEMASK (((1u << 24) - 1) & ~((1u << SPU_FIRSTFX) - 1))

static void calc_pan_table(void)
{
	int half = NUMPANPOS / 2;
	for (int distance = 0; distance <= 63; distance++) {
		int level = (255 * (63 - distance)) / 63;
		for (int angle = 0; angle <= half / 2; angle++) {
			int ramp = level - ((level * angle) / (NUMPANPOS / 4));
			pan_left[angle][distance] = ramp;
			pan_left[half - angle][distance] = ramp;
			pan_left[half + angle][distance] = level;
			pan_left[MAXPANPOS - angle][distance] = level;
			pan_right[angle][distance] = level;
			pan_right[half - angle][distance] = level;
			pan_right[half + angle][distance] = ramp;
			pan_right[MAXPANPOS - angle][distance] = ramp;
		}
	}
}

static fxvoice *voice_of(int handle)
{
	if (handle <= 0) return 0;
	int idx = handle & 31;
	if (idx >= FX_MAXVOICES) return 0;
	fxvoice *v = &V[idx];
	if (!v->active || v->serial != (u32)(handle >> 5)) return 0;
	return v;
}

static void apply_volume(int idx)
{
	fxvoice *v = &V[idx];
	int l = v->l, r = v->r;
	if (fx_reverse) { int t = l; l = r; r = t; }
	u32 vl = ((u32)l * (u32)fx_volume * 0x3FFF) / (255 * 255);
	u32 vr = ((u32)r * (u32)fx_volume * 0x3FFF) / (255 * 255);
	SPU_CH_VOL_L(SPU_FIRSTFX + idx) = (u16)vl;
	SPU_CH_VOL_R(SPU_FIRSTFX + idx) = (u16)vr;
}

static void voice_stop(int idx, int docallback)
{
	fxvoice *v = &V[idx];
	if (!v->active) return;
	v->active = 0;
	spu_key_off(1u << (SPU_FIRSTFX + idx));
	if (docallback && fx_callback) fx_callback(v->cb);
}

/* audiolib MV_AllocVoice: free voice, else steal the lowest priority one if priority >= its */
static int voice_alloc(int priority)
{
	for (int i = 0; i < FX_MAXVOICES; i++)
		if (!V[i].active) return i;
	int low = 0;
	for (int i = 1; i < FX_MAXVOICES; i++)
		if (V[i].prio < V[low].prio) low = i;
	if (priority >= V[low].prio) {
		voice_stop(low, 1);
		return low;
	}
	return -1;
}

int FX_VoiceAvailable(int priority)
{
	if (!fx_installed) return 0;
	int low = 0;
	for (int i = 0; i < FX_MAXVOICES; i++) {
		if (!V[i].active) return 1;
		if (V[i].prio < V[low].prio) low = i;
	}
	return priority >= V[low].prio;
}

static int fx_play(fx_sample *s, int pitchoffset, int left, int right, int priority, unsigned long cb, int loop)
{
	if (!fx_installed || !s || s->magic != FX_SAMPLE_MAGIC) return FX_Warning;
	if (loop && !s->loop) { fx_free_sample(s); s->loop = 1; }
	if (!FX_LoadSample(s)) return FX_Warning;
	int idx = voice_alloc(priority);
	if (idx < 0) return FX_Warning;
	fxvoice *v = &V[idx];
	cslot *c = &slots[s->slot - 1];
	c->last = fx_clock();
	int ch = SPU_FIRSTFX + idx;
	u32 pitch = spu_pitch_reg(s->rate, spu_pitch_scale(pitchoffset));
	if (++fx_serial > 0x3FFFFF) fx_serial = 1;
	v->active = 1;
	v->looped = s->loop;
	v->prio = (u8)(priority < 0 ? 0 : (priority > 255 ? 255 : priority));
	v->slot = s->slot - 1;
	v->l = (u8)(left < 0 ? 0 : (left > 255 ? 255 : left));
	v->r = (u8)(right < 0 ? 0 : (right > 255 ? 255 : right));
	v->serial = fx_serial;
	v->cb = cb;
	v->s = s;
	v->start = fx_clock();
	/* length in 120 Hz ticks: samples * 120*4096/(44100*pitch) */
	u32 nsamples = (s->size / 16 - 1) * 28;
	v->end = fx_clock() + (long)((nsamples * 11145u) / (pitch * 1000u)) + 3;
	spu_key_off(1u << ch);
	apply_volume(idx);
	SPU_CH_FREQ(ch) = (u16)pitch;
	SPU_CH_ADDR(ch) = (u16)((c->addr + 7) / 8);
	SPU_CH_ADSR1(ch) = SPU_ADSR1_FLAT;
	SPU_CH_ADSR2(ch) = SPU_ADSR2_FLAT;
	spu_key_on(1u << ch);
	if (fx_log_left > 0) {
		fx_log_left--;
		dbg("fx: play voice %d (spu ch %d) handle %d spu addr 0x%05x size %u rate %u pitch %04x l/r %d/%d clock %ld\n", idx, ch,
		    (int)((v->serial << 5) | idx), (unsigned)c->addr, (unsigned)s->size, (unsigned)s->rate, (unsigned)pitch, v->l, v->r,
		    fx_clock());
	}
	return (int)((v->serial << 5) | idx);
}

int FX_PlayVOC(char *ptr, int pitchoffset, int vol, int left, int right, int priority, unsigned long cb)
{
	(void)vol;
	return fx_play((fx_sample *)ptr, pitchoffset, left, right, priority, cb, 0);
}

int FX_PlayLoopedVOC(char *ptr, long loopstart, long loopend, int pitchoffset, int vol, int left, int right,
                     int priority, unsigned long cb)
{
	(void)vol; (void)loopstart; (void)loopend;
	return fx_play((fx_sample *)ptr, pitchoffset, left, right, priority, cb, 1);
}

int FX_PlayWAV(char *ptr, int pitchoffset, int vol, int left, int right, int priority, unsigned long cb)
{
	return FX_PlayVOC(ptr, pitchoffset, vol, left, right, priority, cb);
}

int FX_PlayLoopedWAV(char *ptr, long loopstart, long loopend, int pitchoffset, int vol, int left, int right,
                     int priority, unsigned long cb)
{
	return FX_PlayLoopedVOC(ptr, loopstart, loopend, pitchoffset, vol, left, right, priority, cb);
}

static void pan3d(int angle, int distance, int *left, int *right)
{
	if (distance < 0) {
		distance = -distance;
		angle += NUMPANPOS / 2;
	}
	int volume = MIX_VOLUME(distance);
	angle &= MAXPANPOS;
	*left = pan_left[angle][volume];
	*right = pan_right[angle][volume];
}

int FX_PlayVOC3D(char *ptr, int pitchoffset, int angle, int distance, int priority, unsigned long cb)
{
	int l, r;
	pan3d(angle, distance, &l, &r);
	return fx_play((fx_sample *)ptr, pitchoffset, l, r, priority, cb, 0);
}

int FX_PlayWAV3D(char *ptr, int pitchoffset, int angle, int distance, int priority, unsigned long cb)
{
	return FX_PlayVOC3D(ptr, pitchoffset, angle, distance, priority, cb);
}

int FX_Pan3D(int handle, int angle, int distance)
{
	fxvoice *v = voice_of(handle);
	if (!v) return FX_Warning;
	int l, r;
	pan3d(angle, distance, &l, &r);
	v->l = (u8)l;
	v->r = (u8)r;
	apply_volume(v - V);
	return FX_Ok;
}

int FX_SetPan(int handle, int vol, int left, int right)
{
	(void)vol;
	fxvoice *v = voice_of(handle);
	if (!v) return FX_Warning;
	v->l = (u8)(left < 0 ? 0 : (left > 255 ? 255 : left));
	v->r = (u8)(right < 0 ? 0 : (right > 255 ? 255 : right));
	apply_volume(v - V);
	return FX_Ok;
}

int FX_SetPitch(int handle, int pitchoffset)
{
	fxvoice *v = voice_of(handle);
	if (!v) return FX_Warning;
	SPU_CH_FREQ(SPU_FIRSTFX + (v - V)) = (u16)spu_pitch_reg(v->s->rate, spu_pitch_scale(pitchoffset));
	return FX_Ok;
}

int FX_SetFrequency(int handle, int frequency)
{
	fxvoice *v = voice_of(handle);
	if (!v) return FX_Warning;
	SPU_CH_FREQ(SPU_FIRSTFX + (v - V)) = (u16)spu_pitch_reg(frequency, 0x10000);
	return FX_Ok;
}

int FX_EndLooping(int handle)
{
	fxvoice *v = voice_of(handle);
	if (!v) return FX_Warning;
	if (v->looped) {
		v->looped = 0;
		spu_key_off(1u << (SPU_FIRSTFX + (v - V)));
		v->end = fx_clock() + 4;
	}
	return FX_Ok;
}

int FX_SoundActive(int handle) { return voice_of(handle) != 0; }

int FX_SoundsPlaying(void)
{
	int n = 0;
	for (int i = 0; i < FX_MAXVOICES; i++) n += V[i].active;
	return n;
}

int FX_StopSound(int handle)
{
	fxvoice *v = voice_of(handle);
	if (!v) return FX_Warning;
	voice_stop(v - V, 1);
	return FX_Ok;
}

int FX_StopAllSounds(void)
{
	for (int i = 0; i < FX_MAXVOICES; i++) voice_stop(i, 1);
	return FX_Ok;
}

/* main-thread service: report voices that played to their end */
void fx_update(void)
{
	long now = fx_clock();
	for (int i = 0; i < FX_MAXVOICES; i++) {
		fxvoice *v = &V[i];
		if (v->active && !v->looped && now - v->end >= 0) voice_stop(i, 1);
	}
}

void FX_SetVolume(int volume)
{
	if (volume < 0) volume = 0;
	if (volume > 255) volume = 255;
	fx_volume = volume;
	for (int i = 0; i < FX_MAXVOICES; i++)
		if (V[i].active) apply_volume(i);
}

int FX_GetVolume(void) { return fx_volume; }

void FX_SetReverseStereo(int setting)
{
	fx_reverse = setting != 0;
	for (int i = 0; i < FX_MAXVOICES; i++)
		if (V[i].active) apply_volume(i);
}

int FX_GetReverseStereo(void) { return fx_reverse; }

void FX_SetReverb(int reverb)
{
	if (reverb < 0) reverb = 0;
	if (reverb > 255) reverb = 255;
	if (!fx_installed) { fx_reverb = reverb; return; }
	fx_reverb = reverb;
	u32 evol = ((u32)reverb * 0x2800) / 255;
	SPU_REVERB_VOL_L = (u16)evol;
	SPU_REVERB_VOL_R = (u16)evol;
	if (reverb) {
		SPU_CTRL |= 0x80;
		spu_reverb_voices(FX_VOICEMASK);
	} else {
		spu_reverb_voices(0);
		SPU_CTRL &= (u16)~0x80;
	}
}

void FX_SetFastReverb(int reverb) { FX_SetReverb(reverb); }
int FX_GetMaxReverbDelay(void) { return 256; }
int FX_GetReverbDelay(void) { return fx_reverb_delay; }
void FX_SetReverbDelay(int delay) { fx_reverb_delay = delay; }

int FX_SetCallBack(void (*function)(unsigned long))
{
	fx_callback = function;
	return FX_Ok;
}

char *FX_ErrorString(int ErrorNumber)
{
	switch (ErrorNumber) {
	case FX_Warning: return "FX warning (voice not available or sample not loadable).";
	case FX_Error: return "FX error.";
	case FX_Ok: return "Fx ok.";
	case FX_ASSVersion: return "Apogee Sound System Version incorrect.";
	case FX_BlasterError: return "Blaster error.";
	case FX_SoundCardError: return "SPU error.";
	case FX_InvalidCard: return "Invalid Card.";
	case FX_MultiVocError: return "Multivoc error.";
	case FX_DPMI_Error: return "DPMI error.";
	}
	return "Unknown FX error code.";
}

int FX_SetupCard(int SoundCard, fx_device *device)
{
	(void)SoundCard;
	device->MaxVoices = FX_MAXVOICES;
	device->MaxSampleBits = 16;
	device->MaxChannels = 2;
	return FX_Ok;
}

int FX_GetBlasterSettings(fx_blaster_config *blaster)
{
	memset(blaster, 0, sizeof(*blaster));
	return FX_Ok;
}

int FX_SetupSoundBlaster(fx_blaster_config blaster, int *MaxVoices, int *MaxSampleBits, int *MaxChannels)
{
	(void)blaster;
	*MaxVoices = FX_MAXVOICES;
	*MaxSampleBits = 16;
	*MaxChannels = 2;
	return FX_Ok;
}

int FX_Init(int SoundCard, int numvoices, int numchannels, int samplebits, unsigned mixrate)
{
	(void)SoundCard; (void)numvoices; (void)numchannels; (void)samplebits; (void)mixrate;
	spu_hw_init();
	calc_pan_table();
	memset(V, 0, sizeof(V));
	memset(slots, 0, sizeof(slots));
	fx_installed = 1;
	FX_SetReverb(0);
	return FX_Ok;
}

int FX_Shutdown(void)
{
	if (!fx_installed) return FX_Ok;
	FX_StopAllSounds();
	FX_SetReverb(0);
	fx_installed = 0;
	return FX_Ok;
}
