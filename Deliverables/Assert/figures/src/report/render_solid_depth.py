"""图12 场景层：两个互锁的环面（真三维几何），
分别按“无深度(后画整片覆盖)”与“有深度(逐像素比较)”渲染，并输出同一帧的深度伪彩。
用法：python render_solid_depth.py，输出 solid_off.png / solid_on.png / solid_depth.png 到本目录，
再由 rep_depth.html 排版成图。"""
import numpy as np
from PIL import Image
from pathlib import Path

W, H = 1520, 960
OUT = Path(__file__).parent
OUT.mkdir(exist_ok=True)


def unit(v):
    return v / np.linalg.norm(v)


TARGET = np.array([0.42, 0.0, 0.0])
UP = np.array([0.0, 1.0, 0.0])
FOV = 30.0
DIR = unit(np.array([0.30, 0.40, 1.0]))
EYE = TARGET + DIR * 6.0
f = unit(TARGET - EYE)
s = unit(np.cross(f, UP))
u = np.cross(s, f)
FL = 0.5 * W / np.tan(np.radians(FOV) / 2)


def project(P):
    d = P - EYE
    z = d @ f
    return W / 2 + FL * (d @ s) / z, H / 2 - FL * (d @ u) / z, z


def refit(points, fill=0.92):
    global EYE, f, s, u, FL
    lo, hi = 2.0, 40.0
    for _ in range(40):
        mid = (lo + hi) / 2
        EYE = TARGET + DIR * mid
        f = unit(TARGET - EYE)
        s = unit(np.cross(f, UP))
        u = np.cross(s, f)
        FL = 0.5 * W / np.tan(np.radians(FOV) / 2)
        xs, ys, _ = project(points)
        span = max((xs.max() - xs.min()) / (W * fill), (ys.max() - ys.min()) / (H * fill))
        lo, hi = (mid, hi) if span > 1 else (lo, mid)


def torus(R=1.0, r=0.30, nu=1800, nv=700):
    uu = np.linspace(0, 2 * np.pi, nu, endpoint=False)
    vv = np.linspace(0, 2 * np.pi, nv, endpoint=False)
    U, V = np.meshgrid(uu, vv)
    x = (R + r * np.cos(V)) * np.cos(U)
    y = (R + r * np.cos(V)) * np.sin(U)
    z = r * np.sin(V)
    nx = np.cos(V) * np.cos(U)
    ny = np.cos(V) * np.sin(U)
    nz = np.sin(V)
    P = np.stack([x, y, z], axis=-1).reshape(-1, 3)
    N = np.stack([nx, ny, nz], axis=-1).reshape(-1, 3)
    return P, N


def rot_x(a):
    c, s_ = np.cos(a), np.sin(a)
    return np.array([[1, 0, 0], [0, c, -s_], [0, s_, c]])


# 环 A：xy 平面；环 B：绕 x 转 90° 后沿 x 平移，与 A 互相穿过
PA, NA = torus()
PB, NB = torus()
PB = PB @ rot_x(np.pi / 2).T
NB = NB @ rot_x(np.pi / 2).T
SHIFT = np.array([0.86, 0.0, 0.0])
PB = PB + SHIFT
refit(np.vstack([PA[::7], PB[::7]]))

LIGHT = unit(np.array([0.45, 0.75, 0.85]))
COL_A = np.array([86, 124, 178], float)
COL_B = np.array([206, 152, 68], float)


def shade(N, base):
    lam = np.clip(N @ LIGHT, 0, 1)
    rim = np.clip(1.0 - np.abs(N @ np.array([0.0, 0.0, 1.0])), 0, 1) ** 2
    col = base[None, :] * (0.32 + 0.78 * lam[:, None]) + 30 * rim[:, None]
    return np.clip(col, 0, 255).astype(np.uint8)


def splat(xs, ys, z, col, img, zbuf, test=True, rad=2):
    xi = np.round(xs).astype(np.int32)
    yi = np.round(ys).astype(np.int32)
    for dx in range(-rad, rad + 1):
        for dy in range(-rad, rad + 1):
            if dx * dx + dy * dy > rad * rad + 1:
                continue
            x2, y2 = xi + dx, yi + dy
            ok = (x2 >= 0) & (x2 < W) & (y2 >= 0) & (y2 < H) & (z > 0)
            idx = y2[ok] * W + x2[ok]
            zz = z[ok]
            cc = col[ok]
            if test:
                nearer = zz < zbuf[idx]
                img[idx[nearer]] = cc[nearer]
                zbuf[idx[nearer]] = zz[nearer]
            else:
                img[idx] = cc
                zbuf[idx] = zz


def draw_object(P, N, base, img, zbuf):
    xs, ys, z = project(P)
    col = shade(N, base)
    splat(xs, ys, z, col, img, zbuf, test=True, rad=2)


def render(mode):
    """mode='on' 逐像素深度；mode='off' 每个物体各自成图后整片覆盖（画家算法）。"""
    img = np.full((H * W, 3), 255, np.uint8)
    zbuf = np.full(H * W, np.inf, np.float32)
    if mode == "on":
        for P, N, base in ((PA, NA, COL_A), (PB, NB, COL_B)):
            draw_object(P, N, base, img, zbuf)
        return img.reshape(H, W, 3), zbuf.reshape(H, W)
    # off：逐物体先各自做深度，再按物体顺序整片覆盖
    for P, N, base in ((PA, NA, COL_A), (PB, NB, COL_B)):
        obj_img = np.full((H * W, 3), 0, np.uint8)
        obj_z = np.full(H * W, np.inf, np.float32)
        draw_object(P, N, base, obj_img, obj_z)
        m = np.isfinite(obj_z)
        img[m] = obj_img[m]
        zbuf[m] = obj_z[m]
    return img.reshape(H, W, 3), zbuf.reshape(H, W)


def save(arr, name):
    Image.fromarray(arr).save(OUT / name)
    print("saved", name)


rgb_off, z_off = render("off")
rgb_on, z_on = render("on")
save(rgb_off, "solid_off.png")
save(rgb_on, "solid_on.png")

zs = np.concatenate([z_off[np.isfinite(z_off)], z_on[np.isfinite(z_on)]])
lo, hi = float(np.percentile(zs, 3)), float(np.percentile(zs, 97))
print("z", lo, hi)


def ramp(t):
    near = np.array([214, 82, 74], float)
    mid = np.array([232, 176, 74], float)
    far = np.array([74, 116, 178], float)
    t = np.clip(t, 0, 1)[:, None]
    return np.where(t < 0.5, near + (mid - near) * (t / 0.5), mid + (far - mid) * ((t - 0.5) / 0.5))


m = np.isfinite(z_on)
t = np.clip((z_on[m] - lo) / (hi - lo), 0, 1)
dep = np.full((H, W, 3), 255, np.uint8)
dep[m] = ramp(t).astype(np.uint8)
save(dep, "solid_depth.png")
