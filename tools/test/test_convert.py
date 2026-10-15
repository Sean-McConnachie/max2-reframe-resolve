"""Tests for the GoPro Reframe -> Max2 Reframe timeline converter and the plugin's export search.

Run after build.bat:
    python tools\\test\\test_convert.py [--data DIR] [--pictures]

--data      folder with 05_Eingang.drt (a timeline made with GoPro Reframe), optionally 05_Eingang-sample.drt (the
            same timeline with Max2 Reframe clips added in Resolve), and GS012039.360. Default: data\\ of the repo.
            The clips of 05_Eingang.drt must be on their drive (the converter looks for the .360 files).
--pictures  also compare pictures with the real GoPro Reframe plugin (needs it installed, takes a few minutes).

Needs Python with numpy, opencv-python and zstandard.
"""
import argparse, os, re, shutil, struct, subprocess, sys, tempfile, zipfile, zlib
import xml.dom.minidom

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(ROOT, 'build')
TEST = os.path.join(BUILD, 'convert_test.exe')
CONVERT = os.path.join(BUILD, 'Max2Convert.exe')
RENDER = os.path.join(BUILD, 'max2render.exe')
FFMPEG = shutil.which('ffmpeg') or r'C:\Users\Sean\Documents\ffmpeg-2025-12-22-git-c50e5c7778-full_build\bin\ffmpeg.exe'
GOPRO = r'C:\Program Files\Common Files\OFX\Plugins\GoPro Reframe for OpenFX\gopro-reframe.ofx.bundle\Contents\Win64\gopro-reframe.ofx'

failures = []


def check(cond, what):
    print(('  ok    ' if cond else '  FAIL  ') + what)
    if not cond:
        failures.append(what)


def run(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, **kw)


# ---------------------------------------------------------------- independent decoders (not the C++ code)

def unblob(h):
    import zstandard
    b = bytes.fromhex(h)
    if not b:
        return b''
    assert b[:4] == b'\0\0\0\2' and struct.unpack('>I', b[4:8])[0] == len(b) - 8
    if b[8] == 0x81:
        return zstandard.ZstdDecompressor().decompress(b[9:], max_output_size=1 << 26)
    assert b[8] == 0x80
    return b[9:]


def pb(b):
    """Protocol buffer fields as a list of (number, wire type, value)."""
    out, i = [], 0

    def varint():
        nonlocal i
        r = s = 0
        while True:
            c = b[i]; i += 1; r |= (c & 0x7f) << s; s += 7
            if c < 0x80:
                return r
    while i < len(b):
        k = varint(); f, t = k >> 3, k & 7
        if t == 0: out.append((f, t, varint()))
        elif t == 1: out.append((f, t, struct.unpack('<d', b[i:i + 8])[0])); i += 8
        elif t == 5: out.append((f, t, struct.unpack('<f', b[i:i + 4])[0])); i += 4
        elif t == 2:
            n = varint(); out.append((f, t, b[i:i + n])); i += n
        else:
            raise ValueError('wire type %d' % t)
    return out


def sub(fields, num):
    return [v for f, t, v in fields if f == num]


def skeleton(b, depth=0):
    """Field numbers and wire types of a message tree; values and repeated parameter lists left out."""
    try:
        fields = pb(b)
    except Exception:
        return None
    sk = []
    for f, t, v in fields:
        if t == 2 and depth < 6 and v and skeleton(v, depth + 1) is not None and not all(32 <= c < 127 for c in v):
            sk.append((f, t, skeleton(v, depth + 1)))
        else:
            sk.append((f, t))
    return tuple(sk)


def drt_files(path):
    z = zipfile.ZipFile(path)
    assert z.testzip() is None
    return {n: z.read(n) for n in z.namelist()}


def seq_name(files):
    return [n for n in files if n.startswith('SeqContainer/')][0]


def clips(xml_text):
    return re.findall(r'<Sm2TiVideoClip DbId="[^"]*">.*?</Sm2TiVideoClip>', xml_text, re.S)


def tag(el, t):
    m = re.search('<%s>([^<]*)</%s>' % (t, t), el)
    return m.group(1) if m else ''


def decode_comp(hexs):
    raw = bytes.fromhex(hexs)
    payload = zlib.decompress(raw[4:])
    i = payload.find(b'Compressed = true, }') + len(b'Compressed = true, }')
    return payload, i, zlib.decompress(payload[i + 5:]).decode()


def encode_comp(payload, i, tools):
    data = zlib.compress(tools.encode())
    new = payload[:i + 1] + struct.pack('<I', len(tools.encode())) + data
    # the byte-array value "0_data" is the last field: fix its length
    j = payload.find('0_data'.encode('utf-16-be'))
    k = j + 12 + 5  # name, type, null flag
    old_len = struct.unpack('>I', payload[k:k + 4])[0]
    assert k + 4 + old_len == len(payload)
    new = new[:k] + struct.pack('>I', len(new) - k - 4) + new[k + 4:]
    comp = zlib.compress(new)
    return (struct.pack('>I', len(new)) + comp).hex()


# ---------------------------------------------------------------- GoPro model (independent Python version)

