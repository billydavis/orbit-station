"""Look-and-fit test for one Orbit Station screen pod: a round housing with short
tube stubs above and below. The tube is what will link pods; the lower stub
holds the screen's tab.

Prints as two parts, both flat on the bed:
  pod_test_front.stl  face down; the screen drops in from behind
  pod_test_back.stl   open side up; its ring locates it in the front

Run:  python pod_test.py
Needs: pip install manifold3d trimesh numpy
All sizes in mm. Z is the viewing direction, +Y is up.
"""

from pathlib import Path

import manifold3d as mf
import numpy as np
import trimesh

SEGMENTS = 160

# --- Screen ---------------------------------------------------------------------
MODULE_D = 38.5  # round board, from the screen holder's notes
VISIBLE_D = 33.0  # bezel opening (the display's active area is about 32.4)
MODULE_T = 3.5  # PLACEHOLDER: front of glass to back of the round board
TAB_L = 10.0  # PLACEHOLDER: tab length below the round edge
TAB_W = 22.0  # PLACEHOLDER: tab width
TAB_T = 1.6  # PLACEHOLDER: tab thickness
FIT = 0.3  # clearance per side around the round board and the tab

# --- Pod ------------------------------------------------------------------------
POD_D = 46.0
POD_DEPTH = 22.0
FRONT_ROUND = 2.0  # rounding on the outer front edge
BACK_ROUND = 4.0  # rounding on the outer back edge
LIP_T = 1.2  # material in front of the glass
BEZEL_CHAMFER = 1.6  # how far the bezel's cone widens toward the front
WALL = 2.0  # back wall

# --- Tube -----------------------------------------------------------------------
TUBE_W = 28.0
TUBE_Z0, TUBE_Z1 = 4.8, 20.8  # back and front of the tube
TUBE_ROUND = 4.0
BORE_W = 24.0
BORE_Z0, BORE_Z1 = 6.8, 19.2
BORE_ROUND = 1.0
STUB = 33.0  # tube reaches this far from the pod centre, up and down

# --- Split ----------------------------------------------------------------------
SPLIT_Z = 15.0
RING_H = 1.5  # locating ring on the back part
RING_T = 1.3
RING_FIT = 0.2


def rounded_rect(x0, x1, y0, y1, r, n=16):
    corners = [(x1 - r, y1 - r, 0), (x0 + r, y1 - r, 90), (x0 + r, y0 + r, 180), (x1 - r, y0 + r, 270)]
    pts = []
    for cx, cy, start in corners:
        for a in np.radians(np.linspace(start, start + 90, n)):
            pts.append((cx + r * np.cos(a), cy + r * np.sin(a)))
    return mf.CrossSection([np.array(pts, float)])


def along_y(section, half_length):
    """Extrude a section drawn in (x, z) along Y, centred on the pod."""
    return mf.Manifold.extrude(section, 2 * half_length).translate([0, 0, -half_length]).rotate([90, 0, 0])


def revolve(points):
    return mf.Manifold.revolve(mf.CrossSection([np.array(points, float)]), circular_segments=SEGMENTS)


def arc(cx, cy, r, a0, a1, n=16):
    return [(cx + r * np.cos(a), cy + r * np.sin(a)) for a in np.radians(np.linspace(a0, a1, n))]


def box(x0, x1, y0, y1, z0, z1):
    return mf.Manifold.cube([x1 - x0, y1 - y0, z1 - z0]).translate([x0, y0, z0])


def cylinder(r, z0, z1):
    return revolve([(0, z0), (r, z0), (r, z1), (0, z1)])


def build():
    R = POD_D / 2
    pocket_r = MODULE_D / 2 + FIT
    open_r = VISIBLE_D / 2
    stop_z = POD_DEPTH - LIP_T

    outer = [(0, 0)]
    outer += arc(R - BACK_ROUND, BACK_ROUND, BACK_ROUND, -90, 0)
    outer += arc(R - FRONT_ROUND, POD_DEPTH - FRONT_ROUND, FRONT_ROUND, 0, 90)
    outer += [(0, POD_DEPTH)]
    pod = revolve(outer)
    tube = along_y(rounded_rect(-TUBE_W / 2, TUBE_W / 2, TUBE_Z0, TUBE_Z1, TUBE_ROUND), STUB)

    bore = along_y(rounded_rect(-BORE_W / 2, BORE_W / 2, BORE_Z0, BORE_Z1, BORE_ROUND), STUB + 1)
    pocket = cylinder(pocket_r, WALL, stop_z)
    opening = cylinder(open_r, stop_z - 0.1, POD_DEPTH + 1)
    cone = revolve([(0, stop_z), (open_r, stop_z), (open_r + BEZEL_CHAMFER, POD_DEPTH + 0.01), (0, POD_DEPTH + 0.01)])
    body = (pod + tube) - bore - pocket - opening - cone

    keep_front = box(-60, 60, -60, 60, SPLIT_Z, POD_DEPTH + 1)
    keep_back = box(-60, 60, -60, 60, -1, SPLIT_Z)
    front = body ^ keep_front
    back = body ^ keep_back

    ring_r = pocket_r - RING_FIT
    inner_r, root_r = ring_r - RING_T, pocket_r + 0.5  # root reaches into the wall
    ring = revolve([(inner_r, SPLIT_Z), (root_r, SPLIT_Z - (root_r - inner_r)), (root_r, SPLIT_Z),
                    (ring_r, SPLIT_Z), (ring_r, SPLIT_Z + RING_H), (inner_r, SPLIT_Z + RING_H)])  # 45-degree underside
    back += ring - bore

    board_z = stop_z - MODULE_T
    module = cylinder(MODULE_D / 2, board_z, stop_z)
    tab = box(-TAB_W / 2, TAB_W / 2, -(MODULE_D / 2 + TAB_L), -MODULE_D / 2 + 1, board_z, board_z + TAB_T)
    return front, back, module + tab


def to_trimesh(m):
    mesh = m.to_mesh()
    return trimesh.Trimesh(np.array(mesh.vert_properties)[:, :3], np.array(mesh.tri_verts))


if __name__ == "__main__":
    front, back, screen = build()
    here = Path(__file__).resolve().parent

    clash_front = (front ^ screen).volume()
    clash_back = (back ^ screen).volume()
    print(f"screen overlap: front {clash_front:.3f} mm3, back {clash_back:.3f} mm3")

    printed_front = front.rotate([180, 0, 0]).translate([0, 0, POD_DEPTH])
    for name, part in (("pod_test_front.stl", printed_front), ("pod_test_back.stl", back)):
        t = to_trimesh(part)
        t.export(here / name)
        print(f"{name}  {t.extents.round(1).tolist()} mm  watertight={t.is_watertight}  "
              f"bodies={len(part.decompose())}  volume={t.volume / 1000:.1f} cm3")
