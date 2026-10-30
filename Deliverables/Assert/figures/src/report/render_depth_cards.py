"""渲染图12 的两张场景层 depth_scene_off.png / depth_scene_on.png：
同一对互相穿过的照片卡，分别按“无深度(后画整片覆盖)”与“有深度(逐像素比较)”合成。
用法：python render_depth_cards.py，输出写到本目录，再由 rep_depth.html 排版成图。"""
import numpy as np
from pathlib import Path
from PIL import Image

W, H = 1520, 960

def unit(v):
    return v / np.linalg.norm(v)

def make_camera(eye, target, up, fov_deg):
    f = unit(target - eye)
    s = unit(np.cross(f, up))
    u = np.cross(s, f)
    fl = 0.5 * W / np.tan(np.radians(fov_deg) / 2)
    return eye, s, u, f, fl

def project(P, cam):
    eye, s, u, f, fl = cam
    d = P - eye
    z = d @ f
    return W / 2 + fl * (d @ s) / z, H / 2 - fl * (d @ u) / z, z

def plane_grid(c00, c10, c01, c11, nu, nv):
    U, V = np.meshgrid(np.linspace(0, 1, nu), np.linspace(0, 1, nv))
    P = (c00 * (1 - U)[..., None] * (1 - V)[..., None]
         + c10 * U[..., None] * (1 - V)[..., None]
         + c01 * (1 - U)[..., None] * V[..., None]
         + c11 * U[..., None] * V[..., None])
    return U, V, P

def load_tex(path, w=1500, h=1050):
    im = Image.open(path).convert("RGB").resize((w, h), Image.LANCZOS)
    return np.asarray(im)

def card(center, angle_deg, width, height, tilt_deg=0.0):
    cx, cy, cz = center
    th = np.radians(angle_deg)
    ph = np.radians(tilt_deg)
    ex = np.array([np.cos(th), 0.0, -np.sin(th)]) * (width / 2)
    ey = np.array([0.0, np.cos(ph), np.sin(ph)]) * (height / 2)
    c = np.array([cx, cy, cz])
    return dict(c00=c - ex - ey, c10=c + ex - ey, c01=c - ex + ey, c11=c + ex + ey)

A = card((0.00, -0.05, 0.00), +36, 2.05, 1.15)
B = card((0.02, 0.12, 0.06), -32, 1.55, 1.42, tilt_deg=6)

CORNERS = [A[c] for c in ("c00", "c10", "c01", "c11")] + [B[c] for c in ("c00", "c10", "c01", "c11")]

def fit_camera(direction, target, fov=30.0, fill=0.94):
    d = unit(np.array(direction, float))
    lo, hi = 1.5, 40.0
    for _ in range(48):
        mid = (lo + hi) / 2
        cam = make_camera(target + d * mid, target, np.array([0.0, 1.0, 0.0]), fov)
        xs, ys = [], []
        for c in CORNERS:
            x, y, _ = project(c[None, :], cam)
            xs.append(float(x[0])); ys.append(float(y[0]))
        span = max((max(xs) - min(xs)) / (W * fill), (max(ys) - min(ys)) / (H * fill))
        lo, hi = (mid, hi) if span > 1 else (lo, mid)
    return make_camera(target + d * hi, target, np.array([0.0, 1.0, 0.0]), fov)

CAM_TARGET = np.array([0.0, 0.05, 0.0])
cam = fit_camera((0.30, 0.50, 1.0), CAM_TARGET)

def render(mode):
    img = np.full((H * W, 3), 255, np.uint8)
    zbuf = np.full(H * W, np.inf, np.float32)
    texA = load_tex(r"E:\Workspace\FPGA\Efinix\Deliverables\Assert\third-party\starry_night_520.jpg")
    texB = load_tex(r"E:\Workspace\FPGA\Efinix\Deliverables\Assert\third-party\monalisa_520.jpg")
    border = np.array([62, 74, 88], np.uint8)

    def draw(q, tex, nu, nv, flip_v=False, edge=0.004):
        U, V, P = plane_grid(q["c00"], q["c10"], q["c01"], q["c11"], nu, nv)
        xs, ys, z = project(P, cam)
        xi = np.round(xs).astype(np.int32)
        yi = np.round(ys).astype(np.int32)
        ok = (xi >= 0) & (xi < W) & (yi >= 0) & (yi < H) & (z > 0)
        xi, yi, z, U, V = xi[ok], yi[ok], z[ok], U[ok], V[ok]
        idx = yi * W + xi
        th_, tw_ = tex.shape[:2]
        VV = (1 - V) if flip_v else V
        tx = np.clip((U * (tw_ - 1)).astype(np.int32), 0, tw_ - 1)
        ty = np.clip((VV * (th_ - 1)).astype(np.int32), 0, th_ - 1)
        col = tex[ty, tx].copy()
        on_edge = (U < edge) | (U > 1 - edge) | (VV < edge) | (VV > 1 - edge)
        col[on_edge] = border
        if mode == "on":
            nearer = z < zbuf[idx]
            img[idx[nearer]] = col[nearer]
            zbuf[idx[nearer]] = z[nearer]
        else:
            img[idx] = col
            zbuf[idx] = z

    draw(A, texA, 1500, 1150)
    draw(B, texB, 1250, 1450, flip_v=True)
    out = Path(__file__).parent / (f"depth_scene_{mode}.png")
    Image.fromarray(img.reshape(H, W, 3)).save(out)
    print(mode, "ok")

for m in ("off", "on"):
    render(m)

for nm, q in (("A", A), ("B", B)):
    out = []
    for c in ("c00", "c10", "c01", "c11"):
        x, y, _ = project(q[c][None, :], cam)
        out.append(f"({float(x[0]):.0f},{float(y[0]):.0f})")
    print(nm, " ".join(out))