def gopro_angle(r, zoom, lc):
    """GoPro Reframe view angle in degrees at r pixels from the center of a 3840 x 2160 output."""
    m = 3840 / 1.1
    distort = lc / 200.0
    radius = r * 100.0 / zoom
    fov = distort * 65.0
    fa = abs(fov) + 0.1
    fr = 0.785398 * fa / 45.0
    tfr = np.tan(fr)
    maxr = np.sqrt((m * m + (0.75 * m) ** 2) / 4.0)
    maxrv = 0.5 * m * 9 / 16 * fa / (57.2958 * np.arctan(np.tan(0.785398 * fa / 45)))
    a = np.clip(fov * 10000, 0, 1)
    val = np.clip(radius / maxr * fr, 0, 1.57)
    radius = (1 - a) * (maxr * np.tan(val) / tfr) + a * (maxrv * 57.2958 * np.arctan(radius / maxrv * tfr) / fa)
    rn = radius / maxr
    a2 = np.clip((rn - 0.9) * 10000, 0, 1); aa = np.clip((rn - 1.1) * 10000, 0, 1)
    phi = (1 - a2) * (rn * 85.6 - rn * rn * 10.91) + a2 * (-rn ** 3 * 13.9167 + rn * rn * 12.35 + rn * 76.2542 - 0.284)
    phi = (1 - aa) * phi + aa * (2 * 57.2957795131 * np.arctan(rn * 0.7630377885))
    return np.minimum(phi, 180.0)


def max2_angle(r, fov, curv):
    k = min(max(1 - min(max(curv, 0), 2) * 0.5, 0), 1)
    tm = min(np.radians(fov) / 2, np.pi)
    if k > 1e-4:
        tm = min(tm, np.pi / (2 * k) - 0.01)
    tm = max(tm, 0.001)
    rs = np.tan(k * tm) / k if k > 1e-4 else tm
    rho = r / 1920.0 * rs
    th = np.arctan(k * rho) / k if k > 1e-4 else rho
    return np.minimum(np.degrees(th), 180.0)


def frame_radii():
    ys, xs = np.mgrid[4:2160:8, 4:3840:8]
    return np.hypot(xs - 1920.0, ys - 1080.0).ravel()


def lens_error(zoom, lc, fov, curv, r=None):
    r = frame_radii() if r is None else r
    d = np.abs(max2_angle(r, fov, curv) - gopro_angle(r, zoom, lc))
    return d.mean(), d.max()


# Fusion BezierSpline, as Fusion evaluates it (independent of fusion.cpp)
def braced(text, at):
    """The text from the '{' at `at` to its matching '}' (both included)."""
    depth = 0
    for i in range(at, len(text)):
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        if depth == 0:
            return text[at:i + 1]
    raise ValueError('unbalanced braces')


def parse_splines(tools):
    out = {}
    for m in re.finditer(r'(\w+) = BezierSpline \{', tools):
        spline = braced(tools, m.end() - 1)
        kf = spline.index('KeyFrames = {') + len('KeyFrames = ')
        table = braced(spline, kf)
        keys = []
        for k in re.finditer(r'\[(-?[\d.]+)\] = \{', table):
            entry = braced(table, k.end() - 1)
            t = float(k.group(1))
            v = float(re.match(r'\{ (-?[\d.e+-]+)', entry).group(1))
            lh = re.search(r'LH = \{ (-?[\d.e+-]+), (-?[\d.e+-]+)', entry)
            rh = re.search(r'RH = \{ (-?[\d.e+-]+), (-?[\d.e+-]+)', entry)
            keys.append((t, v, tuple(map(float, lh.groups())) if lh else None, tuple(map(float, rh.groups())) if rh else None))
        out[m.group(1)] = sorted(keys)
    return out


def bezier_eval(keys, t):
    if t <= keys[0][0]: return keys[0][1]
    if t >= keys[-1][0]: return keys[-1][1]
    for a, b in zip(keys, keys[1:]):
        if a[0] <= t < b[0]:
            break
    dt = b[0] - a[0]
    p1 = (min(max(a[3][0], a[0]), b[0]), a[3][1]) if a[3] else (a[0] + dt / 3, a[1] + (b[1] - a[1]) / 3)
    p2 = (min(max(b[2][0], a[0]), b[0]), b[2][1]) if b[2] else (b[0] - dt / 3, b[1] - (b[1] - a[1]) / 3)
    bez = lambda q0, q1, q2, q3, s: (1-s)**3*q0 + 3*(1-s)**2*s*q1 + 3*(1-s)*s*s*q2 + s**3*q3
    lo, hi = 0.0, 1.0
    for _ in range(60):
        mid = (lo + hi) / 2
        if bez(a[0], p1[0], p2[0], b[0], mid) < t: lo = mid
        else: hi = mid
    return bez(a[1], p1[1], p2[1], b[1], (lo + hi) / 2)


def effect_params(effect_list_bytes):
    """Max2 Reframe parameters of an effect list: name -> (static value, [(time, value)...])."""
    for f, t, entry in pb(effect_list_bytes):
        e = pb(entry)
        body = pb(sub(e, 9)[0]) if sub(e, 9) else []
        if not body or not sub(body, 3):
            continue
        plugin = pb(sub(pb(sub(body, 3)[0]), 2)[0])
        if sub(plugin, 2)[0] != b'com.max2resolve.reframe':
            continue
        out = {}
        for p in sub(plugin, 5):
            q = pb(p)
            name = sub(q, 1)[0].decode()
            val = pb(sub(q, 2)[0])
            static = sub(val, 2)[0] if sub(val, 2) else sub(val, 3)[0]
            keys = []
            for k in sub(q, 3):
                kf = pb(k)
                time = sub(kf, 1)[0] if sub(kf, 1) else 0
                kv = pb(sub(pb(sub(kf, 2)[0]), 1)[0])
                keys.append((time, sub(kv, 2)[0]))
            out[name] = (static, keys)
        return out
    return None


def at_frame(param, t):
    static, keys = param
    if not keys:
        return static
    h = 2 * t  # keyframe times are half frames
    if h <= keys[0][0]: return keys[0][1]
    if h >= keys[-1][0]: return keys[-1][1]
    for (t0, v0), (t1, v1) in zip(keys, keys[1:]):
        if t0 <= h <= t1:
            return v0 + (v1 - v0) * (h - t0) / (t1 - t0)


# ---------------------------------------------------------------- tests

