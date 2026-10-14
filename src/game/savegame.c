/*
 * PS1 save games on the memory card.
 *
 * FILE (one per slot, via src/ps1/memcard.c, 1..15 blocks of 8 KB):
 *   0x000  PS1 save header (title, icon palette)      } written by memcard.c
 *   0x080  icon frame                                 }
 *   0x100  128 byte meta block (struct meta below: magic "DK3D", format/engine versions, payload
 *          length + checksum, CON script checksum, struct layout checksum, name, episode, level,
 *          skill, player count)
 *   0x180  payload: the game state as a stream of "residual" bytes, zero-run-length coded
 *
 * PAYLOAD CODING.  Every field is written as (value XOR prediction); the stream of residual bytes
 * is then coded with tokens (control byte c): 0x00..0x7f = (c>>4) zero bytes then ((c&15)+1) literal
 * bytes; 0x80..0xbf = (c&63)+1 zero bytes; 0xc0..0xff = (c&63)+1 literal bytes.  Predictions:
 *   - sectors / walls / sprites: the pristine record of the level's .MAP file (re-read from the CD
 *     through kopen4load; walls and sectors are almost all unchanged during play, so they cost a
 *     few bytes); the sprite owner field is predicted as the sprite's own index (what spawn() does);
 *   - weaponhit (hittype) records: the values spawn() would have given them (struct padding byte
 *     is not stored);
 *   - sprite list links: "inserted in index order" (what loadboard() produces);
 *   - everything else is coded against 0.
 * Only what is in use is saved: numsectors/numwalls records, used sprites (statnum != MAXSTATUS),
 * numcyclers/numanimwalls/animatecnt/spriteqamount entries, connected players.  Absolute pointers
 * (hittype T2/T5/T6 into script[], ps.palette, animateptr into sector[]) are stored as offsets/indices.
 * script[] and actorscrptr[] are not saved (the CON script is compiled at startup and never changes
 * at run time); a checksum of script[] (pointers made relative) in the meta block rejects saves from
 * another CON build.  The thumbnail of the DOS save format is not stored.
 *
 * SIZE.  Saving is two passes over the same encoder: pass 1 only counts bytes/checksum to size the file
 * (the BIOS needs the block count when the file is created), pass 2 writes.  Loading first reads the
 * whole file once to verify the checksum (so a corrupt card cannot half-load a game), then decodes.
 * Stack use is ~4.5 KB (list prediction tables), static ~0.4 KB.
 */

#pragma GCC optimize ("Os") /* PS1: cold code, keep the executable small */
#include "duke3d.h"
#include "kfile.h"
#include "memcard.h"
#include "savegame.h"
#include "loadscreen.h"

#define SG_MAGIC "DK3D"
#define SG_FMT   1
#define META_BYTES 128

/* PS1: saving screen: percentage at the end of each pass of sg_write() (see loadscreen.c); measured
   (vsyncs): count pass 251, card write 581, read back 287 */
#define LS_SAVE_COUNT 22
#define LS_SAVE_WRITE 73

const char *sg_error = "";

typedef struct
{
    char magic[4];
    unsigned short fmt, bytever;
    unsigned long len, sum, script_sum, layout_sum;
    unsigned long flags;                 /* bit0: no map baseline (user map) */
    sg_info info;
    unsigned long seq;                   /* save generation: the higher of the two copies of a slot wins */
    unsigned char pad[META_BYTES-56];
} meta;

_Static_assert(sizeof(meta) == META_BYTES, "meta size");
_Static_assert(sizeof(sectortype) == 40 && sizeof(walltype) == 32 && sizeof(spritetype) == 44, "map record sizes");

/* ---- residual stream codec ------------------------------------------------------------------ */

enum { M_COUNT, M_WRITE, M_READ };

static struct
{
    int mode, err;
    unsigned long len, sum;
    int nlit, zrun;                      /* encoder */
    unsigned char lit[64];
    int dzero, dlit, ip, il;             /* decoder */
    long inleft;
    unsigned char buf[128];              /* card write staging / card read staging */
    int bn;
} S;

#ifdef PS1_SAVETEST
static unsigned long marks[16];
static const char *marknames[16];
static int nmarks;
#define MARK(n) do { if(S.mode == M_COUNT && nmarks < 16) { marknames[nmarks] = n; marks[nmarks++] = S.len + S.nlit + (S.zrun?1:0); } } while(0)
#else
#define MARK(n)
#endif

/* PS1: loading / saving screen (loadscreen.c): progress of the current stage = (pg_off + position in the
   pass) / pg_tot; pg_len is the payload length of the pass being decoded.  No-ops without a loading screen. */
static unsigned long pg_off, pg_tot, pg_len;

static void obflush(void)
{
    if(S.bn)
    {
        if(mc_write(S.buf,S.bn) < 0) S.err = 1;
        S.bn = 0;
        loadscreen_progress(S.len,pg_tot);
    }
}

static void emit(unsigned char b)
{
    S.len++;
    S.sum = (S.sum ^ b) * 16777619u;
    if(S.mode == M_WRITE)
    {
        S.buf[S.bn++] = b;
        if(S.bn == (int)sizeof(S.buf)) obflush();
    }
}

/* Token coding of the residual stream (control byte c):
     0x00..0x7f   (c>>4) zero bytes, then ((c&15)+1) literal bytes
     0x80..0xbf   (c&63)+1 zero bytes
     0xc0..0xff   (c&63)+1 literal bytes */
