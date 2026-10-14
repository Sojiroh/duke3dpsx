/* VRAM texture cache with CD streaming for Build tiles. */
#pragma once
#include "psx.h"

typedef struct {
	u16 tpage;      /* 8-bit CLUT page, abr 0 */
	u8 u, v;        /* texel offset of the piece inside the page */
	u16 w, h;       /* piece size in texels */
	u16 bw, bh;     /* allocated block size (pow2 >= 8); wrap window covers bw x bh */
	u32 twin;       /* texture window command word (0xE2......) for wrapping within the block */
	u16 xoff, yoff; /* piece origin within the tile (multi-piece tiles) */
	u16 picnum;     /* tile number */
	u16 tw, th;     /* whole tile size */
	u8 npieces;
	u8 half;        /* 1 = texture stored at half resolution: scale u,v by 1/2 */
} TexRef;

void tex_init(void);
int tex_lookup(int picnum, int piece, TexRef *out);   /* 1 = resident (marks LRU); 0 = not (queues a load) */
int tex_npieces(int picnum);
int tex_lookup_sync(int picnum, int piece, TexRef *out);  /* like tex_lookup but loads synchronously on a miss */
void tex_preload(int picnum);
void tex_preload_range(int first, int last, int lock);  /* single-piece tiles, batched CD reads, optionally locked resident */                         /* synchronous load of all pieces */
u32 tex_sector(int picnum);                             /* CD sector of the tile (preload ordering) */
int tex_vram_pct(void);                                /* % of cache VRAM in use */
void tex_frame(void);                                 /* call once per frame (LRU tick + streaming pump) */
void tex_flush(void);                                 /* evict everything */
void tex_lock(int picnum, int lock);                  /* keep resident (HUD etc.) */
extern int tex_stat_resident, tex_stat_misses, tex_stat_loads;
int tex_vram_used(void);                             /* resident block texels */
