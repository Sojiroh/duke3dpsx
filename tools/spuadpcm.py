"""PlayStation SPU-ADPCM ("VAG") encoder/decoder used by mkdata.py / mkmusic.py.

Block = 16 bytes: byte0 = (filter << 4) | shift, byte1 = flags, bytes 2..15 = 28 4-bit samples
(low nibble first).  Flags: 0x01 = loop end / sample end, 0x02 = repeat (with 0x01: jump to the
loop start and keep playing; without 0x02 the voice is muted), 0x04 = loop start marker.

  one-shot :  ... [0x00] [0x00] [0x01 terminator]
  looped   :  [0x04 first loop block] ... [0x03 last loop block]
"""
import numpy as np

F0 = (0, 60, 115, 98, 122)
F1 = (0, 0, -52, -55, -60)

FLAG_END = 0x01
FLAG_REPEAT = 0x02
FLAG_LOOPSTART = 0x04


def _open_loop_shifts(x, nblk):
    """Per-filter, per-block finest shift whose quantiser range covers the open-loop residual.
    Returns int array [5][nblk]."""
    xx = np.zeros(nblk * 28 + 2, dtype=np.float64)
    xx[2:] = x
    cur = x.reshape(nblk, 28).astype(np.float64)
    p1 = xx[1:-1].reshape(nblk, 28)
    p2 = xx[:-2].reshape(nblk, 28)
    shifts = np.zeros((5, nblk), dtype=np.int32)
    for f in range(5):
        res = cur - (p1 * F0[f] + p2 * F1[f]) / 64.0
        mx = np.abs(res).max(axis=1)
        sh = np.zeros(nblk, dtype=np.int32)
        done = np.zeros(nblk, dtype=bool)
        for s in range(12, 0, -1):  # finest step first
            ok = (mx <= 7.2 * (1 << (12 - s))) & ~done
            sh[ok] = s
            done |= ok
        shifts[f] = sh
    return shifts


def _enc_block(xl, base, f, sh, s1, s2):
    f0, f1 = F0[f], F1[f]
    step = 1 << (12 - sh)
    half = step >> 1
    nib = []
    err = 0
    for i in range(base, base + 28):
        v = xl[i]
        pred = (s1 * f0 + s2 * f1 + 32) >> 6
        q = (v - pred + half) // step
        if q > 7:
            q = 7
        elif q < -8:
            q = -8
        r = pred + q * step
        if r > 32767:
            r = 32767
        elif r < -32768:
            r = -32768
        d = v - r
        err += d * d
        s2, s1 = s1, r
        nib.append(q & 15)
    return err, nib, s1, s2


def encode(pcm, loop=False, loop_start_block=0):
    """Encode int16 mono PCM to SPU-ADPCM (closed-loop choice of filter per block).

    loop=False: returns audio blocks + one silent terminator block (flag 0x01).
    loop=True : the whole signal repeats from block `loop_start_block`; that block is flagged
                0x04 and the last block 0x03, no terminator.  len(pcm) is padded to a multiple
                of 28 (callers should pass loop lengths that already are).  The loop-start block
                is encoded with filter 0 so that it does not depend on the history at the loop end.
    Returns bytes."""
    x = np.asarray(pcm, dtype=np.int64)
    n = len(x)
    nblk = max(1, (n + 27) // 28)
    if n < nblk * 28:
        x = np.concatenate([x, np.zeros(nblk * 28 - n, dtype=np.int64)])
    x = np.clip(x, -32768, 32767)
    shifts = _open_loop_shifts(x, nblk)
    xl = x.tolist()
    out = bytearray()
    s1 = s2 = 0
    for b in range(nblk):
        base = b * 28
        if loop and b == loop_start_block:
            cands = [(0, int(shifts[0][b]))]
        else:
            cands = []
            for f in range(5):
                sh = int(shifts[f][b])
                cands.append((f, sh))
                if sh < 12:
                    cands.append((f, sh + 1))  # finer step: may clip a few samples but lowers noise
        best = None
        for f, sh in cands:
            r = _enc_block(xl, base, f, sh, s1, s2)
            if best is None or r[0] < best[0][0]:
                best = (r, f, sh)
        (err, nib, s1, s2), f, sh = best
        flag = 0
        if loop:
            if b == loop_start_block:
                flag |= FLAG_LOOPSTART
            if b == nblk - 1:
                flag |= FLAG_END | FLAG_REPEAT
        out.append((f << 4) | sh)
        out.append(flag)
        for i in range(0, 28, 2):
            out.append(nib[i] | (nib[i + 1] << 4))
    if not loop:
        out += bytes([0, FLAG_END]) + bytes(14)
    return bytes(out)


def decode(data):
    """Decode SPU-ADPCM bytes to int16 numpy array (ignores flags; stops after an end block
    only for terminator-style streams: stops at the first block with flag&1 that is silent)."""
    s1 = s2 = 0
    out = []
    for b in range(0, len(data) - 15, 16):
        hdr = data[b]
        f, sh = hdr >> 4, hdr & 15
        if f > 4:
            f = 4
        if sh > 12:
            sh = 9
        f0, f1 = F0[f], F1[f]
        for i in range(14):
            byte = data[b + 2 + i]
            for nib in (byte & 15, byte >> 4):
                if nib > 7:
                    nib -= 16
                v = ((nib << 12) >> sh) + ((s1 * f0 + s2 * f1 + 32) >> 6)
                v = max(-32768, min(32767, v))
                s2, s1 = s1, v
                out.append(v)
    return np.array(out, dtype=np.int16)


def resample(x, src_rate, dst_rate):
    """Linear-interpolating resampler with a simple box low-pass for down-sampling."""
    x = np.asarray(x, dtype=np.float64)
    if src_rate == dst_rate or len(x) < 2:
        return x
    ratio = src_rate / dst_rate
    if ratio > 1.0:
        k = int(round(ratio))
        if k >= 2:
            # averaging filter of width ~ratio before decimation (anti-alias)
            w = np.ones(k) / k
            x = np.convolve(x, w, mode="same")
    n = int(len(x) / ratio)
    pos = np.arange(n) * ratio
    i0 = np.floor(pos).astype(np.int64)
    fr = pos - i0
    i1 = np.minimum(i0 + 1, len(x) - 1)
    return x[i0] * (1 - fr) + x[i1] * fr


def rms_error(src, decoded):
    """RMS error between float/int source and decoded int16, over the common length (LSB units)."""
    n = min(len(src), len(decoded))
    d = np.asarray(src[:n], dtype=np.float64) - decoded[:n].astype(np.float64)
    return float(np.sqrt((d * d).mean())) if n else 0.0
