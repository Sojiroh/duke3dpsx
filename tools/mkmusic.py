#!/usr/bin/env python3
"""Convert the Duke3D .MID songs (MSQ1 event streams) and build the SPU instrument bank (MBK1).

Pack entries produced by add_music(g, pack):
  MUSBANK (aux = #regions)   instrument bank: header, region table, SPU-ADPCM sample data
  <NAME>.MID (aux = length)  one MSQ1 event stream per song

Formats are documented next to the writers (write_song / build_bank).  Python 3 + numpy only.
Usage:  python3 tools/mkmusic.py <DUKE3D.GRP> [--selftest]
"""
import bisect
import importlib.util
import os
import struct
import sys
import time

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
import spuadpcm  # noqa: E402

TICK_HZ = 120  # song time base: 1/120 s

# ------------------------------------------------------------------------------------------
# MIDI parsing
# ------------------------------------------------------------------------------------------


def _vlq(data, p):
    v = 0
    while True:
        b = data[p]
        p += 1
        v = (v << 7) | (b & 0x7F)
        if not b & 0x80:
            return v, p


def parse_midi(data):
    """Return (division, events, track_end_tick).

    events: list of (tick, trk, seq, typ, ch, a, b) with typ in
      'on','off' (a=note,b=vel) 'cc' (a=ctl,b=val) 'prog' (a) 'bend' (a=14-bit value) 'tempo' (a=usec/qn)."""
    if data[:4] != b"MThd":
        raise ValueError("not a MIDI file")
    hlen = struct.unpack(">I", data[4:8])[0]
    fmt, ntrk, div = struct.unpack(">HHH", data[8:14])
    if div & 0x8000:
        raise ValueError("SMPTE division not supported")
    p = 8 + hlen
    events = []
    end_tick = 0
    seq = 0
    for trk in range(ntrk):
        if data[p:p + 4] != b"MTrk":
            break
        ln = struct.unpack(">I", data[p + 4:p + 8])[0]
        p += 8
        endp = min(p + ln, len(data))
        tick = 0
        status = 0
        while p < endp:
            dt, p = _vlq(data, p)
            tick += dt
            b = data[p]
            if b & 0x80:
                status = b
                p += 1
            hi, ch = status >> 4, status & 15
            if status == 0xFF:
                mt = data[p]
                ml, p2 = _vlq(data, p + 1)
                body = data[p2:p2 + ml]
                p = p2 + ml
                if mt == 0x51 and ml == 3:
                    events.append((tick, trk, seq, "tempo", 0, int.from_bytes(body, "big"), 0))
                    seq += 1
                elif mt == 0x2F:
                    end_tick = max(end_tick, tick)
                    break
            elif status in (0xF0, 0xF7):
                ml, p = _vlq(data, p)
                p += ml
            elif hi in (0x8, 0x9, 0xB, 0xE, 0xA):
                a, bb = data[p], data[p + 1]
                p += 2
                if hi == 0x9 and bb:
                    events.append((tick, trk, seq, "on", ch, a, bb))
                elif hi in (0x8, 0x9):
                    events.append((tick, trk, seq, "off", ch, a, bb))
                elif hi == 0xB:
                    events.append((tick, trk, seq, "cc", ch, a, bb))
                elif hi == 0xE:
                    events.append((tick, trk, seq, "bend", ch, a | (bb << 7), 0))
                seq += 1
            elif hi in (0xC, 0xD):
                a = data[p]
                p += 1
                if hi == 0xC:
                    events.append((tick, trk, seq, "prog", ch, a, 0))
                    seq += 1
            else:
                raise ValueError("bad MIDI status %02x" % status)
        end_tick = max(end_tick, tick)
        p = endp
    events.sort(key=lambda e: (e[0], e[1], e[2]))
    return div, events, end_tick


class TempoMap:
    def __init__(self, div, events):
        segs = []  # (tick, seconds_at_tick, usec_per_qn)
        t0, s0, us = 0, 0.0, 500000
        segs.append((0, 0.0, us))
        for e in events:
            if e[3] != "tempo":
                continue
            s0 += (e[0] - t0) * us / (div * 1e6)
            t0 = e[0]
            us = e[5]
            if segs and segs[-1][0] == t0:
                segs[-1] = (t0, s0, us)
            else:
                segs.append((t0, s0, us))
        self.div = div
        self.segs = segs
        self.keys = [s[0] for s in segs]

    def seconds(self, tick):
        i = bisect.bisect_right(self.keys, tick) - 1
        t0, s0, us = self.segs[i]
        return s0 + (tick - t0) * us / (self.div * 1e6)

    def ticks(self, tick):
        return int(round(self.seconds(tick) * TICK_HZ))


# ------------------------------------------------------------------------------------------
# MIDI -> MSQ1 event stream
# ------------------------------------------------------------------------------------------

EV_NOTE, EV_PROG, EV_VOL, EV_PAN, EV_EXPR, EV_BEND, EV_ALLOFF, EV_END = 0, 1, 2, 3, 4, 5, 6, 0xF
CTRL_WINDOW = {EV_BEND: 3, EV_PAN: 3, EV_VOL: 2, EV_EXPR: 2}  # ticks
DEFAULTS = {"prog": 0, "vol": 100, "expr": 127, "pan": 64, "bend": 0}


def vlq_bytes(v):
    assert 0 <= v < (1 << 28)
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    return bytes(reversed(out))


class Song:
    """Decoded song: events = list of (tick, kind, ch, a, b) in emission order; notes carry duration in b."""

    def __init__(self):
        self.events = []
        self.loop_tick = 0
        self.notes = []  # (tick, ch, note, vel, dur, program)


