// C replacements for Ken Silverman's PRAGMAS.H (Watcom inline assembly).
// 64-bit intermediates map to MIPS mult/mflo/mfhi; divscale/scale use a
// 32-bit division fast path when the shifted numerator fits.
#pragma once
#include <string.h>

#ifndef min
#define min(a,b) (((a)<(b))?(a):(b))
#endif
#ifndef max
#define max(a,b) (((a)>(b))?(a):(b))
#endif

extern long dmval;
extern unsigned long slowdiv_count, slowdiv_site[4];

static inline long klabs(long a) { return a < 0 ? -a : a; }
static inline long sqr(long a) { return a*a; }

/* 64/32 -> 32 unsigned division (u1:u0)/v with u1 < v (Hacker's Delight divlu):
   two hardware divisions instead of libgcc's generic __udivmoddi4. */
static inline unsigned long divlu(unsigned long u1, unsigned long u0, unsigned long v)
{
	const unsigned long b = 65536;
	unsigned long un1, un0, vn1, vn0, q1, q0, un32, un21, un10, rhat;
	int s = __builtin_clz(v);
	v <<= s;
	vn1 = v >> 16; vn0 = v & 0xffff;
	un32 = (u1 << s) | ((u0 >> (32 - s)) & (-s >> 31));
	un10 = u0 << s;
	un1 = un10 >> 16; un0 = un10 & 0xffff;
	q1 = un32 / vn1; rhat = un32 - q1*vn1;
	while (q1 >= b || q1*vn0 > b*rhat + un1) { q1--; rhat += vn1; if (rhat >= b) break; }
	un21 = un32*b + un1 - q1*v;
	q0 = un21 / vn1; rhat = un21 - q0*vn1;
	while (q0 >= b || q0*vn0 > b*rhat + un0) { q0--; rhat += vn1; if (rhat >= b) break; }
	return q1*b + q0;
}

/* signed (hi:lo)/d truncating toward zero, quotient assumed to fit in 32 bits */
static inline long div64_32(long long n, long d)
{
	unsigned long nh, nl, dd, q;
	int neg = 0;
	if (n < 0) { n = -n; neg ^= 1; }
	if (d < 0) { d = -d; neg ^= 1; }
	nh = (unsigned long)((unsigned long long)n >> 32); nl = (unsigned long)n; dd = (unsigned long)d;
	if (nh == 0) q = nl / dd;
	else if (nh >= dd) q = 0x7fffffff;      /* overflow: saturate */
	else q = divlu(nh, nl, dd);
	return neg ? -(long)q : (long)q;
}

static inline long scale(long a, long b, long c)
{
	long long p = (long long)a*b;
	if (p >= -2147483647LL && p <= 2147483647LL) return (long)p / c;
	slowdiv_count++; slowdiv_site[0]++;
	return div64_32(p, c);
}

static inline long mulscale(long a, long b, long c) { return (long)(((long long)a*b)>>c); }
static inline long dmulscale(long a, long b, long c, long d, long e) { return (long)(((long long)a*b+(long long)c*d)>>e); }

static inline long divscale(long a, long b, long c)
{
	long long n = (long long)a<<c;
	if (n >= -2147483647LL && n <= 2147483647LL) return (long)n / b;
	slowdiv_count++; slowdiv_site[1]++;
	return div64_32(n, b);
}

/* scale() for screen-space projection: keeps 30 significant bits, relative error < 2^-13 */
static inline long scale_approx(long a, long b, long c)
{
	long long p = (long long)a*b;
	int s;
	if (p >= -2147483647LL && p <= 2147483647LL) return (long)p / c;
	{
		unsigned long long ap = (unsigned long long)(p < 0 ? -p : p);
		s = 64 - __builtin_clzll(ap) - 30;
	}
	if (klabs(c >> s) < 4096) { slowdiv_count++; slowdiv_site[0]++; return div64_32(p, c); }
	return (long)(p >> s) / (c >> s);
}

/* Approximate divscale for ratios known to be small (|a/b| <= 2^10): pre-shifts
   both operands so a single 32-bit division suffices; relative error < 2^-13. */
static inline long divscale_approx(long a, long b, long c)
{
	long long n = (long long)a<<c;
	int s;
	if (n >= -2147483647LL && n <= 2147483647LL) return (long)n / b;
	s = 32 - __builtin_clz((unsigned long)klabs(a)) - (30 - c);
	if (s < 0) s = 0;
	if (klabs(b >> s) < 4096) { slowdiv_count++; slowdiv_site[2]++; return div64_32(n, b); }
	return ((a >> s) << c) / (b >> s);
}

#define MULSCALE(n) static inline long mulscale##n(long a, long b) { return (long)(((long long)a*b)>>n); }
#define DMULSCALE(n) static inline long dmulscale##n(long a, long b, long c, long d) { return (long)(((long long)a*b+(long long)c*d)>>n); }
#define TMULSCALE(n) static inline long tmulscale##n(long a, long b, long c, long d, long e, long f) { return (long)(((long long)a*b+(long long)c*d+(long long)e*f)>>n); }
#define DIVSCALE(n) static inline long divscale##n(long a, long b) { return divscale(a,b,n); }

