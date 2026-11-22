"""Prototype stand: a wall holding five JST XH 7-pin housings, with a
breadboard tray behind it. Each screen plugs into its housing and is held up
by it.

Push each housing into its hole from the back, mating face first, until it
stops against the lip at the front. The screen's pins reach the housing
through the opening in that lip; the wires trail out the back to the board.

Run:  python screen_holder.py          -> screen_holder.stl (full stand)
      python screen_holder.py --test   -> screen_holder_test.stl (one hole, to check the fit)
Needs: pip install manifold3d trimesh numpy
All sizes in mm.
"""

import sys
from pathlib import Path

import manifold3d as mf
import numpy as np
import trimesh

# --- JST XH 7-pin housing ---------------------------------------------------------
HOUSING_W = 19.9  # long side (7 pins x 2.5 mm pitch + walls)
HOUSING_H = 5.75  # short side
FIT = 0.4  # added to the housing's width and height: snug slide, not a press fit
FACE_W = 18.5  # opening in the front lip for the screen's pins and their plastic strip
FACE_H = 3.5

# --- Wall -----------------------------------------------------------------------
COUNT = 5
PITCH = 42.0  # hole centre to centre (screens are ~38.5 mm round)
END_MARGIN = 21.5  # hole centre to wall end -> 211 mm overall
WALL_T = 7.5  # front to back; the housing pokes out the back if it is longer
LIP_T = 1.5  # front lip that the housing stops against
ABOVE_HOLE = 4.5  # wall left above the top of each hole
BRACE = 35.0  # triangular braces at both ends, in line with the tray side walls
BRACE_T = 2.0  # same as the tray side walls (RIM_T), so they line up

# --- Breadboard tray ------------------------------------------------------------------
BOARD_W = 175.0  # along the wall
BOARD_D = 67.0  # front to back
BOARD_FIT = 1.0  # clearance all round
FLOOR_T = 2.0
RIM_T = 2.0
RIM_H = 5.0  # rim height above the floor
BOARD_T = 9.5  # breadboard thickness (measure yours)
HOLE_ABOVE_BOARD = 33.5  # hole centre above the top of the breadboard (45 mm off the desk)
HOLE_Z = FLOOR_T + BOARD_T + HOLE_ABOVE_BOARD

TEST_END_MARGIN = 15.0  # test piece: one hole, 30 mm wide, short tray stub
TEST_TRAY_D = 15.0
TEST_HOLE_Z = 12.0  # test piece stays short: it only checks the housing fit


def box(x0, x1, y0, y1, z0, z1):
    return mf.Manifold.cube([x1 - x0, y1 - y0, z1 - z0]).translate([x0, y0, z0])


def build(count=COUNT, end_margin=END_MARGIN, tray_d=None, hole_z=HOLE_Z):
    hw, hh = (HOUSING_W + FIT) / 2, (HOUSING_H + FIT) / 2
    wall_h = hole_z + hh + ABOVE_HOLE
    width = PITCH * (count - 1) + 2 * end_margin
    xs = [end_margin + i * PITCH for i in range(count)]

    # Wall: front face at y=0, back face at y=WALL_T.
    body = box(0, width, 0, WALL_T, 0, wall_h)

    # Tray behind the wall: floor plus a rim on the sides and back.
    inner_d = BOARD_D + BOARD_FIT if tray_d is None else tray_d
    tray_y1 = WALL_T + inner_d + RIM_T
    body += box(0, width, 0, tray_y1, 0, FLOOR_T)
    body += box(0, RIM_T, 0, tray_y1, 0, FLOOR_T + RIM_H)
    body += box(width - RIM_T, width, 0, tray_y1, 0, FLOOR_T + RIM_H)
    if tray_d is None:
        body += box(0, width, tray_y1 - RIM_T, tray_y1, 0, FLOOR_T + RIM_H)
        # End braces: triangles in the Y-Z plane, built on top of the side rims.
        tri = mf.CrossSection([np.array([[0, 0], [BRACE, 0], [0, BRACE]], float)])
        for x in (0, width - BRACE_T):
            g = mf.Manifold.extrude(tri, BRACE_T).rotate([90, 0, 90])  # local (y, z, t) -> (t, y, z)
            body += g.translate([x, WALL_T, FLOOR_T])

    cuts = []
    for x in xs:
        # Housing pocket from the back, stopping at the front lip.
        cuts.append(box(x - hw, x + hw, LIP_T, WALL_T + 1, hole_z - hh, hole_z + hh))
        # Opening through the front lip for the screen's pins.
        cuts.append(box(x - FACE_W / 2, x + FACE_W / 2, -1, LIP_T + 0.01,
                        hole_z - FACE_H / 2, hole_z + FACE_H / 2))
    return body - mf.Manifold.batch_boolean(cuts, mf.OpType.Add)


def to_trimesh(m):
    mesh = m.to_mesh()
    return trimesh.Trimesh(np.array(mesh.vert_properties)[:, :3], np.array(mesh.tri_verts))


if __name__ == "__main__":
    test = "--test" in sys.argv[1:]
    m = build(1, TEST_END_MARGIN, TEST_TRAY_D, TEST_HOLE_Z) if test else build()
    t = to_trimesh(m)
    out = Path(__file__).resolve().parent / ("screen_holder_test.stl" if test else "screen_holder.stl")
    t.export(out)
    print(f"{out}  {t.extents.round(1).tolist()} mm  watertight={t.is_watertight}  "
          f"bodies={len(t.split(only_watertight=False))}")