static void flush_tok(void)
{
    int i;
    while(S.zrun > 64) { emit(0x80|63); S.zrun -= 64; }
    if(S.nlit == 0)
    {
        if(S.zrun) emit(0x80|(S.zrun-1));
        S.zrun = 0;
        return;
    }
    if(S.zrun <= 7 && S.nlit <= 16) emit((S.zrun<<4)|(S.nlit-1));
    else
    {
        if(S.zrun) emit(0x80|(S.zrun-1));
        emit(0xc0|(S.nlit-1));
    }
    for(i=0;i<S.nlit;i++) emit(S.lit[i]);
    S.nlit = 0;
    S.zrun = 0;
}

static void enc(unsigned char b)
{
    if(b == 0)
    {
        if(S.nlit) flush_tok();
        S.zrun++;
        return;
    }
    if(S.nlit == 64) flush_tok();
    S.lit[S.nlit++] = b;
}

static void enc_end(void)
{
    flush_tok();
    obflush();
}

static unsigned char getb(void)
{
    if(S.ip == S.il)
    {
        int n = S.inleft > 128 ? 128 : (int)S.inleft;
        if(n <= 0 || mc_read(S.buf,n) < 0) { S.err = 1; return 0; }
        S.inleft -= n;
        S.il = n;
        S.ip = 0;
        loadscreen_progress(pg_off+pg_len-S.inleft,pg_tot);
    }
    return S.buf[S.ip++];
}

static unsigned char dec(void)
{
    for(;;)
    {
        unsigned char c;
        if(S.dzero) { S.dzero--; return 0; }
        if(S.dlit) { S.dlit--; return getb(); }
        c = getb();
        if(S.err) return 0;
        if(c < 0x80) { S.dzero = c >> 4; S.dlit = (c & 15) + 1; }
        else if(c < 0xc0) S.dzero = (c & 63) + 1;
        else S.dlit = (c & 63) + 1;
    }
}

static void codec_reset(int mode)
{
    memset(&S,0,sizeof(S));
    S.mode = mode;
    S.sum = 2166136261u;
}

/* sync n bytes of *p with the stream, coded against base[] (NULL = zeros) */
static void sx(void *p, const void *base, int n)
{
    unsigned char *d = p;
    const unsigned char *b = base;
    int i;

    if(S.mode == M_READ)
    {
        for(i=0;i<n;i++) d[i] = dec() ^ (b ? b[i] : 0);
    }
    else
    {
        for(i=0;i<n;i++) enc(d[i] ^ (b ? b[i] : 0));
    }
}

#define SYNC(v) sx(&(v),0,sizeof(v))

/* sync an nb byte little endian value coded against 'base' */
static unsigned xv(unsigned v, unsigned base, int nb)
{
    unsigned r = 0;
    int k;
    for(k=0;k<nb;k++)
    {
        unsigned b = (base >> (8*k)) & 255;
        if(S.mode == M_READ) r |= (unsigned)((dec() ^ b) & 255) << (8*k);
        else enc(((v >> (8*k)) & 255) ^ b);
    }
    return S.mode == M_READ ? r : v;
}

#define XS(v,base) ((short)xv((unsigned short)(v),(unsigned short)(base),2))

/* ---- map baseline ---------------------------------------------------------------------------- */

static long bl_h = -1;
static long bl_ns, bl_nw, bl_nspr;
enum { BL_SEC, BL_WALL, BL_SPR };

static int bl_name(char *nm, int vol, int lvl)
{
    char *p;
    if(vol < 0 || lvl < 0 || vol*11+lvl >= 44) return 0;
    strncpy(nm,level_file_names[vol*11+lvl],127);
    nm[127] = 0;
    p = strrchr(nm,'.');
    if(p) *p = 0;
    return nm[0] != 0;
}

static void bl_open(int vol, int lvl, int disabled)
{
    char nm[128];
    short n;
    long ver;

    bl_h = -1;
    bl_ns = bl_nw = bl_nspr = 0;
    if(disabled || !bl_name(nm,vol,lvl)) return;
    bl_h = kopen4load(nm,0);
#ifdef PS1_SAVETEST
    dbg("baseline %s -> handle %ld\n",nm,bl_h);
#endif
    if(bl_h < 0) return;
    ver = 0;
    kread(bl_h,&ver,4);
    if(ver != 7) goto bad;
    klseek(bl_h,20,0);
    if(kread(bl_h,&n,2) != 2) goto bad;
    bl_ns = n;
    klseek(bl_h,22+40*bl_ns,0);
    if(kread(bl_h,&n,2) != 2) goto bad;
    bl_nw = n;
    klseek(bl_h,24+40*bl_ns+32*bl_nw,0);
    if(kread(bl_h,&n,2) != 2) goto bad;
    bl_nspr = n;
#ifdef PS1_SAVETEST
    dbg("baseline: %ld sectors %ld walls %ld sprites\n",bl_ns,bl_nw,bl_nspr);
#endif
    return;
bad:
    kclose(bl_h);
    bl_h = -1;
    bl_ns = bl_nw = bl_nspr = 0;
}

static void bl_close(void)
{
    if(bl_h >= 0) kclose(bl_h);
    bl_h = -1;
}

