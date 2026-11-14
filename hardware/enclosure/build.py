"""
Oil Tank Monitor - enclosure CAD (CadQuery). Units: mm.

Two assemblies:
  1. Gauge Cap  - clamps around the top of the float-gauge vial, VL53L1X looks down at the puck
  2. Hub        - magnet-mount enclosure for an ESP32 Feather-format board

Run:  python3 build.py <out_dir>
Outputs STL per printed part, a STEP of each assembly, and meshes.json for the viewer.
"""
import sys, os, json, math, base64
import numpy as np
import cadquery as cq

# ---------------------------------------------------------------- parameters
P = dict(
    # --- gauge vial (measure yours; King/Beckett-style vial is ~1.25 in dia x 4 in tall)
    vial_od=32.0,
    vial_h=100.0,
    # --- gauge cap
    cap_od=48.0,
    skirt_h=24.0,          # how far the collar reaches down around the vial
    liner_t=1.8,           # TPU liner wall; print thicker/thinner liners for other vials
    cap_floor=2.0,
    cap_cavity_h=12.0,
    cap_cavity_d=42.0,
    aperture_d=13.0,       # ToF field-of-view opening
    window_w=14.0,         # sight window so the puck stays readable at "Full"
    # --- ToF breakout (STEMMA QT / Qwiic format, 1.0 x 0.7 in)
    tof_l=25.4, tof_w=17.8, tof_t=1.6, tof_hole_dx=20.32, tof_hole_dy=12.7,
    # --- cable (JST-SH 4-pin "Qwiic" cable)
    cable_d=3.6,
    # --- hub
    hub_l=84.0, hub_w=46.0, hub_h=23.0, hub_r=11.0, wall=2.4,
    # --- Feather-format board
    fb_l=50.8, fb_w=22.86, fb_t=1.6, fb_hole_inset=2.54,
    fb_x0=-12.8,           # board left edge in hub coords (USB-C end sits at +X wall)
    standoff_h=4.0,
    magnet_d=10.0, magnet_t=3.0,
)

def drop_profile(scale=1.0):
    """Teardrop outline (oil drop logo), tip at +Y."""
    pts = []
    r = 1.0
    for i in range(0, 41):
        a = math.radians(-30 - i * (240 / 40.0))  # sweep the round bottom
        pts.append((r * math.cos(a), r * math.sin(a) - 0.35))
    pts.append((0.0, 1.75))
    return [(x * scale, y * scale) for x, y in pts]

def box(x0, x1, y0, y1, z0, z1):
    return (cq.Workplane("XY").box(x1 - x0, y1 - y0, z1 - z0, centered=False)
            .translate((x0, y0, z0)))

def cyl_z(d, z0, z1, x=0, y=0):
    return cq.Workplane("XY").workplane(offset=z0).center(x, y).circle(d / 2).extrude(z1 - z0)

def cyl_y(d, y0, y1, x=0, z=0):
    return (cq.Workplane("XZ").workplane(offset=-y0).center(x, z).circle(d / 2).extrude(-(y1 - y0)))

def cyl_x(d, x0, x1, y=0, z=0):
    return (cq.Workplane("YZ").workplane(offset=x0).center(y, z).circle(d / 2).extrude(x1 - x0))

