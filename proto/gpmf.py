"""Minimal GPMF (GoPro metadata) parser for prototyping.

GPMF is a KLV stream: 4-byte key, 1-byte type, 1-byte struct size, 2-byte repeat (big endian),
payload padded to 4 bytes. Type 0 means nested KLVs.
"""
import struct
import subprocess
import sys

import numpy as np

TYPES = {
    'b': ('b', 1), 'B': ('B', 1), 's': ('h', 2), 'S': ('H', 2), 'l': ('i', 4), 'L': ('I', 4),
    'f': ('f', 4), 'd': ('d', 8), 'j': ('q', 8), 'J': ('Q', 8), 'q': ('i', 4), 'Q': ('q', 8),
}


def parse(buf, off=0, end=None):
    """Yield (key, type, size, repeat, payload_bytes or children) tuples."""
    end = len(buf) if end is None else end
    out = []
    while off + 8 <= end:
        key = buf[off:off + 4].decode('latin1')
        typ = chr(buf[off + 4])
        size = buf[off + 5]
        rep = struct.unpack('>H', buf[off + 6:off + 8])[0]
        n = size * rep
        payload = buf[off + 8:off + 8 + n]
        if typ == '\0':
            out.append((key, typ, size, rep, parse(buf, off + 8, off + 8 + n)))
        else:
            out.append((key, typ, size, rep, payload))
        off += 8 + ((n + 3) & ~3)
    return out


def values(typ, size, rep, payload, typedef=None):
    if typ == 'c':
        return payload.decode('latin1').rstrip('\0')
    if typ == '?' and typedef:
        fmt = '>' + ''.join(TYPES[c][0] for c in typedef)
        return [struct.unpack(fmt, payload[i * size:(i + 1) * size]) for i in range(rep)]
    if typ in TYPES:
        f, w = TYPES[typ]
        cnt = size // w
        arr = np.frombuffer(payload, dtype='>' + {'b': 'i1', 'B': 'u1', 'h': 'i2', 'H': 'u2', 'i': 'i4', 'I': 'u4',
                                                   'f': 'f4', 'd': 'f8', 'q': 'i8', 'Q': 'u8'}[f]).astype(np.float64)
        return arr.reshape(rep, cnt) if cnt > 1 else arr
    return payload


def streams(path):
    """Return {key: [(payload_index, scaled ndarray, info dict)]} for all STRM in the file."""
    data = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-map', '0:d:1', '-c', 'copy', '-f', 'data', '-'],
                          capture_output=True, check=True).stdout
    res = {}
    for pi, (k, t, s, r, devc) in enumerate(parse(data)):
        if k != 'DEVC':
            continue
        for k2, t2, s2, r2, strm in devc:
            if k2 != 'STRM':
                continue
            info = {}
            scal = None
            typedef = None
            for k3, t3, s3, r3, p3 in strm:
                if k3 in ('STNM', 'SIUN', 'UNIT', 'ORIN', 'ORIO', 'TYPE'):
                    info[k3] = values(t3, s3, r3, p3)
                elif k3 == 'SCAL':
                    scal = values(t3, s3, r3, p3)
                elif k3 in ('STMP', 'TSMP', 'TICK', 'TOCK', 'EMPT'):
                    info[k3] = values(t3, s3, r3, p3)
                elif t3 != '\0' and len(k3) == 4 and k3.isupper() and r3 >= 1 and k3 not in info:
                    v = values(t3, s3, r3, p3, info.get('TYPE'))
                    if scal is not None and isinstance(v, np.ndarray):
                        sc = np.asarray(scal, dtype=np.float64)
                        v = v / (sc if sc.size == (v.shape[-1] if v.ndim > 1 else 1) or sc.size == 1 else sc[0])
                    res.setdefault(k3, []).append((pi, v, dict(info)))
    return res


if __name__ == '__main__':
    s = streams(sys.argv[1])
    for k, lst in s.items():
        v = lst[0][1]
        shp = getattr(v, 'shape', None)
        print(k, len(lst), shp, lst[0][2].get('STNM', ''), lst[0][2].get('SIUN', lst[0][2].get('UNIT', '')),
              lst[0][2].get('ORIN', ''), str(v[:2] if hasattr(v, '__getitem__') else v)[:120].replace('\n', ' '))
