"""Extract files that the firmware printed with the serial remote's "get <file>" command.

    python decode_file_dump.py serial.log out_dir

Each file sits between 'FILE_B64_BEGIN <name> <size>' and 'FILE_B64_END', as base64 lines
with an "F:" prefix (other lines in between are log output and are ignored).
"""
import base64
import os
import re
import sys

if len(sys.argv) < 3:
    print(__doc__)
    sys.exit(2)

text = open(sys.argv[1], "r", errors="replace").read()
os.makedirs(sys.argv[2], exist_ok=True)
found = 0
for m in re.finditer(r"FILE_B64_BEGIN (\S+) (-?\d+)\s*(.*?)FILE_B64_END", text, re.S):
    name, expected = m.group(1), int(m.group(2))
    b64 = "".join(l[2:].strip() for l in m.group(3).splitlines() if l.startswith("F:"))
    data = base64.b64decode(b64)
    path = os.path.join(sys.argv[2], os.path.basename(name))
    open(path, "wb").write(data)
    print(f"wrote {path}: {len(data)} bytes (device said {expected}){'' if len(data) == expected else '  SIZE MISMATCH'}")
    found += 1
if not found:
    print("no file dump found")
    sys.exit(1)
