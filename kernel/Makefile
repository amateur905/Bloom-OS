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

kernel.o: kernel.c boot_info.h font8x16.h logo.h idt.h keyboard.h mouse.h pmm.h pci.h ahci.h fat32.h blockdev.h nvme.h
	gcc -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -m64 -Wall -c kernel.c -o kernel.o

kernel.elf: start.o irq_stubs.o idt.o keyboard.o mouse.o pmm.o pci.o ahci.o blockdev.o nvme.o fat32.o kernel.o linker.ld
	ld -T linker.ld -o kernel.elf start.o irq_stubs.o idt.o keyboard.o mouse.o pmm.o pci.o ahci.o blockdev.o nvme.o fat32.o kernel.o

$(TARGET): kernel.elf
	objcopy -O binary kernel.elf $(TARGET)

clean:
	rm -f *.o kernel.elf $(TARGET)

.PHONY: all clean