def test_model():
    print('GoPro lens model (C++ against the Python version)')
    worst = 0
    for zoom in (30, 45, 76.7, 100, 171.4, 300):
        for lc in (0, 37, 60, 98.6, 100):
            rs = [str(r) for r in range(0, 2300, 100)]
            got = list(map(float, run([TEST, 'angle', str(zoom), str(lc)] + rs).stdout.split()))
            want = gopro_angle(np.array(list(map(float, rs))), zoom, lc)
            worst = max(worst, float(np.max(np.abs(np.array(got) - want))))
    check(worst < 1e-4, 'goproAngle matches the Python model (max diff %.2g deg)' % worst)
    worst = 0
    for fov, curv in ((145, 0), (90, 0.4), (200, 1), (300, 2), (82, 0.965)):
        rs = [str(r) for r in range(0, 2300, 100)]
        got = list(map(float, run([TEST, 'max2angle', str(fov), str(curv)] + rs).stdout.split()))
        worst = max(worst, float(np.max(np.abs(np.array(got) - max2_angle(np.array(list(map(float, rs))), fov, curv)))))
    check(worst < 1e-4, 'max2Angle matches the Python model (max diff %.2g deg)' % worst)


def max2_radius(theta, fov, curv):
    """Distance from the center (pixels, 3840 wide) where Max2 shows a direction theta degrees off the axis."""
    k = min(max(1 - min(max(curv, 0), 2) * 0.5, 0), 1)
    tm = min(np.radians(fov) / 2, np.pi)
    if k > 1e-4:
        tm = min(tm, np.pi / (2 * k) - 0.01)
    tm = max(tm, 0.001)
    rs = np.tan(k * tm) / k if k > 1e-4 else tm
    th = np.radians(theta)
    if k > 1e-4:
        rho = np.where(k * th < np.pi / 2 - 1e-6, np.tan(np.minimum(k * th, np.pi / 2 - 1e-6)) / k, 1e9)
    else:
        rho = th
    return rho / rs * 1920


def lens_error_px(zoom, lc, fov, curv, r=None):
    """Mean and largest distance (output pixels, 3840 wide) between where GoPro and Max2 show the same direction."""
    r = frame_radii() if r is None else r
    px = np.minimum(np.abs(max2_radius(gopro_angle(r, zoom, lc), fov, curv) - r), 1e4)
    return px.mean(), px.max()


def test_fit():
    print('Lens fit over the GoPro range (errors in pixels of a 3840 x 2160 output)')
    r = frame_radii()
    rows = {}
    normal, wide, table = [], [], []
    for lc in (0, 25, 50, 75, 100):
        for zoom in (30, 45, 60, 80, 100, 120, 150, 200, 300):
            fov, curv, mean, mx = map(float, run([TEST, 'fit', str(zoom), str(lc)]).stdout.split())
            pm, px = lens_error_px(zoom, lc, fov, curv, r)
            rows[(zoom, lc)] = (fov, curv)
            line = 'zoom %5g curve %3g -> fov %6.2f curv %.3f: mean %.2f px, max %6.2f px' % (zoom, lc, fov, curv, pm, px)
            table.append(line)
            if abs(pm - mean) > 0.1 + 0.05 * pm:
                normal.append(line + ' (the C++ report says mean %.2f px)' % mean)
            # Max2's lens family follows GoPro closely, except at the edges of very wide views
            if fov <= 135:
                if pm > 4.5 or px > 15: normal.append(line)
            elif pm > 25 or px > 120:
                wide.append(line)
    print('          ' + '\n          '.join(table))
    check(not normal, 'views up to 135 degrees wide: mean 4.5 px, max 15 px' + ''.join('\n          ' + b for b in normal))
    check(not wide, 'wider views: mean 25 px, max 120 px' + ''.join('\n          ' + b for b in wide))
    jumps = []
    zs = (30, 45, 60, 80, 100, 120, 150, 200, 300)
    for lc in (0, 25, 50, 75, 100):
        f = [rows[(z, lc)][0] for z in zs]
        if any(b >= a for a, b in zip(f, f[1:])):
            jumps.append('curve %g: %s' % (lc, ['%.1f' % x for x in f]))
    check(not jumps, 'field of view falls steadily as GoPro zoom rises' + ''.join('\n          ' + j for j in jumps))


def test_splines(original):
    print('Fusion curves (C++ against the Python version)')
    files = drt_files(original)
    s = files[seq_name(files)].decode()
    expected = {}
    for idx, el in enumerate(clips(s), 1):
        h = tag(el, 'CompositionBA')
        if h:
            for name, keys in parse_splines(decode_comp(h)[2]).items():
                expected[(idx, name)] = keys
    out = run([TEST, 'splines', original]).stdout.splitlines()
    worst, n = 0, 0
    for line in out:
        parts = line.split()
        keys = expected.get((int(parts[0]), parts[1]))
        if keys is None:
            continue
        for tv in parts[2:]:
            t, v = map(float, tv.split('='))
            worst = max(worst, abs(v - bezier_eval(keys, t))); n += 1
    check(n > 100 and worst < 1e-6, 'curve values match (%d samples, max diff %.2g)' % (n, worst))


def convert(src, dst):
    r = run([TEST, 'convert', src, dst])
    rep = {'notes': [], 'clips': []}
    for line in r.stdout.splitlines():
        k, _, v = line.partition(' ')
        if k == 'note': rep['notes'].append(v)
        elif k == 'clip': rep['clips'].append(v)
        else: rep[k] = v
    return rep


