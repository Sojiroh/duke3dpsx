/* Private header shared by spu_fx.c and spu_music.c (SPU hardware layer of the audio port). */
#pragma once
#include "psx.h"
#include <psxspu.h>

/* ---- SPU RAM map (byte addresses) ---------------------------------------------------------
   0x00000-0x00FFF  hardware capture buffers (reserved)
   0x01000-0x0100F  dummy loop block uploaded by SpuInit()
   0x01040-0x29FFF  music instrument bank (MUSBANK ADPCM data, <= 164,288 bytes)
   0x2A000-0x7D93F  sound effect sample cache (LRU, variable sized slots, 64-byte aligned)
   0x7D940-0x7FFFF  reverb work area ("Room" preset, 0x26C0 bytes) */
#define SPU_MUSIC_BASE   0x01040u
#define SPU_MUSIC_LIMIT  0x2A000u
#define SPU_FX_BASE      0x2A000u
#define SPU_FX_LIMIT     0x7D940u
#define SPU_REVERB_BASE  0x7D940u
#define SPU_REVERB_SIZE  0x26C0u

/* Voice split: music voices 0..SPU_NMUSIC-1, effects SPU_NMUSIC..23 */
#define SPU_NMUSIC       10
#define SPU_FIRSTFX      SPU_NMUSIC
#define SPU_NFX          (24 - SPU_NMUSIC)

#define SPU_STAGING_BYTES 12288          /* shared staging buffer (6 CD sectors) */

void spu_hw_init(void);                  /* idempotent: SpuInit, master volume, reverb setup */
u8  *spu_staging(void);                  /* SPU_STAGING_BYTES, 4-byte aligned (not re-entrant) */
/* DMA `bytes` (multiple of 64) from main RAM (4-byte aligned) to SPU RAM `addr` (multiple of 8). */
void spu_dma_write(u32 addr, const void *src, u32 bytes);
/* Copy `bytes` of SPU-ADPCM from DUKE.DAT (sector `sector`, byte 0 of the entry) to SPU `addr`,
   through the staging buffer; `bytes` is rounded up to 64 for the DMA (the caller reserves that).
   loop != 0: set the loop flags (first block 0x04, last audio block = size/16-2 ... 0x03). */
int  spu_load_adpcm(u32 sector, u32 bytes, u32 addr, int loop);

/* 2^(cents/1200) as 16.16 fixed point (audiolib PITCH_GetScale). */
u32 spu_pitch_scale(int cents);
/* SPU pitch register for a sample of `rate` Hz played at 16.16 ratio `scale` (clamped 1..0x3FFF). */
u32 spu_pitch_reg(u32 rate, u32 scale);

#define SPU_ADSR1_FLAT 0x00FF            /* instant attack, no decay, full sustain */
#define SPU_ADSR2_FLAT 0x0000            /* sustain at full level, instant release */

static inline void spu_key_on(u32 mask)  { SPU_KEY_ON1 = (u16)mask;  SPU_KEY_ON2 = (u16)(mask >> 16); }
static inline void spu_key_off(u32 mask) { SPU_KEY_OFF1 = (u16)mask; SPU_KEY_OFF2 = (u16)(mask >> 16); }

/* Reverb enable bits (EON) for a voice mask. */
void spu_reverb_voices(u32 mask);        /* sets EON1/EON2 */

void fx_debug(void);                     /* print cache / voice state with dbg() */
void music_debug(void);                  /* print sequencer / music voice state with dbg() */
