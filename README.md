# Kiln

A USB flasher for Linux, Windows and macOS. Kiln writes a bootable image onto a
USB stick or SD card and reads it back to check every byte. It also builds
Windows install sticks from a Windows ISO, with optional Rufus-style setup
tweaks (no TPM, Secure Boot or RAM check, local account, no Microsoft account),
and it makes Ventoy disks, which boot any ISO file you copy onto them.

Licensed under the GNU GPL v3 or later (see `LICENSE`).

> **Early software.** Kiln erases whole drives. It has been tested carefully
> (see [What has been tested](#what-has-been-tested)), but not yet on many
> real machines and sticks. Check the drive name twice before you write, and
> keep backups of anything you care about.

## Features

- **Raw write with verification.** Linux ISOs (hybrid ISOs) and disk images are
  written byte for byte, then read back and compared. An optional SHA-256 is
  checked before anything is written.
- **Windows install sticks.** A Windows ISO cannot be copied raw, so Kiln builds
  the stick from its files: GPT, FAT32, UEFI boot with Secure Boot left on.
  `install.wim` files over 4 GiB are split into `.swm` parts. Every file is read
  back and compared with the ISO.
- **Windows setup tweaks**, as in Rufus: skip the TPM 2.0, Secure Boot and 4 GB
  RAM checks, allow an offline account, create a local account, answer the
  privacy questions "no", turn off automatic BitLocker, and turn off OneDrive,
  Copilot, Teams, Outlook and ads.
- **Ventoy disks.** Makes a [Ventoy](https://www.ventoy.net) 1.1.18 disk (MBR or
  GPT, with or without Secure Boot support). Copy ISO files onto it afterwards
  and Ventoy's menu boots them.
- **Safe drive list.** Only USB drives, SD cards and removable drives are ever
  offered. See [What Kiln will never write to](#what-kiln-will-never-write-to).
- **GUI and command line** on all three systems. The GUI never runs as an
  administrator itself; only a small helper does, for the write.

## Platforms

| | Linux | Windows | macOS |
|---|---|---|---|
| Command line | yes | yes (elevated terminal) | yes |
| GUI | yes (Qt 6) | yes (Qt 6) | yes (Qt 6) |
| Admin prompt | polkit (`pkexec`) | UAC | macOS's own dialog (`authopen`) |
| Raw write + verify | yes | yes | yes |
| Windows install sticks | yes | yes | yes |
| Ventoy disks | yes | yes | yes |
| Ready-made packages | Arch, Debian/Ubuntu, Fedora | not yet | not yet |

## Installing on Linux

Packages for x86_64 are attached to each
[release](https://github.com/TryCoffee/kiln/releases). Each one installs
`kiln`, `kiln-gui`, the helper and its polkit rule, and pulls in Qt 6 and
pkexec through your package manager.

| Distribution | Package | Install |
|---|---|---|
| Arch Linux, CachyOS | `kiln-VERSION-1-x86_64.pkg.tar.zst` | `sudo pacman -U kiln-*.pkg.tar.zst` |
| Debian 13, Ubuntu 26.04 | `kiln_VERSION_amd64.deb` | `sudo apt install ./kiln_*_amd64.deb` |
| Fedora 44 | `kiln-VERSION-1.x86_64.rpm` | `sudo dnf install ./kiln-*.x86_64.rpm` |

`SHA256SUMS` in the release lists every file's checksum. The Arch package is
built from `packaging/arch/PKGBUILD`, which also works with `makepkg` on its own.
Other platforms: build from source (below).

## What Kiln will never write to

- **Linux:** only USB-attached drives, SD cards and drives the kernel flags as
  removable are offered. A disk holding `/`, `/boot`, `/home`, `/usr`, `/var`
  (and a few more) or swap is never offered, followed through dm-crypt, LVM and
  md down to the physical disk. Internal disks are never offered, even when
  nothing on them is mounted.
- **Windows:** only USB drives, SD cards and removable media. Never virtual
  disks (VHD, Storage Spaces), internal disks, or any disk behind the Windows
  folder, the boot partition or a page file.
- **macOS:** only external USB drives and SD cards. Never internal disks, disk
  images, or the disk behind `/` or `/System/Volumes/*` (followed from an APFS
  container to its physical disk).
- **Everywhere:** never the drive the image itself is stored on.

`kiln list --all` shows every disk and why it is or is not offered.

## Using the GUI

    kiln-gui [image]

Pick a drive, pick an image (or choose "Make a Ventoy disk"), tick the setup
tweaks if it is a Windows ISO, and press Write. Your system asks for an
administrator password once per write.

## Using the command line

    kiln list                     # drives Kiln will write to
    kiln list --all               # every disk, and why the others are left out
    kiln info image.iso [--sha256]
    sudo kiln write image.iso sdc [--sha256 HEX] [--unmount] [--no-verify]
    kiln verify image.iso sdc

`kiln write` asks you to type the drive's name before it erases anything
(`--yes` skips that, for scripts). On Windows the drive is named like
`PhysicalDrive2` and the command needs a terminal opened with "Run as
administrator". On macOS it is `disk4`, and macOS asks for approval itself.

### Windows install sticks

`kiln write` recognises a Windows ISO and builds the stick instead of copying
it. `kiln info win.iso` lists the editions and the smallest stick that fits.
For Windows 11 25H2 that is 7.9 GiB, so most "8 GB" sticks (7.45 GiB) are too
small. Splitting `install.wim` needs about that much free space in a temporary
folder (`--temp-dir DIR` to choose it).

UEFI boot only. Legacy BIOS boot is not supported for Windows sticks.

### Windows setup tweaks

    sudo kiln write win11.iso sdc --bypass-checks --no-ms-account --local-account alex

| option | what it does |
|---|---|
| `--bypass-checks` | no TPM 2.0, Secure Boot or 4 GB RAM requirement |
| `--no-ms-account` | Setup offers an offline account |
| `--local-account NAME` | creates local administrator NAME with an empty password, to be changed at first sign-in |
| `--no-data-collection` | privacy questions answered "no", licence page skipped |
| `--no-bitlocker` | no automatic device encryption |
| `--declutter` | OneDrive, Outlook, Teams, Copilot, news and ads off; classic context menu |

The bypass is for clean installs started from the stick. Upgrading a running
Windows 11 in place with it is not supported.

### Ventoy disks

    sudo kiln ventoy sdc [--gpt] [--no-secure-boot] [--label NAME] [--reserve MIB]

Kiln lays the disk out the way Ventoy's own installer does. The partition
tables are written last, so a write that fails or is cancelled leaves a drive
with no partitions, not a half-made Ventoy disk. Not done yet: updating Ventoy
on an existing disk while keeping the ISOs, and copying ISO files on from Kiln.

### Images that will not boot when written raw

A raw write only boots when the image carries its own partition table (most
Linux ISOs and disk images do). Anything else is refused; `--force` writes it
anyway.

### Trying it without a stick

`--to-file` writes into an image file instead of a drive, which you can then
boot in QEMU:

    truncate -s 8G stick.img
    kiln write archlinux.iso --to-file stick.img
    qemu-system-x86_64 -enable-kvm -m 2G -device usb-ehci,id=ehci \
        -drive file=stick.img,format=raw,if=none,id=usb \
        -device usb-storage,bus=ehci.0,drive=usb,bootindex=0

A Windows stick needs a bigger file (`truncate -s 16G`) and UEFI (OVMF) with
`-machine q35 -cpu host -m 4G`. Use the EHCI controller as above: with QEMU's
xHCI the Windows boot manager sometimes stalls or fails with 0xc0000428, for
any stick.

## How the admin rights work

Only the write needs administrator rights, so only that part gets them.

- **Linux:** the GUI starts `kiln-helper` through `pkexec`. The helper checks
  the drive list again itself and writes only to a drive that is offered at
  that moment. It opens the image as the user who asked, so it cannot be used
  to copy a file only root may read onto a stick.
- **Windows:** the GUI starts `kiln-helper.exe` through UAC and talks to it over
  a named pipe only that process may join. The helper works with the user's
  own rights and steps up only to unmount and open the drive.
- **macOS:** nothing runs as root. Once the drive is chosen, macOS's own
  `authopen` dialog asks for approval and hands Kiln access to that one drive.

## What has been tested

Tested:

- **Linux:** an Arch Linux ISO written to a real USB stick, the stick's
  checksum equal to the ISO's. Windows 11 sticks and Ventoy disks built on
  virtual sticks and booted in QEMU; a full Windows 11 install from a tweaked
  stick on a virtual machine with no TPM, 3 GB of RAM and Secure Boot off,
  ending signed in to the local account.
- **Windows:** Windows 11 sticks, raw writes and Ventoy disks made on Windows
  onto a virtual USB stick, checked from Linux and booted in QEMU.
- **macOS (Apple silicon):** raw writes on a file-backed disk; Windows 11
  sticks and Ventoy disks made into image files, checked from Linux and booted
  in QEMU.

Not tested yet:

- a physical stick on Windows or macOS;
- the GUI's admin prompt on Windows (UAC) and macOS (`authopen`);
- a Ventoy disk on a machine with Secure Boot enforced;
- Intel Macs.

Bug reports are welcome in the issue tracker.

## Building

Needs CMake 3.25+, a C/C++20 compiler whose C side knows C23's `#embed`
(GCC 15+, Clang 19+; on Debian 13 use `-DCMAKE_C_COMPILER=clang-19`), `make`
and `patch`, and Qt 6.4+ Widgets for the GUI (`-DKILN_GUI=OFF` builds without
it). CMake builds libcdio 2.4.0, wimlib 1.14.5 and hivex 1.3.24 from source
and takes three files from Ventoy 1.1.18's release, all pinned by SHA-256
(`cmake/third-party.cmake`). The first build downloads them; to build offline,
fetch them first and point the build at them:

    cmake -DKILN_DOWNLOAD_DIR=deps -P cmake/fetch-third-party.cmake
    cmake -S . -B build -G Ninja -DKILN_DOWNLOAD_DIR=deps

    cmake -S . -B build -G Ninja
    cmake --build build
    (cd build && ctest --output-on-failure)
    sudo cmake --install build    # Linux: also installs the polkit policy

The Linux packages are built the same way, offline, in a container per
distribution: `packaging/linux/build.sh arch|debian|fedora` (needs Docker).

The tests use sfdisk, fsck.fat, mtools, fsck.exfat and xz as independent
checkers; a missing tool marks a test skipped, not passed.

- **Windows:** MSYS2's UCRT64 toolchain (GCC, CMake, Ninja, `patch`, Qt 6).
  libcdio, wimlib and hivex are built from source. Windows 10 1903 or newer.
- **macOS:** Xcode command line tools, CMake, Ninja, Qt 6
  (`-DCMAKE_PREFIX_PATH=<qt>`); libcdio is built from source.

## Third-party code

- FatFs R0.16 by ChaN, vendored in `third_party/fatfs/` (1-clause BSD-style).
- XZ Embedded by Lasse Collin, vendored in `third_party/xz-embedded/` (0BSD).
- Ventoy 1.1.18 by longpanda (GPL-3.0+): its `boot/boot.img`, `boot/core.img.xz`
  and `ventoy/ventoy.disk.img.xz`, unmodified from the official release, built
  into `kiln` and `kiln-helper`. The partition image also carries GRUB, shim,
  iPXE, wimboot and other parts under their own licences, listed in Ventoy's
  `License/` directory. Source: https://github.com/ventoy/Ventoy/tree/v1.1.18.
- wimlib 1.14.5 (GPL-3.0+ / LGPL-2.1+), built from source with a patch
  (`third_party/wimlib/`).
- hivex 1.3.24 (LGPL-2.1+), the library only, built from source.
- libcdio 2.4.0 (GPL-3.0+), built from source.
- Every package carries these components' licence texts and
  `THIRD-PARTY-NOTICES.md`, which also says where their source is.
- Rufus (GPL-3.0+) is the reference for the Windows stick layout and setup
  tweaks; code ported from it (`src/unattend.cpp`) carries its attribution.
