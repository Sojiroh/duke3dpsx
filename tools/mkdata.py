#!/usr/bin/env python3
"""Build DUKE.DAT (sector-aligned asset pack for the PS1 port) from DUKE3D.GRP.

Usage: mkdata.py <DUKE3D.GRP> <out dir> [--sf2 FILE.sf2]
Writes <out>/DUKE.DAT and <out>/data_index.h.  Music instruments come from the SoundFont given with
--sf2 (explicit, never searched for); without it the built-in synthesised bank is used.

Pack layout (little-endian, every entry starts on a 2048-byte sector):
  sector 0..: directory  = "DK3DPAK1", u32 count, count * ENTRY (32 bytes:
              char name[16], u32 sector, u32 size, u32 aux)
  entries:    PALS      palette blob (see write_pals)
              TABLES    raw TABLES.DAT
              TILEIDX   per-tile index, MAXTILES * TILEREC (12 bytes):
                        u16 sizx, u16 sizy, u32 picanm, u16 piece0, u8 npieces, u8 flags
              PIECES    piece records (16 bytes): u16 tile, u8 w8?,... see TilePiece.pack
              TILEDATA  concatenated tile pixel data (row-major, even stride), sector aligned per piece
              <maps>    E1L1.MAP ... as-is
              GAME.CON etc. as-is
              <sounds>  (added by snd pipeline) ...
"""
import os
import re
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import grp as G  # noqa: E402
import spuadpcm  # noqa: E402

SECTOR = 2048
MAXTILES = 4096
MAXPIECE = 256  # max texture page dimension in texels


def align(n, a=SECTOR):
    return (n + a - 1) & ~(a - 1)