MULSCALE(1) MULSCALE(2) MULSCALE(3) MULSCALE(4) MULSCALE(5) MULSCALE(6) MULSCALE(7) MULSCALE(8)
MULSCALE(9) MULSCALE(10) MULSCALE(11) MULSCALE(12) MULSCALE(13) MULSCALE(14) MULSCALE(15) MULSCALE(16)
MULSCALE(17) MULSCALE(18) MULSCALE(19) MULSCALE(20) MULSCALE(21) MULSCALE(22) MULSCALE(23) MULSCALE(24)
MULSCALE(25) MULSCALE(26) MULSCALE(27) MULSCALE(28) MULSCALE(29) MULSCALE(30) MULSCALE(31) MULSCALE(32)
DMULSCALE(1) DMULSCALE(2) DMULSCALE(3) DMULSCALE(4) DMULSCALE(5) DMULSCALE(6) DMULSCALE(7) DMULSCALE(8)
DMULSCALE(9) DMULSCALE(10) DMULSCALE(11) DMULSCALE(12) DMULSCALE(13) DMULSCALE(14) DMULSCALE(15) DMULSCALE(16)
DMULSCALE(17) DMULSCALE(18) DMULSCALE(19) DMULSCALE(20) DMULSCALE(21) DMULSCALE(22) DMULSCALE(23) DMULSCALE(24)
DMULSCALE(25) DMULSCALE(26) DMULSCALE(27) DMULSCALE(28) DMULSCALE(29) DMULSCALE(30) DMULSCALE(31) DMULSCALE(32)
TMULSCALE(1) TMULSCALE(2) TMULSCALE(3) TMULSCALE(4) TMULSCALE(5) TMULSCALE(6) TMULSCALE(7) TMULSCALE(8)
TMULSCALE(9) TMULSCALE(10) TMULSCALE(11) TMULSCALE(12) TMULSCALE(13) TMULSCALE(14) TMULSCALE(15) TMULSCALE(16)
TMULSCALE(17) TMULSCALE(18) TMULSCALE(19) TMULSCALE(20) TMULSCALE(21) TMULSCALE(22) TMULSCALE(23) TMULSCALE(24)
TMULSCALE(25) TMULSCALE(26) TMULSCALE(27) TMULSCALE(28) TMULSCALE(29) TMULSCALE(30) TMULSCALE(31) TMULSCALE(32)
DIVSCALE(1) DIVSCALE(2) DIVSCALE(3) DIVSCALE(4) DIVSCALE(5) DIVSCALE(6) DIVSCALE(7) DIVSCALE(8)
DIVSCALE(9) DIVSCALE(10) DIVSCALE(11) DIVSCALE(12) DIVSCALE(13) DIVSCALE(14) DIVSCALE(15) DIVSCALE(16)
DIVSCALE(17) DIVSCALE(18) DIVSCALE(19) DIVSCALE(20) DIVSCALE(21) DIVSCALE(22) DIVSCALE(23) DIVSCALE(24)
DIVSCALE(25) DIVSCALE(26) DIVSCALE(27) DIVSCALE(28) DIVSCALE(29) DIVSCALE(30) DIVSCALE(31)
static inline long divscale32(long a, long b) { slowdiv_count++; slowdiv_site[3]++; return div64_32((long long)a<<32, b); }
#undef MULSCALE
#undef DMULSCALE
#undef TMULSCALE
#undef DIVSCALE

static inline long boundmulscale(long a, long b, long c)
{
	long long p = ((long long)a*b)>>c;
	if (p > 0x7fffffffLL) return 0x7fffffff;
	if (p < -0x7fffffffLL-1) return (long)0x80000000;
	return (long)p;
}

static inline long ksgn(long a) { return (a > 0) - (a < 0); }
static inline long umin(unsigned long a, unsigned long b) { return a < b ? a : b; }
static inline long umax(unsigned long a, unsigned long b) { return a > b ? a : b; }
static inline long kmin(long a, long b) { return a < b ? a : b; }
static inline long kmax(long a, long b) { return a > b ? a : b; }
static inline long mul3(long a) { return a*3; }
static inline long mul5(long a) { return a*5; }
static inline long mul9(long a) { return a*9; }
static inline long divmod(long a, long b) { dmval = a % b; return a / b; }
static inline long moddiv(long a, long b) { dmval = a / b; return a % b; }

static inline void swapchar(char *a, char *b) { char t = *a; *a = *b; *b = t; }
static inline void swapshort(short *a, short *b) { short t = *a; *a = *b; *b = t; }
static inline void swaplong(void *a, void *b) { long t = *(long *)a; *(long *)a = *(long *)b; *(long *)b = t; }
static inline void swapbuf4(void *a, void *b, long c) { long *x = a, *y = b; while (c-- > 0) { long t = *x; *x++ = *y; *y++ = t; } }
static inline void swap64bit(void *a, void *b) { swaplong(a, b); swaplong((long *)a+1, (long *)b+1); }
static inline void swapchar2(char *a, char *b, long s) { swapchar(a, b); swapchar(a+1, b+s); }

static inline void clearbuf(void *d, long c, long a) { long *p = d; while (c-- > 0) *p++ = a; }
static inline void clearbufbyte(void *d, long c, long a)
{
	unsigned char *p = d; unsigned long v = a;
	while (c-- > 0) { *p++ = (unsigned char)v; v = (v>>8)|(v<<24); }
}
static inline void copybuf(const void *s, void *d, long c) { const long *x = s; long *y = d; while (c-- > 0) *y++ = *x++; }
static inline void copybufbyte(const void *s, void *d, long c) { memcpy(d, s, c); }
static inline void copybufreverse(const void *s, void *d, long c)
{
	const unsigned char *x = s; unsigned char *y = d;
	while (c-- > 0) *y++ = *x--;
}

static inline void qinterpolatedown16(void *bufptr, long num, long val, long add)
{
	long *buf = bufptr;
	for (long i = 0; i < num; i++) { buf[i] = (val>>16); val += add; }
}
static inline void qinterpolatedown16short(void *bufptr, long num, long val, long add)
{
	short *buf = bufptr;
	for (long i = 0; i < num; i++) { buf[i] = (short)(val>>16); val += add; }
}
