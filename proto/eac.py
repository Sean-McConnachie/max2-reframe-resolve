"""Prototype: GoPro Max 2 dual-stream EAC -> directions / ERP, used to pin down geometry and conventions.

Camera coordinates: +x right, +y up, +z forward (front lens).
"""
import numpy as np
import cv2

FACE = 1920          # cube face size in pixels (Max 2)
HALF = 992           # width of each half of a split (seam) face: FACE/2 + 32 overlap
OVL = 64             # overlap between the two halves of a split face, in face columns
MID = 1984           # x of the middle face
RIGHT = 3904         # x of the right-slot face


def set_scale(k):
    """Use streams decoded at k times full size."""
    global FACE, HALF, OVL, MID, RIGHT
    FACE, HALF, OVL, MID, RIGHT = (int(round(v * k)) for v in (1920, 992, 64, 1984, 3904))

X, Y, Z = np.eye(3)
# (stream, face slot, forward, image-right, image-down)
FACES = [
    (0, 0, -X, +Z, -Y),  # left   (split by lens seam)
    (0, 1, +Z, +X, -Y),  # front
    (0, 2, +X, -Z, -Y),  # right  (split)
    (1, 0, -Y, -Z, -X),  # bottom (split)
    (1, 1, -Z, +Y, -X),  # back
    (1, 2, +Y, +Z, -X),  # top    (split)
]


def erp_dirs(w, h):
    lon = (np.arange(w) + 0.5) / w * 2 * np.pi - np.pi
    lat = np.pi / 2 - (np.arange(h) + 0.5) / h * np.pi
    lon, lat = np.meshgrid(lon, lat)
    return np.stack([np.cos(lat) * np.sin(lon), np.sin(lat), np.cos(lat) * np.cos(lon)], -1)


def face_col_to_x(slot, col):
    """Map face column (0..FACE) of a face in a slot to stream x. Returns (xA, xB, wB) for split faces."""
    if slot == 1:
        return MID + col, None, None
    base = 0 if slot == 0 else RIGHT
    xa = base + col                    # first half holds face cols [0, HALF)
    xb = base + HALF + (col - (FACE - HALF))  # second half holds face cols [FACE-HALF, FACE)
    # blend weight for second half across the overlap [FACE-HALF, HALF)
    wb = np.clip((col - (FACE - HALF)) / OVL, 0, 1)
    return xa, xb, wb


def bilinear(img, x, y):
    """Bilinear sample img (HxWxC) at float pixel coords x, y (1-D arrays)."""
    h, w = img.shape[:2]
    x = np.clip(x, 0, w - 1.001); y = np.clip(y, 0, h - 1.001)
    x0 = x.astype(np.int64); y0 = y.astype(np.int64)
    fx = (x - x0)[:, None]; fy = (y - y0)[:, None]
    a = img[y0, x0]; b = img[y0, x0 + 1]; c = img[y0 + 1, x0]; d = img[y0 + 1, x0 + 1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def sample(streams, dirs):
    """streams: [img0, img1] HxWx3 float32. dirs: ...x3 camera-space unit vectors. Returns image."""
    out = np.zeros(dirs.shape[:-1] + (3,), np.float32)
    for s, slot, f, r, dn in FACES:
        df = dirs @ f
        m = (df > 0) & (df >= np.abs(dirs).max(-1) - 1e-9)
        if not m.any():
            continue
        d = dirs[m]
        u = (d @ r) / df[m]
        v = (d @ dn) / df[m]
        # equi-angular warp
        cu = (np.arctan(u) * 4 / np.pi + 1) * 0.5 * FACE - 0.5
        cv = (np.arctan(v) * 4 / np.pi + 1) * 0.5 * FACE - 0.5
        img = streams[s]
        if slot == 1:
            mx, my = (MID + cu).astype(np.float32), cv.astype(np.float32)
            out[m] = bilinear(img, mx, my)
        else:
            xa, xb, wb = face_col_to_x(slot, cu)
            base = 0 if slot == 0 else RIGHT
            xa = np.minimum(xa, base + HALF - 1)
            xb = np.maximum(xb, base + HALF)
            pa = bilinear(img, xa, cv)
            pb = bilinear(img, xb, cv)
            out[m] = pa * (1 - wb)[:, None] + pb * wb[:, None]
    return out


def to_erp(streams, w, h, R=np.eye(3)):
    """R maps output (ERP) directions into camera directions."""
    return sample(streams, erp_dirs(w, h) @ R.T)


if __name__ == '__main__':
    import sys
    s0 = cv2.imread(sys.argv[1]).astype(np.float32)
    s1 = cv2.imread(sys.argv[2]).astype(np.float32)
    cv2.imwrite(sys.argv[3], np.clip(to_erp([s0, s1], 1920, 960), 0, 255).astype(np.uint8))
