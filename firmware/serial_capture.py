"""Reset the board, capture its serial log for a while, optionally send remote commands.

    python serial_capture.py PORT [seconds] [--out FILE] [T:command ...]

    python serial_capture.py COM10 10
    python serial_capture.py COM10 30 --out run.log 5:photo 8:video 12:video 15:ls

Each "T:command" is sent T seconds after the reset (see main/system/remote.c for the
commands). Run with the ESP-IDF python env (esptool + pyserial). Uses esptool's own
connect + hard_reset sequence, because a bare pyserial RTS pulse does not reset this
board reliably through the CH343 bridge.
"""
import sys, time
import esptool

args = sys.argv[1:]
out_path = None
if "--out" in args:
    i = args.index("--out")
    out_path = args[i + 1]
    del args[i:i + 2]
port_name = args[0]
secs = float(args[1]) if len(args) > 1 else 10.0
sends = []
for a in args[2:]:
    t, cmd = a.split(":", 1)
    sends.append((float(t), cmd))
sends.sort()

esp = esptool.get_default_connected_device([port_name], port=port_name,
                                           connect_attempts=8, initial_baud=115200,
                                           chip="esp32p4")
port = esp._port
esp.hard_reset()
port.timeout = 0.05
t0 = time.time()
buf = b""
while time.time() - t0 < secs:
    buf += port.read(4096)
    while sends and time.time() - t0 >= sends[0][0]:
        port.write((sends.pop(0)[1] + "\n").encode())
text = buf.decode("utf-8", errors="replace")
if out_path:
    open(out_path, "w", encoding="utf-8", newline="\n").write(text)
sys.stdout.buffer.write(text.encode("ascii", errors="replace"))