def test_convert(original, tmp):
    print('Conversion of %s' % os.path.basename(original))
    out = os.path.join(tmp, 'converted.drt')
    rep = convert(original, out)
    check(rep.get('ok') == '1' and rep.get('gopro') == '14' and rep.get('converted') == '14',
          'all 14 GoPro Reframe clips converted (gopro %s, converted %s)' % (rep.get('gopro'), rep.get('converted')))
    check(not rep['notes'], 'no clip skipped' + ''.join('\n          ' + n for n in rep['notes']))
    a, b = drt_files(original), drt_files(out)
    check(list(a) == list(b), 'same zip entries in the same order')
    for n, d in b.items():
        try:
            xml.dom.minidom.parseString(d.replace(b'::', b'__'))
            good = True
        except Exception:
            good = False
        check(good, '%s is well-formed XML' % n)
    check(a['project.xml'] == b['project.xml'], 'project.xml unchanged')
    mpa, mpb = a['MediaPool/Master/MpFolder.xml'].decode(), b['MediaPool/Master/MpFolder.xml'].decode()
    check(mpb.replace('05_Eingang (Max2 Reframe)', '05_Eingang') == mpa and mpb.count('05_Eingang (Max2 Reframe)') == 2,
          'Media Pool unchanged except the timeline name "05_Eingang (Max2 Reframe)"')
    check(rep.get('timeline') == '05_Eingang (Max2 Reframe)', 'report gives the new timeline name')

    sa, sb = a[seq_name(a)].decode(), b[seq_name(b)].decode()
    ca, cb = clips(sa), clips(sb)
    check(len(ca) == len(cb), 'same number of clips (%d)' % len(cb))
    strip = lambda el: re.sub(r'<EffectFiltersBA>[0-9a-f]*</EffectFiltersBA>|<EffectFiltersBA/>', '<E/>', el)
    nocomp = lambda el: re.sub(r'\n\s*<CompositionTable>.*?</CompositionTable>', '', el, flags=re.S)
    same_other = all(x == y for x, y in zip(ca, cb) if '<CompositionTable>' not in x)
    check(same_other, 'clips without a GoPro composition are unchanged, byte for byte')
    only = all(strip(nocomp(x)) == strip(y) for x, y in zip(ca, cb) if '<CompositionTable>' in x)
    check(only, 'converted clips differ only in the effect list and the removed composition')
    outside = re.sub(r'<Sm2TiVideoClip DbId="[^"]*">.*?</Sm2TiVideoClip>', '', sa, flags=re.S) == \
        re.sub(r'<Sm2TiVideoClip DbId="[^"]*">.*?</Sm2TiVideoClip>', '', sb, flags=re.S)
    check(outside, 'everything outside the video clips is unchanged (tracks, audio, titles, transitions)')
    check('<CompositionTable>' not in sb, 'no Fusion composition is left')

    # each converted clip: the old effects kept, Max2 Reframe added with the GoPro view
    ok_effects, ok_static, ok_keys, details = True, True, True, []
    for x, y in zip(ca, cb):
        if '<CompositionTable>' not in x:
            continue
        old = unblob(tag(x, 'EffectFiltersBA'))
        new = unblob(tag(y, 'EffectFiltersBA'))
        if not new.startswith(old):
            ok_effects = False
        params = effect_params(new)
        payload, i, tools = decode_comp(tag(x, 'CompositionBA'))
        splines = parse_splines(tools)

        def gp(name, default):
            m = re.search(r'[ ,{]%s = Input \{ (Value = (-?[\d.e+-]+)|SourceOp = "(\w+)")' % name, tools)
            if not m: return lambda f: default
            if m.group(2) is not None: v = float(m.group(2)); return lambda f: v
            keys = splines[m.group(3)]; return lambda f: bezier_eval(keys, f)
        g = {n: gp(n, d) for n, d in (('pan', 0), ('tilt', 0), ('dutch', 0), ('zoom', 100), ('distort', 0))}
        In = float(tag(x, 'In') or 0)
        dur = int(tag(x, 'Duration'))
        mfps = struct.unpack('<d', bytes.fromhex(tag(x, 'MediaFrameRate'))[:8])[0]
        worst_angle, worst_mean, worst_max, worst_extra = 0, 0, 0, 0
        for t in range(dur):
            f = (In + t) * mfps / 30.0
            for name, src in (('pan', 'pan'), ('tilt', 'tilt'), ('roll', 'dutch')):
                worst_angle = max(worst_angle, abs(at_frame(params[name], t) - g[src](f)))
            if t % 8 == 0 or t == dur - 1:
                zoom, lc = g['zoom'](f), g['distort'](f)
                m, mx = lens_error_px(zoom, lc, at_frame(params['fov'], t), at_frame(params['curv'], t))
                worst_mean, worst_max = max(worst_mean, m), max(worst_max, mx)
                # against the best fit for this frame alone: what the keyframes and the held curvature cost
                bf, bc = map(float, run([TEST, 'fit', str(zoom), str(lc)]).stdout.split()[:2])
                worst_extra = max(worst_extra, mx - lens_error_px(zoom, lc, bf, bc)[1])
        animated = any(params[n][1] for n in params if isinstance(params[n][0], float))
        if worst_angle > 0.0201 or worst_mean > 4 or worst_max > 40 or worst_extra > 1.5:
            if animated: ok_keys = False
            else: ok_static = False
        details.append('%s In %g: %s, pan/tilt/roll max diff %.4f deg, lens offset mean %.2f px, max %.1f px '
                       '(%.2f px more than the best fit)' %
                       (tag(x, 'Name'), In, 'animated' if animated else 'static', worst_angle, worst_mean, worst_max, worst_extra))
    check(ok_effects, 'existing Edit page effects are kept in front of Max2 Reframe')
    check(ok_static, 'static clips: pan, tilt, roll exact; lens offset mean 4 px, max 40 px, at most 1.5 px over the best fit' +
          ''.join('\n          ' + d for d in details if 'static' in d))
    check(ok_keys, 'animated clip: keyframes follow the GoPro curves at every frame (0.02 deg; lens as above)' +
          ''.join('\n          ' + d for d in details if 'animated' in d))
    return out


