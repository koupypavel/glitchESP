"""Extract a JPEG that the firmware printed as base64 (no SD card + serial dump enabled).

    python decode_jpeg_dump.py serial.log out.jpg

    python decode_jpeg_dump.py serial.log out.jpg 2     (third dump in the log)

Looks for the block between 'JPEG_B64_BEGIN <size>' and 'JPEG_B64_END'.
"""
import base64
import re
import sys

if len(sys.argv) < 3:
    print(__doc__)
    sys.exit(2)

text = open(sys.argv[1], "r", errors="replace").read()
ms = list(re.finditer(r"JPEG_B64_BEGIN (\d+)\s*(.*?)JPEG_B64_END", text, re.S))
m = ms[int(sys.argv[3]) if len(sys.argv) > 3 else 0] if ms else None
if not m:
    print("no JPEG dump found")
    sys.exit(1)
expected = int(m.group(1))
lines = m.group(2).splitlines()
if any(l.startswith("J:") for l in lines):
    # current firmware: payload lines carry a "J:" prefix, anything else is log output
    b64 = "".join(l[2:].strip() for l in lines if l.startswith("J:"))
else:
    b64 = "".join(ch for ch in m.group(2) if ch.isalnum() or ch in "+/=")
data = base64.b64decode(b64)
open(sys.argv[2], "wb").write(data)
ok = data[:2] == b"\xff\xd8" and data[-2:] == b"\xff\xd9"
print(f"wrote {sys.argv[2]}: {len(data)} bytes (device said {expected}), SOI/EOI markers {'ok' if ok else 'MISSING'}")