static const void *bl_rec(int kind, long idx, void *tmp)
{
    long off, sz;
    if(bl_h < 0) return 0;
    switch(kind)
    {
        case BL_SEC:  if(idx >= bl_ns) return 0; sz = 40; off = 22+40*idx; break;
        case BL_WALL: if(idx >= bl_nw) return 0; sz = 32; off = 24+40*bl_ns+32*idx; break;
        default:      if(idx >= bl_nspr) return 0; sz = 44; off = 26+40*bl_ns+32*bl_nw+44*idx; break;
    }
    klseek(bl_h,off,0);
    if(kread(bl_h,tmp,sz) != sz)
    {
        S.err = 1;      /* PS1: an unreadable baseline record would silently be coded/decoded as zeros */
        return 0;
    }
    return tmp;
}

/* ---- state ------------------------------------------------------------------------------------ */

static char *palette_table(int i)
{
    switch(i)
    {
        case 1: return waterpal;
        case 2: return slimepal;
        case 3: return titlepal;
        case 4: return drealms;
        case 5: return endingpal;
    }
    return palette;
}

static int palette_index(const char *p)
{
    int i;
    for(i=1;i<6;i++) if(p == palette_table(i)) return i;
    return 0;
}

static void predict_hit(struct weaponhit *pr, const spritetype *sp)
{
    memset(pr,0,sizeof(*pr));
    pr->picnum = sp->picnum;
    pr->ang = sp->ang;
    pr->extra = -1;
    pr->owner = sp->owner;
    pr->actorstayput = -1;
    pr->floorz = sector[sp->sectnum].floorz;
    pr->ceilingz = sector[sp->sectnum].ceilingz;
    pr->bposx = sp->x;
    pr->bposy = sp->y;
    pr->bposz = sp->z;
}