def test_effect_layout(converted, sample):
    print('Effect layout against the one Resolve wrote')
    if not os.path.exists(sample):
        print('  skip  (no %s)' % sample)
        return
    s = drt_files(sample)
    resolve = [unblob(tag(el, 'EffectFiltersBA')) for el in clips(s[seq_name(s)].decode())
               if 'com.max2resolve.reframe'.encode().hex() in unblob(tag(el, 'EffectFiltersBA')).hex()]
    c = drt_files(converted)
    mine = [unblob(tag(el, 'EffectFiltersBA')) for el in clips(c[seq_name(c)].decode())
            if b'com.max2resolve.reframe' in unblob(tag(el, 'EffectFiltersBA'))]
    check(len(resolve) == 1 and len(mine) == 14, 'found the effect from Resolve and 14 converted ones')

    def shape(b):
        e = pb(pb(b)[-1][2])
        body = pb(sub(e, 9)[0])
        plugin = pb(sub(pb(sub(body, 3)[0]), 2)[0])
        top = [(f, t, v if t == 0 else None) for f, t, v in e if f != 9]
        return top, [(f, t, v if t == 0 else None) for f, t, v in body if f not in (3, 4)], \
            [(f, t, v if t != 2 else None) for f, t, v in plugin if f != 5], sub(body, 4)
    ra, ma = shape(resolve[0]), shape(mine[0])
    check(ra[0] == ma[0], 'effect header fields match Resolve %s' % (ra[0],))
    check(ra[1] == ma[1], 'effect body fields match Resolve')
    check(ra[2] == ma[2] and ra[3] == ma[3], 'plugin identification matches Resolve')
    rp, mp = effect_params(resolve[0]), effect_params(mine[0])
    keyed = [m for m in mine if any(k for _, k in effect_params(m).values() if isinstance(k, list))]
    # a parameter with keyframes and one without, in Resolve's form
    def pshape(b, name):
        e = pb(pb(b)[-1][2])
        plugin = pb(sub(pb(sub(pb(sub(e, 9)[0]), 3)[0]), 2)[0])
        for p in sub(plugin, 5):
            q = pb(p)
            if sub(q, 1)[0] == name.encode():
                return skeleton(p)
    check(bool(keyed) and _same_keyed_shape(resolve[0], keyed[0]), 'keyframed parameter has the same layout as in Resolve')
    check(pshape(resolve[0], 'tilt') == pshape(mine[0], 'tilt'), 'plain parameter has the same layout as in Resolve')
    check(pshape(resolve[0], 'motionBlur') == pshape(mine[0], 'motionBlur'), 'switch parameter has the same layout as in Resolve')
    check(pshape(resolve[0], 'ResolveUseAlphaForTrackCompositing') == pshape(mine[0], 'ResolveUseAlphaForTrackCompositing'),
          'Resolve\'s own parameter has the same layout')
    names = list(effect_params(mine[0]))
    check(names == sorted(names), 'parameters in name order, as Resolve writes them')
    check(rp['pan'][1] == [(0, 11.0), (60, 22.0)], 'Resolve sample: Pan keyframes 11 at 0 and 22 at 60 half frames')


def _same_keyed_shape(a, b):
    def keyshape(blob):
        e = pb(pb(blob)[-1][2])
        plugin = pb(sub(pb(sub(pb(sub(e, 9)[0]), 3)[0]), 2)[0])
        for p in sub(plugin, 5):
            q = pb(p)
            if sub(q, 3):
                return [(f, t) for f, t, v in pb(sub(q, 3)[-1])], [(f, t) for f, t, v in pb(sub(pb(sub(q, 3)[-1]), 2)[0])]
    return keyshape(a) == keyshape(b)


def test_sample(sample, tmp):
    print('Conversion of the sample with Max2 Reframe clips already in it')
    if not os.path.exists(sample):
        print('  skip  (no %s)' % sample)
        return
    out = os.path.join(tmp, 'sample_converted.drt')
    rep = convert(sample, out)
    check(rep.get('converted') == '14', '14 GoPro clips converted')
    a, b = drt_files(sample), drt_files(out)
    ca, cb = clips(a[seq_name(a)].decode()), clips(b[seq_name(b)].decode())
    max2 = [(x, y) for x, y in zip(ca, cb) if '.360.mp4' in tag(x, 'MediaFilePath')]
    check(len(max2) >= 2 and all(x == y for x, y in max2), 'clips that already had Max2 Reframe are unchanged')


def test_second_run(converted, tmp):
    print('A second run on a converted file')
    out = os.path.join(tmp, 'twice.drt')
    rep = convert(converted, out)
    check(rep.get('ok') == '1' and rep.get('gopro') == '0' and not os.path.exists(out), 'nothing to convert, no file made')


