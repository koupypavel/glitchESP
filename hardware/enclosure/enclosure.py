"""
glitchESP enclosure, generated with CadQuery.

    python enclosure.py            -> stl/*.stl, step/enclosure.step, build report

Coordinates are those of Waveshare's 3D model of the board (millimetres): seen from the
front, +X is right, +Y is up (the USB-C ports are at the bottom), +Z comes out of the
screen. The glass front is at z = +3.0, the back of the PCB at z = -4.6, the four M2.5
standoffs end at z = -8.6. All board positions below were read from that model
(doc/README.md says where to get it); everything about parts the model does not contain
(camera, battery, speaker, push-button, encoder) is a parameter in the first section.

The case is a tub printed back-down. The display unit drops in from the front and is
fastened through the back with four M2.5 screws into its own standoffs, which is how the
board is meant to be mounted. Behind the board: a battery bay with a cover plate, a channel
over the 40-pin header for the control wiring, and at the top a "forehead" that holds the
speaker and the shutter button. The lens is held by a small hood that is screwed on from
outside and can slide a few millimetres, because the kit camera lies loose on its cable.
"""
import math
import os

import cadquery as cq

# ---------------------------------------------------------------- parameters you may change

BATTERY = (56.75, 88.0, 9.0)        # bay width (x), length (y), cell thickness (z)
SPEAKER = (26.0, 26.0, 5.0)         # along the top edge (x), depth into the case (z), thickness (y)
SHUTTER_HOLE_D = 12.2               # 12 mm panel push-button
SHUTTER_NUT_D = 16.8                # room for its nut (across corners)
ENCODER_HOLE_D = 7.2                # EC11: M7 bush
ENCODER_BODY = 12.6                 # EC11 body is 12 x 12 mm
LENS_Y = 48.0                       # lens centre above the board centre line (estimate from Waveshare's photo)
LENS_TRAVEL = 3.5                   # the hood can slide this far up and down
LENS_BLOCK = 8.5                    # the lens block is about 8.5 x 8.5 mm ...
LENS_TOP_Z = -10.5                  # ... and its top is about 5.9 mm above the PCB
FOV_HALF_X = 17.0                   # half angles the hood must leave free, degrees: across the
FOV_HALF_Y = 27.0                   # screen's short side and along its long side, with margin
SCREW_LEN = 12.0                    # M2.5 x 12 into the standoffs

WALL = 2.4                          # side walls
BACK = 2.0                          # back wall
FIT = 0.4                           # gap around the glass
RIM = 0.5                           # the wall stands this much proud of the glass

# ---------------------------------------------------------------- the board (from the STEP model)

GLASS_W, GLASS_H, GLASS_R = 70.7, 126.9, 3.0
GLASS_FRONT = 3.0
PCB_BACK = -4.6
STANDOFF_Z = -8.6                                   # back faces of the M2.5 standoffs
STANDOFFS = [(-28.5, -58.65), (28.5, -58.65), (-28.5, 53.35), (28.5, 53.35)]
USB_X = (-8.97, 8.97)                               # two USB-C sockets in the bottom edge
USB_Z = -6.22
KEY_Z = -5.3                                        # side buttons on the right edge:
KEY_POWER_Y, KEY_BOOT_Y, KEY_RESET_Y = 34.24, 22.55, 10.87
KEY_TIP_X = 32.74                                   # where their actuators end
SD_Y, SD_Z = -2.72, -5.6                            # card slot on the right edge
MIC_Y, MIC_Z = -47.3, -5.1                          # microphones near both side edges
LED_X, LED_Z = -19.8, -4.85                         # power LED near the bottom edge
HEADER_X = (-31.13, -24.52)                         # 40-pin header (reaches z = -9.8)
SPK_CONN = (25.69, 30.54, -35.59, -29.83)           # speaker connector: x0, x1, y0, y1

# ---------------------------------------------------------------- derived sizes