# ================================================================ GAUGE CAP
# local origin: centre of vial top face. +X = cable exit, -Y = sight window, +Y = thumbscrew
def make_cap():
    p = P
    R = p["cap_od"] / 2
    top = p["cap_floor"] + p["cap_cavity_h"]
    bore_d = p["vial_od"] + 2 * p["liner_t"] + 0.4

    body = cyl_z(p["cap_od"], -p["skirt_h"], top)
    body = body.faces("<Z").edges().fillet(2.0)
    # cable strain-relief snout
    snout = box(R - 4, R + 6, -5.5, 5.5, 0.5, top).edges("|Z").edges(">X").fillet(2.5)
    body = body.union(snout)
    # thumbscrew boss
    body = body.union(cyl_y(11, R - 3, R + 4, z=-12).faces(">Y").edges().fillet(1.2))
    # bores
    body = body.cut(cyl_z(bore_d, -p["skirt_h"] - 1, 0))
    body = body.cut(cyl_z(p["cap_cavity_d"], p["cap_floor"], top + 1))
    body = body.cut(cyl_z(p["aperture_d"], -1, p["cap_floor"] + 1))
    # cable channel (cable drops in from above, lid tab closes it)
    body = body.cut(box(R - 6, R + 7, -p["cable_d"] / 2 - 0.4, p["cable_d"] / 2 + 0.4, 3.0, top + 1))
    # sight window
    w = p["window_w"]
    body = body.cut(box(-w / 2, w / 2, -R - 2, -8, -p["skirt_h"] - 1, -11))
    body = body.cut(cyl_y(w, -R - 2, -8, z=-11))
    # thumbscrew hole (M4 nylon thumbscrew threads straight into the plastic)
    body = body.cut(cyl_y(3.4, 10, R + 6, z=-12))
    # board standoffs + locating pins (board presses on, no screws)
    for sx in (-1, 1):
        for sy in (-1, 1):
            x, y = sx * p["tof_hole_dx"] / 2, sy * p["tof_hole_dy"] / 2
            body = body.union(cyl_z(4.6, p["cap_floor"], p["cap_floor"] + 3.2, x, y))
            body = body.union(cyl_z(2.2, p["cap_floor"], p["cap_floor"] + 6.0, x, y))

    # TPU liner: grips the vial, cushions the thumbscrew, blocks stray light
    liner = cyl_z(bore_d - 0.4, -p["skirt_h"] + 1, -0.2)
    liner = liner.cut(cyl_z(p["vial_od"] + 0.3, -p["skirt_h"], -1.4))
    liner = liner.cut(cyl_z(p["aperture_d"] + 5, -2, 1))
    liner = liner.cut(box(-w / 2, w / 2, -R, -8, -p["skirt_h"] - 1, -11))
    liner = liner.cut(cyl_y(w, -R, -8, z=-11))
    liner = liner.cut(box(8, R, -0.6, 0.6, -p["skirt_h"] - 1, -1.4))   # relief slit

    # lid: press-fit plug, tab closes the cable channel
    lid = cyl_z(p["cap_od"], top, top + 3.6).faces(">Z").edges().fillet(2.2)
    plug = cyl_z(p["cap_cavity_d"] - 0.3, top - 2.2, top).cut(cyl_z(p["cap_cavity_d"] - 3.6, top - 3, top))
    lid = lid.union(plug)
    lid_snout = box(R - 4, R + 6, -5.5, 5.5, top, top + 3.6).edges("|Z").edges(">X").fillet(2.5)
    lid_snout = lid_snout.faces(">Z").edges().fillet(1.5)
    lid = lid.union(lid_snout)
    lid = lid.union(box(R - 5.5, R + 5.5, -p["cable_d"] / 2 - 0.2, p["cable_d"] / 2 + 0.2,
                        3.0 + p["cable_d"] + 0.5, top))
    logo = (cq.Workplane("XY").workplane(offset=top + 3.0).polyline(drop_profile(5.2)).close().extrude(1.0))
    lid = lid.cut(logo)
    ring = cyl_z(38, top + 3.1, top + 4).cut(cyl_z(36.4, top + 3, top + 4.1))
    lid = lid.cut(ring)

    # thumbscrew (buy: M4 x 12 nylon thumbscrew) - modelled for the render
    screw = cyl_y(4.0, 17.4, R + 4.2, z=-12)
    head = (cq.Workplane("XZ").workplane(offset=-(R + 4.2)).center(0, -12)
            .polygon(12, 13).extrude(-6.5))
    head = head.faces(">Y").edges().fillet(1.0)
    screw = screw.union(head)
    return dict(cap_body=body, cap_liner=liner, cap_lid=lid, cap_thumbscrew=screw)