def test_skips(original, tmp):
    print('Clips that must not be converted')
    files = drt_files(original)
    s = files[seq_name(files)].decode()
    els = clips(s)
    gopro = [el for el in els if '<CompositionTable>' in el]

    def variant(name, change):
        el = gopro[change[0]]
        h = tag(el, 'CompositionBA')
        new_el = change[1](el, h)
        f = dict(files)
        f[seq_name(f)] = s.replace(el, new_el).encode()
        path = os.path.join(tmp, name + '.drt')
        with zipfile.ZipFile(path, 'w') as z:
            for n, d in f.items():
                z.writestr(n, d)
        return path

    def tools_edit(fn):
        def go(el, h):
            payload, i, tools = decode_comp(h)
            return el.replace(h, encode_comp(payload, i, fn(tools)))
        return go
    cases = [
        ('offset', (0, tools_edit(lambda t: t.replace('xoffset = Input { Value = 0, }', 'xoffset = Input { Value = 0.1, }'))), 'xoffset'),
        ('extra', (1, tools_edit(lambda t: t.replace('MediaOut1 = MediaOut {', 'Blur1 = Blur { }, MediaOut1 = MediaOut {'))), 'other nodes'),
        ('size', (2, tools_edit(lambda t: t.replace('projection = Input { Value = 7, }', 'projection = Input { Value = 2, }'))), 'output size'),
        ('second', (3, tools_edit(lambda t: t.replace('enable_second = Input { Value = 0, }', 'enable_second = Input { Value = 1, }'))), 'Second Camera'),
        ('yaw', (4, tools_edit(lambda t: t.replace('yaw = Input { Value = 0, }', 'yaw = Input { Value = 5, }'))), 'yaw'),
        ('missing', (5, lambda el, h: re.sub(r'<MediaFilePath>[^<]*</MediaFilePath>', r'<MediaFilePath>X:\\nowhere\\GS019999.mp4</MediaFilePath>', el)), 'no .360 file'),
    ]
    for name, change, words in cases:
        path = variant(name, change)
        rep = convert(path, os.path.join(tmp, name + '_out.drt'))
        check(rep.get('converted') == '13' and len(rep['notes']) == 1 and words in rep['notes'][0],
              '%s: 13 converted, the changed clip is reported (%s)' % (name, rep['notes'][0] if rep['notes'] else 'no note'))
    rep = convert(os.path.join(ROOT, 'README.md'), os.path.join(tmp, 'x.drt'))
    check(rep.get('ok') == '0' and 'zip' in rep.get('error', ''), 'a file that is not a timeline gives an error (%s)' % rep.get('error'))


def test_search(data, tmp):
    print('Finding the .360 file of an export (plugin and converter)')
    src = os.path.join(data, 'GS012039.360')
    if not os.path.exists(src):
        print('  skip  (no %s)' % src)
        return
    trip = os.path.join(tmp, 'Trip')
    os.makedirs(os.path.join(trip, 'MAX2'))
    os.makedirs(os.path.join(trip, 'MAX2_equi'))
    os.makedirs(os.path.join(trip, 'Deep', 'equi'))
    os.makedirs(os.path.join(trip, 'Deep', 'MAX2', 'card'))
    os.makedirs(os.path.join(trip, 'Far', 'a', 'b'))
    shutil.copy(src, os.path.join(trip, 'MAX2', 'GS012039.360'))
    shutil.copy(src, os.path.join(trip, 'Deep', 'MAX2', 'card', 'GS012039.360'))
    tc = '15:22:34;01'

    def fake(path, extra=(), timecode=tc):
        run([FFMPEG, '-v', 'error', '-y', '-i', src, '-map', '0:v:0', '-c', 'copy', '-timecode', timecode, *extra, path])
    fake(os.path.join(trip, 'MAX2_equi', 'GS012039.mp4'))
    fake(os.path.join(trip, 'MAX2_equi', 'GS012039_short.mp4'), ['-frames:v', '200'])
    fake(os.path.join(trip, 'Deep', 'equi', 'GS012039.mp4'))
    fake(os.path.join(trip, 'Far', 'a', 'b', 'GS012039.mp4'))
    os.makedirs(os.path.join(trip, 'TC', 'equi'))
    os.makedirs(os.path.join(trip, 'TC', 'MAX2'))
    shutil.copy(src, os.path.join(trip, 'TC', 'MAX2', 'GS012039.360'))
    fake(os.path.join(trip, 'TC', 'equi', 'GS012039.mp4'), timecode='10:00:00;00')
    # A short export with the same name: rename after making it
    short_dir = os.path.join(trip, 'Short')
    os.makedirs(os.path.join(short_dir, 'equi')); os.makedirs(os.path.join(short_dir, 'MAX2'))
    shutil.copy(src, os.path.join(short_dir, 'MAX2', 'GS012039.360'))
    shutil.move(os.path.join(trip, 'MAX2_equi', 'GS012039_short.mp4'), os.path.join(short_dir, 'equi', 'GS012039.mp4'))

    def render(path, out):
        r = run([RENDER, path, '--frame', '100', '--w', '640', '--h', '360', '--fov', '120', '--out', out])
        return r.returncode == 0 and os.path.exists(out), (r.stdout + r.stderr).strip().splitlines()[-1:]
    ok, _ = render(os.path.join(trip, 'MAX2', 'GS012039.360'), os.path.join(tmp, 'ref.ppm'))
    check(ok, 'reference render from the .360 file')
    ref = open(os.path.join(tmp, 'ref.ppm'), 'rb').read()
    for name, path, want in (('sibling folder', os.path.join(trip, 'MAX2_equi', 'GS012039.mp4'), True),
                             ('one level below a sibling folder', os.path.join(trip, 'Deep', 'equi', 'GS012039.mp4'), True),
                             ('too far away', os.path.join(trip, 'Far', 'a', 'b', 'GS012039.mp4'), False),
                             ('other timecode', os.path.join(trip, 'TC', 'equi', 'GS012039.mp4'), False),
                             ('fewer frames', os.path.join(short_dir, 'equi', 'GS012039.mp4'), False)):
        out = os.path.join(tmp, 'r_%s.ppm' % name.replace(' ', '_'))
        ok, msg = render(path, out)
        same = ok and open(out, 'rb').read() == ref
        check(same if want else not ok, '%s: %s %s' % (name, 'uses the .360 file' if want else 'refused', msg if not want else ''))


def read_export_list(path):
    text = open(path, encoding='utf-8').read()
    return re.findall(r'\{ Export = \[==\[(.*?)\]==\], Source = \[==\[(.*?)\]==\] \}', text)


