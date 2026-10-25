TARGET = kernel.bin

all: $(TARGET)

start.o: start.asm
	nasm -f elf64 start.asm -o start.o

irq_stubs.o: irq_stubs.asm
	nasm -f elf64 irq_stubs.asm -o irq_stubs.o

idt.o: idt.c idt.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mgeneral-regs-only -m64 -Wall -c idt.c -o idt.o

keyboard.o: keyboard.c keyboard.h idt.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c keyboard.c -o keyboard.o

mouse.o: mouse.c mouse.h idt.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c mouse.c -o mouse.o

pmm.o: pmm.c pmm.h boot_info.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c pmm.c -o pmm.o

pci.o: pci.c pci.h io.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c pci.c -o pci.o

ahci.o: ahci.c ahci.h pmm.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c ahci.c -o ahci.o

blockdev.o: blockdev.c blockdev.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c blockdev.c -o blockdev.o

nvme.o: nvme.c nvme.h pmm.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c nvme.c -o nvme.o

fat32.o: fat32.c fat32.h blockdev.h pmm.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c fat32.c -o fat32.o

xhci.o: xhci.c xhci.h pci.h pmm.h idt.h klog.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c xhci.c -o xhci.o

usbmsc.o: usbmsc.c usbmsc.h xhci.h blockdev.h idt.h klog.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c usbmsc.c -o usbmsc.o

hda.o: hda.c hda.h pmm.h idt.h klog.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mgeneral-regs-only -m64 -Wall -c hda.c -o hda.o

part.o: part.c part.h blockdev.h fat32.h klog.h pmm.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c part.c -o part.o

fs.o: fs.c fs.h part.h blockdev.h fat32.h klog.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c fs.c -o fs.o

power.o: power.c power.h io.h klog.h console.h idt.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c power.c -o power.o

klog.o: klog.c klog.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c klog.c -o klog.o

wav.o: wav.c wav.h fs.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c wav.c -o wav.o

ui.o: ui.c ui.h gfx.h mouse.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c ui.c -o ui.o

errors.o: errors.c errors.h klog.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c errors.c -o errors.o

viewer.o: viewer.c viewer.h ui.h errors.h fs.h keyboard.h player.h idt.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c viewer.c -o viewer.o

explorer.o: explorer.c explorer.h ui.h errors.h viewer.h wav.h console.h keyboard.h fs.h player.h idt.h klog.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c explorer.c -o explorer.o

player.o: player.c player.h wav.h fs.h hda.h console.h keyboard.h idt.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c player.c -o player.o

gfx.o: gfx.c gfx.h font8x16.h boot_info.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c gfx.c -o gfx.o

console.o: console.c console.h gfx.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c console.c -o console.o

shell.o: shell.c shell.h xhci.h usbmsc.h player.h explorer.h console.h keyboard.h pmm.h blockdev.h hda.h klog.h power.h fs.h part.h io.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c shell.c -o shell.o

kernel.o: kernel.c boot_info.h logo.h gfx.h console.h shell.h klog.h power.h fs.h part.h idt.h keyboard.h mouse.h pmm.h pci.h ahci.h fat32.h blockdev.h nvme.h hda.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c kernel.c -o kernel.o

kernel.elf: start.o irq_stubs.o idt.o keyboard.o mouse.o pmm.o pci.o ahci.o blockdev.o nvme.o fat32.o xhci.o usbmsc.o hda.o klog.o power.o part.o fs.o wav.o player.o ui.o errors.o viewer.o explorer.o gfx.o console.o shell.o kernel.o linker.ld
	ld -T linker.ld -o kernel.elf start.o irq_stubs.o idt.o keyboard.o mouse.o pmm.o pci.o ahci.o blockdev.o nvme.o fat32.o xhci.o usbmsc.o hda.o klog.o power.o part.o fs.o wav.o player.o ui.o errors.o viewer.o explorer.o gfx.o console.o shell.o kernel.o

$(TARGET): kernel.elf
	objcopy -O binary kernel.elf $(TARGET)

clean:
	rm -f *.o kernel.elf $(TARGET)

.PHONY: all clean