static void xfer_state(void)
{
    long i, j, n;
    int rd = (S.mode == M_READ);
    unsigned char tmp[44] __attribute__((aligned(4)));
    short ls_sect[MAXSECTORS+1], ls_stat[MAXSTATUS+1];
    long sbase = (long)&script[0], send = (long)&script[MAXSCRIPTSIZE];
    short np = numplayers;

    MARK("world");
    SYNC(numsectors);
    SYNC(numwalls);
    if(rd && (numsectors < 0 || numsectors > MAXSECTORS || numwalls < 0 || numwalls > MAXWALLS))
    {
        S.err = 1;
        numsectors = numwalls = 0;
        return;
    }
    for(i=0;i<numsectors;i++) sx(&sector[i],bl_rec(BL_SEC,i,tmp),sizeof(sectortype));
    MARK("sectors");
    for(i=0;i<numwalls;i++) sx(&wall[i],bl_rec(BL_WALL,i,tmp),sizeof(walltype));
    MARK("walls");

    for(i=0;i<=MAXSECTORS;i++) ls_sect[i] = -1;
    for(i=0;i<=MAXSTATUS;i++) ls_stat[i] = -1;
    if(rd)
    {
        for(i=0;i<=MAXSECTORS;i++) headspritesect[i] = -1;
        for(i=0;i<=MAXSTATUS;i++) headspritestat[i] = -1;
    }

    for(i=0;i<MAXSPRITES;i++)
    {
        spritetype *sp = &sprite[i];
        int used = xv(rd ? 0 : sp->statnum != MAXSTATUS, i < bl_nspr, 1);
        short pn, pt;

        if(used)
        {
            struct weaponhit pr;
            unsigned fl = 0;
            const void *bs;

            bs = bl_rec(BL_SPR,i,tmp);
            if(!bs) { memset(tmp,0,sizeof(tmp)); bs = tmp; }
            ((spritetype *)tmp)->owner = i;      /* spawn() makes most sprites their own owner */
            sx(sp,bs,sizeof(spritetype));
            if(rd && (sp->sectnum < 0 || sp->sectnum >= MAXSECTORS || sp->statnum < 0 || sp->statnum >= MAXSTATUS))
            {
                S.err = 1;
                sp->sectnum = sp->statnum = 0;
            }

            if(!rd && actorscrptr[sp->picnum])
            {
                if(hittype[i].temp_data[1] >= sbase && hittype[i].temp_data[1] < send) fl |= 1;
                if(hittype[i].temp_data[4] >= sbase && hittype[i].temp_data[4] < send) fl |= 2;
                if(hittype[i].temp_data[5] >= sbase && hittype[i].temp_data[5] < send) fl |= 4;
            }
            fl = xv(fl,0,1);
            if(!rd)
            {
                if(fl&1) hittype[i].temp_data[1] -= sbase;
                if(fl&2) hittype[i].temp_data[4] -= sbase;
                if(fl&4) hittype[i].temp_data[5] -= sbase;
            }
            predict_hit(&pr,sp);
            sx(&hittype[i],&pr,1);
            sx((char *)&hittype[i]+2,(char *)&pr+2,sizeof(struct weaponhit)-2);   /* byte 1 is struct padding */
            if(rd) ((char *)&hittype[i])[1] = 0;
            if(fl&1) hittype[i].temp_data[1] += sbase;
            if(fl&2) hittype[i].temp_data[4] += sbase;
            if(fl&4) hittype[i].temp_data[5] += sbase;

            pn = XS(nextspritesect[i],ls_sect[sp->sectnum]);
            pt = XS(nextspritestat[i],ls_stat[sp->statnum]);
            ls_sect[sp->sectnum] = i;
            ls_stat[sp->statnum] = i;
            if(rd) { nextspritesect[i] = pn; nextspritestat[i] = pt; }
        }
        else
        {
            short pred = i+1 < MAXSPRITES ? i+1 : -1;
            if(rd) { sp->sectnum = MAXSECTORS; sp->statnum = MAXSTATUS; }
            pn = XS(nextspritesect[i],pred);
            pt = XS(nextspritestat[i],pred);
            if(rd) { nextspritesect[i] = pn; nextspritestat[i] = pt; }
        }
    }
    MARK("sprites");

    for(j=0;j<numsectors;j++) headspritesect[j] = XS(headspritesect[j],ls_sect[j]);
    headspritesect[MAXSECTORS] = XS(headspritesect[MAXSECTORS],0);
    for(j=0;j<MAXSTATUS;j++) headspritestat[j] = XS(headspritestat[j],ls_stat[j]);
    headspritestat[MAXSTATUS] = XS(headspritestat[MAXSTATUS],0);

    if(rd)
    {
        short p, k;
        for(j=0;j<=MAXSECTORS;j++)
            for(p=-1,k=headspritesect[j],n=0;k >= 0 && k < MAXSPRITES && n <= MAXSPRITES;k=nextspritesect[k],n++)
            {
                prevspritesect[k] = p;
                p = k;
            }
        for(j=0;j<=MAXSTATUS;j++)
            for(p=-1,k=headspritestat[j],n=0;k >= 0 && k < MAXSPRITES && n <= MAXSPRITES;k=nextspritestat[k],n++)
            {
                prevspritestat[k] = p;
                p = k;
            }
    }
    MARK("lists");

    SYNC(numcyclers);
    if(rd && (numcyclers < 0 || numcyclers > MAXCYCLERS)) { S.err = 1; numcyclers = 0; }
    sx(&cyclers[0][0],0,numcyclers*sizeof(cyclers[0]));

    for(j=0;j<np;j++)
    {
        struct player_struct *p = &ps[j];
        char *pal = p->palette;
        if(!rd) p->palette = (char *)palette_index(pal);
        sx(p,0,sizeof(*p));
        p->palette = rd ? palette_table((long)p->palette) : pal;
        sx(&po[j],0,sizeof(po[j]));
    }
    MARK("players");

    SYNC(numanimwalls);
    if(rd && (numanimwalls < 0 || numanimwalls > MAXANIMWALLS)) { S.err = 1; numanimwalls = 0; }
    sx(&animwall[0],0,numanimwalls*sizeof(animwall[0]));

    n = 0;
    if(!rd)
    {
        for(j=sizeof(msx)/sizeof(long)-1;j >= 0 && !msx[j] && !msy[j];j--);
        n = j+1;
    }
    n = xv(n,0,2);
    if(n > (long)(sizeof(msx)/sizeof(long))) { S.err = 1; n = 0; }
    if(rd) { memset(msx,0,sizeof(msx)); memset(msy,0,sizeof(msy)); }
    sx(&msx[0],0,n*sizeof(long));
    sx(&msy[0],0,n*sizeof(long));

    SYNC(spriteqloc);
    SYNC(spriteqamount);
    if(rd && (spriteqamount < 0 || spriteqamount > (short)(sizeof(spriteq)/sizeof(short)))) { S.err = 1; spriteqamount = 0; }
    sx(&spriteq[0],0,spriteqamount*sizeof(short));
    SYNC(mirrorcnt);
    SYNC(mirrorwall);
    SYNC(mirrorsector);
    sx(&show2dsector[0],0,MAXSECTORS>>3);
    SYNC(boardfilename);
    SYNC(numclouds);
    SYNC(clouds);
    SYNC(cloudx);
    SYNC(cloudy);
    SYNC(lockclock);
    SYNC(pskybits);
    SYNC(pskyoff);

    SYNC(animatecnt);
    if(rd && (animatecnt < 0 || animatecnt > MAXANIMATES)) { S.err = 1; animatecnt = 0; }
    sx(&animatesect[0],0,animatecnt*sizeof(animatesect[0]));
    for(i=0;i<animatecnt;i++)
    {
        unsigned off = xv(rd ? 0 : (long)animateptr[i]-(long)&sector[0],0,4);
        if(rd) animateptr[i] = (long *)((long)&sector[0]+(long)off);
    }
    sx(&animategoal[0],0,animatecnt*sizeof(animategoal[0]));
    sx(&animatevel[0],0,animatecnt*sizeof(animatevel[0]));
    MARK("misc");

    SYNC(earthquaketime);
    SYNC(ud.from_bonus);
    SYNC(ud.secretlevel);
    SYNC(ud.respawn_monsters);
    SYNC(ud.respawn_items);
    SYNC(ud.respawn_inventory);
    SYNC(ud.god);
    SYNC(ud.auto_run);
    SYNC(ud.crosshair);
    SYNC(ud.monsters_off);
    SYNC(ud.last_level);
    SYNC(ud.eog);
    SYNC(ud.coop);
    SYNC(ud.marker);
    SYNC(ud.ffire);
    SYNC(camsprite);
    SYNC(connecthead);
    SYNC(connectpoint2);
    SYNC(numplayersprites);
    SYNC(frags);
    SYNC(randomseed);
    SYNC(global_random);
    SYNC(parallaxyscale);
    MARK("end");
}

/* ---- checksums / meta ------------------------------------------------------------------------ */

