/* Audio smoke test: void audio_selftest(void) (called from main.c by the parent when wanted).
   Loads a sound effect + the music bank, plays a sound and a song, and prints the SPU voice /
   sequencer state with dbg() (PCSX-Redux headless has no audio output).  Takes ~6 seconds. */
#include "spu_priv.h"
#include "fx_man.h"
#include "music.h"

#ifndef AUDIO_TEST_SECONDS
#define AUDIO_TEST_SECONDS 6
#endif

static volatile int cb_count;
static unsigned long cb_last;
static long cb_time[8];

static void test_cb(unsigned long v)
{
	if (cb_count < 8) cb_time[cb_count] = totalclock;
	cb_count++;
	cb_last = v;
}

/* read back `bytes` of SPU RAM at `addr` into the staging buffer */
static void spu_readback(u32 addr, u32 bytes)
{
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	SpuSetTransferStartAddr(addr);
	SpuRead((uint32_t *)spu_staging(), bytes);
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
}

static void wait_ticks(int ticks)
{
	long end = totalclock + ticks;
	while (totalclock - end < 0) {
		fx_update();
		music_update();
		VSync(0);
	}
}

void audio_selftest(void)
{
	dbg("audio_selftest: start (totalclock=%ld)\n", totalclock);

	int st = FX_Init(0, FX_MAXVOICES, 2, 16, 44100);
	dbg("audio_selftest: FX_Init=%d\n", st);
	FX_SetVolume(255);
	FX_SetCallBack(test_cb);

	const PakEntry *e = pak_find("DUKNUK14.VOC");
	if (!e) e = pak_find("PISTOL.VOC");
	if (!e) {
		dbg("audio_selftest: no sample found\n");
		return;
	}
	static fx_sample smp;
	smp.magic = FX_SAMPLE_MAGIC;
	smp.sector = e->sector;
	smp.size = e->size;
	smp.rate = (unsigned short)e->aux;
	smp.loop = 0;
	smp.slot = 0;
	unsigned t0 = prof_now();
	int ok = FX_LoadSample(&smp);
	dbg("audio_selftest: %s size=%u rate=%u loaded=%d in %u ms\n", e->name, (unsigned)e->size, (unsigned)smp.rate, ok,
	    (unsigned)PROF_MS(prof_now() - t0));

	/* compare the first 64 bytes of the uploaded ADPCM with the pack */
	if (ok) {
		static u32 ref[2048 / 4];
		pak_read(e->sector, 1, ref);
		spu_readback(0x2A000, 64);
		int same = memcmp(ref, spu_staging(), 64) == 0;
		dbg("audio_selftest: SPU RAM readback %s (first word %08x vs %08x)\n", same ? "OK" : "MISMATCH",
		    (unsigned)ref[0], (unsigned)((u32 *)spu_staging())[0]);
	}

	/* play it at the front, close; then to the right, far */
	long t_play = totalclock;
	int h1 = FX_PlayVOC3D((char *)&smp, 0, 0, 105, 200, 7);
	dbg("audio_selftest: FX_PlayVOC3D handle=%d\n", h1);
	fx_debug();
	int h2 = FX_PlayVOC3D((char *)&smp, 300, 8, 200, 100, 8);
	dbg("audio_selftest: second voice handle=%d active=%d\n", h2, FX_SoundsPlaying());
	FX_Pan3D(h2, 24, 100);
	fx_debug();

	/* music */
	st = MUSIC_Init(0, 0);
	dbg("audio_selftest: MUSIC_Init=%d\n", st);
	if (st == MUSIC_Ok) {
		MUSIC_SetVolume(200);
		st = MUSIC_PlaySong((unsigned char *)"STALKER.MID", MUSIC_LoopSong);
		dbg("audio_selftest: MUSIC_PlaySong=%d\n", st);
		for (int k = 0; k < AUDIO_TEST_SECONDS; k++) {
			wait_ticks(120);
			if (k < 6 || (k % 10) == 0) music_debug();
		}
		MUSIC_Pause();
		dbg("audio_selftest: paused\n");
		music_debug();
		MUSIC_Continue();
		wait_ticks(60);
		music_debug();
		MUSIC_StopSong();
		dbg("audio_selftest: stopped, playing=%d\n", MUSIC_SongPlaying());
	}
	fx_debug();
	dbg("audio_selftest: callbacks=%d last=%lu active=%d\n", cb_count, cb_last, FX_SoundsPlaying());
	/* let the one-shot voices play out: callbacks must arrive from fx_update() */
	for (int k = 0; k < 90 && FX_SoundsPlaying(); k++) wait_ticks(12);
	dbg("audio_selftest: after wait: active=%d callbacks=%d (durations in ticks: %ld %ld, played at %ld)\n", FX_SoundsPlaying(), cb_count,
	    cb_time[0] - t_play, cb_time[1] - t_play, t_play);
	FX_StopAllSounds();
	dbg("audio_selftest: after StopAll callbacks=%d active=%d\n", cb_count, FX_SoundsPlaying());
	dbg("audio_selftest: done\n");
}
