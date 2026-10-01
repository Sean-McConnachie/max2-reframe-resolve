"""Estimate per-frame rotation between our raw (unstabilized) ERP and GoPro Player exports.

Saves work/fit.npz with:
  raw_R[name][i]: rotation R such that export(dir) == raw(R @ dir), i.e. maps export dirs to camera dirs.
"""
import subprocess
import sys

import cv2
import numpy as np

import eac

W, H = 1920, 960
K = 0.5
eac.set_scale(K)
SW, SH = int(5888 * K), int(1920 * K)


def frames(path, maps, w, h):
    cmd = ['ffmpeg', '-v', 'error', '-i', path]
    for m in maps:
        cmd += ['-map', m, '-vf', f'scale={w}:{h}', '-f', 'rawvideo', '-pix_fmt', 'bgr24', 'pipe:']
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    return p


def read_all(path, w, h):
    data = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-map', '0:v:0', '-vf', f'scale={w}:{h}',
                           '-f', 'rawvideo', '-pix_fmt', 'bgr24', 'pipe:'], capture_output=True, check=True).stdout
    return np.frombuffer(data, np.uint8).reshape(-1, h, w, 3)


def read_stream(path, idx, w, h):
    data = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-map', f'0:{idx}', '-vf', f'scale={w}:{h}',
                           '-f', 'rawvideo', '-pix_fmt', 'bgr24', 'pipe:'], capture_output=True, check=True).stdout
    return np.frombuffer(data, np.uint8).reshape(-1, h, w, 3)


def kp_dirs(kps, w, h):
    pts = np.array([k.pt for k in kps])
    lon = (pts[:, 0] + 0.5) / w * 2 * np.pi - np.pi
    lat = np.pi / 2 - (pts[:, 1] + 0.5) / h * np.pi
    return np.stack([np.cos(lat) * np.sin(lon), np.sin(lat), np.cos(lat) * np.cos(lon)], -1)


def kabsch(a, b):
    """R minimizing |R a - b|."""
    u, s, vt = np.linalg.svd(b.T @ a)
    d = np.sign(np.linalg.det(u @ vt))
    return u @ np.diag([1, 1, d]) @ vt


def ransac_rot(a, b, iters=400, thr=np.radians(0.4)):
    rng = np.random.default_rng(0)
    best = None
    for _ in range(iters):
        i = rng.choice(len(a), 3, replace=False)
        R = kabsch(a[i], b[i])
        err = np.arccos(np.clip(np.sum((a @ R.T) * b, 1), -1, 1))
        inl = err < thr
        if best is None or inl.sum() > best.sum():
            best = inl
    R = kabsch(a[best], b[best])
    err = np.arccos(np.clip(np.sum((a @ R.T) * b, 1), -1, 1))
    return R, best.sum(), np.degrees(np.median(err[best]))


def main():
    data = '../data/'
    s0 = read_stream(data + 'GS011833-original.360', 0, SW, SH)
    s1 = read_stream(data + 'GS011833-original.360', 4, SW, SH)
    n = len(s0)
    exports = {k: read_all(data + f'GS011833-{k}.mp4', W, H) for k in ('no-direction-lock', 'direction-lock')}
    sift = cv2.SIFT_create(4000)
    bf = cv2.BFMatcher()
    # only use mid latitudes, where ERP distortion is low
    mask = np.zeros((H, W), np.uint8)
    mask[int(H * 0.15):int(H * 0.85)] = 255
    res = {k: np.full((n, 3, 3), np.nan) for k in exports}
    for i in range(0, n, 2):
        raw = np.clip(eac.to_erp([s0[i].astype(np.float32), s1[i].astype(np.float32)], W, H), 0, 255).astype(np.uint8)
        if i == 60:
            cv2.imwrite('../work/raw60.png', raw)
        kr, dr = sift.detectAndCompute(cv2.cvtColor(raw, cv2.COLOR_BGR2GRAY), mask)
        vr = kp_dirs(kr, W, H)
        for k, ex in exports.items():
            ke, de = sift.detectAndCompute(cv2.cvtColor(ex[i], cv2.COLOR_BGR2GRAY), mask)
            ms = [m for m, m2 in bf.knnMatch(de, dr, k=2) if m.distance < 0.75 * m2.distance]
            a = kp_dirs([ke[m.queryIdx] for m in ms], W, H)
            b = vr[[m.trainIdx for m in ms]]
            R, ninl, med = ransac_rot(a, b)
            res[k][i] = R
            print(i, k, len(ms), ninl, round(med, 3), flush=True)
    np.savez('../work/fit.npz', **{k.replace('-', '_'): v for k, v in res.items()})


if __name__ == '__main__':
    main()
