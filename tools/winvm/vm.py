# vm.py — drive a Windows-setup test VM through QEMU's monitor (see README.md).
#   vm.py start <stick.img|none>  vm.py shot <name>      vm.py key <k> [<k> ...]
#   vm.py type <text>             vm.py stop             vm.py status
# Env: KILN_VM_DIR (default /var/tmp/kiln-vm), RAM (MiB, default 3072),
#      KEEPVARS=1 (keep the UEFI variables, e.g. when restarting from the disk).
import os, socket, subprocess, sys, time
VM = os.environ.get("KILN_VM_DIR", "/var/tmp/kiln-vm"); SP = VM
os.makedirs(VM, exist_ok=True)
MON = f"{VM}/mon.sock"; PID = f"{VM}/qemu.pid"
def mon(cmd, wait=0.3):
    s = socket.socket(socket.AF_UNIX); s.connect(MON); s.settimeout(2)
    time.sleep(0.2)
    try: s.recv(65536)
    except Exception: pass
    s.sendall((cmd + "\n").encode()); time.sleep(wait)
    out = b""
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            out += d
    except Exception: pass
    s.close(); return out.decode(errors="replace")
cmd = sys.argv[1]
if cmd == "start":
    stick = sys.argv[2]
    disk = f"{VM}/win.qcow2"
    if not os.path.exists(disk): subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", disk, "64G"], check=True)
    if not os.environ.get("KEEPVARS"):
        subprocess.run(["cp", "/usr/share/edk2/x64/OVMF_VARS.4m.fd", f"{VM}/vars.fd"], check=True)
    usb = [] if stick == "none" else ["-device", "usb-ehci,id=ehci", "-drive", f"file={stick},format=raw,if=none,id=usb,readonly=on",
        "-device", "usb-storage,bus=ehci.0,drive=usb,bootindex=0"]
    for f in (MON,):
        if os.path.exists(f): os.unlink(f)
    subprocess.run(["qemu-system-x86_64", "-enable-kvm", "-machine", "q35", "-cpu", "host", "-smp", "4",
        "-m", os.environ.get("RAM", "3072"), "-display", "none", "-vga", "std",
        "-monitor", f"unix:{MON},server,nowait", "-daemonize", "-pidfile", PID,
        "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd",
        "-drive", f"if=pflash,format=raw,file={VM}/vars.fd",
        "-drive", f"file={disk},format=qcow2,if=none,id=disk", "-device", "ide-hd,drive=disk,bus=ide.0,bootindex=1",
        *usb, "-nic", "none"], check=True)
    print("started, pid", open(PID).read().strip())
elif cmd == "shot":
    ppm = f"{VM}/shot.ppm"; mon(f"screendump {ppm}", 1.0)
    png = f"{SP}/vm-{sys.argv[2]}.png"
    subprocess.run(["magick", ppm, "-resize", "60%", png], check=True); print(png)
elif cmd == "key":
    for k in sys.argv[2:]: mon(f"sendkey {k}", 0.4)
elif cmd == "type":
    names = {" ": "spc", "-": "minus", ".": "dot", "_": "shift-minus", "\\": "backslash", "/": "slash",
             ":": "shift-semicolon", "\"": "shift-apostrophe", "|": "shift-backslash", "&": "shift-7",
             "*": "shift-8", ">": "shift-dot", "=": "equal", ",": "comma", "$": "shift-4", "(": "shift-9", ")": "shift-0"}
    for ch in sys.argv[2]:
        k = names.get(ch, ch.lower() if ch.isalnum() else None)
        if ch.isupper(): k = "shift-" + ch.lower()
        mon(f"sendkey {k}", 0.15)
elif cmd == "status":
    print(mon("info status").strip().splitlines()[-1])
elif cmd == "stop":
    pid = int(open(PID).read()); mon("quit", 1.0)
    try: os.kill(pid, 0); print("still running", pid)
    except OSError: print("stopped")
