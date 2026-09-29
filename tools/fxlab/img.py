"""Convert images to/from the binary PPM that fxlab reads and writes (needs Pillow).

    python img.py photo.jpg photo.ppm       # any image -> PPM (resized to 720x1280 if --fit)
    python img.py out.ppm out.png           # PPM -> PNG
    python img.py photo.jpg photo.ppm --fit # center-crop/resize to the camera frame size
"""
import sys
from PIL import Image, ImageOps

if len(sys.argv) < 3:
    print(__doc__)
    sys.exit(2)

src, dst = sys.argv[1], sys.argv[2]
fit = "--fit" in sys.argv

im = Image.open(src).convert("RGB")
if fit:
    im = ImageOps.fit(im, (720, 1280), method=Image.LANCZOS)
if dst.lower().endswith(".ppm"):
    im.save(dst, format="PPM")
else:
    im.save(dst)
print(f"{src} -> {dst} ({im.width}x{im.height})")
