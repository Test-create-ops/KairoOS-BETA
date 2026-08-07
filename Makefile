all:
	make -f build/Makefile.build

iso: all
	rm -rf iso_root
	mkdir -p iso_root/boot/grub
	cp kernel.bin iso_root/boot/kernel.bin
	printf 'set timeout=0\nset default=0\nmenuentry "Viteza OS" {\n    multiboot2 /boot/kernel.bin\n    boot\n}\n' > iso_root/boot/grub/grub.cfg
	x86_64-elf-grub-mkrescue -o viteza.iso iso_root 2>/dev/null
	@echo "ISO pronta: viteza.iso"

clean:
	make -f build/Makefile.build clean
	rm -rf iso_root viteza.iso

