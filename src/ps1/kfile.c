#include <ctype.h>
#include "psx.h"
#include "kfile.h"

#define MAXHANDLES 4

typedef struct {
	const PakEntry *e;
	long pos;
	long cached;      /* sector index currently in buf, -1 if none */
	u32 buf[2048 / 4];
} KFile;

static KFile files[MAXHANDLES];

long initgroupfile(const char *filename) { (void)filename; return 0; }
void uninitgroupfile(void) {}

long kopen4load(const char *filename, char searchfirst)
{
	char name[16];
	int i, n = 0;
	const char *p = filename;
	(void)searchfirst;
	/* strip any path, upper-case, 8.3 */
	for (const char *q = filename; *q; q++)
		if (*q == '/' || *q == '\\') p = q + 1;
	while (*p && n < 15) name[n++] = toupper((unsigned char)*p++);
	name[n] = 0;
	const PakEntry *e = pak_find(name);
	if (!e) return -1;
	for (i = 0; i < MAXHANDLES; i++)
		if (!files[i].e) {
			files[i].e = e;
			files[i].pos = 0;
			files[i].cached = -1;
			return i;
		}
	dbg("kopen4load: out of handles (%s)\n", name);
	return -1;
}

long kread(long handle, void *buffer, long leng)
{
	KFile *f;
	u8 *out = buffer;
	long total = 0;
	if ((unsigned)handle >= MAXHANDLES || !files[handle].e) return 0;
	f = &files[handle];
	if (f->pos + leng > (long)f->e->size) leng = f->e->size - f->pos;
	while (leng > 0) {
		long sec = f->pos / 2048, in = f->pos % 2048;
		long n = 2048 - in;
		if (n > leng) n = leng;
		if (in == 0 && leng >= 2048 && ((uintptr_t)out & 3) == 0) {
			/* bulk read of whole sectors straight into the destination */
			long ns = leng / 2048;
			if (ns > 32) ns = 32;
			if (pak_read(f->e->sector + sec, ns, out) < 0) break;
			n = ns * 2048;
		} else {
			if (f->cached != sec) {
				if (pak_read(f->e->sector + sec, 1, f->buf) < 0) break;
				f->cached = sec;
			}
			memcpy(out, (u8 *)f->buf + in, n);
		}
		out += n; f->pos += n; leng -= n; total += n;
	}
	return total;
}

long klseek(long handle, long offset, long whence)
{
	KFile *f;
	if ((unsigned)handle >= MAXHANDLES || !files[handle].e) return -1;
	f = &files[handle];
	switch (whence) {
	case 0: f->pos = offset; break;          /* SEEK_SET */
	case 1: f->pos += offset; break;         /* SEEK_CUR */
	case 2: f->pos = f->e->size + offset; break; /* SEEK_END */
	}
	if (f->pos < 0) f->pos = 0;
	if (f->pos > (long)f->e->size) f->pos = f->e->size;
	return f->pos;
}

long kfilelength(long handle)
{
	if ((unsigned)handle >= MAXHANDLES || !files[handle].e) return -1;
	return files[handle].e->size;
}

void kclose(long handle)
{
	if ((unsigned)handle >= MAXHANDLES) return;
	files[handle].e = NULL;
}

/* closes every open handle (game restart after gameexit()'s longjmp) */
void kcloseall(void)
{
	for (int i = 0; i < MAXHANDLES; i++) files[i].e = NULL;
}
