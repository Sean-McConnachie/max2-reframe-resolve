"""Reference stabilization model (mirrors what the plugin does in C++).

Conventions: camera coords +x right, +y up, +z forward (front lens).
view_rotation(i) returns R mapping output-view directions to camera directions (dir_cam = R @ dir_out).
"""
import numpy as np
from scipy.ndimage import gaussian_filter1d

M = np.diag([-1.0, 1.0, -1.0])  # GPMF orientation frame -> our camera frame


def qmat(q):
    w, x, y, z = q / np.linalg.norm(q)
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def gauss(a, sigma):
    return gaussian_filter1d(a, sigma, axis=0, mode='mirror') if sigma > 0 else a


class Stabilizer:
    def __init__(self, cori, iori, grav, fps):
        n = len(cori)
        self.fps = fps
        # world0 -> camera
        self.T = np.array([M @ qmat(iori[i]) @ qmat(cori[i]) @ M.T for i in range(n)])
        gw = np.einsum('nji,nj->ni', self.T, grav @ M.T)  # T^T @ (M g)
        gw /= np.linalg.norm(gw, axis=1)[:, None]
        self.gw = gw

    def up(self, sigma_s):
        u = gauss(self.gw, sigma_s * self.fps)
        return u / np.linalg.norm(u, axis=1)[:, None]

    def view(self, horizon=True, direction_lock=False, smooth_s=0.3, grav_s=3.0):
        """Return per-frame R (output dir -> camera dir)."""
        T = self.T
        n = len(T)
        fwd = np.einsum('nji,j->ni', T, [0, 0, 1.0])     # camera forward in world0
        upc = np.einsum('nji,j->ni', T, [0, 1.0, 0])     # camera up in world0
        if horizon:
            up = self.up(grav_s)
        else:
            up = upc if not direction_lock else np.repeat(upc[:1], n, 0)
            if not direction_lock:
                up = gauss(up, smooth_s * self.fps)
        if direction_lock:
            f = np.repeat(fwd[:1], n, 0)
        else:
            f = gauss(fwd, smooth_s * self.fps)
        out = np.empty_like(T)
        for i in range(n):
            u = up[i] / np.linalg.norm(up[i])
            z = f[i] - u * (f[i] @ u)
            z /= np.linalg.norm(z)
            x = np.cross(u, z)
            E = np.stack([x, u, z], 1)  # output -> world0
            out[i] = T[i] @ E
        return out