def test_exe(original, sample, tmp):
    print('Max2Convert.exe')
    src = os.path.join(tmp, 'My Timeline.drt')
    shutil.copy(original, src)
    report = os.path.join(tmp, 'report.txt')
    exports = os.path.join(tmp, 'list', 'exports.lua')
    with open(os.path.join(tmp, 'old.lua'), 'w', encoding='utf-8') as f:
        f.write('return {\n  { Export = [==[D:\\Old\\equi\\GS010001.mp4]==], Source = [==[D:\\Old\\GS010001.360]==] },\n}\n')
    os.makedirs(os.path.dirname(exports))
    shutil.copy(os.path.join(tmp, 'old.lua'), exports)
    r = subprocess.run([CONVERT, src, '--quiet', '--report', report, '--exports', exports])
    out = os.path.join(tmp, 'My Timeline (Max2 Reframe).drt')
    text = open(report, encoding='utf-8').read() if os.path.exists(report) else ''
    check(r.returncode == 0 and os.path.exists(out), 'converts and writes "<name> (Max2 Reframe).drt" next to the input')
    check('converted 14' in text and 'timeline 05_Eingang (Max2 Reframe)' in text and 'listed 1' in text, 'report file lists the result')
    check(open(original, 'rb').read() == open(src, 'rb').read(), 'the input file does not change')
    pairs = read_export_list(exports)
    check(len(pairs) == 7 and all(os.path.exists(s) for e, s in pairs if not e.startswith('D:')) and
          ('D:\\Old\\equi\\GS010001.mp4', 'D:\\Old\\GS010001.360') in pairs,
          'export list: the 6 exports of the timeline added to the entry that was there (%d entries)' % len(pairs))
    if os.path.exists(sample):
        subprocess.run([CONVERT, sample, '--quiet', '--out', os.path.join(tmp, 's.drt'), '--exports', exports])
        check(len(read_export_list(exports)) == 7, 'export list: a second timeline with the same exports adds no duplicates')
    r = subprocess.run([CONVERT, out, '--quiet'])
    check(r.returncode == 2, 'exit code 2 when there is nothing to convert')
    r = subprocess.run([CONVERT, '--quiet'])
    check(r.returncode == 3, 'exit code 3 without a file in quiet mode')


def test_resolve_script():
    print('Resolve script "Use 360 Files for GoPro Exports"')
    fuscript = r'C:\Program Files\Blackmagic Design\DaVinci Resolve\fuscript.exe'
    if not os.path.exists(fuscript):
        print('  skip  (DaVinci Resolve is not installed)')
        return
    env = dict(os.environ, USE360_SCRIPT=os.path.join(ROOT, 'tools', 'resolve', 'Use 360 Files for GoPro Exports.lua'))
    r = run([fuscript, '-l', 'lua', os.path.join(ROOT, 'tools', 'test', 'test_use360.lua')], env=env, timeout=120)
    lines = [l for l in r.stdout.splitlines() if l.startswith('  ')]
    for l in lines:
        check(l.startswith('  ok'), l[8:])
    check('failures 0' in r.stdout, 'the script runs against a fake Resolve' + ('' if lines else ': ' + (r.stdout + r.stderr)[-300:]))