static unsigned long script_sum(void)
{
    unsigned long h = 2166136261u, v;
    unsigned long base = (unsigned long)&script[0], end = (unsigned long)&script[MAXSCRIPTSIZE];
    int i;
    for(i=0;i<MAXSCRIPTSIZE;i++)
    {
        v = script[i];
        if(v >= base && v < end) v -= base;
        h = (h ^ v) * 16777619u;
    }
    return h;
}

static unsigned long layout_sum(void)
{
    unsigned long h = SG_FMT;
    h = h*31 + sizeof(struct player_struct);
    h = h*31 + sizeof(struct player_orig);
    h = h*31 + sizeof(struct weaponhit);
    h = h*31 + sizeof(ud);
    h = h*31 + (MAXSPRITES^(MAXWALLS<<3)^(MAXSECTORS<<7));
    return h;
}

static int meta_valid(const meta *m)
{
    return memcmp(m->magic,SG_MAGIC,4) == 0 && m->fmt == SG_FMT;
}

/* ---- public API ------------------------------------------------------------------------------ */

/* Each slot lives in one of two physical files ("A" = DUKE<n>, "B" = DUKE<n>B); a save is written to
   the other one, verified, and only then is the old one erased.  cur_alt1[slot]: 0 none, 1 = A, 2 = B. */
static unsigned char cur_alt1[SG_SLOTS];
static unsigned long cur_seq[SG_SLOTS];
static int card_state = MC_OK;

const char *sg_warning = "";

static const char *status_msg(int st)
{
    switch(st)
    {
        case MC_NOCARD:      return "NO MEMORY CARD IN SLOT 1";
        case MC_UNFORMATTED: return "MEMORY CARD NOT FORMATTED";
        case MC_ERROR:       return "MEMORY CARD ERROR";
    }
    return 0;
}

const char *sg_card_message(void)
{
    return status_msg(card_state);
}

/* open file (spot,alt) and read its meta block: 0 with the file left open, -1 (closed) if absent/invalid */
static int open_meta(int spot, int alt, meta *m)
{
    if(mc_read_open(spot,alt) < 0) return -1;
    if(mc_read(m,sizeof(*m)) == (int)sizeof(*m) && meta_valid(m)) return 0;
    mc_read_close();
    return -1;
}

/* read the payload of the open file and check its checksum: 0 ok, -1 card error, -2 checksum mismatch */
static int verify_payload(const meta *m)
{
    unsigned long sum = 2166136261u;
    long n;
    unsigned char c[128];

    for(n=m->len;n > 0;)
    {
        int k = n > 128 ? 128 : (int)n, i;
        if(mc_read(c,k) < 0) return -1;
        for(i=0;i<k;i++) sum = (sum ^ c[i]) * 16777619u;
        n -= k;
        loadscreen_progress(pg_off+m->len-n,pg_tot);
    }
    return sum == m->sum ? 0 : -2;
}

int sg_peek(int spot, sg_info *info)
{
    meta m;
    if(spot < 0 || spot >= SG_SLOTS || !cur_alt1[spot]) return -1;
    if(open_meta(spot,cur_alt1[spot]-1,&m) < 0) return -1;
    mc_read_close();
    *info = m.info;
    info->name[19] = 0;
    return 0;
}

void sg_readnames(void)
{
    unsigned mask = 0;
    int i;

    card_state = mc_status();
    if(card_state != MC_NOCARD && card_state != MC_UNFORMATTED) mask = mc_scan();
#ifdef PS1_SAVETEST
    dbg("SAVECARD status %d (%s) files %08x\n",card_state,status_msg(card_state) ? status_msg(card_state) : "ok",mask);
#endif
    for(i=0;i<SG_SLOTS;i++)
    {
        meta m[2];
        int ok[2], alt, cur = -1;

        ud.savegame[i][0] = 0;
        cur_alt1[i] = 0;
        for(alt=0;alt<2;alt++)
        {
            ok[alt] = (mask & (1u << (alt*16+i))) && open_meta(i,alt,&m[alt]) == 0;
            if(ok[alt]) mc_read_close();
        }
        if(ok[0] && ok[1])
        {
            /* both copies exist: an interrupted save.  Prefer the newer one if it verifies. */
            int hi = m[0].seq >= m[1].seq ? 0 : 1;
            int good = 0;
            if(open_meta(i,hi,&m[hi]) == 0)
            {
                good = verify_payload(&m[hi]) == 0;
                mc_read_close();
            }
            cur = good ? hi : 1-hi;
            mc_erase(i,1-cur);
        }
        else if(ok[0]) cur = 0;
        else if(ok[1]) cur = 1;
        if(cur >= 0)
        {
            cur_alt1[i] = cur+1;
            cur_seq[i] = m[cur].seq;
            memcpy(ud.savegame[i],m[cur].info.name,19);
            ud.savegame[i][19] = 0;
        }
#ifdef PS1_SAVETEST
        dbg("SAVENAME slot %d: '%s' (copy %d)\n",i,ud.savegame[i],cur);
#endif
    }
}

static int user_map(void)
{
    return ud.volume_number == 0 && ud.level_number == 7 && boardfilename[0] != 0;
}