def make_tof_board():
    p = P
    z0 = p["cap_floor"] + 3.2
    pcb = box(-p["tof_l"] / 2, p["tof_l"] / 2, -p["tof_w"] / 2, p["tof_w"] / 2, z0, z0 + p["tof_t"])
    pcb = pcb.edges("|Z").fillet(2.0)
    for sx in (-1, 1):
        for sy in (-1, 1):
            pcb = pcb.cut(cyl_z(2.5, z0 - 1, z0 + 3, sx * p["tof_hole_dx"] / 2, sy * p["tof_hole_dy"] / 2))
    chip = box(-2.45, 2.45, -1.25, 1.25, z0 - 1.56, z0)            # VL53L1X, facing down
    conn = box(p["tof_l"] / 2 - 4.6, p["tof_l"] / 2 + 0.2, -3.0, 3.0, z0 - 2.9, z0)
    conn2 = box(-p["tof_l"] / 2 - 0.2, -p["tof_l"] / 2 + 4.6, -3.0, 3.0, z0 - 2.9, z0)
    return dict(tof_pcb=pcb, tof_chip=chip, tof_conn=conn.union(conn2))

# ================================================================ HUB
# local origin: centre of footprint, z=0 is the mounting face. -X = sensor cable, +X = USB-C
def rrect(l, w, r, z0, z1):
    return (cq.Workplane("XY").workplane(offset=z0).rect(l, w).extrude(z1 - z0).edges("|Z").fillet(r))

def make_hub():
    p = P
    L, W, H, R, t = p["hub_l"], p["hub_w"], p["hub_h"], p["hub_r"], p["wall"]
    zb = t                                   # top of base plate
    board_z = zb + p["standoff_h"]           # underside of PCB
    usb_z = board_z + p["fb_t"] + 1.6        # USB-C centreline
    cab_z = 7.4                              # sensor cable centreline

    # ---- base
    base = rrect(L, W, R, 0, zb).faces("<Z").edges().chamfer(0.6)
    lip = rrect(L - 2 * t - 0.3, W - 2 * t - 0.3, R - t, zb, zb + 3.6).cut(
        rrect(L - 2 * t - 3.5, W - 2 * t - 3.5, R - t - 1.6, zb - 1, zb + 5))
    lip = lip.cut(box(L / 2 - 8, L / 2, -7.5, 7.5, zb, zb + 6))       # USB side opening
    lip = lip.cut(box(-L / 2, -L / 2 + 8, -5.5, 5.5, zb, zb + 6))     # cable side opening
    base = base.union(lip)
    # saddle under the cable grommet
    base = base.union(box(-L / 2 + 0.2, -L / 2 + t, -2.9, 2.9, zb, cab_z - 3.0))
    hx0 = p["fb_x0"] + p["fb_hole_inset"]
    hx1 = p["fb_x0"] + p["fb_l"] - p["fb_hole_inset"]
    hy = p["fb_w"] / 2 - p["fb_hole_inset"]
    for x in (hx0, hx1):
        for y in (-hy, hy):
            base = base.union(cyl_z(5.0, zb, board_z, x, y))
            base = base.union(cyl_z(1.9, zb, board_z + 2.6, x, y))
    magnets = [(-29.0, 11.0), (-29.0, -11.0), (12.6, 0.0)]
    for (x, y) in magnets:
        base = base.union(cyl_z(p["magnet_d"] + 4, zb, p["magnet_t"] + 1.4, x, y))
    for (x, y) in magnets:
        base = base.cut(cyl_z(p["magnet_d"] + 0.2, -1, p["magnet_t"] + 0.2, x, y))
    mag = None
    for (x, y) in magnets:
        m = cyl_z(p["magnet_d"], 0.1, p["magnet_t"] + 0.1, x, y)
        mag = m if mag is None else mag.union(m)

    # ---- lid (shell)
    lid = rrect(L, W, R, zb, H).faces(">Z").edges().fillet(5.0)
    cav = rrect(L - 2 * t, W - 2 * t, R - t, zb - 1, H - t).faces(">Z").edges().fillet(3.0)
    lid = lid.cut(cav)
    # USB-C opening + recess for the plug overmold
    lid = lid.cut(box(L / 2 - t - 1, L / 2 + 1, -4.9, 4.9, usb_z - 2.1, usb_z + 2.1).edges("|X").fillet(1.9))
    lid = lid.cut(box(L / 2 - 1.0, L / 2 + 1, -6.6, 6.6, usb_z - 3.6, usb_z + 3.6).edges("|X").fillet(3.0))
    # sensor cable notch (open at the bottom so the cable drops in - nothing to thread through)
    lid = lid.cut(box(-L / 2 - 1, -L / 2 + t + 1, -3.0, 3.0, zb - 1, cab_z))
    lid = lid.cut(cyl_x(6.0, -L / 2 - 1, -L / 2 + t + 1, z=cab_z))
    # status light pipe + recessed reset pinhole
    lp = (18.0, 6.5)
    lid = lid.cut(cyl_z(3.2, H - t - 1, H + 1, *lp))
    lid = lid.cut(cyl_z(1.8, H - t - 1, H + 1, 6.0, -8.0))
    lid = lid.cut(cyl_z(4.2, H - 0.7, H + 1, 6.0, -8.0))
    # logo
    logo = (cq.Workplane("XY").workplane(offset=H - 0.6).polyline(drop_profile(6.0)).close().extrude(1.0)
            .rotate((0, 0, 0), (0, 0, 1), -90).translate((-22, 0, 0)))
    lid = lid.cut(logo)

    pipe = cyl_z(3.0, board_z + p["fb_t"] + 1.2, H + 0.3, *lp).union(
        cyl_z(4.6, H - t - 1.2, H - t, *lp))

    # ---- TPU cable grommet
    x_out, x_in = -L / 2, -L / 2 + t
    g = cyl_x(6.0, x_out - 0.2, x_in + 0.2, z=cab_z)
    g = g.union(cyl_x(9.0, x_in + 0.1, x_in + 1.7, z=cab_z))
    g = g.union(cyl_x(9.0, x_out - 2.2, x_out - 0.1, z=cab_z))
    tail = (cq.Workplane("YZ").workplane(offset=x_out - 2.2).center(0, cab_z).circle(3.6)
            .workplane(offset=-8).circle(2.5).loft())
    g = g.union(tail)
    g = g.cut(cyl_x(P["cable_d"], x_out - 12, x_in + 3, z=cab_z))
    return dict(hub_base=base, hub_lid=lid, hub_lightpipe=pipe, hub_grommet=g, hub_magnets=mag), dict(
        board_z=board_z, usb_z=usb_z, cab_z=cab_z)

