"""Reset the board and capture its serial log for a few seconds (non-interactive).
Usage: python serial_capture.py COM10 [seconds]
Run with the ESP-IDF python env (esptool + pyserial). Uses esptool's own connect +
hard_reset sequence, because a bare pyserial RTS pulse does not reset this board
reliably through the CH343 bridge.
"""
import sys, time
import esptool

port_name = sys.argv[1]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0

esp = esptool.get_default_connected_device([port_name], port=port_name,
                                           connect_attempts=8, initial_baud=115200,
                                           chip="esp32p4")
port = esp._port
esp.hard_reset()
port.timeout = 0.2
end = time.time() + secs
buf = b""
while time.time() < end:
    buf += port.read(4096)
sys.stdout.buffer.write(buf.decode("utf-8", errors="replace").encode("ascii", errors="replace"))