class Pack:
    def __init__(self):
        self.entries = []  # (name, bytes, aux)

    def add(self, name, data, aux=0):
        assert len(name) <= 15, name
        self.entries.append((name.upper(), bytes(data), aux))

    def write(self, path):
        dirsize = align(8 + 4 + 32 * len(self.entries))
        off = dirsize
        table = []
        blobs = []
        for name, data, aux in self.entries:
            table.append((name, off // SECTOR, len(data), aux))
            blobs.append(data)
            off += align(len(data))
        with open(path, "wb") as f:
            f.write(b"DK3DPAK1")
            f.write(struct.pack("<I", len(table)))
            for name, sec, size, aux in table:
                f.write(struct.pack("<16sIII", name.encode("ascii"), sec, size, aux))
            f.write(b"\0" * (dirsize - f.tell()))
            for data in blobs:
                f.write(data)
                f.write(b"\0" * (align(len(data)) - len(data)))
        return table


class TilePiece:
    """A rectangular piece of a tile (<= MAXPIECE on each side)."""

    FMT = "<HHBBHHHI"  # tile, pad, ..  see pack()
    SIZE = 16

    def __init__(self, tile, xoff, yoff, w, h, pixels):
        self.tile, self.xoff, self.yoff, self.w, self.h = tile, xoff, yoff, w, h
        self.pixels = pixels  # row-major bytes, stride multiple of 16, rows multiple of 4
        self.sector = 0

    @property
    def stride(self):
        return max(16, (self.w + 15) & ~15)

    def pack(self):
        # u16 tile, u16 xoff, u16 yoff, u16 w, u16 h, u16 pad, u32 sector
        return struct.pack("<HHHHHHI", self.tile, self.xoff, self.yoff, self.w, self.h, 0, self.sector)


def transpose_tile(t):
    """ART tiles are column-major; return row-major numpy array (h, w)."""
    a = np.frombuffer(t.pixels, dtype=np.uint8).reshape(t.sizx, t.sizy)  # [x][y]
    return a.T  # [y][x]


def avg_color15(img, pal):
    """Average RGB of non-transparent pixels as a 15-bit PS1 color (STP set)."""
    px = img.reshape(-1)
    px = px[px != 255]
    if len(px) == 0:
        return 0x8000
    rgb = pal[px].astype(np.uint32)  # (n,3), 0..63 VGA scale
    r, g, b = (int(rgb[:, i].mean()) for i in range(3))
    return 0x8000 | ((b >> 1) << 10) | ((g >> 1) << 5) | (r >> 1)


def texture_tiles(g):
    """Tiles that only ever appear as wall/floor/ceiling textures in the shipped maps."""
    wallpics, sprpics = set(), set()
    for name in g.names(".MAP"):
        b = G.parse_map(g.read(name))
        for sec in b.sectors:
            wallpics.add(sec[6]); wallpics.add(sec[12])
        for w in b.walls:
            wallpics.add(w[6]); wallpics.add(w[7])
        for sp in b.sprites:
            sprpics.add(sp[4])
    return wallpics - sprpics


def nearest_index_table(pal):
    """5-bit RGB cube -> nearest palette index (index 255 excluded)."""
    p = pal[:255].astype(np.int32)
    r, gg, b = np.meshgrid(np.arange(32), np.arange(32), np.arange(32), indexing="ij")
    cube = np.stack([r, gg, b], -1).reshape(-1, 3) * 2 + 1  # 0..63 scale
    best = np.zeros(len(cube), dtype=np.uint8)
    for i in range(0, len(cube), 4096):
        c = cube[i : i + 4096]
        d = ((c[:, None, :] - p[None, :, :]) ** 2).sum(-1)
        best[i : i + 4096] = d.argmin(1)
    return best.reshape(32, 32, 32)


def downscale_half(img, pal, nearest):
    """2x2 box filter in RGB, re-quantised to the palette. Index 255 stays transparent."""
    h, w = img.shape
    h2, w2 = h // 2, w // 2
    a = img[: h2 * 2, : w2 * 2].reshape(h2, 2, w2, 2).transpose(0, 2, 1, 3).reshape(h2, w2, 4)
    rgb = pal[a].astype(np.int32)  # (h2,w2,4,3) 0..63
    trans = a == 255
    cnt = (~trans).sum(-1)
    rgbsum = (rgb * (~trans)[..., None]).sum(2)
    mean = rgbsum // np.maximum(cnt, 1)[..., None]
    idx = nearest[mean[..., 0] >> 1, mean[..., 1] >> 1, mean[..., 2] >> 1]
    idx[cnt < 2] = 255
    return idx.astype(np.uint8)


def build_tiles(tiles, pal, halfres=()):
    """Return (tileidx bytes, pieces list, tiledata bytes).
    TILEREC (16 bytes): u16 sizx, u16 sizy, s32 picanm, u16 sector (piece 0, relative to TILEDATA),
                        u16 avgcolor15, u16 npieces | flags<<8, u16 aux.
    flags bit 0: a half-resolution copy exists; aux is then its sector (the runtime loads it instead of
    the full one under VRAM pressure). For multi-piece tiles (any dimension > MAXPIECE) aux is the
    first index into PIECES."""
    pieces = []
    recs = bytearray()
    data = bytearray()
    tilepix = {}  # PS1: single-piece tile -> (full-res pixels, half-res pixels or None), for the level groups
    nearest = nearest_index_table(pal) if halfres else None
    nhalf = 0

    def add_piece(n, px, py, sub):
        h, w = sub.shape
        # PSn00bSDK LoadImage DMAs in 16-word (64-byte) chunks, so every upload must be a
        # multiple of 64 bytes: stride is a multiple of 16 texels (min 16) and the row count a
        # multiple of 4 (texcache.c piece_stride/piece_rows mirror this).
        stride = max(16, (w + 15) & ~15)
        rows = (h + 3) & ~3
        buf = np.full((rows, stride), 255, dtype=np.uint8)
        buf[:h, :w] = sub
        p = TilePiece(n, px, py, w, h, buf.tobytes())
        p.sector = len(data) // SECTOR
        assert p.sector < 65536
        data.extend(p.pixels)
        data.extend(b"\0" * (align(len(p.pixels)) - len(p.pixels)))
        return p

    for n in range(MAXTILES):
        t = tiles.get(n)
        if t is None or t.sizx <= 0 or t.sizy <= 0:
            recs += struct.pack("<HHiHHHH", 0, 0, t.picanm if t else 0, 0, 0, 0, 0)
            continue
        img = transpose_tile(t)
        tile_pieces = []
        for py in range(0, t.sizy, MAXPIECE):
            for px in range(0, t.sizx, MAXPIECE):
                tile_pieces.append(add_piece(n, px, py, img[py : py + MAXPIECE, px : px + MAXPIECE]))
        flags, aux = 0, 0
        hpix = None
        if len(tile_pieces) > 1:
            aux = len(pieces)
            pieces += tile_pieces
        elif n in halfres and t.sizx >= 64 and t.sizy >= 64:
            hp = add_piece(n, 0, 0, downscale_half(img, pal, nearest))
            flags, aux = 1, hp.sector
            nhalf += 1
            hpix = hp.pixels
        if len(tile_pieces) == 1:
            tilepix[n] = (tile_pieces[0].pixels, hpix)
        recs += struct.pack("<HHiHHHH", t.sizx, t.sizy, t.picanm, tile_pieces[0].sector,
                            avg_color15(img, pal), len(tile_pieces) | (flags << 8), aux)
    print("tiles with a half-resolution copy: %d" % nhalf)
    return bytes(recs), pieces, data, tilepix


def load_names(path):
    """`#define NAME number` constants of the game's names.h (the group builder mirrors premap.c with them)."""
    d = {}
    for line in open(path):
        m = re.match(r"\s*#define\s+(\w+)\s+(-?\d+)\s*$", line)
        if m:
            d[m.group(1)] = int(m.group(2))
    return d


def sprite_extra_tiles(N, pn):
    """Tiles src/game/premap.c cachespritenum() would request for a sprite of picnum pn, beyond what
    the generic picnum/animation expansion covers (same table as upstream PREMAP.C cachespritenum)."""
    ex = set()
    maxc = 1
    if pn in (N["HYDRENT"], N["TOILET"], N["STALL"]):
        ex.add({N["HYDRENT"]: N["BROKEFIREHYDRENT"], N["TOILET"]: N["TOILETBROKE"], N["STALL"]: N["STALLBROKE"]}[pn])
        ex.update(range(N["TOILETWATER"], N["TOILETWATER"] + 4))
    elif pn == N["RUBBERCAN"]: maxc = 2
    elif pn == N["TOILETWATER"]: maxc = 4
    elif pn == N["FEMPIC1"]: maxc = 44
    elif pn in tuple(N[k] for k in ("LIZTROOP", "LIZTROOPRUNNING", "LIZTROOPSHOOT", "LIZTROOPJETPACK",
                                    "LIZTROOPONTOILET", "LIZTROOPDUCKING")):
        ex.update(range(N["LIZTROOP"], N["LIZTROOP"] + 72))
        ex.update(range(N["HEADJIB1"], N["LEGJIB1"] + 3))
        maxc = 0
    elif pn == N["WOODENHORSE"]:
        maxc = 5
        ex.update(range(N["HORSEONSIDE"], N["HORSEONSIDE"] + 4))
    elif pn in (N["NEWBEAST"], N["NEWBEASTSTAYPUT"]): maxc = 90
    elif pn in (N["BOSS1"], N["BOSS2"], N["BOSS3"]): maxc = 30
    elif pn in (N["OCTABRAIN"], N["OCTABRAINSTAYPUT"], N["COMMANDER"], N["COMMANDERSTAYPUT"]): maxc = 38
    elif pn == N["RECON"]: maxc = 13
    elif pn in (N["PIGCOP"], N["PIGCOPDIVE"]): maxc = 61
    elif pn == N["SHARK"]: maxc = 30
    elif pn in (N["LIZMAN"], N["LIZMANSPITTING"], N["LIZMANFEEDING"], N["LIZMANJUMP"]):
        ex.update(range(N["LIZMANHEAD1"], N["LIZMANLEG1"] + 3))
        maxc = 80
    elif pn == N["APLAYER"]: maxc = 0
    elif pn == N["ATOMICHEALTH"]: maxc = 14
    elif pn == N["DRONE"]: maxc = 10
    elif pn in (N["EXPLODINGBARREL"], N["SEENINE"], N["OOZFILTER"]): maxc = 3
    elif pn in (N["NUKEBARREL"], N["CAMERA1"]): maxc = 5
    ex.update(range(pn, pn + maxc))
    return ex


ALWAYS_RANK = -1      # HUD/font/weapon tiles cachegoodsprites() loads on every level
FAR_RANK = 1 << 20    # referenced only by sectors the portal graph cannot reach from the player start


def sector_hops(board, N):
    """Hop count from the player's start sector to every sector: one hop per wall nextsector link, plus one per
    teleporter jump (two sector effector sprites with lotag 7 and the same hitag; E1L1 starts in a closed room
    that only a teleporter leaves). Sectors the walk never reaches are absent."""
    ns = len(board.sectors)
    adj = {}
    tele = {}
    for sp in board.sprites:   # sprite: [4] picnum, [13] sectnum, [20] lotag, [21] hitag
        if sp[4] == N["SECTOREFFECTOR"] and sp[20] == 7 and 0 <= sp[13] < ns:
            tele.setdefault(sp[21], set()).add(sp[13])
    for sects in tele.values():
        for a in sects:
            adj.setdefault(a, set()).update(sects - {a})
    hop = {board.cursectnum: 0} if 0 <= board.cursectnum < ns else {}
    todo = list(hop)
    for s in todo:
        sec = board.sectors[s]
        for n in [w[4] for w in board.walls[sec[0] : sec[0] + sec[1]]] + sorted(adj.get(s, ())):
            if 0 <= n < ns and n not in hop:
                hop[n] = hop[s] + 1
                todo.append(n)
    return hop


def level_tile_ranks(board, tiles, N):
    """Single source of truth for a level's group: {tile: rank} for every tile the level can ask for (map
    picnums, their picanm animation ranges, cachespritenum() extras, tiles cachegoodsprites() loads each
    level). rank = fewest portal/teleporter hops from the player start to a sector using the tile (walls, overwalls,
    floor, ceiling, sprites); animation frames and cachespritenum() extras take the rank of their base tile;
    the always-needed tiles get ALWAYS_RANK, tiles only used by unreachable sectors FAR_RANK."""
    hop = sector_hops(board, N)
    base = {}

    def use(t, r):
        if r < base.get(t, FAR_RANK + 1):
            base[t] = r

    for si, s in enumerate(board.sectors):
        r = hop.get(si, FAR_RANK)
        use(s[6], r); use(s[12], r)
        if s[6] == N["LA"] or s[12] == N["LA"]:
            use(N["LA"] + 1, r); use(N["LA"] + 2, r)
        for w in board.walls[s[0] : s[0] + s[1]]:
            use(w[6], r); use(w[7], r)
    for sp in board.sprites:
        r = hop.get(sp[13], FAR_RANK)
        use(sp[4], r)
        for t in sprite_extra_tiles(N, sp[4]):
            use(t, r)
    for t in [N["BOTTOMSTATUSBAR"], *range(N["STARTALPHANUM"], N["ENDALPHANUM"] + 1),
              *range(N["FIRSTGUN"], N["FIRSTGUN"] + 3)]:
        base[t] = ALWAYS_RANK
    out = {}
    for t, r in base.items():
        out[t] = min(r, out.get(t, r))
        tl = tiles.get(t)
        if tl is None:
            continue
        frames, typ = tl.picanm & 63, (tl.picanm >> 6) & 3
        if frames:
            for f in (range(t - frames, t + 1) if typ == 3 else range(t, t + frames + 1)):
                out[f] = min(r, out.get(f, r))
    return out


def build_groups(g, tiles, tilepix, data, pack, N):
    """Append per-level tile groups to the TILEDATA blob `data` (a bytearray) and add a TILEMAP_<MAP> entry
    for each map: records (u16 tile, u16 sector, u16 halfsector), sorted by tile, sectors relative to TILEDATA
    (so texcache needs no change). Disc order: full-res pieces first, then the half-res copies, each ordered
    by level_tile_ranks() (always-needed tiles, then by portal distance from the player start, then
    unreachable ones; ties by tile number), so the preload and the streaming read the nearest tiles first.
    Multi-piece tiles stay global. Returns {map: (first sector, sector count)}."""
    groups = {}
    for name in g.names(".MAP"):
        board = G.parse_map(g.read(name))
        rank = level_tile_ranks(board, tiles, N)
        ts = sorted((t for t in rank if t in tilepix), key=lambda t: (rank[t], t))   # nearest first, ties by tile number
        first = len(data) // SECTOR
        sect, half = {}, {}
        for t in ts:
            sect[t] = len(data) // SECTOR
            data.extend(tilepix[t][0]); data.extend(b"\0" * (align(len(data)) - len(data)))
        for t in ts:
            if tilepix[t][1] is not None:
                half[t] = len(data) // SECTOR
                data.extend(tilepix[t][1]); data.extend(b"\0" * (align(len(data)) - len(data)))
        assert len(data) // SECTOR < 65536, "TILEDATA with groups exceeds u16 sectors"
        recs = b"".join(struct.pack("<HHH", t, sect[t], half.get(t, 0)) for t in sorted(ts))
        mname = name[:-4]
        pack.add("TILEMAP_" + mname, recs)
        groups[mname] = (first, len(data) // SECTOR - first)
    return groups


def write_pals(g):
    """PALS blob: u8 numlookups; 768 base palette; numlookups*(1 + 256) lookups (palnum byte then remap);
    then 5 * 768 extra base palettes (water, slime, title, drealms, ending); then 32 shade->brightness bytes."""
    palette = g.read("PALETTE.DAT")
    base = palette[:768]
    numpalookups = struct.unpack("<h", palette[768:770])[0]
    shade = palette[770 : 770 + numpalookups * 256]
    lookup = g.read("LOOKUP.DAT")
    numl = lookup[0]
    p = 1
    out = bytearray()
    out.append(numl)
    out += base
    for _ in range(numl):
        out.append(lookup[p])
        out += lookup[p + 1 : p + 257]
        p += 257
    for _ in range(5):
        out += lookup[p : p + 768]
        p += 768
    # shade -> brightness: mean luminance ratio of remapped colours vs originals
    pal = np.frombuffer(base, dtype=np.uint8).reshape(256, 3).astype(np.float64)
    lum = pal @ np.array([0.299, 0.587, 0.114])
    bright = []
    for s in range(32):
        if s < numpalookups:
            tbl = np.frombuffer(shade[s * 256 : (s + 1) * 256], dtype=np.uint8)
            mask = lum > 4
            ratio = (lum[tbl[mask]] / lum[mask]).mean()
        else:
            ratio = 0.0
        bright.append(max(0, min(128, int(round(ratio * 128)))))
    out += bytes(bright)
    return bytes(out), numl, bright


# ---------------------------------------------------------------------------------------------
# Sound / music (audio port).  Sound entries keep the original .VOC names; the data is
# SPU-ADPCM (tools/spuadpcm.py): (nblocks audio + 1 silent terminator block flagged 0x01), 16 bytes
# per block, aux = native sample rate in Hz.  The runtime (src/ps1/spu_fx.c) patches the loop flags
# in the staging buffer when a sound is played looped.  Music: tools/mkmusic.py (MUSBANK + songs).
# ---------------------------------------------------------------------------------------------
def voc_to_pcm(data):
    """Parse a Creative VOC file; return (rate, int16 mono numpy array)."""
    if not data.startswith(b"Creative Voice File"):
        raise ValueError("not a VOC file")
    p = struct.unpack("<H", data[20:22])[0]
    rate = None
    chunks = []
    bits = 8
    while p < len(data):
        t = data[p]
        if t == 0:
            break
        size = int.from_bytes(data[p + 1 : p + 4], "little")
        body = data[p + 4 : p + 4 + size]
        if t == 1:  # sound data: freq divisor, codec, 8-bit unsigned PCM
            if rate is None:
                rate = 1000000 // (256 - body[0])
                bits = 8
            chunks.append(body[2:])
        elif t == 2:  # continuation
            chunks.append(body)
        elif t == 9:  # new format: u32 rate, u8 bits, u8 channels, u16 codec, 4 reserved
            if rate is None:
                rate = struct.unpack("<I", body[:4])[0]
                bits = body[4] or 8
            chunks.append(body[12:])
        p += 4 + size
    raw = b"".join(chunks)
    if rate is None:
        rate = 8000
    if bits == 16:
        pcm = np.frombuffer(raw[: len(raw) & ~1], dtype="<i2").astype(np.int32)
    else:
        pcm = (np.frombuffer(raw, dtype=np.uint8).astype(np.int32) - 128) << 8
    return rate, pcm


def looped_voc_names(g):
    """Names (upper-case) of VOCs that some definesound marks as looping (type bit 0)."""
    names = set()
    for cname in ("USER.CON", "GAME.CON", "DEFS.CON"):
        if cname not in g.entries:
            continue
        txt = g.read(cname).decode("latin1")
        for m in re.finditer(r"^\s*definesound\s+\w+\s+(\S+)\s+-?\d+\s+-?\d+\s+-?\d+\s+(-?\d+)", txt, re.M):
            if int(m.group(2)) & 1:
                names.add(m.group(1).upper())
    return names


def add_sounds(g, pack):
    looped = looped_voc_names(g)
    raw_total = adpcm_total = 0
    sq_err = sq_sig = 0.0
    nsnd = 0
    worst = (0.0, "")
    biggest = (0, "")
    for name in g.names(".VOC"):
        rate, pcm = voc_to_pcm(g.read(name))
        if len(pcm) == 0:
            pcm = np.zeros(28, dtype=np.int32)
        pcm = pcm.copy()
        if name in looped:
            # PS1: pad to a whole ADPCM block by stretching the cycle, not with zeros (a zero gap
            # would click on every loop; spu_fx.c loops block 0 .. last audio block)
            pcm = spuadpcm.pad_loop(pcm)
        else:
            # short fade-out so the terminator block (silence) does not click
            n = min(len(pcm) // 4, max(8, int(rate * 0.006)))
            if n > 0:
                pcm[-n:] = (pcm[-n:] * np.linspace(1.0, 0.0, n)).astype(np.int32)
        blob = spuadpcm.encode(pcm, restart=name in looped)
        dec = spuadpcm.decode(blob)
        n = len(pcm)
        err = spuadpcm.rms_error(pcm, dec)
        sig = float(np.sqrt((pcm.astype(np.float64) ** 2).mean())) or 1.0
        sq_err += err * err * n
        sq_sig += sig * sig * n
        if err / sig > worst[0] and sig > 200:
            worst = (err / sig, name)
        raw_total += n
        adpcm_total += len(blob)
        nsnd += 1
        if len(blob) > biggest[0]:
            biggest = (len(blob), name)
        pack.add(name, blob, rate)
    snr = 10 * np.log10(sq_sig / max(sq_err, 1e-9))
    print("sounds: %d VOC -> SPU-ADPCM, %.2f MB PCM samples -> %.2f MB ADPCM; round-trip RMS err %.1f LSB (SNR %.1f dB, worst %s rel %.2f); largest %s %d KB; looping %d"
          % (nsnd, raw_total / 1e6, adpcm_total / 1e6, np.sqrt(sq_err / max(raw_total, 1)), snr, worst[1], worst[0],
             biggest[1], biggest[0] // 1024, len(looped)))


def add_music(g, pack, sf2=None):
    import mkmusic  # tools/mkmusic.py: MUSBANK + per-song event streams

    mkmusic.add_music(g, pack, sf2=sf2)


def main():
    argv = sys.argv[1:]
    sf2 = None
    if "--sf2" in argv:  # PS1: explicit SoundFont for the music bank; never auto-detected
        i = argv.index("--sf2")
        sf2 = argv[i + 1] or None
        del argv[i:i + 2]
    grp_path, outdir = argv[0], argv[1]
    os.makedirs(outdir, exist_ok=True)
    g = G.Grp(grp_path)
    pack = Pack()

    pals, numl, bright = write_pals(g)
    pack.add("PALS", pals, numl)
    pack.add("TABLES", g.read("TABLES.DAT"))

    tiles = G.load_all_tiles(g, MAXTILES)
    pal = np.frombuffer(g.read("PALETTE.DAT")[:768], dtype=np.uint8).reshape(256, 3)
    tileidx, pieces, tiledata, tilepix = build_tiles(tiles, pal, texture_tiles(g))
    pack.add("TILEIDX", tileidx, MAXTILES)
    pack.add("PIECES", b"".join(p.pack() for p in pieces), len(pieces))
    gsize = len(tiledata)
    # PS1: the level groups are appended to the TILEDATA blob (their sectors are relative to TILEDATA)
    N = load_names(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "game", "names.h"))
    groups = build_groups(g, tiles, tilepix, tiledata, pack, N)
    pack.add("TILEDATA", tiledata)
    print("tile groups: %d maps, %.1f MB (global TILEDATA %.1f MB)" % (len(groups), (len(tiledata) - gsize) / 1e6, gsize / 1e6))

    for name in g.names(".MAP"):
        pack.add(name, g.read(name))
    for name in ("GAME.CON", "USER.CON", "DEFS.CON", "LOOKUP.DAT"):
        pack.add(name, g.read(name))
    for name in g.names(".ANM"):
        pack.add(name, g.read(name))
    add_music(g, pack, sf2)
    add_sounds(g, pack)
    pack.add("DUKE3D.BIN", g.read("DUKE3D.BIN"))

    table = pack.write(os.path.join(outdir, "DUKE.DAT"))

    with open(os.path.join(outdir, "data_index.h"), "w") as h:
        h.write("/* generated by mkdata.py - do not edit */\n#pragma once\n")
        h.write("#define DAT_MAXTILES %d\n#define DAT_NUMLOOKUPS %d\n#define DAT_NUMPIECES %d\n" % (MAXTILES, numl, len(pieces)))
        for name, sec, size, aux in table:
            ident = re.sub(r"[^A-Z0-9_]", "_", name)
            h.write("#define DAT_%s_SECTOR %d\n#define DAT_%s_SIZE %d\n" % (ident, sec, ident, size))
        for mname, (first, n) in sorted(groups.items()):
            h.write("#define DAT_TILEGRP_%s_SECTOR %d   /* relative to TILEDATA */\n#define DAT_TILEGRP_%s_NSECT %d\n" % (mname, first, mname, n))
    total = os.path.getsize(os.path.join(outdir, "DUKE.DAT"))
    print("DUKE.DAT: %d entries, %d pieces, %.1f MB; shade brightness %s" % (len(table), len(pieces), total / 1e6, bright[:8]))


if __name__ == "__main__":
    main()
