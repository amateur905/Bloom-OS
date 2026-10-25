# Bloom-OS

![Bloom-OS logo](assets/icon.png)

A hobby operating system built entirely from scratch: custom UEFI bootloader, custom kernel, no existing bootloaders or kernels used as a base. Developed as an open, educational OSDev-style project.

## About

The goal is a real, working operating system built from the ground up:

- A custom UEFI bootloader (no Limine, no GRUB), written with gnu-efi
- A custom 64-bit kernel in C and x86_64 assembly
- Memory management, interrupts, drivers, file systems, and eventually a graphical interface
- Long-term goals: a full GUI and networking (wired and Wi-Fi)

## Status

Bloom-OS boots on real hardware (a Lenovo ThinkPad T580) and in QEMU, and runs a console shell. The working parts today:

- UEFI bootloader that loads `KERNEL.BIN`, passes the framebuffer, memory map and ACPI pointer, and shows a boot logo
- Framebuffer text console (8x16 bitmap font), PS/2 keyboard and mouse, PIT timer with interrupts
- Physical memory manager and ACPI power control (`reboot` and `shutdown` work on real hardware)
- Storage: AHCI (SATA), NVMe, and USB mass storage (xHCI)
- A block device layer with any sector size (512, 1024, 2048, 4096) and many devices at once
- Partition tables: GPT and MBR (including extended partitions)
- File system: FAT32, read-only, with long file names and nested folders
- Intel HD Audio driver and a streaming WAV player
- A full-screen file explorer with a text and hex viewer

Not done yet: writing to disks, file systems other than FAT32, USB hubs, networking, and a graphical desktop.

## Target hardware

- Development and testing in QEMU with OVMF UEFI firmware
- Real-hardware target: a Lenovo ThinkPad T580, booting through the UEFI Boot Menu (F12) from a dedicated microSD card in a USB adapter, alongside an existing Windows 11 installation. A USB flash drive works the same way
- Secure Boot must be turned off

## Building

Requirements: gcc, ld, objcopy, make, nasm, and gnu-efi (with its `crt0-efi-x86_64.o` and `elf_x86_64_efi.lds`).

```
cd bootloader && make    # produces main.efi
cd ../kernel && make     # produces kernel.bin
```

## Installing on a boot drive

Format a drive as FAT32 and lay the files out like this:

```
EFI/BOOT/BOOTX64.EFI     (bootloader/main.efi renamed)
KERNEL.BIN               (kernel/kernel.bin renamed)
```

Boot the machine from that drive. The same layout works as the `esp` folder for QEMU:

```
qemu-system-x86_64 -machine q35 -m 512 \
  -drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS.fd \
  -drive format=raw,file=fat:rw:esp
```

To test USB storage in QEMU, add a virtual controller and a drive:

```
  -device qemu-xhci,id=xhci \
  -drive if=none,id=u0,format=raw,file=disk.img \
  -device usb-storage,bus=xhci.0,drive=u0
```

## Console

Type `help` in the shell for the list of commands.

| Command | What it does |
|---------|--------------|
| `help` | list commands |
| `clear` | clear the screen |
| `echo` | print arguments |
| `mem` | show memory usage |
| `lspci` | list PCI devices |
| `disks` | list block devices |
| `lsblk` | list disks and partitions with file system and mount state |
| `usb` | scan USB ports, register new flash drives and mount their volumes |
| `usb list` | show USB devices: port, speed, vendor and product ID, disk names |
| `ls`, `cd`, `pwd` | browse folders, paths look like `usb0p1:/folder` |
| `cat` | print a text file |
| `hexdump` | hex view of a file: `hexdump file [offset] [length]` |
| `play` | play a WAV file |
| `explorer` | full-screen file explorer |
| `hda` | show the audio controller state |
| `beep` | play a test tone |
| `dmesg` | show the boot log, optionally filtered by prefix |
| `reboot`, `shutdown` | restart or power off |

## USB storage

The `usb` command brings up the xHCI controller, finds devices on the root ports, and registers each flash drive as a block device (`usb0`, `usb1`, ...). Card readers with several slots show up as several devices. Run `usb` again after plugging a drive in or out. Volumes are mounted automatically and appear in `lsblk` and in the explorer.

Current limits: read-only, no USB hubs (plug drives directly into a port), at most 64 block devices per session. `dmesg XHCI` and `dmesg USB` show what the driver did step by step.

## Explorer

The `explorer` shell command opens a full-screen graphical file explorer with a volume sidebar, folder navigation, a text and hex viewer, and keyboard and mouse control.

Keys: arrows, PgUp, PgDn, Home and End select; Enter opens a folder, plays a WAV file or opens any other file in the viewer; Backspace goes up; Tab switches volume; Space stops playback; any letter or digit jumps to the next entry starting with it; Esc leaves the explorer. Ctrl+S cycles sorting (name, size, type), Ctrl+A shows or hides hidden files, Ctrl+R refreshes, Ctrl+D shows file details, Ctrl+P toggles autoplay of the next WAV in the folder. With the mouse, click selects and double-click opens or plays.

Viewer keys: arrows, PgUp, PgDn, Home, End scroll; `/` searches, N finds the next match; H switches between text and hex view; Esc or Backspace goes back. Files larger than 256 KB show their first 256 KB. Binary files open in hex view. Text is ASCII only, other characters are shown as `?`.

Playback continues while you browse and view files, and stops when you leave the explorer.

### Error codes

Errors and warnings are shown in the bottom line (red for errors, yellow for warnings) and are written to the kernel log, so `dmesg explorer` and `dmesg viewer` show what went wrong.

| Code | Meaning |
|------|---------|
| E100 | no readable volumes found |
| E102 | cannot read folder |
| E103 | folder is large, first 1024 entries shown (warning) |
| E104 | name is too long to open |
| E105 | path is too long |
| E110 | cannot open file |
| E111 | read error while loading file |
| E112 | file is large, only the first 256 KB shown (warning) |
| E113 | binary file, opened in hex view (warning) |
| E114 | file is empty (warning) |
| E115 | too many lines, view truncated (warning) |
| E116 | internal viewer range error |
| E120 | sample rate not supported by the HDA driver |
| E121 | unsupported WAV format |
| E122 | HDA audio stream not available |
| E123 | not enough low memory for audio buffers |
| E124 | read error during playback |
| E125 | not a RIFF/WAVE file |
| E130 | internal selection error, auto-corrected |
| E131 | screen is too small, at least 800x480 needed |
| E140 | text not found (warning) |
| E141 | cannot read file details |

## Audio

Playback goes through the Intel HD Audio driver. The `play` shell command streams WAV files straight from disk: PCM 8, 16, 24 and 32-bit, 1 to 8 channels (extra channels are dropped, mono is duplicated), sample rates from 8 kHz to 192 kHz. Any key stops playback.

Known limitation: only WAV is supported. MP3 and other compressed formats are not decoded, so convert tracks to WAV first.

## Known limitations

- File systems are read-only, and only FAT32 can be mounted. FAT12/16, NTFS and ext partitions are recognized and listed by `lsblk` but not opened
- USB hubs are not supported
- Paths are limited to 512 characters
- Text is ASCII only

## Development notes

Development of this project is done with the assistance of AI (Anthropic's Claude), used as a technical collaborator throughout the design and implementation process. All architectural decisions and final code are reviewed and directed by the author.

## License

This project is released under an open license permitting free use, modification, and redistribution. **Commercial sale is not permitted** (free distribution only). See [LICENSE](License) for full terms.

## Author

Savva Poliakov
