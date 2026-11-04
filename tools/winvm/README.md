# Windows-setup test VM

How phase 3 was proven (2026-10-18): a VM Windows 11 refuses (no TPM, 3 GB RAM,
Secure Boot off), a stick built by Kiln, Setup driven by keystrokes.

    truncate -s 16G /var/tmp/stick.img
    kiln write win11.iso --to-file /var/tmp/stick.img --bypass-checks --no-ms-account --local-account kiln
    python3 vm.py start /var/tmp/stick.img      # q35, OVMF (no Secure Boot), no TPM, 3 GB,
                                                # stick on USB 2 (EHCI), 64 GB qcow2 disk, no network
    python3 vm.py shot 1                        # screenshot -> $KILN_VM_DIR/vm-1.png
    python3 vm.py key ret | alt-d | tab ...     # QEMU sendkey names
    python3 vm.py type 'reg query HKLM\...'     # ~0.5 s per character
    python3 watch.py                            # during "Installing Windows 11": stops the VM at
                                                # its first reboot (else the stick boots again)
    KEEPVARS=1 python3 vm.py start none         # continue from the disk
    python3 vm.py stop

Use EHCI: on QEMU's xHCI, Windows' boot manager intermittently stalls or stops
with 0xc0000428 for any stick (Kiln's and a mkfs.fat + mtools one alike).

Polish 25H2 Setup, keys that worked: `ret` (language), `ret` (keyboard),
`alt-w` + `spc` (tick "I agree everything will be deleted") + `alt-d`, `tab` +
`ret` ("I don't have a product key"), arrows + `ret` (edition; a keystroke can
get lost: check the summary page), `alt-z` (accept licence), `alt-d` (disk),
`ret` (install). Install ~21 min to the first reboot on the test machine.
