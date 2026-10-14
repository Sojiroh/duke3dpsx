#!/usr/bin/env python3
"""Build DUKE.DAT (sector-aligned asset pack for the PS1 port) from DUKE3D.GRP.

Usage: mkdata.py <DUKE3D.GRP> <out dir>
Writes <out>/DUKE.DAT and <out>/data_index.h.

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
        self.pixels = pixels  # row-major bytes, stride = (w+1)&~1
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
    nearest = nearest_index_table(pal) if halfres else None
    nhalf = 0

    def add_piece(n, px, py, sub):
        h, w = sub.shape
        stride = max(16, (w + 15) & ~15)
        rows = h + (h & 1)
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
        if len(tile_pieces) > 1:
            aux = len(pieces)
            pieces += tile_pieces
        elif n in halfres and t.sizx >= 64 and t.sizy >= 64:
            hp = add_piece(n, 0, 0, downscale_half(img, pal, nearest))
            flags, aux = 1, hp.sector
            nhalf += 1
        recs += struct.pack("<HHiHHHH", t.sizx, t.sizy, t.picanm, tile_pieces[0].sector,
                            avg_color15(img, pal), len(tile_pieces) | (flags << 8), aux)
    print("tiles with a half-resolution copy: %d" % nhalf)
    return bytes(recs), pieces, bytes(data)


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
        if name not in looped:
            # short fade-out so the terminator block (silence) does not click
            n = min(len(pcm) // 4, max(8, int(rate * 0.006)))
            if n > 0:
                pcm[-n:] = (pcm[-n:] * np.linspace(1.0, 0.0, n)).astype(np.int32)
        blob = spuadpcm.encode(pcm)
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


def add_music(g, pack):
    import mkmusic  # tools/mkmusic.py: MUSBANK + per-song event streams

    mkmusic.add_music(g, pack)


def main():
    grp_path, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    g = G.Grp(grp_path)
    pack = Pack()

    pals, numl, bright = write_pals(g)
    pack.add("PALS", pals, numl)
    pack.add("TABLES", g.read("TABLES.DAT"))

    tiles = G.load_all_tiles(g, MAXTILES)
    pal = np.frombuffer(g.read("PALETTE.DAT")[:768], dtype=np.uint8).reshape(256, 3)
    tileidx, pieces, tiledata = build_tiles(tiles, pal, texture_tiles(g))
    pack.add("TILEIDX", tileidx, MAXTILES)
    pack.add("PIECES", b"".join(p.pack() for p in pieces), len(pieces))
    pack.add("TILEDATA", tiledata)

    for name in g.names(".MAP"):
        pack.add(name, g.read(name))
    for name in ("GAME.CON", "USER.CON", "DEFS.CON", "LOOKUP.DAT"):
        pack.add(name, g.read(name))
    for name in g.names(".ANM"):
        pack.add(name, g.read(name))
    add_music(g, pack)
    add_sounds(g, pack)
    pack.add("DUKE3D.BIN", g.read("DUKE3D.BIN"))

    table = pack.write(os.path.join(outdir, "DUKE.DAT"))

    with open(os.path.join(outdir, "data_index.h"), "w") as h:
        h.write("/* generated by mkdata.py - do not edit */\n#pragma once\n")
        h.write("#define DAT_MAXTILES %d\n#define DAT_NUMLOOKUPS %d\n#define DAT_NUMPIECES %d\n" % (MAXTILES, numl, len(pieces)))
        for name, sec, size, aux in table:
            ident = re.sub(r"[^A-Z0-9_]", "_", name)
            h.write("#define DAT_%s_SECTOR %d\n#define DAT_%s_SIZE %d\n" % (ident, sec, ident, size))
    total = os.path.getsize(os.path.join(outdir, "DUKE.DAT"))
    print("DUKE.DAT: %d entries, %d pieces, %.1f MB; shade brightness %s" % (len(table), len(pieces), total / 1e6, bright[:8]))


if __name__ == "__main__":
    main()