def test_pictures(original, tmp):
    print('Pictures: real GoPro Reframe output against Max2 Reframe with the converted values')
    if not os.path.exists(GOPRO):
        print('  skip  (GoPro Reframe is not installed)')
        return
    import cv2
    host = os.path.join(BUILD, 'ofxhost.exe')
    check(os.path.exists(host), 'test host built')
    files = drt_files(original)
    el = [e for e in clips(files[seq_name(files)].decode()) if 'GS011840' in tag(e, 'Name')][0]
    src360 = re.sub(r'MAX2_equi \(Eingang\)\\GS011840\.mp4$', r'MAX2 (Eingang)\\GS011840.360', tag(el, 'MediaFilePath'))
    erp = os.path.join(tmp, 'erp.ppm')
    run([RENDER, src360, '--frame', '100', '--proj', 'erp', '--w', '7680', '--h', '3840', '--ss', '2', '--out', erp])
    im = cv2.imread(erp)[:, :, ::-1].astype(np.float32) / 255
    a = np.ones(im.shape[:2] + (4,), np.float32); a[:, :, :3] = im
    raw = os.path.join(tmp, 'erp.raw'); np.ascontiguousarray(a[::-1]).tofile(raw)
    # the GoPro mapping itself, measured with a coordinate image
    uv = np.zeros((1920, 3840, 4), np.float32)
    ys, xs = np.mgrid[0:1920, 0:3840].astype(np.float32)
    uv[..., 0] = (xs + 0.5) / 3840; uv[..., 1] = (ys + 0.5) / 1920; uv[..., 3] = 1
    uvraw = os.path.join(tmp, 'uv.raw'); np.ascontiguousarray(uv[::-1]).tofile(uvraw)

    def gopro(inp, w, h, pan, tilt, dutch, zoom, lc, out, project=(3840, 2160), projection=7):
        r = run([host, GOPRO, 'com.gopro.plugins.reframe', inp, str(w), str(h), out, '--time', '100', '--project', *map(str, project),
                 '--set', 'projection=%d' % projection, '--set', 'pan=%g' % pan, '--set', 'tilt=%g' % tilt, '--set', 'dutch=%g' % dutch,
                 '--set', 'zoom=%g' % zoom, '--set', 'distort=%g' % lc])
        return r.stdout.split()
    dims = gopro(uvraw, 3840, 1920, 0, 0, 0, 100, 0, os.path.join(tmp, 'p2.raw'), projection=2)
    check(dims[:2] == ['1920', '1080'], 'GoPro output list: 2 is HD 16:9 (got %s)' % dims[:2])
    dims = gopro(uvraw, 3840, 1920, 0, 0, 0, 100, 0, os.path.join(tmp, 'p7.raw'), projection=7)
    check(dims[:2] == ['3840', '2160'], 'GoPro output list: 7 is UHD 16:9 (got %s)' % dims[:2])
    cases = [(0, -32.6, 0, 45, 100), (-15.3, 41, 0, 88.8, 100), (15.5, 14.6, 0, 139.5, 100), (-6.5, 0, 0, 171.4, 0),
             (-7.5, 23.8, 0, 116.1, 98.6), (27.8, 28.4, 15, 76.7, 60)]
    for pan, tilt, dutch, zoom, lc in cases:
        # 1. the GoPro model against the real plugin
        out = os.path.join(tmp, 'uvout.raw')
        gopro(uvraw, 3840, 1920, pan, tilt, dutch, zoom, lc, out)
        m = np.fromfile(out, np.float32).reshape(2160, 3840, 4)[::-1]
        yy, xx = np.mgrid[4:2160:16, 4:3840:16]
        u, v = m[yy, xx, 0].astype(float), m[yy, xx, 1].astype(float)
        ok = np.abs(u - 0.5) < 0.49  # away from the 360 degree seam, where the filter mixes both sides
        lon = u * 2 * np.pi - np.pi; lat = np.pi / 2 - v * np.pi
        d_real = np.stack([np.cos(lat) * np.sin(lon), np.sin(lat), np.cos(lat) * np.cos(lon)], -1)
        # where the converted Max2 view shows each of these directions: turn it into the view, then through the lens
        fov, curv = map(float, run([TEST, 'fit', str(zoom), str(lc)]).stdout.split()[:2])
        c, s_ = np.cos, np.sin
        P, T, R = np.radians(pan), np.radians(-tilt), np.radians(-dutch)
        RY = np.array([[c(P), 0, s_(P)], [0, 1, 0], [-s_(P), 0, c(P)]]); RX = np.array([[1, 0, 0], [0, c(T), -s_(T)], [0, s_(T), c(T)]])
        RZ = np.array([[c(R), -s_(R), 0], [s_(R), c(R), 0], [0, 0, 1]])
        vv = d_real @ (RY @ RX @ RZ)  # world -> view
        theta = np.degrees(np.arccos(np.clip(vv[..., 2], -1, 1)))
        az = np.arctan2(vv[..., 1], vv[..., 0])
        rr = max2_radius(theta, fov, curv)
        x_m, y_m = 1920 + rr * np.cos(az), 1080 - rr * np.sin(az)
        px = np.hypot(x_m - (xx + 0.5), y_m - (yy + 0.5))[ok]
        # the lens model's own limit for this setting, at the same pixels
        expect = np.abs(max2_radius(gopro_angle(np.hypot(xx + 0.5 - 1920, yy + 0.5 - 1080), zoom, lc), fov, curv) -
                        np.hypot(xx + 0.5 - 1920, yy + 0.5 - 1080))[ok]
        check(np.median(px) < 4.5 and np.percentile(np.abs(px - expect), 99) < 1,
              'pan %g tilt %g dutch %g zoom %g curve %g: GoPro and Max2 put each direction median %.2f px apart, 99%% %.2f px '
              '(the lens model predicts this to 1 px at 99%% of points)' %
              (pan, tilt, dutch, zoom, lc, np.median(px), np.percentile(px, 99)))
        # 2. the pictures, from the same 360 frame
        gout = os.path.join(tmp, 'g.raw'); gopro(raw, 7680, 3840, pan, tilt, dutch, zoom, lc, gout)
        g = (np.clip(np.fromfile(gout, np.float32).reshape(2160, 3840, 4)[::-1, :, :3], 0, 1) * 255).astype(np.uint8)
        mo = os.path.join(tmp, 'm.ppm')
        run([RENDER, src360, '--frame', '100', '--w', '3840', '--h', '2160', '--pan', str(pan), '--tilt', str(tilt), '--roll', str(dutch),
             '--fov', str(fov), '--curv', str(curv), '--out', mo])
        mm = cv2.imread(mo)[:, :, ::-1]
        ea = cv2.Laplacian(cv2.GaussianBlur(cv2.cvtColor(g, cv2.COLOR_RGB2GRAY).astype(np.float32), (0, 0), 1.5), cv2.CV_32F)
        eb = cv2.Laplacian(cv2.GaussianBlur(cv2.cvtColor(mm, cv2.COLOR_RGB2GRAY).astype(np.float32), (0, 0), 1.5), cv2.CV_32F)
        th_, tw_ = 540, 640
        win = cv2.createHanningWindow((tw_, th_), cv2.CV_32F)
        shifts = []
        for ty in range(4):
            for tx in range(6):
                (dx, dy), resp = cv2.phaseCorrelate(ea[ty*th_:(ty+1)*th_, tx*tw_:(tx+1)*tw_], eb[ty*th_:(ty+1)*th_, tx*tw_:(tx+1)*tw_], win)
                if resp > 0.05: shifts.append(np.hypot(dx, dy))
        check(len(shifts) >= 20 and np.median(shifts) < 3 and max(shifts) < 6,
              '  pictures: %d of 24 tiles match, shift median %.2f px, max %.2f px' %
              (len(shifts), np.median(shifts) if shifts else -1, max(shifts) if shifts else -1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=os.path.join(ROOT, 'data'))
    ap.add_argument('--pictures', action='store_true')
    a = ap.parse_args()
    original = os.path.join(a.data, '05_Eingang.drt')
    sample = os.path.join(a.data, '05_Eingang-sample.drt')
    tmp = tempfile.mkdtemp(prefix='max2convert_')
    try:
        test_model()
        test_fit()
        test_splines(original)
        converted = test_convert(original, tmp)
        test_effect_layout(converted, sample)
        test_sample(sample, tmp)
        test_second_run(converted, tmp)
        test_skips(original, tmp)
        test_search(a.data, tmp)
        test_exe(original, sample, tmp)
        test_resolve_script()
        if a.pictures:
            test_pictures(original, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('\n%d failure(s)' % len(failures) if failures else '\nAll tests passed.')
    for f in failures:
        print('  - ' + f.splitlines()[0])
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