int sg_write(int spot)
{
    meta m, v;
    unsigned long len1;
    int nblocks, cur, target, safe = 0, free_blocks, st, ok;
    char title[40];

    sg_error = "SAVE ERROR";
    sg_warning = "";
    card_state = st = mc_status();
    if(st == MC_NOCARD || st == MC_UNFORMATTED) { sg_error = status_msg(st); return -1; }   /* fail before the slow state coding */

    cur = (int)cur_alt1[spot]-1;
    memset(&m,0,sizeof(m));
    memcpy(m.magic,SG_MAGIC,4);
    m.fmt = SG_FMT;
    m.bytever = BYTEVERSION;
    m.script_sum = script_sum();
    m.layout_sum = layout_sum();
    m.flags = user_map() ? 1 : 0;
    m.seq = cur >= 0 ? cur_seq[spot]+1 : 1;
    memcpy(m.info.name,ud.savegame[spot],19);
    m.info.vol = ud.volume_number;
    m.info.lvl = ud.level_number;
    m.info.skill = ud.player_skill;
    m.info.nump = numplayers;

    bl_open(ud.volume_number,ud.level_number,m.flags&1);

    loadscreen_stage(LS_SAVE_COUNT,0);   /* the saving screen: pass 1 (count), pass 2 (card write), read back */
    codec_reset(M_COUNT);
#ifdef PS1_SAVETEST
    nmarks = 0;
#endif
    xfer_state();
    enc_end();
    len1 = S.len;
    m.len = S.len;
    m.sum = S.sum;
#ifdef PS1_SAVETEST
    {
        int k;
        unsigned long prev = 0;
        for(k=0;k<nmarks;k++) { dbg("SAVE section %s: %lu bytes\n",marknames[k],marks[k]-prev); prev = marks[k]; }
        { int u = 0, q; for(q=0;q<MAXSPRITES;q++) if(sprite[q].statnum != MAXSTATUS) u++; dbg("SAVE used sprites %d\n",u); }
    }
#endif

    nblocks = (MC_HDR_BYTES + META_BYTES + (int)len1 + MC_BLOCK-1) / MC_BLOCK;
    if(nblocks > MC_MAX_BLOCKS) { bl_close(); sg_error = "SAVE TOO LARGE FOR A MEMORY CARD"; return -1; }

    free_blocks = mc_free_blocks();
    if(cur >= 0)
    {
        if(free_blocks >= nblocks) { safe = 1; target = 1-cur; }
        else if(free_blocks + mc_file_blocks(spot,cur) >= nblocks)
            target = cur;            /* no room for a second copy: replace in place */
        else
        {
            bl_close();
            sg_error = "MEMORY CARD FULL";   /* keep the old save rather than erase it for nothing */
            return -1;
        }
    }
    else
    {
        target = 0;
        if(free_blocks >= 0 && free_blocks < nblocks)
        {
            bl_close();
            sg_error = "MEMORY CARD FULL";
            return -1;
        }
    }

    sprintf(title,"DUKE NUKEM 3D E%dL%d",ud.volume_number+1,ud.level_number+1);
    if(mc_write_open(spot,target,nblocks,title) < 0)
    {
        bl_close();
        sg_error = status_msg(st) ? status_msg(st) : "CANNOT CREATE SAVE ON MEMORY CARD";
        return -1;
    }
    mc_write(&m,sizeof(m));
    loadscreen_stage(LS_SAVE_WRITE,0);
    pg_tot = len1;
    codec_reset(M_WRITE);
    xfer_state();
    enc_end();
    bl_close();
    ok = mc_write_close() == 0 && !S.err && S.len == len1 && S.sum == m.sum;
    if(!ok) sg_error = "MEMORY CARD WRITE ERROR";

    /* read the new file back (meta + payload checksum) before the old copy may go */
    if(ok)
    {
        loadscreen_stage(100,0);
        pg_off = 0; pg_tot = m.len;
        ok = open_meta(spot,target,&v) == 0;
        if(ok)
        {
            ok = v.seq == m.seq && v.len == m.len && v.sum == m.sum && verify_payload(&v) == 0;
            mc_read_close();
        }
        if(!ok) sg_error = "SAVE VERIFY FAILED";
    }
    if(!ok)
    {
        mc_erase(spot,target);       /* with a safe save the old copy is still intact and stays current */
        if(!safe) cur_alt1[spot] = 0;
        return -1;
    }

#ifdef PS1_SAVETEST_KEEPOLD
    if(safe) dbg("SAVETEST: KEEPOLD, old copy %d not erased\n",cur);   /* simulates a crash before the erase */
#else
    if(safe) mc_erase(spot,cur);
#endif
    else if(cur >= 0) sg_warning = "GAME SAVED - CARD FULL, NO BACKUP COPY KEPT";
    cur_alt1[spot] = target+1;
    cur_seq[spot] = m.seq;
#ifdef PS1_SAVETEST
    dbg("SAVE slot %d: payload %lu bytes, file %d block(s) (%d bytes), checksum %08lx, copy %d seq %lu%s\n",
        spot,len1,nblocks,nblocks*MC_BLOCK,m.sum,target,m.seq,safe ? " (safe replace)" : "");
#endif
    return 0;
}

