# Waits while the "Installing Windows 11" screen (blue) is up; when it is gone
# for two samples in a row (the reboot), quits QEMU before the stick boots again.
import os, socket, time
VM = os.environ.get("KILN_VM_DIR", "/var/tmp/kiln-vm"); MON = f"{VM}/mon.sock"
def mon(cmd):
    s = socket.socket(socket.AF_UNIX); s.connect(MON); s.settimeout(1)
    time.sleep(0.2)
    try: s.recv(65536)
    except Exception: pass
    s.sendall((cmd + "\n").encode()); time.sleep(1.0); s.close()
def bg():
    mon(f"screendump {VM}/w.ppm")
    d = open(f"{VM}/w.ppm", "rb").read(); parts = d.split(b"\n", 3); w, h = map(int, parts[1].split())
    i = ((h - 10) * w + 10) * 3; return tuple(parts[3][i:i + 3])
t0 = time.time(); gone = 0; last = None
while time.time() - t0 < 3600:
    c = bg(); blue = c[2] > 120 and c[0] < 40
    if c != last: print(f"{time.time()-t0:5.0f}s bg={c}", flush=True); last = c
    gone = 0 if blue else gone + 1
    if gone >= 2:
        mon("quit"); print(f"{time.time()-t0:5.0f}s reboot seen, VM stopped", flush=True); break
    time.sleep(5)