def make_feather(board_z):
    p = P
    x0, x1 = p["fb_x0"], p["fb_x0"] + p["fb_l"]
    w = p["fb_w"] / 2
    zt = board_z + p["fb_t"]
    pcb = box(x0, x1, -w, w, board_z, zt).edges("|Z").fillet(2.4)
    for x in (x0 + p["fb_hole_inset"], x1 - p["fb_hole_inset"]):
        for y in (-(w - p["fb_hole_inset"]), w - p["fb_hole_inset"]):
            pcb = pcb.cut(cyl_z(2.3, board_z - 1, zt + 1, x, y))
    module = box(x0 + 6.5, x0 + 23.1, -6.6, 6.6, zt, zt + 2.4)          # ESP32 module can
    usb = box(x1 - 6.4, x1 + 0.9, -4.47, 4.47, zt, zt + 3.2).edges("|X").fillet(1.2)
    qt = box(x0 + 27.0, x0 + 33.0, w - 4.6, w + 0.1, zt, zt + 2.9)       # STEMMA QT / Qwiic socket
    jst = box(x1 - 14.5, x1 - 7.0, -w + 0.2, -w + 6.0, zt, zt + 5.0)     # battery socket (unused)
    btn = box(x0 + 26.0, x0 + 30.0, -8.6, -5.6, zt, zt + 1.6)
    led = box(x0 + 29.6, x0 + 32.0, 5.3 - 1.2 + 1.2, 5.3 + 2.4, zt, zt + 0.9).translate((0, -1.2 + 1.2, 0))
    return dict(fb_pcb=pcb, fb_module=module, fb_usb=usb, fb_qt=qt, fb_misc=jst.union(btn), fb_led=led)

# ================================================================ export
def tess(shape, tol=0.04, ang=0.25):
    verts, tris = shape.val().tessellate(tol, ang)
    V = np.array([[v.x, v.y, v.z] for v in verts], dtype=np.float64)
    T = np.array(tris, dtype=np.int64)
    return V, T

