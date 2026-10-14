"""Readers for Build-engine data: GRP archives, ART tiles, MAP boards, VOC sounds.

Formats follow upstream/SRC/ENGINE.C (loadpics, loadboard) and UTIL/KGROUP.C.
"""
import struct
from dataclasses import dataclass, field


class Grp:
    """KenSilverman GRP archive: header 'KenSilverman', u32 count, count*(12-byte name, u32 size), data."""

    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        magic = self.f.read(12)
        if magic != b"KenSilverman":
            raise ValueError("not a GRP file")
        (count,) = struct.unpack("<I", self.f.read(4))
        self.entries = {}  # upper-case name -> (offset, size)
        self.order = []
        off = 16 + 16 * count
        for _ in range(count):
            raw = self.f.read(16)
            name = raw[:12].split(b"\0")[0].decode("ascii").upper()
            (size,) = struct.unpack("<I", raw[12:16])
            self.entries[name] = (off, size)
            self.order.append(name)
            off += size

    def read(self, name):
        off, size = self.entries[name.upper()]
        self.f.seek(off)
        return self.f.read(size)

    def names(self, ext=None):
        if ext is None:
            return list(self.order)
        ext = ext.upper()
        return [n for n in self.order if n.endswith(ext)]


@dataclass
class Tile:
    num: int
    sizx: int
    sizy: int
    picanm: int
    pixels: bytes  # column-major, sizx*sizy bytes (empty if 0x0)


def parse_art(data):
    """Return list of Tile from one TILESxxx.ART blob."""
    ver, numtiles, start, end = struct.unpack("<iiii", data[:16])
    if ver != 1:
        raise ValueError("bad ART version %d" % ver)
    n = end - start + 1
    p = 16
    sizx = struct.unpack("<%dh" % n, data[p : p + 2 * n]); p += 2 * n
    sizy = struct.unpack("<%dh" % n, data[p : p + 2 * n]); p += 2 * n
    anm = struct.unpack("<%di" % n, data[p : p + 4 * n]); p += 4 * n
    tiles = []
    for i in range(n):
        sz = sizx[i] * sizy[i]
        tiles.append(Tile(start + i, sizx[i], sizy[i], anm[i], data[p : p + sz]))
        p += sz
    return tiles


def load_all_tiles(grp, maxtiles=9216):
    """Dict tilenum -> Tile for every TILES###.ART in the GRP (loadpics order)."""
    tiles = {}
    k = 0
    while True:
        name = "TILES%03d.ART" % k
        if name not in grp.entries:
            break
        for t in parse_art(grp.read(name)):
            if t.num < maxtiles:
                tiles[t.num] = t
        k += 1
    return tiles


SECTOR_FMT = "<hhiihhhhbBBBhhbBBBBBhhh"
# wall: long x,y; short point2,nextwall,nextsector,cstat; short picnum,overpicnum; signed char shade; char pal,xrepeat,yrepeat,xpanning,ypanning; short lotag,hitag,extra
WALL_FMT = "<iihhhhhhbBBBBBhhh"
# sprite: long x,y,z; short cstat,picnum; signed char shade; char pal,clipdist,filler; uchar xrepeat,yrepeat; schar xoffset,yoffset; short sectnum,statnum,ang,owner,xvel,yvel,zvel,lotag,hitag,extra
SPRITE_FMT = "<iiihhbBBBBBbbhhhhhhhhhh"

assert struct.calcsize(SECTOR_FMT) == 40
assert struct.calcsize(WALL_FMT) == 32
assert struct.calcsize(SPRITE_FMT) == 44


@dataclass
class Board:
    posx: int; posy: int; posz: int; ang: int; cursectnum: int
    sectors: list = field(default_factory=list)
    walls: list = field(default_factory=list)
    sprites: list = field(default_factory=list)


def parse_map(data):
    (ver,) = struct.unpack("<i", data[:4])
    if ver != 7:
        raise ValueError("map version %d" % ver)
    posx, posy, posz, ang, cursect = struct.unpack("<iiihh", data[4:20])
    p = 20
    (numsectors,) = struct.unpack("<H", data[p : p + 2]); p += 2
    sectors = [struct.unpack(SECTOR_FMT, data[p + 40 * i : p + 40 * i + 40]) for i in range(numsectors)]
    p += 40 * numsectors
    (numwalls,) = struct.unpack("<H", data[p : p + 2]); p += 2
    walls = [struct.unpack(WALL_FMT, data[p + 32 * i : p + 32 * i + 32]) for i in range(numwalls)]
    p += 32 * numwalls
    (numsprites,) = struct.unpack("<H", data[p : p + 2]); p += 2
    sprites = [struct.unpack(SPRITE_FMT, data[p + 44 * i : p + 44 * i + 44]) for i in range(numsprites)]
    return Board(posx, posy, posz, ang, cursect, sectors, walls, sprites)


def map_picnums(board):
    pics = set()
    for s in board.sectors:
        pics.add(s[6]); pics.add(s[12])
    for w in board.walls:
        pics.add(w[6]); pics.add(w[7])
    for sp in board.sprites:
        pics.add(sp[4])
    return pics


def voc_info(data):
    """Return (sample_rate, bits, channels, pcm_bytes) for a Creative VOC file (first sound block)."""
    if not data.startswith(b"Creative Voice File"):
        return None
    p = struct.unpack("<H", data[20:22])[0]
    rate = bits = ch = None
    pcm = 0
    while p < len(data):
        t = data[p]
        if t == 0:
            break
        size = int.from_bytes(data[p + 1 : p + 4], "little")
        body = data[p + 4 : p + 4 + size]
        if t == 1:
            if rate is None:
                rate = 1000000 // (256 - body[0]); bits = 8; ch = 1
            pcm += size - 2
        elif t == 9:
            if rate is None:
                rate = struct.unpack("<I", body[:4])[0]; bits = body[4]; ch = body[5]
            pcm += size - 12
        elif t == 8:
            pass
        p += 4 + size
    return rate, bits, ch, pcm
