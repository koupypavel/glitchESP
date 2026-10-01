"""Pictures of the enclosure for the README and for checking it by eye.

    python render.py        -> img/*.png

Renders off screen with VTK (installed together with CadQuery).
"""
import os

import vtk

import enclosure as enc

HERE = os.path.dirname(os.path.abspath(__file__))


def actor(shape, color, alpha=1.0, tol=0.05):
    verts, tris = shape.val().tessellate(tol, 0.2)
    pts = vtk.vtkPoints()
    for p in verts:
        pts.InsertNextPoint(p.x, p.y, p.z)
    cells = vtk.vtkCellArray()
    for a, b, c in tris:
        cells.InsertNextCell(3, (a, b, c))
    poly = vtk.vtkPolyData()
    poly.SetPoints(pts)
    poly.SetPolys(cells)
    normals = vtk.vtkPolyDataNormals()
    normals.SetInputData(poly)
    normals.SetFeatureAngle(35)
    normals.SplittingOn()
    mapper = vtk.vtkPolyDataMapper()
    mapper.SetInputConnection(normals.GetOutputPort())
    a = vtk.vtkActor()
    a.SetMapper(mapper)
    a.GetProperty().SetColor(*color)
    a.GetProperty().SetOpacity(alpha)
    a.GetProperty().SetAmbient(0.25)
    a.GetProperty().SetDiffuse(0.75)
    a.GetProperty().SetSpecular(0.1)
    if alpha < 1:
        return [a]
    # outline every sharp edge, so that holes and steps show in a flat-lit picture
    edges = vtk.vtkFeatureEdges()
    edges.SetInputData(poly)
    edges.BoundaryEdgesOn()
    edges.FeatureEdgesOn()
    edges.SetFeatureAngle(30)
    edges.ManifoldEdgesOff()
    edges.NonManifoldEdgesOff()
    edges.ColoringOff()
    emap = vtk.vtkPolyDataMapper()
    emap.SetInputConnection(edges.GetOutputPort())
    emap.SetResolveCoincidentTopologyToPolygonOffset()
    e = vtk.vtkActor()
    e.SetMapper(emap)
    e.GetProperty().SetColor(0.05, 0.05, 0.07)
    e.GetProperty().SetLineWidth(1.6)
    return [a, e]


def view(name, items, direction, up=(0, 1, 0), size=(1000, 1100), zoom=1.0, clip=None):
    """direction: from the camera towards the model."""
    ren = vtk.vtkRenderer()
    ren.SetBackground(1, 1, 1)
    for shape, color, alpha in items:
        for a in actor(shape if clip is None else shape.intersect(clip), color, alpha):
            ren.AddActor(a)
    win = vtk.vtkRenderWindow()
    win.SetOffScreenRendering(1)
    win.AddRenderer(ren)
    win.SetSize(*size)
    cam = ren.GetActiveCamera()
    cam.ParallelProjectionOn()
    cam.SetFocalPoint(0, 3, -10)
    cam.SetPosition(-400 * direction[0], 3 - 400 * direction[1], -10 - 400 * direction[2])
    cam.SetViewUp(*up)
    ren.ResetCamera()
    cam.Zoom(zoom)
    win.Render()
    grab = vtk.vtkWindowToImageFilter()
    grab.SetInput(win)
    grab.Update()
    os.makedirs(os.path.join(HERE, "img"), exist_ok=True)
    writer = vtk.vtkPNGWriter()
    writer.SetFileName(os.path.join(HERE, "img", name + ".png"))
    writer.SetInputConnection(grab.GetOutputPort())
    writer.Write()


def main():
    parts = {"body": enc.make_body(), "plate": enc.make_plate(), "hood": enc.make_hood(), "plunger": enc.make_plunger(),
             "cap": enc.make_cap()}
    board, fitted = enc.board_proxies(), enc.fitted_proxies()
    grey, orange, pink = (0.45, 0.48, 0.53), (0.95, 0.6, 0.15), (0.9, 0.15, 0.5)
    glass, green, blue = (0.08, 0.1, 0.14), (0.1, 0.5, 0.28), (0.25, 0.5, 0.9)

    plunger2 = parts["plunger"].translate((0, enc.KEY_POWER_Y - enc.KEY_BOOT_Y, 0))
    case = [(parts["body"], grey, 1), (parts["hood"], pink, 1), (parts["plunger"], pink, 1), (plunger2, pink, 1),
            (parts["cap"], pink, 1)]
    unit = [(board["glass"], glass, 1)]

    view("front", case + unit, (0.35, 0.3, -0.9), zoom=1.25)
    view("back", case, (0.4, 0.3, 0.87), zoom=1.25)
    view("top_edge", case + unit, (0, -1, 0), up=(0, 0, 1), size=(1000, 500), zoom=1.6)
    view("bottom_edge", case + unit, (0, 1, 0), up=(0, 0, 1), size=(1000, 500), zoom=1.6)
    view("right_edge", case + unit, (-1, 0, 0), up=(0, 0, 1), size=(1400, 450), zoom=2.4)
    view("left_edge", case + unit, (1, 0, 0), up=(0, 0, 1), size=(1400, 450), zoom=2.4)

    # looking into the body from the front: empty, with what goes behind the board, with the board
    look = (0.12, 0.18, -0.98)
    view("inside_empty", [(parts["body"], grey, 1)], look, zoom=1.3)
    inside = [(parts["body"], grey, 1), (fitted["battery"], blue, 1), (fitted["speaker"], blue, 1),
              (fitted["shutter"], blue, 1), (fitted["encoder"], blue, 1), (parts["hood"], pink, 1),
              (parts["cap"], pink, 1), (fitted["tripod_nut"], blue, 1)]
    view("inside_fitted", inside, look, zoom=1.3)
    view("inside_plate", inside + [(parts["plate"], orange, 1)], look, zoom=1.3)
    back_parts = [(board[k], green, 1) for k in ("pcb", "header", "usb_a", "usb_b", "sd", "keys", "c6", "core", "rtc",
                                                  "spk_conn", "bat_conn", "csi", "standoffs")]
    everything = inside + [(parts["plate"], orange, 1)] + back_parts + [(fitted["camera"], blue, 1)]
    # the board's back side, seen through the back of the case (body left out)
    view("board_back", everything[1:], (0.0, 0.0, 1.0), zoom=1.3)

    # cuts: through the lens (x = 0) seen from the right, and across the battery (y = -10) seen from below
    cut = everything + [(board["lcd"], (0.3, 0.3, 0.35), 1), (board["glass"], glass, 1)]
    view("section_lens", cut, (-1, 0, 0), up=(0, 0, 1), size=(1500, 500), zoom=2.5, clip=enc.box(-100, 0.0, -100, 100, -100, 100))
    view("section_battery", cut, (0, 1, 0), up=(0, 0, 1), size=(1100, 500), zoom=1.7, clip=enc.box(-100, 100, -10.0, 100, -100, 100))
    view("section_shutter", cut, (0, 1, 0), up=(0, 0, 1), size=(1100, 500), zoom=1.7,
         clip=enc.box(-100, 100, enc.SHUTTER_Y, 100, -100, 100))


if __name__ == "__main__":
    main()