int sg_read(int spot)
{
    meta m;
    int st, rc;

    sg_error = "LOAD ERROR";
    if(spot < 0 || spot >= SG_SLOTS) return -1;
    card_state = st = mc_status();
    if(st == MC_NOCARD || st == MC_UNFORMATTED) { sg_error = status_msg(st); return -1; }
    if(!cur_alt1[spot]) { sg_error = status_msg(st) ? status_msg(st) : "NO SAVED GAME IN THIS SLOT"; return -1; }
    if(open_meta(spot,cur_alt1[spot]-1,&m) < 0) { sg_error = status_msg(st) ? status_msg(st) : "SAVE FILE UNREADABLE"; return -1; }
    if(m.bytever != BYTEVERSION || m.script_sum != script_sum() || m.layout_sum != layout_sum())
    {
        mc_read_close();
        return 1;
    }
    if(m.info.nump != numplayers) { mc_read_close(); return 2; }

    /* pass 1: verify the payload checksum without touching the game state */
    pg_off = 0; pg_tot = 2*m.len;       /* the loading screen bar covers both passes */
    rc = verify_payload(&m);
    mc_read_close();
    if(rc == -1) { sg_error = "CARD READ ERROR"; return -1; }
    if(rc == -2) { sg_error = "SAVE FILE CORRUPT"; return -1; }

    /* pass 2: decode */
    if(open_meta(spot,cur_alt1[spot]-1,&m) < 0) return -1;
    pg_off = pg_len = m.len;
    /* PS1: ud.volume/level/skill and the slot name are committed only after a successful decode */
    /* the player list (connecthead/connectpoint2) is part of the decoded stream; a failed decode leaves
       garbage there (e.g. connectpoint2[0] = 0 loops enterlevel forever), so keep the live values */
    {
        short keep_head = connecthead, keep_p2[MAXPLAYERS];
        memcpy(keep_p2,connectpoint2,sizeof(keep_p2));
        bl_open(m.info.vol,m.info.lvl,m.flags&1);
        codec_reset(M_READ);
        S.inleft = m.len;
        xfer_state();       /* decodes straight into the live game arrays */
        bl_close();
        mc_read_close();
        if(S.err || S.inleft != 0 || S.ip != S.il || S.dzero || S.dlit)
        {
            connecthead = keep_head;
            memcpy(connectpoint2,keep_p2,sizeof(keep_p2));
            /* the world / players were already overwritten: tell the caller (SG_DESTROYED), which must not
               continue with this half-restored state (loadplayer re-enters the current level) */
            sg_error = "SAVE FILE CORRUPT";
            return SG_DESTROYED;
        }
    }
    memcpy(ud.savegame[spot],m.info.name,19);
    ud.savegame[spot][19] = 0;
    ud.volume_number = m.info.vol;
    ud.level_number = m.info.lvl;
    ud.player_skill = m.info.skill;
#ifdef PS1_SAVETEST
    dbg("LOAD slot %d: payload %lu bytes ok\n",spot,m.len);
#endif
    return 0;
}

/* ---- PS1_SAVETEST: automatic save -> modify -> load round trip --------------------------------- */
#ifdef PS1_SAVETEST

#ifndef PS1_SAVETEST_SECS
#define PS1_SAVETEST_SECS PS1_SAVETEST
#endif

static unsigned long fnv(unsigned long h, const void *p, long n)
{
    const unsigned char *b = p;
    while(n-- > 0) h = (h ^ *b++) * 16777619u;
    return h;
}

static void state_sums(unsigned long *out)
{
    long i, j;
    unsigned long h;

    out[0] = fnv(2166136261u,&numwalls,2);
    out[0] = fnv(out[0],&wall[0],numwalls*sizeof(walltype));
    out[1] = fnv(2166136261u,&numsectors,2);
    out[1] = fnv(out[1],&sector[0],numsectors*sizeof(sectortype));
    h = 2166136261u;
    for(i=0;i<MAXSPRITES;i++)
        if(sprite[i].statnum != MAXSTATUS)
        {
            h = fnv(h,&i,4);
            h = fnv(h,&sprite[i],sizeof(spritetype));
            {
                struct weaponhit t = hittype[i];     /* script pointers made relative: comparable between builds */
                long b = (long)&script[0], k;
                for(k=0;k<6;k++)
                    if(t.temp_data[k] >= b && t.temp_data[k] < (long)&script[MAXSCRIPTSIZE]) t.temp_data[k] -= b;
                h = fnv(h,&t,1);
                h = fnv(h,(char *)&t+2,sizeof(t)-2);
            }
            h = fnv(h,&prevspritesect[i],2);
            h = fnv(h,&nextspritesect[i],2);
            h = fnv(h,&prevspritestat[i],2);
            h = fnv(h,&nextspritestat[i],2);
        }
    for(i=0;i<=MAXSECTORS;i++) h = fnv(h,&headspritesect[i],2);
    for(i=0;i<=MAXSTATUS;i++) h = fnv(h,&headspritestat[i],2);
    out[2] = h;
    h = 2166136261u;
    for(j=0;j<numplayers;j++)
    {
        struct player_struct t = ps[j];
        t.palette = (char *)palette_index(t.palette);
        h = fnv(h,&t,sizeof(t));
        h = fnv(h,&po[j],sizeof(po[j]));
    }
    out[3] = h;
    h = 2166136261u;
    h = fnv(h,&cyclers[0][0],numcyclers*sizeof(cyclers[0]));
    h = fnv(h,&animwall[0],numanimwalls*sizeof(animwall[0]));
    h = fnv(h,&msx[0],sizeof(msx));
    h = fnv(h,&msy[0],sizeof(msy));
    h = fnv(h,&spriteq[0],spriteqamount*sizeof(short));
    h = fnv(h,&animatesect[0],animatecnt*sizeof(animatesect[0]));
    for(j=0;j<animatecnt;j++) { long d = (long)animateptr[j]-(long)&sector[0]; h = fnv(h,&d,4); }
    h = fnv(h,&animategoal[0],animatecnt*sizeof(animategoal[0]));
    h = fnv(h,&animatevel[0],animatecnt*sizeof(animatevel[0]));
    h = fnv(h,&frags[0][0],sizeof(frags));
    h = fnv(h,&randomseed,sizeof(randomseed));
    h = fnv(h,&ud.from_bonus,sizeof(ud.from_bonus));
    out[4] = h;
}