IN_X = GLASS_W / 2 + FIT                            # cavity half width            35.75
IN_Y = GLASS_H / 2 + FIT                            # cavity half height           63.85
OUT_X = IN_X + WALL
FOREHEAD = SPEAKER[2] + 0.65 + WALL                 # extra length above the glass
TOP_IN = IN_Y + FOREHEAD - WALL                     # inner face of the top wall
TOP_OUT = IN_Y + FOREHEAD
BOT_OUT = -(IN_Y + WALL)
FRONT = GLASS_FRONT + RIM                           # front edge of the walls
BAY_DEPTH = BATTERY[2] + 0.5                        # battery plus a little room (at least)
PLATE_T = 1.6                                       # cover plate over the battery
PLATE_BACK = STANDOFF_Z - 2.4 - PLATE_T             # plate sits 2.4 mm behind the standoff faces
ROOF_Z = FRONT - 1.2                                # the speaker pocket leaves 1.2 mm of front wall
# inner face of the back wall: deep enough for the battery and for the speaker standing in
# the forehead, whichever needs more
BACK_IN = min(PLATE_BACK - BAY_DEPTH, ROOF_Z - (SPEAKER[1] + 0.6))
BACK_OUT = BACK_IN - BACK

BAY_X0 = IN_X - BATTERY[0]                          # battery bay: against the right wall
BAY_Y0, BAY_Y1 = -54.9, -54.9 + BATTERY[1]
RIB = 1.2