def creased(V, T, crease_deg=38.0):
    """Non-indexed positions + normals, smoothing only across shallow edges."""
    tri = V[T]                                                   # (n,3,3)
    fn = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    area = np.linalg.norm(fn, axis=1, keepdims=True)
    keep = area[:, 0] > 1e-12
    tri, fn, area, Tk = tri[keep], fn[keep], area[keep], T[keep]
    fnu = fn / area
    key = np.round(V, 4)
    _, vid = np.unique(key, axis=0, return_inverse=True)
    vid = vid.reshape(-1)
    Tv = vid[Tk]                                                 # welded vertex ids
    order = np.argsort(Tv.reshape(-1), kind="stable")
    flat = Tv.reshape(-1)[order]
    starts = np.flatnonzero(np.r_[True, flat[1:] != flat[:-1]])
    ends = np.r_[starts[1:], len(flat)]
    N = np.zeros((len(Tk), 3, 3))
    cosc = math.cos(math.radians(crease_deg))
    for s, e in zip(starts, ends):
        idx = order[s:e]
        f, c = idx // 3, idx % 3
        nf = fnu[f]
        w = fn[f]                                                # area-weighted
        m = (nf @ nf.T) >= cosc
        acc = m @ w
        acc /= np.maximum(np.linalg.norm(acc, axis=1, keepdims=True), 1e-12)
        N[f, c] = acc
    return tri.reshape(-1, 3).astype(np.float32), N.reshape(-1, 3).astype(np.float32)

def b64(a):
    return base64.b64encode(np.ascontiguousarray(a).tobytes()).decode()

def main(out):
    os.makedirs(out + "/stl", exist_ok=True)
    cap = make_cap()
    tof = make_tof_board()
    hub, hz = make_hub()
    fb = make_feather(hz["board_z"])
    parts = {}
    parts.update(cap); parts.update(tof); parts.update(hub); parts.update(fb)

    printable = ["cap_body", "cap_liner", "cap_lid", "hub_base", "hub_lid", "hub_grommet", "hub_lightpipe"]
    report = {}
    for n in printable:
        cq.exporters.export(parts[n], f"{out}/stl/{n}.stl", tolerance=0.02, angularTolerance=0.15)
        s = parts[n].val()
        bb = s.BoundingBox()
        report[n] = dict(valid=bool(s.isValid()), volume_cm3=round(s.Volume() / 1000, 2),
                         size=[round(bb.xlen, 1), round(bb.ylen, 1), round(bb.zlen, 1)])

    for name, group in (("gauge_cap", {**cap, **tof}), ("hub", {**hub, **fb})):
        asm = cq.Assembly()
        for n, s in group.items():
            asm.add(s, name=n)
        asm.save(f"{out}/{name}.step")

    # interference checks between parts that must not collide
    def inter(a, b):
        try:
            return round(parts[a].intersect(parts[b]).val().Volume(), 3)
        except Exception:
            return 0.0
    pairs = [("cap_body", "cap_lid"), ("cap_body", "cap_liner"), ("cap_body", "tof_pcb"),
             ("cap_body", "tof_conn"), ("cap_body", "tof_chip"), ("cap_lid", "tof_pcb"),
             ("cap_body", "cap_thumbscrew"),
             ("hub_base", "hub_lid"), ("hub_base", "fb_pcb"), ("hub_lid", "fb_pcb"), ("hub_lid", "fb_usb"),
             ("hub_lid", "fb_misc"), ("hub_lid", "fb_module"), ("hub_lid", "hub_lightpipe"),
             ("hub_lid", "hub_grommet"), ("hub_base", "hub_grommet"), ("hub_base", "hub_magnets"),
             ("hub_lightpipe", "fb_led"), ("hub_base", "fb_usb")]
    report["interference_mm3"] = {f"{a}|{b}": inter(a, b) for a, b in pairs}

    meshes = {}
    for n, s in parts.items():
        V, T = tess(s)
        pos, nor = creased(V, T)
        meshes[n] = dict(p=b64(pos), n=b64(nor), count=int(len(pos)))
    meta = dict(P=P, hub=hz)
    json.dump(dict(meshes=meshes, meta=meta), open(f"{out}/meshes.json", "w"))
    json.dump(report, open(f"{out}/report.json", "w"), indent=1)
    print(json.dumps(report, indent=1))
    print("mesh bytes", os.path.getsize(f"{out}/meshes.json"))

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "out")