def convert_midi(data):
    div, evs, end_midi = parse_midi(data)
    tm = TempoMap(div, evs)
    # per-channel state
    st = [dict(DEFAULTS) for _ in range(16)]
    rpn = [[127, 127] for _ in range(16)]  # [msb(101), lsb(100)]
    brange = [[2, 0] for _ in range(16)]  # semitones, cents
    rawbend = [8192] * 16
    out = []  # (t120, prio, order, kind, ch, a, b)
    open_notes = {}  # (ch, note) -> FIFO list of out indices
    order = 0

    def emit(t, prio, kind, ch, a=0, b=0):
        nonlocal order
        out.append([t, prio, order, kind, ch, a, b])
        order += 1
        return len(out) - 1

    def bend_cents(ch):
        rng = brange[ch][0] * 100 + brange[ch][1]
        return int(round((rawbend[ch] - 8192) / 8192.0 * rng))

    def maybe_bend(t, ch):
        c = max(-32768, min(32767, bend_cents(ch)))
        if c != st[ch]["bend"]:
            st[ch]["bend"] = c
            emit(t, 0, EV_BEND, ch, c)

    for tk, trk, seq, typ, ch, a, b in evs:
        if typ == "tempo":
            continue
        t = tm.ticks(tk)
        if typ == "on":
            idx = emit(t, 1, EV_NOTE, ch, a, b)
            open_notes.setdefault((ch, a), []).append(idx)
        elif typ == "off":
            q = open_notes.get((ch, a))
            if q:
                idx = q.pop(0)
                out[idx].append(t)  # off tick as 8th element
        elif typ == "prog":
            if st[ch]["prog"] != a:
                st[ch]["prog"] = a
                emit(t, 0, EV_PROG, ch, a)
        elif typ == "bend":
            rawbend[ch] = a
            maybe_bend(t, ch)
        elif typ == "cc":
            if a == 7:
                key, kind = "vol", EV_VOL
            elif a == 10:
                key, kind = "pan", EV_PAN
            elif a == 11:
                key, kind = "expr", EV_EXPR
            else:
                key = None
            if key:
                if st[ch][key] != b:
                    st[ch][key] = b
                    emit(t, 0, kind, ch, b)
            elif a == 101:
                rpn[ch][0] = b
            elif a == 100:
                rpn[ch][1] = b
            elif a in (98, 99):
                rpn[ch] = [127, 127]
            elif a in (6, 38) and rpn[ch] == [0, 0]:
                if a == 6:
                    brange[ch][0] = b
                else:
                    brange[ch][1] = b
                maybe_bend(t, ch)
            elif a in (120, 123):
                emit(t, 0, EV_ALLOFF, ch)
    # song end / durations
    end120 = tm.ticks(end_midi)
    for o in out:
        if o[3] == EV_NOTE and len(o) > 7:
            end120 = max(end120, o[7])
        end120 = max(end120, o[0])
    song = Song()
    for o in out:
        if o[3] == EV_NOTE:
            off = o[7] if len(o) > 7 else end120
            o_dur = max(1, off - o[0])
            end120 = max(end120, o[0] + o_dur)
            o.append(o_dur)  # real duration = last element (channel 9 is emitted with duration 1)
    out.sort(key=lambda o: (o[0], o[1], o[2]))
    # same-tick repeats of one controller only matter by their last value; then drop non-changes
    # (continuous controllers are additionally thinned to one update per CTRL_WINDOW ticks)
    last_at = {}
    for i, o in enumerate(out):
        if o[3] != EV_NOTE:
            w = CTRL_WINDOW.get(o[3], 1)
            last_at[(o[0] // w, o[3], o[4])] = i
    cur = {}
    kept = []
    for i, o in enumerate(out):
        if o[3] != EV_NOTE:
            if last_at[(o[0] // CTRL_WINDOW.get(o[3], 1), o[3], o[4])] != i:
                continue
            key = (o[3], o[4])
            base = {EV_PROG: DEFAULTS["prog"], EV_VOL: DEFAULTS["vol"], EV_PAN: DEFAULTS["pan"],
                    EV_EXPR: DEFAULTS["expr"], EV_BEND: 0}.get(o[3])
            if o[3] != EV_ALLOFF and cur.get(key, base) == o[5]:
                continue
            cur[key] = o[5]
        kept.append(o)
    out = kept
    prog_at = [0] * 16
    for o in out:
        t, _, _, kind, ch, a, b = o[:7]
        if kind == EV_PROG:
            prog_at[ch] = a
        if kind == EV_NOTE:
            dur = o[-1]
            song.notes.append((t, ch, a, b, dur, prog_at[ch]))
            song.events.append((t, kind, ch, a, b, 1 if ch == 9 else dur))
        else:
            song.events.append((t, kind, ch, a, b, 0))
    song.loop_tick = end120
    return song


def write_song(song):
    """MSQ1: magic, u32 events_len, u32 length_ticks, u32 0; events: VLQ delta, cmd (type<<4|ch), payload."""
    body = bytearray()
    last = 0
    for t, kind, ch, a, b, dur in song.events:
        body += vlq_bytes(t - last)
        last = t
        body.append((kind << 4) | ch)
        if kind == EV_NOTE:
            body += bytes([a, b]) + vlq_bytes(dur)
        elif kind == EV_BEND:
            body += struct.pack("<h", a)
        elif kind != EV_ALLOFF:
            body.append(a)
    assert song.loop_tick >= last
    body += vlq_bytes(song.loop_tick - last)
    body.append(0xFF)
    return b"MSQ1" + struct.pack("<III", len(body), song.loop_tick, 0) + bytes(body)


# ------------------------------------------------------------------------------------------
# independent verification (own MIDI reader + own stream decoder)
# ------------------------------------------------------------------------------------------


def verify_stream(blob):
    """Decode an MSQ1 blob exactly per spec; return dict(notes=[(tick,ch,note,vel,dur)], ticks, counts)."""
    assert blob[:4] == b"MSQ1"
    elen, length, rsv = struct.unpack("<III", blob[4:16])
    assert rsv == 0 and len(blob) == 16 + elen
    p, end = 16, 16 + elen
    t = 0
    notes = []
    cnt = [0] * 16
    while True:
        d = 0
        while True:
            c = blob[p]
            p += 1
            d = (d << 7) | (c & 0x7F)
            if not c & 0x80:
                break
        t += d
        cmd = blob[p]
        p += 1
        if cmd == 0xFF:
            assert p == end, "END must be last"
            break
        typ, ch = cmd >> 4, cmd & 15
        if typ == 0:
            note, vel = blob[p], blob[p + 1]
            p += 2
            dd = 0
            while True:
                c = blob[p]
                p += 1
                dd = (dd << 7) | (c & 0x7F)
                if not c & 0x80:
                    break
            assert note < 128 and 1 <= vel < 128 and dd >= 1
            notes.append((t, ch, note, vel, dd))
        elif typ in (1, 2, 3, 4):
            p += 1
        elif typ == 5:
            p += 2
        elif typ == 6:
            pass
        else:
            raise AssertionError("bad cmd %02x" % cmd)
        cnt[typ] += 1
    assert t == length, (t, length)
    return dict(notes=notes, ticks=length, counts=cnt)


def ref_midi_stats(data):
    """Independent (simple) MIDI reader: returns (note_on_count, [(sec, ch, note, vel)], end_sec_of_last_note_or_eot)."""
    div = struct.unpack(">H", data[12:14])[0]
    ntrk = struct.unpack(">H", data[10:12])[0]
    p = 14
    tempos = [(0, 500000)]
    raw = []
    eot = 0
    for _ in range(ntrk):
        assert data[p:p + 4] == b"MTrk"
        ln = int.from_bytes(data[p + 4:p + 8], "big")
        q, e, t, rs = p + 8, p + 8 + ln, 0, 0
        while q < e:
            v = 0
            while True:
                c = data[q]
                q += 1
                v = v * 128 + (c & 127)
                if c < 128:
                    break
            t += v
            c = data[q]
            if c >= 128:
                rs = c
                q += 1
            if rs == 0xFF:
                mt = data[q]
                q += 1
                v = 0
                while True:
                    c = data[q]
                    q += 1
                    v = v * 128 + (c & 127)
                    if c < 128:
                        break
                if mt == 0x51:
                    tempos.append((t, int.from_bytes(data[q:q + 3], "big")))
                q += v
            elif rs in (0xF0, 0xF7):
                v = 0
                while True:
                    c = data[q]
                    q += 1
                    v = v * 128 + (c & 127)
                    if c < 128:
                        break
                q += v
            else:
                n = 1 if rs >> 4 in (0xC, 0xD) else 2
                if rs >> 4 == 9 and data[q + 1]:
                    raw.append((t, rs & 15, data[q], data[q + 1]))
                q += n
        eot = max(eot, t)
        p += 8 + ln
    tempos.sort(key=lambda x: x[0])

    def sec(tk):
        s, last_t, us = 0.0, 0, 500000
        for tt, u in tempos:
            if tt > tk:
                break
            s += (tt - last_t) * us / (div * 1e6)
            last_t, us = tt, u
        return s + (tk - last_t) * us / (div * 1e6)

    return len(raw), sorted((sec(t), c, n, v) for t, c, n, v in raw), sec(eot)


def selftest_songs(g, songs):
    tot_notes = 0
    for name, (song, blob) in songs.items():
        info = verify_stream(blob)
        n_ref, ref_notes, eot_sec = ref_midi_stats(g.read(name))
        assert len(info["notes"]) == n_ref == len(song.notes), (name, len(info["notes"]), n_ref)
        got = sorted(info["notes"])
        # every note start must match the reference time within 1 tick
        a = sorted((c, n, v) for _, c, n, v in ref_notes)
        b = sorted((c, n, v) for _, c, n, v, _ in got)
        assert a == b, name + ": note (ch,note,vel) multiset mismatch"
        rs = sorted(ref_notes, key=lambda x: (x[2], x[1], x[3], x[0]))
        gs = sorted(got, key=lambda x: (x[2], x[1], x[3], x[0]))
        mx = max(abs(r[0] * TICK_HZ - x[0]) for r, x in zip(rs, gs))
        assert mx <= 0.5 + 1e-6, (name, mx)
        assert info["ticks"] >= round(eot_sec * TICK_HZ) - 1, (name, info["ticks"], eot_sec * TICK_HZ)
        tot_notes += n_ref
    return tot_notes


# ------------------------------------------------------------------------------------------
# SoundFont 2 reader
# ------------------------------------------------------------------------------------------

GEN_NAMES = {0: "startAddrOfs", 1: "endAddrOfs", 2: "startLoopOfs", 3: "endLoopOfs", 4: "startAddrCoarse",
             12: "endAddrCoarse", 17: "pan", 34: "attack", 35: "hold", 36: "decay", 37: "sustain", 38: "release",
             43: "keyRange", 44: "velRange", 45: "startLoopCoarse", 48: "atten", 50: "endLoopCoarse",
             51: "coarseTune", 52: "fineTune", 53: "sampleID", 54: "sampleModes", 56: "scaleTuning",
             58: "rootKey", 41: "instrument"}
GEN_DEFAULT = {"attack": -12000, "hold": -12000, "decay": -12000, "sustain": 0, "release": -12000,
               "pan": 0, "atten": 0, "coarseTune": 0, "fineTune": 0, "scaleTuning": 100, "sampleModes": 0,
               "rootKey": -1, "startAddrOfs": 0, "endAddrOfs": 0, "startLoopOfs": 0, "endLoopOfs": 0,
               "startAddrCoarse": 0, "endAddrCoarse": 0, "startLoopCoarse": 0, "endLoopCoarse": 0}
ADDITIVE = {"attack", "hold", "decay", "sustain", "release", "pan", "atten", "coarseTune", "fineTune",
            "scaleTuning", "startAddrOfs", "endAddrOfs", "startLoopOfs", "endLoopOfs", "startAddrCoarse",
            "endAddrCoarse", "startLoopCoarse", "endLoopCoarse"}


def tc2s(tc):
    return 2.0 ** (tc / 1200.0)


class SF2:
    class Zone:
        pass

    def __init__(self, path):
        self.path = path
        d = open(path, "rb").read()
        assert d[:4] == b"RIFF" and d[8:12] == b"sfbk"
        chunks = {}

        def walk(p, end):
            while p + 8 <= end:
                cid, ln = d[p:p + 4], struct.unpack("<I", d[p + 4:p + 8])[0]
                if cid == b"LIST":
                    walk(p + 12, p + 8 + ln)
                else:
                    chunks[cid.decode("latin1")] = (p + 8, ln)
                p += 8 + ln + (ln & 1)

        walk(12, len(d))

        def rec(name, size, fmt):
            o, ln = chunks[name]
            return [struct.unpack_from(fmt, d, o + i * size) for i in range(ln // size)]

        o, ln = chunks["smpl"]
        self.smpl = np.frombuffer(d, dtype="<i2", count=ln // 2, offset=o)
        self.phdr = rec("phdr", 38, "<20sHHHIII")
        self.pbag = rec("pbag", 4, "<HH")
        self.pgen = rec("pgen", 4, "<Hh")
        self.inst = rec("inst", 22, "<20sH")
        self.ibag = rec("ibag", 4, "<HH")
        self.igen = rec("igen", 4, "<Hh")
        self.shdr = rec("shdr", 46, "<20sIIIIIBbHH")
        self.cache = {}

    def _gens(self, gens, b0, b1, bag):
        """List of (gendict, is_global) for bags b0..b1-1."""
        zones = []
        for b in range(b0, b1):
            g0, g1 = bag[b][0], bag[b + 1][0]
            gd = {}
            for oper, amt in gens[g0:g1]:
                gd[oper] = amt
            zones.append(gd)
        return zones

    @staticmethod
    def _rng(gd, op):
        if op not in gd:
            return (0, 127)
        v = gd[op] & 0xFFFF
        return (v & 0xFF, v >> 8)

    def preset_zones(self, bank, prog):
        """List of resolved Zone objects (key/vel ranges, generator values, sample header) of a preset."""
        key = (bank, prog)
        if key in self.cache:
            return self.cache[key]
        res = []
        for i in range(len(self.phdr) - 1):
            name, pnum, pbank, pbag, _, _, _ = self.phdr[i]
            if (pbank, pnum) != (bank, prog):
                continue
            pz = self._gens(self.pgen, pbag, self.phdr[i + 1][3], self.pbag)
            pglob = pz[0] if pz and 41 not in pz[0] else {}
            for zg in pz:
                if 41 not in zg:
                    continue
                inst = zg[41] & 0xFFFF
                iz = self._gens(self.igen, self.inst[inst][1], self.inst[inst + 1][1], self.ibag)
                iglob = iz[0] if iz and 53 not in iz[0] else {}
                for ig in iz:
                    if 53 not in ig:
                        continue
                    z = SF2.Zone()
                    z.key = self._intersect(self._rng(ig, 43) if 43 in ig else self._rng(iglob, 43),
                                            self._rng(zg, 43) if 43 in zg else self._rng(pglob, 43))
                    z.vel = self._intersect(self._rng(ig, 44) if 44 in ig else self._rng(iglob, 44),
                                            self._rng(zg, 44) if 44 in zg else self._rng(pglob, 44))
                    if z.key[0] > z.key[1] or z.vel[0] > z.vel[1]:
                        continue
                    z.sid = ig[53] & 0xFFFF
                    for op, nm in GEN_NAMES.items():
                        if nm in ("keyRange", "velRange", "sampleID", "instrument"):
                            continue
                        v = iglob.get(op, GEN_DEFAULT.get(nm))
                        v = ig.get(op, v)
                        if v is None:
                            continue
                        if nm in ADDITIVE:
                            v += zg.get(op, pglob.get(op, 0))
                        setattr(z, nm, v)
                    for nm, dv in GEN_DEFAULT.items():
                        if not hasattr(z, nm):
                            setattr(z, nm, dv)
                    z.hdr = self.shdr[z.sid]
                    res.append(z)
        self.cache[key] = res
        return res

    @staticmethod
    def _intersect(a, b):
        return (max(a[0], b[0]), min(a[1], b[1]))

    def find_zone(self, bank, prog, key, vel=100):
        """Best zone for (key, vel): mono samples first, first match otherwise."""
        zs = [z for z in self.preset_zones(bank, prog) if z.key[0] <= key <= z.key[1] and z.vel[0] <= vel <= z.vel[1]]
        if not zs:
            zs = [z for z in self.preset_zones(bank, prog) if z.key[0] <= key <= z.key[1]]
        if not zs:
            return None
        zs.sort(key=lambda z: (z.hdr[9] & 0x7FFF) != 1)  # mono first
        return zs[0]

    def zone_audio(self, z):
        """Return (pcm float64 array [-1,1], loop_start, loop_end, looped, rate, root, fine_cents)
        with addresses relative to the returned array start."""
        name, st, en, ls, le, rate, pitch, corr, link, typ = z.hdr
        st2 = st + z.startAddrOfs + z.startAddrCoarse * 32768
        en2 = en + z.endAddrOfs + z.endAddrCoarse * 32768
        ls2 = ls + z.startLoopOfs + z.startLoopCoarse * 32768
        le2 = le + z.endLoopOfs + z.endLoopCoarse * 32768
        pcm = self.smpl[st2:en2].astype(np.float64) / 32768.0
        looped = z.sampleModes in (1, 3)
        ls2 -= st2
        le2 -= st2
        if looped and not (0 <= ls2 < le2 <= len(pcm) and le2 - ls2 >= 8):
            looped = False
        root = z.rootKey if z.rootKey >= 0 else (pitch if pitch <= 127 else 60)
        fine = corr + z.coarseTune * 100 + z.fineTune
        return pcm, ls2, le2, looped, rate, root, fine


def find_sf2():
    env = os.environ.get("DUKE_SF2")
    if env is not None:
        return None if env.lower() == "none" else env
    for p in ("/home/sojiroh/Games/DooM/gzdoom/soundfonts/gzdoom.sf2",
              "/home/sojiroh/Games/DooM/DSC-1.0.1/DSC-1.0/data/soundfonts/gzdoom.sf2"):
        if os.path.exists(p):
            return p
    return None


# ------------------------------------------------------------------------------------------
# Instrument table: GM program groups (<= 36 distinct melodic sample sets)
# ------------------------------------------------------------------------------------------
# name: (SF2 source program, [GM programs served], class, synth recipe)
GROUPS = [
    ("piano",      0, [0, 1],            "key",  "piano"),
    ("epiano",     4, [4],               "key",  "epiano"),
    ("clav",       7, [7],               "key",  "clav"),
    ("celesta",    8, [8, 10],           "key",  "bell"),
    ("marimba",   12, [12, 13],          "key",  "mallet"),
    ("tubular",   14, [14],              "key",  "bell"),
    ("organ",     16, [16, 18],          "org",  "organ"),
    ("mutegtr",   28, [28],              "gtr",  "pluck"),
    ("distgtr",   30, [29, 30],          "gtr",  "distgtr"),
    ("harmonic",  31, [31],              "gtr",  "sine"),
    ("bass",      33, [33, 34, 35],      "bass", "bass"),
    ("slapbass",  37, [37],              "bass", "bass"),
    ("synbass",   38, [38, 39],          "bass", "synbass"),
    ("contrabs",  43, [43],              "bass", "bass"),
    ("strings",   48, [44, 48, 49],      "str",  "strings"),
    ("pizz",      45, [45],              "gtr",  "pluck"),
    ("harp",      46, [46],              "key",  "pluck"),
    ("timpani",   47, [47],              "perc", "timpani"),
    ("synstr",    50, [50, 51],          "pad",  "synstr"),
    ("choir",     52, [52, 54],          "pad",  "choir"),
    ("trumpet",   56, [56],              "lead", "brass"),
    ("brass",     61, [61, 62, 63],      "lead", "brass"),
    ("horn",      60, [58, 60],          "lead", "horn"),
    ("flute",     74, [74, 76, 79],      "lead", "flute"),
    ("leadsq",    80, [80],              "lead", "square"),
    ("leadsaw",   81, [81, 82, 85, 87],  "lead", "saw"),
    ("padwarm",   89, [88, 89, 91, 92],  "pad",  "pad"),
    ("padmetal",  95, [90, 93, 94, 95],  "pad",  "pad"),
    ("fx97",      97, [97, 101, 102],    "fx",   "pad"),
    ("crystal",   98, [98],              "key",  "bell"),
    ("kalimba",  108, [108],             "key",  "mallet"),
    ("steeldrm", 114, [115],             "key",  "mallet"),
    ("taiko",    116, [116],             "perc", "timpani"),
    ("syntom",   117, [117, 118],        "perc", "timpani"),
    ("revcym",   119, [119, 120],        "fx",   "noise"),
    ("seashore", 122, [121, 122],        "fx",   "noise"),
]
# class -> (initial rate Hz, loudness offset dB)
CLASSES = {"key": (22050, -1.0), "org": (16000, -2.0), "gtr": (16000, -1.0), "bass": (11025, 0.0),
           "str": (16000, -4.0), "pad": (11025, -4.0), "lead": (22050, -2.0), "fx": (11025, -3.0),
           "perc": (11025, 0.0)}
HEAD_S, WIN_S = 0.30, 0.25
SAMPLE_BUDGET = 148000  # bytes of ADPCM
MAX_REGIONS = 120
RATES = (22050, 16000, 11025, 8000)

# drum note -> (kind, max seconds kept, initial rate)
DRUM_KIND = {}
for _n in (35, 36):
    DRUM_KIND[_n] = ("kick", 0.25, 11025)
for _n in (38, 40, 37, 39, 31, 33):
    DRUM_KIND[_n] = ("snare", 0.28, 22050)
for _n in (41, 43, 45, 47, 48, 50):
    DRUM_KIND[_n] = ("tom", 0.35, 11025)
for _n in (42, 44):
    DRUM_KIND[_n] = ("hat", 0.15, 22050)
DRUM_KIND[46] = ("ohat", 0.40, 22050)
for _n in (49, 51, 52, 53, 55, 57, 59):
    DRUM_KIND[_n] = ("cym", 0.55, 22050)
DRUM_REL = {"kick": 0.06, "snare": 0.06, "tom": 0.10, "hat": 0.03, "ohat": 0.12, "cym": 0.20, "other": 0.06}
DRUM_DEFAULT = ("other", 0.30, 16000)

# ------------------------------------------------------------------------------------------
# SPU envelope model + ADSR register fitting
# ------------------------------------------------------------------------------------------

SR = 44100.0


def env_time(exp, dec, shift, stepf, l0, target, tmax):
    """Samples until the envelope level crosses `target` (>= when rising, <= when falling) from l0, or None.
    inc steps +7..+4, dec steps -8..-5 for stepf 0..3 (release/decay pass stepf=0)."""
    cyc0 = 1 << max(0, shift - 11)
    base = (-(8 - stepf) if dec else (7 - stepf)) << max(0, 11 - shift)
    lvl, t = l0, 0
    while t <= tmax:
        if (dec and lvl <= target) or (not dec and lvl >= target):
            return t
        step = base
        cyc = cyc0
        if exp and dec:
            step = (base * lvl) >> 15
        elif exp and lvl > 0x6000:
            cyc *= 4
        if dec and step == 0:
            return None
        lvl = max(0, min(0x7FFF, lvl + step))
        t += cyc
    return None


_tabs = {}


def _attack_table():
    if "a" not in _tabs:
        tab = []
        for exp in (0, 1):
            for shift in range(32):
                for st in range(4):
                    t = env_time(exp, 0, shift, st, 0, 0x7FFF, SR * 1.0)
                    if t is not None:
                        tab.append((t / SR, (exp, shift, st)))
        _tabs["a"] = tab
    return _tabs["a"]


def _decay_table():
    """per decay shift: list of seconds to fall from full level to level (SL+1)*0x800, SL=0..14."""
    if "d" not in _tabs:
        _tabs["d"] = [[env_time(1, 1, sh, 0, 0x7FFF, (sl + 1) * 0x800, SR * 30) for sl in range(15)] for sh in range(16)]
    return _tabs["d"]


def _susdec_table():
    """(dB per second, (shift, step)) of exponential sustain-decrease settings."""
    if "s" not in _tabs:
        tab = []
        for sh in range(0, 26):
            for st in range(4):
                t = env_time(1, 1, sh, st, 0x7FFF, 0x2000, SR * 200)
                if t:
                    tab.append((12.04 / (t / SR), (sh, st)))
        _tabs["s"] = tab
    return _tabs["s"]


def _release_table():
    """seconds for the exponential release to fall from full level below ~1%, per shift 0..31."""
    if "r" not in _tabs:
        _tabs["r"] = [(env_time(1, 1, sh, 0, 0x7FFF, 0x147, SR * 60), sh) for sh in range(32)]
        _tabs["r"] = [(t / SR, sh) for t, sh in _tabs["r"] if t]
    return _tabs["r"]


def _nearest(tab, v):
    return min(tab, key=lambda e: abs(np.log(max(e[0], 1e-9) / max(v, 1e-9))))


def fit_release(sec):
    t, sh = _nearest(_release_table(), sec)
    return sh, t


def fit_adsr(env, looped, drum):
    """env = dict(attack s, decay s (per 100 dB), sustain dB, release s (per 100 dB)).
    Returns (adsr1, adsr2, rel_ticks)."""
    if drum:
        a = (0, 0, 0)
        sl, dsh = 15, 0
        const = True
        rel_sec = env["relsec"]
    else:
        ta = min(env["attack"], 0.5)
        if ta <= 0.004:
            a = (0, 0, 0)
        else:
            a = _nearest([e for e in _attack_table() if e[0] <= 0.5 and e[1][0] == 0], ta)[1]
        rel_sec = min(max(0.4 * env["release"], 0.05), 0.5)
        sl, dsh, const = 15, 0, True
        sdb = env["sustain"]
        if looped:
            if sdb >= 30.0:
                const = False
                rate = 100.0 / max(env["decay"], 0.05)  # dB per second
                (sus_sh, sus_st) = _nearest_rate(rate)
            elif sdb > 1.5:
                lvl = 0x7FFF * 10 ** (-sdb / 20.0)
                sl = int(min(14, max(0, round(lvl / 0x800) - 1)))
                tdec = max(0.01, env["decay"] * sdb / 100.0)
                tab = _decay_table()
                dsh = min(range(16), key=lambda s: abs(np.log((tab[s][sl] or 1e9) / SR / tdec)) if tab[s][sl] else 1e9)
    rsh, rt = fit_release(rel_sec)
    adsr1 = (a[0] << 15) | (a[1] << 10) | (a[2] << 8) | (dsh << 4) | sl
    if const:
        adsr2 = 0x1F00 | 0x20 | rsh
    else:
        adsr2 = 0x8000 | 0x4000 | (sus_sh << 8) | (sus_st << 6) | 0x20 | rsh
    return adsr1, adsr2, int(min(120, max(2, round(rt * TICK_HZ))))


def _nearest_rate(rate):
    e = min(_susdec_table(), key=lambda e: abs(np.log(e[0] / rate)))
    return e[1]


# ------------------------------------------------------------------------------------------
# sample sources: SoundFont and pure synthesis
# ------------------------------------------------------------------------------------------


class ZoneInfo:
    """A source sample plus the parameters needed to play it."""

    def __init__(self, key, pcm, ls, le, looped, sr, root, fine, pan=0, atten=0, env=None, scale=100):
        self.key, self.pcm, self.ls, self.le, self.looped = key, pcm, ls, le, looped
        self.sr, self.root, self.fine, self.pan, self.atten, self.scale = sr, root, fine, pan, atten, scale
        self.env = env or dict(attack=0.001, decay=0.05, sustain=0.0, release=0.1)


class SF2Source:
    def __init__(self, path):
        self.sf = SF2(path)
        self.name = os.path.basename(path)

    def _info(self, z):
        pcm, ls, le, looped, rate, root, fine = self.sf.zone_audio(z)
        stereo = (z.hdr[9] & 0x7FFF) in (2, 4)
        env = dict(attack=tc2s(z.attack), decay=tc2s(z.decay), sustain=min(z.sustain, 1440) / 10.0,
                   release=tc2s(z.release))
        return ZoneInfo((z.sid, ls, le, len(pcm), looped), pcm, ls, le, looped, float(rate), root, fine,
                        0 if stereo else z.pan, z.atten, env, z.scaleTuning)

    def melodic(self, prog, key):
        z = self.sf.find_zone(0, prog, key, 100)
        if z is None:
            for p in (prog - prog % 8, 0):
                z = self.sf.find_zone(0, p, key, 100)
                if z:
                    break
        return self._info(z)

    def drum(self, note):
        z = self.sf.find_zone(128, 0, note, 100)
        if z is None:
            return None
        zi = self._info(z)
        if z.scaleTuning == 0:
            zi.root = note
        return zi


def _lowpass(x, a):
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc += a * (x[i] - acc)
        y[i] = acc
    return y


class SynthSource:
    """Fallback bank: single-cycle looped waveforms per GM family and noise-burst drums."""
    name = "synth"
    DRUM_SR = 22050.0
    CYC = 128

    def _cycle(self, recipe):
        n = self.CYC
        ph = np.arange(n) / n
        k = np.arange(1, 40)

        def add(amps):
            return sum(a * np.sin(2 * np.pi * (i + 1) * ph) for i, a in enumerate(amps) if a)

        rec = {
            "piano": (add([1, .6, .4, .3, .2, .12, .07, .04, .02]), 0.002, 8.0, 100.0, 0.25),
            "epiano": (add([1, .1, .35, .05, .12, .02]), 0.002, 4.0, 100.0, 0.2),
            "clav": (add([1, .8, .7, .6, .5, .4, .3, .2, .1]), 0.001, 1.5, 100.0, 0.1),
            "bell": (add([1, 0, .5, 0, .3, 0, .2, 0, .1]), 0.001, 3.0, 100.0, 0.4),
            "mallet": (add([1, 0, .2, 0, .04]), 0.001, 1.2, 100.0, 0.15),
            "organ": (add([1, 0.8, 0.0, 0.5, 0, 0.3, 0, 0.2]), 0.01, 0.1, 0.0, 0.08),
            "pluck": (add([1, .7, .5, .35, .25, .17, .1, .06]), 0.001, 3.0, 100.0, 0.15),
            "distgtr": (np.clip(2.5 * add([1, .8, .6, .5, .4, .3, .2, .1]), -1, 1), 0.002, 0.2, 0.0, 0.15),
            "sine": (add([1]), 0.002, 1.5, 100.0, 0.2),
            "bass": (add([1, .55, .3, .15, .08]), 0.003, 3.0, 40.0, 0.12),
            "synbass": (np.where(ph < 0.5, 1.0, -1.0) * 0.7 + add([1, 0, .3]) * 0.2, 0.002, 0.2, 0.0, 0.1),
            "strings": (sum(np.sin(2 * np.pi * k_ * ph) / k_ for k_ in range(1, 20)), 0.12, 0.1, 0.0, 0.25),
            "synstr": (sum(np.sin(2 * np.pi * k_ * ph) / k_ for k_ in range(1, 20)) * 0.8, 0.2, 0.1, 0.0, 0.3),
            "choir": (add([1, .6, .8, .3, .15, .1]), 0.15, 0.1, 0.0, 0.3),
            "timpani": (add([1, .3, .1]), 0.001, 1.0, 100.0, 0.2),
            "brass": (sum(np.sin(2 * np.pi * k_ * ph) / k_ ** 0.8 for k_ in range(1, 20)), 0.04, 0.1, 0.0, 0.12),
            "horn": (add([1, .7, .4, .25, .12, .05]), 0.06, 0.1, 0.0, 0.15),
            "flute": (add([1, .25, .08, .03]), 0.05, 0.1, 0.0, 0.12),
            "square": (np.where(ph < 0.5, 1.0, -1.0) * 1.0, 0.005, 0.1, 0.0, 0.1),
            "saw": (2.0 * ph - 1.0, 0.005, 0.1, 0.0, 0.1),
            "pad": (add([1, .5, .35, .25, .12]) + 0.4 * np.sin(4 * np.pi * ph + 1), 0.25, 0.1, 0.0, 0.4),
        }[recipe]
        w, att, dec, sus, rel = rec
        w = np.asarray(w, dtype=np.float64)
        w = w / max(1e-9, np.abs(w).max())
        return w, dict(attack=att, decay=dec, sustain=sus, release=rel)

    def melodic(self, prog, key, recipe):
        if recipe == "noise":
            rng = np.random.RandomState(prog)
            n = int(0.4 * 22050)
            x = _lowpass(rng.uniform(-1, 1, n), 0.25 if prog != 119 else 0.7)
            x = x / np.abs(x).max()
            return ZoneInfo(("noise", prog), x, 0, n, True, 22050.0, 60, 0, 0, 0,
                            dict(attack=0.05, decay=0.1, sustain=0.0, release=0.2))
        w, env = self._cycle(recipe)
        sr = self.CYC * 440.0 * 2 ** ((60 - 69) / 12.0)
        return ZoneInfo(("cyc", recipe), w, 0, len(w), True, sr, 60, 0, 0, 0, env)

    def drum(self, note):
        sr = self.DRUM_SR
        rng = np.random.RandomState(note)
        kind = DRUM_KIND.get(note, DRUM_DEFAULT)[0]
        n = int(DRUM_KIND.get(note, DRUM_DEFAULT)[1] * sr)
        t = np.arange(n) / sr
        f = 440.0 * 2 ** ((note - 69) / 12.0)
        noise = rng.uniform(-1, 1, n)

        def hp(x):
            return x - _lowpass(x, 0.35)

        if kind == "kick":
            fr = 45 + 90 * np.exp(-t / 0.03)
            x = np.sin(2 * np.pi * np.cumsum(fr) / sr) * np.exp(-t / 0.09)
            x[:40] += noise[:40] * 0.4
        elif kind == "snare":
            x = (hp(noise) * np.exp(-t / 0.05) * 0.8 + np.sin(2 * np.pi * 190 * t) * np.exp(-t / 0.03) * 0.6)
        elif kind == "tom":
            fr = 2 * f * (1 + 0.5 * np.exp(-t / 0.04))
            x = np.sin(2 * np.pi * np.cumsum(fr) / sr) * np.exp(-t / 0.12)
        elif kind in ("hat", "ohat"):
            x = hp(hp(noise)) * np.exp(-t / (0.02 if kind == "hat" else 0.10))
        elif kind == "cym":
            x = hp(hp(noise)) * np.exp(-t / 0.30) + 0.2 * np.sin(2 * np.pi * 4200 * t) * np.exp(-t / 0.2)
        else:
            k = (note * 7) % 5
            fr = [880, 560, 1500, 2300, 3300][k]
            if note in (39, 54, 69, 70, 82, 58, 73, 74):
                x = hp(noise) * np.exp(-t / 0.06) * (1 + 0.7 * np.sin(2 * np.pi * 45 * t))
            else:
                x = np.sin(2 * np.pi * fr * t) * np.exp(-t / 0.06) + 0.5 * np.sin(2 * np.pi * fr * 1.5 * t) * np.exp(-t / 0.04)
        x = x / max(1e-9, np.abs(x).max())
        return ZoneInfo(("drum", note), x, 0, n, False, sr, note, 0, 0, 0)


# ------------------------------------------------------------------------------------------
# sample preparation
# ------------------------------------------------------------------------------------------


def _fade(n, up=True):
    f = 0.5 - 0.5 * np.cos(np.pi * (np.arange(n) + 0.5) / n)
    return f if up else f[::-1]


def resample_to(x, n_out):
    """Non-periodic resample of x to n_out samples (FIR low-pass when decimating, Catmull-Rom interpolation)."""
    x = np.asarray(x, dtype=np.float64)
    n_in = len(x)
    if n_out <= 0 or n_in == 0:
        return np.zeros(max(n_out, 0))
    if n_in == n_out:
        return x.copy()
    ratio = n_in / n_out
    if ratio > 1.0:
        fc = 0.45 / ratio  # cycles/sample
        m = int(min(n_in // 2 - 1, 8 * ratio)) | 1
        k = np.arange(-(m // 2), m // 2 + 1)
        h = np.sinc(2 * fc * k) * np.hanning(len(k))
        h /= h.sum()
        pad = len(h) // 2
        xp = np.concatenate([x[pad:0:-1] if pad < n_in else np.zeros(pad), x, x[-2:-pad - 2:-1] if pad < n_in else np.zeros(pad)])
        if len(xp) != n_in + 2 * pad:
            xp = np.concatenate([np.full(pad, x[0]), x, np.full(pad, x[-1])])
        L = len(xp) + len(h)
        nf = 1 << (L - 1).bit_length()
        y = np.fft.irfft(np.fft.rfft(xp, nf) * np.fft.rfft(h, nf), nf)
        x = y[2 * pad:2 * pad + n_in]
    pos = np.arange(n_out) * ratio
    i1 = np.floor(pos).astype(np.int64)
    fr = pos - i1
    xe = np.concatenate([[x[0]], x, [x[-1], x[-1]]])
    p0, p1, p2, p3 = xe[i1], xe[i1 + 1], xe[i1 + 2], xe[i1 + 3]
    return p1 + 0.5 * fr * (p2 - p0 + fr * (2 * p0 - 5 * p1 + 4 * p2 - p3 + fr * (3 * (p1 - p2) + p3 - p0)))


def resample_periodic(x, n_out):
    """Resample one period of a periodic signal to exactly n_out samples (spectral, wrap-continuous)."""
    x = np.asarray(x, dtype=np.float64)
    n_in = len(x)
    if n_in == n_out:
        return x.copy()
    X = np.fft.rfft(x)
    m = min(n_in, n_out) // 2
    Y = np.zeros(n_out // 2 + 1, dtype=np.complex128)
    Y[:m] = X[:m]
    if n_out < n_in:
        t0 = int(m * 0.85)
        Y[t0:m] *= np.linspace(1.0, 0.0, m - t0, endpoint=False)
    return np.fft.irfft(Y, n_out) * (n_out / n_in)


class Prepared:
    def __init__(self, pcm, looped, loop_block, rate):
        self.pcm, self.looped, self.loop_block, self.rate = pcm, looped, loop_block, rate
        self.nblk = (len(pcm) + 27) // 28
        self.size = self.nblk * 16 + (0 if looped else 16)


def _to_int16(x):
    pk = np.abs(x).max()
    if pk > 0:
        x = x * (0.9 * 32767.0 / pk)
    return np.clip(np.round(x), -32768, 32767).astype(np.int16)


def prepare_loop(z, rate_t, head_s, win_s):
    data = z.pcm[:z.le]
    ls, le, sr = z.ls, z.le, z.sr
    L = le - ls
    rho_t = rate_t / sr
    X = max(8, int(0.015 * sr))
    winmax = max(16, int(win_s * sr))
    if L <= winmax:
        k = max(1, int(np.ceil(112 / (L * rho_t))))
        k = min(k, max(1, winmax // L))
        W = k * L
        window = np.tile(data[ls:le], k)
        q = ls
    else:
        W = max(16, min(winmax, L - X))
        q = le - W
        window = data[q:le].copy()
        Xc = min(X, q - ls) if q - ls > 0 else 0
        if Xc > 0:
            f = _fade(Xc)
            window[W - Xc:] = window[W - Xc:] * (1 - f) + data[q - Xc:q] * f
    n_loop = max(28, int(round(W * rho_t / 28.0)) * 28)
    rho = n_loop / W
    loop = resample_periodic(window, n_loop)
    H = min(q, int(head_s * sr))
    head = data[:H].copy()
    if 0 < H < q:
        Xc = min(X, H)
        f = _fade(Xc)
        head[H - Xc:] = head[H - Xc:] * (1 - f) + data[q - Xc:q] * f
    n_head = int(round(H * rho))
    hres = resample_to(head, n_head) if n_head > 0 else np.zeros(0)
    pad = (-n_head) % 28
    full = np.concatenate([np.zeros(pad), hres, loop])
    rate = int(round(rho * sr))
    return full, (pad + n_head) // 28, rate


def prepare_oneshot(z, rate_t, tmax_s, fade_s=0.040):
    data = z.pcm[:z.le] if z.looped else z.pcm
    n = min(len(data), int(tmax_s * z.sr))
    data = data[:n].copy()
    nf = min(int(fade_s * z.sr), n // 2)
    if nf > 0:
        data[n - nf:] *= _fade(nf, up=False)
    n_out = max(28, int(round(n * rate_t / z.sr)))
    return resample_to(data, n_out), rate_t


class Job:
    """One shared sample: a ZoneInfo plus quality levels."""

    def __init__(self, zi, kind, rate0, tmax, weight):
        self.zi, self.kind, self.weight, self.tmax = zi, kind, weight, tmax
        self.looped = kind == "mel" and zi.looped
        rates = [r for r in RATES if r <= rate0] or [RATES[-1]]
        lv = {}
        for r in rates:
            for s in (1.0, 0.75, 0.55):
                lv[(r, s)] = r * s
        self.levels = [k for k, _ in sorted(lv.items(), key=lambda kv: -kv[1])]
        self.level = 0
        self._cache = {}

    def prepared(self, level=None):
        level = self.level if level is None else level
        if level not in self._cache:
            r, s = self.levels[level]
            z = self.zi
            if self.looped:
                full, lb, rate = prepare_loop(z, r, max(0.08, HEAD_S * s), max(0.06, WIN_S * s))
                self._cache[level] = Prepared(_to_int16(full), True, lb, rate)
            else:
                x, rate = prepare_oneshot(z, r, self.tmax * (s if self.kind == "mel" else max(s, 0.75)))
                self._cache[level] = Prepared(_to_int16(x), False, 0, rate)
        return self._cache[level]

    def size(self, level=None):
        return self.prepared(level).size


# ------------------------------------------------------------------------------------------
# bank construction
# ------------------------------------------------------------------------------------------


class Region:
    pass


def analyse_songs(songs):
    """Return (prog_notes: {prog: [notes]}, drum_counts: {note: n}, total notes)."""
    prog_notes, drums = {}, {}
    for song, _ in songs.values():
        for t, ch, note, vel, dur, prog in song.notes:
            if ch == 9:
                drums[note] = drums.get(note, 0) + 1
            else:
                prog_notes.setdefault(prog, []).append(note)
    return prog_notes, drums


def group_for_programs(progs):
    """prog -> group index; unlisted programs join the closest listed program of the same GM family."""
    m = {}
    for gi, (_, _, plist, _, _) in enumerate(GROUPS):
        for p in plist:
            m[p] = gi
    for p in sorted(progs):
        if p not in m:
            cands = [q for q in m if q // 8 == p // 8] or list(m)
            m[p] = m[min(cands, key=lambda q: abs(q - p))]
    return m


def build_bank(songs, src, verbose=True):
    prog_notes, drum_counts = analyse_songs(songs)
    pmap = group_for_programs(prog_notes.keys())
    jobs = {}  # key -> Job

    def job_for(zi, kind, rate0, tmax, weight):
        key = (zi.key, kind, rate0, round(tmax, 3))
        j = jobs.get(key)
        if j is None:
            j = jobs[key] = Job(zi, kind, rate0, tmax, weight)
        else:
            j.weight += weight
        return j

    regions = []  # Region objects, melodic first
    prog_regions = {}  # group idx -> (first region idx, count)
    for gi, (gname, srcprog, plist, cls, recipe) in enumerate(GROUPS):
        notes = sorted(n for p in plist for n in prog_notes.get(p, []))
        if not notes:
            continue
        a = np.array(notes)
        med = int(np.median(a))
        p10, p90 = np.percentile(a, 10), np.percentile(a, 90)
        halves = [(0, 127, med, len(a))]
        if p90 - p10 > 30:
            lo = a[a <= med]
            hi = a[a > med]
            if len(lo) and len(hi):
                halves = [(0, med, int(np.median(lo)), len(lo)), (med + 1, 127, int(np.median(hi)), len(hi))]
        rate0 = CLASSES[cls][0]
        first = len(regions)
        for klo, khi, kmed, cnt in halves:
            zi = src.melodic(srcprog, kmed) if isinstance(src, SF2Source) else src.melodic(srcprog, kmed, recipe)
            r = Region()
            r.kind, r.group, r.cls = "mel", gname, cls
            r.klo, r.khi = klo, khi
            r.zi = zi
            r.job = job_for(zi, "mel", rate0, 0.5, cnt)
            r.weight = cnt
            if regions and len(regions) > first and regions[-1].zi.key == zi.key:
                regions[-1].khi = khi  # both halves resolved to the same sample: merge
                regions[-1].weight += cnt
                continue
            regions.append(r)
        prog_regions[gi] = (first, len(regions) - first)
    nmel = len(regions)
    # drums
    drum_region = [0xFF] * 128
    drum_first = len(regions)
    for note in sorted(drum_counts):
        if note >= 128:
            continue
        zi = src.drum(note)
        if zi is None:
            continue
        kind, tmax, rate0 = DRUM_KIND.get(note, DRUM_DEFAULT)
        r = Region()
        r.kind, r.group, r.cls = "drum", "drum%d" % note, "drum"
        r.klo = r.khi = note
        r.zi = zi
        r.dkind = kind
        r.job = job_for(zi, "drum", rate0, tmax, drum_counts[note])
        r.weight = drum_counts[note]
        regions.append(r)
        drum_region[note] = len(regions) - 1
    assert len(regions) <= MAX_REGIONS, len(regions)

    # ---- fit to the sample budget -----------------------------------------------------------
    jl = list(jobs.values())
    total = sum(j.size() for j in jl)
    while total > SAMPLE_BUDGET:
        best = None
        for j in jl:
            if j.level + 1 >= len(j.levels):
                continue
            saved = j.size() - j.size(j.level + 1)
            if saved <= 0:
                continue
            cost = (j.weight + 5) / saved
            if best is None or cost < best[0]:
                best = (cost, j)
        if best is None:
            raise RuntimeError("cannot fit samples in the budget (%d bytes)" % total)
        j = best[1]
        total -= j.size() - j.size(j.level + 1)
        j.level += 1

    # ---- ADPCM encode, loudness, ADSR --------------------------------------------------------
    blobs = []
    offs = {}
    off = 0
    for j in jl:
        p = j.prepared()
        j.blob = spuadpcm.encode(p.pcm, loop=p.looped, loop_start_block=p.loop_block)
        assert len(j.blob) == p.size
        offs[id(j)] = off
        blobs.append(j.blob)
        off += len(j.blob)
    data = b"".join(blobs)
    assert off == len(data) and off % 16 == 0

    # loudness references (RMS of melodic samples, class offsets)
    rms = {}
    for j in jl:
        x = j.prepared().pcm.astype(np.float64)
        rms[id(j)] = 20 * np.log10(max(1e-6, np.sqrt((x * x).mean()) / 32768.0))
    vals = [rms[id(r.job)] - CLASSES[r.cls][1] for r in regions if r.kind == "mel"]
    ref = float(np.percentile(vals, 15)) if vals else -20.0
    for r in regions:
        p = r.job.prepared()
        zi = r.zi
        if r.kind == "mel":
            db = ref + CLASSES[r.cls][1] - rms[id(r.job)]
        else:
            db = -4.0 - 0.05 * zi.atten  # half of the SF2 attenuation (samples are peak-normalised)
        r.gain = int(max(1, min(127, round(127 * 10 ** (min(0.0, db) / 20.0)))))
        r.rate = min(44100, p.rate)
        # root/fine: total cents offset -> semitone part into the root
        c = zi.fine
        semis = int(round(c / 100.0))
        r.root = max(0, min(127, zi.root - semis))
        r.fine = max(-100, min(100, int(round(c - 100 * semis))))
        r.flags = 1 if p.looped else 0
        r.off = offs[id(r.job)]
        r.size = len(r.job.blob)
        r.pan = max(-64, min(63, int(round(zi.pan / 500.0 * 63))))
        r.dur = 0 if p.looped else int(round(len(p.pcm) / float(p.rate) * TICK_HZ))
        env = dict(zi.env)
        if r.kind == "drum":
            env["relsec"] = DRUM_REL[r.dkind]
        r.adsr1, r.adsr2, r.rel = fit_adsr(env, p.looped, r.kind == "drum")
        r.rel = min(r.rel, 0xFFFF)

    nreg = len(regions)
    progfirst = [0] * 128
    prognum = [0] * 128
    for p, gi in pmap.items():
        if gi in prog_regions and 0 <= p < 128:
            progfirst[p], prognum[p] = prog_regions[gi]
    hdr_size = 0x190 + nreg * 28
    data_off = (hdr_size + 2047) // 2048 * 2048
    out = bytearray()
    out += b"MBK1" + struct.pack("<HHII", nreg, 0, data_off, len(data))
    out += bytes(progfirst) + bytes(prognum) + bytes(drum_region)
    assert len(out) == 0x190
    for r in regions:
        out += struct.pack("<IIHBBBbBBHHHHb3x", r.off, r.size, r.rate, r.klo, r.khi, r.root, r.fine, r.flags, r.gain,
                           r.adsr1, r.adsr2, r.rel, r.dur, r.pan)
    out += bytes(data_off - len(out))
    out += data
    info = dict(nregions=nreg, nmel=nmel, data_size=len(data), data_off=data_off, regions=regions, jobs=jl,
                ref=ref, rms=rms, progs=sorted(prog_notes), groups=prog_regions)
    return bytes(out), info


# ------------------------------------------------------------------------------------------
# public entry points + verification
# ------------------------------------------------------------------------------------------


def convert_songs(g):
    songs = {}
    for n in g.names(".MID"):
        s = convert_midi(g.read(n))
        songs[n] = (s, write_song(s))
    return songs


def make_source(verbose=True):
    path = find_sf2()
    if path and os.path.exists(path):
        return SF2Source(path), path
    return SynthSource(), None


def add_music(g, pack, verbose=True, songs=None):
    """Add MUSBANK and one entry per .MID to `pack` (see module docstring); returns a stats dict."""
    t0 = time.time()
    songs = songs or convert_songs(g)
    src, path = make_source(verbose)
    bank, info = build_bank(songs, src, verbose)
    pack.add("MUSBANK", bank, info["nregions"])
    tot = 0
    sizes = {}
    for n, (s, blob) in songs.items():
        pack.add(n, blob, len(blob))
        sizes[n] = len(blob)
        tot += len(blob)
    info.update(songs=songs, song_bytes=tot, song_sizes=sizes, sf2=path, source=src, bank=bank)
    if verbose:
        print("MUSBANK: %d bytes (header %d + ADPCM %d), %d regions (%d melodic, %d drum), %d samples; source: %s; %.1fs"
              % (len(bank), info["data_off"], info["data_size"], info["nregions"], info["nmel"],
                 info["nregions"] - info["nmel"], len(info["jobs"]), path or "built-in synthesis", time.time() - t0))
        print("songs: " + ", ".join("%s %d" % (n, b) for n, b in sizes.items()))
        print("songs total %d bytes in %d files (max %d)" % (tot, len(sizes), max(sizes.values())))
    return info


def verify_bank(bank):
    """Parse MUSBANK exactly per the documented layout; returns dict of per-region sample data views."""
    assert bank[:4] == b"MBK1"
    nreg, z, data_off, data_size = struct.unpack("<HHII", bank[4:16])
    assert z == 0 and nreg <= 120 and data_off % 2048 == 0 and data_size % 16 == 0
    assert data_off >= 0x190 + nreg * 28 and len(bank) == data_off + data_size
    progfirst, prognum, drum = bank[0x10:0x90], bank[0x90:0x110], bank[0x110:0x190]
    regs = []
    for i in range(nreg):
        (off, size, rate, klo, khi, root, fine, flags, gain, a1, a2, rel, dur, pan) = struct.unpack_from(
            "<IIHBBBbBBHHHHb", bank, 0x190 + i * 28)
        assert off % 16 == 0 and size % 16 == 0 and off + size <= data_size, i
        assert 0 < rate <= 44100 and klo <= khi <= 127 and root < 128 and -100 <= fine <= 100
        assert flags in (0, 1) and 1 <= gain <= 127 and 2 <= rel <= 120
        assert (dur == 0) == (flags == 1) and -64 <= pan <= 63
        regs.append(dict(off=off, size=size, rate=rate, klo=klo, khi=khi, root=root, fine=fine, flags=flags,
                         gain=gain, adsr1=a1, adsr2=a2, rel=rel, dur=dur, pan=pan,
                         data=bank[data_off + off:data_off + off + size]))
        blk = regs[-1]["data"]
        if flags & 1:
            assert blk[1] & 4 or any(blk[b + 1] & 4 for b in range(0, size, 16)), i
            assert blk[size - 15] == 3, "looped sample must end with flag 3"
        else:
            assert blk[size - 15] == 1, "one-shot sample must end with flag 1"
    nmapped = 0
    for p in range(128):
        n = prognum[p]
        if not n:
            continue
        nmapped += 1
        f = progfirst[p]
        assert f + n <= nreg
        keys = 0
        for k in range(n):
            r = regs[f + k]
            assert r["klo"] == keys, ("program %d keys do not tile" % p)
            keys = r["khi"] + 1
        assert keys == 128, "program %d tiles to %d" % (p, keys)
    ndrum = 0
    for note in range(128):
        if drum[note] != 0xFF:
            assert drum[note] < nreg
            ndrum += 1
    return dict(regions=regs, nmapped=nmapped, ndrum=ndrum)


def selftest(g, info, verbose=True):
    songs = info["songs"]
    n = selftest_songs(g, songs)
    print("songs verified: %d songs, %d notes (note multiset+timing vs independent MIDI reader; END/length checked)"
          % (len(songs), n))
    v = verify_bank(info["bank"])
    errs = []
    rows = []
    for i, r in enumerate(v["regions"]):
        dec = spuadpcm.decode(r["data"])
        reg = info["regions"][i]
        p = reg.job.prepared()
        e = spuadpcm.rms_error(p.pcm, dec)
        pk = float(np.sqrt((p.pcm.astype(np.float64) ** 2).mean())) or 1.0
        errs.append(e)
        rows.append((i, reg.group, reg.kind, r["klo"], r["khi"], r["root"], r["fine"], r["rate"], r["size"], r["flags"],
                     r["gain"], r["adsr1"], r["adsr2"], r["rel"], r["dur"], r["pan"], e, pk))
        assert len(dec) >= len(p.pcm)
        d = r["data"]
        flags = [d[b + 1] for b in range(0, len(d), 16)]
        if p.looped:
            assert flags.count(4) == 1 and flags[p.loop_block] == 4 and flags[-1] == 3 and flags.count(3) == 1, i
            assert len(p.pcm) % 28 == 0 and p.loop_block * 28 < len(p.pcm)
        else:
            assert flags[-1] == 1 and not any(flags[:-1]), i
    if verbose:
        print(" #  group      kind keys     root fine  rate  bytes L gain adsr1 adsr2  rel dur  pan  rmserr  rms")
        for r in rows:
            print("%3d %-10s %-4s %3d-%-3d %4d %4d %5d %6d %d %4d %04x  %04x %4d %3d %4d %7.1f %6.0f" % r)
    mean = float(np.mean(errs))
    snr = float(np.mean([20 * np.log10(r[17] / max(r[16], 1e-9)) for r in rows]))
    print("bank verified: %d regions, %d programs mapped, %d drum notes; ADPCM %d bytes; mean RMS error %.1f LSB "
          "(mean SNR %.1f dB)" % (len(v["regions"]), v["nmapped"], v["ndrum"], info["data_size"], mean, snr))
    return mean


def _load_grp(path):
    spec = importlib.util.spec_from_file_location("duke_grp", os.path.join(_HERE, "grp.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m.Grp(path)


class _Pack:
    def __init__(self):
        self.entries = []

    def add(self, name, data, aux=0):
        self.entries.append((name.upper(), bytes(data), aux))


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 1
    g = _load_grp(args[0])
    pack = _Pack()
    t0 = time.time()
    info = add_music(g, pack)
    ok_bank = len(info["bank"]) <= 160 * 1024
    assert ok_bank, "bank too large"
    if "--selftest" in argv:
        selftest(g, info)
    print("total %.1fs" % (time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))