# shutter button in the right wall: between the POWER button and the top-right standoff,
# low enough that its nut clears the board's parts
SHUTTER_Y, SHUTTER_Z = 43.2, BACK_IN + SHUTTER_NUT_D / 2 + 0.05
SHUTTER_DEPTH = 19.0                # how far the button reaches into the case behind the wall
ENCODER_Y, ENCODER_Z = -22.0, BACK_IN + ENCODER_BODY / 2 + 0.5
SPK_X0 = -IN_X + 1.55                               # speaker pocket in the forehead, left
SPK_X1 = SPK_X0 + SPEAKER[0] + 1.2
SPK_ZC = BACK_IN + 0.3 + SPEAKER[1] / 2                # speaker centre (depth)
_n = int((SPEAKER[0] - 6.0) // 4.0) + 1               # grille slots, 4 mm apart, centred on the speaker
GRILLE_X = [SPK_X0 + 0.6 + SPEAKER[0] / 2 + (i - (_n - 1) / 2) * 4.0 for i in range(_n)]

HOOD_T = 1.6                                        # hood flange, lies on the outside of the back
HOOD_WALL = 1.4
HOOD_SHOULDER_Z = LENS_TOP_Z - 1.3                  # the lens block may be this much taller
HOOD_SOCKET = LENS_BLOCK + 0.4
HOOD_APERTURE = 3.2                                 # half width of the opening above the lens
HOOD_FRONT_Z = HOOD_SHOULDER_Z + 3.5                # how far the socket reaches towards the board
HOOD_OUT_Z = BACK_OUT - HOOD_T
HOOD_SCREWS = [(-12.5, 9.0), (-12.5, -9.0), (12.5, -9.0)]      # relative to the lens centre

PLATE_BOSSES = [(0.0, -59.6), (-19.0, BAY_Y1 + RIB + 3.3)]     # the second one left of the lens, clear of the shutter
POST_D, BOSS_D = 8.0, 5.5


def hood_half(z, wall=0.0):
    """Half sizes (x, y) of the hood's opening at depth z, plus `wall`."""
    d = HOOD_SHOULDER_Z - z
    return (HOOD_APERTURE + d * math.tan(math.radians(FOV_HALF_X)) + wall,
            HOOD_APERTURE + d * math.tan(math.radians(FOV_HALF_Y)) + wall)


# ---------------------------------------------------------------- small helpers

def box(x0, x1, y0, y1, z0, z1):
    return cq.Workplane("XY").box(x1 - x0, y1 - y0, z1 - z0, centered=False).translate((x0, y0, z0))


def rounded(x0, x1, y0, y1, z0, z1, r):
    return box(x0, x1, y0, y1, z0, z1).edges("|Z").fillet(r)


def cyl_z(x, y, z0, z1, d):
    return cq.Workplane("XY").circle(d / 2).extrude(z1 - z0).translate((x, y, z0))


def cyl_x(x0, x1, y, z, d):
    return cq.Workplane("YZ").circle(d / 2).extrude(x1 - x0).translate((x0, y, z))


def cyl_y(y0, y1, x, z, d):
    return cq.Workplane("XZ").circle(d / 2).extrude(-(y1 - y0)).translate((x, y0, z))


def slot_y(y0, y1, x, z, w, h, angle=0):
    """A rounded slot through a wall that faces +-Y: w along x, h along z (angle 90 swaps them)."""
    return (cq.Workplane("XZ").slot2D(w, h, angle).extrude(-(y1 - y0)).translate((x, y0, z)))


def slot_x(x0, x1, y, z, w, h):
    """A rounded slot through a wall that faces +-X: w along y, h along z."""
    return (cq.Workplane("YZ").slot2D(w, h).extrude(x1 - x0).translate((x0, y, z)))


def frustum(z0, half0, z1, half1):
    """Rectangular pyramid stump between two z levels, centred on the z axis."""
    return (cq.Workplane("XY").workplane(offset=z0).rect(2 * half0[0], 2 * half0[1])
            .workplane(offset=z1 - z0).rect(2 * half1[0], 2 * half1[1]).loft(combine=True))


# ---------------------------------------------------------------- the body

def make_body():
    body = rounded(-OUT_X, OUT_X, BOT_OUT, TOP_OUT, BACK_OUT, FRONT, GLASS_R + FIT + WALL)
    body = body.faces("<Z").edges().chamfer(1.2)

    # the cavity has the outline of the glass all the way down
    body = body.cut(rounded(-IN_X, IN_X, -IN_Y, IN_Y, BACK_IN, FRONT + 1, GLASS_R + FIT))

    # forehead: a pocket for the speaker (it stands against the top wall and plays through it)
    body = body.cut(box(SPK_X0, SPK_X1, IN_Y, TOP_IN, BACK_IN, ROOF_Z))
    for x in GRILLE_X:                                           # speaker grille
        body = body.cut(slot_y(TOP_IN - 1, TOP_OUT + 1, x, SPK_ZC, 0.6 * SPEAKER[1], 1.5, 90))

    # posts under the four standoffs
    for x, y in STANDOFFS:
        body = body.union(cyl_z(x, y, BACK_IN - 0.1, STANDOFF_Z, POST_D))

    # battery bay: three ribs, the right wall of the case is the fourth side
    rib_top = PLATE_BACK
    body = body.union(box(BAY_X0 - RIB, BAY_X0, BAY_Y0 - RIB, BAY_Y1 + RIB, BACK_IN - 0.1, rib_top))
    body = body.union(box(BAY_X0 - RIB, IN_X + 0.1, BAY_Y0 - RIB, BAY_Y0, BACK_IN - 0.1, rib_top))
    body = body.union(box(BAY_X0 - RIB, IN_X + 0.1, BAY_Y1, BAY_Y1 + RIB, BACK_IN - 0.1, rib_top))
    for x, y in PLATE_BOSSES:                                    # the plate is screwed to these
        body = body.union(cyl_z(x, y, BACK_IN - 0.1, rib_top, BOSS_D))
    for dx, dy in HOOD_SCREWS:                                   # the hood is screwed to these
        body = body.union(cyl_z(dx, LENS_Y + dy, BACK_IN - 0.1, BACK_IN + 2.0, BOSS_D))

    # --- everything below removes material ---

    # a gap in the bay's left rib, where the battery lead leaves towards its connector
    body = body.cut(box(BAY_X0 - RIB - 0.5, BAY_X0 + 0.5, -42.0, -36.0, BACK_IN, PLATE_BACK + 1))

    seat = STANDOFF_Z - (SCREW_LEN - 3.5)                        # screw head rests here
    for x, y in STANDOFFS:
        body = body.cut(cyl_z(x, y, BACK_OUT - 1, STANDOFF_Z + 1, 2.9))
        body = body.cut(cyl_z(x, y, BACK_OUT - 1, seat, 5.2))
    for x, y in PLATE_BOSSES:
        body = body.cut(cyl_z(x, y, BACK_IN + 1.0, rib_top + 1, 1.7))
    for dx, dy in HOOD_SCREWS:
        body = body.cut(cyl_z(dx, LENS_Y + dy, BACK_OUT - 1, BACK_IN + 3, 1.7))

    # the opening for the lens hood: its cross-section at the back wall, plus the travel
    hx, hy = hood_half(BACK_OUT, HOOD_WALL + 0.3)        # the hood is widest at the outside
    y_top = min(LENS_Y + hy + LENS_TRAVEL, IN_Y - 0.4)
    body = body.cut(box(-hx, hx, LENS_Y - hy - LENS_TRAVEL, y_top, BACK_OUT - 1, BACK_IN + 0.01))

    # bottom edge: two USB-C plugs (the opening takes the plug's moulding), power LED
    for x in USB_X:
        body = body.cut(slot_y(BOT_OUT - 1, -IN_Y + 1, x, USB_Z, 13.0, 7.2))
    body = body.cut(cyl_y(BOT_OUT - 1, -IN_Y + 1, LED_X, LED_Z, 2.0))

    # right edge: POWER and BOOT get printed plungers, RESET a pin hole, the card a slot
    for y in (KEY_POWER_Y, KEY_BOOT_Y):
        body = body.cut(cyl_x(IN_X - 1, OUT_X + 1, y, KEY_Z, 3.6))
    body = body.cut(cyl_x(IN_X - 1, OUT_X + 1, KEY_RESET_Y, KEY_Z, 2.2))
    body = body.cut(slot_x(IN_X - 1, OUT_X + 1, SD_Y, SD_Z, 15.0, 5.0))
    body = body.cut(slot_x(OUT_X - 1.4, OUT_X + 1, SD_Y, SD_Z, 20.0, 9.0))     # finger scoop

    # microphones: a small hole in each side wall
    body = body.cut(cyl_x(-OUT_X - 1, OUT_X + 1, MIC_Y, MIC_Z, 1.6))

    # right edge, near the top: the shutter button, where the index finger rests
    body = body.cut(cyl_x(IN_X - 1, OUT_X + 1, SHUTTER_Y, SHUTTER_Z, SHUTTER_HOLE_D))

    # left edge: rotary encoder
    body = body.cut(cyl_x(-OUT_X - 1, -IN_X + 1, ENCODER_Y, ENCODER_Z, ENCODER_HOLE_D))
    return body


# ---------------------------------------------------------------- the plate over the battery

def make_plate():
    z0, z1 = PLATE_BACK, PLATE_BACK + PLATE_T
    c = 0.3
    plate = box(BAY_X0 - RIB, IN_X - c, BAY_Y0 - RIB, BAY_Y1 + RIB, z0, z1)
    for x, y in PLATE_BOSSES:                                    # ears that reach the bosses
        if y < BAY_Y0:
            ear = box(x - 4.5, x + 4.5, max(y - 4.5, -IN_Y + c), BAY_Y0, z0, z1)
        else:
            ear = box(x - 4.5, min(x + 4.5, IN_X - c), BAY_Y1, y + 4.5, z0, z1)
        plate = plate.union(ear).cut(cyl_z(x, y, z0 - 1, z1 + 1, 2.4))
    # room for the speaker plug and its wires
    plate = plate.cut(box(SPK_CONN[0] - 3, IN_X + 1, SPK_CONN[2] - 3, SPK_CONN[3] + 3, z0 - 1, z1 + 1))
    # the bottom right post stands at the corner of the bay
    x, y = STANDOFFS[1]
    plate = plate.cut(cyl_z(x, y, z0 - 1, z1 + 1, POST_D + 0.8))
    return plate


# ---------------------------------------------------------------- the lens hood

def make_hood():
    """Modelled at the lens position LENS_Y; the opening widens towards the outside."""
    out_h = hood_half(HOOD_OUT_Z)
    shell = frustum(HOOD_SHOULDER_Z, (HOOD_SOCKET / 2 + 1.2, HOOD_SOCKET / 2 + 1.2),
                    HOOD_OUT_Z, (out_h[0] + HOOD_WALL, out_h[1] + HOOD_WALL))
    collar = box(-HOOD_SOCKET / 2 - 1.2, HOOD_SOCKET / 2 + 1.2, -HOOD_SOCKET / 2 - 1.2, HOOD_SOCKET / 2 + 1.2,
                 HOOD_SHOULDER_Z, HOOD_FRONT_Z)
    fy = out_h[1] + HOOD_WALL + LENS_TRAVEL + 3.0
    flange = rounded(-15.5, 15.5, -fy, fy, HOOD_OUT_Z, BACK_OUT, 3.0)
    hood = shell.union(collar).union(flange)
    hood = hood.cut(frustum(HOOD_SHOULDER_Z - 0.01, (HOOD_APERTURE, HOOD_APERTURE), HOOD_OUT_Z - 0.01, out_h))
    hood = hood.cut(box(-HOOD_SOCKET / 2, HOOD_SOCKET / 2, -HOOD_SOCKET / 2, HOOD_SOCKET / 2,
                        HOOD_SHOULDER_Z, HOOD_FRONT_Z + 1))
    for dx, dy in HOOD_SCREWS:                                   # slots, so the hood can slide
        hood = hood.cut(cq.Workplane("XY").slot2D(2 * LENS_TRAVEL + 2.4, 2.4, 90)
                        .extrude(HOOD_T + 2).translate((dx, dy, HOOD_OUT_Z - 1)))
    return hood.translate((0, LENS_Y, 0))


# ---------------------------------------------------------------- button plungers

def make_plunger():
    """Sits in the right wall between a side button and the outside. Modelled at BOOT."""
    tip = KEY_TIP_X + 0.15
    flange = cyl_x(tip, IN_X - 0.45, 0, 0, 5.5)
    shaft = cyl_x(tip, OUT_X + 1.4, 0, 0, 3.2)
    return flange.union(shaft).translate((0, KEY_BOOT_Y, KEY_Z))


# ---------------------------------------------------------------- a quick first print

def make_fit_test(body):
    """The front 15 mm of the body with a thin floor: about half the plastic. It
    checks the fit of the glass, the standoff posts and every opening at board level before
    the full body is printed. Do not screw the board into it: there is no seat for the
    screw heads, so a screw could reach through the standoff to the display."""
    z_cut = STANDOFF_Z - 3.0
    top = body.intersect(box(-100, 100, -100, 100, z_cut, 100))
    floor = rounded(-OUT_X, OUT_X, BOT_OUT, TOP_OUT, z_cut, z_cut + 1.2, GLASS_R + FIT + WALL)
    return top.union(floor)


# ---------------------------------------------------------------- stand-ins for what goes inside

def board_proxies():
    """Boxes for the display unit, used for the picture and the collision check."""
    return {
        "glass": rounded(-GLASS_W / 2, GLASS_W / 2, -GLASS_H / 2, GLASS_H / 2, 1.4, GLASS_FRONT, GLASS_R),
        "lcd": box(-32.35, 32.35, -62.0, 56.7, -3.0, 1.4),
        "pcb": box(-32.25, 32.26, -61.9, 56.61, PCB_BACK, -3.0),
        "header": box(HEADER_X[0], HEADER_X[1], -7.35, 43.95, -9.8, PCB_BACK),
        "usb_a": box(4.17, 13.76, -63.22, -55.51, -7.85, PCB_BACK),
        "usb_b": box(-13.78, -4.2, -63.22, -55.51, -7.85, PCB_BACK),
        "sd": box(17.27, 32.27, -10.77, 5.33, -6.45, PCB_BACK),
        "keys": box(29.09, KEY_TIP_X, 8.59, 36.51, -6.4, -4.2),
        "c6": box(8.97, 22.17, 40.02, 56.62, -7.07, PCB_BACK),
        "core": box(-8.75, 8.65, -10.1, 11.3, -7.12, PCB_BACK),
        "rtc": box(-18.88, -15.48, 50.22, 55.22, -8.0, PCB_BACK),
        "spk_conn": box(SPK_CONN[0], SPK_CONN[1], SPK_CONN[2], SPK_CONN[3], -8.9, PCB_BACK),
        "bat_conn": box(-31.23, -26.03, -31.05, -23.34, -7.95, PCB_BACK),
        "csi": box(-5.54, 5.57, 27.6, 33.4, -6.62, PCB_BACK),
        "standoffs": cyl_z(STANDOFFS[0][0], STANDOFFS[0][1], STANDOFF_Z, PCB_BACK, 5.5)
        .union(cyl_z(STANDOFFS[1][0], STANDOFFS[1][1], STANDOFF_Z, PCB_BACK, 5.5))
        .union(cyl_z(STANDOFFS[2][0], STANDOFFS[2][1], STANDOFF_Z, PCB_BACK, 5.5))
        .union(cyl_z(STANDOFFS[3][0], STANDOFFS[3][1], STANDOFF_Z, PCB_BACK, 5.5)),
    }


def fitted_proxies():
    """What you add: battery, speaker, push-button, encoder, camera."""
    return {
        "battery": box(BAY_X0 + 0.9, BAY_X0 + 0.9 + 55.0, BAY_Y0 + 0.5, BAY_Y0 + 0.5 + min(BATTERY[1] - 1, 87.0),
                       BACK_IN, BACK_IN + BATTERY[2]),
        "speaker": box(SPK_X0 + 0.6, SPK_X0 + 0.6 + SPEAKER[0], TOP_IN - SPEAKER[2], TOP_IN,
                       BACK_IN + 0.3, BACK_IN + 0.3 + SPEAKER[1]),
        "shutter": cyl_x(IN_X - SHUTTER_DEPTH, IN_X, SHUTTER_Y, SHUTTER_Z, 12.0)
        .union(cyl_x(IN_X - 3.0, IN_X, SHUTTER_Y, SHUTTER_Z, SHUTTER_NUT_D - 0.6)),
        "encoder": box(-IN_X, -IN_X + 10.5, ENCODER_Y - 6, ENCODER_Y + 6, ENCODER_Z - 6, ENCODER_Z + 6),
        "camera": box(-LENS_BLOCK / 2, LENS_BLOCK / 2, LENS_Y - LENS_BLOCK / 2, LENS_Y + LENS_BLOCK / 2,
                      LENS_TOP_Z, PCB_BACK).union(box(-4.5, 4.5, 33.4, LENS_Y, -6.3, PCB_BACK)),
    }


# ---------------------------------------------------------------- build, check, export

def volume(shape):
    try:
        return sum(s.Volume() for s in shape.solids().vals())
    except Exception:
        return 0.0


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    os.makedirs(os.path.join(here, "stl"), exist_ok=True)
    os.makedirs(os.path.join(here, "step"), exist_ok=True)

    parts = {"body": make_body(), "battery_plate": make_plate(), "lens_hood": make_hood(), "button_plunger": make_plunger()}
    board, fitted = board_proxies(), fitted_proxies()

    print(f"case: {2 * OUT_X:.1f} x {TOP_OUT - BOT_OUT:.1f} x {FRONT - BACK_OUT:.1f} mm "
          f"(+ {HOOD_T:.1f} mm lens hood); battery bay {BATTERY[0]:.1f} x {BATTERY[1]:.1f} x {BAY_DEPTH:.1f} mm")
    print(f"levels: front {FRONT:.1f}, plate {PLATE_BACK + PLATE_T:.1f}..{PLATE_BACK:.1f}, back inside {BACK_IN:.1f}, outside {BACK_OUT:.1f}")

    # collisions: every printed part against every stand-in, and the printed parts with each other
    problems = 0
    things = {**board, **fitted}
    for pn, p in parts.items():
        for tn, t in things.items():
            v = volume(p.intersect(t))
            if v > 0.05:
                problems += 1
                bb = p.intersect(t).val().BoundingBox()
                print(f"  COLLISION {pn} / {tn}: {v:.2f} mm3 at x {bb.xmin:.1f}..{bb.xmax:.1f} y {bb.ymin:.1f}..{bb.ymax:.1f} z {bb.zmin:.1f}..{bb.zmax:.1f}")
    names = list(parts)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            v = volume(parts[names[i]].intersect(parts[names[j]]))
            if v > 0.05:
                problems += 1
                bb = parts[names[i]].intersect(parts[names[j]]).val().BoundingBox()
                print(f"  COLLISION {names[i]} / {names[j]}: {v:.2f} mm3 at x {bb.xmin:.1f}..{bb.xmax:.1f} y {bb.ymin:.1f}..{bb.ymax:.1f} z {bb.zmin:.1f}..{bb.zmax:.1f}")
    print("collision check:", "clean" if problems == 0 else f"{problems} problem(s)")

    # openings: a thin probe pushed through each one must not touch the body
    probes = {
        "USB-C left": cyl_y(BOT_OUT - 2, -IN_Y + 2, USB_X[0], USB_Z, 5.0),
        "USB-C right": cyl_y(BOT_OUT - 2, -IN_Y + 2, USB_X[1], USB_Z, 5.0),
        "power LED": cyl_y(BOT_OUT - 2, -IN_Y + 2, LED_X, LED_Z, 1.0),
        "POWER plunger": cyl_x(IN_X - 2, OUT_X + 2, KEY_POWER_Y, KEY_Z, 3.3),
        "BOOT plunger": cyl_x(IN_X - 2, OUT_X + 2, KEY_BOOT_Y, KEY_Z, 3.3),
        "RESET pin hole": cyl_x(IN_X - 2, OUT_X + 2, KEY_RESET_Y, KEY_Z, 1.5),
        "card slot": box(IN_X - 2, OUT_X + 2, SD_Y - 5.6, SD_Y + 5.6, SD_Z - 0.6, SD_Z + 0.6),
        "microphone right": cyl_x(IN_X - 2, OUT_X + 2, MIC_Y, MIC_Z, 1.0),
        "microphone left": cyl_x(-OUT_X - 2, -IN_X + 2, MIC_Y, MIC_Z, 1.0),
        "encoder": cyl_x(-OUT_X - 2, -IN_X + 2, ENCODER_Y, ENCODER_Z, 7.0),
        "shutter button": cyl_x(IN_X - 2, OUT_X + 2, SHUTTER_Y, SHUTTER_Z, 12.0),
        "speaker grille": box(GRILLE_X[0] - 0.4, GRILLE_X[0] + 0.4, TOP_IN - 2, TOP_OUT + 2, SPK_ZC - 3.0, SPK_ZC + 3.0),
        "lens hood": box(-5, 5, LENS_Y - 5, LENS_Y + 5, BACK_OUT - 2, BACK_IN + 2),
        "screw, bottom left": cyl_z(STANDOFFS[0][0], STANDOFFS[0][1], BACK_OUT - 2, STANDOFF_Z + 2, 2.5),
        "screw, top right": cyl_z(STANDOFFS[3][0], STANDOFFS[3][1], BACK_OUT - 2, STANDOFF_Z + 2, 2.5),
    }
    blocked = [n for n, pr in probes.items() if volume(parts["body"].intersect(pr)) > 0.01]
    print("openings check:", "all open" if not blocked else "BLOCKED: " + ", ".join(blocked))

    # each part in the position it is printed in: largest flat face on the bed
    printable = {
        "body": parts["body"].translate((0, 0, -BACK_OUT)),
        "fit_test": make_fit_test(parts["body"]).translate((0, 0, -(STANDOFF_Z - 3.0))),
        "battery_plate": parts["battery_plate"].translate((0, 0, -PLATE_BACK)),
        "lens_hood": parts["lens_hood"].translate((0, -LENS_Y, -HOOD_OUT_Z)),
        "button_plunger": parts["button_plunger"].translate((-(KEY_TIP_X + 0.15), -KEY_BOOT_Y, -KEY_Z))
        .rotate((0, 0, 0), (0, 1, 0), -90),
    }
    for name, shape in printable.items():
        bb = shape.val().BoundingBox()
        path = os.path.join(here, "stl", name + ".stl")
        cq.exporters.export(shape, path, tolerance=0.02, angularTolerance=0.15)
        solids = shape.val().Solids()
        print(f"{name}: {bb.xlen:.1f} x {bb.ylen:.1f} x {bb.zlen:.1f} mm, z from {bb.zmin:.2f}, "
              f"{volume(shape) / 1000:.1f} cm3, {len(solids)} solid(s) -> stl/{name}.stl")

    asm = cq.Assembly()
    colors = {"body": (0.25, 0.27, 0.3), "battery_plate": (0.8, 0.5, 0.1), "lens_hood": (0.85, 0.1, 0.45), "button_plunger": (0.85, 0.1, 0.45)}
    for name, shape in parts.items():
        asm.add(shape, name=name, color=cq.Color(*colors[name]))
    asm.add(parts["button_plunger"].translate((0, KEY_POWER_Y - KEY_BOOT_Y, 0)), name="button_plunger_power",
            color=cq.Color(*colors["button_plunger"]))
    for name, shape in {**board, **fitted}.items():
        asm.add(shape, name="ref_" + name, color=cq.Color(0.2, 0.5, 0.8, 0.5))
    asm.save(os.path.join(here, "step", "enclosure.step"))
    return parts, board, fitted


if __name__ == "__main__":
    main()
