/* Memory card (slot 1, "bu00:") save files over the BIOS file API.
 *
 * One save = one file named "BASLUS-00000DUKE<slot>" (slot 0..9, alternate copy suffixed "B") of 1..15 blocks of 8192 bytes.
 * The file starts with the standard 128 byte PS1 save header (magic "SC", Shift-JIS title, icon
 * palette) followed by one 128 byte icon frame; mc_* hides these 256 bytes, callers only see the
 * payload that follows.  Payload I/O is byte oriented (staged through a small buffer and
 * transferred in 128 byte card sectors, as the BIOS requires). */
#pragma once

#define MC_SLOTS      10
#define MC_BLOCK      8192
#define MC_HDR_BYTES  256      /* header frame + icon frame, written/skipped by mc_* */
#define MC_MAX_BLOCKS 15

/* Initialise the BIOS card driver (idempotent).  Must be called after pad_init(). */
void mc_init(void);

/* Card state (probes the card; takes a few vsyncs). */
enum { MC_OK, MC_NOCARD, MC_UNFORMATTED, MC_ERROR };
int mc_status(void);

/* Files present: bit i = "DUKE<i>" exists, bit 16+i = alternate copy "DUKE<i>B" exists.
   0 when there is no usable card. */
unsigned mc_scan(void);

/* Free blocks on the card (counting every file, not only ours). */
int mc_free_blocks(void);

/* Erase file (slot, alt); 0 or -1. */
int mc_erase(int slot, int alt);

/* Payload capacity in bytes of a file of 'nblocks' blocks. */
#define MC_CAPACITY(nblocks) ((nblocks)*MC_BLOCK - MC_HDR_BYTES)

/* Open slot for reading.  Returns 0 or -1.  The first payload byte is the next one read. */
int mc_read_open(int slot, int alt);
/* Read exactly 'len' payload bytes sequentially; returns len, or -1 on a card error. */
int mc_read(void *dst, int len);
void mc_read_close(void);

/* Create file (slot, alt) (replacing an existing file of that name) with 'nblocks' blocks; 'title' is ASCII (shown in the
   memory card manager).  Returns 0 or -1 (no card / card full / write error). */
int mc_write_open(int slot, int alt, int nblocks, const char *title);
/* Append payload bytes; returns 0 or -1 on error. */
int mc_write(const void *src, int len);
/* Pad the last card sector and close.  Returns 0 or -1 if any write failed. */
int mc_write_close(void);