static void print_sums(const char *tag, const unsigned long *s)
{
    dbg("SAVETEST %s: walls=%08lx sectors=%08lx sprites=%08lx players=%08lx misc=%08lx pos=(%ld,%ld,%ld) ang=%d sect=%d\n",
        tag,s[0],s[1],s[2],s[3],s[4],ps[myconnectindex].posx,ps[myconnectindex].posy,ps[myconnectindex].posz,
        ps[myconnectindex].ang,ps[myconnectindex].cursectnum);
}

static void modify_world(int k)
{
    int i, n;
    ps[myconnectindex].posx += 6000*k;
    ps[myconnectindex].posy -= 6000*k;
    ps[myconnectindex].ang = (ps[myconnectindex].ang + 700*k) & 2047;
    ps[myconnectindex].horiz += 40;
    wall[0].x += 512*k;
    wall[3].picnum += 1;
    sector[0].floorz += 2048*k;
    sector[1].ceilingpicnum += 1;
    msx[5] = 12345*k;
    for(i=0,n=0;i<MAXSPRITES && n<6*k;i++)
        if(sprite[i].statnum == 0) { deletesprite(i); n++; }
    for(n=0;n<4;n++) insertsprite(0,5);
    numanimwalls++;
}

/* Save slot 0, modify the world, save again (safe replace: second copy, then the first is erased),
   modify again, then load: the loaded state must equal the state of the second save. */
void sg_savetest_tick(void)
{
    static int phase;                    /* 0 wait, 1 first save done, 2 second save done, 3 finished/busy */
    static unsigned long A[5], B[5], C[5], D[5];
    static long t_mod;
    int i, k;

    if(phase >= 3 || (ps[myconnectindex].gm & MODE_GAME) != MODE_GAME || (ps[myconnectindex].gm & MODE_MENU)) return;

#ifdef PS1_SAVETEST_LOADONLY
    if(phase == 0)
    {
        if(totalclock < PS1_SAVETEST_SECS*120L) return;
        phase = 3;
        state_sums(A);
        print_sums("fresh game (before load)",A);
        i = loadplayer(0);
        state_sums(C);
        print_sums("after load from card",C);
        dbg("SAVETEST: loadplayer returned %d (%s)\n",i,i ? sg_error : "ok");
        return;
    }
#endif
    if(phase == 0)
    {
        if(totalclock < PS1_SAVETEST_SECS*120L) return;
        phase = 3;                       /* re-entrancy guard (getpackets is called inside save/load) */
        state_sums(A);
        print_sums("before save 1",A);
        strcpy(ud.savegame[0],"SAVETEST");
        {
            extern int video_vsync_count(void);
            int t0 = video_vsync_count();
            i = saveplayer(0);
            dbg("SAVETEST: saveplayer(0) = %d took %d vsyncs (ended at vsync %d)\n",i,video_vsync_count()-t0,video_vsync_count());
        }
        if(i != 0) { dbg("SAVETEST: saveplayer failed: %s\n",sg_error); return; }
        dbg("SAVETEST: saved (1) warning '%s'\n",sg_warning);
        modify_world(1);
        t_mod = totalclock;
        phase = 1;
        return;
    }
    if(phase == 1)
    {
        if(totalclock < t_mod + 2*120) return;
        phase = 3;
        state_sums(B);
        print_sums("before save 2",B);
        i = saveplayer(0);
        if(i != 0) { dbg("SAVETEST: second saveplayer failed: %s\n",sg_error); return; }
        dbg("SAVETEST: saved (2) warning '%s'\n",sg_warning);
        modify_world(2);
        state_sums(C);
        print_sums("after modify 2",C);
        t_mod = totalclock;
        phase = 2;
        return;
    }
    if(phase == 2)
    {
        if(totalclock < t_mod + 4*120) return;
        phase = 3;
#ifndef PS1_SAVETEST_KEEPOLD
        sg_readnames();                  /* as after a reboot: which copy is current? */
#endif
        i = loadplayer(0);
        state_sums(D);
        print_sums("after load",D);
        dbg("SAVETEST: loadplayer returned %d\n",i);
        for(k=0;k<5;k++) if(B[k] != D[k]) break;
        dbg("SAVETEST RESULT: %s (load == state of save 2; differs from save 1 state: %s, from modify 2 state: %s)\n",
            k == 5 ? "ROUNDTRIP OK" : "ROUNDTRIP MISMATCH",
            (A[0]!=D[0]||A[1]!=D[1]||A[2]!=D[2]||A[3]!=D[3]||A[4]!=D[4]) ? "yes" : "NO",
            (C[0]!=D[0]||C[1]!=D[1]||C[2]!=D[2]||C[3]!=D[3]||C[4]!=D[4]) ? "yes" : "NO");
    }
}

#endif
